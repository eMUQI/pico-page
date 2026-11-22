#pragma once

#include <cstdint>

namespace pico_page::clock_time {
inline constexpr std::int64_t floor = 1704067200;
inline constexpr std::int64_t ceiling = 4102444799;
inline constexpr std::int64_t utc8_offset_seconds = 8 * 3600;
inline constexpr std::int64_t unix_seconds_per_day = 86400;

inline constexpr bool valid(std::int64_t utc_seconds) {
    return utc_seconds >= floor && utc_seconds <= ceiling;
}
}
