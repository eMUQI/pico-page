#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace pico_page {

struct StatisticsClock {
    std::uint64_t monotonic_ms = 0;
    std::optional<std::int64_t> unix_seconds;
};

struct StatisticsThresholds {
    std::uint16_t minimum_seconds = 5;
    std::uint16_t maximum_seconds = 120;
};

constexpr std::int32_t statistics_unknown_day = -1;
constexpr std::int32_t statistics_day_limit = 65536;

/** Dates are integral UTC+8 days since 1970-01-01. IDs are scoped to this collector's lifetime. */
struct StatisticsDelta {
    std::uint64_t id = 0;
    std::uint64_t book_token = 0;
    std::int32_t day = statistics_unknown_day;
    std::uint32_t duration_ms = 0;
};

class ReadingStatistics {
public:
    static constexpr std::size_t capacity = 32;

    /** Threshold changes apply to the next visit. A visit may span several paused intervals. */
    bool configure(StatisticsThresholds thresholds);
    /** Called only after presentation. The caller supplies a nonzero book instance and identifies actual navigation. */
    void present(std::uint64_t book_token, bool new_visit, StatisticsClock now);
    void sample(StatisticsClock now);
    void pause(StatisticsClock now);
    void end_visit(StatisticsClock now);

    /** Inspecting a row freezes it and all earlier rows until acknowledged, including after a failed write. */
    const StatisticsDelta* pending(std::size_t index = 0);
    std::size_t pending_count() const { return count_; }
    bool has_pending(std::uint64_t book_token) const;
    bool acknowledge(std::uint64_t id);
    /** After durable clearing and completion of outstanding writes, discard this book or all books (zero). */
    void discard(std::uint64_t book_token);
    std::uint64_t dropped_ms() const { return dropped_ms_; }

private:
    struct Interval {
        std::uint64_t monotonic_ms;
        std::optional<std::uint64_t> local_ms;
    };
    struct DayDuration {
        std::int32_t day = statistics_unknown_day;
        std::uint32_t duration_ms = 0;
    };

    StatisticsThresholds thresholds_, visit_thresholds_;
    std::uint64_t book_token_ = 0;
    std::uint32_t elapsed_ms_ = 0, unattributed_ms_ = 0;
    std::optional<Interval> interval_;
    std::array<DayDuration, 8> below_threshold_{};
    std::array<StatisticsDelta, capacity> queue_{};
    std::size_t head_ = 0, count_ = 0, frozen_ = 0;
    std::uint64_t next_id_ = 1, dropped_ms_ = 0;

    void reset_visit();
    void credit(std::int32_t day, std::uint32_t duration_ms);
    void enqueue(std::int32_t day, std::uint32_t duration_ms);
};

}
