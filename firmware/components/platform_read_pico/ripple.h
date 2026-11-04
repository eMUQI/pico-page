#pragma once
#include "epd_highlevel.h"

namespace pico_page::platform {
bool prepare_ripple();
void release_ripple();
EpdDrawError present_ripple(EpdiyHighlevelState& display, int temperature, bool reverse = false);
}
