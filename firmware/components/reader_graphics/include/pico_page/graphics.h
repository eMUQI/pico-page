#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace pico_page::graphics {

/** Row-major 4-bit gray pixels: even x in the low nibble, 0 black and 15 white. */
class Canvas {
public:
    explicit Canvas(int width = 684, int height = 1216);
    int width() const { return width_; }
    int height() const { return height_; }
    int stride() const { return (width_ + 1) / 2; }
    std::uint8_t* data() { return pixels_.data(); }
    const std::uint8_t* data() const { return pixels_.data(); }
    const std::vector<std::uint8_t>& pixels() const { return pixels_; }
    void clear(std::uint8_t gray = 15);
    std::uint8_t pixel(int x, int y) const;
    void pixel(int x, int y, std::uint8_t gray);
    void fill_rect(int x, int y, int width, int height, std::uint8_t gray);
    void line(int x0, int y0, int x1, int y1, std::uint8_t gray, int thickness = 1);
private:
    int width_;
    int height_;
    std::vector<std::uint8_t> pixels_;
};

struct FontMetrics {
    int ascender = 0;
    int descender = 0;  // Positive distance below the baseline.
    int height = 0;
};

/** A single-owner font face; callers serialize sizing, measuring and drawing. */
class Font {
public:
    Font();
    ~Font();
    Font(const Font&) = delete;
    Font& operator=(const Font&) = delete;
    bool open(const std::string& path);
    /** The caller retains immutable font bytes until this face is closed. */
    bool open_memory(const unsigned char* bytes, std::size_t length);
    bool open_fallback();
    bool size(int pixels);
    bool valid() const { return face_ != nullptr; }
    const std::string& error() const { return error_; }
    int pixel_size() const { return pixel_size_; }
    bool contains(char32_t scalar) const;
    int advance(char32_t scalar);
    FontMetrics metrics() const;
    bool draw(Canvas& canvas, char32_t scalar, int x, int baseline, std::uint8_t gray = 1, bool bold = false);
private:
    void close();
    void* library_ = nullptr;
    void* face_ = nullptr;
    int pixel_size_ = 0;
    std::string error_;
    std::unordered_map<std::uint64_t, int> advances_;
    struct Glyph {
        int left = 0, top = 0;
        unsigned width = 0, rows = 0;
        std::vector<std::uint8_t> coverage;  // Row-major, one byte per pixel.
        std::uint32_t used = 0;  // glyph_tick_ at the most recent draw.
    };
    std::unordered_map<std::uint64_t, Glyph> glyphs_;
    std::size_t glyph_bytes_ = 0;
    std::uint32_t glyph_tick_ = 0;
};

struct LayoutSpec {
    int left = 36;
    int top = 64;
    int width = 612;
    int height = 1064;
    int font_px = 38;
    int line_height = 57;
    int indent_chars = 2;
    int paragraph_gap = 0;
};

struct PositionedGlyph {
    char32_t scalar;
    std::size_t index;
    int x;
    int baseline;
};

struct TextLine {
    std::size_t start;
    std::size_t end;
    int top;
    int baseline;
    int bottom;
};

struct Page {
    std::size_t start = 0;
    std::size_t end = 0;
    int font_px = 0;
    std::vector<TextLine> lines;
    std::vector<PositionedGlyph> glyphs;
    std::string error;
};

/**
 * Layout only the page at a Unicode scalar offset. [start,end) includes consumed
 * paragraph separators. A nonempty error leaves end equal to start.
 */
Page layout_page(Font& font, std::u32string_view text, std::size_t start,
                 const LayoutSpec& spec = {});
bool render_page(Canvas& canvas, Font& font, const Page& page, std::uint8_t gray = 1);

/** Decode PNG/baseline JPEG centered in the box. Crop fills the box; otherwise fit the whole image.
 * Failure leaves the canvas unchanged. Temporary pixels are bounded by the box dimensions;
 * the scaling tables hold one 16-bit entry per source column and row.
 */
bool render_image(Canvas& canvas, const std::vector<std::uint8_t>& encoded,
                  int x, int y, int width, int height, std::string& error, bool crop = false);

/** Invalid UTF-8 sequences become U+FFFD; valid non-BMP characters remain one scalar. */
std::u32string decode_utf8(std::string_view text);

}  // namespace pico_page::graphics
