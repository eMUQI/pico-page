#include "pico_page/stage_timer.h"
#include "pico_page/graphics.h"
#include "pngle.h"
#include "tjpgd.h"
#include "JPEGDEC.h"

#include <algorithm>
#include <atomic>
#include <array>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <vector>
#ifdef ESP_PLATFORM
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#endif

namespace pico_page::graphics {
namespace {
std::atomic<std::uint64_t> decode_count{0};
constexpr std::size_t kEncodedLimit = 2 * 1024 * 1024;
constexpr std::uint64_t kPixelLimit = 8 * 1024 * 1024;

bool fail(std::string& error, ContentFailureKind* failure_kind, const std::string& message,
          ContentFailureKind kind = ContentFailureKind::InvalidFormat) {
    if (failure_kind) *failure_kind = kind;
    error = message;
    return false;
}
std::uint32_t be32(const std::uint8_t* p) {
    return std::uint32_t(p[0]) << 24 | std::uint32_t(p[1]) << 16 | std::uint32_t(p[2]) << 8 | p[3];
}
bool dimensions(unsigned width, unsigned height) {
    return width && height && width <= 4096 && height <= 4096 && std::uint64_t(width) * height <= kPixelLimit;
}
struct Raster {
    unsigned source_width = 0, source_height = 0;
    unsigned width = 0, height = 0, stride = 0;
    unsigned scaled_width = 0, scaled_height = 0, crop_x = 0, crop_y = 0;
    bool crop = false;
    bool keep_alpha = false, has_alpha = false;
    unsigned box_width = 0, box_height = 0;
    std::string layer;
    std::unique_ptr<std::uint8_t, decltype(&std::free)> pixels{nullptr, &std::free};
    std::uint64_t samples = 0;
    bool done = false;
    const std::function<bool()>* stop = nullptr;
    const std::function<void(int, int, int, int, std::uint8_t, std::uint8_t)>* paint = nullptr;
    bool cancelled() const { return stop && *stop && (*stop)(); }
    std::vector<std::uint16_t> columns, rows;
    bool create(unsigned source_w, unsigned source_h, int box_w, int box_h) {
        width = static_cast<unsigned>(box_w);
        height = std::max(1U, static_cast<unsigned>(std::uint64_t(source_h) * width / source_w));
        if ((!crop && height > static_cast<unsigned>(box_h)) ||
            (crop && height < static_cast<unsigned>(box_h))) {
            height = static_cast<unsigned>(box_h);
            width = std::max(1U, static_cast<unsigned>(std::uint64_t(source_w) * height / source_h));
        }
        scaled_width = width; scaled_height = height;
        if (crop) {
            width = static_cast<unsigned>(box_w); height = static_cast<unsigned>(box_h);
            crop_x = (scaled_width - width) / 2; crop_y = (scaled_height - height) / 2;
        }
        stride = (width + 1) / 2;
        if (paint) {
            box_width = static_cast<unsigned>(box_w); box_height = static_cast<unsigned>(box_h);
            source(source_w, source_h);
            return true;
        }
        if (keep_alpha) {
            box_width = static_cast<unsigned>(box_w); box_height = static_cast<unsigned>(box_h);
            layer.assign(std::size_t(box_width) * box_height * 2, '\0');
            source(source_w, source_h);
            return true;
        }
        pixels.reset(static_cast<std::uint8_t*>(std::malloc(std::size_t(stride) * height)));
        if (!pixels) return false;
        std::memset(pixels.get(), 0xff, std::size_t(stride) * height);
        source(source_w, source_h);
        return true;
    }
    void source(unsigned source_w, unsigned source_h) {
        source_width = source_w; source_height = source_h;
        columns.resize(source_w + 1);
        rows.resize(source_h + 1);
        for (unsigned x = 0; x <= source_w; ++x)
            columns[x] = static_cast<std::uint16_t>(mapped(x, scaled_width, source_w, crop_x, width));
        for (unsigned y = 0; y <= source_h; ++y)
            rows[y] = static_cast<std::uint16_t>(mapped(y, scaled_height, source_h, crop_y, height));
    }
    static unsigned mapped(unsigned coordinate, unsigned scaled, unsigned source, unsigned offset, unsigned limit) {
        const auto position = (std::uint64_t(coordinate) * scaled + source - 1) / source;
        return position <= offset ? 0 : static_cast<unsigned>(std::min<std::uint64_t>(position - offset, limit));
    }
    void sample(unsigned x, unsigned y, unsigned w, unsigned h, const std::uint8_t* rgb, unsigned alpha = 255) {
        ++samples;
#ifdef ESP_PLATFORM
        if ((samples & 65535U) == 0) vTaskDelay(1);
#endif
        if (x >= source_width || y >= source_height) return;
        has_alpha = has_alpha || alpha != 255;
        if (paint) {
            // Adam7 visits each real source pixel once. Expanded preview rectangles would blend twice.
            const auto left = columns[x], right = columns[x + 1], top = rows[y], bottom = rows[y + 1];
            if (left != right && top != bottom) {
                const auto gray = static_cast<std::uint8_t>((77U * rgb[0] + 150U * rgb[1] + 29U * rgb[2] + 128U) >> 8);
                (*paint)(left + (box_width - width) / 2, top + (box_height - height) / 2,
                         right - left, bottom - top, gray, static_cast<std::uint8_t>(alpha));
            }
            return;
        }
        const unsigned left = columns[x], right = columns[std::min(x + w, source_width)];
        const unsigned top = rows[y], bottom = rows[std::min(y + h, source_height)];
        if (left == right || top == bottom) return;
        const unsigned luminance = (77U * rgb[0] + 150U * rgb[1] + 29U * rgb[2] + 128U) >> 8;
        if (keep_alpha) {
            const unsigned ox = (box_width - width) / 2, oy = (box_height - height) / 2;
            for (unsigned row = top; row < bottom; ++row) for (unsigned col = left; col < right; ++col) {
                const auto at = (std::size_t(row + oy) * box_width + col + ox) * 2;
                layer[at] = static_cast<char>(luminance);
                layer[at + 1] = static_cast<char>(alpha);
            }
            return;
        }
        const unsigned blended = (luminance * alpha + 255U * (255U - alpha) + 127U) / 255U;
        const auto gray = static_cast<std::uint8_t>((blended * 15U + 127U) / 255U);
        for (unsigned row = top; row < bottom; ++row) for (unsigned col = left; col < right; ++col) {
            auto& value = pixels.get()[std::size_t(row) * stride + col / 2];
            value = col & 1 ? (value & 0x0f) | (gray << 4) : (value & 0xf0) | gray;
        }
    }
};

bool png(const std::vector<std::uint8_t>& input, int width, int height, Raster& raster, std::string& error, ContentFailureKind* failure_kind) {
    if (input.size() < 33 || be32(input.data() + 8) != 13 || std::memcmp(input.data() + 12, "IHDR", 4))
        return fail(error, failure_kind, "Invalid PNG header");
    const auto source_w = be32(input.data() + 16), source_h = be32(input.data() + 20);
    if (!dimensions(source_w, source_h)) return fail(error, failure_kind, "PNG exceeds 4096-pixel side or 8-megapixel limit");
    if (input[28] > 1) return fail(error, failure_kind, "Invalid PNG interlace method");
    bool ended = false;
    for (std::size_t at = 8; at < input.size();) {
        if (raster.cancelled()) return fail(error, failure_kind, kImageCancelled, ContentFailureKind::Cancelled);
        if (input.size() - at < 12) return fail(error, failure_kind, "Truncated PNG chunk");
        const auto length = be32(input.data() + at);
        if (length > input.size() - at - 12) return fail(error, failure_kind, "Truncated PNG data");
        const auto* type = input.data() + at + 4;
        if (!(type[0] & 32) && std::memcmp(type, "IHDR", 4) && std::memcmp(type, "PLTE", 4) &&
            std::memcmp(type, "IDAT", 4) && std::memcmp(type, "IEND", 4)) return fail(error, failure_kind, "Unsupported critical PNG chunk");
        if (std::memcmp(type, "IEND", 4) == 0) {
            if (length || at + 12 != input.size()) return fail(error, failure_kind, "Invalid PNG end marker");
            ended = true;
        }
        at += 12 + length;
    }
    if (!ended) return fail(error, failure_kind, "Missing PNG end marker");
    if (!raster.create(source_w, source_h, width, height)) return fail(error, failure_kind, "Image canvas allocation failed", ContentFailureKind::OutOfMemory);
    std::unique_ptr<pngle_t, decltype(&pngle_destroy)> decoder(pngle_new(), &pngle_destroy);
    if (!decoder) return fail(error, failure_kind, "PNG decoder allocation failed", ContentFailureKind::OutOfMemory);
    pngle_set_user_data(decoder.get(), &raster);
    pngle_set_draw_callback(decoder.get(), [](pngle_t* png, std::uint32_t x, std::uint32_t y, std::uint32_t w, std::uint32_t h, const std::uint8_t rgba[4]) {
        static_cast<Raster*>(pngle_get_user_data(png))->sample(x, y, w, h, rgba, rgba[3]);
    });
    pngle_set_done_callback(decoder.get(), [](pngle_t* png) { static_cast<Raster*>(pngle_get_user_data(png))->done = true; });
    std::size_t at = 0, window = 32;
    while (at < input.size()) {
        if (raster.cancelled()) return fail(error, failure_kind, kImageCancelled, ContentFailureKind::Cancelled);
        const auto count = std::min(window, input.size() - at);
        const int consumed = pngle_feed(decoder.get(), input.data() + at, count);
        if (consumed < 0) return fail(error, failure_kind, std::string("PNG decode failed: ") + pngle_error(decoder.get()),
            pngle_memory_error(decoder.get()) ? ContentFailureKind::OutOfMemory : ContentFailureKind::InvalidFormat);
        if (!consumed) {
            // Headers and palette chunks require contiguous bytes; retain the unconsumed prefix.
            if (count == input.size() - at) return fail(error, failure_kind, "Truncated PNG stream");
            window += 32;
        } else { at += static_cast<std::size_t>(consumed); window = 32; }
    }
    if (!raster.done || raster.samples != std::uint64_t(source_w) * source_h)
        return fail(error, failure_kind, "PNG pixel stream is incomplete");
    return true;
}

struct GifLzw {
    std::array<std::uint16_t, 4096> prefix{};
    std::array<std::uint8_t, 4096> suffix{}, stack{};
};
bool gif(const std::vector<std::uint8_t>& input, int width, int height, Raster& raster, std::string& error, ContentFailureKind* failure_kind) {
    const auto word = [&](std::size_t at) { return unsigned(input[at]) | unsigned(input[at + 1]) << 8; };
    if (input.size() < 13) return fail(error, failure_kind, "Truncated GIF header");
    const unsigned sw = word(6), sh = word(8);
    if (!dimensions(sw, sh)) return fail(error, failure_kind, "GIF exceeds 4096-pixel side or 8-megapixel limit");
    std::size_t at = 13;
    const std::uint8_t* global = nullptr;
    unsigned global_count = 0;
    if (input[10] & 128) {
        global_count = 2U << (input[10] & 7);
        if (global_count * 3 > input.size() - at) return fail(error, failure_kind, "Truncated GIF palette");
        global = input.data() + at; at += global_count * 3;
    }
    bool decoded = false;
    int transparent = -1;
    const auto blocks = [&]() {
        while (at < input.size()) {
            const auto count = input[at++];
            if (!count) return true;
            if (count > input.size() - at) return false;
            at += count;
        }
        return false;
    };
    while (at < input.size()) {
        if (raster.cancelled()) return fail(error, failure_kind, kImageCancelled, ContentFailureKind::Cancelled);
        const auto marker = input[at++];
        if (marker == 0x3b) return decoded && at == input.size() ? true : fail(error, failure_kind, "Invalid GIF trailer");
        if (marker == 0x21) {
            if (at == input.size()) return fail(error, failure_kind, "Truncated GIF extension");
            const auto type = input[at++];
            if (type == 0xf9) {
                if (input.size() - at < 6 || input[at] != 4 || input[at + 5]) return fail(error, failure_kind, "Invalid GIF graphic control");
                transparent = input[at + 1] & 1 ? input[at + 4] : -1;
                at += 6;
            } else if (!blocks()) return fail(error, failure_kind, "Truncated GIF extension");
            continue;
        }
        if (marker != 0x2c || input.size() - at < 9) return fail(error, failure_kind, "Invalid GIF image descriptor");
        const unsigned left = word(at), top = word(at + 2), fw = word(at + 4), fh = word(at + 6);
        const auto flags = input[at + 8]; at += 9;
        if (!fw || !fh || left > sw || fw > sw - left || top > sh || fh > sh - top || (flags & 0x18))
            return fail(error, failure_kind, "Invalid GIF frame bounds");
        const auto* palette = global;
        auto colors = global_count;
        if (flags & 128) {
            colors = 2U << (flags & 7);
            if (colors * 3 > input.size() - at) return fail(error, failure_kind, "Truncated GIF palette");
            palette = input.data() + at; at += colors * 3;
        }
        if (!palette || at == input.size()) return fail(error, failure_kind, "Missing GIF palette or image data");
        const unsigned minimum = input[at++];
        if (minimum < 2 || minimum > 8) return fail(error, failure_kind, "Invalid GIF LZW minimum code size");
        if (decoded) { if (!blocks()) return fail(error, failure_kind, "Truncated GIF animation frame"); continue; }
        if (!raster.create(sw, sh, width, height)) return fail(error, failure_kind, "Image canvas allocation failed", ContentFailureKind::OutOfMemory);
        if (transparent < 0 && global && input[11] < global_count) {
            const auto* rgb = global + input[11] * 3;
            const unsigned gray = (((77U * rgb[0] + 150U * rgb[1] + 29U * rgb[2] + 128U) >> 8) * 15U + 127U) / 255U;
            std::memset(raster.pixels.get(), gray | gray << 4, std::size_t(raster.stride) * raster.height);
        }
        std::unique_ptr<GifLzw> table(new (std::nothrow) GifLzw);
        if (!table) return fail(error, failure_kind, "GIF decoder allocation failed", ContentFailureKind::OutOfMemory);
        unsigned block_left = 0, buffer = 0, buffered_bits = 0;
        bool ended_blocks = false;
        const auto byte = [&]() -> int {
            if (!block_left) {
                if (at == input.size()) return -1;
                block_left = input[at++];
                if (!block_left) { ended_blocks = true; return -1; }
                if (block_left > input.size() - at) return -1;
            }
            --block_left; return input[at++];
        };
        const unsigned clear = 1U << minimum, end = clear + 1;
        unsigned next = end + 1, bits = minimum + 1, previous = 4096, first = 0;
        std::size_t pixels = 0;
        unsigned x = 0, y = 0, pass = 0;
        bool cleared = false, ended = false;
        const auto pixel = [&](unsigned index) {
            if (index >= colors || pixels >= std::size_t(fw) * fh || y >= fh) return false;
            raster.sample(left + x, top + y, 1, 1, palette + index * 3, static_cast<int>(index) == transparent ? 0 : 255);
            ++pixels;
            if (++x == fw) {
                x = 0;
                if (!(flags & 64)) ++y;
                else {
                    static constexpr unsigned starts[]{0, 4, 2, 1}, steps[]{8, 8, 4, 2};
                    y += steps[pass];
                    while (y >= fh && pass < 3) y = starts[++pass];
                }
            }
            return true;
        };
        for (;;) {
            if (raster.cancelled()) return fail(error, failure_kind, kImageCancelled, ContentFailureKind::Cancelled);
            while (buffered_bits < bits) {
                const auto value = byte();
                if (value < 0) return fail(error, failure_kind, "Truncated GIF LZW stream");
                buffer |= unsigned(value) << buffered_bits; buffered_bits += 8;
            }
            const unsigned code = buffer & ((1U << bits) - 1);
            buffer >>= bits; buffered_bits -= bits;
            if (code == clear) { next = end + 1; bits = minimum + 1; previous = 4096; cleared = true; continue; }
            if (!cleared) return fail(error, failure_kind, "GIF LZW stream has no initial clear code");
            if (code == end) { ended = true; break; }
            unsigned value = code, count = 0;
            if (code >= next) {
                if (code != next || previous == 4096) return fail(error, failure_kind, "Invalid GIF LZW code");
                table->stack[count++] = static_cast<std::uint8_t>(first); value = previous;
            }
            while (value >= clear) {
                if (value >= next || count >= table->stack.size()) return fail(error, failure_kind, "Invalid GIF LZW dictionary");
                table->stack[count++] = table->suffix[value]; value = table->prefix[value];
            }
            first = value;
            if (count >= table->stack.size()) return fail(error, failure_kind, "Invalid GIF LZW sequence");
            table->stack[count++] = static_cast<std::uint8_t>(first);
            while (count) if (!pixel(table->stack[--count])) return fail(error, failure_kind, "Invalid GIF pixel stream");
            if (previous != 4096 && next < 4096) {
                table->prefix[next] = static_cast<std::uint16_t>(previous);
                table->suffix[next++] = static_cast<std::uint8_t>(first);
                if (next == (1U << bits) && bits < 12) ++bits;
            }
            previous = code;
        }
        if (!ended || pixels != std::size_t(fw) * fh || ended_blocks || block_left > input.size() - at)
            return fail(error, failure_kind, "Incomplete GIF pixel stream");
        at += block_left;
        if (!blocks()) return fail(error, failure_kind, "Truncated GIF image terminator");
        decoded = true;
    }
    return fail(error, failure_kind, "Missing GIF trailer");
}

struct JpegInput {
    const std::vector<std::uint8_t>& bytes;
    std::size_t offset = 0;
    Raster& raster;
};
bool progressive_jpeg(const std::vector<std::uint8_t>& input, int width, int height,
                      Raster& raster, std::string& error, ContentFailureKind* failure_kind) {
    std::unique_ptr<JPEGDEC> decoder(new (std::nothrow) JPEGDEC);
    if (!decoder) return fail(error, failure_kind, "JPEG decoder allocation failed", ContentFailureKind::OutOfMemory);
    JpegInput io{input, 0, raster};
    const auto opened = decoder->open(&io, static_cast<int>(input.size()), nullptr,
        [](JPEGFILE* file, std::uint8_t* destination, std::int32_t count) -> std::int32_t {
            auto& io = *static_cast<JpegInput*>(file->fHandle);
            if (io.raster.cancelled() || count <= 0 || file->iPos < 0 ||
                static_cast<std::size_t>(file->iPos) > io.bytes.size()) return 0;
            const auto size = std::min<std::size_t>(count, io.bytes.size() - file->iPos);
            std::memcpy(destination, io.bytes.data() + file->iPos, size);
            file->iPos += static_cast<std::int32_t>(size);
            return static_cast<std::int32_t>(size);
        },
        [](JPEGFILE* file, std::int32_t position) -> std::int32_t {
            auto& io = *static_cast<JpegInput*>(file->fHandle);
            if (io.raster.cancelled() || position < 0 ||
                static_cast<std::size_t>(position) > io.bytes.size()) return -1;
            return file->iPos = position;
        },
        [](JPEGDRAW* draw) -> int {
            auto& raster = *static_cast<Raster*>(draw->pUser);
            if (raster.cancelled() || draw->iBpp != 8 || draw->x < 0 || draw->y < 0 ||
                draw->iWidth <= 0 || draw->iWidthUsed <= 0 || draw->iWidthUsed > draw->iWidth ||
                draw->iHeight <= 0 || static_cast<unsigned>(draw->x) >= raster.source_width ||
                static_cast<unsigned>(draw->y) >= raster.source_height) return 0;
            const auto columns = std::min<unsigned>(draw->iWidthUsed, raster.source_width - draw->x);
            const auto rows = std::min<unsigned>(draw->iHeight, raster.source_height - draw->y);
            const auto* pixels = reinterpret_cast<const std::uint8_t*>(draw->pPixels);
            for (unsigned y = 0; y < rows; ++y) for (unsigned x = 0; x < columns; ++x) {
                const auto gray = pixels[y * draw->iWidth + x];
                const std::uint8_t rgb[]{gray, gray, gray};
                raster.sample(draw->x + x, draw->y + y, 1, 1, rgb);
            }
            return 1;
        });
    if (raster.cancelled()) return fail(error, failure_kind, kImageCancelled, ContentFailureKind::Cancelled);
    if (!opened || decoder->getJPEGType() != JPEG_MODE_PROGRESSIVE)
        return fail(error, failure_kind, "JPEG header is invalid or unsupported",
            decoder->getLastError() == JPEG_ERROR_MEMORY ? ContentFailureKind::OutOfMemory : ContentFailureKind::InvalidFormat);
    const auto source_w = decoder->getWidth(), source_h = decoder->getHeight();
    if (!dimensions(source_w, source_h)) return fail(error, failure_kind, "JPEG exceeds 4096-pixel side or 8-megapixel limit");
    if (!raster.create(source_w, source_h, width, height)) return fail(error, failure_kind, "Image canvas allocation failed", ContentFailureKind::OutOfMemory);
    // JPEGDEC retains only DC coefficients, producing one pixel per 8-by-8 source block.
    raster.source((source_w + 7) / 8, (source_h + 7) / 8);
    decoder->setPixelType(EIGHT_BIT_GRAYSCALE);
    decoder->setMaxOutputSize(1);
    decoder->setUserPointer(&raster);
    const auto decoded = decoder->decode(0, 0, JPEG_SCALE_EIGHTH);
    if (raster.cancelled()) return fail(error, failure_kind, kImageCancelled, ContentFailureKind::Cancelled);
    if (!decoded || raster.samples != std::uint64_t(raster.source_width) * raster.source_height)
        return fail(error, failure_kind, "JPEG pixel stream is invalid or incomplete",
            decoder->getLastError() == JPEG_ERROR_MEMORY ? ContentFailureKind::OutOfMemory : ContentFailureKind::InvalidFormat);
    return true;
}
bool jpeg(const std::vector<std::uint8_t>& input, int width, int height, Raster& raster, std::string& error, ContentFailureKind* failure_kind) {
    JpegInput io{input, 0, raster};
    std::unique_ptr<void, decltype(&std::free)> workspace(std::malloc(8192), &std::free);
    if (!workspace) return fail(error, failure_kind, "JPEG decoder allocation failed", ContentFailureKind::OutOfMemory);
    JDEC decoder{};
    auto result = jd_prepare(&decoder, [](JDEC* decoder, std::uint8_t* destination, std::size_t count) -> std::size_t {
        auto& io = *static_cast<JpegInput*>(decoder->device);
        if (io.raster.cancelled()) return 0;
        count = std::min(count, io.bytes.size() - io.offset);
        if (destination) std::memcpy(destination, io.bytes.data() + io.offset, count);
        io.offset += count;
        return count;
    }, workspace.get(), 8192, &io);
    if (raster.cancelled()) return fail(error, failure_kind, kImageCancelled, ContentFailureKind::Cancelled);
    if (result == JDR_FMT3) {
        workspace.reset();
        return progressive_jpeg(input, width, height, raster, error, failure_kind);
    }
    if (result != JDR_OK) return fail(error, failure_kind, "JPEG header is invalid or unsupported",
        result == JDR_MEM1 || result == JDR_MEM2 ? ContentFailureKind::OutOfMemory : ContentFailureKind::InvalidFormat);
    if (!dimensions(decoder.width, decoder.height)) return fail(error, failure_kind, "JPEG exceeds 4096-pixel side or 8-megapixel limit");
    if (!raster.create(decoder.width, decoder.height, width, height)) return fail(error, failure_kind, "Image canvas allocation failed", ContentFailureKind::OutOfMemory);
    unsigned scale = 0;
    while (scale < 3 && static_cast<unsigned>(decoder.width >> (scale + 1)) >= raster.width &&
           static_cast<unsigned>(decoder.height >> (scale + 1)) >= raster.height) ++scale;
    raster.source(decoder.width >> scale, decoder.height >> scale);
    result = jd_decomp(&decoder, [](JDEC* decoder, void* bitmap, JRECT* rect) -> int {
        auto& raster = static_cast<JpegInput*>(decoder->device)->raster;
        if (raster.cancelled()) return 0;
        const auto* rgb = static_cast<const std::uint8_t*>(bitmap);
        for (unsigned row = rect->top; row <= rect->bottom; ++row)
            for (unsigned col = rect->left; col <= rect->right; ++col, rgb += 3)
                raster.sample(col, row, 1, 1, rgb);
        return 1;
    }, static_cast<std::uint8_t>(scale));
    if (raster.cancelled()) return fail(error, failure_kind, kImageCancelled, ContentFailureKind::Cancelled);
    if (result != JDR_OK || raster.samples != std::uint64_t(raster.source_width) * raster.source_height)
        return fail(error, failure_kind, "JPEG pixel stream is invalid or incomplete");
    return true;
}
}

std::uint64_t image_decode_count() { return decode_count.load(std::memory_order_relaxed); }

bool decode_png_layer(const std::vector<std::uint8_t>& encoded, int width, int height,
                      std::string& layer, bool& has_alpha, std::string& error, const std::function<bool()>& stop, ContentFailureKind* failure_kind) {
    const StageTimer timer{"image_decode_layer", 0};
    decode_count.fetch_add(1, std::memory_order_relaxed);
    layer.clear(); has_alpha = false; error.clear();
    if (failure_kind) *failure_kind = ContentFailureKind::None;
    if (stop && stop()) return fail(error, failure_kind, kImageCancelled, ContentFailureKind::Cancelled);
    if (width <= 0 || height <= 0 || width > 684 || height > 1216 ||
        encoded.size() < 8 || encoded.size() > kEncodedLimit ||
        std::memcmp(encoded.data(), "\x89PNG\r\n\x1a\n", 8)) return fail(error, failure_kind, "Invalid PNG layer input");
    Raster raster;
    raster.keep_alpha = true; raster.stop = &stop;
    if (!png(encoded, width, height, raster, error, failure_kind)) return false;
    if (raster.cancelled()) return fail(error, failure_kind, kImageCancelled, ContentFailureKind::Cancelled);
    has_alpha = raster.has_alpha;
    layer = std::move(raster.layer);
    return true;
}

bool decode_png_stream(std::size_t size,
                       const std::function<bool(std::size_t, char*, std::size_t)>& read,
                       int width, int height,
                       const std::function<void(int, int, int, int, std::uint8_t, std::uint8_t)>& paint,
                       ImageBox& painted, bool& has_alpha, std::string& error, const std::function<bool()>& stop, ContentFailureKind* failure_kind) {
    const StageTimer timer{"image_decode_stream", 0};
    decode_count.fetch_add(1, std::memory_order_relaxed);
    has_alpha = false; painted = {}; error.clear();
    if (failure_kind) *failure_kind = ContentFailureKind::None;
    if (stop && stop()) return fail(error, failure_kind, kImageCancelled, ContentFailureKind::Cancelled);
    if (!read || !paint || width <= 0 || height <= 0 || width > 684 || height > 1216 ||
        size < 33 || size > kEncodedLimit) return fail(error, failure_kind, "Invalid PNG stream input");
    std::array<std::uint8_t, 33> header{};
    if (!read(0, reinterpret_cast<char*>(header.data()), header.size())) return fail(error, failure_kind, "PNG read failed", ContentFailureKind::IoError);
    if (std::memcmp(header.data(), "\x89PNG\r\n\x1a\n", 8) || be32(header.data() + 8) != 13 ||
        std::memcmp(header.data() + 12, "IHDR", 4) || header[28] > 1) return fail(error, failure_kind, "Invalid PNG header");
    const auto sw = be32(header.data() + 16), sh = be32(header.data() + 20);
    if (!dimensions(sw, sh)) return fail(error, failure_kind, "PNG exceeds 4096-pixel side or 8-megapixel limit");
    bool ended = false;
    for (std::size_t at = 8; at < size;) {
        if (stop && stop()) return fail(error, failure_kind, kImageCancelled, ContentFailureKind::Cancelled);
        if (size - at < 12) return fail(error, failure_kind, "Truncated PNG chunk");
        if (!read(at, reinterpret_cast<char*>(header.data()), 8)) return fail(error, failure_kind, "PNG read failed", ContentFailureKind::IoError);
        const auto length = be32(header.data());
        const auto* type = header.data() + 4;
        if (length > size - at - 12) return fail(error, failure_kind, "Truncated PNG data");
        if (!(type[0] & 32) && std::memcmp(type, "IHDR", 4) && std::memcmp(type, "PLTE", 4) &&
            std::memcmp(type, "IDAT", 4) && std::memcmp(type, "IEND", 4))
            return fail(error, failure_kind, "Unsupported critical PNG chunk");
        if (!std::memcmp(type, "IEND", 4)) {
            if (length || at + 12 != size) return fail(error, failure_kind, "Invalid PNG end marker");
            ended = true;
        }
        at += 12 + length;
    }
    if (!ended) return fail(error, failure_kind, "Missing PNG end marker");
    Raster raster; raster.stop = &stop; raster.paint = &paint;
    if (!raster.create(sw, sh, width, height)) return fail(error, failure_kind, "Image canvas allocation failed", ContentFailureKind::OutOfMemory);
    painted.x = (width - raster.width) / 2; painted.y = (height - raster.height) / 2;
    painted.width = raster.width; painted.height = raster.height;
    std::unique_ptr<pngle_t, decltype(&pngle_destroy)> decoder(pngle_new(), &pngle_destroy);
    if (!decoder) return fail(error, failure_kind, "PNG decoder allocation failed", ContentFailureKind::OutOfMemory);
    pngle_set_user_data(decoder.get(), &raster);
    pngle_set_draw_callback(decoder.get(), [](pngle_t* png, std::uint32_t x, std::uint32_t y,
                                             std::uint32_t w, std::uint32_t h, const std::uint8_t rgba[4]) {
        static_cast<Raster*>(pngle_get_user_data(png))->sample(x, y, w, h, rgba, rgba[3]);
    });
    pngle_set_done_callback(decoder.get(), [](pngle_t* png) { static_cast<Raster*>(pngle_get_user_data(png))->done = true; });
    std::array<char, 4096> input{};
    std::size_t at = 0, buffered = 0;
    while (at < size || buffered) {
        if (stop && stop()) return fail(error, failure_kind, kImageCancelled, ContentFailureKind::Cancelled);
        const auto count = std::min(input.size() - buffered, size - at);
        if (count && !read(at, input.data() + buffered, count)) return fail(error, failure_kind, "PNG read failed", ContentFailureKind::IoError);
        at += count; buffered += count;
        const int consumed = pngle_feed(decoder.get(), input.data(), buffered);
        if (consumed < 0) return fail(error, failure_kind, std::string("PNG decode failed: ") + pngle_error(decoder.get()),
            pngle_memory_error(decoder.get()) ? ContentFailureKind::OutOfMemory : ContentFailureKind::InvalidFormat);
        if (!consumed) return fail(error, failure_kind, "Truncated PNG stream");
        buffered -= static_cast<std::size_t>(consumed);
        std::memmove(input.data(), input.data() + consumed, buffered);
    }
    if (stop && stop()) return fail(error, failure_kind, kImageCancelled, ContentFailureKind::Cancelled);
    if (!raster.done || raster.samples != std::uint64_t(sw) * sh) return fail(error, failure_kind, "PNG pixel stream is incomplete");
    has_alpha = raster.has_alpha;
    return true;
}

bool composite_image_layer(Canvas& canvas, std::string_view layer, int first_row, int row_count) {
    if (canvas.width() <= 0 || canvas.width() > 684 || canvas.height() <= 0 || canvas.height() > 1216 ||
        layer.size() != std::size_t(canvas.width()) * canvas.height() * 2 ||
        first_row < 0 || row_count < 0 || first_row > canvas.height() || row_count > canvas.height() - first_row) return false;
    const auto stride = std::size_t(canvas.width()) * 2;
    return composite_image_rows(canvas, layer.substr(first_row * stride, row_count * stride), first_row, row_count);
}

bool composite_image_rows(Canvas& canvas, std::string_view rows, int first_row, int row_count) {
    if (canvas.width() <= 0 || canvas.width() > 684 || canvas.height() <= 0 || canvas.height() > 1216 ||
        first_row < 0 || row_count < 0 || first_row > canvas.height() || row_count > canvas.height() - first_row ||
        rows.size() != std::size_t(canvas.width()) * row_count * 2) return false;
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(rows.data());
    for (int y = first_row; y < first_row + row_count; ++y) for (int x = 0; x < canvas.width(); ++x) {
        const auto at = (std::size_t(y - first_row) * canvas.width() + x) * 2;
        const unsigned alpha = bytes[at + 1];
        if (!alpha) continue;
        const unsigned mixed = (bytes[at] * alpha + canvas.pixel(x, y) * 17U * (255U - alpha) + 127U) / 255U;
        canvas.pixel(x, y, static_cast<std::uint8_t>((mixed * 15U + 127U) / 255U));
    }
    return true;
}

bool render_image(Canvas& canvas, const std::vector<std::uint8_t>& encoded,
                  int x, int y, int width, int height, std::string& error, bool crop, const std::function<bool()>& stop, ContentFailureKind* failure_kind) {
    const StageTimer timer{"image_decode_draw", 0};
    decode_count.fetch_add(1, std::memory_order_relaxed);
    error.clear();
    if (failure_kind) *failure_kind = ContentFailureKind::None;
    if (stop && stop()) return fail(error, failure_kind, kImageCancelled, ContentFailureKind::Cancelled);
    if (x < 0 || y < 0 || width <= 0 || height <= 0 || width > canvas.width() || height > canvas.height() ||
        x > canvas.width() - width || y > canvas.height() - height || std::uint64_t(width) * height > 684U * 1216U)
        return fail(error, failure_kind, "Invalid image drawing area");
    if (encoded.empty() || encoded.size() > kEncodedLimit) return fail(error, failure_kind, "Encoded image exceeds 2 MiB limit");
    Raster raster;
    raster.crop = crop;
    raster.stop = &stop;
    bool success;
    if (encoded.size() >= 8 && std::memcmp(encoded.data(), "\x89PNG\r\n\x1a\n", 8) == 0)
        success = png(encoded, width, height, raster, error, failure_kind);
    else if (encoded.size() >= 3 && encoded[0] == 0xff && encoded[1] == 0xd8 && encoded[2] == 0xff)
        success = jpeg(encoded, width, height, raster, error, failure_kind);
    else if (encoded.size() >= 6 && (!std::memcmp(encoded.data(), "GIF87a", 6) || !std::memcmp(encoded.data(), "GIF89a", 6)))
        success = gif(encoded, width, height, raster, error, failure_kind);
    else return fail(error, failure_kind, "Image format is not PNG, JPEG or GIF");
    if (!success) return false;
    if (raster.cancelled()) return fail(error, failure_kind, kImageCancelled, ContentFailureKind::Cancelled);
    x += (width - static_cast<int>(raster.width)) / 2;
    y += (height - static_cast<int>(raster.height)) / 2;
    for (unsigned row = 0; row < raster.height; ++row) {
        for (unsigned col = 0; col < raster.width; ++col) {
            const auto packed = raster.pixels.get()[std::size_t(row) * raster.stride + col / 2];
            canvas.pixel(x + col, y + row, col & 1 ? packed >> 4 : packed & 15);
        }
        if (raster.cancelled()) return fail(error, failure_kind, kImageCancelled, ContentFailureKind::Cancelled);
    }
    return true;
}
}
