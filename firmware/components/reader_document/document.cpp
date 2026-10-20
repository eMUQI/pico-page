#include "pico_page/stage_timer.h"
#include "pico_page/document.h"
#include "gb18030.h"
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
std::atomic<std::uint64_t> identity_scans{0}, epub_opens{0};
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
bool txt_path(std::string_view path) {
    if (path.size() < 4) return false;
    path.remove_prefix(path.size() - 4);
    return (path[0] == '.' && (path[1] == 't' || path[1] == 'T') &&
            (path[2] == 'x' || path[2] == 'X') && (path[3] == 't' || path[3] == 'T'));
}
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
        // The XHTML 1.x DTD is never loaded, so its named character entities arrive here. Each is
        // delivered as one scalar of the surrounding text node.
        XML_SetSkippedEntityHandler(parser, [](void* opaque, const char* name, int parameter) {
            auto& x = *static_cast<Xml*>(opaque);
            // U+00A0 through U+00FF in order.
            static const char* const latin1[] = {
                "nbsp", "iexcl", "cent", "pound", "curren", "yen", "brvbar", "sect", "uml", "copy", "ordf", "laquo", "not", "shy", "reg", "macr",
                "deg", "plusmn", "sup2", "sup3", "acute", "micro", "para", "middot", "cedil", "sup1", "ordm", "raquo", "frac14", "frac12", "frac34", "iquest",
                "Agrave", "Aacute", "Acirc", "Atilde", "Auml", "Aring", "AElig", "Ccedil", "Egrave", "Eacute", "Ecirc", "Euml", "Igrave", "Iacute", "Icirc", "Iuml",
                "ETH", "Ntilde", "Ograve", "Oacute", "Ocirc", "Otilde", "Ouml", "times", "Oslash", "Ugrave", "Uacute", "Ucirc", "Uuml", "Yacute", "THORN", "szlig",
                "agrave", "aacute", "acirc", "atilde", "auml", "aring", "aelig", "ccedil", "egrave", "eacute", "ecirc", "euml", "igrave", "iacute", "icirc", "iuml",
                "eth", "ntilde", "ograve", "oacute", "ocirc", "otilde", "ouml", "divide", "oslash", "ugrave", "uacute", "ucirc", "uuml", "yacute", "thorn", "yuml"};
            static const std::pair<const char*, char32_t> other[] = {
                {"OElig", 0x152}, {"oelig", 0x153}, {"Scaron", 0x160}, {"scaron", 0x161}, {"Yuml", 0x178}, {"circ", 0x2c6}, {"tilde", 0x2dc},
                {"ensp", 0x2002}, {"emsp", 0x2003}, {"thinsp", 0x2009}, {"zwnj", 0x200c}, {"zwj", 0x200d}, {"lrm", 0x200e}, {"rlm", 0x200f},
                {"ndash", 0x2013}, {"mdash", 0x2014}, {"lsquo", 0x2018}, {"rsquo", 0x2019}, {"sbquo", 0x201a}, {"ldquo", 0x201c}, {"rdquo", 0x201d}, {"bdquo", 0x201e},
                {"dagger", 0x2020}, {"Dagger", 0x2021}, {"bull", 0x2022}, {"hellip", 0x2026}, {"permil", 0x2030}, {"prime", 0x2032}, {"Prime", 0x2033},
                {"lsaquo", 0x2039}, {"rsaquo", 0x203a}, {"oline", 0x203e}, {"frasl", 0x2044}, {"euro", 0x20ac}, {"trade", 0x2122},
                {"larr", 0x2190}, {"uarr", 0x2191}, {"rarr", 0x2192}, {"darr", 0x2193}, {"harr", 0x2194}, {"minus", 0x2212}, {"infin", 0x221e}, {"ne", 0x2260}, {"le", 0x2264}, {"ge", 0x2265}};
            char32_t scalar = 0;
            if (!parameter) {
                for (std::size_t i = 0; i < std::size(latin1) && !scalar; ++i) if (!std::strcmp(name, latin1[i])) scalar = static_cast<char32_t>(0xa0 + i);
                for (const auto& entity : other) if (!scalar && !std::strcmp(name, entity.first)) scalar = entity.second;
            }
            if (!scalar) { x.stop(std::string("Unsupported XML entity: ") + name); return; }
            // Every listed scalar lies in U+00A0 through U+FFFF: two or three UTF-8 bytes.
            const char utf8[] = {static_cast<char>(0xe0 | scalar >> 12), static_cast<char>(scalar < 0x800 ? 0xc0 | scalar >> 6 : 0x80 | (scalar >> 6 & 0x3f)),
                                 static_cast<char>(0x80 | (scalar & 0x3f))};
            if (x.text) scalar < 0x800 ? x.text(utf8 + 1, 2) : x.text(utf8, 3);
        });
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

constexpr std::size_t kMaxPackageEntries = 4096;
constexpr std::size_t kMaxTocEntries = 8192;
constexpr std::size_t kMaxPackageStrings = 1024 * 1024;
constexpr std::size_t kMaxNavigationLabel = 32768;

// Counts live string payloads, not allocator capacity or record overhead.
struct PackageBudget {
    std::size_t bytes = 0;
    bool add(Xml& xml, std::size_t count) {
        if (count > kMaxPackageStrings - bytes) {
            xml.stop("Package/navigation text limit exceeded"); return false;
        }
        bytes += count; return true;
    }
    bool replace(Xml& xml, std::string& target, std::string value) {
        const auto old = target.size();
        bytes -= old;
        if (!add(xml, value.size())) { bytes += old; return false; }
        target = std::move(value); return true;
    }
    void append(Xml& xml, std::string& target, const char* value, std::size_t count) {
        if (count > kMaxNavigationLabel - target.size()) {
            xml.stop("Package/navigation text limit exceeded"); return;
        }
        if (add(xml, count)) target.append(value, count);
    }
};
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
        std::string background;  // body only: image resource named by its inline style.
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
    const std::function<void(const Chapter&)>* visit = nullptr;
    void visit_part(bool last) {
        if (!visit || !*visit) return;
        chapter.part = part; chapter.last_part = last;
        const bool separator = !chapter.text.empty() && chapter.text.back() == U'\n';
        if (separator) chapter.text.pop_back();
        (*visit)(chapter);
        if (separator) chapter.text.push_back(U'\n');
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
        visit_part(false);
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
    /** Appends a media placeholder as a block of its own, mapped to the node at path. Returns false after stopping the parser. */
    bool place_media(const std::string& label, const std::string& path, std::uint32_t& begin) {
        finish_block();
        // A split left pending by the block before takes effect here, so that the placeholder,
        // its run and the image entry all use offsets of the part that holds them.
        if (split_pending && !next_part()) return false;
        block_kind = BlockKind::Image;
        std::u32string scalar; decode(label.data(), label.size(), scalar);
        begin = static_cast<std::uint32_t>(chapter.text.size());
        if (scalar.size() > 256) { scalar.resize(255); scalar.push_back(U'…'); }
        for (auto c : scalar) append(c, nullptr);
        retained_metadata_bytes += path.size() + chapter.resource.size();
        if (chapter.runs.size() >= 8192 || retained_metadata_bytes > 512 * 1024) { xml.stop("Chapter source mapping limit exceeded"); return false; }
        if (keep_anchors) chapter.runs.push_back({begin, static_cast<std::uint32_t>(chapter.text.size()), {chapter.resource, path, 0}, true});
        else { chapter.runs.push_back({begin, static_cast<std::uint32_t>(chapter.text.size()), {}, true}); last_run_node = path; }
        return true;
    }
    void start(const std::string& name, const char** attrs) {
        const auto simple_image_style = [](const char** attributes) {
            for (const auto* property : {"transform", "style", "opacity", "filter", "mask", "clip-path"})
                if (!attr(attributes, property).empty()) return false;
            // calibre writes "none" on its cover page; the image then covers the whole viewBox and
            // is drawn fitted with its own aspect ratio, as for "xMidYMid meet".
            const auto aspect = attr(attributes, "preserveAspectRatio");
            return aspect.empty() || aspect == "xMidYMid meet" || aspect == "none";
        };
        if (!room(0)) return;
        static const std::set<std::string> plain_elements = {"html", "head", "title", "meta", "link", "style", "body", "h1", "h2", "h3", "h4", "h5", "h6", "p", "div", "section", "article", "aside", "header", "footer", "nav", "blockquote", "span", "em", "strong", "b", "i", "u", "s", "a", "ul", "ol", "li", "br", "code", "small", "sub", "sup", "mark", "abbr", "cite", "q", "del", "ins", "img", "hr"};
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
        if (name == "body") {
            // Only the inline style is read; a background set by a stylesheet rule is not found.
            auto at = inline_css.find("background");
            if (at != std::string::npos) at = inline_css.find("url(", at);
            const auto close = at == std::string::npos ? at : inline_css.find(')', at);
            if (close != std::string::npos) {
                auto url = inline_css.substr(at + 4, close - at - 4);
                const auto first = url.find_first_not_of(" \t'\""), last = url.find_last_not_of(" \t'\"");
                std::string fragment;
                if (first != std::string::npos && (!resolve(chapter.resource, url.substr(first, last - first + 1), frame.background, fragment) || !fragment.empty()))
                    frame.background.clear();
            }
        }
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
                const std::string label = name == "img" || name == "image" ? "[图片：" + (attr(attrs, "alt").empty() ? "未渲染" : attr(attrs, "alt")) + "]" : "[不支持的媒体：" + name + "]";
                std::uint32_t begin;
                if (!place_media(label, frame.path, begin)) return;
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
        // A page whose only content is the body background, such as a title page, shows that image.
        if (!frame.background.empty() && part == 0 && !captured && chapter.text.empty()) {
            std::uint32_t begin;
            if (!place_media("[图片：未渲染]", frame.path, begin)) return;
            chapter.images.push_back({begin, static_cast<std::uint32_t>(chapter.text.size()), frame.background, "", {chapter.resource, frame.path, 0}});
            retained_metadata_bytes += frame.background.size();
            finish_block(); block_kind = BlockKind::Paragraph;
            warn(chapter, "Images and media use explicit placeholders");
        }
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

namespace {
struct ZipTiming {
#ifdef CONFIG_PICO_PAGE_LEGACY_PERF_LOG
    using Clock = std::chrono::steady_clock;
    Clock::time_point started = Clock::now(), checked = started;
    std::int64_t read_us = 0, read_max_us = 0, xml_us = 0, xml_max_us = 0, checkpoint_max_us = 0;
    static std::int64_t elapsed(Clock::time_point start) {
        return std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - start).count();
    }
    void checkpoint() {
        checkpoint_max_us = std::max(checkpoint_max_us, elapsed(checked)); checked = Clock::now();
    }
    ~ZipTiming() {
#ifdef ESP_PLATFORM
        checkpoint();
        const auto total = elapsed(started);
        ESP_LOGI("reader_perf", "zip_stage total_us=%lld read_us=%lld read_max_us=%lld xml_us=%lld xml_max_us=%lld other_us=%lld checkpoint_max_us=%lld",
            static_cast<long long>(total), static_cast<long long>(read_us), static_cast<long long>(read_max_us),
            static_cast<long long>(xml_us), static_cast<long long>(xml_max_us),
            static_cast<long long>(std::max<std::int64_t>(0, total - read_us - xml_us)), static_cast<long long>(checkpoint_max_us));
#endif
    }
#else
    void checkpoint() {}
#endif
};
}

struct EpubDocument::Impl {
    FILE* file = nullptr;
    bool txt = false;
    std::uint32_t txt_bytes = 0;
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
    struct ZipEntry { std::uint32_t member, bytes; };
    static_assert(sizeof(ZipEntry) == 8);
    std::unique_ptr<ZipEntry, decltype(&std::free)> zip_index{nullptr, &std::free};
    std::size_t zip_entries = 0;
    // Names remain owned by miniz; the index is valid until this archive is closed.
    const ZipEntry* find_member(const std::string& path) {
        if (!zip_index) return nullptr;
        const auto* begin = zip_index.get();
        const auto* end = begin + zip_entries;
        char name[512];
        const auto* found = std::lower_bound(begin, end, path, [&](const ZipEntry& entry, const std::string& target) {
            mz_zip_reader_get_filename(&zip, entry.member, name, sizeof(name));
            return std::strcmp(name, target.c_str()) < 0;
        });
        if (found == end) return nullptr;
        mz_zip_reader_get_filename(&zip, found->member, name, sizeof(name));
        return path == name ? found : nullptr;
    }
    std::vector<SpineItem> spine;
    std::vector<TocEntry> toc;
    std::string cover, cover_page;
    ~Impl() { if (zip.m_pState) mz_zip_reader_end(&zip); if (file) std::fclose(file); }
    ZipTiming* read_timing = nullptr;
    static std::size_t read(void* opaque, mz_uint64 offset, void* buffer, std::size_t size) {
        auto* self = static_cast<Impl*>(opaque);
#ifdef CONFIG_PICO_PAGE_LEGACY_PERF_LOG
        const auto started = ZipTiming::Clock::now();
#endif
        const auto count = offset <= kMaxBook && std::fseek(self->file, static_cast<long>(offset), SEEK_SET) == 0
            ? std::fread(buffer, 1, size, self->file) : 0;
#ifdef CONFIG_PICO_PAGE_LEGACY_PERF_LOG
        if (self->read_timing) {
            const auto us = ZipTiming::elapsed(started);
            self->read_timing->read_us += us;
            self->read_timing->read_max_us = std::max(self->read_timing->read_max_us, us);
        }
#endif
        return count;
    }
    bool load(std::size_t index, Chapter& chapter, std::string& error, ChapterLoad detail,
              std::uint32_t wanted, const std::function<bool(const Chapter&)>* match, const std::function<bool()>& stop,
              const std::function<void(const Chapter&)>& visit);
    bool load_txt(Chapter& chapter, std::string& error, ChapterLoad detail, std::uint32_t wanted,
                  const std::function<bool(const Chapter&)>* match, const std::function<bool()>& stop,
                  const std::function<void(const Chapter&)>& visit);
    bool parse(const std::string& path, Xml& xml, std::string& error, std::uint32_t max_bytes = kMaxXmlBytes, const std::function<bool()>& stop = {}) {
        const StageTimer timer{"xml_read_inflate_parse", 0};
        const auto* entry = find_member(path);
        if (!entry) return fail(error, "Missing EPUB resource: " + path);
        if (entry->bytes > max_bytes) return fail(error, "EPUB XML resource exceeds limit: " + path);
        ZipTiming timing;
        struct ActiveRead {
            Impl& owner;
            ActiveRead(Impl& owner, ZipTiming& timing) : owner(owner) { owner.read_timing = &timing; }
            ~ActiveRead() { owner.read_timing = nullptr; }
        } active(*this, timing);
        struct Feed {
            Xml& xml;
            ZipTiming& timing;
            const std::function<bool()>& stop;
            bool cancelled = false;
        } feed{xml, timing, stop};
        const bool extracted = mz_zip_reader_extract_to_callback(&zip, entry->member, [](void* opaque, mz_uint64, const void* data, std::size_t count) -> std::size_t {
            auto& feed = *static_cast<Feed*>(opaque);
            for (std::size_t offset = 0; offset < count;) {
                feed.timing.checkpoint();
                if (feed.stop && feed.stop()) {
                    feed.cancelled = true;
                    return 0;
                }
                const auto size = std::min<std::size_t>(4096, count - offset);
#ifdef CONFIG_PICO_PAGE_LEGACY_PERF_LOG
                const auto started = ZipTiming::Clock::now();
#endif
                const bool parsed = feed.xml.feed(static_cast<const char*>(data) + offset, size, false);
#ifdef CONFIG_PICO_PAGE_LEGACY_PERF_LOG
                const auto us = ZipTiming::elapsed(started);
                feed.timing.xml_us += us; feed.timing.xml_max_us = std::max(feed.timing.xml_max_us, us);
#endif
                if (!parsed) return 0;
                offset += size;
            }
            return count;
        }, &feed, 0);
        timing.checkpoint();
        if (feed.cancelled || (stop && stop())) {
            xml.error = kChapterCancelled;
            return fail(error, kChapterCancelled);
        }
        if (!extracted) return fail(error, xml.error.empty() ? "Invalid compressed EPUB resource: " + path : xml.error + ": " + path);
#ifdef CONFIG_PICO_PAGE_LEGACY_PERF_LOG
        const auto started = ZipTiming::Clock::now();
#endif
        const bool finished = xml.feed("", 0, true);
#ifdef CONFIG_PICO_PAGE_LEGACY_PERF_LOG
        const auto us = ZipTiming::elapsed(started);
        timing.xml_us += us; timing.xml_max_us = std::max(timing.xml_max_us, us);
#endif
        if (!finished) return fail(error, xml.error + ": " + path);
        return true;
    }
};

EpubDocument::EpubDocument() : impl_(new Impl) {}
EpubDocument::~EpubDocument() = default;
EpubDocument::EpubDocument(EpubDocument&&) noexcept = default;
EpubDocument& EpubDocument::operator=(EpubDocument&&) noexcept = default;
void EpubDocument::close() { impl_.reset(new Impl); }
const DocumentMetadata& EpubDocument::metadata() const { return impl_->metadata; }
const std::vector<SpineItem>& EpubDocument::spine() const { return impl_->spine; }
const std::vector<TocEntry>& EpubDocument::toc() const { return impl_->toc; }

bool EpubDocument::open(const std::string& path, std::string& error, const std::function<bool()>& stop) {
    return open_impl(path, error, false, nullptr, stop);
}

std::uint64_t identity_hash_count() { return identity_scans.load(std::memory_order_relaxed); }
std::uint64_t epub_open_count() { return epub_opens.load(std::memory_order_relaxed); }

bool EpubDocument::open_verified(const std::string& path, const DocumentMetadata& identity, std::string& error, const std::function<bool()>& stop) {
    const auto hex = [](const std::string& s, std::size_t size) {
        return s.size() == size && std::all_of(s.begin(), s.end(), [](char c) {
            return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        });
    };
    if (!hex(identity.book_id, 64) || !hex(identity.partial_md5, 32)) return fail(error, "Invalid verified book identity");
    const auto copy = identity;
    return open_impl(path, error, false, &copy, stop);
}

bool EpubDocument::open_impl(const std::string& path, std::string& error, bool cover_only, const DocumentMetadata* identity, const std::function<bool()>& stop) {
    if (txt_path(path)) {
        close(); error.clear();
        if (cover_only) return fail(error, "TXT 没有内嵌封面");
        if (stop && stop()) return fail(error, "TXT 读取已取消");
        std::unique_ptr<Impl> next(new Impl);
        next->file = std::fopen(path.c_str(), "rb");
        if (!next->file) return fail(error, "无法打开 TXT 文件");
        if (std::fseek(next->file, 0, SEEK_END) != 0) return fail(error, "无法读取 TXT 文件");
        const auto length = std::ftell(next->file);
        if (length <= 0 || static_cast<std::uint64_t>(length) > 64ULL * 1024 * 1024)
            return fail(error, "TXT 文件为空或超过 64 MiB");
        next->txt = true;
        next->txt_bytes = static_cast<std::uint32_t>(length);
        if (identity) {
            next->metadata.book_id = identity->book_id;
            next->metadata.partial_md5 = identity->partial_md5;
            next->metadata.txt_gb18030 = identity->txt_gb18030;
            next->metadata.txt_offsets = identity->txt_offsets;
            if (identity->txt_gb18030 && (identity->txt_offsets.size() < 2 ||
                identity->txt_offsets.front() != 0 || identity->txt_offsets.back() != next->txt_bytes ||
                !std::is_sorted(identity->txt_offsets.begin(), identity->txt_offsets.end())))
                return fail(error, "TXT 阅读位置索引无效");
        } else {
            identity_scans.fetch_add(1, std::memory_order_relaxed);
            Hash sha(false), md5(true);
            std::array<unsigned char, 8192> buffer{};
            if (std::fseek(next->file, 0, SEEK_SET) != 0) return fail(error, "无法读取 TXT 文件");
            unsigned remaining = 0;
            char32_t cp = 0, minimum = 0;
            bool utf8_valid = true, utf8_has_text = false;
            bool gb_valid = true, gb_has_text = false;
            gb18030::Decoder gb_decoder;
            char32_t previous_gb_scalar = 0;
            std::vector<std::uint32_t> gb_offsets{0};
            std::uint32_t position = 0, next_boundary = kTxtPartBytes;
            for (;;) {
                if (stop && stop()) return fail(error, "TXT 读取已取消");
                const auto count = std::fread(buffer.data(), 1, buffer.size(), next->file);
                if (count && !sha.update(buffer.data(), count)) return fail(error, "TXT SHA-256 failed");
                for (std::size_t i = 0; i < count; ++i) {
                    const auto byte = buffer[i];
                    if (byte == 0) return fail(error, "TXT 文件包含 NUL 字节");
                    if (utf8_valid) {
                        if (remaining) {
                            if ((byte & 0xc0) != 0x80) utf8_valid = false;
                            else {
                                cp = (cp << 6) | (byte & 63);
                                if (--remaining == 0 && (cp < minimum || cp > 0x10ffff ||
                                    (cp >= 0xd800 && cp <= 0xdfff))) utf8_valid = false;
                            }
                        } else if (byte < 0x80) cp = byte;
                        else if (byte >= 0xc2 && byte <= 0xdf) { cp = byte & 31; minimum = 0x80; remaining = 1; }
                        else if (byte >= 0xe0 && byte <= 0xef) { cp = byte & 15; minimum = 0x800; remaining = 2; }
                        else if (byte >= 0xf0 && byte <= 0xf4) { cp = byte & 7; minimum = 0x10000; remaining = 3; }
                        else utf8_valid = false;
                        if (utf8_valid && !remaining && !space(cp) && cp != 0xfeff) utf8_has_text = true;
                    }
                    if (gb_valid) {
                        if (!gb_decoder.pending() && position + i >= next_boundary && next_boundary < next->txt_bytes) {
                            const auto split = static_cast<std::uint32_t>(position + i +
                                (byte == '\n' && previous_gb_scalar == U'\r'));
                            gb_offsets.push_back(split);
                            next_boundary += kTxtPartBytes;
                        }
                        char32_t scalar = 0;
                        const auto result = gb_decoder.feed(byte, scalar);
                        if (result == gb18030::Decoder::Result::Invalid) gb_valid = false;
                        else if (result == gb18030::Decoder::Result::Complete) {
                            previous_gb_scalar = scalar;
                            if (!space(scalar) && scalar != 0xfeff) gb_has_text = true;
                        }
                    }
                }
                position += static_cast<std::uint32_t>(count);
                if (count < buffer.size()) {
                    if (std::ferror(next->file)) return fail(error, "无法读取 TXT 文件");
                    break;
                }
            }
            utf8_valid = utf8_valid && remaining == 0;
            gb_valid = gb_valid && !gb_decoder.pending();
            if (utf8_valid ? !utf8_has_text : !gb_valid || !gb_has_text)
                return fail(error, utf8_valid || gb_valid ? "TXT 没有可阅读的文字" :
                    "TXT 编码不受支持（支持 UTF-8、GB18030）");
            if (!utf8_valid) {
                next->metadata.txt_gb18030 = true;
                if (gb_offsets.back() != next->txt_bytes) gb_offsets.push_back(next->txt_bytes);
                next->metadata.txt_offsets = std::move(gb_offsets);
            }
            if (!sha.finish(next->metadata.book_id)) return fail(error, "TXT SHA-256 failed");
            for (std::uint64_t offset = 0; offset < static_cast<std::uint64_t>(length); offset = offset == 0 ? 1024 : offset * 4) {
                const auto size = static_cast<std::size_t>(std::min<std::uint64_t>(1024, length - offset));
                if (Impl::read(next.get(), offset, buffer.data(), size) != size || !md5.update(buffer.data(), size))
                    return fail(error, "TXT partial MD5 failed");
            }
            if (!md5.finish(next->metadata.partial_md5)) return fail(error, "TXT partial MD5 failed");
        }
        const auto slash = path.rfind('/');
        next->metadata.title = path.substr(slash == std::string::npos ? 0 : slash + 1);
        next->metadata.title.resize(next->metadata.title.size() - 4);
        next->spine.push_back({"text", "text", true, static_cast<std::uint32_t>(length)});
        next->toc.push_back({"正文", "text", "", 0});
        impl_ = std::move(next);
        return true;
    }
    const StageTimer timer{"epub_open", 0};
    epub_opens.fetch_add(1, std::memory_order_relaxed);
    close(); error.clear();
    if (stop && stop()) return fail(error, "EPUB open cancelled");
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
    next->zip_entries = mz_zip_reader_get_num_files(&next->zip);
    next->zip_index.reset(static_cast<Impl::ZipEntry*>(std::calloc(next->zip_entries, sizeof(Impl::ZipEntry))));
    if (!next->zip_index) return fail(error, "ZIP index allocation failed");
    {
        const StageTimer timer{"zip_index_build", 0};
        std::uint64_t total = 0;
        for (mz_uint i = 0; i < next->zip_entries; ++i) {
            if (stop && stop()) return fail(error, "EPUB open cancelled");
            mz_zip_archive_file_stat stat{};
            if (!mz_zip_reader_file_stat(&next->zip, i, &stat)) return fail(error, "Invalid ZIP entry");
            const auto name_bytes = mz_zip_reader_get_filename(&next->zip, i, nullptr, 0);
            if (name_bytes > 512) return fail(error, "ZIP path exceeds limit");
            const std::string name = stat.m_filename; std::string canonical; std::u32string unicode;
            if (name_bytes != name.size() + 1) return fail(error, "Unsafe or duplicate ZIP path");
            const bool directory = !name.empty() && name.back() == '/';
            if (!decode(name.data(), name.size(), unicode) || !path_parts(name, canonical) || canonical != (directory ? name.substr(0, name.size() - 1) : name)) return fail(error, "Unsafe or duplicate ZIP path");
            const auto file_type = (stat.m_external_attr >> 16) & 0170000;
            if ((stat.m_bit_flag & 1) || (stat.m_method != 0 && stat.m_method != 8) || file_type == 0120000) return fail(error, "Encrypted, linked or unsupported ZIP entry");
            total += stat.m_uncomp_size;
            if (stat.m_uncomp_size > 32 * 1024 * 1024 || total > 512ULL * 1024 * 1024 || (stat.m_uncomp_size > 4096 && stat.m_uncomp_size > std::max<mz_uint64>(1, stat.m_comp_size) * 1000)) return fail(error, "ZIP expansion limit exceeded");
            next->zip_index.get()[i] = {i, static_cast<std::uint32_t>(stat.m_uncomp_size)};
        }
        const auto compare = [&](const Impl::ZipEntry& a, const Impl::ZipEntry& b) {
            char left[512], right[512];
            mz_zip_reader_get_filename(&next->zip, a.member, left, sizeof(left));
            mz_zip_reader_get_filename(&next->zip, b.member, right, sizeof(right));
            return std::strcmp(left, right);
        };
        auto* entries = next->zip_index.get();
        std::sort(entries, entries + next->zip_entries, [&](const auto& a, const auto& b) { return compare(a, b) < 0; });
        for (std::size_t i = 1; i < next->zip_entries; ++i) {
            if (stop && stop()) return fail(error, "EPUB open cancelled");
            if (compare(entries[i - 1], entries[i]) == 0) return fail(error, "Unsafe or duplicate ZIP path");
        }
    }
    if (next->find_member("META-INF/encryption.xml")) return fail(error, "Encrypted or obfuscated EPUB is unsupported");
    char mime[64]{};
    mz_zip_archive_file_stat mime_stat{};
    const int mime_index = mz_zip_reader_locate_file(&next->zip, "mimetype", nullptr, 0);
    if (mime_index < 0) return fail(error, "Missing EPUB mimetype");
    if (!mz_zip_reader_file_stat(&next->zip, mime_index, &mime_stat)) return fail(error, "Cannot read EPUB mimetype");
    if (mime_stat.m_is_directory) return fail(error, "Invalid EPUB mimetype");
    if (mime_stat.m_uncomp_size > sizeof(mime)) return fail(error, "EPUB mimetype exceeds limit");
    if (!mz_zip_reader_extract_to_mem(&next->zip, mime_index, mime, sizeof(mime), 0)) return fail(error, "Cannot read EPUB mimetype");
    if (trim(std::string(mime, static_cast<std::size_t>(mime_stat.m_uncomp_size))) != "application/epub+zip")
        return fail(error, "Invalid EPUB mimetype");
    if (identity) {
        next->metadata.book_id = identity->book_id;
        next->metadata.partial_md5 = identity->partial_md5;
    } else if (!cover_only) {
        identity_scans.fetch_add(1, std::memory_order_relaxed);
        const StageTimer timer{"identity_read_hash", 0};
        Hash sha(false), md5(true); std::array<unsigned char, 8192> buffer{};
        if (std::fseek(next->file, 0, SEEK_SET) != 0) return fail(error, "Cannot hash EPUB");
        for (;;) {
            if (stop && stop()) return fail(error, "EPUB open cancelled");
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
    Xml container;
    std::string package;
    bool rootfile_found = false;
    container.start = [&](const std::string& name, const char** attrs) {
        if (!rootfile_found && name == "rootfile" && (attr(attrs, "media-type").empty() || attr(attrs, "media-type") == "application/oebps-package+xml")) {
            rootfile_found = true; package = attr(attrs, "full-path");
        }
    };
    if (!next->parse("META-INF/container.xml", container, error, kMaxXmlBytes, stop)) return false;
    std::string normalized;
    if (!path_parts(package, normalized) || package != normalized) return fail(error, "Invalid package path");
    PackageBudget budget;
    std::string navigation, ncx;
    {
        struct RawItem { std::string id, href, type, properties; };
        std::vector<RawItem> items;
        std::vector<std::pair<std::string, bool>> itemrefs;
        std::vector<std::string> guide_covers, elements;
        std::string cover_id, text, metadata_name;
        unsigned metadata_depth = 0;
        Xml opf;
        opf.start = [&](const std::string& name, const char** attrs) {
            if (elements.empty() && name != "package") { opf.stop("Missing EPUB package"); return; }
            const auto parent = elements.empty() ? std::string{} : elements.back();
            if (parent == "metadata") {
                if (name == "meta" && attr(attrs, "name") == "cover") budget.replace(opf, cover_id, attr(attrs, "content"));
                if (!metadata_depth && (name == "title" || name == "creator" || name == "language")) {
                    metadata_name = name; metadata_depth = opf.depth;
                }
            }
            if (name == "item" && parent == "manifest") {
                if (items.size() >= kMaxPackageEntries) { opf.stop("Package/navigation node limit exceeded"); return; }
                RawItem item{attr(attrs, "id"), attr(attrs, "href"), attr(attrs, "media-type"), attr(attrs, "properties")};
                if (!budget.add(opf, item.id.size() + item.href.size() + item.type.size() + item.properties.size())) return;
                items.push_back(std::move(item));
            }
            if (name == "itemref" && parent == "spine") {
                if (itemrefs.size() >= kMaxPackageEntries) { opf.stop("Package/navigation node limit exceeded"); return; }
                auto id = attr(attrs, "idref");
                if (!budget.add(opf, id.size())) return;
                itemrefs.emplace_back(std::move(id), attr(attrs, "linear") != "no");
            }
            if (name == "reference" && parent == "guide" && token(attr(attrs, "type"), "cover")) {
                // Empty references also consume vector storage and require a record bound.
                if (guide_covers.size() >= kMaxPackageEntries) { opf.stop("Package/navigation node limit exceeded"); return; }
                auto href = attr(attrs, "href");
                if (!budget.add(opf, href.size())) return;
                guide_covers.push_back(std::move(href));
            }
            elements.push_back(name);
        };
        opf.text = [&](const char* value, std::size_t count) { if (metadata_depth) budget.append(opf, text, value, count); };
        opf.end = [&](const std::string&) {
            // Expat may deliver an empty element's end callback after start aborts parsing.
            if (!opf.error.empty()) return;
            if (metadata_depth == opf.depth) {
                auto& target = metadata_name == "title" ? next->metadata.title : metadata_name == "creator" ? next->metadata.author : next->metadata.language;
                auto value = trim(text);
                budget.bytes -= text.size(); text.clear();
                if (target.empty()) budget.replace(opf, target, std::move(value));
                metadata_depth = 0;
            }
            elements.pop_back();
        };
        if (!next->parse(package, opf, error, kMaxXmlBytes, stop))
            return opf.error == "Missing EPUB package" ? fail(error, opf.error) : false;
        struct Item { std::string path, type, properties; };
        std::map<std::string, Item> manifest;
        for (auto& item : items) {
            if (stop && stop()) return fail(error, kChapterCancelled);
            std::string resource, fragment;
            if (item.id.empty() || manifest.count(item.id) || !resolve(package, item.href, resource, fragment) || !fragment.empty()) return fail(error, "Invalid manifest resource");
            if (!budget.replace(opf, item.href, std::move(resource))) return fail(error, opf.error);
            if (token(item.properties, "cover-image") && next->cover.empty()) budget.replace(opf, next->cover, item.href);
            if (token(item.properties, "nav")) budget.replace(opf, navigation, item.href);
            if (item.type == "application/x-dtbncx+xml") budget.replace(opf, ncx, item.href);
            manifest.emplace(std::move(item.id), Item{std::move(item.href), std::move(item.type), std::move(item.properties)});
        }
        if (next->cover.empty() && manifest.count(cover_id)) budget.replace(opf, next->cover, manifest.at(cover_id).path);
        for (const auto& href : guide_covers) {
            if (stop && stop()) return fail(error, kChapterCancelled);
            std::string resource, fragment;
            if (resolve(package, href, resource, fragment)) budget.replace(opf, next->cover_page, std::move(resource));
        }
        const StageTimer spine_timer{"epub_spine_validate", 0};
        for (auto& ref : itemrefs) {
            if (stop && stop()) return fail(error, kChapterCancelled);
            const auto& id = ref.first; auto item = manifest.find(id);
            if (item == manifest.end() || item->second.type != "application/xhtml+xml") return fail(error, "Unsupported or missing spine resource");
            const auto* entry = next->find_member(item->second.path);
            if (!entry) return fail(error, "Unsupported or missing spine resource");
            if (!budget.add(opf, item->second.path.size())) return fail(error, opf.error);
            next->spine.push_back({item->second.path, std::move(ref.first), ref.second, entry->bytes});
        }
        if (!opf.error.empty()) return fail(error, opf.error);
    }
    if (next->spine.empty()) return fail(error, "EPUB spine is empty");
    if (next->metadata.title.empty()) { auto slash = path.rfind('/'); next->metadata.title = path.substr(slash == std::string::npos ? 0 : slash + 1); }
    // Raw OPF records are no longer live; navigation shares the remaining payload budget.
    budget.bytes = next->metadata.title.size() + next->metadata.author.size() + next->metadata.language.size() + next->cover.size() + next->cover_page.size() + navigation.size() + ncx.size();
    std::size_t fallback_bytes = 0;
    for (const auto& item : next->spine) fallback_bytes += item.id.size() + item.resource.size();
    budget.bytes += fallback_bytes;
    // The mandatory spine-derived table must also fit if every optional source fails.
    if (budget.bytes > kMaxPackageStrings || fallback_bytes > kMaxPackageStrings - budget.bytes)
        return fail(error, "Package/navigation text limit exceeded");
    if (cover_only) { impl_ = std::move(next); return true; }
    for (const auto* source : {&navigation, &ncx}) {
        if (source->empty() || !next->toc.empty()) continue;
        Xml xml;
        auto toc_budget = budget;
        std::vector<TocEntry> toc;
        struct OpenEntry { std::size_t index; unsigned depth; };
        std::vector<OpenEntry> open, texts;
        std::vector<unsigned> toc_navs;
        unsigned lists = 0;
        const bool is_ncx = source == &ncx;
        auto add_entry = [&]() {
            if (toc.size() >= kMaxTocEntries) { xml.stop("Navigation entry limit exceeded"); return false; }
            toc.push_back({}); return true;
        };
        xml.start = [&](const std::string& name, const char** attrs) {
            if (is_ncx) {
                if (name == "navPoint") {
                    if (!add_entry()) return;
                    toc.back().depth = open.size(); open.push_back({toc.size() - 1, xml.depth});
                } else if (!open.empty() && name == "text") {
                    const auto index = open.back().index;
                    toc_budget.replace(xml, toc[index].label, ""); texts.push_back({index, xml.depth});
                } else if (!open.empty() && name == "content") {
                    auto& entry = toc[open.back().index];
                    std::string resource = entry.resource, fragment = entry.fragment;
                    resolve(*source, attr(attrs, "src"), resource, fragment);
                    toc_budget.replace(xml, entry.resource, std::move(resource));
                    toc_budget.replace(xml, entry.fragment, std::move(fragment));
                }
            } else {
                if (name == "nav" && token(attr(attrs, "type"), "toc")) toc_navs.push_back(xml.depth);
                if (name == "ol") ++lists;
                if (name == "a" && !toc_navs.empty()) {
                    if (!add_entry()) return;
                    auto& entry = toc.back(); entry.depth = lists ? lists - 1 : 0;
                    std::string resource, fragment;
                    if (resolve(*source, attr(attrs, "href"), resource, fragment)) {
                        toc_budget.replace(xml, entry.resource, std::move(resource));
                        toc_budget.replace(xml, entry.fragment, std::move(fragment));
                    }
                    open.push_back({toc.size() - 1, xml.depth});
                }
            }
        };
        xml.text = [&](const char* value, std::size_t count) {
            if (is_ncx) {
                if (!texts.empty() && !open.empty() && texts.back().index == open.back().index)
                    toc_budget.append(xml, toc[texts.back().index].label, value, count);
            } else for (const auto& entry : open) toc_budget.append(xml, toc[entry.index].label, value, count);
        };
        xml.end = [&](const std::string& name) {
            if (is_ncx) {
                if (name == "text" && !texts.empty() && texts.back().depth == xml.depth) texts.pop_back();
                if (name == "navPoint" && !open.empty()) open.pop_back();
            } else {
                if (name == "a" && !open.empty() && open.back().depth == xml.depth) open.pop_back();
                if (name == "ol") --lists;
                if (name == "nav" && !toc_navs.empty() && toc_navs.back() == xml.depth) toc_navs.pop_back();
            }
        };
        std::string toc_error;
        if (next->parse(*source, xml, toc_error, kMaxXmlBytes, stop)) {
            for (auto& entry : toc) entry.label = trim(entry.label);
            toc.erase(std::remove_if(toc.begin(), toc.end(), [](const TocEntry& entry) { return entry.resource.empty(); }), toc.end());
            next->toc = std::move(toc);
        } else if (toc_error == kChapterCancelled) return fail(error, toc_error);
        // Optional navigation failures fall through to NCX or the spine-derived table.
    }
    if (next->toc.empty()) for (const auto& item : next->spine) next->toc.push_back({item.id, item.resource, "", 0});
    impl_ = std::move(next);
    return true;
}

bool EpubDocument::read_cover(const std::string& path, std::vector<std::uint8_t>& bytes, std::string& error, const std::function<bool()>& stop) {
    bytes.clear(); error.clear();
    EpubDocument book;
    if (!book.open_impl(path, error, true, nullptr, stop)) return false;
    if (!book.impl_->cover.empty() && book.read_image(book.impl_->cover, bytes, error, stop)) return true;
    if (stop && stop()) return fail(error, kChapterCancelled);
    Chapter unused;
    const auto resource = book.impl_->cover_page.empty() ? book.impl_->spine.front().resource : book.impl_->cover_page;
    if (!book.impl_->cover_page.empty() && book.read_image(resource, bytes, error, stop)) return true;
    if (stop && stop()) return fail(error, kChapterCancelled);
    error.clear();
    // A cover page is a single image; a page long enough to span parts is not one.
    ChapterParser parser(unused, resource);
    if (!book.impl_->parse(resource, parser.xml, error, 262144, stop)) return false;
    parser.finish_block();
    const auto& chapter = parser.chapter;
    if (parser.part != 0 || chapter.images.size() != 1 || chapter.blocks.size() != 1 || chapter.blocks.front().kind != BlockKind::Image)
        return fail(error, "EPUB has no supported cover");
    return book.read_image(chapter.images.front().resource, bytes, error, stop);
}

bool chapter_memory_available(std::size_t bytes) { return memory_room(bytes); }

void set_chapter_memory_probe(bool (*probe)(std::size_t bytes)) {
    memory_room = probe ? probe : platform_memory_room;
}

struct FileDigest::Impl {
    std::unique_ptr<FILE, decltype(&std::fclose)> file{nullptr, &std::fclose};
    Hash sha{false};
    bool finished = false;
    std::string path, value;
    explicit Impl(const std::string& name) : path(name) {}
};
FileDigest::FileDigest(const std::string& path) : impl_(std::make_unique<Impl>(path)) {}
FileDigest::~FileDigest() = default;
const std::string& FileDigest::digest() const { return impl_->value; }
bool FileDigest::step(std::size_t budget, const std::function<bool()>& stop) {
    auto& state = *impl_;
    if (state.finished) return true;
    if (!budget || (stop && stop())) return false;
    if (!state.file) {
        state.file.reset(std::fopen(state.path.c_str(), "rb"));
        if (!state.file) { state.finished = true; return true; }
    }
    std::array<unsigned char, 8192> bytes;
    while (budget && (!stop || !stop())) {
        const auto requested = std::min(budget, bytes.size());
        const auto count = std::fread(bytes.data(), 1, requested, state.file.get());
        if (count && !state.sha.update(bytes.data(), count)) {
            state.finished = true;
        } else if (count < requested) {
            if (!std::ferror(state.file.get())) state.sha.finish(state.value);
            state.finished = true;
        }
        if (state.finished) { state.file.reset(); return true; }
        budget -= count;
    }
    return false;
}
std::string sha256_file(const std::string& path, const std::function<bool()>& stop) {
    FileDigest scan(path);
    return scan.step(std::numeric_limits<std::size_t>::max(), stop) ? scan.digest() : std::string();
}

std::string md5_hex(std::string_view bytes) {
    Hash md5(true);
    std::string digest;
    md5.update(reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size());
    return md5.finish(digest) ? digest : std::string();
}

struct Md5Digest::Impl {
    Hash hash{true};
    bool finished = false;
};
Md5Digest::Md5Digest() : impl_(std::make_unique<Impl>()) {}
Md5Digest::~Md5Digest() = default;
bool Md5Digest::update(std::string_view bytes) {
    return !impl_->finished && impl_->hash.update(reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size());
}
std::string Md5Digest::finish() {
    if (impl_->finished) return {};
    impl_->finished = true;
    std::string value;
    return impl_->hash.finish(value) ? value : std::string();
}

void release(Chapter& chapter) {
    // Assigning an empty value may keep the capacity of the strings; swapping hands the buffers
    // to a temporary that frees them.
    Chapter empty;
    std::swap(chapter, empty);
}

bool EpubDocument::load_chapter(std::size_t index, Chapter& chapter, std::string& error, ChapterLoad detail, std::uint32_t part,
                                const std::function<bool()>& stop, const std::function<void(const Chapter&)>& visit) {
    return impl_->load(index, chapter, error, detail, part, nullptr, stop, visit);
}
bool EpubDocument::find_chapter_part(std::size_t index, Chapter& chapter, std::string& error, ChapterLoad detail,
                                     const std::function<bool(const Chapter&)>& match, const std::function<bool()>& stop, const std::function<void(const Chapter&)>& visit) {
    return impl_->load(index, chapter, error, detail, 0, &match, stop, visit);
}
bool EpubDocument::Impl::load(std::size_t index, Chapter& chapter, std::string& error, ChapterLoad detail,
                              std::uint32_t wanted, const std::function<bool(const Chapter&)>* match, const std::function<bool()>& stop,
                              const std::function<void(const Chapter&)>& visit) {
    const StageTimer timer{"chapter_parse", 0};
    pico_page::release(chapter); error.clear();
    if (stop && stop()) return fail(error, kChapterCancelled);
    if (!file || index >= spine.size()) return fail(error, "Invalid chapter index");
    if (txt) return load_txt(chapter, error, detail, wanted, match, stop, visit);
    ChapterParser parser(chapter, spine[index].resource);
    parser.wanted = wanted;
    parser.match = match;
    if (detail == ChapterLoad::Reading) parser.visit = &visit;
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
        parser.visit_part(true);
        if (!parser.captured && (wanted == kLastChapterPart || parser.selected())) parser.capture(true);
    }
    if (!parser.captured) { pico_page::release(chapter); return fail(error, "Chapter part is not available: " + spine[index].resource); }
    if (detail == ChapterLoad::Mapping) {
        for (const auto& path : parser.css_links) {
            if (stop && stop()) { pico_page::release(chapter); return fail(error, kChapterCancelled); }
            const auto* entry = find_member(path);
            if (!entry || entry->bytes > kMaxCssBytes - parser.css_bytes) {
                parser.chapter.plain_dom = false; warn(parser.chapter, "Missing or oversized stylesheet is not loaded"); continue;
            }
            std::string css(entry->bytes, '\0');
            if (!mz_zip_reader_extract_to_mem(&zip, entry->member, css.data(), css.size(), 0)) {
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
bool EpubDocument::Impl::load_txt(Chapter& chapter, std::string& error, ChapterLoad detail,
                                  std::uint32_t wanted, const std::function<bool(const Chapter&)>* match,
                                  const std::function<bool()>& stop, const std::function<void(const Chapter&)>& visit) {
    const bool gb = metadata.txt_gb18030;
    auto last = gb ? static_cast<std::uint32_t>(metadata.txt_offsets.size() - 2) :
        (txt_bytes - 1) / kTxtPartBytes;
    const auto aligned = [this](std::uint32_t boundary, std::uint32_t& result) {
        result = std::min(boundary, txt_bytes);
        unsigned char byte = 0, previous = 0;
        while (result < txt_bytes) {
            if (read(this, result, &byte, 1) != 1) return false;
            if ((byte & 0xc0) != 0x80) break;
            ++result;
        }
        if (result && result < txt_bytes && byte == '\n') {
            if (read(this, result - 1, &previous, 1) != 1) return false;
            if (previous == '\r') ++result;
        }
        return true;
    };
    // A character or line ending that crosses the final boundary moves the aligned start of the
    // last part to the end of the file; the preceding part then holds the rest of the text.
    if (!gb && last > 0) {
        std::uint32_t start = 0;
        if (!aligned(last * kTxtPartBytes, start)) return fail(error, "无法读取 TXT 文件");
        if (start >= txt_bytes) --last;
    }
    if (!match && wanted != kLastChapterPart && wanted > last)
        return fail(error, "TXT 内容段不可用");
    for (std::uint32_t part = match ? 0 : wanted == kLastChapterPart ? last : wanted; part <= last; ++part) {
        if (stop && stop()) { pico_page::release(chapter); return fail(error, kChapterCancelled); }
        std::uint32_t begin = 0, end = 0;
        if (gb) { begin = metadata.txt_offsets[part]; end = metadata.txt_offsets[part + 1]; }
        else if (!aligned(part * kTxtPartBytes, begin) || !aligned((part + 1) * kTxtPartBytes, end)) {
            pico_page::release(chapter); return fail(error, "无法读取 TXT 文件");
        }
        if (end < begin) { pico_page::release(chapter); return fail(error, "TXT 阅读位置索引无效"); }
        const auto size = end - begin;
        if (!memory_room(size * (sizeof(char32_t) + 1))) {
            pico_page::release(chapter); return fail(error, std::string(kChapterMemoryError) + ": text");
        }
        std::string bytes(size, '\0');
        if (read(this, begin, bytes.data(), size) != size) {
            pico_page::release(chapter); return fail(error, "无法读取 TXT 文件");
        }
        if (!gb && part == 0 && bytes.size() >= 3 && bytes.compare(0, 3, "\xef\xbb\xbf") == 0) bytes.erase(0, 3);
        Chapter current;
        current.resource = "text";
        current.part = part;
        current.last_part = part == last;
        current.split_node = part != 0 || !current.last_part;
        current.plain_dom = false;
        current.text.reserve(bytes.size());
        if (!(gb ? gb18030::decode(bytes, current.text) : decode(bytes.data(), bytes.size(), current.text))) {
            pico_page::release(chapter); return fail(error, "TXT 内容解码失败");
        }
        if (!current.text.empty()) {
            current.blocks.push_back({0, static_cast<std::uint32_t>(current.text.size()), BlockKind::Paragraph});
            Anchor source;
            if (detail != ChapterLoad::Layout) source = {current.resource, "part:" + std::to_string(part), 0};
            current.runs.push_back({0, static_cast<std::uint32_t>(current.text.size()), std::move(source)});
        }
        if (detail == ChapterLoad::Reading && visit) visit(current);
        if ((match && (*match)(current)) || (!match && (wanted == kLastChapterPart ? part == last : part == wanted))) {
            chapter = std::move(current);
            return true;
        }
        if (!match) break;
    }
    pico_page::release(chapter);
    return fail(error, "TXT 阅读位置不可用");
}
bool EpubDocument::chapter_index_for(const std::string& resource, std::size_t& index) const {
    for (std::size_t i = 0; i < impl_->spine.size(); ++i) if (impl_->spine[i].resource == resource) { index = i; return true; }
    return false;
}
bool EpubDocument::read_image(const std::string& resource, std::vector<std::uint8_t>& bytes, std::string& error,
                              const std::function<bool()>& stop) {
    const StageTimer timer{"image_read_inflate", 0};
    bytes.clear(); error.clear();
    if (stop && stop()) return fail(error, "Image load cancelled");
    std::string canonical;
    if (!impl_->file || resource.empty() || !path_parts(resource, canonical) || canonical != resource) return fail(error, "Invalid internal image path");
    const auto* entry = impl_->find_member(resource);
    if (!entry) return fail(error, "Missing EPUB image");
    if (!entry->bytes || entry->bytes > 2 * 1024 * 1024) return fail(error, "EPUB image exceeds 2 MiB limit");
    if (!platform_memory_room(entry->bytes)) return fail(error, "Image allocation unavailable");
    bytes.resize(entry->bytes);
    struct Output { std::vector<std::uint8_t>& bytes; const std::function<bool()>& stop; bool cancelled = false; } output{bytes, stop};
    const bool extracted = mz_zip_reader_extract_to_callback(&impl_->zip, entry->member,
        [](void* opaque, mz_uint64 offset, const void* data, std::size_t count) -> std::size_t {
            auto& output = *static_cast<Output*>(opaque);
            if (output.stop && output.stop()) { output.cancelled = true; return 0; }
            if (offset > output.bytes.size() || count > output.bytes.size() - offset) return 0;
            std::memcpy(output.bytes.data() + offset, data, count);
            return count;
        }, &output, 0);
    if (!extracted || output.cancelled || (stop && stop())) {
        bytes.clear();
        if (output.cancelled || (stop && stop())) return fail(error, "Image load cancelled");
        const auto zip_error = mz_zip_get_last_error(&impl_->zip);
        if (zip_error == MZ_ZIP_FILE_READ_FAILED || zip_error == MZ_ZIP_FILE_SEEK_FAILED)
            return fail(error, "Cannot read EPUB image");
        if (zip_error == MZ_ZIP_ALLOC_FAILED) return fail(error, "Image allocation unavailable");
        return fail(error, "Invalid compressed EPUB image");
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
