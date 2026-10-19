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
    const auto fallback_text = decode_utf8("未检测到存储卡，请插入后重新启动。初始化失败，重试。正在准备书籍章节图片…取消。内容任务无法启动。内存不足。");
    for (const auto c : fallback_text) assert(font.contains(c));
    // The built-in subset holds common body characters; a rare one is absent and draws as the placeholder box.
    for (const auto c : decode_utf8("我们在黄昏时读完了这本书，“窗外”下起雨来——他说：好！①")) assert(font.contains(c));
    {
        Canvas missing, other, space;
        assert(!font.contains(U'龘') && font.size(28) && font.draw(missing, U'龘', 40, 60));
        assert(missing.pixels() != Canvas().pixels());
        // Every missing character draws the same hollow box inside its advance and shares one cache entry.
        const auto cached = font.cached_glyphs();
        assert(!font.contains(U'歡') && font.advance(U'歡') == 28 && font.advance(U'龘') == 28);
        assert(font.draw(other, U'歡', 40, 60) && other.pixels() == missing.pixels() && font.cached_glyphs() == cached);
        // A wide character takes a full em and a 24 x 22 box; the box sits one tenth of an em below the baseline.
        assert(missing.pixel(42, 50) == 1 && missing.pixel(65, 50) == 1 && missing.pixel(41, 50) == 15 && missing.pixel(66, 50) == 15);
        assert(missing.pixel(50, 40) == 1 && missing.pixel(50, 61) == 1 && missing.pixel(50, 39) == 15 && missing.pixel(50, 62) == 15);
        assert(missing.pixel(50, 50) == 15);
        // A narrow character takes half an em and a narrower box of the same height.
        Canvas narrow;
        assert(!font.contains(U'Ж') && font.advance(U'Ж') == 14 && font.draw(narrow, U'Ж', 40, 60));
        assert(narrow.pixel(42, 50) == 1 && narrow.pixel(51, 50) == 1 && narrow.pixel(52, 50) == 15 && narrow.pixel(46, 50) == 15);
        assert(font.cached_glyphs() == cached + 1);
        // A zero-width character the face lacks takes no room and leaves the canvas untouched.
        assert(!font.contains(U'\u200b') && font.advance(U'\u200b') == 0);
        assert(font.missing(U'龘') && font.missing(U'Ж') && !font.missing(U'\u200b') && !font.missing(U'我'));
        // A missing separator or space is not known to be invisible, so it draws the box and cannot join its neighbours.
        assert(font.missing(U'\u2028') && font.advance(U'\u2028') == 14 && font.missing(U'\u0085'));
        // Default-ignorable format characters outside the common ranges are blank as well.
        for (const char32_t ignorable : {U'\u034f', U'\u2066', U'\u2069', U'\U000e0001'})
            assert(!font.missing(ignorable) && font.advance(ignorable) == 0);
        assert(font.draw(space, U'\u200b', 40, 60) && space.pixels() == Canvas().pixels());
    }
    {
        // Prewarming an independent font face must preserve the rasterized pixels.
        Canvas rasterized, cached;
        assert(font.size(28) && font.draw(rasterized, fallback_text[0], 40, 60));
        Font warmed;
        assert(warmed.open_fallback() && warmed.size(28) && warmed.warm(fallback_text[0]));
        assert(warmed.draw(cached, fallback_text[0], 40, 60));
        assert(rasterized.pixels() == cached.pixels() && rasterized.pixels() != Canvas().pixels());
    }
    {
        Canvas regular, medium, bold, restored;
        assert(font.size(38));
        const auto width = font.advance(U'读');
        assert(font.draw(regular, U'读', 40, 70));
        font.weight(1); assert(font.warm(U'读') && font.draw(medium, U'读', 40, 70));
        font.weight(2); assert(font.draw(bold, U'读', 40, 70));
        font.weight(0); assert(font.draw(restored, U'读', 40, 70));
        assert(regular.pixels() != medium.pixels() && medium.pixels() != bold.pixels());
        assert(restored.pixels() == regular.pixels() && font.advance(U'读') == width);
    }
    if (argc > 1) assert(font.open(argv[1]));

    Canvas canvas;
    std::string image_error;
    std::vector<std::uint8_t> rgba(std::begin(kRgbaPng), std::end(kRgbaPng));
    {
        std::string layer;
        bool alpha = false;
        assert(decode_png_layer(rgba, 4, 8, layer, alpha, image_error) && alpha && layer.size() == 64);
        Canvas composited(4, 8); composited.clear(7);
        assert(composite_image_layer(composited, layer, 0, 4));
        assert(composited.pixel(0, 0) == 7 && composited.pixel(0, 2) == 0 && composited.pixel(3, 2) == 15);
        assert(composited.pixel(0, 5) == 7);
        assert(composite_image_layer(composited, layer, 4, 4));
        assert(composited.pixel(0, 5) == 5 && composited.pixel(3, 5) == 7 && composited.pixel(0, 7) == 7);
        const auto original = composited.pixels();
        assert(!composite_image_layer(composited, layer, 7, 2) && composited.pixels() == original);
        std::string half_alpha(2, '\0'); half_alpha[1] = static_cast<char>(128);
        Canvas half(1, 1);
        assert(composite_image_layer(half, half_alpha, 0, 1) && half.pixel(0, 0) == 7);
        auto broken = rgba; broken.back() ^= 1;
        assert(!decode_png_layer(broken, 4, 8, layer, alpha, image_error) && layer.empty());
        assert(!decode_png_layer(rgba, 4, 8, layer, alpha, image_error, [] { return true; }) && layer.empty());
        assert(!decode_png_layer(rgba, 685, 8, layer, alpha, image_error));
    }
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
    std::vector<std::uint8_t> uniform(std::begin(kUniformPng), std::end(kUniformPng));
    assert(render_image(canvas, uniform, 0, 0, 8, 8, image_error));
    for (int y = 0; y < 8; ++y) for (int x = 0; x < 8; ++x) assert(canvas.pixel(x, y) == 0);
    const auto jpeg_unchanged = canvas.pixels();
    unsigned cancellation_checks = 0;
    assert(!render_image(canvas, jpeg, 0, 0, 32, 24, image_error, false,
                         [&] { return ++cancellation_checks >= 3; }));
    assert(image_error == kImageCancelled && canvas.pixels() == jpeg_unchanged);
    // Cancel after output starts; the remaining rows must retain their previous pixels.
    Canvas partial;
    partial.clear(7);
    assert(!render_image(partial, rgba, 10, 20, 4, 4, image_error, false,
                         [&] { return partial.pixel(10, 20) != 7; }));
    assert(image_error == kImageCancelled && partial.pixel(10, 20) == 0 && partial.pixel(10, 21) == 7);
    cancellation_checks = 0;
    assert(!render_image(canvas, rgba, 0, 0, 4, 4, image_error, false,
                         [&] { return ++cancellation_checks >= 3; }));
    assert(image_error == kImageCancelled && canvas.pixels() == jpeg_unchanged);
    assert(!render_image(canvas, bad, 0, 0, 32, 24, image_error) && canvas.pixels() == jpeg_unchanged);
    for (const auto& progressive : {
            std::vector<std::uint8_t>(std::begin(kProgressiveJpeg), std::end(kProgressiveJpeg)),
            std::vector<std::uint8_t>(std::begin(kProgressiveGrayJpeg), std::end(kProgressiveGrayJpeg))}) {
        assert(render_image(canvas, progressive, 0, 0, 32, 32, image_error));
        assert(canvas.pixel(4, 4) >= 4 && canvas.pixel(4, 4) <= 5);
        assert(canvas.pixel(28, 4) >= 8 && canvas.pixel(28, 4) <= 9);
        assert(canvas.pixel(4, 28) >= 1 && canvas.pixel(4, 28) <= 2);
        assert(canvas.pixel(28, 28) >= 14);
        const auto before = canvas.pixels();
        for (const unsigned stop_at : {1U, 3U, 6U, 12U}) {
            unsigned checks = 0;
            assert(!render_image(canvas, progressive, 0, 0, 32, 32, image_error, false,
                                 [&] { return ++checks >= stop_at; }));
            assert(image_error == kImageCancelled && canvas.pixels() == before);
        }
        auto invalid = progressive;
        for (std::size_t i = 0; i + 9 < invalid.size(); ++i) if (invalid[i] == 0xff && invalid[i + 1] == 0xc2) {
            invalid[i + 7] = 0x10; invalid[i + 8] = 1;
            break;
        }
        assert(!render_image(canvas, invalid, 0, 0, 32, 32, image_error) && canvas.pixels() == before);
        invalid = progressive;
        for (std::size_t i = 0; i + 2 < invalid.size(); ++i) if (invalid[i] == 0xff && invalid[i + 1] == 0xda) {
            invalid.resize(i + 2);
            break;
        }
        assert(!render_image(canvas, invalid, 0, 0, 32, 32, image_error) && canvas.pixels() == before);
    }
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
    const auto retained_glyphs = font.cached_glyphs();
    font.trim_glyph_cache(0, 32);
    assert(retained_glyphs - font.cached_glyphs() <= 32 && font.cached_glyphs() < retained_glyphs);
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
            const auto counted = layout_page(font, text, page.start, spec, false);
            assert(counted.error == page.error && counted.start == page.start && counted.end == page.end);
            assert(counted.glyphs.empty() && counted.lines.empty());
            const auto cancelled = layout_page(font, text, page.start, spec, false, [] { return true; });
            assert(cancelled.error == "Layout cancelled" && cancelled.end == cancelled.start);
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
