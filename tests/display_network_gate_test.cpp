#include "display_network_gate.h"
#include "pico_page/platform_display.h"
#include <cassert>

int main() {
    using namespace pico_page::platform;
    auto& gate = presentation_gate;
    assert(gate.try_network());
    assert(!try_begin_display());
    gate.finish_network();
    assert(!gate.try_network());
    assert(try_begin_display());
    assert(!gate.try_network());
    finish_display();
    assert(gate.try_network());
    assert(!try_begin_display());
    cancel_display();
    assert(!gate.try_network());
    gate.finish_network();
    assert(gate.try_network());
    gate.finish_network();
    assert(try_begin_display());
    assert(!gate.try_network());
    finish_display();
    assert(gate.try_network());
    gate.finish_network();
}
