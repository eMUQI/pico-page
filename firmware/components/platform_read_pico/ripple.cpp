/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * Adapted from kiikoread e0470_page_turn.c at
 * 8d6f1a6f2f5cbee5bba08df43ebb5001d2584d83. Pico Page restricts the
 * scheduler to full-screen inverted portrait, retains FULL scanning, and
 * uses checked allocation and the existing panel power/recovery transaction.
 */
#include "ripple.h"
#include "pico_page/platform.h"
#include "pico_page/ripple_schedule.h"
#include "e0470_epaper_waveform.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
extern "C" {
#include "output_common/lut.h"
}
#include <cstring>

namespace pico_page::platform {
namespace {
std::uint8_t* phase_data = nullptr;
const std::uint8_t* phase_luts[ripple_phases]{};
DRAM_ATTR std::int8_t line_phases[width];
const std::uint8_t hold_lut[64]{};
const EpdWaveformPhases single_phase{1, hold_lut, nullptr};
const EpdWaveformPhases* single_ranges[] = {&single_phase};
const EpdWaveformMode single_mode{MODE_GL16, 1, single_ranges};
const EpdWaveformMode* single_modes[] = {&single_mode};
static_assert(ripple_phases == E0470_GL16_FRAMES);
}
bool prepare_ripple() {
    if (phase_data) return true;
    const auto* source = e0470_waveform_phases(&E0470_WAVEFORM, MODE_GL16);
    if (!source || !source->luts || source->phases != ripple_phases || source->phase_times) return false;
    phase_data = static_cast<std::uint8_t*>(heap_caps_aligned_alloc(
        16, ripple_phases * EPD_1PPB_LUT_1K_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!phase_data) return false;
    for (int phase = 0; phase < ripple_phases; ++phase) {
        auto* lut = phase_data + phase * EPD_1PPB_LUT_1K_SIZE;
        epd_build_1ppB_lut_1k(lut, source, phase);
        phase_luts[phase] = lut;
    }
    return true;
}
void release_ripple() {
    heap_caps_free(phase_data);
    phase_data = nullptr;
    for (auto& lut : phase_luts) lut = nullptr;
}
EpdDrawError present_ripple(EpdiyHighlevelState& display, int temperature, bool reverse) {
    if (!phase_data || epd_width() != height || epd_height() != width ||
        epd_get_rotation() != EPD_ROT_INVERTED_PORTRAIT) return EPD_DRAW_NO_PHASES_AVAILABLE;
    const auto area = epd_full_screen();
    const EpdWaveform waveform{1, 1, single_modes, E0470_WAVEFORM.temp_intervals};
    epd_difference_image_cropped(display.front_fb, display.back_fb, area, display.difference_fb,
                                  display.dirty_lines, display.dirty_columns);
    std::memset(display.dirty_columns, 0xFF, height / 2);
    for (int tick = 0; tick < ripple_ticks; ++tick) {
        for (int row = 0; row < width; ++row) {
            line_phases[row] = static_cast<std::int8_t>(ripple_phase(tick, row, width, reverse));
            display.dirty_lines[row] = line_phases[row] >= 0;
        }
        epd_set_line_phase_luts(phase_luts, line_phases, ripple_phases);
        const auto result = epd_draw_base(area, display.difference_fb, area,
            static_cast<EpdDrawMode>(MODE_PACKING_1PPB_DIFFERENCE | MODE_GL16), temperature,
            display.dirty_lines, display.dirty_columns, &waveform);
        epd_set_line_phase_luts(nullptr, nullptr, 0);
        if (result != EPD_DRAW_SUCCESS) return result;
    }
    std::memcpy(display.back_fb, display.front_fb, framebuffer_bytes);
    return EPD_DRAW_SUCCESS;
}
} // namespace pico_page::platform
