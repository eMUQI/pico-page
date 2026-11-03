#include "host_allocations.h"
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdlib>
#include <limits>
#include <mutex>
#include <new>

namespace {
// Fixed test storage avoids recursive allocation in the accounting path.
struct Entry { void* pointer = nullptr; std::size_t bytes = 0; };
constexpr std::size_t slots = 1U << 18;
Entry entries[slots];
void* const removed = reinterpret_cast<void*>(1);
std::mutex mutex;
std::atomic<bool> active{false};
host_allocations::Snapshot counters;
std::size_t bucket(void* pointer) { return (reinterpret_cast<std::uintptr_t>(pointer) >> 4) % slots; }
void allocated(void* pointer, std::size_t bytes) {
    if (!pointer || !active.load(std::memory_order_relaxed)) return;
    std::lock_guard<std::mutex> lock(mutex);
    auto index = bucket(pointer);
    std::size_t probes = 0;
    while (entries[index].pointer && entries[index].pointer != removed) {
        index = (index + 1) % slots;
        assert(++probes < slots);
    }
    entries[index] = {pointer, bytes};
    ++counters.calls; counters.bytes += bytes; counters.live += bytes;
    counters.peak = std::max(counters.peak, counters.live);
}
void freed(void* pointer) {
    if (!pointer || !active.load(std::memory_order_relaxed)) return;
    std::lock_guard<std::mutex> lock(mutex);
    auto index = bucket(pointer);
    for (std::size_t probes = 0; probes < slots && entries[index].pointer; ++probes, index = (index + 1) % slots) {
        if (entries[index].pointer != pointer) continue;
        counters.live -= entries[index].bytes; entries[index].pointer = removed; return;
    }
}
}
namespace host_allocations {
void begin() {
    assert(!active.load());
    for (auto& entry : entries) entry = {};
    counters = {}; active.store(true);
}
Snapshot end() {
    active.store(false);
    const auto result = counters;
    // Accounting must not keep leaked payloads reachable to LSan after a capture.
    for (auto& entry : entries) entry = {};
    return result;
}
}
extern "C" {
void* __real_malloc(std::size_t);
void* __real_calloc(std::size_t, std::size_t);
void* __real_realloc(void*, std::size_t);
void __real_free(void*);
int __real_posix_memalign(void**, std::size_t, std::size_t);
void* __wrap_malloc(std::size_t size) { auto* p = __real_malloc(size); allocated(p, size); return p; }
void* __wrap_calloc(std::size_t count, std::size_t size) {
    auto* p = __real_calloc(count, size); if (p) allocated(p, count * size); return p;
}
void __wrap_free(void* p) { freed(p); __real_free(p); }
void* __wrap_realloc(void* p, std::size_t size) {
    // Failed realloc retains the old allocation; remove accounting only after success.
    const auto address = reinterpret_cast<std::uintptr_t>(p);
    auto* next = __real_realloc(p, size);
    if (next || !size) { freed(reinterpret_cast<void*>(address)); allocated(next, size); }
    return next;
}
int __wrap_posix_memalign(void** p, std::size_t alignment, std::size_t size) {
    const auto status = __real_posix_memalign(p, alignment, size);
    if (!status) allocated(*p, size);
    return status;
}
}
void* operator new(std::size_t size) {
    if (auto* p = __wrap_malloc(std::max<std::size_t>(size, 1))) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* p) noexcept { __wrap_free(p); }
void operator delete[](void* p) noexcept { __wrap_free(p); }
void operator delete(void* p, std::size_t) noexcept { __wrap_free(p); }
void operator delete[](void* p, std::size_t) noexcept { __wrap_free(p); }
void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    try { return ::operator new(size); } catch (...) { return nullptr; }
}
void* operator new[](std::size_t size, const std::nothrow_t& value) noexcept { return ::operator new(size, value); }
void operator delete(void* p, const std::nothrow_t&) noexcept { __wrap_free(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { __wrap_free(p); }
void* operator new(std::size_t size, std::align_val_t alignment) {
    void* p = nullptr;
    if (__wrap_posix_memalign(&p, static_cast<std::size_t>(alignment), std::max<std::size_t>(size, 1))) throw std::bad_alloc();
    return p;
}
void* operator new[](std::size_t size, std::align_val_t alignment) { return ::operator new(size, alignment); }
void operator delete(void* p, std::align_val_t) noexcept { __wrap_free(p); }
void operator delete[](void* p, std::align_val_t) noexcept { __wrap_free(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { __wrap_free(p); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { __wrap_free(p); }
