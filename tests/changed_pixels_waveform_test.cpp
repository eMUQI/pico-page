#include "pico_page/changed_pixels_waveform.h"
#include "pico_page/ripple_schedule.h"
#include "../firmware/patches/epdiy-unchanged-pixels.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <limits>
#include <vector>

using namespace pico_page::platform;
namespace {
void hold_suffix(const std::uint8_t* difference, std::uint8_t* drive, int first, int width) {
    std::array<std::uint8_t, 1216 / 4 + 1> mask;
    mask.fill(0xA5);
    const int bytes = epd_build_unchanged_suffix_mask(difference, mask.data(), first, width);
    assert(bytes >= 0 && bytes <= 1216 / 4 && bytes % 16 == 0);
    assert(std::all_of(mask.begin() + bytes, mask.end(), [](auto value) { return value == 0xA5; }));
    if (!bytes) return;
    const int offset = first / 64 * 16;
    assert(offset % 16 == 0 && offset + bytes == width / 4);
    for (int i = 0; i < bytes; ++i) drive[offset + i] &= mask[i];
}
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
            const unsigned pattern = to == 15 && from == 15 ? 1 : (to * 16 + from) % 5;
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
                       (to == from && to != 15 ? 0 : get(source.data(), phase, to, from)));
}
}
int main() {
    // Exhaust all grayscale pairs in every output bit lane and at the footer boundary.
    constexpr int native_width = 1216, footer_column = 1128;
    std::array<std::uint8_t, native_width> difference{};
    std::array<std::uint8_t, native_width / 4 + 1> drive{};
    for (int pair = 0; pair < 256; ++pair) {
        difference.fill(static_cast<std::uint8_t>(pair));
        for (int first : {-1, 0, 1, 63, 64, 65, footer_column - 1, footer_column,
                          footer_column + 1, native_width - 1, native_width, native_width + 1}) {
            drive.fill(0xFF);
            hold_suffix(difference.data(), drive.data(), first, native_width);
            for (int x = 0; x < native_width; ++x) {
                const bool hold = first >= 0 && x >= first && pair != 0xFF && (pair & 15) == (pair >> 4);
                assert(((drive[x / 4] >> (2 * (x % 4))) & 3) == (hold ? 0 : 3));
            }
            assert(drive.back() == 0xFF);
        }
    }
    // Mixed equal and changed neighbors retain their original phase drive actions.
    for (int x = 0; x < native_width; ++x) difference[x] = x % 2 ? 0x77 : 0x37;
    drive.fill(0xE4);
    hold_suffix(difference.data(), drive.data(), footer_column, native_width);
    for (int x = 0; x < native_width; ++x) {
        const auto expected = x >= footer_column && x % 2 ? 0 : x % 4;
        assert(((drive[x / 4] >> (2 * (x % 4))) & 3) == expected);
    }
    assert(drive.back() == 0xE4);
    const auto unchanged_drive = drive;
    for (int invalid_width : {0, -64, native_width - 1, native_width - 4}) {
        hold_suffix(difference.data(), drive.data(), footer_column, invalid_width);
        assert(drive == unchanged_drive);
    }
    // One immutable mask applies to every phase, independently of its drive actions.
    std::array<std::uint8_t, 32 + 1> mask;
    mask.fill(0xA5);
    assert(epd_build_unchanged_suffix_mask(difference.data(), mask.data(), footer_column, native_width) == 32);
    assert(mask.back() == 0xA5);
    const auto saved_mask = mask;
    for (int phase = 0; phase < 37; ++phase) {
        drive.fill(static_cast<std::uint8_t>(phase * 7));
        const auto original = drive;
        for (int i = 0; i < 32; ++i) drive[272 + i] &= mask[i];
        for (int x = 0; x < native_width; ++x) {
            const unsigned expected = x >= footer_column && x % 2 ? 0 :
                (original[x / 4] >> (2 * (x % 4))) & 3u;
            assert(((drive[x / 4] >> (2 * (x % 4))) & 3u) == expected);
        }
        assert(drive.back() == original.back() && mask == saved_mask);
    }
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
