#pragma once

#include "pico_page/core.h"
#include <string>

namespace pico_page::platform {

/** Configured card path, valid for the platform lifetime even when unmounted.
 * This does not grant filesystem access; card_root() determines mount availability. */
const char* card_mount_path();

/** Returns the root path for the currently valid mount, otherwise nullptr. */
const char* card_root();
/** Why card_root() is null after the start-up mount: absent, without a file system, or unreadable. */
CardIssue card_start_issue();
const std::string& card_error();

/** Closes and remounts the card without formatting. Close all files first. */
bool refresh_card(std::string& error);

/** Releases the application mount after all consumers close their files. */
bool release_card_for_usb(std::string& error);
/** Starts exclusive raw-card MSC access on the application task; failure requires restart. */
bool start_usb_storage(std::string& error);
/** Stops new USB I/O, waits for synchronous writes, and disconnects before restart. */
bool stop_usb_storage(std::string& error);

}
