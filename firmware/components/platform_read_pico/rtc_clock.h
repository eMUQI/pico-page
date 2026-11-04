#pragma once
#include <sys/time.h>

namespace pico_page::platform::rtc_clock {
void restore();
void synchronized(timeval* time);
void persist();
}
