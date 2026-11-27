#include "pico_page/graphics.h"
#include "../firmware/components/reader_app/font_error_message.h"
extern "C" {
#include "qrcodegen.h"
}
#include "image_fixtures.h"

#include <algorithm>
#include <cassert>
#include <fstream>
#include <iostream>
#include <iterator>
#include <utility>

using namespace pico_page::graphics;

int main(int argc, char** argv) {
    Font font;
    assert(font.open_fallback());
    const auto fallback_text = decode_utf8("未检测到存储卡，请插入后重新启动。初始化失败，重试。正在准备书籍章节图片…取消。内容任务无法启动。内存不足。");
    for (const auto c : fallback_text) assert(font.contains(c));
    for (const auto c : decode_utf8("我们在黄昏时读完了这本书，“窗外”下起雨来——他说：好！①")) assert(font.contains(c));
    {
        Canvas missing, other, space;
        assert(!font.contains(U'龘') && font.set_pixel_size(28) && font.draw(missing, U'龘', 40, 60));
        assert(missing.pixels() != Canvas().pixels());
        const auto cached = font.cached_glyphs();
        assert(!font.contains(U'鱻') && font.advance(U'鱻') == 28 && font.advance(U'龘') == 28);
        assert(font.draw(other, U'鱻', 40, 60) && other.pixels() == missing.pixels() && font.cached_glyphs() == cached);
        assert(missing.pixel(42, 50) == 1 && missing.pixel(65, 50) == 1 && missing.pixel(41, 50) == 15 && missing.pixel(66, 50) == 15);
        assert(missing.pixel(50, 40) == 1 && missing.pixel(50, 61) == 1 && missing.pixel(50, 39) == 15 && missing.pixel(50, 62) == 15);
        assert(missing.pixel(50, 50) == 15);
        Canvas narrow;
        assert(!font.contains(U'Ж') && font.advance(U'Ж') == 14 && font.draw(narrow, U'Ж', 40, 60));
        assert(narrow.pixel(42, 50) == 1 && narrow.pixel(51, 50) == 1 && narrow.pixel(52, 50) == 15 && narrow.pixel(46, 50) == 15);
        assert(font.cached_glyphs() == cached + 1);
        assert(!font.contains(U'\u200b') && font.advance(U'\u200b') == 0);
        assert(font.missing(U'龘') && font.missing(U'Ж') && !font.missing(U'\u200b') && !font.missing(U'我'));
        assert(font.missing(U'\u2028') && font.advance(U'\u2028') == 14 && font.missing(U'\u0085'));
        for (const char32_t ignorable : {U'\u034f', U'\u2066', U'\u2069', U'\U000e0001'})
            assert(!font.missing(ignorable) && font.advance(ignorable) == 0);
        assert(font.draw(space, U'\u200b', 40, 60) && space.pixels() == Canvas().pixels());
    }
    {
        Canvas clipped(19, 21), full(80, 80);
        constexpr int crop_x = 31, crop_y = 23;
        constexpr std::uint8_t background = 7;
        clipped.clear(background);
        full.clear(background);
        assert(font.set_pixel_size(28));
        const auto draw = [&](char32_t scalar, int x, int baseline, std::uint8_t gray) {
            assert(font.draw(clipped, scalar, x, baseline, gray));
            assert(font.draw(full, scalar, x + crop_x, baseline + crop_y, gray));
        };
        draw(U'龘', -5, 16, 2);
        assert(clipped.pixel(0, 17) == 2 && clipped.pixel(3, 3) == background);
        draw(U'我', -5, 16, 0);
        draw(U'读', 5, 25, 14);

        for (int y = 0; y < clipped.height(); ++y) {
            for (int x = 0; x < clipped.width(); ++x)
                assert(clipped.pixel(x, y) == full.pixel(x + crop_x, y + crop_y));
            const auto last_byte = static_cast<std::size_t>(y + 1) * clipped.stride() - 1;
            assert((clipped.pixels()[last_byte] >> 4) == background);
        }
    }
    {
        Canvas rasterized, cached;
        assert(font.set_pixel_size(28) && font.draw(rasterized, fallback_text[0], 40, 60));
        Font warmed;
        assert(warmed.open_fallback() && warmed.set_pixel_size(28) && warmed.warm(fallback_text[0]));
        assert(warmed.draw(cached, fallback_text[0], 40, 60));
        assert(rasterized.pixels() == cached.pixels() && rasterized.pixels() != Canvas().pixels());
    }
    {
        Canvas regular, medium, bold, restored;
        assert(font.set_pixel_size(38));
        const auto width = font.advance(U'读');
        assert(font.draw(regular, U'读', 40, 70));
        font.set_synthetic_weight(1); assert(font.warm(U'读') && font.draw(medium, U'读', 40, 70));
        font.set_synthetic_weight(2); assert(font.draw(bold, U'读', 40, 70));
        font.set_synthetic_weight(0); assert(font.draw(restored, U'读', 40, 70));
        assert(regular.pixels() != medium.pixels() && medium.pixels() != bold.pixels());
        assert(restored.pixels() == regular.pixels() && font.advance(U'读') == width);
    }
    {
        AuthorFonts embedded;
        embedded.faces.push_back(std::make_unique<Font>());
        assert(embedded.faces.front()->open_fallback());
        Page page;
        page.font_px = 38;
        page.glyphs = {{U'读', 0, 10, 60, 38}, {U'读', 1, 60, 60, 38, true},
                       {U'读', 2, 110, 60, 38, false, 0, false, false, 1}};
        const auto draw = [&](int weight_adjustment) {
            Canvas canvas(150, 80);
            page.weight_adjustment = weight_adjustment;
            assert(render_page(canvas, font, page, 1, {}, &embedded));
            return canvas;
        };
        const auto ink = [](const Canvas& canvas, int left) {
            int darkness = 0;
            for (int y = 0; y < canvas.height(); ++y)
                for (int x = left; x < left + 50; ++x) darkness += 15 - canvas.pixel(x, y);
            return darkness;
        };
        const auto original = draw(0), lighter = draw(-1), heavier = draw(1);
        for (int left : {0, 50, 100})
            assert(0 < ink(lighter, left) && ink(lighter, left) < ink(original, left) &&
                   ink(original, left) < ink(heavier, left));
        assert(ink(lighter, 0) < ink(lighter, 50) && ink(heavier, 0) < ink(heavier, 50));
        assert(draw(0).pixels() == original.pixels() && draw(-1).pixels() == lighter.pixels());
    }
    for (bool bold : {false, true}) {
        Canvas measured(96, 96);
        int above = 0, rows = 0;
        assert(font.set_pixel_size(28) && font.ink_rows(U'中', above, rows, bold));
        assert(font.draw(measured, U'中', 30, 60, 1, bold));
        int first = measured.height(), last = -1;
        for (int y = 0; y < measured.height(); ++y)
            for (int x = 0; x < measured.width(); ++x)
                if (measured.pixel(x, y) < 15) { first = std::min(first, y); last = std::max(last, y); }
        assert(last >= first && first >= 60 - above && last < 60 - above + rows);
    }
    {
        Font small(2048, 4);
        assert(small.open_fallback() && small.set_pixel_size(28));
        assert(font.set_pixel_size(28));
        Canvas expected, actual;
        int x = 20;
        for (const auto c : decode_utf8("读书学习天地人文读")) {
            assert(small.draw(actual, c, x, 60) && font.draw(expected, c, x, 60));
            assert(small.advance(c) == font.advance(c));
            assert(small.cached_glyph_bytes() <= 2048 && small.cached_advances() <= 4);
            x += small.advance(c);
        }
        assert(actual.pixels() == expected.pixels());
        small.close();
        assert(!small.valid() && !small.cached_glyphs() && !small.cached_advances() && !small.cached_glyph_bytes());
        assert(small.open_fallback() && small.set_pixel_size(28) && small.warm(U'读'));
        Font insufficient(1, 1);
        assert(insufficient.open_fallback() && !insufficient.warm(U'读'));
        assert(!insufficient.cached_glyph_bytes() && !insufficient.cached_glyphs());
        assert(insufficient.error() == "Font glyph exceeds cache budget [U+8BFB]");
        assert(pico_page::font_error_message(insufficient.error()) == "单个字形超过缓存上限（F10） [U+8BFB]");
        Font unavailable;
        assert(!unavailable.warm(U'读'));
        assert(pico_page::font_error_message(unavailable.error()) == "正文字体未就绪（F11） [U+8BFB]");
        assert(!unavailable.set_pixel_size(28));
        assert(pico_page::font_error_message(unavailable.error()) == "正文字体未就绪（F11）");
        assert(unavailable.open_fallback() && !unavailable.set_pixel_size(257));
        assert(pico_page::font_error_message(unavailable.error()) == "字体字号设置失败（F04）");
        assert(unavailable.set_pixel_size(28) && unavailable.error().empty());
        const unsigned char invalid_font[] = {0, 1, 2, 3};
        assert(!unavailable.open_memory(invalid_font, sizeof(invalid_font)));
        assert(unavailable.error().find("Cannot open embedded font [FT=") == 0);
        assert(pico_page::font_error_message(unavailable.error()).find("内置字体打开失败（F02） [FT=") == 0);
        assert(unavailable.open_fallback() && unavailable.error().empty());
        assert(!unavailable.open("/pico-page-missing-font.ttf"));
        assert(unavailable.error().find("Cannot open font [FT=") == 0);
        assert(unavailable.error().find("; path=/pico-page-missing-font.ttf") != std::string::npos);
        assert(pico_page::font_error_message(unavailable.error()).find("字体文件打开失败（F02） [FT=") == 0);
        assert(pico_page::font_error_message("Cannot open font [FT=1]; path=FreeType out of memory [U+FFFF].ttf") ==
               "字体文件打开失败（F02） [FT=1]");
        assert(pico_page::font_error_message("Cannot read chapter").empty());
        const std::pair<const char*, const char*> failures[] = {
            {"FreeType initialization failed", "字体引擎初始化失败（F01）"},
            {"Cannot open font", "字体文件打开失败（F02）"},
            {"Font has no Unicode character map", "字体字符表不可用（F03）"},
            {"Font glyph metrics failed", "字形尺寸读取失败（F05）"},
            {"Font glyph load failed", "字形读取失败（F06）"},
            {"Font stroke adjustment failed", "字形加粗处理失败（F07）"},
            {"Font glyph rasterization failed", "字形绘制失败（F08）"},
            {"Unsupported glyph pixel format", "字形像素格式不支持（F09）"},
        };
        for (const auto& failure : failures) {
            const auto message = pico_page::font_error_message(std::string(failure.first) + " [FT=6] [U+1F600]");
            assert(message == std::string(failure.second) + " [FT=6] [U+1F600]");
            for (const auto scalar : decode_utf8(message)) assert(font.contains(scalar));
        }
        const auto memory = pico_page::font_error_message("Font glyph rasterization failed: FreeType out of memory [FT=64] [U+8BFB]");
        assert(memory == "字体处理内存不足（F12） [FT=64] [U+8BFB]");
        for (const auto scalar : decode_utf8(memory)) assert(font.contains(scalar));
    }
    if (argc > 1) {
        std::ifstream file(argv[1], std::ios::binary);
        const std::vector<unsigned char> bytes{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
        Font memory;
        assert(font.open(argv[1]) && memory.open_memory(bytes.data(), bytes.size()));
        for (const int pixels : {28, 38, 28}) {
            assert(font.set_pixel_size(pixels) && memory.set_pixel_size(pixels));
            Canvas from_file(600, 100), from_memory(600, 100);
            int x = 8;
            for (const auto scalar : decode_utf8("阅读、落叶与风。Aa 龘天地书！读")) {
                const auto advance = font.advance(scalar);
                assert(advance >= 0 && advance == memory.advance(scalar));
                assert(font.draw(from_file, scalar, x, 64) && memory.draw(from_memory, scalar, x, 64));
                x += advance;
            }
            assert(from_file.pixels() == from_memory.pixels());
        }
        assert(font.open(argv[1]));
    }

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
        Canvas streamed(4, 8); streamed.clear(7);
        for (int y = 0; y < 8; ++y)
            assert(composite_image_rows(streamed, std::string_view(layer).substr(y * 8, 8), y, 1));
        assert(streamed.pixels() == composited.pixels());
        const auto streamed_pixels = streamed.pixels();
        assert(!composite_image_rows(streamed, std::string_view(layer).substr(0, 7), 2, 1));
        assert(!composite_image_rows(streamed, std::string_view(layer).substr(0, 8), 8, 1));
        assert(streamed.pixels() == streamed_pixels);
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
    {
        const std::vector<std::uint8_t> offset(std::begin(kOffsetGrayGif), std::end(kOffsetGrayGif));
        Canvas background(8, 8);
        assert(render_image(background, offset, 0, 0, 8, 8, image_error));
        for (int y = 0; y < 8; ++y) for (int x = 0; x < 8; ++x) assert(background.pixel(x, y) == 8);
        const std::vector<std::uint8_t> gif(std::begin(kPaletteGif), std::end(kPaletteGif));
        const std::vector<std::uint8_t> interlaced(std::begin(kInterlacedGif), std::end(kInterlacedGif));
        Canvas regular(16, 16), progressive(16, 16);
        assert(render_image(regular, gif, 0, 0, 16, 16, image_error));
        assert(render_image(progressive, interlaced, 0, 0, 16, 16, image_error));
        assert(regular.pixels() == progressive.pixels());
        for (int y = 0; y < 16; ++y) for (int x = 0; x < 16; ++x) {
            const auto palette = (x / 4 + y / 4) % 4;
            assert(regular.pixel(x, y) == (palette == 0 ? 0 : palette == 2 ? 5 : 15));
        }
        for (std::size_t size = 0; size < gif.size(); ++size) {
            const std::vector<std::uint8_t> truncated(gif.begin(), gif.begin() + size);
            Canvas unchanged(16, 16); unchanged.clear(7);
            const auto pixels = unchanged.pixels();
            assert(!render_image(unchanged, truncated, 0, 0, 16, 16, image_error));
            assert(unchanged.pixels() == pixels);
        }
        auto invalid = gif;
        invalid[6] = invalid[7] = 0xff;
        assert(!render_image(regular, invalid, 0, 0, 16, 16, image_error));
        unsigned checks = 0;
        assert(!render_image(regular, gif, 0, 0, 16, 16, image_error, false, [&] { return ++checks >= 5; }));
        assert(image_error == kImageCancelled);
    }
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
    for (const auto& source : {rgba, adam7, std::vector<std::uint8_t>(std::begin(kOpaqueRgbaPng), std::end(kOpaqueRgbaPng)),
                               std::vector<std::uint8_t>(std::begin(kGray16Png), std::end(kGray16Png))}) {
        for (const auto& dimensions : {std::pair{1, 1}, std::pair{3, 7}, std::pair{17, 13}, std::pair{684, 1216}}) {
            const auto w = dimensions.first, h = dimensions.second;
            std::string expected, actual(std::size_t(w) * h * 2, '\0');
            std::vector<unsigned char> visits(std::size_t(w) * h);
            bool old_alpha = false, new_alpha = false;
            ImageBox bounds{};
            assert(decode_png_layer(source, w, h, expected, old_alpha, image_error));
            auto read = [&](std::size_t at, char* bytes, std::size_t size) {
                assert(size <= 4096 && at <= source.size() && size <= source.size() - at);
                std::copy_n(source.data() + at, size, bytes); return true;
            };
            auto paint = [&](int x, int y, int width, int height, std::uint8_t gray, std::uint8_t alpha) {
                for (int py = y; py < y + height; ++py) for (int px = x; px < x + width; ++px) {
                    assert(px >= 0 && px < w && py >= 0 && py < h);
                    const auto at = std::size_t(py) * w + px;
                    assert(++visits[at] == 1);
                    actual[at * 2] = static_cast<char>(gray); actual[at * 2 + 1] = static_cast<char>(alpha);
                }
            };
            assert(decode_png_stream(source.size(), read, w, h, paint, bounds, new_alpha, image_error));
            assert(old_alpha == new_alpha && expected == actual);
            unsigned checks = 0;
            assert(!decode_png_stream(source.size(), read, w, h, paint, bounds, new_alpha, image_error,
                                       [&] { return ++checks == 2; }));
            assert(image_error == kImageCancelled);
            assert(!decode_png_stream(source.size(), [&](std::size_t at, char* bytes, std::size_t size) {
                return at != 8 && read(at, bytes, size);
            }, w, h, paint, bounds, new_alpha, image_error));
            assert(image_error == "PNG read failed");
        }
    }
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
    assert(font.set_pixel_size(28));
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
    assert(font.set_pixel_size(64));
    for (char32_t cp = 0x4e00; cp < 0x4e00 + 1024; ++cp) assert(font.warm(cp));
    const auto retained_glyphs = font.cached_glyphs();
    font.trim_glyph_cache(0, 32);
    assert(retained_glyphs - font.cached_glyphs() <= 32 && font.cached_glyphs() < retained_glyphs);
    assert(font.set_pixel_size(28));
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
                assert(glyph.advance == font.advance(glyph.scalar));
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
        LayoutSpec spec;
        spec.indent_chars = 0; spec.width = 110; spec.height = 300;
        const auto page = layout_page(font, U"甲乙丙丁戊己庚辛", 0, spec);
        assert(page.error.empty() && page.lines.size() >= 2);
        const auto cached = font.cached_advances();
        for (const auto& glyph : page.glyphs) {
            const auto hit = text_hit(page, glyph.x + glyph.advance / 2, glyph.baseline);
            assert(hit && *hit == glyph.index);
        }
        assert(!text_hit(page, spec.left - 1, page.lines.front().baseline));
        assert(!text_hit(page, spec.left, spec.top - 1));
        assert(!text_hit(page, spec.left, page.lines.back().bottom));
        assert(!text_hit(page, spec.left + spec.width + 1, page.lines.front().baseline));
        assert(text_hit(page, spec.left - 20, page.lines[1].baseline, true) == page.lines[1].start);
        assert(text_hit(page, spec.left, spec.top - 20, true) == page.start);
        assert(text_hit(page, 680, page.lines.back().bottom + 20, true) == page.glyphs.back().index);
        auto illustrated = page;
        illustrated.images.push_back({0, spec.left, spec.top, 100, 100, "image.png"});
        assert(!text_hit(illustrated, spec.left + 10, spec.top + 10, true));
        const auto boxes = selection_boxes(page, 1, page.lines[1].end);
        assert(boxes.size() == 2 && boxes[0].x == page.glyphs[1].x);
        assert(boxes[1].x == spec.left && boxes[1].y == page.lines[1].top);
        assert(selection_boxes(page, 5, 3).empty());
        assert(selection_boxes(page, page.end, page.end + 1).empty());
        assert(font.cached_advances() == cached);
        const auto spaced = layout_page(font, U"甲　乙 \ufffc\u00ad", 0, LayoutSpec{});
        for (const auto& glyph : spaced.glyphs)
            if (glyph.scalar == U' ' || glyph.scalar == U'　' || glyph.scalar == U'\ufffc')
                assert(!text_hit(spaced, glyph.x + glyph.advance / 2, glyph.baseline));
        auto failed = page; failed.error = "Layout cancelled";
        assert(!text_hit(failed, page.glyphs[0].x, page.lines[0].baseline));
        assert(selection_boxes(failed, 0, page.end).empty());
    }
    {
        LayoutSpec spec;
        spec.indent_chars = 0;
        assert(font.set_pixel_size(spec.font_px));
        const int em = font.advance(U'甲');
        spec.width = em * 7 / 2;
        const std::u32string text = U"甲乙丙，丁戊\n己庚\n辛";
        const auto ragged = layout_page(font, text, 0, spec);
        spec.justify = true;
        const auto justified = layout_page(font, text, 0, spec);
        assert(justified.error.empty() && justified.end == ragged.end && justified.lines.size() == ragged.lines.size());
        const auto glyph_at = [&](std::size_t index) {
            return *std::find_if(justified.glyphs.begin(), justified.glyphs.end(),
                                 [&](const PositionedGlyph& glyph) { return glyph.index == index; });
        };
        const auto right_edge = [](const PositionedGlyph& glyph) { return glyph.x + glyph.advance; };
        assert(justified.lines[0].end == 2 && right_edge(glyph_at(1)) == spec.left + spec.width);
        assert(right_edge(ragged.glyphs[1]) < spec.left + spec.width);
        assert(justified.lines[1].end == 5 && right_edge(glyph_at(4)) == spec.left + spec.width);
        assert(glyph_at(3).x == right_edge(glyph_at(2)));
        assert(glyph_at(8).x == right_edge(glyph_at(7)));
        const StyledLayout author{0, 0, TextAlign::Justify, [&](std::size_t index) {
            return GlyphStyle{spec.font_px, spec.line_height, false, false, false, false, index == 1};
        }};
        spec.justify = false;
        const auto styled = layout_page(font, U"甲x乙丙，丁", 0, spec, true, {}, &author);
        assert(styled.error.empty() && styled.lines[0].end == 3);
        assert(right_edge(styled.glyphs[1]) == spec.left + spec.width);
        spec.justify = true;
        const std::u32string latin = U"ab cd ef";
        spec.width = 1;
        for (const auto scalar : latin.substr(0, 6)) spec.width += font.advance(scalar);
        const auto words = layout_page(font, latin, 0, spec);
        assert(words.error.empty() && words.lines[0].end == 6);
        const auto d = std::find_if(words.glyphs.begin(), words.glyphs.end(),
                                    [](const PositionedGlyph& glyph) { return glyph.index == 4; });
        assert(right_edge(*d) == spec.left + spec.width);
        const auto at = [](const Page& page, std::size_t index) {
            return *std::find_if(page.glyphs.begin(), page.glyphs.end(),
                                 [&](const PositionedGlyph& glyph) { return glyph.index == index; });
        };
        const std::u32string joined = U"ab cd ef gh";
        spec.width = 1;
        for (const auto i : {0, 1, 3, 4, 5, 6, 7, 8}) spec.width += font.advance(joined[i]);
        const StyledLayout hidden_space{0, 0, TextAlign::Justify, [&](std::size_t index) {
            return GlyphStyle{spec.font_px, spec.line_height, false, false, false, false, index == 2};
        }};
        const auto hidden = layout_page(font, joined, 0, spec, true, {}, &hidden_space);
        assert(hidden.error.empty() && hidden.lines[0].end == 9);
        assert(at(hidden, 3).x == right_edge(at(hidden, 1)) && right_edge(at(hidden, 7)) == spec.left + spec.width);
        spec.width = em * 2 + font.advance(U' ') + em / 2;
        const auto spaced_end = layout_page(font, U"甲乙 丙丁戊", 0, spec);
        assert(spaced_end.error.empty() && spaced_end.lines[0].end == 3);
        assert(right_edge(at(spaced_end, 1)) == spec.left + spec.width);
        const int ideographic_space = font.advance(U'\u3000');
        spec.width = ideographic_space * 2 + em * 2 + em / 2;
        const auto source_indent = layout_page(font, U"　　甲乙丙，丁戊己庚", 0, spec);
        assert(source_indent.error.empty() && source_indent.lines[0].end == 4);
        assert(at(source_indent, 2).x == spec.left + ideographic_space * 2);
        assert(right_edge(at(source_indent, 3)) == spec.left + spec.width);
        spec.width = 612;
        spec.indent_chars = 2;
        const StyledLayout author_indent{0, em, TextAlign::Start, [&](std::size_t) {
            return GlyphStyle{spec.font_px, spec.line_height};
        }};
        const auto original = layout_page(font, U"　　甲乙", 0, spec, true, {}, &author_indent);
        const auto unified = layout_page(font, U"　　甲乙", 0, spec);
        assert(original.error.empty() && original.end == 4 && unified.error.empty() && unified.end == 4);
        assert(at(original, 2).x == spec.left + ideographic_space * 2 + em);
        assert(at(unified, 2).x == spec.left + ideographic_space * 2);
    }
    {
        LayoutSpec spec;
        spec.indent_chars = 0;
        spec.justify = true;
        spec.limit_english_justification = true;
        assert(font.set_pixel_size(spec.font_px));
        const auto width = [&](std::u32string_view word) {
            int result = 0;
            for (const auto scalar : word) result += font.advance(scalar);
            return result;
        };
        const auto space = width(U" ");
        const int content = width(U"ab cd");
        const auto right = [](const Page& page, std::size_t index) {
            const auto glyph = std::find_if(page.glyphs.begin(), page.glyphs.end(),
                [&](const PositionedGlyph& item) { return item.index == index; });
            assert(glyph != page.glyphs.end());
            return glyph->x + glyph->advance;
        };
        spec.width = content + space;
        const auto fitted = layout_page(font, U"ab cd ef", 0, spec);
        assert(fitted.error.empty() && fitted.lines[0].end == 6 && right(fitted, 4) == spec.left + spec.width);
        assert(std::none_of(fitted.glyphs.begin(), fitted.glyphs.end(),
                            [](const PositionedGlyph& glyph) { return glyph.index == 5; }));
        spec.width = content + space + 1;
        const auto wide_gap = layout_page(font, U"ab cd ef", 0, spec);
        assert(wide_gap.error.empty() && wide_gap.lines[0].end == 6 && right(wide_gap, 4) == spec.left + content + space);

        spec.width = width(U"ab cd ef") - 1;
        const auto compressed = layout_page(font, U"ab cd ef gh ij", 0, spec);
        assert(compressed.error.empty() && compressed.lines[0].end == 9);
        assert(right(compressed, 7) == spec.left + spec.width);
        assert(right(compressed, 13) == spec.left + width(U"gh ij"));
        const auto natural_end = layout_page(font, U"ab cd ef", 0, spec);
        assert(natural_end.error.empty() && natural_end.lines.size() == 2 && natural_end.lines[0].end == 6);
        assert(right(natural_end, 7) == spec.left + width(U"ef"));
        const auto forced_break = layout_page(font, U"ab cd ef\nxy", 0, spec);
        assert(forced_break.error.empty() && forced_break.lines.size() == 3 && forced_break.lines[0].end == 6);
        assert(right(forced_break, 7) == spec.left + width(U"ef"));
        const auto counted_compressed = layout_page(font, U"ab cd ef gh ij", 0, spec, false);
        assert(counted_compressed.error.empty() && counted_compressed.end == compressed.end &&
               counted_compressed.next_top == compressed.next_top);

        spec.justify = false;
        const auto hyphen = font.contains(U'-') ? U'-' : U'\u2010';
        const auto generated = [](const Page& page) {
            return std::find_if(page.glyphs.begin(), page.glyphs.end(),
                [](const PositionedGlyph& glyph) { return glyph.generated; });
        };
        spec.width = width(U"coo") + font.advance(hyphen);
        const auto manual = layout_page(font, U"co\u00adoperate", 0, spec);
        const auto mark = generated(manual);
        assert(manual.error.empty() && manual.lines[0].end == 3 && mark != manual.glyphs.end());
        assert(mark->index == 2 && mark->scalar == hyphen);
        assert(!text_hit(manual, mark->x + mark->advance / 2, mark->baseline));
        assert(selection_boxes(manual, 2, 3).empty());
        Canvas canvas(684, 1216);
        assert(render_page(canvas, font, manual));
        const auto counted = layout_page(font, U"co\u00adoperate", 0, spec, false);
        assert(counted.error.empty() && counted.end == manual.end && counted.next_top == manual.next_top);
        spec.hyphenation = Hyphenation::None;
        const auto disabled = layout_page(font, U"co\u00adoperate", 0, spec);
        assert(disabled.error.empty() && generated(disabled) == disabled.glyphs.end());
        spec.width = width(U"aab");
        const auto zero_width = layout_page(font, U"aa\u200bbb", 0, spec);
        assert(zero_width.error.empty() && zero_width.lines[0].end == 3);
        spec.width = width(U"a a\u00a0");
        const auto nonbreaking = layout_page(font, U"a a\u00a0b", 0, spec);
        assert(nonbreaking.error.empty() && nonbreaking.lines[0].end == 2);

        spec.hyphenation = Hyphenation::Auto;
        spec.english = EnglishHyphenation::Gb;
        spec.width = width(U"rec") + font.advance(hyphen);
        const std::u32string word = U"recondite";
        const auto automatic = layout_page(font, word, 0, spec);
        assert(automatic.error.empty() && automatic.lines[0].end == 2 && generated(automatic) != automatic.glyphs.end());
        spec.justify = true;
        spec.width = width(U"ab cd re") + font.advance(hyphen);
        const auto adjusted = layout_page(font, U"ab cd recondite", 0, spec);
        assert(adjusted.error.empty() && adjusted.lines[0].end == 8 && generated(adjusted) != adjusted.glyphs.end());
        assert(generated(adjusted)->x + generated(adjusted)->advance == spec.left + spec.width);
        spec.justify = false;
        spec.width = width(U"rec") + font.advance(hyphen);
        const StyledLayout override{0, 0, TextAlign::Start, [&](std::size_t offset) {
            GlyphStyle style{spec.font_px, spec.line_height};
            style.hyphenation = offset == word.size() - 1 ? Hyphenation::None : Hyphenation::Auto;
            style.english = spec.english;
            return style;
        }};
        const auto mixed_rules = layout_page(font, word, 0, spec, true, {}, &override);
        assert(mixed_rules.error.empty() && generated(mixed_rules) == mixed_rules.glyphs.end());

        spec.width = 612;
        spec.txt_reflow = true;
        const std::vector<std::uint32_t> soft_breaks{6};
        const auto reflowed = layout_page(font, U"a line\r\ncontinued\n\nnew", 0, spec, true, {}, nullptr, nullptr, &soft_breaks);
        assert(reflowed.error.empty() && reflowed.lines[0].end == 18 && reflowed.lines[1].end == 19);
        assert(std::none_of(reflowed.glyphs.begin(), reflowed.glyphs.end(),
                            [](const PositionedGlyph& glyph) { return glyph.index == 6; }));
        assert(std::any_of(reflowed.glyphs.begin(), reflowed.glyphs.end(),
                           [](const PositionedGlyph& glyph) { return glyph.index == 7 && glyph.scalar == U' '; }));
    }
    {
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
