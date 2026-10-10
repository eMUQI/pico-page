#pragma once

#include "platform.h"
#include <cstdlib>
#include <optional>

namespace pico_page::platform {

/**
 * Contact time from which a touch is a long press. The value is a starting point for
 * the capacitive panel and has to be confirmed on the device.
 */
inline constexpr std::int64_t kLongPressUs = 600 * 1000;

/** Gesture distances are portrait pixels and duration is microseconds; confirm on the device. */
inline constexpr int kSwipeMinPx = 60;
inline constexpr std::int64_t kSwipeMaxUs = 700 * 1000;
inline constexpr int kLongPressSlopPx = 28;
/** Integer approximation of tan(60 degrees), limiting deviation from horizontal to 30 degrees. */
inline constexpr int kSwipeAngleRatio = 173;
inline constexpr int kSwipeAngleScale = 100;

enum class ReleasedGesture { Tap, Previous, Next, Ignore };

/** Classifies a released touch using endpoint displacement, without changing contact ownership. */
inline ReleasedGesture released_gesture(const Event& begin, const Event& end) {
    if (begin.kind != EventKind::Touch || end.kind != EventKind::Touch ||
        begin.phase != EventPhase::Begin || end.phase != EventPhase::End ||
        begin.sequence != end.sequence || end.time_us < begin.time_us) return ReleasedGesture::Ignore;
    const auto dx = static_cast<std::int64_t>(end.x) - begin.x;
    const auto dy = static_cast<std::int64_t>(end.y) - begin.y;
    const auto ax = std::abs(dx), ay = std::abs(dy);
    if (ax < kSwipeMinPx && ay < kSwipeMinPx) return ReleasedGesture::Tap;
    if (ax >= kSwipeMinPx && end.time_us - begin.time_us <= kSwipeMaxUs &&
        ax * kSwipeAngleScale >= ay * kSwipeAngleRatio)
        return dx < 0 ? ReleasedGesture::Next : ReleasedGesture::Previous;
    return ReleasedGesture::Ignore;
}

/** Released long presses must remain within the per-axis slop throughout the sampled contact. */
inline bool long_press(const Event& begin, const Event& end) {
    return begin.kind == EventKind::Touch && end.time_us - begin.time_us >= kLongPressUs &&
        std::abs(static_cast<std::int64_t>(end.x) - begin.x) <= kLongPressSlopPx &&
        std::abs(static_cast<std::int64_t>(end.y) - begin.y) <= kLongPressSlopPx &&
        std::abs(static_cast<std::int64_t>(end.min_x) - begin.x) <= kLongPressSlopPx &&
        std::abs(static_cast<std::int64_t>(end.max_x) - begin.x) <= kLongPressSlopPx &&
        std::abs(static_cast<std::int64_t>(end.min_y) - begin.y) <= kLongPressSlopPx &&
        std::abs(static_cast<std::int64_t>(end.max_y) - begin.y) <= kLongPressSlopPx;
}

/** Retains the initial target until release or an explicit long press, unless the frame changes. */
class ReleasedTap {
public:
    bool pending() const { return held_.has_value(); }
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
