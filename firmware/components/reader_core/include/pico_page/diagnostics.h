#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#if defined(ESP_PLATFORM) && __has_include("sdkconfig.h")
#include "sdkconfig.h"
#endif

// Disabled calls discard their arguments, including clocks and context construction.
#if defined(CONFIG_PICO_PAGE_PERF_METRICS) && CONFIG_PICO_PAGE_PERF_METRICS
#define PP_DIAGNOSTIC(...) __VA_ARGS__
#else
#define PP_DIAGNOSTIC(...)
#endif

namespace pico_page::diagnostics {
enum class Kind : unsigned { Frame, Input, Network, Content, ImagePatch, Count };
enum class Outcome : unsigned { Success, Failure, Cancelled, Timeout, Count };
struct Context {
    std::uint64_t session = 0, layout = 0, frame = 0, request = 0;
    std::uint32_t detail = 0;
};
struct Token {
    std::uint64_t id = 0, started_us = 0;
    Kind kind = Kind::Frame;
    Context context{};
};
struct Event {
    std::uint64_t sequence = 0, time_us = 0, operation = 0, duration_us = 0;
    Context context{};
    std::uintptr_t task = 0;
    Kind kind = Kind::Frame;
    Outcome outcome = Outcome::Success;
    bool begin = false;
};
struct Metric {
    std::uint64_t started = 0, active = 0, total_us = 0, max_us = 0;
    std::array<std::uint64_t, static_cast<unsigned>(Outcome::Count)> outcomes{};
};
struct Snapshot {
    std::array<Metric, static_cast<unsigned>(Kind::Count)> metrics{};
    std::uint64_t sequence = 0, dropped = 0;
    std::size_t queued = 0;
    bool stopped = false;
};
/** Fixed storage; caller serializes access. Each token must end exactly once. */
class Recorder {
public:
    Token begin(Kind kind, Context context, std::uint64_t now_us, std::uintptr_t task);
    void end(Token token, Outcome outcome, std::uint64_t now_us, std::uintptr_t task);
    Snapshot snapshot() const;
    bool pop(Event& event);
#if defined(CONFIG_PICO_PAGE_TRACE_EVENTS) && CONFIG_PICO_PAGE_TRACE_EVENTS
    /** Start a fresh capture; tokens from previous captures cannot finish in it. */
    void start();
    /** Freeze events and counters atomically; active operations remain unfinished. */
    void stop();
#endif
private:
    void record(Event event);
    Snapshot state_{};
    std::uint64_t next_operation_ = 0;
#if defined(CONFIG_PICO_PAGE_TRACE_EVENTS) && CONFIG_PICO_PAGE_TRACE_EVENTS
    static constexpr std::size_t capacity = 64;
    std::array<Event, capacity> events_{};
    std::size_t head_ = 0;
    std::uint64_t first_operation_ = 1;
#endif
};
#if defined(CONFIG_PICO_PAGE_PERF_METRICS) && CONFIG_PICO_PAGE_PERF_METRICS
/** Task context only, never ISR. Cross-core critical sections only copy fixed data.
 * No allocation, I/O, formatting, task creation or polling. Time is monotonic microseconds.
 */
Token begin(Kind kind, Context context = {});
void end(Token token, Outcome outcome);
Snapshot snapshot();
bool pop(Event& event);
#if defined(CONFIG_PICO_PAGE_TRACE_EVENTS) && CONFIG_PICO_PAGE_TRACE_EVENTS
void start();
void stop();
#endif
#endif
} // namespace pico_page::diagnostics
