#pragma once
#include "pico_page/content_failure.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#include <deque>
#include <list>
#include <functional>
#include <optional>

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
    /** Cache budgets exclude FreeType and container overhead; zero budgets are clamped to one. */
    explicit Font(std::size_t glyph_cache_limit_bytes = 1024U * 1024, std::size_t advance_cache_limit = 8192);
    ~Font();
    Font(const Font&) = delete;
    Font& operator=(const Font&) = delete;
    /** Closes the previous face before attempting to open path; failure does not restore that face. */
    bool open(const std::string& path);
    /** Replaces the previous face; the caller retains immutable font bytes until this face is closed. */
    bool open_memory(const unsigned char* bytes, std::size_t length);
    bool open_fallback();
    /** Releases the font file and all cached glyphs and widths. The object can be opened again. */
    void close();
    /** SHA-256 of the compiled fallback font bytes. */
    static const char* fallback_identity();
    /**
     * Sets the pixel height of an open font face.
     * @param pixels Requested height in pixels, from 8 through 256.
     * @return True on success; false for a missing face, invalid size or FreeType failure.
     *         error() describes the failure.
     */
    bool set_pixel_size(int pixels);
    /** Synthetic stroke strength: 0 regular, 1 medium, 2 bold; larger values clamp to 2. Advances are unchanged. */
    void set_synthetic_weight(unsigned strength) { weight_ = strength > 2 ? 2 : strength; }
    bool valid() const { return face_ != nullptr; }
    /** Failure stage, with the native FreeType error and Unicode scalar when available. */
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
    /** A character the face lacks draws as a hollow box: one em wide for CJK characters, half
     * an em otherwise. Missing characters that have no visible form take no room. */
    int advance(char32_t scalar);
    FontMetrics metrics() const;
    /** Rasterizes into the existing bounded glyph cache without modifying a canvas. */
    bool warm(char32_t scalar, bool bold = false);
    bool draw(Canvas& canvas, char32_t scalar, int x, int baseline, std::uint8_t gray = 1, bool bold = false);
    /** Rows of drawn coverage for scalar at the current size: above_baseline is the first row's
     * distance above the baseline. False when the glyph cannot be rasterized. */
    bool ink_rows(char32_t scalar, int& above_baseline, int& rows);
private:
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
    const std::size_t glyph_cache_limit_bytes_, advance_cache_limit_;
    std::uint64_t glyph_key(char32_t scalar, bool bold) const;
    bool rasterize(char32_t scalar, bool bold, Glyph& glyph);
    bool placeholder(char32_t scalar, Glyph& glyph);
    bool fail(const char* stage, int native_error = 0, std::optional<char32_t> scalar = std::nullopt);

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
    int advance = 0;  // Measured during layout; hit testing never accesses the font face.
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

struct SelectionBox { int x, y, width, height; };
/** Returns the Unicode scalar index under a visible glyph cell. Whitespace,
 * images and margins have no hit. With nearest=true, whitespace and margins
 * snap to the nearest text line and glyph for dragging an existing selection.
 * Geometry is from the already laid-out page; images still have no hit.
 */
std::optional<std::size_t> text_hit(const Page& page, int x, int y, bool nearest = false);
/** One rectangle per selected text line for the scalar range [begin,end).
 * Clips to the current page; does not select images or allocate a pixel buffer.
 */
std::vector<SelectionBox> selection_boxes(const Page& page, std::size_t begin, std::size_t end);

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

/** Decode PNG/JPEG or the first GIF frame centered in the box. Progressive JPEG uses a reduced-detail DC preview.
 * Crop fills the box; otherwise fit the whole image.
 * Validation and decode failures leave the canvas unchanged. Temporary pixels are bounded by
 * the box dimensions; scaling tables hold one 16-bit entry per source column and row.
 * Cancellation is checked between PNG input chunks, JPEG blocks, GIF LZW codes and output rows; it returns
 * false with kImageCancelled and may leave partial pixels after output begins. When stop is
 * provided, use an unpublished destination and discard its contents on cancellation.
 */
/** Optional failure_kind is reset on entry and set at the failure source; success reports None. */
bool render_image(Canvas& canvas, const std::vector<std::uint8_t>& encoded,
                  int x, int y, int width, int height, std::string& error, bool crop = false,
                  const std::function<bool()>& stop = {}, ContentFailureKind* failure_kind = nullptr);

/** Decode a centered, fitted PNG into interleaved 8-bit gray/alpha pairs. Padding is transparent.
 * On failure, clears layer. has_alpha reports transparency in source pixels, excluding padding.
 * Dimensions must be positive, at most 684 x 1216. The result owns no source or canvas references.
 */
bool decode_png_layer(const std::vector<std::uint8_t>& encoded, int width, int height,
                      std::string& layer, bool& has_alpha, std::string& error,
                      const std::function<bool()>& stop = {}, ContentFailureKind* failure_kind = nullptr);

/** Streams a fitted PNG without an encoded-file or full-layer allocation.
 * read supplies exact bounded byte ranges. paint receives final, disjoint destination rectangles
 * and 8-bit gray/alpha; Adam7 preview blocks are excluded. Padding receives no callbacks.
 * painted returns the centered image bounds; has_alpha excludes padding. All callbacks are
 * synchronous. Failure/cancellation may follow paint calls: discard unpublished output.
 */
bool decode_png_stream(std::size_t size,
                       const std::function<bool(std::size_t, char*, std::size_t)>& read,
                       int width, int height,
                       const std::function<void(int, int, int, int, std::uint8_t, std::uint8_t)>& paint,
                       ImageBox& painted, bool& has_alpha, std::string& error,
                       const std::function<bool()>& stop = {}, ContentFailureKind* failure_kind = nullptr);

/** Blend complete rows of a gray/alpha layer into an unpublished canvas. Returns false for
 * invalid dimensions, payload or row bounds without modifying the canvas. Pixels use 0..15 gray.
 */
bool composite_image_layer(Canvas& canvas, std::string_view layer, int first_row, int row_count);

/** Blends row_count gray/alpha rows starting at first_row. Unlike a full layer, rows contains
 * only those rows. Invalid dimensions, payload or bounds leave the canvas unchanged.
 */
bool composite_image_rows(Canvas& canvas, std::string_view rows, int first_row, int row_count);

/**
 * Draws text as a QR code centred in a size x size box at (x, y), black on white with a
 * four-module quiet zone and error correction level M. Returns false and leaves the canvas
 * unchanged when the text does not fit version 10 or a module would be under 3 pixels.
 */
bool draw_qr(Canvas& canvas, std::string_view text, int x, int y, int size);

/** Invalid UTF-8 sequences become U+FFFD; valid non-BMP characters remain one scalar. */
std::u32string decode_utf8(std::string_view text);

}  // namespace pico_page::graphics
