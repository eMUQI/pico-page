#include "pico_page/document.h"
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
    assert(book.metadata().partial_md5 == "acc8613b9b4639011c4ff6dbdef6b6f7");
    assert(book.spine().size() == 2 && book.toc().size() == 2);
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
        assert(!isolated.load_chapter(1, second, error) && second.text.empty());
    }
    if (argc >= 5) {
        EpubDocument styled;
        assert(styled.open(argv[4], error));
        assert(styled.load_chapter(0, first, error));
        assert(first.text.find(U"正文保留 内联") != std::u32string::npos);
        assert(first.text.find(U"[图片：示例图片]") != std::u32string::npos);
        assert(!first.warnings.empty() && !first.plain_dom);
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
        assert(images.open(fixture_dir + "complex-svg-cover.epub", error) && images.load_chapter(0, first, error));
        assert(first.images.size() == 1 && first.images[0].resource.empty());
    }
    return 0;
}
