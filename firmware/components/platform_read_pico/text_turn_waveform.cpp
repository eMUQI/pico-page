/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * Adapted from FreeInk SDK e0470_epaper_waveform.c at
 * 96de1be6ce08eb732909e6e8149af8f892b9a2c5. Pico Page adds independent
 * options and checked buffer bounds, and removes the temporary sequence array.
 */
#include "pico_page/text_turn_waveform.h"

#include <cstring>

namespace pico_page::platform {
bool build_text_turn_waveform(const std::uint8_t* source, std::size_t source_bytes,
                              std::size_t phases, TextTurnOptions options,
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
            if (options.hold_diagonal && to == from) continue;
            const auto cell = to * 4 + from / 4;
            const auto shift = 6 - 2 * (from % 4);
            const auto action = [&](std::size_t phase) {
                return (source[phase * waveform_phase_bytes + cell] >> shift) & 3u;
            };
            std::size_t first = 0;
            if (options.left_align)
                while (first < phases && action(first) == 0) ++first;
            bool erase_prefix = options.remove_dark_erase && to != 15;
            for (std::size_t phase = first; phase < phases; ++phase) {
                const auto value = action(phase);
                if (erase_prefix && value == 2) continue;
                erase_prefix = false;
                destination[(phase - first) * waveform_phase_bytes + cell] |=
                    static_cast<std::uint8_t>(value << shift);
            }
        }
    }
    return true;
}
} // namespace pico_page::platform
