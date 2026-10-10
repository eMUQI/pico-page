#pragma once
#include "epd_highlevel.h"

namespace pico_page::platform {
/** Application display owner only; prepare/release between synchronous draws. */
bool prepare_ripple();
void release_ripple();
/** Full-frame inverted-portrait ripple; failure leaves back_fb unchanged. */
EpdDrawError present_ripple(EpdiyHighlevelState& display, int temperature);
}
