#include "pico_page/reading_statistics.h"

#include <cstdlib>
#include <iostream>
#include <limits>
#include <map>

using namespace pico_page;

#define CHECK(...) do { if (!(__VA_ARGS__)) { std::cerr << __FILE__ << ':' << __LINE__ << ": " #__VA_ARGS__ "\n"; std::exit(1); } } while (false)

constexpr std::uint64_t book = 14;
constexpr std::int32_t day = 20000;
constexpr std::int64_t midnight_utc = std::int64_t{day} * 86400 - 8 * 3600;

static StatisticsClock at(std::uint64_t ms) {
    return {ms, midnight_utc + static_cast<std::int64_t>(ms / 1000)};
}

static std::map<std::int32_t, std::uint64_t> collect(ReadingStatistics& stats) {
    std::map<std::int32_t, std::uint64_t> totals;
    while (const auto* delta = stats.pending()) {
        totals[delta->day] += delta->duration_ms;
        CHECK(stats.acknowledge(delta->id));
    }
    return totals;
}

static void visits() {
    ReadingStatistics stats;
    stats.present(book, true, at(0));
    stats.pause(at(40000));
    stats.sample(at(100000));
    stats.present(book, false, at(220000));
    stats.end_visit(at(250000));
    CHECK(collect(stats)[day] == 70000);
    stats.present(book, true, at(250000));
    stats.sample(at(850000));
    stats.pause(at(900000));
    stats.present(book, false, at(950000));
    stats.end_visit(at(1000000));
    CHECK(collect(stats)[day] == 120000);

    for (unsigned i = 0; i < 20; ++i) stats.present(book, true, at(i * 4000));
    stats.end_visit(at(79999));
    CHECK(stats.pending_count() == 0);
    stats.present(book, true, at(80000));
    stats.pause(at(83000));
    stats.present(book, false, at(99000));
    stats.sample(at(100999));
    CHECK(stats.pending_count() == 0);
    stats.sample(at(101000));
    CHECK(collect(stats)[day] == 5000);
    stats.present(book, false, at(102000));
    stats.end_visit(at(104000));
    CHECK(collect(stats)[day] == 3000);
}

static void civil_time() {
    ReadingStatistics stats;
    stats.present(book, true, {0, midnight_utc - 2});
    stats.pause({3000, midnight_utc + 1});
    CHECK(stats.pending_count() == 0);
    stats.present(book, false, {3600000, midnight_utc + 3598});
    stats.end_visit({3605000, midnight_utc + 3603});
    auto days = collect(stats);
    CHECK(days.size() == 2 && days[day - 1] == 2000 && days[day] == 6000);

    stats.present(book, true, {0, {}});
    stats.present(book, false, {4000, midnight_utc});
    stats.sample({8000, midnight_utc + 3 * 86400});
    stats.sample({11000, midnight_utc - 2 * 86400});
    stats.end_visit({17000, {}});
    days = collect(stats);
    CHECK(days.size() == 4 && days[statistics_unknown_day] == 4000 && days[day] == 4000);
    CHECK(days[day + 3] == 3000 && days[day - 2] == 6000);

    stats.present(book, true, {0, midnight_utc - 60});
    stats.end_visit({600000, midnight_utc + 540});
    days = collect(stats);
    CHECK(days.size() == 2 && days[day - 1] == 60000 && days[day] == 60000);

    stats.present(book, true, {0, midnight_utc - 2});
    for (unsigned ms = 250; ms <= 5250; ms += 250)
        stats.sample({ms, midnight_utc - 2 + ms / 1000});
    stats.end_visit({5250, midnight_utc + 3});
    days = collect(stats);
    CHECK(days[day - 1] == 2000 && days[day] == 3250);

    constexpr auto last_midnight = std::int64_t{statistics_day_limit} * 86400 - 8 * 3600;
    stats.present(book, true, {0, last_midnight - 3});
    stats.sample({9000, last_midnight + 6});
    stats.end_visit({12000, last_midnight + 9});
    days = collect(stats);
    CHECK(days.size() == 2 && days[statistics_day_limit - 1] == 3000 && days[statistics_unknown_day] == 9000);

    stats.configure({10, 120});
    stats.present(book, true, at(0));
    for (unsigned i = 0; i < 10; ++i)
        stats.sample({(i + 1) * 1000, midnight_utc + (i + 1) * 86400});
    stats.end_visit({10000, midnight_utc + 10 * 86400});
    days = collect(stats);
    CHECK(days.size() == 9 && !days.count(statistics_unknown_day) && stats.dropped_ms() == 1000);

    stats.present(book, true, {0, std::numeric_limits<std::int64_t>::max()});
    stats.end_visit({std::numeric_limits<std::uint64_t>::max(), {}});
    CHECK(collect(stats)[statistics_unknown_day] == 120000);
}

static void thresholds_and_acknowledgements() {
    ReadingStatistics stats;
    CHECK(!stats.configure({0, 50}));
    CHECK(!stats.configure({50, 49}));
    CHECK(stats.configure({1, 10}));
    stats.present(book, true, at(0));
    CHECK(stats.configure({30, 60}));
    stats.sample(at(6000));
    const auto* first = stats.pending();
    CHECK(first && first->duration_ms == 6000 && stats.has_pending(book));
    const auto sent = *first;
    stats.pause(at(37000));
    CHECK(stats.pending_count() == 2);
    CHECK(stats.pending()->id == sent.id && first->duration_ms == sent.duration_ms);
    CHECK(!stats.acknowledge(stats.pending(1)->id));
    CHECK(stats.acknowledge(sent.id) && !stats.acknowledge(sent.id));
    CHECK(collect(stats)[day] == 4000);
    CHECK(!stats.has_pending(book));
    stats.present(book + 1, false, at(38000));
    stats.end_visit(at(67000));
    CHECK(stats.pending_count() == 0);
    stats.present(book + 1, true, at(67000));
    stats.end_visit(at(97000));
    CHECK(collect(stats)[day] == 30000);
    stats.present(0, true, at(97000));
    stats.sample(at(195000));
    CHECK(stats.pending_count() == 0);
}

static void bounded_queue() {
    ReadingStatistics stats;
    for (std::size_t i = 0; i < stats.capacity + 2; ++i) {
        stats.present(i + 1, true, at(i * 5000));
        stats.end_visit(at((i + 1) * 5000));
    }
    CHECK(stats.pending_count() == stats.capacity && stats.dropped_ms() == 10000);
    CHECK(!stats.pending(stats.capacity));
    CHECK(stats.pending(stats.capacity - 1)->book_token == stats.capacity);
    for (std::size_t i = 0; i < stats.capacity; ++i) {
        const auto delta = *stats.pending();
        CHECK(delta.book_token == i + 1 && delta.duration_ms == 5000);
        CHECK(stats.acknowledge(delta.id));
        stats.present(book, true, at(200000 + i * 5000));
    }
    stats.end_visit(at(200000 + stats.capacity * 5000));
    CHECK(collect(stats)[day] == stats.capacity * 5000);
}

static void clear_with_pending_writes() {
    ReadingStatistics stats;
    for (std::size_t i = 0; i < stats.capacity; ++i) {
        stats.present(100 + i, true, at(0));
        stats.end_visit(at(5000));
    }
    stats.present(99, true, at(0));
    stats.end_visit(at(6000));
    CHECK(stats.dropped_ms() == 6000);

    for (std::size_t i = 0; i + 1 < stats.capacity; ++i) {
        const auto* row = stats.pending();
        CHECK(row && stats.acknowledge(row->id));
    }
    const auto retained = *stats.pending();
    stats.present(book, true, at(0));
    stats.pause(at(5000));
    const auto removed = *stats.pending(1);
    stats.present(book + 1, true, at(7000));
    stats.pause(at(14000));
    const auto other = *stats.pending(2);
    stats.present(book, true, at(15000));
    stats.pause(at(18000));
    stats.discard(book);
    stats.present(book, false, at(20000));
    stats.end_visit(at(23000));
    CHECK(!stats.has_pending(book) && stats.pending_count() == 2);
    CHECK(!stats.acknowledge(removed.id));
    CHECK(stats.acknowledge(retained.id) && stats.acknowledge(other.id));
    CHECK(stats.dropped_ms() == 6000);

    stats.present(book + 1, true, at(0));
    stats.pause(at(2000));
    stats.discard(book);
    stats.present(book + 1, false, at(5000));
    stats.sample(at(9000));
    const auto pending_id = stats.pending()->id;
    stats.discard(0);
    CHECK(!stats.acknowledge(pending_id) && stats.pending_count() == 0 && !stats.dropped_ms());
    stats.sample(at(20000));
    CHECK(!stats.pending());
    stats.present(book + 1, true, at(30000));
    stats.end_visit(at(35000));
    CHECK(stats.pending()->id > pending_id);
}

int main() {
    visits();
    civil_time();
    thresholds_and_acknowledgements();
    bounded_queue();
    clear_with_pending_writes();
}
