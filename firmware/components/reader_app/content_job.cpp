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
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#else
#include <system_error>
#endif

namespace pico_page {
namespace {
#ifdef ESP_PLATFORM
constexpr unsigned content_stack_bytes = 32768;
QueueHandle_t content_requests = nullptr;
void log_worker_memory(const char* phase, ContentJob::Kind kind) {
    constexpr auto internal = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    ESP_LOGW("content_worker", "phase=%s kind=%d stack_bytes=%u internal_free=%u internal_largest=%u internal_min=%u psram_free=%u psram_largest=%u psram_min=%u",
             phase, static_cast<int>(kind), content_stack_bytes,
             static_cast<unsigned>(heap_caps_get_free_size(internal)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(internal)),
             static_cast<unsigned>(heap_caps_get_minimum_free_size(internal)),
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)),
             static_cast<unsigned>(heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM)));
}
#else
thread_local ContentJob::Kind failed_kind = ContentJob::Kind::MapDownload;
thread_local unsigned failed_starts = 0;
thread_local ContentJob::StartFailure injected_failure = ContentJob::StartFailure::TaskCreate;
#endif
} // namespace
#ifndef ESP_PLATFORM
void ContentJob::fail_starts_for_test(Kind kind, unsigned count, StartFailure failure) {
    failed_kind = kind; failed_starts = count; injected_failure = failure;
}
#endif
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
    const bool styled = !source.images.empty() || std::any_of(source.blocks.begin(), source.blocks.end(),
        [](const TextBlock& block) { return block.kind == BlockKind::Heading; });
    if (!styled) return graphics::layout_page(font, source.text, start, spec, collect, stop);
    graphics::Page page;
    page.start = page.end = start;
    page.font_px = spec.font_px;
    int top = spec.top;
    const int bottom = spec.top + spec.height;
    std::size_t cursor = start;
    while (cursor < source.text.size() && top < bottom) {
        if (stop && stop()) { page.error = "Layout cancelled"; break; }
        const auto block = std::lower_bound(source.blocks.begin(), source.blocks.end(), cursor,
            [](const TextBlock& item, std::size_t offset) { return item.end <= offset; });
        if (block == source.blocks.end()) break;
        cursor = std::max<std::size_t>(cursor, block->begin);
        if (const auto* image = image_at(source, cursor)) {
            const bool image_only = source.blocks.size() == 1 && source.images.size() == 1;
            const int height = image_only ? spec.height : std::min(spec.height / 2, 480);
            const int gap = image_only ? 0 : std::min(16, spec.font_px / 3);
            if (top + height + (top > spec.top ? gap : 0) > bottom && page.end > page.start) break;
            if (top > spec.top) top += gap;
            if (collect) page.images.push_back({image->begin, spec.left, top, spec.width,
                                                 std::min(height, bottom - top), image->resource});
            top += std::min(height, bottom - top) + gap;
            page.end = image->end;
            cursor = readable_offset(source, image->end);
            continue;
        }
        const bool heading = block->kind == BlockKind::Heading;
        graphics::LayoutSpec part = spec;
        if (heading) {
            part.font_px = std::min(96, std::max(spec.font_px + 2, (spec.font_px * 23 + 10) / 20));
            part.line_height = (spec.line_height * part.font_px + spec.font_px - 1) / spec.font_px;
            part.indent_chars = 0;
            part.paragraph_gap = 0;
        }
        const int before = heading && top > spec.top ? spec.font_px / 2 : 0;
        if (top + before + part.line_height > bottom && page.end > page.start) break;
        part.top = top + before;
        part.height = bottom - part.top;
        auto section = graphics::layout_page(font, std::u32string_view(source.text).substr(0, block->end),
                                             cursor, part, collect, stop);
        if (!section.error.empty()) { page.error = section.error; break; }
        if (section.end <= cursor) {
            if (page.end == page.start) page.error = "A text line does not fit the page";
            break;
        }
        const bool complete = section.end == block->end;
        const int after = heading ? spec.font_px / 3 : spec.paragraph_gap;
        if (heading && page.end > page.start &&
            (!complete || (std::next(block) != source.blocks.end() &&
                           section.next_top + after + spec.line_height > bottom))) break;
        if (heading) for (auto& glyph : section.glyphs) glyph.bold = true;
        page.lines.insert(page.lines.end(), section.lines.begin(), section.lines.end());
        page.glyphs.insert(page.glyphs.end(), section.glyphs.begin(), section.glyphs.end());
        page.end = section.end;
        top = section.next_top;
        if (!complete) break;
        top += after;
        cursor = readable_offset(source, section.end);
    }
    page.next_top = top;
    return page;
}
ContentJob::ContentJob(Request value) : request(std::move(value)) {}
ContentJob::~ContentJob() { cancel(); wait(); }
#ifdef ESP_PLATFORM
ContentJob::StartFailure ContentJob::prepare_worker() {
    if (content_requests) return StartFailure::None;
    auto queue = xQueueCreate(1, sizeof(ContentJob*));
    if (!queue) return StartFailure::InternalMemory;
    const auto created = xTaskCreate([](void* opaque) {
        const auto requests = static_cast<QueueHandle_t>(opaque);
        for (;;) {
            ContentJob* job = nullptr;
            if (xQueueReceive(requests, &job, portMAX_DELAY) != pdTRUE) continue;
            job->run();
            // Publishing completion relinquishes the job; its owner may immediately destroy it.
            job->done_.store(true, std::memory_order_release);
        }
    }, "content_worker", content_stack_bytes, queue, 1, nullptr);
    if (created != pdPASS) {
        vQueueDelete(queue);
        return heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) < content_stack_bytes
            ? StartFailure::InternalMemory : StartFailure::TaskCreate;
    }
    content_requests = queue;
    return StartFailure::None;
}
#endif
bool ContentJob::start() {
    if (started_) return false;
    PP_DIAGNOSTIC(
        measurement_ = diagnostics::begin(diagnostics::Kind::Content,
            {request.diagnostic_session, request.diagnostic_layout, 0, request.generation,
             static_cast<std::uint32_t>(request.kind)});
        struct StartGuard {
            ContentJob& job;
            ~StartGuard() {
                if (!job.started_) diagnostics::end(job.measurement_, diagnostics::Outcome::Failure);
            }
        } guard{*this};
    )
    start_failure_ = StartFailure::None;
#ifdef ESP_PLATFORM
    // Optional work retains the same headroom after accounting for the already reserved stack.
    const auto required_internal = 32U * 1024 + (content_requests ? 0 : content_stack_bytes);
    if ((request.kind == Kind::Pagination || request.kind == Kind::FontIdentity || request.kind == Kind::LibraryCovers ||
         request.kind == Kind::PageCacheRead || request.kind == Kind::PageCacheWrite || request.kind == Kind::SleepCover || request.prefetch) &&
        heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) < required_internal) {
        start_failure_ = StartFailure::InternalMemory;
        return false;
    }
    start_failure_ = prepare_worker();
    if (start_failure_ == StartFailure::None) {
        auto* job = this;
        started_ = xQueueSend(content_requests, &job, 0) == pdTRUE;
        if (!started_) start_failure_ = StartFailure::TaskCreate;
    }
    if (!started_) {
        log_worker_memory("create_failed", request.kind);
    }
#else
    if (failed_starts && request.kind == failed_kind) {
        --failed_starts; start_failure_ = injected_failure; return false;
    }
    try {
        thread_ = std::thread([this] { run(); done_.store(true, std::memory_order_release); });
    } catch (const std::system_error&) {
        start_failure_ = StartFailure::TaskCreate; return false;
    }
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
#ifdef CONFIG_PICO_PAGE_PERF_METRICS
void ContentJob::run() {
    execute();
    const bool failed = !result.error.empty() || !result.failed_images.empty() ||
        (request.kind == Kind::SaveProgress && result.progress.status != StoreStatus::Ok) ||
        (request.kind == Kind::SaveBookmarks && result.bookmark_status != StoreStatus::Ok);
    diagnostics::end(measurement_, cancelled_.load() ? diagnostics::Outcome::Cancelled :
        failed ? diagnostics::Outcome::Failure : diagnostics::Outcome::Success);
}
void ContentJob::execute() {
#else
void ContentJob::run() {
#endif
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
    if (request.kind == Kind::SaveBookmarks) {
        // Not cancellable: a started commit runs to its result so the owner learns what the card holds.
        const StageTimer timer{"bookmark_commit"};
        FileStore store(request.root);
        if (!request.bookmark_base) { result.bookmark_status = StoreStatus::Invalid; return; }
        result.bookmark_status = store.save_bookmarks(request.relative_path, request.identity.book_id, request.content_revision,
                                                      *request.bookmark_base, request.bookmarks, request.bookmark_next, result.bookmarks);
        if (result.bookmark_status == StoreStatus::ExternalChange)
            result.bookmarks = store.load_bookmarks(request.relative_path, request.identity.book_id, request.content_revision);
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
    if (request.kind == Kind::SleepCover) { prepare_cover(); return; }
    if (request.kind == Kind::LibraryCovers) { library_covers(); return; }
    if (request.kind == Kind::Chapter) {
        load_chapter();
        return;
    }
    if (request.kind == Kind::Open || request.kind == Kind::Inspect) { open_book(); return; }
    if (request.kind == Kind::Image || request.kind == Kind::ImageViewer) { render_image(); return; }
    const auto session = request.session ? request.session : std::make_shared<PaginationSession>();
    auto& document = session->document;
    auto& font = session->font;
    auto& source = session->chapter;
    if (session->path != request.path) {
        release(source);
        session->opened = session->loaded = false; session->path = request.path;
    }
    if (session->font_path != request.font) { session->font_opened = false; session->font_path = request.font; }
    if (!session->opened) {
        session->opened = document.open_verified(request.path, request.identity, result.error, stop);
        if (!session->opened) {
            // A cancelled open reports an error text; the next counting job repeats it, so it is not a failure.
            if (stop()) result.error.clear();
            return;
        }
    }
    if (stop()) return;
    if (!session->font_opened) {
        session->font_opened = request.font.empty() ? font.open_fallback() : font.open(request.font);
        if (!session->font_opened) { result.error = font.error(); return; }
    }
    if (stop()) return;
    if (!session->loaded || session->chapter_index != request.chapter || session->part != request.part) {
        release(source);
        session->loaded = false;
        if (!load_content(FileStore(request.root), document, request.chapter, source, result.error, ChapterLoad::Layout, request.part, stop)) {
            if (result.error == kChapterCancelled) result.error.clear();
            return;
        }
        session->loaded = true; session->chapter_index = request.chapter; session->part = request.part;
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
void ContentJob::render_image() {
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
    const bool cacheable = request.page.images.size() == 1 && request.page.glyphs.empty() &&
                           request.page.images.front().height == request.layout.height && !request.cache_name.empty();
    if (cacheable) {
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
    for (const auto& box : request.page.images) {
        std::string detail;
        bytes.clear();
        if (box.resource.empty() || !document.read_image(box.resource, bytes, detail, stop) ||
            !graphics::render_image(*result.image, bytes, box.x, box.y, box.width, box.height, detail, false, stop)) {
            if (stop()) { result.error = graphics::kImageCancelled; return; }
#ifdef ESP_PLATFORM
            ESP_LOGW("content_worker", "Image resource unavailable: resource=%s error=%s",
                     box.resource.c_str(), box.resource.empty() ? "Empty image resource" : detail.c_str());
#endif
            result.failed_images.push_back(box.begin);
        } else {
            PP_DIAGNOSTIC(++result.decoded_images;)
        }
    }
    std::vector<std::uint8_t>().swap(bytes);
    if (stop()) return;
    if (cacheable && result.failed_images.empty()) {
        std::string pixels(reinterpret_cast<const char*>(result.image->data()), result.image->pixels().size());
        pixels += md5_hex(pixels);
        store.save_cache(request.cache_name, pixels);
    }
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
    {
        std::string cached;
        if (store.load_cache(request.cache_name, request.cache_header.size() + count, cached) == StoreStatus::Ok &&
            cached.compare(0, request.cache_header.size(), request.cache_header) == 0) {
            if (cached.size() == request.cache_header.size()) { result.cache_finished = true; return; }
            if (cached.size() == request.cache_header.size() + count) {
                if (request.kind != Kind::SleepCover) {
                    result.image = std::make_unique<graphics::Canvas>(width, height);
                    std::memcpy(result.image->data(), cached.data() + request.cache_header.size(), count);
                }
                result.cache_finished = true;
                return;
            }
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
    const bool read = EpubDocument::read_cover(request.path, encoded, error, stop);
    const bool loaded = read && graphics::render_image(*image, encoded, 0, 0, width, height, error, request.cover_crop, stop);
    if (stop()) return;
    std::vector<std::uint8_t>().swap(encoded);
    if (loaded) {
        std::string bytes = request.cache_header;
        bytes.append(reinterpret_cast<const char*>(image->data()), image->pixels().size());
        result.cache_finished = store.save_cache(request.cache_name, bytes) == StoreStatus::Ok;
        if (request.kind != Kind::SleepCover) result.image = std::move(image);
    } else if ((read && error.find("allocation") == std::string::npos && error.find("memory") == std::string::npos) ||
               error == "EPUB has no supported cover" || error == "Invalid internal image path" ||
               error == "Missing EPUB image" || error == "EPUB image exceeds 2 MiB limit" ||
               error == "Image format is not PNG or JPEG" || error == "Invalid compressed EPUB image") {
        // Read failures may be transient; only a confirmed unusable cover gets a negative entry.
        result.cache_finished = store.save_cache(request.cache_name, request.cache_header) == StoreStatus::Ok;
    }
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
    result.progress = store.load_progress(request.relative_path, document.metadata().book_id,
        document.metadata().txt_gb18030 ? kTxtGb18030Revision : request.content_revision);
    if (request.kind == Kind::Open && !stop())
        result.bookmarks = store.load_bookmarks(request.relative_path, document.metadata().book_id,
            document.metadata().txt_gb18030 ? kTxtGb18030Revision : request.content_revision);
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
    if (request.target == Target::Start) {
        if (request.prefetch && !stop()) prepare_chapter_page(document);
        return;
    }
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
void ContentJob::prepare_chapter_page(EpubDocument& document) {
    const auto stop = [this] { return cancelled_.load(std::memory_order_relaxed); };
    graphics::Font font;
    if (!(request.font.empty() ? font.open_fallback() : font.open(request.font))) {
        result.error = font.error(); return;
    }
    result.page = plan_content(font, result.chapter, result.offset, request.layout, true, stop);
    const auto layout_bytes = result.page.glyphs.capacity() * sizeof(graphics::PositionedGlyph) +
        result.page.lines.capacity() * sizeof(graphics::TextLine);
    if (!result.page.error.empty() || result.page.end <= result.page.start || layout_bytes > 256U * 1024 ||
        chapter_cache_bytes(result.chapter) > 2U * 1024 * 1024) {
        result.error = "Prefetched page layout failed"; return;
    }
    if (result.page.images.empty()) {
        result.image = std::make_unique<graphics::Canvas>();
        if (!graphics::render_page(*result.image, font, result.page, 1, stop)) {
            result.error = "Prefetched page rasterization failed"; result.image.reset();
        }
        return;
    }
#ifdef ESP_PLATFORM
    if (heap_caps_get_free_size(MALLOC_CAP_SPIRAM) < 3U * 1024 * 1024 ||
        heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) < 2U * 1024 * 1024) {
        result.error = "Image preparation memory unavailable"; return;
    }
#endif
    result.image = std::make_unique<graphics::Canvas>();
    std::vector<std::uint8_t> encoded;
    for (const auto& box : result.page.images) {
        std::string detail;
        encoded.clear();
        if (box.resource.empty() || !document.read_image(box.resource, encoded, detail, stop) ||
            !graphics::render_image(*result.image, encoded, box.x, box.y, box.width, box.height, detail, false, stop)) {
            if (stop()) { result.error = graphics::kImageCancelled; result.image.reset(); return; }
            result.failed_images.push_back(box.begin);
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
        graphics::Canvas thumbnail(kLibraryCoverWidth, kLibraryCoverHeight);
        std::vector<std::uint8_t> encoded;
        std::string error;
        const bool loaded = EpubDocument::read_cover(request.root + "/" + source.path, encoded, error, stop) &&
            graphics::render_image(thumbnail, encoded, 0, 0, kLibraryCoverWidth, kLibraryCoverHeight, error, true, stop);
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
