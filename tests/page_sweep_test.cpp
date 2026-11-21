#include "../firmware/components/platform_read_pico/page_sweep.h"
#include "pico_page/page_sweep_schedule.h"
#include "display_geometry.h"
#include "e0470_epaper_waveform.h"
#include "esp_heap_caps.h"
#include <array>
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <vector>

using namespace pico_page::platform;
using pico_page::PageTurnOrigin;
namespace {
std::array<std::uint8_t, E0470_GRAY8_GL16_FRAMES * 64> source{};
EpdWaveformPhases source_phases{E0470_GRAY8_GL16_FRAMES, source.data(), nullptr};
const EpdWaveformTempInterval interval{0, 50};
bool allocation_fails = false, source_missing = false, reverse = false;
PageWaveform waveform = PageWaveform::Ripple;
PageTurnOrigin origin = PageTurnOrigin::Middle;
int allocations = 0, frees = 0, builds = 0, draws = 0, fail_at = -1, rotation = EPD_ROT_INVERTED_PORTRAIT;
const std::uint8_t* const* borrowed = nullptr;
const std::int8_t* rows = nullptr;
unsigned count = 0;
const EpdPhaseSpan* spans = nullptr;
unsigned span_count = 0;
}
extern const EpdWaveform E0470_WAVEFORM{0, 0, nullptr, nullptr};
extern const EpdWaveform E0470_GRAY8_WAVEFORM{0, 1, nullptr, &interval};
const EpdWaveformPhases* e0470_waveform_phases(const EpdWaveform* waveform, int mode) {
    assert(waveform == &E0470_GRAY8_WAVEFORM && mode == MODE_GL16);
    return source_missing ? nullptr : &source_phases;
}
void* heap_caps_aligned_alloc(std::size_t alignment, std::size_t bytes, unsigned caps) {
    assert(alignment == 16);
    if (waveform == PageWaveform::Ripple) {
        assert(bytes == E0470_GRAY8_GL16_FRAMES * 1024 && caps == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    } else {
        const int slots = waveform == PageWaveform::TouchPosition ? 16 : 13;
        assert(bytes == static_cast<std::size_t>(slots * 1024) && caps == (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    }
    if (allocation_fails) return nullptr;
    ++allocations;
    return std::aligned_alloc(alignment, bytes);
}
void heap_caps_free(void* p) { assert(!borrowed); if (p) ++frees; std::free(p); }
extern "C" void epd_build_1ppB_lut_1k(std::uint8_t* lut, const EpdWaveformPhases* source, int phase) {
    assert(!borrowed && source == &source_phases && phase >= 0 && phase < source->phases);
    ++builds;
    std::memset(lut, phase, 1024);
}
int epd_width() { return height; }
int epd_height() { return width; }
int epd_get_rotation() { return rotation; }
EpdRect epd_full_screen() { return {0, 0, height, width}; }
void epd_difference_image_cropped(const std::uint8_t*, const std::uint8_t*, EpdRect, std::uint8_t*, bool*, std::uint8_t*) {}
void epd_set_line_phase_luts(const std::uint8_t* const* p, const std::int8_t* r, unsigned n,
                            const EpdPhaseSpan* draw_spans, unsigned draw_span_count) {
    borrowed = p; rows = r; count = n; spans = draw_spans; span_count = draw_span_count;
}
EpdDrawError epd_draw_base(EpdRect, const std::uint8_t*, EpdRect, EpdDrawMode mode, int temperature,
                          const bool* dirty, const std::uint8_t*, const EpdWaveform* drive_waveform) {
    assert(borrowed && rows && count > 0 && temperature == 24);
    assert(mode == (MODE_PACKING_1PPB_DIFFERENCE | MODE_GL16));
    assert(drive_waveform->num_temp_ranges == 1 && drive_waveform->temp_intervals == &interval);
    assert(drive_waveform->mode_data[0]->range_data[0]->phases == 1);
    const bool touch = waveform == PageWaveform::TouchPosition;
    const bool diagonal = waveform == PageWaveform::Diagonal || (touch && origin != PageTurnOrigin::Middle);
    if (diagonal) {
        assert(spans && span_count == 9 && count <= 13);
        int start_column = 0;
        for (unsigned i = 0; i < span_count; ++i) {
            const auto span = spans[i];
            assert(span.end_column > start_column && span.end_column <= height && span.end_column % 16 == 0);
            for (int column = start_column; column < span.end_column; column += 16) {
                const int distance = touch && origin == PageTurnOrigin::Top ? column : height - 1 - column;
                assert(span.delay == (distance / 16) * 9 / (height / 16));
            }
            start_column = span.end_column;
        }
        assert(start_column == height);
        for (int row = 0; row < width; ++row) {
            const int row_delay = (reverse ? width - 1 - row : row) * 5 / width;
            bool active = false;
            for (unsigned i = 0; i < span_count; ++i) {
                const int expected = draws - row_delay - spans[i].delay;
                const int phase_index = rows[row] - spans[i].delay;
                if (expected >= 0 && expected < E0470_GRAY8_GL16_FRAMES) {
                    assert(phase_index >= 0 && static_cast<unsigned>(phase_index) < count);
                    assert(borrowed[phase_index][0] == expected);
                    active = true;
                } else {
                    assert(phase_index < 0 || static_cast<unsigned>(phase_index) >= count);
                }
            }
            assert(dirty[row] == active);
        }
    } else {
        assert(!spans && span_count == 0);
        if (touch) assert(count <= 16);
        else assert(count == E0470_GRAY8_GL16_FRAMES);
        for (int row = 0; row < width; ++row) {
            const int distance = reverse ? width - 1 - row : row;
            const int expected = draws - distance * 16 / width;
            const bool active = expected >= 0 && expected < E0470_GRAY8_GL16_FRAMES;
            assert(dirty[row] == active);
            assert((rows[row] >= 0 && static_cast<unsigned>(rows[row]) < count) == active);
            if (active) assert(borrowed[rows[row]][0] == expected);
        }
    }
    return draws++ == fail_at ? EPD_DRAW_EMPTY_LINE_QUEUE : EPD_DRAW_SUCCESS;
}
int main() {
    source_missing = true; assert(!prepare_page_sweep(PageWaveform::Ripple)); source_missing = false;
    source_phases.phases = E0470_GL16_FRAMES;
    assert(!prepare_page_sweep(PageWaveform::Ripple));
    source_phases.phases = E0470_GRAY8_GL16_FRAMES;
    allocation_fails = true; assert(!prepare_page_sweep(PageWaveform::Ripple)); allocation_fails = false;
    assert(prepare_page_sweep(PageWaveform::Ripple) && allocations == 1 && builds == E0470_GRAY8_GL16_FRAMES);
    allocation_fails = true;
    for (auto unavailable : {PageWaveform::Diagonal, PageWaveform::TouchPosition}) {
        waveform = unavailable;
        assert(!prepare_page_sweep(unavailable) && frees == 0);
    }
    waveform = PageWaveform::Ripple; allocation_fails = false;
    std::vector<std::uint8_t> front(framebuffer_bytes, 0x73), back(framebuffer_bytes, 0x15);
    std::vector<std::uint8_t> difference(width * height), columns(height / 2);
    std::array<bool, width> dirty{};
    EpdiyHighlevelState display{front.data(), back.data(), difference.data(), dirty.data(), columns.data()};
    rotation = 0; assert(present_page_sweep(display, 24, false, origin) == EPD_DRAW_NO_PHASES_AVAILABLE);
    rotation = EPD_ROT_INVERTED_PORTRAIT;
    const auto verify_turn = [&](int ticks) {
        for (bool direction : {false, true}) {
            reverse = direction;
            std::fill(back.begin(), back.end(), 0x15);
            for (fail_at = 0; fail_at < ticks; ++fail_at) {
                draws = 0;
                assert(present_page_sweep(display, 24, reverse, origin) == EPD_DRAW_EMPTY_LINE_QUEUE);
                assert(!borrowed && !rows && count == 0 && !spans && span_count == 0 && draws == fail_at + 1);
                for (auto value : back) assert(value == 0x15);
            }
            fail_at = -1; draws = 0;
            const int before_builds = builds;
            assert(present_page_sweep(display, 24, reverse, origin) == EPD_DRAW_SUCCESS && draws == ticks);
            assert(!borrowed && !spans && back == front);
            assert(builds - before_builds == (waveform == PageWaveform::Ripple ? 0 : E0470_GRAY8_GL16_FRAMES));
        }
    };
    origin = PageTurnOrigin::Top;
    verify_turn(45);
    waveform = PageWaveform::Diagonal;
    assert(prepare_page_sweep(waveform) && allocations == 2);
    verify_turn(42);
    waveform = PageWaveform::TouchPosition;
    assert(prepare_page_sweep(waveform) && prepare_page_sweep(waveform) && allocations == 3);
    for (auto touch_origin : {PageTurnOrigin::Bottom, PageTurnOrigin::Middle, PageTurnOrigin::Top}) {
        origin = touch_origin;
        verify_turn(origin == PageTurnOrigin::Middle ? 45 : 42);
    }
    assert(allocations == 3 && frees == 2);
    waveform = PageWaveform::Ripple;
    assert(prepare_page_sweep(waveform) && allocations == 4 && frees == 3);
    release_page_sweep(); assert(frees == 4);
    assert(present_page_sweep(display, 24, false, origin) == EPD_DRAW_NO_PHASES_AVAILABLE);
}
