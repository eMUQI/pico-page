#include "pico_page/graphics.h"
extern "C" {
#include "qrcodegen.h"
}
#include "image_fixtures.h"

#include <cassert>
#include <fstream>
#include <iostream>

using namespace pico_page::graphics;

int main(int argc, char** argv) {
    Font font;
    assert(font.open_fallback());
    const auto fallback_text = decode_utf8("未检测到存储卡，请插入后重新启动。初始化失败，重试。");
    for (const auto c : fallback_text) assert(font.contains(c));
    {
        // Prewarming an independent font face must preserve the rasterized pixels.
        Canvas rasterized, cached;
        assert(font.size(28) && font.draw(rasterized, fallback_text[0], 40, 60));
        Font warmed;
        assert(warmed.open_fallback() && warmed.size(28) && warmed.warm(fallback_text[0]));
        assert(warmed.draw(cached, fallback_text[0], 40, 60));
        assert(rasterized.pixels() == cached.pixels() && rasterized.pixels() != Canvas().pixels());
    }
    if (argc > 1) assert(font.open(argv[1]));

    Canvas canvas;
    std::string image_error;
    std::vector<std::uint8_t> rgba(std::begin(kRgbaPng), std::end(kRgbaPng));
    canvas.clear(7);
    assert(render_image(canvas, rgba, 10, 20, 4, 4, image_error));
    assert(canvas.pixel(10, 20) == 0 && canvas.pixel(13, 20) == 15);
    assert(canvas.pixel(10, 23) == 5 && canvas.pixel(13, 23) == 15);
    assert(canvas.pixel(9, 20) == 7 && canvas.pixel(14, 20) == 7);
    canvas.clear(7);
    assert(render_image(canvas, rgba, 10, 20, 2, 4, image_error, true));
    assert(canvas.pixel(10, 20) == 0 && canvas.pixel(11, 20) == 15);
    assert(canvas.pixel(10, 23) == 5 && canvas.pixel(11, 23) == 15);
    assert(canvas.pixel(9, 20) == 7 && canvas.pixel(12, 20) == 7);
    std::vector<std::uint8_t> adam7(std::begin(kAdam7Png), std::end(kAdam7Png));
    assert(render_image(canvas, adam7, 10, 20, 4, 4, image_error));
    assert(canvas.pixel(10, 20) == 0 && canvas.pixel(13, 20) == 15);
    assert(canvas.pixel(10, 23) == 5 && canvas.pixel(13, 23) == 15);
    std::vector<std::uint8_t> gray16(std::begin(kGray16Png), std::end(kGray16Png));
    assert(render_image(canvas, gray16, 10, 20, 4, 4, image_error));
    assert(canvas.pixel(10, 20) == 0 && canvas.pixel(13, 20) == 15);
    assert(canvas.pixel(10, 23) == 15 && canvas.pixel(13, 23) == 0);
    const auto unchanged = canvas.pixels();
    auto bad = rgba; bad.resize(bad.size() - 5);
    assert(!render_image(canvas, bad, 0, 0, 100, 100, image_error) && canvas.pixels() == unchanged);
    bad = rgba; bad[16] = 1;
    assert(!render_image(canvas, bad, 0, 0, 100, 100, image_error) && canvas.pixels() == unchanged);
    bad = rgba; bad[bad.size() - 1] ^= 1;
    assert(!render_image(canvas, bad, 0, 0, 100, 100, image_error) && canvas.pixels() == unchanged);
    std::vector<std::uint8_t> jpeg(std::begin(kQuadrantsJpeg), std::end(kQuadrantsJpeg));
    assert(render_image(canvas, jpeg, 0, 0, 32, 24, image_error));
    assert(canvas.pixel(4, 4) <= 1 && canvas.pixel(28, 4) >= 14);
    assert(canvas.pixel(4, 20) >= 14 && canvas.pixel(28, 20) <= 1);
    canvas.clear(7);
    assert(render_image(canvas, jpeg, 10, 20, 8, 24, image_error, true));
    assert(canvas.pixel(10, 20) <= 1 && canvas.pixel(17, 20) >= 14);
    assert(canvas.pixel(10, 43) >= 14 && canvas.pixel(17, 43) <= 1);
    assert(canvas.pixel(9, 20) == 7 && canvas.pixel(18, 43) == 7);
    bad = jpeg; bad.resize(bad.size() / 2);
    const auto jpeg_unchanged = canvas.pixels();
    assert(!render_image(canvas, bad, 0, 0, 32, 24, image_error) && canvas.pixels() == jpeg_unchanged);
    canvas.pixel(0, 0, 1);
    canvas.pixel(1, 0, 3);
    assert(canvas.data()[0] == 0x31);
    assert(canvas.pixels().size() == 415872);
    canvas.clear();
    assert(font.size(28));
    assert(font.draw(canvas, U'中', 40, 80));
    const auto regular = canvas.pixels();
    canvas.clear();
    assert(font.draw(canvas, U'中', 40, 80, 1, true));
    const auto bold = canvas.pixels();
    assert(bold != regular);
    canvas.clear();
    assert(font.draw(canvas, U'中', 40, 80));
    assert(canvas.pixels() == regular);
    canvas.clear();
    assert(font.draw(canvas, U'中', 40, 80, 1, true));
    assert(canvas.pixels() == bold);
    const auto previous_advance = font.advance(U'中');
    for (char32_t cp = 0x4e00; cp < 0x4e00 + 9000; ++cp) assert(font.advance(cp) >= 0);
    assert(font.advance(U'中') == previous_advance);
    assert(font.size(64));
    for (char32_t cp = 0x4e00; cp < 0x4e00 + 1024; ++cp) assert(font.warm(cp));
    assert(font.size(28));
    canvas.clear();
    assert(font.draw(canvas, U'中', 40, 80));
    assert(canvas.pixels() == regular);
    canvas.clear();
    std::u32string text;
    const auto paragraph = decode_utf8("中文阅读：完整行分页。中文﹐中文﹒中文﹖中文﹗“中文”。English words stay together where possible.\n");
    for (int i = 0; i < 50; ++i) text += paragraph;
    text += U"\U00020000\U0001f642结尾";
    assert(decode_utf8("𠀀🙂") == U"\U00020000\U0001f642");
    assert(decode_utf8("\xed\xa0\x80")[0] == U'\ufffd');

    for (const int pixels : {28, 38, 56}) {
        LayoutSpec spec;
        spec.font_px = pixels;
        spec.line_height = pixels * 3 / 2;
        std::size_t cursor = 0;
        std::size_t drawn = 0;
        while (cursor < text.size()) {
            auto page = layout_page(font, text, cursor, spec);
            if (!page.error.empty()) std::cerr << page.error << '\n';
            assert(page.error.empty());
            assert(page.start == cursor && page.end > cursor && page.end <= text.size());
            for (const auto& line : page.lines) {
                assert(line.top >= spec.top && line.bottom <= spec.top + spec.height);
                assert(line.baseline - font.metrics().ascender >= line.top);
                assert(line.baseline + font.metrics().descender <= line.bottom);
                assert(std::u32string_view(U"﹐﹒﹖﹗”").find(text[line.start]) == std::u32string_view::npos);
                assert(text[line.end - 1] != U'“');
            }
            std::size_t next = cursor;
            for (const auto& glyph : page.glyphs) {
                while (next < page.end && (text[next] == U'\n' || text[next] == U'\r')) ++next;
                assert(glyph.index == next && glyph.scalar == text[next]);
                assert(glyph.x >= spec.left);
                assert(glyph.x + font.advance(glyph.scalar) <= spec.left + spec.width);
                ++next;
                ++drawn;
            }
            while (next < page.end && (text[next] == U'\n' || text[next] == U'\r')) ++next;
            assert(next == page.end);
            assert(render_page(canvas, font, page));
            cursor = page.end;
        }
        assert(drawn == text.size() - 50);
        const auto repaginated = layout_page(font, text, 113, spec);
        assert(repaginated.start == 113 && repaginated.glyphs.front().index == 113);
        const auto indented = layout_page(font, U"　　中文段落", 0, spec);
        assert(indented.error.empty() && indented.glyphs.front().x == spec.left);
    }
    {
        // Every module lands where the encoder put it: 264 px holds version 2 at 8 px a module.
        const std::string address = "http://192.168.4.1";
        std::vector<std::uint8_t> work(qrcodegen_BUFFER_LEN_FOR_VERSION(10)), code(work.size());
        assert(qrcodegen_encodeText(address.c_str(), work.data(), code.data(), qrcodegen_Ecc_MEDIUM, 1, 10,
                                    qrcodegen_Mask_AUTO, false));
        const int modules = qrcodegen_getSize(code.data());
        assert(modules == 25);
        Canvas qr;
        qr.fill_rect(0, 0, 684, 1216, 7);
        assert(draw_qr(qr, address, 36, 520, 264));
        const int origin = (264 - modules * 8) / 2;
        for (int row = -4; row < modules + 4; ++row)
            for (int column = -4; column < modules + 4; ++column) {
                const bool dark = row >= 0 && column >= 0 && row < modules && column < modules &&
                                  qrcodegen_getModule(code.data(), column, row);
                for (const int inset : {0, 7})
                    assert(qr.pixel(36 + origin + column * 8 + inset, 520 + origin + row * 8 + inset) == (dark ? 0 : 15));
            }
        assert(qr.pixel(35, 520) == 7 && qr.pixel(300, 783) == 7 && qr.pixel(36, 520) == 15 && qr.pixel(299, 783) == 15);
        const auto before = qr.pixels();
        assert(!draw_qr(qr, std::string(300, 'x'), 36, 520, 264) && !draw_qr(qr, address, 36, 520, 90));
        assert(qr.pixels() == before);
    }
    auto too_short = LayoutSpec{};
    too_short.line_height = 1;
    const auto rejected = layout_page(font, text, 7, too_short);
    assert(!rejected.error.empty() && rejected.end == 7 && rejected.glyphs.empty());
    std::cout << "Packed pixels, fallback coverage and scalar-continuous full-line pagination passed\n";
}
