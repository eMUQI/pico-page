#include "pico_page/power_key.h"
#include <cassert>

int main() {
    using Key = pico_page::platform::PowerKey;
    Key key;
    assert(key.accept(Key::Event::Down) == Key::Action::None);
    assert(key.accept(Key::Event::Short) == Key::Action::Sleep);
    assert(key.accept(Key::Event::Down) == Key::Action::None);
    key.cancel_sleep_press();
    assert(key.accept(Key::Event::Long) == Key::Action::Menu);
    assert(key.accept(Key::Event::Short) == Key::Action::None);
    assert(key.accept(Key::Event::Down) == Key::Action::None);
    key.cancel_sleep_press();
    assert(key.accept(Key::Event::Short) == Key::Action::None);
    assert(key.accept(Key::Event::Down) == Key::Action::None);
    assert(key.accept(Key::Event::Short) == Key::Action::Sleep);
    assert(key.accept(Key::Event::Short) == Key::Action::None);
    assert(key.accept(Key::Event::Down) == Key::Action::None);
    assert(key.accept(Key::Event::Long) == Key::Action::Menu);
    assert(key.accept(Key::Event::Long) == Key::Action::None);
    assert(key.accept(Key::Event::Short) == Key::Action::None);
    assert(key.accept(Key::Event::Down) == Key::Action::None);
    assert(key.accept(Key::Event::Short) == Key::Action::Sleep);
}
