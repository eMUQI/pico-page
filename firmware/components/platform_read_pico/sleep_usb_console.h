#pragma once

#include <cstdint>
#include "esp_err.h"

namespace pico_page::platform {

class SleepUsbConsole {
public:
    esp_err_t enter_light_sleep();
    void resume();
    bool entered_sleep() const { return entered_sleep_; }

private:
    std::uint32_t original_config_ = 0;
    std::int64_t disconnected_at_us_ = 0;
    bool pending_ = false, entered_sleep_ = false;
};

}
