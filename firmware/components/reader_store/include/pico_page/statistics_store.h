#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace pico_page {

struct StatisticsOptions {
    bool enabled = true;
    unsigned minimum_seconds = 5, maximum_seconds = 120;
};

inline bool operator!=(const StatisticsOptions& a, const StatisticsOptions& b) {
    return a.enabled != b.enabled || a.minimum_seconds != b.minimum_seconds ||
           a.maximum_seconds != b.maximum_seconds;
}

struct StatisticsBookSource {
    std::string path, content_id, title;
    int progress_percent = -1;
    std::string content_revision{};
};

struct StatisticsWrite {
    std::uint64_t id;
    std::int32_t day;
    std::uint32_t duration_ms;
};

struct StatisticsBook {
    std::uint64_t id = 0;
    std::string path, title;
    std::uint64_t total_ms = 0, unknown_ms = 0, selected_day_ms = 0;
    unsigned days = 0;
    std::int32_t first_day = -1, last_day = -1;
    int progress_percent = -1;
    std::string content_id{};
};

struct StatisticsQuery {
    std::int32_t from_day = 0, to_day = -1;
    std::int32_t selected_day = -2;
    std::uint64_t book_id = 0;
    unsigned offset = 0, limit = 0;
};

struct StatisticsReport {
    std::uint64_t total_ms = 0, unknown_ms = 0;
    unsigned days = 0, book_count = 0, matching_books = 0;
    std::int32_t first_day = -1, last_day = -1;
    std::vector<std::uint64_t> day_ms;
    std::vector<StatisticsBook> books;
};

}
