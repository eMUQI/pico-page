#pragma once

namespace pico_page::platform {
inline constexpr int ripple_bands = 16;
inline constexpr int ripple_phases = 37;
inline constexpr int ripple_ticks = ripple_phases + ripple_bands - 1;
/** Physical rows increase from right to left in inverted portrait orientation. */
constexpr int ripple_phase(int tick, int row, int rows) {
    if (rows <= 0 || row < 0 || row >= rows || tick < 0 || tick >= ripple_ticks) return -1;
    const int phase = tick - row * ripple_bands / rows;
    return phase >= 0 && phase < ripple_phases ? phase : -1;
}
} // namespace pico_page::platform
