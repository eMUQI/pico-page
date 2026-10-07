#pragma once

#include "platform.h"
#include <optional>

namespace pico_page::platform {

/**
 * Contact time from which a released touch is a long press. The value is a starting point for
 * the capacitive panel and has to be confirmed on the device.
 */
inline constexpr std::int64_t kLongPressUs = 600 * 1000;

/** True when the contact that began with begin and was released by end lasted a long press. */
inline bool long_press(const Event& begin, const Event& end) {
    return begin.kind == EventKind::Touch && end.time_us - begin.time_us >= kLongPressUs;
}

/** Dispatch the initial target after release only if no intervening frame changed the UI. */
class ReleasedTap {
public:
    std::optional<Event> accept(const Event& event, std::uint64_t frame) {
        if (event.kind == EventKind::InputReset) {
            held_.reset();
            return std::nullopt;
        }
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
