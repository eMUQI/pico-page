#pragma once

#include "pico_page/app.h"
#include <cstdint>
#include <string>

namespace pico_page::console {

/** Configures nonblocking stdin on the application task; disabled builds are inert. */
bool initialize(std::string &error);
/** Stops the diagnostic driver before USB PHY handoff; rejects an active import. */
bool shutdown(std::string &error);

/**
 * Processes at most one bounded command on the application task. card_root is
 * null for an unavailable card. All application input sources must use the same
 * increasing input_sequence; a physical End/Cancel retains its Begin sequence.
 * Set actions_allowed=false while another task owns storage or the app is busy.
 * input_allowed permits key, tap, hold, swipe, press and release while other actions
 * are blocked. Pending frames and imports still reject those commands.
 * Keep input_allowed false during sleep or a physical contact.
 * Returns true when a line outside the observation commands was dispatched,
 * including rejected commands. The caller uses this result to reset the idle timer.
 */
bool poll(ReaderApp &app, const char *card_root, std::uint64_t &input_sequence,
          bool actions_allowed = true, bool input_allowed = false);

/** Records an actual platform result before ReaderApp::complete_frame mutates its canvas. */
void frame_completed(const Frame &frame, bool success);

/** True while a staged import owns card writes; the main loop must defer new input and sleep. */
bool transfer_active();

}
