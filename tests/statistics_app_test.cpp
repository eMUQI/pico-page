#include "../firmware/components/reader_app/content_job.h"
#include "pico_page/app.h"
#include "pico_page/document.h"
#include "pico_page/reading_statistics.h"
#include "pico_page/store.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <thread>
#include <vector>
#include <unistd.h>

using namespace pico_page;
namespace fs = std::filesystem;

namespace {

constexpr std::int32_t november_24 = 20781;
constexpr std::int64_t november_24_noon = 1795492800;

struct Clock {
    std::uint64_t milliseconds = 0;
    std::optional<std::int64_t> origin = november_24_noon;

    StatisticsClock read() const {
        return {milliseconds, origin ? std::optional<std::int64_t>(*origin + milliseconds / 1000)
                                     : std::nullopt};
    }
};

void advance_worker(ReaderApp& app) {
    app.poll_background();
    if (app.pending_frame() && app.prepare_frame()) app.complete_frame(true);
    if (app.save_due() && !app.save_busy()) app.save_progress();
}

template <typename Predicate>
void wait_for(ReaderApp& app, Predicate done) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (!done()) {
        advance_worker(app);
        assert(std::chrono::steady_clock::now() < until);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

struct Device {
    Clock clock;
    ReaderApp app;
    std::uint64_t sequence = 0;

    Device(const fs::path& root, bool async) : app(root.string(), "statistics-test", true, async) {
        app.set_statistics_clock_for_test([this] { return clock.read(); });
        assert(app.initialize());
        settle();
        assert(app.view() == View::Library);
    }

    void settle() {
        wait_for(app, [&] { return !app.pending_frame() && !app.content_busy() && !app.save_busy(); });
    }

    void press(Input key, int x = 0, int y = 0, bool hold = false) {
        const bool touch = key == Input::TouchCenter;
        ++sequence;
        app.input({{sequence, Phase::Begin, key}, touch, x, y, false, hold});
        app.input({{sequence, Phase::End, key}, touch, x, y, false, hold});
    }

    void tap(int x, int y) {
        press(Input::TouchCenter, x, y);
        settle();
    }

    void key(Input input) {
        press(input);
        settle();
    }

    void check_full_refresh() {
        const auto pixels = app.canvas().pixels();
        press(Input::K2);
        assert(app.pending_frame() && app.pending_frame()->refresh == RefreshIntent::FullClean);
        settle();
        assert(app.view() == View::Statistics && app.canvas().pixels() == pixels);
    }

    void handoff() {
        std::string error;
        wait_for(app, [&] { return app.prepare_usb_transfer(&error); });
    }

    void open_statistics() {
        tap(300, 50);
        assert(app.view() == View::Statistics);
    }

    void capture(const fs::path& shots, const std::string& name) {
        if (shots.empty()) return;
        fs::create_directories(shots);
        std::ofstream output(shots / (name + ".pgm"), std::ios::binary);
        output << "P5\n684 1216\n255\n";
        for (int y = 0; y < 1216; ++y)
            for (int x = 0; x < 684; ++x) output.put(static_cast<char>(app.canvas().pixel(x, y) * 17));
        assert(output.good());
    }
};

void prepare_card(const fs::path& root, const fs::path& font) {
    fs::create_directories(root / "Books");
    fs::create_directories(root / "Fonts");
    fs::create_directories(root / "PicoPage");
    fs::copy_file(font, root / "Fonts/test.ttf");
    std::ofstream config(root / "PicoPage/settings.toml");
    config << "schema_version=1\n[onboarding]\ndone=true\n[reader]\nfont='Fonts/test.ttf'\n";
    assert(config.good());
}

StatisticsReport report(const fs::path& root) {
    StatisticsReport result;
    StatisticsQuery query;
    query.from_day = november_24;
    query.to_day = november_24;
    query.limit = 8;
    assert(FileStore(root.string()).query_statistics(query, result) == StoreStatus::Ok);
    return result;
}

std::vector<std::uint8_t> acquisition(const fs::path& root, const fs::path& fixtures, const fs::path& font,
                 const std::string& book, bool async) {
    prepare_card(root, font);
    const std::string path = "Books/" + book;
    fs::copy_file(fixtures / book, root / path);
    std::vector<std::uint8_t> usb_pixels;
    {
        Device device(root, async);
        assert(device.app.open_book(path));
        device.settle();
        assert(device.app.view() == View::Reading);
        device.clock.milliseconds += 40000;
        device.tap(342, 500);
        assert(device.app.view() == View::Tools);
        device.clock.milliseconds += 180000;
        device.tap(342, 500);
        assert(device.app.view() == View::Reading);
        device.clock.milliseconds += 30000;
        device.press(Input::K2);
        assert(device.app.pending_frame() && device.app.pending_frame()->refresh == RefreshIntent::FullClean);
        device.clock.milliseconds += 20000;
        device.settle();
        device.handoff();
        device.app.show_usb_transfer(false, "USB 启动失败");
        device.settle();
        assert(device.app.view() == View::UsbTransfer && device.app.status() == "USB 启动失败");
        usb_pixels = device.app.canvas().pixels();
    }
    const auto value = report(root);
    assert(value.total_ms == 70000 && value.book_count == 1 && value.days == 1);
    assert(value.day_ms.size() == 1 && value.day_ms.front() == value.total_ms);
    return usb_pixels;
}

void immediate_reopen_and_idle_commit(const fs::path& root, const fs::path& fixtures, const fs::path& font) {
    prepare_card(root, font);
    const std::string path = "Books/sample.txt";
    fs::copy_file(fixtures / "sample.txt", root / path);
    Device device(root, true);
    for (unsigned visit = 0; visit < 2; ++visit) {
        assert(device.app.open_book(path));
        device.settle();
        assert(device.app.view() == View::Reading && report(root).total_ms == 0);
        device.clock.milliseconds += 6000;
        device.tap(342, 500);
        device.press(Input::TouchCenter, 50, 52);
        device.clock.milliseconds += ReaderApp::background_idle_ms;
        device.settle();
        assert(device.app.view() == View::Library);
        device.app.background_step({});
        wait_for(device.app, [&] { return !device.app.background_due(); });
        assert(report(root).total_ms == 0);
    }
    device.clock.milliseconds += ReaderApp::background_idle_ms - 1;
    device.app.poll_background();
    assert(!device.app.background_due());
    ++device.clock.milliseconds;
    device.app.poll_background();
    assert(device.app.background_due());
    wait_for(device.app, [&] {
        if (!device.app.background_due()) return true;
        device.app.background_step({});
        return false;
    });
    assert(report(root).total_ms == 12000);
}

void cancellable_sleep_commit(const fs::path& root, const fs::path& fixtures, const fs::path& font) {
    prepare_card(root, font);
    fs::copy_file(fixtures / "valid.epub", root / "Books/sample.epub");
    Device device(root, true);
    assert(device.app.open_book("Books/sample.epub"));
    device.settle();
    device.clock.milliseconds += 12000;
    ContentJob::delay_starts_for_test(ContentJob::Kind::ReadingStatistics, std::chrono::milliseconds(350));
    assert(device.app.begin_sleep());
    device.settle();
    assert(!device.app.prepare_sleep() && device.app.sleep_pending());
    assert(!device.app.set_diagnostic_transfer(true));
    device.app.cancel_sleep();
    device.settle();
    assert(device.app.view() == View::Reading);
    device.clock.milliseconds += 6000;
    ContentJob::delay_starts_for_test(ContentJob::Kind::ReadingStatistics, {});
    device.handoff();
    assert(report(root).total_ms == 18000);
}

void force_power_without_statistics(const fs::path& root, const fs::path& fixtures, const fs::path& font) {
    prepare_card(root, font);
    fs::copy_file(fixtures / "valid.epub", root / "Books/sample.epub");
    Device device(root, true);
    assert(device.app.open_book("Books/sample.epub"));
    device.settle();
    const auto last_book = root / "PicoPage/data/last-book.txt";
    fs::remove(last_book);
    fs::create_directory(last_book);
    device.clock.milliseconds += 12000;
    assert(device.app.show_power_menu());
    device.settle();
    device.tap(300, 520);
    const auto first = device.app.take_power_request();
    assert(first.action == PowerAction::Shutdown && !first.without_save);
    assert(!device.app.prepare_power(first));
    device.settle();
    device.tap(500, 1100);
    const auto forced = device.app.take_power_request();
    assert(forced.action == PowerAction::Shutdown && forced.without_save);
    assert(device.app.prepare_power(forced));
    device.settle();
    assert(device.app.view() == View::PowerOff);
    assert(!fs::exists(root / "PicoPage/data/statistics"));
}

void usb_handoff_write_failure(const fs::path& root, const fs::path& fixtures, const fs::path& font,
                               const std::vector<std::uint8_t>& healthy_pixels) {
    prepare_card(root, font);
    fs::copy_file(fixtures / "valid.epub", root / "Books/sample.epub");
    Device device(root, true);
    assert(device.app.open_book("Books/sample.epub"));
    device.settle();
    const auto statistics = root / "PicoPage/data/statistics";
    fs::create_directories(statistics.parent_path());
    std::ofstream(statistics) << "fixture";
    device.clock.milliseconds += 21000;
    device.handoff();
    device.app.show_usb_transfer(false, "USB 启动失败");
    device.settle();
    assert(device.app.view() == View::UsbTransfer);
    assert(device.app.status() == "USB 启动失败");
    const auto& pixels = device.app.canvas().pixels();
    const auto stride = device.app.canvas().stride();
    auto previous_line_end = healthy_pixels.begin() + 800 * stride;
    while (previous_line_end > healthy_pixels.begin() && *(previous_line_end - 1) == 0xff) --previous_line_end;
    assert(previous_line_end > healthy_pixels.begin() + 310 * stride);
    assert(std::equal(healthy_pixels.begin(), previous_line_end, pixels.begin()));
    const auto after_error = pixels.begin() + (previous_line_end - healthy_pixels.begin());
    assert(std::any_of(after_error, pixels.begin() + 800 * stride, [](std::uint8_t value) { return value != 0xff; }));
}

void wifi_transfer_and_resume(const fs::path& root, const fs::path& fixtures, const fs::path& font) {
    prepare_card(root, font);
    fs::copy_file(fixtures / "valid.epub", root / "Books/sample.epub");
    Device device(root, true);
    assert(device.app.open_book("Books/sample.epub"));
    device.settle();
    const auto before = device.app.saved_record();
    assert(before);
    device.clock.milliseconds += 90000;
    device.tap(342, 500);
    device.tap(50, 52);
    assert(device.app.view() == View::Library);
    device.tap(468, 52);
    assert(device.app.view() == View::Import);
    device.tap(342, 660);
    assert(device.app.take_portal_action() == PortalAction::Upload);
    assert(device.app.prepare_portal());
    device.settle();
    assert(device.app.view() == View::Portal);
    PortalStatus portal{PortalPhase::Waiting, "PicoPage-TEST", "abcd2345", "http://192.168.4.1", {}, {},
                        0, {}, device.app.portal_generation()};
    device.app.show_portal(portal);
    device.settle();
    device.clock.milliseconds += 600000;
    device.tap(342, 1120);
    assert(device.app.take_portal_action() == PortalAction::Stop);
    portal.phase = PortalPhase::Closed;
    device.app.show_portal(portal);
    device.settle();
    assert(device.app.view() == View::Library);
    device.tap(342, 224);
    assert(device.app.view() == View::Reading && device.app.saved_record() == before);
    device.clock.milliseconds += 70000;
    device.handoff();
    assert(report(root).total_ms == 120000);
}

void reflow_and_jump(const fs::path& root, const fs::path& fixtures, const fs::path& font) {
    prepare_card(root, font);
    fs::copy_file(fixtures / "valid.epub", root / "Books/sample.epub");
    std::ofstream(root / "PicoPage/sync.toml")
        << "schema_version=1\ndom_version=20260812\nendpoint='https://example.invalid'\n"
           "username='reader'\nuserkey='fixture'\ndevice_id='host'\n";
    std::ofstream(root / "PicoPage/network.toml")
        << "schema_version=2\n[[wifi]]\nssid='example'\npassword='example-value'\n";
    Device device(root, true);
    assert(device.app.open_book("Books/sample.epub"));
    device.settle();
    assert(device.app.saved_record());
    const auto initial = device.app.saved_record()->anchor;
    device.clock.milliseconds += 95000;
    device.tap(342, 500);
    device.tap(205, 1140);
    assert(device.app.view() == View::Typography);
    device.clock.milliseconds += 240000;
    device.tap(580, 1020);
    device.tap(500, 1130);
    assert(device.app.view() == View::Reading && device.app.saved_record()->anchor == initial);
    device.clock.milliseconds += 50000;
    device.tap(342, 500);
    device.tap(342, 1140);
    assert(device.app.view() == View::Sync);
    device.clock.milliseconds += 180000;
    device.tap(504, 1164);
    const auto request = device.app.take_network_request();
    assert(request && request->request.method == "GET");
    device.app.complete_network(request->id, 200,
        R"({"progress":"/body[1]/DocFragment[2]/body[1]/h1[1]/text()[1].0","percentage":0.9})", "");
    device.settle();
    assert(std::string(device.app.sync_state()) == "different");
    device.tap(500, 850);
    assert(device.app.view() == View::Reading && !(device.app.saved_record()->anchor == initial));
    device.clock.milliseconds += 11000;
    assert(device.app.begin_sleep());
    device.settle();
    wait_for(device.app, [&] { return device.app.prepare_sleep(); });
    device.settle();
    assert(device.app.view() == View::Sleeping);
    assert(report(root).total_ms == 131000);
    device.clock.milliseconds += 600000;
    device.app.wake();
    device.settle();
    assert(device.app.view() == View::Reading);
    device.clock.milliseconds += 9000;
    assert(device.app.begin_sleep());
    device.settle();
    device.clock.milliseconds += 35000;
    device.app.cancel_sleep();
    device.settle();
    assert(device.app.view() == View::Reading);
    device.clock.milliseconds += 6000;
    device.handoff();
    assert(report(root).total_ms == 146000);
}

void presentation_failure(const fs::path& root, const fs::path& fixtures, const fs::path& font) {
    prepare_card(root, font);
    fs::copy_file(fixtures / "long-chapter.epub", root / "Books/long.epub");
    Device device(root, true);
    assert(device.app.open_book("Books/long.epub"));
    device.settle();
    for (int i = 0; i < 3; ++i) {
        device.clock.milliseconds += 4000;
        device.key(Input::K3);
        assert(device.app.view() == View::Reading);
    }
    device.clock.milliseconds += 7000;
    device.press(Input::K3);
    assert(device.app.pending_frame() && device.app.pending_frame()->purpose == FramePurpose::Body);
    device.clock.milliseconds += 20000;
    device.app.complete_frame(false);
    device.settle();
    assert(device.app.view() == View::Error);
    device.clock.milliseconds += 600000;
    device.handoff();
    assert(report(root).total_ms == 7000);
}

void image_updates(const fs::path& root, const fs::path& fixtures, const fs::path& font) {
    prepare_card(root, font);
    fs::copy_file(fixtures / "image-flow.epub", root / "Books/images.epub");
    Device device(root, true);
    assert(device.app.open_book("Books/images.epub"));
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (!device.app.pending_frame() || device.app.pending_frame()->purpose != FramePurpose::Body) {
        if (device.app.pending_frame()) device.app.complete_frame(true);
        device.app.poll_background();
        assert(std::chrono::steady_clock::now() < until);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    device.app.complete_frame(true);
    device.clock.milliseconds += 90000;
    while (!device.app.pending_frame()) {
        device.app.poll_background();
        assert(std::chrono::steady_clock::now() < until);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    assert(device.app.pending_frame()->region && device.app.pending_frame()->changed_pixels_only);
    device.clock.milliseconds += 21000;
    device.settle();
    device.clock.milliseconds += 60000;
    device.press(Input::TouchCenter, 120, 500, true);
    device.settle();
    device.clock.milliseconds += 600000;
    device.tap(100, 50);
    device.clock.milliseconds += 15000;
    device.handoff();
    assert(report(root).total_ms == 120000);
}

void seed_history(const fs::path& root) {
    FileStore store(root.string());
    std::uint64_t writer = 0;
    assert(store.begin_statistics_writer(writer) == StoreStatus::Ok);
    const std::int32_t month_starts[] = {20454, 20485, 20513, 20544, 20574, 20605,
                                        20635, 20666, 20697, 20727, 20758};
    std::uint64_t id = 0;
    for (unsigned book_index = 0; book_index < 2; ++book_index) {
        const std::string title = book_index ? "沿河散步" : "山中来信";
        const std::string path = "Books/" + title + ".txt";
        {
            std::ofstream book(root / path);
            for (int p = 0; p < 200; ++p) book << title << "。冬天的河水静静流过石桥。\n";
            assert(book.good());
        }
        StatisticsBookSource book{path, sha256_file((root / path).string()), title, book_index ? 72 : 38, "txt-utf8-v1"};
        ReadingRecord progress{book.content_id, book.content_revision, "reader", 1, {"text", "1", 400}};
        assert(store.save_progress(path, progress).status == StoreStatus::Ok);
        for (unsigned month = 0; month < 11; ++month) {
            std::vector<StatisticsWrite> records;
            for (unsigned week = 0; week < 4; ++week) {
                const auto day = month_starts[month] + static_cast<int>((3 * month + week * 5 + book_index * 2) % 23);
                records.push_back({++id, day, 60000u * (3 + (week + 1) * (month % 5 + 1) * 3)});
            }
            assert(store.commit_statistics(writer, book, records) == StoreStatus::Ok);
        }
        std::vector<StatisticsWrite> today;
        for (unsigned visit = 0; visit < (book_index ? 8U : 17U); ++visit)
            today.push_back({++id, november_24, 120000});
        today.push_back({++id, statistics_unknown_day, 90000});
        assert(store.commit_statistics(writer, book, today) == StoreStatus::Ok);
    }
}

void calendar_and_books(Device& device, const fs::path& shots) {
    device.open_statistics();
    device.capture(shots, "overview");
    const auto overview = device.app.canvas().pixels();
    assert(device.app.canvas().pixel(600, 441) == 1);
    device.check_full_refresh();
    device.key(Input::K3);
    assert(device.app.canvas().pixels() == overview);

    device.tap(342, 850);
    device.capture(shots, "year");
    const auto year = device.app.canvas().pixels();
    assert(year != overview);
    device.check_full_refresh();
    device.tap(342, 893);
    const auto month = device.app.canvas().pixels();
    assert(month != year);
    device.capture(shots, "month");
    device.key(Input::K1);
    assert(device.app.canvas().pixels() != month);
    device.key(Input::K3);
    assert(device.app.canvas().pixels() == month);
    device.check_full_refresh();
    device.tap(166, 758);
    device.capture(shots, "day");
    const auto day = device.app.canvas().pixels();
    assert(day != month);
    device.tap(342, 344);
    device.capture(shots, "book");
    assert(device.app.canvas().pixels() != day);
    device.tap(50, 52);
    assert(device.app.canvas().pixels() == day);
    device.tap(50, 52);
    assert(device.app.canvas().pixels() == month);
    device.tap(50, 52);
    assert(device.app.canvas().pixels() == year);
    device.tap(50, 52);
    assert(device.app.canvas().pixels() == overview);

    device.tap(342, 745);
    device.capture(shots, "unknown-date");
    assert(device.app.canvas().pixels() != day);
    device.tap(50, 52);
    device.tap(342, 960);
    device.capture(shots, "books");
    const auto list = device.app.canvas().pixels();
    device.tap(342, 284);
    assert(device.app.canvas().pixels() != list);
    device.tap(342, 1030);
    device.check_full_refresh();
    device.tap(150, 1048);
    device.tap(50, 52);
    assert(device.app.canvas().pixels() == list);
    device.tap(50, 52);
    assert(device.app.canvas().pixels() == overview);
}

void settings_and_clear(Device& device, const fs::path& root, const fs::path& shots) {
    const auto history = report(root);
    device.tap(342, 1060);
    const auto simplified = device.app.canvas().pixels();
    const auto language_settings = [&](i18n::Language language, int y) {
        device.tap(50, 52);
        device.tap(50, 52);
        assert(device.app.view() == View::Library);
        device.tap(600, 50);
        device.tap(300, 370);
        assert(device.app.view() == View::LanguageSettings);
        device.tap(300, y);
        assert(device.app.language() == language);
        device.tap(30, 50);
        device.tap(30, 50);
        device.open_statistics();
        device.tap(342, 1060);
    };
    language_settings(i18n::Language::TraditionalChinese, 340);
    assert(device.app.canvas().pixels() != simplified);
    device.capture(shots, "settings-zh-hant");
    language_settings(i18n::Language::English, 480);
    device.tap(300, 174);
    device.tap(606, 420);
    device.tap(606, 540);
    device.tap(342, 852);
    StatisticsOptions options;
    assert(FileStore(root.string()).load_statistics_options(options) == StoreStatus::Ok);
    assert(!options.enabled && options.minimum_seconds == 6 && options.maximum_seconds == 125);
    device.tap(50, 52);
    device.capture(shots, "overview-en");
    device.tap(50, 52);
    assert(device.app.view() == View::Library);
    device.press(Input::TouchCenter, 200, 251, true);
    device.settle();
    assert(device.app.view() == View::BookDetails);
    device.tap(342, 815);
    assert(device.app.view() == View::Statistics);
    device.tap(342, 1030);
    device.capture(shots, "confirm-en");
    device.key(Input::K3);
    device.check_full_refresh();
    assert(report(root).total_ms == history.total_ms);
    device.tap(507, 1048);
    const auto cleared_book = report(root);
    assert(cleared_book.book_count == 1 && cleared_book.total_ms < history.total_ms);
    device.tap(50, 52);
    assert(device.app.view() == View::BookDetails);
    device.tap(50, 52);
    device.open_statistics();
    device.tap(342, 1060);
    device.tap(342, 1036);
    device.tap(150, 1048);
    assert(report(root).total_ms == cleared_book.total_ms);
    device.tap(342, 1036);
    device.tap(507, 1048);
    assert(report(root).book_count == 0);
}

void cancel_query(Device& device) {
    ContentJob::delay_starts_for_test(ContentJob::Kind::ReadingStatistics, std::chrono::milliseconds(90));
    device.press(Input::TouchCenter, 300, 50);
    assert(device.app.pending_frame() && device.app.content_busy());
    device.app.complete_frame(true);
    device.app.poll_background();
    device.press(Input::K2);
    assert(device.app.pending_frame() && device.app.pending_frame()->refresh == RefreshIntent::FullClean);
    device.app.complete_frame(true);
    assert(device.app.view() == View::Statistics && device.app.content_busy());
    device.tap(50, 52);
    assert(device.app.view() == View::Library);
    ContentJob::delay_starts_for_test(ContentJob::Kind::ReadingStatistics, std::chrono::milliseconds(0));
    const auto pixels = device.app.canvas().pixels();
    wait_for(device.app, [&] { return !device.app.background_due(); });
    assert(device.app.view() == View::Library && device.app.canvas().pixels() == pixels);
}

void catalog_capacity(const fs::path& root, const fs::path& fixtures, const fs::path& font) {
    prepare_card(root, font);
    const std::string existing = "Books/existing.txt", extra = "Books/extra.txt";
    for (const auto& path : {existing, extra}) fs::copy_file(fixtures / "sample.txt", root / path);
    FileStore store(root.string());
    std::uint64_t writer = 0, existing_id = 0, id = 0;
    assert(store.begin_statistics_writer(writer) == StoreStatus::Ok);
    StatisticsBookSource book{existing, sha256_file((root / existing).string()), "existing", -1, "txt-utf8-v1"};
    assert(store.statistics_book_id(book, existing_id, true) == StoreStatus::Ok);
    for (unsigned n = 1; n < 512; ++n) {
        book.path = "History/" + std::to_string(n) + ".txt";
        assert(store.statistics_book_id(book, id, true) == StoreStatus::Ok);
    }
    book.path = extra;
    assert(store.statistics_book_id(book, id, true) == StoreStatus::StatisticsCatalogFull);
    Device device(root, true);
    for (const auto& path : {extra, existing}) {
        assert(device.app.open_book(path));
        device.settle();
        device.clock.milliseconds += 6000;
        device.tap(342, 500);
        device.tap(50, 52);
        assert(device.app.view() == View::Library);
    }
    device.handoff();
    StatisticsReport result;
    assert(store.query_statistics({0, -1, -2, existing_id, 0, 1}, result) == StoreStatus::Ok);
    assert(result.total_ms == 6000 && result.book_count == 1);
}

void clear_with_failed_mirror(const fs::path& root, const fs::path& fixtures, const fs::path& font) {
    prepare_card(root, font);
    const std::string path = "Books/sample.txt";
    fs::copy_file(fixtures / "sample.txt", root / path);
    Device device(root, true);
    assert(device.app.open_book(path));
    device.settle();
    device.clock.milliseconds += 21000;
    device.tap(342, 500);
    device.tap(50, 52);
    device.open_statistics();
    assert(report(root).total_ms == 21000);
    device.tap(342, 1060);
    device.tap(342, 1036);
    const auto blocked = root / "PicoPage/data/statistics/catalog.1.tmp";
    fs::create_directory(blocked);
    device.tap(507, 1048);
    assert(device.app.canvas().pixel(60, 194) == 1);
    assert(report(root).total_ms == 0);
    assert(fs::remove(blocked));
    assert(device.app.open_book(path));
    device.settle();
    device.clock.milliseconds += 6000;
    device.handoff();
    assert(report(root).total_ms == 6000);
}

void failed_write_and_remount(const fs::path& root, const fs::path& fixtures, const fs::path& font) {
    prepare_card(root, font);
    const std::string book = "Books/sample.epub";
    fs::copy_file(fixtures / "valid.epub", root / book);
    const auto data = root / "PicoPage/data/statistics";
    std::filesystem::create_directories(data.parent_path());
    {
        Device device(root, true);
        assert(device.app.open_book(book));
        device.settle();
        const auto initial = device.app.saved_record();
        assert(initial);
        std::ofstream(data) << "fixture";
        device.clock.milliseconds += 50000;
        device.key(Input::K3);
        assert(device.app.saved_record() && device.app.saved_record()->anchor != initial->anchor);
        device.tap(342, 500);
        device.tap(50, 52);
        device.open_statistics();
        assert(device.app.canvas().pixel(60, 194) == 1);
        assert(fs::remove(data));
        device.clock.milliseconds += 45001;
        device.tap(342, 472);
        assert(device.app.canvas().pixel(60, 194) != 1);
        device.handoff();
    }
    assert(report(root).total_ms == 50000);
    {
        Device device(root, true);
        assert(device.app.open_book(book));
        device.settle();
        device.clock.milliseconds += 6000;
        device.app.card_unavailable(CardIssue::Removed);
        device.settle();
        assert(device.app.view() == View::Error);
    }
    assert(report(root).total_ms == 50000);
}

void empty_and_error(const fs::path& root, const fs::path& font, const fs::path& shots) {
    prepare_card(root, font);
    Device device(root, true);
    cancel_query(device);
    device.open_statistics();
    device.capture(shots, "empty");
    const auto empty = device.app.canvas().pixels();
    device.tap(50, 52);
    const auto data = root / "PicoPage/data/statistics";
    fs::create_directories(data);
    const auto damaged = data / "catalog.0";
    std::ofstream(damaged) << "interrupted catalog";
    device.open_statistics();
    const auto error = device.app.canvas().pixels();
    assert(error != empty);
    device.capture(shots, "error");
    device.check_full_refresh();
    device.tap(342, 605);
    const auto settings = device.app.canvas().pixels();
    assert(settings != error);
    device.tap(300, 174);
    assert(device.app.canvas().pixels() != settings);
    device.tap(50, 52);
    assert(device.app.canvas().pixels() == error);
    device.tap(342, 605);
    assert(device.app.canvas().pixels() == settings);
    device.tap(605, 420);
    device.tap(342, 852);
    StatisticsOptions saved;
    assert(FileStore(root.string()).load_statistics_options(saved) == StoreStatus::Ok);
    assert(saved.minimum_seconds == 6);
    device.tap(50, 52);
    assert(device.app.canvas().pixels() == error);
    assert(fs::remove(damaged));
    device.tap(342, 472);
    assert(device.app.canvas().pixels() == empty);
}

}

int main(int argc, char** argv) {
    assert(argc == 3 || argc == 4);
    char temporary[] = "/tmp/pico-statistics-app-XXXXXX";
    assert(mkdtemp(temporary));
    const fs::path directory(temporary), fixtures(argv[1]), font(argv[2]);
    const fs::path shots = argc == 4 ? fs::path(argv[3]) : fs::path{};
    const auto healthy_usb = acquisition(directory / "epub", fixtures, font, "valid.epub", true);
    acquisition(directory / "txt", fixtures, font, "sample.txt", false);
    force_power_without_statistics(directory / "forced-power", fixtures, font);
    immediate_reopen_and_idle_commit(directory / "immediate-reopen", fixtures, font);
    cancellable_sleep_commit(directory / "sleep-commit", fixtures, font);
    usb_handoff_write_failure(directory / "usb-failure", fixtures, font, healthy_usb);
    wifi_transfer_and_resume(directory / "wifi-transfer", fixtures, font);
    reflow_and_jump(directory / "reflow", fixtures, font);
    presentation_failure(directory / "failure", fixtures, font);
    image_updates(directory / "images", fixtures, font);
    failed_write_and_remount(directory / "write-failure", fixtures, font);
    catalog_capacity(directory / "capacity", fixtures, font);
    clear_with_failed_mirror(directory / "clear-mirror", fixtures, font);
    const auto root = directory / "calendar";
    prepare_card(root, font);
    seed_history(root);
    {
        Device device(root, true);
        calendar_and_books(device, shots);
        settings_and_clear(device, root, shots);
    }
    empty_and_error(directory / "blank", font, shots);
    fs::remove_all(directory);
}
