#pragma once
#include "epd_highlevel.h"
#include "pico_page/platform_display.h"

namespace pico_page::platform {
bool prepare_page_sweep(PageWaveform waveform);
void release_page_sweep();
EpdDrawError present_page_sweep(EpdiyHighlevelState& display, int temperature, bool reverse, PageTurnOrigin origin);
}
