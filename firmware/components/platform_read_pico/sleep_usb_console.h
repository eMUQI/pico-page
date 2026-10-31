#pragma once

#include <cstdint>
#include "esp_err.h"

namespace pico_page::platform {

/** Holds the USB console disconnected across one logical sleep and its PMU wakeups.
 * The application task calls resume() after releasing the touch lock, including on
 * failure. Preparation cancelled before enter_light_sleep() leaves USB unchanged.
 */
class SleepUsbConsole {
public:
    esp_err_t enter_light_sleep();
    /** Restores the saved PHY state; a successful sleep requires at least 100 ms detached. */
    void resume();
    bool entered_sleep() const { return entered_sleep_; }

private:
    std::uint32_t original_config_ = 0;
    std::int64_t disconnected_at_us_ = 0;
    bool pending_ = false, entered_sleep_ = false;
};

} // namespace pico_page::platform
