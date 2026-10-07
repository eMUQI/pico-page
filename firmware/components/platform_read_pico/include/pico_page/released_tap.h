#pragma once

#include "platform.h"
#include <optional>

namespace pico_page::platform {

/**
 * Contact time from which a touch is a long press. The value is a starting point for
 * the capacitive panel and has to be confirmed on the device.
 */
inline constexpr std::int64_t kLongPressUs = 600 * 1000;

/** True when the contact that began with begin and was released by end lasted a long press. */
inline bool long_press(const Event& begin, const Event& end) {
    return begin.kind == EventKind::Touch && end.time_us - begin.time_us >= kLongPressUs;
}

/** Retains the initial target until release or an explicit long press, unless the frame changes. */
class ReleasedTap {
public:
    /** Returns a held touch when its threshold has elapsed without a presentation change. */
    std::optional<Event> due(std::int64_t now_us, std::uint64_t frame) const {
        if (held_ && held_->kind == EventKind::Touch && frame == frame_ &&
            now_us - held_->time_us >= kLongPressUs)
            return held_;
        return std::nullopt;
    }
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
