#include "pico_page/stage_timer.h"
#include "pico_page/document.h"
#include "expat.h"
#include "miniz.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <limits>
#include <map>
#include <set>
#include <utility>
#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#include "esp_random.h"
#include "psa/crypto.h"
#else
#include <openssl/evp.h>
#include <openssl/rand.h>
#endif

namespace pico_page {
namespace {
std::atomic<std::uint64_t> identity_scans{0};
constexpr std::uint64_t kMaxBook = 256ULL * 1024 * 1024;
// XHTML is parsed as a stream and kept one part at a time, so these two bound parsing time
// rather than memory.
constexpr std::uint32_t kMaxXmlBytes = 16 * 1024 * 1024;
constexpr std::uint32_t kMaxText = 2 * kChapterPartScalars;
// A part is closed at the first block boundary past the first set of thresholds and inside a
// block past the second; the record limits further below are therefore never reached by text.
constexpr std::size_t kPartRecords = 4096, kPartMetadata = 256 * 1024;
constexpr std::size_t kPartRecordsHard = 6000, kPartMetadataHard = 448 * 1024;
constexpr char kPartComplete[] = "Chapter part complete";
bool platform_memory_room(std::size_t bytes) {
#ifdef ESP_PLATFORM
    // Free PSRAM kept while a chapter is parsed. An allocation failure aborts the firmware, so
    // the parser stops first. Allocations between two checks are smaller than this reserve: one
    // decoded XML chunk, record vectors growing within their limits, and strings below the
    // checked sizes.
    constexpr std::size_t reserve = 768 * 1024;
    return heap_caps_get_free_size(MALLOC_CAP_SPIRAM) >= bytes + reserve &&
           (bytes < 64 * 1024 || heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) >= bytes);
#else
    static_cast<void>(bytes);
    return true;
#endif
}
bool (*memory_room)(std::size_t) = platform_memory_room;
constexpr std::size_t kMaxNodes = 400000;
constexpr std::size_t kMaxPath = 2048;
constexpr std::size_t kMaxCssBytes = 256 * 1024;

bool fail(std::string& error, const std::string& message) { error = message; return false; }
std::string local_name(const char* name) {
    const char* sep = std::strrchr(name, '|');
    return sep ? sep + 1 : name;
}
std::string attr(const char** attrs, const char* name) {
    for (auto p = attrs; p && *p; p += 2) if (local_name(p[0]) == name) return p[1];
    return {};
}
bool token(const std::string& list, const std::string& wanted) {
    std::size_t pos = 0;
    while (pos < list.size()) {
        auto end = list.find_first_of(" \r\n\t", pos);
        if (list.substr(pos, end == std::string::npos ? end : end - pos) == wanted) return true;
        if (end == std::string::npos) break;
        pos = end + 1;
    }
    return false;
}
bool space(char32_t c) { return c == U' ' || c == U'\t' || c == U'\r' || c == U'\n'; }
std::string trim(const std::string& value) {
    const auto first = value.find_first_not_of(" \r\n\t");
    if (first == std::string::npos) return {};
    return value.substr(first, value.find_last_not_of(" \r\n\t") - first + 1);
}
bool decode(const char* data, std::size_t size, std::u32string& out) {
    out.clear();
    for (std::size_t i = 0; i < size;) {
        const auto c = static_cast<unsigned char>(data[i++]);
        unsigned extra = 0; char32_t cp = c;
        if (c >= 0xc2 && c <= 0xdf) { extra = 1; cp = c & 31; }
        else if (c >= 0xe0 && c <= 0xef) { extra = 2; cp = c & 15; }
        else if (c >= 0xf0 && c <= 0xf4) { extra = 3; cp = c & 7; }
        else if (c >= 0x80) return false;
        if (i + extra > size) return false;
        for (unsigned j = 0; j < extra; ++j) {
            auto next = static_cast<unsigned char>(data[i++]);
            if ((next & 0xc0) != 0x80) return false;
            cp = (cp << 6) | (next & 63);
        }
        if ((extra == 1 && cp < 0x80) || (extra == 2 && cp < 0x800) ||
            (extra == 3 && cp < 0x10000) || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) return false;
        out.push_back(cp);
    }
    return true;
}
int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
bool unescape(const std::string& value, std::string& out) {
    out.clear();
    for (std::size_t i = 0; i < value.size(); ++i) {
        unsigned char c = value[i];
        if (c == '%') {
            if (i + 2 >= value.size()) return false;
            const int a = hex_digit(value[i + 1]), b = hex_digit(value[i + 2]);
            if (a < 0 || b < 0) return false;
            c = static_cast<unsigned char>(a * 16 + b); i += 2;
        }
        if (c < 32 || c == 127 || c == '\\') return false;
        out.push_back(static_cast<char>(c));
    }
    std::u32string valid;
    return decode(out.data(), out.size(), valid);
}
bool path_parts(const std::string& path, std::string& result) {
    if (path.empty() || path.front() == '/' || path.find_first_of("\\:\0", 0, 3) != std::string::npos) return false;
    std::vector<std::string> parts;
    std::size_t start = 0;
    while (start < path.size()) {
        auto end = path.find('/', start);
        std::string part = path.substr(start, end == std::string::npos ? end : end - start);
        if (part == "..") { if (parts.empty()) return false; parts.pop_back(); }
        else if (!part.empty() && part != ".") parts.push_back(std::move(part));
        if (end == std::string::npos) break;
        start = end + 1;
    }
    result.clear();
    for (const auto& p : parts) { if (!result.empty()) result += '/'; result += p; }
    return !result.empty() && result.size() <= 511;
}
bool resolve(const std::string& base, const std::string& href, std::string& resource, std::string& fragment) {
    auto hash = href.find('#');
    std::string path;
    if (!unescape(href.substr(0, hash), path) || path.find('?') != std::string::npos) return false;
    if (!unescape(hash == std::string::npos ? "" : href.substr(hash + 1), fragment)) return false;
    if (path.empty()) { resource = base; return !base.empty(); }
    if (path.front() == '/' || path.find(':') != std::string::npos) return false;
    auto slash = base.rfind('/');
    return path_parts((slash == std::string::npos ? "" : base.substr(0, slash + 1)) + path, resource);
}
std::uint16_t le16(const unsigned char* p) { return p[0] | (p[1] << 8); }
std::uint32_t le32(const unsigned char* p) {
    return std::uint32_t(p[0]) | (std::uint32_t(p[1]) << 8) | (std::uint32_t(p[2]) << 16) | (std::uint32_t(p[3]) << 24);
}
std::string hex(const unsigned char* data, std::size_t size) {
    std::string out; out.reserve(size * 2);
    for (std::size_t i = 0; i < size; ++i) { out += "0123456789abcdef"[data[i] >> 4]; out += "0123456789abcdef"[data[i] & 15]; }
    return out;
}

class Hash {
public:
#ifdef ESP_PLATFORM
    explicit Hash(bool md5) { ok_ = psa_crypto_init() == PSA_SUCCESS && psa_hash_setup(&context_, md5 ? PSA_ALG_MD5 : PSA_ALG_SHA_256) == PSA_SUCCESS; }
    ~Hash() { psa_hash_abort(&context_); }
    bool update(const unsigned char* bytes, std::size_t size) { return ok_ = ok_ && psa_hash_update(&context_, bytes, size) == PSA_SUCCESS; }
    bool finish(std::string& out) {
        unsigned char bytes[32]; std::size_t size = 0;
        if (!ok_ || psa_hash_finish(&context_, bytes, sizeof(bytes), &size) != PSA_SUCCESS) return false;
        out = hex(bytes, size); return true;
    }
private:
    psa_hash_operation_t context_ = PSA_HASH_OPERATION_INIT;
#else
    explicit Hash(bool md5) : context_(EVP_MD_CTX_new()) { ok_ = context_ && EVP_DigestInit_ex(context_, md5 ? EVP_md5() : EVP_sha256(), nullptr) == 1; }
    ~Hash() { EVP_MD_CTX_free(context_); }
    bool update(const unsigned char* bytes, std::size_t size) { return ok_ = ok_ && EVP_DigestUpdate(context_, bytes, size) == 1; }
    bool finish(std::string& out) {
        unsigned char bytes[EVP_MAX_MD_SIZE]; unsigned size = 0;
        if (!ok_ || EVP_DigestFinal_ex(context_, bytes, &size) != 1) return false;
        out = hex(bytes, size); return true;
    }
private:
    EVP_MD_CTX* context_;
#endif
    bool ok_ = false;
};

struct Xml {
    XML_Parser parser = nullptr;
    std::function<void(const std::string&, const char**)> start;
    std::function<void(const std::string&)> end;
    std::function<void(const char*, std::size_t)> text;
    std::function<void()> boundary;
    std::function<void()> complex_markup;
    bool foreign_namespace = false;
    std::string error;
    unsigned depth = 0;
    std::size_t nodes = 0, bytes = 0;
    Xml() {
        parser = XML_ParserCreateNS(nullptr, '|');
        if (!parser) { error = "XML parser allocation failed"; return; }
        unsigned long salt = 0;
#ifdef ESP_PLATFORM
        esp_fill_random(&salt, sizeof(salt));
#else
        if (RAND_bytes(reinterpret_cast<unsigned char*>(&salt), sizeof(salt)) != 1) { error = "XML hash entropy failed"; return; }
#endif
        XML_SetHashSalt(parser, salt ? salt : 1);
        XML_SetUserData(parser, this);
        XML_SetElementHandler(parser, [](void* opaque, const char* name, const char** attrs) {
            auto& x = *static_cast<Xml*>(opaque);
            if (++x.depth > 64 || ++x.nodes > kMaxNodes) { x.stop("XML structure limit exceeded"); return; }
            std::size_t attributes = 0;
            for (auto p = attrs; p && *p; p += 2) {
                if (++attributes > 64 || std::strlen(p[0]) > kMaxPath || std::strlen(p[1]) > 8192) { x.stop("XML attribute limit exceeded"); return; }
            }
            const char* separator = std::strchr(name, '|');
            if (separator && std::string(name, separator - name) != "http://www.w3.org/1999/xhtml") x.foreign_namespace = true;
            if (x.start) x.start(local_name(name), attrs);
        }, [](void* opaque, const char* name) {
            auto& x = *static_cast<Xml*>(opaque);
            if (x.end) x.end(local_name(name));
            --x.depth;
        });
        XML_SetCharacterDataHandler(parser, [](void* opaque, const char* value, int count) {
            auto& x = *static_cast<Xml*>(opaque);
            if (x.text) x.text(value, static_cast<std::size_t>(count));
        });
        XML_SetStartDoctypeDeclHandler(parser, [](void* opaque, const char*, const char*, const char*, int has_internal_subset) {
            auto& x = *static_cast<Xml*>(opaque);
            if (has_internal_subset) x.stop("Internal DTD subsets are unsupported");
        });
        XML_SetSkippedEntityHandler(parser, [](void* opaque, const char*, int) { static_cast<Xml*>(opaque)->stop("Unresolved XML entities are unsupported"); });
        XML_SetExternalEntityRefHandler(parser, [](XML_Parser, const char*, const char*, const char*, const char*) -> int { return XML_STATUS_ERROR; });
        XML_SetStartCdataSectionHandler(parser, [](void* opaque) { auto& x = *static_cast<Xml*>(opaque); if (x.complex_markup) x.complex_markup(); });
        XML_SetCommentHandler(parser, [](void* opaque, const char*) { auto& x = *static_cast<Xml*>(opaque); if (x.boundary) x.boundary(); });
        XML_SetProcessingInstructionHandler(parser, [](void* opaque, const char*, const char*) { auto& x = *static_cast<Xml*>(opaque); if (x.boundary) x.boundary(); });
    }
    ~Xml() { if (parser) XML_ParserFree(parser); }
    void stop(const std::string& message) { if (error.empty()) error = message; if (parser) XML_StopParser(parser, XML_FALSE); }
    bool feed(const char* value, std::size_t count, bool final) {
        bytes += count;
        if (!error.empty() || !parser) return false;
        if (bytes > kMaxXmlBytes) { stop("XML byte limit exceeded"); return false; }
        if (XML_Parse(parser, value, static_cast<int>(count), final ? XML_TRUE : XML_FALSE) != XML_STATUS_OK) {
            if (error.empty()) error = std::string("Invalid XML: ") + XML_ErrorString(XML_GetErrorCode(parser));
            return false;
        }
        return true;
    }
};

struct TreeNode {
    std::string name, text;
    std::map<std::string, std::string> attrs;
    std::size_t parent = 0;
};
struct XmlTree {
    std::vector<TreeNode> nodes;
    std::vector<std::size_t> stack;
    Xml xml;
    std::size_t text_bytes = 0;
    XmlTree() {
        xml.start = [this](const std::string& name, const char** attrs) {
            if (nodes.size() >= 4096) { xml.stop("Package/navigation node limit exceeded"); return; }
            TreeNode node; node.name = name; node.parent = stack.empty() ? 0 : stack.back();
            for (auto p = attrs; p && *p; p += 2) node.attrs[local_name(p[0])] = p[1];
            nodes.push_back(std::move(node)); stack.push_back(nodes.size() - 1);
        };
        xml.end = [this](const std::string&) { if (!stack.empty()) stack.pop_back(); };
        xml.text = [this](const char* value, std::size_t count) {
            if (stack.empty()) return;
            for (auto index : stack) {
                auto& node = nodes[index];
                const bool label = node.name == "title" || node.name == "creator" || node.name == "language" || node.name == "a" || node.name == "text";
                if (index != stack.back() && !label) continue;
                text_bytes += count;
                if (text_bytes > 262144 || node.text.size() + count > 32768) { xml.stop("Package/navigation text limit exceeded"); return; }
                node.text.append(value, count);
            }
        };
    }
};
std::string get(const TreeNode& node, const std::string& name) {
    auto found = node.attrs.find(name); return found == node.attrs.end() ? "" : found->second;
}
void warn(Chapter& chapter, const std::string& value) {
    if (std::find(chapter.warnings.begin(), chapter.warnings.end(), value) == chapter.warnings.end()) chapter.warnings.push_back(value);
}

struct ChapterParser {
    struct Frame {
        std::string name, path;
        std::map<std::string, unsigned> children;
        unsigned text_index = 0;
        std::uint32_t source_offset = 0;
        bool in_text = false, hidden = false, body = false, pre = false, block = false, has_child = false;
        std::size_t css = std::numeric_limits<std::size_t>::max();
        BlockKind kind = BlockKind::Paragraph;
        std::size_t link = std::numeric_limits<std::size_t>::max();
        std::size_t source_node = std::numeric_limits<std::size_t>::max();
        std::string node;  // Path of the current text node when the chapter keeps no source_nodes paths.
        std::size_t image = std::numeric_limits<std::size_t>::max();
        bool simple_svg = true;
        unsigned svg_images = 0, svg_width = 0, svg_height = 0;
    };
    // chapter is the part being collected; the wanted part is moved to result when it closes.
    Chapter chapter;
    Chapter& result;
    std::uint32_t wanted = 0;  // Part number, or kLastChapterPart.
    const std::function<bool(const Chapter&)>* match = nullptr;  // Replaces wanted when set.
    std::uint32_t part = 0;
    std::size_t part_nodes = 0;
    bool split_pending = false, captured = false;
    Xml xml;
    std::vector<Frame> frames;
    std::uint32_t block_start = 0, source_count = 0;
    std::size_t retained_metadata_bytes = 0;
    std::size_t css_bytes = 0;
    std::vector<std::string> css_links;
    bool pending_space = false, seen_body = false;
    // Every ChapterLoad level applies the same limits and run boundaries; the flags only select
    // what is stored. keep_mapping: source node paths and text, stylesheets. keep_anchors: the
    // resource and node of each run.
    bool keep_mapping = true, keep_anchors = true;
    std::string last_run_node;  // Node of the last run when runs keep no anchors.
    Anchor pending_anchor;
    BlockKind block_kind = BlockKind::Paragraph;
    ChapterParser(Chapter& value, const std::string& resource) : result(value) {
        chapter.resource = resource;
        xml.start = [this](const std::string& name, const char** attrs) { start(name, attrs); };
        xml.end = [this](const std::string&) { end(); };
        xml.text = [this](const char* text, std::size_t count) { characters(text, count); };
        xml.boundary = [this]() {
            if (!frames.empty()) {
                if (frames.back().body) chapter.plain_dom = false;
                frames.back().in_text = false;
            }
        };
        xml.complex_markup = [this]() { if (!frames.empty() && frames.back().body) chapter.plain_dom = false; };
    }
    bool room(std::size_t bytes) {
        if (memory_room(bytes)) return true;
        xml.stop(kChapterMemoryError);
        return false;
    }
    /** Appends to the chapter text; growth doubles the buffer while the old one is still held. */
    bool push(char32_t cp) {
        if (chapter.text.size() == chapter.text.capacity() &&
            !room(2 * sizeof(char32_t) * std::max<std::size_t>(chapter.text.capacity(), 16))) return false;
        chapter.text.push_back(cp);
        return true;
    }
    bool retain_css(const std::string& css) {
        if (css.size() > kMaxCssBytes - css_bytes || chapter.css_sources.size() >= 256) {
            chapter.plain_dom = false;
            warn(chapter, "Stylesheet mapping limit exceeded");
            return false;
        }
        css_bytes += css.size();
        chapter.css_sources.push_back(keep_mapping ? css : std::string());
        return true;
    }
    bool soft_full() const {
        return chapter.text.size() >= kChapterPartScalars || chapter.runs.size() >= kPartRecords ||
               part_nodes >= kPartRecords || retained_metadata_bytes >= kPartMetadata;
    }
    bool hard_full() const {
        return chapter.text.size() >= kMaxText || chapter.runs.size() >= kPartRecordsHard ||
               part_nodes >= kPartRecordsHard || retained_metadata_bytes >= kPartMetadataHard;
    }
    bool selected() const {
        return match ? (*match)(chapter) : wanted != kLastChapterPart && part == wanted;
    }
    /** Hands the collected part to the caller; the stylesheet state stays with the parser. */
    void capture(bool last) {
        auto css = std::move(chapter.css_sources);
        auto warnings = std::move(chapter.warnings);
        const bool plain = chapter.plain_dom;
        if (!chapter.text.empty() && chapter.text.back() == U'\n') chapter.text.pop_back();
        chapter.part = part;
        chapter.last_part = last;
        result = std::move(chapter);
        chapter = Chapter();
        chapter.resource = result.resource;
        chapter.css_sources = std::move(css);
        chapter.warnings = std::move(warnings);
        chapter.plain_dom = plain;
        captured = true;
    }
    /**
     * Closes the collected part because more text follows. Returns false when parsing stops
     * here: the wanted part is complete and nothing later in the document is needed.
     */
    bool next_part() {
        const bool inside_block = !split_pending;
        const auto none = std::numeric_limits<std::size_t>::max();
        if (inside_block) { finish_block(); chapter.split_node = true; }
        split_pending = false;
        for (const auto& frame : frames)
            if (frame.link < chapter.links.size()) chapter.links[frame.link].end = static_cast<std::uint32_t>(chapter.text.size());
        // The text node being read continues in the new part. At a block boundary none of its
        // text belongs to the closed part, so its entry moves; inside a block both parts keep one.
        std::vector<std::pair<Frame*, SourceTextNode>> open;
        for (auto& frame : frames) {
            if (!frame.in_text || frame.source_node == none) continue;
            open.emplace_back(&frame, keep_mapping ? chapter.source_nodes[frame.source_node] : SourceTextNode{});
            if (keep_mapping && !inside_block && frame.source_node + 1 == chapter.source_nodes.size()) chapter.source_nodes.pop_back();
        }
        // A whitespace-only first child of an element that is still open decides how KOReader
        // numbers the later text nodes of that element, so it stays visible to the next part.
        std::vector<SourceTextNode> carried;
        for (const auto& node : chapter.source_nodes) {
            const auto parent = node.node.substr(0, node.node.rfind('/'));
            if (node.first_child && std::all_of(node.text.begin(), node.text.end(), space) &&
                std::any_of(frames.begin(), frames.end(), [&](const Frame& frame) { return frame.path == parent; }))
                carried.push_back(node);
        }
        if (!captured && selected()) {
            capture(false);
            // Position mapping reads the stylesheets and structure of the whole document.
            if (!keep_mapping) { xml.stop(kPartComplete); return false; }
        }
        chapter.text.clear(); chapter.blocks.clear(); chapter.runs.clear(); chapter.fragments.clear();
        chapter.links.clear(); chapter.images.clear();
        chapter.source_nodes = std::move(carried);
        chapter.split_node = inside_block;
        // Carried entries are not counted: the limits must divide a document identically at
        // every ChapterLoad level, and only a Mapping load has them.
        retained_metadata_bytes = 0; part_nodes = 0;
        block_start = 0; source_count = 0; pending_space = false; last_run_node.clear();
        for (auto& frame : frames) frame.source_node = frame.link = frame.image = none;
        for (auto& [frame, node] : open) {
            ++part_nodes;
            retained_metadata_bytes += frame->node.size();
            frame->source_node = chapter.source_nodes.size();
            if (keep_mapping) { node.first_child = false; chapter.source_nodes.push_back(std::move(node)); }
        }
        ++part;
        return true;
    }
    void finish_block() {
        pending_space = false;
        const auto end = static_cast<std::uint32_t>(chapter.text.size());
        if (end > block_start) { chapter.blocks.push_back({block_start, end, block_kind}); push(U'\n'); }
        block_start = static_cast<std::uint32_t>(chapter.text.size());
        if (soft_full()) split_pending = true;
    }
    void append(char32_t cp, const Anchor* source) {
        // A part is closed only when text follows it, so the last part is never empty.
        if ((split_pending || hard_full()) && !next_part()) return;
        const auto begin = static_cast<std::uint32_t>(chapter.text.size());
        if (!push(cp) || !source) return;
        if (!chapter.runs.empty()) {
            auto& last = chapter.runs.back();
            const auto& node = keep_anchors ? last.source.node : last_run_node;
            if (last.end == begin && node == source->node && last.source.offset + last.end - last.begin == source->offset) { ++last.end; return; }
        }
        retained_metadata_bytes += source->node.size() + source->resource.size();
        if (chapter.runs.size() >= 8192 || retained_metadata_bytes > 512 * 1024) { xml.stop("Chapter source mapping limit exceeded"); return; }
        if (keep_anchors) chapter.runs.push_back({begin, begin + 1, *source});
        else { chapter.runs.push_back({begin, begin + 1, {{}, {}, source->offset}}); last_run_node = source->node; }
    }
    void start(const std::string& name, const char** attrs) {
        const auto simple_image_style = [](const char** attributes) {
            for (const auto* property : {"transform", "style", "opacity", "filter", "mask", "clip-path"})
                if (!attr(attributes, property).empty()) return false;
            const auto aspect = attr(attributes, "preserveAspectRatio");
            return aspect.empty() || aspect == "xMidYMid meet";
        };
        if (!room(0)) return;
        static const std::set<std::string> plain_elements = {"html", "head", "title", "meta", "link", "style", "body", "h1", "h2", "h3", "h4", "h5", "h6", "p", "div", "section", "article", "aside", "header", "footer", "nav", "blockquote", "span", "em", "strong", "b", "i", "u", "s", "a", "ul", "ol", "li", "br", "code", "small", "sub", "sup", "mark", "abbr", "cite", "q", "del", "ins"};
        if (!plain_elements.count(name) || xml.foreign_namespace || !attr(attrs, "space").empty()) chapter.plain_dom = false;
        Frame frame; frame.name = name;
        if (!frames.empty()) {
            auto& parent = frames.back(); parent.in_text = false; parent.has_child = true;
            frame.path = parent.path + '/' + name + '[' + std::to_string(++parent.children[name]) + ']';
            frame.hidden = parent.hidden; frame.body = parent.body; frame.pre = parent.pre;
        } else frame.path = '/' + name + "[1]";
        if (frame.path.size() > kMaxPath) { xml.stop("Source path length limit exceeded"); return; }
        for (auto& ancestor : frames) if (ancestor.name == "svg" && ancestor.image < chapter.images.size()) {
            const bool direct = &ancestor == &frames.back();
            if (direct && name == "image") {
                ++ancestor.svg_images;
                const auto zero = [](const std::string& s) { return s.empty() || s == "0" || s == "0.0"; };
                const auto width = attr(attrs, "width"), height = attr(attrs, "height");
                if (!zero(attr(attrs, "x")) || !zero(attr(attrs, "y")) || !simple_image_style(attrs) ||
                    width != std::to_string(ancestor.svg_width) || height != std::to_string(ancestor.svg_height)) ancestor.simple_svg = false;
                std::string resource, fragment;
                if (resolve(chapter.resource, attr(attrs, "href"), resource, fragment) && fragment.empty())
                    chapter.images[ancestor.image].resource = std::move(resource);
                else ancestor.simple_svg = false;
            } else if (!direct || (name != "title" && name != "desc")) ancestor.simple_svg = false;
        }
        if (name == "body") { frame.body = true; seen_body = true; }
        if (name == "head" || name == "script" || name == "style") frame.hidden = true;
        const auto inline_css = attr(attrs, "style");
        if (!inline_css.empty()) retain_css("_ {" + inline_css + "}");
        if (name == "style" && retain_css("")) frame.css = chapter.css_sources.size() - 1;
        if (name == "link" && token(attr(attrs, "rel"), "stylesheet")) {
            std::string resource, fragment;
            if (css_links.size() < 64 && resolve(chapter.resource, attr(attrs, "href"), resource, fragment) && fragment.empty())
                css_links.push_back(std::move(resource));
            else { chapter.plain_dom = false; warn(chapter, "External or excessive stylesheet references are not loaded"); }
        }
        if (!attr(attrs, "style").empty() || name == "style" || (name == "link" && token(attr(attrs, "rel"), "stylesheet"))) warn(chapter, "CSS is rendered with basic document styles");
        if (name == "script") warn(chapter, "Scripts are not executed");
        frame.block = name == "p" || name == "div" || name == "section" || name == "article" || name == "aside" || name == "blockquote" || name == "li" || name == "pre" || name == "tr" || name == "table" || name == "dl" || name == "dt" || name == "dd" || (name.size() == 2 && name[0] == 'h' && name[1] >= '1' && name[1] <= '6');
        if (name.size() == 2 && name[0] == 'h' && name[1] >= '1' && name[1] <= '6') frame.kind = BlockKind::Heading;
        else if (name == "li") frame.kind = BlockKind::ListItem;
        else if (name == "pre") { frame.kind = BlockKind::Preformatted; frame.pre = true; }
        if (attr(attrs, "space") == "preserve") frame.pre = true;
        if (frame.body && !frame.hidden) {
            if (frame.block || name == "br") { finish_block(); block_kind = frame.kind; }
            if (name == "td" || name == "th") { if (!chapter.text.empty() && chapter.text.back() != U'\n') append(U'\t', nullptr); warn(chapter, "Tables are rendered as text rows"); }
            auto id = attr(attrs, "id");
            if (!id.empty()) {
                if (std::any_of(chapter.fragments.begin(), chapter.fragments.end(), [&](const auto& f) { return f.id == id; })) { xml.stop("Duplicate XHTML fragment id"); return; }
                retained_metadata_bytes += id.size();
                if (chapter.fragments.size() >= 2048 || retained_metadata_bytes > 512 * 1024) { xml.stop("Chapter fragment limit exceeded"); return; }
                chapter.fragments.push_back({id, static_cast<std::uint32_t>(chapter.text.size())});
            }
            if (name == "a" && !attr(attrs, "href").empty()) {
                DocumentLink link; link.begin = static_cast<std::uint32_t>(chapter.text.size());
                auto href = attr(attrs, "href");
                if (!resolve(chapter.resource, href, link.resource, link.fragment)) { link.external = true; link.resource = href; }
                retained_metadata_bytes += link.resource.size() + link.fragment.size();
                if (chapter.links.size() >= 2048 || retained_metadata_bytes > 512 * 1024) { xml.stop("Chapter link limit exceeded"); return; }
                frame.link = chapter.links.size(); chapter.links.push_back(std::move(link));
            }
            if (name == "img" || name == "image" || name == "svg" || name == "math" || name == "audio" || name == "video" || name == "object") {
                finish_block(); block_kind = BlockKind::Image;
                const std::string label = name == "img" || name == "image" ? "[图片：" + (attr(attrs, "alt").empty() ? "未渲染" : attr(attrs, "alt")) + "]" : "[不支持的媒体：" + name + "]";
                std::u32string scalar; decode(label.data(), label.size(), scalar);
                const auto begin = static_cast<std::uint32_t>(chapter.text.size());
                if (scalar.size() > 256) { scalar.resize(255); scalar.push_back(U'…'); }
                for (auto c : scalar) append(c, nullptr);
                retained_metadata_bytes += frame.path.size() + chapter.resource.size();
                if (chapter.runs.size() >= 8192 || retained_metadata_bytes > 512 * 1024) { xml.stop("Chapter source mapping limit exceeded"); return; }
                if (keep_anchors) chapter.runs.push_back({begin, static_cast<std::uint32_t>(chapter.text.size()), {chapter.resource, frame.path, 0}, true});
                else { chapter.runs.push_back({begin, static_cast<std::uint32_t>(chapter.text.size()), {}, true}); last_run_node = frame.path; }
                if (name == "img" || name == "image" || name == "svg") {
                    if (chapter.images.size() >= 256) { xml.stop("Chapter image reference limit exceeded"); return; }
                    DocumentImage image{begin, static_cast<std::uint32_t>(chapter.text.size()), "", attr(attrs, "alt"), {chapter.resource, frame.path, 0}};
                    std::string fragment;
                    if (name != "svg" && (!resolve(chapter.resource, attr(attrs, name == "img" ? "src" : "href"), image.resource, fragment) || !fragment.empty())) image.resource.clear();
                    retained_metadata_bytes += image.resource.size() + image.alt.size() + image.source.node.size();
                    if (retained_metadata_bytes > 512 * 1024) { xml.stop("Chapter image metadata limit exceeded"); return; }
                    frame.image = chapter.images.size();
                    chapter.images.push_back(std::move(image));
                    if (name == "svg") {
                        unsigned left = 1, top = 1; char trailing = 0;
                        const auto view_box = attr(attrs, "viewBox");
                        frame.simple_svg = std::sscanf(view_box.c_str(), "%u %u %u %u %c", &left, &top, &frame.svg_width, &frame.svg_height, &trailing) == 4 &&
                            left == 0 && top == 0 && frame.svg_width && frame.svg_height && simple_image_style(attrs);
                    }
                }
                finish_block(); block_kind = BlockKind::Paragraph;
                warn(chapter, "Images and media use explicit placeholders");
                if (name == "svg" || name == "math" || name == "audio" || name == "video" || name == "object") frame.hidden = true;
            }
        }
        frames.push_back(std::move(frame));
    }
    void end() {
        if (frames.empty()) return;
        auto frame = std::move(frames.back()); frames.pop_back();
        if (frame.name == "svg" && frame.image < chapter.images.size() && (!frame.simple_svg || frame.svg_images != 1)) chapter.images[frame.image].resource.clear();
        if (frame.link != std::numeric_limits<std::size_t>::max()) chapter.links[frame.link].end = static_cast<std::uint32_t>(chapter.text.size());
        if (frame.body && !frame.hidden && frame.block) finish_block();
        if (!frames.empty()) { frames.back().in_text = false; block_kind = frames.back().kind; }
    }
    void characters(const char* bytes, std::size_t size) {
        if (frames.empty() || !room(0)) return;
        auto& frame = frames.back();
        if (frame.css != std::numeric_limits<std::size_t>::max()) {
            if (size <= kMaxCssBytes - css_bytes) {
                if (keep_mapping) chapter.css_sources[frame.css].append(bytes, size);
                css_bytes += size;
            } else { chapter.plain_dom = false; warn(chapter, "Stylesheet mapping limit exceeded"); }
        }
        const bool new_node = !frame.in_text;
        const bool first_child = !frame.has_child;
        frame.has_child = true;
        if (new_node) { ++frame.text_index; frame.source_offset = 0; frame.in_text = true; }
        std::u32string scalars;
        if (!decode(bytes, size, scalars)) { xml.stop("Invalid UTF-8 XML text"); return; }
        source_count += static_cast<std::uint32_t>(scalars.size());
        if (source_count > kMaxText * 2) { xml.stop("Chapter source scalar limit exceeded"); return; }
        if (!frame.body || frame.hidden) {
            frame.source_offset += static_cast<std::uint32_t>(scalars.size());
            return;
        }
        // A node that continues after a part was closed inside it is entered again in the new part.
        if (new_node || frame.source_node == std::numeric_limits<std::size_t>::max()) {
            if (new_node) frame.node = frame.path + "/text()[" + std::to_string(frame.text_index) + ']';
            retained_metadata_bytes += frame.node.size();
            if (part_nodes >= 8192 || retained_metadata_bytes > 512 * 1024) { xml.stop("Chapter source node limit exceeded"); return; }
            ++part_nodes;
            frame.source_node = chapter.source_nodes.size();
            if (keep_mapping) chapter.source_nodes.push_back({frame.node, {}, new_node && first_child});
        }
        if (keep_mapping) {
            auto& raw = chapter.source_nodes[frame.source_node].text;
            const auto size = raw.size() + scalars.size();
            if (size > raw.capacity() && !room(sizeof(char32_t) * std::max(2 * raw.capacity(), size))) return;
            raw += scalars;
        }
        // The loop below may close the part, which invalidates references into the chapter.
        Anchor source{chapter.resource, frame.node, 0};
        for (auto cp : scalars) {
            source.offset = frame.source_offset++;
            if (!frame.pre && space(cp)) {
                if (!pending_space && chapter.text.size() > block_start) { pending_space = true; pending_anchor = source; }
                continue;
            }
            if (pending_space) { append(U' ', &pending_anchor); pending_space = false; }
            append(cp, &source);
            if (!xml.error.empty()) return;
        }
    }
};
}  // namespace

struct EpubDocument::Impl {
    FILE* file = nullptr;
    mz_zip_archive zip{};
    std::size_t zip_allocated = 0;
    struct alignas(std::max_align_t) Allocation { std::size_t size; };
    static void* allocate(void* opaque, std::size_t count, std::size_t size) { return reallocate(opaque, nullptr, count, size); }
    static void release(void* opaque, void* pointer) {
        if (!pointer) return;
        auto* header = static_cast<Allocation*>(pointer) - 1;
        static_cast<Impl*>(opaque)->zip_allocated -= header->size;
        std::free(header);
    }
    static void* reallocate(void* opaque, void* pointer, std::size_t count, std::size_t size) {
        auto& allocated = static_cast<Impl*>(opaque)->zip_allocated;
        constexpr std::size_t budget = 2 * 1024 * 1024;
        if (size && count > budget / size) return nullptr;
        const auto bytes = count * size;
        auto* previous = pointer ? static_cast<Allocation*>(pointer) - 1 : nullptr;
        const auto previous_size = previous ? previous->size : 0;
        if (bytes > budget - (allocated - previous_size)) return nullptr;
        auto* result = static_cast<Allocation*>(std::realloc(previous, sizeof(Allocation) + bytes));
        if (!result) return nullptr;
        allocated = allocated - previous_size + bytes; result->size = bytes;
        return result + 1;
    }
    DocumentMetadata metadata;
    std::vector<SpineItem> spine;
    std::vector<TocEntry> toc;
    std::string cover, cover_page;
    ~Impl() { if (zip.m_pState) mz_zip_reader_end(&zip); if (file) std::fclose(file); }
    static std::size_t read(void* opaque, mz_uint64 offset, void* buffer, std::size_t size) {
        auto* self = static_cast<Impl*>(opaque);
        if (offset > kMaxBook || std::fseek(self->file, static_cast<long>(offset), SEEK_SET) != 0) return 0;
        return std::fread(buffer, 1, size, self->file);
    }
    bool load(std::size_t index, Chapter& chapter, std::string& error, ChapterLoad detail,
              std::uint32_t wanted, const std::function<bool(const Chapter&)>* match, const std::function<bool()>& stop = {});
    bool parse(const std::string& path, Xml& xml, std::string& error, std::uint32_t max_bytes = kMaxXmlBytes, const std::function<bool()>& stop = {}) {
        const StageTimer timer{"xml_read_inflate_parse"};
        const int index = mz_zip_reader_locate_file(&zip, path.c_str(), nullptr, MZ_ZIP_FLAG_CASE_SENSITIVE);
        mz_zip_archive_file_stat stat{};
        if (index < 0 || !mz_zip_reader_file_stat(&zip, index, &stat)) return fail(error, "Missing EPUB resource: " + path);
        if (stat.m_uncomp_size > max_bytes) return fail(error, "EPUB XML resource exceeds limit: " + path);
        struct Feed {
            Xml& xml;
            const std::function<bool()>& stop;
            bool cancelled = false;
        } feed{xml, stop};
        const bool extracted = mz_zip_reader_extract_to_callback(&zip, index, [](void* opaque, mz_uint64, const void* data, std::size_t count) -> std::size_t {
            auto& feed = *static_cast<Feed*>(opaque);
            for (std::size_t offset = 0; offset < count;) {
                if (feed.stop && feed.stop()) {
                    feed.cancelled = true;
                    return 0;
                }
                const auto size = std::min<std::size_t>(4096, count - offset);
                if (!feed.xml.feed(static_cast<const char*>(data) + offset, size, false)) return 0;
                offset += size;
            }
            return count;
        }, &feed, 0);
        if (feed.cancelled || (stop && stop())) {
            xml.error = kChapterCancelled;
            return fail(error, kChapterCancelled);
        }
        if (!extracted || !xml.feed("", 0, true)) return fail(error, xml.error.empty() ? "Invalid compressed EPUB resource: " + path : xml.error + ": " + path);
        return true;
    }
};

EpubDocument::EpubDocument() : impl_(new Impl) {}
EpubDocument::~EpubDocument() = default;
void EpubDocument::close() { impl_.reset(new Impl); }
const DocumentMetadata& EpubDocument::metadata() const { return impl_->metadata; }
const std::vector<SpineItem>& EpubDocument::spine() const { return impl_->spine; }
const std::vector<TocEntry>& EpubDocument::toc() const { return impl_->toc; }

bool EpubDocument::open(const std::string& path, std::string& error) {
    return open_impl(path, error, false);
}

std::uint64_t identity_hash_count() { return identity_scans.load(std::memory_order_relaxed); }

bool EpubDocument::open_verified(const std::string& path, const DocumentMetadata& identity, std::string& error) {
    const auto hex = [](const std::string& s, std::size_t size) {
        return s.size() == size && std::all_of(s.begin(), s.end(), [](char c) {
            return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        });
    };
    if (!hex(identity.book_id, 64) || !hex(identity.partial_md5, 32)) return fail(error, "Invalid verified EPUB identity");
    const auto copy = identity;
    return open_impl(path, error, false, &copy);
}

bool EpubDocument::open_impl(const std::string& path, std::string& error, bool cover_only, const DocumentMetadata* identity) {
    const StageTimer timer{"epub_open"};
    close(); error.clear();
    std::unique_ptr<Impl> next(new Impl);
    next->file = std::fopen(path.c_str(), "rb");
    if (!next->file) return fail(error, "Cannot open EPUB");
    if (std::fseek(next->file, 0, SEEK_END) != 0) return fail(error, "Cannot seek EPUB");
    const auto length = std::ftell(next->file);
    if (length < 22 || static_cast<std::uint64_t>(length) > kMaxBook) return fail(error, "EPUB file size is invalid or exceeds 256 MiB");
    std::vector<unsigned char> tail(static_cast<std::size_t>(std::min<long>(length, 65557)));
    if (Impl::read(next.get(), length - tail.size(), tail.data(), tail.size()) != tail.size()) return fail(error, "Cannot read ZIP directory");
    bool directory_ok = false;
    for (std::size_t i = tail.size() - 22;; --i) {
        const auto* p = tail.data() + i;
        if (le32(p) == 0x06054b50 && i + 22 + le16(p + 20) == tail.size()) {
            const auto entries = le16(p + 10); const auto directory_size = le32(p + 12), directory_offset = le32(p + 16);
            directory_ok = !(i >= 20 && le32(p - 20) == 0x07064b50) && le16(p + 4) == 0 && le16(p + 6) == 0 && le16(p + 8) == entries && entries > 0 && entries <= 4096 && directory_size <= 1024 * 1024 && std::uint64_t(directory_offset) + directory_size <= std::uint64_t(length) - 22;
            break;
        }
        if (i == 0) break;
    }
    if (!directory_ok) return fail(error, "Unsupported or oversized ZIP directory");
    next->zip.m_pRead = Impl::read; next->zip.m_pIO_opaque = next.get();
    next->zip.m_pAlloc = Impl::allocate; next->zip.m_pFree = Impl::release;
    next->zip.m_pRealloc = Impl::reallocate; next->zip.m_pAlloc_opaque = next.get();
    if (!mz_zip_reader_init(&next->zip, length, 0)) return fail(error, "Invalid EPUB ZIP or ZIP memory limit exceeded");
    if (mz_zip_is_zip64(&next->zip) || mz_zip_reader_get_num_files(&next->zip) > 4096) return fail(error, "ZIP64 or oversized ZIP directory is unsupported");
    std::set<std::string> names;
    std::uint64_t total = 0;
    for (mz_uint i = 0; i < mz_zip_reader_get_num_files(&next->zip); ++i) {
        mz_zip_archive_file_stat stat{};
        if (!mz_zip_reader_file_stat(&next->zip, i, &stat)) return fail(error, "Invalid ZIP entry");
        if (mz_zip_reader_get_filename(&next->zip, i, nullptr, 0) > 512) return fail(error, "ZIP path exceeds limit");
        const std::string name = stat.m_filename; std::string canonical; std::u32string unicode;
        const bool directory = !name.empty() && name.back() == '/';
        if (!decode(name.data(), name.size(), unicode) || !path_parts(name, canonical) || canonical != (directory ? name.substr(0, name.size() - 1) : name) || !names.insert(name).second) return fail(error, "Unsafe or duplicate ZIP path");
        const auto file_type = (stat.m_external_attr >> 16) & 0170000;
        if ((stat.m_bit_flag & 1) || (stat.m_method != 0 && stat.m_method != 8) || file_type == 0120000) return fail(error, "Encrypted, linked or unsupported ZIP entry");
        total += stat.m_uncomp_size;
        if (stat.m_uncomp_size > 32 * 1024 * 1024 || total > 512ULL * 1024 * 1024 || (stat.m_uncomp_size > 4096 && stat.m_uncomp_size > std::max<mz_uint64>(1, stat.m_comp_size) * 1000)) return fail(error, "ZIP expansion limit exceeded");
    }
    if (names.count("META-INF/encryption.xml")) return fail(error, "Encrypted or obfuscated EPUB is unsupported");
    char mime[21]{};
    if (!mz_zip_reader_extract_file_to_mem(&next->zip, "mimetype", mime, 20, 0) || std::string(mime) != "application/epub+zip") return fail(error, "Invalid EPUB mimetype");
    if (identity) {
        next->metadata.book_id = identity->book_id;
        next->metadata.partial_md5 = identity->partial_md5;
    } else if (!cover_only) {
        identity_scans.fetch_add(1, std::memory_order_relaxed);
        const StageTimer timer{"identity_read_hash"};
        Hash sha(false), md5(true); std::array<unsigned char, 8192> buffer{};
        if (std::fseek(next->file, 0, SEEK_SET) != 0) return fail(error, "Cannot hash EPUB");
        for (;;) {
            const auto size = std::fread(buffer.data(), 1, buffer.size(), next->file);
            if (size && !sha.update(buffer.data(), size)) return fail(error, "SHA-256 failed");
            if (size < buffer.size()) { if (std::ferror(next->file)) return fail(error, "Cannot hash EPUB"); break; }
        }
        for (std::uint64_t offset = 0;; offset = offset == 0 ? 1024 : offset * 4) {
            if (offset >= static_cast<std::uint64_t>(length) || offset > 1073741824ULL) break;
            const auto size = static_cast<std::size_t>(std::min<std::uint64_t>(1024, length - offset));
            if (Impl::read(next.get(), offset, buffer.data(), size) != size || !md5.update(buffer.data(), size)) return fail(error, "Partial MD5 failed");
        }
        if (!sha.finish(next->metadata.book_id) || !md5.finish(next->metadata.partial_md5)) return fail(error, "EPUB identity hash failed");
    }
    XmlTree container;
    if (!next->parse("META-INF/container.xml", container.xml, error, 262144)) return false;
    std::string package;
    for (const auto& n : container.nodes) if (n.name == "rootfile" && (get(n, "media-type").empty() || get(n, "media-type") == "application/oebps-package+xml")) { package = get(n, "full-path"); break; }
    std::string normalized;
    if (!path_parts(package, normalized) || package != normalized) return fail(error, "Invalid package path");
    XmlTree opf;
    if (!next->parse(package, opf.xml, error, 262144)) return false;
    if (opf.nodes.empty() || opf.nodes[0].name != "package") return fail(error, "Missing EPUB package");
    struct Item { std::string path, type, properties; };
    std::map<std::string, Item> manifest;
    std::string navigation, ncx, cover_id;
    for (const auto& n : opf.nodes) {
        const auto& parent = opf.nodes[n.parent];
        if (parent.name == "metadata") {
            if (n.name == "meta" && get(n, "name") == "cover") cover_id = get(n, "content");
            if (n.name == "title" && next->metadata.title.empty()) next->metadata.title = trim(n.text);
            if (n.name == "creator" && next->metadata.author.empty()) next->metadata.author = trim(n.text);
            if (n.name == "language" && next->metadata.language.empty()) next->metadata.language = trim(n.text);
        }
        if (n.name == "item" && parent.name == "manifest") {
            std::string resource, fragment;
            const auto id = get(n, "id");
            if (id.empty() || manifest.count(id) || !resolve(package, get(n, "href"), resource, fragment) || !fragment.empty()) return fail(error, "Invalid manifest resource");
            manifest[id] = {resource, get(n, "media-type"), get(n, "properties")};
            if (token(get(n, "properties"), "cover-image") && next->cover.empty()) next->cover = resource;
            if (token(get(n, "properties"), "nav")) navigation = resource;
            if (get(n, "media-type") == "application/x-dtbncx+xml") ncx = resource;
        }
    }
    if (next->cover.empty() && manifest.count(cover_id)) next->cover = manifest.at(cover_id).path;
    for (const auto& n : opf.nodes) if (n.name == "reference" && opf.nodes[n.parent].name == "guide" && token(get(n, "type"), "cover")) {
        std::string resource, fragment;
        if (resolve(package, get(n, "href"), resource, fragment)) next->cover_page = resource;
    }
    for (const auto& n : opf.nodes) if (n.name == "itemref" && opf.nodes[n.parent].name == "spine") {
        const auto id = get(n, "idref"); auto item = manifest.find(id);
        if (item == manifest.end() || item->second.type != "application/xhtml+xml" || !names.count(item->second.path)) return fail(error, "Unsupported or missing spine resource");
        mz_zip_archive_file_stat stat{};
        const int member = mz_zip_reader_locate_file(&next->zip, item->second.path.c_str(), nullptr, MZ_ZIP_FLAG_CASE_SENSITIVE);
        if (member < 0 || !mz_zip_reader_file_stat(&next->zip, member, &stat)) return fail(error, "Unsupported or missing spine resource");
        next->spine.push_back({item->second.path, id, get(n, "linear") != "no", static_cast<std::uint32_t>(stat.m_uncomp_size)});
    }
    if (next->spine.empty()) return fail(error, "EPUB spine is empty");
    if (next->metadata.title.empty()) { auto slash = path.rfind('/'); next->metadata.title = path.substr(slash == std::string::npos ? 0 : slash + 1); }
    if (cover_only) { impl_ = std::move(next); return true; }
    if (!navigation.empty()) {
        XmlTree nav;
        if (!next->parse(navigation, nav.xml, error, 262144)) return false;
        for (std::size_t i = 0; i < nav.nodes.size(); ++i) {
            const auto& n = nav.nodes[i];
            if (n.name != "a") continue;
            bool in_toc = false; unsigned depth = 0;
            for (auto p = n.parent; p > 0; p = nav.nodes[p].parent) {
                if (nav.nodes[p].name == "nav" && token(get(nav.nodes[p], "type"), "toc")) in_toc = true;
                if (nav.nodes[p].name == "ol") ++depth;
            }
            if (!in_toc) continue;
            TocEntry entry; entry.depth = depth > 0 ? depth - 1 : 0; entry.label = trim(n.text);
            if (resolve(navigation, get(n, "href"), entry.resource, entry.fragment)) next->toc.push_back(std::move(entry));
        }
    } else if (!ncx.empty()) {
        XmlTree nav;
        if (!next->parse(ncx, nav.xml, error, 262144)) return false;
        for (std::size_t i = 0; i < nav.nodes.size(); ++i) if (nav.nodes[i].name == "navPoint") {
            TocEntry entry;
            for (auto p = nav.nodes[i].parent; p > 0; p = nav.nodes[p].parent) if (nav.nodes[p].name == "navPoint") ++entry.depth;
            for (std::size_t j = i + 1; j < nav.nodes.size(); ++j) {
                auto p = nav.nodes[j].parent;
                while (p > i && nav.nodes[p].name != "navPoint") p = nav.nodes[p].parent;
                if (p != i) continue;
                if (nav.nodes[j].name == "text") entry.label = trim(nav.nodes[j].text);
                if (nav.nodes[j].name == "content") resolve(ncx, get(nav.nodes[j], "src"), entry.resource, entry.fragment);
            }
            if (!entry.resource.empty()) next->toc.push_back(std::move(entry));
        }
    }
    if (next->toc.empty()) for (const auto& item : next->spine) next->toc.push_back({item.id, item.resource, "", 0});
    impl_ = std::move(next);
    return true;
}

bool EpubDocument::read_cover(const std::string& path, std::vector<std::uint8_t>& bytes, std::string& error) {
    bytes.clear(); error.clear();
    EpubDocument book;
    if (!book.open_impl(path, error, true)) return false;
    if (!book.impl_->cover.empty() && book.read_image(book.impl_->cover, bytes, error)) return true;
    Chapter unused;
    const auto resource = book.impl_->cover_page.empty() ? book.impl_->spine.front().resource : book.impl_->cover_page;
    if (!book.impl_->cover_page.empty() && book.read_image(resource, bytes, error)) return true;
    error.clear();
    // A cover page is a single image; a page long enough to span parts is not one.
    ChapterParser parser(unused, resource);
    if (!book.impl_->parse(resource, parser.xml, error, 262144)) return false;
    parser.finish_block();
    const auto& chapter = parser.chapter;
    if (parser.part != 0 || chapter.images.size() != 1 || chapter.blocks.size() != 1 || chapter.blocks.front().kind != BlockKind::Image)
        return fail(error, "EPUB has no supported cover");
    return book.read_image(chapter.images.front().resource, bytes, error);
}

bool chapter_memory_available(std::size_t bytes) { return memory_room(bytes); }

void set_chapter_memory_probe(bool (*probe)(std::size_t bytes)) {
    memory_room = probe ? probe : platform_memory_room;
}

std::string sha256_file(const std::string& path, const std::function<bool()>& stop) {
    std::unique_ptr<FILE, decltype(&std::fclose)> file(std::fopen(path.c_str(), "rb"), &std::fclose);
    if (!file) return {};
    Hash sha(false);
    std::array<unsigned char, 8192> bytes;
    while (!stop || !stop()) {
        const auto count = std::fread(bytes.data(), 1, bytes.size(), file.get());
        if (count && !sha.update(bytes.data(), count)) return {};
        if (count < bytes.size()) {
            std::string digest;
            return !std::ferror(file.get()) && sha.finish(digest) ? digest : std::string();
        }
    }
    return {};
}

std::string md5_hex(std::string_view bytes) {
    Hash md5(true);
    std::string digest;
    md5.update(reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size());
    return md5.finish(digest) ? digest : std::string();
}

void release(Chapter& chapter) {
    // Assigning an empty value may keep the capacity of the strings; swapping hands the buffers
    // to a temporary that frees them.
    Chapter empty;
    std::swap(chapter, empty);
}

bool EpubDocument::load_chapter(std::size_t index, Chapter& chapter, std::string& error, ChapterLoad detail, std::uint32_t part,
                                const std::function<bool()>& stop) {
    return impl_->load(index, chapter, error, detail, part, nullptr, stop);
}
bool EpubDocument::find_chapter_part(std::size_t index, Chapter& chapter, std::string& error, ChapterLoad detail,
                                     const std::function<bool(const Chapter&)>& match) {
    return impl_->load(index, chapter, error, detail, 0, &match);
}
bool EpubDocument::Impl::load(std::size_t index, Chapter& chapter, std::string& error, ChapterLoad detail,
                              std::uint32_t wanted, const std::function<bool(const Chapter&)>* match, const std::function<bool()>& stop) {
    pico_page::release(chapter); error.clear();
    if (stop && stop()) return fail(error, kChapterCancelled);
    if (!file || index >= spine.size()) return fail(error, "Invalid EPUB chapter index");
    ChapterParser parser(chapter, spine[index].resource);
    parser.wanted = wanted;
    parser.match = match;
    if (detail != ChapterLoad::Mapping) {
        parser.keep_mapping = false;
        parser.keep_anchors = detail == ChapterLoad::Reading;
        // Decoded text holds at most one scalar per XHTML byte and a part is closed at kMaxText;
        // reserving that avoids the transient copies of geometric growth.
        const auto scalars = std::min<std::size_t>(spine[index].bytes, kMaxText) + 1024;
        if (memory_room(scalars * sizeof(char32_t))) parser.chapter.text.reserve(scalars);
        else if (detail == ChapterLoad::Layout) return fail(error, std::string(kChapterMemoryError) + ": " + spine[index].resource);
    }
    const bool parsed = parse(spine[index].resource, parser.xml, error, kMaxXmlBytes, stop);
    if (!parsed && parser.xml.error != kPartComplete) { pico_page::release(chapter); return false; }
    error.clear();
    if (parsed) {
        parser.finish_block();
        if (!parser.seen_body || (parser.part == 0 && !parser.captured && parser.chapter.text.empty())) {
            pico_page::release(chapter);
            return fail(error, "Chapter contains no supported body content");
        }
        if (!parser.captured && (wanted == kLastChapterPart || parser.selected())) parser.capture(true);
    }
    if (!parser.captured) { pico_page::release(chapter); return fail(error, "Chapter part is not available: " + spine[index].resource); }
    if (detail == ChapterLoad::Mapping) {
        for (const auto& path : parser.css_links) {
            if (stop && stop()) { pico_page::release(chapter); return fail(error, kChapterCancelled); }
            mz_zip_archive_file_stat info{};
            const auto member = mz_zip_reader_locate_file(&zip, path.c_str(), nullptr, MZ_ZIP_FLAG_CASE_SENSITIVE);
            if (member < 0 || !mz_zip_reader_file_stat(&zip, member, &info) || info.m_uncomp_size > kMaxCssBytes - parser.css_bytes) {
                parser.chapter.plain_dom = false; warn(parser.chapter, "Missing or oversized stylesheet is not loaded"); continue;
            }
            std::string css(static_cast<std::size_t>(info.m_uncomp_size), '\0');
            if (!mz_zip_reader_extract_to_mem(&zip, member, css.data(), css.size(), 0)) {
                parser.chapter.plain_dom = false; warn(parser.chapter, "Invalid stylesheet is not loaded"); continue;
            }
            if (stop && stop()) { pico_page::release(chapter); return fail(error, kChapterCancelled); }
            parser.retain_css(css);
        }
        // Stylesheets and structure are properties of the whole document, known only now.
        chapter.css_sources = std::move(parser.chapter.css_sources);
        chapter.warnings = std::move(parser.chapter.warnings);
        chapter.plain_dom = parser.chapter.plain_dom;
    }
    // A chapter kept for reading gives back the unused part of its reservation.
    if (detail == ChapterLoad::Reading && chapter.text.capacity() - chapter.text.size() > 4096 &&
        memory_room(chapter.text.size() * sizeof(char32_t))) {
        std::u32string exact(chapter.text.begin(), chapter.text.end());
        chapter.text.swap(exact);
    }
    return true;
}
bool EpubDocument::chapter_index_for(const std::string& resource, std::size_t& index) const {
    for (std::size_t i = 0; i < impl_->spine.size(); ++i) if (impl_->spine[i].resource == resource) { index = i; return true; }
    return false;
}
bool EpubDocument::read_image(const std::string& resource, std::vector<std::uint8_t>& bytes, std::string& error,
                              const std::function<bool()>& stop) {
    const StageTimer timer{"image_read_inflate"};
    bytes.clear(); error.clear();
    if (stop && stop()) return fail(error, "Image load cancelled");
    std::string canonical;
    if (!impl_->file || resource.empty() || !path_parts(resource, canonical) || canonical != resource) return fail(error, "Invalid internal image path");
    const int index = mz_zip_reader_locate_file(&impl_->zip, resource.c_str(), nullptr, MZ_ZIP_FLAG_CASE_SENSITIVE);
    mz_zip_archive_file_stat stat{};
    if (index < 0 || !mz_zip_reader_file_stat(&impl_->zip, index, &stat)) return fail(error, "Missing EPUB image");
    if (!stat.m_uncomp_size || stat.m_uncomp_size > 2 * 1024 * 1024) return fail(error, "EPUB image exceeds 2 MiB limit");
    bytes.resize(static_cast<std::size_t>(stat.m_uncomp_size));
    struct Output { std::vector<std::uint8_t>& bytes; const std::function<bool()>& stop; bool cancelled = false; } output{bytes, stop};
    const bool extracted = mz_zip_reader_extract_to_callback(&impl_->zip, index,
        [](void* opaque, mz_uint64 offset, const void* data, std::size_t count) -> std::size_t {
            auto& output = *static_cast<Output*>(opaque);
            if (output.stop && output.stop()) { output.cancelled = true; return 0; }
            if (offset > output.bytes.size() || count > output.bytes.size() - offset) return 0;
            std::memcpy(output.bytes.data() + offset, data, count);
            return count;
        }, &output, 0);
    if (!extracted || output.cancelled || (stop && stop())) {
        bytes.clear(); return fail(error, output.cancelled || (stop && stop()) ? "Image load cancelled" : "Invalid compressed EPUB image");
    }
    const bool png = bytes.size() >= 8 && std::memcmp(bytes.data(), "\x89PNG\r\n\x1a\n", 8) == 0;
    const bool jpeg = bytes.size() >= 3 && bytes[0] == 0xff && bytes[1] == 0xd8 && bytes[2] == 0xff;
    if (!png && !jpeg) { bytes.clear(); return fail(error, "Image format is not PNG or JPEG"); }
    return true;
}
bool Chapter::anchor_at(std::uint32_t offset, Anchor& anchor) const {
    auto run = std::upper_bound(runs.begin(), runs.end(), offset, [](std::uint32_t value, const TextRun& r) { return value < r.begin; });
    if (run == runs.begin()) return false;
    --run;
    if (offset >= run->end) return false;
    anchor = run->source; if (!run->element_reference) anchor.offset += offset - run->begin; return true;
}
bool Chapter::offset_for(const Anchor& anchor, std::uint32_t& offset) const {
    if (anchor.resource != resource) return false;
    for (const auto& run : runs) {
        if (run.source.node != anchor.node) continue;
        if (run.element_reference) { if (anchor.offset == 0) { offset = run.begin; return true; } }
        else if (anchor.offset >= run.source.offset && anchor.offset < run.source.offset + run.end - run.begin) { offset = run.begin + anchor.offset - run.source.offset; return true; }
    }
    return false;
}
bool Chapter::fragment_offset(const std::string& id, std::uint32_t& offset) const {
    if (id.empty()) { offset = 0; return true; }
    for (const auto& target : fragments) if (target.id == id) { offset = target.offset; return true; }
    return false;
}
}  // namespace pico_page
