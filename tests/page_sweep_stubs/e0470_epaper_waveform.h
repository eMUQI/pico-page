#pragma once
#include "epd_highlevel.h"
#define E0470_GL16_FRAMES 37
extern const EpdWaveform E0470_WAVEFORM;
#define E0470_GRAY8_GL16_FRAMES 30
extern const EpdWaveform E0470_GRAY8_WAVEFORM;
const EpdWaveformPhases* e0470_waveform_phases(const EpdWaveform*, int);
