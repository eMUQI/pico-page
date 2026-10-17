#include "../firmware/components/platform_read_pico/ripple.h"
#include "pico_page/ripple_schedule.h"
#include "pico_page/platform.h"
#include <array>
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <vector>

using namespace pico_page::platform;
namespace {
std::array<std::uint8_t, 37 * 64> source{};
EpdWaveformPhases source_phases{37, source.data(), nullptr};
const EpdWaveformTempInterval interval{0, 50};
bool allocation_fails = false, source_missing = false, reverse = false;
int allocations = 0, frees = 0, draws = 0, fail_at = -1, rotation = EPD_ROT_INVERTED_PORTRAIT;
const std::uint8_t* const* borrowed = nullptr;
const std::int8_t* rows = nullptr;
unsigned count = 0;
}
extern const EpdWaveform E0470_WAVEFORM{0, 1, nullptr, &interval};
const EpdWaveformPhases* e0470_waveform_phases(const EpdWaveform*, int) { return source_missing ? nullptr : &source_phases; }
void* heap_caps_aligned_alloc(std::size_t alignment, std::size_t bytes, unsigned caps) {
    assert(alignment == 16 && bytes == 37 * 1024 && caps == 3);
    if (allocation_fails) return nullptr;
    ++allocations;
    return std::aligned_alloc(alignment, bytes);
}
void heap_caps_free(void* p) { assert(!borrowed); if (p) ++frees; std::free(p); }
extern "C" void epd_build_1ppB_lut_1k(std::uint8_t* lut, const EpdWaveformPhases*, int phase) {
    std::memset(lut, phase, 1024);
}
int epd_width() { return height; }
int epd_height() { return width; }
int epd_get_rotation() { return rotation; }
EpdRect epd_full_screen() { return {0, 0, height, width}; }
void epd_difference_image_cropped(const std::uint8_t*, const std::uint8_t*, EpdRect, std::uint8_t*, bool*, std::uint8_t*) {}
void epd_set_line_phase_luts(const std::uint8_t* const* p, const std::int8_t* r, unsigned n) {
    borrowed = p; rows = r; count = n;
}
EpdDrawError epd_draw_base(EpdRect, const std::uint8_t*, EpdRect, EpdDrawMode mode, int temperature,
                          const bool* dirty, const std::uint8_t*, const EpdWaveform* waveform) {
    assert(borrowed && rows && count == 37 && temperature == 24);
    assert(mode == (MODE_PACKING_1PPB_DIFFERENCE | MODE_GL16));
    assert(waveform->mode_data[0]->range_data[0]->phases == 1);
    for (int row = 0; row < width; ++row) {
        assert(rows[row] == ripple_phase(draws, row, width, reverse));
        assert(dirty[row] == (rows[row] >= 0));
        if (rows[row] >= 0) assert(borrowed[rows[row]][0] == rows[row]);
    }
    return draws++ == fail_at ? EPD_DRAW_EMPTY_LINE_QUEUE : EPD_DRAW_SUCCESS;
}
int main() {
    source_missing = true; assert(!prepare_ripple()); source_missing = false;
    source_phases.phases = 38; assert(!prepare_ripple()); source_phases.phases = 37;
    allocation_fails = true; assert(!prepare_ripple()); allocation_fails = false;
    assert(prepare_ripple() && prepare_ripple() && allocations == 1);
    std::vector<std::uint8_t> front(framebuffer_bytes, 0x73), back(framebuffer_bytes, 0x15);
    std::vector<std::uint8_t> difference(width * height), columns(height / 2);
    std::array<bool, width> dirty{};
    EpdiyHighlevelState display{front.data(), back.data(), difference.data(), dirty.data(), columns.data()};
    rotation = 0; assert(present_ripple(display, 24) == EPD_DRAW_NO_PHASES_AVAILABLE); rotation = EPD_ROT_INVERTED_PORTRAIT;
    for (bool direction : {false, true}) {
        reverse = direction;
        std::fill(back.begin(), back.end(), 0x15);
        for (fail_at = 0; fail_at < 52; ++fail_at) {
            draws = 0;
            assert(present_ripple(display, 24, reverse) == EPD_DRAW_EMPTY_LINE_QUEUE);
            assert(!borrowed && !rows && count == 0 && draws == fail_at + 1);
            for (auto value : back) assert(value == 0x15);
        }
        fail_at = -1; draws = 0;
        assert(present_ripple(display, 24, reverse) == EPD_DRAW_SUCCESS && draws == 52);
        assert(!borrowed && back == front);
    }
    release_ripple(); assert(frees == 1);
    assert(present_ripple(display, 24) == EPD_DRAW_NO_PHASES_AVAILABLE);
}
