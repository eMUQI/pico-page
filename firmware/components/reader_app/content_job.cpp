#include "content_job.h"
#include "content_cache.h"
#include "pico_page/sync.h"
#include "pico_page/stage_timer.h"
#include <sys/stat.h>
#include <chrono>
#include "pico_page/store.h"
#include <cstring>
#include <algorithm>
#include <array>
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
thread_local ContentJob::Kind delayed_kind = ContentJob::Kind::LockScreen;
thread_local std::chrono::milliseconds injected_delay{};
#endif
} // namespace
#ifndef ESP_PLATFORM
void ContentJob::fail_starts_for_test(Kind kind, unsigned count, StartFailure failure) {
    failed_kind = kind; failed_starts = count; injected_failure = failure;
}
void ContentJob::delay_starts_for_test(Kind kind, std::chrono::milliseconds delay) {
    delayed_kind = kind; injected_delay = delay;
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
         request.kind == Kind::PageCacheRead || request.kind == Kind::PageCacheWrite || request.kind == Kind::ContentCacheWrite ||
         request.kind == Kind::SleepCover || request.kind == Kind::LockScreen || request.kind == Kind::LockScreenPreview ||
         dictionary_kind(request.kind)) &&
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
        const auto delay = request.kind == delayed_kind ? injected_delay : std::chrono::milliseconds{};
        thread_ = std::thread([this, delay] {
            const auto until = std::chrono::steady_clock::now() + delay;
            while (!checkpoint() && std::chrono::steady_clock::now() < until)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            run(); done_.store(true, std::memory_order_release);
        });
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
    const auto stop = [this] { return checkpoint(); };
    if (request.kind == Kind::ContentCacheWrite) {
        result.cache_finished = save_content_cache(FileStore(request.root), request.content_cache, stop);
        return;
    }
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
    if (dictionary_kind(request.kind)) { read_dictionary(); return; }
    if (stop()) {
        result.retryable_failure = request.kind == Kind::LockScreen;
        return;
    }
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
    if (request.kind == Kind::LockScreenList || request.kind == Kind::LockScreen || request.kind == Kind::LockScreenPreview) { prepare_lock_screen(); return; }
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
        // A ready worker must block to give the lower-priority idle task CPU time.
        vTaskDelay(1);
#endif
        if (checkpoint()) return;
    }
}
std::string image_cache_key(const std::string& book_id, const std::string& resource, const graphics::LayoutSpec& spec) {
    return "images/gray-v1/" + book_id + "/" + md5_hex(resource + ":" + std::to_string(spec.left) + ":" +
        std::to_string(spec.top) + ":" + std::to_string(spec.width) + ":" + std::to_string(spec.height));
}
bool read_body_image_cache(CacheReader& reader, graphics::Canvas& image, const std::function<bool()>& stop) {
    const auto size = image.pixels().size();
    if (image.width() != 684 || image.height() != 1216 || reader.size() != size + 32) return false;
    Md5Digest digest;
    for (std::size_t at = 0; at < size;) {
        const auto count = std::min<std::size_t>(16 * 1024, size - at);
        auto* bytes = reinterpret_cast<char*>(image.data() + at);
        if ((stop && stop()) || !reader.read(bytes, count) || !digest.update({bytes, count})) return false;
        at += count;
    }
    std::array<char, 32> expected;
    return (!stop || !stop()) && reader.read(expected.data(), expected.size()) &&
           digest.finish() == std::string_view(expected.data(), expected.size()) && (!stop || !stop());
}

bool write_body_image_cache(const FileStore& store, const std::string& name, const graphics::Canvas& image,
                            const std::function<bool()>& stop) {
    if (image.width() != 684 || image.height() != 1216 || (stop && stop())) return false;
    const auto size = image.pixels().size();
    std::unique_ptr<CacheWriter> writer;
    if (store.begin_cache(name, size + 32, writer) != StoreStatus::Ok) return false;
    Md5Digest digest;
    for (std::size_t at = 0; at < size;) {
        const auto count = std::min<std::size_t>(16 * 1024, size - at);
        const std::string_view bytes(reinterpret_cast<const char*>(image.data() + at), count);
        if ((stop && stop()) || !digest.update(bytes) || !writer->write(at, bytes)) return false;
        at += count;
    }
    const auto checksum = digest.finish();
    if (checksum.size() != 32 || (stop && stop()) || !writer->write(size, checksum)) return false;
    const StageTimer commit_timer{"image_cache_commit", 0};
    return (!stop || !stop()) && writer->commit(size + checksum.size());
}

void ContentJob::render_image() {
    const StageTimer timer{"image_worker", 0};
    const auto stop = [this] { return cancelled_.load(std::memory_order_relaxed); };
    if (stop()) return;
    FileStore store(request.root);
    const bool cacheable = request.page.images.size() == 1 && request.page.glyphs.empty() &&
                           request.page.images.front().height == request.layout.height && !request.cache_name.empty();
    constexpr std::size_t size = 684U * 1216U / 2;
    if (cacheable) {
        const StageTimer cache_timer{"image_cache_read", 0};
#ifdef ESP_PLATFORM
        // Cache reads retain only the result canvas; cold decoding has a separate, larger budget.
        if (heap_caps_get_free_size(MALLOC_CAP_SPIRAM) < size + 256U * 1024 ||
            heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) < size) {
            result.error = "Image preparation memory unavailable"; return;
        }
#endif
        std::unique_ptr<CacheReader> reader;
        if (store.open_cache(request.cache_name, size + 32, reader) == StoreStatus::Ok && reader->size() == size + 32) {
            result.image = std::make_unique<graphics::Canvas>();
            if (read_body_image_cache(*reader, *result.image, stop)) return;
            result.image.reset();
        }
    }
    if (stop()) return;
#ifdef ESP_PLATFORM
    if (heap_caps_get_free_size(MALLOC_CAP_SPIRAM) < image_room ||
        heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) < size) {
        result.error = "Image preparation memory unavailable"; return;
    }
#endif
    result.image = std::make_unique<graphics::Canvas>();
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
        const StageTimer cache_timer{"image_cache_write", 0};
        write_body_image_cache(store, request.cache_name, *result.image, stop);
    }
}
void ContentJob::map_position() {
    const StageTimer timer{"position_mapping"};
    const auto stop = [this] { return cancelled_.load(std::memory_order_relaxed); };
    EpubDocument document;
    if (!document.open_verified(request.path, request.identity, result.error, stop)) return;
    if (request.kind == Kind::MapDownload) {
        sync::from_xpointer(document, request.xpointer, request.dom_version, result.mapped_anchor, result.error, stop,
                            &result.mapped_place);
        return;
    }
    if (!request.record) { result.error = "Saved record unavailable"; return; }
    sync::map_upload_position(document, request.record->anchor, request.dom_version,
                              result.xpointer, result.progress_fraction, result.error, stop);
}
bool read_lock_screen_cache(CacheReader& reader, const std::string& header, graphics::Canvas* image,
                            bool& alpha, const std::function<bool()>& stop) {
    if ((stop && stop()) || (image && (image->width() != 684 || image->height() != 1216)) ||
        !reader.rewind()) return false;
    std::string prefix(header.size() + 1, '\0');
    if (!reader.read(prefix.data(), prefix.size()) || prefix.compare(0, header.size(), header) != 0) return false;
    const char format = prefix.back();
    if (format != 'A' && format != 'G') return false;
    const std::size_t stride = format == 'A' ? 684 * 2 : 342;
    if (reader.size() != prefix.size() + stride * 1216 + 32) return false;
    alpha = format == 'A';
    Md5Digest digest;
    std::array<char, 684 * 2> row;
    for (int y = 0; y < 1216; ++y) {
        if ((stop && stop()) || !reader.read(row.data(), stride)) return false;
        const std::string_view bytes(row.data(), stride);
        if (!digest.update(bytes)) return false;
        if (image) {
            if (alpha) graphics::composite_image_rows(*image, bytes, y, 1);
            else std::memcpy(image->data() + std::size_t(y) * stride, row.data(), stride);
        }
    }
    std::array<char, 32> expected;
    return (!stop || !stop()) && reader.read(expected.data(), expected.size()) &&
           digest.finish() == std::string_view(expected.data(), expected.size()) && reader.rewind();
}

namespace {
// Only the temporary cache holds full gray/alpha coverage. Adam7 revisits rows, never final pixels.
bool stream_lock_png(FileStore& store, std::unique_ptr<CacheReader>& source, const std::string& header,
                     graphics::Canvas* image, bool& alpha, bool& cached, std::string& error,
                     const std::function<bool()>& stop, const std::function<bool()>& unchanged) {
    constexpr std::size_t stride = 684 * 2, height = 1216;
    std::unique_ptr<CacheWriter> writer;
    store.begin_cache("lockscreens/current", header.size() + 1 + stride * height + 32, writer);
    const auto base = header.size() + 1;
    if (writer && !writer->write(0, header + 'A')) writer.reset();
    std::array<char, stride> row{};
    std::array<bool, height> written{};
    int row_y = -1, row_height = 0, painted_y = -1;
    const auto flush = [&] {
        if (!writer || row_y < 0) return;
        for (int y = row_y; y < row_y + row_height; ++y) {
            if (stop() || !writer->write(base + std::size_t(y) * stride, {row.data(), row.size()})) {
                writer.reset(); return;
            }
            written[y] = true;
        }
    };
    graphics::ImageBox bounds{};
    const bool decoded = graphics::decode_png_stream(source->size(),
        [&](std::size_t offset, char* bytes, std::size_t size) { return source->read_at(offset, bytes, size); },
        684, 1216,
        [&](int x, int y, int w, int h, std::uint8_t gray, std::uint8_t coverage) {
            if (painted_y != y && stop()) return;
            painted_y = y;
            if (writer) {
                if (row_y != y) {
                    flush();
                    row_y = y; row_height = h; row.fill(0);
                    if (writer && written[y] && !writer->read(base + std::size_t(y) * stride, row.data(), row.size()))
                        writer.reset();
                }
                for (int col = x; col < x + w; ++col) {
                    row[col * 2] = static_cast<char>(gray);
                    row[col * 2 + 1] = static_cast<char>(coverage);
                }
            }
            if (image && coverage) {
                for (int py = y; py < y + h; ++py) for (int px = x; px < x + w; ++px) {
                    const unsigned mixed = (gray * coverage + image->pixel(px, py) * 17U * (255U - coverage) + 127U) / 255U;
                    image->pixel(px, py, static_cast<std::uint8_t>((mixed * 15U + 127U) / 255U));
                }
            }
        }, bounds, alpha, error, stop);
    if (!decoded) return false;
    flush();
    // The card permits eight open files. Release the decoder's handle before reopening for SHA-256.
    source.reset();
    if (stop()) { error = "休眠图片准备已取消或超时"; return false; }
    if (!unchanged()) { error = stop() ? "休眠图片准备已取消或超时" : "图片已改变或无法校验，请重试"; return false; }
    // Opaque sources retain white fitted margins even when overlay is enabled.
    if (image && !alpha) {
        image->fill_rect(0, 0, 684, bounds.y, 15);
        image->fill_rect(0, bounds.y + bounds.height, 684, 1216 - bounds.y - bounds.height, 15);
        image->fill_rect(0, bounds.y, bounds.x, bounds.height, 15);
        image->fill_rect(bounds.x + bounds.width, bounds.y, 684 - bounds.x - bounds.width, bounds.height, 15);
    }
    Md5Digest digest;
    constexpr std::size_t gray_stride = 342;
    const bool use_canvas = !alpha && image;
    if (use_canvas) {
        // The completed opaque canvas already has the cache's exact 4-bit representation.
        const auto size = image->pixels().size();
        for (std::size_t offset = 0; writer && offset < size; offset += 16U * 1024) {
            if (stop()) { error = "休眠图片准备已取消或超时"; return false; }
            const std::string_view bytes(reinterpret_cast<const char*>(image->data()) + offset,
                                          std::min<std::size_t>(16U * 1024, size - offset));
            if (!digest.update(bytes) || !writer->write(base + offset, bytes)) writer.reset();
        }
    }
    // Batch the readback to avoid one seek/read/write sequence per row on FAT.
    constexpr std::size_t batch_rows = 8;
    std::unique_ptr<char[]> batch;
    if (writer && !use_canvas) {
        batch.reset(new (std::nothrow) char[stride * batch_rows]);
        if (!batch) writer.reset();
    }
    for (std::size_t y = 0; writer && !use_canvas && y < height; y += batch_rows) {
        if (stop()) { error = "休眠图片准备已取消或超时"; return false; }
        const auto rows = std::min(batch_rows, height - y);
        std::memset(batch.get(), 0, rows * stride);
        for (std::size_t first = 0; writer && first < rows;) {
            const bool present = written[y + first];
            auto end = first + 1;
            while (end < rows && written[y + end] == present) ++end;
            if (present) {
                if (!writer->read(base + (y + first) * stride, batch.get() + first * stride, (end - first) * stride))
                    writer.reset();
            } else if (alpha && !writer->write(base + (y + first) * stride,
                                               {batch.get() + first * stride, (end - first) * stride})) writer.reset();
            first = end;
        }
        if (!writer) break;
        if (!alpha) {
            // Compact forward in place; output bytes always precede unread gray/alpha pairs.
            for (std::size_t x = 0; x < 684 * rows; x += 2) {
                const auto quantize = [&](std::size_t col) {
                    const unsigned g = static_cast<unsigned char>(batch[col * 2]);
                    const unsigned a = static_cast<unsigned char>(batch[col * 2 + 1]);
                    const unsigned mixed = (g * a + 255U * (255U - a) + 127U) / 255U;
                    return (mixed * 15U + 127U) / 255U;
                };
                batch[x / 2] = static_cast<char>(quantize(x) | (quantize(x + 1) << 4));
            }
        }
        const auto output_stride = alpha ? stride : gray_stride;
        const std::string_view bytes(batch.get(), rows * output_stride);
        if (!digest.update(bytes) ||
            (!alpha && !writer->write(base + y * output_stride, bytes))) writer.reset();
    }
    const auto payload = (alpha ? stride : gray_stride) * height;
    const auto sum = digest.finish();
    cached = writer && sum.size() == 32 && !stop() && writer->write(0, header + (alpha ? 'A' : 'G')) &&
             writer->write(base + payload, sum) && writer->commit(base + payload + sum.size());
    if (stop()) { error = "休眠图片准备已取消或超时"; return false; }
    error = cached ? "" : "休眠图片缓存未保存，下次将重新准备";
    return true;
}
} // namespace

void ContentJob::prepare_lock_screen() {
    const StageTimer timer{"lock_screen_prepare"};
    const auto deadline = request.lock_screen_deadline == std::chrono::steady_clock::time_point{} ?
        std::chrono::steady_clock::now() + kLockScreenPrepareBudget : request.lock_screen_deadline;
    const auto stop = [this, deadline] {
        const bool stopped = cancelled_.load(std::memory_order_relaxed) || std::chrono::steady_clock::now() >= deadline;
        if (stopped) result.retryable_failure = true;
        return stopped;
    };
    FileStore store(request.root);
    if (!request.lock_screen_shown.empty()) {
        result.cursor_saved = store.save_lock_screen(request.lock_screen_shown) == StoreStatus::Ok;
#ifdef ESP_PLATFORM
        if (!result.cursor_saved) ESP_LOGW("content_worker", "Lock screen rotation cursor was not saved");
#endif
    }
    const auto status = store.lock_screen_files(result.lock_screens, stop);
    if (status != StoreStatus::Ok && status != StoreStatus::Missing) {
        result.error = "图片目录不可读或超过数量限制"; return;
    }
    if (request.kind == Kind::LockScreenList) return;
    if (result.lock_screens.empty()) { result.error = "LockScreens 中没有可用图片"; return; }
    std::size_t index = 0;
    if (!request.sleep_image.empty()) {
        const auto it = std::find(result.lock_screens.begin(), result.lock_screens.end(), request.sleep_image);
        if (it == result.lock_screens.end()) { result.error = "指定图片已不存在"; return; }
        index = static_cast<std::size_t>(it - result.lock_screens.begin());
    } else {
        // The presented image advances this rotation even when its cursor commit failed.
        const auto last = request.lock_screen_shown.empty() ? store.last_lock_screen() : request.lock_screen_shown;
        const auto it = std::upper_bound(result.lock_screens.begin(), result.lock_screens.end(), last);
        if (it != result.lock_screens.end()) index = static_cast<std::size_t>(it - result.lock_screens.begin());
    }
    const auto attempts = request.sleep_image.empty() ? result.lock_screens.size() : 1;
    for (std::size_t attempt = 0; attempt < attempts; ++attempt) {
        if (stop()) { result.error = "休眠图片准备已取消或超时"; return; }
        result.lock_screen = result.lock_screens[(index + attempt) % result.lock_screens.size()];
        const auto path = request.root + "/LockScreens/" + result.lock_screen;
        struct stat before{}, after{};
        if (stat(path.c_str(), &before)) {
            result.error = "图片不可读"; result.retryable_failure = true; return;
        }
        if (!S_ISREG(before.st_mode) || before.st_size < 8 || before.st_size > 2 * 1024 * 1024) {
            result.error = "图片不完整或超过 2 MiB"; continue;
        }
        const auto identity = sha256_file(path, stop);
        if (identity.empty() || stop()) {
            result.retryable_failure = true; result.error = "图片身份校验失败或已取消"; return;
        }
        const std::string cache = "lockscreens/current";
        const auto header = "PPLOCK4\n" + identity + "\n" + result.lock_screen + "\n" + std::to_string(before.st_size) + "\n" +
                            std::to_string(before.st_mtime) + "\n684x1216-fit\n";
        constexpr std::size_t count = 342 * 1216;
        const auto canvas_available = [&] {
#ifdef ESP_PLATFORM
            const bool available = heap_caps_get_free_size(MALLOC_CAP_SPIRAM) >= count + 256U * 1024 &&
                                   heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) >= count;
#else
            const bool available = !request.lock_canvas_limit_for_test || count <= request.lock_canvas_limit_for_test;
#endif
            if (!available) { result.retryable_failure = true; result.error = "图片准备内存不足"; }
            return available;
        };
        const auto prepare_canvas = [&](bool preserve_background = false) {
            const bool reuse = request.sleep_overlay && request.lock_background;
            if ((!reuse || preserve_background) && !canvas_available()) return false;
            if (!reuse) result.image = std::make_unique<graphics::Canvas>();
            else if (preserve_background) result.image = std::make_unique<graphics::Canvas>(*request.lock_background);
            else result.image = std::make_unique<graphics::Canvas>(std::move(*request.lock_background));
            return true;
        };
        std::string pixels;
        bool alpha = false;
        bool cache_available = true;
#ifndef ESP_PLATFORM
        cache_available = !request.lock_cache_limit_for_test || request.lock_cache_limit_for_test >= 684 * 2;
#endif
        const auto cache_started = std::chrono::steady_clock::now();
        std::unique_ptr<CacheReader> reader;
        if (cache_available && store.open_cache(cache, header.size() + 4 * count + 33, reader) == StoreStatus::Ok) {
            result.cache_finished = read_lock_screen_cache(*reader, header, nullptr, alpha, stop);
        }
        const bool cached = result.cache_finished;
        if (cached && !request.lock_screen_prepare) {
#ifndef ESP_PLATFORM
            if (request.lock_cache_validated_for_test) request.lock_cache_validated_for_test();
#endif
            // Validate before moving the background; the second pass also verifies every consumed byte.
            if (!prepare_canvas()) return;
            if (!read_lock_screen_cache(*reader, header, result.image.get(), alpha, stop)) {
                result.image.reset(); result.cache_finished = false; result.retryable_failure = true;
                result.error = stop() ? "图片准备已取消" : "休眠图片缓存读取失败，请重试";
                return;
            }
        }
        reader.reset();
        [[maybe_unused]] const auto cache_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - cache_started).count();
        if (!cached) {
            if (stop()) { result.error = "休眠图片准备已取消或超时"; return; }
            alpha = false;
            std::unique_ptr<CacheReader> source;
            std::array<char, 8> signature{};
            if (store.open_lock_screen(result.lock_screen, source) != StoreStatus::Ok ||
                source->size() != static_cast<std::size_t>(before.st_size) ||
                !source->read(signature.data(), signature.size())) {
                result.retryable_failure = true; result.error = "图片读取失败或已改变"; return;
            }
            if (!std::memcmp(signature.data(), "\x89PNG\r\n\x1a\n", 8)) {
                // A damaged PNG can fail after painting; rotation needs an unchanged base for its next candidate.
                const bool preserve_background = attempts > 1 && request.sleep_overlay && request.lock_background;
#ifdef ESP_PLATFORM
                // Stream input, decoder state, source-coordinate maps and cache rows retain a fixed reserve.
                const auto output_bytes = request.lock_screen_prepare ||
                    (request.sleep_overlay && request.lock_background && !preserve_background) ? 0U : count;
                if (heap_caps_get_free_size(MALLOC_CAP_SPIRAM) < output_bytes + 768U * 1024 ||
                    heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) < 128U * 1024) {
                    result.retryable_failure = true; result.error = "图片准备内存不足"; return;
                }
#endif
                if (!request.lock_screen_prepare && !prepare_canvas(preserve_background)) return;
                const auto unchanged = [&] {
                    return !stat(path.c_str(), &after) && before.st_size == after.st_size &&
                        before.st_mtime == after.st_mtime && sha256_file(path, stop) == identity && !stop();
                };
                const bool rendered = stream_lock_png(store, source, header, result.image.get(), alpha,
                                                       result.cache_finished, result.error, stop, unchanged);
                if (!rendered) {
                    result.image.reset();
                    if (result.error.find("allocation") != std::string::npos ||
                        result.error.find("memory") != std::string::npos) {
                        result.retryable_failure = true; result.error = "图片准备内存不足"; return;
                    }
                    if (stop() || result.error.find("read") != std::string::npos ||
                        result.error == "图片已改变或无法校验，请重试") {
                        result.retryable_failure = true; return;
                    }
                    result.error = "图片损坏、不支持或像素超限"; continue;
                }
                result.retryable_failure = !result.cache_finished;
#ifdef ESP_PLATFORM
                ESP_LOGI("content_worker", "Lock screen ready: source=stream alpha=%d cached=%d prepare=%d overlay=%d free=%u largest=%u stack_free=%u",
                    alpha, result.cache_finished, request.lock_screen_prepare, request.sleep_overlay,
                    static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
                    static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)),
                    static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
#endif
                return;
            }
            source.reset();
#ifdef ESP_PLATFORM
            const auto encoded_size = static_cast<std::size_t>(before.st_size);
            if (heap_caps_get_free_size(MALLOC_CAP_SPIRAM) < encoded_size + 2 * count + 768U * 1024 ||
                heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) < std::max(encoded_size, count)) {
                result.retryable_failure = true; result.error = "图片准备内存不足"; return;
            }
#endif
            std::vector<std::uint8_t> encoded;
            if (stop() || store.read_lock_screen(result.lock_screen, encoded, stop) != StoreStatus::Ok) {
                result.retryable_failure = true; result.error = "图片读取失败或已取消"; return;
            }
            bool rendered;
            {
                graphics::Canvas image;
                rendered = graphics::render_image(image, encoded, 0, 0, 684, 1216, result.error, false, stop);
                if (rendered) pixels.assign(reinterpret_cast<const char*>(image.data()), count);
            }
            std::vector<std::uint8_t>().swap(encoded);
            if (stop()) { result.error = "休眠图片准备已取消或超时"; return; }
            if (!rendered) {
                if (result.error.find("allocation failed") != std::string::npos || result.error.find("memory") != std::string::npos) {
                    result.retryable_failure = true; result.error = "图片准备内存不足"; return;
                }
                result.error = "图片损坏、不支持或像素超限"; continue;
            }
            if (stat(path.c_str(), &after) || before.st_size != after.st_size || before.st_mtime != after.st_mtime ||
                sha256_file(path, stop) != identity || stop()) {
                result.retryable_failure = true; result.error = "图片已改变，请重试"; return;
            }
            const auto digest = md5_hex(pixels);
            result.cache_finished = store.save_cache(cache, pixels, header + (alpha ? 'A' : 'G'), digest) == StoreStatus::Ok;
            if (!result.cache_finished) {
                result.retryable_failure = true; result.error = "休眠图片缓存未保存，下次将重新准备";
#ifdef ESP_PLATFORM
                ESP_LOGW("content_worker", "Lock screen cache was not saved");
#endif
                if (request.lock_screen_prepare) return;
            } else result.error.clear();
        } else result.error.clear();
        if (!cached && !request.lock_screen_prepare) {
            if (!prepare_canvas()) return;
            std::memcpy(result.image->data(), pixels.data(), count);
        }
        if (stop()) { result.image.reset(); result.error = "图片准备已取消"; return; }
#ifdef ESP_PLATFORM
        ESP_LOGI("content_worker", "Lock screen ready: source=%s alpha=%d cached=%d prepare=%d overlay=%d payload=%u cache_ms=%lld free=%u largest=%u stack_free=%u",
            cached ? "cache" : "decode", alpha, result.cache_finished, request.lock_screen_prepare, request.sleep_overlay,
            static_cast<unsigned>(alpha ? 4 * count : count), static_cast<long long>(cache_ms),
            static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
            static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)),
            static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
#endif
        return;
    }
    if (request.sleep_image.empty()) result.error = "轮播图片均不可用：" + result.error;
    result.lock_screen.clear();
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
    const StageTimer timer{"book_open_worker", 0};
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
            !find_content(store, document, result.chapter_index, result.chapter, result.error, ChapterLoad::Reading, match, stop, &result.content_cache) ||
            !result.chapter.offset_for(anchor, offset)) {
            if (result.error.empty()) result.error = "Saved position cannot be mapped";
            return;
        }
        result.offset = offset;
    } else {
        while (result.chapter_index + 1 < document.spine().size() && !document.spine()[result.chapter_index].linear)
            ++result.chapter_index;
        if (!load_content(store, document, result.chapter_index, result.chapter, result.error, ChapterLoad::Reading, 0, stop, &result.content_cache)) return;
    }
    if (stop()) return;
    if (stat(request.path.c_str(), &after) != 0 || before.st_size != after.st_size ||
        before.st_mtime != after.st_mtime || before.st_ino != after.st_ino) {
        result.error = "Book changed while opening"; return;
    }
    result.file_size = after.st_size; result.file_modified = after.st_mtime; result.file_inode = after.st_ino;
}
bool ContentJob::checkpoint() {
    while (!cancelled_.load(std::memory_order_relaxed)) {
        const auto generation = pause_generation_.load(std::memory_order_acquire);
        if (!(generation & 1U)) break;
        paused_generation_.store(generation, std::memory_order_release);
#ifdef ESP_PLATFORM
        vTaskDelay(1);
#else
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
#endif
    }
    return cancelled_.load(std::memory_order_relaxed);
}
void ContentJob::load_chapter() {
    const auto stop = [this] { return checkpoint(); };
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
            if (!load_content(store, document, request.chapter, result.chapter, result.error, ChapterLoad::Reading, 0, stop, &result.content_cache)) return;
        } else if (!find_content(store, document, request.chapter, result.chapter, result.error, ChapterLoad::Reading, match, stop, &result.content_cache)) return;
        result.offset = position;
        if (request.target == Target::Fragment && result.offset >= result.chapter.text.size() && !result.chapter.last_part) {
            const auto next = result.chapter.part + 1;
            if (!load_content(store, document, request.chapter, result.chapter, result.error, ChapterLoad::Reading, next, stop, &result.content_cache)) return;
            result.offset = 0;
        }
        return;
    }
    if (!load_content(store, document, request.chapter, result.chapter, result.error, ChapterLoad::Reading, request.part, stop, &result.content_cache)) return;
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
