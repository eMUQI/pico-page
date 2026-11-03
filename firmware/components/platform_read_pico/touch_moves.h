#pragma once

#include "pico_page/platform_input.h"
#include <optional>

namespace pico_page::platform {

/** One queued notification retains the latest move. Callers serialize access with the event queue. */
class TouchMoves {
public:
    bool pending_for(std::uint64_t sequence) const { return latest_ && latest_->sequence == sequence; }
    bool publish(const Event& event) {
        if (latest_) {
            if (latest_->sequence == event.sequence) latest_ = event;
            return false;
        }
        latest_ = event;
        return true;
    }
    void receive(Event& event) {
        if (event.phase == EventPhase::Move && latest_ && latest_->sequence == event.sequence) {
            event = *latest_;
            latest_.reset();
        }
    }
    void clear() { latest_.reset(); }
private:
    std::optional<Event> latest_;
};

} // namespace pico_page::platform
