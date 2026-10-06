#include "content_job.h"
#include "content_cache.h"
#include <sys/stat.h>
#include "pico_page/store.h"
#include <cstring>
#include <algorithm>
#include <utility>
#ifdef ESP_PLATFORM
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#endif

namespace pico_page {
std::size_t readable_offset(const Chapter& source, std::size_t start) {
    // Inserted block separators have no source anchor for a saved page start.
    Anchor anchor;
    while (start < source.text.size() && !source.anchor_at(static_cast<std::uint32_t>(start), anchor)) ++start;
    return start;
}
const DocumentImage* image_at(const Chapter& source, std::size_t start) {
    for (const auto& image : source.images)
        if (image.begin <= start && start < image.end && image.end <= source.text.size()) return &image;
    return nullptr;
}
graphics::Page plan_content(graphics::Font& font, const Chapter& source, std::size_t start,
                            const graphics::LayoutSpec& spec, bool collect, const std::function<bool()>& stop) {
    start = readable_offset(source, start);
    if (const auto* image = image_at(source, start)) {
        graphics::Page page;
        page.start = image->begin; page.end = image->end; page.font_px = spec.font_px;
        return page;
    }
    auto end = source.text.size();
    for (const auto& image : source.images)
        if (image.begin > start && image.begin < image.end && image.end <= source.text.size())
            end = std::min<std::size_t>(end, image.begin);
    return graphics::layout_page(font, std::u32string_view(source.text).substr(0, end), start, spec, collect, stop);
}
ContentJob::ContentJob(Request value) : request(std::move(value)) {}
ContentJob::~ContentJob() { cancel(); wait(); }
bool ContentJob::start() {
    if (started_) return false;
#ifdef ESP_PLATFORM
    started_ = xTaskCreate([](void* opaque) {
        auto* job = static_cast<ContentJob*>(opaque);
        job->run();
        ESP_LOGI("content_worker", "stack_free=%u", static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
        job->done_.store(true, std::memory_order_release);
        vTaskDelete(nullptr);
    }, "content_worker", 32768, this, 1, nullptr) == pdPASS;
#else
    thread_ = std::thread([this] { run(); done_.store(true, std::memory_order_release); });
    started_ = true;
#endif
    return started_;
}
void ContentJob::wait() {
    if (!started_) return;
#ifdef ESP_PLATFORM
    while (!done()) vTaskDelay(1);
#else
    if (thread_.joinable()) thread_.join();
#endif
}
void ContentJob::run() {
    result.offset = request.offset;
    const auto stop = [this] { return cancelled_.load(std::memory_order_relaxed); };
    if (stop()) return;
    if (request.kind == ContentJob::Kind::FontIdentity) {
        result.font_identity = request.font.empty() ? graphics::Font::fallback_identity() : sha256_file(request.font, stop);
        if (result.font_identity.empty() && !stop()) result.error = "Font identity read failed";
        return;
    }
    if (request.kind == Kind::Open) { open_book(); return; }
    if (request.kind == Kind::Image) { render_image(); return; }
    EpubDocument document;
    graphics::Font font;
    Chapter source;
    if (!document.open_verified(request.path, request.identity, result.error, stop) || stop()) return;
    if (!(request.font.empty() ? font.open_fallback() : font.open(request.font))) {
        result.error = font.error(); return;
    }
    if (stop()) return;
    if (!document.load_chapter(request.chapter, source, result.error, ChapterLoad::Layout, request.part, stop)) {
        if (result.error == kChapterCancelled) result.error.clear();
        return;
    }
    while (!stop()) {
        const auto start = readable_offset(source, result.offset);
        if (start >= source.text.size()) {
            result.part_done = true; result.last_part = source.last_part; return;
        }
        const auto page = plan_content(font, source, start, request.layout, false, stop);
        if (page.error == "Layout cancelled") return;
        if (!page.error.empty() || page.end <= page.start) {
            result.error = page.error.empty() ? "Pagination made no progress" : page.error; return;
        }
        result.starts.push_back(static_cast<std::uint32_t>(page.start));
        result.offset = page.end;
        // Bound the result independently of chapter length and font geometry.
        if (result.starts.size() >= 16384) return;
#ifdef ESP_PLATFORM
        taskYIELD();
#endif
    }
}
std::string image_cache_key(const std::string& book_id, const std::string& resource, const graphics::LayoutSpec& spec) {
    return "images/gray-v1/" + book_id + "/" + md5_hex(resource + ":" + std::to_string(spec.left) + ":" +
        std::to_string(spec.top) + ":" + std::to_string(spec.width) + ":" + std::to_string(spec.height));
}
void ContentJob::render_image() {
    const auto stop = [this] { return cancelled_.load(std::memory_order_relaxed); };
#ifdef ESP_PLATFORM
    if (heap_caps_get_free_size(MALLOC_CAP_SPIRAM) < 3U * 1024 * 1024 ||
        heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) < 2U * 1024 * 1024) {
        result.error = "Image preparation memory unavailable"; return;
    }
#endif
    if (stop()) return;
    result.image = std::make_unique<graphics::Canvas>();
    FileStore store(request.root);
    {
        std::string cached;
        const auto size = result.image->pixels().size();
        if (store.load_cache(request.cache_name, size + 32, cached) == StoreStatus::Ok && cached.size() == size + 32) {
            const auto checksum = cached.substr(size);
            cached.resize(size);
            if (md5_hex(cached) == checksum) {
                std::memcpy(result.image->data(), cached.data(), size);
                return;
            }
        }
    }
    if (stop()) return;
    EpubDocument document;
    std::vector<std::uint8_t> bytes;
    if (!document.open_verified(request.path, request.identity, result.error, stop) || stop()) return;
    if (!document.read_image(request.image_resource, bytes, result.error, stop)) return;
    const auto& spec = request.layout;
    if (!graphics::render_image(*result.image, bytes, spec.left, spec.top, spec.width, spec.height, result.error, false, stop)) return;
    std::vector<std::uint8_t>().swap(bytes);
    if (stop()) return;
    std::string pixels(reinterpret_cast<const char*>(result.image->data()), result.image->pixels().size());
    pixels += md5_hex(pixels);
    store.save_cache(request.cache_name, pixels);
}
void ContentJob::open_book() {
    const auto stop = [this] { return cancelled_.load(std::memory_order_relaxed); };
    struct stat before{}, after{};
    if (stat(request.path.c_str(), &before) != 0) { result.error = "Book file unavailable"; return; }
    result.document = std::make_unique<EpubDocument>();
    auto& document = *result.document;
    if (!(request.identity.book_id.empty() ? document.open(request.path, result.error, stop) :
          document.open_verified(request.path, request.identity, result.error, stop)) || stop()) return;
    FileStore store(request.root);
    result.configuration = store.load_settings(request.relative_path);
    result.progress = store.load_progress(request.relative_path, document.metadata().book_id, request.content_revision);
    if (result.progress.status != StoreStatus::Ok && result.progress.status != StoreStatus::Missing &&
        result.progress.status != StoreStatus::Cleared) {
        result.error = "Reading progress cannot be read; original record preserved"; return;
    }
    if (result.progress.record) {
        const auto& anchor = result.progress.record->anchor;
        std::uint32_t offset = 0;
        const auto match = [&](const Chapter& part) { return part.offset_for(anchor, offset); };
        if (!document.chapter_index_for(anchor.resource, result.chapter_index) ||
            !find_content(store, document, result.chapter_index, result.chapter, result.error, ChapterLoad::Reading, match, stop) ||
            !result.chapter.offset_for(anchor, offset)) {
            if (result.error.empty()) result.error = "Saved position cannot be mapped";
            return;
        }
        result.offset = offset;
    } else {
        while (result.chapter_index + 1 < document.spine().size() && !document.spine()[result.chapter_index].linear)
            ++result.chapter_index;
        if (!load_content(store, document, result.chapter_index, result.chapter, result.error, ChapterLoad::Reading, 0, stop)) return;
    }
    if (stop()) return;
    if (stat(request.path.c_str(), &after) != 0 || before.st_size != after.st_size ||
        before.st_mtime != after.st_mtime || before.st_ino != after.st_ino) {
        result.error = "Book changed while opening"; return;
    }
    result.file_size = after.st_size; result.file_modified = after.st_mtime; result.file_inode = after.st_ino;
}
} // namespace pico_page
