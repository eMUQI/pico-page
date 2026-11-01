#include "pico_page/graphics.h"

#include <algorithm>
#include <cstdlib>
#include <cstdio>
#include <limits>

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_ADVANCES_H
#include FT_SYNTHESIS_H
#include FT_OUTLINE_H
#ifdef ESP_PLATFORM
#include FT_MODULE_H
#include "esp_heap_caps.h"
#endif

#include "fallback_font.h"

namespace pico_page::graphics {

#ifdef ESP_PLATFORM
namespace {
void* font_allocate(FT_Memory, long size) {
    return size > 0 ? heap_caps_malloc(static_cast<std::size_t>(size),
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) : nullptr;
}

void font_free(FT_Memory, void* block) {
    heap_caps_free(block);
}

void* font_reallocate(FT_Memory, long, long size, void* block) {
    if (size < 0) return nullptr;
    if (size == 0) {
        heap_caps_free(block);
        return nullptr;
    }
    // A failed resize preserves the original allocation and its contents.
    return heap_caps_realloc(block, static_cast<std::size_t>(size),
                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

// FreeType retains this descriptor for the lifetime of every library instance.
FT_MemoryRec_ font_memory{nullptr, font_allocate, font_free, font_reallocate};
} // namespace
#endif

Canvas::Canvas(int width, int height)
    : width_(std::clamp(width, 1, 4096)), height_(std::clamp(height, 1, 4096)),
      pixels_(static_cast<std::size_t>(stride()) * height_, 0xff) {}

void Canvas::clear(std::uint8_t gray) {
    gray &= 15;
    std::fill(pixels_.begin(), pixels_.end(), gray | (gray << 4));
}

std::uint8_t Canvas::pixel(int x, int y) const {
    if (x < 0 || x >= width_ || y < 0 || y >= height_) return 15;
    return (pixels_[static_cast<std::size_t>(y) * stride() + x / 2] >> ((x & 1) * 4)) & 15;
}

void Canvas::pixel(int x, int y, std::uint8_t gray) {
    if (x < 0 || x >= width_ || y < 0 || y >= height_) return;
    auto& byte = pixels_[static_cast<std::size_t>(y) * stride() + x / 2];
    const int shift = (x & 1) * 4;
    byte = static_cast<std::uint8_t>((byte & ~(15 << shift)) | ((gray & 15) << shift));
}

void Canvas::fill_rect(int x, int y, int width, int height, std::uint8_t gray) {
    const auto right = std::min<std::int64_t>(width_, static_cast<std::int64_t>(x) + width);
    const auto bottom = std::min<std::int64_t>(height_, static_cast<std::int64_t>(y) + height);
    for (int row = std::max(0, y); row < bottom; ++row)
        for (int col = std::max(0, x); col < right; ++col) pixel(col, row, gray);
}

void Canvas::line(int x0, int y0, int x1, int y1, std::uint8_t gray, int thickness) {
    if (thickness < 1 || thickness > 4096 || x0 < -16384 || x0 > 16384 || y0 < -16384 ||
        y0 > 16384 || x1 < -16384 || x1 > 16384 || y1 < -16384 || y1 > 16384) return;
    const int dx = std::abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    const int dy = -std::abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int error = dx + dy;
    for (;;) {
        fill_rect(x0 - thickness / 2, y0 - thickness / 2, thickness, thickness, gray);
        if (x0 == x1 && y0 == y1) break;
        const int twice = error * 2;
        if (twice >= dy) { error += dy; x0 += sx; }
        if (twice <= dx) { error += dx; y0 += sy; }
    }
}

Font::Font(std::size_t glyph_cache_limit_bytes, std::size_t advance_cache_limit)
    : glyph_cache_limit_bytes_(std::max<std::size_t>(1, glyph_cache_limit_bytes)),
      advance_cache_limit_(std::max<std::size_t>(1, advance_cache_limit)) {
    FT_Library library = nullptr;
#ifdef ESP_PLATFORM
    const auto result = FT_New_Library(&font_memory, &library);
    if (!result) {
        FT_Add_Default_Modules(library);
        FT_Set_Default_Properties(library);
    }
#else
    const auto result = FT_Init_FreeType(&library);
#endif
    if (result) fail("FreeType initialization failed", result);
    library_ = library;
}

Font::~Font() {
    close();
#ifdef ESP_PLATFORM
    if (library_) FT_Done_Library(static_cast<FT_Library>(library_));
#else
    if (library_) FT_Done_FreeType(static_cast<FT_Library>(library_));
#endif
}

void Font::close() {
    advances_.clear();
    advance_order_.clear();
    glyph_order_.clear();
    glyphs_.clear();
    glyph_bytes_ = 0;
    if (face_) FT_Done_Face(static_cast<FT_Face>(face_));
    face_ = nullptr;
    pixel_size_ = 0;
}

bool Font::fail(const char* stage, int native_error, std::optional<char32_t> scalar) {
    error_ = stage;
    if (native_error && FT_ERROR_BASE(native_error) == FT_Err_Out_Of_Memory)
        error_ += ": FreeType out of memory";
    if (native_error) error_ += " [FT=" + std::to_string(native_error) + "]";
    if (scalar) {
        char codepoint[16];
        std::snprintf(codepoint, sizeof(codepoint), " [U+%04lX]", static_cast<unsigned long>(*scalar));
        error_ += codepoint;
    }
    return false;
}

bool Font::open(const std::string& path) {
    close();
    FT_Face face = nullptr;
    if (!library_) return fail("FreeType initialization failed");
    const auto opened = FT_New_Face(static_cast<FT_Library>(library_), path.c_str(), 0, &face);
    if (opened) {
        fail("Cannot open font", opened);
        error_ += "; path=" + path;
        return false;
    }
    face_ = face;
    const auto mapped = FT_Select_Charmap(face, FT_ENCODING_UNICODE);
    if (mapped) {
        close(); return fail("Font has no Unicode character map", mapped);
    }
    error_.clear();
    return set_pixel_size(28);
}

bool Font::open_memory(const unsigned char* bytes, std::size_t length) {
    close();
    FT_Face face = nullptr;
    if (!library_) return fail("FreeType initialization failed");
    if (!bytes || length > static_cast<std::size_t>(std::numeric_limits<FT_Long>::max()))
        return fail("Cannot open embedded font");
    const auto opened = FT_New_Memory_Face(static_cast<FT_Library>(library_), bytes,
                                         static_cast<FT_Long>(length), 0, &face);
    if (opened) return fail("Cannot open embedded font", opened);
    face_ = face;
    const auto mapped = FT_Select_Charmap(face, FT_ENCODING_UNICODE);
    if (mapped) {
        close(); return fail("Font has no Unicode character map", mapped);
    }
    error_.clear();
    return set_pixel_size(28);
}

const char* Font::fallback_identity() { return kFallbackIdentity; }

bool Font::open_fallback() { return open_memory(kFallbackFont, kFallbackFontSize); }

bool Font::set_pixel_size(int pixels) {
    if (!face_) return fail("Font face unavailable");
    if (pixels < 8 || pixels > 256) return fail("Invalid font size");
    const auto sized = FT_Set_Pixel_Sizes(static_cast<FT_Face>(face_), 0, pixels);
    if (sized) return fail("Invalid font size", sized);
    pixel_size_ = pixels;
    error_.clear();
    return true;
}

void Font::trim_glyph_cache(std::size_t target_bytes, std::size_t max_entries) {
    while (max_entries-- && glyph_bytes_ > target_bytes && !glyph_order_.empty()) {
        const auto oldest = glyph_order_.front();
        glyph_bytes_ -= glyphs_.at(oldest).coverage.size();
        glyphs_.erase(oldest); glyph_order_.pop_front();
    }
}

namespace {
// Unicode Default_Ignorable_Code_Point and the C0 controls: characters with no visible form.
// A face without them draws nothing; any other missing character draws the placeholder.
bool blank(char32_t c) {
    return c < 0x20 || c == 0xad || c == 0x34f || c == 0x61c || c == 0x115f || c == 0x1160 || c == 0x17b4 ||
           c == 0x17b5 || (c >= 0x180b && c <= 0x180f) || (c >= 0x200b && c <= 0x200f) ||
           (c >= 0x202a && c <= 0x202e) || (c >= 0x2060 && c <= 0x206f) || c == 0x3164 ||
           (c >= 0xfe00 && c <= 0xfe0f) || c == 0xfeff || c == 0xffa0 || (c >= 0xfff0 && c <= 0xfff8) ||
           (c >= 0x1bca0 && c <= 0x1bca3) || (c >= 0x1d173 && c <= 0x1d17a) || (c >= 0xe0000 && c <= 0xe0fff);
}
bool cjk(char32_t c) {
    return (c >= 0x2e80 && c <= 0x9fff) || (c >= 0xf900 && c <= 0xfaff) ||
           (c >= 0x20000 && c <= 0x323af);
}
}  // namespace

bool Font::contains(char32_t scalar) const {
    return face_ && FT_Get_Char_Index(static_cast<FT_Face>(face_), scalar) != 0;
}

bool Font::missing(char32_t scalar) const { return !contains(scalar) && !blank(scalar); }

int Font::advance(char32_t scalar) {
    if (scalar == U'\n' || scalar == U'\r') return 0;
    if (scalar == U'\t') return advance(U' ') * 4;
    const auto key = (std::uint64_t(pixel_size_) << 32) | std::uint32_t(scalar);
    const auto cached = advances_.find(key);
    if (cached != advances_.end()) return cached->second;
    const auto face = static_cast<FT_Face>(face_);
    if (!face) { fail("Font face unavailable", 0, scalar); return -1; }
    int width;
    FT_Fixed design = 0;
    if (!FT_Get_Char_Index(face, scalar)) {
        // A missing character takes the cell of a present one, whatever the face's own .notdef
        // advance is: a full em where line breaking treats it as CJK, half an em otherwise.
        width = blank(scalar) ? 0 : cjk(scalar) ? pixel_size_ : (pixel_size_ + 1) / 2;
    } else {
        // For static SFNT faces the hinted loader yields the scaled design advance rounded to a whole
        // pixel. Reading that advance alone avoids fetching and scaling the outline from storage.
        if (FT_IS_SFNT(face) && !FT_HAS_MULTIPLE_MASTERS(face) &&
            !FT_Get_Advance(face, FT_Get_Char_Index(face, scalar), FT_LOAD_NO_SCALE, &design)) {
            width = static_cast<int>((FT_MulFix(design, face->size->metrics.x_scale) + 32) >> 6);
        } else {
            const auto loaded = FT_Load_Char(face, scalar, FT_LOAD_DEFAULT);
            if (loaded) { fail("Font glyph metrics failed", loaded, scalar); return -1; }
            width = static_cast<int>((face->glyph->advance.x + 32) >> 6);
        }
    }
    // Bound per-face metrics memory across all sizes; reopening invalidates every entry.
    if (advances_.size() >= advance_cache_limit_) {
        advances_.erase(advance_order_.front());
        advance_order_.pop_front();
    }
    advances_.emplace(key, width);
    advance_order_.push_back(key);
    return width;
}

FontMetrics Font::metrics() const {
    if (!face_ || !pixel_size_) return {};
    const auto& metrics = static_cast<FT_Face>(face_)->size->metrics;
    return {static_cast<int>((metrics.ascender + 63) >> 6),
            static_cast<int>((-metrics.descender + 63) >> 6),
            static_cast<int>((metrics.height + 63) >> 6)};
}

std::uint64_t Font::glyph_key(char32_t scalar, bool bold) const {
    // The characters a face lacks share one wide and one narrow placeholder per size.
    if (!contains(scalar))
        return (std::uint64_t(pixel_size_) << 32) | (blank(scalar) ? 0xfffffffdU : cjk(scalar) ? 0xffffffffU : 0xfffffffeU);
    return (std::uint64_t(bold) << 63) | (std::uint64_t(weight_) << 60) | (std::uint64_t(pixel_size_) << 32) | std::uint32_t(scalar);
}

bool Font::placeholder(char32_t scalar, Glyph& glyph) {
    // Faces draw their own .notdef as a box, a pattern or nothing at all. A hollow box inside
    // the cell that advance() reserves keeps every missing character visible.
    const int reserved = advance(scalar), stroke = std::max(1, pixel_size_ / 16);
    if (reserved < 0) return false;
    const int width = std::min(reserved, pixel_size_) - 2 * std::max(1, pixel_size_ / 10);
    const int rows = pixel_size_ * 4 / 5;
    if (blank(scalar) || width < 3 * stroke || rows < 3 * stroke) return true;
    glyph.left = (reserved - width) / 2; glyph.top = rows - pixel_size_ / 10;
    glyph.width = static_cast<unsigned>(width); glyph.rows = static_cast<unsigned>(rows);
    glyph.coverage.assign(static_cast<std::size_t>(width) * rows, 0);
    for (int row = 0; row < rows; ++row)
        for (int col = 0; col < width; ++col)
            if (row < stroke || row >= rows - stroke || col < stroke || col >= width - stroke)
                glyph.coverage[static_cast<std::size_t>(row) * width + col] = 255;
    return true;
}

bool Font::rasterize(char32_t scalar, bool bold, Glyph& glyph) {
    const auto loaded = FT_Load_Char(static_cast<FT_Face>(face_), scalar, FT_LOAD_DEFAULT | FT_LOAD_TARGET_NORMAL);
    if (loaded) return fail("Font glyph load failed", loaded, scalar);
    const auto slot = static_cast<FT_Face>(face_)->glyph;
    if (weight_ && slot->format == FT_GLYPH_FORMAT_OUTLINE) {
        const auto adjusted = FT_Outline_Embolden(&slot->outline, static_cast<FT_Pos>(pixel_size_ * weight_));
        if (adjusted) return fail("Font stroke adjustment failed", adjusted, scalar);
    }
    if (bold) FT_GlyphSlot_Embolden(slot);
    const auto rendered = FT_Render_Glyph(slot, FT_RENDER_MODE_NORMAL);
    if (rendered) return fail("Font glyph rasterization failed", rendered, scalar);
    const auto& bitmap = slot->bitmap;
    if (bitmap.pixel_mode != FT_PIXEL_MODE_GRAY && bitmap.pixel_mode != FT_PIXEL_MODE_MONO &&
        bitmap.rows) return fail("Unsupported glyph pixel format", 0, scalar);
    glyph.left = slot->bitmap_left; glyph.top = slot->bitmap_top;
    glyph.width = bitmap.width; glyph.rows = bitmap.rows;
    glyph.coverage.resize(static_cast<std::size_t>(bitmap.width) * bitmap.rows);
    for (unsigned row = 0; row < bitmap.rows; ++row) {
        const auto* bytes = bitmap.buffer + (bitmap.pitch >= 0 ? row : bitmap.rows - 1 - row) *
                                           std::abs(bitmap.pitch);
        for (unsigned col = 0; col < bitmap.width; ++col)
            glyph.coverage[static_cast<std::size_t>(row) * bitmap.width + col] =
                bitmap.pixel_mode == FT_PIXEL_MODE_MONO
                    ? ((bytes[col / 8] & (0x80 >> (col & 7))) ? 255 : 0) : bytes[col];
    }
    return true;
}

bool Font::warm(char32_t scalar, bool bold) {
    if (scalar == U'\n' || scalar == U'\r' || scalar == U'\t') return true;
    if (!face_) return fail("Font face unavailable", 0, scalar);
    const auto key = glyph_key(scalar, bold);
    auto cached = glyphs_.find(key);
    if (cached == glyphs_.end()) {
        Glyph glyph;
        if (!(contains(scalar) ? rasterize(scalar, bold, glyph) : placeholder(scalar, glyph))) return false;
        if (glyph.coverage.size() > glyph_cache_limit_bytes_) {
            return fail("Font glyph exceeds cache budget", 0, scalar);
        }
        while (!glyph_order_.empty() && glyph_bytes_ + glyph.coverage.size() > glyph_cache_limit_bytes_) {
            const auto oldest = glyph_order_.front();
            glyph_bytes_ -= glyphs_.at(oldest).coverage.size();
            glyphs_.erase(oldest);
            glyph_order_.pop_front();
        }
        glyph_order_.push_back(key);
        glyph.age = std::prev(glyph_order_.end());
        glyph_bytes_ += glyph.coverage.size();
        cached = glyphs_.emplace(key, std::move(glyph)).first;
    }
    glyph_order_.splice(glyph_order_.end(), glyph_order_, cached->second.age);
    return true;
}

bool Font::draw(Canvas& canvas, char32_t scalar, int x, int baseline, std::uint8_t gray, bool bold) {
    if (!warm(scalar, bold)) return false;
    if (scalar == U'\n' || scalar == U'\r' || scalar == U'\t') return true;
    const auto& glyph = glyphs_.at(glyph_key(scalar, bold));
    for (unsigned row = 0; row < glyph.rows; ++row) {
        const auto* bytes = glyph.coverage.data() + static_cast<std::size_t>(row) * glyph.width;
        for (unsigned col = 0; col < glyph.width; ++col) {
            const unsigned coverage = bytes[col];
            if (!coverage) continue;
            const int px = x + glyph.left + static_cast<int>(col);
            const int py = baseline - glyph.top + static_cast<int>(row);
            const auto old = canvas.pixel(px, py);
            canvas.pixel(px, py, static_cast<std::uint8_t>(
                (old * (255 - coverage) + (gray & 15) * coverage + 127) / 255));
        }
    }
    return true;
}

bool Font::ink_rows(char32_t scalar, int& above_baseline, int& rows) {
    if (!warm(scalar)) return false;
    const auto found = glyphs_.find(glyph_key(scalar, false));
    if (found == glyphs_.end()) return false;
    above_baseline = found->second.top;
    rows = static_cast<int>(found->second.rows);
    return true;
}

namespace {
bool closing(char32_t c) { return std::u32string_view(U"，。！？、：；）】》」』〉…’”﹐﹑﹒﹔﹕﹖﹗﹚﹜﹞｡､｣,.!?;:)]}").find(c) != std::u32string_view::npos; }
bool opening(char32_t c) { return std::u32string_view(U"（【《「『〈‘“﹙﹛﹝｢([{").find(c) != std::u32string_view::npos; }
bool break_after(char32_t previous, char32_t next) {
    if (closing(next) || opening(previous)) return false;
    return previous == U' ' || previous == U'\t' || previous == U'-' || cjk(previous) || cjk(next);
}
}  // namespace

Page layout_page(Font& font, std::u32string_view text, std::size_t start, const LayoutSpec& spec, bool collect, const std::function<bool()>& stop) {
    Page page;
    page.start = page.end = start;
    page.font_px = spec.font_px;
    if (start > text.size() || spec.width <= 0 || spec.width > 4096 || spec.height <= 0 ||
        spec.height > 4096 || spec.top < 0 || spec.top > 4096 || spec.left < 0 || spec.left > 4096 ||
        spec.line_height <= 0 || spec.line_height > 1024 || spec.indent_chars < 0 ||
        spec.indent_chars > 4 || spec.paragraph_gap < 0 || spec.paragraph_gap > 1024 ||
        !font.set_pixel_size(spec.font_px)) {
        page.error = "Invalid page geometry or font"; return page;
    }
    const auto metrics = font.metrics();
    if (metrics.ascender + metrics.descender > spec.line_height || spec.line_height > spec.height) {
        page.error = "Line height cannot contain the complete font line"; return page;
    }
    std::size_t cursor = start;
    int top = spec.top;
    while (cursor < text.size() && top + spec.line_height <= spec.top + spec.height) {
        if (stop && stop()) { page.error = "Layout cancelled"; page.lines.clear(); page.glyphs.clear(); return page; }
        const auto line_start = cursor;
        const bool paragraph_start = cursor == 0 || text[cursor - 1] == U'\n' || text[cursor - 1] == U'\r';
        const bool source_indented = text[cursor] == U'\u3000' || text[cursor] == U'\t';
        const int indent = paragraph_start && !source_indented ? spec.indent_chars * spec.font_px : 0;
        int used = indent;
        std::size_t end = cursor;
        std::size_t last_break = cursor;
        bool newline = false;
        while (end < text.size()) {
            if ((end - cursor) % 64 == 0 && stop && stop()) { page.error = "Layout cancelled"; break; }
            if (text[end] == U'\n' || text[end] == U'\r') {
                ++end;
                if (text[end - 1] == U'\r' && end < text.size() && text[end] == U'\n') ++end;
                newline = true;
                break;
            }
            const int width = font.advance(text[end]);
            if (width < 0) { page.error = font.error(); break; }
            if (used + width > spec.width) {
                if (last_break > cursor) end = last_break;
                break;
            }
            used += width;
            ++end;
            if (end < text.size() && break_after(text[end - 1], text[end])) last_break = end;
            if (end - cursor > 65536) { page.error = "Too many scalars in one text line"; break; }
        }
        if (!page.error.empty() || end == cursor) {
            if (page.error.empty()) page.error = "A glyph does not fit the text column";
            page.lines.clear(); page.glyphs.clear(); page.end = start; return page;
        }
        const int baseline = top + (spec.line_height - metrics.ascender - metrics.descender) / 2 + metrics.ascender;
        int x = spec.left + indent;
        for (std::size_t i = cursor; collect && i < end; ++i) {
            if (text[i] == U'\n' || text[i] == U'\r') continue;
            const int advance = font.advance(text[i]);
            page.glyphs.push_back({text[i], i, x, baseline, spec.font_px, false, advance});
            x += advance;
        }
        if (collect) page.lines.push_back({line_start, end, top, baseline, top + spec.line_height});
        cursor = end;
        top += spec.line_height + (newline ? spec.paragraph_gap : 0);
    }
    page.end = cursor;
    page.next_top = top;
    return page;
}

std::optional<std::size_t> text_hit(const Page& page, int x, int y, bool nearest) {
    if (!page.error.empty()) return std::nullopt;
    for (const auto& image : page.images)
        if (x >= image.x && static_cast<std::int64_t>(x) < static_cast<std::int64_t>(image.x) + image.width &&
            y >= image.y && static_cast<std::int64_t>(y) < static_cast<std::int64_t>(image.y) + image.height) return std::nullopt;
    std::optional<std::size_t> result;
    std::int64_t best_y = 0, best_x = 0;
    for (const auto& line : page.lines) {
        if (!nearest && (y < line.top || y >= line.bottom)) continue;
        const auto dy = std::max<std::int64_t>({0, static_cast<std::int64_t>(line.top) - y,
                                               static_cast<std::int64_t>(y) - line.bottom + 1});
        for (const auto& glyph : page.glyphs) {
            if (glyph.index < line.start || glyph.index >= line.end || glyph.advance <= 0 ||
                blank(glyph.scalar) || glyph.scalar == U' ' || glyph.scalar == U'\u00a0' ||
                glyph.scalar == U'\u1680' || (glyph.scalar >= 0x2000 && glyph.scalar <= 0x200a) ||
                glyph.scalar == U'\u2028' || glyph.scalar == U'\u2029' || glyph.scalar == U'\u202f' ||
                glyph.scalar == U'\u205f' || glyph.scalar == U'\u3000' || glyph.scalar == U'\ufffc') continue;
            const auto dx = std::max<std::int64_t>({0, static_cast<std::int64_t>(glyph.x) - x,
                                                   static_cast<std::int64_t>(x) - glyph.x - glyph.advance + 1});
            if (!dy && !dx) return glyph.index;
            if (nearest && (!result || dy < best_y || (dy == best_y && dx < best_x))) {
                result = glyph.index; best_y = dy; best_x = dx;
            }
        }
    }
    return result;
}

std::vector<SelectionBox> selection_boxes(const Page& page, std::size_t begin, std::size_t end) {
    std::vector<SelectionBox> boxes;
    if (!page.error.empty() || begin >= end) return boxes;
    begin = std::max(begin, page.start); end = std::min(end, page.end);
    for (const auto& line : page.lines) {
        if (line.end <= begin || line.start >= end) continue;
        int left = 0, right = 0;
        bool present = false;
        for (const auto& glyph : page.glyphs) {
            if (glyph.index < std::max(begin, line.start) || glyph.index >= std::min(end, line.end) ||
                glyph.advance <= 0 || blank(glyph.scalar) || glyph.scalar == U'\ufffc') continue;
            left = present ? std::min(left, glyph.x) : glyph.x;
            right = std::max(right, glyph.x + glyph.advance);
            present = true;
        }
        if (present) boxes.push_back({left, line.top, right - left, line.bottom - line.top});
    }
    return boxes;
}

bool render_page(Canvas& canvas, Font& font, const Page& page, std::uint8_t gray, const std::function<bool()>& stop) {
    if (!page.error.empty() || !font.set_pixel_size(page.font_px)) return false;
    int current_size = page.font_px;
    for (const auto& glyph : page.glyphs) {
        if (stop && stop()) return false;
        if (glyph.font_px != current_size) {
            if (!font.set_pixel_size(glyph.font_px)) return false;
            current_size = glyph.font_px;
        }
        if (!font.draw(canvas, glyph.scalar, glyph.x, glyph.baseline, gray, glyph.bold)) return false;
    }
    return current_size == page.font_px || font.set_pixel_size(page.font_px);
}

std::u32string decode_utf8(std::string_view text) {
    std::u32string result;
    for (std::size_t i = 0; i < text.size();) {
        const auto lead = static_cast<unsigned char>(text[i]);
        int count = lead < 0x80 ? 1 : lead >= 0xc2 && lead <= 0xdf ? 2 :
                    lead >= 0xe0 && lead <= 0xef ? 3 : lead >= 0xf0 && lead <= 0xf4 ? 4 : 0;
        char32_t scalar = count == 1 ? lead : count ? lead & ((1 << (7 - count)) - 1) : 0;
        bool valid = count && i + count <= text.size();
        for (int j = 1; valid && j < count; ++j) {
            const auto byte = static_cast<unsigned char>(text[i + j]);
            valid = (byte & 0xc0) == 0x80;
            scalar = (scalar << 6) | (byte & 0x3f);
        }
        valid = valid && !(count == 2 && scalar < 0x80) && !(count == 3 && scalar < 0x800) &&
                !(count == 4 && scalar < 0x10000) && scalar <= 0x10ffff &&
                !(scalar >= 0xd800 && scalar <= 0xdfff);
        result.push_back(valid ? scalar : U'\ufffd');
        i += valid ? count : 1;
    }
    return result;
}

}  // namespace pico_page::graphics
