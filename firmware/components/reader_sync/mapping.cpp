#include "pico_page/sync.h"
#include <algorithm>
#include <charconv>
#include <cctype>
#include <limits>
#include <string_view>

namespace pico_page::sync {
namespace {
struct Segment { std::string name; std::uint32_t index; };

bool number(std::string_view text, std::uint32_t& value) {
    if (text.empty() || text.size() > 10) return false;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
}
bool parse_path(std::string_view text, std::vector<Segment>& segments) {
    if (text.empty() || text[0] != '/' || text.size() > 4096) return false;
    for (std::size_t begin = 1; begin < text.size();) {
        const auto slash = text.find('/', begin);
        const auto token = text.substr(begin, slash == text.npos ? text.size() - begin : slash - begin);
        if (token.empty()) return false;
        const auto bracket = token.find('[');
        Segment part{std::string(token.substr(0, bracket)), 1};
        if (bracket != token.npos && (token.back() != ']' ||
            !number(token.substr(bracket + 1, token.size() - bracket - 2), part.index) || !part.index)) return false;
        if (part.name != "text()" && (part.name.empty() || !std::all_of(part.name.begin(), part.name.end(), [](char c) {
                return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '1' && c <= '6');
            }))) return false;
        if (part.index > 65536 || segments.size() >= 64) return false;
        segments.push_back(std::move(part));
        if (slash == text.npos) return true;
        begin = slash + 1;
    }
    return false;
}
std::string canonical(const std::vector<Segment>& parts, std::size_t begin) {
    std::string path;
    for (std::size_t i = begin; i < parts.size(); ++i)
        path += '/' + parts[i].name + '[' + std::to_string(parts[i].index) + ']';
    return path;
}
bool block_tag(const std::string& tag) {
    return tag == "body" || tag == "p" || (tag.size() == 2 && tag[0] == 'h' && tag[1] >= '1' && tag[1] <= '6') ||
           tag == "div" || tag == "section" || tag == "article" || tag == "aside" || tag == "header" ||
           tag == "footer" || tag == "nav" || tag == "blockquote" || tag == "ul" || tag == "ol" || tag == "li";
}
bool supported_structure(const std::vector<Segment>& parts) {
    if (parts.size() < 4 || parts[0].name != "html" || parts[0].index != 1 ||
        parts[1].name != "body" || parts[1].index != 1 || parts.back().name != "text()") return false;
    bool inline_parent = false, anchor_parent = false;
    for (std::size_t i = 2; i + 1 < parts.size(); ++i) {
        const auto& tag = parts[i].name;
        const bool inline_tag = tag == "span" || tag == "em" || tag == "strong" || tag == "b" ||
                                tag == "i" || tag == "u" || tag == "s" || tag == "a" || tag == "code" ||
                                tag == "small" || tag == "sub" || tag == "sup" || tag == "mark" ||
                                tag == "abbr" || tag == "cite" || tag == "q" || tag == "del" || tag == "ins";
        const bool paragraph = tag == "p" || (tag.size() == 2 && tag[0] == 'h');
        const bool block = block_tag(tag);
        if ((!inline_tag && !block) || (inline_parent && !inline_tag)) return false;
        if (tag == "a" && anchor_parent) return false;
        if (tag == "li" && parts[i - 1].name != "ul" && parts[i - 1].name != "ol") return false;
        if (parts[i - 1].name == "ul" || parts[i - 1].name == "ol") if (tag != "li") return false;
        inline_parent = inline_parent || inline_tag || paragraph;
        anchor_parent = anchor_parent || tag == "a";
    }
    return true;
}
bool xml_space(char32_t value) { return value == U' ' || value == U'\t' || value == U'\n' || value == U'\r'; }

struct CssProfile { bool normal_whitespace = true; bool changes_display = false; };

CssProfile css_profile(const Chapter& chapter) {
    CssProfile profile;
    for (const auto& source : chapter.css_sources) {
        std::string css;
        css.reserve(source.size());
        for (std::size_t i = 0; i < source.size(); ++i) {
            if (source[i] == '/' && i + 1 < source.size() && source[i + 1] == '*') {
                const auto end = source.find("*/", i + 2);
                if (end == std::string::npos) { profile.normal_whitespace = false; return profile; }
                i = end + 1;
            } else if (source[i] == '\\') {
                // Escaped identifiers require a CSS parser before their whitespace effect can be established.
                profile.normal_whitespace = false; return profile;
            } else css.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(source[i]))));
        }
        if (css.find(":before") != css.npos || css.find(":after") != css.npos ||
            css.find(":first-letter") != css.npos || css.find(":first-line") != css.npos || css.find("@import") != css.npos) {
            profile.normal_whitespace = false; return profile;
        }
        for (std::size_t i = 0; i < css.size();) {
            const auto begin = i;
            while (i < css.size() && (std::isalnum(static_cast<unsigned char>(css[i])) || css[i] == '-' || css[i] == '_')) ++i;
            if (begin == i) { ++i; continue; }
            const auto name = css.substr(begin, i - begin);
            while (i < css.size() && std::isspace(static_cast<unsigned char>(css[i]))) ++i;
            if (i >= css.size() || css[i] != ':') continue;
            ++i;
            if (name == "display") profile.changes_display = true;
            if (name != "white-space") continue;
            while (i < css.size() && std::isspace(static_cast<unsigned char>(css[i]))) ++i;
            const auto value_begin = i;
            while (i < css.size() && (std::isalpha(static_cast<unsigned char>(css[i])) || css[i] == '-')) ++i;
            const auto value = css.substr(value_begin, i - value_begin);
            if (value != "normal" && value != "nowrap" && value != "inherit" && value != "initial" && value != "unset")
                profile.normal_whitespace = false;
        }
    }
    return profile;
}

bool mapping_profile(const Chapter& chapter, CssProfile& css, std::string& error) {
    if (!chapter.plain_dom) { error = "This chapter contains an unsupported KOReader DOM structure"; return false; }
    if (chapter.split_node) { error = "This chapter part divides a text node"; return false; }
    css = css_profile(chapter);
    if (!css.normal_whitespace) { error = "This chapter requires CSS whitespace or generated-content mapping"; return false; }
    return true;
}

bool mapped_path(const Chapter& chapter, const SourceTextNode& node, const CssProfile& css,
                 std::string& path, std::string& error) {
    std::vector<Segment> parts;
    if (!parse_path(node.node, parts) || !supported_structure(parts)) { error = "Unsupported source node structure"; return false; }
    const auto parent = node.node.substr(0, node.node.rfind('/'));
    const auto leading = std::find_if(chapter.source_nodes.begin(), chapter.source_nodes.end(), [&](const auto& sibling) {
        return sibling.first_child && sibling.node.substr(0, sibling.node.rfind('/')) == parent &&
               std::all_of(sibling.text.begin(), sibling.text.end(), xml_space);
    });
    if (leading != chapter.source_nodes.end()) {
        if (css.changes_display) { error = "Leading whitespace requires a resolved CSS display value"; return false; }
        if (block_tag(parts[parts.size() - 2].name)) {
            if (&*leading == &node) { error = "Leading block whitespace is absent from the KOReader DOM"; return false; }
            --parts.back().index;
        }
    }
    path = canonical(parts, 0);
    return true;
}

std::vector<std::uint32_t> normalized_offsets(const SourceTextNode& node) {
    std::vector<std::uint32_t> offsets;
    offsets.reserve(node.text.size());
    bool previous_space = false;
    for (std::size_t i = 0; i < node.text.size(); ++i) {
        const bool space = xml_space(node.text[i]);
        if (!space || !previous_space) offsets.push_back(static_cast<std::uint32_t>(i));
        previous_space = space;
    }
    return offsets;
}
bool supported_version(int version, std::string& error) {
    if (version == 20240114 || version == 20260812) return true;
    error = "Unsupported KOReader DOM version"; return false;
}
}  // namespace

bool to_xpointer(const EpubDocument& document, const Chapter& chapter, const Anchor& anchor,
                 int dom_version, std::string& xpointer, std::string& error) {
    xpointer.clear();
    error.clear();
    if (!supported_version(dom_version, error)) return false;
    std::size_t spine = 0;
    std::uint32_t visible = 0;
    if (anchor.resource != chapter.resource || !document.chapter_index_for(anchor.resource, spine) ||
        !chapter.offset_for(anchor, visible)) { error = "Position is not retained in this chapter"; return false; }
    CssProfile css;
    if (!mapping_profile(chapter, css, error)) return false;
    const auto node = std::find_if(chapter.source_nodes.begin(), chapter.source_nodes.end(),
                                 [&](const auto& item) { return item.node == anchor.node; });
    if (node == chapter.source_nodes.end()) { error = "Source text node is unavailable"; return false; }
    std::string path;
    if (!mapped_path(chapter, *node, css, path, error)) return false;
    const auto offsets = normalized_offsets(*node);
    const auto point = std::lower_bound(offsets.begin(), offsets.end(), anchor.offset);
    if (point == offsets.end() || *point != anchor.offset) { error = "Position lies in collapsed XML whitespace"; return false; }
    xpointer = "/body[1]/DocFragment[" + std::to_string(spine + 1) + ']' +
               path.substr(std::string("/html[1]").size()) + '.' + std::to_string(point - offsets.begin());
    return true;
}

bool from_xpointer(EpubDocument& document, const std::string& xpointer, int dom_version,
                   Anchor& anchor, std::string& error, const std::function<bool()>& stop) {
    anchor = {};
    if (stop && stop()) { error = kChapterCancelled; return false; }
    error.clear();
    if (!supported_version(dom_version, error)) return false;
    if (xpointer.size() > 4096) { error = "KOReader XPointer exceeds the length limit"; return false; }
    const auto dot = xpointer.rfind('.');
    std::uint32_t offset = 0;
    std::vector<Segment> parts;
    if (dot == std::string::npos || !number(std::string_view(xpointer).substr(dot + 1), offset) ||
        !parse_path(std::string_view(xpointer).substr(0, dot), parts) || parts.size() < 5 ||
        parts[0].name != "body" || parts[0].index != 1 || parts[1].name != "DocFragment" ||
        parts[2].name != "body" || parts[2].index != 1 || parts.back().name != "text()" ||
        parts[1].index > document.spine().size()) {
        error = "Unsupported or invalid KOReader text XPointer"; return false;
    }
    const auto index = static_cast<std::size_t>(parts[1].index - 1);
    Chapter chapter;
    const std::string source = "/html[1]" + canonical(parts, 2);
    const std::string_view parent(source.data(), source.rfind('/'));
    const auto locate = [&](const Chapter& candidate, const CssProfile& css) -> const SourceTextNode* {
        for (const auto& item : candidate.source_nodes) {
            if (std::string_view(item.node).substr(0, item.node.rfind('/')) != parent) continue;
            std::string path, ignored;
            if (mapped_path(candidate, item, css, path, ignored) && path == source) return &item;
        }
        return nullptr;
    };
    // The part is chosen before the stylesheets are complete; the profile check below repeats
    // the lookup with them and rejects what they make inexact.
    if (!document.find_chapter_part(index, chapter, error, ChapterLoad::Mapping,
                                    [&](const Chapter& candidate) { return locate(candidate, CssProfile{}) != nullptr; }, stop)) {
        if (error.rfind("Chapter part is not available", 0) == 0) error = "KOReader text node cannot be mapped exactly";
        return false;
    }
    CssProfile css;
    if (!mapping_profile(chapter, css, error)) return false;
    if (stop && stop()) { error = kChapterCancelled; return false; }
    const SourceTextNode* node = locate(chapter, css);
    if (!node) { error = "KOReader text node cannot be mapped exactly"; return false; }
    const auto offsets = normalized_offsets(*node);
    if (offset >= offsets.size()) { error = "KOReader text offset is outside the source node"; return false; }
    Anchor candidate{chapter.resource, node->node, offsets[offset]};
    std::uint32_t visible = 0;
    if (!chapter.offset_for(candidate, visible)) { error = "KOReader position is not retained in the reading text"; return false; }
    anchor = std::move(candidate);
    return true;
}

}  // namespace pico_page::sync
