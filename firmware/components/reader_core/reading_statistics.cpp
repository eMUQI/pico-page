#include "pico_page/reading_statistics.h"

#include <algorithm>

namespace pico_page {
namespace {

constexpr std::uint64_t day_ms = 86400000;
constexpr std::uint64_t utc_offset_seconds = 8 * 3600;
constexpr std::uint64_t max_local_ms = statistics_day_limit * day_ms - 1;

std::optional<std::uint64_t> local_milliseconds(StatisticsClock clock) {
    if (!clock.unix_seconds || *clock.unix_seconds < 0) return {};
    const auto utc = static_cast<std::uint64_t>(*clock.unix_seconds);
    if (utc > max_local_ms / 1000 - utc_offset_seconds) return {};
    return (utc + utc_offset_seconds) * 1000;
}

}

bool ReadingStatistics::configure(StatisticsThresholds thresholds) {
    if (!thresholds.minimum_seconds || thresholds.minimum_seconds > thresholds.maximum_seconds) return false;
    thresholds_ = thresholds;
    return true;
}

void ReadingStatistics::present(std::uint64_t book_token, bool new_visit, StatisticsClock now) {
    if (!book_token || new_visit || book_token != book_token_) {
        end_visit(now);
        book_token_ = book_token;
        visit_thresholds_ = thresholds_;
    }
    if (interval_) sample(now);
    else if (book_token) interval_ = Interval{now.monotonic_ms, local_milliseconds(now)};
}

void ReadingStatistics::sample(StatisticsClock now) {
    if (!interval_ || now.monotonic_ms < interval_->monotonic_ms) return;
    const auto elapsed = now.monotonic_ms - interval_->monotonic_ms;
    const auto limit_ms = visit_thresholds_.maximum_seconds * 1000u;
    const auto duration_ms = static_cast<std::uint32_t>(std::min<std::uint64_t>(elapsed, limit_ms - elapsed_ms_));
    const auto start = interval_->local_ms;
    if (duration_ms) {
        elapsed_ms_ += duration_ms;
        if (start) {
            const auto first_ms = std::min<std::uint64_t>(duration_ms, day_ms - *start % day_ms);
            const auto day = static_cast<std::int32_t>(*start / day_ms);
            credit(day, static_cast<std::uint32_t>(first_ms));
            if (duration_ms > first_ms)
                credit(day + 1 == statistics_day_limit ? statistics_unknown_day : day + 1, duration_ms - first_ms);
        } else {
            credit(statistics_unknown_day, duration_ms);
        }
    }
    auto next_local_ms = local_milliseconds(now);
    if (start && next_local_ms && elapsed <= max_local_ms - *start) {
        const auto projected = *start + elapsed;
        const auto wall_ms = *next_local_ms;
        if (projected >= wall_ms && projected - wall_ms < 1000) next_local_ms = projected;
    }
    interval_ = Interval{now.monotonic_ms, next_local_ms};
}

void ReadingStatistics::credit(std::int32_t day, std::uint32_t duration_ms) {
    if (elapsed_ms_ < visit_thresholds_.minimum_seconds * 1000u) {
        for (auto& bucket : below_threshold_) {
            if (!bucket.duration_ms || bucket.day == day) {
                bucket.day = day;
                bucket.duration_ms += duration_ms;
                return;
            }
        }
        unattributed_ms_ += duration_ms;
        return;
    }
    dropped_ms_ += std::min<std::uint64_t>(unattributed_ms_, UINT64_MAX - dropped_ms_);
    unattributed_ms_ = 0;
    for (auto& bucket : below_threshold_) {
        if (bucket.duration_ms) enqueue(bucket.day, bucket.duration_ms);
        bucket = {};
    }
    enqueue(day, duration_ms);
}

void ReadingStatistics::enqueue(std::int32_t day, std::uint32_t duration_ms) {
    for (std::size_t i = count_; i > frozen_; --i) {
        auto& row = queue_[(head_ + i - 1) % capacity];
        if (row.book_token == book_token_ && row.day == day && duration_ms <= UINT32_MAX - row.duration_ms) {
            row.duration_ms += duration_ms;
            return;
        }
    }
    if (count_ == capacity || !next_id_) {
        dropped_ms_ += std::min<std::uint64_t>(duration_ms, UINT64_MAX - dropped_ms_);
        return;
    }
    queue_[(head_ + count_) % capacity] = StatisticsDelta{next_id_++, book_token_, day, duration_ms};
    ++count_;
}

void ReadingStatistics::pause(StatisticsClock now) {
    sample(now);
    interval_.reset();
}

void ReadingStatistics::reset_visit() {
    interval_.reset();
    book_token_ = 0;
    elapsed_ms_ = 0;
    unattributed_ms_ = 0;
    below_threshold_ = {};
}

void ReadingStatistics::end_visit(StatisticsClock now) {
    sample(now);
    reset_visit();
}

const StatisticsDelta* ReadingStatistics::pending(std::size_t index) {
    if (index >= count_) return nullptr;
    frozen_ = std::max(frozen_, index + 1);
    return &queue_[(head_ + index) % capacity];
}

bool ReadingStatistics::has_pending(std::uint64_t book_token) const {
    for (std::size_t i = 0; i < count_; ++i)
        if (queue_[(head_ + i) % capacity].book_token == book_token) return true;
    return false;
}

void ReadingStatistics::discard(std::uint64_t book_token) {
    if (!book_token || book_token == book_token_) reset_visit();
    if (!book_token) {
        count_ = 0;
        frozen_ = 0;
        dropped_ms_ = 0;
        return;
    }
    std::size_t retained = 0, retained_frozen = 0;
    for (std::size_t i = 0; i < count_; ++i) {
        const auto& row = queue_[(head_ + i) % capacity];
        if (row.book_token == book_token) continue;
        queue_[(head_ + retained) % capacity] = row;
        ++retained;
        if (i < frozen_) ++retained_frozen;
    }
    count_ = retained;
    frozen_ = retained_frozen;
}

void ReadingStatistics::reject(std::uint64_t id) {
    const auto duration_ms = queue_[head_].duration_ms;
    if (acknowledge(id)) dropped_ms_ += std::min<std::uint64_t>(duration_ms, UINT64_MAX - dropped_ms_);
}

bool ReadingStatistics::acknowledge(std::uint64_t id) {
    if (!frozen_ || queue_[head_].id != id) return false;
    head_ = (head_ + 1) % capacity;
    --count_;
    --frozen_;
    return true;
}

}
