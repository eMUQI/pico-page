#include "pico_page/platform_display.h"
#include "display_network_gate.h"

namespace pico_page::platform {
DisplayNetworkGate presentation_gate;

bool try_begin_display() { return presentation_gate.try_display(); }
void finish_display() { presentation_gate.finish_display(); }
void cancel_display() { presentation_gate.cancel_display(); }
} // namespace pico_page::platform
