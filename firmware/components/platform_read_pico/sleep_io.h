#pragma once

#include <cstdint>
#include "cst836u.h"
#include "esp_err.h"

namespace pico_page::platform {

/** Parks board I/O after display and file work stop. Owned by the application task.
 * The caller serializes these methods and sampler pending() reads with the touch lock.
 * No board output setters may run while a saved register snapshot is pending.
 */
class SleepIo {
public:
    /** Saves runtime registers before any write; light sleep keeps CW_INT as input.
     * A failed write may have reached the device, so every failure requires resume(). */
    esp_err_t suspend(cst836u_handle_t touch, bool host_off);
    /** Restores registers before waking touch; partial failures retain pending state for retry. */
    esp_err_t resume();
    bool pending() const { return ioe_pending_ || touch_ != nullptr; }

private:
    std::uint16_t output_ = 0, config_ = 0;
    bool ioe_pending_ = false;
    cst836u_handle_t touch_ = nullptr;
};

} // namespace pico_page::platform
