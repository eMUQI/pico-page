#include "pico_page/diagnostics.h"
#if defined(CONFIG_PICO_PAGE_PERF_METRICS) && CONFIG_PICO_PAGE_PERF_METRICS
#ifdef ESP_PLATFORM
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#else
#include <chrono>
#include <mutex>
#endif

namespace pico_page::diagnostics {
Token Recorder::begin(Kind kind, Context context, std::uint64_t now, std::uintptr_t task) {
#if defined(CONFIG_PICO_PAGE_TRACE_EVENTS) && CONFIG_PICO_PAGE_TRACE_EVENTS
    if (state_.stopped) return {};
#endif
    Token token{++next_operation_, now, kind, context};
    auto& metric = state_.metrics[static_cast<unsigned>(kind)];
    ++metric.started;
    ++metric.active;
    record({0, now, token.id, 0, context, task, kind, Outcome::Success, true});
    return token;
}
void Recorder::end(Token token, Outcome outcome, std::uint64_t now, std::uintptr_t task) {
#if defined(CONFIG_PICO_PAGE_TRACE_EVENTS) && CONFIG_PICO_PAGE_TRACE_EVENTS
    if (state_.stopped || token.id < first_operation_) return;
#endif
    auto& metric = state_.metrics[static_cast<unsigned>(token.kind)];
    --metric.active;
    ++metric.outcomes[static_cast<unsigned>(outcome)];
    const auto elapsed = now - token.started_us;
    metric.total_us += elapsed;
    if (elapsed > metric.max_us) metric.max_us = elapsed;
    record({0, now, token.id, elapsed, token.context, task, token.kind, outcome, false});
}
void Recorder::record(Event event) {
#if defined(CONFIG_PICO_PAGE_TRACE_EVENTS) && CONFIG_PICO_PAGE_TRACE_EVENTS
    event.sequence = ++state_.sequence;
    if (state_.queued == capacity) { ++state_.dropped; return; }
    events_[(head_ + state_.queued++) % capacity] = event;
#else
    (void)event;
#endif
}
Snapshot Recorder::snapshot() const { return state_; }
#if defined(CONFIG_PICO_PAGE_TRACE_EVENTS) && CONFIG_PICO_PAGE_TRACE_EVENTS
void Recorder::start() {
    state_ = {};
    head_ = 0;
    first_operation_ = next_operation_ + 1;
}
void Recorder::stop() { state_.stopped = true; }
#endif
bool Recorder::pop(Event& event) {
#if defined(CONFIG_PICO_PAGE_TRACE_EVENTS) && CONFIG_PICO_PAGE_TRACE_EVENTS
    if (!state_.queued) return false;
    event = events_[head_];
    head_ = (head_ + 1) % capacity;
    --state_.queued;
    return true;
#else
    (void)event;
    return false;
#endif
}
namespace {
Recorder recorder;
#ifdef ESP_PLATFORM
portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
struct Lock {
    Lock() { portENTER_CRITICAL(&mux); }
    ~Lock() { portEXIT_CRITICAL(&mux); }
};
std::uint64_t clock_us() { return esp_timer_get_time(); }
std::uintptr_t task_id() { return reinterpret_cast<std::uintptr_t>(xTaskGetCurrentTaskHandle()); }
#else
std::mutex mutex;
struct Lock { std::lock_guard<std::mutex> guard{mutex}; };
std::uint64_t clock_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
std::uintptr_t task_id() { return 0; }
#endif
}
Token begin(Kind kind, Context context) {
    Lock lock;
    return recorder.begin(kind, context, clock_us(), task_id());
}
void end(Token token, Outcome outcome) {
    Lock lock;
    recorder.end(token, outcome, clock_us(), task_id());
}
Snapshot snapshot() { Lock lock; return recorder.snapshot(); }
bool pop(Event& event) { Lock lock; return recorder.pop(event); }
#if defined(CONFIG_PICO_PAGE_TRACE_EVENTS) && CONFIG_PICO_PAGE_TRACE_EVENTS
void start() { Lock lock; recorder.start(); }
void stop() { Lock lock; recorder.stop(); }
#endif
} // namespace pico_page::diagnostics
#endif
