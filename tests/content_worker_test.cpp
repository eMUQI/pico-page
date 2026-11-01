#include "../firmware/components/reader_app/content_job.h"
#include "freertos/FreeRTOS.h"
#include "esp_heap_caps.h"
#include <cassert>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>
#include <sys/stat.h>
#include <unistd.h>

namespace diagnostics = pico_page::diagnostics;

namespace {
struct Queue {
    std::mutex mutex;
    std::condition_variable changed;
    void* item = nullptr;
    bool receive_enabled = false, shutdown = false;
};
struct StopWorker {};
Queue* queue = nullptr;
std::thread worker;
bool fail_queue = false, fail_task = false;
unsigned allocations = 0, creations = 0;
#ifdef PICO_PAGE_WRAP_CALLOC_TEST
std::atomic<unsigned> fail_calloc_on{0}, failed_callocs{0};
std::string counted_dictionary;
std::string unreadable_cover;
std::atomic<unsigned> preparation_attempts{0};
#endif
std::size_t internal_free = 128 * 1024, internal_largest = 64 * 1024;
std::size_t psram_free = 4 * 1024 * 1024, psram_largest = 3 * 1024 * 1024;
void finish(pico_page::ContentJob& job) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!job.done() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    assert(job.done());
    job.wait();
}
}
#ifdef PICO_PAGE_WRAP_CALLOC_TEST
extern "C" FILE* __real_fopen(const char*, const char*);
extern "C" FILE* __wrap_fopen(const char* path, const char* mode) {
    if (path == counted_dictionary) ++preparation_attempts;
    if (path == unreadable_cover) { errno = EIO; return nullptr; }
    return __real_fopen(path, mode);
}
extern "C" void* __real_calloc(std::size_t count, std::size_t size);
extern "C" void* __wrap_calloc(std::size_t count, std::size_t size) {
    auto remaining = fail_calloc_on.load();
    while (remaining && !fail_calloc_on.compare_exchange_weak(remaining, remaining - 1)) {}
    if (remaining == 1) { ++failed_callocs; return nullptr; }
    return __real_calloc(count, size);
}
#endif
QueueHandle_t xQueueCreate(unsigned capacity, unsigned item_size) {
    assert(capacity == 1 && item_size == sizeof(void*));
    if (fail_queue) return nullptr;
    ++allocations;
    return queue = new Queue;
}
void vQueueDelete(QueueHandle_t value) {
    assert(value == queue);
    delete queue; queue = nullptr; --allocations;
}
BaseType_t xQueueSend(QueueHandle_t value, const void* item, TickType_t wait) {
    assert(value == queue && wait == 0);
    std::lock_guard<std::mutex> lock(queue->mutex);
    if (queue->item) return 0;
    std::memcpy(&queue->item, item, sizeof(void*));
    queue->changed.notify_one();
    return pdTRUE;
}
BaseType_t xQueueReceive(QueueHandle_t value, void* item, TickType_t wait) {
    assert(value == queue && wait == portMAX_DELAY);
    std::unique_lock<std::mutex> lock(queue->mutex);
    queue->changed.wait(lock, [] { return queue->shutdown || (queue->receive_enabled && queue->item); });
    if (queue->shutdown) throw StopWorker{};
    std::memcpy(item, &queue->item, sizeof(void*));
    queue->item = nullptr;
    return pdTRUE;
}
BaseType_t xTaskCreate(void (*entry)(void*), const char*, unsigned stack_bytes, void* argument,
                       unsigned priority, void* handle) {
    assert(stack_bytes == 32768 && priority == 1 && handle == nullptr);
    if (fail_task) return 0;
    ++creations;
    worker = std::thread([=] { try { entry(argument); } catch (const StopWorker&) {} });
    return pdPASS;
}
unsigned uxTaskGetStackHighWaterMark(void*) { return 8192; }
void vTaskDelay(TickType_t) { std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
void taskYIELD() { std::this_thread::yield(); }
std::size_t heap_caps_get_free_size(std::uint32_t caps) {
    return caps == MALLOC_CAP_SPIRAM ? psram_free : internal_free;
}
std::size_t heap_caps_get_largest_free_block(std::uint32_t caps) {
    return caps == MALLOC_CAP_SPIRAM ? psram_largest : internal_largest;
}
std::size_t heap_caps_get_minimum_free_size(std::uint32_t caps) { return heap_caps_get_free_size(caps); }

int main(int argc, char** argv) {
    assert(argc == 3);
    using pico_page::ContentJob;
    using Failure = ContentJob::StartFailure;
    fail_queue = true;
    assert(ContentJob::prepare_worker() == Failure::InternalMemory && allocations == 0);
    fail_queue = false; fail_task = true;
    assert(ContentJob::prepare_worker() == Failure::TaskCreate && allocations == 0);
    internal_largest = 31744;
    assert(ContentJob::prepare_worker() == Failure::InternalMemory && allocations == 0);
    fail_task = false; internal_largest = 64 * 1024;
    assert(ContentJob::prepare_worker() == Failure::None);
    assert(ContentJob::prepare_worker() == Failure::None && creations == 1 && allocations == 1);

    // Reuse must not require another contiguous stack, even when task creation would fail.
    internal_largest = 1024; internal_free = 40 * 1024; fail_task = true;
    ContentJob::Request request;
    request.kind = ContentJob::Kind::MapUpload;
    ContentJob cancelled(request), rejected(request);
    assert(cancelled.start() && !cancelled.start());
    cancelled.cancel();
    assert(!cancelled.done());
    assert(!rejected.start() && rejected.start_failure() == Failure::TaskCreate);
    {
        std::lock_guard<std::mutex> lock(queue->mutex);
        queue->receive_enabled = true; queue->changed.notify_one();
    }
    finish(cancelled);
    assert(cancelled.result.error.empty());
    assert(rejected.start()); finish(rejected);
    assert(!rejected.result.error.empty());
    for (unsigned i = 0; i < 100; ++i) {
        ContentJob job(request);
        assert(job.start()); finish(job);
        assert(!job.result.error.empty());
    }
    request.kind = ContentJob::Kind::Pagination;
    ContentJob optional(request);
    optional.cancel();
    assert(optional.start()); finish(optional);
    internal_free = 31 * 1024;
    ContentJob low_memory(request);
    assert(!low_memory.start() && low_memory.start_failure() == Failure::InternalMemory);
    request.kind = ContentJob::Kind::Chapter; request.chapter_prefetch = true;
    ContentJob low_memory_prefetch(request);
    assert(!low_memory_prefetch.start() && low_memory_prefetch.start_failure() == Failure::InternalMemory);
    internal_free = 40 * 1024;
    ContentJob admitted_prefetch(request); admitted_prefetch.cancel();
    assert(admitted_prefetch.start()); finish(admitted_prefetch);
    {
        namespace fs = std::filesystem;
        char temporary[] = "/tmp/pico-cover-worker-XXXXXX";
        const char* directory = mkdtemp(temporary); assert(directory);
        const fs::path root(directory), cache = root / "PicoPage/cache/covers/sleep";
        ContentJob::Request cover;
        cover.kind = ContentJob::Kind::SleepCover;
        cover.root = root.string(); cover.path = (root / "book.epub").string();
        cover.cache_name = "covers/sleep"; cover.cache_header = "PPCOVER3\nprobe\n";
        cover.layout.width = 684; cover.layout.height = 1040; cover.cover_crop = true;
        fs::copy_file(argv[1], cover.path);
        {
            pico_page::graphics::Canvas pixels;
            pixels.clear(6);
            pico_page::FileStore store(root.string());
            assert(pico_page::write_body_image_cache(store, "images/gray-v1/memory", pixels, {}));
            ContentJob::Request body;
            body.kind = ContentJob::Kind::Image; body.root = root.string();
            body.cache_name = "images/gray-v1/memory"; body.layout.height = 1216;
            body.page.images.push_back({0, 0, 0, 684, 1216, "EPUB/cover.png"});
            const auto decoded = pico_page::graphics::image_decode_count();
            psram_free = 342 * 1216 + 256 * 1024; psram_largest = 342 * 1216;
            ContentJob hot(body); assert(hot.start()); finish(hot);
            assert(hot.result.error.empty() && hot.result.image && hot.result.image->pixels() == pixels.pixels());
            for (bool fragmented : {false, true}) {
                psram_free = 342 * 1216 + 256 * 1024 - (fragmented ? 0 : 1);
                psram_largest = 342 * 1216 - (fragmented ? 1 : 0);
                ContentJob refused(body); assert(refused.start()); finish(refused);
                assert(!refused.result.error.empty() && !refused.result.image);
            }
            psram_free = 342 * 1216 + 256 * 1024; psram_largest = 342 * 1216;
            assert(store.save_cache(body.cache_name, "invalid") == pico_page::StoreStatus::Ok);
            ContentJob cold_refused(body); assert(cold_refused.start()); finish(cold_refused);
            assert(!cold_refused.result.error.empty() && !cold_refused.result.image);
            assert(pico_page::graphics::image_decode_count() == decoded);
            psram_free = 4 * 1024 * 1024; psram_largest = 3 * 1024 * 1024;
        }
        fs::create_directories(root / "LockScreens");
        std::ofstream(root / "LockScreens/test.png", std::ios::binary) << "invalid image";
        ContentJob::Request custom;
        custom.kind = ContentJob::Kind::LockScreen; custom.root = root.string(); custom.lock_screen_prepare = true;
        const auto custom_decodes = pico_page::graphics::image_decode_count();
        for (bool fragmented : {false, true}) {
            psram_free = fragmented ? 4 * 1024 * 1024 : 1024 * 1024;
            psram_largest = fragmented ? 342 * 1216 - 1 : 3 * 1024 * 1024;
            ContentJob refused(custom); assert(refused.start()); finish(refused);
            assert(!refused.result.cache_finished && !refused.result.image && !refused.result.error.empty() &&
                   refused.result.retryable_failure);
            assert(pico_page::graphics::image_decode_count() == custom_decodes);
        }
        psram_free = 2 * 1024 * 1024; psram_largest = 512 * 1024;
        ContentJob admitted(custom); assert(admitted.start()); finish(admitted);
        assert(!admitted.result.cache_finished && !admitted.result.retryable_failure);
        assert(admitted.result.error.find("损坏") != std::string::npos);
        std::ofstream(root / "LockScreens/test.png", std::ios::binary) << std::string(1800 * 1024, 'x');
        const auto before_large = pico_page::graphics::image_decode_count();
        ContentJob too_large(custom); assert(too_large.start()); finish(too_large);
        assert(too_large.result.retryable_failure && !too_large.result.cache_finished);
        assert(pico_page::graphics::image_decode_count() == before_large);
        psram_free = 4 * 1024 * 1024; psram_largest = 3 * 1024 * 1024;
        ContentJob cancelled_custom(custom); cancelled_custom.cancel();
        assert(cancelled_custom.start()); finish(cancelled_custom);
        assert(!cancelled_custom.result.cache_finished && !cancelled_custom.result.image && cancelled_custom.result.retryable_failure);
        assert(!fs::exists(root / "PicoPage/data/last-lock-screen.txt"));

        std::vector<std::uint8_t> png;
        std::string error;
        assert(pico_page::EpubDocument::read_cover(argv[1], png, error));
        std::ofstream(root / "LockScreens/01-good.png", std::ios::binary).write(
            reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
        custom.preview = true; custom.lock_screen_prepare = false;
#ifdef PICO_PAGE_WRAP_CALLOC_TEST
        std::ofstream(root / "LockScreens/02-good.png", std::ios::binary).write(
            reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
        const auto before_failure = pico_page::graphics::image_decode_count();
        fail_calloc_on = 2; // Decoder creation succeeds; its scanline allocation fails.
        ContentJob exhausted_custom(custom); assert(exhausted_custom.start()); finish(exhausted_custom);
        assert(fail_calloc_on == 0 && failed_callocs == 1);
        assert(!exhausted_custom.result.image && !exhausted_custom.result.cache_finished && exhausted_custom.result.retryable_failure);
        assert(exhausted_custom.result.error.find("内存不足") != std::string::npos);
        assert(pico_page::graphics::image_decode_count() == before_failure + 1);
        assert(!fs::exists(root / "PicoPage/cache/lockscreens/current"));
        assert(!fs::exists(root / "PicoPage/data/last-lock-screen.txt"));
#endif
        const auto before_cold = pico_page::graphics::image_decode_count();
        ContentJob cold_custom(custom); assert(cold_custom.start()); finish(cold_custom);
        assert(cold_custom.result.image && cold_custom.result.cache_finished && cold_custom.result.lock_screen == "01-good.png");
        assert(pico_page::graphics::image_decode_count() == before_cold + 1);
        assert(!fs::exists(root / "PicoPage/data/last-lock-screen.txt"));
        psram_free = 1024 * 1024;
        ContentJob warm_custom(custom); assert(warm_custom.start()); finish(warm_custom);
        assert(warm_custom.result.image && warm_custom.result.cache_finished && !warm_custom.result.retryable_failure);
        assert(warm_custom.result.image->pixels() == cold_custom.result.image->pixels());
        for (bool fragmented : {false, true}) {
            psram_free = fragmented ? 4 * 1024 * 1024 : 342 * 1216 + 256 * 1024 - 1;
            psram_largest = fragmented ? 342 * 1216 - 1 : 3 * 1024 * 1024;
            ContentJob refused_warm_custom(custom); assert(refused_warm_custom.start()); finish(refused_warm_custom);
            assert(!refused_warm_custom.result.image && refused_warm_custom.result.cache_finished &&
                   refused_warm_custom.result.retryable_failure);
            assert(pico_page::graphics::image_decode_count() == before_cold + 1);
        }
        psram_free = 4 * 1024 * 1024;
        psram_largest = 3 * 1024 * 1024;
        custom.lock_screen_deadline = std::chrono::steady_clock::now() - std::chrono::milliseconds(1);
        ContentJob expired_custom(custom); assert(expired_custom.start()); finish(expired_custom);
        assert(!expired_custom.result.image && expired_custom.result.retryable_failure);
        assert(pico_page::graphics::image_decode_count() == before_cold + 1);
        // A job past its deadline still records the presented image before it returns.
        custom.lock_screen_shown = "01-good.png";
        ContentJob cursor_commit(custom); assert(cursor_commit.start()); finish(cursor_commit);
        assert(cursor_commit.result.cursor_saved && !cursor_commit.result.image);
        assert(pico_page::FileStore(root.string()).last_lock_screen() == "01-good.png");
        custom.lock_screen_shown.clear();

        constexpr std::size_t cover_bytes = 684U * 1040U / 2;
        for (const bool fragmented : {false, true}) {
            psram_free = fragmented ? 4 * 1024 * 1024 : ContentJob::image_room - 1;
            psram_largest = fragmented ? cover_bytes - 1 : 3 * 1024 * 1024;
            ContentJob deferred(cover); assert(deferred.start()); finish(deferred);
            assert(!deferred.result.cache_finished && !deferred.result.image && !fs::exists(cache));
        }
        psram_free = 4 * 1024 * 1024; psram_largest = 3 * 1024 * 1024;
        {
            std::lock_guard<std::mutex> lock(queue->mutex);
            queue->receive_enabled = false;
        }
        ContentJob cancelled_cover(cover); assert(cancelled_cover.start()); cancelled_cover.cancel();
        {
            std::lock_guard<std::mutex> lock(queue->mutex);
            queue->receive_enabled = true; queue->changed.notify_one();
        }
        finish(cancelled_cover);
        assert(!cancelled_cover.result.cache_finished && !fs::exists(cache));
        fs::rename(cover.path, root / "hidden.epub");
        ContentJob unreadable(cover); assert(unreadable.start()); finish(unreadable);
        assert(!unreadable.result.cache_finished && !fs::exists(cache));
        fs::rename(root / "hidden.epub", cover.path);
        fs::create_directories(cache.parent_path());
        fs::create_directory(cache.string() + ".tmp");
        // A small cover fits even when neither the old total nor contiguous 2 MiB gate is met.
        psram_free = 2 * 1024 * 1024; psram_largest = 1024 * 1024;
        ContentJob unwritable(cover); assert(unwritable.start()); finish(unwritable);
        assert(!unwritable.result.cache_finished && !fs::exists(cache));
        assert(unwritable.result.image && unwritable.result.image->pixel(50, 100) == 0 &&
               unwritable.result.image->pixel(50, 900) == 5);
        fs::remove(cache.string() + ".tmp");
        ContentJob recovered(cover); assert(recovered.start()); finish(recovered);
        assert(recovered.result.cache_finished && recovered.result.image);
        assert(fs::file_size(cache) == cover.cache_header.size() + 684U * 1040U / 2);
        const auto decodes = pico_page::graphics::image_decode_count();
        psram_free = cover_bytes + 256 * 1024; psram_largest = cover_bytes;
        ContentJob cached(cover); assert(cached.start()); finish(cached);
        assert(cached.result.cache_finished && cached.result.image);
        assert(cached.result.image->pixels() == recovered.result.image->pixels());
        assert(pico_page::graphics::image_decode_count() == decodes);
        psram_largest = cover_bytes - 1;
        ContentJob fragmented_cache(cover); assert(fragmented_cache.start()); finish(fragmented_cache);
        assert(!fragmented_cache.result.image && fs::exists(cache));
        {
            const pico_page::graphics::Canvas pixels;
            std::string bytes(reinterpret_cast<const char*>(pixels.data()), pixels.pixels().size());
            const auto valid = bytes + pico_page::md5_hex(bytes);
            pico_page::FileStore store(root.string());
            ContentJob::Request image;
            image.kind = ContentJob::Kind::Image; image.root = root.string();
            image.path = (root / "missing.epub").string(); image.cache_name = "images/admission";
            image.page.images.push_back({0, 24, 64, 636, image.layout.height, "cover.png"});
            assert(store.save_cache(image.cache_name, valid) == pico_page::StoreStatus::Ok);
            {
                psram_free = 2560 * 1024; psram_largest = 1024 * 1024;
                ContentJob warm(image); assert(warm.start()); finish(warm);
                assert(warm.result.error.empty() && warm.result.image);
                assert(warm.result.image->pixels() == pixels.pixels());
            }
            psram_free = psram_largest = 512 * 1024;
            ContentJob too_small(image); assert(too_small.start()); finish(too_small);
            assert(!too_small.result.image && !too_small.result.error.empty());
            // Enough for the cached pixels, below the room a cold decode requires.
            psram_free = 1280 * 1024; psram_largest = 512 * 1024;
            for (const auto& invalid : {valid.substr(1), std::string(valid.size(), 'x')}) {
                assert(store.save_cache(image.cache_name, invalid) == pico_page::StoreStatus::Ok);
                ContentJob corrupt(image); assert(corrupt.start()); finish(corrupt);
                assert(!corrupt.result.image && corrupt.result.error == "Image preparation memory unavailable");
            }
            fs::remove(root / "PicoPage/cache" / image.cache_name);
            ContentJob cold(image); assert(cold.start()); finish(cold);
            assert(!cold.result.image && cold.result.error == "Image preparation memory unavailable");
            // With that room the decode is admitted whatever the encoded size limit; the missing book then fails it.
            psram_free = ContentJob::image_room;
            ContentJob admitted(image); assert(admitted.start()); finish(admitted);
            assert(!admitted.result.error.empty() && admitted.result.error != "Image preparation memory unavailable");
            assert(pico_page::graphics::image_decode_count() == decodes);
        }
        psram_free = 4 * 1024 * 1024; psram_largest = 3 * 1024 * 1024;
        cover.path = (fs::path(argv[1]).parent_path() / "valid.epub").string();
        cover.cache_name = "covers/no-cover";
        ContentJob absent(cover); assert(absent.start()); finish(absent);
        assert(absent.result.cache_finished && !absent.result.image);
        assert(fs::file_size(root / "PicoPage/cache/covers/no-cover") == cover.cache_header.size());
        psram_free = psram_largest = 512 * 1024;
        ContentJob cached_absent(cover); assert(cached_absent.start()); finish(cached_absent);
        assert(cached_absent.result.cache_finished && !cached_absent.result.image);
        psram_free = 4 * 1024 * 1024; psram_largest = 3 * 1024 * 1024;
        for (const auto* kind : {"missing", "oversized", "unsupported"}) {
            cover.path = (fs::path(argv[1]).parent_path() / (std::string(kind) + "-image-cover.epub")).string();
            cover.cache_name = std::string("covers/") + kind;
            ContentJob invalid(cover); assert(invalid.start()); finish(invalid);
            assert(invalid.result.cache_finished && !invalid.result.image);
            assert(fs::file_size(root / "PicoPage/cache" / cover.cache_name) == cover.cache_header.size());
        }
        cover.path = (fs::path(argv[1]).parent_path() / "gif-image-cover.epub").string();
        cover.cache_name = "covers/gif-upgrade";
        const auto gif_cache = root / "PicoPage/cache" / cover.cache_name;
        const std::string obsolete = "PPCOVER2\nprobe\n";
        pico_page::FileStore cover_store(root.string());
        assert(cover_store.save_cache(cover.cache_name, obsolete) == pico_page::StoreStatus::Ok);
        std::string preserved;
        const auto gif_decodes = pico_page::graphics::image_decode_count();
        for (const bool fragmented : {false, true}) {
            psram_free = fragmented ? 4 * 1024 * 1024 : ContentJob::image_room - 1;
            psram_largest = fragmented ? cover_bytes - 1 : 3 * 1024 * 1024;
            ContentJob deferred_gif(cover); assert(deferred_gif.start()); finish(deferred_gif);
            assert(!deferred_gif.result.cache_finished && !deferred_gif.result.image);
            assert(cover_store.load_cache(cover.cache_name, obsolete.size(), preserved) == pico_page::StoreStatus::Ok && preserved == obsolete);
            assert(pico_page::graphics::image_decode_count() == gif_decodes);
        }
        psram_free = 4 * 1024 * 1024; psram_largest = 3 * 1024 * 1024;
        ContentJob cancelled_gif(cover); cancelled_gif.cancel(); assert(cancelled_gif.start()); finish(cancelled_gif);
        assert(!cancelled_gif.result.cache_finished);
        assert(cover_store.load_cache(cover.cache_name, obsolete.size(), preserved) == pico_page::StoreStatus::Ok && preserved == obsolete);
        ContentJob recovered_gif(cover); assert(recovered_gif.start()); finish(recovered_gif);
        assert(recovered_gif.result.cache_finished && recovered_gif.result.image);
        assert(fs::file_size(gif_cache) == cover.cache_header.size() + 684U * 1040U / 2);
        assert(pico_page::graphics::image_decode_count() == gif_decodes + 1);
        psram_free = cover_bytes + 256 * 1024; psram_largest = cover_bytes;
        ContentJob cached_gif(cover); assert(cached_gif.start()); finish(cached_gif);
        assert(cached_gif.result.cache_finished && cached_gif.result.image);
        assert(cached_gif.result.image->pixels() == recovered_gif.result.image->pixels());
        assert(pico_page::graphics::image_decode_count() == gif_decodes + 1);
        psram_free = 4 * 1024 * 1024; psram_largest = 3 * 1024 * 1024;
        fs::copy_file(cover.path, root / "gif.epub");
        struct stat gif_info{};
        assert(stat((root / "gif.epub").c_str(), &gif_info) == 0);
        ContentJob::Request library;
        library.kind = ContentJob::Kind::LibraryCovers; library.root = root.string();
        library.covers.push_back({"gif.epub", "covers/gif-library-upgrade", cover.cache_header, gif_info.st_size, gif_info.st_mtime});
        assert(cover_store.save_cache(library.covers[0].cache_name, obsolete) == pico_page::StoreStatus::Ok);
        pico_page::set_chapter_memory_probe([](std::size_t) { return false; });
        ContentJob refused_library(library); assert(refused_library.start()); finish(refused_library);
        pico_page::set_chapter_memory_probe(nullptr);
        assert(refused_library.result.covers.size() == 1 && !refused_library.result.covers[0]);
        assert(cover_store.load_cache(library.covers[0].cache_name, obsolete.size(), preserved) == pico_page::StoreStatus::Ok && preserved == obsolete);
        for (const bool fragmented : {false, true}) {
            psram_free = fragmented ? 4 * 1024 * 1024 : 512 * 1024;
            psram_largest = fragmented ? 23000 - 1 : 3 * 1024 * 1024;
            ContentJob exhausted_library(library); assert(exhausted_library.start()); finish(exhausted_library);
            assert(exhausted_library.result.covers.size() == 1 && !exhausted_library.result.covers[0]);
            assert(cover_store.load_cache(library.covers[0].cache_name, obsolete.size(), preserved) == pico_page::StoreStatus::Ok && preserved == obsolete);
        }
#ifdef PICO_PAGE_WRAP_CALLOC_TEST
        psram_free = 4 * 1024 * 1024; psram_largest = 3 * 1024 * 1024;
        unreadable_cover = (root / "gif.epub").string();
        ContentJob unreadable_library(library); assert(unreadable_library.start()); finish(unreadable_library);
        unreadable_cover.clear();
        assert(unreadable_library.result.covers.size() == 1 && !unreadable_library.result.covers[0]);
        assert(cover_store.load_cache(library.covers[0].cache_name, obsolete.size(), preserved) == pico_page::StoreStatus::Ok && preserved == obsolete);
#endif
        psram_free = 2 * 1024 * 1024; psram_largest = 512 * 1024;
        ContentJob small_library(library); assert(small_library.start()); finish(small_library);
        assert(small_library.result.covers.size() == 1 && small_library.result.covers[0] && !small_library.result.covers[0]->empty());
        psram_free = 4 * 1024 * 1024; psram_largest = 3 * 1024 * 1024;
        ContentJob recovered_library(library); assert(recovered_library.start()); finish(recovered_library);
        assert(recovered_library.result.covers.size() == 1 && recovered_library.result.covers[0] && !recovered_library.result.covers[0]->empty());
        fs::copy_file(fs::path(argv[1]).parent_path() / "valid.epub", root / "no-cover.epub");
        assert(stat((root / "no-cover.epub").c_str(), &gif_info) == 0);
        library.covers = {{"no-cover.epub", "covers/library-no-cover", cover.cache_header, gif_info.st_size, gif_info.st_mtime}};
        ContentJob absent_library(library); assert(absent_library.start()); finish(absent_library);
        assert(absent_library.result.covers.size() == 1 && absent_library.result.covers[0] && absent_library.result.covers[0]->empty());
        assert(fs::file_size(root / "PicoPage/cache" / library.covers[0].cache_name) == cover.cache_header.size());
        fs::remove_all(root);
    }
    {
        namespace fs = std::filesystem;
        namespace dict = pico_page::dictionary;
        char temporary[] = "/tmp/pico-dictionary-worker-XXXXXX";
        const char* directory = mkdtemp(temporary); assert(directory);
        const fs::path root(directory), dictionaries = root / "Dictionaries";
        fs::create_directories(dictionaries / "English");
        for (const auto* suffix : {".ifo", ".idx", ".dict.dz", ".syn.dz"})
            fs::copy_file(fs::path(argv[2]) / (std::string("compressed") + suffix),
                          dictionaries / "English" / (std::string("main") + suffix));
        { std::ofstream bad(dictionaries / "bad.ifo"); bad << "bad metadata"; }
        ContentJob::Request query;
        query.root = root.string(); query.kind = ContentJob::Kind::DictionaryList;
        ContentJob list(query); assert(list.start()); finish(list);
        assert(list.result.dictionary_status && list.result.dictionary_complete && list.result.dictionaries.size() == 2);
        assert(list.result.dictionaries[0].id == "English/main.ifo" && list.result.dictionaries[0].status);
        assert(!list.result.dictionaries[1].status);
        assert(!fs::exists(root / "PicoPage/cache"));
        query.kind = ContentJob::Kind::DictionaryLookup;
        query.relative_path = "English/main.ifo";
        query.dictionary_session = std::make_shared<pico_page::DictionarySession>();
        query.dictionary_candidates = {{{4, 9}, "apple", false}};
        ContentJob first(query); assert(first.start()); finish(first);
        assert(first.result.dictionary_status && first.result.dictionary_match->word == "apple");
        assert(first.result.dictionary_entries.size() == 1 && first.result.dictionary_fields[0].text == "lower <literal>");
        assert(!first.result.dictionary_reused && query.dictionary_session->verified.size() == 1);
        ContentJob warm(query); assert(warm.start()); finish(warm);
        assert(warm.result.dictionary_status && warm.result.dictionary_reused);
        query.dictionary_candidates = {{{0, 7}, "missing", false}, {{4, 9}, "alias", false}};
        ContentJob alias(query); assert(alias.start()); finish(alias);
        assert(alias.result.dictionary_status && alias.result.dictionary_entries.size() == 2);
        assert(alias.result.dictionary_match->word == "alias" && alias.result.dictionary_fields[0].text == "upper");
        query.dictionary_candidates = {{{0, 9}, "duplicate", false}};
        ContentJob duplicates(query); assert(duplicates.start()); finish(duplicates);
        assert(duplicates.result.dictionary_status && duplicates.result.dictionary_more && duplicates.result.dictionary_entries.size() == 32);
        query.dictionary_ordinal = duplicates.result.dictionary_entries.back().ordinal + 1;
        ContentJob continuation(query); assert(continuation.start()); finish(continuation);
        assert(continuation.result.dictionary_status && continuation.result.dictionary_entries[0].ordinal == query.dictionary_ordinal);
        query.kind = ContentJob::Kind::DictionaryRead; query.dictionary_ordinal = 2;
        ContentJob read(query); assert(read.start()); finish(read);
        assert(read.result.dictionary_status && read.result.dictionary_fields[0].text == "lower <literal>");
        query.kind = ContentJob::Kind::DictionaryLookup; query.dictionary_ordinal = 0;
        query.dictionary_candidates = {{{0, 7}, "missing", false}};
        ContentJob missing(query); assert(missing.start()); finish(missing);
        assert(missing.result.dictionary_status.status == dict::Status::NotFound && missing.result.error.empty());
        query.dictionary_candidates = {{{4, 9}, "apple", false}};
        ContentJob paused(query);
        assert(!paused.pause()); assert(paused.start());
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (!paused.pause() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        assert(paused.pause() && !paused.done());
        paused.cancel(); finish(paused);
        assert(paused.result.dictionary_status.status == dict::Status::Cancelled && paused.result.dictionary_fields.empty());
        ContentJob resumed(query);
        assert(!resumed.pause()); assert(resumed.start());
        resumed.resume(); finish(resumed); assert(resumed.result.dictionary_status);
        internal_free = 31 * 1024;
        ContentJob low(query); assert(!low.start() && low.start_failure() == Failure::InternalMemory);
        internal_free = 40 * 1024;
        query.relative_path = "../main.ifo";
        ContentJob unsafe(query); assert(unsafe.start()); finish(unsafe);
        assert(unsafe.result.dictionary_status.status == dict::Status::InvalidFormat);
        // One search opens each source once; a bad IDX tail is fully prepared once per session.
        const auto copy_dictionary = [&](const char* fixture, const char* name) {
            for (const auto* suffix : {".ifo", ".idx", ".dict.dz"})
                fs::copy_file(fs::path(argv[2]) / (std::string(fixture) + suffix),
                              dictionaries / (std::string(name) + suffix), fs::copy_options::overwrite_existing);
        };
        const auto corrupt_tail = [&] {
            std::fstream index(dictionaries / "tail.idx", std::ios::binary | std::ios::in | std::ios::out);
            index.seekp(-1, std::ios::end); index.put(static_cast<char>(255));
        };
        copy_dictionary("query-short", "short");
        copy_dictionary("query-short", "tail"); corrupt_tail();
        const std::vector<std::string> words = {"中", "中文", "中文甲", "中文甲乙", "中文甲乙丙",
            "中文甲乙丙丁", "中文甲乙丙丁戊", "中文甲乙丙丁戊己"};
        std::vector<pico_page::dictionary::Candidate> han;
        for (std::size_t length = 8; length; --length) han.push_back({{0, length}, words[length - 1], false});
        query.dictionary_session = std::make_shared<pico_page::DictionarySession>();
        query.relative_path.clear(); query.dictionary_sources = {"tail.ifo", "short.ifo"};
        query.dictionary_candidates = han;
        const auto search = [&] {
            ContentJob job(query); assert(job.start()); finish(job);
            return std::move(job.result);
        };
#ifdef PICO_PAGE_WRAP_CALLOC_TEST
        counted_dictionary = (dictionaries / "tail.dict.dz").string(); preparation_attempts = 0;
#endif
        auto found = search();
        assert(found.dictionary_status && found.dictionary_match->word == "中");
        assert(found.dictionary_source == 1 && found.dictionary_partial);
        assert(found.dictionary_failed_sources.size() == 1 && found.dictionary_failed_sources[0].id == "tail.ifo");
        // A later lookup in the same session reuses the definitive failure.
        found = search();
        assert(found.dictionary_status && found.dictionary_source == 1 && found.dictionary_partial);
#ifdef PICO_PAGE_WRAP_CALLOC_TEST
        assert(preparation_attempts == 1U);
#endif
        assert(query.dictionary_session->preparation_failures.size() == 1);
        // Explicit retry re-reads the source and observes a repair.
        copy_dictionary("query-short", "tail");
        found = search(); assert(found.dictionary_source == 1 && found.dictionary_partial);
        query.dictionary_retry = true;
        found = search();
        assert(found.dictionary_status && found.dictionary_source == 0 && !found.dictionary_partial);
        assert(query.dictionary_session->preparation_failures.empty());
        assert(found.dictionary_failed_sources.empty());
        query.dictionary_retry = false;
#ifdef PICO_PAGE_WRAP_CALLOC_TEST
        counted_dictionary.clear();
#endif
        // Longer candidates win across sources; equal candidates keep the earlier source.
        copy_dictionary("han-short", "first"); copy_dictionary("han-long", "second");
        copy_dictionary("han-long", "third");
        query.dictionary_sources = {"first.ifo", "second.ifo", "third.ifo"};
        found = search();
        assert(found.dictionary_status && found.dictionary_match->word == "中文" && found.dictionary_source == 1);
        assert(found.dictionary_fields[0].text == "long match" && !found.dictionary_partial);
        query.dictionary_sources = {"second.ifo", "first.ifo"};
        found = search(); assert(found.dictionary_source == 0 && found.dictionary_match->word == "中文");
        // Navigation stops at the first usable source and skips broken, missing and empty sources.
        const auto ranked_session = query.dictionary_session;
        query.dictionary_session = std::make_shared<pico_page::DictionarySession>();
        query.dictionary_navigation = 1;
        query.dictionary_sources = {"first.ifo", "second.ifo", "third.ifo"};
        found = search(); assert(found.dictionary_source == 0 && found.dictionary_match->word == "中");
        query.dictionary_navigation = -1;
        query.dictionary_sources = {"third.ifo", "second.ifo", "first.ifo"};
        found = search(); assert(found.dictionary_source == 0 && found.dictionary_match->word == "中文");
        copy_dictionary("query-short", "navigation-bad");
        { std::fstream index(dictionaries / "navigation-bad.idx", std::ios::binary | std::ios::in | std::ios::out);
          index.seekp(-1, std::ios::end); index.put(static_cast<char>(255)); }
        query.dictionary_candidates = {{{0, 2}, "中文", false}};
        query.dictionary_sources = {"navigation-bad.ifo", "missing.ifo", "first.ifo", "second.ifo", "third.ifo"};
        found = search(); assert(found.dictionary_status && found.dictionary_source == 3);
        assert(found.dictionary_failed_sources.size() == 1 && found.dictionary_failed_sources[0].id == "navigation-bad.ifo");
        query.dictionary_navigation = 0;
        query.dictionary_sources = {"missing.ifo", "navigation-bad.ifo"};
        found = search(); assert(found.dictionary_status.status == dict::Status::IoError && !found.error.empty());
        query.dictionary_sources.assign(33, "first.ifo");
        found = search(); assert(found.dictionary_status.status == dict::Status::ResourceLimit);
        query.dictionary_session = ranked_session; query.dictionary_candidates = han;
        // An unreadable longer entry still lets a shorter candidate in the same source match.
        copy_dictionary("han-broken", "broken");
        query.dictionary_sources = {"broken.ifo"};
        found = search();
        assert(found.dictionary_status && found.dictionary_match->word == "中" && found.dictionary_partial);
        assert(found.dictionary_fields[0].text == "short match");
        query.dictionary_candidates = {{{0, 2}, "中文", false}}; query.dictionary_sources = {"broken.ifo", "first.ifo"};
        found = search(); assert(found.dictionary_status.status == dict::Status::NotFound && found.dictionary_partial);
        query.relative_path = "broken.ifo"; query.dictionary_sources.clear();
        found = search(); assert(found.dictionary_status.status == dict::Status::InvalidFormat && !found.error.empty());
        query.relative_path.clear(); query.dictionary_sources = {"first.ifo", "second.ifo", "third.ifo"};
        query.dictionary_candidates = {{{0, 7}, "missing", false}};
        found = search();
        assert(found.dictionary_status.status == dict::Status::NotFound && !found.dictionary_partial && found.error.empty());
        query.dictionary_sources.clear(); query.dictionary_candidates = {{{0, 1}, "中", false}};
        // Temporary source I/O and individual entry errors are not remembered.
        copy_dictionary("query-short", "temporary");
        fs::rename(dictionaries / "temporary.dict.dz", dictionaries / "held.dict.dz");
        query.relative_path = "temporary.ifo";
        ContentJob io_error(query); assert(io_error.start()); finish(io_error);
        assert(io_error.result.dictionary_status.status == dict::Status::IoError);
        assert(query.dictionary_session->preparation_failures.empty());
        fs::rename(dictionaries / "held.dict.dz", dictionaries / "temporary.dict.dz");
        ContentJob io_retry(query); assert(io_retry.start()); finish(io_retry); assert(io_retry.result.dictionary_status);
        copy_dictionary("invalid-field", "entry"); query.relative_path = "entry.ifo";
        query.dictionary_candidates = {{{0, 6}, "sample", false}};
        ContentJob entry_error(query); assert(entry_error.start()); finish(entry_error);
        assert(entry_error.result.dictionary_status.status == dict::Status::InvalidFormat);
        assert(query.dictionary_session->preparation_failures.empty());
        copy_dictionary("query-short", "limited"); query.relative_path = "limited.ifo";
        const auto limited_metadata = dictionaries / "limited.ifo";
        std::ifstream metadata_input(limited_metadata);
        std::string metadata{std::istreambuf_iterator<char>(metadata_input), std::istreambuf_iterator<char>()};
        metadata_input.close();
        const auto size_at = metadata.find("idxfilesize="); assert(size_at != std::string::npos);
        metadata.replace(size_at, metadata.find('\n', size_at) - size_at, "idxfilesize=67108865");
        { std::ofstream output(limited_metadata); output << metadata; }
        ContentJob resource_error(query); assert(resource_error.start()); finish(resource_error);
        assert(resource_error.result.dictionary_status.status == dict::Status::ResourceLimit);
        assert(query.dictionary_session->preparation_failures.empty());
        copy_dictionary("query-short", "limited"); query.dictionary_candidates = {{{0, 1}, "中", false}};
        ContentJob resource_retry(query); assert(resource_retry.start()); finish(resource_retry);
        assert(resource_retry.result.dictionary_status);
        copy_dictionary("offset64", "unsupported"); query.relative_path = "unsupported.ifo";
        ContentJob unsupported(query); assert(unsupported.start()); finish(unsupported);
        assert(unsupported.result.dictionary_status.status == dict::Status::Unsupported);
        assert(query.dictionary_session->preparation_failures.size() == 1);
        // Explicit preparation is not answered from the remembered failure.
        query.kind = ContentJob::Kind::DictionaryPrepare;
        ContentJob prepare_unsupported(query); assert(prepare_unsupported.start()); finish(prepare_unsupported);
        assert(prepare_unsupported.result.dictionary_status.status == dict::Status::Unsupported);
        assert(query.dictionary_session->preparation_failures.size() == 1);
        query.kind = ContentJob::Kind::DictionaryLookup;
        query.relative_path = "short.ifo";
        ContentJob cancelled_query(query); assert(!cancelled_query.pause()); assert(cancelled_query.start());
        while (!cancelled_query.pause() && !cancelled_query.done()) std::this_thread::yield();
        assert(cancelled_query.pause()); cancelled_query.cancel(); finish(cancelled_query);
        assert(cancelled_query.result.dictionary_status.status == dict::Status::Cancelled);
        assert(cancelled_query.result.dictionary_fields.empty() && !cancelled_query.result.dictionary_match);
        ContentJob after_cancel(query); assert(after_cancel.start()); finish(after_cancel);
        assert(after_cancel.result.dictionary_status);
        internal_free = 31 * 1024;
        ContentJob no_memory(query); assert(!no_memory.start());
        internal_free = 40 * 1024;
        ContentJob::Request layout;
        layout.kind = ContentJob::Kind::DictionaryPage;
        std::string rich = "<b>";
        for (unsigned i = 0; i < 80; ++i) rich += "word" + std::to_string(i) + " ";
        rich += "</b><p>last</p>";
        layout.dictionary_definition = std::make_shared<const std::vector<dict::Field>>(std::vector<dict::Field>{{'h', rich}});
        layout.layout.left = 10; layout.layout.top = 10; layout.layout.width = 200;
        layout.layout.height = 84; layout.layout.font_px = 28; layout.layout.line_height = 42;
        ContentJob page_one(layout); assert(page_one.start()); finish(page_one);
        assert(page_one.result.dictionary_status && !page_one.result.dictionary_complete);
        assert(!page_one.result.dictionary_page.glyphs.empty() && page_one.result.dictionary_page.glyphs.front().bold);
        layout.dictionary_cursor = page_one.result.dictionary_next;
        ContentJob page_two(layout); assert(page_two.start()); finish(page_two);
        assert(page_two.result.dictionary_status && page_two.result.dictionary_page.glyphs.front().bold);
        assert(page_two.result.dictionary_next.byte > page_one.result.dictionary_next.byte);
        layout.dictionary_cursor = {}; layout.dictionary_seek_pages = 1;
        ContentJob replay(layout); assert(replay.start()); finish(replay);
        assert(replay.result.dictionary_status && replay.result.dictionary_next.byte == page_two.result.dictionary_next.byte);
        assert(replay.result.dictionary_current.byte == page_one.result.dictionary_next.byte);
        const auto& expected = page_two.result.dictionary_page.glyphs;
        const auto& actual = replay.result.dictionary_page.glyphs;
        assert(actual.size() == expected.size());
        for (std::size_t i = 0; i < actual.size(); ++i)
            assert(actual[i].scalar == expected[i].scalar && actual[i].x == expected[i].x &&
                   actual[i].baseline == expected[i].baseline && actual[i].bold == expected[i].bold);
        // Reflow starts at the same decoded text, while the prefix remains reachable.
        const auto anchor = page_two.result.dictionary_current.scalars;
        assert(anchor > 0);
        layout.dictionary_reflow = true; layout.dictionary_seek_pages = 0;
        layout.dictionary_layout_anchor = anchor; layout.layout.height = 336;
        ContentJob expanded(layout); assert(expanded.start()); finish(expanded);
        assert(expanded.result.dictionary_status && expanded.result.dictionary_current.scalars == anchor);
        assert(expanded.result.dictionary_page.glyphs.front().scalar == expected.front().scalar);
        assert(expanded.result.dictionary_page.glyphs.front().bold && expanded.result.dictionary_page_number == 1);
        layout.dictionary_reflow = false;
        ContentJob prefix(layout); assert(prefix.start()); finish(prefix);
        assert(prefix.result.dictionary_status && prefix.result.dictionary_current.scalars == 0);
        assert(prefix.result.dictionary_next.scalars == anchor);
        layout.dictionary_cursor = expanded.result.dictionary_current;
        ContentJob forward(layout); assert(forward.start()); finish(forward);
        assert(forward.result.dictionary_status && forward.result.dictionary_next.scalars == expanded.result.dictionary_next.scalars);
        layout.dictionary_cursor = {}; layout.dictionary_reflow = true; layout.layout.height = 84;
        ContentJob collapsed(layout); assert(collapsed.start()); finish(collapsed);
        assert(collapsed.result.dictionary_status && collapsed.result.dictionary_current.scalars == anchor);
        assert(collapsed.result.dictionary_next.scalars == page_two.result.dictionary_next.scalars);
        layout.dictionary_reflow = false; layout.dictionary_layout_anchor = 0;
        layout.dictionary_seek_pages = 131072;
        ContentJob beyond(layout); assert(beyond.start()); finish(beyond);
        assert(beyond.result.dictionary_status.status == dict::Status::NotFound);
        layout.dictionary_seek_pages = 0;
        psram_free = 1100 * 1024;
        ContentJob no_layout_memory(layout); assert(no_layout_memory.start()); finish(no_layout_memory);
        assert(no_layout_memory.result.dictionary_status.status == dict::Status::ResourceLimit);
        psram_free = 4 * 1024 * 1024;
        ContentJob paused_page(layout); assert(!paused_page.pause()); assert(paused_page.start());
        paused_page.cancel(); finish(paused_page);
        assert(paused_page.result.dictionary_page.glyphs.empty());
        layout.font = (root / "missing.ttf").string();
        ContentJob no_font(layout); assert(no_font.start()); finish(no_font);
        assert(no_font.result.dictionary_status.status == dict::Status::IoError);
        layout.font.clear();
        layout.dictionary_definition = std::make_shared<const std::vector<dict::Field>>(std::vector<dict::Field>{{'m', "\xf4\x8f\xbf\xbf"}});
        ContentJob missing_glyph(layout); assert(missing_glyph.start()); finish(missing_glyph);
        assert(missing_glyph.result.dictionary_status && missing_glyph.result.dictionary_missing_glyphs);
        fs::remove_all(root);
    }
    assert(creations == 1 && allocations == 1);
    PP_DIAGNOSTIC(
        const auto measured = diagnostics::snapshot().metrics[static_cast<unsigned>(diagnostics::Kind::Content)];
        assert(measured.active == 0);
        assert(measured.outcomes[static_cast<unsigned>(diagnostics::Outcome::Failure)] > 0);
        assert(measured.outcomes[static_cast<unsigned>(diagnostics::Outcome::Cancelled)] > 0);
        assert(measured.outcomes[static_cast<unsigned>(diagnostics::Outcome::Success)] > 0);
    )
    {
        std::lock_guard<std::mutex> lock(queue->mutex);
        queue->shutdown = true; queue->changed.notify_one();
    }
    worker.join();
    vQueueDelete(queue);
}
