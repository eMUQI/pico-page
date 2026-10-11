#include "../firmware/components/reader_app/content_job.h"
#include "freertos/FreeRTOS.h"
#include "esp_heap_caps.h"
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <thread>
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
    assert(argc == 2);
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
    internal_free = 40 * 1024;
    {
        namespace fs = std::filesystem;
        char temporary[] = "/tmp/pico-cover-worker-XXXXXX";
        const char* directory = mkdtemp(temporary); assert(directory);
        const fs::path root(directory), cache = root / "PicoPage/cache/covers/sleep";
        ContentJob::Request cover;
        cover.kind = ContentJob::Kind::SleepCover;
        cover.root = root.string(); cover.path = (root / "book.epub").string();
        cover.cache_name = "covers/sleep"; cover.cache_header = "PPCOVER2\nprobe\n";
        cover.layout.width = 684; cover.layout.height = 1040; cover.cover_crop = true;
        fs::copy_file(argv[1], cover.path);
        for (const bool fragmented : {false, true}) {
            psram_free = fragmented ? 4 * 1024 * 1024 : 3 * 1024 * 1024 - 1;
            psram_largest = fragmented ? 2 * 1024 * 1024 - 1 : 3 * 1024 * 1024;
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
        ContentJob unwritable(cover); assert(unwritable.start()); finish(unwritable);
        assert(!unwritable.result.cache_finished && !fs::exists(cache));
        fs::remove(cache.string() + ".tmp");
        ContentJob recovered(cover); assert(recovered.start()); finish(recovered);
        assert(recovered.result.cache_finished && !recovered.result.image);
        assert(fs::file_size(cache) == cover.cache_header.size() + 684U * 1040U / 2);
        const auto decodes = pico_page::graphics::image_decode_count();
        psram_free = psram_largest = 512 * 1024;
        ContentJob cached(cover); assert(cached.start()); finish(cached);
        assert(cached.result.cache_finished && !cached.result.image);
        assert(pico_page::graphics::image_decode_count() == decodes);
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
        for (const auto* kind : {"missing", "oversized", "unsupported"}) {
            cover.path = (fs::path(argv[1]).parent_path() / (std::string(kind) + "-image-cover.epub")).string();
            cover.cache_name = std::string("covers/") + kind;
            ContentJob invalid(cover); assert(invalid.start()); finish(invalid);
            assert(invalid.result.cache_finished && !invalid.result.image);
            assert(fs::file_size(root / "PicoPage/cache" / cover.cache_name) == cover.cache_header.size());
        }
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
