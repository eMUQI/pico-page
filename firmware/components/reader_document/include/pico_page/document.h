#pragma once

#include "pico_page/record.h"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace pico_page {

struct DocumentMetadata {
    std::string title;
    std::string author;
    std::string language;
    std::string book_id;  // SHA-256 of the unchanged original EPUB.
    std::string partial_md5;
};
struct SpineItem { std::string resource; std::string id; bool linear = true; };
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

/** One chapter of normalized Unicode text. All offsets are Unicode scalar indices. */
struct Chapter {
    std::string resource;
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

/** Bounded, read-only EPUB access. XML text source paths are not KOReader XPointer values. */
class EpubDocument {
public:
    EpubDocument();
    ~EpubDocument();
    EpubDocument(const EpubDocument&) = delete;
    EpubDocument& operator=(const EpubDocument&) = delete;
    bool open(const std::string& path, std::string& error);
    void close();
    const DocumentMetadata& metadata() const;
    const std::vector<SpineItem>& spine() const;
    const std::vector<TocEntry>& toc() const;
    /** Parse only the selected spine resource; output is cleared on failure. */
    bool load_chapter(std::size_t index, Chapter& chapter, std::string& error);
    bool chapter_index_for(const std::string& resource, std::size_t& index) const;
    /** Read an internal PNG/JPEG resource, at most 2 MiB; output is cleared on failure. */
    bool read_image(const std::string& resource, std::vector<std::uint8_t>& bytes, std::string& error);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace pico_page
