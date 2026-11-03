#pragma once

#include <string>

namespace pico_page::platform {

/**
 * Initializes the selected platform instance on the calling application task.
 * Subsequent display/input calls must use that task. Storage mount failure is
 * nonfatal; inspect card_root(). Failed display setup must not enable unsafe output.
 * Allocation and self-test failures may terminate startup inside the provider.
 */
bool initialize(std::string& error);
/** True when initialization permits display transactions; does not validate a prior frame. */
bool panel_ready();

/** Network worker pairs these calls while holding the display/Flash lock.
 * Quiesces peripheral access that cannot safely overlap flash operations.
 */
void guard_flash_io(bool begin);

/**
 * Requests the platform's high-performance mode (true) or permits idle operation (false).
 * Call on the application task. present() restores the performance required by the display.
 * Platforms without dynamic performance control may leave their operating mode unchanged.
 */
void set_performance(bool high);

} // namespace pico_page::platform
