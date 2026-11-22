#pragma once

namespace pico_page::platform {
inline constexpr int ripple_bands = 16;
inline constexpr int sweep_phases = 30;
inline constexpr int ripple_ticks = sweep_phases + ripple_bands - 1;
inline constexpr int diagonal_row_bands = 5;
inline constexpr int diagonal_column_bands = 9;
inline constexpr int diagonal_block_pixels = 16;
inline constexpr int diagonal_max_delay = diagonal_row_bands + diagonal_column_bands - 2;
inline constexpr int diagonal_ticks = sweep_phases + diagonal_max_delay;
/** Physical rows increase from right to left in inverted portrait; reverse mirrors the bands. */
constexpr int ripple_phase(int tick, int row, int rows, bool reverse = false) {
    if (rows <= 0 || row < 0 || row >= rows || tick < 0 || tick >= ripple_ticks) return -1;
    const int source_row = reverse ? rows - 1 - row : row;
    const int phase = tick - source_row * ripple_bands / rows;
    return phase >= 0 && phase < sweep_phases ? phase : -1;
}
}
