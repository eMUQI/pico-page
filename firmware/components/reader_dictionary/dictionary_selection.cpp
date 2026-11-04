#include "pico_page/dictionary_selection.h"
#include <algorithm>
#include <iterator>

namespace pico_page::dictionary {
namespace {
constexpr std::size_t kMaxScalars = 64, kMaxBytes = 255, kMaxCandidates = 36, kHanLength = 8;
bool han(char32_t c) {
    return (c >= 0x3400 && c <= 0x4dbf) || (c >= 0x4e00 && c <= 0x9fff) ||
           (c >= 0xf900 && c <= 0xfaff) || (c >= 0x20000 && c <= 0x2ee5f) ||
           (c >= 0x2f800 && c <= 0x2fa1f) || (c >= 0x30000 && c <= 0x323af);
}
bool latin(char32_t c) {
    return (c >= U'a' && c <= U'z') || (c >= U'A' && c <= U'Z') ||
           (c >= 0xc0 && c <= 0x2af && c != 0xd7 && c != 0xf7) ||
           (c >= 0x1e00 && c <= 0x1eff);
}
bool mark(char32_t c) { return c >= 0x300 && c <= 0x36f; }
bool joiner(char32_t c) { return c == U'\'' || c == U'\u2019' || c == U'-' || c == U'\u00ad'; }
bool word_at(std::u32string_view text, Selection visible, std::size_t at) {
    if (latin(text[at]) || mark(text[at])) return true;
    return joiner(text[at]) && at > visible.begin && at + 1 < visible.end &&
           (latin(text[at - 1]) || mark(text[at - 1])) && latin(text[at + 1]);
}
struct LookupRange { char32_t first, last; };
constexpr LookupRange kLookupRanges[] = {
#include "third_party/unicode/lookup_ranges.inc"
};
bool unit_at(std::u32string_view text, Selection visible, std::size_t at) {
    const auto c = text[at];
    if (word_at(text, visible, at)) return true;
    const auto found = std::lower_bound(std::begin(kLookupRanges), std::end(kLookupRanges), c,
        [](const LookupRange& range, char32_t scalar) { return range.last < scalar; });
    return found != std::end(kLookupRanges) && found->first <= c;
}
std::size_t utf8_bytes(std::u32string_view text) {
    std::size_t bytes = 0;
    for (const auto c : text) bytes += c < 0x80 ? 1 : c < 0x800 ? 2 : c < 0x10000 ? 3 : 4;
    return bytes;
}
bool contains(Selection outer, Selection inner) {
    return outer.begin <= inner.begin && inner.begin < inner.end && inner.end <= outer.end;
}
Result invalid() { return {Status::InvalidFormat, "Invalid dictionary selection"}; }
Result too_long() { return {Status::ResourceLimit, "Dictionary query exceeds 64 scalars or 255 UTF-8 bytes"}; }
bool encode(std::u32string_view text, std::string& out) {
    out.clear();
    for (const auto c : text) {
        if (!c || c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff)) return false;
        if (c < 0x80) out += static_cast<char>(c);
        else if (c < 0x800) {
            out += static_cast<char>(0xc0 | (c >> 6)); out += static_cast<char>(0x80 | (c & 0x3f));
        } else if (c < 0x10000) {
            out += static_cast<char>(0xe0 | (c >> 12)); out += static_cast<char>(0x80 | ((c >> 6) & 0x3f));
            out += static_cast<char>(0x80 | (c & 0x3f));
        } else {
            out += static_cast<char>(0xf0 | (c >> 18)); out += static_cast<char>(0x80 | ((c >> 12) & 0x3f));
            out += static_cast<char>(0x80 | ((c >> 6) & 0x3f)); out += static_cast<char>(0x80 | (c & 0x3f));
        }
    }
    return true;
}
void add(std::vector<Candidate>& out, Selection range, std::string word, bool inflection = false) {
    if (word.empty() || word.size() > kMaxBytes || out.size() >= kMaxCandidates ||
        std::any_of(out.begin(), out.end(), [&](const auto& item) { return item.word == word; })) return;
    out.push_back({range, std::move(word), inflection});
}
bool ends(const std::string& word, std::string_view suffix) {
    return word.size() > suffix.size() && word.compare(word.size() - suffix.size(), suffix.size(), suffix) == 0;
}
void forms(std::vector<Candidate>& out, Selection range, const std::string& word, bool inflection = false) {
    add(out, range, word, inflection);
    std::string lower = word;
    for (auto& c : lower) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    add(out, range, lower, inflection);
    auto title = lower;
    if (!title.empty() && title[0] >= 'a' && title[0] <= 'z') title[0] -= 'a' - 'A';
    add(out, range, std::move(title), inflection);
    auto upper = lower;
    for (auto& c : upper) if (c >= 'a' && c <= 'z') c -= 'a' - 'A';
    add(out, range, std::move(upper), inflection);
}
void inflections(std::vector<Candidate>& out, Selection range, const std::string& word) {
    if (ends(word, "'s")) forms(out, range, word.substr(0, word.size() - 2), true);
    if (word.size() > 3 && ends(word, "ies")) forms(out, range, word.substr(0, word.size() - 3) + 'y', true);
    if (word.size() > 3 && ends(word, "ied")) forms(out, range, word.substr(0, word.size() - 3) + 'y', true);
    if (word.size() > 3 && ends(word, "es")) forms(out, range, word.substr(0, word.size() - 2), true);
    if (word.size() > 2 && ends(word, "s") && !ends(word, "ss")) forms(out, range, word.substr(0, word.size() - 1), true);
    for (const auto suffix : {std::string_view("ed"), std::string_view("ing")}) {
        if (word.size() <= suffix.size() + 1 || !ends(word, suffix)) continue;
        auto stem = word.substr(0, word.size() - suffix.size());
        forms(out, range, stem, true);
        forms(out, range, stem + 'e', true);
        if (stem.size() > 2 && stem.back() == stem[stem.size() - 2] &&
            std::string_view("bdgmnprt").find(stem.back()) != std::string_view::npos)
            forms(out, range, stem.substr(0, stem.size() - 1), true);
    }
}
}

Result initial_selection(std::u32string_view text, Selection visible, std::size_t hit, Selection& selection) {
    selection = {};
    if (visible.end > text.size() || hit < visible.begin || hit >= visible.end) return invalid();
    if (han(text[hit])) { selection = {hit, hit + 1}; return {}; }
    if (!word_at(text, visible, hit)) return {Status::NotFound, "No dictionary word at this position"};
    auto begin = hit, end = hit + 1;
    while (begin > visible.begin && word_at(text, visible, begin - 1) && end - begin <= kMaxScalars) --begin;
    while (end < visible.end && word_at(text, visible, end) && end - begin <= kMaxScalars) ++end;
    if (end - begin > kMaxScalars) return too_long();
    selection = {begin, end}; return {};
}

Result selection_candidates(std::u32string_view text, Selection visible, Selection selection,
                            std::size_t hit, bool manual, std::vector<Candidate>& candidates) {
    candidates.clear();
    if (visible.end > text.size() || !contains(visible, selection) ||
        (!manual && (hit < selection.begin || hit >= selection.end))) return invalid();
    if (selection.end - selection.begin > kMaxScalars) return too_long();
    std::string original;
    if (!encode(text.substr(selection.begin, selection.end - selection.begin), original)) return invalid();
    if (original.size() > kMaxBytes) return too_long();
    if (manual) { add(candidates, selection, std::move(original)); return {}; }
    if (han(text[hit])) {
        auto begin = hit, end = hit + 1;
        while (begin > visible.begin && hit - begin + 1 < kHanLength && han(text[begin - 1])) --begin;
        while (end < visible.end && end - hit < kHanLength && han(text[end])) ++end;
        for (auto length = std::min(kHanLength, end - begin); length > 0; --length) {
            const auto first = hit + 1 > length ? std::max(begin, hit + 1 - length) : begin;
            for (auto at = first; at <= hit && length <= end - at; ++at) {
                std::string word;
                if (!encode(text.substr(at, length), word)) return invalid();
                add(candidates, {at, at + length}, std::move(word));
            }
        }
        return {};
    }
    forms(candidates, selection, original);
    std::u32string normalized;
    for (auto c : text.substr(selection.begin, selection.end - selection.begin)) {
        if (c == U'­') continue;
        if (c == U'’') c = U'\'';
        normalized += c;
    }
    std::string word;
    if (!encode(normalized, word)) return invalid();
    forms(candidates, selection, word);
    for (auto& c : word) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    inflections(candidates, selection, word);
    return {};
}

Result adjust_selection(std::u32string_view text, Selection visible, SelectionEdge edge,
                        int direction, bool by_scalar, Selection& selection) {
    if (visible.end > text.size() || !contains(visible, selection) || (direction != -1 && direction != 1)) return invalid();
    const auto unit = [&](std::size_t at) { return unit_at(text, visible, at); };
    const auto word = [&](std::size_t at) { return word_at(text, visible, at); };
    auto next = selection;
    if (edge == SelectionEdge::Begin && direction < 0) {
        auto at = next.begin;
        while (at > visible.begin && !unit(at - 1)) --at;
        if (at == visible.begin) return {Status::NotFound, {}};
        if (!by_scalar && word(at - 1)) while (at > visible.begin && word(at - 1)) --at;
        else --at;
        next.begin = at;
    } else if (edge == SelectionEdge::Begin) {
        auto at = next.begin;
        if (!by_scalar && word(at)) while (at < next.end && word(at)) ++at;
        else ++at;
        while (at < next.end && !unit(at)) ++at;
        if (at >= next.end) return {Status::NotFound, {}};
        next.begin = at;
    } else if (direction > 0) {
        auto at = next.end;
        while (at < visible.end && !unit(at)) ++at;
        if (at == visible.end) return {Status::NotFound, {}};
        if (!by_scalar && word(at)) while (at < visible.end && word(at)) ++at;
        else ++at;
        next.end = at;
    } else {
        auto at = next.end;
        if (!by_scalar && word(at - 1)) while (at > next.begin && word(at - 1)) --at;
        else --at;
        while (at > next.begin && !unit(at - 1)) --at;
        if (at <= next.begin) return {Status::NotFound, {}};
        next.end = at;
    }
    if (next.end - next.begin > kMaxScalars || utf8_bytes(text.substr(next.begin, next.end - next.begin)) > kMaxBytes)
        return too_long();
    selection = next;
    return {};
}

}
