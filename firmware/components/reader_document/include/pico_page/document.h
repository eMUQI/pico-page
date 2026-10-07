#pragma once

#include "pico_page/record.h"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace pico_page {

struct DocumentMetadata {
    std::string title;
    std::string author;
    std::string language;
    std::string book_id;  // SHA-256 of the unchanged original EPUB.
    std::string partial_md5;
};
struct SpineItem {
    std::string resource;
    std::string id;
    bool linear = true;
    std::uint32_t bytes = 0;  // Uncompressed size of the XHTML resource; a weight for position estimates.
};
struct TocEntry { std::string label; std::string resource; std::string fragment; unsigned depth = 0; };
enum class BlockKind { Paragraph, Heading, ListItem, Preformatted, Image };
struct TextBlock { std::uint32_t begin = 0; std::uint32_t end = 0; BlockKind kind = BlockKind::Paragraph; };
struct TextRun {
    std::uint32_t begin = 0;
    std::uint32_t end = 0;
    Anchor source;
    bool element_reference = false;  // Media placeholders refer to the source element at offset zero.
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
};

/**
 * A spine item is read in parts so that memory does not grow with its length. A part ends at
 * the first block boundary after this many scalars, or inside a block at twice as many; an item
 * shorter than this is a single part.
 */
inline constexpr std::uint32_t kChapterPartScalars = 60000;
/** Selects the last part of a spine item in load_chapter. */
inline constexpr std::uint32_t kLastChapterPart = 0xffffffffU;

/**
 * How much of a chapter load_chapter keeps. Every level applies the same limits and yields the
 * same text, blocks, images, links, fragments and run boundaries.
 */
enum class ChapterLoad {
    /** Everything, including the source text of each node and the stylesheets; KOReader position mapping needs these. */
    Mapping,
    /** What reading needs: run anchors for saving and restoring positions. source_nodes entries and css_sources are empty and no linked stylesheet is read. */
    Reading,
    /** Pagination only: run anchors are empty as well, so anchor_at only reports whether an offset has a source. */
    Layout
};

/**
 * One part of a chapter as normalized Unicode text. Offsets are Unicode scalar indices into
 * this part; source anchors are independent of the division into parts.
 */
struct Chapter {
    std::string resource;
    std::uint32_t part = 0;      // Position of this part within the spine item.
    bool last_part = true;       // False when a later part holds more text.
    bool split_node = false;     // The part begins or ends inside a text node; position mapping is refused.
    std::u32string text;
    std::vector<TextBlock> blocks;
    std::vector<TextRun> runs;
    std::vector<SourceTextNode> source_nodes;
    bool plain_dom = true;  // False when constructs require a separately verified external DOM mapping profile.
    std::vector<std::string> css_sources;  // Inline and local linked CSS, bounded to 256 KiB per chapter.
    std::vector<FragmentTarget> fragments;
    std::vector<DocumentLink> links;
    std::vector<DocumentImage> images;
    std::vector<std::string> warnings;
    /** Return a text scalar or media element source; inserted separators return false. */
    bool anchor_at(std::uint32_t offset, Anchor& anchor) const;
    /** Find a retained text scalar or media start; removed whitespace has no mapping. */
    bool offset_for(const Anchor& anchor, std::uint32_t& offset) const;
    bool fragment_offset(const std::string& id, std::uint32_t& offset) const;
};

/** Number of full-file EPUB identity scan attempts in this process. */
std::uint64_t identity_hash_count();
/** Number of EPUB container opens attempted in this process. */
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
    /** Reads at most budget bytes; true means finished, including an error (empty digest). */
    bool step(std::size_t budget, const std::function<bool()>& stop = {});
    const std::string& digest() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/** Lowercase hexadecimal MD5 of bytes; empty when the digest backend fails. */
std::string md5_hex(std::string_view bytes);

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
/** Applies the same allocation reserve to cached and parsed chapter loads. */
bool chapter_memory_available(std::size_t bytes);

/** Bounded, read-only EPUB access. XML text source paths are not KOReader XPointer values. */
class EpubDocument {
public:
    EpubDocument();
    ~EpubDocument();
    EpubDocument(const EpubDocument&) = delete;
    EpubDocument& operator=(const EpubDocument&) = delete;
    /** Transfers exclusive ownership; a moved-from document may be destroyed or reopened. */
    EpubDocument(EpubDocument&&) noexcept;
    EpubDocument& operator=(EpubDocument&&) noexcept;
    bool open(const std::string& path, std::string& error, const std::function<bool()>& stop = {});
    /** Reuses a previously verified digest only under exclusive, unchanged storage ownership.
     * The caller must invalidate that trust after import, removal, sleep or external access.
     * ZIP structure and metadata are still read. File attributes alone do not establish trust.
     */
    bool open_verified(const std::string& path, const DocumentMetadata& identity, std::string& error, const std::function<bool()>& stop = {});
    void close();
    const DocumentMetadata& metadata() const;
    const std::vector<SpineItem>& spine() const;
    const std::vector<TocEntry>& toc() const;
    /**
     * Parses the spine resource as a stream and keeps one part of it; output is released on
     * failure. Earlier parts are parsed and discarded, so a later part takes longer to reach.
     * For Reading loads, visit receives each completed normalized part before it is discarded
     * or returned. Its reference is valid only during the callback; it must not reenter this document.
     * Below Mapping the text is reserved once, at most twice kChapterPartScalars, instead of
     * growing geometrically; a Reading part is then trimmed to its length. A Layout load fails
     * with kChapterMemoryError when that reservation does not fit.
     * stop is called synchronously before loading, between XML chunks of at most 4096 bytes,
     * and around linked stylesheet reads. It must not reenter this document. A true result
     * releases output and returns false with kChapterCancelled; retry starts parsing again.
     * Blocking storage and decompressor calls are not interrupted by this callback.
     */
    bool load_chapter(std::size_t index, Chapter& chapter, std::string& error,
                      ChapterLoad detail = ChapterLoad::Mapping, std::uint32_t part = 0,
                      const std::function<bool()>& stop = {},
                      const std::function<void(const Chapter&)>& visit = {});
    /**
     * Like load_chapter, for the first part that match accepts. match sees each complete part
     * at the given detail, before the stylesheet fields of a Mapping load are final.
     */
    bool find_chapter_part(std::size_t index, Chapter& chapter, std::string& error, ChapterLoad detail,
                           const std::function<bool(const Chapter&)>& match, const std::function<bool()>& stop = {},
                           const std::function<void(const Chapter&)>& visit = {});
    bool chapter_index_for(const std::string& resource, std::size_t& index) const;
    /** Read an internal PNG/JPEG resource, at most 2 MiB; output is cleared on failure. */
    bool read_image(const std::string& resource, std::vector<std::uint8_t>& bytes, std::string& error,
                    const std::function<bool()>& stop = {});
    /** Read a declared cover or single-image cover page without hashing the book or changing a reading session.
     * PNG/JPEG output is bounded to 2 MiB and cleared on failure.
     */
    static bool read_cover(const std::string& path, std::vector<std::uint8_t>& bytes, std::string& error,
                           const std::function<bool()>& stop = {});
private:
    bool open_impl(const std::string& path, std::string& error, bool cover_only,
                   const DocumentMetadata* identity = nullptr, const std::function<bool()>& stop = {});
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace pico_page
