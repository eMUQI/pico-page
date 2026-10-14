#pragma once

#include "platform.h"

namespace pico_page::platform {

/** Retains only power intents while a cancelled sleep restores the application. */
class SleepInput {
public:
    void begin() { *this = {}; active_ = true; }
    bool active() const { return active_; }
    /** Restoration and radio teardown must both finish before a retained power intent runs. */
    bool ready(bool page_ready, bool network_idle) const { return active_ && page_ready && network_idle; }
    void clear() { *this = {}; }
    /** Contacts and queue resets cannot cancel sleep or replay into the restored page. */
    static bool ignores(EventKind kind) {
        return kind == EventKind::Touch || kind == EventKind::K1 ||
               kind == EventKind::K2 || kind == EventKind::K3 || kind == EventKind::InputReset;
    }
    void accept(const Event& event) {
        if (!active_) return;
        if (event.kind == EventKind::PowerLong) { menu_ = true; power_ = false; return; }
        if (event.kind == EventKind::Power && !menu_) power_ = !power_;
    }
    bool menu() const { return menu_; }
    bool power() const { return power_; }
private:
    bool active_ = false, menu_ = false, power_ = false;
};
static_assert(sizeof(SleepInput) <= 3, "Sleep cancellation retains only power flags");

} // namespace pico_page::platform
