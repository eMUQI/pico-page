#pragma once
#include <sys/time.h>

namespace pico_page::platform::rtc_clock {
/** Restores an unset system clock from the PMU. Application task only, before networking. */
void restore();
/** SNTP callback: requests a deferred write without accessing the PMU. */
void synchronized(timeval* time);
/** Attempts one pending write and readback. Application task only, at an idle boundary. */
void persist();
}
