#pragma once

// Host probes count work without changing target timing, storage or allocation budgets.
#ifdef ESP_PLATFORM
#define PP_HOST_WORK(...)
#else
#define PP_HOST_WORK(...) __VA_ARGS__
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>

namespace pico_page::host_work {
enum Counter : unsigned { ZipNames, ZipComparisons, SpineComparisons, InflatedBytes, XmlBytes,
    SourceBytes, PageIndexVisits, ReclaimVisits, Count };
inline constexpr const char* stages[] = {"total", "zip_init", "zip_index_build", "epub_opf",
    "epub_spine_validate", "epub_toc"};
inline constexpr unsigned stage_count = sizeof(stages) / sizeof(stages[0]);
inline std::atomic<bool> enabled{false};
// Set only by the probe while the application owner is idle; substitutes host memory pressure.
inline bool memory_pressure = false;
inline std::array<std::array<std::atomic<std::uint64_t>, Count>, stage_count> counters{};
inline std::array<std::atomic<std::uint64_t>, stage_count> durations_us{};
inline thread_local unsigned current_stage = 0;
inline void add(Counter counter, std::uint64_t count = 1) {
    if (!enabled.load(std::memory_order_relaxed)) return;
    counters[0][counter].fetch_add(count, std::memory_order_relaxed);
    if (current_stage) counters[current_stage][counter].fetch_add(count, std::memory_order_relaxed);
}
/** Begin/end captures only after all measured workers have joined. */
inline void reset() {
    for (auto& stage : counters) for (auto& counter : stage) counter.store(0);
    for (auto& duration : durations_us) duration.store(0);
}
/** Times existing stage scopes. Times are inclusive; work counts belong to the innermost named stage. */
struct Stage {
    unsigned previous = current_stage, index = 0;
    std::chrono::steady_clock::time_point started;
    explicit Stage(const char* name) {
        if (!enabled.load(std::memory_order_relaxed)) return;
        for (unsigned i = 1; i < stage_count; ++i) if (std::strcmp(name, stages[i]) == 0) { index = i; break; }
        if (index) { current_stage = index; started = std::chrono::steady_clock::now(); }
    }
    ~Stage() {
        if (!index) return;
        durations_us[index].fetch_add(std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - started).count(), std::memory_order_relaxed);
        current_stage = previous;
    }
};
} // namespace pico_page::host_work
#endif
