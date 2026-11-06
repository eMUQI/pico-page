#include "pico_page/document.h"
#include <algorithm>
#include <fstream>
#include <cassert>
#include <iostream>
#include <filesystem>
#include <tuple>

using namespace pico_page;

#ifdef PICO_PAGE_WRAP_CALLOC_TEST
static bool fail_zip_index = false;
extern "C" void* __real_calloc(std::size_t count, std::size_t size);
extern "C" void* __wrap_calloc(std::size_t count, std::size_t size) {
    if (fail_zip_index && count == 7 && size == 8) {
        fail_zip_index = false;
        return nullptr;
    }
    return __real_calloc(count, size);
}
#endif


static void same_reading_part(const Chapter& a, const Chapter& b) {
    assert(a.resource == b.resource && a.part == b.part && a.last_part == b.last_part);
    assert(a.split_node == b.split_node && a.plain_dom == b.plain_dom && a.long_source_node == b.long_source_node);
    assert(a.text == b.text && a.css_sources == b.css_sources && a.warnings == b.warnings);
    assert(a.source_nodes.empty() && b.source_nodes.empty());
    assert(a.blocks.size() == b.blocks.size() && a.runs.size() == b.runs.size());
    assert(a.fragments.size() == b.fragments.size() && a.links.size() == b.links.size() && a.images.size() == b.images.size());
    for (std::size_t i = 0; i < a.blocks.size(); ++i) {
        const auto& x = a.blocks[i]; const auto& y = b.blocks[i];
        assert(std::tie(x.begin, x.end, x.kind) == std::tie(y.begin, y.end, y.kind));
    }
    for (std::size_t i = 0; i < a.runs.size(); ++i) {
        const auto& x = a.runs[i]; const auto& y = b.runs[i];
        assert(std::tie(x.begin, x.end, x.element_reference) == std::tie(y.begin, y.end, y.element_reference));
        assert(x.source == y.source);
    }
    for (std::size_t i = 0; i < a.fragments.size(); ++i)
        assert(a.fragments[i].id == b.fragments[i].id && a.fragments[i].offset == b.fragments[i].offset);
    for (std::size_t i = 0; i < a.links.size(); ++i) {
        const auto& x = a.links[i]; const auto& y = b.links[i];
        assert(std::tie(x.begin, x.end, x.resource, x.fragment, x.external) ==
               std::tie(y.begin, y.end, y.resource, y.fragment, y.external));
    }
    for (std::size_t i = 0; i < a.images.size(); ++i) {
        const auto& x = a.images[i]; const auto& y = b.images[i];
        assert(std::tie(x.begin, x.end, x.resource, x.alt) == std::tie(y.begin, y.end, y.resource, y.alt));
        assert(x.source == y.source);
    }
}

int main(int argc, char** argv) {
    {
        Md5Digest digest;
        assert(digest.update("a") && digest.update("bc"));
        assert(digest.finish() == "900150983cd24fb0d6963f7d28e17f72");
        assert(!digest.update("d") && digest.finish().empty());
        Md5Digest empty;
        assert(empty.finish() == "d41d8cd98f00b204e9800998ecf8427e");
    }
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
        static unsigned fragment_index_denials = 0;
        for (const bool fragment_index_available : {true, false}) {
            fragment_index_denials = 0;
            if (!fragment_index_available) {
                set_chapter_memory_probe([](std::size_t bytes) {
                    constexpr std::size_t fragment_index_bytes = 2048 * sizeof(std::uint16_t);
                    if (bytes != fragment_index_bytes) return true;
                    ++fragment_index_denials;
                    return false;
                });
            }
            EpubDocument fragment_book;
            Chapter first_part, second_part;
            assert(fragment_book.open(directory + "fragment-index.epub", error));
            assert(fragment_book.load_chapter(0, first_part, error, ChapterLoad::Reading, 0, {}, {}, true));
            assert(!first_part.last_part && first_part.fragments.size() == 2);
            assert(first_part.fragments[0].id == "z" && first_part.fragments[0].offset == 0);
            assert(first_part.fragments[1].id == "a" && first_part.fragments[1].offset == 2);
            assert(fragment_book.load_chapter(0, second_part, error, ChapterLoad::Reading, 1, {}, {}, true));
            assert(second_part.last_part && second_part.fragments.size() == 1 && second_part.fragments[0].id == "z");
            assert(second_part.text[second_part.fragments[0].offset] == U'丙');
            for (const bool sequential : {false, true}) {
                Chapter duplicate;
                assert(!fragment_book.load_chapter(1, duplicate, error, ChapterLoad::Reading, 0, {}, {}, sequential));
                assert(error.rfind("Duplicate XHTML fragment id", 0) == 0 && duplicate.fragments.empty());
            }
            assert(fragment_index_available == (fragment_index_denials == 0));
            set_chapter_memory_probe(nullptr);
        }
        {
            EpubDocument html;
            Chapter chapter;
            assert(html.open(directory + "html-body.epub", error));
            assert(html.load_chapter(0, chapter, error));
            assert(chapter.text == U"甲\u00a0&乙 < 3\n尾 段\n末尾");
            assert(!chapter.plain_dom && chapter.text.find(U"var") == std::u32string::npos);
            assert(html.open(directory + "xhtml5-strict.epub", error));
            assert(html.load_chapter(0, chapter, error) && chapter.plain_dom && chapter.text == U"严格源树");
            assert(html.open(directory + "xhtml5-strict-html-extension.epub", error));
            assert(html.load_chapter(0, chapter, error) && chapter.plain_dom && chapter.text == U"严格源树");
            Anchor source;
            assert(chapter.anchor_at(1, source) && source.node == "/html[1]/body[1]/p[1]/text()[1]" && source.offset == 1);

            assert(html.open(directory + "xhtml5-unknown-entity.epub", error));
            assert(!html.load_chapter(0, chapter, error) && chapter.text.empty());
            assert(html.open(directory + "html-body-parts.epub", error));
            std::u32string complete;
            for (std::uint32_t part = 0;; ++part) {
                Chapter ordinary, streamed;
                assert(html.load_chapter(0, ordinary, error, ChapterLoad::Reading, part));
                assert(html.load_chapter(0, streamed, error, ChapterLoad::Reading, part, {}, {}, true));
                same_reading_part(ordinary, streamed);
                if (!complete.empty()) complete += U'\n';
                complete += streamed.text;
                if (streamed.last_part) { assert(part >= 2); break; }
            }
            std::u32string expected;
            for (unsigned i = 0; i < 20000; ++i) {
                if (i) expected += U'\n';
                expected += U"甲乙&丁\u00a0😀";
                for (const auto digit : std::to_string(i)) expected += static_cast<char32_t>(digit);
            }
            assert(complete == expected);
            EpubDocument late_ordinary, late_stream;
            assert(late_ordinary.open(directory + "html-late-repair.epub", error));
            assert(late_stream.open(directory + "html-late-repair.epub", error));
            std::u32string late_text;
            for (std::uint32_t part = 0;; ++part) {
                Chapter ordinary, streamed;
                assert(late_ordinary.load_chapter(0, ordinary, error, ChapterLoad::Reading, part));
                assert(late_stream.read_next_chapter_part(0, streamed, error));
                same_reading_part(ordinary, streamed);
                if (!late_text.empty()) late_text += U'\n';
                late_text += streamed.text;
                if (streamed.last_part) { assert(part >= 2); break; }
            }
            std::u32string late_expected;
            for (unsigned i = 0; i < 20000; ++i) {
                if (i) late_expected += U'\n';
                late_expected += U"甲乙丁😀";
                for (const auto digit : std::to_string(i)) late_expected += static_cast<char32_t>(digit);
            }
            late_expected += U"\n尾部";
            assert(late_text == late_expected);
            for (const auto kind : {"tree", "comment", "void", "namespace", "cdata"}) {
                EpubDocument repaired;
                assert(repaired.open(directory + "html-late-" + kind + ".epub", error));
                Chapter original;
                assert(repaired.read_next_chapter_part(0, original, error) && !original.last_part);
                for (;;) {
                    Chapter tail;
                    assert(repaired.read_next_chapter_part(0, tail, error));
                    if (tail.last_part) break;
                }
                Chapter replayed;
                assert(repaired.load_chapter(0, replayed, error, ChapterLoad::Reading, 0));
                assert(original.text == replayed.text && original.part == replayed.part && original.last_part == replayed.last_part);
                assert(original.blocks.size() == replayed.blocks.size());
                for (std::size_t i = 0; i < original.blocks.size(); ++i) {
                    const auto& a = original.blocks[i]; const auto& b = replayed.blocks[i];
                    assert(std::tie(a.begin, a.end, a.kind) == std::tie(b.begin, b.end, b.kind));
                }
                for (const auto& run : original.runs) {
                    Anchor saved;
                    assert(original.anchor_at(run.begin, saved));
                    std::uint32_t restored = 0;
                    assert(replayed.offset_for(saved, restored) && restored == run.begin);
                }
            }
            assert(html.open(directory + "html-bom.epub", error));
            assert(html.load_chapter(0, chapter, error) && chapter.text == U"有 BOM 的正文");
            for (const auto kind : {"script", "script-case", "script-ns", "style", "style-case", "style-ns", "closed", "after-repair",
                                    "nested-script-inner", "nested-script-outer", "nested-style-inner", "nested-style-outer"}) {
                const auto path = directory + "html-raw-" + kind + ".epub";
                EpubDocument ordinary, sequential;
                assert(ordinary.open(path, error) && sequential.open(path, error));
                assert(ordinary.load_chapter(0, chapter, error));
                assert(chapter.text == U"正文");
                if (std::string(kind) == "style" || std::string(kind) == "style-ns") {
                    assert(chapter.css_sources.size() == 1);
                    assert(chapter.css_sources.front().find("&missing; </stylex> 甲😀 if(a<b)") != std::string::npos);
                }
                Chapter reading, streamed;
                assert(ordinary.load_chapter(0, reading, error, ChapterLoad::Reading));
                assert(sequential.read_next_chapter_part(0, streamed, error));
                same_reading_part(reading, streamed);
                assert(streamed.text == U"正文");
            }
            for (const auto kind : {"denied", "data", "limit", "entities"}) {
                const auto path = directory + "html-image-" + kind + ".epub";
                EpubDocument ordinary, sequential;
                assert(ordinary.open(path, error) && sequential.open(path, error));
                Chapter reading, streamed;
                assert(ordinary.load_chapter(0, reading, error, ChapterLoad::Reading));
                assert(sequential.read_next_chapter_part(0, streamed, error));
                same_reading_part(reading, streamed);
                assert(reading.text.find(U"前文") != std::u32string::npos && reading.text.find(U"后文") != std::u32string::npos);
                assert(reading.images.size() == 1 && reading.images.front().resource.empty());
            }
            for (const auto name : {"html-image-oversized.epub", "html-image-external.epub",
                                    "html-image-truncated.epub", "html-attribute-oversized.epub"}) {
                assert(html.open(directory + name, error));
                assert(!html.load_chapter(0, chapter, error) && chapter.text.empty());
                assert(html.open(directory + name, error));
                assert(!html.read_next_chapter_part(0, chapter, error) && chapter.text.empty());
            }



            for (const auto name : {"html-internal-dtd.epub", "html-token-limit.epub"}) {
                assert(html.open(directory + name, error));
                assert(!html.load_chapter(0, chapter, error));
                assert(chapter.text.empty());
            }
            assert(html.open(directory + "denied-inline-image.epub", error));
            assert(html.load_chapter(0, chapter, error));
            assert(chapter.text.find(U"前文") != std::u32string::npos && chapter.text.find(U"后文") != std::u32string::npos);
            assert(chapter.images.size() == 1 && chapter.images.front().resource.empty());
            assert(!html.open(directory + "opf-unescaped-amp.epub", error));
            assert(error.find("Invalid XML:") == 0 && error.find("line ") != std::string::npos);
        }
        for (const char* name : {"opf-invalid-empty.epub", "opf-invalid-paired.epub", "opf-invalid-attribute.epub"}) {
            EpubDocument invalid;
            assert(!invalid.open(directory + name, error));
            assert(error.find("Missing EPUB package") != std::string::npos ||
                   error.find("XML attribute limit exceeded") != std::string::npos);
            assert(invalid.spine().empty() && invalid.toc().empty());
        }
        EpubDocument indexed;
        assert(indexed.open(directory + "zip-index-paths.epub", error));
        assert(indexed.spine().size() == 4);
#ifdef PICO_PAGE_WRAP_CALLOC_TEST
        fail_zip_index = true;
        assert(!indexed.open(directory + "zip-index-paths.epub", error));
        assert(!fail_zip_index && error == "ZIP index allocation failed" && indexed.spine().empty());
        assert(indexed.open(directory + "zip-index-paths.epub", error));
#endif
        for (std::size_t i = 0; i < 4; ++i) {
            Chapter chapter;
            assert(indexed.load_chapter(i, chapter, error));
            assert(chapter.text == U"chapter " + std::u32string(1, U'0' + i));
        }
        for (const char* name : {"zip-index-missing-case.epub", "zip-index-duplicate.epub", "zip-index-nul.epub"}) {
            assert(!indexed.open(directory + name, error));
            assert(error == (std::string(name) == "zip-index-missing-case.epub" ?
                "Unsupported or missing spine resource" : "Unsafe or duplicate ZIP path"));
            assert(indexed.spine().empty());
        }
        for (const unsigned cancel_at : {5U, 9U}) {
            unsigned index_checks = 0;
            assert(!indexed.open(directory + "zip-index-paths.epub", error, [&] { return ++index_checks == cancel_at; }));
            assert(error == "EPUB open cancelled" && indexed.spine().empty());
        }
        assert(indexed.open(directory + "zip-index-paths.epub", error));
        const auto check_toc = [&](const char* name, const std::vector<TocEntry>& expected) {
            EpubDocument source;
            assert(source.open(directory + name, error));
            assert(source.toc().size() == expected.size());
            for (std::size_t i = 0; i < expected.size(); ++i) {
                const auto& actual = source.toc()[i];
                assert(actual.label == expected[i].label && actual.resource == expected[i].resource &&
                       actual.fragment == expected[i].fragment && actual.depth == expected[i].depth);
            }
        };
        check_toc("nested-toc.epub", {
            {"第一章：长标题检查目录折行与右侧展开热区不能重叠", "EPUB/chapter1.xhtml", "", 0},
            {"内联文字", "EPUB/chapter1.xhtml", "mixed", 1},
            {"脚注位置", "EPUB/chapter1.xhtml", "note", 1},
            {"第二章", "EPUB/chapter2.xhtml", "", 0}});
        check_toc("ncx-nested.epub", {
            {"第一卷 & 起点", "EPUB/chapter1.xhtml", "", 0},
            {"内联 文字", "EPUB/chapter1.xhtml", "mixed", 1},
            {"脚注位置", "EPUB/chapter1.xhtml", "note", 1},
            {"第二章", "EPUB/chapter2.xhtml", "cross-chapter", 0}});
        for (const char* name : {"nav-malformed-ncx.epub", "nav-empty-ncx.epub"})
            check_toc(name, {
                {"第一卷 & 起点", "EPUB/chapter1.xhtml", "", 0},
                {"内联 文字", "EPUB/chapter1.xhtml", "mixed", 1},
                {"脚注位置", "EPUB/chapter1.xhtml", "note", 1},
                {"第二章", "EPUB/chapter2.xhtml", "cross-chapter", 0}});
        for (const char* name : {"ncx-malformed.epub", "ncx-budget-entries.epub",
                                 "ncx-budget-label.epub", "ncx-budget-strings.epub"})
            check_toc(name, {{"one", "EPUB/chapter1.xhtml", "", 0},
                             {"two", "EPUB/chapter2.xhtml", "", 0}});
        for (const char* name : {"opf-budget-items.epub", "opf-budget-strings.epub"}) {
            EpubDocument excessive;
            assert(!excessive.open(directory + name, error));
            assert(error.find("Package/navigation") != std::string::npos);
        }
        EpubDocument large;
        const auto path = directory + "large-toc.epub";
        unsigned checks = 0;
        assert(large.open(path, error, [&] { ++checks; return false; }));
        assert(large.spine().size() == 3000 && large.toc().size() == 3030);
        for (std::size_t volume = 0; volume < 30; ++volume) {
            const auto& parent = large.toc()[volume * 101];
            assert(parent.label == "卷 " + std::to_string(volume) && parent.depth == 0 && parent.fragment.empty());
            assert(parent.resource == "EPUB/chapter" + std::to_string(volume * 100) + ".xhtml");
            for (std::size_t child = 0; child < 100; ++child) {
                const auto number = std::to_string(volume * 100 + child);
                const auto& item = large.toc()[volume * 101 + child + 1];
                assert(item.label == "章 " + number + " & 正文" && item.resource == "EPUB/chapter" + number + ".xhtml");
                assert(item.fragment == "body" && item.depth == 1);
            }
        }
        Chapter last;
        assert(large.load_chapter(2999, last, error) && last.text == U"正文 2999");
        EpubDocument reversed;
        assert(reversed.open(directory + "large-toc-reversed.epub", error));
        assert(reversed.spine().size() == large.spine().size() && reversed.toc().size() == large.toc().size());
        for (std::size_t i = 0; i < large.spine().size(); ++i)
            assert(reversed.spine()[i].resource == large.spine()[i].resource && reversed.spine()[i].bytes == large.spine()[i].bytes);
        assert(reversed.load_chapter(2999, last, error) && last.text == U"正文 2999");
        EpubDocument cancelled;
        unsigned cancelling_checks = 0;
        assert(checks > 4 && !cancelled.open(path, error, [&] { return ++cancelling_checks == checks - 2; }));
        assert(error == kChapterCancelled && cancelled.spine().empty() && cancelled.toc().empty());
    }
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
        EpubDocument block_book;
        assert(block_book.open(argv[3], error));
        assert(block_book.load_chapter(1, found, error, ChapterLoad::Reading) && found.split_node && !found.last_part);
        assert(found.text.size() == 2 * kChapterPartScalars);
        assert(block_book.load_chapter(1, middle, error, ChapterLoad::Reading, kLastChapterPart) && middle.part == 1 && middle.split_node);
        assert(found.text.size() + middle.text.size() == 200001);
    }
    for (int argument = 1; argument < argc; ++argument) {
        EpubDocument source;
        if (!source.open(argv[argument], error)) continue;
        for (std::size_t index = 0; index < source.spine().size(); ++index) {
            Chapter full, layout;
            std::string full_error, layout_error;
            const bool loaded = source.load_chapter(index, full, full_error);
            assert(source.load_chapter(index, layout, layout_error, ChapterLoad::Layout) == loaded && layout_error == full_error);
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
        assert(!first.warnings.empty() && first.plain_dom);
        const auto image = static_cast<std::uint32_t>(first.text.find(U"[图片：示例图片]"));
        Anchor element;
        assert(first.anchor_at(image, element) && element.node == "/html[1]/body[1]/p[2]/img[1]" && element.offset == 0);
        std::uint32_t image_restored = 0;
        assert(first.offset_for(element, image_restored) && image_restored == image);
        assert(first.images.size() == 1 && first.images[0].resource == "EPUB/image.png" && first.images[0].source == element);
        Chapter original;
        assert(styled.load_chapter(0, original, error, ChapterLoad::Original));
        assert(original.text == first.text && original.runs.size() == first.runs.size());
        assert(original.style(original.blocks.front().style).align == document::css::Align::Justify);
        const auto style_at = [&](std::u32string_view text) -> const document::css::ComputedStyle& {
            const auto offset = original.text.find(text);
            const auto run = std::find_if(original.runs.begin(), original.runs.end(),
                [&](const auto& item) { return item.begin <= offset && offset < item.end; });
            assert(run != original.runs.end());
            return original.style(run->style);
        };
        const auto& emphasis = style_at(U"内联");
        assert(emphasis.italic && emphasis.font_px == 24);
        assert(style_at(U"隐藏").display == document::css::Display::None);
        const auto& image_style = style_at(U"[图片：示例图片]");
        assert(image_style.width_px == 40 && image_style.max_width_px == 80);
        assert(image_style.box_percentages & document::css::ComputedStyle::width_percent);
        Anchor styled_anchor;
        assert(original.anchor_at(image, styled_anchor) && styled_anchor == element);
        const std::string styled_path = argv[4];
        const auto entity_path = styled_path.substr(0, styled_path.find_last_of('/') + 1) + "unresolved-entity.epub";
        EpubDocument unresolved;
        assert(unresolved.open(entity_path, error));
        assert(!unresolved.load_chapter(0, first, error) && first.text.empty());
        const auto fixture_dir = styled_path.substr(0, styled_path.find_last_of('/') + 1);
        EpubDocument font_layout;
        assert(font_layout.open(fixture_dir + "font-layout.epub", error));
        assert(font_layout.load_chapter(0, original, error, ChapterLoad::Original));
        assert(original.author_fonts.size() == 2 && style_at(U"内联").font_families == 1);
        assert(original.author_fonts.front().resource == "EPUB/Fonts/welcome.ttf");
        EpubDocument original_layout;
        assert(original_layout.open(fixture_dir + "original-layout.epub", error));
        assert(original_layout.load_chapter(0, original, error, ChapterLoad::Original));
        const auto block_at = [&](std::u32string_view text) {
            const auto offset = original.text.find(text);
            assert(offset != std::u32string::npos);
            return std::find_if(original.blocks.begin(), original.blocks.end(),
                [&](const auto& block) { return block.begin <= offset && offset < block.end; });
        };
        assert(original.text.find(U"第一章\n山边小村") != std::u32string::npos && block_at(U"第一章") == block_at(U"山边小村"));
        assert(original.text.find(U"甲乙\n丙丁") != std::u32string::npos && block_at(U"甲乙") == block_at(U"丙丁"));
        assert(original.images.size() == 2);
        assert(original.images[0].width == 300 && original.images[0].height == 150);
        assert(original.images[1].width == 400 && original.images[1].height == 200);
        Chapter reloaded;
        EpubDocument header_memory;
        assert(header_memory.open(fixture_dir + "original-layout.epub", error));
        set_chapter_memory_probe([](std::size_t bytes) { return bytes != 64U * 1024 + 128U * 1024; });
        assert(!header_memory.load_chapter(0, reloaded, error, ChapterLoad::Original));
        assert(error.rfind(kChapterMemoryError, 0) == 0 && reloaded.text.empty());
        set_chapter_memory_probe(nullptr);
        assert(header_memory.load_chapter(0, reloaded, error, ChapterLoad::Original) && reloaded.images[0].width == 300);
        assert(original_layout.load_chapter(0, reloaded, error, ChapterLoad::Original));
        assert(reloaded.images[0].width == 300 && reloaded.images[1].height == 200);
        assert(original.style(block_at(U"[图片：横幅]")->style).bleed == 0b1011);
        assert(original.style(block_at(U"[图片：照片]")->style).bleed == 0);
        assert(original_layout.load_chapter(0, first, error, ChapterLoad::Reading));
        assert(first.images.size() == 2 && !first.images[0].width && !first.images[0].height);
        assert(first.blocks.size() == original.blocks.size() + 2);
        EpubDocument images;
        assert(images.open(fixture_dir + "svg-cover.epub", error) && images.load_chapter(0, first, error));
        assert(first.images.size() == 1 && first.images[0].resource == "EPUB/cover.png");
        assert(first.images[0].source.node == "/html[1]/body[1]/svg[1]");
        assert(first.anchor_at(first.images[0].begin, element) && element == first.images[0].source);
        std::vector<std::uint8_t> encoded;
        EpubDocument resources;
        assert(resources.open_verified(fixture_dir + "svg-cover.epub", images.metadata(), error, {},
                                       images.verified_epub_index()));
        assert(resources.read_stats().xml_feed_bytes == 0);
        assert(resources.read_image(first.images[0].resource, encoded, error) && encoded.size() > 32);
        assert(!resources.read_image("../EPUB/cover.png", encoded, error) && encoded.empty());
        assert(!resources.read_image("https://example.invalid/cover.png", encoded, error) && encoded.empty());
        assert(!resources.read_image("EPUB/large.png", encoded, error) && encoded.empty());
        assert(!resources.read_image("EPUB/chapter1.xhtml", encoded, error) && encoded.empty());
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
        for (const auto* encoding : {"utf-8", "gb18030", "utf-16-le", "utf-16-be"}) {
            EpubDocument headings;
            const auto path = directory + "toc-" + encoding + ".txt";
            assert(headings.open(path, error));
            const std::vector<std::string> labels = {"序章 开始", "第一章 边界😀", "Chapter II: Café", "尾声 结束"};
            assert(headings.toc().size() == labels.size());
            const auto& entries = headings.metadata().txt_headings;
            assert(entries.size() == labels.size() && entries[1].byte_offset == 49150 && entries[1].part == 0);
            for (std::size_t i = 0; i < entries.size(); ++i) {
                assert(entries[i].label == labels[i] && headings.toc()[i].label == labels[i]);
                Chapter target;
                assert(headings.load_chapter(0, target, error, ChapterLoad::Reading, entries[i].part));
                assert(entries[i].offset < target.text.size());
                assert(target.text[entries[i].offset] == (i == 0 ? U'序' : i == 1 ? U'第' : i == 2 ? U'C' : U'尾'));
                Anchor anchor;
                assert(target.anchor_at(entries[i].offset, anchor) && anchor.node == "part:" + std::to_string(entries[i].part));
            }
            EpubDocument reopened;
            assert(reopened.open_verified(path, headings.metadata(), error));
            assert(reopened.toc().size() == labels.size() && reopened.metadata().txt_headings[2].offset == entries[2].offset);
            EpubDocument cancelled;
            unsigned checkpoints = 0;
            assert(!cancelled.open(path, error, [&] { return ++checkpoints == 3; }));
            assert(cancelled.toc().empty() && cancelled.metadata().txt_headings.empty());
            set_chapter_memory_probe([](std::size_t) { return false; });
            EpubDocument without_toc;
            assert(without_toc.open(path, error));
            assert(without_toc.metadata().txt_headings.empty() && without_toc.toc().size() == 1 && without_toc.toc()[0].label == "正文");
            set_chapter_memory_probe(nullptr);
        }
        {
            const auto path = directory + "toc-limit.txt";
            std::ofstream file(path, std::ios::binary);
            for (unsigned i = 0; i < 2050; ++i) file << "Chapter " << i << "\n";
            file.close();
            EpubDocument limited;
            assert(limited.open(path, error));
            assert(limited.metadata().txt_headings.empty() && limited.toc().size() == 1 && limited.toc()[0].label == "正文");
            assert(!limited.open(path, error, [] { return true; }));
        }
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
        for (const auto encoding : {TxtEncoding::Utf16Le, TxtEncoding::Utf16Be}) {
            const std::string name = encoding == TxtEncoding::Utf16Le ? "utf-16-le" : "utf-16-be";
            const auto path = directory + name + ".txt";
            assert(txt.open(path, error));
            assert(txt.metadata().txt_encoding == encoding && txt.metadata().book_id == sha256_file(path));
            assert(txt.metadata().txt_offsets.size() == 4 && txt.metadata().txt_offsets[1] == 49154);
            assert(verified.open_verified(path, txt.metadata(), error));
            std::u32string text;
            Anchor middle;
            for (std::uint32_t part = 0;; ++part) {
                Chapter piece;
                assert(verified.load_chapter(0, piece, error, ChapterLoad::Reading, part));
                assert(!piece.text.empty());
                if (part == 1) assert(piece.anchor_at(2, middle));
                text += piece.text;
                if (piece.last_part) break;
                assert(piece.text.back() != U'\r');
            }
            std::u32string expected(24574, U'甲');
            expected += U"😀\r\n第二段"; expected.append(30000, U'乙'); expected += U"末尾";
            assert(text == expected);
            assert(verified.find_chapter_part(0, restored, error, ChapterLoad::Reading,
                [&](const Chapter& part) { std::uint32_t offset = 0; return part.offset_for(middle, offset); }));
            assert(restored.part == 1);
            assert(txt.open(directory + name + "-cr-run.txt", error));
            assert(txt.metadata().txt_offsets.size() >= 4);
            for (std::size_t i = 1; i < txt.metadata().txt_offsets.size(); ++i)
                assert(txt.metadata().txt_offsets[i] - txt.metadata().txt_offsets[i - 1] <= kTxtPartBytes + 6);
            std::u32string cr_text;
            for (std::uint32_t part = 0;; ++part) {
                Chapter piece;
                assert(txt.load_chapter(0, piece, error, ChapterLoad::Reading, part));
                cr_text += piece.text;
                if (piece.last_part) break;
            }
            assert(cr_text == U"甲" + std::u32string(60000, U'\r') + U"乙");
            unsigned checks = 0;
            assert(!txt.open(path, error, [&] { return ++checks >= 3; }) && txt.spine().empty());
            for (const auto suffix : {"odd", "high", "low", "pair", "nul", "empty"}) {
                assert(!txt.open(directory + name + "-" + suffix + ".txt", error));
                assert(error.find("UTF-16") != std::string::npos && txt.spine().empty());
            }
        }
        assert(!txt.open(directory + "invalid.txt", error) && error.find("编码不受支持") != std::string::npos);
        assert(txt.open(directory + "gb18030.txt", error) && txt.metadata().txt_encoding == TxtEncoding::Gb18030);
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
    {
        const auto directory = std::filesystem::path(argv[1]).parent_path();
        for (const char* name : {"valid.epub", "multi-part.epub", "multi-part-link.epub", "mapping-node-parts.epub",
                                 "mapping-node-utf16.epub", "mapping-node-entities.epub", "image-flow.epub",
                                 "stream-events.epub", "stream-stored.epub", "stream-deferred-final.epub", "part-boundary-image.epub"}) {
            EpubDocument reference, sequential;
            assert(reference.open((directory / name).string(), error));
            assert(sequential.open_verified((directory / name).string(), reference.metadata(), error));
            for (std::size_t index = 0; index < reference.spine().size(); ++index) {
                for (std::uint32_t part = 0;; ++part) {
                    Chapter expected, actual;
                    if (!reference.load_chapter(index, expected, error, ChapterLoad::Reading, part)) {
                        std::cerr << name << " chapter=" << index << " part=" << part << ": " << error << std::endl;
                        return 1;
                    }
                    assert(sequential.read_next_chapter_part(index, actual, error));
                    same_reading_part(expected, actual);
                    if (actual.last_part) { assert(!sequential.read_stats().stream_active); break; }
                    assert(sequential.read_stats().stream_active);
                }
            }
        }
        EpubDocument sequential;
        const auto path = (directory / "stream-events.epub").string();
        assert(sequential.open(path, error));
        const auto base_zip = sequential.read_stats().zip_allocated_bytes;
        Chapter part;
        assert(sequential.read_next_chapter_part(0, part, error) && !part.last_part);
        unsigned checks = 0;
        assert(!sequential.read_next_chapter_part(0, part, error, [&] { return ++checks == 4; }));
        assert(error == kChapterCancelled && part.text.empty() && !sequential.read_stats().stream_active);
        assert(sequential.read_stats().zip_allocated_bytes == base_zip);
        assert(sequential.read_next_chapter_part(0, part, error) && part.part == 0);
        set_chapter_memory_probe([](std::size_t) { return false; });
        assert(!sequential.read_next_chapter_part(0, part, error));
        set_chapter_memory_probe(nullptr);
        assert(error == kChapterMemoryError && part.text.empty() && !sequential.read_stats().stream_active);
        assert(sequential.read_stats().zip_allocated_bytes == base_zip);
        assert(sequential.read_next_chapter_part(0, part, error));
        const auto reads = sequential.read_stats().source_read_bytes;
        sequential.reset_chapter_stream();
        assert(!sequential.read_stats().stream_active && sequential.read_stats().source_read_bytes == reads);
        assert(sequential.read_stats().zip_allocated_bytes == base_zip);
        assert(sequential.read_next_chapter_part(0, part, error));
        assert(sequential.load_chapter(0, part, error, ChapterLoad::Reading, 1));
        assert(!sequential.read_stats().stream_active);
        assert(sequential.read_next_chapter_part(0, part, error) && part.part == 0);
        EpubDocument moved = std::move(sequential);
        assert(moved.read_next_chapter_part(0, part, error) && part.part == 1);
        moved.close();
        assert(!moved.read_stats().stream_active && moved.read_stats().zip_allocated_bytes == 0);
        assert(moved.open((directory / "valid.epub").string(), error));
        assert(moved.read_next_chapter_part(0, part, error) && part.part == 0);
        for (const char* name : {"stream-malformed.epub", "stream-events.epub"}) {
            const auto temporary = directory / "stream-interrupted.epub";
            std::filesystem::copy_file(directory / name, temporary, std::filesystem::copy_options::overwrite_existing);
            EpubDocument interrupted;
            assert(interrupted.open(temporary.string(), error));
            assert(interrupted.read_next_chapter_part(0, part, error) && !part.last_part);
            if (std::string(name) == "stream-events.epub") std::filesystem::resize_file(temporary, 1);
            bool failed = false;
            for (unsigned i = 0; i < 64; ++i) {
                if (!interrupted.read_next_chapter_part(0, part, error)) { failed = true; break; }
                assert(!part.last_part);
            }
            assert(failed && !error.empty() && part.text.empty() && !interrupted.read_stats().stream_active);
            interrupted.close();
            std::filesystem::remove(temporary);
        }
        EpubDocument large_token;
        assert(large_token.open((directory / "stream-large-token.epub").string(), error));
        const auto before_token = large_token.read_stats();
        assert(!large_token.read_next_chapter_part(0, part, error));
        assert(error == "Sequential XML token budget exceeded" && part.text.empty());
        assert(!large_token.read_stats().stream_active);
        assert(large_token.read_stats().zip_allocated_bytes == before_token.zip_allocated_bytes);
        assert(large_token.read_stats().xml_feed_bytes - before_token.xml_feed_bytes <= 64U * 1024);
        std::size_t fallback_scalars = 0;
        for (std::uint32_t index = 0;; ++index) {
            assert(large_token.load_chapter(0, part, error, ChapterLoad::Reading, index));
            fallback_scalars += part.text.size() + 1;
            if (part.last_part) break;
        }
        assert(fallback_scalars == std::u32string(U"连续解析保留正文与源位置。\n").size() * 12000);
        Chapter ordinary;
        assert(large_token.load_chapter(0, ordinary, error, ChapterLoad::Reading, 1));
        assert(large_token.load_chapter(0, part, error, ChapterLoad::Reading, 1, {}, {}, true));
        same_reading_part(ordinary, part);
        assert(!large_token.read_stats().stream_active);
        assert(sequential.open(path, error));
        assert(sequential.find_chapter_part(0, part, error, ChapterLoad::Reading,
            [](const Chapter& c) { return c.part == 1; }, {}, {}, true));
        const auto identity = sequential.metadata().book_id;
        const auto catalog_size = sequential.spine().size();
        set_chapter_memory_probe([](std::size_t) { return false; });
        assert(!sequential.detach_reader() && sequential.read_stats().stream_active);
        set_chapter_memory_probe([](std::size_t bytes) { return bytes <= 2048; });
        auto reader = sequential.detach_reader();
        set_chapter_memory_probe(nullptr);
        assert(reader && reader->read_stats().stream_active && !sequential.read_stats().stream_active);
        assert(sequential.read_stats().zip_allocated_bytes == 0 && sequential.metadata().book_id == identity);
        assert(sequential.spine().size() == catalog_size && reader->metadata().book_id == identity);
        assert(!sequential.load_chapter(0, part, error, ChapterLoad::Reading));
        sequential.close();
        const auto opens = epub_open_count();
        assert(reader->load_chapter(0, part, error, ChapterLoad::Reading, 2, {}, {}, true));
        assert(part.part == 2 && epub_open_count() == opens);
        assert(reader->load_chapter(0, ordinary, error, ChapterLoad::Reading, 2));
        same_reading_part(ordinary, part);
        EpubDocument reopened;
        assert(reopened.open_verified(path, reader->metadata(), error, {}, reader->verified_epub_index()));
        assert(reopened.read_stats().xml_feed_bytes == 0 && reopened.spine().size() == catalog_size);
        assert(reopened.load_chapter(0, ordinary, error, ChapterLoad::Reading, 2));
        same_reading_part(ordinary, part);
        assert(reader->load_chapter(0, part, error, ChapterLoad::Reading, 0, {}, {}, true));
        assert(part.part == 0);
        assert(!reader->load_chapter(0, part, error, ChapterLoad::Reading, 1, [] { return true; }, {}, true));
        assert(error == kChapterCancelled && part.text.empty() && !reader->read_stats().stream_active);
        EpubDocument corrupt;
        assert(corrupt.open((directory / "stream-bad-crc.epub").string(), error));
        bool rejected = false;
        for (unsigned i = 0; i < 64; ++i) {
            if (!corrupt.read_next_chapter_part(0, part, error)) { rejected = true; break; }
            assert(!part.last_part);
        }
        assert(rejected && error == "Invalid EPUB stream length or CRC" && part.text.empty());
        assert(!corrupt.read_stats().stream_active);
    }
    return 0;
}
