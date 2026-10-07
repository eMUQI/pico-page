#pragma once

#include <chrono>
#include <cstdint>
#ifdef ESP_PLATFORM
#include "esp_log.h"
#endif

namespace pico_page {
/** Inclusive stage duration; nested stages must not be added to their parent duration. */
struct StageTimer {
    const char* stage;
    std::int64_t min_us = 20'000;
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    ~StageTimer() {
#ifdef ESP_PLATFORM
        const auto us = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - start).count();
        if (us >= min_us)
            ESP_LOGI("reader_perf", "stage=%s us=%lld min_us=%lld", stage, static_cast<long long>(us), static_cast<long long>(min_us));
#endif
    }
};
} // namespace pico_page
