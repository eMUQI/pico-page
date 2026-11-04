#pragma once

#include <cstdint>
#include "cst836u.h"
#include "esp_err.h"

namespace pico_page::platform {

class SleepIo {
public:

    esp_err_t suspend(cst836u_handle_t touch, bool host_off);

    esp_err_t resume();
    bool pending() const { return ioe_pending_ || touch_ != nullptr; }

private:
    std::uint16_t output_ = 0, config_ = 0;
    bool ioe_pending_ = false;
    cst836u_handle_t touch_ = nullptr;
};

}
