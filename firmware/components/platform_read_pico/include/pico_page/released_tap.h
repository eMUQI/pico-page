#pragma once

#include "platform.h"
#include <optional>

namespace pico_page::platform {

/** Dispatch the initial target after release only if no intervening frame changed the UI. */
class ReleasedTap {
public:
    std::optional<Event> accept(const Event& event, std::uint64_t frame) {
        if (event.phase == EventPhase::Begin) {
            if (!held_) { held_ = event; frame_ = frame; }
            return std::nullopt;
        }
        if (!held_ || held_->sequence != event.sequence) return std::nullopt;
        const auto tap = held_;
        held_.reset();
        return event.phase == EventPhase::End && frame == frame_ ? tap : std::nullopt;
    }
private:
    std::optional<Event> held_;
    std::uint64_t frame_ = 0;
};

} // namespace pico_page::platform
