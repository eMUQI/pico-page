#include "pico_page/graphics.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <sys/stat.h>
#include <unistd.h>

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_ADVANCES_H
#include FT_SYNTHESIS_H
#include FT_OUTLINE_H
#ifdef ESP_PLATFORM
#include FT_MODULE_H
#include "esp_heap_caps.h"
#endif
#ifdef CONFIG_PICO_PAGE_LEGACY_PERF_LOG
#include "esp_log.h"
#include "esp_timer.h"
#endif

#include "fallback_font.h"

namespace pico_page::graphics {

#ifdef CONFIG_PICO_PAGE_LEGACY_PERF_LOG
namespace {
struct PageRenderTiming;
thread_local PageRenderTiming* page_render_timing = nullptr;

struct PageRenderTiming {
    explicit PageRenderTiming(std::size_t glyphs)
        : previous(page_render_timing), start_us(esp_timer_get_time()), glyphs(glyphs) {
        page_render_timing = this;
    }

    ~PageRenderTiming() {
        const auto elapsed_us = esp_timer_get_time() - start_us;
        page_render_timing = previous;
        ESP_LOGI("reader_perf", "stage=page_render us=%lld min_us=0 glyph_prepare_us=%lld glyph_blend_us=%lld glyph_calls=%u glyphs=%u success=%d cancelled=%d",
            static_cast<long long>(elapsed_us), static_cast<long long>(glyph_prepare_us),
            static_cast<long long>(glyph_blend_us), static_cast<unsigned>(glyph_calls),
            static_cast<unsigned>(glyphs), success, cancelled);
    }

    PageRenderTiming* previous;
    std::int64_t start_us;
    std::int64_t glyph_prepare_us = 0;
    std::int64_t glyph_blend_us = 0;
    std::size_t glyph_calls = 0;
    std::size_t glyphs;
    bool success = false;
    bool cancelled = false;
};
}
#endif

struct Font::Stream {
    FT_StreamRec record{};
    std::function<bool(std::size_t, char*, std::size_t)> read;
};

struct Font::FileStream {
    static constexpr unsigned long window_bytes = 512;

    struct Window {
        unsigned long offset = 0;
        unsigned long size = 0;
        std::array<unsigned char, window_bytes> bytes;
    };

    FT_StreamRec record{};
    int fd = -1;
    off_t file_offset = 0;
    std::array<Window, 4> windows;
    std::size_t next_window = 0;

    ~FileStream() { if (fd >= 0) ::close(fd); }

    bool open(const std::string& path) {
        fd = ::open(path.c_str(), O_RDONLY);
        struct stat info{};
        if (fd < 0 || fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) || info.st_size <= 0 ||
            static_cast<std::uintmax_t>(info.st_size) > std::numeric_limits<unsigned long>::max()) return false;
        record.size = static_cast<unsigned long>(info.st_size);
        record.descriptor.pointer = this;
        record.read = [](FT_Stream stream, unsigned long offset, unsigned char* buffer, unsigned long count) -> unsigned long {
            if (!count) return offset > stream->size;
            return static_cast<FileStream*>(stream->descriptor.pointer)->read(offset, buffer, count);
        };
        return true;
    }

    unsigned long read_file(unsigned long offset, unsigned char* buffer, unsigned long count) {
        const auto position = static_cast<off_t>(offset);
        if (position != file_offset) {
            file_offset = lseek(fd, position, SEEK_SET);
            if (file_offset != position) return 0;
        }
        unsigned long filled = 0;
        while (filled < count) {
            const auto size = std::min<unsigned long>(count - filled, std::numeric_limits<ssize_t>::max());
            const auto received = ::read(fd, buffer + filled, size);
            if (received < 0 && errno == EINTR) continue;
            if (received < 0) file_offset = -1;
            if (received <= 0) break;
            file_offset += received;
            filled += static_cast<unsigned long>(received);
        }
        return filled;
    }

    unsigned long read(unsigned long offset, unsigned char* buffer, unsigned long count) {
        if (offset >= record.size) return 0;
        count = std::min(count, record.size - offset);
        if (count > window_bytes) return read_file(offset, buffer, count);

        unsigned long filled = 0;
        while (filled < count) {
            const auto position = offset + filled;
            const auto window_offset = position - position % window_bytes;
            auto window = std::find_if(windows.begin(), windows.end(), [window_offset](const auto& entry) {
                return entry.size && entry.offset == window_offset;
            });
            if (window == windows.end()) {
                window = windows.begin() + next_window;
                next_window = (next_window + 1) % windows.size();
                window->offset = window_offset;
                const auto size = std::min(window_bytes, record.size - window_offset);
                window->size = read_file(window_offset, window->bytes.data(), size);
            }
            const auto within = position - window_offset;
            if (within >= window->size) return filled + read_file(position, buffer + filled, count - filled);
            const auto size = std::min(count - filled, window->size - within);
            std::memcpy(buffer + filled, window->bytes.data() + within, size);
            filled += size;
        }
        return filled;
    }
};

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
    return heap_caps_realloc(block, static_cast<std::size_t>(size),
                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

// FreeType retains this descriptor for the lifetime of every library instance.
FT_MemoryRec_ font_memory{nullptr, font_allocate, font_free, font_reallocate};
}
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

void Canvas::blend_glyph(std::int64_t x, std::int64_t y, unsigned glyph_width, unsigned glyph_height,
                         const std::vector<std::uint8_t>& mask, std::uint8_t gray) {
    const auto left = std::max<std::int64_t>(0, x);
    const auto top = std::max<std::int64_t>(0, y);
    const auto right = std::min<std::int64_t>(width_, x + glyph_width);
    const auto bottom = std::min<std::int64_t>(height_, y + glyph_height);
    if (left >= right || top >= bottom || !glyph_width || glyph_height > mask.size() / glyph_width)
        return;

    const int clipped_width = static_cast<int>(right - left);
    const int clipped_height = static_cast<int>(bottom - top);
    const auto row_stride = static_cast<std::size_t>(stride());
    const auto first_byte = static_cast<std::size_t>(top) * row_stride + static_cast<std::size_t>(left) / 2;
    const auto mask_offset = static_cast<std::size_t>(top - y) * glyph_width + static_cast<std::size_t>(left - x);
    const auto ink = static_cast<unsigned>(gray & 15);
    const auto blend = [ink](unsigned old, unsigned coverage) {
        return (old * (255 - coverage) + ink * coverage + 127) / 255;
    };

    for (int row = 0; row < clipped_height; ++row) {
        auto* pixels = pixels_.data() + first_byte + static_cast<std::size_t>(row) * row_stride;
        const auto* coverage = mask.data() + mask_offset + static_cast<std::size_t>(row) * glyph_width;
        int remaining = clipped_width;
        if (left & 1) {
            if (*coverage) *pixels = static_cast<std::uint8_t>((*pixels & 15) | (blend(*pixels >> 4, *coverage) << 4));
            ++pixels;
            ++coverage;
            --remaining;
        }
        for (; remaining >= 2; remaining -= 2, coverage += 2, ++pixels) {
            const unsigned low = coverage[0], high = coverage[1];
            if (!(low | high)) continue;
            const unsigned old = *pixels;
            *pixels = static_cast<std::uint8_t>(blend(old & 15, low) | (blend(old >> 4, high) << 4));
        }
        if (remaining && *coverage)
            *pixels = static_cast<std::uint8_t>((*pixels & 0xf0) | blend(*pixels & 15, *coverage));
    }
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
    stream_.reset();
    file_stream_.reset();
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
    const auto library = static_cast<FT_Library>(library_);
    auto file = std::unique_ptr<FileStream>(new (std::nothrow) FileStream);
    if (file && file->open(path)) {
        FT_Open_Args args{};
        args.flags = FT_OPEN_STREAM;
        args.stream = &file->record;
        if (FT_Open_Face(library, &args, 0, &face) == 0) file_stream_ = std::move(file);
    }
    FT_Error opened = 0;
    if (!file_stream_) {
        file.reset();
        opened = FT_New_Face(library, path.c_str(), 0, &face);
    }
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

bool Font::open_stream(std::size_t bytes, std::function<bool(std::size_t, char*, std::size_t)> read) {
    close();
    unsigned char signature[4];
    if (!library_) return fail("FreeType initialization failed");
    if (!read || bytes < 12 || bytes > std::numeric_limits<FT_Long>::max() ||
        !read(0, reinterpret_cast<char*>(signature), sizeof(signature)) ||
        (std::memcmp(signature, "OTTO", 4) && std::memcmp(signature, "true", 4) &&
         std::memcmp(signature, "\x00\x01\x00\x00", 4)))
        return fail("Invalid embedded SFNT font");
    stream_.reset(new (std::nothrow) Stream{});
    if (!stream_) return fail("Cannot allocate embedded font stream");
    stream_->read = std::move(read);
    auto& record = stream_->record;
    record.size = bytes;
    record.descriptor.pointer = stream_.get();
    record.read = [](FT_Stream stream, unsigned long offset, unsigned char* buffer, unsigned long count) -> unsigned long {
        if (!count) return offset > stream->size;
        if (offset > stream->size || count > stream->size - offset) return 0;
        const auto& source = *static_cast<Stream*>(stream->descriptor.pointer);
        return source.read(offset, reinterpret_cast<char*>(buffer), count) ? count : 0;
    };
    FT_Open_Args args{};
    args.flags = FT_OPEN_STREAM;
    args.stream = &record;
    FT_Face face = nullptr;
    const auto opened = FT_Open_Face(static_cast<FT_Library>(library_), &args, 0, &face);
    if (opened) { close(); return fail("Cannot open embedded font stream", opened); }
    face_ = face;
    const auto mapped = FT_Select_Charmap(face, FT_ENCODING_UNICODE);
    if (mapped || !FT_IS_SCALABLE(face)) {
        close(); return fail("Embedded font must have scalable Unicode glyphs", mapped);
    }
    if (set_pixel_size(28)) return true;
    close();
    return false;
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
bool invisible_without_glyph(char32_t c) {
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
}

bool Font::contains(char32_t scalar) const {
    return face_ && FT_Get_Char_Index(static_cast<FT_Face>(face_), scalar) != 0;
}

bool Font::missing(char32_t scalar) const { return !contains(scalar) && !invisible_without_glyph(scalar); }

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
        width = invisible_without_glyph(scalar) ? 0 : cjk(scalar) ? pixel_size_ : (pixel_size_ + 1) / 2;
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

std::uint64_t Font::glyph_key(char32_t scalar, bool bold, bool italic, int weight_adjustment) const {
    if (!contains(scalar))
        return (std::uint64_t(pixel_size_) << 32) | (invisible_without_glyph(scalar) ? 0xfffffffdU : cjk(scalar) ? 0xffffffffU : 0xfffffffeU);
    const auto adjustment = std::clamp(weight_adjustment, -1, 1) + 1;
    return (std::uint64_t(bold) << 63) | (std::uint64_t(weight_) << 60) | (std::uint64_t(italic) << 59) |
        (std::uint64_t(adjustment) << 57) | (std::uint64_t(pixel_size_) << 32) | std::uint32_t(scalar);
}

bool Font::placeholder(char32_t scalar, Glyph& glyph) {
    // A font's .notdef glyph may be blank or a pattern rather than a missing-character box.
    const int reserved = advance(scalar), stroke = std::max(1, pixel_size_ / 16);
    if (reserved < 0) return false;
    const int width = std::min(reserved, pixel_size_) - 2 * std::max(1, pixel_size_ / 10);
    const int rows = pixel_size_ * 4 / 5;
    if (invisible_without_glyph(scalar) || width < 3 * stroke || rows < 3 * stroke) return true;
    glyph.left = (reserved - width) / 2; glyph.top = rows - pixel_size_ / 10;
    glyph.width = static_cast<unsigned>(width); glyph.rows = static_cast<unsigned>(rows);
    glyph.coverage.assign(static_cast<std::size_t>(width) * rows, 0);
    for (int row = 0; row < rows; ++row)
        for (int col = 0; col < width; ++col)
            if (row < stroke || row >= rows - stroke || col < stroke || col >= width - stroke)
                glyph.coverage[static_cast<std::size_t>(row) * width + col] = 255;
    return true;
}

bool Font::rasterize(char32_t scalar, bool bold, bool italic, int weight_adjustment, Glyph& glyph) {
    const auto loaded = FT_Load_Char(static_cast<FT_Face>(face_), scalar, FT_LOAD_DEFAULT | FT_LOAD_TARGET_NORMAL);
    if (loaded) return fail("Font glyph load failed", loaded, scalar);
    const auto slot = static_cast<FT_Face>(face_)->glyph;
    const int adjustment = weight_adjustment < 0 ? -pixel_size_ / 2 : weight_adjustment > 0 ? pixel_size_ : 0;
    const int strength = pixel_size_ * static_cast<int>(weight_) + adjustment;
    if (strength && slot->format == FT_GLYPH_FORMAT_OUTLINE) {
        const auto adjusted = FT_Outline_Embolden(&slot->outline, strength);
        if (adjusted && weight_) return fail("Font stroke adjustment failed", adjusted, scalar);
    }
    if (italic) FT_GlyphSlot_Oblique(slot);
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

bool Font::warm(char32_t scalar, bool bold, bool italic, int weight_adjustment) {
    if (scalar == U'\n' || scalar == U'\r' || scalar == U'\t') return true;
    if (!face_) return fail("Font face unavailable", 0, scalar);
    const auto key = glyph_key(scalar, bold, italic, weight_adjustment);
    auto cached = glyphs_.find(key);
    if (cached == glyphs_.end()) {
        Glyph glyph;
        if (!(contains(scalar) ? rasterize(scalar, bold, italic, weight_adjustment, glyph) : placeholder(scalar, glyph))) return false;
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

bool Font::draw(Canvas& canvas, char32_t scalar, int x, int baseline, std::uint8_t gray, bool bold, bool italic,
                int weight_adjustment) {
#ifdef CONFIG_PICO_PAGE_LEGACY_PERF_LOG
    auto* const timing = page_render_timing;
    const auto preparation_start_us = timing ? esp_timer_get_time() : 0;
#endif
    const bool prepared = warm(scalar, bold, italic, weight_adjustment);
#ifdef CONFIG_PICO_PAGE_LEGACY_PERF_LOG
    if (timing) {
        timing->glyph_prepare_us += esp_timer_get_time() - preparation_start_us;
        ++timing->glyph_calls;
    }
#endif
    if (!prepared) return false;
    if (scalar == U'\n' || scalar == U'\r' || scalar == U'\t') return true;
#ifdef CONFIG_PICO_PAGE_LEGACY_PERF_LOG
    const auto blend_start_us = timing ? esp_timer_get_time() : 0;
#endif
    const auto& glyph = glyphs_.at(glyph_key(scalar, bold, italic, weight_adjustment));
    canvas.blend_glyph(static_cast<std::int64_t>(x) + glyph.left, static_cast<std::int64_t>(baseline) - glyph.top,
                       glyph.width, glyph.rows, glyph.coverage, gray);
#ifdef CONFIG_PICO_PAGE_LEGACY_PERF_LOG
    if (timing) timing->glyph_blend_us += esp_timer_get_time() - blend_start_us;
#endif
    return true;
}

bool Font::ink_rows(char32_t scalar, int& above_baseline, int& rows, bool bold) {
    if (!warm(scalar, bold)) return false;
    const auto found = glyphs_.find(glyph_key(scalar, bold));
    if (found == glyphs_.end()) return false;
    above_baseline = found->second.top;
    rows = static_cast<int>(found->second.rows);
    return true;
}

namespace {
bool closing(char32_t c) { return std::u32string_view(U"，。！？、：；）】》」』〉…’”﹐﹑﹒﹔﹕﹖﹗﹚﹜﹞｡､｣,.!?;:)]}").find(c) != std::u32string_view::npos; }
bool opening(char32_t c) { return std::u32string_view(U"（【《「『〈‘“﹙﹛﹝｢([{").find(c) != std::u32string_view::npos; }
bool latin_letter(char32_t scalar) {
    return (scalar >= U'A' && scalar <= U'Z') || (scalar >= U'a' && scalar <= U'z') ||
           (scalar >= 0xc0 && scalar <= 0x24f && scalar != 0xd7 && scalar != 0xf7);
}

bool pattern_letter(char32_t scalar) {
    return (scalar >= U'A' && scalar <= U'Z') || (scalar >= U'a' && scalar <= U'z');
}

bool line_control(char32_t scalar) {
    return scalar == 0xad || scalar == 0x200b || scalar == 0x200c || scalar == 0x200d ||
           scalar == 0x2060 || scalar == 0xfeff;
}

bool continues_grapheme(char32_t previous, char32_t next) {
    return previous == 0x200d || next == 0x200c || next == 0x200d ||
           (next >= 0x300 && next <= 0x36f) || (next >= 0x1ab0 && next <= 0x1aff) ||
           (next >= 0x1dc0 && next <= 0x1dff) || (next >= 0x20d0 && next <= 0x20ff) ||
           (next >= 0xfe00 && next <= 0xfe0f) || (next >= 0xfe20 && next <= 0xfe2f) ||
           (next >= 0x1f3fb && next <= 0x1f3ff) || (next >= 0xe0100 && next <= 0xe01ef);
}

bool no_break_space(char32_t scalar) {
    return scalar == 0xa0 || scalar == 0x2007 || scalar == 0x202f;
}

bool glue(char32_t scalar) {
    return no_break_space(scalar) || scalar == 0x2011 || scalar == 0x2060 || scalar == 0xfeff;
}

bool break_after(char32_t previous, char32_t next) {
    if (glue(previous) || glue(next) || closing(next) || opening(previous) ||
        continues_grapheme(previous, next) || next == 0x200b || next == 0xad || previous == 0xad) return false;
    if (previous == 0x200b) return true;
    if (next == U'-' || (next >= 0x2010 && next <= 0x2013)) return false;
    if (previous == 0x2014 || next == 0x2014) return previous != next;
    return previous == U' ' || previous == U'\t' || previous == U'-' ||
        (previous >= 0x2010 && previous <= 0x2013) || cjk(previous) || cjk(next);
}

bool justification_gap(char32_t previous, char32_t next) {
    return (previous == U' ' || previous == U'\t' || cjk(previous) || cjk(next)) &&
        previous != U'-' && !line_control(previous) && !line_control(next);
}

bool extends_pattern_word(char32_t scalar) {
    return latin_letter(scalar) || (scalar >= U'0' && scalar <= U'9') ||
        scalar == U'\'' || scalar == 0x2019 || scalar == 0xad || scalar == 0x2060 ||
        continues_grapheme(0, scalar);
}

class LayoutText {
public:
    LayoutText(std::u32string_view text, const std::vector<std::uint32_t>* soft_line_breaks, bool starts_in_paragraph)
        : text_(text), soft_line_breaks_(soft_line_breaks), starts_in_paragraph_(starts_in_paragraph) {}

    char32_t at(std::size_t index) const {
        const auto scalar = text_[index];
        if (scalar == U'\r' && soft_break(index))
            return index + 1 < text_.size() && text_[index + 1] == U'\n' ? U'\r' : U' ';
        if (scalar == U'\n' && (soft_break(index) ||
              (index && text_[index - 1] == U'\r' && soft_break(index - 1)))) return U' ';
        return scalar;
    }

    bool suppressed(std::size_t index) const {
        return text_[index] == U'\r' && index + 1 < text_.size() &&
            text_[index + 1] == U'\n' && soft_break(index);
    }

    bool paragraph_start(std::size_t index) const {
        if (!index) return !starts_in_paragraph_;
        const auto previous = at(index - 1);
        return previous == U'\n' || (previous == U'\r' && !suppressed(index - 1));
    }

private:
    bool soft_break(std::size_t index) const {
        return soft_line_breaks_ && std::binary_search(soft_line_breaks_->begin(), soft_line_breaks_->end(), index);
    }

    std::u32string_view text_;
    const std::vector<std::uint32_t>* soft_line_breaks_;
    bool starts_in_paragraph_;
};

struct MeasuredLine {
    int width = 0, ascender = 0, descender = 0, height = 0, gaps = 0, font_px = 0;
    int trailing_space = 0, trailing_gaps = 0;
    int smallest_gap_px = std::numeric_limits<int>::max();
    int smallest_content_gap_px = std::numeric_limits<int>::max();
    std::size_t content_end = 0;
    bool latin = false, east_asian = false;

    static constexpr int comfortable_expansion_percent = 33;
    static constexpr int maximum_expansion_percent = 100;
    static constexpr int maximum_compression_percent = 20;

    void include(char32_t scalar, std::size_t index, int advance, const Font& font, const GlyphStyle& style,
                 bool gap, int space_width, bool inspect_script) {
        width += advance;
        if (inspect_script) {
            latin = latin || latin_letter(scalar);
            east_asian = east_asian || cjk(scalar);
        }
        if (gap) {
            ++gaps;
            smallest_gap_px = std::min(smallest_gap_px, space_width);
        }
        if (scalar == U' ') {
            trailing_space += advance;
            trailing_gaps += gap;
        } else {
            trailing_space = trailing_gaps = 0;
            content_end = index + 1;
            smallest_content_gap_px = smallest_gap_px;
        }
        const auto metrics = font.metrics();
        ascender = std::max(ascender, metrics.ascender + (style.bold ? 2 : 0));
        descender = std::max(descender, metrics.descender + (style.underline ? 2 : 0));
        height = std::max(height, style.line_height);
        font_px = std::max(font_px, style.font_px);
    }

    int content_gaps() const { return gaps - trailing_gaps; }
    int remaining_width(int column_width) const { return column_width - width + trailing_space; }

    static int gap_budget(int count, int natural_width_px, int percent) {
        return count ? count * (natural_width_px * percent / 100) : 0;
    }

    int content_gap_budget(int percent) const { return gap_budget(content_gaps(), smallest_content_gap_px, percent); }

    int compression_budget(bool add_gap, int space_width) const {
        return gap_budget(gaps + add_gap, add_gap ? std::min(smallest_gap_px, space_width) : smallest_gap_px,
                          maximum_compression_percent);
    }
};

struct LineEnd {
    std::size_t offset = 0;
    MeasuredLine line;
    std::optional<PositionedGlyph> hyphen;
};

}

Font& font_for_slot(Font& body, const AuthorFonts* fonts, std::uint8_t slot) {
    return fonts && slot && slot <= fonts->faces.size() && fonts->faces[slot - 1] ? *fonts->faces[slot - 1] : body;
}

Page layout_page(Font& font, std::u32string_view text, std::size_t start, const LayoutSpec& spec, bool collect,
                 const std::function<bool()>& stop, const StyledLayout* styles, const AuthorFonts* fonts,
                 const std::vector<std::uint32_t>* soft_line_breaks) {
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
    if (!styles && (metrics.ascender + metrics.descender > spec.line_height || spec.line_height > spec.height)) {
        page.error = "Line height cannot contain the complete font line"; return page;
    }
    const auto select_slot = [&](std::uint16_t families, char32_t scalar) -> std::uint8_t {
        for (; fonts && families; families >>= 4) {
            const auto slot = static_cast<std::uint8_t>(families & 15);
            auto& candidate = font_for_slot(font, fonts, slot);
            if (&candidate != &font && candidate.contains(scalar)) return slot;
        }
        return 0;
    };
    const auto prepare_size = [&](Font& face, int font_px) {
        if (face.pixel_size() == font_px || face.set_pixel_size(font_px)) return true;
        page.error = face.error();
        return false;
    };
    const auto glyph_style = [&](std::size_t index) {
        if (styles) return styles->at(index);
        GlyphStyle style{spec.font_px, spec.line_height};
        style.hyphenation = spec.hyphenation;
        style.english = spec.english;
        return style;
    };
    const LayoutText layout_text(text, spec.txt_reflow ? soft_line_breaks : nullptr, spec.starts_in_paragraph);
    const auto alignment = styles ? styles->align : spec.justify ? TextAlign::Justify : TextAlign::Start;
    const bool inspect_script = spec.limit_english_justification && alignment == TextAlign::Justify;
    const auto english_justification = [&](const MeasuredLine& candidate) {
        return inspect_script && candidate.latin && !candidate.east_asian;
    };
    const auto add_hyphen = [&](LineEnd& candidate, std::size_t source, const GlyphStyle& style) {
        constexpr char32_t hyphen = U'-';
        auto slot = select_slot(style.font_families, hyphen);
        const auto scalar = font_for_slot(font, fonts, slot).contains(hyphen) ? hyphen : U'\u2010';
        if (scalar != hyphen) slot = select_slot(style.font_families, scalar);
        auto& face = font_for_slot(font, fonts, slot);
        if (!prepare_size(face, style.font_px)) return false;
        const int advance = face.advance(scalar);
        if (advance < 0) { page.error = face.error(); return false; }
        const int compression = english_justification(candidate.line) ? candidate.line.compression_budget(false, 0) : 0;
        if (candidate.line.width + advance - spec.width > compression) return false;
        candidate.line.include(scalar, source, advance, face, style, false, 0, inspect_script);
        candidate.hyphen = PositionedGlyph{scalar, source, 0, 0, style.font_px, style.bold, advance,
                                         style.italic, style.underline, slot, true};
        return true;
    };
    const auto automatic_break = [&](std::size_t word_start, std::size_t overflow, const MeasuredLine& before_word,
                                     const GlyphStyle& style, bool leading_gap, int space_width, LineEnd& chosen) {
        constexpr std::size_t max_letters = 63;
        const std::size_t block_start = styles ? styles->block_start : 0;
        std::size_t begin = word_start;
        while (begin > block_start && word_start - begin < max_letters && pattern_letter(text[begin - 1])) --begin;
        if (begin > block_start && extends_pattern_word(text[begin - 1])) return;
        std::size_t finish = overflow;
        while (finish < text.size() && finish - begin < max_letters && pattern_letter(text[finish])) ++finish;
        if (finish == overflow || (finish < text.size() && extends_pattern_word(text[finish]))) return;
        for (std::size_t i = begin; i < finish; ++i) {
            const auto letter_style = glyph_style(i);
            if (letter_style.hidden || letter_style.omitted || letter_style.hyphenation != style.hyphenation ||
                letter_style.english != style.english) return;
        }
        const auto breaks = english_hyphenation_breaks(text.substr(begin, finish - begin), style.english);
        if (!breaks) return;
        if (stop && stop()) { page.error = "Layout cancelled"; return; }
        const bool normal_break = chosen.offset > 0;
        const bool sparse = chosen.line.remaining_width(spec.width) >
            chosen.line.content_gap_budget(MeasuredLine::maximum_expansion_percent);
        MeasuredLine candidate = before_word;
        for (std::size_t i = word_start; i < overflow; ++i) {
            const auto letter_style = glyph_style(i);
            auto& face = font_for_slot(font, fonts, select_slot(letter_style.font_families, text[i]));
            if (!prepare_size(face, letter_style.font_px)) return;
            const int advance = face.advance(text[i]);
            if (advance < 0) { page.error = face.error(); return; }
            candidate.include(text[i], i, advance, face, letter_style, i == word_start && leading_gap,
                              space_width, inspect_script);
            const auto offset = i + 1;
            if (offset <= chosen.offset || offset - word_start < 2 ||
                !(breaks & (std::uint64_t{1} << (offset - begin)))) continue;
            LineEnd hyphenated{offset, candidate, std::nullopt};
            if (add_hyphen(hyphenated, i, letter_style)) {
                const int remaining = hyphenated.line.remaining_width(spec.width);
                const int comfortable = hyphenated.line.content_gap_budget(MeasuredLine::comfortable_expansion_percent);
                const int maximum = hyphenated.line.content_gap_budget(MeasuredLine::maximum_expansion_percent);
                if (!normal_break || remaining <= comfortable || (sparse && remaining <= maximum))
                    chosen = std::move(hyphenated);
            }
            if (!page.error.empty()) return;
        }
    };
    std::size_t cursor = start;
    int top = spec.top;
    const auto none = text.size();
    while (cursor < text.size() && top + (styles ? 1 : spec.line_height) <= spec.top + spec.height) {
        if (stop && stop()) { page.error = "Layout cancelled"; break; }
        const bool paragraph_start = styles ? cursor == styles->block_start : layout_text.paragraph_start(cursor);
        const bool source_indented = text[cursor] == U'\u3000' || text[cursor] == U'\t';
        int indent = 0;
        if (paragraph_start) {
            if (styles) indent = styles->indent_px;
            else if (!source_indented) indent = spec.indent_chars * spec.font_px;
        }
        MeasuredLine line;
        line.width = indent;
        line.height = spec.line_height;
        LineEnd last_break, before_cluster;
        MeasuredLine before_word;
        std::size_t word_start = none;
        bool leading_word_gap = false;
        int preceding_word_space = 0;
        std::size_t indent_end = cursor;
        while (paragraph_start && indent_end < text.size() && (text[indent_end] == U'\u3000' || text[indent_end] == U'\t'))
            ++indent_end;
        std::size_t end = cursor, previous = none;
        char32_t prior = 0;
        int prior_width = 0;
        bool newline = false;
        while (end < text.size()) {
            if ((end - cursor) % 64 == 0 && stop && stop()) { page.error = "Layout cancelled"; break; }
            const auto format = glyph_style(end);
            if (format.omitted || layout_text.suppressed(end)) { ++end; continue; }
            const auto scalar = layout_text.at(end);
            if (scalar == U'\n' || scalar == U'\r') {
                ++end;
                if (scalar == U'\r' && end < text.size() && layout_text.at(end) == U'\n') ++end;
                newline = true;
                break;
            }
            const bool legal_break = previous != none && break_after(prior, scalar);
            if (legal_break) {
                last_break = {end, line, std::nullopt};
            } else if (prior == 0xad && !line.trailing_space &&
                       !glue(scalar) && !closing(scalar) && !continues_grapheme(prior, scalar)) {
                const auto format_before = glyph_style(previous);
                if (!format_before.hidden && format_before.hyphenation != Hyphenation::None && line.content_end > cursor) {
                    LineEnd candidate{end, line, std::nullopt};
                    if (add_hyphen(candidate, previous, format_before)) last_break = std::move(candidate);
                    if (!page.error.empty()) break;
                }
            }
            const bool gap = alignment == TextAlign::Justify && legal_break && previous >= indent_end &&
                justification_gap(prior, scalar);
            const int space_width = prior == U' ' ? prior_width : 0;
            if (format.hyphenation == Hyphenation::Auto && format.english != EnglishHyphenation::None &&
                pattern_letter(scalar) && !pattern_letter(prior)) {
                word_start = end;
                before_word = line;
                leading_word_gap = gap;
                preceding_word_space = space_width;
            }

            if (end + 1 < text.size() && (continues_grapheme(scalar, text[end + 1]) || text[end + 1] == 0xad) &&
                (previous == none || !continues_grapheme(prior, scalar)))
                before_cluster = {end, line, std::nullopt};
            if (line_control(scalar)) {
                previous = end++;
                prior = scalar;
                prior_width = 0;
            } else {
                auto& face = font_for_slot(font, fonts, select_slot(format.font_families, scalar));
                if (!prepare_size(face, format.font_px)) break;
                const int width = face.advance(scalar);
                if (width < 0) { page.error = face.error(); break; }
                const bool can_adjust = inspect_script && (line.latin || latin_letter(scalar)) &&
                    !line.east_asian && !cjk(scalar);
                if (line.width + width > spec.width && (!can_adjust ||
                    (scalar != U' ' && line.width + width - spec.width > line.compression_budget(gap, space_width)))) {
                    const bool poor_justify = english_justification(last_break.line) &&
                        last_break.line.remaining_width(spec.width) >
                            last_break.line.content_gap_budget(MeasuredLine::comfortable_expansion_percent);
                    if (word_start < end && pattern_letter(prior) && pattern_letter(scalar) &&
                        format.hyphenation == Hyphenation::Auto && format.english != EnglishHyphenation::None &&
                        (!last_break.offset || poor_justify)) {
                        automatic_break(word_start, end, before_word, format, leading_word_gap, preceding_word_space, last_break);
                    }
                    if (!last_break.offset && before_cluster.offset > cursor &&
                        (continues_grapheme(prior, scalar) || prior == 0xad))
                        last_break = std::move(before_cluster);
                    if (last_break.offset > cursor) { end = last_break.offset; line = last_break.line; }
                    break;
                }
                line.include(scalar, end, width, face, format, gap, space_width, inspect_script);
                previous = end++;
                prior = scalar;
                prior_width = width;
            }
            if (end - cursor > 65536) { page.error = "Too many scalars in one text line"; break; }
        }
        if ((newline || end == text.size()) && line.remaining_width(spec.width) < 0 && last_break.offset > cursor) {
            end = last_break.offset;
            line = last_break.line;
            newline = false;
        }
        if (!page.error.empty() || end == cursor) {
            if (page.error.empty()) page.error = "A glyph does not fit the text column";
            break;
        }
        if (!line.ascender && !line.descender) {
            line.ascender = metrics.ascender;
            line.descender = metrics.descender;
        }
        const int height = std::max(line.height, styles ? line.font_px : line.ascender + line.descender);
        if (top + height > spec.top + spec.height) break;
        const int baseline = top + (height - line.ascender - line.descender) / 2 + line.ascender;
        const int justify_gaps = line.content_gaps();
        const bool justify = alignment == TextAlign::Justify && !newline && end < text.size() && justify_gaps;
        const bool english_line = english_justification(line);
        const int remaining = justify
            ? (english_line ? std::min(line.remaining_width(spec.width),
                                       line.content_gap_budget(MeasuredLine::maximum_expansion_percent))
                            : std::max(0, line.remaining_width(spec.width)))
            : std::max(0, spec.width - line.width);
        int x = spec.left + indent;
        if (alignment == TextAlign::Center) x += remaining / 2;
        if (alignment == TextAlign::Right) x += remaining;
        int gaps = 0;
        previous = none;
        prior = 0;
        for (std::size_t i = cursor; collect && i < end; ++i) {
            if (layout_text.suppressed(i)) continue;
            const auto scalar = layout_text.at(i);
            if (scalar == U'\n' || scalar == U'\r') continue;
            const auto format = glyph_style(i);
            if (format.omitted) continue;
            if (line_control(scalar)) { previous = i; prior = scalar; continue; }
            const auto slot = select_slot(format.font_families, scalar);
            auto& face = font_for_slot(font, fonts, slot);
            if (!prepare_size(face, format.font_px)) break;
            const int advance = face.advance(scalar);
            if (advance < 0) { page.error = face.error(); break; }
            if (justify && i < line.content_end && previous != none && previous >= indent_end &&
                justification_gap(prior, scalar) && break_after(prior, scalar)) {
                const int adjustment = remaining * (gaps + 1) / justify_gaps - remaining * gaps / justify_gaps;
                if (english_line && prior == U' ' && !page.glyphs.empty() && page.glyphs.back().index == previous)
                    page.glyphs.back().advance += adjustment;
                x += adjustment;
                ++gaps;
            }
            if (scalar == U' ' && i >= line.content_end && english_line && x + advance > spec.left + spec.width)
                break;
            previous = i;
            prior = scalar;
            if (!format.hidden)
                page.glyphs.push_back({scalar, i, x, baseline, format.font_px, format.bold, advance,
                                      format.italic, format.underline, slot});
            x += advance;
        }
        if (!page.error.empty()) break;
        if (collect) {
            if (end < text.size() && end == last_break.offset && last_break.hyphen) {
                auto mark = *last_break.hyphen;
                mark.x = x;
                mark.baseline = baseline;
                page.glyphs.push_back(mark);
            }
            page.lines.push_back({cursor, end, top, baseline, top + height});
        }
        cursor = end;
        top += height + (newline ? spec.paragraph_gap : 0);
    }
    if (!page.error.empty()) { page.lines.clear(); page.glyphs.clear(); return page; }
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
            if (glyph.generated || glyph.index < line.start || glyph.index >= line.end || glyph.advance <= 0 ||
                invisible_without_glyph(glyph.scalar) || glyph.scalar == U' ' || glyph.scalar == U'\u00a0' ||
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
            if (glyph.generated || glyph.index < std::max(begin, line.start) || glyph.index >= std::min(end, line.end) ||
                glyph.advance <= 0 || invisible_without_glyph(glyph.scalar) || glyph.scalar == U'\ufffc') continue;
            left = present ? std::min(left, glyph.x) : glyph.x;
            right = std::max(right, glyph.x + glyph.advance);
            present = true;
        }
        if (present) boxes.push_back({left, line.top, right - left, line.bottom - line.top});
    }
    return boxes;
}

bool render_page(Canvas& canvas, Font& font, const Page& page, std::uint8_t gray, const std::function<bool()>& stop,
                 const AuthorFonts* fonts) {
#ifdef CONFIG_PICO_PAGE_LEGACY_PERF_LOG
    PageRenderTiming timing{page.glyphs.size()};
#endif
    const AuthorFontIo input{fonts};
    if (!page.error.empty() || !font.set_pixel_size(page.font_px)) return false;
    for (const auto& glyph : page.glyphs) {
        if (stop && stop()) {
#ifdef CONFIG_PICO_PAGE_LEGACY_PERF_LOG
            timing.cancelled = true;
#endif
            return false;
        }
        auto& face = font_for_slot(font, fonts, glyph.font_slot);
        if ((glyph.font_slot && &face == &font) ||
            (face.pixel_size() != glyph.font_px && !face.set_pixel_size(glyph.font_px)) ||
            !face.draw(canvas, glyph.scalar, glyph.x, glyph.baseline, gray, glyph.bold, glyph.italic, page.weight_adjustment)) return false;
        if (glyph.underline && glyph.advance > 0)
            canvas.fill_rect(glyph.x, glyph.baseline + 2, glyph.advance, 1, gray);
    }
    const bool rendered = font.pixel_size() == page.font_px || font.set_pixel_size(page.font_px);
#ifdef CONFIG_PICO_PAGE_LEGACY_PERF_LOG
    timing.success = rendered;
#endif
    return rendered;
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

}
