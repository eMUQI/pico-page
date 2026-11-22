/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * Adapted from kiikoread e0470_page_turn.c at
 * 8d6f1a6f2f5cbee5bba08df43ebb5001d2584d83. Pico Page restricts the
 * scheduler to full-screen inverted portrait, retains FULL scanning, and
 * uses checked allocation and the existing panel power/recovery transaction.
 */
#include "page_sweep.h"
#include "display_geometry.h"
#include "pico_page/page_sweep_schedule.h"
#include "e0470_epaper_waveform.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
extern "C" {
#include "output_common/lut.h"
}
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>

namespace pico_page::platform {
namespace {
std::uint8_t* phase_data = nullptr;
const EpdWaveformPhases* source_phases = nullptr;
PageWaveform prepared_sweep = PageWaveform::Gray16;
DRAM_ATTR const std::uint8_t* phase_luts[sweep_phases]{};
DRAM_ATTR std::int8_t line_phases[width];
constexpr int diagonal_lut_slots = diagonal_max_delay + 1;
constexpr int touch_lut_slots = std::max(diagonal_lut_slots, ripple_bands);
constexpr int diagonal_blocks = height / diagonal_block_pixels;
static_assert(height % diagonal_block_pixels == 0 && diagonal_blocks >= diagonal_column_bands);

constexpr auto make_diagonal_spans(bool from_top) {
    std::array<EpdPhaseSpan, diagonal_column_bands> spans{};
    for (int i = 0; i < diagonal_column_bands; ++i) {
        const int delay = from_top ? i : diagonal_column_bands - 1 - i;
        const int end_block = ((i + 1) * diagonal_blocks + (from_top ? diagonal_column_bands - 1 : 0))
                              / diagonal_column_bands;
        spans[i] = {static_cast<std::uint16_t>(end_block * diagonal_block_pixels),
                    static_cast<std::uint8_t>(delay)};
    }
    return spans;
}

DRAM_ATTR const auto bottom_spans = make_diagonal_spans(false);
DRAM_ATTR const auto top_spans = make_diagonal_spans(true);
const std::uint8_t hold_lut[64]{};
const EpdWaveformPhases single_phase{1, hold_lut, nullptr};
const EpdWaveformPhases* single_ranges[] = {&single_phase};
const EpdWaveformMode single_mode{MODE_GL16, 1, single_ranges};
const EpdWaveformMode* single_modes[] = {&single_mode};
static_assert(sweep_phases == E0470_GRAY8_GL16_FRAMES);
}
bool prepare_page_sweep(PageWaveform waveform) {
    if (waveform != PageWaveform::Ripple && waveform != PageWaveform::Diagonal && waveform != PageWaveform::TouchPosition)
        return false;
    if (prepared_sweep == waveform) return true;
    const auto* source = e0470_waveform_phases(&E0470_GRAY8_WAVEFORM, MODE_GL16);
    if (!source || !source->luts || source->phases != sweep_phases || source->phase_times) return false;
    const bool precompute = waveform == PageWaveform::Ripple;
    const int slots = precompute ? sweep_phases : (waveform == PageWaveform::TouchPosition ? touch_lut_slots : diagonal_lut_slots);
    const auto caps = (precompute ? MALLOC_CAP_SPIRAM : MALLOC_CAP_INTERNAL) | MALLOC_CAP_8BIT;
    auto* buffer = static_cast<std::uint8_t*>(heap_caps_aligned_alloc(16, slots * EPD_1PPB_LUT_1K_SIZE, caps));
    if (!buffer) return false;
    release_page_sweep();
    phase_data = buffer;
    source_phases = source;
    prepared_sweep = waveform;
    if (precompute) {
        for (int phase = 0; phase < sweep_phases; ++phase) {
            auto* lut = phase_data + phase * EPD_1PPB_LUT_1K_SIZE;
            epd_build_1ppB_lut_1k(lut, source, phase);
            phase_luts[phase] = lut;
        }
    }
    return true;
}
void release_page_sweep() {
    heap_caps_free(phase_data);
    phase_data = nullptr;
    source_phases = nullptr;
    prepared_sweep = PageWaveform::Gray16;
    for (auto& lut : phase_luts) lut = nullptr;
}
EpdDrawError present_page_sweep(EpdiyHighlevelState& display, int temperature, bool reverse, PageTurnOrigin origin) {
    if (!phase_data || epd_width() != height || epd_height() != width ||
        epd_get_rotation() != EPD_ROT_INVERTED_PORTRAIT) return EPD_DRAW_NO_PHASES_AVAILABLE;
    const auto area = epd_full_screen();
    const EpdWaveform waveform{1, 1, single_modes, E0470_GRAY8_WAVEFORM.temp_intervals};
    epd_difference_image_cropped(display.front_fb, display.back_fb, area, display.difference_fb,
                                  display.dirty_lines, display.dirty_columns);
    std::memset(display.dirty_columns, 0xFF, height / 2);
    const bool touch_position = prepared_sweep == PageWaveform::TouchPosition;
    const bool diagonal = prepared_sweep == PageWaveform::Diagonal || (touch_position && origin != PageTurnOrigin::Middle);
    const auto& spans = touch_position && origin == PageTurnOrigin::Top ? top_spans : bottom_spans;
    const int max_delay = diagonal ? diagonal_max_delay : ripple_bands - 1;
    const int slots = max_delay + 1;
    const int column_delay = diagonal ? diagonal_column_bands - 1 : 0;
    const int ticks = sweep_phases + max_delay;
    for (int tick = 0; tick < ticks; ++tick) {
        int first_phase = 0;
        int phase_count = sweep_phases;
        if (prepared_sweep != PageWaveform::Ripple) {
            const int last_phase = std::min(tick, sweep_phases - 1);
            first_phase = std::max(0, tick - max_delay);
            phase_count = last_phase - first_phase + 1;
            if (tick < sweep_phases) {
                auto* lut = phase_data + tick % slots * EPD_1PPB_LUT_1K_SIZE;
                epd_build_1ppB_lut_1k(lut, source_phases, tick);
            }
            for (int phase = first_phase; phase <= last_phase; ++phase) {
                phase_luts[phase - first_phase] =
                    phase_data + phase % slots * EPD_1PPB_LUT_1K_SIZE;
            }
        }
        for (int row = 0; row < width; ++row) {
            const int source_row = reverse ? width - 1 - row : row;
            const int row_phase = (diagonal ? tick - source_row * diagonal_row_bands / width
                                            : ripple_phase(tick, row, width, reverse)) - first_phase;
            line_phases[row] = static_cast<std::int8_t>(row_phase);
            display.dirty_lines[row] = row_phase >= 0 && row_phase < phase_count + column_delay;
        }
        epd_set_line_phase_luts(phase_luts, line_phases, phase_count,
                               diagonal ? spans.data() : nullptr, diagonal ? spans.size() : 0);
        const auto result = epd_draw_base(area, display.difference_fb, area,
            static_cast<EpdDrawMode>(MODE_PACKING_1PPB_DIFFERENCE | MODE_GL16), temperature,
            display.dirty_lines, display.dirty_columns, &waveform);
        epd_set_line_phase_luts(nullptr, nullptr, 0, nullptr, 0);
        if (result != EPD_DRAW_SUCCESS) return result;
    }
    std::memcpy(display.back_fb, display.front_fb, framebuffer_bytes);
    return EPD_DRAW_SUCCESS;
}
}
