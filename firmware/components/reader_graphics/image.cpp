#include "pico_page/stage_timer.h"
#include "pico_page/graphics.h"
#include "pngle.h"
#include "tjpgd.h"
#include "JPEGDEC.h"

#include <algorithm>
#include <atomic>
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

bool fail(std::string& error, const std::string& message) { error = message; return false; }
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
    std::unique_ptr<std::uint8_t, decltype(&std::free)> pixels{nullptr, &std::free};
    std::uint64_t samples = 0;
    bool done = false;
    const std::function<bool()>* stop = nullptr;
    bool cancelled() const { return stop && *stop && (*stop)(); }
    // Destination edge of every source column and row; sample() runs once per source pixel.
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
        pixels.reset(static_cast<std::uint8_t*>(std::malloc(std::size_t(stride) * height)));
        if (!pixels) return false;
        std::memset(pixels.get(), 0xff, std::size_t(stride) * height);
        source(source_w, source_h);
        return true;
    }
    /** Sets the decoded size, which a prescaling decoder reduces after create(). */
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
        // Each destination pixel samples its source coordinate; Adam7 updates are progressively refined.
        const unsigned left = columns[x], right = columns[std::min(x + w, source_width)];
        const unsigned top = rows[y], bottom = rows[std::min(y + h, source_height)];
        if (left == right || top == bottom) return;
        const unsigned luminance = (77U * rgb[0] + 150U * rgb[1] + 29U * rgb[2] + 128U) >> 8;
        const unsigned blended = (luminance * alpha + 255U * (255U - alpha) + 127U) / 255U;
        const auto gray = static_cast<std::uint8_t>((blended * 15U + 127U) / 255U);
        for (unsigned row = top; row < bottom; ++row) for (unsigned col = left; col < right; ++col) {
            auto& value = pixels.get()[std::size_t(row) * stride + col / 2];
            value = col & 1 ? (value & 0x0f) | (gray << 4) : (value & 0xf0) | gray;
        }
    }
};

bool png(const std::vector<std::uint8_t>& input, int width, int height, Raster& raster, std::string& error) {
    if (input.size() < 33 || be32(input.data() + 8) != 13 || std::memcmp(input.data() + 12, "IHDR", 4))
        return fail(error, "Invalid PNG header");
    const auto source_w = be32(input.data() + 16), source_h = be32(input.data() + 20);
    if (!dimensions(source_w, source_h)) return fail(error, "PNG exceeds 4096-pixel side or 8-megapixel limit");
    if (input[28] > 1) return fail(error, "Invalid PNG interlace method");
    bool ended = false;
    for (std::size_t at = 8; at < input.size();) {
        if (raster.cancelled()) return fail(error, kImageCancelled);
        if (input.size() - at < 12) return fail(error, "Truncated PNG chunk");
        const auto length = be32(input.data() + at);
        if (length > input.size() - at - 12) return fail(error, "Truncated PNG data");
        const auto* type = input.data() + at + 4;
        if (!(type[0] & 32) && std::memcmp(type, "IHDR", 4) && std::memcmp(type, "PLTE", 4) &&
            std::memcmp(type, "IDAT", 4) && std::memcmp(type, "IEND", 4)) return fail(error, "Unsupported critical PNG chunk");
        if (std::memcmp(type, "IEND", 4) == 0) {
            if (length || at + 12 != input.size()) return fail(error, "Invalid PNG end marker");
            ended = true;
        }
        at += 12 + length;
    }
    if (!ended) return fail(error, "Missing PNG end marker");
    if (!raster.create(source_w, source_h, width, height)) return fail(error, "Image canvas allocation failed");
    std::unique_ptr<pngle_t, decltype(&pngle_destroy)> decoder(pngle_new(), &pngle_destroy);
    if (!decoder) return fail(error, "PNG decoder allocation failed");
    pngle_set_user_data(decoder.get(), &raster);
    pngle_set_draw_callback(decoder.get(), [](pngle_t* png, std::uint32_t x, std::uint32_t y, std::uint32_t w, std::uint32_t h, const std::uint8_t rgba[4]) {
        static_cast<Raster*>(pngle_get_user_data(png))->sample(x, y, w, h, rgba, rgba[3]);
    });
    pngle_set_done_callback(decoder.get(), [](pngle_t* png) { static_cast<Raster*>(pngle_get_user_data(png))->done = true; });
    std::size_t at = 0, window = 32;
    while (at < input.size()) {
        if (raster.cancelled()) return fail(error, kImageCancelled);
        const auto count = std::min(window, input.size() - at);
        const int consumed = pngle_feed(decoder.get(), input.data() + at, count);
        if (consumed < 0) return fail(error, std::string("PNG decode failed: ") + pngle_error(decoder.get()));
        if (!consumed) {
            // Headers and palette chunks require contiguous bytes; retain the unconsumed prefix.
            if (count == input.size() - at) return fail(error, "Truncated PNG stream");
            window += 32;
        } else { at += static_cast<std::size_t>(consumed); window = 32; }
    }
    if (!raster.done || raster.samples != std::uint64_t(source_w) * source_h)
        return fail(error, "PNG pixel stream is incomplete");
    return true;
}

struct JpegInput {
    const std::vector<std::uint8_t>& bytes;
    std::size_t offset = 0;
    Raster& raster;
};
bool progressive_jpeg(const std::vector<std::uint8_t>& input, int width, int height,
                      Raster& raster, std::string& error) {
    std::unique_ptr<JPEGDEC> decoder(new (std::nothrow) JPEGDEC);
    if (!decoder) return fail(error, "JPEG decoder allocation failed");
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
    if (raster.cancelled()) return fail(error, kImageCancelled);
    if (!opened || decoder->getJPEGType() != JPEG_MODE_PROGRESSIVE)
        return fail(error, "JPEG header is invalid or unsupported");
    const auto source_w = decoder->getWidth(), source_h = decoder->getHeight();
    if (!dimensions(source_w, source_h)) return fail(error, "JPEG exceeds 4096-pixel side or 8-megapixel limit");
    if (!raster.create(source_w, source_h, width, height)) return fail(error, "Image canvas allocation failed");
    // JPEGDEC retains only DC coefficients, producing one pixel per 8-by-8 source block.
    raster.source((source_w + 7) / 8, (source_h + 7) / 8);
    decoder->setPixelType(EIGHT_BIT_GRAYSCALE);
    decoder->setMaxOutputSize(1);
    decoder->setUserPointer(&raster);
    const auto decoded = decoder->decode(0, 0, JPEG_SCALE_EIGHTH);
    if (raster.cancelled()) return fail(error, kImageCancelled);
    if (!decoded || raster.samples != std::uint64_t(raster.source_width) * raster.source_height)
        return fail(error, "JPEG pixel stream is invalid or incomplete");
    return true;
}
bool jpeg(const std::vector<std::uint8_t>& input, int width, int height, Raster& raster, std::string& error) {
    JpegInput io{input, 0, raster};
    std::unique_ptr<void, decltype(&std::free)> workspace(std::malloc(8192), &std::free);
    if (!workspace) return fail(error, "JPEG decoder allocation failed");
    JDEC decoder{};
    auto result = jd_prepare(&decoder, [](JDEC* decoder, std::uint8_t* destination, std::size_t count) -> std::size_t {
        auto& io = *static_cast<JpegInput*>(decoder->device);
        if (io.raster.cancelled()) return 0;
        count = std::min(count, io.bytes.size() - io.offset);
        if (destination) std::memcpy(destination, io.bytes.data() + io.offset, count);
        io.offset += count;
        return count;
    }, workspace.get(), 8192, &io);
    if (raster.cancelled()) return fail(error, kImageCancelled);
    if (result == JDR_FMT3) {
        workspace.reset();
        return progressive_jpeg(input, width, height, raster, error);
    }
    if (result != JDR_OK) return fail(error, "JPEG header is invalid or unsupported");
    if (!dimensions(decoder.width, decoder.height)) return fail(error, "JPEG exceeds 4096-pixel side or 8-megapixel limit");
    if (!raster.create(decoder.width, decoder.height, width, height)) return fail(error, "Image canvas allocation failed");
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
    if (raster.cancelled()) return fail(error, kImageCancelled);
    if (result != JDR_OK || raster.samples != std::uint64_t(raster.source_width) * raster.source_height)
        return fail(error, "JPEG pixel stream is invalid or incomplete");
    return true;
}
} // namespace

std::uint64_t image_decode_count() { return decode_count.load(std::memory_order_relaxed); }

bool render_image(Canvas& canvas, const std::vector<std::uint8_t>& encoded,
                  int x, int y, int width, int height, std::string& error, bool crop, const std::function<bool()>& stop) {
    const StageTimer timer{"image_decode_draw"};
    decode_count.fetch_add(1, std::memory_order_relaxed);
    error.clear();
    if (stop && stop()) return fail(error, kImageCancelled);
    if (x < 0 || y < 0 || width <= 0 || height <= 0 || width > canvas.width() || height > canvas.height() ||
        x > canvas.width() - width || y > canvas.height() - height || std::uint64_t(width) * height > 684U * 1216U)
        return fail(error, "Invalid image drawing area");
    if (encoded.empty() || encoded.size() > kEncodedLimit) return fail(error, "Encoded image exceeds 2 MiB limit");
    Raster raster;
    raster.crop = crop;
    raster.stop = &stop;
    bool success;
    if (encoded.size() >= 8 && std::memcmp(encoded.data(), "\x89PNG\r\n\x1a\n", 8) == 0)
        success = png(encoded, width, height, raster, error);
    else if (encoded.size() >= 3 && encoded[0] == 0xff && encoded[1] == 0xd8 && encoded[2] == 0xff)
        success = jpeg(encoded, width, height, raster, error);
    else return fail(error, "Image format is not PNG or JPEG");
    if (!success) return false;
    if (raster.cancelled()) return fail(error, kImageCancelled);
    x += (width - static_cast<int>(raster.width)) / 2;
    y += (height - static_cast<int>(raster.height)) / 2;
    for (unsigned row = 0; row < raster.height; ++row) for (unsigned col = 0; col < raster.width; ++col) {
        const auto packed = raster.pixels.get()[std::size_t(row) * raster.stride + col / 2];
        canvas.pixel(x + col, y + row, col & 1 ? packed >> 4 : packed & 15);
    }
    return true;
}
} // namespace pico_page::graphics
