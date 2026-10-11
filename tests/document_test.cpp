#include "pico_page/document.h"
#include <fstream>
#include <cassert>
#include <iostream>

using namespace pico_page;

int main(int argc, char** argv) {
    assert(argc >= 2);
    std::string error;
    EpubDocument book;
    assert(book.open(argv[1], error));
    assert(book.metadata().title == "Pico Page 合成位置样本");
    assert(book.metadata().language == "zh-CN");
    assert(book.metadata().book_id == "62338330991b0244f2388036892a2891546fd9fe446fab0547c18b9707a6b118");
    {
        FileDigest scan(argv[1]);
        bool complete = false;
        for (unsigned slices = 0; !complete; ++slices) {
            assert(slices < 10000);
            assert(!scan.step(8192, [] { return true; }));
            unsigned checks = 0;
            complete = scan.step(7, [&] { return ++checks > 2; });
        }
        assert(scan.digest() == book.metadata().book_id);
        assert(scan.step(1) && scan.digest() == book.metadata().book_id);
        FileDigest missing(std::string(argv[1]) + ".missing");
        assert(missing.step(8192) && missing.digest().empty());
    }
    assert(book.metadata().partial_md5 == "acc8613b9b4639011c4ff6dbdef6b6f7");
    assert(book.spine().size() == 2 && book.toc().size() == 2);
    {
        const std::string fixture = argv[1];
        const auto directory = fixture.substr(0, fixture.rfind('/') + 1);
        for (const auto kind : {"crlf", "padding", "limit"}) {
            const auto path = directory + "mimetype-" + kind + ".epub";
            FileDigest before(path);
            while (!before.step(8192)) {}
            EpubDocument padded;
            assert(padded.open(path, error));
            assert(padded.metadata().book_id == before.digest());
            assert(padded.spine().size() == book.spine().size());
            Chapter chapter;
            assert(padded.load_chapter(0, chapter, error) && !chapter.text.empty());
            FileDigest after(path);
            while (!after.step(8192)) {}
            assert(after.digest() == before.digest());
        }
        for (const auto kind : {"invalid", "internal-space", "nul", "empty", "oversized", "missing", "read-failure",
                                "directory"}) {
            EpubDocument invalid;
            assert(!invalid.open(directory + "mimetype-" + kind + ".epub", error));
            const std::string name = kind;
            assert(error == (name == "oversized" ? "EPUB mimetype exceeds limit" :
                             name == "read-failure" ? "Cannot read EPUB mimetype" :
                             name == "missing" ? "Missing EPUB mimetype" : "Invalid EPUB mimetype"));
        }
    }
    Chapter first;
    assert(book.load_chapter(0, first, error));
    const auto mixed = first.text.find(U"中文 内联𠮷😀 与 English & 空白。\u00a0保留不换行空格。");
    assert(mixed != std::u32string::npos);
    assert(first.text.find(U"第一章：春水\n中文") == 0);
    for (std::uint32_t i = 0; i < first.text.size(); ++i) {
        Anchor anchor;
        if (!first.anchor_at(i, anchor)) { assert(first.text[i] == U'\n'); continue; }
        std::uint32_t restored = 0;
        assert(first.offset_for(anchor, restored) && restored == i);
        assert(anchor.resource == book.spine()[0].resource);
    }
    Anchor non_bmp;
    auto emoji = static_cast<std::uint32_t>(first.text.find(U'😀'));
    assert(first.anchor_at(emoji, non_bmp));
    assert(non_bmp.node == "/html[1]/body[1]/p[1]/em[1]/text()[1]");
    assert(non_bmp.offset == 3);
    assert(first.plain_dom);
    bool raw_found = false;
    for (const auto& node : first.source_nodes) if (node.node == "/html[1]/body[1]/p[1]/text()[1]") {
        assert(node.text == U"  中文 "); raw_found = true;
    }
    assert(raw_found);
    std::uint32_t note = 0;
    assert(first.fragment_offset("note", note));
    assert(first.text.substr(note).find(U"这是原创脚注文本。") == 0);
    assert(first.links.size() == 1 && first.links[0].fragment == "note" && !first.links[0].external);
    assert(first.links[0].resource == first.resource);
    Chapter second;
    assert(book.load_chapter(1, second, error));
    assert(second.text.find(U"第二章：远山") == 0 && second.text.size() > 1900);
    assert(!second.offset_for(non_bmp, note));
    assert(!book.load_chapter(2, second, error) && second.text.empty());
    {
        // A parse that would leave too little free memory stops with the memory error and
        // releases the chapter; the same load succeeds once memory is available again.
        static std::size_t allowance;
        allowance = 100;
        set_chapter_memory_probe([](std::size_t bytes) { return bytes <= allowance; });
        Chapter limited;
        for (const auto detail : {ChapterLoad::Mapping, ChapterLoad::Reading, ChapterLoad::Layout}) {
            limited.text.assign(64, U'x');
            assert(!book.load_chapter(0, limited, error, detail));
            assert(error.rfind(kChapterMemoryError, 0) == 0 && limited.text.empty() && limited.text.capacity() < 64);
        }
        static unsigned checks;
        checks = 0;
        set_chapter_memory_probe([](std::size_t) { return ++checks < 4; });
        assert(!book.load_chapter(0, limited, error) && error.rfind(kChapterMemoryError, 0) == 0);
        set_chapter_memory_probe(nullptr);
        assert(book.load_chapter(0, limited, error) && limited.text == first.text);
    }
    {
        // A long chapter is read in parts that end at block boundaries. Joined, the parts give
        // the whole text; every scalar keeps its source anchor, and a part can be found by an
        // anchor or a fragment inside it.
        const std::string fixture = argv[1];
        EpubDocument long_book;
        assert(long_book.open(fixture.substr(0, fixture.rfind('/') + 1) + "multi-part.epub", error));
        std::u32string expected;
        for (int number = 0; number < 5000; ++number) {
            if (number) expected += U'\n';
            expected += U"第";
            for (const char digit : std::to_string(number)) expected += static_cast<char32_t>(digit);
            expected += U"段：这一段用于检查长章节按段落边界分成多个部分。";
        }
        for (const auto detail : {ChapterLoad::Mapping, ChapterLoad::Reading, ChapterLoad::Layout}) {
            std::u32string joined;
            std::uint32_t count = 0;
            Chapter piece;
            do {
                assert(long_book.load_chapter(0, piece, error, detail, count) && piece.part == count && !piece.split_node);
                // A part closes at the block that reaches the threshold; its final separator is dropped.
                assert(piece.text.size() + 1 >= kChapterPartScalars || piece.last_part);
                assert(piece.text.size() < kChapterPartScalars + 64 && piece.text.back() != U'\n');
                if (detail == ChapterLoad::Mapping) assert(piece.source_nodes.size() == piece.runs.size());
                if (count) joined += U'\n';
                joined += piece.text;
                ++count;
            } while (!piece.last_part);
            assert(count == 3 && joined == expected);
            assert(!long_book.load_chapter(0, piece, error, detail, count) && piece.text.empty());
            assert(long_book.load_chapter(0, piece, error, detail, kLastChapterPart) && piece.part == count - 1 && piece.last_part);
        }
        {
            Chapter interrupted, retried, baseline;
            unsigned checks = 0;
            assert(!long_book.load_chapter(0, interrupted, error, ChapterLoad::Reading, 2,
                                          [&] { return ++checks == 8; }));
            assert(checks >= 8 && error == kChapterCancelled && interrupted.text.empty() && interrupted.runs.empty());
            assert(long_book.load_chapter(0, retried, error, ChapterLoad::Reading, 2, [] { return false; }));
            assert(long_book.load_chapter(0, baseline, error, ChapterLoad::Reading, 2));
            assert(retried.text == baseline.text && retried.part == 2);
            Anchor a, b;
            assert(retried.anchor_at(1234, a) && baseline.anchor_at(1234, b) && a == b);
        }
        Chapter middle, found;
        assert(long_book.load_chapter(0, middle, error, ChapterLoad::Reading, 1));
        Anchor anchor;
        assert(middle.anchor_at(1234, anchor));
        assert(long_book.find_chapter_part(0, found, error, ChapterLoad::Reading, [&](const Chapter& candidate) {
            std::uint32_t offset = 0;
            return candidate.offset_for(anchor, offset);
        }));
        std::uint32_t offset = 0;
        assert(found.part == 1 && found.text == middle.text && found.offset_for(anchor, offset) && offset == 1234);
        assert(long_book.find_chapter_part(0, found, error, ChapterLoad::Reading, [](const Chapter& candidate) {
            std::uint32_t target = 0;
            return candidate.fragment_offset("late", target);
        }));
        assert(found.part == 2 && found.fragment_offset("late", offset) && found.text.compare(offset, 6, U"第4500段") == 0);
        assert(!long_book.find_chapter_part(0, found, error, ChapterLoad::Reading, [](const Chapter&) { return false; }));
        // A single block longer than a part is divided inside the block.
        EpubDocument block_book;
        assert(block_book.open(argv[3], error));
        assert(block_book.load_chapter(1, found, error, ChapterLoad::Reading) && found.split_node && !found.last_part);
        assert(found.text.size() == 2 * kChapterPartScalars);
        assert(block_book.load_chapter(1, middle, error, ChapterLoad::Reading, kLastChapterPart) && middle.part == 1 && middle.split_node);
        assert(found.text.size() + middle.text.size() == 200001);
    }
    // A layout-only load fails or succeeds like a full load and paginates identically: the same
    // text, blocks, images and run boundaries, with no source strings kept.
    for (int argument = 1; argument < argc; ++argument) {
        EpubDocument source;
        if (!source.open(argv[argument], error)) continue;
        for (std::size_t index = 0; index < source.spine().size(); ++index) {
            Chapter full, layout;
            std::string full_error, layout_error;
            const bool loaded = source.load_chapter(index, full, full_error);
            assert(source.load_chapter(index, layout, layout_error, ChapterLoad::Layout) == loaded && layout_error == full_error);
            // A reading load differs from a full one only in the mapping data it leaves empty.
            Chapter reading;
            std::string reading_error;
            assert(source.load_chapter(index, reading, reading_error, ChapterLoad::Reading) == loaded && reading_error == full_error);
            assert(reading.text == full.text && reading.runs.size() == full.runs.size() && reading.links.size() == full.links.size() &&
                   reading.fragments.size() == full.fragments.size() && reading.images.size() == full.images.size());
            for (std::size_t i = 0; i < full.runs.size(); ++i)
                assert(reading.runs[i].begin == full.runs[i].begin && reading.runs[i].end == full.runs[i].end &&
                       reading.runs[i].source == full.runs[i].source &&
                       reading.runs[i].element_reference == full.runs[i].element_reference);
            for (std::uint32_t offset = 0; offset < full.text.size(); ++offset) {
                Anchor expected, actual;
                const bool anchored = full.anchor_at(offset, expected);
                assert(reading.anchor_at(offset, actual) == anchored && (!anchored || actual == expected));
            }
            assert(reading.source_nodes.empty());
            for (const auto& css : reading.css_sources) assert(css.empty());
            assert(reading.text.capacity() - reading.text.size() <= 4096);
            assert(layout.text == full.text && layout.blocks.size() == full.blocks.size());
            for (std::size_t i = 0; i < full.blocks.size(); ++i)
                assert(layout.blocks[i].begin == full.blocks[i].begin && layout.blocks[i].end == full.blocks[i].end &&
                       layout.blocks[i].kind == full.blocks[i].kind);
            assert(layout.images.size() == full.images.size() && layout.runs.size() == full.runs.size());
            for (std::size_t i = 0; i < full.images.size(); ++i)
                assert(layout.images[i].begin == full.images[i].begin && layout.images[i].end == full.images[i].end &&
                       layout.images[i].resource == full.images[i].resource);
            for (std::size_t i = 0; i < full.runs.size(); ++i)
                assert(layout.runs[i].begin == full.runs[i].begin && layout.runs[i].end == full.runs[i].end &&
                       layout.runs[i].element_reference == full.runs[i].element_reference &&
                       layout.runs[i].source.node.empty() && layout.runs[i].source.resource.empty());
            assert(layout.source_nodes.empty());
            for (const auto& css : layout.css_sources) assert(css.empty());
        }
    }
    std::cout << "native EPUB metadata, identities, chapters, anchors, links: passed\n";
    if (argc >= 3) {
        EpubDocument malformed;
        assert(!malformed.open(argv[2], error));
        assert(!error.empty());
    }
    if (argc >= 4) {
        EpubDocument isolated;
        assert(isolated.open(argv[3], error));
        assert(isolated.load_chapter(0, first, error));
        assert(isolated.load_chapter(1, second, error) && !second.last_part);
    }
    if (argc >= 5) {
        EpubDocument styled;
        assert(styled.open(argv[4], error));
        assert(styled.load_chapter(0, first, error));
        assert(first.text.find(U"正文保留 内联") != std::u32string::npos);
        assert(first.text.find(U"[图片：示例图片]") != std::u32string::npos);
        // An image is reported as a placeholder warning and stays inside the mapping profile.
        assert(!first.warnings.empty() && first.plain_dom);
        const auto image = static_cast<std::uint32_t>(first.text.find(U"[图片：示例图片]"));
        Anchor element;
        assert(first.anchor_at(image, element) && element.node == "/html[1]/body[1]/p[2]/img[1]" && element.offset == 0);
        std::uint32_t image_restored = 0;
        assert(first.offset_for(element, image_restored) && image_restored == image);
        assert(first.images.size() == 1 && first.images[0].resource == "EPUB/image.png" && first.images[0].source == element);
        const std::string styled_path = argv[4];
        const auto entity_path = styled_path.substr(0, styled_path.find_last_of('/') + 1) + "unresolved-entity.epub";
        EpubDocument unresolved;
        assert(unresolved.open(entity_path, error));
        assert(!unresolved.load_chapter(0, first, error) && first.text.empty());
        const auto fixture_dir = styled_path.substr(0, styled_path.find_last_of('/') + 1);
        EpubDocument images;
        assert(images.open(fixture_dir + "svg-cover.epub", error) && images.load_chapter(0, first, error));
        assert(first.images.size() == 1 && first.images[0].resource == "EPUB/cover.png");
        assert(first.images[0].source.node == "/html[1]/body[1]/svg[1]");
        assert(first.anchor_at(first.images[0].begin, element) && element == first.images[0].source);
        std::vector<std::uint8_t> encoded;
        assert(images.read_image(first.images[0].resource, encoded, error) && encoded.size() > 32);
        assert(!images.read_image("../EPUB/cover.png", encoded, error) && encoded.empty());
        assert(!images.read_image("https://example.invalid/cover.png", encoded, error) && encoded.empty());
        assert(!images.read_image("EPUB/large.png", encoded, error) && encoded.empty());
        assert(!images.read_image("EPUB/chapter1.xhtml", encoded, error) && encoded.empty());
        assert(images.open(fixture_dir + "calibre-svg-cover.epub", error) && images.load_chapter(0, first, error));
        assert(first.images.size() == 1 && first.images[0].resource == "EPUB/cover.png");
        assert(images.open(fixture_dir + "background-page.epub", error) && images.load_chapter(0, first, error));
        assert(first.images.size() == 1 && first.images[0].resource == "EPUB/cover.png" && first.images[0].source.node == "/html[1]/body[1]");
        assert(images.open(fixture_dir + "background-text.epub", error) && images.load_chapter(0, first, error) && first.images.empty());
        assert(images.open(fixture_dir + "named-entities.epub", error) && images.load_chapter(0, first, error));
        assert(first.text.rfind(U"甲\u00a0乙\u2014丙\u00e9", 0) == 0 && first.runs.size() == 1 && first.runs[0].end - first.runs[0].begin == 6);
        assert(images.open(fixture_dir + "unknown-entity.epub", error) && !images.load_chapter(0, first, error));
        assert(error.find("Unsupported XML entity: unknown") != std::string::npos);
        assert(images.open(fixture_dir + "complex-svg-cover.epub", error) && images.load_chapter(0, first, error));
        assert(first.images.size() == 1 && first.images[0].resource.empty());
        const auto identity = book.metadata().book_id;
        for (const auto* name : {"epub3-cover", "epub2-cover", "guide-cover", "image-cover", "svg-cover"}) {
            assert(EpubDocument::read_cover(fixture_dir + name + ".epub", encoded, error));
            assert(encoded.size() > 32 && error.empty());
        }
        for (const auto* name : {"valid", "unsafe-cover", "image-flow", "complex-svg-cover", "malformed"}) {
            encoded.assign(10, 1);
            assert(!EpubDocument::read_cover(fixture_dir + name + ".epub", encoded, error));
            assert(encoded.empty() && !error.empty());
        }
        assert(book.metadata().book_id == identity);
        assert(book.load_chapter(0, first, error) && first.resource == "EPUB/chapter1.xhtml");
    }
    {
        const std::string directory = std::string(argv[1]).substr(0, std::string(argv[1]).find_last_of('/') + 1);
        EpubDocument txt;
        assert(txt.open(directory + "sample.txt", error));
        assert(txt.metadata().title == "sample" && txt.spine().size() == 1 && txt.toc().size() == 1);
        assert(txt.metadata().book_id == sha256_file(directory + "sample.txt"));
        EpubDocument verified;
        assert(verified.open_verified(directory + "sample.txt", txt.metadata(), error));
        std::u32string joined;
        Anchor saved;
        for (std::uint32_t part = 0;; ++part) {
            Chapter piece;
            assert(verified.load_chapter(0, piece, error, ChapterLoad::Reading, part));
            assert(piece.part == part && !piece.text.empty());
            if (part == 1) {
                assert(piece.anchor_at(10, saved));
                std::uint32_t position = 0;
                assert(piece.offset_for(saved, position) && position == 10);
            }
            joined += piece.text;
            if (piece.last_part) break;
        }
        std::u32string expected(16384, U'甲');
        expected += U"\r\n第二段😀\n";
        for (int i = 0; i < 12000; ++i) expected += U"长段文字。";
        assert(joined == expected);
        Chapter restored;
        assert(verified.find_chapter_part(0, restored, error, ChapterLoad::Reading,
                                          [&](const Chapter& part) { std::uint32_t offset = 0; return part.offset_for(saved, offset); }));
        assert(restored.part == 1);
        {
            // A character that crosses the last 48 KiB boundary belongs to the preceding part,
            // which is then the final one; no empty part follows it.
            const auto path = directory + "boundary.txt";
            std::ofstream(path, std::ios::binary) << std::string(kTxtPartBytes - 2, 'a') << "\xf0\x9f\x98\x80";
            EpubDocument crossing;
            Chapter piece;
            assert(crossing.open(path, error));
            assert(crossing.load_chapter(0, piece, error, ChapterLoad::Reading, 0));
            assert(piece.last_part && piece.text.size() == kTxtPartBytes - 1);
            assert(!crossing.load_chapter(0, piece, error, ChapterLoad::Reading, 1));
            assert(crossing.load_chapter(0, piece, error, ChapterLoad::Reading, kLastChapterPart) && piece.part == 0);
        }
        assert(!txt.open(directory + "invalid.txt", error) && error.find("编码不受支持") != std::string::npos);
        assert(txt.open(directory + "gb18030.txt", error) && txt.metadata().txt_gb18030);
        assert(txt.metadata().txt_offsets.size() == 3 && txt.metadata().txt_offsets[1] == 49154);
        assert(verified.open_verified(directory + "gb18030.txt", txt.metadata(), error));
        std::u32string decoded;
        for (std::uint32_t part = 0;; ++part) {
            Chapter piece;
            assert(verified.load_chapter(0, piece, error, ChapterLoad::Reading, part));
            decoded += piece.text;
            if (piece.last_part) break;
        }
        assert(decoded == std::u32string(24575, U'甲') + U"😀\ue587" + std::u32string(500, U'乙') + U"\r\n末尾");
    }
    return 0;
}
