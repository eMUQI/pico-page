#pragma once

#include "pico_page/store.h"
#include <optional>

namespace pico_page {

struct StatisticsWork {
    enum class Kind { Commit, Query, LoadOptions, Options, Clear };
    Kind kind = Kind::Query;
    std::uint64_t mount = 0, request = 0, writer = 0, book_id = 0, discard_through = 0;
    StatisticsQuery query;
    std::optional<StatisticsBookSource> source;
    StatisticsOptions options, previous_options;
    std::vector<StatisticsWrite> records;
    std::vector<std::pair<std::uint64_t, StatisticsBookSource>> bindings;
    std::vector<std::uint64_t> cleared_tokens;
    StoreStatus status = StoreStatus::IoError;
    StatisticsReport report;

    void run(const FileStore& store, const std::function<bool()>& stop);
};

}
