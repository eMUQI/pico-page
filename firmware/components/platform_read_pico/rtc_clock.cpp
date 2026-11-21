#include "rtc_clock.h"
#include "pico_page/clock_time.h"
#include "esp_log.h"
#include "read_pico_pmu.h"

#include <atomic>
#include <time.h>
#ifdef ESP_PLATFORM
#include "esp_sntp.h"
#endif

namespace pico_page::platform::rtc_clock {
namespace {
constexpr char tag[] = "pico_clock";
std::atomic<bool> pending{false};
std::atomic<std::uint32_t> synchronized_at{0};
std::atomic<bool> defer_sample{false};
std::atomic<std::uint32_t> sample_seconds{0};
std::atomic<std::uint32_t> sample_microseconds{0};

bool apply(const timeval& time) {
    if (settimeofday(&time, nullptr) != 0) return false;
    synchronized_at.store(static_cast<std::uint32_t>(time.tv_sec));
    pending.store(true);
    return true;
}

bool read() {
    const auto result = read_pico_pmu_cmd(PMU_CMD_TIME_GET, nullptr, 0);
    const auto* snapshot = read_pico_pmu_get();
    return result == ESP_OK && snapshot->last_op_status == PMU_STATUS_OK &&
           snapshot->time_ok && snapshot->time_synced == 1 && clock_time::valid(snapshot->unix_sec);
}
}

void restore() {
    if (::time(nullptr) >= clock_time::floor) return;
    if (!read()) {
        ESP_LOGW(tag, "RTC unavailable or unset; waiting for network time");
        return;
    }
    timeval time{static_cast<time_t>(read_pico_pmu_get()->unix_sec), 0};
    if (settimeofday(&time, nullptr) != 0) {
        ESP_LOGW(tag, "Cannot restore system clock from RTC");
        return;
    }
    ESP_LOGI(tag, "System clock restored from RTC");
}

void begin_sntp(bool defer_update) {
    sample_seconds.store(0);
    defer_sample.store(defer_update);
}

bool has_sample() { return sample_seconds.load() != 0; }

bool apply_sample() {
    if (!defer_sample.load()) return false;
    const auto seconds = sample_seconds.load();
    if (!seconds || !apply({static_cast<time_t>(seconds), static_cast<suseconds_t>(sample_microseconds.load())}))
        return false;
    defer_sample.store(false);
    return true;
}

std::int64_t take_synchronized_at() { return synchronized_at.exchange(0); }

static void receive_sntp_sample(const timeval* time) {
    if (!time || !clock_time::valid(time->tv_sec) || time->tv_usec < 0 || time->tv_usec >= 1'000'000) return;
    if (defer_sample.load()) {
        if (has_sample()) return;
        sample_microseconds.store(static_cast<std::uint32_t>(time->tv_usec));
    } else if (!apply(*time)) {
        return;
    }
    sample_seconds.store(static_cast<std::uint32_t>(time->tv_sec));
}

void persist() {
    if (!pending.exchange(false)) return;
    const auto now = ::time(nullptr);
    if (!clock_time::valid(now)) {
        ESP_LOGW(tag, "System clock outside RTC range; RTC write skipped");
        return;
    }
    const auto seconds = static_cast<std::uint32_t>(now);
    const std::uint8_t payload[] = {
        static_cast<std::uint8_t>(seconds), static_cast<std::uint8_t>(seconds >> 8),
        static_cast<std::uint8_t>(seconds >> 16), static_cast<std::uint8_t>(seconds >> 24)};
    const auto result = read_pico_pmu_cmd(PMU_CMD_TIME_SYNC, payload, sizeof(payload));
    if (result != ESP_OK || read_pico_pmu_get()->last_op_status != PMU_STATUS_OK || !read()) {
        ESP_LOGW(tag, "RTC write not confirmed; system clock remains available");
        return;
    }
    const auto difference = static_cast<std::int64_t>(read_pico_pmu_get()->unix_sec) - seconds;
    if (difference < 0 || difference >= 5) {
        ESP_LOGW(tag, "RTC readback differs from submitted time");
        return;
    }
    ESP_LOGI(tag, "Network time saved to RTC");
}
}

// The ESP-IDF 6.1 weak hook normally applies the sample and notifies listeners.
extern "C" void sntp_sync_time(timeval* time) {
    pico_page::platform::rtc_clock::receive_sntp_sample(time);
}
