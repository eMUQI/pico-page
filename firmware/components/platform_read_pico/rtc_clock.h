#pragma once
#include <cstdint>
#include <sys/time.h>

namespace pico_page::platform::rtc_clock {
void restore();
void begin_sntp(bool defer_update);
bool has_sample();
bool apply_sample();
std::int64_t take_synchronized_at();
void persist();
}
