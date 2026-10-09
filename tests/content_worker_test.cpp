#include "../firmware/components/reader_app/content_job.h"
#include "freertos/FreeRTOS.h"
#include "esp_heap_caps.h"
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>

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
    return caps == MALLOC_CAP_SPIRAM ? 4 * 1024 * 1024 : internal_free;
}
std::size_t heap_caps_get_largest_free_block(std::uint32_t caps) {
    return caps == MALLOC_CAP_SPIRAM ? 3 * 1024 * 1024 : internal_largest;
}
std::size_t heap_caps_get_minimum_free_size(std::uint32_t caps) { return heap_caps_get_free_size(caps); }

int main() {
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
    assert(creations == 1 && allocations == 1);
    {
        std::lock_guard<std::mutex> lock(queue->mutex);
        queue->shutdown = true; queue->changed.notify_one();
    }
    worker.join();
    vQueueDelete(queue);
}
