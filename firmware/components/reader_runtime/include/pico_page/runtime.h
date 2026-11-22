#pragma once

namespace pico_page::runtime {
/** Runs the firmware once on the ESP-IDF application task; owns application state. */
void run();
}
