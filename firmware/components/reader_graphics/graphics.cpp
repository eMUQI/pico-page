#include "pico_page/graphics.h"

#include <algorithm>
#include <cstdlib>
#include <limits>

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_ADVANCES_H
#include FT_SYNTHESIS_H
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

Font::Font() {
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
    if (result) error_ = "FreeType initialization failed";
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
    glyphs_.clear();
    glyph_bytes_ = 0;
    if (face_) FT_Done_Face(static_cast<FT_Face>(face_));
    face_ = nullptr;
    pixel_size_ = 0;
}

bool Font::open(const std::string& path) {
    close();
    FT_Face face = nullptr;
    if (!library_ || FT_New_Face(static_cast<FT_Library>(library_), path.c_str(), 0, &face)) {
        error_ = "Cannot open font: " + path;
        return false;
    }
    face_ = face;
    if (FT_Select_Charmap(face, FT_ENCODING_UNICODE)) {
        close(); error_ = "Font has no Unicode character map"; return false;
    }
    error_.clear();
    return size(28);
}

bool Font::open_memory(const unsigned char* bytes, std::size_t length) {
    close();
    FT_Face face = nullptr;
    if (!bytes || length > static_cast<std::size_t>(std::numeric_limits<FT_Long>::max()) ||
        !library_ || FT_New_Memory_Face(static_cast<FT_Library>(library_), bytes,
                                      static_cast<FT_Long>(length), 0, &face)) {
        error_ = "Cannot open embedded font";
        return false;
    }
    face_ = face;
    if (FT_Select_Charmap(face, FT_ENCODING_UNICODE)) {
        close(); error_ = "Font has no Unicode character map"; return false;
    }
    error_.clear();
    return size(28);
}

bool Font::open_fallback() { return open_memory(kFallbackFont, kFallbackFontSize); }

bool Font::size(int pixels) {
    if (!face_ || pixels < 8 || pixels > 256 ||
        FT_Set_Pixel_Sizes(static_cast<FT_Face>(face_), 0, pixels)) {
        error_ = "Invalid font size or unavailable font"; return false;
    }
    pixel_size_ = pixels;
    error_.clear();
    return true;
}

bool Font::contains(char32_t scalar) const {
    return face_ && FT_Get_Char_Index(static_cast<FT_Face>(face_), scalar) != 0;
}

int Font::advance(char32_t scalar) {
    if (scalar == U'\n' || scalar == U'\r') return 0;
    if (scalar == U'\t') return advance(U' ') * 4;
    const auto key = (std::uint64_t(pixel_size_) << 32) | std::uint32_t(scalar);
    const auto cached = advances_.find(key);
    if (cached != advances_.end()) return cached->second;
    const auto face = static_cast<FT_Face>(face_);
    if (!face) { error_ = "Font glyph metrics failed"; return -1; }
    int width;
    FT_Fixed design = 0;
    // For static SFNT faces the hinted loader yields the scaled design advance rounded to a whole
    // pixel. Reading that advance alone avoids fetching and scaling the outline from storage.
    if (FT_IS_SFNT(face) && !FT_HAS_MULTIPLE_MASTERS(face) &&
        !FT_Get_Advance(face, FT_Get_Char_Index(face, scalar), FT_LOAD_NO_SCALE, &design)) {
        width = static_cast<int>((FT_MulFix(design, face->size->metrics.x_scale) + 32) >> 6);
    } else if (!FT_Load_Char(face, scalar, FT_LOAD_DEFAULT)) {
        width = static_cast<int>((face->glyph->advance.x + 32) >> 6);
    } else {
        error_ = "Font glyph metrics failed"; return -1;
    }
    // Bound per-face metrics memory across all sizes; reopening invalidates every entry.
    if (advances_.size() >= 8192) advances_.clear();
    advances_.emplace(key, width);
    return width;
}

FontMetrics Font::metrics() const {
    if (!face_ || !pixel_size_) return {};
    const auto& metrics = static_cast<FT_Face>(face_)->size->metrics;
    return {static_cast<int>((metrics.ascender + 63) >> 6),
            static_cast<int>((-metrics.descender + 63) >> 6),
            static_cast<int>((metrics.height + 63) >> 6)};
}

bool Font::warm(char32_t scalar, bool bold) {
    if (scalar == U'\n' || scalar == U'\r' || scalar == U'\t') return true;
    const auto key = (std::uint64_t(bold) << 63) | (std::uint64_t(pixel_size_) << 32) | std::uint32_t(scalar);
    auto cached = glyphs_.find(key);
    if (cached == glyphs_.end()) {
        if (!face_ || FT_Load_Char(static_cast<FT_Face>(face_), scalar,
                                 FT_LOAD_DEFAULT | FT_LOAD_TARGET_NORMAL)) {
            error_ = "Font glyph rasterization failed"; return false;
        }
        const auto slot = static_cast<FT_Face>(face_)->glyph;
        if (bold) FT_GlyphSlot_Embolden(slot);
        if (FT_Render_Glyph(slot, FT_RENDER_MODE_NORMAL)) {
            error_ = "Font glyph rasterization failed"; return false;
        }
        const auto& bitmap = slot->bitmap;
        if (bitmap.pixel_mode != FT_PIXEL_MODE_GRAY && bitmap.pixel_mode != FT_PIXEL_MODE_MONO &&
            bitmap.rows) { error_ = "Unsupported glyph pixel format"; return false; }
        Glyph glyph{slot->bitmap_left, slot->bitmap_top, bitmap.width, bitmap.rows, {}};
        glyph.coverage.resize(static_cast<std::size_t>(bitmap.width) * bitmap.rows);
        for (unsigned row = 0; row < bitmap.rows; ++row) {
            const auto* bytes = bitmap.buffer + (bitmap.pitch >= 0 ? row : bitmap.rows - 1 - row) *
                                               std::abs(bitmap.pitch);
            for (unsigned col = 0; col < bitmap.width; ++col)
                glyph.coverage[static_cast<std::size_t>(row) * bitmap.width + col] =
                    bitmap.pixel_mode == FT_PIXEL_MODE_MONO
                        ? ((bytes[col / 8] & (0x80 >> (col & 7))) ? 255 : 0) : bytes[col];
        }
        constexpr std::size_t glyph_cache_limit = 1024 * 1024;
        if (glyph_bytes_ + glyph.coverage.size() > glyph_cache_limit) {
            // Keep the more recently drawn half so the page being drawn stays cached.
            std::vector<std::uint32_t> stamps;
            stamps.reserve(glyphs_.size());
            for (const auto& entry : glyphs_) stamps.push_back(entry.second.used);
            const auto middle = stamps.begin() + stamps.size() / 2;
            std::nth_element(stamps.begin(), middle, stamps.end());
            for (auto entry = glyphs_.begin(); middle != stamps.end() && entry != glyphs_.end();) {
                if (entry->second.used < *middle) {
                    glyph_bytes_ -= entry->second.coverage.size();
                    entry = glyphs_.erase(entry);
                } else {
                    ++entry;
                }
            }
            if (glyph_bytes_ + glyph.coverage.size() > glyph_cache_limit) {
                glyphs_.clear();
                glyph_bytes_ = 0;
            }
        }
        glyph_bytes_ += glyph.coverage.size();
        cached = glyphs_.emplace(key, std::move(glyph)).first;
    }
    cached->second.used = ++glyph_tick_;
    return true;
}

bool Font::draw(Canvas& canvas, char32_t scalar, int x, int baseline, std::uint8_t gray, bool bold) {
    if (!warm(scalar, bold)) return false;
    if (scalar == U'\n' || scalar == U'\r' || scalar == U'\t') return true;
    const auto key = (std::uint64_t(bold) << 63) | (std::uint64_t(pixel_size_) << 32) | std::uint32_t(scalar);
    const auto& glyph = glyphs_.at(key);
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

namespace {
bool cjk(char32_t c) {
    return (c >= 0x2e80 && c <= 0x9fff) || (c >= 0xf900 && c <= 0xfaff) ||
           (c >= 0x20000 && c <= 0x323af);
}
bool closing(char32_t c) { return std::u32string_view(U"，。！？、：；）】》」』〉…’”﹐﹑﹒﹔﹕﹖﹗﹚﹜﹞｡､｣,.!?;:)]}").find(c) != std::u32string_view::npos; }
bool opening(char32_t c) { return std::u32string_view(U"（【《「『〈‘“﹙﹛﹝｢([{").find(c) != std::u32string_view::npos; }
bool break_after(char32_t previous, char32_t next) {
    if (closing(next) || opening(previous)) return false;
    return previous == U' ' || previous == U'\t' || previous == U'-' || cjk(previous) || cjk(next);
}
}  // namespace

Page layout_page(Font& font, std::u32string_view text, std::size_t start, const LayoutSpec& spec) {
    Page page;
    page.start = page.end = start;
    page.font_px = spec.font_px;
    if (start > text.size() || spec.width <= 0 || spec.width > 4096 || spec.height <= 0 ||
        spec.height > 4096 || spec.top < 0 || spec.top > 4096 || spec.left < 0 || spec.left > 4096 ||
        spec.line_height <= 0 || spec.line_height > 1024 || spec.indent_chars < 0 ||
        spec.indent_chars > 4 || spec.paragraph_gap < 0 || spec.paragraph_gap > 1024 ||
        !font.size(spec.font_px)) {
        page.error = "Invalid page geometry or font"; return page;
    }
    const auto metrics = font.metrics();
    if (metrics.ascender + metrics.descender > spec.line_height || spec.line_height > spec.height) {
        page.error = "Line height cannot contain the complete font line"; return page;
    }
    std::size_t cursor = start;
    int top = spec.top;
    while (cursor < text.size() && top + spec.line_height <= spec.top + spec.height) {
        const auto line_start = cursor;
        const bool paragraph_start = cursor == 0 || text[cursor - 1] == U'\n' || text[cursor - 1] == U'\r';
        const bool source_indented = text[cursor] == U'\u3000' || text[cursor] == U'\t';
        const int indent = paragraph_start && !source_indented ? spec.indent_chars * spec.font_px : 0;
        int used = indent;
        std::size_t end = cursor;
        std::size_t last_break = cursor;
        bool newline = false;
        while (end < text.size()) {
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
        for (std::size_t i = cursor; i < end; ++i) {
            if (text[i] == U'\n' || text[i] == U'\r') continue;
            page.glyphs.push_back({text[i], i, x, baseline});
            x += font.advance(text[i]);
        }
        page.lines.push_back({line_start, end, top, baseline, top + spec.line_height});
        cursor = end;
        top += spec.line_height + (newline ? spec.paragraph_gap : 0);
    }
    page.end = cursor;
    return page;
}

bool render_page(Canvas& canvas, Font& font, const Page& page, std::uint8_t gray) {
    if (!page.error.empty() || !font.size(page.font_px)) return false;
    for (const auto& glyph : page.glyphs)
        if (!font.draw(canvas, glyph.scalar, glyph.x, glyph.baseline, gray)) return false;
    return true;
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
