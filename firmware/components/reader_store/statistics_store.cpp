#include "pico_page/store.h"
#include "pico_page/reading_statistics.h"
#include "store_internal.h"

#include <algorithm>
#include <array>
#include <string_view>

namespace pico_page {
namespace {

constexpr char directory[] = "PicoPage/data/statistics/";
constexpr unsigned days_per_segment = 256;
constexpr unsigned segment_count = statistics_day_limit / days_per_segment;
static_assert(segment_count <= 256 && segment_count * days_per_segment == statistics_day_limit);
constexpr unsigned book_capacity = 512;
constexpr std::size_t file_limit = 16384;
constexpr auto ok = StoreStatus::Ok;
constexpr auto missing = StoreStatus::Missing;
constexpr auto invalid = StoreStatus::Invalid;
using DayBits = std::array<std::uint64_t, 4>;
using DayTotals = std::array<std::uint64_t, days_per_segment>;

std::uint64_t checksum(std::string_view bytes) {
    std::uint64_t hash = 14695981039346656037ULL;
    for (unsigned char c : bytes) hash = (hash ^ c) * 1099511628211ULL;
    return hash;
}

void put(std::string& out, std::uint64_t value, unsigned size = 8) {
    for (unsigned i = 0; i < size; ++i) {
        out.push_back(static_cast<char>(value & 255));
        value >>= 8;
    }
}

void put_text(std::string& out, const std::string& value) {
    put(out, value.size(), 2);
    out += value;
}

struct Reader {
    std::string_view bytes;
    bool valid = true;

    std::uint64_t number(unsigned size = 8) {
        if (bytes.size() < size) { valid = false; return 0; }
        std::uint64_t result = 0;
        for (unsigned i = 0; i < size; ++i)
            result |= std::uint64_t(static_cast<unsigned char>(bytes[i])) << (8 * i);
        bytes.remove_prefix(size);
        return result;
    }

    std::string text(std::size_t limit) {
        const auto size = number(2);
        if (!valid || size > bytes.size() || size > limit) { valid = false; return {}; }
        std::string result(bytes.substr(0, size));
        bytes.remove_prefix(size);
        return result;
    }

    bool done() const { return valid && bytes.empty(); }
};

std::string file_bytes(char kind, std::uint64_t sequence, const std::string& body) {
    std::string bytes = kind == 'B' ? "PPST2" : "PPST1";
    bytes += kind;
    put(bytes, sequence);
    bytes += body;
    put(bytes, checksum(bytes));
    return bytes;
}

StoreStatus payload(const std::string& bytes, char kind, std::uint64_t& sequence, std::string_view& body) {
    if (bytes.size() < 22 || bytes.compare(0, 4, "PPST") != 0 || bytes[5] != kind) return invalid;
    if (bytes[4] != '1' && !(kind == 'B' && bytes[4] == '2')) return StoreStatus::UnsupportedVersion;
    Reader trailer{std::string_view(bytes).substr(bytes.size() - 8)};
    if (trailer.number() != checksum(std::string_view(bytes).substr(0, bytes.size() - 8))) return invalid;
    Reader reader{std::string_view(bytes).substr(6, bytes.size() - 14)};
    sequence = reader.number();
    body = reader.bytes;
    return sequence ? ok : invalid;
}

struct Snapshot {
    std::array<std::optional<std::string>, 2> slots;
    unsigned active = 1;
    std::uint64_t sequence = 0;

    std::string_view body() const {
        const auto& bytes = *slots[active];
        return std::string_view(bytes).substr(14, bytes.size() - 22);
    }
};

StoreStatus load(const std::string& root, const std::string& path, char kind, Snapshot& snapshot) {
    bool malformed = false;
    for (unsigned slot = 0; slot < 2; ++slot) {
        std::string bytes;
        auto status = store_internal::read_file(root, path + "." + std::to_string(slot), file_limit, bytes);
        if (status == missing) continue;
        if (status != ok) return status;
        snapshot.slots[slot] = std::move(bytes);
        std::uint64_t sequence;
        std::string_view body;
        status = payload(*snapshot.slots[slot], kind, sequence, body);
        if (status == invalid) { malformed = true; continue; }
        if (status != ok) return status;
        if (sequence == snapshot.sequence) return invalid;
        if (sequence > snapshot.sequence) { snapshot.active = slot; snapshot.sequence = sequence; }
    }
    return snapshot.sequence ? ok : malformed ? invalid : missing;
}

StoreStatus publish(const std::string& root, const std::string& path, char kind,
                    Snapshot& snapshot, const std::string& body, bool mirror = false) {
    for (unsigned pass = 0; pass < (mirror ? 2U : 1U); ++pass) {
        if (snapshot.sequence == UINT64_MAX) return invalid;
        const auto slot = 1 - snapshot.active;
        auto next = file_bytes(kind, snapshot.sequence + 1, body);
        std::string error;
        const auto destination = path + "." + std::to_string(slot);
        auto status = store_internal::commit_file(root, destination, snapshot.slots[slot], next, file_limit, error, true);
        if (status != ok) {
            if (pass != 0) return StoreStatus::MirrorIncomplete;
            if (mirror) {
                std::string visible;
                if (store_internal::read_file(root, destination, file_limit, visible) == ok &&
                    visible == next) return StoreStatus::MirrorIncomplete;
            }
            return status;
        }
        snapshot.slots[slot] = std::move(next);
        ++snapshot.sequence;
        snapshot.active = slot;
    }
    return ok;
}

struct Catalog {
    Snapshot snapshot;
    std::uint64_t writer = 0, clear_writer = 0;
    unsigned generation = 1, count = 0;

    std::uint64_t book_id(unsigned index) const {
        return (std::uint64_t(generation) << 32) | index;
    }

    StoreStatus read(const std::string& root) {
        auto status = load(root, std::string(directory) + "catalog", 'C', snapshot);
        if (status == missing) {
            status = store_internal::safe_path(root, std::string(directory) + "books");
            return status == missing ? ok : status == ok ? invalid : status;
        }
        if (status != ok) return status;
        Reader in{snapshot.body()};
        writer = in.number();
        clear_writer = in.number();
        generation = static_cast<unsigned>(in.number(4));
        count = static_cast<unsigned>(in.number(2));
        return in.done() && generation && count <= book_capacity && clear_writer <= writer ? ok : invalid;
    }

    StoreStatus save(const std::string& root) {
        std::string bytes;
        put(bytes, writer); put(bytes, clear_writer); put(bytes, generation, 4); put(bytes, count, 2);
        return publish(root, std::string(directory) + "catalog", 'C', snapshot, bytes, true);
    }
};

std::string book_directory(std::uint64_t id) {
    return std::string(directory) + "books/" + std::to_string(static_cast<std::uint32_t>(id)) + "/";
}

std::string marker(const std::string& book) { return book + ".picopage/statistics-id"; }

struct Segment {
    unsigned index = 0, slot = 0;
    std::uint64_t version = 0;
    DayBits days{};

    void include(unsigned offset) { days[offset / 64] |= std::uint64_t(1) << (offset % 64); }
};

struct DatedTime {
    std::int32_t day = statistics_unknown_day;
    std::uint64_t duration_ms = 0;

    unsigned segment() const { return static_cast<unsigned>(day) / days_per_segment; }
    unsigned offset() const { return static_cast<unsigned>(day) % days_per_segment; }
};

struct Book {
    Snapshot snapshot;
    std::uint64_t id = 0, writer = 0, last_record = 0, clear_writer = 0, total = 0, unknown = 0;
    StatisticsBookSource source;
    std::uint64_t progress_signature = 0;
    std::vector<Segment> segments;
    DatedTime active_day;

    Segment& segment_for(unsigned index) {
        auto segment = std::lower_bound(segments.begin(), segments.end(), index,
                                        [](const Segment& entry, unsigned key) { return entry.index < key; });
        if (segment == segments.end() || segment->index != index)
            segment = segments.insert(segment, Segment{index});
        return *segment;
    }

    StoreStatus read(const std::string& root, std::uint64_t expected) {
        auto status = load(root, book_directory(expected) + "summary", 'B', snapshot);
        if (status != ok) return status;
        Reader in{snapshot.body()};
        id = in.number();
        if (id != expected) return missing;
        writer = in.number(); last_record = in.number(); clear_writer = in.number();
        total = in.number(); unknown = in.number();
        if ((*snapshot.slots[snapshot.active])[4] == '2') {
            const auto encoded_day = in.number(4);
            active_day.duration_ms = in.number();
            if (encoded_day > statistics_day_limit || (encoded_day == 0) != (active_day.duration_ms == 0)) return invalid;
            active_day.day = static_cast<std::int32_t>(encoded_day) - 1;
        }
        progress_signature = in.number();
        source.progress_percent = static_cast<int>(in.number(1)) - 1;
        source.path = in.text(1024); source.title = in.text(1024);
        source.content_id = in.text(256); source.content_revision = in.text(256);
        const auto count = in.number(2);
        if (!in.valid || unknown > total || active_day.duration_ms > total - unknown || count > segment_count ||
            source.progress_percent > 100 || !store_internal::relative_path(source.path)) return invalid;
        bool found_active = false, has_archive = false;
        segments.reserve(count);
        for (unsigned i = 0; i < count; ++i) {
            Segment segment;
            segment.index = static_cast<unsigned>(in.number(1));
            segment.slot = static_cast<unsigned>(in.number(1));
            segment.version = in.number();
            if (segment.slot > 1 || (i && segment.index <= segments.back().index)) return invalid;
            for (auto& bits : segment.days) bits = in.number();
            if (segment.days == DayBits{}) return invalid;
            has_archive |= segment.version != 0;
            if (active_day.duration_ms && segment.index == active_day.segment()) {
                const auto offset = active_day.offset();
                DayBits active_bit{};
                active_bit[offset / 64] = std::uint64_t(1) << (offset % 64);
                found_active = (segment.days[offset / 64] & active_bit[offset / 64]) != 0;
                if (!segment.version && (segment.slot != 0 || segment.days != active_bit)) return invalid;
            } else if (!segment.version) return invalid;
            segments.push_back(segment);
        }
        const auto archived_ms = total - unknown - active_day.duration_ms;
        return in.done() && (found_active || !active_day.duration_ms) && has_archive == (archived_ms != 0) ? ok : invalid;
    }

    StoreStatus save(const std::string& root, bool mirror = false) {
        std::string bytes;
        put(bytes, id); put(bytes, writer); put(bytes, last_record); put(bytes, clear_writer);
        put(bytes, total); put(bytes, unknown); put(bytes, active_day.day + 1, 4);
        put(bytes, active_day.duration_ms); put(bytes, progress_signature);
        put(bytes, source.progress_percent + 1, 1);
        put_text(bytes, source.path); put_text(bytes, source.title);
        put_text(bytes, source.content_id); put_text(bytes, source.content_revision);
        put(bytes, segments.size(), 2);
        for (const auto& segment : segments) {
            put(bytes, segment.index, 1); put(bytes, segment.slot, 1); put(bytes, segment.version);
            for (auto bits : segment.days) put(bytes, bits);
        }
        return publish(root, book_directory(id) + "summary", 'B', snapshot, bytes, mirror);
    }
};

StoreStatus read_marker(const std::string& root, const std::string& path, Snapshot& snapshot,
                        std::string& content, std::uint64_t& id) {
    const auto status = load(root, marker(path), 'I', snapshot);
    if (status != ok) return status;
    Reader in{snapshot.body()};
    id = in.number();
    content = in.text(256);
    return in.done() && id && !content.empty() ? ok : invalid;
}

StoreStatus resolve(const std::string& root, Catalog& catalog, const StatisticsBookSource& source,
                    bool create, Book& book) {
    if (!store_internal::relative_path(source.path)) return StoreStatus::UnsafePath;
    if (source.content_id.empty() || source.content_id.size() > 256 || source.title.size() > 1024 ||
        source.progress_percent < -1 || source.progress_percent > 100 || source.content_revision.size() > 256)
        return invalid;
    auto status = store_internal::safe_path(root, source.path);
    if (status != ok && status != missing) return status;
    Snapshot sidecar;
    std::string content;
    std::uint64_t id = 0;
    status = read_marker(root, source.path, sidecar, content, id);
    if (status != missing && status != ok) return status;
    if (status == ok && content == source.content_id && (id >> 32) == catalog.generation &&
        static_cast<std::uint32_t>(id) && static_cast<std::uint32_t>(id) <= catalog.count) {
        status = book.read(root, id);
        if (status != ok) return status == missing ? invalid : status;
        if (book.source.content_id != content) return invalid;
        if (book.source.path == source.path) return ok;
        Snapshot previous_marker;
        std::string previous_content;
        std::uint64_t previous_id = 0;
        status = read_marker(root, book.source.path, previous_marker, previous_content, previous_id);
        if (status != ok && status != missing) return status;
        if (status == missing || previous_id != id) {
            if (create) { book.source.path = source.path; return book.save(root); }
            return ok;
        }
        book = {};
    }
    if (!create) return missing;
    bool reserved = false;
    if (catalog.count) {
        status = book.read(root, catalog.book_id(catalog.count));
        if (status != ok) return status == missing ? invalid : status;
        reserved = !book.writer && !book.clear_writer && book.source.path == source.path &&
                   book.source.content_id == source.content_id;
    }
    if (!reserved) {
        if (catalog.count == book_capacity) return StoreStatus::StatisticsCatalogFull;
        id = catalog.book_id(catalog.count + 1);
        book = {};
        status = book.read(root, id);
        if (status != ok && status != missing) return status;
        book.id = id;
        book.source = source;
        status = book.save(root, true);
        if (status != ok) return status;
        ++catalog.count;
        status = catalog.save(root);
        if (status != ok) return status;
    }
    std::string binding;
    put(binding, book.id); put_text(binding, source.content_id);
    return publish(root, marker(source.path), 'I', sidecar, binding, true);
}

std::string segment_path(std::uint64_t id, const Segment& segment) {
    return book_directory(id) + "d" + std::to_string(segment.index) + "." + std::to_string(segment.slot);
}

StoreStatus read_segment(const std::string& root, std::uint64_t id, const Segment& segment,
                         DayTotals& totals) {
    if (!segment.version) return ok;
    std::string bytes;
    auto status = store_internal::read_file(root, segment_path(id, segment), file_limit, bytes);
    if (status != ok) return status == missing ? invalid : status;
    std::uint64_t version;
    std::string_view body;
    status = payload(bytes, 'D', version, body);
    if (status != ok) return status;
    Reader reader{body};
    if (version != segment.version || reader.number() != id || reader.number(1) != segment.index) return invalid;
    for (auto& total : totals) total = reader.number();
    return reader.done() ? ok : invalid;
}

StoreStatus save_segment(const std::string& root, std::uint64_t id, Segment& segment, const DayTotals& totals) {
    if (segment.version == UINT64_MAX) return invalid;
    ++segment.version;
    segment.slot = 1 - segment.slot;
    std::string bytes;
    put(bytes, id); put(bytes, segment.index, 1);
    for (auto value : totals) put(bytes, value);
    auto path = segment_path(id, segment);
    std::string previous, error;
    auto status = store_internal::read_file(root, path, file_limit, previous);
    if (status != missing && status != ok) return status;
    return store_internal::commit_file(root, path, status == ok ? std::optional<std::string>(std::move(previous)) : std::nullopt,
                                       file_bytes('D', segment.version, bytes), file_limit, error, true);
}

bool add(std::uint64_t& value, std::uint64_t increment) {
    if (value > UINT64_MAX - increment) return false;
    value += increment;
    return true;
}


}

StoreStatus FileStore::begin_statistics_writer(std::uint64_t& writer) const {
    Catalog catalog;
    auto status = catalog.read(root_);
    if (status != ok) return status;
    if (catalog.writer == UINT64_MAX) return invalid;
    ++catalog.writer;
    status = catalog.save(root_);
    if (status == ok) writer = catalog.writer;
    return status;
}

StoreStatus FileStore::statistics_book_id(const StatisticsBookSource& source, std::uint64_t& id, bool create) const {
    Catalog catalog;
    auto status = catalog.read(root_);
    if (status != ok) return status;
    Book book;
    status = resolve(root_, catalog, source, create, book);
    if (status == ok) id = book.id;
    return status;
}

StoreStatus FileStore::commit_statistics(std::uint64_t writer, const StatisticsBookSource& source,
                                        const std::vector<StatisticsWrite>& records) const {
    if (records.empty() || records.size() > 128 || !writer) return invalid;
    std::uint64_t last = 0;
    for (auto record : records) {
        if (record.id <= last || !record.duration_ms || record.day < -1 ||
            record.day >= static_cast<int>(segment_count * days_per_segment)) return invalid;
        last = record.id;
    }
    Catalog catalog;
    auto status = catalog.read(root_);
    if (status != ok) return status;
    if (writer != catalog.writer || writer <= catalog.clear_writer) return StoreStatus::ExternalChange;
    Book book;
    status = resolve(root_, catalog, source, true, book);
    if (status != ok) return status;
    if (writer <= book.clear_writer) return StoreStatus::ExternalChange;
    if (writer != book.writer) { book.writer = writer; book.last_record = 0; }
    auto first = std::find_if(records.begin(), records.end(), [&](auto record) { return record.id > book.last_record; });
    if (first == records.end()) return book.save(root_);
    auto active = book.active_day;
    for (auto it = records.end(); it != first;) {
        if ((--it)->day >= 0) { active.day = it->day; break; }
    }
    std::vector<DatedTime> archive;
    if (book.active_day.duration_ms && book.active_day.day != active.day) {
        archive.push_back(book.active_day);
        active.duration_ms = 0;
    }
    for (auto it = first; it != records.end(); ++it) {
        if (!add(book.total, it->duration_ms)) return invalid;
        if (it->day < 0) {
            if (!add(book.unknown, it->duration_ms)) return invalid;
        } else if (it->day == active.day) {
            if (!add(active.duration_ms, it->duration_ms)) return invalid;
        } else {
            archive.push_back({it->day, it->duration_ms});
        }
    }
    std::sort(archive.begin(), archive.end(), [](const DatedTime& a, const DatedTime& b) { return a.day < b.day; });
    for (auto it = archive.begin(); it != archive.end();) {
        auto& segment = book.segment_for(it->segment());
        DayTotals totals{};
        status = read_segment(root_, book.id, segment, totals);
        if (status != ok) return status;
        do {
            const auto offset = it->offset();
            if (!add(totals[offset], it->duration_ms)) return invalid;
            segment.include(offset);
            ++it;
        } while (it != archive.end() && it->segment() == segment.index);
        status = save_segment(root_, book.id, segment, totals);
        if (status != ok) return status;
    }
    if (active.duration_ms) book.segment_for(active.segment()).include(active.offset());
    book.active_day = active;
    book.last_record = last;
    book.source = source;
    book.progress_signature = 0;
    if (source.progress_percent >= 0 && !source.content_revision.empty()) {
        const auto progress = load_progress(source.path, source.content_id, source.content_revision);
        if (progress.record) book.progress_signature = position_signature(source.path, *progress.record);
    }
    return book.save(root_);
}

StoreStatus FileStore::clear_statistics(std::uint64_t book_id, std::uint64_t discard_through) const {
    Catalog catalog;
    auto status = catalog.read(root_);
    if (status != ok) return status;
    if (!book_id) {
        if (catalog.generation == UINT32_MAX) return invalid;
        ++catalog.generation;
        catalog.count = 0;
        catalog.clear_writer = catalog.writer;
        return catalog.save(root_);
    }
    if ((book_id >> 32) != catalog.generation || static_cast<std::uint32_t>(book_id) > catalog.count)
        return missing;
    Book book;
    status = book.read(root_, book_id);
    if (status != ok) return status;
    book.total = book.unknown = 0;
    book.active_day = {};
    if (discard_through == UINT64_MAX) {
        book.last_record = 0;
        book.clear_writer = catalog.writer;
    } else {
        const auto acknowledged = book.writer == catalog.writer ? book.last_record : 0;
        book.last_record = std::max(acknowledged, discard_through);
        book.writer = catalog.writer;
    }
    book.segments.clear();
    return book.save(root_, true);
}

int FileStore::current_statistics_progress(std::uint64_t id, const StatisticsBookSource& source,
                                           std::uint64_t recorded_signature, std::string& bound_content) const {
    if (store_internal::safe_path(root_, source.path) != ok) return -1;
    Snapshot binding;
    std::uint64_t bound_id = 0;
    std::string content;
    if (read_marker(root_, source.path, binding, content, bound_id) != ok ||
        bound_id != id || content != source.content_id) return -1;
    bound_content = content;
    if (source.content_revision.empty()) return -1;
    const auto saved = load_progress(source.path, content, source.content_revision);
    if (!saved.record) return -1;
    const auto current = position_signature(source.path, *saved.record);
    if (current && current == recorded_signature && source.progress_percent >= 0) return source.progress_percent;
    return cached_progress_percent(source.path, *saved.record);
}

StoreStatus FileStore::query_statistics(const StatisticsQuery& query, StatisticsReport& report,
                                       const std::function<bool()>& stop) const {
    constexpr int day_limit = segment_count * days_per_segment;
    if (query.from_day < 0 || query.from_day >= day_limit || query.to_day < -1 || query.to_day >= day_limit ||
        (query.to_day != -1 && (query.to_day < query.from_day || query.to_day - query.from_day >= 372)) ||
        query.selected_day < -2 || query.selected_day >= day_limit || query.limit > 64) return invalid;
    Catalog catalog;
    auto status = catalog.read(root_);
    if (status != ok) return status;
    StatisticsReport result;
    if (query.to_day >= query.from_day) result.day_ms.resize(query.to_day - query.from_day + 1);
    std::vector<DayBits> occupied(segment_count);
    auto begin = 1U, end = catalog.count;
    if (query.book_id) {
        begin = end = static_cast<std::uint32_t>(query.book_id);
        if ((query.book_id >> 32) != catalog.generation || begin == 0 || end > catalog.count) {
            report = std::move(result);
            return ok;
        }
    }
    for (unsigned index = begin; index <= end; ++index) {
        if (stop && stop()) return StoreStatus::Cancelled;
        Book book;
        status = book.read(root_, catalog.book_id(index));
        if (status != ok) return status == missing ? invalid : status;
        if (!book.total) continue;
        if (!add(result.total_ms, book.total) || !add(result.unknown_ms, book.unknown)) return invalid;
        ++result.book_count;
        StatisticsBook row;
        row.id = book.id;
        row.total_ms = book.total;
        row.unknown_ms = book.unknown;
        if (query.selected_day == -1) row.selected_day_ms = book.unknown;
        for (const auto& segment : book.segments) {
            if (stop && stop()) return StoreStatus::Cancelled;
            const int start = static_cast<int>(segment.index * days_per_segment);
            const int finish = start + days_per_segment - 1;
            for (unsigned i = 0; i < segment.days.size(); ++i) {
                const auto bits = segment.days[i];
                if (!bits) continue;
                occupied[segment.index][i] |= bits;
                row.days += static_cast<unsigned>(__builtin_popcountll(bits));
                const auto word_start = start + static_cast<int>(i * 64);
                if (row.first_day < 0) row.first_day = word_start + __builtin_ctzll(bits);
                row.last_day = word_start + 63 - __builtin_clzll(bits);
            }
            const bool selected = query.selected_day >= start && query.selected_day <= finish;
            const std::int32_t range_start = std::max<std::int32_t>(start, query.from_day);
            const std::int32_t range_end = std::min<std::int32_t>(finish, query.to_day);
            if (!selected && range_start > range_end) continue;
            DayTotals totals{};
            status = read_segment(root_, book.id, segment, totals);
            if (status != ok) return status;
            if (book.active_day.duration_ms && segment.index == book.active_day.segment() &&
                !add(totals[book.active_day.offset()], book.active_day.duration_ms)) return invalid;
            if (selected) row.selected_day_ms = totals[query.selected_day - start];
            for (int day = range_start; day <= range_end; ++day) {
                if (!add(result.day_ms[day - query.from_day], totals[day - start])) return invalid;
            }
        }
        if (row.first_day >= 0 && (result.first_day < 0 || row.first_day < result.first_day))
            result.first_day = row.first_day;
        result.last_day = std::max(result.last_day, row.last_day);
        if (query.selected_day != -2 && !row.selected_day_ms) continue;
        if (result.matching_books++ < query.offset || result.books.size() >= query.limit) continue;
        row.path = book.source.path;
        row.title = book.source.title;
        if (query.book_id) {
            row.progress_percent = current_statistics_progress(book.id, book.source, book.progress_signature,
                                                              row.content_id);
        }
        result.books.push_back(std::move(row));
    }
    for (const auto& segment : occupied)
        for (auto word : segment) result.days += static_cast<unsigned>(__builtin_popcountll(word));
    report = std::move(result);
    return ok;
}

}
