#pragma once

#include "released_tap.h"

namespace pico_page::platform {

/** Retains one cancelling gesture while the application restores its reading view. */
class SleepInput {
public:
    void begin() { *this = {}; active_ = true; }
    bool active() const { return active_; }
    void clear() { *this = {}; }
    void accept(const Event& event) {
        if (!active_) return;
        if (event.kind == EventKind::InputReset) { clear(); return; }
        if (event.kind == EventKind::PowerLong) { menu_ = true; return; }
        if (event.kind != EventKind::Touch && event.kind != EventKind::K1 &&
            event.kind != EventKind::K2 && event.kind != EventKind::K3) return;
        if (tap_) return;
        if (const auto tap = contact_.accept(event, 0)) {
            tap_ = tap;
            end_ = event;
        }
    }
    bool menu() const { return menu_; }
    bool held() const { return contact_.pending(); }
    const std::optional<Event>& tap() const { return tap_; }
    const Event& end() const { return end_; }
private:
    bool active_ = false, menu_ = false;
    ReleasedTap contact_;
    std::optional<Event> tap_;
    Event end_;
};
static_assert(sizeof(SleepInput) <= 256, "Sleep cancellation input must remain bounded");

} // namespace pico_page::platform
