#pragma once

#include <algorithm>
#include <functional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace pico_page {
/** Converts explicitly declared HTML body resources into bounded, balanced XML tokens.
 * Package and navigation XML bypass this adapter. The output has local anchor semantics;
 * HTML tree repair does not establish a compatible KOReader source tree.
 */
class HtmlReader {
public:
    explicit HtmlReader(std::size_t prefix_bytes = 0) : prefix_remaining_(prefix_bytes) {}
    std::string error;
    void observe_start(std::string name) {
        if (raw_.empty() && raw_text_element(name)) raw_ = lower(name);
        stack_.push_back(std::move(name));
    }
    void observe_end() {
        if (stack_.empty()) return;
        const bool closes_raw = !raw_.empty() && lower(stack_.back()) == raw_;
        stack_.pop_back();
        if (closes_raw) restore_raw_ancestor();
    }
    std::size_t retained_bytes() const {
        std::size_t bytes = sizeof(*this) + error.capacity() + pending_.capacity() + raw_.capacity() + stack_.capacity() * sizeof(std::string);
        for (const auto& name : stack_) bytes += name.capacity();
        return bytes;
    }
    bool feed(std::string_view bytes, bool final, const std::function<bool(std::string_view)>& emit) {
        pending_.append(bytes);
        if (prefix_remaining_ && !pending_.empty()) {
            const auto count = std::min(prefix_remaining_, pending_.size());
            const auto prefix = pending_.substr(0, count);
            pending_.erase(0, count); prefix_remaining_ -= count;
            // Previously completed XML events retain their original tree and text-node boundaries.
            if (!emit(prefix)) return false;
        }
        if (prefix_remaining_) return !final || fail("Truncated HTML source prefix");
        while (!pending_.empty()) {
            std::size_t count = 0;
            std::string output;
            if (!raw_.empty()) {
                const auto folded = lower(pending_);
                const auto marker = "</" + raw_;
                auto close = folded.find(marker);
                while (close != std::string::npos) {
                    const auto end = close + marker.size();
                    if (end < folded.size() && (space(folded[end]) || folded[end] == '/' || folded[end] == '>')) break;
                    close = folded.find(marker, end);
                }
                if (close == std::string::npos) {
                    count = final ? pending_.size() : pending_.size() > raw_.size() + 3 ? pending_.size() - raw_.size() - 3 : 0;
                    if (!count) break;
                    output = escaped(std::string_view(pending_).substr(0, count), false);
                } else if (close) {
                    count = close;
                    output = escaped(std::string_view(pending_).substr(0, count), false);
                } else raw_.clear();
            }
            if (!count && raw_.empty()) {
                if (pending_.front() != '<') {
                    const auto next = pending_.find('<');
                    count = next == std::string::npos ? pending_.size() : next;
                    if (!final && next == std::string::npos) {
                        const auto entity = pending_.rfind('&');
                        if (entity != std::string::npos && pending_.find(';', entity) == std::string::npos && count - entity <= 32) count = entity;
                        while (count && count < pending_.size() && (static_cast<unsigned char>(pending_[count]) & 0xc0) == 0x80) --count;
                        if (count == pending_.size()) {
                            auto start = count;
                            while (start && (static_cast<unsigned char>(pending_[start - 1]) & 0xc0) == 0x80) --start;
                            if (start) {
                                --start;
                                const auto lead = static_cast<unsigned char>(pending_[start]);
                                const unsigned length = lead < 0x80 ? 1 : lead < 0xe0 ? 2 : lead < 0xf0 ? 3 : 4;
                                if (count - start < length) count = start;
                            }
                        }
                    }
                    if (!count) break;
                    output = escaped(std::string_view(pending_).substr(0, count), true);
                } else if (pending_.size() >= 2 && pending_[1] != '!' && pending_[1] != '/' &&
                           !((pending_[1] >= 'a' && pending_[1] <= 'z') || (pending_[1] >= 'A' && pending_[1] <= 'Z'))) {
                    count = 1; output = "&lt;";
                } else if (pending_.compare(0, 4, "<!--") == 0) {
                    const auto end = pending_.find("-->", 4);
                    if (end == std::string::npos) { if (pending_.size() > kTokenLimit || final) return fail("Unterminated HTML comment"); break; }
                    count = end + 3;
                } else {
                    char quote = 0;
                    std::size_t end = 1;
                    for (; end < pending_.size(); ++end) {
                        const char c = pending_[end];
                        if (quote) { if (c == quote) quote = 0; }
                        else if (c == '\'' || c == '"') quote = c;
                        else if (c == '>') break;
                    }
                    if (end == pending_.size()) { if (pending_.size() > kTokenLimit || final) return fail("Unterminated HTML tag"); break; }
                    count = end + 1;
                    if (count > kTokenLimit) return fail("HTML token limit exceeded");
                    if (!tag(std::string_view(pending_).substr(1, end - 1), output)) return false;
                }
            }
            pending_.erase(0, count);
            if (!output.empty() && !emit(output)) return false;
        }
        if (pending_.size() > kTokenLimit) return fail("HTML token limit exceeded");
        if (final && pending_.empty() && !stack_.empty()) {
            std::string output;
            while (!stack_.empty()) { output += "</" + stack_.back() + ">"; stack_.pop_back(); }
            if (!emit(output)) return false;
        }
        return true;
    }
private:
    static constexpr std::size_t kTokenLimit = 64 * 1024;
    std::string pending_, raw_;
    std::size_t prefix_remaining_ = 0;
    std::vector<std::string> stack_;
    bool fail(const char* message) { error = message; return false; }
    static bool space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f'; }
    static std::string lower(std::string_view value) {
        std::string result(value);
        for (auto& c : result) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
        return result;
    }
    static bool raw_text_element(std::string_view name) {
        const auto colon = name.rfind(':');
        const auto local = lower(name.substr(colon == std::string_view::npos ? 0 : colon + 1));
        return local == "script" || local == "style";
    }
    void restore_raw_ancestor() {
        raw_.clear();
        for (const auto& name : stack_) if (raw_text_element(name)) { raw_ = lower(name); break; }
    }
    static std::string escaped(std::string_view value, bool entities) {
        std::string out;
        for (std::size_t i = 0; i < value.size(); ++i) {
            const char c = value[i];
            if (c == '&' && entities) {
                const auto end = value.find(';', i + 1);
                if (end != std::string_view::npos && end - i <= 32) {
                    const auto name = value.substr(i + 1, end - i - 1);
                    const auto scalar = named_html_entity(name);
                    if (scalar) { out += "&#" + std::to_string(scalar) + ';'; i = end; continue; }
                    if (name == "amp" || name == "lt" || name == "gt" || name == "quot" || name == "apos" || (!name.empty() && name.front() == '#')) {
                        out.append(value.substr(i, end - i + 1)); i = end; continue;
                    }
                }
            }
            if (c == '\v' || c == '\f') out += ' ';
            else if (c == '&') out += "&amp;";
            else if (c == '<') out += "&lt;";
            else if (c == '"') out += "&quot;";
            else out += c;
        }
        return out;
    }
    bool tag(std::string_view value, std::string& out) {
        std::size_t at = 0;
        while (at < value.size() && space(value[at])) ++at;
        if (at == value.size()) { out = "&lt;>"; return true; }
        if (value[at] == '!') {
            const auto declaration = lower(value.substr(at));
            if (declaration.rfind("!doctype html", 0) != 0 || declaration.find('[') != std::string::npos) return fail("Unsupported HTML declaration");
            return true;
        }
        if (value[at] == '?') return fail("Unsupported HTML processing instruction");
        const bool closing = value[at] == '/';
        if (closing) ++at;
        const auto begin = at;
        while (at < value.size() && !space(value[at]) && value[at] != '/') ++at;
        const auto name = lower(value.substr(begin, at - begin));
        if (name.empty() || name.front() < 'a' || name.front() > 'z' ||
            name.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789:_-") != std::string::npos) return fail("Invalid HTML element name");
        if (closing) {
            const auto found = std::find_if(stack_.rbegin(), stack_.rend(), [&](const auto& open) { return lower(open) == name; });
            if (found == stack_.rend()) return true;
            const auto remaining = static_cast<std::size_t>(stack_.rend() - found - 1);
            while (stack_.size() > remaining) { out += "</" + stack_.back() + ">"; stack_.pop_back(); }
            restore_raw_ancestor();
            return true;
        }
        // HTML's optional paragraph/list end tags are repaired before XML receives the next token.
        if (!stack_.empty() && ((stack_.back() == name && (name == "li" || name == "dt" || name == "dd" || name == "p")) ||
            (stack_.back() == "p" && (name == "div" || name == "section" || name == "ul" || name == "ol" || name == "table")))) {
            out += "</" + stack_.back() + ">"; stack_.pop_back();
        }
        out += '<' + name;
        std::set<std::string> attributes;
        while (at < value.size()) {
            while (at < value.size() && space(value[at])) ++at;
            if (at == value.size() || value[at] == '/') break;
            const auto start = at;
            while (at < value.size() && !space(value[at]) && value[at] != '=' && value[at] != '/') ++at;
            if (start == at) return fail("Invalid HTML attribute");
            auto attribute = lower(value.substr(start, at - start));
            while (at < value.size() && space(value[at])) ++at;
            std::string_view content;
            if (at < value.size() && value[at] == '=') {
                ++at;
                while (at < value.size() && space(value[at])) ++at;
                const char quote = at < value.size() && (value[at] == '\'' || value[at] == '"') ? value[at++] : 0;
                const auto text = at;
                while (at < value.size() && (quote ? value[at] != quote : !space(value[at]))) ++at;
                content = value.substr(text, at - text);
                if (quote) { if (at == value.size()) return fail("Unterminated HTML attribute"); ++at; }
            }
            // The XML callback applies the image URI policy to the decoded attribute value.
            const auto value_limit = name == "img" && attribute == "src" ? kTokenLimit : 8192;
            if (attribute.size() > 2048 || content.size() > value_limit || attributes.size() >= 64) return fail("HTML attribute limit exceeded");
            if (attribute.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789:_-") == std::string::npos && attributes.insert(attribute).second)
                out += ' ' + attribute + "=\"" + escaped(content, true) + '"';
        }
        static const std::set<std::string> voids = {"area", "base", "br", "col", "embed", "hr", "img", "input", "link", "meta", "param", "source", "track", "wbr"};
        const bool empty = voids.count(name) || (!value.empty() && value.back() == '/');
        out += empty ? "/>" : ">";
        if (!empty) {
            if (stack_.size() >= 64) return fail("HTML structure limit exceeded");
            stack_.push_back(name);
            if (raw_text_element(name)) raw_ = name;
        }
        return true;
    }
};
} // namespace pico_page
