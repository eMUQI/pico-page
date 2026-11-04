#pragma once

#include <cstddef>
#include <cstdint>

namespace pico_page::platform {

inline constexpr std::size_t waveform_phase_bytes = 16 * 4;
/**
 * Transforms packed [phase][to][from / 4] actions, with from 0 in bits 7:6.
 * Unchanged nonwhite transitions hold; white-to-white retains the source's
 * maintenance sequence. Changed transitions and the phase count remain intact.
 * No allocation occurs.
 * Source and destination must be disjoint and each cover phases * 64 bytes.
 * Returns false without writing for null, overlapping, zero-phase or undersized
 * buffers. On success only phases * 64 destination bytes are written.
 */
bool build_changed_pixels_waveform(const std::uint8_t* source, std::size_t source_bytes,
                              std::size_t phases,
                              std::uint8_t* destination, std::size_t destination_bytes);

}
