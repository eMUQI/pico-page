#include "pico_page/document.h"
#include "pico_page/graphics.h"

#include <algorithm>
#include <iostream>
#include <fstream>
#include <set>

using namespace pico_page;

int main(int argc, char **argv) {
    if (argc < 3) {
        std::cerr << "Usage: public_book_probe FONT EPUB...\n";
        return 2;
    }
    graphics::Font font;
    if (!font.open(argv[1])) {
        std::cerr << font.error() << '\n';
        return 1;
    }
    for (int file = 2; file < argc; ++file) {
        EpubDocument document;
        std::string error;
        if (!document.open(argv[file], error)) {
            std::cerr << argv[file] << ": " << error << '\n';
            return 1;
        }
        std::cout << "BOOK " << argv[file] << " title=" << document.metadata().title
                  << " spine=" << document.spine().size() << " toc=" << document.toc().size()
                  << " sha256=" << document.metadata().book_id
                  << " partial_md5=" << document.metadata().partial_md5 << '\n';
        for (const int size : {28, 38, 56}) {
            std::size_t pages = 0, scalars = 0;
            std::set<char32_t> missing;
            for (std::size_t index = 0; index < document.spine().size(); ++index) {
                Chapter chapter;
                if (!document.load_chapter(index, chapter, error)) {
                    std::cerr << "Chapter " << index << ": " << error << '\n';
                    return 1;
                }
                if (size == 38) for (std::size_t image = 0; image < chapter.images.size(); ++image) {
                    std::vector<std::uint8_t> encoded;
                    graphics::Canvas canvas;
                    if (!document.read_image(chapter.images[image].resource, encoded, error) ||
                        !graphics::render_image(canvas, encoded, 36, 64, 612, 1064, error)) {
                        std::cerr << "Image " << index << ':' << image << ": " << error << '\n';
                        return 1;
                    }
                    const auto path = ".local/public-books/rendered-" + std::to_string(file - 2) + "-" + std::to_string(index) + "-" + std::to_string(image) + ".pgm";
                    std::ofstream pixels(path, std::ios::binary);
                    pixels << "P5\n684 1216\n255\n";
                    for (int y = 0; y < 1216; ++y) for (int x = 0; x < 684; ++x) pixels.put(static_cast<char>(canvas.pixel(x, y) * 17));
                    if (!pixels.good()) return 1;
                    std::cout << "IMAGE resource=" << chapter.images[image].resource << " output=" << path << '\n';
                }
                for (const auto &run : chapter.runs) {
                    for (auto offset = run.begin; offset < run.end; ++offset) {
                        Anchor anchor;
                        std::uint32_t restored = 0;
                        const auto expected = run.element_reference ? run.begin : offset;
                        if (!chapter.anchor_at(offset, anchor) ||
                            !chapter.offset_for(anchor, restored) || restored != expected) {
                            std::cerr << "Source round-trip failed at " << index << ':' << offset << '\n';
                            return 1;
                        }
                    }
                }
                graphics::LayoutSpec spec;
                spec.font_px = size;
                font.set_pixel_size(size);
                const auto metrics = font.metrics();
                spec.line_height = std::max(size * 3 / 2, metrics.ascender + metrics.descender);
                std::size_t start = 0, count = 0;
                while (start < chapter.text.size()) {
                    const auto page = graphics::layout_page(font, chapter.text, start, spec);
                    if (!page.error.empty() || page.end <= start) {
                        std::cerr << "Layout " << index << ':' << start << ": " << page.error << '\n';
                        return 1;
                    }
                    Anchor anchor;
                    auto first = start;
                    while (first < page.end && !chapter.anchor_at(first, anchor))
                        ++first;
                    std::uint32_t restored = 0;
                    if (first == page.end || !chapter.offset_for(anchor, restored) || restored > first) {
                        std::cerr << "Page has no recoverable source at " << index << ':' << start << '\n';
                        return 1;
                    }
                    graphics::Canvas canvas;
                    if (!graphics::render_page(canvas, font, page)) {
                        std::cerr << "Render: " << font.error() << '\n';
                        return 1;
                    }
                    for (const auto &glyph : page.glyphs)
                        if (!font.contains(glyph.scalar))
                            missing.insert(glyph.scalar);
                    start = page.end;
                    ++count;
                }
                std::cout << "CHAPTER font=" << size << " index=" << index
                          << " chars=" << chapter.text.size() << " pages=" << count
                          << " nodes=" << chapter.source_nodes.size() << " runs=" << chapter.runs.size()
                          << " plain=" << chapter.plain_dom << " resource=" << chapter.resource << '\n';
                for (const auto &warning : chapter.warnings)
                    std::cout << "WARNING " << warning << '\n';
                scalars += chapter.text.size();
                pages += count;
            }
            std::cout << "TOTAL font=" << size << " chars=" << scalars << " pages=" << pages
                      << " missing_glyphs=" << missing.size() << '\n';
            for (const auto scalar : missing)
                std::cout << "MISSING U+" << std::hex << static_cast<unsigned>(scalar) << std::dec << '\n';
        }
    }
}
