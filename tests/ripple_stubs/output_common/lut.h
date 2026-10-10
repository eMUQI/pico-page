#pragma once
#include "epd_highlevel.h"
#define EPD_1PPB_LUT_1K_SIZE 1024
void epd_build_1ppB_lut_1k(std::uint8_t*, const EpdWaveformPhases*, int);
