#include "pico_page/changed_pixels_waveform.h"
#include "pico_page/ripple_schedule.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <limits>
#include <vector>

using namespace pico_page::platform;
namespace {
unsigned get(const std::uint8_t* lut, std::size_t phase, unsigned to, unsigned from) {
    return (lut[phase * 64 + to * 4 + from / 4] >> (6 - 2 * (from % 4))) & 3;
}
void put(std::uint8_t* lut, std::size_t phase, unsigned to, unsigned from, unsigned value) {
    lut[phase * 64 + to * 4 + from / 4] |= value << (6 - 2 * (from % 4));
}
void exercise(std::size_t phases) {
    std::vector<std::uint8_t> source(phases * 64);
    for (unsigned to = 0; to < 16; ++to) {
        for (unsigned from = 0; from < 16; ++from) {
            // Includes all-hold, full-length, internal holds, erase-only and action 3 sequences.
            const unsigned pattern = (to * 16 + from) % 5;
            for (std::size_t f = 0; f < phases; ++f) {
                unsigned value = 0;
                if (pattern == 1) value = f % 2 + 1;
                if (pattern == 2 && f >= phases / 3) value = f < phases / 2 ? 2 : (f % 4);
                if (pattern == 3 && f >= phases / 2) value = 2;
                if (pattern == 4 && f == phases - 1) value = 1;
                put(source.data(), f, to, from, value);
            }
        }
    }
    const auto unchanged = source;
    std::vector<std::uint8_t> output(source.size() + 1, 0xA5);
    assert(build_changed_pixels_waveform(source.data(), source.size(), phases,
                                         output.data(), source.size()));
    assert(output.back() == 0xA5 && source == unchanged);
    for (unsigned to = 0; to < 16; ++to)
        for (unsigned from = 0; from < 16; ++from)
            for (std::size_t phase = 0; phase < phases; ++phase)
                assert(get(output.data(), phase, to, from) ==
                       (to == from ? 0 : get(source.data(), phase, to, from)));
}
}
int main() {
    for (int row = 0; row < 684; ++row) {
        int expected = 0;
        for (int tick = 0; tick < ripple_ticks; ++tick) {
            const int phase = ripple_phase(tick, row, 684);
            if (phase >= 0) assert(phase == expected++);
        }
        assert(expected == 37);
    }
    // Mirroring changes only band order, including uneven band sizes.
    for (int rows : {1, 7, 16, 17, 684}) {
        for (int row = 0; row < rows; ++row) {
            int expected = 0;
            for (int tick = 0; tick < ripple_ticks; ++tick) {
                const int phase = ripple_phase(tick, row, rows, true);
                assert(phase == ripple_phase(tick, rows - 1 - row, rows));
                if (phase >= 0) assert(phase == expected++);
            }
            assert(expected == 37);
        }
    }
    assert(ripple_phase(0, 683, 684, true) == 0);
    assert(ripple_phase(0, 0, 684, true) == -1);
    assert(ripple_phase(51, 0, 684, true) == 36);
    assert(ripple_phase(0, -1, 684, true) == -1);
    assert(ripple_ticks == 52);
    assert(ripple_phase(0, 0, 684) == 0 && ripple_phase(0, 683, 684) == -1);
    assert(ripple_phase(51, 683, 684) == 36);
    assert(ripple_phase(-1, 0, 684) == -1 && ripple_phase(52, 0, 684) == -1);
    assert(ripple_phase(0, -1, 684) == -1 && ripple_phase(0, 684, 684) == -1);
    assert(ripple_phase(0, 0, 0) == -1);
    for (auto phases : {1u, 37u, 64u}) exercise(phases);
    std::array<std::uint8_t, 128> source{}, output{};
    output.fill(0xA5);
    const auto unchanged = output;
    for (auto phases : {std::size_t{0}, std::size_t{3}, std::numeric_limits<std::size_t>::max()})
        assert(!build_changed_pixels_waveform(source.data(), source.size(), phases, output.data(), output.size()));
    assert(!build_changed_pixels_waveform(source.data(), 63, 1, output.data(), output.size()));
    assert(!build_changed_pixels_waveform(source.data(), source.size(), 1, output.data(), 63));
    assert(!build_changed_pixels_waveform(nullptr, source.size(), 1, output.data(), output.size()));
    assert(!build_changed_pixels_waveform(source.data(), source.size(), 1, nullptr, output.size()));
    assert(output == unchanged);
    assert(!build_changed_pixels_waveform(source.data(), 64, 1, source.data(), 64));
    assert(!build_changed_pixels_waveform(source.data(), 64, 1, source.data() + 1, 64));
    assert(!build_changed_pixels_waveform(source.data() + 1, 64, 1, source.data(), 64));
    assert(std::all_of(source.begin(), source.end(), [](auto v) { return v == 0; }));
}
