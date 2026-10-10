#pragma once
#include <cstdint>
struct EpdRect { int x, y, width, height; };
struct EpdWaveformPhases { int phases; const std::uint8_t* luts; const int* phase_times; };
struct EpdWaveformMode { std::uint8_t type, temp_ranges; const EpdWaveformPhases** range_data; };
struct EpdWaveformTempInterval { int min, max; };
struct EpdWaveform { std::uint8_t num_modes, num_temp_ranges; const EpdWaveformMode** mode_data; const EpdWaveformTempInterval* temp_intervals; };
struct EpdiyHighlevelState {
    std::uint8_t *front_fb, *back_fb, *difference_fb;
    bool* dirty_lines;
    std::uint8_t* dirty_columns;
};
enum EpdDrawMode { MODE_GL16 = 5, MODE_PACKING_1PPB_DIFFERENCE = 0x100 };
enum EpdDrawError { EPD_DRAW_SUCCESS, EPD_DRAW_NO_PHASES_AVAILABLE, EPD_DRAW_EMPTY_LINE_QUEUE };
inline constexpr int EPD_ROT_INVERTED_PORTRAIT = 3;
int epd_width();
int epd_height();
int epd_get_rotation();
EpdRect epd_full_screen();
void epd_difference_image_cropped(const std::uint8_t*, const std::uint8_t*, EpdRect, std::uint8_t*, bool*, std::uint8_t*);
void epd_set_line_phase_luts(const std::uint8_t* const*, const std::int8_t*, unsigned);
EpdDrawError epd_draw_base(EpdRect, const std::uint8_t*, EpdRect, EpdDrawMode, int, const bool*, const std::uint8_t*, const EpdWaveform*);
