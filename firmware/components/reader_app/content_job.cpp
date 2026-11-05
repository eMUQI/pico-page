#include "content_job.h"
#include "content_cache.h"
#include "pico_page/sync.h"
#include "pico_page/stage_timer.h"
#include <sys/stat.h>
#include <chrono>
#include "pico_page/store.h"
#include <cstring>
#include <cmath>
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
}
#ifndef ESP_PLATFORM
void ContentJob::fail_starts_for_test(Kind kind, unsigned count, StartFailure failure) {
    failed_kind = kind; failed_starts = count; injected_failure = failure;
}
void ContentJob::delay_starts_for_test(Kind kind, std::chrono::milliseconds delay) {
    delayed_kind = kind; injected_delay = delay;
}
#endif
std::size_t readable_offset(const Chapter& source, std::size_t start, bool author) {
    auto run = std::lower_bound(source.runs.begin(), source.runs.end(), start,
        [](const TextRun& item, std::size_t offset) { return item.end <= offset; });
    for (; run != source.runs.end(); ++run) {
        const auto& style = source.style(run->style);
        if (!author || (!style.hidden && style.display != document::css::Display::None))
            return std::max<std::size_t>(start, run->begin);
    }
    return source.text.size();
}
std::size_t restore_readable_offset(const Chapter& source, std::size_t start) {
    const auto next = readable_offset(source, start, true);
    if (next < source.text.size() || start >= source.text.size()) return next;
    for (auto run = source.runs.rbegin(); run != source.runs.rend(); ++run) {
        const auto& style = source.style(run->style);
        if (!style.hidden && style.display != document::css::Display::None) return run->begin;
    }
    return next;
}

const DocumentImage* image_at(const Chapter& source, std::size_t start) {
    for (const auto& image : source.images)
        if (image.begin <= start && start < image.end && image.end <= source.text.size()) return &image;
    return nullptr;
}
namespace {
int css_pixels(float value, float scale, int lower, int upper) {
    return static_cast<int>(std::lround(std::clamp(value * scale, static_cast<float>(lower), static_cast<float>(upper))));
}

graphics::Page plan_author_content(graphics::Font& font, const Chapter& source, std::size_t start,
                                   const graphics::LayoutSpec& spec, bool collect, const std::function<bool()>& stop,
                                   const graphics::AuthorFonts* fonts) {
    namespace css = document::css;
    graphics::Page page;
    page.start = page.end = start;
    page.font_px = spec.font_px;
    const auto bottom = spec.top + spec.height;
    const float scale = spec.font_px / 16.f;
    int top = spec.top;
    auto cursor = start;
    while (cursor < source.text.size() && top < bottom) {
        if (stop && stop()) { page.error = "Layout cancelled"; break; }
        const auto block = std::lower_bound(source.blocks.begin(), source.blocks.end(), cursor,
            [](const TextBlock& item, std::size_t offset) { return item.end <= offset; });
        if (block == source.blocks.end()) break;
        const auto& style = source.style(block->style);
        const bool page_started = page.end > page.start;
        const auto block_start = readable_offset(source, block->begin, true);
        const bool first_line = cursor == block_start;
        if (first_line && page_started && style.break_before == css::Break::Always) break;
        const auto length = [&](float value, unsigned bit, int width) {
            return value * (style.box_percentages & bit ? width / 100.f : scale);
        };
        const auto side = [&](std::size_t i, int limit) {
            return css_pixels(length(style.margin_px[i], css::ComputedStyle::margin_percent << i, spec.width) +
                              length(style.padding_px[i], css::ComputedStyle::padding_percent << i, spec.width), 1, 0, limit);
        };
        const int before = first_line ? side(0, spec.height / 6) : 0;
        const int after = side(2, spec.height / 6);
        graphics::LayoutSpec part = spec;
        const int left = side(3, spec.width / 4), right = side(1, spec.width / 4);
        part.left += left;
        part.width -= left + right;
        part.top = top + before;
        part.height = bottom - part.top;
        if (part.height <= 0) break;
        const auto* image = image_at(source, cursor);
        if (image) {
            const bool image_only = source.blocks.size() == 1;
            const auto minimum_width = std::min(32, part.width);
            int width = part.width;
            if (style.width_px >= 0) width = css_pixels(length(style.width_px, css::ComputedStyle::width_percent, part.width), 1, minimum_width, width);
            if (style.max_width_px >= 0) width = css_pixels(length(style.max_width_px, css::ComputedStyle::max_width_percent, part.width), 1, minimum_width, width);
            const bool auto_left = style.margin_auto & (1U << 3), auto_right = style.margin_auto & (1U << 1);
            int x = part.left;
            if ((auto_left && auto_right) || style.align == css::Align::Center) x += (part.width - width) / 2;
            else if (auto_left || style.align == css::Align::Right) x += part.width - width;
            const int full_height = image_only ? std::max(1, spec.height - before - after) : std::min(spec.height / 2, 480);
            const int height = std::max(32, full_height * width / part.width);
            if (height > part.height && page_started) break;
            if (collect) page.images.push_back({image->begin, x, part.top, width,
                                               std::min(height, part.height), image->resource});
            page.end = image->end;
            top = part.top + std::min(height, part.height);
        } else {
            const auto maximum_font = std::min(part.width / 2, 128);
            part.font_px = css_pixels(style.font_px, scale, 8, maximum_font);
            part.line_height = css_pixels(style.line_height_px, scale, 1, 320);
            part.paragraph_gap = 0;
            const auto run_at = [&](std::size_t i) {
                return std::lower_bound(source.runs.begin(), source.runs.end(), i,
                    [](const TextRun& item, std::size_t offset) { return item.end <= offset; });
            };
            auto run = run_at(cursor);
            const graphics::StyledLayout format{block_start,
                css_pixels(length(style.indent_px, css::ComputedStyle::indent_percent, part.width), 1, -left, part.width / 3),
                static_cast<graphics::TextAlign>(style.align), [&](std::size_t i) {
                    if (run == source.runs.end() || i >= run->end || i < run->begin) run = run_at(i);
                    const auto& selected = run != source.runs.end() && run->begin <= i ? source.style(run->style) : style;
                    return graphics::GlyphStyle{
                        css_pixels(selected.font_px, scale, 8, maximum_font),
                        css_pixels(selected.line_height_px, scale, 1, 320),
                        selected.bold, selected.italic, selected.underline,
                        selected.hidden, selected.display == css::Display::None, selected.font_families};
                }};
            auto section = graphics::layout_page(font, std::u32string_view(source.text).substr(0, block->end),
                                                 cursor, part, collect, stop, &format, fonts);
            if (!section.error.empty()) { page.error = std::move(section.error); break; }
            if (section.end <= cursor) break;
            const bool complete = section.end == block->end;
            const bool keep_with_next = block->kind == BlockKind::Heading || style.break_after == css::Break::Avoid;
            if (page_started && first_line &&
                ((!complete && (keep_with_next || style.break_inside == css::Break::Avoid)) ||
                 (complete && keep_with_next && std::next(block) != source.blocks.end() &&
                  section.next_top + after + spec.line_height > bottom))) break;
            page.lines.insert(page.lines.end(), section.lines.begin(), section.lines.end());
            page.glyphs.insert(page.glyphs.end(), section.glyphs.begin(), section.glyphs.end());
            page.end = section.end;
            top = section.next_top;
            if (!complete) break;
        }
        top += after;
        if (style.break_after == css::Break::Always) break;
        cursor = readable_offset(source, page.end, true);
    }
    page.next_top = top;
    return page;
}
}

graphics::Page plan_content(graphics::Font& font, const Chapter& source, std::size_t start,
                            const graphics::LayoutSpec& spec, bool collect, const std::function<bool()>& stop,
                            const graphics::AuthorFonts* fonts) {
    start = readable_offset(source, start, spec.original_epub);
    if (spec.original_epub && !source.author_styles.empty()) {
        const graphics::AuthorFontIo input{fonts};
        return plan_author_content(font, source, start, spec, collect, stop, fonts);
    }
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
graphics::AuthorFonts prepare_embedded_fonts(const FileStore& store, EpubDocument& document, const Chapter& chapter,
    bool& stable, const std::function<bool()>& stop) {
    stable = true;
    if (chapter.author_fonts.empty()) return {};
    const StageTimer timer{"epub_font_prepare", 0};
    unsigned used = 0;
    for (const auto& run : chapter.runs) {
        const auto& style = chapter.style(run.style);
        if (style.hidden || style.display == document::css::Display::None || run.element_reference) continue;
        for (auto families = style.font_families; families; families >>= 4) used |= 1U << (families & 15);
    }
    if (!used) return {};
    if (!chapter_memory_available(128U * 1024)) { stable = false; return {}; }
    struct Input {
        FileStore store;
        std::unique_ptr<CacheReader> reader;
        std::size_t slot = 0;
        bool failed = false;
    };
    auto source = std::make_shared<Input>(Input{store, {}});
    graphics::AuthorFonts fonts;
    fonts.faces.resize(chapter.author_fonts.size());
    fonts.close_input = [source] { source->reader.reset(); };
    unsigned loaded = 0, hits = 0, bytes = 0;
    for (std::size_t i = 0; i < fonts.faces.size(); ++i) {
        if (!(used & (1U << (i + 1)))) continue;
        if (stop && stop()) return {};
        const auto& font = chapter.author_fonts[i];
        const auto name = "epub-fonts-v1/" + document.metadata().book_id + "-" + md5_hex(font.resource);
        auto& reader = source->reader;
        reader.reset();
        if (store.open_cache(name, font.bytes, reader) == StoreStatus::Ok &&
            reader->size() == font.bytes && reader->matches_crc32(font.crc32, stop)) {
            ++hits;
        } else {
            reader.reset();
            std::unique_ptr<CacheWriter> writer;
            if ((stop && stop()) || store.begin_cache(name, font.bytes, writer) != StoreStatus::Ok ||
                !document.read_font(font, [&](std::size_t offset, std::string_view data) {
                    return writer->write(offset, data);
                }, stop) || !writer->commit(font.bytes) ||
                store.open_cache(name, font.bytes, reader) != StoreStatus::Ok || reader->size() != font.bytes) {
                stable = false;
                continue;
            }
        }
        auto face = std::unique_ptr<graphics::Font>(new (std::nothrow) graphics::Font(64U * 1024, 512));
        source->slot = i;
        source->failed = false;
        if (!face || !face->open_stream(font.bytes, [source, i, name, size = font.bytes](std::size_t offset, char* data, std::size_t count) {
                auto& stream = source->reader;
                if (!stream || source->slot != i) {
                    stream.reset();
                    if (source->store.open_cache(name, size, stream) != StoreStatus::Ok || stream->size() != size) {
                        stream.reset();
                        source->failed = true;
                        return false;
                    }
                    source->slot = i;
                }
                const auto read = stream->read_at(offset, data, count);
                source->failed |= !read;
                return read;
            })) {
            stable &= face && !source->failed && face->error().find("memory") == std::string::npos &&
                      face->error().find("allocate") == std::string::npos && face->error().find("initialization") == std::string::npos;
            continue;
        }
        fonts.faces[i] = std::move(face);
        ++loaded;
        bytes += font.bytes;
    }
#if defined(ESP_PLATFORM) && defined(CONFIG_PICO_PAGE_DIAGNOSTIC_CONSOLE)
    ESP_LOGI("epub_font", "used_mask=%u loaded=%u cache_hits=%u bytes=%u stable=%d psram_free=%u psram_largest=%u psram_min=%u internal_min=%u stack_free=%u",
        used, loaded, hits, bytes, stable, static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
        static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)),
        static_cast<unsigned>(heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM)),
        static_cast<unsigned>(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
        static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
#else
    static_cast<void>(loaded); static_cast<void>(hits); static_cast<void>(bytes);
#endif
    fonts.release_io();
    return fonts;
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
#ifdef CONFIG_PICO_PAGE_LEGACY_PERF_LOG
    ESP_LOGI("reader_perf", "content_request job=%p kind=%u generation=%llu chapter=%u part=%u target=%u prefetch=%d",
        static_cast<void*>(this), static_cast<unsigned>(request.kind), static_cast<unsigned long long>(request.generation),
        static_cast<unsigned>(request.chapter), request.part, static_cast<unsigned>(request.target), request.chapter_prefetch);
#endif
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
    const auto required_internal = 32U * 1024 + (content_requests ? 0 : content_stack_bytes);
    if ((request.kind == Kind::Pagination || request.kind == Kind::FontIdentity || request.kind == Kind::LibraryCovers ||
         request.kind == Kind::PageCacheRead || request.kind == Kind::PageCacheWrite || request.kind == Kind::ContentCacheWrite ||
         request.kind == Kind::SleepCover || request.kind == Kind::LockScreen || request.kind == Kind::LockScreenPreview ||
         dictionary_kind(request.kind) || request.chapter_prefetch) &&
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
        (request.kind == Kind::SaveBookmarks && result.bookmark_status != StoreStatus::Ok) ||
        (request.kind == Kind::ClearCache && result.clear_cache_status != StoreStatus::Ok);
    diagnostics::end(measurement_, cancelled_.load() ? diagnostics::Outcome::Cancelled :
        failed ? diagnostics::Outcome::Failure : diagnostics::Outcome::Success);
}
void ContentJob::execute() {
#else
void ContentJob::run() {
#endif
#ifdef CONFIG_PICO_PAGE_LEGACY_PERF_LOG
    struct WorkerMeasurement {
        ContentJob& job;
        std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
        ~WorkerMeasurement() {
            const auto us = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - started).count();
            const bool cancelled = job.cancelled_.load(std::memory_order_relaxed);
            const bool failed = !job.result.error.empty() || !job.result.failed_images.empty() ||
                (job.request.kind == Kind::SaveProgress && job.result.progress.status != StoreStatus::Ok) ||
                (job.request.kind == Kind::SaveBookmarks && job.result.bookmark_status != StoreStatus::Ok) ||
                (job.request.kind == Kind::ClearCache && job.result.clear_cache_status != StoreStatus::Ok);
            ESP_LOGI("reader_perf", "stage=content_worker us=%lld min_us=0 job=%p kind=%u generation=%llu outcome=%s chapter=%u part=%u scalars=%u retained_hit=%d cache_committed=%d",
                static_cast<long long>(us), static_cast<void*>(&job), static_cast<unsigned>(job.request.kind),
                static_cast<unsigned long long>(job.request.generation),
                cancelled ? "cancelled" : failed ? "failed" : "complete",
                static_cast<unsigned>(job.result.chapter_index), job.result.chapter.part,
                static_cast<unsigned>(job.result.chapter.text.size()), job.result.retained_cache_hit, job.result.cache_finished);
        }
    } measurement{*this};
#endif
    struct FinishChapterSession {
        ContentJob& job;
        ~FinishChapterSession() {
            if (!job.request.chapter_session) return;
            auto& document = job.request.chapter_session->document;
            if (!job.result.error.empty() || job.cancelled_.load(std::memory_order_relaxed)) document.close();
            job.result.chapter_stream_bytes = document.metadata().book_id.empty() ? 0 : document.retained_bytes();
        }
    } finish{*this};
    result.offset = request.offset;
    const auto stop = [this] { return checkpoint(); };
    if (request.kind == Kind::StorageScan || request.kind == Kind::ClearCache) {
        FileStore store(request.root);
        if (request.kind == Kind::ClearCache) {
            const StageTimer timer{"cache_clear"};
            result.clear_cache_status = store.clear_cache(stop);
        }
        if (!stop()) {
            const StageTimer timer{"storage_scan"};
            result.storage = store.storage_usage(stop);
        }
        return;
    }
    if (request.kind == Kind::ContentCacheWrite) {
        result.cache_finished = request.content_cache && save_content_cache(FileStore(request.root), *request.content_cache, stop);
        return;
    }
    if (request.kind == Kind::SaveProgress) {
        const StageTimer timer{"progress_commit", 0};
        FileStore store(request.root);
        if (request.record) result.progress = store.save_progress(request.relative_path, *request.record);
        if (request.save_last_book && result.progress.status == StoreStatus::Ok)
            result.last_book_saved = store.save_last_book(request.relative_path, result.error) == StoreStatus::Ok;
        return;
    }
    if (request.kind == Kind::SaveBookmarks) {
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
        result.failure_kind = ContentFailureKind::Cancelled;
        if (request.kind == Kind::Chapter) result.error = kChapterCancelled;
        return;
    }
    if (request.kind == Kind::PageCacheRead || request.kind == Kind::PageCacheWrite) {
        FileStore store(request.root);
        if (request.kind == Kind::PageCacheRead) {
            result.page_cache = read_page_cache(store, request.cache_name, request.cache_header, request.page_linear, stop);
        } else if (request.page_cache) {
            result.cache_finished = write_page_cache(store, request.cache_name, request.cache_header,
                                                     request.page_cache->pages, request.page_cache->counted, stop);
        }
#if defined(ESP_PLATFORM) && defined(CONFIG_PICO_PAGE_DIAGNOSTIC_CONSOLE)
        ESP_LOGI("page_index_cache", "read=%d hit=%d write=%d original=%d", request.kind == Kind::PageCacheRead,
            result.page_cache.has_value(), result.cache_finished, request.cache_name.rfind("pages-original/", 0) == 0);
#endif
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
        session->opened = document.open_verified(request.path, request.identity, result.error, stop, request.book_index);
        if (!session->opened) {
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
        session->author_fonts.clear();
        session->loaded = false;
        if (!load_content(FileStore(request.root), document, request.chapter, source, result.error, request.layout.original_epub ? ChapterLoad::Original : ChapterLoad::Layout, request.part, stop)) {
            if (result.error == kChapterCancelled) result.error.clear();
            return;
        }
        if (source.author_style_fallback) { result.error = "Original layout unavailable for page counting"; return; }
        session->author_fonts = prepare_embedded_fonts(FileStore(request.root), document, source, result.author_fonts_stable, stop);
        if (stop()) return;
        if (!result.author_fonts_stable) { result.error = "Embedded fonts temporarily unavailable for page counting"; return; }
        session->loaded = true; session->chapter_index = request.chapter; session->part = request.part;
    }
    while (!stop()) {
        const auto start = readable_offset(source, result.offset, request.layout.original_epub);
        if (start >= source.text.size()) {
            result.part_done = true; result.last_part = source.last_part; return;
        }
        const auto page = plan_content(font, source, start, request.layout, false, stop, &session->author_fonts);
        if (page.error == "Layout cancelled") return;
        if (!page.error.empty() || page.end <= page.start) {
            result.error = page.error.empty() ? "Pagination made no progress" : page.error; return;
        }
        result.starts.push_back(static_cast<std::uint32_t>(page.start));
        result.offset = page.end;
        if (result.starts.size() >= 8) return;
#ifdef ESP_PLATFORM
        vTaskDelay(1);
#endif
        if (checkpoint()) return;
    }
}
std::string image_cache_key(const std::string& book_id, const std::string& resource, const graphics::LayoutSpec& spec) {
    return "images/gray-v1/" + book_id + "/" + md5_hex(resource + ":" + std::to_string(spec.left) + ":" +
        std::to_string(spec.top) + ":" + std::to_string(spec.width) + ":" + std::to_string(spec.height));
}
std::string page_image_cache_key(const std::string& book_id, const graphics::Page& page, const graphics::LayoutSpec& spec) {
    if (page.images.size() != 1 || page.images.front().resource.empty()) return {};
    const auto& box = page.images.front();
    if (spec.original_epub) {
        return "images/author-gray-v1/" + book_id + "/" + md5_hex(box.resource + ":" + std::to_string(box.x) + ":" +
            std::to_string(box.y) + ":" + std::to_string(box.width) + ":" + std::to_string(box.height));
    }
    return page.glyphs.empty() && box.height == spec.height ? image_cache_key(book_id, box.resource, spec) : std::string{};
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
    const bool cacheable = request.kind == Kind::Image && !request.cache_name.empty();
    constexpr std::size_t size = 684U * 1216U / 2;
    if (cacheable) {
        const StageTimer cache_timer{"image_cache_read", 0};
#ifdef ESP_PLATFORM
        if (heap_caps_get_free_size(MALLOC_CAP_SPIRAM) < size + 256U * 1024 ||
            heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) < size) {
            result.error = "Image preparation memory unavailable"; return;
        }
#endif
        std::unique_ptr<CacheReader> reader;
        if (store.open_cache(request.cache_name, size + 32, reader) == StoreStatus::Ok && reader->size() == size + 32) {
            result.image = std::make_unique<graphics::Canvas>();
            if (read_body_image_cache(*reader, *result.image, stop)) {
#if defined(ESP_PLATFORM) && defined(CONFIG_PICO_PAGE_DIAGNOSTIC_CONSOLE)
                ESP_LOGI("page_image_cache", "read=hit original=%d", request.layout.original_epub);
#endif
                return;
            }
            result.image.reset();
        }
    }
    if (stop()) return;
#ifdef ESP_PLATFORM
    if (heap_caps_get_free_size(MALLOC_CAP_SPIRAM) < cold_image_memory_required_bytes ||
        heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) < size) {
        result.error = "Image preparation memory unavailable"; return;
    }
#endif
    result.image = std::make_unique<graphics::Canvas>();
    EpubDocument document;
    std::vector<std::uint8_t> bytes;
    if (!document.open_verified(request.path, request.identity, result.error, stop, request.book_index) || stop()) return;
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
bool stream_lock_png(FileStore& store, std::unique_ptr<CacheReader>& source, const std::string& header,
                     graphics::Canvas* image, bool& alpha, bool& cached, std::string& error, ContentFailureKind& failure_kind,
                     const std::function<bool()>& stop, const std::function<ContentFailureKind()>& source_failure) {
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
        }, bounds, alpha, error, stop, &failure_kind);
    if (!decoded) return false;
    flush();
    source.reset();
    if (stop()) { failure_kind = ContentFailureKind::Cancelled; error = "休眠图片准备已取消或超时"; return false; }
    failure_kind = source_failure();
    if (failure_kind != ContentFailureKind::None) {
        error = "图片已改变或无法校验，请重试"; return false;
    }
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
        const auto size = image->pixels().size();
        for (std::size_t offset = 0; writer && offset < size; offset += 16U * 1024) {
            if (stop()) { failure_kind = ContentFailureKind::Cancelled; error = "休眠图片准备已取消或超时"; return false; }
            const std::string_view bytes(reinterpret_cast<const char*>(image->data()) + offset,
                                          std::min<std::size_t>(16U * 1024, size - offset));
            if (!digest.update(bytes) || !writer->write(base + offset, bytes)) writer.reset();
        }
    }
    constexpr std::size_t batch_rows = 8;
    std::unique_ptr<char[]> batch;
    if (writer && !use_canvas) {
        batch.reset(new (std::nothrow) char[stride * batch_rows]);
        if (!batch) { failure_kind = ContentFailureKind::OutOfMemory; writer.reset(); }
    }
    for (std::size_t y = 0; writer && !use_canvas && y < height; y += batch_rows) {
        if (stop()) { failure_kind = ContentFailureKind::Cancelled; error = "休眠图片准备已取消或超时"; return false; }
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
    if (stop()) { failure_kind = ContentFailureKind::Cancelled; error = "休眠图片准备已取消或超时"; return false; }
    if (cached) failure_kind = ContentFailureKind::None;
    else if (failure_kind != ContentFailureKind::OutOfMemory) failure_kind = ContentFailureKind::IoError;
    error = cached ? "" : "休眠图片缓存未保存，下次将重新准备";
    return true;
}
}

void ContentJob::prepare_lock_screen() {
    const StageTimer timer{"lock_screen_prepare"};
    const auto deadline = request.lock_screen_deadline == std::chrono::steady_clock::time_point{} ?
        std::chrono::steady_clock::now() + kLockScreenPrepareBudget : request.lock_screen_deadline;
    const auto stop = [this, deadline] {
        const bool stopped = cancelled_.load(std::memory_order_relaxed) || std::chrono::steady_clock::now() >= deadline;
        if (stopped) result.failure_kind = cancelled_.load(std::memory_order_relaxed) ?
            ContentFailureKind::Cancelled : ContentFailureKind::TimedOut;
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
        if (!stop()) result.failure_kind = status == StoreStatus::Invalid || status == StoreStatus::UnsafePath ?
            ContentFailureKind::InvalidFormat : ContentFailureKind::IoError;
        result.error = status == StoreStatus::Invalid ? "LockScreens/ 中的文件过多，最多支持 256 张图片"
                     : status == StoreStatus::UnsafePath ? "LockScreens 不是普通文件夹" : "无法读取 LockScreens/ 目录";
        return;
    }
    if (request.kind == Kind::LockScreenList) return;
    if (result.lock_screens.empty()) { result.failure_kind = ContentFailureKind::MissingResource; result.error = "LockScreens 中没有可用图片"; return; }
    std::size_t index = 0;
    if (!request.sleep_image.empty()) {
        const auto it = std::find(result.lock_screens.begin(), result.lock_screens.end(), request.sleep_image);
        if (it == result.lock_screens.end()) { result.failure_kind = ContentFailureKind::MissingResource; result.error = "指定图片已不存在"; return; }
        index = static_cast<std::size_t>(it - result.lock_screens.begin());
    } else {
        const auto last = request.lock_screen_shown.empty() ? store.last_lock_screen() : request.lock_screen_shown;
        const auto it = std::upper_bound(result.lock_screens.begin(), result.lock_screens.end(), last);
        if (it != result.lock_screens.end()) index = static_cast<std::size_t>(it - result.lock_screens.begin());
    }
    const auto attempts = request.sleep_image.empty() ? result.lock_screens.size() : 1;
    for (std::size_t attempt = 0; attempt < attempts; ++attempt) {
        if (stop()) { result.error = "休眠图片准备已取消或超时"; return; }
        result.failure_kind = ContentFailureKind::None;
        result.lock_screen = result.lock_screens[(index + attempt) % result.lock_screens.size()];
        const auto path = request.root + "/LockScreens/" + result.lock_screen;
        struct stat before{}, after{};
        if (stat(path.c_str(), &before)) {
            result.error = "图片不可读"; result.failure_kind = ContentFailureKind::IoError; return;
        }
        if (!S_ISREG(before.st_mode) || before.st_size < 8 || before.st_size > 2 * 1024 * 1024) {
            result.failure_kind = ContentFailureKind::InvalidFormat; result.error = "图片不完整或超过 2 MiB"; continue;
        }
        const auto identity = sha256_file(path, stop);
        if (identity.empty() || stop()) {
            if (!stop()) result.failure_kind = ContentFailureKind::IoError;
            result.error = "图片读取失败或已取消"; return;
        }
        const auto source_failure = [&] {
            if (stop()) return result.failure_kind;
            if (stat(path.c_str(), &after)) return ContentFailureKind::IoError;
            if (before.st_size != after.st_size || before.st_mtime != after.st_mtime)
                return ContentFailureKind::SourceChanged;
            const auto current_identity = sha256_file(path, stop);
            if (stop()) return result.failure_kind;
            if (current_identity.empty()) return ContentFailureKind::IoError;
            return current_identity == identity ? ContentFailureKind::None : ContentFailureKind::SourceChanged;
        };
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
            if (!available) { result.failure_kind = ContentFailureKind::OutOfMemory; result.error = "图片准备内存不足"; }
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
            if (!prepare_canvas()) return;
            if (!read_lock_screen_cache(*reader, header, result.image.get(), alpha, stop)) {
                result.image.reset(); result.cache_finished = false; result.failure_kind = ContentFailureKind::IoError;
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
                result.failure_kind = ContentFailureKind::IoError; result.error = "图片读取失败或已改变"; return;
            }
            if (!std::memcmp(signature.data(), "\x89PNG\r\n\x1a\n", 8)) {
                const bool preserve_background = attempts > 1 && request.sleep_overlay && request.lock_background;
#ifdef ESP_PLATFORM
                const auto output_bytes = request.lock_screen_prepare ||
                    (request.sleep_overlay && request.lock_background && !preserve_background) ? 0U : count;
                if (heap_caps_get_free_size(MALLOC_CAP_SPIRAM) < output_bytes + 768U * 1024 ||
                    heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) < 128U * 1024) {
                    result.failure_kind = ContentFailureKind::OutOfMemory; result.error = "图片准备内存不足"; return;
                }
#endif
                if (!request.lock_screen_prepare && !prepare_canvas(preserve_background)) return;
                const bool rendered = stream_lock_png(store, source, header, result.image.get(), alpha,
                                                       result.cache_finished, result.error, result.failure_kind, stop, source_failure);
                if (!rendered) {
                    result.image.reset();
                    if (stop()) return;
                    if (result.failure_kind == ContentFailureKind::OutOfMemory) {
                        result.error = "图片准备内存不足"; return;
                    }
                    if (result.retryable_failure()) return;
                    result.error = "图片损坏、不支持或像素超限"; continue;
                }
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
                result.failure_kind = ContentFailureKind::OutOfMemory; result.error = "图片准备内存不足"; return;
            }
#endif
            std::vector<std::uint8_t> encoded;
            if (stop() || store.read_lock_screen(result.lock_screen, encoded, stop) != StoreStatus::Ok) {
                if (!stop()) result.failure_kind = ContentFailureKind::IoError;
                result.error = "图片读取失败或已取消"; return;
            }
            bool rendered;
            {
                graphics::Canvas image;
                rendered = graphics::render_image(image, encoded, 0, 0, 684, 1216, result.error, false, stop, &result.failure_kind);
                if (rendered) pixels.assign(reinterpret_cast<const char*>(image.data()), count);
            }
            std::vector<std::uint8_t>().swap(encoded);
            if (stop()) { result.error = "休眠图片准备已取消或超时"; return; }
            if (!rendered) {
                if (result.failure_kind == ContentFailureKind::OutOfMemory) {
                    result.error = "图片准备内存不足"; return;
                }
                if (result.retryable_failure()) return;
                result.error = "图片损坏、不支持或像素超限"; continue;
            }
            result.failure_kind = source_failure();
            if (result.failure_kind != ContentFailureKind::None) {
                result.error = "图片已改变，请重试"; return;
            }
            const auto digest = md5_hex(pixels);
            result.cache_finished = store.save_cache(cache, pixels, header + (alpha ? 'A' : 'G'), digest) == StoreStatus::Ok;
            if (!result.cache_finished) {
                result.failure_kind = ContentFailureKind::IoError; result.error = "休眠图片缓存未保存，下次将重新准备";
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

std::string cover_failure_payload(const std::string& identity, ContentFailureKind kind) {
    if (identity.size() != 64 || !content_failure_is_permanent(kind)) return {};
    return std::string("NEG1\n") + (kind == ContentFailureKind::MissingResource ? "M\n" : "I\n") + identity;
}
ContentFailureKind cached_cover_failure(std::string_view payload, const std::string& identity) {
    if (identity.size() != 64 || payload.size() != kCoverFailureBytes ||
        payload.substr(0, 5) != "NEG1\n" || payload[6] != '\n' || payload.substr(7) != identity)
        return ContentFailureKind::None;
    return payload[5] == 'M' ? ContentFailureKind::MissingResource :
           payload[5] == 'I' ? ContentFailureKind::InvalidFormat : ContentFailureKind::None;
}

void ContentJob::prepare_cover() {
    const auto stop = [this] {
        if (!cancelled_.load(std::memory_order_relaxed)) return false;
        result.failure_kind = ContentFailureKind::Cancelled; return true;
    };
    FileStore store(request.root);
    const auto width = request.layout.width, height = request.layout.height;
    const auto count = static_cast<std::size_t>((width + 1) / 2) * height;
    if (stop()) return;
    std::string identity;
    {
        std::unique_ptr<CacheReader> reader;
        std::string header(request.cache_header.size(), '\0');
        if (store.open_cache(request.cache_name, header.size() + std::max(count, kCoverFailureBytes), reader) == StoreStatus::Ok &&
            reader->read(header.data(), header.size()) && header == request.cache_header) {
            if (reader->size() == header.size() + kCoverFailureBytes) {
                std::array<char, kCoverFailureBytes> payload;
                if (reader->read(payload.data(), payload.size())) {
                    reader.reset();
                    identity = sha256_file(request.path, stop);
                    if (stop()) return;
                    result.failure_kind = cached_cover_failure({payload.data(), payload.size()}, identity);
                    if (content_failure_is_permanent(result.failure_kind)) { result.cache_finished = true; return; }
                }
            }
            if (reader && reader->size() == header.size() + count) {
#ifdef ESP_PLATFORM
                if (heap_caps_get_free_size(MALLOC_CAP_SPIRAM) < count + 256U * 1024 ||
                    heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) < count) {
                    result.failure_kind = ContentFailureKind::OutOfMemory;
                    if (request.kind == Kind::SleepCover) result.error = "Cover cache memory unavailable";
                    return;
                }
#endif
                auto image = std::make_unique<graphics::Canvas>(width, height);
                std::size_t at = 0;
                while (at < count && !stop()) {
                    const auto size = std::min<std::size_t>(16 * 1024, count - at);
                    if (!reader->read(reinterpret_cast<char*>(image->data() + at), size)) break;
                    at += size;
                }
                if (at == count && !stop()) {
                    result.image = std::move(image);
                    result.cache_finished = true;
                    return;
                }
            }
        }
    }
    if (stop()) return;
#ifdef ESP_PLATFORM
    if (heap_caps_get_free_size(MALLOC_CAP_SPIRAM) < cold_image_memory_required_bytes ||
        heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) < count) {
        result.failure_kind = ContentFailureKind::OutOfMemory;
        if (request.kind == Kind::SleepCover) result.error = "Cover preparation memory unavailable";
        return;
    }
#endif
    if (identity.empty()) identity = sha256_file(request.path, stop);
    if (stop()) return;
    if (identity.empty()) { result.failure_kind = ContentFailureKind::IoError; result.error = "Cannot read cover source"; return; }
    std::vector<std::uint8_t> encoded;
    std::string error;
    auto image = std::make_unique<graphics::Canvas>(width, height);
    const bool read = EpubDocument::read_cover(request.path, encoded, error, stop, &result.failure_kind);
    const bool loaded = read && graphics::render_image(*image, encoded, 0, 0, width, height, error, request.cover_crop, stop, &result.failure_kind);
    if (stop()) return;
    if (!loaded && request.kind == Kind::SleepCover) result.error = error;
    std::vector<std::uint8_t>().swap(encoded);
    if (loaded) {
        std::string bytes = request.cache_header;
        bytes.append(reinterpret_cast<const char*>(image->data()), image->pixels().size());
        result.cache_finished = store.save_cache(request.cache_name, bytes) == StoreStatus::Ok;
        result.image = std::move(image);
    } else if (content_failure_is_permanent(result.failure_kind)) {
        const auto current_identity = sha256_file(request.path, stop);
        if (stop()) return;
        if (current_identity.empty()) { result.failure_kind = ContentFailureKind::IoError; return; }
        if (current_identity != identity) { result.failure_kind = ContentFailureKind::SourceChanged; return; }
        result.cache_finished = store.save_cache(request.cache_name,
            request.cache_header + cover_failure_payload(identity, result.failure_kind)) == StoreStatus::Ok;
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
    const auto detail = result.configuration.settings.original_epub ? ChapterLoad::Original : ChapterLoad::Reading;
    const auto revision = document.metadata().txt_encoding == TxtEncoding::Utf8 ? request.content_revision :
        txt_content_revision(document.metadata().txt_encoding);
    result.progress = store.load_progress(request.relative_path, document.metadata().book_id, revision);
    if (request.kind == Kind::Open && !stop())
        result.bookmarks = store.load_bookmarks(request.relative_path, document.metadata().book_id, revision);
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
            !find_content(store, document, result.chapter_index, result.chapter, result.error, detail, match, stop, &result.content_cache, !!request.chapter_session) ||
            !result.chapter.offset_for(anchor, offset)) {
            if (result.error.empty()) result.error = "Saved position cannot be mapped";
            return;
        }
        result.offset = offset;
    } else {
        while (result.chapter_index + 1 < document.spine().size() && !document.spine()[result.chapter_index].linear)
            ++result.chapter_index;
        if (!load_content(store, document, result.chapter_index, result.chapter, result.error, detail, 0, stop, &result.content_cache, false, !!request.chapter_session)) return;
    }
    result.author_fonts = prepare_embedded_fonts(store, document, result.chapter, result.author_fonts_stable, stop);
    if (stop()) return;
    if (stat(request.path.c_str(), &after) != 0 || before.st_size != after.st_size ||
        before.st_mtime != after.st_mtime || before.st_ino != after.st_ino) {
        result.error = "Book changed while opening"; return;
    }
    result.file_size = after.st_size; result.file_modified = after.st_mtime; result.file_inode = after.st_ino;
    if (request.chapter_session) {
        if (auto reader = document.detach_reader()) {
            request.chapter_session->document = std::move(*reader);
            request.chapter_session->path = request.path;
            result.chapter_stream_bytes = request.chapter_session->document.retained_bytes();
        } else document.reset_chapter_stream();
    }
}
bool ContentJob::checkpoint() {
#ifdef CONFIG_PICO_PAGE_LEGACY_PERF_LOG
    std::optional<StageTimer> pause_timer;
#endif
    while (!cancelled_.load(std::memory_order_relaxed)) {
        const auto generation = pause_generation_.load(std::memory_order_acquire);
        if (!(generation & 1U)) break;
#ifdef CONFIG_PICO_PAGE_LEGACY_PERF_LOG
        if (!pause_timer) pause_timer.emplace("content_pause", 0);
#endif
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
    const StageTimer timer{request.chapter_prefetch ? "chapter_prefetch_worker" : "chapter_worker", 0};
    const auto stop = [this] { return checkpoint(); };
    EpubDocument local;
    auto& document = request.chapter_session ? request.chapter_session->document : local;
    if (request.chapter_session && (request.chapter_session->path != request.path ||
        document.metadata().book_id != request.identity.book_id)) {
        document.close(); request.chapter_session->path = request.path;
    }
    if (request.target != Target::Start) document.reset_chapter_stream();
    FileStore store(request.root);
    result.chapter_index = request.chapter;
    const auto detail = request.layout.original_epub ? ChapterLoad::Original : ChapterLoad::Reading;
    const bool locate = request.target == Target::Anchor || request.target == Target::Fragment;
    bool cached = false;
    if (!locate && !request.chapter_resource.empty()) {
        if (request.content_cache)
            cached = result.retained_cache_hit = load_content_cache(store, request.identity.book_id,
                request.chapter_resource, request.chapter, request.part, result.chapter, stop, request.content_cache.get(),
                nullptr, request.layout.original_epub);
        if (!cached && !stop()) cached = load_content_cache(store, request.identity.book_id,
            request.chapter_resource, request.chapter, request.part, result.chapter, stop, nullptr, &result.content_cache,
            request.layout.original_epub);
    }
    if (stop()) { result.error = kChapterCancelled; return; }
    if ((!cached || !result.chapter.author_fonts.empty()) && document.metadata().book_id.empty() &&
        !document.open_verified(request.path, request.identity, result.error, stop, request.book_index)) return;
    if (stop()) { result.error = kChapterCancelled; return; }
    std::uint32_t position = 0;
    if (request.target == Target::Anchor || request.target == Target::Fragment) {
        const auto match = [&](const Chapter& part) {
            return request.target == Target::Anchor ? part.offset_for(request.anchor, position) :
                part.fragment_offset(request.fragment, position);
        };
        if (request.target == Target::Fragment && request.fragment.empty()) {
            if (!load_content(store, document, request.chapter, result.chapter, result.error, detail, 0, stop, &result.content_cache)) return;
        } else if (!find_content(store, document, request.chapter, result.chapter, result.error, detail, match, stop, &result.content_cache)) return;
        result.offset = position;
        if (request.target == Target::Fragment && result.offset >= result.chapter.text.size() && !result.chapter.last_part) {
            const auto next = result.chapter.part + 1;
            if (!load_content(store, document, request.chapter, result.chapter, result.error, detail, next, stop, &result.content_cache)) return;
            result.offset = 0;
        }
        result.author_fonts = prepare_embedded_fonts(store, document, result.chapter, result.author_fonts_stable, stop);
        return;
    }
    if (!cached && !load_content(store, document, request.chapter, result.chapter, result.error, detail,
        request.part, stop, &result.content_cache, request.chapter < document.spine().size() &&
        request.chapter_resource == document.spine()[request.chapter].resource,
        request.chapter_session && request.target == Target::Start)) return;
    result.author_fonts = prepare_embedded_fonts(store, document, result.chapter, result.author_fonts_stable, stop);
    if (stop()) return;
    if (request.target == Target::Start) return;
    if (result.author_fonts_stable && request.known_start && *request.known_start < result.chapter.text.size() &&
        (!request.known_end || *request.known_end == result.chapter.text.size())) { result.offset = *request.known_start; return; }
    graphics::Font font;
    if (!(request.font.empty() ? font.open_fallback() : font.open(request.font))) { result.error = font.error(); return; }
    const auto limit = request.target == Target::End ? result.chapter.text.size() : request.offset;
    std::size_t start = 0;
    result.offset = 0;
    while (start < limit && !stop()) {
        const auto page = plan_content(font, result.chapter, start, request.layout, false, stop, &result.author_fonts);
        if (!page.error.empty() || page.end <= start) {
            result.error = page.error.empty() ? "Pagination made no progress" : page.error;
            return;
        }
        if (page.start >= limit) break;
        result.offset = page.start;
        start = readable_offset(result.chapter, page.end, request.layout.original_epub);
        if (result.history.size() == 64) result.history.erase(result.history.begin());
        result.history.emplace_back(static_cast<std::uint32_t>(page.start), static_cast<std::uint32_t>(start));
    }
}
void ContentJob::library_covers() {
    const StageTimer timer{"library_covers"};
    const auto stop = [this] {
        if (!cancelled_.load(std::memory_order_relaxed)) return false;
        result.failure_kind = ContentFailureKind::Cancelled; return true;
    };
    FileStore store(request.root);
    for (const auto& source : request.covers) {
        if (stop()) return;
        result.covers.emplace_back();
#ifdef ESP_PLATFORM
        constexpr std::size_t thumbnail_bytes = kLibraryCoverWidth * kLibraryCoverHeight / 2;
        if (heap_caps_get_free_size(MALLOC_CAP_SPIRAM) < 2 * thumbnail_bytes + 768U * 1024 ||
            heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) < thumbnail_bytes) {
            result.failure_kind = ContentFailureKind::OutOfMemory; continue;
        }
#endif
        const auto path = request.root + "/" + source.path;
        const auto identity = sha256_file(path, stop);
        if (stop()) return;
        if (identity.empty()) { result.failure_kind = ContentFailureKind::IoError; continue; }
        std::string cached;
        if (store.load_cache(source.cache_name, source.cache_header.size() + kCoverFailureBytes, cached) == StoreStatus::Ok &&
            cached.compare(0, source.cache_header.size(), source.cache_header) == 0) {
            const auto failure = cached_cover_failure(std::string_view(cached).substr(source.cache_header.size()), identity);
            if (content_failure_is_permanent(failure)) {
                result.failure_kind = failure; result.covers.back().emplace(); continue;
            }
        }
        graphics::Canvas thumbnail(kLibraryCoverWidth, kLibraryCoverHeight);
        std::vector<std::uint8_t> encoded;
        std::string error;
        ContentFailureKind failure_kind;
        const bool read = EpubDocument::read_cover(request.root + "/" + source.path, encoded, error, stop, &failure_kind);
        const bool loaded = read &&
            graphics::render_image(thumbnail, encoded, 0, 0, kLibraryCoverWidth, kLibraryCoverHeight, error, true, stop, &failure_kind);
        if (stop()) return;
        if (!loaded) { result.failure_kind = failure_kind; result.error = error; }
        struct stat info{};
        if (stat((request.root + "/" + source.path).c_str(), &info) != 0 ||
            info.st_size != source.size || info.st_mtime != source.modified) {
            result.failure_kind = ContentFailureKind::SourceChanged; continue;
        }
        if (!loaded && !content_failure_is_permanent(failure_kind)) continue;
        if (!loaded) {
            const auto current_identity = sha256_file(path, stop);
            if (stop()) return;
            if (current_identity.empty()) { result.failure_kind = ContentFailureKind::IoError; continue; }
            if (current_identity != identity) { result.failure_kind = ContentFailureKind::SourceChanged; continue; }
        }
        auto& pixels = result.covers.back().emplace();
        if (loaded) pixels = thumbnail.pixels();
        std::vector<std::uint8_t>().swap(encoded);
        store.save_cache(source.cache_name, source.cache_header + (loaded ? std::string(pixels.begin(), pixels.end()) : cover_failure_payload(identity, failure_kind)));
    }
}

}
