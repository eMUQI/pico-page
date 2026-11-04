#pragma once
#include <atomic>
#include <cstdint>
#include <memory>

namespace pico_page::network {
using Hold = std::shared_ptr<std::atomic_bool>;
inline bool live(const Hold& hold) { return hold && !hold->load(std::memory_order_acquire); }
inline void release(const Hold& hold) { if (hold) hold->store(true, std::memory_order_release); }

/** Monotonic microsecond activity clock; callers serialize updates and expiry checks. */
class IdleTimer {
public:
    void update(bool active, std::int64_t now_us) {
        active_ = active;
        last_activity_us_ = now_us;
    }
    bool expired(std::int64_t now_us, std::int64_t timeout_us) const {
        return !active_ && now_us - last_activity_us_ >= timeout_us;
    }
private:
    std::int64_t last_activity_us_ = 0;
    bool active_ = false;
};

/** Worker-owned holds. The UI only releases their atomic cancellation flags.
 * A sync decision keeps STA for at most two minutes after a completed request;
 * a manual hold has no idle deadline, but must be released before sleep.
 */
class Holds {
public:
    static constexpr std::int64_t decision_us = 120'000'000;
    Hold manual, sync;
    bool manual_suspended = false;
    void await_decision(Hold hold, std::int64_t now_us) {
        if (sync != hold) release(sync);
        sync = std::move(hold);
        deadline_us_ = now_us + decision_us;
    }
    void expire(std::int64_t now_us) {
        if (live(sync) && now_us >= deadline_us_) release(sync);
    }
    bool retained() const { return (live(manual) && !manual_suspended) || live(sync); }
    void stop() { release(manual); release(sync); manual_suspended = false; }
private:
    std::int64_t deadline_us_ = 0;
};
}
