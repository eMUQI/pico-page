#pragma once

namespace pico_page::platform {

/** PMU key events share one press; LONG consumes any subsequent SHORT for that press. */
class PowerKey {
public:
    enum class Event { Down, Short, Long };
    enum class Action { None, Sleep, Menu };
    Action accept(Event event) {
        if (event == Event::Down) { consumed_ = false; cancel_short_ = false; return Action::None; }
        if (consumed_) return Action::None;
        consumed_ = true;
        if (event == Event::Short && cancel_short_) return Action::None;
        return event == Event::Long ? Action::Menu : Action::Sleep;
    }
    /** A press that aborted sleep may still open the menu, but its release must not sleep again. */
    void cancel_sleep_press() { cancel_short_ = true; }
private:
    bool consumed_ = false, cancel_short_ = false;
};

} // namespace pico_page::platform
