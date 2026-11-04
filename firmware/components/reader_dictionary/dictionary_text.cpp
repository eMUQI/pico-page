#include "pico_page/dictionary_text.h"
#include "pico_page/document.h"
#include <algorithm>
#include <charconv>
#include <string_view>
#include <limits>

namespace pico_page::dictionary {
namespace {
Result invalid(const char* message) { return {Status::InvalidFormat, message}; }
std::uint32_t tag_id(std::string_view name) {
    std::uint32_t hash = 2166136261U;
    for (auto c : name) { if (c >= 'A' && c <= 'Z') c += 'a' - 'A'; hash = (hash ^ static_cast<unsigned char>(c)) * 16777619U; }
    return hash;
}
bool one_of(std::string_view name, std::initializer_list<std::string_view> names) {
    return std::find(names.begin(), names.end(), name) != names.end();
}
unsigned suppression(std::string_view name) {
    for (const auto& item : {std::pair<std::string_view, unsigned>{"script", 1}, {"style", 2}, {"head", 3}}) {
        if (name.size() != item.first.size()) continue;
        bool equal = true;
        for (std::size_t i = 0; i < name.size(); ++i) {
            auto c = name[i]; if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
            if (c != item.first[i]) { equal = false; break; }
        }
        if (equal) return item.second;
    }
    return 0;
}
bool scalar(std::string_view bytes, std::size_t& at, char32_t& cp) {
    const auto a = static_cast<unsigned char>(bytes[at++]);
    if (a < 128) { cp = a; return a != 0; }
    const auto count = a >= 0xc2 && a <= 0xdf ? 1U : a >= 0xe0 && a <= 0xef ? 2U : a >= 0xf0 && a <= 0xf4 ? 3U : 0U;
    if (!count || count > bytes.size() - at) return false;
    cp = a & ((1U << (6 - count)) - 1);
    for (unsigned i = 0; i < count; ++i) {
        const auto b = static_cast<unsigned char>(bytes[at++]);
        if ((b & 0xc0) != 0x80) return false;
        cp = (cp << 6) | (b & 0x3f);
    }
    return cp >= (count == 1 ? 0x80U : count == 2 ? 0x800U : 0x10000U) &&
           cp <= 0x10ffff && !(cp >= 0xd800 && cp <= 0xdfff);
}
Result next(const std::vector<Field>& fields, TextCursor& cursor, char32_t& cp, bool& bold, bool& end, const Stop& stop) {
    end = false;
    const auto emit = [&](char32_t value) {
        cp = cursor.last = value;
        bold = std::any_of(cursor.emphasis.begin(), cursor.emphasis.begin() + cursor.depth, [](bool value) { return value; });
    };
    while (cursor.field < fields.size()) {
        if (stop && stop()) return {Status::Cancelled, "Dictionary text cancelled"};
        if (cursor.pending) { const auto pending = cursor.pending--; emit(pending == 2 ? U'•' : U' '); return {}; }
        const auto& field = fields[cursor.field];
        const auto bytes = std::string_view(field.text);
        const bool markup = field.type == 'h' || field.type == 'g' || field.type == 'x';
        if (cursor.byte > bytes.size()) return invalid("Invalid definition cursor");
        if (cursor.byte == bytes.size()) {
            if (cursor.suppressed) return invalid("Unclosed dictionary script or style");
            ++cursor.field; cursor.byte = 0; cursor.depth = 0;
            if (cursor.field < fields.size() && cursor.last && cursor.last != U'\n') { emit(U'\n'); return {}; }
            continue;
        }
        if (cursor.suppressed) {
            bool closes = false;
            if (bytes.substr(cursor.byte, 2) == "</") {
                auto at = cursor.byte + 2;
                while (at < bytes.size() && ((bytes[at] >= 'a' && bytes[at] <= 'z') || (bytes[at] >= 'A' && bytes[at] <= 'Z'))) ++at;
                closes = suppression(bytes.substr(cursor.byte + 2, at - cursor.byte - 2)) == cursor.suppressed &&
                         at < bytes.size() && (bytes[at] == '>' || bytes[at] == ' ' || bytes[at] == '\t' || bytes[at] == '\n');
            }
            if (!closes) { ++cursor.byte; continue; }
        }
        if (markup && bytes[cursor.byte] == '<') {
            if (bytes.substr(cursor.byte, 4) == "<!--") {
                cursor.byte += 4;
                while (bytes.substr(cursor.byte, 3) != "-->") {
                    if (cursor.byte == bytes.size()) return invalid("Unclosed dictionary comment");
                    if (cursor.byte % 256 == 0 && stop && stop()) return {Status::Cancelled, "Dictionary text cancelled"};
                    ++cursor.byte;
                }
                cursor.byte += 3; continue;
            }
            std::size_t at = cursor.byte + 1;
            const bool closing = at < bytes.size() && bytes[at] == '/';
            if (closing) ++at;
            std::string name;
            while (at < bytes.size() && ((bytes[at] >= 'a' && bytes[at] <= 'z') ||
                   (bytes[at] >= 'A' && bytes[at] <= 'Z') || (bytes[at] >= '0' && bytes[at] <= '9') || bytes[at] == ':')) {
                if (name.size() == 64) return invalid("Dictionary tag name is too long");
                auto c = bytes[at++]; if (c >= 'A' && c <= 'Z') c += 'a' - 'A'; name += c;
            }
            char quote = 0;
            for (; at < bytes.size(); ++at) {
                if (at % 256 == 0 && stop && stop()) return {Status::Cancelled, "Dictionary text cancelled"};
                const auto c = bytes[at];
                if (quote) { if (c == quote) quote = 0; }
                else if (c == '\'' || c == '"') quote = c;
                else if (c == '>') break;
            }
            if (at == bytes.size()) return invalid("Unclosed dictionary tag");
            const bool empty = at && bytes[at - 1] == '/';
            cursor.byte = at + 1;
            if (name.empty()) { cursor.degraded = true; continue; }
            const auto id = tag_id(name);
            if (cursor.suppressed) { if (closing && suppression(name) == cursor.suppressed) cursor.suppressed = 0; continue; }
            if (!closing && suppression(name)) { if (!empty) cursor.suppressed = suppression(name); continue; }
            const bool block = one_of(name, {"p", "div", "br", "hr", "li", "ul", "ol", "blockquote", "section", "h1", "h2", "h3", "h4", "h5", "h6", "def", "ex", "tr", "rref", "dt", "dd"});
            const bool emphasis = one_of(name, {"b", "strong", "i", "em", "k", "abr", "h1", "h2", "h3", "h4", "h5", "h6"});
            const bool void_tag = one_of(name, {"br", "hr", "img", "meta", "link", "input", "wbr", "source"});
            if (closing) {
                auto depth = cursor.depth;
                while (depth && cursor.tags[depth - 1] != id) --depth;
                if (depth) cursor.depth = depth - 1;
            } else if (!empty && !void_tag) {
                // HTML paragraphs and list items may omit their closing tags.
                if ((name == "p" || name == "li") && cursor.depth && cursor.tags[cursor.depth - 1] == id) --cursor.depth;
                if (cursor.depth == cursor.tags.size()) return {Status::ResourceLimit, "Dictionary markup exceeds 64 levels"};
                cursor.tags[cursor.depth] = id; cursor.emphasis[cursor.depth++] = emphasis;
            }
            if (!closing && name == "li") cursor.pending = 2;
            if (block && cursor.last && cursor.last != U'\n') { emit(U'\n'); return {}; }
            continue;
        }
        char32_t value = 0;
        if (markup && bytes[cursor.byte] == '&' && !cursor.suppressed) {
            const auto relative = bytes.substr(cursor.byte + 1, 32).find(';');
            if (relative != std::string_view::npos) {
                const auto semicolon = cursor.byte + 1 + relative;
                auto name = bytes.substr(cursor.byte + 1, semicolon - cursor.byte - 1);
                if (!name.empty() && name.front() == '#') {
                    name.remove_prefix(1); int base = 10;
                    if (!name.empty() && (name.front() == 'x' || name.front() == 'X')) { base = 16; name.remove_prefix(1); }
                    std::uint32_t number = 0;
                    const auto converted = std::from_chars(name.data(), name.data() + name.size(), number, base);
                    if (converted.ec != std::errc{} || converted.ptr != name.data() + name.size() || !number ||
                        number > 0x10ffff || (number >= 0xd800 && number <= 0xdfff)) return invalid("Invalid dictionary character entity");
                    value = static_cast<char32_t>(number);
                } else if (name == "amp") value = U'&';
                else if (name == "lt") value = U'<';
                else if (name == "gt") value = U'>';
                else if (name == "quot") value = U'"';
                else if (name == "apos") value = U'\'';
                else value = named_html_entity(name);
                if (value) cursor.byte = semicolon + 1;
            }
            if (!value) cursor.degraded = true;
        }
        if (!value && !scalar(bytes, cursor.byte, value)) return invalid("Invalid UTF-8 dictionary text");
        if (cursor.suppressed) continue;
        if (value == U'\r') { if (cursor.byte < bytes.size() && bytes[cursor.byte] == '\n') ++cursor.byte; value = U'\n'; }
        if (value < 32 && value != U'\n' && value != U'\t') continue;
        if (markup && field.type != 'g' && (value == U'\n' || value == U'\t' || value == U' ' || value == U'\u00a0')) {
            if (!cursor.last || cursor.last == U' ' || cursor.last == U'\n') continue;
            value = U' ';
        }
        if (value == U'\t') value = U' ';
        emit(value); return {};
    }
    end = true; return {};
}
}

Result text_chunk(const std::vector<Field>& fields, TextCursor& cursor, std::size_t limit, TextChunk& chunk, const Stop& stop) {
    chunk = {};
    if (!limit || limit > 4096 || cursor.depth > cursor.tags.size() || cursor.field > fields.size() ||
        cursor.pending > 2 || cursor.suppressed > 3)
        return invalid("Invalid dictionary text chunk");
    auto next_cursor = cursor;
    for (std::size_t i = 0; i < limit; ++i) {
        char32_t cp = 0; bool bold = false, end = false;
        auto result = next(fields, next_cursor, cp, bold, end, stop);
        if (!result) { chunk = {}; return result; }
        if (end) { chunk.end = true; break; }
        chunk.text += cp; chunk.emphasis.push_back(bold);
    }
    if (next_cursor.scalars > std::numeric_limits<std::size_t>::max() - chunk.text.size()) {
        chunk = {}; return invalid("Dictionary text offset overflow");
    }
    next_cursor.scalars += chunk.text.size();
    cursor = next_cursor; return {};
}
}
