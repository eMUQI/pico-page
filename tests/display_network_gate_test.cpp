#include "pico_page/display_network_gate.h"
#include <cassert>

int main() {
    pico_page::platform::DisplayNetworkGate gate;
    assert(gate.try_network());
    assert(!gate.try_display());
    gate.finish_network();
    assert(!gate.try_network());
    assert(gate.try_display());
    assert(!gate.try_network());
    gate.finish_display();
    assert(gate.try_network());
    assert(!gate.try_display());
    gate.cancel_display();
    assert(!gate.try_network());
    gate.finish_network();
    assert(gate.try_network());
    gate.finish_network();
    assert(gate.try_display());
    assert(!gate.try_network());
    gate.finish_display();
    assert(gate.try_network());
    gate.finish_network();
}
