#include "pagination.h"
#include <algorithm>
#include <utility>
#ifdef ESP_PLATFORM
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
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
PaginationJob::PaginationJob(Request value) : request(std::move(value)) {}
PaginationJob::~PaginationJob() { cancel(); wait(); }
bool PaginationJob::start() {
    if (started_) return false;
#ifdef ESP_PLATFORM
    started_ = xTaskCreate([](void* opaque) {
        auto* job = static_cast<PaginationJob*>(opaque);
        job->run();
        ESP_LOGI("pagination", "stack_free=%u", static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
        job->done_.store(true, std::memory_order_release);
        vTaskDelete(nullptr);
    }, "pagination", 32768, this, 1, nullptr) == pdPASS;
#else
    thread_ = std::thread([this] { run(); done_.store(true, std::memory_order_release); });
    started_ = true;
#endif
    return started_;
}
void PaginationJob::wait() {
    if (!started_) return;
#ifdef ESP_PLATFORM
    while (!done()) vTaskDelay(1);
#else
    if (thread_.joinable()) thread_.join();
#endif
}
void PaginationJob::run() {
    result.offset = request.offset;
    const auto stop = [this] { return cancelled_.load(std::memory_order_relaxed); };
    if (stop()) return;
    if (request.identity_only) {
        result.font_identity = request.font.empty() ? graphics::Font::fallback_identity() : sha256_file(request.font, stop);
        if (result.font_identity.empty() && !stop()) result.error = "Font identity read failed";
        return;
    }
    EpubDocument document;
    graphics::Font font;
    Chapter source;
    if (!document.open_verified(request.path, request.identity, result.error) || stop()) return;
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
} // namespace pico_page
