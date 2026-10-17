/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * Adapted from FreeInk SDK e0470_epaper_waveform.c at
 * 96de1be6ce08eb732909e6e8149af8f892b9a2c5. Pico Page adds
 * checked buffer bounds and retains only unchanged-pixel suppression.
 */
#include "pico_page/changed_pixels_waveform.h"

#include <cstring>

namespace pico_page::platform {
bool build_changed_pixels_waveform(const std::uint8_t* source, std::size_t source_bytes,
                              std::size_t phases,
                              std::uint8_t* destination, std::size_t destination_bytes) {
    if (!source || !destination || !phases || phases > source_bytes / waveform_phase_bytes ||
        phases > destination_bytes / waveform_phase_bytes) return false;
    const auto bytes = phases * waveform_phase_bytes;
    const auto src = reinterpret_cast<std::uintptr_t>(source);
    const auto dst = reinterpret_cast<std::uintptr_t>(destination);
    if ((src <= dst ? dst - src : src - dst) < bytes) return false;
    std::memset(destination, 0, bytes);
    for (unsigned to = 0; to < 16; ++to) {
        for (unsigned from = 0; from < 16; ++from) {
            const auto cell = to * 4 + from / 4;
            const auto shift = 6 - 2 * (from % 4);
            const auto action = [&](std::size_t phase) {
                return (source[phase * waveform_phase_bytes + cell] >> shift) & 3u;
            };
            if (to == from) continue;
            for (std::size_t phase = 0; phase < phases; ++phase) {
                destination[phase * waveform_phase_bytes + cell] |=
                    static_cast<std::uint8_t>(action(phase) << shift);
            }
        }
    }
    return true;
}
} // namespace pico_page::platform
