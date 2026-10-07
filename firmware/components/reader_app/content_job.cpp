#include "content_job.h"
#include "content_cache.h"
#include "pico_page/sync.h"
#include "pico_page/stage_timer.h"
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
    if ((request.kind == Kind::Pagination || request.kind == Kind::FontIdentity || request.kind == Kind::LibraryCovers ||
         request.kind == Kind::PageCacheRead || request.kind == Kind::PageCacheWrite || request.kind == Kind::SleepCover || request.prefetch) &&
        heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) < 64U * 1024) return false;
    started_ = xTaskCreate([](void* opaque) {
        auto* job = static_cast<ContentJob*>(opaque);
        job->run();
        ESP_LOGI("content_worker", "stack_free=%u kind=%d psram_free=%u psram_min=%u", static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)),
                 static_cast<int>(job->request.kind), static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
                 static_cast<unsigned>(heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM)));
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
    if (request.kind == Kind::SaveProgress) {
        const StageTimer timer{"progress_commit"};
        FileStore store(request.root);
        if (request.record) result.progress = store.save_progress(request.relative_path, *request.record);
        if (request.save_last_book && result.progress.status == StoreStatus::Ok)
            result.last_book_saved = store.save_last_book(request.relative_path, result.error) == StoreStatus::Ok;
        return;
    }
    if (stop()) return;
    if (request.kind == Kind::PageCacheRead || request.kind == Kind::PageCacheWrite) {
        FileStore store(request.root);
        if (request.kind == Kind::PageCacheRead)
            result.page_cache = read_page_cache(store, request.cache_name, request.cache_header, request.spine, stop);
        else if (request.page_cache)
            write_page_cache(store, request.cache_name, request.cache_header, request.page_cache->pages, request.page_cache->counted, stop);
        result.cache_finished = !stop();
        return;
    }
    if (request.kind == ContentJob::Kind::FontIdentity) {
        if (request.font_scan && request.font_scan->step(64U * 1024, stop)) {
            result.font_identity = request.font_scan->digest();
            if (result.font_identity.empty()) result.error = "Font identity read failed";
        }
        return;
    }
    if (request.kind == Kind::MapUpload || request.kind == Kind::MapDownload) { map_position(); return; }
    if (request.kind == Kind::SleepCover) { prepare_cover(); result.cache_finished = !stop(); return; }
    if (request.kind == Kind::LibraryCovers) { library_covers(); return; }
    if (request.kind == Kind::Chapter) {
        load_chapter();
        if (request.prefetch && !stop() && result.error.empty()) prepare_chapter_page();
        return;
    }
    if (request.kind == Kind::Open || request.kind == Kind::Inspect) { open_book(); return; }
    if (request.kind == Kind::Image) { render_image(request.image_resource, request.cache_name); return; }
    EpubDocument document;
    graphics::Font font;
    Chapter source;
    if (!document.open_verified(request.path, request.identity, result.error, stop) || stop()) {
        // A cancelled open reports an error text; the next counting job repeats it, so it is not a failure.
        if (stop()) result.error.clear();
        return;
    }
    if (!(request.font.empty() ? font.open_fallback() : font.open(request.font))) {
        result.error = font.error(); return;
    }
    if (stop()) return;
    if (!load_content(FileStore(request.root), document, request.chapter, source, result.error, ChapterLoad::Layout, request.part, stop)) {
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
        // Yield between small batches so font verification can also advance while reading.
        if (result.starts.size() >= 8) return;
#ifdef ESP_PLATFORM
        taskYIELD();
#endif
    }
}
std::string image_cache_key(const std::string& book_id, const std::string& resource, const graphics::LayoutSpec& spec) {
    return "images/gray-v1/" + book_id + "/" + md5_hex(resource + ":" + std::to_string(spec.left) + ":" +
        std::to_string(spec.top) + ":" + std::to_string(spec.width) + ":" + std::to_string(spec.height));
}
void ContentJob::render_image(const std::string& resource, const std::string& cache_name) {
    const auto stop = [this] { return cancelled_.load(std::memory_order_relaxed); };
#ifdef ESP_PLATFORM
    if (heap_caps_get_free_size(MALLOC_CAP_SPIRAM) < (request.prefetch ? 5U : 3U) * 1024 * 1024 ||
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
        if (store.load_cache(cache_name, size + 32, cached) == StoreStatus::Ok && cached.size() == size + 32) {
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
    if (!document.read_image(resource, bytes, result.error, stop)) return;
    const auto& spec = request.layout;
    if (!graphics::render_image(*result.image, bytes, spec.left, spec.top, spec.width, spec.height, result.error, false, stop)) return;
    std::vector<std::uint8_t>().swap(bytes);
    if (stop()) return;
    std::string pixels(reinterpret_cast<const char*>(result.image->data()), result.image->pixels().size());
    pixels += md5_hex(pixels);
    store.save_cache(cache_name, pixels);
}
void ContentJob::map_position() {
    const StageTimer timer{"position_mapping"};
    const auto stop = [this] { return cancelled_.load(std::memory_order_relaxed); };
    EpubDocument document;
    if (!document.open_verified(request.path, request.identity, result.error, stop)) return;
    if (request.kind == Kind::MapDownload) {
        sync::from_xpointer(document, request.xpointer, request.dom_version, result.mapped_anchor, result.error, stop);
        return;
    }
    if (!request.record) { result.error = "Saved record unavailable"; return; }
    Chapter source;
    std::size_t index;
    std::uint32_t offset = 0;
    const auto& anchor = request.record->anchor;
    if (!document.chapter_index_for(anchor.resource, index) ||
        !document.find_chapter_part(index, source, result.error, ChapterLoad::Mapping,
            [&](const Chapter& part) { return part.offset_for(anchor, offset); }, stop) ||
        !sync::to_xpointer(document, source, anchor, request.dom_version, result.xpointer, result.error)) {
        if (result.error.empty()) result.error = "Saved position cannot be mapped";
        return;
    }
    if (stop()) { result.error = kChapterCancelled; return; }
    result.percentage = (static_cast<double>(index) +
        (source.text.empty() ? 0. : static_cast<double>(offset) / source.text.size())) / document.spine().size();
}
void ContentJob::prepare_cover() {
    const auto stop = [this] { return cancelled_.load(std::memory_order_relaxed); };
    FileStore store(request.root);
    const auto width = request.layout.width, height = request.layout.height;
    const auto count = static_cast<std::size_t>((width + 1) / 2) * height;
    std::string cached;
    if (store.load_cache(request.cache_name, request.cache_header.size() + count, cached) == StoreStatus::Ok &&
        cached.compare(0, request.cache_header.size(), request.cache_header) == 0) {
        if (cached.size() == request.cache_header.size()) return;
        if (cached.size() == request.cache_header.size() + count) {
            result.image = std::make_unique<graphics::Canvas>(width, height);
            std::memcpy(result.image->data(), cached.data() + request.cache_header.size(), count); return;
        }
    }
    if (stop()) return;
#ifdef ESP_PLATFORM
    if (heap_caps_get_free_size(MALLOC_CAP_SPIRAM) < 3U * 1024 * 1024 ||
        heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) < 2U * 1024 * 1024) return;
#endif
    std::vector<std::uint8_t> encoded;
    std::string error;
    auto image = std::make_unique<graphics::Canvas>(width, height);
    const bool loaded = EpubDocument::read_cover(request.path, encoded, error, stop) &&
        graphics::render_image(*image, encoded, 0, 0, width, height, error, request.cover_crop, stop);
    if (stop()) return;
    if (loaded) {
        store.save_cache(request.cache_name, request.cache_header + std::string(image->pixels().begin(), image->pixels().end()));
        result.image = std::move(image);
    } else if (error.find("allocation") == std::string::npos) store.save_cache(request.cache_name, request.cache_header);
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
    if (request.kind != Kind::Inspect && result.progress.status != StoreStatus::Ok && result.progress.status != StoreStatus::Missing &&
        result.progress.status != StoreStatus::Cleared) {
        result.error = "Reading progress cannot be read; original record preserved"; return;
    }
    if (request.kind == Kind::Inspect) prepare_cover();
    else if (result.progress.record) {
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
void ContentJob::load_chapter() {
    const auto stop = [this] { return cancelled_.load(std::memory_order_relaxed); };
    EpubDocument document;
    FileStore store(request.root);
    result.chapter_index = request.chapter;
    if (!document.open_verified(request.path, request.identity, result.error, stop) || stop()) return;
    std::uint32_t position = 0;
    if (request.target == Target::Anchor || request.target == Target::Fragment) {
        const auto match = [&](const Chapter& part) {
            return request.target == Target::Anchor ? part.offset_for(request.anchor, position) :
                part.fragment_offset(request.fragment, position);
        };
        if (request.target == Target::Fragment && request.fragment.empty()) {
            if (!load_content(store, document, request.chapter, result.chapter, result.error, ChapterLoad::Reading, 0, stop)) return;
        } else if (!find_content(store, document, request.chapter, result.chapter, result.error, ChapterLoad::Reading, match, stop)) return;
        result.offset = position;
        if (request.target == Target::Fragment && result.offset >= result.chapter.text.size() && !result.chapter.last_part) {
            const auto next = result.chapter.part + 1;
            if (!load_content(store, document, request.chapter, result.chapter, result.error, ChapterLoad::Reading, next, stop)) return;
            result.offset = 0;
        }
        return;
    }
    if (!load_content(store, document, request.chapter, result.chapter, result.error, ChapterLoad::Reading, request.part, stop)) return;
    if (request.target == Target::Start) return;
    if (request.known_start && *request.known_start < result.chapter.text.size()) { result.offset = *request.known_start; return; }
    graphics::Font font;
    if (!(request.font.empty() ? font.open_fallback() : font.open(request.font))) { result.error = font.error(); return; }
    const auto limit = request.target == Target::End ? result.chapter.text.size() : request.offset;
    std::size_t start = 0;
    result.offset = 0;
    while (start < limit && !stop()) {
        const auto page = plan_content(font, result.chapter, start, request.layout, false, stop);
        if (!page.error.empty() || page.end <= start) {
            result.error = page.error.empty() ? "Pagination made no progress" : page.error;
            return;
        }
        if (page.start >= limit) break;
        result.offset = page.start;
        start = readable_offset(result.chapter, page.end);
        if (result.history.size() == 64) result.history.erase(result.history.begin());
        result.history.emplace_back(static_cast<std::uint32_t>(page.start), static_cast<std::uint32_t>(start));
    }
}
void ContentJob::prepare_chapter_page() {
    const auto stop = [this] { return cancelled_.load(std::memory_order_relaxed); };
    graphics::Font font;
    const auto* image = image_at(result.chapter, readable_offset(result.chapter, result.offset));
    if (!image && !(request.font.empty() ? font.open_fallback() : font.open(request.font))) {
        result.error = font.error(); return;
    }
    result.page = plan_content(font, result.chapter, result.offset, request.layout, true, stop);
    const auto layout_bytes = result.page.glyphs.capacity() * sizeof(graphics::PositionedGlyph) +
        result.page.lines.capacity() * sizeof(graphics::TextLine);
    if (!result.page.error.empty() || result.page.end <= result.page.start || layout_bytes > 256U * 1024 ||
        chapter_cache_bytes(result.chapter) > 2U * 1024 * 1024) {
        result.error = "Prefetched page layout failed"; return;
    }
    if (image) render_image(image->resource, image_cache_key(request.identity.book_id, image->resource, request.layout));
    else {
        result.image = std::make_unique<graphics::Canvas>();
        if (!graphics::render_page(*result.image, font, result.page, 1, stop)) {
            result.error = "Prefetched page rasterization failed"; result.image.reset();
        }
    }
}
void ContentJob::library_covers() {
    const StageTimer timer{"library_covers"};
    const auto stop = [this] { return cancelled_.load(std::memory_order_relaxed); };
    FileStore store(request.root);
    for (const auto& source : request.covers) {
        if (stop()) return;
        result.covers.emplace_back();
#ifdef ESP_PLATFORM
        if (heap_caps_get_free_size(MALLOC_CAP_SPIRAM) < 3U * 1024 * 1024) continue;
#endif
        graphics::Canvas thumbnail(164, 184);
        std::vector<std::uint8_t> encoded;
        std::string error;
        const bool loaded = EpubDocument::read_cover(request.root + "/" + source.path, encoded, error, stop) &&
            graphics::render_image(thumbnail, encoded, 0, 0, 164, 184, error, false, stop);
        if (stop()) return;
        struct stat info{};
        if (stat((request.root + "/" + source.path).c_str(), &info) != 0 ||
            info.st_size != source.size || info.st_mtime != source.modified) continue;
        if (loaded) result.covers.back() = thumbnail.pixels();
        if (loaded || error.find("allocation") == std::string::npos)
            store.save_cache(source.cache_name, source.cache_header +
                std::string(result.covers.back().begin(), result.covers.back().end()));
    }
}

} // namespace pico_page
