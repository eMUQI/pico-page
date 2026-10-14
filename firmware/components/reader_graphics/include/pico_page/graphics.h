#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#include <deque>
#include <list>
#include <functional>

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
    /** SHA-256 of the compiled fallback font bytes. */
    static const char* fallback_identity();
    bool size(int pixels);
    /** Synthetic stroke strength: 0 regular, 1 medium, 2 bold; advances are unchanged. */
    void weight(unsigned strength) { weight_ = strength > 2 ? 2 : strength; }
    bool valid() const { return face_ != nullptr; }
    const std::string& error() const { return error_; }
    int pixel_size() const { return pixel_size_; }
    /** Pixel payload and entry counts exclude allocator and hash-container overhead. */
    std::size_t cached_glyph_bytes() const { return glyph_bytes_; }
    std::size_t cached_glyphs() const { return glyphs_.size(); }
    std::size_t cached_advances() const { return advances_.size(); }
    /** Evicts at most max_entries least-recent glyphs toward target_bytes; existing canvas pixels are unchanged. */
    void trim_glyph_cache(std::size_t target_bytes, std::size_t max_entries = 32);
    bool contains(char32_t scalar) const;
    /** True for a character the face lacks that draws as the placeholder box. */
    bool missing(char32_t scalar) const;
    /** A character the face lacks draws as a hollow box: one em wide for CJK and other wide
     * characters, half an em otherwise. Missing spaces and zero-width characters draw nothing. */
    int advance(char32_t scalar);
    FontMetrics metrics() const;
    /** Rasterizes into the existing bounded glyph cache without modifying a canvas. */
    bool warm(char32_t scalar, bool bold = false);
    bool draw(Canvas& canvas, char32_t scalar, int x, int baseline, std::uint8_t gray = 1, bool bold = false);
private:
    void close();
    void* library_ = nullptr;
    void* face_ = nullptr;
    int pixel_size_ = 0;
    unsigned weight_ = 0;
    std::string error_;
    std::unordered_map<std::uint64_t, int> advances_;
    std::deque<std::uint64_t> advance_order_;
    std::list<std::uint64_t> glyph_order_;
    struct Glyph {
        int left = 0, top = 0;
        unsigned width = 0, rows = 0;
        std::vector<std::uint8_t> coverage;  // Row-major, one byte per pixel.
        std::list<std::uint64_t>::iterator age;
    };
    std::unordered_map<std::uint64_t, Glyph> glyphs_;
    std::size_t glyph_bytes_ = 0;
    std::uint64_t glyph_key(char32_t scalar, bool bold) const;
    bool rasterize(char32_t scalar, bool bold, Glyph& glyph);
    bool placeholder(char32_t scalar, Glyph& glyph);

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
    int font_px;
    bool bold = false;
};

struct ImageBox {
    std::size_t begin;
    int x, y, width, height;
    std::string resource;
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
    int next_top = 0;
    std::vector<TextLine> lines;
    std::vector<PositionedGlyph> glyphs;
    std::vector<ImageBox> images;
    std::string error;
};

/**
 * Layout only the page at a Unicode scalar offset. [start,end) includes consumed
 * paragraph separators. A nonempty error leaves end equal to start.
 * collect=false uses identical line breaking without retaining drawing commands. stop is
 * checked between lines and every 64 measured scalars; cancellation returns "Layout cancelled".
 */
Page layout_page(Font& font, std::u32string_view text, std::size_t start,
                 const LayoutSpec& spec = {}, bool collect = true,
                 const std::function<bool()>& stop = {});
/** Cancellation may leave partial pixels; use an unpublished destination when stop is provided. */
bool render_page(Canvas& canvas, Font& font, const Page& page, std::uint8_t gray = 1,
                 const std::function<bool()>& stop = {});

/** Number of image decode attempts in this process, including failures. */
std::uint64_t image_decode_count();

inline constexpr char kImageCancelled[] = "Image decode cancelled";

/** Decode PNG/baseline JPEG centered in the box. Crop fills the box; otherwise fit the whole image.
 * Validation and decode failures leave the canvas unchanged. Temporary pixels are bounded by
 * the box dimensions; scaling tables hold one 16-bit entry per source column and row.
 * Cancellation is checked between PNG input chunks, JPEG blocks and output rows; it returns
 * false with kImageCancelled and may leave partial pixels after output begins. When stop is
 * provided, use an unpublished destination and discard its contents on cancellation.
 */
bool render_image(Canvas& canvas, const std::vector<std::uint8_t>& encoded,
                  int x, int y, int width, int height, std::string& error, bool crop = false,
                  const std::function<bool()>& stop = {});

/**
 * Draws text as a QR code centred in a size x size box at (x, y), black on white with a
 * four-module quiet zone and error correction level M. Returns false and leaves the canvas
 * unchanged when the text does not fit version 10 or a module would be under 3 pixels.
 */
bool draw_qr(Canvas& canvas, std::string_view text, int x, int y, int size);

/** Invalid UTF-8 sequences become U+FFFD; valid non-BMP characters remain one scalar. */
std::u32string decode_utf8(std::string_view text);

}  // namespace pico_page::graphics
