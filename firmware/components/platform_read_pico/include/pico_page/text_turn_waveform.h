#pragma once

#include <cstddef>
#include <cstdint>

namespace pico_page::platform {

inline constexpr std::size_t waveform_phase_bytes = 16 * 4;
struct TextTurnOptions {
    bool left_align = false;
    bool hold_diagonal = false;
    bool remove_dark_erase = false;
};

/**
 * Transforms packed [phase][to][from / 4] actions, with from 0 in bits 7:6.
 * Options execute in declaration order; erase removal replaces the leading run
 * of action 2 with holds without shifting later actions. No allocation occurs.
 * Source and destination must be disjoint and each cover phases * 64 bytes.
 * Returns false without writing for null, overlapping, zero-phase or undersized
 * buffers. On success only phases * 64 destination bytes are written.
 */
bool build_text_turn_waveform(const std::uint8_t* source, std::size_t source_bytes,
                              std::size_t phases, TextTurnOptions options,
                              std::uint8_t* destination, std::size_t destination_bytes);

} // namespace pico_page::platform
