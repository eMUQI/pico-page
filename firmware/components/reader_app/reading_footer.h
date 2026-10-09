#pragma once

#include "pico_page/provision.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <string>

namespace pico_page::reading_footer {
enum class Position { Pages, Chapters, Percent };
enum class Device { Battery, Clock };
inline constexpr int device_left = 540;

/** Formats a whole-book fraction as a rounded percentage with two decimal places. */
inline std::string percent(double fraction) {
    if (!std::isfinite(fraction)) return "—";
    const auto hundredths = static_cast<int>(std::round(std::clamp(fraction, 0.0, 1.0) * 10000));
    char value[16];
    std::snprintf(value, sizeof(value), "%d.%02d%%", hundredths / 100, hundredths % 100);
    return value;
}

/** Formats a calibrated Unix clock in the selected UTC offset; an unset clock stays unknown. */
inline std::string clock(std::time_t utc, int offset_minutes) {
    if (utc < provision::clock_floor) return "—";
    // Shift only the bounded time of day, avoiding epoch overflow and global TZ changes.
    const auto minute = ((static_cast<std::int64_t>(utc) % 86400 / 60 + offset_minutes) % 1440 + 1440) % 1440;
    char value[8];
    std::snprintf(value, sizeof(value), "%02d:%02d", static_cast<int>(minute / 60), static_cast<int>(minute % 60));
    return value;
}
} // namespace pico_page::reading_footer
