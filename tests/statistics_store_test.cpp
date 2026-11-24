#include "pico_page/store.h"

#include <cassert>
#include <charconv>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <string_view>
#include <unistd.h>

namespace fs = std::filesystem;
using namespace pico_page;

namespace {

constexpr auto ok = StoreStatus::Ok;

struct Card {
    fs::path root;
    FileStore store;
    StatisticsBookSource original{"Books/story.txt", "first-sha", "Story", 23, "txt-v1"};
    std::uint64_t writer = 0;

    static fs::path temporary() {
        char path[] = "/tmp/pico-statistics-XXXXXX";
        auto created = mkdtemp(path);
        assert(created);
        return created;
    }

    Card() : root(temporary()), store(root.string()) {
        fs::create_directory(root / "Books");
        std::ofstream(root / original.path) << "one";
        assert(store.begin_statistics_writer(writer) == ok && writer);
    }

    ~Card() { fs::remove_all(root); }

    StatisticsReport report(StatisticsQuery query = {}) const {
        StatisticsReport result;
        assert(FileStore(root.string()).query_statistics(query, result) == ok);
        return result;
    }

    std::uint64_t id(const StatisticsBookSource& book) const {
        std::uint64_t result = 0;
        assert(store.statistics_book_id(book, result, false) == ok);
        return result;
    }
};

void global_settings() {
    Card card;
    const auto config = card.root / "PicoPage/settings.toml";
    const std::string original =
        "schema_version = 1\n[reader]\nfont_size_px = 34 # personal size\n"
        "[statistics]\nenabled = true # collect\nminimum_seconds = 7\nmaximum_seconds = 45\nlanguage = \"zh-Hant\"\n";
    std::ofstream(config) << original;
    StatisticsOptions loaded;
    assert(card.store.load_statistics_options(loaded) == ok && loaded.enabled && loaded.minimum_seconds == 7 &&
           loaded.maximum_seconds == 45 && loaded.language == 1);
    StatisticsOptions changed = loaded;
    changed.enabled = false;
    assert(card.store.save_statistics_options(changed, &loaded) == ok);
    std::ifstream input(config);
    const std::string updated{std::istreambuf_iterator<char>(input), {}};
    auto expected = original;
    expected.replace(expected.find("enabled = true"), 14, "enabled = false");
    assert(updated == expected);
    FileStore reopened(card.root.string());
    assert(reopened.load_statistics_options(loaded) == ok && !loaded.enabled);
    std::ofstream(config) << original;
    assert(card.store.save_statistics_options(changed, &loaded) == StoreStatus::ExternalChange);
    const auto sidecar = card.root / (card.original.path + ".picopage");
    fs::create_directory(sidecar);
    std::ofstream(sidecar / "settings.toml") << "schema_version = 1\n[statistics]\nenabled = false\n";
    auto book_settings = card.store.load_settings(card.original.path);
    assert(!book_settings.diagnostics.empty() && book_settings.settings.statistics.enabled);
}

void totals_and_replay() {
    Card card;
    const auto& source = card.original;
    StatisticsOptions options;
    assert(card.store.load_statistics_options(options) == ok && options.enabled && options.maximum_seconds == 120);
    options = {false, 10, 50, 2};
    assert(card.store.save_statistics_options(options) == ok);
    options = {};
    assert(card.store.load_statistics_options(options) == ok && !options.enabled && options.minimum_seconds == 10 &&
           options.maximum_seconds == 50 && options.language == 2);
    auto invalid_options = options;
    invalid_options.minimum_seconds = 51;
    assert(card.store.save_statistics_options(invalid_options) == StoreStatus::Invalid);
    auto report = card.report({200, 260, -2, 0, 0, 6});
    assert(report.total_ms == 0 && report.day_ms.size() == 61 && report.days == 0);
    ReadingRecord progress{source.content_id, source.content_revision, "reader", 1, {"chapter", "node", 12}};
    assert(card.store.save_progress(source.path, progress).record);
    std::vector<StatisticsWrite> rows{{1, 255, 42000}, {2, 256, 51000}, {3, -1, 12345678}};
    assert(card.store.commit_statistics(card.writer, source, rows) == ok);
    auto id = card.id(source);
    assert(FileStore(card.root.string()).commit_statistics(card.writer, source, rows) == ok);
    report = card.report({255, 260, -2, id, 0, 2});
    assert(report.total_ms == 12438678 && report.unknown_ms == 12345678 && report.days == 2 && report.book_count == 1);
    assert(report.first_day == 255 && report.last_day == 256 && report.day_ms[0] == 42000 && report.day_ms[1] == 51000);
    assert(report.books.size() == 1 && report.books[0].progress_percent == 23);
    progress.local_revision++;
    progress.anchor.offset++;
    assert(card.store.save_progress(source.path, progress).record);
    assert(card.report({0, -1, -2, id, 0, 2}).books[0].progress_percent == -1);
    const auto cache = FileStore::position_cache_name(source.path);
    const auto position = card.store.encode_cached_position(source.path, progress, "next chapter", "46%");
    assert(card.store.save_cache(cache, position) == ok);
    assert(card.store.recalled_position(source.path).second == "46%");
    assert(card.report({0, -1, -2, id, 0, 2}).books[0].progress_percent == 46);
    auto legacy = position;
    legacy.replace(0, 6, "PPPOS1");
    const auto signature_start = legacy.find('\n', legacy.find('\n', 7) + 1) + 1;
    legacy.erase(signature_start, legacy.find('\n', signature_start) - signature_start + 1);
    assert(card.store.save_cache(cache, legacy) == ok);
    assert(card.store.recalled_position(source.path).second == "46%");
    assert(card.report({0, -1, -2, id, 0, 2}).books[0].progress_percent == -1);
    assert(card.store.save_cache(cache, position) == ok);
    assert(card.store.clear_cache() == ok);
    assert(card.report({0, -1, -2, id, 0, 2}).books[0].progress_percent == -1);
    assert(card.store.save_cache(cache, position) == ok);
    BookSnapshot snapshot;
    assert(card.store.book_snapshot(source.path, snapshot) == ok);
    assert(card.store.manage_book(snapshot, false, source.content_id, source.content_revision) == ok);
    assert(card.report({0, -1, -2, id, 0, 2}).books[0].progress_percent == -1);
    assert(card.store.commit_statistics(card.writer, source, {{1, 2, 7000},{1, 3, 8000}}) == StoreStatus::Invalid);
    assert(card.store.commit_statistics(card.writer, source, {{4, 65536, 8000}}) == StoreStatus::Invalid);
    assert(card.store.commit_statistics(card.writer, source, {{3, -1, 12345678}, {5, 256, 2000}}) == ok);
    assert(card.report().total_ms == report.total_ms + 2000);
    assert(card.store.begin_statistics_writer(card.writer) == ok);
    assert(card.store.commit_statistics(card.writer, source, {{1, 256, 6000}}) == ok);
    assert(card.report().total_ms == report.total_ms + 8000);
    assert(card.store.commit_statistics(card.writer - 1, source, rows) == StoreStatus::ExternalChange);
    StatisticsReport untouched;
    untouched.total_ms = 1;
    assert(card.store.query_statistics({}, untouched, []{ return true; }) == StoreStatus::Cancelled && untouched.total_ms == 1);
    assert(card.store.query_statistics({0, 372}, untouched) == StoreStatus::Invalid);
    assert(card.report({0, -1, -1, 0, 0, 6}).books[0].selected_day_ms == 12345678);
}

void file_identity_and_clear() {
    Card card;
    auto original = card.original;
    assert(card.store.commit_statistics(card.writer, original, {{1, 20, 7000}}) == ok);
    const auto initial_id = card.id(original);
    auto moved = original;
    moved.path = "Books/moved.txt";
    fs::rename(card.root / original.path, card.root / moved.path);
    fs::rename(card.root / (original.path + ".picopage"), card.root / (moved.path + ".picopage"));
    assert(card.id(moved) == initial_id);
    assert(card.store.commit_statistics(card.writer, moved, {{2, 21, 8000}}) == ok);
    auto copy = moved;
    copy.path = "Books/copy.txt";
    fs::copy_file(card.root / moved.path, card.root / copy.path);
    fs::copy(card.root / (moved.path + ".picopage"), card.root / (copy.path + ".picopage"), fs::copy_options::recursive);
    assert(card.store.commit_statistics(card.writer, copy, {{3, 20, 9000}}) == ok);
    const auto copy_id = card.id(copy);
    assert(copy_id != initial_id);
    auto report = card.report({20, 21, 20, 0, 1, 1});
    assert(report.days == 2 && report.matching_books == 2 && report.books[0].id == copy_id &&
           report.day_ms[0] == 16000 && report.books[0].selected_day_ms == 9000);
    auto replacement = moved;
    replacement.content_id = "second-sha";
    std::ofstream(card.root / replacement.path) << "two";
    assert(card.store.commit_statistics(card.writer, replacement, {{4, 18, 5000}}) == ok);
    assert(card.id(replacement) != initial_id);
    assert(card.report().book_count == 3);
    fs::remove(card.root / copy.path);
    assert(card.report({0, -1, -2, copy_id, 0, 1}).books[0].progress_percent == -1);
    assert(card.store.clear_cache() == ok);
    assert(card.report().total_ms == 29000);
    assert(card.store.clear_statistics(initial_id) == ok);
    assert(card.report().total_ms == 14000 && card.report().days == 2);
    fs::remove(card.root / "PicoPage/data/statistics/books/1/summary.0");
    assert(card.report().total_ms == 14000);
    assert(card.store.clear_statistics(0) == ok);
    fs::remove(card.root / "PicoPage/data/statistics/catalog.0");
    assert(card.report().total_ms == 0);
    assert(card.store.commit_statistics(card.writer, replacement, {{5, 22, 5000}}) == StoreStatus::ExternalChange);
    assert(card.store.begin_statistics_writer(card.writer) == ok);
    assert(card.store.commit_statistics(card.writer, replacement, {{1, 30, 11000}}) == ok);
    assert(card.id(replacement) != initial_id && card.report().total_ms == 11000);
    assert(card.store.clear_statistics(card.id(replacement)) == ok);
    assert(card.store.commit_statistics(card.writer, replacement, {{2, 22, 5000}}) == StoreStatus::ExternalChange);
    assert(card.store.begin_statistics_writer(card.writer) == ok);
    assert(card.store.commit_statistics(card.writer, replacement, {{1, 22, 12000}}) == ok);
    assert(card.report().total_ms == 12000);
    assert(card.report({30, 30}).day_ms[0] == 0);
}

void single_book_clear_with_unacknowledged_records() {
    Card card;
    auto other = card.original;
    other.path = "Books/other.txt";
    std::ofstream(card.root / other.path) << "two";
    assert(card.store.commit_statistics(card.writer, card.original, {{1, 101, 9000}}) == ok);
    const std::vector<StatisticsWrite> unacknowledged{{2, 97, 10000}};
    assert(card.store.commit_statistics(card.writer, other, unacknowledged) == ok);
    fs::remove(card.root / card.original.path);
    const auto original_id = card.id(card.original);
    assert(card.store.clear_statistics(original_id, 3) == ok);
    assert(card.store.commit_statistics(card.writer, other, unacknowledged) == ok);
    assert(card.store.commit_statistics(card.writer, card.original, {{3, 101, 7000}, {4, 102, 6000}}) == ok);
    auto report = card.report({97,102});
    assert(report.total_ms == 16000 && report.day_ms[4] == 0 && report.day_ms[5] == 6000);
    assert(card.store.clear_statistics(0) == ok);
    fs::remove(card.root / "PicoPage/data/statistics/catalog.0");
    fs::remove(card.root / "PicoPage/data/statistics/catalog.1");
    assert(card.store.query_statistics({}, report) == StoreStatus::Invalid);
}

void interrupted_registration() {
    Card card;
    const auto area = card.root / "PicoPage/data/statistics";
    fs::create_directory(area / "catalog.0.tmp");
    assert(card.store.commit_statistics(card.writer, card.original, {{1, 22, 8000}}) != ok);
    assert(card.report().book_count == 0);
    assert(!fs::exists(card.root / (card.original.path + ".picopage")));
    fs::remove(area / "catalog.0.tmp");
    auto copy = card.original;
    copy.path = "Books/copy.txt";
    std::ofstream(card.root / copy.path) << "one";
    assert(card.store.commit_statistics(card.writer, copy, {{2, 21, 7000}}) == ok);
    const auto copy_id = card.id(copy);
    fs::remove(card.root / copy.path);
    assert(card.store.commit_statistics(card.writer, copy, {{3, 21, 6000}}) == ok);
    auto second = card.original;
    second.content_id = "replacement";
    fs::create_directories(card.root / (second.path + ".picopage/statistics-id.0.tmp"));
    assert(card.store.commit_statistics(card.writer, second, {{4, 22, 9000}}) != ok);
    fs::remove(card.root / (second.path + ".picopage/statistics-id.0.tmp"));
    assert(card.store.commit_statistics(card.writer, second, {{4, 22, 9000}}) == ok);
    const auto second_id = card.id(second);
    assert(second_id != copy_id && static_cast<std::uint32_t>(second_id) == 2);
    assert(!fs::exists(area / "books/3"));
    assert(card.report().total_ms == 22000);
    auto deleted = card.original;
    deleted.path = "Books/deleted-before-flush.txt";
    assert(card.store.commit_statistics(card.writer, deleted, {{5, 21, 6500}}) == ok);
    assert(card.report().book_count == 3 && card.report().total_ms == 28500);
}

std::string fixture_bytes(std::string_view hex) {
    assert(hex.size() % 2 == 0);
    std::string bytes;
    while (!hex.empty()) {
        unsigned value = 0;
        const auto next = hex.data() + 2;
        const auto parsed = std::from_chars(hex.data(), next, value, 16);
        assert(parsed.ec == std::errc{} && parsed.ptr == next);
        bytes.push_back(static_cast<char>(value));
        hex.remove_prefix(2);
    }
    return bytes;
}

void load_ppst1_card(const Card& card) {
    const auto books = card.root / "PicoPage/data/statistics";
    const auto directory = books / "books/1";
    const auto binding = card.root / (card.original.path + ".picopage");
    fs::create_directories(directory);
    fs::create_directories(binding);
    const auto put = [](const fs::path& path, const std::string& data) {
        std::ofstream out(path, std::ios::binary);
        out.write(data.data(), static_cast<std::streamsize>(data.size()));
        assert(out.good());
    };
    put(books / "catalog.0", fixture_bytes(
        "505053543143030000000000000001000000000000000000000000000000010000000100746c4abb47f3c4df"));
    put(books / "catalog.1", fixture_bytes(
        "5050535431430400000000000000010000000000000000000000000000000100000001007b5e306631a02c02"));
    put(binding / "statistics-id.0", fixture_bytes(
        "50505354314901000000000000000100000001000000090066697273742d736861f94863ac4dd8f28a"));
    put(binding / "statistics-id.1", fixture_bytes(
        "50505354314902000000000000000100000001000000090066697273742d736861cc1a88b43a30ac49"));
    put(directory / "summary.0", fixture_bytes(
        "5050535431420300000000000000010000000100000001000000000000000300000000000000000000000000"
        "0000f8a7000000000000a00f0000000000000000000000000000180f00426f6f6b732f73746f72792e747874"
        "050053746f7279090066697273742d73686106007478742d7631010000010100000000000000000000000000"
        "00000000000000000000000000000000000000000000000000c0443061839ff921f7"));
    put(directory / "summary.1", fixture_bytes(
        "5050535431420200000000000000010000000100000000000000000000000000000000000000000000000000"
        "0000000000000000000000000000000000000000000000000000180f00426f6f6b732f73746f72792e747874"
        "050053746f7279090066697273742d73686106007478742d763100002894dcdde3744624"));
    put(directory / "d0.1", fixture_bytes("50505354314401000000000000000100000001") + std::string(2036, '\0') +
        fixture_bytes("e02e00000000000078690000000000000be605f3d0f339dd"));
}

auto day_segment_files(const fs::path& directory) {
    std::map<std::string, std::pair<std::string, fs::file_time_type>> files;
    for (const auto& entry : fs::directory_iterator(directory)) {
        const auto name = entry.path().filename().string();
        if (name.front() != 'd') continue;
        std::ifstream in(entry.path(), std::ios::binary);
        assert(in.good());
        std::string contents{std::istreambuf_iterator<char>(in), {}};
        files.emplace(name, std::make_pair(std::move(contents), entry.last_write_time()));
    }
    return files;
}

void legacy_and_active_day() {
    Card card;
    load_ppst1_card(card);
    const auto directory = card.root / "PicoPage/data/statistics/books/1";
    const auto& source = card.original;
    const auto id = card.id(source);
    auto report = card.report({254, 255, 255, id, 0, 1});
    assert(report.total_ms == 43000 && report.unknown_ms == 4000 && report.days == 2);
    assert((report.day_ms == std::vector<std::uint64_t>{12000, 27000}));
    assert(report.books.size() == 1 && report.books.front().selected_day_ms == 27000);
    fs::last_write_time(directory / "d0.1", fs::file_time_type::clock::now() - std::chrono::hours(72));
    const auto before = day_segment_files(directory);
    assert(before.size() == 1);
    assert(card.store.commit_statistics(card.writer, source, {{4, 255, 3000}}) == ok);
    assert(FileStore(card.root.string()).commit_statistics(card.writer, source, {{5, -1, 6000}}) == ok);
    const std::vector<StatisticsWrite> same_day{{6, 255, 7000}};
    assert(FileStore(card.root.string()).commit_statistics(card.writer, source, same_day) == ok);
    assert(FileStore(card.root.string()).commit_statistics(card.writer, source, same_day) == ok);
    report = card.report({254, 255, 255, id, 0, 1});
    assert(report.total_ms == 59000 && report.unknown_ms == 10000 && report.days == 2);
    assert(report.first_day == 254 && report.last_day == 255 && report.books.front().days == 2);
    assert((report.day_ms == std::vector<std::uint64_t>{12000, 37000}));
    assert(report.books.front().selected_day_ms == 37000);
    assert(day_segment_files(directory) == before);

    assert(card.store.commit_statistics(card.writer, source, {{7, 256, 5000}}) == ok);
    const std::vector<StatisticsWrite> jumps{{8, 254, 1000}, {9, 256, 2000}, {10, 255, 4000},
                                             {11, 257, 9000}, {12, 254, 6000}, {13, -1, 2000}};
    assert(card.store.commit_statistics(card.writer, source, jumps) == ok);
    assert(FileStore(card.root.string()).commit_statistics(card.writer, source, jumps) == ok);
    auto other = source;
    other.path = "Books/other.txt";
    std::ofstream(card.root / other.path) << "two";
    assert(card.store.commit_statistics(card.writer, other,
        {{14, 255, 11000}, {15, 257, 6000}, {16, 258, 3000}, {17, -1, 2000}}) == ok);
    report = card.report({254, 258, 254, id, 0, 1});
    assert((report.day_ms == std::vector<std::uint64_t>{19000, 41000, 7000, 9000, 0}));
    assert(report.first_day == 254 && report.last_day == 257 && report.days == 4);
    assert(report.total_ms == 88000 && report.unknown_ms == 12000 && report.books.front().selected_day_ms == 19000);
    report = card.report({254, 258, -1, 0, 0, 7});
    assert(report.days == 5 && report.first_day == 254 && report.last_day == 258 && report.book_count == 2);
    assert(report.total_ms == 110000 && report.unknown_ms == 14000 && report.matching_books == 2);
    assert((report.day_ms == std::vector<std::uint64_t>{19000, 52000, 7000, 15000, 3000}));
    assert(report.books[0].selected_day_ms == 12000 && report.books[1].selected_day_ms == 2000);
    assert(card.store.clear_statistics(id) == ok);
    report = card.report({254, 258, 254, 0, 0, 7});
    assert(report.total_ms == 22000 && report.book_count == 1 && report.days == 3 && report.unknown_ms == 2000);
    assert(report.matching_books == 0 && report.books.empty());
    assert((report.day_ms == std::vector<std::uint64_t>{0, 11000, 0, 6000, 3000}));
}

void interrupted_batch() {
    Card card;
    auto source = card.original;
    assert(card.store.commit_statistics(card.writer, source, {{1, 20, 10000}}) == ok);
    auto directory = card.root / "PicoPage/data/statistics/books/1";
    auto failing_slot = directory / "d1.1.tmp";
    fs::create_directory(failing_slot);
    const std::vector<StatisticsWrite> records{{2, 21, 10000}, {3, 257, 9500}, {4, 520, 6500}};
    assert(card.store.commit_statistics(card.writer, source, records) != ok);
    auto report = card.report({20, 257});
    assert(report.total_ms == 10000 && report.day_ms[1] == 0 && report.day_ms.back() == 0);
    fs::remove(failing_slot);
    for (const auto* slot : {"summary.0.tmp", "summary.1.tmp"}) fs::create_directory(directory / slot);
    assert(FileStore(card.root.string()).commit_statistics(card.writer, source, records) != ok);
    const auto unpublished = card.report({520, 520});
    assert(unpublished.total_ms == 10000 && unpublished.day_ms.front() == 0);
    for (const auto* slot : {"summary.0.tmp", "summary.1.tmp"}) fs::remove(directory / slot);
    FileStore reopened(card.root.string());
    assert(reopened.commit_statistics(card.writer, source, records) == ok);
    assert(reopened.commit_statistics(card.writer, source, records) == ok);
    report = card.report({20, 257});
    assert(report.total_ms == 36000 && report.day_ms[1] == 10000 && report.day_ms.back() == 9500);
    assert(card.report({520, 520}).day_ms.front() == 6500);
    std::ofstream(directory / "summary.0", std::ios::trunc) << "torn";
    assert(card.report().total_ms == 36000);
    std::ofstream(directory / "summary.1", std::ios::trunc) << "bad";
    assert(reopened.query_statistics({}, report) == StoreStatus::Invalid);
}

}

int main() {
    global_settings();
    totals_and_replay();
    file_identity_and_clear();
    single_book_clear_with_unacknowledged_records();
    interrupted_registration();
    interrupted_batch();
    legacy_and_active_day();
}
