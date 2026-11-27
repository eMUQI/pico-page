#include "pico_page/app.h"
#include "pico_page/reading_statistics.h"
#include "pico_page/store.h"
#include <atomic>
#include <chrono>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unistd.h>

namespace {
using namespace pico_page;
using Clock = std::chrono::steady_clock;
namespace fs = std::filesystem;

std::atomic<unsigned> sync_calls{0}, publish_calls{0}, statistics_publishes{0};
unsigned turn_sync_calls = 0;
thread_local bool measuring_input = false;
constexpr unsigned dwell_ms = 8000;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

struct TestCard {
    fs::path root;
    TestCard() {
        auto pattern = (fs::temp_directory_path() / "pico-statistics-bench-XXXXXX").string();
        require(mkdtemp(pattern.data()) != nullptr, "Cannot create test card");
        root = pattern;
    }
    ~TestCard() {
        std::error_code error;
        fs::remove_all(root, error);
    }
};

void stage(const char* name, Clock::time_point begin) {
    const auto us = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - begin).count();
    std::cout << "reader_perf: stage=" << name << " us=" << us << " min_us=0\n";
}

std::uint64_t caller_cpu_us() {
    timespec cpu{};
    require(clock_gettime(CLOCK_THREAD_CPUTIME_ID, &cpu) == 0, "Caller CPU clock unavailable");
    return static_cast<std::uint64_t>(cpu.tv_sec) * 1000000 + cpu.tv_nsec / 1000;
}

void wait_until(Clock::time_point deadline) {
    require(Clock::now() < deadline, "Timed out waiting for the application");
    std::this_thread::sleep_for(std::chrono::microseconds(100));
}

void present(ReaderApp& app) {
    const auto deadline = Clock::now() + std::chrono::seconds(20);
    while (app.pending_frame()) {
        while (!app.prepare_frame()) wait_until(deadline);
        app.complete_frame(true);
    }
}

Anchor present_body(ReaderApp& app) {
    const auto deadline = Clock::now() + std::chrono::seconds(20);
    for (;;) {
        if (app.pending_frame()) {
            while (!app.prepare_frame()) wait_until(deadline);
            if (app.pending_frame()->purpose == FramePurpose::Body) {
                auto anchor = app.pending_frame()->anchor;
                app.complete_frame(true);
                require(app.view() == View::Reading, "Body presented outside Reading");
                return anchor;
            }
            app.complete_frame(true);
        } else {
            require(app.view() != View::Error, app.status());
            app.poll_background();
            if (!app.pending_frame()) wait_until(deadline);
        }
    }
}

void idle(ReaderApp& app, const Anchor& anchor, bool prefetch) {
    const auto deadline = Clock::now() + std::chrono::seconds(20);
    present(app);
    app.poll_background();
    if (app.save_due()) app.save_progress();
    while (app.save_busy()) {
        app.poll_background();
        present(app);
        if (app.save_busy()) wait_until(deadline);
    }
    require(app.saved_record() && app.saved_record()->anchor == anchor, "Progress was not saved");
    for (int step = 0; step < 8; ++step) {
        app.poll_background();
        present(app);
        if (prefetch && app.background_due()) app.background_step();
    }
}

void sleep(ReaderApp& app) {
    const auto deadline = Clock::now() + std::chrono::seconds(20);
    require(app.begin_sleep(), "Sleep request rejected");
    present(app);
    while (!app.prepare_sleep()) {
        require(app.sleep_pending(), "Sleep preparation failed: " + app.status());
        app.poll_background();
        present(app);
        wait_until(deadline);
    }
    require(app.pending_frame() && app.view() == View::Sleeping, "No sleep frame");
    present(app);
}
}

extern "C" int __real_fsync(int);
extern "C" int __wrap_fsync(int fd) {
    const int result = __real_fsync(fd);
    if (!result) {
        ++sync_calls;
        if (measuring_input) ++turn_sync_calls;
    }
    return result;
}
extern "C" int __real_rename(const char*, const char*);
extern "C" int __wrap_rename(const char* source, const char* target) {
    const int result = __real_rename(source, target);
    if (!result) {
        ++publish_calls;
        if (std::strstr(target, "/PicoPage/data/statistics/")) ++statistics_publishes;
    }
    return result;
}

int main(int argc, char** argv) try {
    require(argc == 4 || argc == 5, "Usage: reading_statistics_bench SYNTHETIC_BOOK BODY_FONT on|off [turns]");
    const std::string mode = argv[3];
    require(mode == "on" || mode == "off", "Expected on or off");
    const auto turns = argc == 5 ? std::stoi(argv[4]) : 64;
    require(turns >= 2 && turns <= 1000 && turns % 2 == 0, "Turn count must be even and in [2,1000]");
    TestCard card;
    fs::create_directories(card.root / "PicoPage");
    fs::create_directories(card.root / "Fonts");
    fs::create_directories(card.root / "Books");
    const auto book = "Books/fixture" + fs::path(argv[1]).extension().string();
    fs::copy_file(argv[1], card.root / book);
    fs::copy_file(argv[2], card.root / "Fonts/LXGWWenKai-Regular.ttf");
    std::ofstream(card.root / "PicoPage/settings.toml")
        << "schema_version = 1\n[onboarding]\ndone = true\n[reader]\nfont = \"Fonts/LXGWWenKai-Regular.ttf\"\n"
           "[power]\nsleep_screen = \"simple\"\n";
    FileStore store(card.root.string());
    const bool enabled = mode == "on";
    require(store.save_statistics_options({enabled, 5, 120}) == StoreStatus::Ok, "Could not set statistics options");
    sync_calls = 0; publish_calls = 0; statistics_publishes = 0;
    std::uint64_t dwell_clock = 10000;
    ReaderApp app(card.root.string(), "statistics-bench", true, true);
    app.set_statistics_clock_for_test([&] {
        return StatisticsClock{dwell_clock, 1760000000 + static_cast<std::int64_t>(dwell_clock / 1000)};
    });
    require(app.initialize(), "Could not initialize app");
    present(app);
    auto begin = Clock::now();
    require(app.open_book(book), "Could not open synthetic book");
    auto anchor = present_body(app);
    stage("host_open", begin);
    for (int turn = 0; turn < turns; ++turn) {
        dwell_clock += dwell_ms;
        idle(app, anchor, turn % 8 >= 4);
        const auto hits = app.cache_usage().bitmap_hits;
        const auto key = turn < turns / 2 ? Input::K3 : Input::K1;
        const std::uint64_t sequence = turn + 1;
        measuring_input = true;
        const auto cpu_start = caller_cpu_us();
        begin = Clock::now();
        app.input({{sequence, Phase::Begin, key}});
        app.input({{sequence, Phase::End, key}});
        auto next = present_body(app);
        const auto cpu_us = caller_cpu_us() - cpu_start;
        stage("host_turn", begin);
        std::cout << "reader_perf: stage=host_turn_cpu us=" << cpu_us << " min_us=0\n";
        measuring_input = false;
        require(next != anchor, "Page did not advance");
        std::cout << "turn=" << turn << " hit=" << (app.cache_usage().bitmap_hits != hits)
                  << " anchor=" << next.resource << '/' << next.node << '/' << next.offset << '\n';
        anchor = std::move(next);
    }
    dwell_clock += dwell_ms;
    const auto before_sleep = sync_calls.load();
    begin = Clock::now();
    sleep(app);
    stage("host_sleep", begin);
    require(app.saved_record() && app.saved_record()->anchor == anchor, "Sleep lost reading position");
    std::cout << "fsync_ok=" << sync_calls << " sleep_fsync_ok=" << sync_calls - before_sleep
              << " input_thread_fsync_ok=" << turn_sync_calls << " rename_ok=" << publish_calls
              << " statistics_rename_ok=" << statistics_publishes << '\n';
    std::ifstream status("/proc/self/status");
    std::string field;
    while (status >> field && field != "VmHWM:") std::getline(status, field);
    unsigned long rss_kib = 0;
    require(field == "VmHWM:" && (status >> rss_kib) && rss_kib != 0, "Cannot inspect peak RSS");
    std::cout << "maxrss_kib=" << rss_kib << '\n';
    StatisticsReport report;
    require(store.query_statistics({}, report) == StoreStatus::Ok, "Could not verify statistics");
    const auto expected_ms = enabled ? (static_cast<unsigned>(turns) + 1) * dwell_ms : 0;
    std::cout << "persisted_ms=" << report.total_ms << " expected_ms=" << expected_ms << '\n';
    require(report.total_ms == expected_ms, "Persisted duration does not match visible pages");
    if (enabled) {
        const auto& record = *app.saved_record();
        const StatisticsBookSource source{book, record.book_id, "fixture", -1, record.content_revision};
        std::uint64_t writer = 0;
        require(store.begin_statistics_writer(writer) == StoreStatus::Ok, "Cannot start commit measurement");
        for (unsigned sample = 1; sample <= 16; ++sample) {
            const std::vector<StatisticsWrite> batch{{sample, 20000, 45000}};
            begin = Clock::now();
            const auto status = store.commit_statistics(writer, source, batch);
            stage("host_store_commit", begin);
            require(status == StoreStatus::Ok, "Statistics commit failed");
        }
        StatisticsReport after;
        require(store.query_statistics({}, after) == StoreStatus::Ok && after.book_count == 1 &&
                after.total_ms == expected_ms + 16 * 45000, "Timed commits did not persist");
    }
    return 0;
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
}
