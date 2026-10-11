#include "rtc_clock.h"
#include "pico_page/provision.h"
#include "esp_log.h"
#include "read_pico_pmu.h"

#include <atomic>
#include <time.h>

namespace pico_page::platform::rtc_clock {
namespace {
constexpr char tag[] = "pico_clock";
// TIME_SYNC accepts UTC seconds through 2099-12-31T23:59:59Z.
constexpr std::int64_t clock_ceiling = 4102444799LL;
std::atomic<bool> pending{false};

bool valid(std::int64_t seconds) {
    return seconds >= provision::clock_floor && seconds <= clock_ceiling;
}

bool read() {
    const auto result = read_pico_pmu_cmd(PMU_CMD_TIME_GET, nullptr, 0);
    const auto* snapshot = read_pico_pmu_get();
    return result == ESP_OK && snapshot->last_op_status == PMU_STATUS_OK &&
           snapshot->time_ok && snapshot->time_synced == 1 && valid(snapshot->unix_sec);
}
}

void restore() {
    // Preserve an already running system clock; PMU time is only a boot fallback.
    if (::time(nullptr) >= provision::clock_floor) return;
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

void synchronized(timeval* time) {
    if (time && valid(time->tv_sec)) pending.store(true);
}

void persist() {
    if (!pending.exchange(false)) return;
    const auto now = ::time(nullptr);
    if (!valid(now)) {
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
