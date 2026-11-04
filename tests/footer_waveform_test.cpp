#include "e0470_epaper_waveform.h"
#include "pico_page/changed_pixels_waveform.h"
#include "../boards/read_pico/patches/epdiy-unchanged-pixels.h"

#include <array>
#include <cassert>
#include <cstdio>
#include <vector>

namespace {
void verify(const EpdWaveform& waveform, int white_tick) {
    const auto* phases = e0470_waveform_phases(&waveform, 5);
    assert(phases && phases->luts);
    const std::size_t bytes = phases->phases * 64;
    const std::vector<std::uint8_t> original(phases->luts, phases->luts + bytes);
    std::vector<std::uint8_t> overlay(bytes);
    assert(pico_page::platform::build_changed_pixels_waveform(phases->luts, bytes,
        phases->phases, overlay.data(), overlay.size()));
    std::array<std::uint8_t, 1216> difference;
    std::array<std::uint8_t, 32> mask;
    for (int pair = 0; pair < 256; ++pair) {
        const int to = pair >> 4, from = pair & 15;
        difference.fill(static_cast<std::uint8_t>(pair));
        assert(epd_build_unchanged_suffix_mask(difference.data(), mask.data(), 1128, 1216) == 32);
        for (int phase = 0; phase < phases->phases; ++phase) {
            const int action = e0470_phase_action(phases, phase, to, from);
            if (pair == 0xFF)
                assert(action == (phase == white_tick ? 2 : 0));
            for (int x = 1088; x < 1216; ++x) {
                const int allowed = (mask[(x - 1088) / 4] >> (2 * (x % 4))) & 3;
                const int expected = x < 1128 || from != to || pair == 0xFF ? action : 0;
                assert((action & allowed) == expected);
            }
            const int overlay_action = (overlay[phase * 64 + to * 4 + from / 4] >> (6 - 2 * (from % 4))) & 3;
            assert(overlay_action == (to == from && to != 15 ? 0 : action));
        }
    }
    assert(std::vector<std::uint8_t>(phases->luts, phases->luts + bytes) == original);
    std::printf("GL16 phases=%d white_tick=%d: all 256 transitions preserved as required\n",
                phases->phases, white_tick);
}
}

int main() {
    e0470_waveform_init();
    verify(E0470_WAVEFORM, 33);
    verify(E0470_FULL_WAVEFORM, 45);
    verify(E0470_GRAY8_WAVEFORM, -1);
}
