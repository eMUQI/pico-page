#pragma once

#include "page_cache.h"
#include <memory>

namespace pico_page::chapter_jump {

enum class PositionKind { Start, End, SourceByte };

struct Position {
    PositionKind kind;
    std::size_t index;
    std::uint32_t part;
    /** Chapter-local scalar for Start, file or spine-resource byte offset for SourceByte. */
    std::uint32_t offset;
};

std::optional<std::size_t> toc_index(std::size_t count, std::uint64_t ordinal);
std::optional<Position> epub_position(const std::vector<SpineItem>& spine, const PageIndices* pages,
                                      std::uint64_t percent);
/** Offsets are the validated TXT part starts and EOF. An empty index uses the fixed UTF-8 part size. */
std::optional<Position> txt_position(std::uint32_t bytes, const std::vector<std::uint32_t>& offsets,
                                     std::uint64_t percent);
std::optional<std::size_t> containing_txt_heading(const std::vector<TxtHeading>& headings, std::uint32_t bytes,
                                                  const Position& position);

/** Selects the first entry at a spine item, or the last entry at the nearest preceding listed item. */
class EpubTocIndex {
public:
    EpubTocIndex() = default;
    EpubTocIndex(const std::vector<TocEntry>& toc, const std::vector<SpineItem>& spine);
    std::optional<std::size_t> containing_entry(std::size_t spine_index) const;

private:
    std::unique_ptr<std::uint16_t[]> entry_for_spine_;
    std::size_t spine_size_ = 0;
};

}
