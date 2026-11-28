#include "chapter_jump.h"

#include <cassert>
#include <limits>
#include <numeric>
#include <string>

using namespace pico_page;
using namespace pico_page::chapter_jump;

static void check_position(const std::optional<Position>& position, PositionKind kind,
                           std::size_t index, std::uint32_t part, std::uint32_t offset) {
    assert(position && position->kind == kind && position->index == index &&
           position->part == part && position->offset == offset);
}

int main(int argc, char** argv) {
    assert(argc == 2);
    assert(!toc_index(0, 1));
    assert(!toc_index(1710, 0));
    assert(!toc_index(1710, 1711));
    assert(toc_index(1710, 500) == 499);
    assert(toc_index(1, 1) == 0);
    constexpr auto max_count = std::numeric_limits<std::size_t>::max();
    assert(toc_index(max_count, max_count) == max_count - 1);
    constexpr auto max_input = std::numeric_limits<std::uint64_t>::max();
    assert(!toc_index(1710, max_input));

    const std::vector<SpineItem> book{{"cover.xhtml", "cover", true, 10}, {"one.xhtml", "one", true, 10},
                                      {"two.xhtml", "two", true, 10}, {"three.xhtml", "three", true, 10},
                                      {"four.xhtml", "four", true, 10}};
    const std::vector<TocEntry> toc{{"Three", "three.xhtml", "", 0}, {"One", "one.xhtml", "", 0},
                                    {"Missing", "missing.xhtml", "", 0}, {"One B", "one.xhtml", "b", 1},
                                    {"Three B", "three.xhtml", "b", 1}};
    const EpubTocIndex index(toc, book);
    assert(!index.containing_entry(0));
    assert(index.containing_entry(1) == 1);
    assert(index.containing_entry(2) == 3);
    assert(index.containing_entry(3) == 0);
    assert(index.containing_entry(4) == 4);
    assert(!index.containing_entry(5));
    assert(!EpubTocIndex(toc, {}).containing_entry(0));
    assert(!EpubTocIndex({}, book).containing_entry(0));
    const std::vector<TxtHeading> headings{{"A", 0, 10, 10}, {"B", 0, 50, 50}};
    const auto in_text = [&](PositionKind kind, std::uint32_t byte) {
        return containing_txt_heading(headings, 100, Position{kind, 0, 0, byte});
    };
    assert(!in_text(PositionKind::Start, 0));
    assert(!in_text(PositionKind::SourceByte, 9));
    assert(in_text(PositionKind::SourceByte, 10) == 0);
    assert(in_text(PositionKind::SourceByte, 50) == 1);
    assert(in_text(PositionKind::End, 0) == 1);

    constexpr auto max_bytes = std::numeric_limits<std::uint32_t>::max();
    const std::vector<SpineItem> spine{{"notes-1", "", false, max_bytes}, {"first", "", true, max_bytes},
                                     {"last", "", true, max_bytes}, {"notes-2", "", false, max_bytes}};
    assert(!epub_position({}, nullptr, 0));
    assert(!epub_position({spine.front()}, nullptr, 50));
    assert(!epub_position(spine, nullptr, max_input));
    check_position(epub_position(spine, nullptr, 0), PositionKind::Start, 1, 0, 0);
    check_position(epub_position(spine, nullptr, 25), PositionKind::SourceByte, 1, 0, max_bytes / 2);
    check_position(epub_position(spine, nullptr, 50), PositionKind::Start, 2, 0, 0);
    check_position(epub_position(spine, nullptr, 100), PositionKind::End, 2, kLastChapterPart, 0);
    check_position(epub_position({spine[1]}, nullptr, 50), PositionKind::SourceByte, 0, 0, max_bytes / 2);
    const PageIndices pages{{{0, 100}}, {{0, 42}, {0, 55}}, {{0}}, {{0, 100}}};
    check_position(epub_position(spine, &pages, 60), PositionKind::Start, 1, 1, 0);
    check_position(epub_position(spine, &pages, 61), PositionKind::Start, 1, 1, 55);
    check_position(epub_position(spine, &pages, 80), PositionKind::Start, 1, 1, 55);
    check_position(epub_position(spine, &pages, 81), PositionKind::Start, 2, 0, 0);
    PageIndices counted{{std::vector<std::uint32_t>(100)}};
    std::iota(counted[0][0].begin(), counted[0][0].end(), 0U);
    check_position(epub_position({spine[1]}, &counted, 49), PositionKind::Start, 0, 0, 48);
    check_position(epub_position({spine[1]}, &counted, 50), PositionKind::Start, 0, 0, 49);

    assert(!txt_position(0, {}, 0));
    assert(!txt_position(max_bytes, {}, max_input));
    check_position(txt_position(max_bytes, {}, 0), PositionKind::Start, 0, 0, 0);
    check_position(txt_position(max_bytes, {}, 100), PositionKind::End, 0, kLastChapterPart, 0);
    constexpr auto midpoint = max_bytes / 2;
    check_position(txt_position(max_bytes, {}, 50), PositionKind::SourceByte, 0, midpoint / kTxtPartBytes, midpoint);
    check_position(txt_position(max_bytes, {0, midpoint, max_bytes}, 50), PositionKind::SourceByte, 0, 1, midpoint);
    check_position(txt_position(max_bytes, {0, midpoint + 1, max_bytes}, 50), PositionKind::SourceByte, 0, 0, midpoint);

    const std::string fixtures = std::string(argv[1]) + "/";
    EpubDocument document;
    Chapter chapter;
    std::string error;
    std::uint32_t offset = 0;
    assert(document.open(fixtures + "sample.txt", error));
    assert(document.load_chapter_at_byte(0, 1, kTxtPartBytes + 15, chapter, offset, error, ChapterLoad::Reading));
    assert(chapter.text[offset] == U'😀');
    assert(document.open(fixtures + "gb18030.txt", error));
    assert(document.load_chapter_at_byte(0, 1, kTxtPartBytes + 3, chapter, offset, error, ChapterLoad::Reading));
    assert(chapter.text[offset] == U'\ue587');
    for (const char* name : {"utf-16-le.txt", "utf-16-be.txt"}) {
        assert(document.open(fixtures + name, error));
        assert(document.load_chapter_at_byte(0, 0, kTxtPartBytes - 2, chapter, offset, error, ChapterLoad::Reading));
        assert(chapter.text[offset] == U'😀');
    }

    assert(document.open(fixtures + "multi-part.epub", error));
    const auto position = epub_position(document.spine(), nullptr, 90);
    assert(position && position->kind == PositionKind::SourceByte && position->index == 0);
    for (const auto detail : {ChapterLoad::Reading, ChapterLoad::Original}) {
        assert(document.load_chapter_at_byte(0, 0, position->offset, chapter, offset, error, detail));
        assert(chapter.part == 2 && chapter.last_part && offset > 0);
        Anchor anchor;
        assert(chapter.anchor_at(offset, anchor) && anchor.resource == document.spine().front().resource);
    }
    assert(!document.load_chapter_at_byte(0, 0, position->offset, chapter, offset, error, ChapterLoad::Reading,
                                          [] { return true; }));
    assert(error == kChapterCancelled && chapter.text.empty());
}
