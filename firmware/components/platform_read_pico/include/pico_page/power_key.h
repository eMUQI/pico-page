#pragma once

namespace pico_page::platform {

/** PMU key events share one press; LONG consumes any subsequent SHORT for that press. */
class PowerKey {
public:
    enum class Event { Down, Short, Long };
    enum class Action { None, Sleep, Menu };
    Action accept(Event event) {
        if (event == Event::Down) { consumed_ = false; return Action::None; }
        if (consumed_) return Action::None;
        consumed_ = true;
        return event == Event::Long ? Action::Menu : Action::Sleep;
    }
private:
    bool consumed_ = false;
};

} // namespace pico_page::platform
