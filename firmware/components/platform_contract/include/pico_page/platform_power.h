#pragma once

#include <cstdint>
#include <string>

namespace pico_page::platform {

struct Battery {
    /** Percentage validity; a validated charging state does not depend on SOC validity. */
    bool valid = false;
    int percent = 0;
    int millivolts = 0;
    bool charging = false;  // Cleared when the charging status cannot be validated.
};

/** Returns the latest validated battery snapshot, refreshed by poll(). */
Battery battery();

/**
 * Enters sleep and returns after a power-key wake or an error.
 * The caller must finish display work, persist progress and stop network work first.
 * Touch and auxiliary keys do not wake this mode. Pending contacts are discarded,
 * including on aborted preparation; only power-key events cancel the handoff.
 * Contacts remain suppressed until release after wake. The wake input is consumed;
 * this function does not advance reading state. Periodic maintenance wakeups and
 * rejected sleep requests are handled without returning to the application.
 * entered_sleep becomes true only after actual sleep and stays true if wake cleanup fails.
 * An aborted preparation preserves a cancelling long press for the next poll.
 * USB storage must be stopped; connected diagnostic-console handles may need to reopen.
 * Incomplete peripheral recovery blocks sampling/display until restoration succeeds.
 */
bool sleep(std::string& error, bool& entered_sleep);

/** Stops display power and attempts storage release after all file consumers stop. Failure does not resume I/O. */
bool finish_power(std::string& error);
/** Requests hardware shutdown after a final-frame and finish_power attempt,
 * including release failure. On failure restores the ability to show the manual-off
 * frame; files stay closed and application work must not resume. Incomplete recovery
 * blocks sampling/display until restoration succeeds. */
bool power_off(std::string& error);

/** Attempts deferred clock persistence on the application task at an idle boundary.
 * May block for driver command retries; failures are logged without changing system time. */
void persist_clock();
/** Consumes the latest accepted SNTP timestamp, or returns zero when none is pending. */
std::int64_t take_clock_sync();

}
