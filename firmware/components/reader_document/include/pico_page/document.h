#pragma once
#include "pico_page/content_failure.h"
#include "pico_page/document_style.h"

#include "pico_page/record.h"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace pico_page {

enum class TxtEncoding { Utf8, Gb18030, Utf16Le, Utf16Be };

/** Recognized TXT heading and its unchanged part-local Unicode scalar position. */
struct TxtHeading {
    std::string label;
    std::uint32_t part = 0;
    std::uint32_t offset = 0;
    std::uint32_t byte_offset = 0;
};

struct DocumentMetadata {
    std::string title;
    std::string author;
    std::string language;
    std::string book_id;  // SHA-256 of the unchanged original file.
    std::string partial_md5;
    TxtEncoding txt_encoding = TxtEncoding::Utf8;
    std::vector<TxtHeading> txt_headings;
    std::vector<std::uint32_t> txt_offsets;  // Non-UTF-8 part starts and EOF, measured in original file bytes.
};
struct SpineItem {
    std::string resource;
    std::string id;
    bool linear = true;
    std::uint32_t bytes = 0;  // Uncompressed size of the XHTML resource; a weight for position estimates.
};
struct TocEntry { std::string label; std::string resource; std::string fragment; unsigned depth = 0; };
enum class BlockKind { Paragraph, Heading, ListItem, Preformatted, Image };
struct TextBlock { std::uint32_t begin = 0; std::uint32_t end = 0; BlockKind kind = BlockKind::Paragraph; std::uint16_t style = 0; };
struct TextRun {
    std::uint32_t begin = 0;
    std::uint32_t end = 0;
    Anchor source;
    bool element_reference = false;  // Media placeholders refer to the source element at offset zero.
    std::uint16_t style = 0;
};
struct SourceTextNode {
    std::string node;
    std::u32string text;
    bool first_child = false;  // No preceding XML text or element child in the source parent.
};
struct FragmentTarget { std::string id; std::uint32_t offset = 0; };
struct DocumentLink { std::uint32_t begin = 0; std::uint32_t end = 0; std::string resource; std::string fragment; bool external = false; };
struct DocumentImage {
    std::uint32_t begin = 0, end = 0;
    std::string resource, alt;
    Anchor source;
    /** Header pixel size, read only by Original loads; zero when unknown. */
    std::uint16_t width = 0, height = 0;
};

/**
 * EPUB part scalar target at a block boundary. In-block splitting uses twice
 * this target. Run, source-node or metadata limits may split a part earlier.
 */
inline constexpr std::uint32_t kChapterPartScalars = 60000;
/** TXT parts use fixed byte targets adjusted to complete code points or CRLF. */
inline constexpr std::uint32_t kTxtPartBytes = 48 * 1024;
inline constexpr std::uint32_t kTxtReflowMinLineScalars = 56;
/** Selects the last part of a spine item in load_chapter. */
inline constexpr std::uint32_t kLastChapterPart = 0xffffffffU;

/**
 * How much of a chapter load_chapter keeps. Mapping, Reading and Layout use the same
 * normalization and part boundaries. Original can use different block boundaries while
 * retaining the same source element paths and scalar offsets.
 */
enum class ChapterLoad {
    /** Everything, including the source text of each node and the stylesheets; KOReader position mapping needs these. */
    Mapping,
    /** Keeps run anchors for local position recovery, without source text nodes or stylesheet contents. */
    Reading,
    /** Pagination only: run anchors are empty as well, so anchor_at only reports whether an offset has a source. */
    Layout,
    /** Source anchors and the supported author styles for native EPUB layout. */
    Original
};

struct EmbeddedFont {
    std::string resource;
    std::uint32_t bytes = 0, crc32 = 0;
};
inline constexpr std::size_t kMaxEmbeddedFontBytes = 2U * 1024 * 1024;

/**
 * One part of a chapter as normalized Unicode text. Offsets are Unicode scalar indices into
 * this part; source anchors are independent of the division into parts.
 */
struct Chapter {
    std::string resource;
    std::uint32_t part = 0;
    bool last_part = true;
    bool starts_in_paragraph = false;
    bool split_node = false;     // An in-node boundary prevents KOReader XPointer mapping.
    bool long_source_node = false;  // Mapping-only: source text may be split into multiple CREngine nodes.
    std::u32string text;
    // Optional TXT joins identify the first scalar of each source line ending.
    std::vector<std::uint32_t> txt_soft_breaks;
    std::vector<TextBlock> blocks;
    std::vector<TextRun> runs;
    std::vector<SourceTextNode> source_nodes;
    bool plain_dom = true;  // False when constructs require a separately verified external DOM mapping profile.
    std::vector<std::string> css_sources;  // Inline and local linked CSS, bounded to 256 KiB per chapter.
    std::vector<FragmentTarget> fragments;
    std::vector<DocumentLink> links;
    std::vector<DocumentImage> images;
    std::vector<std::string> warnings;
    std::vector<document::css::ComputedStyle> author_styles;
    std::vector<EmbeddedFont> author_fonts;
    bool author_style_fallback = false;
    /** Style indices are one-based; zero denotes the initial style. */
    const document::css::ComputedStyle& style(std::uint16_t index) const;
    /** Maps a Unicode scalar index in text to a source anchor; inserted separators return false. */
    bool anchor_at(std::uint32_t scalar_offset, Anchor& anchor) const;
    /** Finds the Unicode scalar index in text for a retained text scalar or media start; removed whitespace has no mapping. */
    bool offset_for(const Anchor& anchor, std::uint32_t& scalar_offset) const;
    /** Finds a fragment's Unicode scalar index in text; an empty ID resolves to zero, an unknown ID returns false. */
    bool fragment_offset(const std::string& id, std::uint32_t& scalar_offset) const;
};

std::uint64_t identity_hash_count();
std::uint64_t epub_open_count();

/** SHA-256 of a file, read in 8 KiB chunks. Empty on read failure or cancellation. */
std::string sha256_file(const std::string& path, const std::function<bool()>& stop = {});

/** Incremental SHA-256 reader. A single owner calls step(); cancellation retains the cursor.
 * The caller must discard the reader before the underlying storage may change or unmount.
 */
class FileDigest {
public:
    explicit FileDigest(const std::string& path);
    ~FileDigest();
    /**
     * Reads at most budget_bytes; true means finished, including an error (empty digest).
     * False means unfinished, including zero budget or cancellation; a later step can resume.
     * stop is checked between reads and does not interrupt an in-flight file operation.
     */
    bool step(std::size_t budget_bytes, const std::function<bool()>& stop = {});
    /** Lowercase SHA-256 after successful completion; empty before completion or after failure. */
    const std::string& digest() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/** Lowercase hexadecimal MD5 of bytes; empty when the digest backend fails. */
std::string md5_hex(std::string_view bytes);
/** Named XHTML character entity without delimiters; zero when unsupported. */
char32_t named_html_entity(std::string_view name);

/** Single-owner incremental MD5. Updates retain no input references; finish consumes the digest.
 * update returns false and finish returns empty on backend failure or use after finish.
 */
class Md5Digest {
public:
    Md5Digest();
    ~Md5Digest();
    bool update(std::string_view bytes);
    std::string finish();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/** Single-owner incremental SHA-256. Updates retain no input references; finish consumes the digest.
 * update returns false and finish returns empty on backend failure or use after finish.
 */
class Sha256Digest {
public:
    Sha256Digest();
    ~Sha256Digest();
    bool update(std::string_view bytes);
    std::string finish();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/** Frees all memory held by a chapter; assigning an empty chapter may keep string capacity. */
void release(Chapter& chapter);

/** Exact load_chapter error when its stop callback requests cancellation. */
inline constexpr char kChapterCancelled[] = "Chapter load cancelled";

/**
 * Start of the load_chapter error when parsing stopped because free memory would have fallen
 * below the amount kept in reserve. The caller may release other memory and load again.
 */
inline constexpr char kChapterMemoryError[] = "Chapter exceeds available memory";

/**
 * Replaces the check that load_chapter makes before it grows a chapter: probe(bytes) reports
 * whether an allocation of that size leaves the reserve intact, and probe(0) whether the reserve
 * is intact now. nullptr restores the platform check, which always passes off the device.
 */
void set_chapter_memory_probe(bool (*probe)(std::size_t bytes));
bool chapter_memory_available(std::size_t bytes);

/** Read diagnostics count bytes returned by file reads and supplied to Expat, including read-ahead. */
struct DocumentReadStats {
    std::uint64_t source_read_bytes = 0, xml_feed_bytes = 0;
    std::size_t zip_allocated_bytes = 0;
    bool stream_active = false;
};

/** Bounded, read-only EPUB and UTF-8/GB18030/BOM-marked UTF-16 TXT access. XML source paths are not KOReader XPointer values. */
class EpubDocument {
public:
    struct BookIndex;
    using VerifiedIndex = std::shared_ptr<const BookIndex>;
    EpubDocument();
    ~EpubDocument();
    EpubDocument(const EpubDocument&) = delete;
    EpubDocument& operator=(const EpubDocument&) = delete;
    /** Transfers exclusive ownership; a moved-from document may be destroyed or reopened. */
    EpubDocument(EpubDocument&&) noexcept;
    EpubDocument& operator=(EpubDocument&&) noexcept;
    /** Reuses a previous reading index for this path only after hashing the opened file and matching its full digest. */
    bool open(const std::string& path, std::string& error, const std::function<bool()>& stop = {},
              VerifiedIndex previous_index = {});
    /** Reuses a previously verified digest only under exclusive, unchanged storage ownership.
     * The caller must invalidate that trust after import, removal, sleep or external access.
     * Pass an index from the same verified EPUB to reuse its parsed structure across readers.
     * ZIP resources are always checked. File attributes alone do not establish trust.
     */
    bool open_verified(const std::string& path, const DocumentMetadata& identity, std::string& error,
                       const std::function<bool()>& stop = {}, VerifiedIndex index = {});
    /** The parsed catalog of a verified EPUB, or an empty handle for TXT or an unopened reader. */
    VerifiedIndex verified_epub_index() const;
    /** The immutable TXT index, or an empty handle for EPUB or an unopened reader. */
    VerifiedIndex txt_index() const;
    /** The immutable reading index, or an empty handle for an unopened reader. */
    VerifiedIndex book_index() const;
    /** Returns the direct TXT part number encoded in a canonical source anchor. */
    std::optional<std::uint32_t> txt_part_for_anchor(const Anchor& anchor) const;
    void close();
    /** Transfers file, ZIP and parser ownership to a new reader, retaining only an immutable
     * metadata, spine and TOC snapshot here. The original reader must reopen to load content.
     * Returns null without changes for an unopened document, TXT or insufficient memory.
     * Call only after all operations on this document have completed.
     */
    std::unique_ptr<EpubDocument> detach_reader();
    const DocumentMetadata& metadata() const;
    const std::vector<SpineItem>& spine() const;
    const std::vector<TocEntry>& toc() const;
    /**
     * Parses the spine resource as a stream and keeps one part of it; output is released on
     * failure. Earlier parts are parsed and discarded, so a later part takes longer to reach.
     * For Reading loads, visit receives each completed normalized part before it is discarded
     * or returned. Its reference is valid only during the callback; it must not reenter this document.
     * Reading and Layout attempt a bounded text reservation. Reading can fall back to incremental
     * growth and shrinks excess capacity only when the temporary copy fits the memory budget.
     * Layout fails with kChapterMemoryError when the initial reservation does not fit.
     * stop is called synchronously before loading, between XML chunks of at most 4096 bytes,
     * and around linked stylesheet reads. It must not reenter this document. A true result
     * releases output and returns false with kChapterCancelled; retry starts parsing again.
     * sequential retains a Reading or Original cursor for forward part requests. Backward requests restart;
     * unsupported streams fall back to the ordinary loader after releasing the cursor.
     * Blocking storage and decompressor calls are not interrupted by this callback.
     */
    bool load_chapter(std::size_t index, Chapter& chapter, std::string& error,
                      ChapterLoad detail = ChapterLoad::Mapping, std::uint32_t part = 0,
                      const std::function<bool()>& stop = {},
                      const std::function<void(const Chapter&)>& visit = {}, bool sequential = false);
    /**
     * Like load_chapter, for the first part that match accepts. match sees each complete part
     * at the given detail, before the stylesheet fields of a Mapping load are final.
     * sequential retains a Reading or Original cursor after the selected part; searches always start at zero.
     */
    bool find_chapter_part(std::size_t index, Chapter& chapter, std::string& error, ChapterLoad detail,
                           const std::function<bool(const Chapter&)>& match, const std::function<bool()>& stop = {},
                           const std::function<void(const Chapter&)>& visit = {}, bool sequential = false);
    /** Sequential Reading API used by the content worker and parser probes.
     * Starts at part zero; repeated calls for the same index continue one part at a time.
     * Last-part completion, errors, cancellation, random access and close discard the cursor.
     * The document and stream have one executor. Reset/close only after an active call returns.
     * Original XML input has a 64 KiB budget between complete events. Failure clears
     * output and discards the stream; load_chapter remains the fallback.
     * stop must not reenter this document. It runs between XML chunks/resumes; a storage or DEFLATE call is not interruptible.
     */
    bool read_next_chapter_part(std::size_t index, Chapter& chapter, std::string& error,
                                const std::function<bool()>& stop = {});
    /** Checks whether the existing cursor can continue at the given part. Call between loads. */
    bool has_chapter_stream(std::size_t index, ChapterLoad detail, std::uint32_t next_part) const;
    /** Discards the sequential cursor without closing the verified document. No parsing or I/O. */
    void reset_chapter_stream();
    DocumentReadStats read_stats() const;
    /** Estimate of owned memory, excluding Expat pools and allocator overhead. Call between loads. */
    std::size_t retained_bytes() const;
    bool chapter_index_for(const std::string& resource, std::size_t& index) const;
    /** Read an internal PNG/JPEG or GIF resource (GIF rendering uses its first frame), at most 2 MiB; output is cleared on failure. */
    bool read_image(const std::string& resource, std::vector<std::uint8_t>& bytes, std::string& error,
                    const std::function<bool()>& stop = {}, ContentFailureKind* failure_kind = nullptr);
    /** Streams the matching ZIP member through a bounded sink. Output is provisional until CRC verification succeeds. */
    bool read_font(const EmbeddedFont& font, const std::function<bool(std::size_t, std::string_view)>& write,
                   const std::function<bool()>& stop = {});
    /** Read a declared cover or single-image cover page without hashing the book or changing a reading session.
     * PNG/JPEG/GIF output is bounded to 2 MiB and cleared on failure.
     * Optional failure_kind reports the source failure independently of error text; success reports None.
     */
    static bool read_cover(const std::string& path, std::vector<std::uint8_t>& bytes, std::string& error,
                           const std::function<bool()>& stop = {}, ContentFailureKind* failure_kind = nullptr);
private:
    enum class OpenMode { Book, Cover, Resources };
    bool open_impl(const std::string& path, std::string& error, OpenMode mode,
                   const DocumentMetadata* identity = nullptr, const std::function<bool()>& stop = {},
                   ContentFailureKind* failure_kind = nullptr, VerifiedIndex previous_index = {});
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}
