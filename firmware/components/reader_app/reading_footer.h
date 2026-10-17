#pragma once

#include "pico_page/provision.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <string>
#include <string_view>

namespace pico_page::reading_footer {
enum class Position { Pages, Chapters, ChapterName, Percent, Hidden };
enum class Device { Battery, Clock, Hidden };
inline constexpr int device_left = 540;

/** Formats a rounded whole-book percentage; uncounted pages use whole percent. */
inline std::string percent(double fraction, bool pages_counted = true) {
    if (!std::isfinite(fraction)) return "—";
    if (!pages_counted)
        return std::to_string(static_cast<int>(std::round(std::clamp(fraction, 0.0, 1.0) * 100))) + "%";
    const auto hundredths = static_cast<int>(std::round(std::clamp(fraction, 0.0, 1.0) * 10000));
    char value[16];
    std::snprintf(value, sizeof(value), "%d.%02d%%", hundredths / 100, hundredths % 100);
    return value;
}

/** Bounds copied UTF-8 display text before glyph decoding and measurement. Navigation labels
 * have already been trimmed by the document parser; empty labels share the ordinal fallback. */
inline std::string chapter_label(std::string_view label, std::size_t index) {
    if (label.empty()) return "第 " + std::to_string(index + 1) + " 章";
    constexpr std::size_t limit = 128;
    if (label.size() <= limit) return std::string(label);
    auto end = limit;
    while (end && (static_cast<unsigned char>(label[end]) & 0xc0) == 0x80) --end;
    return std::string(label.substr(0, end)) + "…";
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
