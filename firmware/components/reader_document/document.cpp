#include "pico_page/stage_timer.h"
#include "pico_page/host_work_probe.h"
#include "pico_page/document.h"
#include "css.h"
#include "gb18030.h"
#include "expat.h"
#include "miniz.h"
#include "html_reader.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iterator>
#include <limits>
#include <map>
#include <tuple>
#include <unordered_map>
#include <utility>
#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#ifdef CONFIG_PICO_PAGE_LEGACY_PERF_LOG
#include "esp_timer.h"
#endif
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "psa/crypto.h"
#else
#include <openssl/evp.h>
#include <openssl/rand.h>
#endif

namespace pico_page {
char32_t named_html_entity(std::string_view name) {
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
    for (std::size_t i = 0; i < std::size(latin1); ++i)
        if (name == latin1[i]) return static_cast<char32_t>(0xa0 + i);
    for (const auto& entity : other) if (name == entity.first) return entity.second;
    return 0;
}

namespace {
std::atomic<std::uint64_t> identity_scans{0}, epub_opens{0};
constexpr std::uint64_t kMaxBook = 256ULL * 1024 * 1024;
constexpr std::uint32_t kMaxXmlBytes = 16 * 1024 * 1024;
constexpr std::uint32_t kNoHtmlRepair = std::numeric_limits<std::uint32_t>::max();
constexpr std::uint32_t kMaxText = 2 * kChapterPartScalars;
constexpr std::size_t kPartRecords = 4096, kPartMetadata = 256 * 1024;
constexpr std::size_t kPartRecordsHard = 6000, kPartMetadataHard = 448 * 1024;
constexpr char kPartComplete[] = "Chapter part complete";
bool platform_memory_room(std::size_t bytes) {
#ifdef ESP_PLATFORM
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
constexpr std::size_t kMaxInlineImageUri = 32 * 1024;
bool optional_image_uri(std::string_view value) {
    return value.size() <= kMaxInlineImageUri &&
        (value.rfind("data:", 0) == 0 || value.rfind("denied:data:", 0) == 0);
}

bool fail(std::string& error, const std::string& message) { error = message; return false; }
auto content_failure_reporter(ContentFailureKind* output) {
    if (output) *output = ContentFailureKind::None;
    return [output](std::string& error, const std::string& message,
                    ContentFailureKind kind = ContentFailureKind::InvalidFormat) {
        if (output) *output = kind;
        error = message;
        return false;
    };
}
ContentFailureKind zip_failure_kind(mz_zip_archive& zip) {
    switch (mz_zip_get_last_error(&zip)) {
    case MZ_ZIP_ALLOC_FAILED: return ContentFailureKind::OutOfMemory;
    case MZ_ZIP_FILE_READ_FAILED: case MZ_ZIP_FILE_SEEK_FAILED: return ContentFailureKind::IoError;
    default: return ContentFailureKind::InvalidFormat;
    }
}

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
std::string_view attr_view(const char** attrs, std::string_view name) {
    for (auto p = attrs; p && *p; p += 2) {
        const char* separator = std::strrchr(p[0], '|');
        if (name == (separator ? separator + 1 : p[0])) return p[1];
    }
    return {};
}
std::string attr(const char** attrs, const char* name) { return std::string(attr_view(attrs, name)); }
bool token(std::string_view list, std::string_view wanted) {
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
struct Utf16Decoder {
    enum class Result { Pending, Complete, Invalid };
    bool big_endian = false, has_byte = false;
    std::uint8_t first_byte = 0;
    std::uint16_t high = 0;
    bool pending() const { return has_byte || high; }
    Result feed(std::uint8_t byte, char32_t& scalar) {
        if (!has_byte) { first_byte = byte; has_byte = true; return Result::Pending; }
        has_byte = false;
        const auto unit = static_cast<std::uint16_t>(big_endian ? first_byte * 256U + byte : byte * 256U + first_byte);
        if (high) {
            if (unit < 0xdc00 || unit > 0xdfff) return Result::Invalid;
            scalar = 0x10000 + (high - 0xd800U) * 1024U + unit - 0xdc00U;
            high = 0; return Result::Complete;
        }
        if (unit >= 0xd800 && unit <= 0xdbff) { high = unit; return Result::Pending; }
        if (unit >= 0xdc00 && unit <= 0xdfff) return Result::Invalid;
        scalar = unit;
        return unit ? Result::Complete : Result::Invalid;
    }
};
bool decode_utf16(std::string_view bytes, bool big_endian, std::u32string& text) {
    text.clear();
    Utf16Decoder decoder; decoder.big_endian = big_endian;
    for (const auto byte : bytes) {
        char32_t scalar = 0;
        const auto result = decoder.feed(static_cast<std::uint8_t>(byte), scalar);
        if (result == Utf16Decoder::Result::Invalid) return false;
        if (result == Utf16Decoder::Result::Complete) text.push_back(scalar);
    }
    return !decoder.pending();
}
class TxtTocBuilder {
public:
    std::vector<TxtHeading> headings;
    void feed(char32_t scalar, std::uint32_t byte_start) {
        if (!enabled_) return;
        if (!byte_start && scalar == 0xfeff) return;
        auto part = byte_start / kTxtPartBytes;
        if (scalar == U'\n' && previous_ == U'\r' && part > part_) part = part_;
        if (part != part_) { part_ = part; offset_ = 0; }
        if (scalar == U'\r' || scalar == U'\n') {
            finish_line();
            line_.clear(); long_line_ = false;
        } else if (!long_line_) {
            if (line_.empty() && (space(scalar) || scalar == 0x3000)) {
                ++offset_; previous_ = scalar; return;
            }
            if (line_.empty()) { line_part_ = part_; line_offset_ = offset_; line_byte_offset_ = byte_start; }
            if (line_.size() < 80) line_.push_back(scalar);
            else { line_.clear(); long_line_ = true; }
        }
        ++offset_; previous_ = scalar;
    }
    void finish_line() {
        if (!enabled_) return;
        while (!line_.empty() && (space(line_.back()) || line_.back() == 0x3000)) line_.pop_back();
        if (long_line_ || line_.empty() || !heading(line_)) return;
        const auto bytes = line_.size() * 4 + sizeof(TxtHeading);
        const auto growth = headings.size() == headings.capacity() ?
            std::max<std::size_t>(1, headings.capacity() * 2) * sizeof(TxtHeading) : 0;
        if (headings.size() >= 2048 || retained_ + bytes > 256 * 1024 || !memory_room(growth + bytes * 2)) {
            std::vector<TxtHeading>().swap(headings); enabled_ = false; return;
        }
        std::string label;
        for (const auto scalar : line_) {
            if (scalar < 0x80) label.push_back(static_cast<char>(scalar));
            else if (scalar < 0x800) {
                label.push_back(static_cast<char>(0xc0 | (scalar >> 6)));
                label.push_back(static_cast<char>(0x80 | (scalar & 63)));
            } else {
                if (scalar >= 0x10000) label.push_back(static_cast<char>(0xf0 | (scalar >> 18)));
                label.push_back(static_cast<char>((scalar < 0x10000 ? 0xe0 : 0x80) | ((scalar >> 12) & 63)));
                label.push_back(static_cast<char>(0x80 | ((scalar >> 6) & 63)));
                label.push_back(static_cast<char>(0x80 | (scalar & 63)));
            }
        }
        retained_ += bytes;
        headings.push_back({std::move(label), line_part_, line_offset_, line_byte_offset_});
    }
private:
    static bool heading(const std::u32string& line) {
        if (line[0] == U'第') {
            constexpr std::u32string_view numbers = U"0123456789０１２３４５６７８９零〇一二三四五六七八九十百千万两壹贰叁肆伍陆柒捌玖拾佰仟";
            std::size_t end = 1;
            while (end < line.size() && numbers.find(line[end]) != std::u32string::npos) ++end;
            if (end > 1 && end < line.size() && std::u32string_view(U"章回节卷部篇集").find(line[end]) != std::u32string::npos)
                return true;
        }
        for (const auto* prefix : {U"序章", U"楔子", U"前言", U"序言", U"引言", U"后记", U"尾声", U"终章", U"附录"}) {
            const std::u32string_view word(prefix);
            if (line == word || (line.size() > word.size() && line.compare(0, word.size(), word) == 0 &&
                (space(line[word.size()]) || line[word.size()] == U'：' || line[word.size()] == U':'))) return true;
        }
        const auto first = line[0] >= U'A' && line[0] <= U'Z' ? line[0] + U'a' - U'A' : line[0];
        if (first != U'c' && first != U'p' && first != U'b' && first != U'v') return false;
        std::u32string lower = line;
        for (auto& scalar : lower) if (scalar >= U'A' && scalar <= U'Z') scalar += U'a' - U'A';
        for (const auto* prefix : {U"chapter", U"part", U"book", U"volume"}) {
            const std::u32string_view word(prefix);
            if (lower.size() <= word.size() || lower.compare(0, word.size(), word) != 0 || !space(lower[word.size()])) continue;
            auto number = word.size();
            while (number < lower.size() && space(lower[number])) ++number;
            auto end = number;
            while (end < lower.size() && std::u32string_view(U"0123456789ivxlcdm").find(lower[end]) != std::u32string::npos) ++end;
            if (end > number && (end == lower.size() || space(lower[end]) || lower[end] == U':' || lower[end] == U'.' || lower[end] == U'-')) return true;
        }
        return false;
    }
    std::u32string line_;
    std::uint32_t part_ = 0, offset_ = 0, line_part_ = 0, line_offset_ = 0, line_byte_offset_ = 0;
    char32_t previous_ = 0;
    std::size_t retained_ = 0;
    bool enabled_ = true, long_line_ = false;
};

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
    bool source_utf8 = false, in_cdata = false;
    bool body_resource = false, allow_html = false, html_declared = false, html_fallback = false, html_final = false;
    std::unique_ptr<HtmlReader> html;
    bool suspend_requested = false, suspended = false;
    std::string error;
    ContentFailureKind failure_kind = ContentFailureKind::InvalidFormat;
    unsigned depth = 0;
    std::size_t nodes = 0, bytes = 0;
    std::size_t pending_bytes = 0, pending_limit = 0, completed_bytes = 0;
    std::uint32_t html_prefix_bytes = 0;
    Xml() {
        parser = XML_ParserCreateNS(nullptr, '|');
        if (!parser) { failure_kind = ContentFailureKind::OutOfMemory; error = "XML parser allocation failed"; return; }
        unsigned long salt = 0;
#ifdef ESP_PLATFORM
        esp_fill_random(&salt, sizeof(salt));
#else
        if (RAND_bytes(reinterpret_cast<unsigned char*>(&salt), sizeof(salt)) != 1) { failure_kind = ContentFailureKind::IoError; error = "XML hash entropy failed"; return; }
#endif
        XML_SetHashSalt(parser, salt ? salt : 1);
        XML_SetUserData(parser, this);
        XML_SetXmlDeclHandler(parser, [](void* opaque, const char*, const char* encoding, int) {
            auto& x = *static_cast<Xml*>(opaque);
            if (!encoding) return;
            const auto matches = [encoding](const char* expected) {
                std::size_t i = 0;
                for (; expected[i]; ++i)
                    if (std::tolower(static_cast<unsigned char>(encoding[i])) != expected[i]) return false;
                return encoding[i] == '\0';
            };
            x.source_utf8 = x.source_utf8 && (matches("utf-8") || matches("utf8") || matches("us-ascii"));
        });
        XML_SetElementHandler(parser, [](void* opaque, const char* name, const char** attrs) {
            auto& x = *static_cast<Xml*>(opaque);
            if (++x.depth > 64 || ++x.nodes > kMaxNodes) { x.stop("XML structure limit exceeded"); return; }
            std::size_t attributes = 0;
            for (auto p = attrs; p && *p; p += 2) {
                const bool optional_image = x.body_resource && local_name(name) == "img" &&
                    local_name(p[0]) == "src" && optional_image_uri(p[1]);
                if (++attributes > 64 || std::strlen(p[0]) > kMaxPath ||
                    (std::strlen(p[1]) > 8192 && !optional_image)) { x.stop("XML attribute limit exceeded"); return; }
            }
            if (x.html && XML_GetCurrentByteIndex(x.parser) < x.html_prefix_bytes) {
                int offset = 0, available = 0;
                const char* context = XML_GetInputContext(x.parser, &offset, &available);
                if (!context || offset < 0 || offset >= available || context[offset] != '<') {
                    x.stop("Cannot retain HTML source element name"); return;
                }
                auto end = offset + 1;
                while (end < available && context[end] != ' ' && context[end] != '\t' && context[end] != '\r' &&
                       context[end] != '\n' && context[end] != '/' && context[end] != '>') ++end;
                if (end == available || end == offset + 1) { x.stop("Cannot retain HTML source element name"); return; }
                x.html->observe_start(std::string(context + offset + 1, end - offset - 1));
            }
            const char* separator = std::strchr(name, '|');
            if (separator && std::string(name, separator - name) != "http://www.w3.org/1999/xhtml") x.foreign_namespace = true;
            if (x.start) x.start(local_name(name), attrs);
            x.complete_event();
        }, [](void* opaque, const char* name) {
            auto& x = *static_cast<Xml*>(opaque);
            const auto index = XML_GetCurrentByteIndex(x.parser);
            if (x.html && index >= 0 && (index < x.html_prefix_bytes ||
                (index == x.html_prefix_bytes && XML_GetCurrentByteCount(x.parser) == 0))) x.html->observe_end();
            if (x.end) x.end(local_name(name));
            --x.depth;
            x.complete_event();
        });
        XML_SetCharacterDataHandler(parser, [](void* opaque, const char* value, int count) {
            auto& x = *static_cast<Xml*>(opaque);
            if (x.text) x.text(value, static_cast<std::size_t>(count));
            x.complete_event();
        });
        XML_SetStartDoctypeDeclHandler(parser, [](void* opaque, const char*, const char*, const char*, int has_internal_subset) {
            auto& x = *static_cast<Xml*>(opaque);
            if (has_internal_subset) x.stop("Internal DTD subsets are unsupported");
        });
        // The XHTML 1.x DTD is never loaded, so its named character entities arrive here. Each is
        // delivered as one scalar of the surrounding text node.
        XML_SetSkippedEntityHandler(parser, [](void* opaque, const char* name, int parameter) {
            auto& x = *static_cast<Xml*>(opaque);
            const char32_t scalar = parameter ? 0 : named_html_entity(name);
            if (!scalar) { x.stop(std::string("Unsupported XML entity: ") + name); return; }
            const char utf8[] = {static_cast<char>(0xe0 | scalar >> 12), static_cast<char>(scalar < 0x800 ? 0xc0 | scalar >> 6 : 0x80 | (scalar >> 6 & 0x3f)),
                                 static_cast<char>(0x80 | (scalar & 0x3f))};
            if (x.text) scalar < 0x800 ? x.text(utf8 + 1, 2) : x.text(utf8, 3);
            x.complete_event();
        });
        XML_SetExternalEntityRefHandler(parser, [](XML_Parser, const char*, const char*, const char*, const char*) -> int { return XML_STATUS_ERROR; });
        XML_SetStartCdataSectionHandler(parser, [](void* opaque) { auto& x = *static_cast<Xml*>(opaque); x.in_cdata = true; if (x.complex_markup) x.complex_markup(); });
        XML_SetEndCdataSectionHandler(parser, [](void* opaque) { auto& x = *static_cast<Xml*>(opaque); x.in_cdata = false; x.complete_event(); });
        XML_SetCommentHandler(parser, [](void* opaque, const char*) { auto& x = *static_cast<Xml*>(opaque); if (x.boundary) x.boundary(); x.complete_event(); });
        XML_SetProcessingInstructionHandler(parser, [](void* opaque, const char*, const char*) { auto& x = *static_cast<Xml*>(opaque); if (x.boundary) x.boundary(); x.complete_event(); });
    }
    ~Xml() { if (parser) XML_ParserFree(parser); }
    void stop(const std::string& message, ContentFailureKind kind = ContentFailureKind::InvalidFormat) { if (error.empty()) { error = message; failure_kind = kind; } if (parser) XML_StopParser(parser, XML_FALSE); }
    // Expat may deliver additional callbacks after suspension; finish each event in full.
    void complete_event() {
        if (!html || XML_GetCurrentByteIndex(parser) < html_prefix_bytes) {
            const auto index = XML_GetCurrentByteIndex(parser);
            const auto count = XML_GetCurrentByteCount(parser);
            if (index >= 0 && count >= 0 && static_cast<std::size_t>(index) + count <= bytes) {
                completed_bytes = std::max(completed_bytes, static_cast<std::size_t>(index) + count);
                if (pending_limit) pending_bytes = bytes - (static_cast<std::size_t>(index) + count);
            }
        }
        if (!suspend_requested || suspended || !error.empty()) return;
        suspend_requested = false;
        if (XML_StopParser(parser, XML_TRUE) == XML_STATUS_OK) suspended = true;
        else stop("Cannot suspend XML parser");
    }
    bool parse_status(XML_Status status) {
        if (status != XML_STATUS_ERROR) return error.empty();
        if (XML_GetErrorCode(parser) == XML_ERROR_NO_MEMORY) failure_kind = ContentFailureKind::OutOfMemory;
        if (error.empty()) error = std::string("Invalid XML: ") + XML_ErrorString(XML_GetErrorCode(parser)) + " (line " + std::to_string(XML_GetCurrentLineNumber(parser)) + ", column " + std::to_string(XML_GetCurrentColumnNumber(parser)) + ")";
        return false;
    }
    bool html_syntax_error() const {
        switch (XML_GetErrorCode(parser)) {
        case XML_ERROR_SYNTAX:
        case XML_ERROR_NO_ELEMENTS:
        case XML_ERROR_INVALID_TOKEN:
        case XML_ERROR_TAG_MISMATCH:
        case XML_ERROR_DUPLICATE_ATTRIBUTE:
        case XML_ERROR_UNDEFINED_ENTITY: return true;
        default: return false;
        }
    }
    bool resume() {
        suspended = false;
        if (!parse_status(XML_ResumeParser(parser))) return false;
        if (suspended) return true;
        return !html || normalized_feed({}, html_final);
    }
    bool normalized_feed(std::string_view value, bool final) {
        const bool ok = html->feed(value, final, [this](std::string_view token) {
            return parse_status(XML_Parse(parser, token.data(), static_cast<int>(token.size()), XML_FALSE)) && !suspended;
        });
        if (!ok && !suspended && error.empty()) error = html->error;
        if (!error.empty()) return false;
        return !final || suspended || parse_status(XML_Parse(parser, "", 0, XML_TRUE));
    }
    bool feed(const char* value, std::size_t count, bool final) {
        if (!bytes && allow_html && count >= 14) {
            auto source = std::string_view(value, count);
            if (source.substr(0, 3) == "\xef\xbb\xbf") source.remove_prefix(3);
            const auto end = source.find('>');
            std::string declaration(source.substr(0, end == std::string_view::npos ? 0 : end));
            for (auto& c : declaration) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
            html_declared = trim(declaration) == "<!doctype html";
            if (html_declared && html_fallback) html = std::make_unique<HtmlReader>(html_prefix_bytes);
        }
        if (!bytes && count >= 4) {
            source_utf8 = true;
            for (std::size_t i = 0; i < 4; ++i)
                if (value[i] == '\0' || static_cast<unsigned char>(value[i]) >= 0xfe) source_utf8 = false;
        }
        if (!error.empty() || !parser) return false;
        if (pending_limit && (!html || XML_GetCurrentByteIndex(parser) < html_prefix_bytes)) {
            if (pending_bytes > pending_limit || count > pending_limit - pending_bytes) {
                stop("Sequential XML token budget exceeded"); return false;
            }
            pending_bytes += count;
        }
        bytes += count;
        if (bytes > kMaxXmlBytes) { stop("XML byte limit exceeded"); return false; }
        if (html) {
            if (!memory_room(512U * 1024)) { stop(kChapterMemoryError, ContentFailureKind::OutOfMemory); return false; }
            source_utf8 = false; html_final = final;
            return normalized_feed(std::string_view(value, count), final);
        }
        return parse_status(XML_Parse(parser, value, static_cast<int>(count), final ? XML_TRUE : XML_FALSE));
    }
};

constexpr std::size_t kMaxPackageEntries = 4096;
constexpr std::size_t kMaxTocEntries = 8192;
constexpr std::size_t kMaxPackageStrings = 1024 * 1024;
constexpr std::size_t kMaxNavigationLabel = 32768;

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

constexpr std::size_t kKoreaderTextSplitScalars = 8192;

namespace css = document::css;
constexpr char kAuthorUnavailable[] = "EPUB author styles exceed trial limits";
constexpr std::size_t kAuthorStyleCount = 256;
constexpr std::size_t kAuthorSourceBytes = 184 * 1024;
constexpr std::size_t kAuthorSources = 48;

char* css_source_buffer() {
    if (!memory_room(kAuthorSourceBytes)) return nullptr;
#ifdef ESP_PLATFORM
    return static_cast<char*>(heap_caps_malloc(kAuthorSourceBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
#else
    return static_cast<char*>(std::malloc(kAuthorSourceBytes));
#endif
}

struct AuthorLayoutState {
#ifdef ESP_PLATFORM
    static void* operator new(std::size_t bytes, const std::nothrow_t&) noexcept {
        return heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    static void operator delete(void* value) noexcept { std::free(value); }
#endif
    struct Ancestor {
        css::Element element;
        css::ComputedStyle style;
        std::size_t names_begin = 0;
        bool suppressed = false;
    };
    css::StyleSheet sheet;
    std::array<std::string, css::kEmbeddedFontCount> font_names;
    css::FontFamilies font_families{};
    std::vector<EmbeddedFont> fonts;
    css::Context context;
    css::WorkBudget work;
    std::array<Ancestor, 64> ancestors;
    std::array<char, 32 * 1024> names;
    std::size_t names_used = 0;
    const std::function<bool()>& stop;
#ifdef ESP_PLATFORM
    TickType_t yielded_at = xTaskGetTickCount();
#endif
    css::Checkpoint checkpoint;

    AuthorLayoutState(css::Limits limits, const std::function<bool()>& cancel)
        : sheet(limits), stop(cancel), checkpoint{[](void* opaque) {
            auto& owner = *static_cast<AuthorLayoutState*>(opaque);
#ifdef ESP_PLATFORM
            if (xTaskGetTickCount() - owner.yielded_at >= pdMS_TO_TICKS(20)) {
                vTaskDelay(1);
                owner.yielded_at = xTaskGetTickCount();
            }
#endif
            return owner.stop && owner.stop();
        }, this} { context.defer_box_percentages = true; }

    bool start(std::size_t depth, std::string_view tag, const char** attrs, bool block, bool heading) {
        if (depth >= ancestors.size()) return false;
        if (work.styles >= 64 || work.work >= work.maximum_work / 2) {
            work = {};
#ifdef ESP_PLATFORM
            vTaskDelay(1);
#endif
        }
        auto* parent = depth ? &ancestors[depth - 1] : nullptr;
        auto& current = ancestors[depth];
        current.names_begin = names_used;
        const auto copy = [&](std::string_view value) {
            auto* destination = names.data() + names_used;
            if (!value.empty()) std::memcpy(destination, value.data(), value.size());
            names_used += value.size();
            return std::string_view(destination, value.size());
        };
        const auto id = attr_view(attrs, "id"), classes = attr_view(attrs, "class");
        if (tag.size() + id.size() + classes.size() > names.size() - names_used) return false;
        current.element = {copy(tag), copy(id), copy(classes), parent ? &parent->element : nullptr};
        css::ComputedStyle defaults;
        if (parent) {
            const auto& inherited = parent->style;
            defaults.font_px = inherited.font_px;
            defaults.font_families = inherited.font_families;
            defaults.bleed = inherited.bleed;
            defaults.line_height_px = inherited.line_height_px;
            defaults.line_height_factor = inherited.line_height_factor;
            defaults.indent_px = inherited.indent_px;
            defaults.box_percentages = inherited.box_percentages & css::ComputedStyle::indent_percent;
            defaults.align = inherited.align;
            defaults.bold = inherited.bold;
            defaults.italic = inherited.italic;
            defaults.hidden = inherited.hidden;
        } else context.root_font_px = context.initial_font_px;
        defaults.display = block ? css::Display::Block : css::Display::Inline;
        if (heading || tag == "small") {
            defaults.font_px = std::clamp(defaults.font_px * (heading ? 1.2f : .8f), context.minimum_font_px, context.maximum_font_px);
            if (defaults.line_height_factor >= 0) defaults.line_height_px = defaults.font_px * defaults.line_height_factor;
        }
        if (heading || tag == "b" || tag == "strong") defaults.bold = true;
        if (tag == "em" || tag == "i") defaults.italic = true;
        if (tag == "u") defaults.underline = true;
        if (sheet.compute(current.element, parent ? &parent->style : nullptr, context, attr_view(attrs, "style"),
                          work, current.style, checkpoint, &defaults, &font_families) != css::Result::Complete) return false;
        if (parent && parent->style.underline) current.style.underline = true;
        current.suppressed = current.style.display == css::Display::None || (parent && parent->suppressed);
        if (current.suppressed) current.style.display = css::Display::None;
        if (!parent) context.root_font_px = current.style.font_px;
        return true;
    }
};

#ifdef CONFIG_PICO_PAGE_LEGACY_PERF_LOG
struct ChapterCallbackTiming {
    enum class Phase { Other, Memory, Text, Anchors, Markup, Count };
    static constexpr auto phase_count = static_cast<std::size_t>(Phase::Count);
    std::array<std::int64_t, phase_count> elapsed_us{};
    std::array<unsigned, phase_count> calls{};
    std::int64_t started_us = 0;
    Phase active = Phase::Other;

    Phase switch_to(Phase phase) {
        const auto now_us = esp_timer_get_time();
        if (active != Phase::Other) elapsed_us[static_cast<std::size_t>(active)] += now_us - started_us;
        started_us = now_us;
        return std::exchange(active, phase);
    }

    long long microseconds(Phase phase) const {
        return elapsed_us[static_cast<std::size_t>(phase)];
    }

    struct Scope {
        ChapterCallbackTiming* timing;
        Phase previous = Phase::Other;
        Scope(ChapterCallbackTiming* value, Phase phase) : timing(value) {
            if (!timing) return;
            ++timing->calls[static_cast<std::size_t>(phase)];
            previous = timing->switch_to(phase);
        }
        ~Scope() { if (timing) timing->switch_to(previous); }
    };
};
#endif

struct ChapterParser {
    static constexpr std::size_t kFragmentLimit = 2048;
    static constexpr auto kFragmentIndexBytes = kFragmentLimit * sizeof(std::uint16_t);

    struct Frame {
        std::string name, path;
        std::map<std::string, unsigned> children;
        unsigned text_index = 0;
        std::uint32_t source_offset = 0;
        std::size_t mapping_source_length = 0;
        bool in_text = false, hidden = false, body = false, pre = false, block = false, has_child = false;
        std::size_t css = std::numeric_limits<std::size_t>::max();
        BlockKind kind = BlockKind::Paragraph;
        std::size_t link = std::numeric_limits<std::size_t>::max();
        std::size_t source_node = std::numeric_limits<std::size_t>::max();
        std::string node;
        std::size_t image = std::numeric_limits<std::size_t>::max();
        bool simple_svg = true;
        unsigned svg_images = 0, svg_width = 0, svg_height = 0;
        std::string background;
    };
    Chapter chapter;
    Chapter& result;
    std::uint32_t requested_part = 0;
    const std::function<bool(const Chapter&)>* match_part = nullptr;
    std::uint32_t part = 0;
    std::size_t part_source_nodes = 0;
    bool split_pending = false, captured = false, resumable = false;
    Xml xml;
    std::vector<Frame> frames;
    std::unique_ptr<std::uint16_t, decltype(&std::free)> fragment_ordinals{nullptr, &std::free};
    std::uint32_t block_start = 0, source_count = 0;
    std::size_t retained_metadata_bytes = 0;
    std::size_t css_bytes = 0;
    std::vector<std::string> css_links;
    bool pending_space = false, seen_body = false;
    bool keep_mapping = true, keep_anchors = true;
    std::string last_run_node;
    Anchor pending_anchor;
    BlockKind block_kind = BlockKind::Paragraph;
    AuthorLayoutState* author = nullptr;
    css::ComputedStyle block_style, pending_space_style;
#ifdef CONFIG_PICO_PAGE_LEGACY_PERF_LOG
    ChapterCallbackTiming* callback_timing = nullptr;
#endif

    std::uint16_t retain_style(const css::ComputedStyle& style) {
        auto& styles = chapter.author_styles;
        const auto found = std::find(styles.begin(), styles.end(), style);
        if (found != styles.end()) return static_cast<std::uint16_t>(found - styles.begin() + 1);
        if (styles.size() == kAuthorStyleCount) { xml.stop(kAuthorUnavailable); return 0; }
        if (!styles.capacity()) {
            constexpr auto bytes = kAuthorStyleCount * sizeof(css::ComputedStyle);
            const bool available = memory_room(bytes)
#ifdef ESP_PLATFORM
                && heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) >= bytes
#endif
                ;
            if (!available) { xml.stop(kAuthorUnavailable); return 0; }
            styles.reserve(kAuthorStyleCount);
        }
        styles.push_back(style);
        return static_cast<std::uint16_t>(styles.size());
    }
    void set_block_style(const css::ComputedStyle& style) { if (author) block_style = style; }
    ChapterParser(Chapter& value, const std::string& resource) : result(value) {
        chapter.resource = resource;
        xml.body_resource = true;
        auto extension = resource.substr(resource.find_last_of('.') == std::string::npos ? resource.size() : resource.find_last_of('.'));
        for (auto& c : extension) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
        xml.allow_html = extension == ".html" || extension == ".htm";
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
    bool require_memory_room(std::size_t bytes) {
#ifdef CONFIG_PICO_PAGE_LEGACY_PERF_LOG
        const ChapterCallbackTiming::Scope timing{callback_timing, ChapterCallbackTiming::Phase::Memory};
#endif
        if (memory_room(bytes)) return true;
        xml.stop(kChapterMemoryError, ContentFailureKind::OutOfMemory);
        return false;
    }
    bool append_text_scalar(char32_t cp) {
        if (chapter.text.size() == chapter.text.capacity() &&
            !require_memory_room(2 * sizeof(char32_t) * std::max<std::size_t>(chapter.text.capacity(), 16))) return false;
        chapter.text.push_back(cp);
        return true;
    }
    bool record_fragment(std::string id) {
        auto& fragments = chapter.fragments;
        if (fragments.empty() && !fragment_ordinals && memory_room(kFragmentIndexBytes)) {
#ifdef ESP_PLATFORM
            fragment_ordinals.reset(static_cast<std::uint16_t*>(heap_caps_malloc(kFragmentIndexBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)));
#else
            fragment_ordinals.reset(static_cast<std::uint16_t*>(std::malloc(kFragmentIndexBytes)));
#endif
        }
        auto* ordinal_end = fragment_ordinals ? fragment_ordinals.get() + fragments.size() : nullptr;
        std::uint16_t* position = nullptr;
        bool duplicate;
        if (fragment_ordinals) {
            position = std::lower_bound(fragment_ordinals.get(), ordinal_end, id,
                [&](std::uint16_t ordinal, const std::string& key) { return fragments[ordinal].id < key; });
            duplicate = position != ordinal_end && fragments[*position].id == id;
        } else {
            duplicate = std::any_of(fragments.begin(), fragments.end(), [&](const auto& fragment) { return fragment.id == id; });
        }
        if (duplicate) { xml.stop("Duplicate XHTML fragment id"); return false; }
        retained_metadata_bytes += id.size();
        const auto fragment_count = fragments.size();
        if (fragment_count >= kFragmentLimit || retained_metadata_bytes > 512 * 1024) { xml.stop("Chapter fragment limit exceeded"); return false; }
        fragments.push_back({std::move(id), static_cast<std::uint32_t>(chapter.text.size())});
        if (position) {
            std::move_backward(position, ordinal_end, ordinal_end + 1);
            *position = static_cast<std::uint16_t>(fragment_count);
        }
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
               part_source_nodes >= kPartRecords || retained_metadata_bytes >= kPartMetadata;
    }
    bool hard_full() const {
        return chapter.text.size() >= kMaxText || chapter.runs.size() >= kPartRecordsHard ||
               part_source_nodes >= kPartRecordsHard || retained_metadata_bytes >= kPartMetadataHard;
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
        return match_part ? (*match_part)(chapter) : requested_part != kLastChapterPart && part == requested_part;
    }
    void capture(bool last) {
        auto css = std::move(chapter.css_sources);
        auto warnings = std::move(chapter.warnings);
        const bool plain = chapter.plain_dom;
        const bool long_source = chapter.long_source_node;
        if (!chapter.text.empty() && chapter.text.back() == U'\n') chapter.text.pop_back();
        chapter.part = part;
        chapter.last_part = last;
        result = std::move(chapter);
        chapter = Chapter();
        chapter.resource = result.resource;
        chapter.css_sources = std::move(css);
        chapter.warnings = std::move(warnings);
        chapter.plain_dom = plain;
        chapter.long_source_node = long_source;
        captured = true;
    }
    bool next_part() {
        const bool inside_block = !split_pending;
        const auto none = std::numeric_limits<std::size_t>::max();
        if (inside_block) { finish_block(); chapter.split_node = true; }
        split_pending = false;
        for (const auto& frame : frames)
            if (frame.link < chapter.links.size()) chapter.links[frame.link].end = static_cast<std::uint32_t>(chapter.text.size());
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
            if (!keep_mapping && !resumable) { xml.stop(kPartComplete); return false; }
            if (resumable) xml.suspend_requested = true;
        }
        chapter.text.clear(); chapter.blocks.clear(); chapter.runs.clear(); chapter.fragments.clear();
        chapter.links.clear(); chapter.images.clear(); chapter.author_styles.clear();
        chapter.source_nodes = std::move(carried);
        chapter.split_node = inside_block;
        retained_metadata_bytes = 0; part_source_nodes = 0;
        block_start = 0; source_count = 0; pending_space = false; last_run_node.clear();
        for (auto& frame : frames) frame.source_node = frame.link = frame.image = none;
        for (auto& [frame, node] : open) {
            ++part_source_nodes;
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
        if (end > block_start) {
            const std::uint16_t style = author ? retain_style(block_style) : 0;
            {
#ifdef CONFIG_PICO_PAGE_LEGACY_PERF_LOG
                const ChapterCallbackTiming::Scope timing{callback_timing, ChapterCallbackTiming::Phase::Anchors};
#endif
                chapter.blocks.push_back({block_start, end, block_kind, style});
            }
            append_text_scalar(U'\n');
        }
        block_start = static_cast<std::uint32_t>(chapter.text.size());
        if (soft_full()) split_pending = true;
    }
    void append(char32_t cp, const Anchor* source, const css::ComputedStyle* style = nullptr) {
        if ((split_pending || hard_full()) && !next_part()) return;
        const auto begin = static_cast<std::uint32_t>(chapter.text.size());
        if (!append_text_scalar(cp) || !source) return;
#ifdef CONFIG_PICO_PAGE_LEGACY_PERF_LOG
        const ChapterCallbackTiming::Scope timing{callback_timing, ChapterCallbackTiming::Phase::Anchors};
#endif
        if (!chapter.runs.empty()) {
            auto& last = chapter.runs.back();
            const auto& node = keep_anchors ? last.source.node : last_run_node;
            if (last.end == begin && node == source->node && last.source.offset + last.end - last.begin == source->offset) { ++last.end; return; }
        }
        retained_metadata_bytes += source->node.size() + source->resource.size();
        if (chapter.runs.size() >= 8192 || retained_metadata_bytes > 512 * 1024) { xml.stop("Chapter source mapping limit exceeded"); return; }
        if (keep_anchors) chapter.runs.push_back({begin, begin + 1, *source, false, style ? retain_style(*style) : std::uint16_t{0}});
        else { chapter.runs.push_back({begin, begin + 1, {{}, {}, source->offset}}); last_run_node = source->node; }
    }
    bool place_media(const std::string& label, const std::string& path, std::uint32_t& begin,
                     const css::ComputedStyle* style = nullptr) {
        finish_block();
        if (split_pending && !next_part()) return false;
        block_kind = BlockKind::Image;
        if (style) set_block_style(*style);
        std::u32string scalar; decode(label.data(), label.size(), scalar);
        begin = static_cast<std::uint32_t>(chapter.text.size());
        if (scalar.size() > 256) { scalar.resize(255); scalar.push_back(U'…'); }
        for (auto c : scalar) append(c, nullptr);
        retained_metadata_bytes += path.size() + chapter.resource.size();
        if (chapter.runs.size() >= 8192 || retained_metadata_bytes > 512 * 1024) { xml.stop("Chapter source mapping limit exceeded"); return false; }
        if (keep_anchors) chapter.runs.push_back({begin, static_cast<std::uint32_t>(chapter.text.size()), {chapter.resource, path, 0}, true, style ? retain_style(*style) : std::uint16_t{0}});
        else { chapter.runs.push_back({begin, static_cast<std::uint32_t>(chapter.text.size()), {}, true}); last_run_node = path; }
        return true;
    }
    void start(const std::string& name, const char** attrs) {
#ifdef CONFIG_PICO_PAGE_LEGACY_PERF_LOG
        const ChapterCallbackTiming::Scope callback{callback_timing, ChapterCallbackTiming::Phase::Markup};
#endif
        const auto simple_image_style = [](const char** attributes) {
            for (const auto* property : {"transform", "style", "opacity", "filter", "mask", "clip-path"})
                if (!attr_view(attributes, property).empty()) return false;
            // calibre writes "none" on its cover page; the image then covers the whole viewBox and
            // is drawn fitted with its own aspect ratio, as for "xMidYMid meet".
            const auto aspect = attr(attributes, "preserveAspectRatio");
            return aspect.empty() || aspect == "xMidYMid meet" || aspect == "none";
        };
        if (!require_memory_room(0)) return;
        if (xml.html) { chapter.plain_dom = false; warn(chapter, "HTML body uses repaired local source anchors"); }
        static constexpr std::string_view plain_elements[] = {
            "a", "abbr", "article", "aside", "b", "blockquote", "body", "br", "cite", "code",
            "del", "div", "em", "footer", "h1", "h2", "h3", "h4", "h5", "h6",
            "head", "header", "hr", "html", "i", "img", "ins", "li", "link", "mark",
            "meta", "nav", "ol", "p", "q", "s", "section", "small", "span", "strong",
            "style", "sub", "sup", "title", "u", "ul"
        };
        if (chapter.plain_dom && (xml.foreign_namespace || !attr_view(attrs, "space").empty() ||
            !std::binary_search(std::begin(plain_elements), std::end(plain_elements), std::string_view{name})))
            chapter.plain_dom = false;
        Frame frame; frame.name = name;
        {
#ifdef CONFIG_PICO_PAGE_LEGACY_PERF_LOG
            const ChapterCallbackTiming::Scope timing{callback_timing, ChapterCallbackTiming::Phase::Anchors};
#endif
            if (!frames.empty()) {
                auto& parent = frames.back(); parent.in_text = false; parent.has_child = true;
                frame.path = parent.path + '/' + name + '[' + std::to_string(++parent.children[name]) + ']';
                frame.hidden = parent.hidden; frame.body = parent.body; frame.pre = parent.pre;
            } else frame.path = '/' + name + "[1]";
        }
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
        const auto inline_css = attr_view(attrs, "style");
        if (author && inline_css.size() > 4096) { xml.stop(kAuthorUnavailable); return; }
        if (!author && !inline_css.empty()) retain_css("_ {" + std::string(inline_css) + "}");
        if (name == "body") {
            auto at = inline_css.find("background");
            if (at != std::string::npos) at = inline_css.find("url(", at);
            const auto close = at == std::string::npos ? at : inline_css.find(')', at);
            if (close != std::string::npos) {
                auto url = inline_css.substr(at + 4, close - at - 4);
                const auto first = url.find_first_not_of(" \t'\""), last = url.find_last_not_of(" \t'\"");
                std::string fragment;
                if (first != std::string::npos && (!resolve(chapter.resource, std::string(url.substr(first, last - first + 1)), frame.background, fragment) || !fragment.empty()))
                    frame.background.clear();
            }
        }
        if (!author && name == "style" && retain_css("")) frame.css = chapter.css_sources.size() - 1;
        if (!author && name == "link" && token(attr(attrs, "rel"), "stylesheet")) {
            std::string resource, fragment;
            if (css_links.size() < 64 && resolve(chapter.resource, attr(attrs, "href"), resource, fragment) && fragment.empty())
                css_links.push_back(std::move(resource));
            else { chapter.plain_dom = false; warn(chapter, "External or excessive stylesheet references are not loaded"); }
        }
        if (!author && (!inline_css.empty() || name == "style" || (name == "link" && token(attr(attrs, "rel"), "stylesheet")))) warn(chapter, "CSS is rendered with basic document styles");
        if (name == "script") warn(chapter, "Scripts are not executed");
        frame.block = name == "p" || name == "div" || name == "section" || name == "article" || name == "aside" || name == "blockquote" || name == "li" || name == "pre" || name == "tr" || name == "table" || name == "dl" || name == "dt" || name == "dd" || (name.size() == 2 && name[0] == 'h' && name[1] >= '1' && name[1] <= '6');
        if (name.size() == 2 && name[0] == 'h' && name[1] >= '1' && name[1] <= '6') frame.kind = BlockKind::Heading;
        else if (name == "li") frame.kind = BlockKind::ListItem;
        else if (name == "pre") { frame.kind = BlockKind::Preformatted; frame.pre = true; }
        if (attr(attrs, "space") == "preserve") frame.pre = true;
        const css::ComputedStyle* style = nullptr;
        if (author) {
            const bool block = frame.block || name == "body";
            if (!author->start(frames.size(), name, attrs, block, frame.kind == BlockKind::Heading)) {
                xml.stop(kAuthorUnavailable); return;
            }
            style = &author->ancestors[frames.size()].style;
            if (style->display != css::Display::None) frame.block = style->display == css::Display::Block;
        }
        if (frame.body && !frame.hidden) {
            if (author && name == "br" && !frame.block) {
                pending_space = false;
                append(U'\n', nullptr);
            } else if (frame.block || name == "br") {
                finish_block();
                if (!style || frame.block) block_kind = frame.kind;
                if (style && frame.block) set_block_style(*style);
            }
            if (name == "td" || name == "th") { if (!chapter.text.empty() && chapter.text.back() != U'\n') append(U'\t', nullptr); warn(chapter, "Tables are rendered as text rows"); }
            auto id = attr(attrs, "id");
            if (!id.empty() && !record_fragment(std::move(id))) return;
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
                if (!place_media(label, frame.path, begin, style)) return;
                if (name == "img" || name == "image" || name == "svg") {
                    if (chapter.images.size() >= 256) { xml.stop("Chapter image reference limit exceeded"); return; }
                    DocumentImage image{begin, static_cast<std::uint32_t>(chapter.text.size()), "", attr(attrs, "alt"), {chapter.resource, frame.path, 0}};
                    std::string fragment;
                    if (name != "svg" && (!resolve(chapter.resource, attr(attrs, name == "img" ? "src" : "href"), image.resource, fragment) || !fragment.empty())) image.resource.clear();
                    if (name == "img" && optional_image_uri(attr(attrs, "src")))
                        warn(chapter, "Inline or source-denied image uses an explicit placeholder");
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
#ifdef CONFIG_PICO_PAGE_LEGACY_PERF_LOG
        const ChapterCallbackTiming::Scope callback{callback_timing, ChapterCallbackTiming::Phase::Markup};
#endif
        if (frames.empty()) return;
        auto frame = std::move(frames.back()); frames.pop_back();
        if (frame.name == "svg" && frame.image < chapter.images.size() && (!frame.simple_svg || frame.svg_images != 1)) chapter.images[frame.image].resource.clear();
        if (frame.link != std::numeric_limits<std::size_t>::max()) chapter.links[frame.link].end = static_cast<std::uint32_t>(chapter.text.size());
        if (!frame.background.empty() && part == 0 && !captured && chapter.text.empty()) {
            std::uint32_t begin;
            if (!place_media("[图片：未渲染]", frame.path, begin, author ? &author->ancestors[frames.size()].style : nullptr)) return;
            chapter.images.push_back({begin, static_cast<std::uint32_t>(chapter.text.size()), frame.background, "", {chapter.resource, frame.path, 0}});
            retained_metadata_bytes += frame.background.size();
            finish_block(); block_kind = BlockKind::Paragraph;
            warn(chapter, "Images and media use explicit placeholders");
        }
        if (frame.body && !frame.hidden && frame.block) finish_block();
        if (frames.empty()) return;
        frames.back().in_text = false;
        if (!author) { block_kind = frames.back().kind; return; }
        author->names_used = author->ancestors[frames.size()].names_begin;
        if (frame.block || frame.image != std::numeric_limits<std::size_t>::max()) {
            const auto parent = std::find_if(frames.rbegin(), frames.rend(), [](const Frame& f) { return f.block; });
            block_kind = parent == frames.rend() ? BlockKind::Paragraph : parent->kind;
            if (parent != frames.rend()) set_block_style(author->ancestors[frames.size() - 1 - (parent - frames.rbegin())].style);
        }
    }
    void characters(const char* bytes, std::size_t size) {
        if (frames.empty() || !require_memory_room(0)) return;
#ifdef CONFIG_PICO_PAGE_LEGACY_PERF_LOG
        const ChapterCallbackTiming::Scope timing{callback_timing, ChapterCallbackTiming::Phase::Text};
#endif
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
        if (new_node) { ++frame.text_index; frame.source_offset = 0; frame.mapping_source_length = 0; frame.in_text = true; }
        if (keep_mapping && frame.body && !chapter.long_source_node) {
            // CREngine splits at 8192 source scalars before entity expansion and whitespace
            // folding. Count the original event, including entity spelling and CRLF; when
            // Expat does not expose it, its byte count is a conservative upper bound.
            const int count = XML_GetCurrentByteCount(xml.parser);
            int offset = 0, available = 0;
            const char* context = XML_GetInputContext(xml.parser, &offset, &available);
            std::size_t length = count > 0 ? static_cast<std::size_t>(count) : kKoreaderTextSplitScalars;
            if (xml.source_utf8 && context && count > 0 && length < 4 * kKoreaderTextSplitScalars &&
                offset >= 0 && offset <= available && count <= available - offset) {
                for (int i = 0; i < count; ++i)
                    if ((static_cast<unsigned char>(context[offset + i]) & 0xc0) == 0x80) --length;
            }
            frame.mapping_source_length = std::min(kKoreaderTextSplitScalars, frame.mapping_source_length + length);
            if (frame.mapping_source_length >= kKoreaderTextSplitScalars) chapter.long_source_node = true;
        }
        std::u32string scalars;
        if (!decode(bytes, size, scalars)) { xml.stop("Invalid UTF-8 XML text"); return; }
        source_count += static_cast<std::uint32_t>(scalars.size());
        if (source_count > kMaxText * 2) { xml.stop("Chapter source scalar limit exceeded"); return; }
        if (!frame.body || frame.hidden) {
            frame.source_offset += static_cast<std::uint32_t>(scalars.size());
            return;
        }
        if (new_node || frame.source_node == std::numeric_limits<std::size_t>::max()) {
#ifdef CONFIG_PICO_PAGE_LEGACY_PERF_LOG
            const ChapterCallbackTiming::Scope anchor_timing{callback_timing, ChapterCallbackTiming::Phase::Anchors};
#endif
            if (new_node) frame.node = frame.path + "/text()[" + std::to_string(frame.text_index) + ']';
            retained_metadata_bytes += frame.node.size();
            if (part_source_nodes >= 8192 || retained_metadata_bytes > 512 * 1024) { xml.stop("Chapter source node limit exceeded"); return; }
            ++part_source_nodes;
            frame.source_node = chapter.source_nodes.size();
            if (keep_mapping) chapter.source_nodes.push_back({frame.node, {}, new_node && first_child});
        }
        if (keep_mapping) {
            auto& raw = chapter.source_nodes[frame.source_node].text;
            const auto size = raw.size() + scalars.size();
            if (size > raw.capacity() && !require_memory_room(sizeof(char32_t) * std::max(2 * raw.capacity(), size))) return;
            raw += scalars;
        }
        auto source = [&] {
#ifdef CONFIG_PICO_PAGE_LEGACY_PERF_LOG
            const ChapterCallbackTiming::Scope anchor_timing{callback_timing, ChapterCallbackTiming::Phase::Anchors};
#endif
            return Anchor{chapter.resource, frame.node, 0};
        }();
        const auto* style = author ? &author->ancestors[frames.size() - 1].style : nullptr;
        for (auto cp : scalars) {
            source.offset = frame.source_offset++;
            if (!frame.pre && space(cp)) {
                if (!pending_space && chapter.text.size() > block_start && chapter.text.back() != U'\n') { pending_space = true; pending_anchor = source; if (style) pending_space_style = *style; }
                continue;
            }
            if (pending_space) { append(U' ', &pending_anchor, style ? &pending_space_style : nullptr); pending_space = false; }
            append(cp, &source, style);
            if (!xml.error.empty()) return;
        }
    }
};
}

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

#ifdef CONFIG_PICO_PAGE_LEGACY_PERF_LOG
struct ChapterStreamTiming {
    ZipTiming::Clock::time_point started = ZipTiming::Clock::now();
    std::int64_t zip_read_inflate_us = 0, xml_us = 0, memory_check_us = 0;
    ChapterCallbackTiming callbacks;

    template <typename Operation>
    static auto measure(std::int64_t& elapsed_us, Operation operation) {
        const auto start = ZipTiming::Clock::now();
        const auto result = operation();
        elapsed_us += ZipTiming::elapsed(start);
        return result;
    }

    template <typename Operation>
    bool measure_xml(ChapterParser& parser, Operation operation) {
        struct ActiveCallbacks {
            ChapterParser& parser;
            ~ActiveCallbacks() { parser.callback_timing = nullptr; }
        } active{parser};
        parser.callback_timing = &callbacks;
        return measure(xml_us, operation);
    }

    ~ChapterStreamTiming() {
        using Phase = ChapterCallbackTiming::Phase;
        ESP_LOGI("reader_perf", "stage=chapter_sequential_read us=%lld min_us=0 zip_read_inflate_us=%lld xml_us=%lld memory_check_us=%lld callback_memory_us=%lld callback_memory_calls=%u callback_text_us=%lld callback_anchor_us=%lld callback_markup_us=%lld",
            static_cast<long long>(ZipTiming::elapsed(started)), static_cast<long long>(zip_read_inflate_us),
            static_cast<long long>(xml_us), static_cast<long long>(memory_check_us),
            callbacks.microseconds(Phase::Memory), callbacks.calls[static_cast<std::size_t>(Phase::Memory)],
            callbacks.microseconds(Phase::Text), callbacks.microseconds(Phase::Anchors), callbacks.microseconds(Phase::Markup));
    }
};
#endif
}

struct ChapterStream {
    std::size_t index;
    Chapter ready;
    ChapterParser parser;
    mz_zip_reader_extract_iter_state* iterator = nullptr;
    std::array<char, 4096> input{};
    bool input_finished = false;
    ChapterStream(std::size_t value, const std::string& resource) : index(value), parser(ready, resource) {
        parser.keep_mapping = false; parser.keep_anchors = true; parser.resumable = true;
    // Expat retains incomplete token bytes while a parser remains open between calls.
        parser.xml.pending_limit = 64U * 1024;
    }
    ~ChapterStream() { if (iterator) mz_zip_reader_extract_iter_free(iterator); }
};

namespace {
constexpr std::size_t kImageHeaderBytes = 64U * 1024, kKnownImageSizes = 256;
enum class ImageHeader { Found, Incomplete, Invalid };
ImageHeader image_header_size(std::string_view bytes, std::uint32_t& width, std::uint32_t& height) {
    const auto byte = [&](std::size_t i) { return static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[i])); };
    const auto big16 = [&](std::size_t i) { return byte(i) << 8 | byte(i + 1); };
    if (bytes.size() < 10) return ImageHeader::Incomplete;
    if (bytes.substr(0, 8) == std::string_view("\x89PNG\r\n\x1a\n", 8)) {
        if (bytes.size() < 24) return ImageHeader::Incomplete;
        if (bytes.substr(12, 4) != "IHDR") return ImageHeader::Invalid;
        width = big16(16) << 16 | big16(18);
        height = big16(20) << 16 | big16(22);
        return ImageHeader::Found;
    }
    if (bytes.substr(0, 6) == "GIF87a" || bytes.substr(0, 6) == "GIF89a") {
        width = byte(6) | byte(7) << 8;
        height = byte(8) | byte(9) << 8;
        return ImageHeader::Found;
    }
    if (byte(0) != 0xff || byte(1) != 0xd8) return ImageHeader::Invalid;
    for (std::size_t at = 2;;) {
        if (at + 4 > bytes.size()) return ImageHeader::Incomplete;
        if (byte(at) != 0xff) return ImageHeader::Invalid;
        const auto marker = byte(at + 1);
        if (marker == 0xff) { ++at; continue; }
        if (marker == 0x01 || (marker >= 0xd0 && marker <= 0xd8)) { at += 2; continue; }
        if (marker == 0xd9 || marker == 0xda) return ImageHeader::Invalid;
        const auto length = big16(at + 2);
        if (length < 2) return ImageHeader::Invalid;
        if (marker >= 0xc0 && marker <= 0xcf && marker != 0xc4 && marker != 0xc8 && marker != 0xcc) {
            if (at + 9 > bytes.size()) return ImageHeader::Incomplete;
            height = big16(at + 5);
            width = big16(at + 7);
            return ImageHeader::Found;
        }
        at += 2 + length;
    }
}
}

struct EpubDocument::BookIndex {
    DocumentMetadata metadata;
    std::vector<SpineItem> spine;
    std::vector<TocEntry> toc;
};

struct EpubDocument::Impl {
    FILE* file = nullptr;
    std::unique_ptr<ChapterStream> chapter_stream;
    std::uint64_t source_read_bytes = 0, xml_feed_bytes = 0;
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
    VerifiedIndex index;
    const BookIndex& book() const {
        static const BookIndex empty;
        return index ? *index : empty;
    }
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
            PP_HOST_WORK(host_work::add(host_work::ZipComparisons);)
            return std::strcmp(name, target.c_str()) < 0;
        });
        if (found == end) return nullptr;
        mz_zip_reader_get_filename(&zip, found->member, name, sizeof(name));
        PP_HOST_WORK(host_work::add(host_work::ZipComparisons);)
        return path == name ? found : nullptr;
    }
    std::vector<std::uint32_t> html_repair_offsets;
    std::string cover, cover_page;
    ~Impl() { chapter_stream.reset(); if (zip.m_pState) mz_zip_reader_end(&zip); if (file) std::fclose(file); }
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
        PP_HOST_WORK(host_work::add(host_work::SourceBytes, count);)
        self->source_read_bytes += count;
        return count;
    }
    bool enable_html_repair(std::size_t index, const Xml& xml) {
        if (index >= book().spine.size() || xml.in_cdata || !xml.allow_html || !xml.html_declared || xml.html_fallback || !xml.html_syntax_error() || xml.error.rfind("Invalid XML:", 0) != 0) return false;
        html_repair_offsets.resize(book().spine.size(), kNoHtmlRepair);
        html_repair_offsets[index] = static_cast<std::uint32_t>(xml.completed_bytes);
        return true;
    }
    std::unique_ptr<AuthorLayoutState> prepare_author_styles(std::size_t index, const std::function<bool()>& stop);
    std::unordered_map<std::string, std::pair<std::uint16_t, std::uint16_t>> image_sizes;
    bool read_image_sizes(Chapter& chapter, std::string& error, const std::function<bool()>& stop) {
        const StageTimer timer{"image_header_read", 0};
        for (auto& image : chapter.images) {
            if (stop && stop()) return fail(error, kChapterCancelled);
            if (const auto known = image_sizes.find(image.resource); known != image_sizes.end()) {
                std::tie(image.width, image.height) = known->second;
                continue;
            }
            const auto* entry = image.resource.empty() ? nullptr : find_member(image.resource);
            if (entry && !memory_room(kImageHeaderBytes + 128U * 1024))
                return fail(error, std::string(kChapterMemoryError) + ": " + image.resource);
            struct Header { std::string bytes; std::uint32_t width = 0, height = 0; ImageHeader state = ImageHeader::Incomplete; } header;
            if (entry && !mz_zip_reader_extract_to_callback(&zip, entry->member,
                [](void* opaque, mz_uint64, const void* data, std::size_t count) -> std::size_t {
                    auto& header = *static_cast<Header*>(opaque);
                    header.bytes.append(static_cast<const char*>(data), std::min(count, kImageHeaderBytes - header.bytes.size()));
                    header.state = image_header_size(header.bytes, header.width, header.height);
                    return header.state == ImageHeader::Incomplete && header.bytes.size() < kImageHeaderBytes ? count : 0;
                }, &header, 0)) {
                const auto zip_error = mz_zip_get_last_error(&zip);
                if (zip_error == MZ_ZIP_ALLOC_FAILED)
                    return fail(error, std::string(kChapterMemoryError) + ": " + image.resource);
                if (zip_error == MZ_ZIP_FILE_READ_FAILED || zip_error == MZ_ZIP_FILE_SEEK_FAILED)
                    return fail(error, "Cannot read EPUB image header: " + image.resource);
            }
            if (header.state == ImageHeader::Found && header.width && header.height &&
                header.width <= 0xffff && header.height <= 0xffff) {
                image.width = static_cast<std::uint16_t>(header.width);
                image.height = static_cast<std::uint16_t>(header.height);
            }
            if (image_sizes.size() >= kKnownImageSizes) image_sizes.clear();
            image_sizes.emplace(image.resource, std::make_pair(image.width, image.height));
        }
        return true;
    }
    bool load(std::size_t index, Chapter& chapter, std::string& error, ChapterLoad detail,
              std::uint32_t wanted, const std::function<bool(const Chapter&)>* match, const std::function<bool()>& stop,
              const std::function<void(const Chapter&)>& visit);
    bool load_txt(Chapter& chapter, std::string& error, ChapterLoad detail, std::uint32_t wanted,
                  const std::function<bool(const Chapter&)>* match, const std::function<bool()>& stop,
                  const std::function<void(const Chapter&)>& visit);
    bool parse(const std::string& path, Xml& xml, std::string& error, std::uint32_t max_bytes = kMaxXmlBytes, const std::function<bool()>& stop = {}, ContentFailureKind* failure_kind = nullptr) {
        const auto fail = content_failure_reporter(failure_kind);
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
            Impl& owner;
            ZipTiming& timing;
            const std::function<bool()>& stop;
            bool cancelled = false;
#ifdef ESP_PLATFORM
            TickType_t yielded_at = xTaskGetTickCount();
#endif
        } feed{xml, *this, timing, stop};
        const bool extracted = mz_zip_reader_extract_to_callback(&zip, entry->member, [](void* opaque, mz_uint64, const void* data, std::size_t count) -> std::size_t {
            auto& feed = *static_cast<Feed*>(opaque);
            for (std::size_t offset = 0; offset < count;) {
#ifdef ESP_PLATFORM
                // Cached ZIP reads can remain CPU-bound; periodic blocking admits the lower-priority idle task.
                if (xTaskGetTickCount() - feed.yielded_at >= pdMS_TO_TICKS(20)) {
                    vTaskDelay(1);
                    feed.yielded_at = xTaskGetTickCount();
                }
#endif
                feed.timing.checkpoint();
                if (feed.stop && feed.stop()) {
                    feed.cancelled = true;
                    return 0;
                }
                const auto size = std::min<std::size_t>(4096, count - offset);
#ifdef CONFIG_PICO_PAGE_LEGACY_PERF_LOG
                const auto started = ZipTiming::Clock::now();
#endif
                PP_HOST_WORK(host_work::add(host_work::XmlBytes, size);)
                feed.owner.xml_feed_bytes += size;
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
            return fail(error, kChapterCancelled, ContentFailureKind::Cancelled);
        }
        if (!extracted) return fail(error, xml.error.empty() ? "Invalid compressed EPUB resource: " + path : xml.error + ": " + path, xml.error.empty() ? zip_failure_kind(zip) : xml.failure_kind);
#ifdef CONFIG_PICO_PAGE_LEGACY_PERF_LOG
        const auto started = ZipTiming::Clock::now();
#endif
        const bool finished = xml.feed("", 0, true);
#ifdef CONFIG_PICO_PAGE_LEGACY_PERF_LOG
        const auto us = ZipTiming::elapsed(started);
        timing.xml_us += us; timing.xml_max_us = std::max(timing.xml_max_us, us);
#endif
        if (!finished) return fail(error, xml.error + ": " + path, xml.failure_kind);
        return true;
    }
};

EpubDocument::EpubDocument() : impl_(new Impl) {}
EpubDocument::~EpubDocument() = default;
EpubDocument::EpubDocument(EpubDocument&&) noexcept = default;
EpubDocument& EpubDocument::operator=(EpubDocument&&) noexcept = default;
void EpubDocument::close() { impl_.reset(new Impl); }
const DocumentMetadata& EpubDocument::metadata() const { return impl_->book().metadata; }
const std::vector<SpineItem>& EpubDocument::spine() const { return impl_->book().spine; }
const std::vector<TocEntry>& EpubDocument::toc() const { return impl_->book().toc; }

bool EpubDocument::open(const std::string& path, std::string& error, const std::function<bool()>& stop) {
    return open_impl(path, error, OpenMode::Book, nullptr, stop);
}

std::uint64_t identity_hash_count() { return identity_scans.load(std::memory_order_relaxed); }
std::uint64_t epub_open_count() { return epub_opens.load(std::memory_order_relaxed); }

bool EpubDocument::open_verified(const std::string& path, const DocumentMetadata& identity, std::string& error,
                                 const std::function<bool()>& stop, VerifiedIndex index) {
    const auto hex = [](const std::string& s, std::size_t size) {
        return s.size() == size && std::all_of(s.begin(), s.end(), [](char c) {
            return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        });
    };
    if (!hex(identity.book_id, 64) || !hex(identity.partial_md5, 32)) return fail(error, "Invalid verified book identity");
    if (index && index->metadata.book_id == identity.book_id && index->metadata.partial_md5 == identity.partial_md5 &&
        !txt_path(path)) {
        if (!open_impl(path, error, OpenMode::Resources, nullptr, stop)) return false;
        impl_->index = std::move(index);
        return true;
    }
    const auto copy = identity;
    return open_impl(path, error, OpenMode::Book, &copy, stop);
}

EpubDocument::VerifiedIndex EpubDocument::verified_epub_index() const {
    return impl_->txt ? VerifiedIndex{} : impl_->index;
}

bool EpubDocument::open_impl(const std::string& path, std::string& error, OpenMode mode, const DocumentMetadata* identity, const std::function<bool()>& stop, ContentFailureKind* failure_kind) {
    const auto fail = content_failure_reporter(failure_kind);
    auto parsed = std::make_shared<BookIndex>();
    if (txt_path(path)) {
        close(); error.clear();
        if (mode != OpenMode::Book) return fail(error, "TXT 没有内嵌封面", ContentFailureKind::MissingResource);
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
            parsed->metadata.book_id = identity->book_id;
            parsed->metadata.partial_md5 = identity->partial_md5;
            parsed->metadata.txt_encoding = identity->txt_encoding;
            parsed->metadata.txt_offsets = identity->txt_offsets;
            parsed->metadata.txt_headings = identity->txt_headings;
            if (identity->txt_encoding != TxtEncoding::Utf8 && identity->txt_encoding != TxtEncoding::Gb18030 &&
                identity->txt_encoding != TxtEncoding::Utf16Le && identity->txt_encoding != TxtEncoding::Utf16Be)
                return fail(error, "TXT 编码标识无效");
            if ((identity->txt_encoding == TxtEncoding::Utf16Le || identity->txt_encoding == TxtEncoding::Utf16Be) &&
                std::any_of(identity->txt_offsets.begin(), identity->txt_offsets.end(), [](auto offset) { return offset % 2; }))
                return fail(error, "TXT UTF-16 阅读位置索引无效");
            if (identity->txt_encoding != TxtEncoding::Utf8 && (identity->txt_offsets.size() < 2 ||
                identity->txt_offsets.front() != 0 || identity->txt_offsets.back() != next->txt_bytes ||
                std::adjacent_find(identity->txt_offsets.begin(), identity->txt_offsets.end(),
                    std::greater_equal<std::uint32_t>()) != identity->txt_offsets.end()))
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
            Utf16Decoder utf16_decoder;
            bool utf16 = false, utf16_has_text = false;
            TxtTocBuilder unicode_toc, gb_toc;
            std::uint32_t utf8_start = 0, gb_start = 0;
#ifdef ESP_PLATFORM
            auto yielded_at = xTaskGetTickCount();
#endif
            char32_t previous_gb_scalar = 0;
            std::vector<std::uint32_t> part_offsets{0};
            std::uint32_t utf16_cr_boundary = 0;
            std::uint32_t position = 0, next_boundary = kTxtPartBytes;
            for (;;) {
                if (stop && stop()) return fail(error, "TXT 读取已取消");
#ifdef ESP_PLATFORM
                if (xTaskGetTickCount() - yielded_at >= pdMS_TO_TICKS(20)) {
                    vTaskDelay(1); yielded_at = xTaskGetTickCount();
                }
#endif
                const auto count = std::fread(buffer.data(), 1, buffer.size(), next->file);
                if (count && !sha.update(buffer.data(), count)) return fail(error, "TXT SHA-256 failed");
                if (!position && count >= 2 && ((buffer[0] == 0xff && buffer[1] == 0xfe) || (buffer[0] == 0xfe && buffer[1] == 0xff))) {
                    utf16 = true; utf16_decoder.big_endian = buffer[0] == 0xfe;
                    parsed->metadata.txt_encoding = utf16_decoder.big_endian ? TxtEncoding::Utf16Be : TxtEncoding::Utf16Le;
                }
                for (std::size_t i = 0; i < count; ++i) {
                    const auto byte = buffer[i];
                    if (utf16) {
                        if (position + i < 2) continue;
                        char32_t scalar = 0;
                        const auto result = utf16_decoder.feed(byte, scalar);
                        if (result == Utf16Decoder::Result::Invalid) return fail(error, "TXT UTF-16 包含无效代理对或 NUL 字符");
                        if (result == Utf16Decoder::Result::Complete) {
                            if (!space(scalar) && scalar != 0xfeff) utf16_has_text = true;
                            const auto end = static_cast<std::uint32_t>(position + i + 1);
                            unicode_toc.feed(scalar, end - (scalar >= 0x10000 ? 4 : 2));
                            if (utf16_cr_boundary) {
                                const auto split = scalar == U'\n' ? end : utf16_cr_boundary;
                                if (split < next->txt_bytes) { part_offsets.push_back(split); next_boundary += kTxtPartBytes; }
                                utf16_cr_boundary = 0;
                            }
                            if (end >= next_boundary && end < next->txt_bytes) {
                                if (scalar == U'\r') utf16_cr_boundary = end;
                                else { part_offsets.push_back(end); next_boundary += kTxtPartBytes; }
                            }
                        }
                        continue;
                    }
                    if (byte == 0) return fail(error, "TXT 文件包含 NUL 字节");
                    if (utf8_valid) {
                        if (!remaining) utf8_start = position + i;
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
                        if (utf8_valid && !remaining) unicode_toc.feed(cp, utf8_start);
                        if (utf8_valid && !remaining && !space(cp) && cp != 0xfeff) utf8_has_text = true;
                    }
                    if (gb_valid) {
                        if (!gb_decoder.pending() && position + i >= next_boundary && next_boundary < next->txt_bytes) {
                            const auto split = static_cast<std::uint32_t>(position + i +
                                (byte == '\n' && previous_gb_scalar == U'\r'));
                            part_offsets.push_back(split);
                            next_boundary += kTxtPartBytes;
                        }
                        if (!gb_decoder.pending()) gb_start = position + i;
                        char32_t scalar = 0;
                        const auto result = gb_decoder.feed(byte, scalar);
                        if (result == gb18030::Decoder::Result::Invalid) gb_valid = false;
                        else if (result == gb18030::Decoder::Result::Complete) {
                            gb_toc.feed(scalar, gb_start);
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
            if (utf16 ? utf16_decoder.pending() || !utf16_has_text : utf8_valid ? !utf8_has_text : !gb_valid || !gb_has_text)
                return fail(error, utf16 ? "TXT UTF-16 长度无效或没有可阅读的文字" : utf8_valid || gb_valid ? "TXT 没有可阅读的文字" :
                    "TXT 编码不受支持（支持 UTF-8、GB18030、带 BOM 的 UTF-16）");
            if (utf16 || !utf8_valid) {
                if (!utf16) parsed->metadata.txt_encoding = TxtEncoding::Gb18030;
                if (part_offsets.back() != next->txt_bytes) part_offsets.push_back(next->txt_bytes);
                parsed->metadata.txt_offsets = std::move(part_offsets);
            }
            auto& selected_toc = utf16 || utf8_valid ? unicode_toc : gb_toc;
            selected_toc.finish_line();
            parsed->metadata.txt_headings = std::move(selected_toc.headings);
            if (!sha.finish(parsed->metadata.book_id)) return fail(error, "TXT SHA-256 failed");
            for (std::uint64_t offset = 0; offset < static_cast<std::uint64_t>(length); offset = offset == 0 ? 1024 : offset * 4) {
                const auto size = static_cast<std::size_t>(std::min<std::uint64_t>(1024, length - offset));
                if (Impl::read(next.get(), offset, buffer.data(), size) != size || !md5.update(buffer.data(), size))
                    return fail(error, "TXT partial MD5 failed");
            }
            if (!md5.finish(parsed->metadata.partial_md5)) return fail(error, "TXT partial MD5 failed");
        }
        const auto slash = path.rfind('/');
        parsed->metadata.title = path.substr(slash == std::string::npos ? 0 : slash + 1);
        parsed->metadata.title.resize(parsed->metadata.title.size() - 4);
        parsed->spine.push_back({"text", "text", true, static_cast<std::uint32_t>(length)});
        for (const auto& heading : parsed->metadata.txt_headings)
            parsed->toc.push_back({heading.label, "text", "", 0});
        if (parsed->toc.empty()) parsed->toc.push_back({"正文", "text", "", 0});
        next->index = std::move(parsed);
        impl_ = std::move(next);
        return true;
    }
    const StageTimer timer{"epub_open", 0};
    epub_opens.fetch_add(1, std::memory_order_relaxed);
    close(); error.clear();
    if (stop && stop()) return fail(error, "EPUB open cancelled", ContentFailureKind::Cancelled);
    std::unique_ptr<Impl> next(new Impl);
    next->file = std::fopen(path.c_str(), "rb");
    if (!next->file) return fail(error, "Cannot open EPUB", ContentFailureKind::IoError);
    if (std::fseek(next->file, 0, SEEK_END) != 0) return fail(error, "Cannot seek EPUB", ContentFailureKind::IoError);
    const auto length = std::ftell(next->file);
    if (length < 0) return fail(error, "Cannot seek EPUB", ContentFailureKind::IoError);
    if (length < 22 || static_cast<std::uint64_t>(length) > kMaxBook) return fail(error, "EPUB file size is invalid or exceeds 256 MiB");
    std::vector<unsigned char> tail(static_cast<std::size_t>(std::min<long>(length, 65557)));
    if (Impl::read(next.get(), length - tail.size(), tail.data(), tail.size()) != tail.size()) return fail(error, "Cannot read ZIP directory", ContentFailureKind::IoError);
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
    {
        PP_HOST_WORK(const host_work::Stage zip_stage{"zip_init"};)
        if (!mz_zip_reader_init(&next->zip, length, 0)) return fail(error, "Invalid EPUB ZIP or ZIP memory limit exceeded", zip_failure_kind(next->zip));
    }
    if (mz_zip_is_zip64(&next->zip) || mz_zip_reader_get_num_files(&next->zip) > 4096) return fail(error, "ZIP64 or oversized ZIP directory is unsupported");
    next->zip_entries = mz_zip_reader_get_num_files(&next->zip);
    next->zip_index.reset(static_cast<Impl::ZipEntry*>(std::calloc(next->zip_entries, sizeof(Impl::ZipEntry))));
    if (!next->zip_index) return fail(error, "ZIP index allocation failed", ContentFailureKind::OutOfMemory);
    {
        const StageTimer timer{"zip_index_build", 0};
        PP_HOST_WORK(const host_work::Stage index_stage{"zip_index_build"};)
        std::uint64_t total = 0;
        for (mz_uint i = 0; i < next->zip_entries; ++i) {
            if (stop && stop()) return fail(error, "EPUB open cancelled", ContentFailureKind::Cancelled);
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
            PP_HOST_WORK(host_work::add(host_work::ZipComparisons);)
            return std::strcmp(left, right);
        };
        auto* entries = next->zip_index.get();
        std::sort(entries, entries + next->zip_entries, [&](const auto& a, const auto& b) { return compare(a, b) < 0; });
        for (std::size_t i = 1; i < next->zip_entries; ++i) {
            if (stop && stop()) return fail(error, "EPUB open cancelled", ContentFailureKind::Cancelled);
            if (compare(entries[i - 1], entries[i]) == 0) return fail(error, "Unsafe or duplicate ZIP path");
        }
    }
    if (next->find_member("META-INF/encryption.xml")) return fail(error, "Encrypted or obfuscated EPUB is unsupported");
    char mime[64]{};
    mz_zip_archive_file_stat mime_stat{};
    const int mime_index = mz_zip_reader_locate_file(&next->zip, "mimetype", nullptr, 0);
    if (mime_index < 0) return fail(error, "Missing EPUB mimetype");
    if (!mz_zip_reader_file_stat(&next->zip, mime_index, &mime_stat)) return fail(error, "Cannot read EPUB mimetype", zip_failure_kind(next->zip));
    if (mime_stat.m_is_directory) return fail(error, "Invalid EPUB mimetype");
    if (mime_stat.m_uncomp_size > sizeof(mime)) return fail(error, "EPUB mimetype exceeds limit");
    if (!mz_zip_reader_extract_to_mem(&next->zip, mime_index, mime, sizeof(mime), 0)) return fail(error, "Cannot read EPUB mimetype", zip_failure_kind(next->zip));
    if (trim(std::string(mime, static_cast<std::size_t>(mime_stat.m_uncomp_size))) != "application/epub+zip")
        return fail(error, "Invalid EPUB mimetype");
    if (mode == OpenMode::Resources) {
        impl_ = std::move(next);
        return true;
    }
    if (identity) {
        parsed->metadata.book_id = identity->book_id;
        parsed->metadata.partial_md5 = identity->partial_md5;
    } else if (mode == OpenMode::Book) {
        identity_scans.fetch_add(1, std::memory_order_relaxed);
        const StageTimer timer{"identity_read_hash", 0};
        Hash sha(false), md5(true); std::array<unsigned char, 8192> buffer{};
        if (std::fseek(next->file, 0, SEEK_SET) != 0) return fail(error, "Cannot hash EPUB");
        for (;;) {
            if (stop && stop()) return fail(error, "EPUB open cancelled", ContentFailureKind::Cancelled);
            const auto size = std::fread(buffer.data(), 1, buffer.size(), next->file);
            if (size && !sha.update(buffer.data(), size)) return fail(error, "SHA-256 failed");
            if (size < buffer.size()) { if (std::ferror(next->file)) return fail(error, "Cannot hash EPUB"); break; }
        }
        for (std::uint64_t offset = 0;; offset = offset == 0 ? 1024 : offset * 4) {
            if (offset >= static_cast<std::uint64_t>(length) || offset > 1073741824ULL) break;
            const auto size = static_cast<std::size_t>(std::min<std::uint64_t>(1024, length - offset));
            if (Impl::read(next.get(), offset, buffer.data(), size) != size || !md5.update(buffer.data(), size)) return fail(error, "Partial MD5 failed");
        }
        if (!sha.finish(parsed->metadata.book_id) || !md5.finish(parsed->metadata.partial_md5)) return fail(error, "EPUB identity hash failed");
    }
    Xml container;
    std::string package;
    bool rootfile_found = false;
    container.start = [&](const std::string& name, const char** attrs) {
        if (!rootfile_found && name == "rootfile" && (attr(attrs, "media-type").empty() || attr(attrs, "media-type") == "application/oebps-package+xml")) {
            rootfile_found = true; package = attr(attrs, "full-path");
        }
    };
    if (!next->parse("META-INF/container.xml", container, error, kMaxXmlBytes, stop, failure_kind)) return false;
    std::string normalized;
    if (!path_parts(package, normalized) || package != normalized) return fail(error, "Invalid package path");
    PackageBudget budget;
    std::string navigation, ncx;
    {
        PP_HOST_WORK(const host_work::Stage opf_stage{"epub_opf"};)
        struct RawItem { std::string id, href, type, properties; };
        std::vector<RawItem> items;
        std::vector<std::pair<std::string, bool>> itemrefs;
        std::vector<std::string> guide_covers, elements;
        std::string cover_id, text, metadata_name;
        unsigned metadata_depth = 0;
        bool missing_package = false;
        Xml opf;
        opf.start = [&](const std::string& name, const char** attrs) {
            if (elements.empty() && name != "package") { missing_package = true; opf.stop("Missing EPUB package"); return; }
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
                auto& target = metadata_name == "title" ? parsed->metadata.title : metadata_name == "creator" ? parsed->metadata.author : parsed->metadata.language;
                auto value = trim(text);
                budget.bytes -= text.size(); text.clear();
                if (target.empty()) budget.replace(opf, target, std::move(value));
                metadata_depth = 0;
            }
            elements.pop_back();
        };
        if (!next->parse(package, opf, error, kMaxXmlBytes, stop, failure_kind))
            return missing_package ? fail(error, opf.error) : false;
#ifdef ESP_PLATFORM
        const auto spine_bytes = itemrefs.size() * sizeof(SpineItem);
        if (heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) < spine_bytes)
            return fail(error, kChapterMemoryError, ContentFailureKind::OutOfMemory);
#endif
        parsed->spine.reserve(itemrefs.size());
        struct Item { std::string path, type, properties; };
        std::map<std::string, Item> manifest;
        for (auto& item : items) {
            if (stop && stop()) return fail(error, kChapterCancelled, ContentFailureKind::Cancelled);
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
            if (stop && stop()) return fail(error, kChapterCancelled, ContentFailureKind::Cancelled);
            std::string resource, fragment;
            if (resolve(package, href, resource, fragment)) budget.replace(opf, next->cover_page, std::move(resource));
        }
        const StageTimer spine_timer{"epub_spine_validate", 0};
        PP_HOST_WORK(const host_work::Stage spine_stage{"epub_spine_validate"};)
        for (auto& ref : itemrefs) {
            if (stop && stop()) return fail(error, kChapterCancelled, ContentFailureKind::Cancelled);
            const auto& id = ref.first; auto item = manifest.find(id);
            if (item == manifest.end() || item->second.type != "application/xhtml+xml") return fail(error, "Unsupported or missing spine resource");
            const auto* entry = next->find_member(item->second.path);
            if (!entry) return fail(error, "Unsupported or missing spine resource");
            if (!budget.add(opf, item->second.path.size())) return fail(error, opf.error);
            parsed->spine.push_back({item->second.path, std::move(ref.first), ref.second, entry->bytes});
        }
        if (!opf.error.empty()) return fail(error, opf.error);
    }
    if (parsed->spine.empty()) return fail(error, "EPUB spine is empty");
    if (parsed->metadata.title.empty()) { auto slash = path.rfind('/'); parsed->metadata.title = path.substr(slash == std::string::npos ? 0 : slash + 1); }
    budget.bytes = parsed->metadata.title.size() + parsed->metadata.author.size() + parsed->metadata.language.size() + next->cover.size() + next->cover_page.size() + navigation.size() + ncx.size();
    std::size_t fallback_bytes = 0;
    for (const auto& item : parsed->spine) fallback_bytes += item.id.size() + item.resource.size();
    budget.bytes += fallback_bytes;
    if (budget.bytes > kMaxPackageStrings || fallback_bytes > kMaxPackageStrings - budget.bytes)
        return fail(error, "Package/navigation text limit exceeded");
    if (mode == OpenMode::Cover) { next->index = std::move(parsed); impl_ = std::move(next); return true; }
    PP_HOST_WORK(const host_work::Stage toc_stage{"epub_toc"};)
    for (const auto* source : {&navigation, &ncx}) {
        if (source->empty() || !parsed->toc.empty()) continue;
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
            parsed->toc = std::move(toc);
        } else if (toc_error == kChapterCancelled) return fail(error, toc_error);
    }
    if (parsed->toc.empty()) for (const auto& item : parsed->spine) parsed->toc.push_back({item.id, item.resource, "", 0});
    next->index = std::move(parsed);
    impl_ = std::move(next);
    return true;
}

bool EpubDocument::read_cover(const std::string& path, std::vector<std::uint8_t>& bytes, std::string& error, const std::function<bool()>& stop, ContentFailureKind* failure_kind) {
    ContentFailureKind local_failure;
    if (!failure_kind) failure_kind = &local_failure;
    const auto fail = content_failure_reporter(failure_kind);
    bytes.clear(); error.clear();
    if (stop && stop()) return fail(error, kChapterCancelled, ContentFailureKind::Cancelled);
    EpubDocument book;
    if (!book.open_impl(path, error, OpenMode::Cover, nullptr, stop, failure_kind)) return false;
    const auto unavailable = [failure_kind] { return content_failure_is_retryable(*failure_kind); };
    if (!book.impl_->cover.empty() && book.read_image(book.impl_->cover, bytes, error, stop, failure_kind)) return true;
    if (unavailable()) return false;
    if (stop && stop()) return fail(error, kChapterCancelled, ContentFailureKind::Cancelled);
    Chapter unused;
    const auto resource = book.impl_->cover_page.empty() ? book.impl_->book().spine.front().resource : book.impl_->cover_page;
    if (!book.impl_->cover_page.empty() && book.read_image(resource, bytes, error, stop, failure_kind)) return true;
    if (unavailable()) return false;
    if (stop && stop()) return fail(error, kChapterCancelled, ContentFailureKind::Cancelled);
    error.clear();
    ChapterParser parser(unused, resource);
    if (!book.impl_->parse(resource, parser.xml, error, 262144, stop, failure_kind)) return false;
    parser.finish_block();
    const auto& chapter = parser.chapter;
    if (parser.part != 0 || chapter.images.size() != 1 || chapter.blocks.size() != 1 || chapter.blocks.front().kind != BlockKind::Image)
        return fail(error, "EPUB has no supported cover", ContentFailureKind::MissingResource);
    return book.read_image(chapter.images.front().resource, bytes, error, stop, failure_kind);
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
bool FileDigest::step(std::size_t budget_bytes, const std::function<bool()>& stop) {
    auto& state = *impl_;
    if (state.finished) return true;
    if (!budget_bytes || (stop && stop())) return false;
    if (!state.file) {
        state.file.reset(std::fopen(state.path.c_str(), "rb"));
        if (!state.file) { state.finished = true; return true; }
    }
    std::array<unsigned char, 8192> bytes;
    while (budget_bytes && (!stop || !stop())) {
        const auto requested = std::min(budget_bytes, bytes.size());
        const auto count = std::fread(bytes.data(), 1, requested, state.file.get());
        if (count && !state.sha.update(bytes.data(), count)) {
            state.finished = true;
        } else if (count < requested) {
            if (!std::ferror(state.file.get())) state.sha.finish(state.value);
            state.finished = true;
        }
        if (state.finished) { state.file.reset(); return true; }
        budget_bytes -= count;
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

struct Sha256Digest::Impl {
    Hash hash{false};
    bool finished = false;
};
Sha256Digest::Sha256Digest() : impl_(std::make_unique<Impl>()) {}
Sha256Digest::~Sha256Digest() = default;
bool Sha256Digest::update(std::string_view bytes) {
    return !impl_->finished && impl_->hash.update(reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size());
}
std::string Sha256Digest::finish() {
    if (impl_->finished) return {};
    impl_->finished = true;
    std::string value;
    return impl_->hash.finish(value) ? value : std::string();
}

const document::css::ComputedStyle& Chapter::style(std::uint16_t index) const {
    static constexpr document::css::ComputedStyle initial;
    return index && index <= author_styles.size() ? author_styles[index - 1] : initial;
}

void release(Chapter& chapter) {
    Chapter empty;
    std::swap(chapter, empty);
}

DocumentReadStats EpubDocument::read_stats() const {
    return {impl_->source_read_bytes, impl_->xml_feed_bytes, impl_->zip_allocated, !!impl_->chapter_stream};
}
std::size_t EpubDocument::retained_bytes() const {
    const auto& owner = *impl_;
    std::size_t bytes = sizeof(Impl) + owner.zip_allocated + owner.zip_entries * sizeof(Impl::ZipEntry) +
        owner.cover.capacity() + owner.cover_page.capacity() + owner.html_repair_offsets.capacity() * sizeof(std::uint32_t);
    if (owner.index && owner.index.use_count() == 1) {
        const auto& index = *owner.index;
        const auto& metadata = index.metadata;
        bytes += sizeof(BookIndex) + metadata.title.capacity() + metadata.author.capacity() + metadata.language.capacity() +
            metadata.book_id.capacity() + metadata.partial_md5.capacity() + metadata.txt_offsets.capacity() * sizeof(std::uint32_t) +
            metadata.txt_headings.capacity() * sizeof(TxtHeading) +
            index.spine.capacity() * sizeof(SpineItem) + index.toc.capacity() * sizeof(TocEntry);
        for (const auto& heading : metadata.txt_headings) bytes += heading.label.capacity();
        for (const auto& item : index.spine) bytes += item.resource.capacity() + item.id.capacity();
        for (const auto& item : index.toc) bytes += item.label.capacity() + item.resource.capacity() + item.fragment.capacity();
    }
    if (!owner.chapter_stream) return bytes;
    const auto& stream = *owner.chapter_stream;
    const auto& parser = stream.parser;
    bytes += sizeof(ChapterStream) + parser.frames.capacity() * sizeof(ChapterParser::Frame) + parser.xml.error.capacity();
    if (parser.fragment_ordinals) bytes += ChapterParser::kFragmentIndexBytes;
    if (parser.xml.html) bytes += parser.xml.html->retained_bytes();
    for (const auto& frame : parser.frames) {
        bytes += frame.name.capacity() + frame.path.capacity() + frame.node.capacity() + frame.background.capacity();
        for (const auto& child : frame.children) bytes += sizeof(child) + child.first.capacity();
    }
    for (const auto* part : {&parser.chapter, &stream.ready}) {
        bytes += part->resource.capacity() + part->text.capacity() * sizeof(char32_t) +
            part->blocks.capacity() * sizeof(TextBlock) + part->runs.capacity() * sizeof(TextRun) +
            part->fragments.capacity() * sizeof(FragmentTarget) + part->links.capacity() * sizeof(DocumentLink) +
            part->images.capacity() * sizeof(DocumentImage) + part->warnings.capacity() * sizeof(std::string);
        for (const auto& run : part->runs) bytes += run.source.resource.capacity() + run.source.node.capacity();
        for (const auto& fragment : part->fragments) bytes += fragment.id.capacity();
        for (const auto& link : part->links) bytes += link.resource.capacity() + link.fragment.capacity();
        for (const auto& image : part->images)
            bytes += image.resource.capacity() + image.alt.capacity() + image.source.resource.capacity() + image.source.node.capacity();
        for (const auto& warning : part->warnings) bytes += warning.capacity();
    }
    return bytes;
}
std::unique_ptr<EpubDocument> EpubDocument::detach_reader() {
    if (!impl_->file || impl_->txt || !memory_room(sizeof(EpubDocument) + sizeof(Impl))) return {};
    auto reader = std::make_unique<EpubDocument>();
    reader->impl_->index = impl_->index;
    impl_.swap(reader->impl_);
    return reader;
}
void EpubDocument::reset_chapter_stream() { impl_->chapter_stream.reset(); }
bool EpubDocument::read_next_chapter_part(std::size_t index, Chapter& chapter, std::string& error,
                                          const std::function<bool()>& stop) {
#ifdef CONFIG_PICO_PAGE_LEGACY_PERF_LOG
    ChapterStreamTiming timing;
#endif
    release(chapter); error.clear();
    auto& owner = *impl_;
    const auto abort = [&](const std::string& message) {
        const bool retry = owner.chapter_stream && owner.enable_html_repair(index, owner.chapter_stream->parser.xml);
        const auto wanted = owner.chapter_stream ? owner.chapter_stream->parser.part : 0;
        error = message; owner.chapter_stream.reset(); release(chapter);
        if (retry) {
            while (read_next_chapter_part(index, chapter, error, stop)) {
                if (chapter.part == wanted) return true;
                if (chapter.last_part) break;
            }
            release(chapter);
        }
        return false;
    };
    if (stop && stop()) return abort(kChapterCancelled);
    if (!owner.file || owner.txt || index >= owner.book().spine.size()) return abort("Invalid EPUB stream chapter");
    if (owner.chapter_stream && owner.chapter_stream->index != index) owner.chapter_stream.reset();
    if (!owner.chapter_stream) {
        const auto& resource = owner.book().spine[index].resource;
        const auto* entry = owner.find_member(resource);
        if (!entry || entry->bytes > kMaxXmlBytes) return abort("Missing or oversized EPUB stream resource");
        if (!memory_room(sizeof(ChapterStream) + sizeof(mz_zip_reader_extract_iter_state) + 96U * 1024))
            return abort(kChapterMemoryError);
        auto stream = std::make_unique<ChapterStream>(index, resource);
        if (index < owner.html_repair_offsets.size() && owner.html_repair_offsets[index] != kNoHtmlRepair) {
            stream->parser.xml.html_fallback = true;
            stream->parser.xml.html_prefix_bytes = owner.html_repair_offsets[index];
        }
        stream->iterator = mz_zip_reader_extract_iter_new(&owner.zip, entry->member, 0);
        if (!stream->iterator) return abort("Cannot initialize EPUB stream");
        const auto scalars = std::min<std::size_t>(entry->bytes, kMaxText) + 1024;
        if (memory_room(scalars * sizeof(char32_t))) stream->parser.chapter.text.reserve(scalars);
        owner.chapter_stream = std::move(stream);
    }
    auto& stream = *owner.chapter_stream;
    auto& parser = stream.parser;
    parser.captured = false; parser.requested_part = parser.part;
#ifdef ESP_PLATFORM
    auto yielded_at = xTaskGetTickCount();
#endif
    while (!parser.captured) {
#ifdef ESP_PLATFORM
        if (xTaskGetTickCount() - yielded_at >= pdMS_TO_TICKS(20)) {
            vTaskDelay(1); yielded_at = xTaskGetTickCount();
        }
#endif
        if (stop && stop()) return abort(kChapterCancelled);
#ifdef CONFIG_PICO_PAGE_LEGACY_PERF_LOG
        const bool has_room = timing.measure(timing.memory_check_us, [] { return memory_room(0); });
#else
        const bool has_room = memory_room(0);
#endif
        if (!has_room) return abort(kChapterMemoryError);
        if (parser.xml.suspended) {
#ifdef CONFIG_PICO_PAGE_LEGACY_PERF_LOG
            const bool resumed = timing.measure_xml(parser, [&] { return parser.xml.resume(); });
#else
            const bool resumed = parser.xml.resume();
#endif
            if (!resumed) return abort(parser.xml.error);
            continue;
        }
        if (stream.input_finished) {
            parser.finish_block();
            if (!parser.xml.error.empty()) return abort(parser.xml.error);
            if (!parser.seen_body || (parser.part == 0 && parser.chapter.text.empty()))
                return abort("Chapter contains no supported body content");
            parser.capture(true);
            break;
        }
        const auto read = [&] { return mz_zip_reader_extract_iter_read(stream.iterator, stream.input.data(), stream.input.size()); };
#ifdef CONFIG_PICO_PAGE_LEGACY_PERF_LOG
        const auto size = timing.measure(timing.zip_read_inflate_us, read);
#else
        const auto size = read();
#endif
        if (stream.iterator->status < 0) return abort("Invalid compressed EPUB stream");
        if (size) {
            const auto before = parser.xml.bytes;
            const auto feed = [&] { return parser.xml.feed(stream.input.data(), size, false); };
#ifdef CONFIG_PICO_PAGE_LEGACY_PERF_LOG
            const bool parsed = timing.measure_xml(parser, feed);
#else
            const bool parsed = feed();
#endif
            owner.xml_feed_bytes += parser.xml.bytes - before;
            if (!parsed) return abort(parser.xml.error);
            continue;
        }
        // miniz checks the final expanded length and CRC in mz_zip_reader_extract_iter_free.
        const bool valid = mz_zip_reader_extract_iter_free(std::exchange(stream.iterator, nullptr));
        if (!valid) return abort("Invalid EPUB stream length or CRC");
        stream.input_finished = true;
        const auto finish = [&] { return parser.xml.feed("", 0, true); };
#ifdef CONFIG_PICO_PAGE_LEGACY_PERF_LOG
        const bool finished = timing.measure_xml(parser, finish);
#else
        const bool finished = finish();
#endif
        if (!finished) return abort(parser.xml.error);
    }
    if (stop && stop()) return abort(kChapterCancelled);
    chapter = std::move(stream.ready);
    if (chapter.text.capacity() - chapter.text.size() > 4096 && memory_room(chapter.text.size() * sizeof(char32_t))) {
        std::u32string exact(chapter.text.begin(), chapter.text.end()); chapter.text.swap(exact);
    }
    if (chapter.last_part) owner.chapter_stream.reset();
    return true;
}

bool EpubDocument::load_chapter(std::size_t index, Chapter& chapter, std::string& error, ChapterLoad detail, std::uint32_t part,
                                const std::function<bool()>& stop, const std::function<void(const Chapter&)>& visit, bool sequential) {
    if (sequential && detail == ChapterLoad::Reading && part != kLastChapterPart && !impl_->txt) {
        if (impl_->chapter_stream && (impl_->chapter_stream->index != index ||
            impl_->chapter_stream->parser.part > part)) reset_chapter_stream();
        while (read_next_chapter_part(index, chapter, error, stop)) {
            if (visit) visit(chapter);
            if (stop && stop()) { reset_chapter_stream(); release(chapter); return fail(error, kChapterCancelled); }
            if (chapter.part == part) return true;
            if (chapter.last_part) break;
        }
        if (error == kChapterCancelled) return false;
        release(chapter); error.clear();
    }
    reset_chapter_stream();
    return impl_->load(index, chapter, error, detail, part, nullptr, stop, visit);
}
bool EpubDocument::find_chapter_part(std::size_t index, Chapter& chapter, std::string& error, ChapterLoad detail,
                                     const std::function<bool(const Chapter&)>& match, const std::function<bool()>& stop, const std::function<void(const Chapter&)>& visit, bool sequential) {
    reset_chapter_stream();
    if (sequential && detail == ChapterLoad::Reading && !impl_->txt) {
        while (read_next_chapter_part(index, chapter, error, stop)) {
            if (visit) visit(chapter);
            const bool selected = match(chapter);
            if (stop && stop()) { reset_chapter_stream(); release(chapter); return fail(error, kChapterCancelled); }
            if (selected) return true;
            if (chapter.last_part) break;
        }
        if (error == kChapterCancelled) return false;
        reset_chapter_stream(); release(chapter); error.clear();
    }
    return impl_->load(index, chapter, error, detail, 0, &match, stop, visit);
}
std::unique_ptr<AuthorLayoutState> EpubDocument::Impl::prepare_author_styles(
        std::size_t index, const std::function<bool()>& stop) {
    const StageTimer timer{"author_styles_prepare", 0};
    struct Source { std::string path; std::size_t begin = 0, size = 0; };
    std::vector<Source> sources;
    sources.reserve(kAuthorSources);
    std::unique_ptr<char, decltype(&std::free)> raw(nullptr, &std::free);
    std::size_t bytes = 0;
    bool inline_source = false;
    const auto& resource = book().spine[index].resource;
    const auto accepts_media = [](std::string_view media) { return media.empty() || media == "all" || media == "screen"; };
    Xml head;
    head.body_resource = true;
    head.start = [&](const std::string& name, const char** attrs) {
        if (name == "body") { head.stop(kPartComplete); return; }
        if (!accepts_media(attr_view(attrs, "media"))) return;
        if (name == "style") {
            if (sources.size() == kAuthorSources) { head.stop(kAuthorUnavailable); return; }
            sources.push_back({{}, bytes, 0});
            inline_source = true;
        } else if (name == "link" && token(attr_view(attrs, "rel"), "stylesheet")) {
            if (token(attr_view(attrs, "rel"), "alternate")) return;
            if (sources.size() == kAuthorSources) { head.stop(kAuthorUnavailable); return; }
            const auto href = attr_view(attrs, "href");
            if (href.empty() || href.size() > 3 * 512) return;
            std::string path, fragment;
            if (resolve(resource, std::string(href), path, fragment) && fragment.empty())
                sources.push_back({std::string(path.data(), path.size())});
        }
    };
    head.end = [&](const std::string& name) { if (name == "style") inline_source = false; };
    head.text = [&](const char* text, std::size_t count) {
        if (!inline_source) return;
        if (count > kAuthorSourceBytes - bytes) { head.stop(kAuthorUnavailable); return; }
        if (!raw) raw.reset(css_source_buffer());
        if (!raw) { head.stop(kAuthorUnavailable); return; }
        std::memcpy(raw.get() + bytes, text, count);
        bytes += count;
        sources.back().size += count;
    };
    std::string error;
    if (!parse(resource, head, error, kMaxXmlBytes, stop) && head.error != kPartComplete) return {};

    for (auto& source : sources) {
        if (source.path.empty()) continue;
        if (stop && stop()) return {};
        const auto* member = find_member(source.path);
        if (!member || member->bytes > kAuthorSourceBytes - bytes) return {};
        if (!member->bytes) continue;
        if (!raw) raw.reset(css_source_buffer());
        if (!raw) return {};
        struct Read {
            char* destination;
            std::size_t size;
            const std::function<bool()>& stop;
        } read{raw.get() + bytes, member->bytes, stop};
        if (!mz_zip_reader_extract_to_callback(&zip, member->member, [](void* opaque, mz_uint64 offset, const void* buffer, std::size_t count) {
            auto& read = *static_cast<Read*>(opaque);
            if ((read.stop && read.stop()) || offset > read.size || count > read.size - offset) return std::size_t{0};
            std::memcpy(read.destination + offset, buffer, count);
            return count;
        }, &read, 0)) return {};
        source.begin = bytes;
        source.size = member->bytes;
        bytes += source.size;
    }

    const css::Limits limits = bytes ? css::Limits{} : css::Limits{0, 0, 0, 0, 0, 0, 0};
    constexpr std::size_t maximum_pool_bytes = 168 * 1024;
    constexpr std::size_t retained_style_bytes = 4 * kAuthorStyleCount * sizeof(css::ComputedStyle);
    constexpr std::size_t source_records = kAuthorSources * (544 + sizeof(Source));
    constexpr std::size_t font_records = css::kEmbeddedFontCount * (272 + 4 * (544 + sizeof(EmbeddedFont)));
    static_assert(kAuthorSourceBytes + sizeof(AuthorLayoutState) + maximum_pool_bytes +
                  source_records + retained_style_bytes + font_records + 8 * 1024 <= 512 * 1024);
    if (!memory_room(sizeof(AuthorLayoutState) + (bytes ? maximum_pool_bytes : 0))) return {};
    auto author = std::unique_ptr<AuthorLayoutState>(new(std::nothrow) AuthorLayoutState(limits, stop));
    if (!author || author->sheet.report().allocated_bytes > maximum_pool_bytes) return {};
    for (const auto& source : sources) {
        if (!source.size) continue;
        const auto& base = source.path.empty() ? resource : source.path;
        const auto accept = [&](std::string_view family, std::string_view url) {
            std::string path, fragment;
            if (!resolve(base, std::string(url), path, fragment) || !fragment.empty() || path.size() <= 4) return false;
            auto extension = path.substr(path.size() - 4);
            for (auto& c : extension) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
            if (extension != ".ttf" && extension != ".otf") return false;
            const auto* member = find_member(path);
            if (!member || member->bytes < 12 || member->bytes > kMaxEmbeddedFontBytes) return false;
            mz_zip_archive_file_stat stat{};
            if (!mz_zip_reader_file_stat(&zip, member->member, &stat) || stat.m_is_directory) return false;
            auto name = std::find(author->font_names.begin(), author->font_names.end(), family);
            const auto slot = static_cast<std::size_t>(name - author->font_names.begin());
            EmbeddedFont font{std::string(path.data(), path.size()), member->bytes, stat.m_crc32};
            if (name != author->font_names.end()) author->fonts[slot] = std::move(font);
            else if (author->fonts.size() < css::kEmbeddedFontCount) {
                author->font_names[author->fonts.size()] = family;
                author->fonts.push_back(std::move(font));
            }
            return true;
        };
        if (author->sheet.collect_fonts({raw.get() + source.begin, source.size}, author->checkpoint, accept) != css::Result::Complete)
            return {};
    }
    for (std::size_t i = 0; i < author->fonts.size(); ++i) author->font_families[i] = author->font_names[i];
    for (const auto& source : sources) {
        if (source.size && author->sheet.append({raw.get() + source.begin, source.size}, author->checkpoint,
                                                &author->font_families) != css::Result::Complete)
            return {};
    }
#if defined(ESP_PLATFORM) && defined(CONFIG_PICO_PAGE_DIAGNOSTIC_CONSOLE)
    const auto report = author->sheet.report();
    ESP_LOGI("author_css", "sources=%u source_bytes=%u rules=%u pool_bytes=%u workspace_bytes=%u psram_free=%u psram_largest=%u stack_free=%u",
        static_cast<unsigned>(sources.size()), static_cast<unsigned>(bytes), static_cast<unsigned>(report.rules),
        static_cast<unsigned>(report.allocated_bytes), static_cast<unsigned>(sizeof(AuthorLayoutState)),
        static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
        static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)),
        static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
#endif
    return author;
}

bool EpubDocument::Impl::load(std::size_t index, Chapter& chapter, std::string& error, ChapterLoad detail,
                              std::uint32_t wanted, const std::function<bool(const Chapter&)>* match, const std::function<bool()>& stop,
                              const std::function<void(const Chapter&)>& visit) {
    const StageTimer timer{"chapter_parse", 0};
    pico_page::release(chapter); error.clear();
    if (stop && stop()) return fail(error, kChapterCancelled);
    const auto& spine = book().spine;
    if (!file || index >= spine.size()) return fail(error, "Invalid chapter index");
    if (txt) return load_txt(chapter, error, detail, wanted, match, stop, visit);
    auto author = detail == ChapterLoad::Original ? prepare_author_styles(index, stop) : nullptr;
    if (stop && stop()) return fail(error, kChapterCancelled);
    for (;;) {
        ChapterParser parser(chapter, spine[index].resource);
        parser.author = author.get();
        if (index < html_repair_offsets.size() && html_repair_offsets[index] != kNoHtmlRepair) {
            parser.xml.html_fallback = true;
            parser.xml.html_prefix_bytes = html_repair_offsets[index];
        }
        parser.requested_part = wanted;
        parser.match_part = match;
        if (detail == ChapterLoad::Reading) parser.visit = &visit;
        if (detail != ChapterLoad::Mapping) {
            parser.keep_mapping = false;
            parser.keep_anchors = detail != ChapterLoad::Layout;
            const auto scalars = std::min<std::size_t>(spine[index].bytes, kMaxText) + 1024;
            if (memory_room(scalars * sizeof(char32_t))) parser.chapter.text.reserve(scalars);
            else if (detail == ChapterLoad::Layout) return fail(error, std::string(kChapterMemoryError) + ": " + spine[index].resource);
        }
        const bool parsed = parse(spine[index].resource, parser.xml, error, kMaxXmlBytes, stop);
        if (!parsed && parser.xml.error != kPartComplete) {
            if (author && parser.xml.error == kAuthorUnavailable) {
                author.reset();
                pico_page::release(chapter);
                continue;
            }
            const bool retry = enable_html_repair(index, parser.xml);
            pico_page::release(chapter);
            if (retry) continue;
            return false;
        }
        error.clear();
        if (parsed) {
            parser.finish_block();
            if (author && parser.xml.error == kAuthorUnavailable) {
                author.reset();
                pico_page::release(chapter);
                continue;
            }
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
            chapter.css_sources = std::move(parser.chapter.css_sources);
            chapter.warnings = std::move(parser.chapter.warnings);
            chapter.plain_dom = parser.chapter.plain_dom;
            chapter.long_source_node = parser.chapter.long_source_node;
        }
        chapter.author_style_fallback = detail == ChapterLoad::Original && !author;
        if (author && std::none_of(chapter.runs.begin(), chapter.runs.end(), [&](const TextRun& run) {
                const auto& style = chapter.style(run.style);
                return !style.hidden && style.display != css::Display::None;
            })) {
            std::vector<css::ComputedStyle>().swap(chapter.author_styles);
            chapter.author_style_fallback = true;
        }
        if (author && !chapter.author_style_fallback) {
            chapter.author_fonts = std::move(author->fonts);
            if (chapter.blocks.size() > 1 && !read_image_sizes(chapter, error, stop)) { pico_page::release(chapter); return false; }
        }
#if defined(ESP_PLATFORM) && defined(CONFIG_PICO_PAGE_DIAGNOSTIC_CONSOLE)
        if (detail == ChapterLoad::Original)
            ESP_LOGI("author_css", "part=%u styles=%u fallback=%d psram_min=%u internal_min=%u stack_free=%u",
                static_cast<unsigned>(chapter.part), static_cast<unsigned>(chapter.author_styles.size()),
                chapter.author_style_fallback, static_cast<unsigned>(heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM)),
                static_cast<unsigned>(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
                static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
#endif
        if (detail != ChapterLoad::Layout && detail != ChapterLoad::Mapping && chapter.text.capacity() - chapter.text.size() > 4096 &&
            memory_room(chapter.text.size() * sizeof(char32_t))) {
            std::u32string exact(chapter.text.begin(), chapter.text.end());
            chapter.text.swap(exact);
        }
        return true;
    }
}
bool EpubDocument::Impl::load_txt(Chapter& chapter, std::string& error, ChapterLoad detail,
                                  std::uint32_t wanted, const std::function<bool(const Chapter&)>* match,
                                  const std::function<bool()>& stop, const std::function<void(const Chapter&)>& visit) {
    const auto& metadata = book().metadata;
    const bool gb = metadata.txt_encoding == TxtEncoding::Gb18030;
    const bool utf16 = metadata.txt_encoding == TxtEncoding::Utf16Le || metadata.txt_encoding == TxtEncoding::Utf16Be;
    const bool indexed = gb || utf16;
    auto last = indexed ? static_cast<std::uint32_t>(metadata.txt_offsets.size() - 2) :
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
    if (!indexed && last > 0) {
        std::uint32_t start = 0;
        if (!aligned(last * kTxtPartBytes, start)) return fail(error, "无法读取 TXT 文件");
        if (start >= txt_bytes) --last;
    }
    if (!match && wanted != kLastChapterPart && wanted > last)
        return fail(error, "TXT 内容段不可用");
    for (std::uint32_t part = match ? 0 : wanted == kLastChapterPart ? last : wanted; part <= last; ++part) {
        if (stop && stop()) { pico_page::release(chapter); return fail(error, kChapterCancelled); }
        std::uint32_t begin = 0, end = 0;
        if (indexed) { begin = metadata.txt_offsets[part]; end = metadata.txt_offsets[part + 1]; }
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
        if (!indexed && part == 0 && bytes.size() >= 3 && bytes.compare(0, 3, "\xef\xbb\xbf") == 0) bytes.erase(0, 3);
        if (utf16 && part == 0 && bytes.size() >= 2) bytes.erase(0, 2);
        Chapter current;
        current.resource = "text";
        current.part = part;
        current.last_part = part == last;
        current.split_node = part != 0 || !current.last_part;
        current.plain_dom = false;
        current.text.reserve(bytes.size());
        if (!(utf16 ? decode_utf16(bytes, metadata.txt_encoding == TxtEncoding::Utf16Be, current.text) :
              gb ? gb18030::decode(bytes, current.text) : decode(bytes.data(), bytes.size(), current.text))) {
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
    for (std::size_t i = 0; i < impl_->book().spine.size(); ++i) {
        PP_HOST_WORK(host_work::add(host_work::SpineComparisons);)
        if (impl_->book().spine[i].resource == resource) { index = i; return true; }
    }
    return false;
}
bool EpubDocument::read_font(const EmbeddedFont& font,
                            const std::function<bool(std::size_t, std::string_view)>& write,
                            const std::function<bool()>& stop) {
    if (!impl_->file || impl_->txt || font.bytes < 12 || font.bytes > kMaxEmbeddedFontBytes ||
        !memory_room(128U * 1024) || (stop && stop())) return false;
    const auto* entry = impl_->find_member(font.resource);
    mz_zip_archive_file_stat stat{};
    if (!entry || entry->bytes != font.bytes || !mz_zip_reader_file_stat(&impl_->zip, entry->member, &stat) ||
        stat.m_is_directory || stat.m_crc32 != font.crc32) return false;
    struct Output {
        const EmbeddedFont& font;
        const std::function<bool(std::size_t, std::string_view)>& write;
        const std::function<bool()>& stop;
    } output{font, write, stop};
    return mz_zip_reader_extract_to_callback(&impl_->zip, entry->member,
        [](void* opaque, mz_uint64 offset, const void* data, std::size_t count) -> std::size_t {
            const auto& out = *static_cast<Output*>(opaque);
            if (offset > out.font.bytes || count > out.font.bytes - offset || (out.stop && out.stop())) return 0;
            return out.write(static_cast<std::size_t>(offset), {static_cast<const char*>(data), count}) ? count : 0;
        }, &output, 0) && (!stop || !stop());
}
bool EpubDocument::read_image(const std::string& resource, std::vector<std::uint8_t>& bytes, std::string& error,
                              const std::function<bool()>& stop, ContentFailureKind* failure_kind) {
    const auto fail = content_failure_reporter(failure_kind);
    const StageTimer timer{"image_read_inflate", 0};
    bytes.clear(); error.clear();
    if (stop && stop()) return fail(error, "Image load cancelled", ContentFailureKind::Cancelled);
    std::string canonical;
    if (!impl_->file || resource.empty() || !path_parts(resource, canonical) || canonical != resource) return fail(error, "Invalid internal image path");
    const auto* entry = impl_->find_member(resource);
    if (!entry) return fail(error, "Missing EPUB image", ContentFailureKind::MissingResource);
    if (!entry->bytes || entry->bytes > 2 * 1024 * 1024) return fail(error, "EPUB image exceeds 2 MiB limit");
    if (!platform_memory_room(entry->bytes)) return fail(error, "Image allocation unavailable", ContentFailureKind::OutOfMemory);
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
        if (output.cancelled || (stop && stop())) return fail(error, "Image load cancelled", ContentFailureKind::Cancelled);
        const auto zip_error = mz_zip_get_last_error(&impl_->zip);
        if (zip_error == MZ_ZIP_FILE_READ_FAILED || zip_error == MZ_ZIP_FILE_SEEK_FAILED)
            return fail(error, "Cannot read EPUB image", ContentFailureKind::IoError);
        if (zip_error == MZ_ZIP_ALLOC_FAILED) return fail(error, "Image allocation unavailable", ContentFailureKind::OutOfMemory);
        return fail(error, "Invalid compressed EPUB image");
    }
    const bool png = bytes.size() >= 8 && std::memcmp(bytes.data(), "\x89PNG\r\n\x1a\n", 8) == 0;
    const bool jpeg = bytes.size() >= 3 && bytes[0] == 0xff && bytes[1] == 0xd8 && bytes[2] == 0xff;
    const bool gif = bytes.size() >= 6 && (!std::memcmp(bytes.data(), "GIF87a", 6) || !std::memcmp(bytes.data(), "GIF89a", 6));
    if (!png && !jpeg && !gif) { bytes.clear(); return fail(error, "Image format is not PNG, JPEG or GIF"); }
    return true;
}
bool Chapter::anchor_at(std::uint32_t scalar_offset, Anchor& anchor) const {
    auto run = std::upper_bound(runs.begin(), runs.end(), scalar_offset, [](std::uint32_t value, const TextRun& r) { return value < r.begin; });
    if (run == runs.begin()) return false;
    --run;
    if (scalar_offset >= run->end) return false;
    anchor = run->source; if (!run->element_reference) anchor.offset += scalar_offset - run->begin; return true;
}
bool Chapter::offset_for(const Anchor& anchor, std::uint32_t& scalar_offset) const {
    if (anchor.resource != resource) return false;
    for (const auto& run : runs) {
        if (run.source.node != anchor.node) continue;
        if (run.element_reference) { if (anchor.offset == 0) { scalar_offset = run.begin; return true; } }
        else if (anchor.offset >= run.source.offset && anchor.offset < run.source.offset + run.end - run.begin) { scalar_offset = run.begin + anchor.offset - run.source.offset; return true; }
    }
    return false;
}
bool Chapter::fragment_offset(const std::string& id, std::uint32_t& scalar_offset) const {
    if (id.empty()) { scalar_offset = 0; return true; }
    for (const auto& target : fragments) if (target.id == id) { scalar_offset = target.offset; return true; }
    return false;
}
}
