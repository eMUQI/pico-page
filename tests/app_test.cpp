#include "../firmware/components/reader_app/content_cache.h"
#include "../firmware/components/reader_app/content_job.h"
#include "../firmware/components/reader_app/help_content.h"
#include "../firmware/components/reader_app/reading_footer.h"
#include "image_fixtures.h"
#include "pico_page/app.h"
#include "pico_page/document.h"
#include "pico_page/store.h"
#include "pico_page/provision.h"
#include "pico_page/update.h"
#include "../firmware/components/platform_read_pico/include/pico_page/released_tap.h"
#include "../firmware/components/platform_read_pico/include/pico_page/sleep_input.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstring>
#include <thread>
#include <cassert>
#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <fcntl.h>
#include <dirent.h>
#include <sys/stat.h>
#include <cerrno>
#include <sys/resource.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
using namespace pico_page;

#ifdef PICO_PAGE_WRAP_MALLOC_TEST
static std::string counted_dictionary;
static std::atomic<unsigned> dictionary_preparations{0};
extern "C" FILE* __real_fopen(const char*, const char*);
extern "C" FILE* __wrap_fopen(const char* path, const char* mode) {
    if (path == counted_dictionary) ++dictionary_preparations;
    return __real_fopen(path, mode);
}
static std::atomic<bool> fail_wait_footer_malloc{false};
static std::atomic<unsigned> failed_wait_footer_mallocs{0};
extern "C" void* __real_malloc(std::size_t size);
extern "C" void* __wrap_malloc(std::size_t size) {
    if (size == 342U * 88U && fail_wait_footer_malloc.load()) {
        ++failed_wait_footer_mallocs; return nullptr;
    }
    return __real_malloc(size);
}
#endif

#ifdef PICO_PAGE_WRAP_LIBRARY_IO_TEST
static thread_local ino_t library_fault_inode = 0;
static thread_local dev_t library_fault_device = 0;
static thread_local int library_read_failure_after = -1, library_read_entries = 0;
static thread_local bool library_dirty_errno = false;
static thread_local std::string library_stat_failure_path;
static thread_local int library_stat_failure = 0;
extern "C" dirent* __real_readdir(DIR* directory);
extern "C" dirent* __wrap_readdir(DIR* directory) {
    bool target = false;
    if (library_fault_inode) {
        const int prior_errno = errno;
        struct stat info{};
        target = fstat(dirfd(directory), &info) == 0 && info.st_ino == library_fault_inode &&
                 info.st_dev == library_fault_device;
        errno = prior_errno;
    }
    if (target && library_read_failure_after >= 0 && library_read_entries >= library_read_failure_after) {
        errno = EIO; return nullptr;
    }
    auto* item = __real_readdir(directory);
    if (target && item) {
        if (item->d_name[0] != '.') ++library_read_entries;
        if (library_dirty_errno) errno = EIO;
    }
    return item;
}
extern "C" int __real_lstat(const char* path, struct stat* info);
extern "C" int __wrap_lstat(const char* path, struct stat* info) {
    if (library_stat_failure && library_stat_failure_path == path) {
        errno = library_stat_failure; return -1;
    }
    return __real_lstat(path, info);
}
#endif

static std::string read(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
static void write(const fs::path& path, const std::string& value) {
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary); output << value;
    assert(output.good());
}
static void opened(ReaderApp& app, const std::string& book) {
    if (!app.open_book(book)) { std::cerr << "Open failed: " << app.status() << '\n'; std::abort(); }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (app.content_busy()) {
        // A progressive image page is readable before its optional image job completes.
        if (app.pending_frame() && app.pending_frame()->purpose == FramePurpose::Body) break;
        if (app.pending_frame() && app.pending_frame()->purpose != FramePurpose::Body) app.complete_frame(true);
        app.poll_background();
        assert(std::chrono::steady_clock::now() < deadline);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    assert(app.pending_frame() && app.pending_frame()->purpose == FramePurpose::Body);
}
static bool sleep_frame(ReaderApp& app) {
    if (!app.begin_sleep()) return false;
    if (app.pending_frame()) app.complete_frame(true);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (!app.prepare_sleep()) {
        if (!app.sleep_pending()) return false;
        app.poll_background();
        if (app.pending_frame()) app.complete_frame(true);
        assert(std::chrono::steady_clock::now() < deadline);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    assert(!app.sleep_pending());
    return true;
}
// Reports the calling line when an expected frame is missing.
#define present() present_at(__LINE__)
#define check_remote() check_remote_at(__LINE__)
#define sync_view() sync_view_at(__LINE__)
struct Driver {
    ReaderApp& app;
    std::uint64_t sequence = 0;
    std::function<void(const Frame&)> frame_observer = {};
    void key(Input key) {
        ++sequence;
        app.input({{sequence, Phase::Begin, key}, false, 0, 0});
        app.input({{sequence, Phase::End, key}, false, 0, 0});
    }
    void tap(int x, int y) {
        ++sequence;
        app.input({{sequence, Phase::Begin, Input::TouchCenter}, true, x, y});
        app.input({{sequence, Phase::End, Input::TouchCenter}, true, x, y});
    }
    void drag(int x, int y, AppInput::TouchBounds bounds) {
        ++sequence;
        AppInput input{{sequence, Phase::Begin, Input::TouchCenter}, true, x, y};
        input.touch_bounds = bounds;
        app.input(input);
        input.event.phase = Phase::End;
        app.input(input);
    }
    void swipe(bool next, int x = 300, int y = 480, bool queued = false) {
        using namespace pico_page::platform;
        const Event begin{EventKind::Touch, EventPhase::Begin, ++sequence, x, y, 1000};
        const Event end{EventKind::Touch, EventPhase::End, sequence, x + (next ? -100 : 100), y, 651000};
        const auto gesture = released_gesture(begin, end);
        assert(gesture == (next ? ReleasedGesture::Next : ReleasedGesture::Previous));
        AppInput input{{sequence, Phase::Begin, next ? Input::Next : Input::Previous}, false, x, y, queued};
        app.input(input);
        input.event.phase = Phase::End;
        app.input(input);
    }
    void hold(int x, int y) {
        ++sequence;
        app.input({{sequence, Phase::Begin, Input::TouchCenter}, true, x, y, false, true});
        app.input({{sequence, Phase::End, Input::TouchCenter}, true, x, y, false, true});
    }
    // Mirrors the firmware loop: every frame queued by deferred work, then the idle-time save.
    // A contact that began while the previous frame was still in flight.
    void queued(Input key, bool touch = false, int x = 0, int y = 0) {
        ++sequence;
        app.input({{sequence, Phase::Begin, key}, touch, x, y, true});
        app.input({{sequence, Phase::End, key}, touch, x, y, true});
    }
    void present_at(int line) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        while (!app.pending_frame() && app.view() == View::Loading && app.content_busy()) {
            app.poll_background();
            assert(std::chrono::steady_clock::now() < deadline);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (!app.pending_frame()) { std::cerr << "No frame to present at line " << line << '\n'; std::abort(); }
        // A finished background commit may queue a further frame, as in the firmware loop.
        while (app.pending_frame()) {
            while (app.pending_frame()) {
                while (!app.prepare_frame()) {
                    assert(std::chrono::steady_clock::now() < deadline);
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
                if (frame_observer) frame_observer(*app.pending_frame());
                app.complete_frame(true);
            }
            if (app.save_due()) app.save_progress();
            while (app.save_busy()) {
                app.poll_background();
                assert(std::chrono::steady_clock::now() < deadline);
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
        // These scenarios start from the library. Exercise the explicit skip when their
        // fresh fixture now boots into onboarding; onboarding_test owns guide assertions.
        if (app.view() == View::Onboarding) {
            tap(300, 1120);
            assert(app.pending_frame());
            present_at(line);
        }
    }
    void complete_content() {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        while (app.content_busy()) {
            app.poll_background();
            if (app.pending_frame()) present();
            assert(std::chrono::steady_clock::now() < deadline);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    void close() {
        if (app.view() == View::Tools) tap(342, 500);
        else if (app.view() == View::Typography) tap(628, 628);
        else if (app.view() == View::Contents) tap(628, 322);
        else if (app.view() == View::Refresh) tap(628, 474);
        else if (app.view() == View::Sync) {
            tap(628, 480);
            // The position choice has no strip; its local card keeps the current page.
            if (!app.pending_frame()) tap(100, 800);
        }
        else if (app.view() == View::Error || app.view() == View::Confirmation || app.view() == View::UsbTransfer) tap(100, 1100);
        else tap(60, 50);
    }
    void refresh() {
        const auto view = app.view();
        const auto pixels = app.canvas().pixels();
        const auto record = app.saved_record();
        key(Input::K2);
        assert(app.pending_frame() && app.pending_frame()->purpose == FramePurpose::ManualRefresh);
        assert(app.pending_frame()->refresh == RefreshIntent::FullClean);
        assert(!app.pending_frame()->changed_pixels_only);
        const auto token = app.pending_frame()->token;
        key(Input::K2);
        assert(app.pending_frame()->token == token);
        present();
        assert(app.view() == view && app.canvas().pixels() == pixels && app.saved_record() == record);
        assert(!app.pending_frame());
    }
    void portal(PortalStatus status) {
        status.generation = app.portal_generation();
        app.show_portal(status);
    }
    void sync_view_at(int line) {
        tap(342, 500);
        if (app.view() != View::Tools) std::cerr << "Missing tools at line " << line << " view=" << static_cast<int>(app.view()) << "\n";
        assert(app.view() == View::Tools); present();
        tap(342, 1140); assert(app.view() == View::Sync); present();
    }
    NetworkOperation check_remote_at(int line) {
        tap(342, 960); present();
        auto request = app.take_network_request();
        if (!request) std::cerr << "Missing sync request at line " << line << "\n";
        assert(request && request->request.method == "GET");
        return *request;
    }
    void typography() {
        tap(342, 500); assert(app.view() == View::Tools); present(); refresh();
        tap(205, 1140); assert(app.view() == View::Typography); present();
    }
};

// Wi-Fi and account setup on the device: the keyboard, joining before saving, the connection
// mode and the verified account. No step may write a credential before its check succeeds.
static void network_setup_test(const fs::path& root, const fs::path& fixtures, bool async) {
    fs::create_directories(root / "Books");
    fs::copy_file(fixtures / "valid.epub", root / "Books/book.epub");
    const auto network_file = root / "PicoPage/network.toml", account_file = root / "PicoPage/sync.toml";
    ReaderApp app(root.string(), "setup-test", true, async); Driver driver{app};
    assert(app.initialize()); driver.present();
    // Keys of the letter layer by character; rows start at 812, 912 and 1012.
    const auto letter = [&](char c) {
        const std::string rows[] = {"qwertyuiop", "asdfghjkl", "zxcvbnm"};
        const int lefts[] = {40, 73, 140};
        for (int row = 0; row < 3; ++row)
            if (const auto column = rows[row].find(c); column != std::string::npos)
                return driver.tap(lefts[row] + 67 * static_cast<int>(column), 860 + 100 * row);
        std::abort();
    };
    const auto type = [&](const std::string& text) {
        for (const char c : text) letter(c);
        app.poll_background(); driver.present();
    };
    std::vector<provision::SavedWifi> saved; std::optional<provision::SavedWifi> pinned; std::string error;

    driver.tap(600, 50); driver.present(); driver.tap(300, 180); driver.present();
    driver.tap(300, 570); driver.present(); assert(app.view() == View::WifiSettings);
    // Without a saved network there is nothing to connect to.
    driver.tap(300, 740); assert(!app.pending_frame() && !app.take_wifi_request());
    driver.tap(300, 590); driver.present();
    auto scan = app.take_wifi_request();
    assert(scan && scan->kind == WifiOperation::Kind::Scan);
    app.complete_wifi(scan->id, {{"Office-2.4", -40, false}, {"Guest", -60, true}}, ""); driver.present();
    driver.tap(300, 220); driver.present(); assert(app.view() == View::TextEntry && app.keyboard_visible());
    // A network that the scan reported as protected does not accept an empty password.
    driver.tap(579, 1160); assert(!app.pending_frame() && app.view() == View::TextEntry && !app.take_wifi_request());

    // Key taps change the entry only; taps made during a refresh are kept in order, and the next
    // idle poll draws all of them in one regional update.
    const auto empty = app.canvas().pixels();
    driver.tap(40, 860);
    assert(!app.pending_frame() && app.canvas().pixels() == empty);
    driver.queued(Input::TouchCenter, true, 107, 860);
    driver.key(Input::K1); driver.key(Input::K3);
    app.poll_background(true);
    assert(!app.pending_frame() && app.view() == View::TextEntry);
    app.poll_background();
    assert(app.pending_frame() && app.pending_frame()->refresh == RefreshIntent::Content && app.pending_frame()->region);
    assert(app.pending_frame()->changed_pixels_only);
    driver.present(); assert(app.canvas().pixels() != empty);
    // Once the enter key keeps its state, a character repaints the input field alone.
    driver.tap(174, 860); app.poll_background();
    assert(app.pending_frame() && app.pending_frame()->region->y == 104 && app.pending_frame()->region->height == 320);
    assert(app.pending_frame()->changed_pixels_only);
    driver.present();
    // Three characters do not make a password, and a contact that leaves its key enters nothing.
    driver.tap(579, 1160); assert(!app.pending_frame() && app.view() == View::TextEntry && !app.take_wifi_request());
    driver.drag(241, 860, {241, 860, 330, 860}); app.poll_background(); assert(!app.pending_frame());
    // A layer change redraws the keys at once; digits follow on the new layer.
    driver.tap(74, 1160);
    assert(app.pending_frame() && app.pending_frame()->region && app.pending_frame()->region->y == 796);
    assert(app.pending_frame()->changed_pixels_only);
    driver.present();
    for (int digit = 0; digit < 5; ++digit) driver.tap(40 + 67 * digit, 860);
    app.poll_background(); driver.present();
    driver.tap(74, 1160); driver.present();
    // Shift applies to one letter; backspace removes the last character.
    driver.tap(57, 1060);
    assert(app.pending_frame() && app.pending_frame()->changed_pixels_only);
    driver.present();
    driver.tap(40, 860); driver.present();
    letter('q'); letter('z'); driver.tap(627, 1060);
    app.poll_background(); driver.present();
    driver.refresh();
    driver.tap(579, 1160); driver.present(); assert(app.view() == View::WifiSettings);
    auto join = app.take_wifi_request();
    assert(join && join->kind == WifiOperation::Kind::Join && !join->targets.automatic && join->targets.networks.size() == 1);
    assert(join->targets.networks[0].ssid == "Office-2.4" && join->targets.networks[0].password == "qwe12345Qq");
    assert(!fs::exists(network_file));
    // Cancelling returns to the password with its draft; the late result saves nothing.
    driver.tap(300, 1040); driver.present();
    assert(app.view() == View::TextEntry && join->cancelled->load());
    app.complete_wifi(join->id, {}, ""); assert(!app.pending_frame() && !fs::exists(network_file));
    // The enter key right after the character that completes the password, before that
    // character is drawn, still connects: taps meet the keys of the current entry.
    for (int i = 0; i < 3; ++i) driver.tap(627, 1060);
    app.poll_background(); driver.present();
    driver.tap(579, 1160); assert(!app.pending_frame() && app.view() == View::TextEntry);
    driver.tap(74, 1160); driver.present();
    driver.tap(308, 860); assert(!app.pending_frame());
    driver.queued(Input::TouchCenter, true, 579, 1160);
    driver.present(); assert(app.view() == View::WifiSettings);
    join = app.take_wifi_request();
    assert(join && join->targets.networks[0].password == "qwe12345");
    // A refused password saves nothing and leads back to the same draft.
    app.complete_wifi(join->id, {}, "无法连接「Office-2.4」，密码可能不正确。", LinkFailure::Password);
    driver.present(); assert(app.view() == View::WifiSettings && !fs::exists(network_file));
    driver.tap(300, 920); driver.present(); assert(app.view() == View::TextEntry);
    driver.tap(579, 1160); driver.present();
    join = app.take_wifi_request();
    assert(join && join->targets.networks[0].password == "qwe12345");
    // The link of a join earns no connection record: the network is not saved yet.
    app.show_wifi({1, WifiPhase::Connected, "Office-2.4", "", false}); app.poll_background();
    assert(!app.pending_frame() && !fs::exists(network_file));
    app.complete_wifi(join->id, {}, ""); driver.present();
    app.show_wifi({2, WifiPhase::Offline, "", "", false}); app.poll_background();
    // The verified network is saved; adding it leaves the connection automatic.
    assert(provision::load_networks(root.string(), saved, error) && saved.size() == 1 &&
           saved[0].ssid == "Office-2.4" && saved[0].password == "qwe12345" && saved[0].last_connected > 0);
    assert(provision::load_selected_network(root.string(), pinned, error) && !pinned);

    // A typed name of an open network survives a cancelled attempt; a 64-digit key is accepted.
    driver.tap(300, 1040); driver.present(); assert(app.view() == View::WifiSettings);
    // The page carries no notice about an unsaved connection record.
    for (int y = 1056; y < 1100; ++y)
        for (int x = 36; x < 648; ++x) assert(app.canvas().pixel(x, y) == 15);
    driver.tap(300, 590); driver.present();
    scan = app.take_wifi_request(); assert(scan);
    app.complete_wifi(scan->id, {}, ""); driver.present();
    driver.tap(300, 770); driver.present(); assert(app.view() == View::TextEntry);
    type("cafe");
    driver.tap(579, 1160); driver.present(); assert(app.view() == View::TextEntry);
    driver.tap(579, 1160); driver.present(); assert(app.view() == View::WifiSettings);
    join = app.take_wifi_request();
    assert(join && join->targets.networks[0].ssid == "cafe" && join->targets.networks[0].password.empty());
    driver.tap(300, 1040); driver.present(); assert(app.view() == View::TextEntry && join->cancelled->load());
    for (int i = 0; i < 64; ++i) letter('a');
    letter('a');
    app.poll_background(); driver.present();
    driver.tap(579, 1160); driver.present(); assert(app.view() == View::WifiSettings);
    join = app.take_wifi_request();
    assert(join && join->targets.networks[0].ssid == "cafe" && join->targets.networks[0].password == std::string(64, 'a'));
    app.complete_wifi(join->id, {}, "找不到名为「cafe」的网络。", LinkFailure::NotNearby); driver.present();
    assert(provision::load_networks(root.string(), saved, error) && saved.size() == 1);
    // A refused empty password of a typed network leads back to the password, not to a blind retry.
    driver.tap(300, 1040); driver.present(); driver.tap(300, 770); driver.present(); type("cafe");
    driver.tap(579, 1160); driver.present(); driver.tap(579, 1160); driver.present();
    join = app.take_wifi_request(); assert(join && join->targets.networks[0].password.empty());
    app.complete_wifi(join->id, {}, "无法连接「cafe」，密码可能不正确。", LinkFailure::Password); driver.present();
    driver.tap(300, 920); driver.present(); assert(app.view() == View::TextEntry && !app.take_wifi_request());
    driver.tap(60, 50); driver.present(); driver.tap(60, 50); driver.present();
    // A 64-character password that is not a hexadecimal key cannot be submitted.
    assert(app.view() == View::WifiSettings);
    driver.tap(300, 770); driver.present(); type("cafe");
    driver.tap(579, 1160); driver.present();
    for (int i = 0; i < 64; ++i) letter('z');
    app.poll_background(); driver.present();
    driver.tap(579, 1160); assert(!app.pending_frame() && app.view() == View::TextEntry && !app.take_wifi_request());
    driver.tap(60, 50); driver.present(); driver.tap(60, 50); driver.present();
    assert(app.view() == View::WifiSettings);
    driver.tap(60, 50); driver.present(); driver.tap(60, 50); driver.present();
    driver.tap(300, 310); driver.present(); assert(app.view() == View::SyncSettings);

    // Its check runs before anything is written.
    driver.tap(300, 620); assert(!app.pending_frame() && !app.take_network_request());
    driver.tap(300, 310); driver.present(); assert(app.view() == View::TextEntry);
    type("reader");
    driver.tap(579, 1160); driver.present(); assert(app.view() == View::SyncSettings);
    driver.tap(300, 410); driver.present();
    type("secret");
    driver.tap(579, 1160); driver.present();
    driver.tap(300, 620); driver.present();
    auto check = app.take_network_request();
    assert(check && check->request.method == "GET" && check->request.url == std::string(provision::default_endpoint) + "/users/auth");
    assert(check->targets.automatic && check->targets.networks.size() == 1 && check->targets.networks[0].ssid == "Office-2.4");
    bool keyed = false;
    for (const auto& header : check->request.headers)
        keyed |= header.first == "x-auth-key" && header.second == "5ebe2294ecd0e0f08eab7690d2a6ee69";
    assert(keyed && !fs::exists(account_file));
    app.complete_network(check->id, 401, "{}", ""); driver.present();
    assert(app.view() == View::SyncSettings && !fs::exists(account_file) && !network::live(check->session));
    driver.tap(300, 620); driver.present();
    check = app.take_network_request(); assert(check);
    // A link failure is reported with its cause and still saves nothing.
    app.complete_network(check->id, 0, "", "附近没有已保存的网络。", LinkFailure::NotNearby); driver.present();
    assert(app.view() == View::SyncSettings && !fs::exists(account_file));
    driver.tap(300, 620); driver.present();
    check = app.take_network_request(); assert(check);
    app.complete_network(check->id, 200, "{}", ""); driver.present();
    sync::Config account;
    assert(sync::load_config(root.string(), account, error) && account.endpoint == provision::default_endpoint &&
           account.username == "reader" && account.userkey == "5ebe2294ecd0e0f08eab7690d2a6ee69" &&
           account.device_id == "setup-test");
    const auto account_bytes = read(account_file);
    // Re-verification of the unchanged account uses the stored key.
    driver.tap(300, 620); driver.present();
    check = app.take_network_request(); assert(check);
    app.complete_network(check->id, 200, "{}", ""); driver.present();
    assert(read(account_file) == account_bytes);

    // A remote position without a local counterpart keeps reading as the first action.
    opened(app, "Books/book.epub"); driver.present();
    const auto reading = *app.saved_record();
    driver.sync_view();
    auto query = driver.check_remote();
    assert(query.targets.automatic);
    app.complete_network(query.id, 200, R"({"progress":"/body[1]/DocFragment[9]/body[1]/p[77]/text()[1].0","percentage":0.42,"device":"Kindle"})", "");
    driver.present(); driver.complete_content();
    assert(app.view() == View::Sync && *app.saved_record() == reading);
    driver.tap(342, 990); driver.present(); driver.complete_content();
    assert(app.view() == View::Reading && !app.take_network_request() && *app.saved_record() == reading);

    // Pinning is explicit; forgetting the pinned network returns to automatic connection.
    driver.tap(342, 500); driver.present(); driver.tap(60, 50); driver.present(); driver.complete_content();
    assert(app.view() == View::Library);
    driver.tap(600, 50); driver.present();
    driver.tap(300, 180); driver.present(); driver.tap(300, 570); driver.present();
    assert(app.view() == View::WifiSettings);
    driver.tap(300, 390); driver.present(); driver.tap(300, 370); driver.present();
    assert(provision::load_selected_network(root.string(), pinned, error) && pinned && pinned->ssid == "Office-2.4");
    driver.tap(300, 180); driver.present();
    assert(provision::load_selected_network(root.string(), pinned, error) && !pinned);
    driver.tap(300, 370); driver.present();
    driver.tap(60, 50); driver.present(); driver.tap(300, 490); driver.present();
    driver.tap(300, 230); driver.present(); driver.tap(300, 420); driver.present();
    driver.refresh();
    driver.tap(500, 1120); driver.present();
    assert(provision::load_networks(root.string(), saved, error) && saved.empty());
    assert(provision::load_selected_network(root.string(), pinned, error) && !pinned);

    // Removing the account deletes its file after a confirmation.
    driver.tap(60, 50); driver.present(); driver.tap(60, 50); driver.present();
    assert(app.view() == View::Settings);
    driver.tap(300, 310); driver.present(); assert(app.view() == View::SyncSettings);
    driver.tap(300, 740); driver.present(); assert(fs::exists(account_file));
    driver.tap(500, 1120); driver.present();
    assert(app.view() == View::SyncSettings && !fs::exists(account_file));

    // An account name from a hand-edited file may hold multi-byte characters; backspace removes
    // one whole character, so the name that is checked stays valid UTF-8.
    write(account_file, "schema_version = 1\ndom_version = 20260812\nendpoint = \"https://example.invalid\"\n"
                        "username = \"读者ab\"\nuserkey = \"test-only-placeholder\"\ndevice_id = \"setup-test\"\n");
    write(network_file, "schema_version=2\n[[wifi]]\nssid=\"home\"\npassword=\"password\"\n");
    driver.tap(60, 50); driver.present(); driver.tap(300, 310); driver.present();
    driver.tap(300, 310); driver.present(); assert(app.view() == View::TextEntry);
    for (int i = 0; i < 3; ++i) driver.tap(627, 1060);
    app.poll_background(); driver.present();
    driver.tap(579, 1160); driver.present(); assert(app.view() == View::SyncSettings);
    driver.tap(300, 410); driver.present();
    type("secret");
    driver.tap(579, 1160); driver.present();
    driver.tap(300, 620); driver.present();
    check = app.take_network_request();
    bool named = false;
    for (const auto& header : check->request.headers) named |= header.first == "x-auth-user" && header.second == "读";
    assert(check && named);
    driver.tap(300, 1040); driver.present();
    assert(app.view() == View::NetworkClosing && check->cancelled->load());
    app.complete_network(check->id, 200, "{}", ""); driver.present();
    assert(app.view() == View::SyncSettings && read(account_file).find("读者ab") != std::string::npos);

    // A kept connection that an access point session suspended is offline; the page offers to
    // connect again instead of only ending the kept intent.
    driver.tap(60, 50); driver.present(); driver.tap(300, 180); driver.present();
    driver.tap(300, 570); driver.present(); assert(app.view() == View::WifiSettings);
    driver.tap(300, 740); driver.present();
    const auto kept = app.take_wifi_request();
    assert(kept && kept->kind == WifiOperation::Kind::Connect && kept->targets.automatic);
    app.complete_wifi(kept->id, {}, "");
    app.show_wifi({101, WifiPhase::Connected, "home", "", true}); driver.present();
    app.show_wifi({102, WifiPhase::Offline, "", "手动连接已暂停，点「立即连接」可恢复。", false}); driver.present();
    assert(network::live(kept->cancelled));
    driver.tap(300, 740); driver.present();
    const auto resumed = app.take_wifi_request();
    assert(resumed && resumed->kind == WifiOperation::Kind::Connect && kept->cancelled->load());
    app.stop_wifi();
}

static void help_test(const fs::path& root, const fs::path& fixtures, bool async, bool with_book) {
    ReaderApp app(root.string(), "help-test", true, async); Driver driver{app};
    assert(app.initialize()); driver.present();
    if (with_book) {
        fs::create_directories(root / "Books");
        fs::copy_file(fixtures / "valid.epub", root / "Books/book.epub");
        opened(app, "Books/book.epub"); driver.present();
        driver.tap(342, 500); driver.present(); assert(app.view() == View::Tools);
        driver.tap(60, 50); driver.present(); assert(app.view() == View::Library);
    }
    const auto saved = app.saved_record();
    const auto progress = read(root / "Books/book.epub.picopage/progress.json");
    const auto recent = read(root / "PicoPage/data/last-book.txt");
    driver.tap(600, 50); driver.present(); assert(app.view() == View::Settings);
    driver.tap(300, 948); driver.present(); assert(app.view() == View::Help);
    const auto first = app.canvas().pixels();
    driver.key(Input::K1); driver.tap(60, 1160); assert(!app.pending_frame());
    graphics::Font fallback; assert(fallback.open_fallback());
    for (std::size_t index = 0; index < kHelpPages.size(); ++index) {
        const auto& page = kHelpPages[index];
        for (const auto text : {page.title, page.sections[0].title, page.sections[0].body,
                              page.sections[1].title, page.sections[1].body, page.sections[2].title, page.sections[2].body})
            for (const auto scalar : graphics::decode_utf8(text)) assert(fallback.contains(scalar));
        // Text must end before the pager, leaving a visible gap above its border.
        for (int y = 1100; index != 3 && y < 1119; ++y)
            for (int x = 24; x < 660; ++x)
                if (app.canvas().pixel(x, y) != 15) {
                    std::cerr << "Help page " << index + 1 << " overlaps the pager at " << x << ',' << y << '\n';
                    std::abort();
                }
        if (index == 2 || index == 3) {
            const auto original = app.canvas().pixels();
            driver.tap(index == 2 ? 60 : 600, index == 2 ? 350 : 400); driver.present();
            assert(app.canvas().pixels() != original && app.view() == View::Help);
            if (index == 2) {
                for (const int x : {235, 236, 448}) {
                    driver.tap(x, 929); driver.present();
                    const int selected = x < 236 ? 0 : x < 448 ? 1 : 2;
                    for (int cell = 0; cell < 3; ++cell)
                        assert(app.canvas().pixel(32 + cell * 212, 850) == (cell == selected ? 1 : 15));
                }
            }
            if (index == 3) {
                for (const auto& cycle : {std::pair<int, int>{100, 5}, {600, 3}}) {
                    driver.tap(cycle.first, 1160); driver.present();
                    const auto selected = app.canvas().pixels();
                    for (int step = 0; step < cycle.second; ++step) {
                        driver.tap(cycle.first, 1160); driver.present();
                    }
                    assert(app.canvas().pixels() == selected);
                    for (int step = 0; step < cycle.second - 2; ++step) {
                        driver.tap(cycle.first, 1160); driver.present();
                    }
                    assert(app.canvas().pixel(cycle.first, 1140) == 15);
                    driver.refresh();
                    assert(app.canvas().pixel(cycle.first, 1140) == 15);
                    driver.tap(cycle.first, 1160); driver.present();
                    assert(app.canvas().pixel(cycle.first, 1140) == 1);
                }
            }
        }
        driver.refresh();
        const auto pixels = app.canvas().pixels();
        driver.queued(Input::K3); driver.queued(Input::TouchCenter, true, 600, 1160);
        assert(!app.pending_frame() && app.canvas().pixels() == pixels);
        assert(app.saved_record() == saved && !app.take_network_request());
        assert(app.take_portal_action() == PortalAction::None && app.take_usb_transfer_action() == UsbTransferAction::None);
        if (index + 1 < kHelpPages.size()) {
            if (index == 3) driver.key(Input::K3);
            else if (index % 2) driver.tap(600, 1160); else driver.swipe(true);
            assert(app.pending_frame() && app.pending_frame()->purpose == FramePurpose::Overlay);
            driver.present(); assert(app.canvas().pixels() != pixels);
        }
    }
    driver.key(Input::K3); driver.tap(600, 1160); assert(!app.pending_frame());
    for (std::size_t index = 1; index < kHelpPages.size(); ++index) {
        if (kHelpPages.size() - index == 3) driver.key(Input::K1);
        else if (index % 2) driver.tap(60, 1160); else driver.swipe(false);
        driver.present(); assert(app.view() == View::Help);
    }
    assert(app.canvas().pixels() == first && app.saved_record() == saved);
    assert(read(root / "Books/book.epub.picopage/progress.json") == progress);
    assert(read(root / "PicoPage/data/last-book.txt") == recent);
    driver.key(Input::K3); driver.present();
    driver.tap(60, 50); driver.present(); assert(app.view() == View::Settings);
    driver.tap(300, 948); driver.present(); assert(app.canvas().pixels() == first);
    assert(app.show_power_menu()); driver.present(); driver.tap(300, 1120); driver.present();
    assert(app.view() == View::Help && app.canvas().pixels() == first && app.saved_record() == saved);
    if (!with_book) assert(!fs::exists(root));
}

static void content_font_test(const fs::path& root, const fs::path& fixtures, const fs::path& font, bool async) {
    const std::string folder = "歡龘abc", book = "Books/" + folder + "/book.epub";
    std::vector<std::uint8_t> header;
    const auto crop = [](const graphics::Canvas& canvas, int x, int y, int width, int height) {
        std::vector<std::uint8_t> pixels;
        for (int row = y; row < y + height; ++row)
            for (int column = x; column < x + width; ++column) pixels.push_back(canvas.pixel(column, row));
        return pixels;
    };
    for (const std::string mode : {"valid", "missing", "corrupt"}) {
        const auto card = root / mode;
        fs::create_directories(card / "Books" / folder); fs::create_directories(card / "Fonts");
        fs::copy_file(fixtures / "valid.epub", card / book);
        fs::copy_file(fixtures / "valid.epub", card / "Books" / folder / "global.epub");
        if (mode == "valid") fs::copy_file(font, card / "Fonts/Library.ttf");
        if (mode == "corrupt") write(card / "Fonts/Library.ttf", "invalid font");
        fs::copy_file(fs::path(__FILE__).parent_path().parent_path() /
                      "firmware/components/reader_graphics/fonts/PicoPageFallback.otf", card / "Fonts/Book.otf");
        write(card / "PicoPage/settings.toml", "schema_version=1\n[reader]\nfont='Fonts/Library.ttf'\n[library]\nview='list'\n");
        FileStore store(card.string()); std::string error;
        const auto snapshot = store.settings_snapshot(book);
        auto settings = snapshot.config.settings; settings.font = "Fonts/Book.otf";
        assert(store.apply_settings(snapshot, settings, true, error) == StoreStatus::Ok);
        const auto config = read(card / "PicoPage/settings.toml");
        ReaderApp app(card.string(), "content-font", true, async); Driver d{app};
        assert(app.initialize()); d.present();
        graphics::Font expected_font;
        const auto caches = app.cache_usage();
        assert(caches.library_glyph_bytes <= 128U * 1024 && caches.library_advances <= 1024);
        assert(!caches.library_glyph_bytes);
        if (mode == "valid") assert(caches.body_glyph_bytes > 0);
        assert(mode == "valid" ? expected_font.open(font.string()) : expected_font.open_fallback());
        assert(expected_font.set_pixel_size(28));
        graphics::Canvas expected;
        int x = 36;
        for (const auto c : graphics::decode_utf8(folder)) {
            assert(expected_font.draw(expected, c, x, 238 + expected_font.metrics().ascender));
            x += expected_font.advance(c);
        }
        const auto label = crop(expected, 36, 232, 300, 65);
        assert(crop(app.canvas(), 36, 232, 300, 65) == label);
        const auto current_header = crop(app.canvas(), 0, 0, 684, 104);
        if (header.empty()) header = current_header;
        else assert(current_header == header);
        d.refresh();
        opened(app, book); d.present();
        assert(!app.cache_usage().library_glyph_bytes);
        const auto body = crop(app.canvas(), 0, 104, 684, 900);
        const auto saved = app.saved_record();
        d.tap(342, 500); d.present(); d.tap(68, 1150); d.present();
        assert(app.view() == View::Contents);
        graphics::Font book_font; assert(book_font.open_fallback() && book_font.set_pixel_size(28));
        EpubDocument document; assert(document.open((card / book).string(), error));
        expected.clear(1);
        const auto metrics = book_font.metrics();
        const int baseline = 522 + (92 - metrics.ascender - metrics.descender) / 2 + metrics.ascender;
        x = 36;
        for (const auto c : graphics::decode_utf8(document.toc().front().label)) {
            assert(book_font.draw(expected, c, x, baseline, 15, true)); x += book_font.advance(c);
        }
        assert(crop(app.canvas(), 36, 522, 300, 90) == crop(expected, 36, 522, 300, 90));
        d.close(); d.present();
        assert(app.view() == View::Reading && crop(app.canvas(), 0, 104, 684, 900) == body);
        assert(app.saved_record() == saved);
        d.tap(342, 500); d.present(); d.tap(50, 50); d.present();
        assert(app.view() == View::Library);
        assert(crop(app.canvas(), 36, 482, 300, 65) == label);
        assert((app.cache_usage().library_glyph_bytes > 0) == (mode == "valid"));
        assert(read(card / "PicoPage/settings.toml") == config);
        if (mode == "valid") {
            opened(app, "Books/" + folder + "/global.epub"); d.present();
            assert(!app.cache_usage().library_glyph_bytes);
            d.tap(342, 500); d.present(); d.tap(50, 50); d.present();
            assert(sleep_frame(app)); d.present(); app.wake(); d.present();
            assert(app.view() == View::Library && crop(app.canvas(), 36, 482, 300, 65) == label);
        }
        assert(app.prepare_usb_transfer());
        const auto released = app.cache_usage();
        assert(!released.library_glyph_bytes && !released.library_glyphs && !released.library_advances);
    }
}

static void font_picker_test(const fs::path& root, const fs::path& fixtures, const fs::path& font, bool async) {
    fs::create_directories(root / "Books"); fs::create_directories(root / "Fonts");
    fs::copy_file(fixtures / "valid.epub", root / "Books/book.epub");
    fs::copy_file(font, root / "Fonts/LXGWWenKai-Regular.ttf");
    const auto local_font = root / "Fonts/LXGWWenKai-Regular.ttf";
    fs::create_hard_link(local_font, root / "Fonts/NotoSansSC-Regular.otf");
    for (int i = 0; i < 14; ++i) {
        const auto name = (i < 10 ? "0" : "") + std::to_string(i) +
            (i == 13 ? "-a-very-long-font-name-that-wraps-onto-two-lines.ttf" : ".ttf");
        fs::create_hard_link(local_font, root / "Fonts" / name);
    }
    write(root / "Fonts/z-broken.ttf", "not a font");
    write(root / "PicoPage/settings.toml", "schema_version=1\n[reader]\nfont_weight='bold'\n");
    FileStore store(root.string());
    ReaderApp app(root.string(), "font-picker", true, async); Driver d{app};
    assert(app.initialize()); d.present(); opened(app, "Books/book.epub"); d.present();
    const auto saved = app.saved_record();
    const auto progress = read(root / "Books/book.epub.picopage/progress.json");
    const auto config = read(root / "PicoPage/settings.toml");
    const auto original_size = store.load_settings("Books/book.epub").settings.font_size_px;
    d.typography();
    // Preserve a size edit made before entering the font picker.
    d.tap(580, 1020); d.present();
    const auto typography = app.canvas().pixels();
    d.tap(90, 820); d.present();
    assert(app.view() == View::Typography && app.canvas().pixel(624, 849) == 1);
    const auto initial = app.canvas().pixels();
    d.refresh();
    // The current font opens on page four. The fifth page has only the broken file.
    d.tap(540, 1040); d.present();
    assert(std::equal(initial.begin(), initial.begin() + 500 * 342, app.canvas().pixels().begin()));
    d.tap(540, 1040); assert(!app.pending_frame());
    d.tap(300, 760); assert(!app.pending_frame());
    d.tap(300, 660); d.present();
    d.tap(500, 1140); assert(!app.pending_frame());
    assert(read(root / "PicoPage/settings.toml") == config);
    d.tap(150, 1140); d.present();
    assert(app.canvas().pixels() == typography);
    d.tap(90, 820); d.present();
    assert(app.canvas().pixels() == initial);
    // Selection may be on a different list page. Browsing and K2 must retain it.
    d.tap(300, 660); d.present();
    assert(app.canvas().pixel(624, 669) == 1 && app.canvas().pixel(624, 849) == 15);
    d.tap(140, 1040); d.present();
    d.refresh();
    d.tap(540, 1040); d.present();
    assert(app.canvas().pixel(624, 669) == 1);
    // Cancel restores both the earlier font weight and the pre-existing size draft.
    d.tap(150, 1140); d.present();
    assert(app.canvas().pixels() == typography);
    d.tap(90, 820); d.present();
    d.tap(300, 660); d.present();
    d.tap(140, 1040); d.present();
    d.tap(500, 1140); d.present();
    assert(store.load_settings("Books/book.epub").settings.font == "Fonts/LXGWWenKai-Regular.ttf");
    assert(read(root / "PicoPage/settings.toml") == config);
    const auto confirmed = app.canvas().pixels();
    d.tap(90, 820); d.present();
    assert(app.canvas().pixel(624, 669) == 1);
    // Reach both list boundaries, including a full first screen and its disabled control.
    for (int i = 0; i < 3; ++i) { d.tap(140, 1040); d.present(); }
    d.tap(140, 1040); assert(!app.pending_frame());
    d.tap(300, 660); d.present();
    d.tap(150, 1140); d.present();
    assert(app.canvas().pixels() == confirmed);
    assert(app.saved_record() == saved && read(root / "Books/book.epub.picopage/progress.json") == progress);
    // Only the typography panel's apply button persists the confirmed font.
    d.tap(500, 1130); d.present(); d.complete_content();
    const auto settings = store.load_settings("Books/book.epub").settings;
    assert(settings.font == "Fonts/12.ttf" && settings.font_weight == "regular" && settings.font_size_px == original_size + 2);
    assert(app.saved_record()->anchor == saved->anchor);
    assert(read(root / "PicoPage/settings.toml") == config);
    d.typography(); d.tap(90, 820); d.present();
    d.tap(150, 1140); d.present();
    d.close(); d.present();
    assert(app.view() == View::Reading);
}

static void font_picker_image_test(const fs::path& root, const fs::path& fixtures, const fs::path& font) {
    fs::create_directories(root / "Books"); fs::create_directories(root / "Fonts");
    fs::copy_file(fixtures / "image-cover.epub", root / "Books/book.epub");
    const auto local_font = root / "Fonts/LXGWWenKai-Regular.ttf";
    fs::copy_file(font, local_font);
    for (int i = 0; i < 6; ++i)
        fs::create_hard_link(local_font, root / ("Fonts/Picker" + std::to_string(i) + ".ttf"));
    ReaderApp app(root.string(), "font-picker-image", true, true); Driver d{app};
    assert(app.initialize()); d.present(); opened(app, "Books/book.epub"); d.present(); d.complete_content();
    const auto global_before = read(root / "PicoPage/settings.toml");
    const auto saved = app.saved_record();
    const auto progress = read(root / "Books/book.epub.picopage/progress.json");
    d.tap(342, 500); d.present();
    d.tap(205, 1140); d.present(); d.complete_content();
    assert(app.view() == View::Typography);
    d.tap(90, 820); d.present(); d.complete_content();
    assert(app.view() == View::Typography && !app.content_busy());
    const auto pixels = app.canvas().pixels();
    const auto decodes = graphics::image_decode_count();
    auto browse = [&](int x) {
        d.tap(x, 1040);
        // A cached image could avoid decoding yet still start a blocking preview job.
        assert(app.view() == View::Typography && !app.content_busy());
        assert(app.pending_frame() && app.pending_frame()->purpose == FramePurpose::Overlay);
        d.present();
        assert(graphics::image_decode_count() == decodes);
        assert(std::equal(pixels.begin(), pixels.begin() + 500 * app.canvas().stride(), app.canvas().pixels().begin()));
        assert(app.saved_record() == saved && read(root / "Books/book.epub.picopage/progress.json") == progress);
    };
    browse(540); d.refresh(); browse(140); browse(540);
    // A font change still requests a fresh preview; subsequent browsing must reuse its pixels.
    d.tap(300, 660);
    assert(app.view() == View::Loading && app.content_busy());
    d.present(); d.complete_content();
    const auto selected = app.canvas().pixels();
    const auto selected_decodes = graphics::image_decode_count();
    d.tap(140, 1040);
    assert(app.view() == View::Typography && !app.content_busy());
    d.present();
    assert(graphics::image_decode_count() == selected_decodes);
    assert(std::equal(selected.begin(), selected.begin() + 500 * app.canvas().stride(), app.canvas().pixels().begin()));
    d.tap(150, 1140); d.present(); d.complete_content();
    assert(app.view() == View::Typography);
    // Keep earlier size/weight edits across queued, running, completed and failed previews.
    d.tap(580, 1020); d.present(); d.complete_content();
    d.tap(240, 820); d.present(); d.complete_content();
    d.tap(580, 1020); d.present(); d.complete_content();
    const auto draft_pixels = app.canvas().pixels();
    auto assert_draft = [&] {
        assert(app.view() == View::Typography);
        assert(std::equal(draft_pixels.begin() + 605 * app.canvas().stride(), draft_pixels.end(),
                          app.canvas().pixels().begin() + 605 * app.canvas().stride()));
        assert(app.saved_record() == saved);
        assert(read(root / "Books/book.epub.picopage/progress.json") == progress);
        assert(read(root / "PicoPage/settings.toml") == global_before);
    };
    for (int mode = 0; mode < 4; ++mode) {
        d.tap(90, 820); d.present(); d.complete_content();
        if (mode == 1) ContentJob::delay_starts_for_test(ContentJob::Kind::Image, std::chrono::seconds(30));
        if (mode == 3) ContentJob::fail_starts_for_test(ContentJob::Kind::Image, 1, ContentJob::StartFailure::InternalMemory);
        d.tap(300, 760); d.present();
        if (mode == 1) app.poll_background();
        if (mode >= 2) d.complete_content();
        // Both the central loading cancel and the footer cancel return to the draft.
        d.tap(mode == 1 ? 300 : 150, mode == 1 ? 400 : 1140); d.present();
        assert_draft();
        d.complete_content();
        assert_draft();
        assert(!app.pending_frame());
        ContentJob::delay_starts_for_test(ContentJob::Kind::Image, std::chrono::milliseconds{});
        const auto cancelled = app.canvas().pixels();
        d.refresh();
        assert(app.canvas().pixels() == cancelled && !app.content_busy());
    }
    d.close(); d.present(); d.complete_content();
    assert(app.view() == View::Reading && app.saved_record() == saved);
    assert(read(root / "PicoPage/settings.toml") == global_before);
}

static void layout_review_test(const fs::path& root, const fs::path& fixtures, const fs::path& font, bool async) {
    fs::create_directories(root / "Books"); fs::create_directories(root / "Fonts");
    fs::copy_file(font, root / "Fonts/LXGWWenKai-Regular.ttf");
    if (async) fs::copy_file(font, root / "Fonts/LXGWWenKai-Medium.ttf");
    for (const auto name : {"a", "b", "c"}) fs::copy_file(fixtures / "valid.epub", root / (std::string("Books/") + name + ".epub"));
    FileStore store(root.string()); std::string detail;
    assert(store.save_last_book("Books/b.epub", detail, 0) == StoreStatus::Ok);
    assert(store.save_last_book("Books/a.epub", detail, 1704067200) == StoreStatus::Ok);
    ReaderApp app(root.string(), "layout-review", true, async); Driver driver{app};
    assert(app.initialize()); driver.present();
    assert(app.can_long_press_at(300, 220));
    // The resume card owns a; the first row must open b, even though c is unread.
    driver.tap(300, 480); driver.present(); driver.complete_content();
    assert(app.view() == View::Reading && store.load_last_book(detail) == std::optional<std::string>("Books/b.epub"));
    const auto saved = app.saved_record();
    const auto progress = read(root / "Books/b.epub.picopage/progress.json");
    driver.typography(); driver.tap(230, 820); driver.present();
    const auto regular = app.canvas().pixels();
    driver.tap(80, 1020); assert(!app.pending_frame());
    driver.tap(580, 1020); driver.present(); assert(app.canvas().pixels() != regular);
    driver.refresh(); driver.close(); driver.present();
    assert(app.saved_record() == saved && read(root / "Books/b.epub.picopage/progress.json") == progress);
    assert(store.load_settings("Books/b.epub").settings.font_weight == "regular");
    driver.typography(); driver.tap(230, 820); driver.present(); driver.tap(580, 1020); driver.present();
    driver.tap(500, 1130); driver.present(); driver.complete_content();
    assert(store.load_settings("Books/b.epub").settings.font_weight == "medium" && app.saved_record()->anchor == saved->anchor);
    if (async) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        while (app.background_due()) {
            app.poll_background(); app.background_step();
            if (app.pending_frame()) driver.present();
            assert(std::chrono::steady_clock::now() < deadline);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        bool native_cache = false;
        for (const auto& item : fs::directory_iterator(root / "PicoPage/cache/pages"))
            native_cache = native_cache || read(item.path()).find("Fonts/LXGWWenKai-Medium.ttf") != std::string::npos;
        assert(native_cache);
    }
    driver.tap(342, 500); driver.present(); driver.tap(60, 50); driver.present();
    driver.tap(380, 384); driver.present();
    assert(store.load_settings().settings.library_sort == "name");
    driver.tap(380, 384); driver.present(); assert(store.load_settings().settings.library_sort == "name_desc");
    write(root / "PicoPage/settings.toml", "schema_version=9\n");
    driver.tap(380, 384); driver.present();
    assert(app.status() == "排序未保存，请检查 TF 卡或设置文件" && read(root / "PicoPage/settings.toml") == "schema_version=9\n");
    write(root / "PicoPage/settings.toml", "schema_version=1\n[library]\nsort='recent'\n");
    const auto library_root = root / "LibraryReview";
    fs::create_directories(library_root / "Books/z-folder");
    for (const auto name : {"a", "b", "c"})
        fs::copy_file(fixtures / "valid.epub", library_root / (std::string("Books/") + name + ".epub"));
    FileStore library_store(library_root.string());
    assert(library_store.save_last_book("Books/b.epub", detail, 0) == StoreStatus::Ok);
    assert(library_store.save_last_book("Books/a.epub", detail, 0) == StoreStatus::Ok);
    {
        ReaderApp library(library_root.string(), "library-review", true, async); Driver d{library};
        assert(library.initialize()); d.present();
        assert(!library.can_long_press_at(300, 480));
        d.tap(600, 50); d.present(); d.tap(300, 560); d.present();
        assert(library.view() == View::InputSettings);
        d.tap(300, 260); d.present();
        assert(library_store.load_settings().settings.library_folders == "recent");
        d.tap(60, 50); d.present(); d.tap(60, 50); d.present();
        assert(library.view() == View::Library && library.can_long_press_at(300, 480));
    }
    write(library_root / "PicoPage/data/recent-books.txt", "invalid");
    write(library_root / "PicoPage/data/recent-books.txt.bak", "invalid");
    write(library_root / "Books/c.epub.picopage/progress.json", progress);
    {
        ReaderApp library(library_root.string(), "library-review", true, async); Driver d{library};
        assert(library.initialize()); d.present();
        assert(!library.can_long_press_at(300, 480));
        d.tap(600, 50); d.present(); d.tap(300, 560); d.present();
        d.tap(300, 260); d.present();
        assert(library.status() == "最近阅读记录无法读取，暂按书名排序");
        d.tap(60, 50); d.present(); d.tap(60, 50); d.present();
        d.tap(300, 610); d.present(); d.complete_content();
        assert(library_store.load_last_book(detail) == std::optional<std::string>("Books/a.epub"));
        const auto last = library_root / "PicoPage/data/last-book.txt";
        const auto stamp = fs::file_time_type::clock::now() - std::chrono::hours(24);
        fs::last_write_time(last, stamp);
        const auto recorded_stamp = fs::last_write_time(last);
        const auto saved = library.saved_record();
        for (int i = 0; i < 2; ++i) { d.key(Input::K3); d.present(); d.complete_content(); }
        assert(fs::last_write_time(last) == recorded_stamp);
        assert(library.saved_record() && !(library.saved_record() == saved));
    }
    const auto canvas = [&] {
        ReaderApp library(library_root.string(), "library-review", true, false); Driver d{library};
        assert(library.initialize()); d.present();
        return library.canvas().pixels();
    };
    const auto with_primary = canvas();
    fs::rename(library_root / "Books/c.epub.picopage/progress.json", library_root / "Books/c.epub.picopage/progress.json.bak");
    assert(canvas() == with_primary);
    fs::create_directory(library_root / "Books/c.epub.picopage/progress.json");
    assert(canvas() != with_primary);
    fs::remove(library_root / "Books/c.epub.picopage/progress.json");
    assert(canvas() == with_primary);
    fs::remove(library_root / "Books/c.epub.picopage/progress.json.bak");
    assert(canvas() != with_primary);
    if (async) {
        fs::create_directories(library_root / "Fonts");
        write(library_root / "PicoPage/data/recent-books.txt", "PPRECENT1\n");
        write(library_root / "PicoPage/data/recent-books.txt.bak", "PPRECENT1\n");
        fs::copy_file(font, library_root / "Fonts/Legacy-Regular.ttf");
        fs::copy_file(font, library_root / "Fonts/Legacy-Bold.ttf");
        write(library_root / "PicoPage/settings.toml", "schema_version=1\n[reader]\nfont='Fonts/Legacy-Bold.ttf'\n");
        ReaderApp legacy(library_root.string(), "legacy-weight", true, true); Driver d{legacy};
        assert(legacy.initialize()); d.present(); opened(legacy, "Books/b.epub"); d.present();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        while (legacy.background_due()) {
            legacy.poll_background(); legacy.background_step();
            if (legacy.pending_frame()) d.present();
            assert(std::chrono::steady_clock::now() < deadline);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        bool bold_cache = false;
        for (const auto& item : fs::directory_iterator(library_root / "PicoPage/cache/pages"))
            bold_cache = bold_cache || read(item.path()).find("Fonts/Legacy-Bold.ttf") != std::string::npos;
        assert(bold_cache);
        d.typography(); d.tap(230, 820); d.present(); d.tap(580, 1020); d.present();
        d.tap(500, 1130); d.present(); d.complete_content();
        const auto medium_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        while (legacy.background_due()) {
            legacy.poll_background(); legacy.background_step();
            if (legacy.pending_frame()) d.present();
            assert(std::chrono::steady_clock::now() < medium_deadline);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        bool regular_cache = false;
        for (const auto& item : fs::directory_iterator(library_root / "PicoPage/cache/pages"))
            regular_cache = regular_cache || read(item.path()).find("Fonts/Legacy-Regular.ttf") != std::string::npos;
        assert(regular_cache);
        fs::remove(library_root / "Books/b.epub.picopage/settings.toml");
        fs::remove(library_root / "Fonts/Legacy-Regular.ttf");
        write(library_root / "Books/weight.txt", "A plain text page for the unavailable weight notice.\n");
        for (const auto weight : {"medium", "light"}) {
            write(library_root / "PicoPage/settings.toml", std::string("schema_version=1\n[reader]\nfont='Fonts/Legacy-Bold.ttf'\nfont_weight='") + weight + "'\n");
            ReaderApp unavailable(library_root.string(), "weight-unavailable", true, false); Driver check{unavailable};
            assert(unavailable.initialize()); check.present(); opened(unavailable, "Books/weight.txt"); check.present();
            assert(unavailable.status() == "所选字重不可用，保留字体原有字重");
        }
        // A font saved during a transfer session is listed when the session ends, without a restart.
        fs::remove(library_root / "Fonts/Legacy-Bold.ttf");
        fs::copy_file(font, library_root / "Fonts/Legacy-Regular.ttf");
        fs::remove_all(library_root / "PicoPage/cache/pages");
        write(library_root / "PicoPage/settings.toml", "schema_version=1\n[reader]\nfont='Fonts/Legacy-Regular.ttf'\nfont_weight='bold'\n");
        ReaderApp receiver(library_root.string(), "font-upload", true, true); Driver r{receiver};
        assert(receiver.initialize()); r.present();
        assert(receiver.prepare_portal()); r.present();
        r.portal({PortalPhase::Waiting, "PicoPage-0000", "12345678", "http://192.168.4.1", "", "", 0, ""}); r.present();
        fs::copy_file(font, library_root / "Fonts/Legacy-Bold.ttf");
        fs::copy_file(font, library_root / "Fonts/._Legacy-Bold.ttf");
        r.portal({PortalPhase::Closed, "", "", "", "", "", 0, ""}); r.present();
        assert(receiver.view() == View::Library);
        opened(receiver, "Books/b.epub"); r.present();
        const auto upload_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        while (receiver.background_due()) {
            receiver.poll_background(); receiver.background_step();
            if (receiver.pending_frame()) r.present();
            assert(std::chrono::steady_clock::now() < upload_deadline);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        bool uploaded_cache = false;
        for (const auto& item : fs::directory_iterator(library_root / "PicoPage/cache/pages"))
            uploaded_cache = uploaded_cache || read(item.path()).find("Fonts/Legacy-Bold.ttf") != std::string::npos;
        assert(uploaded_cache);
    }
}

static void release_review_test(const fs::path& root, const fs::path& fixtures, bool async) {
    fs::create_directories(root / "Books");
    ReaderApp app(root.string(), "release-review", true, async); Driver driver{app};
    assert(app.initialize()); driver.present();
    // An empty root library lists the ways to add books; the third row only describes one.
    driver.tap(300, 620); assert(!app.pending_frame() && app.view() == View::Library);
    driver.tap(300, 350); assert(app.take_usb_transfer_action() == UsbTransferAction::Start);
    driver.tap(300, 790); driver.present(); assert(app.view() == View::Help);
    driver.tap(60, 50); driver.present(); assert(app.view() == View::Library);
    driver.tap(600, 50); driver.present(); assert(app.view() == View::Settings);
    driver.tap(300, 1030); driver.present(); assert(app.view() == View::About);
    driver.refresh();
    driver.tap(60, 50); driver.present(); assert(app.view() == View::Settings);
    driver.tap(60, 50); driver.present(); assert(app.view() == View::Library);
    // A hotspot that never started reports its reason on the network page, which leads back here.
    driver.tap(300, 480); assert(app.take_portal_action() == PortalAction::Upload);
    driver.portal({PortalPhase::Closed, "", "", "", "设备热点没有启动，请重试。", "", 0, ""}); driver.present();
    assert(app.view() == View::Import && app.status() == "设备热点没有启动，请重试。");
    driver.tap(60, 50); driver.present(); assert(app.view() == View::Library);

    fs::copy_file(fixtures / "valid.epub", root / "Books/book.epub");
    opened(app, "Books/book.epub"); driver.present();
    driver.tap(342, 500); driver.present(); assert(app.view() == View::Tools);
    // The strip between the body and the toolbar closes the tools; the last cell ends at the edge.
    driver.tap(342, 1046); driver.present(); driver.complete_content(); assert(app.view() == View::Reading);
    driver.tap(342, 500); driver.present(); assert(app.view() == View::Tools);
    driver.tap(683, 1150);
    assert(app.pending_frame() && app.pending_frame()->purpose == FramePurpose::ManualRefresh);
    driver.present(); driver.close(); driver.present(); driver.complete_content();

    // Returning from a failed commit preserves the presented body and its unsaved position.
    const auto save = [&] {
        app.save_progress();
        while (app.save_busy()) { app.poll_background(); std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
        if (app.pending_frame()) driver.present();
    };
    const auto saved = app.saved_record();
    const auto sidecar = root / "Books/book.epub.picopage";
    fs::remove_all(sidecar); write(sidecar, "not a directory");
    driver.key(Input::K3); driver.present(); driver.complete_content(); save();
    assert(app.view() == View::Error && app.saved_record() == saved);
    driver.tap(100, 1100); assert(app.view() == View::Reading); app.complete_frame(true);
    app.show_error("进度未保存。请重试。"); driver.present();
    driver.tap(500, 1120); driver.present(); assert(app.view() == View::Error && app.saved_record() == saved);
    fs::remove(sidecar);
    driver.tap(500, 1120); driver.present(); driver.complete_content();
    assert(app.view() == View::Reading && !(app.saved_record() == saved));

    // With Wi-Fi off, the upload row opens the network page, which returns to the library.
    const fs::path offline = root.string() + "Offline";
    fs::create_directories(offline / "Books");
    write(offline / "PicoPage/settings.toml", "schema_version=1\n[network]\nwifi_enabled=false\n");
    ReaderApp disabled(offline.string(), "release-review-offline", true, async); Driver offline_driver{disabled};
    assert(disabled.initialize()); offline_driver.present();
    offline_driver.tap(300, 480); offline_driver.present();
    assert(disabled.view() == View::Import && disabled.take_portal_action() == PortalAction::None);
    // Wi-Fi settings opened from that page return to it, also after Wi-Fi is turned on.
    offline_driver.tap(342, 700); offline_driver.present(); assert(disabled.view() == View::WifiSettings);
    offline_driver.tap(300, 288); offline_driver.present(); assert(disabled.wifi_enabled());
    offline_driver.tap(60, 50); offline_driver.present(); assert(disabled.view() == View::Import);
    offline_driver.tap(60, 50); offline_driver.present(); assert(disabled.view() == View::Library);

    // A long library directory keeps its last components; the leading ones give way to the instruction.
    const auto first_use = [&](const char* name, const std::string& library) {
        const fs::path card = root.string() + name;
        fs::create_directories(card / library);
        write(card / "PicoPage/settings.toml", "schema_version=1\n[library]\nroots=['" + library + "']\n");
        ReaderApp app(card.string(), "release-review-roots", true, async); Driver driver{app};
        assert(app.initialize()); driver.present();
        assert(app.view() == View::Library);
        return app.canvas().pixels();
    };
    const auto trimmed = first_use("RootA", std::string(40, 'a') + "/Shelf/Books");
    assert(trimmed == first_use("RootB", std::string(40, 'b') + "/Shelf/Books"));
    assert(trimmed != first_use("RootC", std::string(40, 'a') + "/Shelf/Texts"));
}
static void shot(const char* name, const ReaderApp& shown) {
    const char* folder = std::getenv("PICO_PAGE_UI_SHOTS");
    if (!folder) return;
    fs::create_directories(folder);
    std::ofstream file(fs::path(folder) / (std::string(name) + ".pgm"), std::ios::binary);
    file << "P5\n684 1216\n255\n";
    for (int y = 0; y < 1216; ++y) for (int x = 0; x < 684; ++x) file.put(static_cast<char>(shown.canvas().pixel(x, y) * 17));
    assert(file.good());
}

// A new card, a missing or blocked library root and an absent card each have their own page.
static void library_root_state_test(const fs::path& base, bool async) {
    const auto open = [&](const fs::path& card, const char* name) {
        if (!fs::exists(card / "PicoPage/settings.toml")) write(card / "PicoPage/settings.toml", "schema_version=1\n[onboarding]\ndone=true\n");
        auto app = std::make_unique<ReaderApp>(card.string(), "library-root-state", true, async);
        Driver driver{*app}; assert(app->initialize()); driver.present();
        assert(app->view() == View::Library); shot(name, *app);
        return app;
    };
    const auto fresh = base / "Fresh"; fs::create_directories(fresh);
    const auto fresh_pixels = open(fresh, "library-new-card")->canvas().pixels();
    for (const auto* directory : {"Books", "Fonts", "LockScreens", "Dictionaries"})
        assert(fs::is_directory(fresh / directory));
    // Existing resource directories and their contents survive repeated initialization.
    const auto existing = base / "Existing"; fs::create_directories(existing / "Books");
    write(existing / "Fonts/license.txt", "font license");
    write(existing / "LockScreens/notes.txt", "lock screen notes");
    write(existing / "Dictionaries/license.txt", "dictionary license");
    for (int i = 0; i < 2; ++i) {
        assert(open(existing, "library-empty")->canvas().pixels() == fresh_pixels);
        assert(read(existing / "Fonts/license.txt") == "font license");
        assert(read(existing / "LockScreens/notes.txt") == "lock screen notes");
        assert(read(existing / "Dictionaries/license.txt") == "dictionary license");
    }

    const auto custom = base / "Custom";
    write(custom / "PicoPage/settings.toml", "schema_version=1\n[library]\nroots=['Shelf']\n[onboarding]\ndone=true\n");
    const auto custom_pixels = open(custom, "library-custom-missing")->canvas().pixels();
    assert(!fs::exists(custom / "Shelf") && !fs::exists(custom / "Books") && custom_pixels != fresh_pixels);

    for (const auto* directory : {"Fonts", "LockScreens", "Dictionaries"})
        assert(fs::is_directory(custom / directory));

    for (const auto* directory : {"Fonts", "LockScreens", "Dictionaries"}) {
        const auto blocked = base / (std::string("Blocked-") + directory);
        write(blocked / directory, "not a directory");
        assert(open(blocked, "library-asset-file")->canvas().pixels() == fresh_pixels);
        assert(read(blocked / directory) == "not a directory");
        for (const auto* other : {"Fonts", "LockScreens", "Dictionaries"})
            if (std::string(other) != directory) assert(fs::is_directory(blocked / other));
    }

    const auto absent = base / "Absent";
    { ReaderApp app(absent.string(), "library-no-card", true, async); assert(app.initialize()); }
    assert(!fs::exists(absent));

    const auto file = base / "File"; write(file / "Books", "not a directory");
    const auto file_pixels = open(file, "library-not-directory")->canvas().pixels();
    assert(read(file / "Books") == "not a directory" && file_pixels != fresh_pixels && file_pixels != custom_pixels);

    // A recent-sorted library that omits the resumed book still explains a removed card.
    const auto recent = base / "Recent"; write(recent / "Books/a.txt", "book");
    write(recent / "PicoPage/settings.toml", "schema_version=1\n[library]\nsort='recent'\n[onboarding]\ndone=true\n");
    { std::string detail; assert(FileStore(recent.string()).save_last_book("Books/a.txt", detail) == StoreStatus::Ok); }
    {
        auto app = open(recent, "library-recent");
        Driver driver{*app};
        app->card_unavailable(CardIssue::Removed); driver.present();
        for (int y = 400; y < 1000 && app->view() != View::PowerMenu; y += 24) {
            driver.tap(300, y);
            if (app->pending_frame()) driver.present();
        }
        assert(app->view() == View::PowerMenu);
    }

    if (geteuid() != 0) {
        // A Books directory that cannot be created does not hide the other configured roots.
        const auto shared = base / "SharedLocked"; write(shared / "Shelf/b.txt", "book");
        write(shared / "PicoPage/settings.toml", "schema_version=1\n[library]\nroots=['Books','Shelf']\n[onboarding]\ndone=true\n");
        fs::permissions(shared, fs::perms::owner_read | fs::perms::owner_exec);
        {
            auto app = open(shared, "library-roots-read-only");
            Driver driver{*app};
            driver.tap(300, 250); assert(app->pending_frame());
        }
        fs::permissions(shared, fs::perms::owner_all);
        assert(!fs::exists(shared / "Books"));

        const auto asset_locked = base / "AssetsLocked"; fs::create_directories(asset_locked / "Books");
        write(asset_locked / "PicoPage/settings.toml", "schema_version=1\n[onboarding]\ndone=true\n");
        fs::permissions(asset_locked, fs::perms::owner_read | fs::perms::owner_exec);
        assert(open(asset_locked, "library-assets-read-only")->canvas().pixels() == fresh_pixels);
        fs::permissions(asset_locked, fs::perms::owner_all);
        for (const auto* directory : {"Fonts", "LockScreens", "Dictionaries"})
            assert(!fs::exists(asset_locked / directory));

        const auto locked = base / "Locked"; fs::create_directories(locked);
        write(locked / "PicoPage/settings.toml", "schema_version=1\n[onboarding]\ndone=true\n");
        fs::permissions(locked, fs::perms::owner_read | fs::perms::owner_exec);
        auto app = open(locked, "library-read-only");
        fs::permissions(locked, fs::perms::owner_all);
        assert(!fs::exists(locked / "Books") && app->canvas().pixels() != fresh_pixels);
    }

    // A failed start-up mount offers the power menu, because remounting needs a restart.
    std::vector<std::vector<std::uint8_t>> pages;
    for (const auto issue : {CardIssue::NotInserted, CardIssue::NoFilesystem, CardIssue::Unreadable, CardIssue::Removed}) {
        auto app = open(existing, "library-before-card");
        Driver driver{*app};
        app->card_unavailable(issue); driver.present();
        assert(app->view() == View::Library);
        static const char* names[] = {"card-removed", "card-not-inserted", "card-no-filesystem", "card-unreadable"};
        shot(names[static_cast<int>(issue)], *app);
        pages.push_back(app->canvas().pixels());
        driver.tap(450, 50); assert(!app->pending_frame());
        // The button follows the steps, whose number differs between states.
        for (int y = 400; y < 1000 && app->view() != View::PowerMenu; y += 24) {
            driver.tap(300, y);
            if (app->pending_frame()) driver.present();
        }
        assert(app->view() == View::PowerMenu);
    }
    for (std::size_t i = 0; i < pages.size(); ++i)
        for (std::size_t j = i + 1; j < pages.size(); ++j) assert(pages[i] != pages[j]);
}

#ifdef PICO_PAGE_WRAP_LIBRARY_IO_TEST
static void library_scan_failure_test(const fs::path& base) {
    for (int mode = 0; mode < 7; ++mode) {
        const auto root = base / std::to_string(mode);
        fs::create_directories(root / "Books");
        write(root / "PicoPage/settings.toml", "schema_version=1\n[library]\nsort='name'\nview='list'\n");
        const auto config = read(root / "PicoPage/settings.toml");
        if (mode != 5) {
            write(root / "Books/a.txt", "first book");
            write(root / "Books/b.txt", "second book");
        }
        if (mode == 6) for (int i = 0; i < 511; ++i)
            write(root / ("Books/extra" + std::to_string(i) + ".txt"), "book");
        struct stat info{};
        assert(stat((root / "Books").c_str(), &info) == 0);
        library_fault_inode = info.st_ino; library_fault_device = info.st_dev;
        library_read_entries = 0; library_read_failure_after = mode < 2 ? mode : -1;
        library_stat_failure_path = (root / "Books/a.txt").string();
        library_stat_failure = mode == 2 ? EIO : mode == 3 ? ENOENT : 0;
        library_dirty_errno = mode == 4 || mode == 5;
        ReaderApp app(root.string(), "library-scan-failure", true, true); Driver d{app};
        assert(app.initialize()); d.present();
        if (mode == 0) {
            // Nothing could be listed: the page offers a retry that scans again.
            assert(app.status().empty());
            const auto failed = app.canvas().pixels();
            const auto scanned = library_read_entries;
            d.refresh();
            assert(app.canvas().pixels() == failed && library_read_entries == scanned);
            library_read_failure_after = -1; library_stat_failure = 0;
            d.tap(300, 570); d.present();
            assert(app.view() == View::Library && app.status().empty());
            assert(app.canvas().pixels() != failed);
        } else if (mode < 3) {
            // A partial listing keeps the entries readable and says the list may be incomplete.
            assert(app.status() == "部分文件读取失败，列表可能不完整");
        } else if (mode == 6) {
            assert(app.status() == "当前目录仅显示前 512 项，请分目录存放");
            assert(library_read_entries == 512);
        } else assert(app.status().empty());
        assert(read(root / "PicoPage/settings.toml") == config);
        library_fault_inode = 0; library_stat_failure = 0; library_dirty_errno = false;
    }
}
#endif

static void library_resume_pagination_test(const fs::path& root, const fs::path& fixtures, bool grid, bool async) {
    fs::create_directories(root / "Books");
    fs::create_directories(root / "Archive");
    fs::copy_file(fixtures / "valid.epub", root / "Archive/resume.epub");
    const int page_size = grid ? 6 : 5, books = page_size * 2 + 1;
    const auto book_path = [](int index) {
        return "Books/book" + std::string(index < 10 ? "0" : "") + std::to_string(index) + ".epub";
    };
    for (int i = 0; i < books; ++i) fs::copy_file(fixtures / "valid.epub", root / book_path(i));
    write(root / "PicoPage/settings.toml", std::string("schema_version=1\n[library]\nsort='name'\nview='") +
          (grid ? "grid" : "list") + "'\n");
    FileStore store(root.string()); std::string detail;
    for (int target = 0; target < 3; ++target) {
        assert(store.save_last_book("Archive/resume.epub", detail, 0) == StoreStatus::Ok);
        ReaderApp app(root.string(), "library-resume-pages", true, async); Driver driver{app};
        assert(app.initialize()); driver.present();
        const auto first = app.canvas().pixels();
        const auto stride = app.canvas().stride();
        const auto card_begin = first.begin() + 128 * stride, card_end = first.begin() + 334 * stride;
        for (int page = 0; page < target; ++page) {
            if (page == 0) driver.key(Input::K3); else driver.swipe(true);
            assert(app.pending_frame() && app.pending_frame()->purpose == FramePurpose::Overlay);
            assert(app.pending_frame()->refresh == RefreshIntent::Content && !app.pending_frame()->region);
            assert(app.pending_frame()->changed_pixels_only);
            driver.present();
            assert(std::equal(card_begin, card_end, app.canvas().pixels().begin() + 128 * stride));
            assert(app.can_long_press_at(300, 220));
        }
        if (target == 2) {
            driver.refresh();
            const auto last = app.canvas().pixels();
            driver.tap(600, 1160); assert(!app.pending_frame() && app.canvas().pixels() == last);
            driver.key(Input::K1); driver.present();
            driver.tap(600, 1160); driver.present(); assert(app.canvas().pixels() == last);
            assert(!app.can_long_press_at(grid ? 300 : 100, grid ? 700 : 640));
        }
        const int slot = target == 2 ? 0 : page_size - 1;
        const int x = grid ? 36 + (slot % 3) * 214 + 80 : 100;
        const int y = grid ? 466 + (slot / 3) * 320 + 100 : 444 + slot * 126 + 60;
        driver.tap(x, y); driver.present(); driver.complete_content();
        assert(app.view() == View::Reading);
        assert(store.load_last_book(detail) == std::optional<std::string>(book_path(target * page_size + slot)));
    }
    assert(store.save_last_book("Archive/resume.epub", detail, 0) == StoreStatus::Ok);
    ReaderApp app(root.string(), "library-resume-open", true, async); Driver driver{app};
    assert(app.initialize()); driver.present();
    driver.key(Input::K3); driver.present(); driver.key(Input::K3); driver.present();
    driver.tap(300, 220); driver.present(); driver.complete_content();
    assert(app.view() == View::Reading && store.load_last_book(detail) == std::optional<std::string>("Archive/resume.epub"));
}

static void swipe_test(const fs::path& root, const fs::path& fixtures, bool grid) {
    fs::create_directories(root / "Books");
    for (int i = 0; i < 15; ++i)
        fs::copy_file(fixtures / "swipe.epub", root / ("Books/book" + std::to_string(i) + ".epub"));
    write(root / "PicoPage/settings.toml", std::string("schema_version = 1\n[library]\nview = '") + (grid ? "grid" : "list") + "'\n");
    ReaderApp app(root.string(), "swipe-test", true, true); Driver driver{app};
    assert(app.initialize()); driver.present();
    const auto first = app.canvas().pixels();
    driver.swipe(false); assert(!app.pending_frame() && app.canvas().pixels() == first);
    assert(app.can_long_press_at(300, 480));
    driver.swipe(true); assert(app.view() == View::Library); driver.present();
    assert(app.canvas().pixels() != first && !app.saved_record());
    driver.swipe(true); driver.present();
    const auto last = app.canvas().pixels();
    driver.swipe(true); assert(!app.pending_frame() && app.canvas().pixels() == last);
    driver.swipe(false); driver.present(); driver.swipe(false); driver.present();
    assert(app.canvas().pixels() == first);
    opened(app, "Books/book0.epub"); driver.present();
    const auto saved = app.saved_record();
    const auto progress = read(root / "Books/book0.epub.picopage/progress.json");
    driver.swipe(true);
    assert(app.pending_frame() && app.saved_record() == saved);
    assert(app.pending_frame()->purpose == FramePurpose::Body && !app.pending_frame()->changed_pixels_only);
    app.complete_frame(true);
    assert(app.save_due() && app.saved_record() == saved);
    assert(read(root / "Books/book0.epub.picopage/progress.json") == progress);
    driver.swipe(true, 300, 480, true);
    assert(app.pending_frame()); driver.present();
    assert(app.saved_record()->anchor != saved->anchor);
    driver.tap(342, 500); driver.present(); assert(app.view() == View::Tools);
    const auto after_turn = app.saved_record();
    for (bool next : {false, true}) driver.swipe(next);
    assert(!app.pending_frame() && app.view() == View::Tools && app.saved_record() == after_turn);
    driver.tap(205, 1140); driver.present(); assert(app.view() == View::Typography);
    const auto preview = app.canvas().pixels();
    for (bool next : {false, true}) driver.swipe(next);
    assert(!app.pending_frame() && app.canvas().pixels() == preview && app.saved_record() == after_turn);
    driver.close(); driver.present();
    if (app.view() == View::Reading) { driver.tap(342, 500); driver.present(); }
    driver.tap(68, 1140); driver.present(); assert(app.view() == View::Contents);
    const auto contents = app.canvas().pixels();
    driver.swipe(true);
    assert(app.pending_frame() && app.pending_frame()->purpose == FramePurpose::Overlay);
    assert(app.pending_frame()->refresh == RefreshIntent::Content && app.pending_frame()->changed_pixels_only);
    driver.present(); assert(app.view() == View::Contents && app.canvas().pixels() != contents);
    driver.swipe(false);
    assert(app.pending_frame() && app.pending_frame()->changed_pixels_only);
    driver.present(); assert(app.view() == View::Contents && app.canvas().pixels() == contents);
    assert(app.saved_record() == after_turn);
}

static std::vector<std::uint8_t> footer_pixels(const ReaderApp& app, int left, int right) {
    std::vector<std::uint8_t> pixels;
    for (int y = 1128; y < 1216; ++y)
        for (int x = left; x < right; ++x) pixels.push_back(app.canvas().pixel(x, y));
    return pixels;
}

static void footer_test(const fs::path& root, const fs::path& fixtures, bool asynchronous,
                        const std::string& fixture = "valid.epub", const std::string& title = "第一章") {
    fs::create_directories(root / "Books");
    fs::copy_file(fixtures / fixture, root / "Books/book.epub");
    ReaderApp app(root.string(), "footer-test", true, asynchronous); Driver driver{app};
    assert(app.initialize()); driver.present();
    app.set_battery(75);
    opened(app, "Books/book.epub"); driver.present();
    // Dismiss any first-open notice, then return to the default page-count mode.
    for (int i = 0; i < 5; ++i) { driver.tap(36, 1160); driver.present(); }
    if (fixture == "empty-label.epub" || fixture == "whitespace-label.epub") {
        driver.tap(342, 500); driver.present();
        assert(app.view() == View::Tools);
        graphics::Canvas expected;
        graphics::Font font; assert(font.open_fallback() && font.set_pixel_size(30));
        int x = 132;
        for (auto scalar : graphics::decode_utf8(title)) {
            font.draw(expected, scalar, x, 14 + font.metrics().ascender, 1, true);
            x += font.advance(scalar);
        }
        for (int y = 14; y < 55; ++y)
            for (int column = 132; column < 400; ++column)
                assert(app.canvas().pixel(column, y) == expected.pixel(column, y));
        driver.close(); driver.present();
    }
    const auto saved = app.saved_record();
    const auto progress = read(root / "Books/book.epub.picopage/progress.json");
    const auto body = app.canvas().pixels();
    const auto battery = footer_pixels(app, 540, 684);
    const auto page_waiting = footer_pixels(app, 0, 540);
    const auto unchanged = [&]() {
        assert(app.view() == View::Reading && app.saved_record() == saved && !app.save_due());
        assert(read(root / "Books/book.epub.picopage/progress.json") == progress);
        assert(std::equal(body.begin(), body.begin() + 1128 * app.canvas().stride(), app.canvas().pixels().begin()));
        assert(!app.take_network_request());
    };
    const auto expect_text = [&](const std::string& value) {
        graphics::Canvas expected;
        graphics::Font font; assert(font.open_fallback() && font.set_pixel_size(24));
        int x = 36, width = 0;
        const auto scalars = graphics::decode_utf8(value);
        for (auto scalar : scalars) width += font.advance(scalar);
        const int available = width > 476 ? 476 - font.advance(U'…') : 476;
        for (const auto scalar : scalars) {
            if (x - 36 + font.advance(scalar) > available) {
                font.draw(expected, U'…', x, 1156 + font.metrics().ascender, 3);
                break;
            }
            font.draw(expected, scalar, x, 1156 + font.metrics().ascender, 3);
            x += font.advance(scalar);
        }
        for (int y = 1128; y < 1216; ++y)
            for (int column = 0; column < 540; ++column)
                assert(app.canvas().pixel(column, y) == expected.pixel(column, y));
    };
    const auto toggle = [&](int x) {
        driver.tap(x, 1160);
        assert(app.pending_frame() && app.pending_frame()->purpose == FramePurpose::Overlay);
        assert(app.pending_frame()->refresh == RefreshIntent::Content && app.pending_frame()->region);
        assert(app.pending_frame()->changed_pixels_only);
        const auto region = *app.pending_frame()->region;
        assert(region.x == 0 && region.y == 1128 && region.width == 684 && region.height == 88);
        const auto frame = app.pending_frame()->token;
        const auto pixels = app.canvas().pixels();
        driver.tap(x == 600 ? 36 : 600, 1160);
        assert(app.pending_frame()->token == frame && app.canvas().pixels() == pixels);
        driver.present(); unchanged();
    };
    toggle(36);
    const auto estimated = footer_pixels(app, 0, 540);
    assert(estimated != page_waiting && footer_pixels(app, 540, 684) == battery);
    toggle(540);
    assert(footer_pixels(app, 0, 540) == estimated && footer_pixels(app, 540, 684) != battery);
    toggle(600);
    const auto hidden_device = footer_pixels(app, 540, 684);
    assert(std::all_of(hidden_device.begin(), hidden_device.end(), [](auto pixel) { return pixel == 15; }));
    assert(footer_pixels(app, 0, 540) == estimated);
    toggle(600);
    assert(footer_pixels(app, 0, 540) == estimated && footer_pixels(app, 540, 684) == battery);
    toggle(539);
    const auto chapters = footer_pixels(app, 0, 540);
    assert(chapters != estimated && footer_pixels(app, 540, 684) == battery);
    expect_text("第 1 章 / 共 2 章");
    toggle(36);
    const auto chapter_name = footer_pixels(app, 0, 540);
    assert(chapter_name != chapters && footer_pixels(app, 540, 684) == battery);
    expect_text(title);
    toggle(36);
    expect_text("");
    assert(footer_pixels(app, 540, 684) == battery);
    toggle(600); toggle(600);
    assert(footer_pixels(app, 540, 684) == hidden_device);
    const auto hidden_pixels = app.canvas().pixels();
    driver.refresh(); unchanged();
    assert(app.canvas().pixels() == hidden_pixels);
    if (fixture == "valid.epub") {
        assert(app.begin_sleep()); driver.present();
        assert(app.canvas().pixels() != hidden_pixels);
        app.cancel_sleep(); driver.present(); unchanged();
        assert(app.canvas().pixels() == hidden_pixels);
    }
    app.set_battery(75, true);
    assert(!app.pending_frame() && app.canvas().pixels() == hidden_pixels);
    app.set_battery(75, false);
    assert(!app.pending_frame() && app.canvas().pixels() == hidden_pixels);
    toggle(600);
    expect_text("");
    toggle(36);
    assert(footer_pixels(app, 0, 540) == page_waiting && footer_pixels(app, 540, 684) == battery);
    const auto pixels = app.canvas().pixels();
    driver.drag(600, 1160, {530, 1150, 610, 1180});
    driver.drag(100, 1160, {90, 1120, 110, 1180});
    driver.queued(Input::TouchCenter, true, 36, 1160);
    driver.queued(Input::TouchCenter, true, 600, 1160);
    assert(!app.pending_frame() && app.canvas().pixels() == pixels);
    driver.key(Input::K2);
    assert(app.pending_frame() && app.pending_frame()->refresh == RefreshIntent::FullClean && !app.pending_frame()->region);
    assert(!app.pending_frame()->changed_pixels_only);
    driver.present(); unchanged();

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (app.background_due()) {
        app.background_step(); app.poll_background();
        assert(!app.pending_frame() && std::chrono::steady_clock::now() < deadline);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const auto counted = app.book_pages();
    assert(counted && counted->total > 0 && app.canvas().pixels() == pixels);
    toggle(36);
    // Check the rendered text against the known page fraction, including its fractional digits.
    const auto value = reading_footer::percent(static_cast<double>(counted->page) / counted->total);
    expect_text(value);
    toggle(600);
    toggle(36);
    assert(footer_pixels(app, 0, 540) != page_waiting);
    toggle(600);
    toggle(600);
    assert(footer_pixels(app, 540, 684) == battery);
    unchanged();

    const auto position = footer_pixels(app, 0, 540);
    const auto before_charging = app.canvas().pixels();
    app.set_battery(75, true, false);
    assert(!app.pending_frame() && app.canvas().pixels() == before_charging);
    driver.key(Input::K2);
    assert(app.pending_frame() && app.pending_frame()->purpose == FramePurpose::ManualRefresh);
    assert(app.canvas().pixels() == before_charging);
    driver.present(); unchanged();
    app.set_battery(75, true);
    assert(app.pending_frame() && app.pending_frame()->region && app.pending_frame()->changed_pixels_only);
    assert(app.pending_frame()->purpose == FramePurpose::Overlay && app.pending_frame()->region->y == 1128);
    const auto charging = footer_pixels(app, 540, 684);
    assert(charging != battery && footer_pixels(app, 0, 540) == position);
    driver.present(); unchanged();
    app.set_battery(75, true);
    assert(!app.pending_frame());
    toggle(600);
    const auto charging_clock = footer_pixels(app, 540, 684);
    assert(charging_clock != charging);
    const auto clock_pixels = app.canvas().pixels();
    app.set_battery(75, false);
    assert(!app.pending_frame() && app.canvas().pixels() == clock_pixels);
    app.set_battery(75, true);
    assert(!app.pending_frame() && app.canvas().pixels() == clock_pixels);
    toggle(600);
    assert(footer_pixels(app, 540, 684) == hidden_device);
    toggle(600);
    assert(footer_pixels(app, 540, 684) == charging);
    app.set_battery(75, false);
    assert(app.pending_frame());
    const auto frame_pixels = app.canvas().pixels();
    app.set_battery(75, true);
    assert(app.canvas().pixels() == frame_pixels);
    driver.present(); unchanged();
    app.set_battery(75, true);
    assert(app.pending_frame() && footer_pixels(app, 540, 684) == charging);
    driver.present(); unchanged();
    app.set_battery(75, false);
    assert(app.pending_frame() && footer_pixels(app, 540, 684) == battery);
    driver.present(); unchanged();
    app.set_battery(75, false);
    assert(!app.pending_frame());

    app.set_battery(std::nullopt, true);
    assert(app.pending_frame());
    const auto unknown_charging = footer_pixels(app, 540, 684);
    driver.present(); unchanged();
    app.set_battery(std::nullopt, false);
    assert(app.pending_frame() && footer_pixels(app, 540, 684) != unknown_charging);
    driver.present(); unchanged();
    app.set_battery(75, false);
    toggle(600); toggle(600); toggle(600);
    assert(footer_pixels(app, 540, 684) == battery);

    if (fixture == "nonlinear-footer.epub") {
        driver.tap(342, 500); driver.present();
        driver.tap(68, 1140); driver.present();
        assert(app.view() == View::Contents);
        driver.tap(300, 522 + 92 + 46); driver.present(); driver.complete_content();
        assert(app.view() == View::Reading && app.saved_record()->anchor.resource.find("chapter2") != std::string::npos);
        // A complete cycle clears notices before returning from chapters to percentage.
        for (int i = 0; i < 9; ++i) { driver.tap(36, 1160); driver.present(); }
        expect_text("100.00%");
        return;
    }
    if (fixture == "valid.epub") {
        assert(app.begin_sleep()); driver.present();
        const auto sleep_pixels = app.canvas().pixels();
        app.set_battery(75, true);
        assert(!app.pending_frame() && app.canvas().pixels() == sleep_pixels);
        app.cancel_sleep(); driver.present(); unchanged();
        app.set_battery(75, true);
    } else driver.tap(36, 1160);
    assert(app.pending_frame() && app.pending_frame()->region);
    app.complete_frame(false);
    assert(app.view() == View::Error && app.saved_record() == saved);
    assert(app.status() == "屏幕局部更新失败。返回将恢复已确认位置。");
    assert(app.pending_frame() && app.pending_frame()->refresh == RefreshIntent::FullClean && !app.pending_frame()->region);
    assert(read(root / "Books/book.epub.picopage/progress.json") == progress);
    driver.present();
}

static void lock_screen_test(const fs::path& root, const fs::path& fixtures) {
    std::vector<std::uint8_t> encoded;
    std::string detail;
    assert(EpubDocument::read_cover((fixtures / "image-cover.epub").string(), encoded, detail));
    const std::string png(reinterpret_cast<const char*>(encoded.data()), encoded.size());
    {
        const auto card = root / "SelectionLayout";
        const std::string long_name = "06-用于验证单行省略且不覆盖保存状态的长文件名.png";
        for (const auto& name : {std::string("01.png"), std::string("02.png"), std::string("03.png"),
                                 std::string("04.png"), std::string("05.png"), long_name})
            write(card / "LockScreens" / name, png);
        ReaderApp app(card.string(), "selection-layout", true, true); Driver driver{app};
        assert(app.initialize()); driver.present();
        driver.tap(600, 50); driver.present(); driver.tap(340, 640); driver.present();
        FileStore settings_store(card.string());
        const auto saved_settings = read(card / "PicoPage/settings.toml");
        const std::pair<int, int> cell_hits[] = {{341, 283}, {342, 192}, {36, 284}, {647, 375}};
        for (int selected = 0; selected < 4; ++selected) {
            driver.tap(cell_hits[selected].first, cell_hits[selected].second); driver.present();
            for (int cell = 0; cell < 4; ++cell)
                assert(app.canvas().pixel(44 + (cell % 2) * 306, 200 + (cell / 2) * 92) == (cell == selected ? 1 : 15));
            assert(read(card / "PicoPage/settings.toml") == saved_settings);
        }
        const auto overlay_off = app.canvas().pixels();
        driver.tap(24, 552); driver.present();
        assert(app.canvas().pixels() != overlay_off && !settings_store.load_settings().settings.sleep_overlay);
        driver.refresh();
        driver.tap(659, 655); driver.present(); assert(app.canvas().pixels() == overlay_off);
        driver.tap(24, 552); driver.present(); driver.tap(150, 1120); driver.present();
        assert(read(card / "PicoPage/settings.toml") == saved_settings);
        driver.tap(340, 640); driver.present();
        assert(app.canvas().pixel(44, 200) == 1);
        driver.tap(647, 375); driver.present(); driver.tap(659, 655); driver.present();
        driver.tap(500, 1120); driver.present();
        assert(settings_store.load_settings().settings.sleep_screen == "custom" && settings_store.load_settings().settings.sleep_overlay);
        driver.tap(340, 640); driver.present(); driver.tap(150, 450); driver.present(); driver.complete_content();
        assert(app.view() == View::LockScreens);
        for (int selected = 0; selected < 6; ++selected) {
            driver.tap(24, 300 + selected * 90); driver.present();
            for (int row = 0; row < 6; ++row)
                assert(app.canvas().pixel(624, 345 + row * 90) == (row == selected ? 1 : 15));
            assert(settings_store.load_settings().settings.sleep_image.empty());
        }
        driver.refresh(); driver.tap(300, 848); assert(!app.pending_frame());
        driver.tap(450, 1020); driver.present(); driver.tap(500, 1120); driver.present();
        assert(settings_store.load_settings().settings.sleep_image == long_name);
    }
    write(root / "LockScreens/01.png", png);
    write(root / "LockScreens/02.PNG", png);
    write(root / "LockScreens/._ignored.png", png);
    write(root / "LockScreens/sub/hidden.png", png);
    fs::create_symlink(root / "LockScreens/01.png", root / "LockScreens/link.png");
    FileStore store(root.string());
    std::vector<std::string> names;
    assert(store.lock_screen_files(names) == StoreStatus::Ok &&
           names == std::vector<std::string>({"01.png", "02.PNG"}));
    assert(!valid_lock_screen_name("../outside.png") && !valid_lock_screen_name(".hidden.png"));
    assert(!valid_lock_screen_name(std::string("a\0.png", 6)));
    assert(store.read_lock_screen("link.png", encoded) == StoreStatus::UnsafePath);
    auto run = [&](bool sleeping, const std::string& name = std::string(), const std::string& shown = std::string(), bool overlay = false) {
        ContentJob::Request request;
        request.kind = ContentJob::Kind::LockScreen; request.root = root.string();
        request.sleep_image = name; request.lock_screen_prepare = !sleeping; request.preview = sleeping;
        request.lock_screen_shown = shown;
        request.sleep_overlay = overlay;
        ContentJob job(std::move(request)); assert(job.start()); job.wait();
        return std::move(job.result);
    };
    auto cold = run(true);
    assert(cold.image && cold.cache_finished && cold.error.empty() && store.last_lock_screen().empty());
    auto first = run(false);
    assert(first.cache_finished && first.lock_screen == "01.png" && !first.image);
    assert(store.last_lock_screen().empty());
    auto ready = run(true);
    assert(ready.image && ready.image->width() == 684 && ready.image->height() == 1216);
    assert(store.last_lock_screen().empty());
    assert(store.save_lock_screen(ready.lock_screen) == StoreStatus::Ok);
    const auto cache_path = root / "PicoPage/cache/lockscreens/current";
    auto damaged_cache = read(cache_path);
    damaged_cache[damaged_cache.size() - 33] ^= 1;
    write(cache_path, damaged_cache);
    const auto before_repair = graphics::image_decode_count();
    assert(run(true, "01.png").image && graphics::image_decode_count() == before_repair + 1);
    assert(run(false, "01.png").cache_finished && run(true, "01.png").image);
    assert(run(false).lock_screen == "02.PNG");
    auto second = run(true);
    assert(second.image && second.lock_screen == "02.PNG" && store.last_lock_screen() == "01.png");
    // The job records a presented image before choosing; a failed commit still advances the choice.
    fs::create_directory(root / "PicoPage/data/last-lock-screen.txt.tmp");
    const auto unsaved = run(false, "", second.lock_screen);
    assert(!unsaved.cursor_saved && unsaved.lock_screen == "01.png" && store.last_lock_screen() == "01.png");
    fs::remove(root / "PicoPage/data/last-lock-screen.txt.tmp");
    const auto recorded = run(false, "", second.lock_screen);
    assert(recorded.cursor_saved && recorded.lock_screen == "01.png" && store.last_lock_screen() == "02.PNG");
    assert(std::distance(fs::directory_iterator(cache_path.parent_path()), fs::directory_iterator{}) == 1);
    assert(store.clear_cache() == StoreStatus::Ok && store.last_lock_screen() == "02.PNG");
    assert(run(true, "01.png").image);
    assert(run(false, "01.png").cache_finished);
    assert(run(true, "01.png").image && store.last_lock_screen() == "02.PNG");
    const auto original_time = fs::last_write_time(root / "LockScreens/01.png");
    write(root / "LockScreens/01.png", std::string(png.size(), 'x'));
    fs::last_write_time(root / "LockScreens/01.png", original_time);
    assert(!run(true, "01.png").image);
    write(root / "LockScreens/01.png", png);
    fs::last_write_time(root / "LockScreens/01.png", original_time);
    assert(run(true, "01.png").image);
    write(root / "LockScreens/01.png", "broken PNG");
    assert(!run(true, "01.png").image && !run(false, "01.png").cache_finished);
    fs::remove(root / "LockScreens/01.png");
    assert(!run(true, "01.png").image);
    write(root / "LockScreens/01.png", png);
    write(root / "PicoPage/settings.toml", "schema_version=1\n[power]\nsleep_screen='custom'\nsleep_image='01.png'\n");
    auto snapshot = store.settings_snapshot();
    assert(snapshot.config.diagnostics.empty() && snapshot.config.settings.sleep_image == "01.png");
    auto values = snapshot.config.settings; values.sleep_image = "../outside.png";
    assert(store.apply_settings(snapshot, values, false, detail) != StoreStatus::Ok);
    values.sleep_image = "02.PNG";
    assert(store.apply_settings(snapshot, values, false, detail) == StoreStatus::Ok);
    assert(store.load_settings().settings.sleep_image == "02.PNG");
    assert(!store.load_settings().settings.sleep_overlay);
    snapshot = store.settings_snapshot(); values = snapshot.config.settings; values.sleep_overlay = true;
    assert(store.apply_settings(snapshot, values, false, detail) == StoreStatus::Ok && store.load_settings().settings.sleep_overlay);
    snapshot = store.settings_snapshot(); values.sleep_overlay = false;
    assert(store.apply_settings(snapshot, values, false, detail) == StoreStatus::Ok && !store.load_settings().settings.sleep_overlay);
    {
        write(root / "LockScreens/alpha.png", std::string(reinterpret_cast<const char*>(kRgbaPng), sizeof(kRgbaPng)));
        write(root / "LockScreens/opaque.png", std::string(reinterpret_cast<const char*>(kOpaqueRgbaPng), sizeof(kOpaqueRgbaPng)));
        {
            // Reading retains other card handles; PNG preparation must need at most two additional files.
            assert(store.clear_cache() == StoreStatus::Ok);
            struct rlimit original{}, limited{};
            assert(getrlimit(RLIMIT_NOFILE, &original) == 0);
            limited = original; limited.rlim_cur = std::min<rlim_t>(original.rlim_cur, 64);
            assert(setrlimit(RLIMIT_NOFILE, &limited) == 0);
            std::vector<int> held;
            for (int fd; (fd = ::open("/dev/null", O_RDONLY)) >= 0;) held.push_back(fd);
            assert(held.size() >= 2);
            for (int n = 0; n < 2; ++n) { ::close(held.back()); held.pop_back(); }
            const auto prepared = run(false, "alpha.png");
            assert(setrlimit(RLIMIT_NOFILE, &original) == 0);
            for (int fd : held) ::close(fd);
            assert(prepared.cache_finished && !prepared.image && prepared.error.empty());
        }
        assert(run(false, "alpha.png").cache_finished);
        assert(fs::file_size(cache_path) > 512 * 1024);
        {
            const auto cache_bytes = read(cache_path);
            const auto header = cache_bytes.substr(0, cache_bytes.size() - 684U * 1216 * 2 - 33);
            std::unique_ptr<CacheReader> reader;
            assert(store.open_cache("lockscreens/current", cache_bytes.size(), reader) == StoreStatus::Ok);
            bool alpha = false;
            unsigned checks = 0;
            assert(!read_lock_screen_cache(*reader, header, nullptr, alpha, [&] { return ++checks == 4; }));
            assert(read_lock_screen_cache(*reader, header, nullptr, alpha, {}) && alpha);
            graphics::Canvas output; output.clear(7);
            checks = 0;
            assert(!read_lock_screen_cache(*reader, header, &output, alpha, [&] { return ++checks == 4; }));
            reader.reset();
            std::string layer; bool source_alpha = false;
            const std::vector<std::uint8_t> source(std::begin(kRgbaPng), std::end(kRgbaPng));
            assert(graphics::decode_png_layer(source, 684, 1216, layer, source_alpha, detail));
            graphics::Canvas expected; expected.clear(7);
            assert(graphics::composite_image_layer(expected, layer, 0, 1216));
            const auto check_overlay = [&](bool cold) {
                ContentJob::Request request;
                request.kind = ContentJob::Kind::LockScreen; request.root = root.string(); request.sleep_image = "alpha.png";
                request.sleep_overlay = true; request.lock_background = std::make_shared<graphics::Canvas>();
                request.lock_background->clear(7); request.lock_cache_limit_for_test = 684 * 2;
                const auto* storage = request.lock_background->data();
                const auto decoded = graphics::image_decode_count();
                ContentJob job(std::move(request)); assert(job.start()); job.wait();
                assert(job.result.image && job.result.cache_finished && job.result.error.empty());
                assert(job.result.image->data() == storage && job.result.image->pixels() == expected.pixels());
                assert(graphics::image_decode_count() == decoded + (cold ? 1 : 0));
            };
            check_overlay(false);
            for (bool truncated : {false, true}) {
                ContentJob::Request request;
                request.kind = ContentJob::Kind::LockScreen; request.root = root.string(); request.sleep_image = "alpha.png";
                request.sleep_overlay = true; request.lock_background = std::make_shared<graphics::Canvas>();
                request.lock_background->clear(7);
                request.lock_cache_validated_for_test = [&] {
                    auto changed = cache_bytes;
                    if (truncated) changed.resize(changed.size() - 1);
                    else changed.back() ^= 1;
                    write(cache_path, changed);
                };
                const auto decoded = graphics::image_decode_count();
                ContentJob failed(std::move(request)); assert(failed.start()); failed.wait();
                assert(!failed.result.image && !failed.result.cache_finished && failed.result.retryable_failure);
                assert(failed.result.error.find("缓存读取失败") != std::string::npos);
                assert(graphics::image_decode_count() == decoded);
                write(cache_path, cache_bytes);
            }
            for (int failure = 0; failure < 5; ++failure) {
                auto damaged = cache_bytes;
                if (failure == 0) damaged[header.size() + 10000] ^= 1;
                if (failure == 1) damaged.back() ^= 1;
                if (failure == 2) damaged.resize(damaged.size() - 9);
                if (failure == 3) damaged[header.size()] = 'Z';
                if (failure == 4) damaged[0] = 'X';
                write(cache_path, damaged);
                check_overlay(true);
            }
            check_overlay(false);
        }
        {
            ContentJob::Request blocked;
            blocked.kind = ContentJob::Kind::LockScreen; blocked.root = root.string(); blocked.sleep_image = "alpha.png";
            blocked.lock_canvas_limit_for_test = 342 * 1216 - 1;
            const auto before = graphics::image_decode_count();
            ContentJob refused(std::move(blocked)); assert(refused.start()); refused.wait();
            assert(refused.result.cache_finished && refused.result.retryable_failure && !refused.result.image);
            assert(!refused.result.error.empty() && graphics::image_decode_count() == before);
        }
        ContentJob::Request request;
        request.kind = ContentJob::Kind::LockScreen; request.root = root.string();
        request.sleep_image = "opaque.png"; request.lock_cache_limit_for_test = 512 * 1024;
        const auto decoded = graphics::image_decode_count();
        ContentJob job(std::move(request)); assert(job.start()); job.wait();
        assert(job.result.image && job.result.cache_finished && job.result.error.empty());
        assert(graphics::image_decode_count() == decoded + 1 && fs::file_size(cache_path) < 512 * 1024);
        graphics::Canvas expected;
        const std::vector<std::uint8_t> opaque(std::begin(kOpaqueRgbaPng), std::end(kOpaqueRgbaPng));
        assert(graphics::render_image(expected, opaque, 0, 0, 684, 1216, detail));
        assert(job.result.image->pixels() == expected.pixels());
        const auto before_hot = graphics::image_decode_count();
        auto hot = run(true, "opaque.png", "", true);
        assert(hot.image && hot.image->pixels() == expected.pixels() && graphics::image_decode_count() == before_hot);
        fs::remove(cache_path);
        assert(run(false, "opaque.png").cache_finished);
        hot = run(true, "opaque.png", "", true);
        assert(hot.image && hot.image->pixels() == expected.pixels() && fs::file_size(cache_path) < 512 * 1024);
        fs::remove(root / "LockScreens/alpha.png"); fs::remove(root / "LockScreens/opaque.png");
    }
    {
        // Interlaced row revisits must preserve final alpha, never blend a progressive preview twice.
        const std::vector<std::uint8_t> source(std::begin(kAdam7Png), std::end(kAdam7Png));
        write(root / "LockScreens/adam7.png", std::string(source.begin(), source.end()));
        std::string layer; bool alpha = false;
        assert(graphics::decode_png_layer(source, 684, 1216, layer, alpha, detail) && alpha);
        for (bool cold : {true, false}) {
            ContentJob::Request request;
            request.kind = ContentJob::Kind::LockScreen; request.root = root.string();
            request.sleep_image = "adam7.png"; request.sleep_overlay = true;
            request.lock_background = std::make_shared<graphics::Canvas>();
            for (int y = 0; y < 1216; ++y) for (int x = 0; x < 684; ++x)
                request.lock_background->pixel(x, y, static_cast<std::uint8_t>((x + y) % 16));
            auto expected = *request.lock_background;
            assert(graphics::composite_image_layer(expected, layer, 0, 1216));
            const auto decoded = graphics::image_decode_count();
            ContentJob job(std::move(request)); assert(job.start()); job.wait();
            assert(job.result.image && job.result.cache_finished && job.result.image->pixels() == expected.pixels());
            assert(graphics::image_decode_count() == decoded + (cold ? 1 : 0));
        }
        fs::remove(root / "LockScreens/adam7.png");
    }
    {
        // A late PNG checksum error must not consume or contaminate the base for the next rotation image.
        const auto rotation_root = root / "DamagedRotation";
        const std::vector<std::uint8_t> source(std::begin(kRgbaPng), std::end(kRgbaPng));
        std::string broken(source.begin(), source.end()); broken.back() ^= 1;
        write(rotation_root / "LockScreens/01-broken.png", broken);
        write(rotation_root / "LockScreens/02.png", std::string(source.begin(), source.end()));
        ContentJob::Request request;
        request.kind = ContentJob::Kind::LockScreen; request.root = rotation_root.string(); request.sleep_overlay = true;
        request.lock_background = std::make_shared<graphics::Canvas>();
        request.lock_background->clear(7);
        auto expected = *request.lock_background;
        std::string layer; bool alpha = false;
        assert(graphics::decode_png_layer(source, 684, 1216, layer, alpha, detail));
        assert(graphics::composite_image_layer(expected, layer, 0, 1216));
        ContentJob job(std::move(request)); assert(job.start()); job.wait();
        assert(job.result.image && job.result.cache_finished && job.result.lock_screen == "02.png");
        assert(job.result.image->pixels() == expected.pixels());
    }
    {
        const auto overlay_root = root / "Overlay";
        const std::vector<std::uint8_t> rgba(std::begin(kRgbaPng), std::end(kRgbaPng));
        write(overlay_root / "LockScreens/01.png", std::string(reinterpret_cast<const char*>(rgba.data()), rgba.size()));
        write(overlay_root / "PicoPage/settings.toml", "schema_version=1\n[power]\nsleep_screen='custom'\nsleep_image='01.png'\nsleep_overlay=true\n");
        ReaderApp app(overlay_root.string(), "overlay", true, true); Driver driver{app};
        assert(app.initialize()); driver.present();
        std::string layer, error; bool alpha = false;
        assert(graphics::decode_png_layer(rgba, 684, 1216, layer, alpha, error) && alpha);
        auto original = app.canvas();
        assert(app.begin_sleep()); driver.present();
        const auto feedback = app.canvas().pixels();
        assert(!app.prepare_sleep() && app.sleep_pending());
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (std::string(app.background_state()) == "sleep-cover") {
            app.poll_background();
            assert(std::chrono::steady_clock::now() < until);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        assert(app.canvas().pixels() == feedback);
        app.cancel_sleep(); driver.present();
        assert(app.canvas().pixels() == original.pixels() && !app.sleep_pending());
        assert(app.cache_usage().cover_pixels == 0);
        ContentJob::delay_starts_for_test(ContentJob::Kind::LockScreen, std::chrono::milliseconds(100));
        assert(app.begin_sleep()); driver.present();
        assert(!app.prepare_sleep() && app.sleep_pending());
        app.cancel_sleep(); driver.present();
        while (std::string(app.background_state()) == "sleep-cover") {
            app.poll_background();
            assert(std::chrono::steady_clock::now() < until);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        ContentJob::delay_starts_for_test(ContentJob::Kind::LockScreen, std::chrono::milliseconds{});
        assert(app.canvas().pixels() == original.pixels() && !app.sleep_pending() && app.cache_usage().cover_pixels == 0);
        const auto decoded = graphics::image_decode_count();
        auto expected = original;
        assert(graphics::composite_image_layer(expected, layer, 0, 1216));
        assert(sleep_frame(app)); driver.present();
        assert(app.canvas().pixels() == expected.pixels() && graphics::image_decode_count() == decoded);
        app.wake(); driver.present();
        driver.tap(600, 50); driver.present(); driver.tap(340, 640); driver.present();
        assert(app.view() == View::PowerSettings);
        driver.tap(300, 610); driver.present();
        assert(FileStore(overlay_root.string()).load_settings().settings.sleep_overlay);
        ContentJob::fail_starts_for_test(ContentJob::Kind::LockScreenPreview, 1, ContentJob::StartFailure::TaskCreate);
        driver.tap(480, 470); driver.present(); driver.complete_content();
        assert(app.view() == View::PowerSettings && FileStore(overlay_root.string()).load_settings().settings.sleep_overlay);
        driver.tap(480, 470); driver.present(); driver.complete_content();
        assert(app.view() == View::LockScreenPreview);
        driver.refresh(); driver.tap(300, 1140); driver.present();
        assert(app.view() == View::PowerSettings);
        driver.tap(300, 610); driver.present();
        auto preview_expected = app.canvas();
        assert(graphics::composite_image_layer(preview_expected, layer, 0, 1216));
        const auto before_preview = graphics::image_decode_count();
        driver.tap(480, 470); driver.present(); driver.complete_content();
        assert(app.view() == View::LockScreenPreview && graphics::image_decode_count() == before_preview);
        assert(std::equal(preview_expected.pixels().begin() + 120 * 342, preview_expected.pixels().begin() + 1048 * 342,
                          app.canvas().pixels().begin() + 120 * 342));
        driver.tap(300, 1140); driver.present(); driver.tap(150, 1120); driver.present();
        assert(app.view() == View::Settings && FileStore(overlay_root.string()).load_settings().settings.sleep_overlay);
        for (bool started : {false, true}) {
            ReaderApp sleeping(overlay_root.string(), "preview-sleep", true, true); Driver sleep_driver{sleeping};
            assert(sleeping.initialize()); sleep_driver.present();
            sleep_driver.tap(600, 50); sleep_driver.present(); sleep_driver.tap(340, 640); sleep_driver.present();
            ContentJob::delay_starts_for_test(ContentJob::Kind::LockScreenPreview, std::chrono::seconds(30));
            sleep_driver.tap(480, 470); sleep_driver.present();
            if (started) sleeping.poll_background();
            assert(sleeping.view() == View::Loading && sleeping.content_busy());
            assert(sleep_frame(sleeping)); sleep_driver.present();
            sleeping.wake(); sleep_driver.present(); sleeping.poll_background();
            assert(sleeping.view() == View::Settings && !sleeping.content_busy());
            const auto returned = sleeping.canvas().pixels();
            sleeping.poll_background();
            assert(sleeping.view() == View::Settings && sleeping.canvas().pixels() == returned);
            ContentJob::delay_starts_for_test(ContentJob::Kind::LockScreenPreview, std::chrono::milliseconds{});
        }
        fs::create_directories(overlay_root / "Books");
        fs::copy_file(fixtures / "image-cover.epub", overlay_root / "Books/cover.epub");
        FileStore overlay_store(overlay_root.string());
        auto book_snapshot = overlay_store.settings_snapshot("Books/cover.epub");
        auto book_values = book_snapshot.config.settings; book_values.sleep_overlay = false;
        assert(overlay_store.apply_settings(book_snapshot, book_values, true, error) == StoreStatus::Invalid);
        app.set_battery(75, true);
        opened(app, "Books/cover.epub"); driver.present(); driver.complete_content();
        original = app.canvas(); expected = original;
        const auto reading_device = footer_pixels(app, 540, 684);
        assert(std::any_of(reading_device.begin(), reading_device.end(), [](auto pixel) { return pixel != 15; }));
        expected.fill_rect(0, 1128, 684, 88, 15);
        assert(graphics::composite_image_layer(expected, layer, 0, 1216));
        assert(app.begin_sleep()); driver.present();
        assert(!app.prepare_sleep() && app.sleep_pending());
        app.cancel_sleep(); driver.present();
        const auto cancelled_until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (std::string(app.background_state()) == "sleep-cover") {
            app.poll_background();
            assert(std::chrono::steady_clock::now() < cancelled_until);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        assert(app.canvas().pixels() == original.pixels() && !app.sleep_pending());
        assert(sleep_frame(app)); driver.present();
        assert(app.canvas().pixels() == expected.pixels());
        app.wake(); driver.present(); driver.complete_content();
        assert(app.view() == View::Reading && footer_pixels(app, 540, 684) == reading_device);
    }
    assert(run(false, "02.PNG").cache_finished);
    {
        const auto rotating = root / "Rotation";
        write(rotating / "LockScreens/00-broken.png", "broken PNG");
        write(rotating / "LockScreens/01.png", png);
        write(rotating / "LockScreens/02.png", png);
        write(rotating / "PicoPage/settings.toml", "schema_version=1\n[power]\nsleep_screen='custom'\n");
        FileStore rotating_store(rotating.string());
        ReaderApp app(rotating.string(), "rotation", true, true); Driver driver{app};
        assert(app.initialize()); driver.present();
        assert(!app.prepare_sleep() && app.sleep_pending());
        app.cancel_sleep();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        while (std::string(app.background_state()) == "sleep-cover") {
            app.poll_background();
            assert(std::chrono::steady_clock::now() < deadline);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        // Runs the idle LockScreen job, which records the presented image and prepares the next one.
        const auto settle = [&] {
            while (app.background_due()) {
                app.background_step(); app.poll_background();
                if (app.pending_frame()) driver.present();
                assert(std::chrono::steady_clock::now() < deadline);
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        };
        assert(rotating_store.last_lock_screen().empty());
        assert(sleep_frame(app));
        app.complete_frame(false);
        assert(app.view() == View::Error && rotating_store.last_lock_screen().empty());
        app.cancel_sleep(); driver.present(); driver.close(); driver.present();
        // The failed frame did not advance the rotation, and presentation alone writes nothing.
        assert(sleep_frame(app) && app.view() == View::Sleeping && rotating_store.last_lock_screen().empty());
        driver.present();
        assert(rotating_store.last_lock_screen().empty());
        app.wake(); driver.present(); settle();
        assert(rotating_store.last_lock_screen() == "01.png");
        // Without an idle step, the next sleep request records the presented image before choosing.
        assert(sleep_frame(app)); driver.present(); app.wake(); driver.present();
        assert(rotating_store.last_lock_screen() == "01.png");
        assert(sleep_frame(app) && rotating_store.last_lock_screen() == "02.png");
        driver.present(); app.wake(); driver.present(); settle();
        assert(rotating_store.last_lock_screen() == "01.png");
        assert(rotating_store.clear_cache() == StoreStatus::Ok);
        const auto decoded = graphics::image_decode_count();
        assert(sleep_frame(app)); driver.present();
        assert(rotating_store.last_lock_screen() == "01.png" && graphics::image_decode_count() == decoded + 1);
        // A failed commit is not reported; the presented image stays pending for the next job.
        fs::create_directory(rotating / "PicoPage/data/last-lock-screen.txt.tmp");
        app.wake(); driver.present(); settle(); app.poll_background();
        assert(app.view() == View::Library && !app.pending_frame() && app.status().find("未保存") == std::string::npos);
        assert(rotating_store.last_lock_screen() == "01.png");
        fs::remove(rotating / "PicoPage/data/last-lock-screen.txt.tmp");
        assert(sleep_frame(app) && rotating_store.last_lock_screen() == "02.png");
        driver.present();
    }
    {
        // A restart before the presented image was recorded shows that image once more.
        const auto rotating = root / "Rotation";
        ReaderApp rebooted(rotating.string(), "rotation-reboot", true, true); Driver driver{rebooted};
        assert(rebooted.initialize()); driver.present();
        assert(FileStore(rotating.string()).last_lock_screen() == "02.png");
        assert(sleep_frame(rebooted)); driver.present(); rebooted.wake(); driver.present();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        while (rebooted.background_due()) {
            rebooted.background_step(); rebooted.poll_background();
            if (rebooted.pending_frame()) driver.present();
            assert(std::chrono::steady_clock::now() < deadline);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        assert(FileStore(rotating.string()).last_lock_screen() == "01.png");
    }
    for (const auto* mode : {"cover", "reading", "simple", "custom"}) {
        const auto feedback = root / (std::string("ImmediateFeedback-") + mode);
        write(feedback / "PicoPage/settings.toml", std::string("schema_version=1\n[power]\nsleep_screen='") + mode + "'\n");
        ReaderApp app(feedback.string(), "immediate-feedback", true, true); Driver driver{app};
        assert(app.initialize()); driver.present();
        const auto original = app.canvas().pixels();
        assert(app.begin_sleep() && app.pending_frame());
        assert(app.view() == View::Library && !app.sleep_pending());
        const auto feedback_pixels = app.canvas().pixels();
        assert(feedback_pixels != original);
        app.poll_background();
        assert(app.canvas().pixels() == feedback_pixels);
        app.cancel_sleep();
        assert(app.canvas().pixels() == feedback_pixels);
        driver.present();
        assert(app.canvas().pixels() == original && !app.pending_frame());
        app.show_error("测试错误。"); driver.present();
        const auto error_pixels = app.canvas().pixels();
        assert(app.begin_sleep()); driver.present();
        app.cancel_sleep(); driver.present();
        assert(app.view() == View::Error && app.canvas().pixels() == error_pixels);
        assert(app.begin_sleep()); driver.present();
        app.show_error("新的测试错误。");
        const auto newer_error = app.canvas().pixels();
        app.cancel_sleep(); driver.present();
        assert(app.canvas().pixels() == newer_error);
    }
    for (int action : {0, 1, 2}) {
        const bool fail_frame = action == 1, open_menu = action == 2;
        const auto waiting = root / (fail_frame ? "WaitFrameFailure" : open_menu ? "WaitPowerMenu" : "WaitCancel");
        write(waiting / "LockScreens/01.png", png);
        write(waiting / "PicoPage/settings.toml", "schema_version=1\n[power]\nsleep_screen='custom'\n");
        ReaderApp app(waiting.string(), "wait-footer", true, true); Driver driver{app};
        assert(app.initialize()); driver.present();
        const auto original = app.canvas().pixels();
        const auto decodes = graphics::image_decode_count();
        assert(app.begin_sleep());
        assert(app.pending_frame() && !app.sleep_pending());
        assert(app.pending_frame()->region && app.pending_frame()->region->y == 1128 && app.pending_frame()->region->height == 88);
        assert(std::equal(original.begin(), original.begin() + 1128 * app.canvas().stride(), app.canvas().pixels().begin()));
        assert(!app.prepare_sleep() && graphics::image_decode_count() == decodes);
        app.poll_background();
        assert(std::string(app.background_state()) != "sleep-cover");
        app.complete_frame(!fail_frame);
        if (!fail_frame) {
            ContentJob::delay_starts_for_test(ContentJob::Kind::LockScreen, std::chrono::milliseconds(1500));
            assert(!app.prepare_sleep() && app.sleep_pending());
            ContentJob::delay_starts_for_test(ContentJob::Kind::LockScreen, std::chrono::milliseconds{});
        }
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        app.cancel_sleep(); // The firmware cancels the request when presentation fails.
        assert(!app.sleep_pending());
        if (open_menu) {
            assert(app.show_power_menu());
            assert(app.power_menu_pending());
            pico_page::platform::SleepInput suppressed;
            suppressed.begin();
            const auto before_menu = app.canvas().pixels();
            for (const auto kind : {pico_page::platform::EventKind::Touch, pico_page::platform::EventKind::K1,
                                    pico_page::platform::EventKind::K2, pico_page::platform::EventKind::K3}) {
                suppressed.accept({kind, pico_page::platform::EventPhase::Begin, 1, 600, 50});
                suppressed.accept({kind, pico_page::platform::EventPhase::End, 1, 600, 50});
                assert(suppressed.active() && !suppressed.power() && !suppressed.menu());
                assert(app.power_menu_pending() && app.canvas().pixels() == before_menu);
            }
            driver.present();
            while (app.view() != View::PowerMenu) {
                app.poll_background();
                if (app.pending_frame()) driver.present();
                assert(std::chrono::steady_clock::now() < deadline);
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            assert(!app.power_menu_pending());
            suppressed.clear();
            driver.tap(300, 1120); driver.present();
            assert(app.view() == View::Library && app.canvas().pixels() == original);
        } else if (fail_frame) {
            assert(app.view() == View::Error && app.status().find("屏幕局部更新失败") != std::string::npos);
            const auto error_pixels = app.canvas().pixels();
            driver.present(); app.wake(false);
            assert(app.view() == View::Error && !app.pending_frame() && app.canvas().pixels() == error_pixels);
        } else {
            assert(app.pending_frame() && app.pending_frame()->region && app.pending_frame()->region->y == 1128 &&
                   app.pending_frame()->region->height == 88);
            driver.present();
            assert(app.view() == View::Library && app.canvas().pixels() == original);
        }
        while (std::string(app.background_state()) == "sleep-cover") {
            app.poll_background();
            if (app.pending_frame()) driver.present();
            assert(std::chrono::steady_clock::now() < deadline);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        assert(FileStore(waiting.string()).last_lock_screen().empty());
    }
#ifdef PICO_PAGE_WRAP_MALLOC_TEST
    {
        const auto waiting = root / "WaitAllocationFailure";
        write(waiting / "LockScreens/01.png", png);
        write(waiting / "PicoPage/settings.toml", "schema_version=1\n[power]\nsleep_screen='custom'\n");
        ReaderApp app(waiting.string(), "wait-allocation-failure", true, true); Driver driver{app};
        assert(app.initialize()); driver.present();
        const auto original = app.canvas().pixels();
        ContentJob::delay_starts_for_test(ContentJob::Kind::LockScreen, std::chrono::milliseconds(1500));
        fail_wait_footer_malloc = true;
        assert(app.begin_sleep());
        assert(!app.prepare_sleep() && app.sleep_pending());
        ContentJob::delay_starts_for_test(ContentJob::Kind::LockScreen, std::chrono::milliseconds{});
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!failed_wait_footer_mallocs.load()) {
            app.poll_background();
            assert(std::chrono::steady_clock::now() < deadline);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        assert(app.view() == View::Library && !app.pending_frame() && app.canvas().pixels() == original);
        app.cancel_sleep(); fail_wait_footer_malloc = false;
        while (std::string(app.background_state()) == "sleep-cover") {
            app.poll_background();
            assert(std::chrono::steady_clock::now() < deadline);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        assert(!app.pending_frame() && app.cache_usage().cover_pixels == 0);
        assert(FileStore(waiting.string()).last_lock_screen().empty());
        const auto decodes = graphics::image_decode_count();
        assert(sleep_frame(app) && app.view() == View::Sleeping);
        assert(graphics::image_decode_count() == decodes + 1);
        driver.present();
    }
#endif
    for (bool slept : {false, true}) {
        // A prepared image is shown although its cache and cursor commits fail; neither is reported.
        const auto blocked = root / (slept ? "CommitFailuresSlept" : "CommitFailuresCancelled");
        write(blocked / "LockScreens/01.png", png);
        write(blocked / "PicoPage/cache/lockscreens", "blocked");
        write(blocked / "PicoPage/settings.toml", "schema_version=1\n[power]\nsleep_screen='custom'\n");
        fs::create_directories(blocked / "PicoPage/data/last-lock-screen.txt.tmp");
        ReaderApp app(blocked.string(), "commit-failures", true, true); Driver driver{app};
        assert(app.initialize()); driver.present();
        const auto decodes = graphics::image_decode_count();
        assert(sleep_frame(app) && app.view() == View::Sleeping && graphics::image_decode_count() == decodes + 1);
        driver.present(); app.wake(slept); driver.present();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        while (app.background_due()) {
            app.background_step(); app.poll_background();
            assert(std::chrono::steady_clock::now() < deadline);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        app.poll_background();
        assert(app.view() == View::Library && !app.pending_frame());
        assert(app.status().find("未保存") == std::string::npos && app.status().find("简洁屏保") == std::string::npos);
        assert(FileStore(blocked.string()).last_lock_screen().empty());
    }
    for (bool overlay : {false, true}) {
        const auto blocked = root / (overlay ? "AlphaCacheFailure" : "CacheFailure");
        const std::string source = overlay ? std::string(reinterpret_cast<const char*>(kRgbaPng), sizeof(kRgbaPng)) : png;
        write(blocked / "LockScreens/01.png", source);
        write(blocked / "PicoPage/cache/lockscreens", "blocked");
        ContentJob::Request request;
        request.kind = ContentJob::Kind::LockScreen; request.root = blocked.string(); request.preview = true;
        request.sleep_overlay = overlay;
        if (overlay) request.lock_background = std::make_shared<graphics::Canvas>();
        ContentJob job(request); assert(job.start()); job.wait();
        assert(job.result.image && !job.result.cache_finished && job.result.retryable_failure);
        assert(job.result.error.find("缓存未保存") != std::string::npos);
        if (overlay) {
            std::string layer; bool alpha = false;
            assert(graphics::decode_png_layer(std::vector<std::uint8_t>(source.begin(), source.end()), 684, 1216, layer, alpha, detail) && alpha);
            graphics::Canvas expected;
            assert(graphics::composite_image_layer(expected, layer, 0, 1216));
            assert(job.result.image->pixels() == expected.pixels());
        }
        request.lock_screen_deadline = std::chrono::steady_clock::now() - std::chrono::milliseconds(1);
        const auto decoded = graphics::image_decode_count();
        ContentJob expired(request); assert(expired.start()); expired.wait();
        assert(!expired.result.image && !expired.result.cache_finished && expired.result.retryable_failure);
        assert(graphics::image_decode_count() == decoded);
    }
    {
        const auto resumed = root / "WakeFailure";
        fs::create_directories(resumed / "Books");
        fs::copy_file(fixtures / "image-cover.epub", resumed / "Books/cover.epub");
        write(resumed / "LockScreens/broken.png", "broken PNG");
        write(resumed / "PicoPage/settings.toml", "schema_version=1\n[power]\nsleep_screen='custom'\nsleep_image='broken.png'\n");
        ReaderApp app(resumed.string(), "wake-failure", true, true); Driver driver{app};
        assert(app.initialize()); driver.present(); opened(app, "Books/cover.epub"); driver.present(); driver.complete_content();
        const auto saved = *app.saved_record();
        const auto body = app.canvas().pixels();
        assert(sleep_frame(app)); driver.present();
        assert(FileStore(resumed.string()).clear_cache() == StoreStatus::Ok);
        const auto decodes = graphics::image_decode_count();
        bool notice_seen = false;
        driver.frame_observer = [&](const Frame& frame) {
            if (app.status().find("简洁屏保") != std::string::npos && frame.region && frame.region->y == 1128) {
                assert(app.view() == View::Reading);
                notice_seen = true;
            }
        };
        app.wake(); driver.present();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        while (app.content_busy() || !notice_seen) {
            app.poll_background();
            if (app.pending_frame()) {
                driver.present();
            }
            assert(std::chrono::steady_clock::now() < deadline);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        assert(app.view() == View::Reading && !app.content_busy());
        driver.frame_observer = {};
        assert(graphics::image_decode_count() > decodes);
        assert(*app.saved_record() == saved);
        assert(std::equal(body.begin(), body.begin() + 1128 * app.canvas().stride(), app.canvas().pixels().begin()));
        driver.tap(342, 500); driver.present(); driver.tap(50, 50); driver.present();
        assert(app.view() == View::Library);
        for (int i = 0; i < 2; ++i) {
            assert(sleep_frame(app)); driver.present();
            app.wake(); driver.present(); app.poll_background();
            assert(app.view() == View::Library && !app.content_busy() && *app.saved_record() == saved);
            assert(app.status().find("简洁屏保") != std::string::npos);
            assert(app.pending_frame() && app.pending_frame()->region && app.pending_frame()->region->y == 1120);
            driver.present();
        }
    }
    {
        ReaderApp app(root.string(), "locks", true, true); Driver driver{app};
        assert(app.initialize()); driver.present();
        ContentJob::fail_starts_for_test(ContentJob::Kind::LockScreen, 1, ContentJob::StartFailure::TaskCreate);
        assert(app.background_due()); app.background_step();
        assert(!app.background_due());
        const auto retry_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!app.background_due()) {
            assert(std::chrono::steady_clock::now() < retry_deadline);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        app.background_step();
        while (app.background_due()) {
            app.poll_background();
            assert(std::chrono::steady_clock::now() < retry_deadline);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        assert(!app.prepare_sleep() && app.sleep_pending());
        app.cancel_sleep();
        while (std::string(app.background_state()) == "sleep-cover") {
            app.poll_background();
            assert(std::chrono::steady_clock::now() < retry_deadline);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        assert(!app.sleep_pending() && app.view() == View::Library && app.cache_usage().cover_pixels == 0);
        assert(sleep_frame(app) && app.view() == View::Sleeping); driver.present();
        auto expected = run(true, "02.PNG"); assert(expected.image);
        assert(app.canvas().pixels() == expected.image->pixels());
        app.wake(); driver.present();
        driver.tap(600, 50); driver.present(); driver.tap(340, 640); driver.present();
        assert(app.view() == View::PowerSettings);
        driver.tap(480, 350); driver.present(); driver.tap(330, 470); driver.present(); driver.complete_content();
        assert(app.view() == View::LockScreens); driver.refresh();
        driver.tap(300, 190); driver.present(); driver.tap(450, 1020); driver.present();
        driver.tap(500, 1120); driver.present();
        assert(store.load_settings().settings.sleep_image.empty());
    }
    {
        const auto switched = root / "CoverSwitch";
        write(switched / "LockScreens/custom.png", png);
        write(switched / "PicoPage/settings.toml", "schema_version=1\n[power]\nsleep_screen='custom'\nsleep_image='custom.png'\n");
        fs::create_directories(switched / "Books");
        fs::copy_file(fixtures / "image-cover.epub", switched / "Books/cover.epub");
        ReaderApp app(switched.string(), "cover-switch", true, true); Driver driver{app};
        assert(app.initialize()); driver.present(); opened(app, "Books/cover.epub"); driver.present();
        driver.complete_content();
        driver.tap(342, 500); driver.present(); driver.tap(50, 50); driver.present();
        driver.tap(600, 50); driver.present(); driver.tap(340, 640); driver.present();
        assert(app.view() == View::PowerSettings);
        const auto prepare_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (std::string(app.background_state()) != "sleep-cover") {
            app.poll_background();
            if (app.pending_frame()) driver.present();
            app.background_step();
            assert(std::chrono::steady_clock::now() < prepare_deadline);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        driver.tap(150, 240); driver.present(); driver.tap(500, 1120); driver.present();
        assert(!app.prepare_sleep() && app.sleep_pending());
        assert(sleep_frame(app)); driver.present();
        assert(app.canvas().pixel(50, 100) == 0 && app.canvas().pixel(50, 900) == 5);
    }
    for (const bool usb : {true, false}) {
        ReaderApp app(root.string(), "lock-transfer", true, true); Driver driver{app};
        assert(app.initialize()); driver.present();
        app.background_step();
        assert(std::string(app.background_state()) == "sleep-cover");
        driver.tap(600, 50); driver.present(); driver.tap(300, 180); driver.present();
        assert(app.view() == View::Import);
        driver.tap(342, usb ? 330 : 660);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        for (;;) {
            const bool ready = usb ? app.take_usb_transfer_action() == UsbTransferAction::Start :
                                     app.take_portal_action() == PortalAction::Upload;
            if (ready) break;
            app.poll_background();
            assert(std::chrono::steady_clock::now() < deadline);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        assert(usb ? app.prepare_usb_transfer() : app.prepare_portal());
        if (app.pending_frame()) driver.present();
    }
    for (int i = 0; i < 257; ++i) write(root / "LockScreens" / ("overflow-" + std::to_string(i) + ".png"), png);
    assert(store.lock_screen_files(names) == StoreStatus::Invalid && names.empty());
    fs::remove_all(root / "LockScreens");
    const auto missing = run(false);
    assert(!run(true).image && !missing.cache_finished && !missing.retryable_failure);
    {
        ReaderApp app(root.string(), "empty-locks", true, true); Driver driver{app};
        assert(app.initialize()); driver.present();
        assert(sleep_frame(app) && app.view() == View::Sleeping); driver.present();
        app.wake(); driver.present(); app.poll_background();
        assert(app.view() == View::Library && app.status().find("简洁屏保") != std::string::npos);
        if (app.pending_frame()) driver.present();
        for (const auto& point : {std::pair{150, 240}, std::pair{480, 240}, std::pair{150, 350}}) {
            driver.tap(600, 50); driver.present(); driver.tap(340, 640); driver.present();
            driver.tap(point.first, point.second); driver.present(); driver.tap(500, 1120); driver.present();
            assert(sleep_frame(app)); driver.present(); app.wake(); driver.present(); app.poll_background();
            assert(app.view() == View::Settings);
            driver.close(); driver.present(); assert(app.view() == View::Library);
        }
    }
}

static std::string update_manifest(const std::string& version, int progress_schema = update::progress_schema,
                                   const std::string& notes = "## 新增\\n- **书签**：可添加。", int config_schema = update::config_schema) {
    return "{\"manifest\":1,\"board\":\"read_pico\",\"version\":\"" + version + "\",\"published_at\":\"2026-10-18\","
           "\"url\":\"https://picopage.wulu.zone/firmware/pico-page-" + version + "-read_pico-release.bin\",\"size\":2936012,"
           "\"sha256\":\"" + std::string(64, 'a') + "\",\"data_schema\":{\"progress\":" + std::to_string(progress_schema) +
           ",\"config\":" + std::to_string(config_schema) + "},\"notes\":\"" + notes + "\"}";
}
// Firmware update: check, release notes, installation conditions, download, restart and the report on the next start.
static void update_test(const fs::path& root) {
    fs::remove_all(root);
    write(root / "PicoPage/network.toml", provision::network_toml({{{"home", "password123"}, 0}}));
    const auto state = [&] {
        update::State loaded;
        assert(update::decode_state(read(root / "PicoPage/cache" / update::state_cache), loaded));
        return loaded;
    };
    const auto step = [](ReaderApp& app, Driver& driver, int x, int y) {
        driver.tap(x, y);
        if (app.pending_frame()) driver.present();
    };
    const auto open_page = [&](ReaderApp& app, Driver& driver) {
        assert(app.initialize()); driver.present(); assert(app.view() == View::Library);
        driver.tap(600, 50); driver.present(); assert(app.view() == View::Settings);
        driver.tap(300, 856); driver.present(); assert(app.view() == View::Update);
    };
    const auto check = [&](ReaderApp& app, Driver& driver, int y, const std::string& body, int status = 200,
                           UpdateFailure failure = UpdateFailure::None, const std::string& detail = "", int x = 300) {
        driver.tap(x, y); driver.present();
        assert(app.update_busy());
        const auto request = app.take_update_request();
        assert(request && request->kind == UpdateOperation::Kind::Check && request->url == update::manifest_url &&
               request->targets.networks.size() == 1 && network::live(request->cancelled) && !app.take_update_request());
        app.complete_update(request->id, failure, status, body, detail);
        driver.present();
        assert(!app.update_busy() && app.view() == View::Update);
    };
    {
        ReaderApp app(root.string()); Driver driver{app};
        app.set_firmware_version("0.2.0-beta.1");
        open_page(app, driver);
        assert(!fs::exists(root / "PicoPage/cache" / update::state_cache));
        // A failed check leaves nothing cached: no link, an HTTP error, another board, a malformed version.
        // A failed check names its step; the retry is the primary button except after a Wi-Fi failure.
        check(app, driver, 1130, "", 0, UpdateFailure::Link, "找不到网络「home」。");
        check(app, driver, 1130, "not found", 404);
        check(app, driver, 1130, "{\"manifest\":1,\"board\":\"other\"}", 200, UpdateFailure::None, "", 500);
        check(app, driver, 1130, update_manifest("0.2"), 200, UpdateFailure::None, "", 500);
        check(app, driver, 1130, "", 0, UpdateFailure::Internet, "无法解析更新服务器的地址", 500);
        assert(!fs::exists(root / "PicoPage/cache" / update::state_cache));
        // A request the worker has not taken ends at once; one it holds ends with its completion.
        driver.tap(500, 1130); driver.present(); assert(app.update_busy());
        driver.tap(300, 1130); driver.present(); assert(!app.update_busy() && !app.take_update_request());
        driver.tap(500, 1130); driver.present();
        auto held = app.take_update_request(); assert(held);
        driver.tap(300, 1130); driver.present();
        assert(app.update_busy() && !network::live(held->cancelled));
        step(app, driver, 300, 1130); assert(app.update_busy());
        app.complete_update(held->id + 1, UpdateFailure::None, 200, update_manifest("0.2.0-beta.2"), "");
        assert(app.update_busy() && !app.pending_frame());
        app.complete_update(held->id, UpdateFailure::None, 200, update_manifest("0.2.0-beta.2"), "");
        driver.present(); assert(!app.update_busy() && !fs::exists(root / "PicoPage/cache" / update::state_cache));

        check(app, driver, 1130, update_manifest("0.2.0-beta.2", update::progress_schema,
              "## 新增\\n" + [] { std::string items; for (int i = 0; i < 60; ++i) items += "- 第 " + std::to_string(i) + " 项改动，说明文字较长以便换行和分屏。\\n"; return items; }()), 200, UpdateFailure::None, "", 500);
        auto cached = state();
        assert(cached.manifest && cached.manifest->version == "0.2.0-beta.2" && cached.pending_to.empty());
        // Release notes span several screens; K1/K3 turn them and the header returns.
        driver.tap(300, 760); driver.present();
        const auto first = app.canvas().pixels();
        driver.key(Input::K3); driver.present(); assert(app.canvas().pixels() != first);
        driver.key(Input::K1); driver.present(); assert(app.canvas().pixels() == first);
        driver.key(Input::K1); assert(!app.pending_frame());
        driver.tap(60, 50); driver.present(); assert(app.view() == View::Update);

        // Installation needs 20% or a charger; an unknown level without one is refused.
        step(app, driver, 300, 1040); assert(!app.update_busy());
        assert(app.canvas().pixel(24, 1040) == 1);
        app.set_battery(19); step(app, driver, 300, 1040); assert(!app.update_busy());
        assert(app.canvas().pixel(24, 1040) == 1);
        app.set_battery(std::nullopt, true); driver.tap(300, 1040); driver.present();
        assert(app.update_busy() && !app.open_book("Books/none.epub"));
        auto download = app.take_update_request();
        assert(download && download->kind == UpdateOperation::Kind::Download && download->size == 2936012 &&
               download->url == cached.manifest->url && download->sha256 == std::string(64, 'a') &&
               download->version == "0.2.0-beta.2");
        // Progress redraws once per 5% step, inside the progress area only.
        app.update_progress(download->id, 2936012 / 2);
        assert(app.pending_frame() && app.pending_frame()->region && app.pending_frame()->region->y == 300);
        driver.present();
        app.update_progress(download->id, 2936012 / 2 + 4096); assert(!app.pending_frame());
        app.update_progress(download->id + 1, 2936012); assert(!app.pending_frame());
        app.update_progress(download->id, 2936012); assert(app.pending_frame() && !app.pending_frame()->region);
        driver.present();
        // A failed download keeps the running firmware and can be repeated.
        app.complete_update(download->id, UpdateFailure::Mismatch, 0, "", ""); driver.present();
        assert(!app.update_busy() && app.take_power_request().action == PowerAction::None);
        driver.tap(300, 1020); driver.present(); assert(app.update_busy());
        download = app.take_update_request(); assert(download && download->kind == UpdateOperation::Kind::Download);
        // Losing the page owner's network permission cancels the download.
        app.stop_wifi(); if (app.pending_frame()) driver.present();
        assert(!network::live(download->cancelled) && app.update_busy());
        app.complete_update(download->id, UpdateFailure::Cancelled, 0, "", ""); driver.present();
        assert(!app.update_busy() && app.view() == View::Update);
        driver.tap(300, 1040); driver.present();
        download = app.take_update_request(); assert(download);
        app.complete_update(download->id, UpdateFailure::None, 0, "", ""); driver.present();
        // "Later" keeps the verified image for this power session.
        driver.tap(300, 1130); driver.present(); assert(app.view() == View::Settings);
        driver.tap(300, 856); driver.present(); assert(app.view() == View::Update);
        // The battery is judged again at the restart: a download can outlast the charge or the charger.
        app.set_battery(19);
        step(app, driver, 300, 1020); assert(app.take_power_request().action == PowerAction::None);
        app.set_battery(40);
        driver.tap(300, 1020);
        auto drained = app.take_power_request(); assert(drained.install);
        app.set_battery(std::nullopt);
        assert(!app.prepare_power(drained) && app.view() == View::Update); driver.present();
        assert(!fs::exists(root / "PicoPage/cache" / update::state_cache) || state().pending_to.empty());
        app.set_battery(std::nullopt, true);
        // The restart request leaves no record: one is written only after the boot slot was switched,
        // so a start that never reached the new firmware is not reported as its failure.
        driver.tap(300, 1020);
        const auto power = app.take_power_request();
        assert(power.action == PowerAction::Restart && power.install && !power.without_save);
        assert(app.prepare_power(power) && app.view() == View::PowerOff); driver.present();
        assert(app.close_power_files() && state().pending_to.empty() && state().pending_from.empty());
        app.record_install();
        cached = state();
        // The cached notes travel with the marker, so the result page can show them offline.
        assert(cached.pending_from == "0.2.0-beta.1" && cached.pending_to == "0.2.0-beta.2" &&
               cached.manifest && cached.manifest->version == "0.2.0-beta.2");

    }
    {
        // The installed version reports success once.
        ReaderApp app(root.string()); Driver driver{app};
        app.set_firmware_version("0.2.0-beta.2");
        assert(app.initialize() && app.view() == View::Update); driver.present();
        // The cached notes of this version are shown and can be paged before leaving.
        const auto summary = app.canvas().pixels();
        driver.key(Input::K3); driver.present(); assert(app.canvas().pixels() != summary);
        driver.tap(300, 1050); driver.present();
        assert(app.view() == View::Library && state().pending_to.empty() && state().skipped.empty());
    }
    {
        ReaderApp app(root.string()); Driver driver{app};
        app.set_firmware_version("0.2.0-beta.2");
        assert(app.initialize() && app.view() == View::Library); driver.present();
    }
    {
        // The previous version running again means that the installed one did not start; it is then withheld.
        update::State pending;
        pending.pending_from = "0.2.0-beta.1"; pending.pending_to = "0.2.0-beta.2";
        write(root / "PicoPage/cache" / update::state_cache, update::encode_state(pending));
        ReaderApp app(root.string()); Driver driver{app};
        app.set_firmware_version("0.2.0-beta.1"); app.set_battery(80);
        assert(app.initialize() && app.view() == View::Update); driver.present();
        step(app, driver, 300, 300); step(app, driver, 600, 50);
        driver.key(Input::K3); assert(!app.pending_frame() && app.view() == View::Update);
        driver.tap(300, 1130); driver.present();
        assert(app.view() == View::Library && state().pending_to.empty() && state().skipped == "0.2.0-beta.2");
        driver.tap(600, 50); driver.present(); driver.tap(300, 856); driver.present(); assert(app.view() == View::Update);
        check(app, driver, 1130, update_manifest("0.2.0-beta.2"));
        step(app, driver, 300, 1040); assert(!app.update_busy() && state().skipped == "0.2.0-beta.2");
        // A later version lifts the hold; a newer data format asks before it installs.
        check(app, driver, 1130, update_manifest("0.2.0-beta.3", update::progress_schema + 1));
        assert(state().skipped.empty());
        driver.tap(300, 1040); driver.present(); assert(!app.update_busy());
        driver.tap(300, 1130); driver.present(); assert(!app.update_busy());
        driver.tap(300, 1040); driver.present(); driver.tap(300, 1020); driver.present();
        const auto download = app.take_update_request();
        assert(download && download->version == "0.2.0-beta.3");
        app.complete_update(download->id, UpdateFailure::Network, 0, "", ""); driver.present();
        driver.tap(300, 1130); driver.present(); assert(!app.update_busy() && app.view() == View::Update);
    }
    {
        // A development build cannot be ordered: the release is offered, never called newer.
        ReaderApp app(root.string()); Driver driver{app};
        app.set_firmware_version("0.2.0-beta.9+g1234567.dirty"); app.set_battery(80);
        open_page(app, driver);
        // A cached development offer with a newer schema requires confirmation after server validation.
        const auto shown = *state().manifest;
        const auto unchanged = update_manifest(shown.version, shown.progress_schema);
        driver.tap(300, 1040); driver.present();
        auto confirm = app.take_update_request();
        assert(confirm && confirm->kind == UpdateOperation::Kind::Check);
        app.complete_update(confirm->id, UpdateFailure::None, 200, unchanged, ""); driver.present();
        assert(!app.update_busy() && !app.take_update_request());
        driver.tap(300, 1020); driver.present();
        auto download = app.take_update_request();
        assert(download && download->kind == UpdateOperation::Kind::Download && download->version == "0.2.0-beta.3" &&
               download->url == shown.url && download->sha256 == shown.sha256);
        app.complete_update(download->id, UpdateFailure::Cancelled, 0, "", ""); driver.present();
        assert(!app.update_busy());
    }
    for (bool cached : {false, true}) {
        for (int formats = 0; formats < 4; ++formats) {
            const auto card = root / "schema-confirmation";
            fs::remove_all(card);
            write(card / "PicoPage/network.toml", provision::network_toml({{{"home", "password123"}, 0}}));
            const auto body = update_manifest("0.4.0", update::progress_schema + (formats & 1), "",
                                               update::config_schema + ((formats >> 1) & 1));
            update::Manifest manifest;
            std::string error;
            assert(update::parse_manifest(body, manifest, error));
            update::State saved;
            saved.manifest = manifest;
            write(card / "PicoPage/cache" / update::state_cache, update::encode_state(saved));
            ReaderApp app(card.string()); Driver driver{app};
            app.set_firmware_version("0.3.0+g1234567.dirty"); app.set_battery(80);
            open_page(app, driver);
            if (!cached) check(app, driver, 920, body);
            driver.tap(300, 1040); driver.present();
            if (cached) {
                const auto request = app.take_update_request();
                assert(request && request->kind == UpdateOperation::Kind::Check);
                app.complete_update(request->id, UpdateFailure::None, 200, body, ""); driver.present();
            }
            if (formats) {
                assert(!app.update_busy() && !app.take_update_request());
                driver.key(Input::K2); assert(app.pending_frame()); driver.present();
                assert(!app.update_busy() && !app.take_update_request());
                driver.tap(300, 1130); driver.present();
                assert(!app.update_busy() && !app.take_update_request());
                driver.tap(300, 1040); driver.present();
                assert(!app.update_busy() && !app.take_update_request());
                driver.tap(300, 1020); driver.present();
            }
            const auto download = app.take_update_request();
            assert(download && download->kind == UpdateOperation::Kind::Download && download->version == manifest.version &&
                   download->url == manifest.url && download->sha256 == manifest.sha256);
            app.complete_update(download->id, UpdateFailure::Cancelled, 0, "", ""); driver.present();
            assert(!app.update_busy() && !app.take_update_request());
        }
    }
    fs::remove_all(root / "schema-confirmation");
    {
        // A card whose cached manifest names another image never directs a download: the server's
        // answer replaces it and the user is asked again.
        auto forged = state();
        forged.manifest->url = "https://example.com/firmware/pico-page-0.2.0-beta.3-read_pico-release.bin";
        forged.manifest->sha256 = std::string(64, 'f');
        write(root / "PicoPage/cache" / update::state_cache, update::encode_state(forged));
        ReaderApp app(root.string()); Driver driver{app};
        app.set_firmware_version("0.2.0-beta.9+g1234567.dirty"); app.set_battery(80);
        open_page(app, driver);
        driver.tap(300, 1040); driver.present();
        const auto confirm = app.take_update_request();
        assert(confirm && confirm->kind == UpdateOperation::Kind::Check && confirm->url == update::manifest_url);
        app.complete_update(confirm->id, UpdateFailure::None, 200, update_manifest("0.2.0-beta.3", update::progress_schema + 1), "");
        driver.present();
        assert(!app.update_busy() && !app.take_update_request() && state().manifest->sha256 == std::string(64, 'a'));
        // A failed confirmation starts nothing either.
        write(root / "PicoPage/cache" / update::state_cache, update::encode_state(forged));
    }
    {
        ReaderApp app(root.string()); Driver driver{app};
        app.set_firmware_version("0.2.0-beta.9+g1234567.dirty"); app.set_battery(80);
        open_page(app, driver);
        driver.tap(300, 1040); driver.present();
        const auto confirm = app.take_update_request();
        assert(confirm && confirm->kind == UpdateOperation::Kind::Check);
        app.complete_update(confirm->id, UpdateFailure::Network, 0, "", ""); driver.present();
        assert(!app.update_busy() && !app.take_update_request());
        // The cause of the failed check stays on the page after leaving it and opening it again.
        const auto failed = app.canvas().pixels();
        driver.tap(60, 50); driver.present(); assert(app.view() == View::Settings);
        driver.tap(300, 890); driver.present(); assert(app.view() == View::Update && app.canvas().pixels() == failed);
        driver.tap(300, 1040); driver.present();
        const auto again = app.take_update_request();
        assert(again && again->kind == UpdateOperation::Kind::Check);
        app.complete_update(again->id, UpdateFailure::Cancelled, 0, "", ""); driver.present();
    }
    {
        // With Wi-Fi switched off the page leads to the switch instead of a check that cannot start.
        write(root / "PicoPage/settings.toml", "schema_version=1\n[network]\nwifi_enabled=false\n");
        {
            ReaderApp app(root.string()); Driver driver{app};
            app.set_firmware_version("0.2.0-beta.3"); app.set_battery(80);
            open_page(app, driver);
            driver.tap(300, 1130); driver.present();
            assert(app.view() == View::WifiSettings && !app.update_busy());
        }
        for (const bool configured : {true, false}) {
            if (!configured) {
                fs::remove(root / "PicoPage/settings.toml");
                fs::remove(root / "PicoPage/network.toml");
            }
            for (const bool notes : {false, true}) {
                ReaderApp app(root.string()); Driver driver{app};
                app.set_firmware_version("0.2.0-beta.1"); app.set_battery(80);
                open_page(app, driver);
                if (notes) { driver.tap(300, 760); driver.present(); assert(app.view() == View::Update); }
                driver.tap(300, 1040); driver.present();
                assert(app.view() == View::WifiSettings && !app.update_busy() && !app.take_update_request());
            }
        }
        write(root / "PicoPage/network.toml", provision::network_toml({{{"home", "password123"}, 0}}));
        fs::remove(root / "PicoPage/settings.toml");
    }
    {
        // The running release is not offered again, and an older data format is refused.
        ReaderApp app(root.string()); Driver driver{app};
        app.set_firmware_version("0.2.0-beta.3"); app.set_battery(80);
        open_page(app, driver);
        step(app, driver, 300, 1040); assert(!app.update_busy());
        check(app, driver, 1130, update_manifest("0.3.0", update::progress_schema - 1));
        assert(state().manifest->version == "0.3.0");
        step(app, driver, 300, 1040); assert(!app.update_busy());
    }
}
static void return_origin_test(const fs::path& root, const fs::path& fixtures, bool async,
                               const std::string& scenario) {
    fs::create_directories(root / "Books/Sub");
    fs::copy_file(fixtures / "long-chapter.epub", root / "Books/book.epub");
    for (int i = 0; i < 10; ++i)
        fs::copy_file(fixtures / "valid.epub", root / ("Books/Sub/a" + std::to_string(i) + ".epub"));
    const auto config = root / "PicoPage/settings.toml";
    write(config, "schema_version=1\n[library]\nsort='name'\n[power]\nsleep_screen='" +
                  std::string(scenario == "sleep-settings-custom" ? "custom" : "simple") + "'\n");
    ReaderApp app(root.string(), "return-test", true, async); Driver driver{app};
    assert(app.initialize()); driver.present(); opened(app, "Books/book.epub");
    if (scenario == "first-frame") {
        assert(!app.saved_record()); app.complete_frame(false);
        assert(app.view() == View::Error && app.status() == "屏幕更新失败。返回将回到书库。");
        driver.present();
        const auto failure = app.canvas().pixels();
        driver.close(); driver.present(); assert(app.view() == View::Library && !app.saved_record());
        app.show_error("屏幕更新失败。返回将回到书库。"); driver.present();
        // A first-frame failure has the same library action as an error raised in the library.
        assert(std::equal(failure.begin() + 1080 * app.canvas().stride(),
                          failure.begin() + 1176 * app.canvas().stride(),
                          app.canvas().pixels().begin() + 1080 * app.canvas().stride()));
        driver.close(); driver.present(); assert(app.view() == View::Library && !app.saved_record());
        return;
    }
    driver.present();
    driver.key(Input::K3); driver.present(); driver.complete_content();
    const auto saved = *app.saved_record();
    const auto body = app.canvas().pixels();
    const auto library = [&] {
        driver.tap(342, 500); driver.present(); driver.tap(60, 50); driver.present();
        assert(app.view() == View::Library);
    };
    const auto settings = [&] {
        library(); driver.tap(600, 50); driver.present();
        assert(app.view() == View::Settings);
    };
    View expected = View::Library;
    std::vector<std::uint8_t> library_pixels;
    if (scenario == "reading" || scenario == "sleep-reading" || scenario == "offline-reading") expected = View::Reading;
    else if (scenario == "settings" || scenario == "invalid-settings" || scenario == "typography-settings" ||
             scenario == "sleep-settings" || scenario == "sleep-settings-custom" || scenario == "sleep-power" || scenario == "offline-settings" || scenario == "card-settings" || scenario == "local-settings") {
        settings(); expected = View::Settings;
        if (scenario == "settings" || scenario == "sleep-power") {
            driver.tap(340, 640); driver.present(); assert(app.view() == View::PowerSettings);
            driver.tap(600, 750); driver.present();
        } else if (scenario == "typography-settings") {
            driver.tap(300, 400); driver.present(); assert(app.view() == View::Refresh);
            driver.tap(300, 280); driver.present(); driver.complete_content();
            assert(app.view() == View::Typography);
        }
    } else {
        library();
        if (scenario == "retry-save" || scenario == "retry-settings") {
            // Cancelling an open leaves the library usable while the old position is being saved.
            opened(app, "Books/book.epub"); driver.present();
            driver.key(Input::K3); app.complete_frame(true);
            const auto sidecar = root / "Books/book.epub.picopage";
            fs::rename(sidecar, sidecar.string() + ".held"); write(sidecar, "blocked");
            assert(app.open_book("Books/Sub/a0.epub"));
            app.complete_frame(true);
            driver.tap(580, 1170); assert(app.view() == View::Library);
            app.complete_frame(true);
            if (scenario == "retry-settings") { driver.tap(600, 50); app.complete_frame(true); }
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
            while (app.save_busy()) {
                app.poll_background(); assert(std::chrono::steady_clock::now() < deadline);
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            assert(app.view() == View::Error && *app.saved_record() == saved);
            driver.present();
            if (scenario == "retry-settings") {
                driver.close(); driver.present(); assert(app.view() == View::Settings);
                driver.tap(342, 1160); driver.present(); assert(app.view() == View::Error);
            } else {
                driver.tap(500, 1120); driver.present(); assert(app.view() == View::Error);
            }
            fs::remove(sidecar); fs::rename(sidecar.string() + ".held", sidecar);
            driver.tap(500, 1120); driver.present(); driver.complete_content();
            assert(app.view() == (scenario == "retry-settings" ? View::Settings : View::Library) &&
                   app.saved_record()->local_revision == saved.local_revision + 1);
            return;
        }
        driver.tap(300, 480); driver.present(); assert(app.view() == View::Library);
        driver.key(Input::K3); driver.present();
        library_pixels = app.canvas().pixels();
        if (scenario == "details" || scenario == "management") {
            driver.hold(300, 220); driver.present(); driver.complete_content();
            assert(app.view() == View::BookDetails);
            if (scenario == "management") {
                driver.tap(342, 918); driver.present(); assert(app.view() == View::BookManagement);
                for (int i = 0; i < 10; ++i)
                    write(root / ("Books/Sub/a" + std::to_string(i) + ".epub"), "changed");
                driver.tap(500, 1120);
                assert(app.view() == View::Error);
            }
        }
    }
    if (scenario.rfind("offline-", 0) == 0) {
        write(root / "PicoPage/sync.toml", "schema_version=1\ndom_version=20260812\nendpoint='https://example.invalid'\n"
                    "username='reader'\nuserkey='test-only-placeholder'\ndevice_id='return-test'\n");
        write(root / "PicoPage/network.toml", "schema_version=2\n[[wifi]]\nssid='home'\npassword='password'\n");
        std::optional<NetworkOperation> request;
        if (scenario == "offline-settings") {
            driver.tap(300, 310); driver.present(); assert(app.view() == View::SyncSettings);
            driver.tap(300, 620); driver.present(); request = app.take_network_request();
            driver.tap(300, 1040);
        } else {
            driver.sync_view(); request = driver.check_remote(); driver.close();
        }
        assert(request && request->cancelled->load() && app.view() == View::NetworkClosing);
        driver.present(); driver.tap(300, 1120); driver.present();
        const auto returned = app.canvas().pixels();
        app.complete_network(request->id, 200, "{}", "");
        assert(!app.pending_frame() && app.canvas().pixels() == returned && app.view() == expected && *app.saved_record() == saved);
        return;
    }
    if (scenario.rfind("sleep-", 0) == 0) {
        assert(sleep_frame(app)); driver.present(); app.wake();
    } else {
        if (scenario == "settings") {
            fs::create_directory(config.string() + ".tmp");
            driver.tap(500, 1120); assert(app.view() == View::Error);
        } else if (scenario == "invalid-settings") {
            write(config, "schema_version=9\n");
            driver.tap(300, 400); assert(app.view() == View::Error);
        } else if (scenario == "local-settings") {
            driver.tap(300, 310); driver.present();
            driver.tap(300, 210); driver.present(); assert(app.view() == View::TextEntry);
            driver.tap(40, 860); app.poll_background();
            assert(app.pending_frame() && app.pending_frame()->region);
            app.complete_frame(false);
            assert(app.status() == "屏幕局部更新失败。返回将回到设置首页。");
        } else if (scenario == "screen") {
            driver.key(Input::K2); app.complete_frame(false);
            assert(app.status() == "屏幕更新失败。返回将回到书库。");
        } else if (scenario == "card-library") {
            // The library explains a removed card itself instead of stacking an error page on it.
            app.card_unavailable(CardIssue::Removed); driver.present();
            assert(app.view() == View::Library);
            return;
        } else if (scenario.rfind("card-", 0) == 0) app.card_unavailable(CardIssue::Removed);
        else if (scenario != "management") app.show_error("测试错误。请返回。");
        driver.present(); assert(app.view() == View::Error); driver.refresh();
        driver.close();
    }
    driver.present(); driver.complete_content();
    if (scenario == "sleep-settings-custom") {
        app.poll_background();
        assert(app.view() == View::Settings && app.status().find("简洁屏保") != std::string::npos &&
               app.pending_frame() && app.pending_frame()->region);
        driver.present(); app.poll_background(); assert(!app.pending_frame());
    }
    assert(app.view() == expected && *app.saved_record() == saved);
    if (expected == View::Reading) assert(std::equal(body.begin(), body.begin() + 1128 * app.canvas().stride(), app.canvas().pixels().begin()));
    if (expected == View::Library && scenario != "card-library")
        assert(std::equal(library_pixels.begin(), library_pixels.begin() + 1120 * app.canvas().stride(), app.canvas().pixels().begin()));
}

static void body_image_cache_checks(const fs::path& root, const fs::path& fixtures) {
    fs::create_directories(root);
    FileStore store(root.string());
    const std::string name = "images/gray-v1/regression";
    const auto path = root / "PicoPage/cache" / name;
    graphics::Canvas original, readback;
    for (std::size_t i = 0; i < original.pixels().size(); ++i)
        original.data()[i] = static_cast<std::uint8_t>(i * 37);
    const std::string pixels(reinterpret_cast<const char*>(original.data()), original.pixels().size());
    const auto legacy = pixels + md5_hex(pixels);
    assert(store.save_cache(name, legacy) == StoreStatus::Ok);
    const auto read = [&](const std::function<bool()>& stop = {}) {
        std::unique_ptr<CacheReader> reader;
        assert(store.open_cache(name, legacy.size(), reader) == StoreStatus::Ok);
        return read_body_image_cache(*reader, readback, stop);
    };
    assert(read() && readback.pixels() == original.pixels());
    readback.clear(7);
    bool cancel_after_payload = false;
    assert(!read([&] {
        if (cancel_after_payload) return true;
        // Request cancellation immediately after the final pre-digest check reports no cancellation.
        cancel_after_payload = readback.pixels() == original.pixels();
        return false;
    }));
    assert(cancel_after_payload && readback.pixels() == original.pixels());
    unsigned checks = 0;
    assert(!read([&] { return ++checks == 3; }));
    checks = 0;
    assert(!read([&] {
        if (++checks == 2) fs::resize_file(path, 16 * 1024);
        return false;
    }));
    assert(write_body_image_cache(store, name, original, {}));
    std::string saved;
    assert(store.load_cache(name, legacy.size(), saved) == StoreStatus::Ok && saved == legacy);
    for (bool damage_digest : {false, true}) {
        auto damaged = legacy;
        damaged[damage_digest ? damaged.size() - 1 : 0] ^= 1;
        assert(store.save_cache(name, damaged) == StoreStatus::Ok && !read());
    }
    assert(store.save_cache(name, legacy.substr(0, legacy.size() - 1)) == StoreStatus::Ok && !read());
    assert(store.save_cache(name, legacy) == StoreStatus::Ok);
    original.clear(7);
    checks = 0;
    assert(!write_body_image_cache(store, name, original, [&] { return ++checks == 3; }));
    assert(!fs::exists(path.string() + ".tmp"));
    assert(store.load_cache(name, legacy.size(), saved) == StoreStatus::Ok && saved == legacy);
    struct rlimit previous{};
    assert(getrlimit(RLIMIT_FSIZE, &previous) == 0);
    auto limited = previous; limited.rlim_cur = 16 * 1024;
    const auto handler = std::signal(SIGXFSZ, SIG_IGN);
    assert(handler != SIG_ERR && setrlimit(RLIMIT_FSIZE, &limited) == 0);
    const bool written = write_body_image_cache(store, name, original, {});
    assert(setrlimit(RLIMIT_FSIZE, &previous) == 0);
    std::signal(SIGXFSZ, handler);
    assert(!written && !fs::exists(path.string() + ".tmp"));
    assert(store.load_cache(name, legacy.size(), saved) == StoreStatus::Ok && saved == legacy);

    ContentJob::Request request;
    request.kind = ContentJob::Kind::Image; request.root = root.string(); request.cache_name = name;
    request.path = (fixtures / "image-cover.epub").string();
    EpubDocument document; std::string error;
    assert(document.open(request.path, error)); request.identity = document.metadata();
    request.layout.width = 684; request.layout.height = 1216;
    request.page.images.push_back({0, 0, 0, 684, 1216, "EPUB/cover.png"});
    fs::remove(path);
    ContentJob cold(request); assert(cold.start()); cold.wait();
    assert(cold.result.error.empty() && cold.result.image && cold.result.failed_images.empty());
    const auto decoded = graphics::image_decode_count();
    auto unavailable = request; unavailable.path = (root / "missing.epub").string();
    ContentJob hot(unavailable); assert(hot.start()); hot.wait();
    assert(hot.result.image && hot.result.image->pixels() == cold.result.image->pixels());
    assert(graphics::image_decode_count() == decoded && hot.result.error.empty());
    assert(store.load_cache(name, legacy.size(), saved) == StoreStatus::Ok);
    saved[0] ^= 1;
    assert(store.save_cache(name, saved) == StoreStatus::Ok);
    fs::create_directory(path.string() + ".tmp");
    ContentJob fallback(request); assert(fallback.start()); fallback.wait();
    assert(fallback.result.error.empty() && fallback.result.failed_images.empty() && fallback.result.image);
    assert(fallback.result.image->pixels() == cold.result.image->pixels());
    assert(graphics::image_decode_count() == decoded + 1);
    fs::remove(path.string() + ".tmp");
}

static void dictionary_query_failure_test(const fs::path& root, const fs::path& fixtures, const char* font) {
#ifdef PICO_PAGE_WRAP_MALLOC_TEST
    fs::create_directories(root / "Fonts"); fs::copy_file(font, root / "Fonts/LXGWWenKai-Regular.ttf");
    write(root / "Books/query.txt", "中文甲乙丙丁戊己\n");
    fs::create_directories(root / "Dictionaries");
    const auto dict = fixtures.parent_path() / "dictionary-fixtures";
    for (const auto* name : {"bad", "good"})
        for (const auto* suffix : {".ifo", ".idx", ".dict.dz"})
            fs::copy_file(dict / (std::string("query-short") + suffix),
                          root / "Dictionaries" / (std::string(name) + suffix));
    { std::fstream index(root / "Dictionaries/bad.idx", std::ios::binary | std::ios::in | std::ios::out);
      index.seekp(-1, std::ios::end); index.put(static_cast<char>(255)); }
    write(root / "PicoPage/settings.toml", "schema_version=1\n[dictionary]\nenabled=['bad.ifo','good.ifo']\n");
    counted_dictionary = (root / "Dictionaries/bad.dict.dz").string(); dictionary_preparations = 0;
    ReaderApp app(root.string(), "dictionary-query", true, true); Driver driver{app};
    assert(app.initialize()); driver.present(); opened(app, "Books/query.txt"); driver.present();
    const auto saved = app.saved_record();
    int hit_x = -1, hit_y = -1;
    for (int y = 64; y < 150 && hit_x < 0; ++y)
        for (int x = 36; x < 640; ++x) if (app.can_long_press_at(x, y)) { hit_x = x; hit_y = y; break; }
    assert(hit_x >= 0);
    driver.hold(hit_x, hit_y); driver.present(); driver.complete_content();
    assert(app.view() == View::Dictionary && dictionary_preparations == 1);
    const auto short_hit = app.canvas().pixels();
    // The mount session remembers the definitive failure; only explicit retry prepares it again.
    driver.tap(140, 700); driver.present(); driver.complete_content();
    driver.tap(100, 230); driver.present(); driver.complete_content();
    assert(dictionary_preparations == 1 && app.canvas().pixels() != short_hit);
    driver.tap(300, 1090); driver.present(); driver.complete_content();
    assert(dictionary_preparations == 2);
    driver.tap(600, 600); driver.present();
    driver.hold(hit_x, hit_y); driver.present(); driver.complete_content();
    assert(dictionary_preparations == 2 && app.canvas().pixels() == short_hit);
    assert(app.saved_record() == saved);
    counted_dictionary.clear();
#else
    (void)root; (void)fixtures; (void)font;
#endif
}

static void dictionary_ui_test(const fs::path& root, const fs::path& fixtures, const char* font) {
    fs::create_directories(root / "Fonts"); fs::copy_file(font, root / "Fonts/LXGWWenKai-Regular.ttf");
    write(root / "Books/dictionary.txt", "omega apple alias duplicate 中文\nReading continues on the same page.\n");
    const auto dict = fixtures.parent_path() / "dictionary-fixtures";
    fs::create_directories(root / "Dictionaries/English");
    for (const auto* suffix : {".ifo", ".idx", ".dict.dz", ".syn.dz"})
        fs::copy_file(dict / (std::string("compressed") + suffix), root / "Dictionaries/English" / (std::string("main") + suffix));
    write(root / "PicoPage/settings.toml", "schema_version=1\n[dictionary]\nenabled=['Missing/main.ifo','English/main.ifo']\n");
    ReaderApp app(root.string(), "dictionary", true, true); Driver driver{app};
    assert(app.initialize()); driver.present(); opened(app, "Books/dictionary.txt"); driver.present();
    const auto saved = app.saved_record(); assert(saved);
    const auto body = app.canvas().pixels();
    int hit_x = -1, hit_y = -1;
    for (int y = 64; y < 150 && hit_x < 0; ++y)
        for (int x = 36; x < 640; ++x) if (app.can_long_press_at(x, y)) { hit_x = x; hit_y = y; break; }
    assert(hit_x >= 0);
    const auto shot = [&](const char* name, const ReaderApp& shown) {
        const char* folder = std::getenv("PICO_PAGE_DICTIONARY_SHOTS");
        if (!folder) return;
        fs::create_directories(folder);
        std::ofstream file(fs::path(folder) / (std::string(name) + ".pgm"), std::ios::binary);
        file << "P5\n684 1216\n255\n";
        for (int y = 0; y < 1216; ++y) for (int x = 0; x < 684; ++x) file.put(static_cast<char>(shown.canvas().pixel(x, y) * 17));
        assert(file.good());
    };
    std::uint64_t contact_sequence = 0;
    const auto contact = [&](Phase phase, int x, int y, bool hold = false, bool deferred = false) {
        if (phase == Phase::Begin) contact_sequence = ++driver.sequence;
        AppInput input{{contact_sequence, phase, Input::TouchCenter}, true, x, y, deferred, hold};
        input.continuous = true; app.input(input);
    };
    // Moves and release update logical endpoints without modifying the in-flight canvas.
    contact(Phase::Begin, hit_x, hit_y, true);
    assert(app.view() == View::Dictionary && !app.content_busy() && app.pending_frame());
    const auto selecting = app.canvas().pixels();
    contact(Phase::Move, 400, hit_y + 130);
    contact(Phase::Move, 250, hit_y + 130);
    assert(app.canvas().pixels() == selecting && !app.content_busy());
    driver.present(); app.poll_background(); driver.present();
    assert(app.canvas().pixels() != selecting && !app.content_busy());
    shot("dictionary-selection", app);
    const auto before_release = app.canvas().pixels();
    contact(Phase::Move, 260, hit_y + 130, false, true);
    app.poll_background(true);
    assert(!app.pending_frame() && app.canvas().pixels() == before_release);
    // After the preview, a slow lookup shows its loading panel only after dictionary_panel_delay.
    ContentJob::delay_starts_for_test(ContentJob::Kind::DictionaryLookup, std::chrono::milliseconds(800));
    contact(Phase::End, 250, hit_y + 130);
    assert(app.content_busy() && !app.pending_frame());
    app.poll_background(); assert(!app.pending_frame() && app.canvas().pixels() == before_release);
    std::this_thread::sleep_for(std::chrono::milliseconds(550));
    app.poll_background(); assert(app.pending_frame() && app.content_busy());
    driver.present();
    ContentJob::delay_starts_for_test(ContentJob::Kind::DictionaryLookup, std::chrono::milliseconds(0));
    unsigned slow_frames = 0;
    driver.frame_observer = [&](const Frame&) { ++slow_frames; };
    driver.complete_content();
    driver.frame_observer = {};
    assert(slow_frames == 1 && app.view() == View::Dictionary);
    assert(app.saved_record() == saved);
    driver.tap(600, 600); driver.present();
    assert(app.canvas().pixels() == body);
    // Cancellation during a refresh consumes the late release and restores the same body.
    contact(Phase::Begin, hit_x, hit_y, true);
    contact(Phase::Cancel, hit_x, hit_y);
    assert(app.view() == View::Dictionary && app.pending_frame());
    driver.present(); app.poll_background(); driver.present();
    contact(Phase::End, hit_x, hit_y);
    assert(app.view() == View::Reading && !app.pending_frame() && !app.content_busy());
    assert(app.canvas().pixels() == body && app.saved_record() == saved);
    contact(Phase::Begin, hit_x, hit_y, true, true);
    contact(Phase::End, hit_x, hit_y);
    assert(app.view() == View::Reading && !app.pending_frame());
    // The power path cancels contact ownership before creating its sleep feedback frame.
    contact(Phase::Begin, hit_x, hit_y, true); driver.present();
    contact(Phase::Cancel, 0, 0, false, true);
    assert(!app.pending_frame() && app.begin_sleep()); driver.present();
    app.cancel_sleep(); if (app.pending_frame()) driver.present();
    app.poll_background(); if (app.pending_frame()) driver.present();
    contact(Phase::End, hit_x, hit_y);
    assert(app.view() == View::Reading && app.canvas().pixels() == body && app.saved_record() == saved);
    contact(Phase::Begin, hit_x, hit_y, true); driver.present();
    opened(app, "Books/dictionary.txt"); driver.present();
    contact(Phase::Move, 400, hit_y + 130);
    contact(Phase::End, 400, hit_y + 130);
    assert(app.view() == View::Reading && !app.pending_frame() && app.saved_record() == saved);
    driver.refresh();
    // A release during the preview frame starts lookup only after that frame completes.
    contact(Phase::Begin, hit_x, hit_y, true);
    const auto preview = app.canvas().pixels();
    contact(Phase::End, hit_x, hit_y);
    assert(!app.content_busy() && app.canvas().pixels() == preview);
    driver.present(); app.poll_background(); driver.complete_content();
    driver.tap(636, 600); driver.present();
    // A fast lookup followed by slow page layout still shows the loading panel after the delay.
    ContentJob::delay_starts_for_test(ContentJob::Kind::DictionaryPage, std::chrono::milliseconds(800));
    contact(Phase::Begin, hit_x, hit_y, true); driver.present();
    contact(Phase::End, hit_x, hit_y); app.poll_background();
    assert(!app.pending_frame());
    std::this_thread::sleep_for(std::chrono::milliseconds(550));
    app.poll_background(); app.poll_background();
    assert(app.pending_frame() && app.content_busy());
    driver.present();
    ContentJob::delay_starts_for_test(ContentJob::Kind::DictionaryPage, std::chrono::milliseconds(0));
    driver.complete_content();
    driver.tap(636, 600); driver.present();
    contact(Phase::Begin, hit_x, hit_y, true); driver.present();
    contact(Phase::End, hit_x, hit_y);
    // A fast lookup replaces the preview with its result without a loading panel.
    assert(app.content_busy() && !app.pending_frame());
    unsigned fast_frames = 0;
    driver.frame_observer = [&](const Frame&) { ++fast_frames; };
    driver.complete_content();
    driver.frame_observer = {};
    assert(fast_frames == 1);
    const auto automatic = app.canvas().pixels();
    // The removed edit control has no hit area; selection remains available through its handles.
    driver.tap(480, 600);
    assert(!app.content_busy() && !app.pending_frame() && app.canvas().pixels() == automatic);
    int handle_x = -1, handle_y = -1;
    for (int y = 0; y < 1128 && handle_x < 0; ++y)
        for (int x = 0; x < 684; ++x) if (app.can_drag_selection_at(x, y)) { handle_x = x + 22; handle_y = y + 22; break; }
    assert(handle_x >= 0);
    // Grabbing and releasing the handle must not jump to the adjacent line.
    contact(Phase::Begin, handle_x, handle_y);
    driver.present();
    driver.key(Input::K3); assert(!app.pending_frame());
    contact(Phase::End, handle_x, handle_y);
    app.poll_background(); driver.complete_content();
    assert(app.canvas().pixels() == automatic);
    contact(Phase::Begin, handle_x, handle_y);
    driver.present();
    contact(Phase::Move, 400, hit_y); driver.present();
    contact(Phase::End, 400, hit_y); driver.complete_content();
    assert(app.canvas().pixels() != automatic && app.saved_record() == saved);
    driver.tap(600, 600); driver.present();
    driver.hold(hit_x, hit_y); assert(app.view() == View::Dictionary && app.content_busy());
    driver.present();
    // The missing first source falls through to the next one behind the loading panel; only the result is presented.
    unsigned lookup_frames = 0;
    driver.frame_observer = [&](const Frame&) { ++lookup_frames; };
    driver.complete_content();
    driver.frame_observer = {};
    assert(lookup_frames == 1);
    assert(app.view() == View::Dictionary && app.saved_record() == saved);
    assert(app.canvas().pixels() != body); shot("dictionary-panel", app);
    const auto first = app.canvas().pixels();
    driver.refresh();
    driver.key(Input::K3); driver.present(); driver.complete_content();
    assert(app.canvas().pixels() != first && app.saved_record() == saved);
    driver.key(Input::K1); driver.present(); driver.complete_content();
    assert(app.canvas().pixels() == first);
    fs::rename(root / "Fonts/LXGWWenKai-Regular.ttf", root / "Fonts/held.ttf");
    driver.key(Input::K3); driver.present(); driver.complete_content();
    assert(app.view() == View::Dictionary && app.saved_record() == saved);
    fs::rename(root / "Fonts/held.ttf", root / "Fonts/LXGWWenKai-Regular.ttf");
    driver.key(Input::K3); driver.present(); driver.complete_content();
    driver.key(Input::K1); driver.present(); driver.complete_content();
    assert(app.canvas().pixels() == first);
    // Source selection and management cancellation retain the displayed result and book position.
    driver.tap(140, 700); driver.present(); driver.complete_content(); shot("dictionary-sources", app);
    driver.refresh(); driver.tap(600, 50); driver.present();
    assert(app.canvas().pixels() == first && app.saved_record() == saved);
    driver.tap(140, 700); driver.present(); driver.complete_content();
    driver.tap(100, 350); driver.present();
    assert(app.canvas().pixels() == first && !app.content_busy());
    driver.tap(140, 700); driver.present(); driver.complete_content();
    driver.tap(400, 1140); driver.present(); driver.complete_content();
    assert(app.view() == View::DictionarySettings);
    driver.tap(100, 1140); driver.present(); driver.complete_content();
    driver.tap(500, 1140); driver.present();
    assert(app.view() == View::Dictionary && app.canvas().pixels() == first);
    ContentJob::delay_starts_for_test(ContentJob::Kind::DictionaryList, std::chrono::milliseconds(200));
    driver.tap(140, 700); driver.present(); app.poll_background();
    driver.tap(600, 50); driver.present();
    assert(app.canvas().pixels() == first);
    std::this_thread::sleep_for(std::chrono::milliseconds(220)); app.poll_background();
    assert(app.canvas().pixels() == first && !app.pending_frame());
    ContentJob::delay_starts_for_test(ContentJob::Kind::DictionaryList, std::chrono::milliseconds(0));
    // Changing source does not fall through to another source when the selected one fails.
    driver.tap(140, 700); driver.present(); driver.complete_content();
    driver.tap(100, 230); driver.present(); driver.complete_content();
    assert(app.canvas().pixels() != first && app.saved_record() == saved);
    shot("dictionary-source-error", app);
    const auto unavailable_source = app.canvas().pixels();
    driver.tap(300, 1090); driver.present(); driver.complete_content();
    assert(app.canvas().pixels() == unavailable_source);
    driver.tap(140, 700); driver.present(); driver.complete_content();
    driver.tap(100, 350); driver.present(); driver.complete_content();
    assert(app.view() == View::Dictionary && app.saved_record() == saved);
    const auto chosen_source = app.canvas().pixels();
    const auto chosen_canvas = app.canvas();
    assert(chosen_source == first);
    driver.tap(574, 700);
    driver.tap(550, 1090);
    assert(!app.content_busy() && !app.pending_frame() && app.canvas().pixels() == chosen_source);
    driver.tap(516, 700);
    assert(!app.content_busy() && !app.pending_frame() && app.canvas().pixels() == chosen_source);
    driver.tap(316, 700); driver.present(); driver.complete_content();
    assert(app.view() == View::Dictionary && app.saved_record() == saved && app.canvas().pixels() != chosen_source);
    // Exhausting earlier sources changes only the previous-source icon, retaining the definition.
    for (int y = 768; y < 1128; ++y)
        for (int x = 0; x < 684; ++x)
            assert(app.canvas().pixel(x, y) == chosen_canvas.pixel(x, y));
    const auto exhausted = app.canvas().pixels();
    driver.tap(316, 700);
    assert(!app.content_busy() && !app.pending_frame());
    driver.tap(516, 700);
    assert(!app.content_busy() && !app.pending_frame() && app.canvas().pixels() == exhausted);
    shot("dictionary-source-navigation", app);
    driver.tap(600, 600); driver.present(); driver.hold(hit_x, hit_y); driver.present(); driver.complete_content();
    assert(app.canvas().pixels() == first);
    driver.key(Input::K3); driver.present(); driver.complete_content();
    const auto second_page = app.canvas().pixels();
    const auto second_canvas = app.canvas();
    // A failed reflow keeps the previous layout and can be retried after the font returns.
    fs::rename(root / "Fonts/LXGWWenKai-Regular.ttf", root / "Fonts/held.ttf");
    driver.tap(636, 1090); driver.present(); driver.complete_content();
    assert(app.view() == View::Dictionary && app.saved_record() == saved);
    fs::rename(root / "Fonts/held.ttf", root / "Fonts/LXGWWenKai-Regular.ttf");
    driver.tap(636, 1090); driver.present(); driver.complete_content(); shot("dictionary-expanded", app);
    assert(app.view() == View::Dictionary && app.saved_record() == saved);
    for (int y = 0; y < 42; ++y)
        for (int x = 36; x < 648; ++x)
            assert(app.canvas().pixel(x, 200 + y) == second_canvas.pixel(x, 768 + y));
    driver.tap(636, 1170); driver.present(); driver.complete_content();
    assert(app.canvas().pixels() == second_page);
    driver.key(Input::K1); driver.present(); driver.complete_content();
    assert(app.canvas().pixels() == first);
    driver.tap(600, 600); driver.present();
    assert(app.view() == View::Reading && app.saved_record() == saved && app.canvas().pixels() == body);
    // Closing a delayed lookup discards its result without waiting in input dispatch.
    ContentJob::delay_starts_for_test(ContentJob::Kind::DictionaryLookup, std::chrono::milliseconds(200));
    driver.hold(hit_x, hit_y); driver.present(); app.poll_background();
    driver.tap(600, 600); driver.present();
    assert(app.view() == View::Reading && app.saved_record() == saved);
    std::this_thread::sleep_for(std::chrono::milliseconds(220)); app.poll_background();
    assert(app.view() == View::Reading && !app.pending_frame());
    driver.hold(hit_x, hit_y); driver.present(); app.poll_background();
    contact(Phase::Begin, handle_x, handle_y); driver.present();
    const auto edited = app.canvas().pixels();
    std::this_thread::sleep_for(std::chrono::milliseconds(220)); app.poll_background();
    assert(app.view() == View::Dictionary && !app.pending_frame() && app.canvas().pixels() == edited);
    contact(Phase::Cancel, handle_x, handle_y); driver.present();
    assert(app.view() == View::Reading && app.canvas().pixels() == body);
    driver.hold(hit_x, hit_y); driver.present(); app.poll_background();
    assert(app.show_power_menu() && app.power_menu_pending());
    const auto menu_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (app.power_menu_pending()) {
        app.poll_background(); if (app.pending_frame()) driver.present();
        assert(std::chrono::steady_clock::now() < menu_deadline);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    assert(app.view() == View::PowerMenu && app.saved_record() == saved);
    driver.tap(300, 1120); driver.present(); assert(app.view() == View::Dictionary);
    driver.tap(600, 600); driver.present();
    driver.hold(hit_x, hit_y); driver.present(); app.poll_background();
    assert(app.begin_sleep()); driver.present();
    assert(!app.prepare_sleep() && app.sleep_pending());
    app.cancel_sleep(); if (app.pending_frame()) driver.present(); driver.complete_content();
    assert(app.view() == View::Dictionary && app.saved_record() == saved);
    driver.tap(600, 600); driver.present();
    driver.hold(hit_x, hit_y); driver.present(); app.poll_background();
    assert(sleep_frame(app)); driver.present();
    assert(app.view() == View::Sleeping && app.saved_record() == saved);
    app.wake(); driver.present(); driver.complete_content();
    assert(app.view() == View::Reading && app.saved_record() == saved);
    driver.hold(hit_x, hit_y); driver.present(); app.poll_background();
    assert(!app.prepare_usb_transfer());
    driver.complete_content();
    assert(app.prepare_usb_transfer() && app.saved_record() == saved);
    app.show_usb_transfer(true, {}); driver.present();
    assert(app.view() == View::UsbTransfer);
    ContentJob::delay_starts_for_test(ContentJob::Kind::DictionaryLookup, std::chrono::milliseconds(0));
    // Reinitialize after the simulated USB handoff, as the device does on transfer exit.
    ReaderApp management(root.string(), "dictionary", true, true); Driver manager{management};
    assert(management.initialize()); manager.present();
    opened(management, "Books/dictionary.txt"); manager.present();
    const auto manage = [&] {
    auto& app = management; auto& driver = manager;
    driver.tap(340, 500); driver.present(); assert(app.view() == View::Tools);
    driver.tap(60, 50); driver.present(); assert(app.view() == View::Library);
    driver.tap(600, 50); driver.present(); assert(app.view() == View::Settings);
    driver.tap(300, 400); driver.present(); assert(app.view() == View::Refresh);
    driver.tap(300, 980); driver.present(); driver.complete_content();
    assert(app.view() == View::DictionarySettings); shot("dictionary-settings", app);
    driver.refresh();
    FileStore store(root.string());
    driver.tap(580, 190); driver.present();
    assert((store.load_settings().settings.dictionary_enabled == std::vector<std::string>{"English/main.ifo"}));
    driver.tap(580, 370); driver.present();
    assert((store.load_settings().settings.dictionary_enabled == std::vector<std::string>{"English/main.ifo", "Missing/main.ifo"}));
    driver.tap(120, 480); driver.present();
    assert((store.load_settings().settings.dictionary_enabled == std::vector<std::string>{"Missing/main.ifo", "English/main.ifo"}));
    const auto management_frame = app.canvas().pixels();
    driver.tap(540, 480);
    assert(app.view() == View::DictionarySettings && !app.content_busy() && !app.pending_frame());
    assert(app.canvas().pixels() == management_frame);
    const auto prior = read(root / "PicoPage/settings.toml");
    write(root / "PicoPage/settings.toml", prior + "\n# external modification\n");
    driver.tap(580, 190); driver.present();
    assert(read(root / "PicoPage/settings.toml") == prior + "\n# external modification\n");
    driver.tap(500, 1140); driver.present(); assert(app.view() == View::Settings);
    };
    manage();
    // Returning beyond the cursor history window must reproduce the original definition pages.
    {
        const auto card = root / "Rich";
        fs::create_directories(card / "Fonts"); fs::copy_file(font, card / "Fonts/LXGWWenKai-Regular.ttf");
        fs::create_directories(card / "Dictionaries/Sample");
        for (const auto* suffix : {".ifo", ".idx", ".dict.dz"})
            fs::copy_file(dict / (std::string("ui-rich") + suffix), card / "Dictionaries/Sample" / (std::string("main") + suffix));
        write(card / "PicoPage/settings.toml", "schema_version=1\n[dictionary]\nenabled=['Sample/main.ifo']\n");
        const std::vector<std::string> queries{"sample", "abcdefghabcdefghabcdefghabcdefghabcdefghabcdefghabcdefghabcdefgh", "missing", "發"};
        for (std::size_t query = 0; query < queries.size(); ++query) {
            const auto book = "Books/sample-" + std::to_string(query) + ".txt";
            write(card / book, queries[query] + "\n");
            ReaderApp reader(card.string(), "dictionary", true, true); Driver d{reader};
            assert(reader.initialize()); d.present(); opened(reader, book); d.present();
            const auto original = reader.canvas().pixels();
            const auto position = reader.saved_record();
            int x = -1, y = -1;
            for (int row = 64; row < 150 && x < 0; ++row)
                for (int col = 36; col < 640; ++col) if (reader.can_long_press_at(col, row)) { x = col; y = row; break; }
            assert(x >= 0); d.hold(x, y); d.present(); d.complete_content();
            assert(reader.view() == View::Dictionary);
            shot(query == 0 ? "dictionary-rich-text" : query == 1 ? "dictionary-long-query" :
                 query == 2 ? "dictionary-missing-glyph" : "dictionary-traditional-headword", reader);
            if (query == 3) {
                graphics::Font expected_font; assert(expected_font.open(font) && expected_font.set_pixel_size(30));
                assert(!expected_font.missing(U'發'));
                graphics::Canvas expected;
                expected.clear();
                assert(expected_font.draw(expected, U'發', 24, 584 + expected_font.metrics().ascender, 1, true));
                for (int row = 584; row < 620; ++row)
                    for (int col = 24; col < 64; ++col)
                        assert(reader.canvas().pixel(col, row) == expected.pixel(col, row));
            }
            if (query == 0) {
                const auto first_page = reader.canvas().pixels();
                auto previous = first_page;
                for (int page = 0; page < 130; ++page) {
                    d.key(Input::K3); d.present(); d.complete_content();
                    assert(reader.canvas().pixels() != previous && reader.saved_record() == position);
                    previous = reader.canvas().pixels();
                }
                shot("dictionary-page-131", reader);
                for (int page = 0; page < 130; ++page) {
                    d.key(Input::K1); d.present(); d.complete_content();
                    assert(reader.canvas().pixels() != previous && reader.saved_record() == position);
                    previous = reader.canvas().pixels();
                }
                assert(reader.canvas().pixels() == first_page);
            }
            d.refresh(); d.tap(600, 600); d.present();
            assert(reader.view() == View::Reading && reader.canvas().pixels() == original && reader.saved_record() == position);
        }
    }
    // Panel placement follows the final cross-line selection, not the initial contact.
    {
        const auto card = root / "Placement";
        fs::create_directories(card / "Fonts"); fs::copy_file(font, card / "Fonts/LXGWWenKai-Regular.ttf");
        std::string lines;
        for (int i = 0; i < 16; ++i) lines += "apple\n";
        write(card / "Books/sample.txt", lines);
        ReaderApp reader(card.string(), "dictionary", true, true); Driver d{reader};
        assert(reader.initialize()); d.present(); opened(reader, "Books/sample.txt"); d.present();
        const auto original = reader.canvas().pixels();
        const auto position = reader.saved_record();
        const auto first_hit = [&](int from, int until) {
            for (int y = from; y < until; ++y)
                for (int x = 36; x < 640; ++x)
                    if (reader.can_long_press_at(x, y)) return std::pair<int, int>{x, y};
            assert(false); return std::pair<int, int>{0, 0};
        };
        const auto upper_start = first_hit(450, 550), lower_end = first_hit(850, 1000);
        const auto lower_start = first_hit(650, 750), upper_end = first_hit(280, 380);
        for (bool downward : {true, false}) {
            const auto start = downward ? upper_start : lower_start;
            const auto end = downward ? lower_end : upper_end;
            ++d.sequence;
            AppInput input{{d.sequence, Phase::Begin, Input::TouchCenter}, true, start.first, start.second, false, true};
            input.continuous = true; reader.input(input); d.present();
            input.event.phase = Phase::End; input.x = end.first; input.y = end.second; input.long_press = false;
            reader.input(input); d.present(); d.complete_content();
            const int top = downward ? 0 : 568;
            assert(reader.view() == View::Dictionary && reader.saved_record() == position);
            assert(reader.canvas().pixel(0, top) == 1);
            assert(reader.canvas().pixel(0, downward ? 568 : 0) == 15);
            d.tap(320, top + 400); assert(!reader.pending_frame() && !reader.content_busy());
            d.tap(100, top + 480); assert(!reader.pending_frame() && !reader.content_busy());
            d.tap(260, top + 480); assert(!reader.pending_frame() && !reader.content_busy());
            shot(downward ? "dictionary-panel-top" : "dictionary-panel-bottom", reader);
            const auto empty_result = reader.canvas().pixels();
            d.tap(300, top + 520); d.present(); d.complete_content();
            assert(reader.view() == View::DictionarySettings);
            d.tap(500, 1140); d.present();
            assert(reader.view() == View::Dictionary && reader.canvas().pixels() == empty_result);
            d.refresh();
            d.tap(600, top + 36); d.present();
            assert(reader.view() == View::Reading && reader.canvas().pixels() == original);
        }
    }
    // Source lists paginate independently; settings changes return to the same book and selection.
    {
        const auto card = root / "Sources";
        fs::create_directories(card / "Fonts"); fs::copy_file(font, card / "Fonts/LXGWWenKai-Regular.ttf");
        write(card / "Books/sample.txt", "apple\n");
        std::string enabled;
        for (int i = 0; i < 6; ++i) {
            const auto name = std::to_string(i);
            fs::create_directories(card / "Dictionaries" / name);
            for (const auto* suffix : {".ifo", ".idx", ".dict.dz", ".syn.dz"})
                fs::copy_file(dict / (std::string("compressed") + suffix), card / "Dictionaries" / name / (std::string("main") + suffix));
            if (i) enabled += ",";
            enabled += "'" + name + "/main.ifo'";
        }
        write(card / "PicoPage/settings.toml", "schema_version=1\n[dictionary]\nenabled=[" + enabled + "]\n");
        ReaderApp reader(card.string(), "dictionary", true, true); Driver d{reader};
        assert(reader.initialize()); d.present(); opened(reader, "Books/sample.txt"); d.present();
        const auto original = reader.canvas().pixels(); const auto saved_position = reader.saved_record();
        d.hold(130, 94); d.present(); d.complete_content();
        d.tap(140, 700); d.present(); d.complete_content();
        const auto first_list = reader.canvas().pixels();
        d.key(Input::K3); d.present(); assert(reader.canvas().pixels() != first_list);
        d.key(Input::K1); d.present(); assert(reader.canvas().pixels() == first_list);
        d.key(Input::K3); d.present(); d.tap(100, 230); d.present(); d.complete_content();
        d.tap(140, 700); d.present(); d.complete_content(); shot("dictionary-sources-page-2", reader);
        d.tap(400, 1140); d.present(); d.complete_content();
        assert(reader.view() == View::DictionarySettings);
        d.tap(580, 190); d.present();  // Disable one enabled source while preserving the chosen source.
        d.tap(500, 1140); d.present(); d.complete_content();
        assert(reader.view() == View::Dictionary && reader.saved_record() == saved_position);
        d.tap(600, 600); d.present(); assert(reader.canvas().pixels() == original);
    }
    // Result navigation skips unusable sources in both directions and discards a cancelled lookup.
    {
        const auto card = root / "Navigation";
        fs::create_directories(card / "Fonts"); fs::copy_file(font, card / "Fonts/LXGWWenKai-Regular.ttf");
        fs::create_directories(card / "Dictionaries");
        for (const auto* source : {"first", "bad", "absent", "last"})
            for (const auto* suffix : {".ifo", ".idx", ".dict.dz"})
                fs::copy_file(dict / (std::string(std::string_view(source) == "absent" ? "ui-rich" : "compressed") + suffix),
                              card / "Dictionaries" / (std::string(source) + suffix));
        for (const auto* source : {"first", "bad", "last"})
            fs::copy_file(dict / "compressed.syn.dz", card / "Dictionaries" / (std::string(source) + ".syn.dz"));
        { std::fstream index(card / "Dictionaries/bad.idx", std::ios::binary | std::ios::in | std::ios::out);
          index.seekp(-1, std::ios::end); index.put(static_cast<char>(255)); }
        write(card / "Books/sample.txt", "omega\n");
        write(card / "PicoPage/settings.toml", "schema_version=1\n[dictionary]\nenabled=['first.ifo','bad.ifo','missing.ifo','absent.ifo','last.ifo']\n");
        ReaderApp reader(card.string(), "dictionary", true, true); Driver d{reader};
        assert(reader.initialize()); d.present(); opened(reader, "Books/sample.txt"); d.present();
        const auto original = reader.canvas().pixels(); const auto position = reader.saved_record();
        int hit_x = -1, hit_y = -1;
        for (int y = 64; y < 150 && hit_x < 0; ++y)
            for (int x = 36; x < 640; ++x) if (reader.can_long_press_at(x, y)) { hit_x = x; hit_y = y; break; }
        assert(hit_x >= 0); d.hold(hit_x, hit_y); d.present(); d.complete_content();
        const auto first_result = reader.canvas().pixels();
        ContentJob::fail_starts_for_test(ContentJob::Kind::DictionaryLookup, 1, ContentJob::StartFailure::TaskCreate);
        d.tap(516, 700); d.present(); d.complete_content();
        assert(reader.saved_record() == position);
        const auto failed_navigation = reader.canvas().pixels(); assert(failed_navigation != first_result);
        d.tap(240, 1090); d.present(); d.complete_content();
        assert(reader.view() == View::Dictionary && reader.saved_record() == position);
        const auto last_result = reader.canvas().pixels(); assert(last_result != first_result);
        d.tap(516, 700); assert(!reader.content_busy() && !reader.pending_frame());
        shot("dictionary-navigation-skipped", reader);
        d.tap(316, 700); d.present(); d.complete_content();
        assert(reader.canvas().pixels() == first_result && reader.saved_record() == position);
        d.tap(316, 700); assert(!reader.content_busy() && !reader.pending_frame());
        // A new definition cannot use the previous definition's next-page cursor after layout fails.
        ContentJob::fail_starts_for_test(ContentJob::Kind::DictionaryPage, 1, ContentJob::StartFailure::TaskCreate);
        d.tap(516, 700); d.present(); d.complete_content();
        const auto failed_first_page = reader.canvas().pixels();
        d.tap(444, 1090); d.key(Input::K3);
        assert(!reader.content_busy() && !reader.pending_frame() && reader.canvas().pixels() == failed_first_page);
        d.tap(240, 1090); d.present(); d.complete_content();
        assert(reader.canvas().pixels() == last_result && reader.saved_record() == position);
        d.tap(316, 700); d.present(); d.complete_content();
        assert(reader.canvas().pixels() == first_result);
        d.tap(140, 700); d.present(); d.complete_content();
        d.tap(400, 1140); d.present(); d.complete_content();
        assert(reader.view() == View::DictionarySettings); shot("dictionary-known-source-error", reader);
        d.tap(500, 1140); d.present();
        assert(reader.canvas().pixels() == first_result);
        ContentJob::delay_starts_for_test(ContentJob::Kind::DictionaryLookup, std::chrono::milliseconds(200));
        d.tap(516, 700); d.present(); reader.poll_background();
        d.tap(636, 600); d.present();
        std::this_thread::sleep_for(std::chrono::milliseconds(220)); reader.poll_background();
        ContentJob::delay_starts_for_test(ContentJob::Kind::DictionaryLookup, std::chrono::milliseconds(0));
        assert(reader.view() == View::Reading && !reader.pending_frame() && !reader.content_busy());
        assert(reader.canvas().pixels() == original && reader.saved_record() == position);
        // Without a displayed definition, exhausted navigation must keep actionable failure feedback.
        write(card / "Books/unlisted.txt", "unlisted\n");
        opened(reader, "Books/unlisted.txt"); d.present();
        hit_x = hit_y = -1;
        for (int y = 64; y < 150 && hit_x < 0; ++y)
            for (int x = 36; x < 640; ++x) if (reader.can_long_press_at(x, y)) { hit_x = x; hit_y = y; break; }
        assert(hit_x >= 0); d.hold(hit_x, hit_y); d.present(); d.complete_content();
        const auto empty_result = reader.canvas(); const auto empty_position = reader.saved_record();
        d.tap(516, 700); d.present(); d.complete_content();
        for (int y = 768; y < 1044; ++y)
            for (int x = 0; x < 684; ++x) assert(reader.canvas().pixel(x, y) == empty_result.pixel(x, y));
        d.tap(140, 700); d.present(); d.complete_content();
        d.tap(100, 470); d.present(); d.complete_content();
        d.tap(316, 700); d.present(); d.complete_content();
        for (int y = 768; y < 1044; ++y)
            for (int x = 0; x < 684; ++x) assert(reader.canvas().pixel(x, y) == empty_result.pixel(x, y));
        assert(reader.saved_record() == empty_position);
    }
    // A final one-entry batch still exposes the action that wraps to the first result.
    {
        const auto card = root / "Entries";
        fs::create_directories(card / "Fonts"); fs::copy_file(font, card / "Fonts/LXGWWenKai-Regular.ttf");
        fs::create_directories(card / "Dictionaries/Sample");
        for (const auto* suffix : {".ifo", ".idx", ".dict.dz"})
            fs::copy_file(dict / (std::string("ui-entries") + suffix), card / "Dictionaries/Sample" / (std::string("main") + suffix));
        write(card / "Books/sample.txt", "sample\n");
        write(card / "PicoPage/settings.toml", "schema_version=1\n[dictionary]\nenabled=['Sample/main.ifo']\n");
        ReaderApp reader(card.string(), "dictionary", true, true); Driver d{reader};
        assert(reader.initialize()); d.present(); opened(reader, "Books/sample.txt"); d.present();
        const auto position = reader.saved_record();
        d.hold(130, 94); d.present(); d.complete_content();
        const auto first_entry = reader.canvas().pixels();
        for (int index = 0; index < 33; ++index) {
            d.tap(550, 790); d.present(); d.complete_content();
            assert(reader.saved_record() == position);
            if (index == 31) { assert(reader.canvas().pixels() != first_entry); shot("dictionary-entry-33", reader); }
        }
        assert(reader.canvas().pixels() == first_entry);
    }
    // Identical visible text must carry a warning when another field could not be displayed.
    std::vector<std::uint8_t> complete_definition;
    for (const auto* source : {"known-only", "binary-last"}) {
        const auto card = root / source;
        fs::create_directories(card / "Fonts"); fs::copy_file(font, card / "Fonts/LXGWWenKai-Regular.ttf");
        write(card / "Books/sample.txt", "sample\n");
        fs::create_directories(card / "Dictionaries/Sample");
        for (const auto* suffix : {".ifo", ".idx", ".dict.dz"})
            fs::copy_file(dict / (std::string(source) + suffix), card / "Dictionaries/Sample" / (std::string("main") + suffix));
        write(card / "PicoPage/settings.toml", "schema_version=1\n[dictionary]\nenabled=['Sample/main.ifo']\n");
        ReaderApp reader(card.string(), "dictionary", true, true); Driver d{reader};
        assert(reader.initialize()); d.present(); opened(reader, "Books/sample.txt"); d.present();
        int x = -1, y = -1;
        for (int row = 64; row < 150 && x < 0; ++row)
            for (int col = 36; col < 640; ++col) if (reader.can_long_press_at(col, row)) { x = col; y = row; break; }
        assert(x >= 0); d.hold(x, y); d.present(); d.complete_content();
        assert(reader.view() == View::Dictionary);
        if (std::string_view(source) == "known-only") complete_definition = reader.canvas().pixels();
        else {
            assert(reader.canvas().pixels() != complete_definition);
            shot("dictionary-partial-fields", reader);
        }
    }
    // A higher-priority single-character dictionary cannot hide a longer Chinese match.
    std::vector<std::uint8_t> chinese_result;
    for (bool short_first : {false, true}) {
        const auto card = root / (short_first ? "ChineseBoth" : "ChineseLong");
        fs::create_directories(card / "Fonts"); fs::copy_file(font, card / "Fonts/LXGWWenKai-Regular.ttf");
        write(card / "Books/chinese.txt", "中文词典\n");
        for (const auto* name : {"han-short", "han-long"}) {
            fs::create_directories(card / "Dictionaries" / name);
            for (const auto* suffix : {".ifo", ".idx", ".dict.dz"})
                fs::copy_file(dict / (std::string(name) + suffix), card / "Dictionaries" / name / (std::string("main") + suffix));
        }
        write(card / "PicoPage/settings.toml", std::string("schema_version=1\n[dictionary]\nenabled=[") +
            (short_first ? "'han-short/main.ifo'," : "") + "'han-long/main.ifo']\n");
        ReaderApp reader(card.string(), "dictionary", true, true); Driver d{reader};
        assert(reader.initialize()); d.present(); opened(reader, "Books/chinese.txt"); d.present();
        int x = -1, y = -1;
        for (int row = 64; row < 150 && x < 0; ++row)
            for (int col = 36; col < 640; ++col) if (reader.can_long_press_at(col, row)) { x = col; y = row; break; }
        assert(x >= 0); d.hold(x, y); d.present(); d.complete_content();
        assert(reader.view() == View::Dictionary);
        if (!short_first) chinese_result = reader.canvas().pixels();
        else {
            const auto& actual = reader.canvas().pixels();
            for (int y = 0; y < 1216; ++y) {
                for (int x = 0; x < 342; ++x) {
                    // Previous-source availability and the ordinal depend on the enabled list.
                    if ((y >= 676 && y < 716 && x >= 148 && x < 168) ||
                        (y >= 682 && y < 716 && x >= 184 && x < 232)) continue;
                    assert(actual[y * 342 + x] == chinese_result[y * 342 + x]);
                }
            }
        }
    }
}

int main(int argc, char** argv) {
    assert(argc == 3 || argc == 4);
    char temporary[] = "/tmp/pico-app-XXXXXX";
    const char* directory = mkdtemp(temporary); assert(directory);
    const fs::path card(directory), fixtures(argv[1]);
    dictionary_query_failure_test(card / "DictionaryQuery", fixtures, argv[2]);
    dictionary_ui_test(card / "Dictionary", fixtures, argv[2]);
    body_image_cache_checks(card / "body-image-cache", fixtures);
    bool tested_origin = false;
    for (bool async : {false, true})
        for (const std::string scenario : {"first-frame", "management", "details", "settings", "invalid-settings", "screen", "reading",
                                            "typography-settings", "sleep-settings", "sleep-settings-custom", "sleep-power", "sleep-library", "sleep-reading", "retry-save", "retry-settings", "offline-settings", "offline-reading", "card-library", "card-settings", "local-settings"}) {
            if (scenario.rfind("retry-", 0) == 0 && !async) continue;
            if (argc == 4 && scenario != argv[3]) continue;
            tested_origin = true;
            return_origin_test(card / (scenario + (async ? "Async" : "Sync")), fixtures, async, scenario);
        }
    assert(tested_origin);
    if (argc == 4) { fs::remove_all(card); return 0; }
    update_test(card / "Update");
    lock_screen_test(card / "CustomLocks", fixtures);
    assert(reading_footer::percent(0) == "0.00%");
    assert(reading_footer::percent(.2697, false) == "27%");
    assert(reading_footer::percent(.2649, false) == "26%");
    assert(reading_footer::percent(.265, false) == "27%");
    assert(reading_footer::percent(0, false) == "0%");
    assert(reading_footer::percent(1, false) == "100%");
    assert(reading_footer::percent(-1, false) == "0%" && reading_footer::percent(2, false) == "100%");
    assert(reading_footer::percent(std::numeric_limits<double>::quiet_NaN(), false) == "—");
    assert(reading_footer::percent(.05) == "5.00%");
    assert(reading_footer::percent(.2137) == "21.37%");
    assert(reading_footer::percent(1.0 / 3) == "33.33%");
    assert(reading_footer::percent(2.0 / 3) == "66.67%");
    assert(reading_footer::percent(1) == "100.00%");
    assert(reading_footer::percent(-1) == "0.00%" && reading_footer::percent(2) == "100.00%");
    assert(reading_footer::percent(std::numeric_limits<double>::quiet_NaN()) == "—");
    assert(reading_footer::clock(0, 480) == "—");
    assert(reading_footer::clock(provision::clock_floor - 1, 480) == "—");
    assert(reading_footer::clock(provision::clock_floor, 480) == "08:00");
    assert(reading_footer::clock(provision::clock_floor + 86340, 480) == "07:59");
    assert(reading_footer::clock(provision::clock_floor, -300) == "19:00");
    footer_test(card / "FooterSync", fixtures, false);
    footer_test(card / "FooterAsync", fixtures, true);
    for (bool async : {false, true}) {
        const auto suffix = async ? "Async" : "Sync";
        footer_test(card / ("FooterEmpty" + std::string(suffix)), fixtures, async, "empty-label.epub", "第 1 章");
        footer_test(card / ("FooterWhitespace" + std::string(suffix)), fixtures, async, "whitespace-label.epub", "第 1 章");
        footer_test(card / ("FooterLong" + std::string(suffix)), fixtures, async, "long-label.epub",
                    std::string("甲甲甲甲甲甲甲甲甲甲甲甲甲甲甲甲甲甲甲甲甲甲甲甲甲甲甲甲甲甲甲甲甲甲甲甲甲甲甲甲甲甲…"));
        footer_test(card / ("FooterNonlinear" + std::string(suffix)), fixtures, async, "nonlinear-footer.epub");
    }
    assert(reading_footer::chapter_label({}, 2) == "第 3 章");
    assert(reading_footer::chapter_label(std::string(32768, 'x'), 0) == std::string(128, 'x') + "…");
    std::string wide_label;
    for (int i = 0; i < 100; ++i) wide_label += "甲";
    const auto bounded = reading_footer::chapter_label(wide_label, 0);
    assert(bounded.size() == 129 && graphics::decode_utf8(bounded).size() == 43);

    layout_review_test(card / "LayoutSync", fixtures, argv[2], false);
    layout_review_test(card / "LayoutAsync", fixtures, argv[2], true);
    content_font_test(card / "ContentFontSync", fixtures, argv[2], false);
    content_font_test(card / "ContentFontAsync", fixtures, argv[2], true);
    font_picker_test(card / "FontPickerSync", fixtures, argv[2], false);
    font_picker_test(card / "FontPickerAsync", fixtures, argv[2], true);
    font_picker_image_test(card / "FontPickerImage", fixtures, argv[2]);
#ifdef PICO_PAGE_WRAP_LIBRARY_IO_TEST
    library_scan_failure_test(card / "LibraryScanFailure");
#endif
    for (bool async : {false, true}) library_root_state_test(card / (async ? "RootStateAsync" : "RootStateSync"), async);
    for (bool async : {false, true})
        for (bool grid : {false, true})
            library_resume_pagination_test(card / (std::string("ResumePages") + (grid ? "Grid" : "List") +
                                           (async ? "Async" : "Sync")), fixtures, grid, async);
    swipe_test(card / "SwipeList", fixtures, false);
    swipe_test(card / "SwipeGrid", fixtures, true);
    help_test(card / "HelpNoCard", fixtures, false, false);
    help_test(card / "HelpSync", fixtures, false, true);
    help_test(card / "HelpAsync", fixtures, true, true);
    release_review_test(card / "ReleaseReviewSync", fixtures, false);
    release_review_test(card / "ReleaseReviewAsync", fixtures, true);
    network_setup_test(card / "NetworkSetupSync", fixtures, false);
    network_setup_test(card / "NetworkSetupAsync", fixtures, true);
    for (bool async : {false, true}) {
        const auto root = card / (async ? "EffectsAsync" : "EffectsSync");
        fs::create_directories(root / "Books");
        fs::copy_file(fixtures / "valid.epub", root / "Books/book.epub");
        ReaderApp app(root.string(), "effects", async, async); Driver driver{app};
        assert(app.initialize()); driver.present(); opened(app, "Books/book.epub"); driver.present();
        assert(app.page_effect() == PageEffect::Off);
        bool reverse = false;
        unsigned body_frames = 0;
        driver.frame_observer = [&](const Frame& frame) {
            if (frame.purpose == FramePurpose::Body) {
                assert(frame.reverse_page_effect == reverse);
                ++body_frames;
            } else assert(!frame.reverse_page_effect);
        };
        for (bool swipe : {false, true}) {
            for (bool previous : {false, true}) {
                reverse = previous;
                const auto before_frames = body_frames;
                if (swipe) driver.swipe(!previous);
                else driver.key(previous ? Input::K1 : Input::K3);
                driver.present(); driver.complete_content();
                assert(body_frames == before_frames + 1);
                driver.refresh();
            }
        }
        driver.frame_observer = {};
        const auto record = app.saved_record();
        const auto config = root / "PicoPage/settings.toml";
        const auto before = read(config);
        driver.tap(342, 500); driver.present();
        driver.tap(479, 1140); driver.present(); assert(app.view() == View::Refresh);
        driver.tap(490, 1060); driver.present(); assert(app.view() == View::PageEffects);
        const PageEffect effects[] = {PageEffect::Off, PageEffect::Ripple};
        for (unsigned index = 0; index < 2; ++index) {
            driver.tap(300, 260 + index * 136); driver.present();
            assert(app.page_effect() == effects[index]);
            driver.refresh();
            assert(app.view() == View::PageEffects && app.page_effect() == effects[index]);
            assert(app.saved_record() == record && read(config) == before);
        }
        app.set_page_effect(PageEffect::Ripple);
        assert(app.pending_frame() && app.page_effect() == PageEffect::Ripple);
        driver.present(); driver.refresh();
        driver.tap(60, 50); driver.present(); assert(app.view() == View::Refresh);
        ReaderApp restarted(root.string()); assert(restarted.page_effect() == PageEffect::Off);
    }
    fs::create_directories(card / "Books");
    fs::create_directories(card / "PicoPage");
    fs::copy_file(fixtures / "valid.epub", card / "Books/book.epub");
    fs::copy_file(fixtures / "nested-toc.epub", card / "Books/nested.epub");
    fs::copy_file(fixtures / "image-cover.epub", card / "Books/cover.epub");
    fs::copy_file(fixtures / "image-flow.epub", card / "Books/images.epub");
    fs::create_directories(card / "Txt");
    fs::copy_file(fixtures / "sample.txt", card / "Txt/sample.txt");
    {
        const auto home = card / "LibraryHome";
        fs::create_directories(home / "Books/Articles");
        fs::copy_file(fixtures / "valid.epub", home / "Books/book.epub");
        fs::copy_file(fixtures / "nested-toc.epub", home / "Books/Articles/article.epub");
        ReaderApp app(home.string()); Driver driver{app};
        assert(app.initialize()); driver.present();
        driver.tap(100, 230); driver.present();
        driver.tap(60, 50); driver.present();
        driver.tap(100, 355); driver.present();
        assert(app.view() == View::Reading && app.saved_record());
    }
    {
        const auto home = card / "CustomLibraryRoot";
        fs::create_directories(home / "PicoPage");
        fs::create_directories(home / "Manage");
        fs::create_directories(home / "Books");
        const auto screens = [&](const char *root) {
            write(home / "PicoPage/settings.toml", std::string("schema_version = 1\n[library]\nroots = ['/") + root + "']\n");
            ReaderApp app(home.string()); Driver driver{app};
            assert(app.initialize()); driver.present();
            const auto library = app.canvas().pixels();
            driver.tap(600, 50); driver.present();
            driver.tap(300, 760); driver.present();
            assert(app.view() == View::StorageSettings);
            return std::pair{library, app.canvas().pixels()};
        };
        const auto custom = screens("Manage");
        const auto defaults = screens("Books");
        assert(custom.first != defaults.first && custom.second != defaults.second);
    }
    // Cover-coordinate checks below use a fixed alphabetical library order.
    write(card / "PicoPage/settings.toml", "schema_version=1\n[library]\nsort='name'\n");
    const auto progress = card / "Books/book.epub.picopage/progress.json";
    const auto settings = card / "Books/book.epub.picopage/settings.toml";
    ReadingRecord confirmed;
    ReadingRecord txt_saved;
    {
        ReaderApp app(card.string()); Driver driver{app};
        assert(app.initialize()); driver.present();
        opened(app, "Txt/sample.txt"); driver.present();
        assert(app.saved_record() && app.saved_record()->content_revision == "txt-utf8-v1");
        driver.key(Input::K3); driver.complete_content(); driver.present();
        txt_saved = *app.saved_record();
        driver.sync_view();
        assert(!app.take_network_request());
        assert(sleep_frame(app)); driver.present();
        const auto progress = FileStore(card.string()).load_progress("Txt/sample.txt", txt_saved.book_id, txt_saved.content_revision);
        assert(progress.record && progress.record->anchor == txt_saved.anchor);
    }
    {
        ReaderApp app(card.string()); Driver driver{app};
        assert(app.initialize()); driver.present();
        opened(app, "Txt/sample.txt");
        assert(app.pending_frame()->anchor == txt_saved.anchor);
        driver.present();
    }
    for (const auto encoding : {TxtEncoding::Gb18030, TxtEncoding::Utf16Le, TxtEncoding::Utf16Be})
    for (const bool async : {false, true}) {
        const std::string name = encoding == TxtEncoding::Gb18030 ? "gb18030" :
            encoding == TxtEncoding::Utf16Le ? "utf-16-le" : "utf-16-be";
        const auto home = card / (name + (async ? "-async" : "-sync"));
        fs::create_directories(home / "Books");
        const std::string path = "Books/book.txt";
        fs::copy_file(fixtures / (name + ".txt"), home / path);
        const auto original = read(home / path);
        EpubDocument source; Chapter part; std::string detail;
        assert(source.open((home / path).string(), detail));
        assert(source.metadata().txt_encoding == encoding && source.metadata().txt_offsets.front() == 0);
        assert(source.metadata().txt_offsets.back() == original.size());
        assert(source.load_chapter(0, part, detail, ChapterLoad::Reading, 1));
        Anchor anchor;
        assert(part.anchor_at(2, anchor) && anchor.node == "part:1" && anchor.offset == 2);
        const std::string revision = encoding == TxtEncoding::Gb18030 ? "txt-gb18030-v1" :
            encoding == TxtEncoding::Utf16Le ? "txt-utf16le-v1" : "txt-utf16be-v1";
        const ReadingRecord seed{source.metadata().book_id, revision, "fixture", 1, anchor};
        assert(FileStore(home.string()).save_progress(path, seed).status == StoreStatus::Ok);
        ReaderApp app(home.string(), "txt-encoding", true, async); Driver driver{app};
        assert(app.initialize()); driver.present(); opened(app, path); driver.present();
        assert(app.saved_record()->anchor == anchor && app.saved_record()->content_revision == revision);
        driver.key(Input::K1); driver.present(); driver.complete_content();
        const auto saved = *app.saved_record();
        assert(saved.anchor != anchor && saved.content_revision == revision);
        driver.tap(342, 500); driver.present(); driver.tap(60, 50); driver.present(); driver.complete_content();
        assert(app.view() == View::Library);
        const auto hashes = identity_hash_count();
        opened(app, path); driver.present();
        assert(app.saved_record()->anchor == saved.anchor && app.saved_record()->content_revision == revision);
        assert(identity_hash_count() == hashes && read(home / path) == original);
        if (async) {
            ContentJob::Request request;
            request.kind = ContentJob::Kind::Chapter; request.root = home.string(); request.path = (home / path).string();
            request.identity = source.metadata(); request.chapter_resource = source.spine()[0].resource;
            request.part = 1;
            ContentJob::delay_starts_for_test(request.kind, std::chrono::milliseconds(100));
            ContentJob cancelled(request); assert(cancelled.start()); cancelled.cancel(); cancelled.wait();
            ContentJob::delay_starts_for_test(request.kind, std::chrono::milliseconds{});
            assert(cancelled.result.chapter.text.empty() && cancelled.result.error == kChapterCancelled);
            assert(FileStore(home.string()).load_progress(path, saved.book_id, revision).record->anchor == saved.anchor);
        }
    }
    {
        ReaderApp app(card.string()); Driver driver{app};
        assert(app.initialize()); driver.present();
        // The card view shows the same cached thumbnail at full size.
        driver.tap(610, 384); driver.present();
        assert(app.canvas().pixel(300, 572) == 0 && app.canvas().pixel(270, 622) == 5);
        driver.refresh();
        const auto original_cover = read(card / "Books/cover.epub");
        write(card / "Books/cover.epub", "broken");
        driver.tap(520, 384); driver.present(); driver.tap(610, 384); driver.present();
        assert(app.canvas().pixel(300, 572) == 15);
        write(card / "Books/cover.epub", original_cover);
        driver.tap(520, 384); driver.present(); driver.tap(610, 384); driver.present();
        assert(app.canvas().pixel(300, 572) == 0 && app.canvas().pixel(270, 622) == 5);
        driver.tap(520, 384); driver.present();
        assert(app.view() == View::Library);
        opened(app, "Books/cover.epub");
        assert(app.canvas().pixel(100, 400) == 0 && app.canvas().pixel(100, 800) == 5);
        driver.present(); const auto cover = *app.saved_record();
        // The text chapter after the cover renders with the built-in subset; it is not saved until presented.
        driver.key(Input::K3);
        assert(app.view() == View::Reading && app.pending_frame() && *app.saved_record() == cover);
    }
    {
        ReaderApp app(card.string()); Driver driver{app};
        assert(app.initialize()); driver.present(); opened(app, "Books/cover.epub"); driver.present();
        driver.tap(342, 500); driver.present(); driver.tap(60, 50); driver.present();
        assert(app.view() == View::Library && app.canvas().pixel(47, 183) == 1);
        assert(app.canvas().pixel(60, 200) < 15);
    }
    {
        // A cached thumbnail survives a restart; an entry that fails validation is rebuilt from the EPUB.
        constexpr std::size_t thumbnail_bytes = 92 * 250;
        fs::path cached;
        for (const auto& item : fs::directory_iterator(card / "PicoPage/cache/covers")) {
            const auto bytes = read(item.path());
            if (bytes.find("\nBooks/cover.epub\n") != std::string::npos && bytes.size() > thumbnail_bytes) cached = item.path();
        }
        assert(!cached.empty());
        auto bytes = read(cached);
        std::fill(bytes.end() - thumbnail_bytes, bytes.end(), '\xff');
        write(cached, bytes);
        const auto grid_pixel = [&](int x, int y) {
            ReaderApp app(card.string()); Driver driver{app};
            assert(app.initialize()); driver.present();
            driver.tap(610, 384); driver.present();
            return app.canvas().pixel(x, y);
        };
        // A white thumbnail counts as no cover: the card shows the title placeholder with its rule.
        assert(grid_pixel(300, 572) == 15 && grid_pixel(270, 694) == 1);
        write(cached, "stale");
        assert(grid_pixel(300, 572) == 0 && grid_pixel(270, 622) == 5);
        assert(read(cached).size() > thumbnail_bytes);
        // A previous decoder's negative cache must not hide a newly supported cover.
        auto obsolete = read(cached);
        assert(obsolete.find("PPCOVER3\n") == 0);
        obsolete[7] = '2';
        obsolete.resize(obsolete.size() - thumbnail_bytes);
        write(cached, obsolete);
        const auto decodes = graphics::image_decode_count();
        assert(grid_pixel(300, 572) == 0 && grid_pixel(270, 622) == 5);
        assert(graphics::image_decode_count() == decodes + 1);
        assert(read(cached).find("PPCOVER3\n") == 0 && read(cached).size() > thumbnail_bytes);
    }
    for (const auto* fixture : {"gif-image-cover", "image-cover", "valid"}) {
        const auto home = card / (std::string("CoverDecoderUpgrade-") + fixture);
        fs::create_directories(home / "Books");
        fs::copy_file(fixtures / (std::string(fixture) + ".epub"), home / "Books/book.epub");
        write(home / "PicoPage/settings.toml", "schema_version=1\n[library]\nview='grid'\n");
        {
            ReaderApp app(home.string()); Driver driver{app};
            assert(app.initialize()); driver.present();
        }
        const auto covers = home / "PicoPage/cache/covers";
        const auto cache = fs::directory_iterator(covers)->path();
        constexpr std::size_t thumbnail_bytes = 92 * 250;
        const bool has_cover = std::string_view(fixture) != "valid";
        const auto previous = read(cache);
        const auto header_bytes = previous.size() - (has_cover ? thumbnail_bytes : 0);
        auto obsolete = previous.substr(0, header_bytes);
        obsolete[7] = '2';
        write(cache, obsolete);
        const auto decodes = graphics::image_decode_count();
        if (std::string_view(fixture) == "gif-image-cover") {
            set_chapter_memory_probe([](std::size_t) { return false; });
            {
                ReaderApp app(home.string()); Driver driver{app};
                assert(app.initialize()); driver.present();
            }
            set_chapter_memory_probe(nullptr);
            assert(read(cache) == obsolete && graphics::image_decode_count() == decodes);
        }
        {
            ReaderApp app(home.string()); Driver driver{app};
            assert(app.initialize()); driver.present();
        }
        assert(graphics::image_decode_count() == decodes + (has_cover ? 1 : 0));
        const auto recovered = read(cache);
        assert(recovered.find("PPCOVER3\n") == 0);
        assert(recovered.size() == header_bytes + (has_cover ? thumbnail_bytes : 0));
        {
            ReaderApp app(home.string()); Driver driver{app};
            assert(app.initialize()); driver.present();
        }
        assert(graphics::image_decode_count() == decodes + (has_cover ? 1 : 0));
        assert(read(cache) == recovered);
    }
    {
        ReaderApp app(card.string(), "sleep-library", true, true); Driver driver{app};
        assert(app.initialize()); driver.present(); opened(app, "Books/cover.epub"); driver.present();
        driver.tap(342, 500); driver.present(); driver.tap(60, 50); driver.present();
        assert(app.view() == View::Library);
        const auto saved = *app.saved_record();
        assert(sleep_frame(app)); driver.present();
        assert(app.view() == View::Sleeping);
        app.wake(); driver.present();
        assert(app.view() == View::Library && *app.saved_record() == saved);
    }
    {
        const auto root = card / "CoverWorker";
        fs::create_directories(root / "Books");
        fs::create_directories(root / "PicoPage");
        for (const auto name : {"a.epub", "b.epub"}) fs::copy_file(fixtures / "image-cover.epub", root / "Books" / name);
        for (const bool cancel : {false, true}) {
            fs::remove_all(root / "PicoPage/cache/covers");
            ReaderApp app(root.string(), "cover-worker", true, true); Driver driver{app};
            assert(app.initialize()); driver.present();
            driver.tap(610, 134); driver.present();
            assert(app.background_due() && !app.pending_frame());
            app.poll_background(); driver.refresh();
            if (cancel) { driver.tap(600, 50); driver.present(); assert(app.view() == View::Settings); }
            const auto before = app.canvas().pixels();
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
            unsigned frames = 0;
            while (app.background_due()) {
                app.poll_background();
                if (app.pending_frame()) { ++frames; driver.present(); }
                assert(std::chrono::steady_clock::now() < deadline);
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            if (cancel) assert(frames == 0 && app.canvas().pixels() == before);
            else {
                assert(frames == 1 && app.view() == View::Library);
                assert(app.canvas().pixel(60, 250) == 0 && app.canvas().pixel(270, 400) == 5);
            }
        }
    }
    {
        const auto root = card / "CoverWorker";
        ReaderApp app(root.string(), "details-worker", true, true); Driver driver{app};
        assert(app.initialize()); driver.present();
        driver.tap(610, 134); driver.present();
        driver.hold(100, 300); driver.present();
        assert(app.view() == View::Loading); driver.refresh();
        app.poll_background(); driver.tap(580, 1170); driver.present(); driver.complete_content();
        assert(app.view() == View::Library && !app.saved_record());
        driver.hold(100, 300); driver.present(); driver.complete_content();
        assert(app.view() == View::BookDetails && !app.saved_record());
        assert(app.canvas().pixel(50, 180) == 0);
        const auto hashes = identity_hash_count();
        driver.tap(300, 660); driver.present(); driver.complete_content();
        assert(app.view() == View::Reading && app.saved_record());
        assert(identity_hash_count() == hashes);
    }
    {
        // Without card fonts the built-in subset renders body text and reports characters outside it.
        const auto root = card / "BuiltinFont";
        fs::create_directories(root / "PicoPage");
        fs::create_directories(root / "Books");
        fs::copy_file(fixtures / "valid.epub", root / "Books/book.epub");
        ReaderApp app(root.string()); Driver driver{app};
        assert(app.initialize()); driver.present();
        opened(app, "Books/book.epub");
        assert(app.view() == View::Reading && app.status() == "部分字符无法显示，可在排版中更换字体");
        assert(app.canvas().pixels() != graphics::Canvas().pixels());
        driver.present(); assert(app.saved_record());
        const auto first = *app.saved_record();
        // The notice belongs to the page with the missing characters; the next page shows the position.
        driver.key(Input::K3); driver.present();
        assert(app.view() == View::Reading && app.saved_record()->anchor != first.anchor);
        assert(app.status().empty());
        driver.key(Input::K1); driver.present();
        assert(app.status() == "部分字符无法显示，可在排版中更换字体");
    }
    {
        // Background page counting completes with the built-in font.
        const auto root = card / "BuiltinFont";
        ReaderApp app(root.string(), "builtin-worker", true, true); Driver driver{app};
        assert(app.initialize()); driver.present();
        assert(app.open_book("Books/book.epub")); driver.complete_content();
        // Input cancels a counting job while it opens the book; counting resumes instead of failing.
        for (int i = 0; i < 200; ++i) {
            app.background_step(); app.poll_background(true);
            assert(app.background_state() != std::string("failed"));
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        while (app.background_due()) {
            app.poll_background(); app.background_step();
            if (app.pending_frame()) driver.present();
            assert(std::chrono::steady_clock::now() < deadline);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        assert(app.view() == View::Reading && app.background_state() == std::string("idle"));
        assert(app.book_pages() && app.book_pages()->total > 1);
    }
    fs::create_directories(card / "Fonts");
    fs::copy_file(argv[2], card / "Fonts/LXGWWenKai-Regular.ttf");
    {
        ReaderApp app(card.string()); Driver driver{app};
        assert(app.initialize()); driver.present(); driver.refresh();
        opened(app, "Books/book.epub"); assert(!app.saved_record()); driver.present();
        assert(app.saved_record()); const auto first = *app.saved_record();
        driver.key(Input::K3); assert(app.pending_frame());
        assert(app.pending_frame()->purpose == FramePurpose::Body && !app.pending_frame()->changed_pixels_only);
        const auto failed_anchor = app.pending_frame()->anchor;
        assert(failed_anchor != first.anchor);
        app.complete_frame(false);
        assert(app.saved_record() && *app.saved_record() == first && app.view() == View::Error);
        assert(app.pending_frame() && app.pending_frame()->refresh == RefreshIntent::FullClean);
        assert(!app.pending_frame()->changed_pixels_only);
        driver.present();
        driver.key(Input::K3); assert(!app.pending_frame() && *app.saved_record() == first);
        driver.close(); assert(app.pending_frame() && app.pending_frame()->anchor == first.anchor); driver.present();
        driver.key(Input::K3); assert(app.pending_frame() && app.pending_frame()->anchor == failed_anchor); driver.present();
        confirmed = *app.saved_record(); const auto before_refresh = read(progress);
        driver.key(Input::K2);
        assert(app.pending_frame() && app.pending_frame()->purpose == FramePurpose::ManualRefresh && app.pending_frame()->refresh == RefreshIntent::FullClean);
        driver.present(); assert(*app.saved_record() == confirmed && read(progress) == before_refresh);
        driver.typography();
        const auto applied_preview = app.canvas().pixels();
        driver.tap(500, 1130); assert(!app.pending_frame());
        driver.tap(580, 1020); driver.present();
        assert(app.canvas().pixels() != applied_preview);
        driver.refresh(); assert(!fs::exists(settings));
        driver.close(); assert(app.view() == View::Reading); driver.present();
        assert(!fs::exists(settings) && *app.saved_record() == confirmed);
        driver.tap(342, 500); driver.present(); driver.tap(68, 1150); driver.present();
        assert(app.view() == View::Contents); driver.refresh();
        driver.tap(256, 400); driver.present(); assert(app.view() == View::Typography);
        driver.close(); driver.present();
        driver.tap(342, 500); driver.present(); driver.tap(60, 50);
        assert(app.pending_frame() && app.pending_frame()->refresh == RefreshIntent::FullClean);
        driver.present();
        assert(app.view() == View::Library); driver.refresh();
        assert(fs::exists(card / "PicoPage/cache/positions") && !fs::is_empty(card / "PicoPage/cache/positions"));
        driver.tap(610, 384); driver.present(); driver.refresh();
        driver.tap(520, 384); driver.present();
        driver.tap(390, 384); driver.present();
        assert(*app.saved_record() == confirmed);
    }
    {
        ReaderApp app(card.string()); Driver driver{app};
        assert(app.initialize()); driver.present(); opened(app, "Books/book.epub");
        assert(app.pending_frame()->anchor == confirmed.anchor);
        app.complete_frame(false); driver.present();
        assert(*app.saved_record() == confirmed);
        driver.close();
        assert(app.pending_frame() && app.pending_frame()->purpose == FramePurpose::Body && app.pending_frame()->anchor == confirmed.anchor);
        driver.present();
        driver.typography(); driver.tap(580, 1020); driver.present(); driver.tap(500, 1130);
        assert(app.view() == View::Reading && app.pending_frame() && app.pending_frame()->anchor == confirmed.anchor);
        driver.present();
        assert(FileStore(card.string()).load_settings("Books/book.epub").settings.font_size_px == 40);
        assert(*app.saved_record() == confirmed);
    }
    const auto valid_progress = read(progress);
    for (const auto& invalid : {std::string("{broken"), std::string("{\"schema_version\":999}")}) {
        write(progress, invalid); fs::remove(progress.string() + ".bak");
        ReaderApp app(card.string()); Driver driver{app};
        assert(app.initialize()); driver.present();
        assert(!app.open_book("Books/book.epub") && app.view() == View::Error);
        driver.present(); driver.close(); driver.present();
        assert(read(progress) == invalid);
    }
    write(progress, valid_progress);
    {
        fs::copy_file(fixtures / "missing-chinese-resource.epub", card / "Books/missing-resource.epub");
        ReaderApp app(card.string()); Driver driver{app};
        assert(app.initialize()); driver.present();
        assert(!app.open_book("Books/missing-resource.epub") && app.view() == View::Error);
        assert(app.status().find("无法解析文件内容") != std::string::npos);
        assert(app.status().find("Missing EPUB resource") == std::string::npos);
    }
    {
        const auto root = card / "SettingsWriteFailure";
        const auto config = root / "PicoPage/settings.toml";
        write(config, "schema_version=1\n");
        ReaderApp app(root.string()); Driver driver{app};
        assert(app.initialize()); driver.present();
        driver.tap(600, 50); driver.present(); driver.tap(340, 640); driver.present();
        assert(app.view() == View::PowerSettings);
        driver.tap(600, 750); driver.present();
        fs::create_directory(config.string() + ".tmp");
        driver.tap(500, 1120); driver.present();
        assert(app.view() == View::Error && app.status() == "休眠设置未保存。操作失败。");
        assert(read(config) == "schema_version=1\n");
    }
    for (const auto& item : {std::pair{"invalid", "EPUB 格式标识不正确"},
                             std::pair{"read-failure", "EPUB 格式标识读取失败"},
                             std::pair{"missing", "EPUB 缺少格式标识"},
                             std::pair{"oversized", "EPUB 格式标识过长"}}) {
        const auto name = std::string("mimetype-") + item.first + ".epub";
        fs::copy_file(fixtures / name, card / "Books" / name);
        ReaderApp app(card.string()); Driver driver{app};
        assert(app.initialize()); driver.present();
        assert(!app.open_book("Books/" + name) && app.view() == View::Error);
        assert(app.status().find(item.second) != std::string::npos);
    }
    {
        ReaderApp app(card.string()); Driver driver{app};
        assert(app.initialize()); driver.present(); opened(app, "Books/cover.epub"); driver.present();
        assert(app.saved_record() && app.saved_record()->anchor.node == "/html[1]/body[1]/img[1]" && app.saved_record()->anchor.offset == 0);
        const auto saved = *app.saved_record();
        const auto decodes = graphics::image_decode_count();
        const auto body_pixels = app.canvas().pixels();
        driver.tap(342, 500); driver.present(); driver.close(); driver.present();
        assert(app.canvas().pixels() == body_pixels && graphics::image_decode_count() == decodes);
        assert(sleep_frame(app)); driver.present();
        assert(app.view() == View::Sleeping && *app.saved_record() == saved);
        assert(app.canvas().pixel(100, 400) == 0 && app.canvas().pixel(100, 800) == 5);
        const auto sleeping_decodes = graphics::image_decode_count();
        app.wake(); driver.present(); assert(*app.saved_record() == saved);
        assert(graphics::image_decode_count() == sleeping_decodes);
        ReaderApp reopened(card.string()); Driver again{reopened};
        assert(reopened.initialize()); again.present();
        const auto before_open = graphics::image_decode_count();
        opened(reopened, "Books/cover.epub"); again.present();
        assert(graphics::image_decode_count() == before_open);
    }
    {
        write(card / "PicoPage/settings.toml", "schema_version=1\n[library]\nsort='name'\n");
        fs::copy_file(fixtures / "long-chapter.epub", card / "Books/long.epub");
        ReaderApp app(card.string()); Driver driver{app};
        assert(app.initialize()); driver.present();
        // Books sorts book, cover, images, long: the fourth list row opens after a status frame.
        driver.tap(240, 432 + 3 * 126 + 40);
        assert(app.view() == View::Library && app.pending_frame()->purpose == FramePurpose::Overlay && !app.saved_record());
        app.complete_frame(true);
        assert(app.view() == View::Reading && app.pending_frame()->purpose == FramePurpose::Body);
        driver.present();
        driver.tap(342, 500); driver.present();
        driver.tap(68, 1140); assert(app.view() == View::Contents); driver.present();
        driver.tap(300, 522 + 92 + 46); assert(app.view() == View::Reading); driver.present();
        const auto second = *app.saved_record();
        assert(second.anchor.resource.find("chapter2") != std::string::npos);
        // Turning back into a long chapter announces the layout and keeps the confirmed position meanwhile.
        driver.key(Input::K1);
        assert(app.pending_frame()->purpose == FramePurpose::Overlay && *app.saved_record() == second);
        app.complete_frame(true);
        assert(app.pending_frame()->purpose == FramePurpose::Body);
        driver.present();
        const auto last = *app.saved_record();
        assert(last.anchor.resource.find("chapter1") != std::string::npos);
        // The page starts found by that layout serve the next backward turn directly.
        driver.key(Input::K1);
        assert(app.pending_frame()->purpose == FramePurpose::Body);
        driver.present();
        assert(!(app.saved_record()->anchor == last.anchor));
        driver.key(Input::K3); driver.present();
        assert(app.saved_record()->anchor == last.anchor);
        driver.key(Input::K3); driver.present();
        assert(app.saved_record()->anchor == second.anchor);
    }
    {
        // A presented page is committed at idle time, or earlier when its save state is shown or the book is left.
        ReaderApp app(card.string()); Driver driver{app};
        assert(app.initialize()); driver.present(); opened(app, "Books/long.epub"); driver.present();
        const auto start = *app.saved_record();
        driver.key(Input::K3); app.complete_frame(true);
        assert(app.save_due() && *app.saved_record() == start);
        driver.key(Input::K3); app.complete_frame(true);
        assert(app.save_due() && *app.saved_record() == start);
        app.save_progress();
        const auto second = *app.saved_record();
        assert(!app.save_due() && !app.pending_frame() && second.local_revision == start.local_revision + 2);
        driver.key(Input::K3); app.complete_frame(true);
        assert(*app.saved_record() == second);
        driver.tap(342, 500);
        assert(app.view() == View::Tools && *app.saved_record() == second);
        driver.present();
        assert(app.saved_record()->local_revision == second.local_revision + 1);
        driver.close(); driver.present();
        driver.key(Input::K3); app.complete_frame(true);
        const auto unsaved = app.saved_record()->local_revision;
        assert(sleep_frame(app) && app.saved_record()->local_revision == unsaved + 1);
        driver.present();
    }
    {
        // Input made during a refresh continues a page turn and is otherwise dropped.
        ReaderApp app(card.string()); Driver driver{app};
        assert(app.initialize()); driver.present(); opened(app, "Books/long.epub"); driver.present();
        const auto first = app.canvas().pixels();
        driver.queued(Input::K3);
        driver.queued(Input::TouchCenter, true, 600, 600);
        assert(!app.pending_frame() && app.canvas().pixels() == first);
        driver.key(Input::K3); driver.present();
        const auto second = app.canvas().pixels();
        driver.queued(Input::TouchCenter, true, 342, 600);
        driver.queued(Input::K2);
        driver.queued(Input::TouchCenter, true, 600, 1180);
        assert(!app.pending_frame() && app.view() == View::Reading);
        driver.queued(Input::TouchCenter, true, 600, 600);
        assert(app.pending_frame() && app.pending_frame()->purpose == FramePurpose::Body);
        driver.present();
        assert(app.canvas().pixels() != second);
        driver.queued(Input::K1); driver.present();
        assert(app.canvas().pixels() == second);
        driver.tap(342, 500); driver.present();
        driver.queued(Input::TouchCenter, true, 68, 1140);
        driver.queued(Input::K3);
        assert(!app.pending_frame() && app.view() == View::Tools);
    }
    {
        EpubDocument source;
        Chapter content;
        graphics::Font font;
        graphics::LayoutSpec spec;
        std::string error;
        assert(font.open(argv[2]));
        assert(source.open((fixtures / "valid.epub").string(), error));
        assert(source.load_chapter(0, content, error));
        auto heading = plan_content(font, content, 0, spec, true);
        assert(heading.error.empty() && heading.glyphs.size() > 10);
        assert(heading.glyphs.front().bold && heading.glyphs.front().font_px > spec.font_px);
        assert(heading.glyphs.front().x == spec.left);
        assert(std::any_of(heading.glyphs.begin(), heading.glyphs.end(),
                           [&spec](const auto& glyph) { return !glyph.bold && glyph.font_px == spec.font_px; }));
        assert(source.open((fixtures / "image-flow.epub").string(), error));
        assert(source.load_chapter(0, content, error));
        auto first = plan_content(font, content, 0, spec, true);
        assert(first.error.empty() && first.images.size() == 1 && !first.glyphs.empty());
        assert(first.glyphs.front().index < first.images.front().begin);
        assert(first.glyphs.back().index > first.images.front().begin);
        assert(plan_content(font, content, 0, spec, false).end == first.end);
        auto second = plan_content(font, content, first.end, spec, true);
        assert(second.error.empty() && second.images.size() == 1 && !second.glyphs.empty());
        assert(second.glyphs.back().index > second.images.front().begin);
        assert(plan_content(font, content, first.end, spec, false).end == second.end);
    }
    {
        const auto root = card / "ProgressiveImages";
        fs::create_directories(root / "Books");
        write(root / "PicoPage/settings.toml", "schema_version = 1\n[reader]\nmargin_x_px = 37\n");
        fs::copy_file(fixtures / "image-flow.epub", root / "Books/images.epub");
        fs::copy_file(fixtures / "valid.epub", root / "Books/text.epub");
        ReaderApp app(root.string(), "progressive", true, true); Driver driver{app};
        assert(app.initialize()); driver.present();
#ifdef CONFIG_PICO_PAGE_TRACE_EVENTS
        diagnostics::start();
#endif
        const auto decodes = graphics::image_decode_count();
        opened(app, "Books/images.epub");
        assert(app.view() == View::Reading && app.content_busy());
        assert(app.pending_frame()->purpose == FramePurpose::Body && !app.pending_frame()->region);
        const auto first_anchor = app.pending_frame()->anchor;
        auto placeholder = app.canvas().pixels();
        app.poll_background();
        assert(app.canvas().pixels() == placeholder && graphics::image_decode_count() == decodes);
        app.complete_frame(true);
        // Pages skipped before the dwell interval must not start an image decoder.
        driver.key(Input::K3); app.complete_frame(true); app.poll_background();
        driver.key(Input::K1);
        assert(app.pending_frame()->anchor == first_anchor);
        app.complete_frame(true); app.poll_background();
        assert(graphics::image_decode_count() == decodes);
        placeholder = app.canvas().pixels();
        // Pending input wins over starting or applying an image job.
        app.poll_background(true);
        assert(!app.pending_frame() && graphics::image_decode_count() == decodes);
        driver.key(Input::K2);
        assert(app.pending_frame()->refresh == RefreshIntent::FullClean && !app.pending_frame()->region);
        app.complete_frame(true);
        app.poll_background();
        assert(graphics::image_decode_count() == decodes);
        std::this_thread::sleep_for(std::chrono::milliseconds(275));
        app.poll_background(); app.poll_background(); // Start without adopting the result.
        const auto decode_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        while (graphics::image_decode_count() == decodes) {
            app.poll_background(true);
            assert(!app.pending_frame() && app.canvas().pixels() == placeholder);
            assert(std::chrono::steady_clock::now() < decode_deadline);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        driver.key(Input::K3);
        assert(app.view() == View::Reading && app.pending_frame()->purpose == FramePurpose::Body);
        assert(app.pending_frame()->anchor != first_anchor);
        const auto second_anchor = app.pending_frame()->anchor;
        const auto second_placeholder = app.canvas().pixels();
        app.poll_background();
        assert(app.canvas().pixels() == second_placeholder);
        app.complete_frame(true);
        const auto save_visible = [&] {
            app.save_progress();
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
            while (app.save_busy()) {
                app.poll_background(true);
                assert(std::chrono::steady_clock::now() < deadline);
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        };
        save_visible();
        const auto second_record = app.saved_record();
        assert(second_record && second_record->anchor == second_anchor);
        const auto wait_patch = [&] {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
            while (!app.pending_frame()) {
                app.poll_background();
                assert(std::chrono::steady_clock::now() < deadline);
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            assert(app.pending_frame()->purpose == FramePurpose::Overlay && app.pending_frame()->region);
            assert(app.pending_frame()->changed_pixels_only);
            assert(app.pending_frame()->region->fits(684, 1216));
        };
        // The second page has a missing image: failure is confined to its image box.
        wait_patch();
        const auto region = *app.pending_frame()->region;
#ifdef CONFIG_PICO_PAGE_TRACE_EVENTS
        diagnostics::Event image_event;
        bool failed_patch = false;
        while (diagnostics::pop(image_event)) {
            if (image_event.kind == diagnostics::Kind::ImagePatch && !image_event.begin) {
                assert(image_event.outcome == diagnostics::Outcome::Failure);
                failed_patch = true;
            }
        }
        assert(failed_patch && diagnostics::snapshot().dropped == 0);
        diagnostics::start();
#endif
        assert(region.x == 37);
        assert(app.canvas().pixels() != second_placeholder);
        for (int y = 0; y < 1216; ++y) for (int x = 0; x < 684; ++x) {
            if (x >= region.x && x < region.x + region.width && y >= region.y && y < region.y + region.height) continue;
            const auto packed = second_placeholder[std::size_t(y) * 342 + x / 2];
            assert(app.canvas().pixel(x, y) == ((packed >> ((x & 1) * 4)) & 15));
        }
        app.complete_frame(true);
        assert(app.saved_record() == second_record && !app.save_due() && !app.content_busy());
        driver.key(Input::K1);
        assert(app.pending_frame()->anchor == first_anchor && app.content_busy());
        app.complete_frame(true);
        save_visible();
        const auto first_record = app.saved_record();
        const auto first_placeholder = app.canvas().pixels();
        wait_patch();
        assert(app.canvas().pixel(120, 200) == 0 && app.canvas().pixel(120, 500) == 5);
        const auto first_region = *app.pending_frame()->region;
#ifdef CONFIG_PICO_PAGE_TRACE_EVENTS
        std::optional<diagnostics::Event> decoded;
        bool confirmed_patch = false;
        while (diagnostics::pop(image_event)) {
            if (image_event.kind == diagnostics::Kind::Content && !image_event.begin &&
                image_event.context.detail == static_cast<unsigned>(ContentJob::Kind::Image))
                decoded = image_event;
            if (image_event.kind == diagnostics::Kind::ImagePatch && !image_event.begin) {
                assert(decoded && decoded->outcome == diagnostics::Outcome::Success);
                assert(image_event.outcome == diagnostics::Outcome::Success && image_event.context.detail > 0);
                assert(image_event.context.session && image_event.context.layout);
                assert(image_event.context.session == decoded->context.session &&
                       image_event.context.layout == decoded->context.layout &&
                       image_event.context.request == decoded->context.request);
                assert(image_event.context.frame == app.pending_frame()->token.request);
                confirmed_patch = true;
            }
        }
        assert(confirmed_patch && diagnostics::snapshot().dropped == 0);
#endif
        for (int y = first_region.y; y < first_region.y + first_region.height; ++y) {
            for (int x : {first_region.x - 1, first_region.x + first_region.width}) {
                const auto packed = first_placeholder[std::size_t(y) * 342 + x / 2];
                assert(app.canvas().pixel(x, y) == ((packed >> ((x & 1) * 4)) & 15));
            }
        }
        const auto complete_pixels = app.canvas().pixels();
        app.complete_frame(true);
        assert(app.saved_record() == first_record && !app.save_due());
        driver.tap(342, 500); driver.present(); driver.close(); driver.present();
        assert(app.canvas().pixels() == complete_pixels && !app.content_busy());
        // Opening the viewer supersedes a pending inline request; closing resumes the body image.
        driver.key(Input::K3); app.complete_frame(true);
        driver.key(Input::K1); app.complete_frame(true);
        app.poll_background();
        driver.hold(120, 500);
        assert(app.view() == View::Loading);
        driver.present(); driver.complete_content();
        assert(app.view() == View::Reading && app.canvas().pixel(400, 95) == 1);
        driver.tap(60, 50); driver.present();
        app.poll_background(); // The next firmware tick resumes the restored placeholder page.
        driver.complete_content();
        assert(app.view() == View::Reading && app.canvas().pixel(120, 200) == 0);
        // Cancellation while a panel is open must not leave a permanent placeholder on return.
        driver.key(Input::K3); app.complete_frame(true);
        driver.tap(342, 500); app.complete_frame(true);
        const auto panel = app.canvas().pixels();
        app.poll_background();
        assert(app.view() == View::Tools && app.canvas().pixels() == panel && !app.pending_frame());
        driver.close(); app.complete_frame(true);
        wait_patch();
        const auto before_failure = app.saved_record();
        app.complete_frame(false);
        assert(app.view() == View::Error && app.saved_record() == before_failure);
        assert(app.pending_frame() && app.pending_frame()->refresh == RefreshIntent::FullClean && !app.pending_frame()->region);
        driver.present();
        // A late image result cannot repaint a different book.
        opened(app, "Books/images.epub"); app.complete_frame(true); app.poll_background();
#ifdef CONFIG_PICO_PAGE_TRACE_EVENTS
        diagnostics::start();
#endif
        opened(app, "Books/text.epub"); driver.present();
        const auto text_pixels = app.canvas().pixels();
        driver.complete_content();
        assert(app.view() == View::Reading && app.canvas().pixels() == text_pixels);
#ifdef CONFIG_PICO_PAGE_TRACE_EVENTS
        while (diagnostics::pop(image_event)) assert(image_event.kind != diagnostics::Kind::ImagePatch);
        assert(diagnostics::snapshot().dropped == 0);
#endif
    }
    {
        const auto root = card / "FirstOpenImage";
        fs::create_directories(root / "Books");
        fs::copy_file(fixtures / "image-flow.epub", root / "Books/images.epub");
        ReaderApp app(root.string(), "first-open-image", true, true); Driver driver{app};
        assert(app.initialize()); driver.present();
        opened(app, "Books/images.epub");
        assert(app.pending_frame()->purpose == FramePurpose::Body);
        // Failure injection makes immediate scheduling observable without waiting on a decoder thread.
        ContentJob::fail_starts_for_test(ContentJob::Kind::Image, 1, ContentJob::StartFailure::InternalMemory);
        app.complete_frame(true);
        app.poll_background(); app.poll_background();
        assert(app.pending_frame() && app.pending_frame()->purpose == FramePurpose::Overlay);
        assert(app.view() == View::Reading && app.pending_frame()->region);
        ContentJob::fail_starts_for_test(ContentJob::Kind::Image, 0, ContentJob::StartFailure::None);
        driver.present();
    }
    ReadingRecord image_record, after_image;
    {
        ReaderApp app(card.string()); Driver driver{app};
        assert(app.initialize()); driver.present(); opened(app, "Books/images.epub"); driver.present();
        image_record = *app.saved_record();
        assert(image_record.anchor.node.find("/p[1]/") != std::string::npos);
        assert(app.canvas().pixel(120, 200) == 0 && app.canvas().pixel(120, 500) == 5);
        const auto image_pixels = app.canvas().pixels();
        driver.key(Input::K2);
        assert(app.pending_frame()->purpose == FramePurpose::ManualRefresh && app.pending_frame()->refresh == RefreshIntent::FullClean);
        driver.present(); assert(*app.saved_record() == image_record && app.canvas().pixels() == image_pixels);
        const auto image_decodes = graphics::image_decode_count();
        driver.hold(120, 500);
        assert(app.view() == View::Loading && *app.saved_record() == image_record);
        driver.present(); driver.complete_content();
        assert(app.view() == View::Reading && app.canvas().pixels() != image_pixels);
        assert(graphics::image_decode_count() > image_decodes && app.canvas().pixel(400, 95) == 1);
        const auto viewer_pixels = app.canvas().pixels();
        assert(app.show_power_menu()); driver.present();
        driver.tap(100, 1100); driver.present();
        assert(app.view() == View::Reading && app.canvas().pixels() == viewer_pixels);
        const auto viewer_decodes = graphics::image_decode_count();
        int gray_x = -1, gray_y = -1;
        for (int y = 104; y < 1192 && gray_x < 0; ++y)
            for (int x = 24; x < 660; ++x)
                if (app.canvas().pixel(x, y) == 5) { gray_x = x; gray_y = y; break; }
        assert(gray_x >= 0);
        for (const int gray : {4, 3, 2}) {
            driver.tap(560, 50);
            assert(app.pending_frame() && app.pending_frame()->refresh == RefreshIntent::FullClean);
            assert(app.canvas().pixel(gray_x, gray_y) == gray &&
                   graphics::image_decode_count() == viewer_decodes && *app.saved_record() == image_record);
            driver.present(); driver.refresh();
        }
        driver.tap(560, 50);
        assert(app.pending_frame() && app.pending_frame()->refresh == RefreshIntent::FullClean);
        assert(app.canvas().pixels() == viewer_pixels &&
               graphics::image_decode_count() == viewer_decodes && *app.saved_record() == image_record);
        driver.present();
        driver.refresh();
        driver.key(Input::K3);
        assert(!app.pending_frame() && *app.saved_record() == image_record);
        driver.tap(100, 50); driver.present();
        assert(app.canvas().pixels() == image_pixels && *app.saved_record() == image_record);
        driver.hold(120, 500); driver.present();
        driver.tap(580, 1170); driver.present();
        assert(app.view() == View::Reading && app.canvas().pixels() == image_pixels);
        driver.typography();
        driver.tap(580, 1020); driver.present();
        assert(*app.saved_record() == image_record);
        driver.close(); driver.present(); assert(*app.saved_record() == image_record);
        driver.key(Input::K3); driver.present(); after_image = *app.saved_record();
        assert(after_image.anchor.node == "/html[1]/body[1]/img[2]");
        assert(app.canvas().pixel(36, 76) == 1);
        driver.key(Input::K3); driver.present();
        assert(app.saved_record()->anchor.resource != after_image.anchor.resource);
        driver.key(Input::K1); driver.present(); assert(app.saved_record()->anchor == after_image.anchor);
    }
    {
        ReaderApp app(card.string()); Driver driver{app};
        assert(app.initialize()); driver.present(); opened(app, "Books/images.epub"); driver.present();
        assert(app.saved_record()->anchor == after_image.anchor);
        driver.key(Input::K1); driver.present();
        assert(app.saved_record()->anchor == image_record.anchor && app.canvas().pixel(120, 200) == 0);
        image_record = *app.saved_record();
    }
    {
        ReaderApp app(card.string()); Driver driver{app};
        assert(app.initialize()); driver.present(); opened(app, "Books/images.epub"); driver.present();
        assert(*app.saved_record() == image_record && app.canvas().pixel(120, 500) == 5);
    }
    const std::string sync_config = "schema_version = 1\ndom_version = 20260812\nendpoint = \"https://example.invalid\"\nusername = \"test-user\"\nuserkey = \"test-only-placeholder\"\ndevice_id = \"app-test\"\n";
    write(card / "PicoPage/network.toml", "schema_version=2\n[[wifi]]\nssid=\"home\"\npassword=\"password\"\n");
    write(card / "PicoPage/sync.toml", sync_config);
    for (const bool async : {false, true}) {
        const auto root = card / (async ? "SyncReturnAsync" : "SyncReturnSync");
        fs::create_directories(root / "Books");
        fs::copy_file(fixtures / "valid.epub", root / "Books/book.epub");
        write(root / "PicoPage/sync.toml", sync_config);
        write(root / "PicoPage/network.toml", "schema_version=2\n[[wifi]]\nssid='home'\npassword='password'\n");
        ReaderApp app(root.string(), "sync-return", true, async); Driver driver{app};
        assert(app.initialize()); driver.present(); opened(app, "Books/book.epub"); driver.present();
        const auto has_return = [&] { return app.canvas().pixel(0, 102) == 1; };
        const auto jump = [&](const std::string& pointer) {
            driver.sync_view();
            const auto request = driver.check_remote();
            app.complete_network(request.id, 200, R"({"progress":")" + pointer + R"(","percentage":0.5})", "");
            driver.present(); driver.complete_content();
            driver.tap(500, 800); driver.present(); driver.complete_content();
            assert(app.view() == View::Reading && has_return());
        };
        const std::string first = "/body[1]/DocFragment[1]/body[1]/h1[1]/text()[1].0";
        const std::string middle = "/body[1]/DocFragment[2]/body[1]/p[2]/text()[1].200";
        jump(middle);
        jump(first);
        const auto boundary = *app.saved_record();
        driver.key(Input::K1);
        assert(!app.pending_frame() && has_return() && *app.saved_record() == boundary);
        driver.refresh();
        driver.tap(100, 50); driver.present(); driver.complete_content();
        assert(!has_return() && app.saved_record()->anchor.resource.find("chapter2") != std::string::npos);
        jump(first);
        if (async) {
            const auto remote = *app.saved_record();
            ContentJob::delay_starts_for_test(ContentJob::Kind::Chapter, std::chrono::milliseconds(5000));
            driver.key(Input::K3); driver.present();
            assert(app.view() == View::Loading && *app.saved_record() == remote);
            ContentJob::delay_starts_for_test(ContentJob::Kind::Chapter, std::chrono::milliseconds{});
            driver.tap(560, 1170); driver.present(); driver.complete_content();
            assert(app.view() == View::Reading && has_return() && app.saved_record()->anchor == remote.anchor);
        }
        driver.key(Input::K3); driver.present(); driver.complete_content();
        assert(!has_return() && app.saved_record()->anchor.resource.find("chapter2") != std::string::npos);
        driver.key(Input::K1); driver.present(); driver.complete_content();
        jump("/body[1]/DocFragment[2]/body[1]/h1[1]/text()[1].0");
        driver.key(Input::K1); driver.present(); driver.complete_content();
        assert(!has_return() && app.saved_record()->anchor.resource.find("chapter1") != std::string::npos);
        for (int control = 0; control < 3; ++control) {
            for (const bool forward : {false, true}) {
                jump(middle);
                const auto remote = *app.saved_record();
                driver.refresh();
                if (!control && forward) {
                    driver.key(Input::K3);
                    assert(app.pending_frame() && app.pending_frame()->purpose == FramePurpose::Body);
                    app.complete_frame(false); driver.present();
                    assert(app.view() == View::Error && *app.saved_record() == remote);
                    driver.close(); driver.present(); driver.complete_content();
                    assert(app.view() == View::Reading && has_return() && app.saved_record()->anchor == remote.anchor);
                }
                if (control == 0) driver.key(forward ? Input::K3 : Input::K1);
                else if (control == 1) driver.tap(forward ? 600 : 60, 500);
                else driver.swipe(forward);
                driver.present(); driver.complete_content();
                assert(app.view() == View::Reading && !has_return() && app.saved_record()->anchor != remote.anchor);
                const auto turned = *app.saved_record();
                driver.tap(342, 50); driver.present(); driver.complete_content();
                assert(app.view() == View::Tools && *app.saved_record() == turned);
                driver.close(); driver.present(); driver.complete_content();
            }
        }
    }
    for (const bool async : {false, true}) {
        const auto root = card / (async ? "WifiLifecycleAsync" : "WifiLifecycleSync");
        fs::create_directories(root / "Books");
        fs::copy_file(fixtures / "valid.epub", root / "Books/book.epub");
        write(root / "PicoPage/sync.toml", sync_config);
        write(root / "PicoPage/network.toml", "schema_version=2\nselected_ssid=\"home\"\n[[wifi]]\nssid=\"home\"\npassword=\"password\"\n[[wifi]]\nssid=\"phone\"\npassword=\"phone-password\"\n");
        ReaderApp app(root.string(), "wifi-lifecycle", true, async); Driver driver{app};
        assert(app.initialize()); driver.present();
        assert(app.wifi_enabled() && !app.take_wifi_request() && !app.take_network_request());
        driver.tap(600, 50); driver.present(); driver.tap(300, 180); driver.present();
        driver.tap(300, 570); driver.present(); assert(app.view() == View::WifiSettings);
        const auto network_before_clock = read(root / "PicoPage/network.toml");
        std::uint64_t cancelled_clock = 0;
        for (const std::string result : {"", "时间未校准，请检查互联网连接后重试。", "cancel", "back"}) {
            driver.tap(580, 970); driver.present();
            auto clock = app.take_wifi_request();
            assert(clock && clock->kind == WifiOperation::Kind::Clock && !clock->targets.automatic);
            assert(clock->targets.networks.size() == 1 && clock->targets.networks[0].ssid == "home");
            if (cancelled_clock) app.complete_wifi(cancelled_clock, {}, "");
            driver.key(Input::K2); driver.present();
            assert(app.view() == View::WifiSettings && !clock->cancelled->load() && !app.take_wifi_request());
            if (result == "back") {
                driver.tap(50, 50); driver.present();
                assert(clock->cancelled->load() && app.view() == View::Settings);
            } else if (result == "cancel") {
                driver.tap(580, 970); driver.present(); assert(clock->cancelled->load());
                driver.tap(580, 970); assert(!app.pending_frame() && !app.take_wifi_request());
            }
            app.complete_wifi(clock->id, {}, result == "back" || result == "cancel" ? "" : result);
            if (result == "back") assert(!app.pending_frame() && app.view() == View::Settings);
            else driver.present();
            assert(clock->cancelled->load());
            assert(read(root / "PicoPage/network.toml") == network_before_clock);
            cancelled_clock = clock->id;
            if (result == "back") {
                driver.tap(300, 180); driver.present(); driver.tap(300, 570); driver.present();
            }
        }
        driver.tap(300, 740); driver.present();
        auto manual = app.take_wifi_request();
        // A pinned network is the only candidate; the worker never substitutes another one.
        assert(manual && manual->kind == WifiOperation::Kind::Connect && !manual->targets.automatic);
        assert(manual->targets.networks.size() == 1 && manual->targets.networks[0].ssid == "home");
        app.show_wifi({1, WifiPhase::Connecting, "home", "", false}); driver.present();
        // A connection carries no card objects; local reading works before it finishes.
        opened(app, "Books/book.epub"); driver.present();
        driver.key(Input::K3); driver.present(); driver.complete_content();
        assert(!manual->cancelled->load());
        app.complete_wifi(manual->id, {}, "");
        app.show_wifi({2, WifiPhase::Connected, "home", "", true});
        assert(!app.pending_frame());
        driver.sync_view(); const auto query = driver.check_remote();
        assert(query.targets.networks.size() == 1 && query.targets.networks[0].ssid == "home" && network::live(query.session));
        const auto busy_pixels = app.canvas().pixels();
        const auto busy_saved = app.saved_record();
        for (const auto phase : {WifiPhase::Connecting, WifiPhase::Clock, WifiPhase::Request}) {
            app.show_wifi({static_cast<std::uint64_t>(phase) + 10, phase, "home", "", true});
            assert(!app.pending_frame() && app.canvas().pixels() == busy_pixels);
            assert(app.saved_record() == busy_saved && network::live(query.session));
        }
        driver.key(Input::K2);
        assert(app.pending_frame() && app.pending_frame()->refresh == RefreshIntent::FullClean);
        driver.present();
        assert(network::live(query.session) && !query.cancelled->load());
        app.complete_network(query.id, 200, "{}", ""); driver.present();
        assert(network::live(query.session) && network::live(manual->cancelled));
        driver.tap(342, 1090); driver.present(); driver.tap(500, 1120); driver.present(); driver.complete_content();
        const auto upload = app.take_network_request();
        assert(upload && upload->request.method == "PUT" && upload->session == query.session);
        app.complete_network(upload->id, 200, "{}", ""); driver.present();
        assert(!network::live(query.session) && network::live(manual->cancelled));
        // An expired decision forces another GET, never a blind PUT.
        driver.close(); driver.present(); driver.complete_content(); driver.sync_view(); const auto expired = driver.check_remote();
        app.complete_network(expired.id, 200, "{}", ""); driver.present();
        network::release(expired.session);
        driver.tap(342, 1090); driver.present();
        auto refreshed = app.take_network_request();
        assert(refreshed && refreshed->request.method == "GET" && refreshed->session != expired.session);
        app.complete_network(refreshed->id, 200, "{}", ""); driver.present();
        driver.close(); driver.present(); driver.complete_content();
        std::string selection_error;
        enum class CancelAt { Request, CoverWait, LockFrame };
        for (const auto at : {CancelAt::Request, CancelAt::CoverWait, CancelAt::LockFrame}) {
            // Only the asynchronous configuration prepares the cover on the content worker.
            if (at == CancelAt::CoverWait && !async) continue;
            app.stop_wifi(true);
            assert(!network::live(manual->cancelled));
            // Cancellation restores the original target even if the selected network changed meanwhile.
            assert(provision::save_selected_network(root.string(), "phone", selection_error));
            if (at == CancelAt::CoverWait) {
                assert(!app.prepare_sleep() && app.sleep_pending());
                // The menu request waits for the cancelled cover worker; the restored connection stays queued behind it.
                assert(app.show_power_menu() && app.view() == View::Reading && !app.take_wifi_request());
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
                while (app.view() != View::PowerMenu) {
                    app.poll_background();
                    assert(!app.take_wifi_request());
                    assert(std::chrono::steady_clock::now() < deadline);
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
                driver.present();
                assert(!app.take_wifi_request());
                driver.tap(100, 1100); driver.present();
                assert(app.view() == View::Reading);
            } else if (at == CancelAt::LockFrame) {
                assert(sleep_frame(app)); driver.present();
                app.wake(false); driver.present(); driver.complete_content();
                assert(app.show_power_menu()); driver.present();
                assert(app.view() == View::PowerMenu && !app.take_wifi_request());
                driver.tap(100, 1100); driver.present();
                assert(app.view() == View::Reading);
            } else app.cancel_sleep();
            auto restored = app.take_wifi_request();
            assert(restored && !restored->targets.automatic && restored->targets.networks.size() == 1 && restored->targets.networks[0].ssid == "home" && restored->cancelled != manual->cancelled);
            assert(network::live(restored->cancelled));
            app.complete_wifi(restored->id, {}, "");
            manual = restored;
            app.cancel_sleep();
            assert(!app.take_wifi_request());
        }
        assert(provision::save_selected_network(root.string(), "home", selection_error));
        app.stop_wifi(true);
        assert(sleep_frame(app)); driver.present();
        assert(!network::live(manual->cancelled) && !network::live(refreshed->session));
        app.wake(); driver.present(); driver.complete_content();
        assert(!app.take_wifi_request() && !app.take_network_request());
        // A closed radio is named in the panel; one explicit control opens it and starts the GET.
        write(root / "PicoPage/settings.toml", "schema_version=1\n[network]\nwifi_enabled=false\n");
        driver.sync_view();
        assert(!app.take_network_request() && !app.wifi_enabled());
        driver.tap(342, 1090); assert(!app.pending_frame() && !app.wifi_enabled());
        driver.tap(342, 990); driver.present();
        auto allowed = app.take_network_request();
        assert(allowed && app.wifi_enabled() && allowed->request.method == "GET");
        app.complete_network(allowed->id, 0, "", "目标网络没有取得 IP 地址。"); driver.present();
        assert(!network::live(allowed->session));
        const auto draining = driver.check_remote();
        driver.tap(342, 1075); driver.present();
        assert(draining.cancelled->load() && !network::live(draining.session));
        driver.tap(342, 1120); driver.present(); driver.complete_content();
        assert(app.view() == View::Reading);
        driver.key(Input::K3); driver.present(); driver.complete_content();
        const auto offline_position = app.saved_record();
        app.complete_network(draining.id, 200, "{}", "");
        assert(!app.pending_frame() && app.saved_record() == offline_position);
        driver.sync_view();
        const auto display_failed = driver.check_remote();
        const auto saved_before_failure = app.saved_record();
        driver.key(Input::K2);
        assert(app.pending_frame());
        app.complete_frame(false); driver.present();
        assert(app.view() == View::Error && app.saved_record() == saved_before_failure);
        assert(display_failed.cancelled->load() && !network::live(display_failed.session));
        assert(app.status() == "屏幕更新失败。返回将恢复已确认位置。");
        app.complete_network(display_failed.id, 200, "{}", "");
        assert(!app.pending_frame());
        assert(app.view() == View::Error && app.saved_record() == saved_before_failure);
        driver.tap(300, 1120); driver.present(); driver.complete_content();
        driver.sync_view();
        // The panel's network row opens the connection mode; a failure notice survives unrelated touches.
        driver.tap(342, 910); driver.present(); assert(app.view() == View::WifiSettings);
        app.show_wifi({20, WifiPhase::Offline, "", "目标网络没有取得 IP 地址。", false}); driver.present();
        const auto failed = app.canvas().pixels();
        driver.tap(680, 600); assert(!app.pending_frame() && app.canvas().pixels() == failed);
        app.show_wifi({2, WifiPhase::Connected, "old", "", true}); assert(!app.pending_frame());
        std::vector<provision::SavedWifi> networks;
        std::optional<provision::SavedWifi> target; std::string error;
        assert(provision::load_networks(root.string(), networks, error));
        const auto phone = std::find_if(networks.begin(), networks.end(), [](const auto& n) { return n.ssid == "phone"; });
        assert(phone != networks.end());
        driver.tap(300, 352 + static_cast<int>(phone - networks.begin()) * 124); driver.present();
        assert(provision::load_selected_network(root.string(), target, error) && target && target->ssid == "phone");
        driver.tap(60, 50); driver.present(); assert(app.view() == View::Sync);
        // A pinned network that is out of range is not replaced; automatic selection is offered for one check.
        auto pinned = driver.check_remote();
        assert(!pinned.targets.automatic && pinned.targets.networks.size() == 1 && pinned.targets.networks[0].ssid == "phone");
        app.complete_network(pinned.id, 0, "", "找不到名为「phone」的网络。", LinkFailure::NotNearby); driver.present();
        assert(app.view() == View::Sync && !network::live(pinned.session));
        driver.tap(342, 1090); driver.present();
        auto once = app.take_network_request();
        assert(once && once->request.method == "GET" && once->targets.automatic && once->targets.networks.size() == 2);
        assert(provision::load_selected_network(root.string(), target, error) && target && target->ssid == "phone");
        app.complete_network(once->id, 0, "", "附近没有已保存的网络。", LinkFailure::NotNearby); driver.present();
        // After an automatic failure the lower control leads to the Wi-Fi page instead.
        driver.tap(342, 1090); driver.present(); assert(app.view() == View::WifiSettings);
        driver.tap(60, 50); driver.present(); assert(app.view() == View::Sync);
        const auto last = driver.check_remote();
        app.stop_wifi(); assert(last.cancelled->load());
    }
    // Each instance models reopening after a restart. Empty records never schedule mapping;
    // transient and sustained task-start failures must preserve the committed progress bytes.
    for (const auto failure : {ContentJob::StartFailure::InternalMemory, ContentJob::StartFailure::TaskCreate}) {
        ReaderApp app(card.string(), "mapping-start", true, true); Driver driver{app};
        assert(app.initialize()); driver.present(); opened(app, "Books/book.epub"); driver.present();
        const auto saved = *app.saved_record();
        const auto progress_path = card / "Books/book.epub.picopage/progress.json";
        const auto progress_bytes = read(progress_path);
        assert(!progress_bytes.empty());
        const std::string remote = R"({"progress":"/body[1]/DocFragment[1]/body[1]/h1[1]/text()[1].0","percentage":0.1})";
        driver.sync_view();
        auto request = driver.check_remote();
        ContentJob::fail_starts_for_test(ContentJob::Kind::MapDownload, 3, failure);
        app.complete_network(request.id, 200, "{}", "");
        assert(app.view() == View::Sync && !app.content_busy()); driver.present();
        // Leave the no-record panel, then exhaust all three mapping starts.
        driver.close(); driver.present(); driver.complete_content(); driver.sync_view();
        request = driver.check_remote(); app.complete_network(request.id, 200, remote, ""); driver.present();
        app.poll_background();
        assert(app.view() == View::Loading && app.content_busy());
        driver.refresh(); driver.complete_content();
        assert(app.view() == View::Sync && !app.take_network_request());
        assert(request.session && request.session->load());
        assert(*app.saved_record() == saved && read(progress_path) == progress_bytes);
        // A fresh explicit check can recover; no PUT is generated by failure or retry.
        ContentJob::fail_starts_for_test(ContentJob::Kind::MapDownload, 2, failure);
        request = driver.check_remote(); app.complete_network(request.id, 200, remote, "");
        driver.present(); driver.complete_content();
        assert(app.view() == View::Sync && !app.take_network_request());
        assert(*app.saved_record() == saved && read(progress_path) == progress_bytes);
        const auto recovered_pixels = app.canvas().pixels();
        assert(request.session && !request.session->load());
        driver.close(); driver.present(); driver.complete_content(); driver.sync_view();
        request = driver.check_remote(); app.complete_network(request.id, 200, remote, "");
        driver.present(); driver.complete_content();
        assert(app.canvas().pixels() == recovered_pixels);
        driver.close(); driver.present(); driver.complete_content(); driver.sync_view();
        request = driver.check_remote();
        ContentJob::fail_starts_for_test(ContentJob::Kind::MapDownload, 3, failure);
        app.complete_network(request.id, 200, remote, ""); driver.present(); app.poll_background();
        driver.tap(342, 1075); driver.present(); driver.complete_content();
        assert(app.view() == View::Sync && !app.take_network_request());
        assert(request.session && request.session->load());
        assert(*app.saved_record() == saved && read(progress_path) == progress_bytes);
        ContentJob::fail_starts_for_test(ContentJob::Kind::MapDownload, 0, failure);

        // Upload mapping fails before any PUT and requires a new check and confirmation.
        request = driver.check_remote(); app.complete_network(request.id, 200, "{}", ""); driver.present();
        driver.tap(342, 1090); driver.present(); driver.tap(500, 1120); driver.present();
        ContentJob::fail_starts_for_test(ContentJob::Kind::MapUpload, 3, failure);
        driver.complete_content();
        assert(app.view() == View::Sync && !app.take_network_request());
        assert(*app.saved_record() == saved && read(progress_path) == progress_bytes);
        request = driver.check_remote(); app.complete_network(request.id, 200, "{}", ""); driver.present();
        driver.tap(342, 1090); driver.present(); driver.tap(500, 1120); driver.present();
        driver.complete_content();
        auto upload = app.take_network_request();
        assert(upload && upload->request.method == "PUT");
        app.complete_network(upload->id, 200, "{}", ""); driver.present();
        assert(*app.saved_record() == saved && read(progress_path) == progress_bytes);
    }
    {
        const auto path = card / "PicoPage/settings.toml";
        const bool existed = fs::exists(path);
        const auto original = read(path);
        const auto accounts = read(card / "PicoPage/sync.toml");
        for (bool asynchronous : {false, true}) {
            write(path, "schema_version=1\n[network]\nwifi_enabled=false # retain\n");
            ReaderApp app(card.string(), "wifi-test", true, asynchronous); Driver driver{app};
            assert(app.initialize()); driver.present();
            assert(!app.wifi_enabled() && !app.prepare_portal() && !app.prepare_portal(true));
            driver.tap(600, 50); driver.present();
            driver.tap(300, 180); driver.present(); assert(app.view() == View::Import);
            driver.tap(300, 720); driver.present(); assert(app.view() == View::WifiSettings);
            driver.refresh();
            driver.tap(300, 300); driver.present(); assert(app.wifi_enabled());
            assert(read(path).find("wifi_enabled=true # retain") != std::string::npos);
            assert(read(card / "PicoPage/sync.toml") == accounts);
            const auto enabled_settings = read(path);
            const auto enabled_screen = app.canvas().pixels();
            // A failed write keeps the permission and replaces the visible success notice.
            assert(fs::create_directory(path.string() + ".tmp"));
            driver.tap(300, 300); driver.present();
            assert(app.view() == View::WifiSettings && app.wifi_enabled());
            assert(read(path) == enabled_settings && app.canvas().pixels() != enabled_screen);
            const auto failed_screen = app.canvas().pixels();
            driver.tap(680, 600);
            assert(!app.pending_frame() && app.canvas().pixels() == failed_screen);
            assert(fs::remove(path.string() + ".tmp"));
            driver.tap(300, 300); driver.present(); assert(!app.wifi_enabled());
            driver.tap(300, 300); driver.present(); assert(app.wifi_enabled());
            driver.close(); driver.present();
            driver.tap(300, 180); driver.present();
            driver.tap(300, 890); assert(app.take_portal_action() == PortalAction::Upload);
            assert(app.prepare_portal()); driver.present();
            const auto admitted = read(path);
            // While a FileImport may own the card, reads use only the admitted in-memory state.
            write(path, "schema_version=1\n[network]\nwifi_enabled='invalid'\n");
            std::string import_error;
            auto in_flight = FileStore(card.string()).begin_import("Books/wifi-inflight.epub", 1, 0, import_error);
            assert(in_flight && app.wifi_enabled() && !app.prepare_portal());
            driver.tap(520, 44); driver.present(); assert(app.view() == View::WifiSettings);
            assert(app.take_portal_action() == PortalAction::None && app.portal_visible());
            driver.tap(60, 50); driver.present(); assert(app.view() == View::Portal);
            driver.tap(520, 44); driver.present();
            driver.tap(300, 930); driver.present();
            assert(app.take_portal_action() == PortalAction::Stop && app.view() == View::Portal);
            driver.refresh();
            assert(read(path).find("'invalid'") != std::string::npos);
            assert(in_flight->cancel(import_error) == StoreStatus::Ok);
            in_flight.reset();
            write(path, admitted);
            PortalStatus old_closed{PortalPhase::Closed, "", "", "", "旧会话错误", "", 0, "", app.portal_generation()};
            app.show_portal(old_closed); driver.present();
            assert(app.view() == View::WifiSettings);
            driver.close(); driver.present();
            driver.tap(300, 180); driver.present(); assert(app.view() == View::Import);
            app.show_portal(old_closed);
            assert(app.view() == View::Import && !app.pending_frame());
            driver.close(); driver.present();
            driver.tap(300, 310); driver.present(); assert(app.view() == View::SyncSettings);
            app.show_portal(old_closed);
            assert(app.view() == View::SyncSettings && !app.pending_frame());
            driver.close(); driver.present();
            driver.tap(300, 180); driver.present();
            driver.tap(300, 890);
            assert(app.take_portal_action() == PortalAction::Upload);
            app.show_portal(old_closed); assert(!app.pending_frame());
            assert(app.prepare_portal()); driver.present();
            assert(app.portal_generation() != old_closed.generation);
            app.show_portal(old_closed);
            old_closed.phase = PortalPhase::Waiting;
            app.show_portal(old_closed);
            assert(app.view() == View::Portal && !app.pending_frame());
            driver.portal({PortalPhase::Closed, "", "", "", "", "", 0, ""}); driver.present();
            assert(app.view() == View::Import);
            driver.tap(300, 570); driver.present();
            driver.tap(300, 300); driver.present(); assert(!app.wifi_enabled());
            write(path, "schema_version=1\n[network]\nwifi_enabled=true\n");
            opened(app, "Books/book.epub"); driver.present();
            driver.sync_view();
            driver.tap(342, 960); driver.present();
            write(path, "schema_version=1\n[network]\nwifi_enabled=false\n");
            assert(!app.take_network_request()); driver.present();
            assert(app.view() == View::Sync && !app.wifi_enabled());
            write(path, "schema_version=1\n[network]\nwifi_enabled=true\n");
            driver.tap(60, 50); driver.present(); driver.complete_content(); driver.sync_view();
            const auto request = driver.check_remote();
            write(path, "schema_version=1\n[network]\nwifi_enabled=false\n");
            app.stop_wifi(); assert(request.cancelled->load()); driver.present();
            const auto saved = app.saved_record();
            assert(app.view() == View::NetworkClosing && !app.prepare_usb_transfer() && !app.prepare_portal());
            const auto config_while_closing = read(path);
            driver.tap(60, 50); driver.tap(520, 44); driver.tap(300, 180);
            assert(!app.pending_frame() && read(path) == config_while_closing);
            driver.refresh();
            app.complete_network(request.id + 1000, 200, "{}", "");
            assert(app.view() == View::NetworkClosing && !app.pending_frame());
            app.complete_network(request.id, 200, "{}", ""); driver.present();
            assert(app.view() == View::Sync && app.saved_record() == saved);
            write(path, "schema_version=1\n[network]\nwifi_enabled='invalid'\n");
            assert(!app.wifi_enabled() && !app.prepare_portal());
            // The settings row must reflect the enforced state, rather than the fallback true.
            ReaderApp summary(card.string()); Driver summary_driver{summary};
            assert(summary.initialize()); summary_driver.present();
            summary_driver.tap(600, 50); summary_driver.present();
            const auto invalid_canvas = summary.canvas();
            write(path, "schema_version=1\n[network]\nwifi_enabled=true\n");
            summary_driver.close(); summary_driver.present();
            summary_driver.tap(600, 50); summary_driver.present();
            bool summary_changed = false;
            for (int y = 190; y < 236; ++y)
                for (int x = 36; x < 590; ++x)
                    summary_changed |= invalid_canvas.pixel(x, y) != summary.canvas().pixel(x, y);
            assert(summary_changed);
        }
        if (existed) write(path, original); else fs::remove(path);
    }
    {
        ReaderApp app(card.string(), "mapping-worker", true, true); Driver driver{app};
        assert(app.initialize()); driver.present(); opened(app, "Books/book.epub"); driver.present();
        const auto saved = *app.saved_record();
        const std::string remote = R"({"progress":"/body[1]/DocFragment[1]/body[1]/h1[1]/text()[1].0","percentage":0.1})";
        driver.sync_view(); auto request = driver.check_remote();
        app.complete_network(request.id, 200, remote, "");
        assert(app.view() == View::Loading && *app.saved_record() == saved);
        driver.present(); driver.refresh(); driver.tap(342, 1075); driver.present();
        driver.complete_content();
        assert(app.view() == View::Sync && !app.take_network_request() && *app.saved_record() == saved);
        request = driver.check_remote(); app.complete_network(request.id, 200, remote, "");
        driver.present(); driver.complete_content();
        assert(app.view() == View::Sync && *app.saved_record() == saved);
        driver.close(); driver.present(); driver.sync_view();
        request = driver.check_remote(); app.complete_network(request.id, 200, "{}", ""); driver.present();
        driver.tap(342, 1090); driver.present(); driver.tap(500, 1120); driver.present();
        assert(app.view() == View::Loading && !app.take_network_request());
        driver.tap(342, 1075); driver.present(); driver.complete_content();
        assert(!app.take_network_request() && *app.saved_record() == saved);
        request = driver.check_remote(); app.complete_network(request.id, 200, "{}", ""); driver.present();
        driver.tap(342, 1090); driver.present(); driver.tap(500, 1120); driver.present();
        driver.complete_content();
        auto upload = app.take_network_request();
        assert(upload && upload->request.method == "PUT" && *app.saved_record() == saved);
        app.complete_network(upload->id, 200, "{}", ""); driver.present();
        assert(*app.saved_record() == saved);
    }
    // Which of two different positions is further comes from where the remote position was mapped,
    // never from the percentage the server stores: each client computes that value its own way.
    for (const bool async : {false, true}) {
        const auto root = card / (async ? "SyncOrderAsync" : "SyncOrderSync");
        fs::create_directories(root / "Books");
        fs::copy_file(fixtures / "valid.epub", root / "Books/book.epub");
        fs::copy_file(fixtures / "multi-part-link.epub", root / "Books/parts.epub");
        fs::copy_file(fixtures / "nonlinear-footer.epub", root / "Books/nonlinear.epub");
        write(root / "PicoPage/sync.toml", sync_config);
        write(root / "PicoPage/network.toml", "schema_version=2\n[[wifi]]\nssid=\"home\"\npassword=\"password\"\n");
        ReaderApp app(root.string(), "sync-order", true, async); Driver driver{app};
        assert(app.initialize()); driver.present();
        // Checks the remote position, returns the order and the panel, then stays on the local page.
        const auto compare = [&](const std::string& pointer, const char* percentage) {
            const auto saved = *app.saved_record();
            driver.sync_view();
            const auto request = driver.check_remote();
            app.complete_network(request.id, 200, R"({"progress":")" + pointer + R"(","percentage":)" + percentage + "}", "");
            driver.present(); driver.complete_content();
            assert(app.view() == View::Sync && std::string(app.sync_state()) == "different");
            const std::pair result{app.sync_order(), app.canvas().pixels()};
            driver.close(); driver.present(); driver.complete_content();
            assert(app.view() == View::Reading && !app.take_network_request() && *app.saved_record() == saved);
            return result;
        };
        const std::string second = "/body[1]/DocFragment[2]/body[1]/";
        opened(app, "Books/book.epub"); driver.present();
        assert(app.saved_record()->anchor.resource.find("chapter1") != std::string::npos);
        // A later chapter is further, whatever percentage the other client stored.
        const auto later = compare(second + "h1[1]/text()[1].0", "0.01");
        assert(later.first == 1);
        assert(compare(second + "h1[1]/text()[1].0", "0.99") == later);
        while (app.saved_record()->anchor.resource.find("chapter2") == std::string::npos) {
            driver.key(Input::K3); driver.present(); driver.complete_content();
        }
        // On the visible page, at another anchor than the saved one: neither side is further.
        assert(compare(second + "p[1]/text()[1].0", "0.99").first == 0);
        driver.key(Input::K3); driver.present(); driver.complete_content();
        // The start of this chapter lies before its second page; the stored 99% does not count.
        const auto earlier = compare(second + "h1[1]/text()[1].0", "0.99");
        assert(earlier.first == -1 && earlier.second != later.second);
        assert(compare(second + "h1[1]/text()[1].0", "0.01") == earlier);
        // Counted pages change how both percentages are computed, not which position is further.
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        while (app.background_due()) {
            app.background_step(); app.poll_background();
            if (app.pending_frame()) driver.present();
            assert(std::chrono::steady_clock::now() < deadline);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        assert(app.book_pages());
        const auto counted = compare(second + "h1[1]/text()[1].0", "0.99");
        assert(counted.first == -1 && compare(second + "h1[1]/text()[1].0", "0.01") == counted);

        // A position in a non-linear item is outside the reading order: no side is further.
        opened(app, "Books/nonlinear.epub"); driver.present();
        assert(compare(second + "h1[1]/text()[1].0", "0.99").first == 0);

        // Within one long chapter the order follows the chapter parts, which hold unequal shares
        // of the book: the first chapter here has three parts and the second a single page.
        const std::string first = "/body[1]/DocFragment[1]/body[1]/";
        opened(app, "Books/parts.epub"); driver.present();
        assert(compare(first + "p[4501]/text()[1].0", "0").first == 1);
        const auto config = FileStore(root.string()).load_settings("Books/parts.epub").settings;
        driver.tap(config.margin_x_px + config.indent_chars * config.font_size_px + config.font_size_px / 2, 80);
        driver.present(); driver.complete_content();
        assert(app.saved_record()->anchor.node.find("/p[4501]/") != std::string::npos);
        driver.key(Input::K3); driver.present(); driver.complete_content();
        assert(compare(first + "p[2]/text()[1].0", "0.99").first == -1);
        assert(compare(first + "p[4501]/text()[1].0", "0.99").first == -1);
        assert(compare(second + "h1[1]/text()[1].0", "0").first == 1);
    }
    // A chapter with a table lies outside the exact mapping profile. Its positions are neither
    // uploaded nor offered as a jump target, and the panel names that cause rather than the
    // fallback for a remote position that the book does not contain.
    for (const bool async : {false, true}) {
        const auto root = card / (async ? "SyncUnsupportedAsync" : "SyncUnsupportedSync");
        fs::create_directories(root / "Books");
        fs::copy_file(fixtures / "table-chapter.epub", root / "Books/table.epub");
        fs::copy_file(fixtures / "image-cover.epub", root / "Books/cover.epub");
        write(root / "PicoPage/sync.toml", sync_config);
        write(root / "PicoPage/network.toml", "schema_version=2\n[[wifi]]\nssid=\"home\"\npassword=\"password\"\n");
        ReaderApp app(root.string(), "sync-unsupported", true, async); Driver driver{app};
        assert(app.initialize()); driver.present(); opened(app, "Books/table.epub"); driver.present();
        driver.complete_content();
        const auto saved = *app.saved_record();
        assert(saved.anchor.resource.find("chapter1") != std::string::npos);
        const auto upload = [&] {
            driver.sync_view();
            const auto request = driver.check_remote();
            app.complete_network(request.id, 200, "{}", ""); driver.present();
            assert(std::string(app.sync_state()) == "no-record");
            const auto empty = app.canvas().pixels();
            driver.tap(342, 1090); driver.present(); driver.tap(500, 1120); driver.present();
            driver.complete_content();
            assert(app.view() == View::Sync);
            return app.canvas().pixels() != empty;
        };
        const auto remote = [&](const std::string& pointer) {
            driver.sync_view();
            const auto request = driver.check_remote();
            app.complete_network(request.id, 200, R"({"progress":")" + pointer + R"(","percentage":0.5})", "");
            driver.present(); driver.complete_content();
            assert(app.view() == View::Sync && std::string(app.sync_state()) == "unmapped");
            const auto pixels = app.canvas().pixels();
            driver.tap(342, 990); driver.present(); driver.complete_content();
            assert(app.view() == View::Reading && !app.take_network_request());
            return pixels;
        };
        // The refused upload sends nothing, keeps the saved position and changes the status text.
        assert(upload() && !app.take_network_request() && *app.saved_record() == saved);
        driver.close(); driver.present(); driver.complete_content();
        {
            // The same refusal while the remote holds another position: the comparison panel
            // stays, nothing is sent, and the panel says why instead of reappearing unchanged.
            driver.sync_view();
            const auto request = driver.check_remote();
            app.complete_network(request.id, 200, R"({"progress":"/body[1]/DocFragment[2]/body[1]/h1[1]/text()[1].0","percentage":0.5})", "");
            driver.present(); driver.complete_content();
            assert(app.view() == View::Sync && std::string(app.sync_state()) == "different" && app.sync_order() == 1);
            const auto choice = app.canvas().pixels();
            driver.tap(342, 1090); driver.present(); driver.tap(500, 1120); driver.present();
            driver.complete_content();
            assert(app.view() == View::Sync && std::string(app.sync_state()) == "different");
            assert(!app.take_network_request() && *app.saved_record() == saved && app.canvas().pixels() != choice);
            driver.close(); driver.present(); driver.complete_content();
            assert(app.view() == View::Reading);
        }
        const auto unsupported = remote("/body[1]/DocFragment[1]/body[1]/p[1]/text()[1].0");
        const auto missing = remote("/body[1]/DocFragment[2]/body[1]/p[77]/text()[1].0");
        assert(unsupported != missing && *app.saved_record() == saved);
        // A position inside the table itself shows the same explanation as one beside it.
        assert(remote("/body[1]/DocFragment[1]/body[1]/table[1]/tr[1]/td[1]/text()[1].0") == unsupported);
        // The next chapter has no table; the same steps there produce the upload.
        while (app.saved_record()->anchor.resource.find("chapter2") == std::string::npos) {
            driver.key(Input::K3); driver.present(); driver.complete_content();
        }
        upload();
        auto sent = app.take_network_request();
        assert(sent && sent->request.method == "PUT");
        app.complete_network(sent->id, 200, "{}", ""); driver.present();

        // A page that begins with an image is uploaded as that image element, and the same
        // element position coming back from the server is recognised as the saved position.
        const std::string image_position = "/body[1]/DocFragment[1]/body[1]/img[1].0";
        opened(app, "Books/cover.epub"); driver.present(); driver.complete_content();
        const auto cover = *app.saved_record();
        assert(cover.anchor.node == "/html[1]/body[1]/img[1]" && cover.anchor.offset == 0);
        upload();
        sent = app.take_network_request();
        assert(sent && sent->request.method == "PUT" && sent->request.body.find(image_position) != std::string::npos);
        app.complete_network(sent->id, 200, "{}", ""); driver.present();
        driver.close(); driver.present(); driver.complete_content();
        driver.sync_view();
        const auto again = driver.check_remote();
        app.complete_network(again.id, 200, R"({"progress":")" + image_position + R"(","percentage":0})", "");
        driver.present(); driver.complete_content();
        assert(app.view() == View::Sync && std::string(app.sync_state()) == "same" && *app.saved_record() == cover);
    }
    {
        ReaderApp app(card.string()); Driver driver{app};
        assert(app.initialize()); driver.present(); opened(app, "Books/book.epub"); driver.present();
        const auto before = *app.saved_record();
        const std::string remote = R"({"progress":"/body[1]/DocFragment[1]/body[1]/h1[1]/text()[1].0","percentage":0.1})";
        driver.sync_view(); const auto cancelled = driver.check_remote();
        driver.refresh(); assert(!cancelled.cancelled->load());
        assert(!app.prepare_usb_transfer());
        assert(cancelled.request.url.find("https://example.invalid/") == 0);
        driver.close(); driver.present();
        assert(app.view() == View::NetworkClosing && cancelled.cancelled->load());
        driver.refresh();
        driver.tap(60, 50); assert(!app.pending_frame());
        app.complete_network(cancelled.id, 200, remote, ""); driver.present();
        assert(app.view() == View::Reading && !app.take_network_request() && *app.saved_record() == before);

        // Cancelling from the panel stays on it; the late response is discarded.
        driver.sync_view(); const auto stopped = driver.check_remote();
        driver.tap(342, 1075);
        assert(app.view() == View::NetworkClosing && stopped.cancelled->load() && app.pending_frame());
        driver.present(); assert(app.status().empty() && !app.take_network_request());
        app.complete_network(stopped.id, 200, remote, ""); driver.present();
        assert(app.view() == View::Sync);
        const auto candidate = driver.check_remote();
        app.complete_network(candidate.id, 200, remote, ""); driver.present();
        auto switched_account = sync_config;
        switched_account.replace(switched_account.find("test-user"), 9, "other-user");
        write(card / "PicoPage/sync.toml", switched_account);
        driver.tap(500, 800);
        assert(app.view() == View::Sync && app.pending_frame() && app.pending_frame()->purpose == FramePurpose::Overlay);
        driver.present(); assert(*app.saved_record() == before && !app.take_network_request());
        driver.close(); driver.present();
        write(card / "PicoPage/sync.toml", sync_config);

        driver.sync_view(); const auto stale = driver.check_remote();
        assert(!app.open_book("Books/cover.epub"));
        driver.close(); driver.present();
        app.complete_network(stale.id, 200, remote, ""); driver.present();
        opened(app, "Books/cover.epub"); driver.present(); const auto cover = *app.saved_record();
        driver.sync_view(); const auto current = driver.check_remote();
        assert(current.id != stale.id);
        app.complete_network(stale.id, 200, remote, "");
        assert(!app.pending_frame() && !app.take_network_request() && *app.saved_record() == cover);
        app.complete_network(current.id, 200, "{}", ""); driver.present();
        assert(*app.saved_record() == cover && !app.take_network_request());
        // The upload control sits below the row of the check control.
        driver.tap(342, 990); assert(app.view() == View::Sync && !app.pending_frame());
        driver.tap(342, 1090); assert(app.view() == View::Confirmation); driver.present();
        driver.refresh(); assert(!app.take_network_request());
        driver.close(); driver.present();
        assert(app.view() == View::Reading && !app.take_network_request() && *app.saved_record() == cover);
        opened(app, "Books/book.epub"); driver.present();
        const auto sync_saved = *app.saved_record();
        driver.sync_view(); const auto no_record = driver.check_remote();
        app.complete_network(no_record.id, 200, "{}", ""); driver.present();
        driver.tap(342, 1090); driver.present(); driver.tap(500, 1120); driver.present();
        auto upload = app.take_network_request(); assert(upload && upload->request.method == "PUT");
        const bool settings_existed = fs::exists(settings);
        const auto original_settings = read(settings);
        write(settings, "schema_version=1\n[network]\nwifi_enabled=false\n");
        app.stop_wifi(); driver.present();
        assert(upload->cancelled->load());
        driver.tap(60, 50); assert(!app.pending_frame() && app.view() == View::NetworkClosing);
        if (settings_existed) write(settings, original_settings); else fs::remove(settings);
        app.complete_network(upload->id, 200, "{}", ""); driver.present();
        assert(app.view() == View::Sync && *app.saved_record() == sync_saved);
        driver.close(); driver.present();
        driver.sync_view(); const auto recheck = driver.check_remote();
        app.complete_network(recheck.id, 401, "{}", ""); driver.present(); driver.refresh();
        assert(!app.take_network_request() && *app.saved_record() == sync_saved);
        // A rejected account leads to the account page; returning from it allows a new check.
        driver.tap(342, 990); driver.present(); assert(app.view() == View::SyncSettings);
        driver.tap(60, 50); driver.present(); assert(app.view() == View::Sync);
        const auto retry = driver.check_remote();
        app.complete_network(retry.id, 200, "{}", ""); driver.present();
        driver.tap(342, 1090); driver.present(); driver.tap(500, 1120); driver.present();
        upload = app.take_network_request(); assert(upload && upload->request.method == "PUT");
        app.complete_network(upload->id, 200, "{}", ""); driver.present(); driver.refresh();
        assert(*app.saved_record() == sync_saved);
    }
    {
        ReaderApp app(card.string()); Driver driver{app};
        assert(app.initialize()); driver.present(); opened(app, "Books/book.epub"); driver.present();
        const auto saved = *app.saved_record();
        const std::string external = "schema_version=1\n[reader]\nfont_size_px=48\nline_height=1.6\nmargin_x_px=44\n[display]\nrefresh_preset='clear'\n";
        write(settings, external);
        driver.typography(); driver.tap(580, 1020); driver.present();
        driver.close(); driver.present();
        assert(read(settings) == external && *app.saved_record() == saved);
        driver.typography(); driver.tap(580, 1020); driver.present(); driver.tap(500, 1130); driver.present();
        auto actual = FileStore(card.string()).load_settings("Books/book.epub").settings;
        assert(actual.font_size_px == 50 && actual.line_height == 1.6 && actual.margin_x_px == 44 && actual.refresh_preset == "clear");
        assert(*app.saved_record() == saved);

        const auto body_before_refresh = app.canvas().pixels();
        write(settings, "schema_version=1\n[reader]\nfont_size_px=54\nline_height=1.55\nmargin_x_px=32\n[display]\nrefresh_preset='balanced'\n");
        driver.tap(342, 500); driver.present(); driver.tap(480, 1140); driver.present();
        assert(app.view() == View::Refresh);
        const auto refresh_file = read(settings);
        driver.tap(342, 930); driver.present();
        assert(app.view() == View::RefreshConfirm && read(settings) == refresh_file);
        driver.refresh();
        driver.tap(150, 1120); driver.present();
        assert(app.view() == View::Refresh && read(settings) == refresh_file);
        driver.tap(342, 930); driver.present();
        driver.tap(500, 1120); driver.present();
        actual = FileStore(card.string()).load_settings("Books/book.epub").settings;
        if (actual.refresh_preset != "fast") std::cerr << "Refresh edit failed: " << app.status() << '\n';
        assert(actual.font_size_px == 54 && actual.line_height == 1.55 && actual.margin_x_px == 32 && actual.refresh_preset == "fast");
        driver.close(); driver.present();
        assert(*app.saved_record() == saved && app.canvas().pixels() == body_before_refresh);

        driver.typography(); driver.tap(588, 910); driver.present();
        driver.tap(580, 1020); driver.present(); driver.tap(500, 1130); driver.present();
        actual = FileStore(card.string()).load_settings("Books/book.epub").settings;
        const auto global = FileStore(card.string()).load_settings().settings;
        assert(actual.font_size_px == global.font_size_px + 2 && actual.line_height == global.line_height && actual.margin_x_px == global.margin_x_px);
        assert(*app.saved_record() == saved);

        const auto applied_pixels = app.canvas().pixels();
        const auto book_overrides = read(settings);
        driver.typography(); driver.tap(580, 1020); driver.present(); driver.tap(180, 1130);
        assert(app.view() == View::Tools && app.pending_frame()->purpose == FramePurpose::Overlay);
        assert(app.status() == "阅读默认已保存，本书排版未变");
        driver.present();
        assert(FileStore(card.string()).load_settings().settings.font_size_px == actual.font_size_px + 2);
        assert(FileStore(card.string()).load_settings("Books/book.epub").settings.font_size_px == actual.font_size_px);
        assert(read(settings) == book_overrides && *app.saved_record() == saved);
        driver.close(); driver.present();
        assert(std::equal(applied_pixels.begin(), applied_pixels.begin() + 342 * 1128,
                          app.canvas().pixels().begin()) && *app.saved_record() == saved);

        const std::string invalid = "schema_version=1\n[reader]\nfont_size_px='invalid'\n";
        write(settings, invalid);
        driver.tap(342, 500); driver.present(); driver.tap(205, 1140);
        assert(app.view() == View::Error); driver.present();
        assert(read(settings) == invalid && *app.saved_record() == saved);
        driver.close(); driver.present();
        driver.tap(342, 500); driver.present(); driver.tap(480, 1140);
        assert(app.view() == View::Error); driver.present();
        assert(read(settings) == invalid && *app.saved_record() == saved);
    }
    {
        write(settings, "schema_version=1\n");
        ReaderApp app(card.string()); Driver driver{app};
        assert(app.initialize()); driver.present();
        opened(app, "Books/book.epub"); driver.present();
        const auto saved = *app.saved_record();
        const auto before = app.canvas().pixels();
        const auto inherited = FileStore(card.string()).load_settings().settings.font_size_px;
        const auto overrides = read(settings);
        driver.typography(); driver.tap(100, 1020); driver.present(); driver.tap(180, 1130);
        assert(app.view() == View::Reading && app.pending_frame()->purpose == FramePurpose::Body);
        assert(app.status() == "阅读默认已保存，当前书已应用");
        assert(*app.saved_record() == saved);
        driver.present();
        assert(FileStore(card.string()).load_settings("Books/book.epub").settings.font_size_px == inherited - 2);
        assert(read(settings) == overrides && *app.saved_record() == saved);
        assert(!std::equal(before.begin(), before.begin() + 342 * 1128, app.canvas().pixels().begin()));
    }
    {
        ReaderApp app(card.string()); Driver driver{app};
        assert(app.initialize()); driver.present(); opened(app, "Books/book.epub"); driver.present();
        const auto saved = *app.saved_record();
        fs::create_directory(progress.string() + ".tmp");
        driver.key(Input::K3); driver.present();
        if (app.pending_frame()) driver.present();
        assert(!app.prepare_usb_transfer() && *app.saved_record() == saved);
        assert(!app.prepare_sleep() && *app.saved_record() == saved);
        fs::remove(progress.string() + ".tmp");
        app.save_progress();
        assert(app.saved_record()->local_revision > saved.local_revision);
    }
    {
        ReaderApp app(card.string()); Driver driver{app};
        assert(app.initialize());
        assert(!app.prepare_usb_transfer());
        driver.present(); opened(app, "Books/book.epub"); driver.present();
        const auto saved = *app.saved_record();
        assert(app.prepare_usb_transfer());
        assert(!app.prepare_usb_transfer());
        const auto detached = card.string() + "-detached";
        fs::rename(card, detached);
        app.show_usb_transfer(true, "");
        assert(app.pending_frame()->refresh == RefreshIntent::Content && !app.pending_frame()->region);
        app.complete_frame(false);
        assert(app.view() == View::UsbTransfer && app.pending_frame()->refresh == RefreshIntent::FullClean);
        driver.present();
        assert(!app.open_book("Books/book.epub"));
        assert(!app.prepare_sleep());
        app.complete_network(123, 200, "{}", "");
        driver.refresh();
        assert(app.take_usb_transfer_action() == UsbTransferAction::None);
        driver.tap(342, 1100); driver.present();
        driver.refresh();
        driver.close(); driver.present();
        assert(app.take_usb_transfer_action() == UsbTransferAction::None);
        driver.tap(342, 1100); driver.present();
        driver.tap(500, 1100);
        assert(app.take_usb_transfer_action() == UsbTransferAction::Exit);
        assert(app.take_usb_transfer_action() == UsbTransferAction::None);
        assert(*app.saved_record() == saved);
        fs::rename(detached, card);
    }
    {
        const auto root = card / "TocTitles";
        fs::create_directories(root / "Books");
        fs::copy_file(fixtures / "toc-titles.epub", root / "Books/book.epub");
        ReaderApp app(root.string()); Driver driver{app};
        assert(app.initialize()); driver.present(); opened(app, "Books/book.epub"); driver.present();
        const auto saved = *app.saved_record();
        driver.tap(342, 500); driver.present(); driver.tap(68, 1150); driver.present();
        graphics::Font font; assert(font.open_fallback() && font.set_pixel_size(28));
        const auto metrics = font.metrics();
        const int line_height = std::max(36, metrics.ascender + metrics.descender);
        const int title_height = metrics.ascender + metrics.descender + line_height;
        const int row_height = std::max(92, title_height + 28);
        graphics::Canvas expected;
        const auto check_title = [&](int top, const std::u32string& first, const std::u32string& second) {
            expected.clear();
            const int baseline = top + (row_height - title_height) / 2 + metrics.ascender;
            for (const auto& line : {std::pair{first, baseline}, std::pair{second, baseline + line_height}}) {
                int x = 36;
                for (const auto scalar : line.first) {
                    assert(font.draw(expected, scalar, x, line.second));
                    x += font.advance(scalar);
                }
            }
            for (int y = top; y < top + row_height - 2; ++y)
                for (int x = 36; x < 492; ++x)
                    if (app.canvas().pixel(x, y) != expected.pixel(x, y)) {
                        std::cerr << "TOC title mismatch at " << x << ',' << y << " top=" << top
                                  << " row=" << row_height << " metrics=" << metrics.ascender << ',' << metrics.descender
                                  << " advances=" << font.advance(U'甲') << ',' << font.advance(U'）') << '\n';
                        assert(false);
                    }
            for (int y = top + row_height - 12; y < top + row_height - 2; ++y)
                for (int x = 36; x < 492; ++x) assert(app.canvas().pixel(x, y) == 15);
        };
        check_title(522 + 92, std::u32string(15, U'甲'), U"甲）？");
        check_title(522 + 92 + row_height, std::u32string(16, U'乙'), std::u32string(15, U'乙') + U"…");
        driver.refresh();
        assert(*app.saved_record() == saved);
        driver.key(Input::K3); driver.present();
        driver.key(Input::K1); driver.present();
        assert(app.view() == View::Contents && *app.saved_record() == saved);
    }
    {
        ReaderApp app(card.string()); Driver driver{app};
        assert(app.initialize()); driver.present(); opened(app, "Books/nested.epub"); driver.present();
        const auto saved = *app.saved_record();
        driver.tap(342, 500); driver.present(); driver.tap(68, 1150); driver.present();
        const auto collapsed = app.canvas().pixels();
        driver.tap(615, 568); driver.present();
        assert(app.view() == View::Contents && app.canvas().pixels() != collapsed && *app.saved_record() == saved);
        driver.refresh();
        driver.tap(615, 568); driver.present();
        assert(app.canvas().pixels() == collapsed && *app.saved_record() == saved);
        driver.tap(615, 568); driver.present();
        driver.tap(200, 660); driver.present();
        assert(app.view() == View::Reading && app.saved_record()->anchor != saved.anchor);
    }
    {
        write(card / "PicoPage/settings.toml", "schema_version=1\n# preserved\n");
        write(settings, "schema_version=1\n");
        ReaderApp app(card.string()); Driver driver{app};
        assert(app.initialize()); driver.present(); opened(app, "Books/book.epub"); driver.present();
        const auto saved = *app.saved_record();
        driver.tap(340, 500); driver.present(); driver.tap(480, 1140); driver.present();
        driver.tap(340, 1170); driver.present();
        assert(app.view() == View::RefreshInterval); driver.refresh();
        driver.tap(600, 480); driver.present(); driver.tap(500, 1120); driver.present();
        assert(FileStore(card.string()).load_settings("Books/book.epub").settings.balanced_full_refresh_pages == 26);
        assert(*app.saved_record() == saved);
        driver.close(); driver.present();
        driver.tap(340, 500); driver.present(); driver.tap(50, 50); driver.present();
        driver.hold(300, 480); driver.present();
        assert(app.view() == View::BookDetails && *app.saved_record() == saved);
        driver.refresh(); driver.close(); driver.present();
        assert(app.view() == View::Library && *app.saved_record() == saved);
        driver.tap(610, 384); driver.present();
        driver.hold(100, 560); driver.present();
        assert(app.view() == View::BookDetails && *app.saved_record() == saved);
        driver.refresh(); driver.close(); driver.present();
        driver.tap(600, 50); driver.present();
        driver.tap(340, 640); driver.present();
        assert(app.view() == View::PowerSettings); driver.refresh();
        const auto global_before = read(card / "PicoPage/settings.toml");
        driver.tap(480, 240); driver.present(); driver.tap(600, 750); driver.present();
        driver.refresh(); driver.tap(150, 1120); driver.present();
        assert(read(card / "PicoPage/settings.toml") == global_before && app.auto_sleep_minutes() == 10);
        driver.tap(340, 640); driver.present(); driver.tap(480, 240); driver.present();
        driver.tap(600, 750); driver.present(); driver.tap(500, 1120); driver.present();
        assert(app.auto_sleep_minutes() == 15 && FileStore(card.string()).load_settings().settings.sleep_screen == "reading");
        assert(sleep_frame(app));
        assert(app.view() == View::Sleeping && app.pending_frame()->refresh == RefreshIntent::FullClean);
        driver.present(); assert(*app.saved_record() == saved);
        driver.key(Input::K3); assert(!app.pending_frame());
        app.wake(); driver.present(); assert(app.view() == View::Settings && *app.saved_record() == saved);
        driver.tap(340, 760); driver.present();
        assert(app.view() == View::StorageSettings); driver.refresh();
        write(card / "PicoPage/cache/layout", "cache");
        const auto book_before = read(card / "Books/book.epub");
        const auto progress_before = read(progress);
        driver.tap(340, 1120); driver.present();
        assert(!fs::exists(card / "PicoPage/cache/layout") && read(progress) == progress_before);
        assert(read(card / "Books/book.epub") == book_before && *app.saved_record() == saved);
        driver.close(); driver.present(); driver.tap(340, 640); driver.present();
        driver.tap(180, 240); driver.present();
        write(card / "PicoPage/settings.toml", global_before + "# external edit\n");
        driver.tap(500, 1120); driver.present();
        assert(app.view() == View::Error && app.auto_sleep_minutes() == 15);
        assert(read(card / "PicoPage/settings.toml") == global_before + "# external edit\n");
    }
    {
        write(card / "PicoPage/settings.toml", "schema_version=1\n[power]\nsleep_screen='reading'\n");
        ReaderApp app(card.string()); Driver driver{app};
        assert(app.initialize()); driver.present(); opened(app, "Books/book.epub"); driver.present();
        const auto saved = *app.saved_record();
        driver.key(Input::K3); assert(app.pending_frame());
        app.complete_frame(false); driver.present();
        assert(app.view() == View::Error && *app.saved_record() == saved);
        assert(sleep_frame(app)); driver.present();
        app.wake(); assert(app.pending_frame()->anchor == saved.anchor); driver.present();
        assert(*app.saved_record() == saved);
    }
    {
        fs::create_directories(card / "Manage");
        fs::copy_file(fixtures / "valid.epub", card / "Manage/only.epub");
        write(card / "PicoPage/settings.toml", "schema_version=1\n[library]\nroots=['Manage']\n");
        ReaderApp app(card.string()); Driver driver{app};
        assert(app.initialize()); driver.present(); opened(app, "Manage/only.epub"); driver.present();
        driver.key(Input::K3); driver.present();
        const auto saved = *app.saved_record();
        const auto sidecar = card / "Manage/only.epub.picopage/progress.json";
        driver.tap(342, 500); driver.present(); driver.tap(60, 50); driver.present();
        driver.hold(300, 220); driver.present();
        assert(app.view() == View::BookDetails);
        driver.tap(342, 918); driver.present();
        assert(app.view() == View::BookManagement); driver.refresh();
        driver.tap(100, 1120); driver.present();
        assert(app.view() == View::BookDetails && *app.saved_record() == saved);
        driver.tap(342, 918); driver.present(); driver.tap(500, 1120); driver.present();
        assert(app.view() == View::BookDetails && !app.saved_record());
        FileStore store(card.string());
        assert(store.load_progress("Manage/only.epub", saved.book_id, saved.content_revision).status == StoreStatus::Cleared);
        const auto cleared = read(sidecar);
        fs::remove(sidecar);
        driver.tap(342, 670); driver.present();
        assert(app.view() == View::Reading && app.saved_record() && app.saved_record()->anchor != saved.anchor);
        assert(read(sidecar.string() + ".bak") == cleared);
        const auto reset_record = read(sidecar);
        driver.tap(342, 500); driver.present(); driver.tap(60, 50); driver.present(); driver.hold(300, 220); driver.present();
        driver.tap(342, 1010); driver.present(); driver.refresh();
        driver.tap(500, 1120); driver.present();
        assert(app.view() == View::Library && !app.saved_record());
        assert(!fs::exists(card / "Manage/only.epub") && read(sidecar) == reset_record);
    }
    {
        // Whole-book page counting runs in idle slices, numbers pages exactly, survives a
        // restart through the cache and restarts when the typography changes.
        fs::create_directories(card / "Count");
        fs::copy_file(fixtures / "long-chapter.epub", card / "Count/long.epub");
        std::uint32_t total = 0;
        std::vector<std::uint8_t> prepared_pixels;
        {
            ReaderApp app(card.string()); Driver driver{app};
            assert(app.initialize()); driver.present(); opened(app, "Count/long.epub"); driver.present();
            assert(app.background_due() && !app.book_pages());
            const auto record = *app.saved_record();
            const auto pixels = app.canvas().pixels();
            app.background_step([] { return true; });
            assert(app.background_due() && !app.book_pages() && !app.pending_frame());
            assert(*app.saved_record() == record && app.canvas().pixels() == pixels);
            app.background_step();
            unsigned checks = 0;
            app.background_step([&] { return ++checks >= 3; });
            assert(checks >= 3 && app.background_due() && !app.pending_frame());
            assert(*app.saved_record() == record && app.canvas().pixels() == pixels);
            unsigned slices = 0;
            while (app.background_due()) { app.background_step(); ++slices; assert(!app.pending_frame()); }
            assert(slices > 1 && *app.saved_record() == record && app.canvas().pixels() == pixels);
            assert(app.book_pages() && app.book_pages()->page == 1 && app.book_pages()->total > 40);
            total = app.book_pages()->total;
            driver.key(Input::K3); driver.present();
            assert(app.book_pages()->page == 2 && app.book_pages()->total == total);
            assert(app.canvas().pixels() != pixels);
            prepared_pixels = app.canvas().pixels();
            driver.key(Input::K1); driver.present();
            assert(app.book_pages()->page == 1);
        }
        {
            ReaderApp app(card.string()); Driver driver{app};
            assert(app.initialize()); driver.present(); opened(app, "Count/long.epub"); driver.present();
            assert(app.background_due() && app.book_pages() && app.book_pages()->total == total);
            assert(std::string(app.background_state()) == "prefetch");
            driver.key(Input::K3); driver.present();
            assert(app.canvas().pixels() == prepared_pixels);
            driver.key(Input::K1); driver.present();
            driver.tap(342, 500); driver.present(); driver.tap(205, 1150); driver.present();
            assert(app.view() == View::Typography);
            driver.tap(588, 1020); driver.present(); driver.tap(500, 1136); driver.present();
            assert(app.view() == View::Reading && app.background_due() && !app.book_pages());
            while (app.background_due()) app.background_step();
            assert(app.book_pages() && app.book_pages()->total > total);
            total = app.book_pages()->total;
        }
        {
            // A cache entry cut short is ignored and the book is counted again.
            const auto entry = *fs::directory_iterator(card / "PicoPage/cache/pages");
            const auto bytes = read(entry.path());
            write(entry.path(), bytes.substr(0, bytes.size() - 2));
            ReaderApp app(card.string()); Driver driver{app};
            assert(app.initialize()); driver.present(); opened(app, "Count/long.epub"); driver.present();
            assert(app.background_due() && !app.book_pages());
            // Inputs release the chapter loaded for counting; the count resumes to the same total.
            for (unsigned slice = 1; app.background_due(); ++slice) {
                app.background_step();
                if (slice % 3 == 0) driver.refresh();
            }
            assert(app.book_pages() && app.book_pages()->total == total);
        }
        {
            // A chapter beyond the parser limits cannot be shown; it has no pages and does not
            // stop the count of the other chapters.
            fs::copy_file(fixtures / "oversized-chapter.epub", card / "Count/oversized.epub");
            ReaderApp app(card.string()); Driver driver{app};
            assert(app.initialize()); driver.present(); opened(app, "Count/oversized.epub"); driver.present();
            while (app.background_due()) app.background_step();
            assert(app.book_pages() && app.book_pages()->page == 1 && app.book_pages()->total >= 1);
            driver.key(Input::K3);
            assert(app.view() == View::Error || app.book_pages()->page <= app.book_pages()->total);
        }
    }
    {
        // Settings sub-pages return to the settings list, K2 keeps them, and the library
        // default view is written to the global settings file without touching other fields.
        ReaderApp app(card.string()); Driver driver{app};
        assert(app.initialize()); driver.present();
        driver.tap(600, 50); driver.present(); assert(app.view() == View::Settings);
        driver.tap(300, 180); driver.present(); assert(app.view() == View::Import);
        driver.tap(60, 50); driver.present(); assert(app.view() == View::Settings);
        driver.tap(300, 310); driver.present(); assert(app.view() == View::SyncSettings); driver.refresh();
        driver.tap(60, 50); driver.present(); assert(app.view() == View::Settings);
        driver.tap(300, 760); driver.present(); assert(app.view() == View::StorageSettings); driver.refresh();
        driver.tap(60, 50); driver.present(); assert(app.view() == View::Settings);
        driver.tap(300, 580); driver.present(); assert(app.view() == View::InputSettings);
        assert(FileStore(card.string()).load_settings().settings.library_view == "list");
        driver.tap(300, 150);
        assert(app.pending_frame() && app.pending_frame()->purpose == FramePurpose::Overlay);
        assert(app.pending_frame()->refresh == RefreshIntent::Content && app.pending_frame()->changed_pixels_only);
        driver.present();
        assert(app.view() == View::InputSettings && app.status().empty());
        assert(FileStore(card.string()).load_settings().settings.library_view == "grid");
        assert(read(card / "PicoPage/settings.toml").find("roots=['Manage']") != std::string::npos);
        // A settings file changed into an invalid one is left alone and the row reports it.
        const auto valid = read(card / "PicoPage/settings.toml");
        write(card / "PicoPage/settings.toml", "schema_version=9\n");
        driver.tap(300, 150); driver.present();
        assert(app.view() == View::Error && read(card / "PicoPage/settings.toml") == "schema_version=9\n");
        write(card / "PicoPage/settings.toml", valid);
    }
    {
        // Turning into a chapter that does not fit in memory: the visible chapter is released
        // and the load repeated; a second failure shows an error and keeps the confirmed page.
        fs::create_directories(card / "Memory");
        fs::copy_file(fixtures / "valid.epub", card / "Memory/book.epub");
        ReaderApp app(card.string()); Driver driver{app};
        assert(app.initialize()); driver.present(); opened(app, "Memory/book.epub"); driver.present();
        const auto start = *app.saved_record();
        const auto pixels = app.canvas().pixels();
        static unsigned checks;
        set_chapter_memory_probe([](std::size_t) { return false; });
        for (unsigned turn = 0; turn < 20 && app.view() == View::Reading; ++turn) { driver.key(Input::K3); driver.present(); }
        assert(app.view() == View::Error && app.status().find("内存不足") != std::string::npos);
        const auto kept = *app.saved_record();
        assert(kept.anchor.resource == start.anchor.resource);
        set_chapter_memory_probe(nullptr);
        driver.close(); driver.present();
        assert(app.view() == View::Reading && *app.saved_record() == kept);
        // The first attempt fails, the attempt after releasing the visible chapter succeeds.
        checks = 0;
        set_chapter_memory_probe([](std::size_t) { return ++checks != 1; });
        driver.key(Input::K3); driver.present();
        set_chapter_memory_probe(nullptr);
        assert(app.view() == View::Reading && app.saved_record()->anchor.resource != start.anchor.resource);
        assert(app.canvas().pixels() != pixels);
        driver.key(Input::K1); driver.present();
        assert(app.view() == View::Reading && app.saved_record()->anchor.resource == start.anchor.resource);
    }
    {
        // A chapter longer than one part: pages turn across part boundaries in both directions
        // with consecutive page numbers, and a position inside a later part is reached from the
        // contents and restored after a restart.
        fs::create_directories(card / "Parts");
        fs::copy_file(fixtures / "multi-part.epub", card / "Parts/long.epub");
        std::uint32_t late_page = 0;
        Anchor late;
        {
            ReaderApp app(card.string()); Driver driver{app};
            assert(app.initialize()); driver.present(); opened(app, "Parts/long.epub"); driver.present();
            while (app.background_due()) app.background_step();
            assert(app.book_pages() && app.book_pages()->page == 1);
            const auto total = app.book_pages()->total;
            std::vector<Anchor> forward{app.saved_record()->anchor};
            for (std::uint32_t page = 2; page <= total; ++page) {
                driver.key(Input::K3); driver.present();
                assert(app.view() == View::Reading && app.book_pages()->page == page);
                forward.push_back(app.saved_record()->anchor);
            }
            assert(total > 200 && forward.back().resource != forward.front().resource);
            for (auto page = forward.size() - 1; page-- > 0;) {
                driver.key(Input::K1); driver.present();
                assert(app.view() == View::Reading && app.saved_record()->anchor == forward[page]);
                assert(app.book_pages()->page == page + 1);
            }
            driver.tap(342, 500); driver.present(); driver.tap(68, 1150); driver.present();
            assert(app.view() == View::Contents);
            driver.tap(300, 660); driver.present();
            assert(app.view() == View::Reading);
            late = app.saved_record()->anchor;
            late_page = app.book_pages()->page;
            assert(late.node.find("/p[4501]/") != std::string::npos && late_page > total / 2);
            // A contents jump may start between standard boundaries and changes body height.
            const auto jumped_pixels = app.canvas().pixels();
            driver.key(Input::K3); driver.present();
            driver.key(Input::K1); driver.present();
            assert(app.saved_record()->anchor == late && app.canvas().pixels() == jumped_pixels);
            driver.refresh();
            assert(app.saved_record()->anchor == late);
        }
        {
            ReaderApp app(card.string()); Driver driver{app};
            assert(app.initialize()); driver.present(); opened(app, "Parts/long.epub"); driver.present();
            assert(app.saved_record()->anchor == late && app.book_pages() && app.book_pages()->page == late_page);
            driver.key(Input::K1); driver.present();
            assert(app.view() == View::Reading && app.book_pages()->page + 1 >= late_page);
            const auto restored = app.saved_record()->anchor;
            ReaderApp async(card.string(), "async-restore", true, true); Driver async_driver{async};
            assert(async.initialize()); async_driver.present(); opened(async, "Parts/long.epub"); async_driver.present();
            assert(async.saved_record()->anchor == restored);
            assert(std::equal(app.canvas().pixels().begin(), app.canvas().pixels().begin() + 342 * 1128,
                              async.canvas().pixels().begin()));
        }
    }
    {
        FileStore store(card.string());
        EpubDocument doc;
        std::string error;
        assert(doc.open((fixtures / "multi-part.epub").string(), error));
        Chapter original, cached;
        assert(load_content(store, doc, 0, original, error, ChapterLoad::Reading, 2));
        const auto cache = card / "PicoPage/cache/content/content-reading-v4" / doc.metadata().book_id / "0/2";
        assert(fs::exists(cache));
        const auto data = read(cache);
        const auto old_cache = card / "PicoPage/cache/content/content-reading-v3" / doc.metadata().book_id / "0/2";
        auto old_data = data.substr(0, data.size() - 32);
        old_data.replace(old_data.find("content-reading-v4"), std::strlen("content-reading-v4"), "content-reading-v3");
        old_data += md5_hex(old_data);
        write(old_cache, old_data); fs::remove(cache);
        assert(!load_content_cache(store, doc.metadata().book_id, doc.spine()[0].resource, 0, 2, cached));
        assert(cached.text.empty() && read(old_cache) == old_data);
        assert(load_content(store, doc, 0, cached, error, ChapterLoad::Reading, 2));
        assert(cached.text == original.text && read(cache) == data);
        const std::string old_pages_header = "PPPAGES3\nepub-native-v1:pagination-v4\n";
        const std::string pages_header = "PPPAGES3\nepub-native-v1:pagination-v5\n";
        PageIndices page_indices(doc.spine().size());
        std::vector<bool> pages_counted(doc.spine().size(), true);
        for (auto& pages : page_indices) pages = {{0}};
        assert(write_page_cache(store, "pages/version-check", old_pages_header, page_indices, pages_counted));
        assert(!read_page_cache(store, "pages/version-check", pages_header, doc.spine()));
        assert(write_page_cache(store, "pages/version-check", pages_header, page_indices, pages_counted));
        assert(read_page_cache(store, "pages/version-check", pages_header, doc.spine()));
        for (std::uint32_t part = 0; part < 2; ++part) {
            assert(fs::exists(cache.parent_path() / std::to_string(part)));
            Chapter parsed, prior;
            assert(doc.load_chapter(0, parsed, error, ChapterLoad::Reading, part));
            assert(load_content(store, doc, 0, prior, error, ChapterLoad::Reading, part));
            assert(parsed.text == prior.text && parsed.part == prior.part && parsed.last_part == prior.last_part);
            for (std::uint32_t offset = 0; offset < parsed.text.size(); ++offset) {
                Anchor a, b;
                assert(parsed.anchor_at(offset, a) == prior.anchor_at(offset, b));
                assert(a == b);
            }
        }
        assert(load_content(store, doc, 0, cached, error, ChapterLoad::Reading, 2));
        assert(cached.text == original.text && cached.runs.size() == original.runs.size());
        for (std::uint32_t n = 0; n < cached.text.size(); ++n) {
            Anchor a, b;
            assert(cached.anchor_at(n, a) == original.anchor_at(n, b) && a == b);
        }
        // A cached part fits below the former fixed 2 MiB allocation request.
        static std::size_t cache_budget;
        cache_budget = std::max<std::size_t>(data.size() + 128, 512U * 1024);
        assert(cache_budget < 2U * 1024 * 1024);
        set_chapter_memory_probe([](std::size_t bytes) { return bytes <= cache_budget; });
        unsigned stops = 0;
        assert(load_content_cache(store, doc.metadata().book_id, doc.spine()[0].resource, 0, 2, cached));
        set_chapter_memory_probe(nullptr);
        assert(cached.text == original.text);
        // Rejecting the cache buffer falls back to the source without losing its anchors.
        static unsigned allocations;
        allocations = 0;
        set_chapter_memory_probe([](std::size_t) { return ++allocations != 1; });
        assert(load_content(store, doc, 0, cached, error, ChapterLoad::Reading, 2));
        set_chapter_memory_probe(nullptr);
        assert(cached.text == original.text);
        fs::remove_all(cache.parent_path());
        ContentCacheEntry deferred;
        assert(load_content(store, doc, 0, cached, error, ChapterLoad::Reading, 2, {}, &deferred));
        assert(cached.text == original.text && !deferred.bytes.empty() && !deferred.from_disk && !fs::exists(cache));
        assert(!save_content_cache(store, deferred, [] { return true; }) && !fs::exists(cache));
        write(card / "CacheBlocked", "not a directory");
        assert(!save_content_cache(FileStore((card / "CacheBlocked").string()), deferred));
        assert(cached.text == original.text && save_content_cache(store, deferred));
        assert(read(cache) == data);
        stops = 0;
        assert(find_content(store, doc, 0, cached, error, ChapterLoad::Reading,
            [](const Chapter& c) { return c.part == 2; }, [&] { ++stops; return false; }, &deferred));
        assert(deferred.from_disk && deferred.bytes == data && cached.text == original.text);
        fs::remove_all(cache.parent_path());
        assert(find_content(store, doc, 0, cached, error, ChapterLoad::Reading,
            [](const Chapter& c) { return c.part == 2; }, {}, &deferred));
        assert(!deferred.bytes.empty() && !fs::exists(cache));
        assert(fs::exists(cache.parent_path() / "0") && fs::exists(cache.parent_path() / "1"));
        assert(save_content_cache(store, deferred));
        assert(load_content(store, doc, 0, cached, error, ChapterLoad::Reading, 2, {}, &deferred));
        assert(deferred.from_disk && deferred.bytes == data && cached.text == original.text);
        write(cache, data.substr(0, data.size() / 2));
        assert(load_content(store, doc, 0, cached, error, ChapterLoad::Reading, 2));
        assert(cached.text == original.text && read(cache) == data);
        assert(!load_content(store, doc, 0, cached, error, ChapterLoad::Reading, 2, [] { return true; }));
        assert(error == kChapterCancelled && cached.text.empty());
        // Hot chapter jobs need neither EPUB metadata nor font setup for a known last page.
        ContentJob::Request hot;
        hot.kind = ContentJob::Kind::Chapter; hot.target = ContentJob::Target::End;
        hot.path = (fixtures / "multi-part.epub").string(); hot.root = card.string(); hot.identity = doc.metadata();
        hot.chapter = 0; hot.part = 2; hot.chapter_resource = doc.spine()[0].resource;
        ContentJob last(hot); assert(last.start()); last.wait();
        assert(last.result.error.empty() && !last.result.history.empty());
        hot.known_start = last.result.offset; hot.known_end = last.result.chapter.text.size();
        hot.font = (card / "missing-font.ttf").string();
        const auto opens = epub_open_count();
        ContentJob known(hot); assert(known.start()); known.wait();
        assert(known.result.error.empty() && known.result.offset == last.result.offset && known.result.history.empty());
        assert(known.result.content_cache.from_disk && known.result.content_cache.bytes == data);
        assert(epub_open_count() == opens && known.result.chapter.text == original.text);
        // A validated disk hit remains available after its cache file disappears.
        hot.content_cache = std::make_shared<ContentCacheEntry>(std::move(known.result.content_cache));
        fs::remove(cache);
        ContentJob disk_retained(hot); assert(disk_retained.start()); disk_retained.wait();
        assert(disk_retained.result.error.empty() && disk_retained.result.retained_cache_hit);
        assert(disk_retained.result.chapter.text == original.text && epub_open_count() == opens);
        hot.content_cache.reset();
        assert(save_content_cache(store, *disk_retained.request.content_cache));
        hot.known_end = *hot.known_end + 1;
        ContentJob stale(hot); assert(stale.start()); stale.wait();
        assert(!stale.result.error.empty());
        hot.target = ContentJob::Target::Start;
        hot.chapter_resource = "wrong-resource";
        ContentJob mismatch(hot); assert(mismatch.start()); mismatch.wait();
        assert(mismatch.result.error.empty() && mismatch.result.chapter.text == original.text && epub_open_count() == opens + 1);
        hot.chapter_resource = doc.spine()[0].resource;
        // Unwritten neighboring content survives a quick return; a damaged retained payload falls back.
        fs::remove(cache);
        assert(load_content(store, doc, 0, cached, error, ChapterLoad::Reading, 2, {}, &deferred));
        hot.content_cache = std::make_shared<ContentCacheEntry>(std::move(deferred));
        const auto retained_opens = epub_open_count();
        ContentJob retained(hot); assert(retained.start()); retained.wait();
        assert(retained.result.error.empty() && retained.result.retained_cache_hit && retained.result.chapter.text == original.text);
        assert(epub_open_count() == retained_opens && !fs::exists(cache));
        auto damaged = std::make_shared<ContentCacheEntry>(*hot.content_cache);
        damaged->bytes.back() ^= 1; hot.content_cache = damaged;
        ContentJob repaired(hot); assert(repaired.start()); repaired.wait();
        assert(repaired.result.error.empty() && !repaired.result.retained_cache_hit && repaired.result.chapter.text == original.text);
        assert(epub_open_count() == retained_opens + 1);
        assert(save_content_cache(store, repaired.result.content_cache));
        // Cancellation inside a decoded part never publishes partial text or source anchors.
        stops = 0;
        assert(!load_content_cache(store, doc.metadata().book_id, doc.spine()[0].resource, 0, 2, cached,
                                   [&] { return ++stops == 10; }, nullptr, &deferred));
        assert(cached.text.empty() && deferred.bytes.empty() && !deferred.from_disk);
        // Re-reading a later cold part must not rewrite previously cached prefix files.
        const auto prefix = cache.parent_path() / "0";
        const auto timestamp = fs::file_time_type::clock::now() - std::chrono::hours(1);
        fs::last_write_time(prefix, timestamp); fs::remove(cache);
        assert(load_content(store, doc, 0, cached, error, ChapterLoad::Reading, 2, {}, &deferred));
        assert(fs::last_write_time(prefix) == timestamp && !deferred.bytes.empty());
        assert(save_content_cache(store, deferred));
        for (const auto kind : {ContentJob::Kind::Chapter, ContentJob::Kind::Pagination})
        for (bool cancel : {false, true}) {
            ContentJob::Request request;
            request.kind = kind; request.root = card.string();
            request.path = (fixtures / "multi-part.epub").string(); request.identity = doc.metadata();
            request.chapter = 0; request.part = 2; request.target = ContentJob::Target::Start;
            ContentJob::delay_starts_for_test(kind, std::chrono::milliseconds(100));
            ContentJob job(std::move(request));
            assert(!job.pause() && job.start());
            ContentJob::delay_starts_for_test(kind, std::chrono::milliseconds{});
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
            while (!job.pause()) {
                assert(!job.done() && std::chrono::steady_clock::now() < deadline);
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            assert(!job.done());
            job.resume();
            // A new pause must not accept the preceding frame's acknowledgement.
            while (!job.pause()) {
                assert(!job.done() && std::chrono::steady_clock::now() < deadline);
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            if (cancel) job.cancel(); else job.resume();
            while (!job.done()) {
                assert(std::chrono::steady_clock::now() < deadline);
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            if (kind == ContentJob::Kind::Chapter)
                assert(cancel ? job.result.chapter.text.empty() : job.result.chapter.text == original.text);
            else
                assert(cancel ? job.result.starts.empty() : !job.result.starts.empty());
        }
    }
    {
        const auto root = card / "CountWithoutCache";
        fs::create_directories(root / "Books");
        fs::copy_file(fixtures / "long-chapter.epub", root / "Books/book.epub");
        const auto font = read(card / "Fonts/LXGWWenKai-Regular.ttf");
        // Trailing padding preserves the font while making its identity scan span many slices.
        write(root / "Fonts/LXGWWenKai-Regular.ttf", font + std::string(8U * 1024 * 1024, '\0'));
        write(root / "PicoPage/cache/pages", "Blocks only the optional pagination cache directory.");
        ReaderApp app(root.string(), "count-without-cache", true, true); Driver driver{app};
        assert(app.initialize()); driver.present(); opened(app, "Books/book.epub"); driver.present();
        const auto saved = app.saved_record();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        bool counted_before_identity = false;
        const auto opens = epub_open_count();
        while (app.background_due()) {
            app.background_step(); app.poll_background();
            if (app.book_pages() && std::string(app.background_state()) == "font-identity")
                counted_before_identity = true;
            assert(app.saved_record() == saved && !app.pending_frame());
            assert(std::chrono::steady_clock::now() < deadline);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        assert(counted_before_identity && app.book_pages() && app.book_pages()->total > 40);
        // The counting jobs share one open book; they do not reopen it for each batch of pages.
        assert(epub_open_count() - opens < 5);
        const auto total = app.book_pages()->total;
        opened(app, "Books/book.epub"); driver.present();
        while (app.background_due()) {
            app.background_step(); app.poll_background();
            assert(std::chrono::steady_clock::now() < deadline);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        assert(app.book_pages() && app.book_pages()->total == total);
        ReaderApp reference(root.string()); Driver ref_driver{reference};
        assert(reference.initialize()); ref_driver.present(); opened(reference, "Books/book.epub"); ref_driver.present();
        while (reference.background_due()) {
            reference.background_step();
            assert(std::chrono::steady_clock::now() < deadline);
        }
        assert(reference.book_pages() && reference.book_pages()->total == total);
    }
    {
        const auto root = card / "CountInputReuse";
        fs::create_directories(root / "Books");
        fs::copy_file(fixtures / "multi-part.epub", root / "Books/book.epub");
        ReaderApp app(root.string(), "count-input-reuse", true, true); Driver driver{app};
        assert(app.initialize()); driver.present(); opened(app, "Books/book.epub"); driver.present();
        const auto initial = app.cache_usage().page_index;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        while (app.cache_usage().page_index < initial + 64 * sizeof(std::uint32_t)) {
            app.background_step(); app.poll_background();
            assert(std::chrono::steady_clock::now() < deadline);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        // A completed batch owns enough chapter text to continue without another file read.
        fs::remove_all(root / "PicoPage/cache/content");
        fs::resize_file(root / "Books/book.epub", 0);
        const auto counted = app.cache_usage().page_index;
        driver.key(Input::K2); driver.present();
        driver.key(Input::K3); driver.present();
        driver.key(Input::K1); driver.present();
        while (app.cache_usage().page_index == counted) {
            app.background_step(); app.poll_background();
            assert(std::string(app.background_state()) != "failed");
            assert(std::chrono::steady_clock::now() < deadline);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        assert(app.cache_usage().page_index > counted && app.view() == View::Reading);
    }
    {
        ReaderApp app(card.string(), "exclusive-storage-test", true); Driver driver{app};
        assert(app.initialize()); driver.present(); opened(app, "Books/cover.epub"); driver.present();
        const auto scans = identity_hash_count();
        const auto saved = *app.saved_record();
        opened(app, "Books/cover.epub"); driver.present();
        assert(identity_hash_count() == scans && *app.saved_record() == saved);
        app.invalidate_identity_cache();
        opened(app, "Books/cover.epub"); driver.present();
        assert(identity_hash_count() == scans + 1 && *app.saved_record() == saved);
    }
    {
        fs::copy_file(fixtures / "long-chapter.epub", card / "Count/worker.epub");
        ReaderApp app(card.string(), "worker-test", true, true); Driver driver{app};
        assert(app.initialize()); driver.present(); opened(app, "Count/worker.epub"); driver.present();
        const auto record = *app.saved_record();
        const auto pixels = app.canvas().pixels();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        while (app.background_due()) {
            app.background_step();
            app.poll_background();
            assert(!app.pending_frame() && app.canvas().pixels() == pixels && *app.saved_record() == record);
            assert(app.cache_usage().page_index <= 1024U * 1024);
            assert(std::chrono::steady_clock::now() < deadline);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        assert(app.book_pages() && app.book_pages()->total > 40);
        const auto total = app.book_pages()->total;
        opened(app, "Count/worker.epub"); driver.present();
        assert(!app.book_pages());
        while (app.background_due()) {
            app.background_step(); app.poll_background();
            assert(!app.pending_frame() && std::chrono::steady_clock::now() < deadline);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        assert(app.book_pages() && app.book_pages()->total == total);
        fs::copy_file(fixtures / "long-chapter.epub", card / "Count/worker-stale.epub");
        opened(app, "Count/worker-stale.epub"); driver.present();
        // A result from the previous book cannot publish counts into the new session.
        for (unsigned n = 0; n < 20; ++n) app.background_step();
        assert(app.open_book("Books/cover.epub")); driver.present();
        driver.complete_content();
        app.poll_background(true);
        assert(app.view() == View::Reading);
        ReaderApp reference(card.string()); Driver ref_driver{reference};
        assert(reference.initialize()); ref_driver.present(); opened(reference, "Count/worker.epub"); ref_driver.present();
        while (reference.background_due()) reference.background_step();
        assert(reference.book_pages() && reference.book_pages()->total == total);
        app.invalidate_identity_cache();
        bool rejected_oversized_index = false;
        for (const auto& entry : fs::directory_iterator(card / "PicoPage/cache/pages")) {
            if (!entry.is_regular_file()) continue;
            const auto original = read(entry.path());
            if (original.find("Count/worker.epub\n") == std::string::npos) continue;
            std::size_t at = 0;
            for (unsigned n = 0; n < 9; ++n) at = original.find('\n', at) + 1;
            std::string oversized = original.substr(0, at);
            const std::uint32_t parts = 50000;
            oversized.append(reinterpret_cast<const char*>(&parts), sizeof(parts));
            oversized.append(parts * sizeof(std::uint32_t), '\0');
            oversized += md5_hex(oversized);
            write(entry.path(), oversized);
            ReaderApp bounded(card.string()); Driver bounded_driver{bounded};
            assert(bounded.initialize()); bounded_driver.present();
            opened(bounded, "Count/worker.epub"); bounded_driver.present();
            assert(!bounded.book_pages() && bounded.cache_usage().page_index <= 1024U * 1024);
            rejected_oversized_index = true;
            write(entry.path(), original);
        }
        assert(rejected_oversized_index);
        const auto font_path = card / "Fonts/LXGWWenKai-Regular.ttf";
        const auto original_font = read(font_path);
        const auto font_time = fs::last_write_time(font_path);
        auto replaced_font = original_font;
        replaced_font.back() ^= 1;
        write(font_path, replaced_font); fs::last_write_time(font_path, font_time);
        {
            ReaderApp replaced(card.string()); Driver replaced_driver{replaced};
            assert(replaced.initialize()); replaced_driver.present();
            opened(replaced, "Count/worker.epub"); replaced_driver.present();
            assert(!replaced.book_pages());
        }
        write(font_path, original_font); fs::last_write_time(font_path, font_time);
    }
    {
        using namespace pico_page::platform;
        const auto root = card / "SleepInput";
        fs::create_directories(root / "Books");
        fs::copy_file(fixtures / "valid.epub", root / "Books/book.epub");
        ReaderApp app(root.string(), "sleep-input", true, true); Driver driver{app};
        assert(app.initialize()); driver.present(); opened(app, "Books/book.epub"); driver.present();
        driver.complete_content();
        driver.key(Input::K3); driver.present(); driver.complete_content();
        for (const auto kind : {EventKind::K1, EventKind::K3, EventKind::K2, EventKind::Touch}) {
            const auto saved = *app.saved_record();
            const auto pixels = app.canvas().pixels();
            assert(sleep_frame(app)); driver.present();
            app.wake(false);
            assert(app.view() == View::Loading);
            SleepInput input;
            input.begin();
            // Contacts during lock-screen presentation or restoration are never replayed.
            for (const auto phase : {EventPhase::Begin, EventPhase::End, EventPhase::Cancel}) {
                assert(SleepInput::ignores(kind));
                input.accept({kind, phase, 1, 342, 500});
                assert(input.active() && !input.menu() && !input.power());
            }
            driver.present(); driver.complete_content();
            assert(app.view() == View::Reading && *app.saved_record() == saved);
            // Reopening may update footer counts; the confirmed body must remain identical.
            assert(std::equal(pixels.begin(), pixels.begin() + 1128 * app.canvas().stride(), app.canvas().pixels().begin()));
            input.clear();
            // A fresh contact after restoration still performs its ordinary action.
            const auto restored_pixels = app.canvas().pixels();
            if (kind == EventKind::Touch) driver.tap(342, 500);
            else driver.key(kind == EventKind::K1 ? Input::K1 : kind == EventKind::K3 ? Input::K3 : Input::K2);
            if (kind == EventKind::K2)
                assert(app.pending_frame() && app.pending_frame()->refresh == RefreshIntent::FullClean);
            driver.present(); driver.complete_content();
            if (kind == EventKind::K1 || kind == EventKind::K3) {
                assert(app.saved_record()->anchor != saved.anchor);
            } else if (kind == EventKind::Touch) {
                assert(app.view() == View::Tools);
            } else assert(*app.saved_record() == saved && app.canvas().pixels() == restored_pixels);
        }
        driver.tap(50, 50); driver.present();
        assert(app.view() == View::Library && app.can_long_press_at(300, 220));
        assert(!app.can_long_press_at(50, 50));
        assert(sleep_frame(app)); driver.present(); app.wake(false); driver.present();
        SleepInput input;
        input.begin();
        input.accept({EventKind::Touch, EventPhase::Begin, 2, 300, 220, 1000});
        input.accept({EventKind::Touch, EventPhase::End, 2, 300, 220, 1000 + kLongPressUs});
        assert(app.view() == View::Library && !input.menu() && !input.power());
        input.accept({EventKind::PowerLong});
        input.accept({EventKind::InputReset});
        assert(input.menu());
        assert(app.show_power_menu()); driver.present();
        assert(app.view() == View::PowerMenu);
        input.clear();
    }
    {
        const auto root = card / "SleepWorker";
        fs::create_directories(root / "Books"); fs::create_directories(root / "PicoPage");
        fs::copy_file(fixtures / "image-cover.epub", root / "Books/cover.epub");
        ReaderApp app(root.string(), "sleep-worker", true, true); Driver driver{app};
        assert(app.initialize()); driver.present(); opened(app, "Books/cover.epub"); driver.present();
        driver.complete_content();
        const auto cold_decodes = graphics::image_decode_count();
        const auto saved = *app.saved_record();
        const auto body_pixels = app.canvas().pixels();
        assert(!app.book_pages());
        assert(!app.prepare_sleep() && app.sleep_pending());
        assert(!app.pending_frame() && app.canvas().pixels() == body_pixels && *app.saved_record() == saved);
        assert(sleep_frame(app)); driver.present();
        assert(graphics::image_decode_count() == cold_decodes + 1 && app.view() == View::Sleeping);
        assert(app.canvas().pixel(50, 100) == 0 && app.canvas().pixel(50, 900) == 5);
        assert(*app.saved_record() == saved);
        app.wake(); driver.present(); driver.complete_content();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        const auto prepared_decodes = graphics::image_decode_count();
        assert(sleep_frame(app)); driver.present();
        assert(graphics::image_decode_count() == prepared_decodes && app.view() == View::Sleeping);
        assert(app.canvas().pixel(50, 100) == 0 && app.canvas().pixel(50, 900) == 5);
        app.wake(); driver.present(); driver.complete_content();
        // Reading spends idle time on saving, prefetch and pagination; it never starts the cover worker.
        fs::remove_all(root / "PicoPage/cache/covers");
        while (app.save_due() || app.save_busy() || app.background_due()) {
            if (app.save_due()) app.save_progress();
            app.background_step(); app.poll_background();
            assert(std::string(app.background_state()) != "sleep-cover" && !fs::exists(root / "PicoPage/cache/covers"));
            assert(std::chrono::steady_clock::now() < deadline);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        assert(graphics::image_decode_count() == prepared_decodes && app.book_pages());
    }
    {
        const auto root = card / "SleepAdopt";
        fs::create_directories(root / "Books");
        fs::copy_file(fixtures / "image-cover.epub", root / "Books/cover.epub");
        ReaderApp app(root.string(), "sleep-adopt", true, true); Driver driver{app};
        assert(app.initialize()); driver.present(); opened(app, "Books/cover.epub");
        const auto content_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        do {
            if (app.pending_frame()) app.complete_frame(true);
            app.poll_background();
            assert(std::chrono::steady_clock::now() < content_deadline);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        } while (app.content_busy() || app.pending_frame());
        assert(app.save_due() && !app.background_due());
        app.background_step();
        assert(!app.background_due());
        app.save_progress();
        while (app.save_busy()) {
            app.poll_background();
            assert(std::chrono::steady_clock::now() < content_deadline);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        assert(!app.save_due() && std::string(app.background_state()) != "sleep-cover");
        const auto pixels = app.canvas().pixels();
        assert(!app.prepare_sleep() && app.sleep_pending());
        app.cancel_sleep();
        assert(!app.sleep_pending() && std::string(app.background_state()) == "sleep-cover");
        // A repeated request adopts the cancelled worker: with starts failing, only adoption can leave it pending.
        ContentJob::fail_starts_for_test(ContentJob::Kind::SleepCover, 1, ContentJob::StartFailure::TaskCreate);
        assert(!app.prepare_sleep() && app.sleep_pending());
        ContentJob::fail_starts_for_test(ContentJob::Kind::SleepCover, 0, ContentJob::StartFailure::TaskCreate);
        assert(app.canvas().pixels() == pixels && !app.pending_frame());
        assert(sleep_frame(app)); driver.present();
        assert(app.view() == View::Sleeping);
        // The cancelled attempt may have stopped before publishing; the next request completes the cover.
        app.wake(); driver.present(); driver.complete_content();
        assert(sleep_frame(app)); driver.present();
        assert(app.canvas().pixel(50, 100) == 0 && app.canvas().pixel(50, 900) == 5);
    }
    {
        const auto root = card / "SleepHandoff";
        fs::create_directories(root / "Books");
        fs::copy_file(fixtures / "image-cover.epub", root / "Books/cover.epub");
        ReaderApp app(root.string(), "sleep-handoff", true, true); Driver driver{app};
        assert(app.initialize()); driver.present(); opened(app, "Books/cover.epub"); driver.present();
        driver.complete_content();
        const auto saved = *app.saved_record();
        const auto pixels = app.canvas().pixels();
        assert(app.begin_sleep() && app.pending_frame());
        app.poll_background();
        assert(*app.saved_record() == saved && app.pending_frame()->purpose == FramePurpose::Overlay);
        driver.present(); app.cancel_sleep(); driver.present();
        assert(*app.saved_record() == saved && app.canvas().pixels() == pixels);
        // Failed publication ends the sleep request after one attempt; nothing retries until the next request.
        fs::remove_all(root / "PicoPage/cache/covers");
        write(root / "PicoPage/cache/covers", "blocked");
        assert(!app.prepare_sleep() && app.sleep_pending());
        app.cancel_sleep();
        assert(!app.sleep_pending() && app.view() == View::Reading);
        assert(app.show_power_menu());
        assert(app.view() == View::Reading && !app.pending_frame());
        assert(!app.prepare_sleep());
        driver.key(Input::K2);
        assert(app.pending_frame() && app.pending_frame()->refresh == RefreshIntent::FullClean);
        driver.present();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        while (app.view() != View::PowerMenu) {
            app.poll_background();
            assert(*app.saved_record() == saved);
            assert(std::chrono::steady_clock::now() < deadline);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        driver.present(); driver.tap(300, 1120); driver.present();
        assert(app.view() == View::Reading && app.canvas().pixels() == pixels);
        assert(std::string(app.background_state()) != "sleep-cover");
        const auto retry_decodes = graphics::image_decode_count();
        assert(sleep_frame(app)); driver.present();
        assert(graphics::image_decode_count() == retry_decodes + 1);
        assert(app.view() == View::Sleeping && *app.saved_record() == saved);
        app.wake(); driver.present(); driver.complete_content();
        ContentJob::fail_starts_for_test(ContentJob::Kind::SleepCover, 1, ContentJob::StartFailure::InternalMemory);
        assert(app.prepare_sleep() && !app.sleep_pending()); driver.present();
        assert(app.view() == View::Sleeping && *app.saved_record() == saved);
        app.wake(); driver.present(); driver.complete_content();
        assert(sleep_frame(app)); driver.present();
        assert(app.view() == View::Sleeping && *app.saved_record() == saved);
        assert(fs::is_regular_file(root / "PicoPage/cache/covers"));
        app.wake(); driver.present(); driver.complete_content();
        fs::remove(root / "PicoPage/cache/covers");
        assert(sleep_frame(app)); driver.present();
        assert(app.canvas().pixel(50, 100) == 0 && app.canvas().pixel(50, 900) == 5);
        assert(*app.saved_record() == saved);
    }
    {
        fs::copy_file(fixtures / "image-flow.epub", card / "Books/prefetch-image.epub");
        ReaderApp app(card.string(), "image-prefetch", true, true); Driver driver{app};
        assert(app.initialize()); driver.present(); opened(app, "Books/prefetch-image.epub"); driver.present();
        driver.complete_content();
        // Idle time plans the next page without preparing its image; the turn shows the placeholder first.
        const auto decodes = graphics::image_decode_count();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        while (app.background_due()) {
            app.background_step(); app.poll_background();
            assert(!app.pending_frame() && std::chrono::steady_clock::now() < deadline);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        assert(app.cache_usage().next_layout && !app.cache_usage().next_bitmap && graphics::image_decode_count() == decodes);
        driver.key(Input::K3);
        assert(app.pending_frame()->purpose == FramePurpose::Body && app.content_busy());
        driver.present();
        assert(app.cache_usage().bitmap_hits == 0 && graphics::image_decode_count() == decodes);
        driver.complete_content();
        assert(app.canvas().pixel(36, 76) == 1);
    }
    {
        fs::copy_file(fixtures / "next-chapter-image.epub", card / "Books/next-chapter-image.epub");
        ReaderApp app(card.string(), "next-chapter-image", true, true); Driver driver{app};
        assert(app.initialize()); driver.present(); opened(app, "Books/next-chapter-image.epub"); driver.present();
        // Idle time prepares the adjacent text and source anchors, without decoding its image.
        const auto decodes = graphics::image_decode_count();
        const auto saved = app.saved_record();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        while (app.background_due()) {
            app.background_step(); app.poll_background();
            assert(!app.pending_frame() && std::chrono::steady_clock::now() < deadline);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        assert(!app.cache_usage().next_layout && !app.cache_usage().next_bitmap && app.cache_usage().next_chapter);
        assert(graphics::image_decode_count() == decodes && app.saved_record() == saved);
        const auto opens = epub_open_count();
        driver.key(Input::K3);
        assert(app.cache_usage().chapter_prefetch_hits == 1 && epub_open_count() == opens);
        assert(app.content_busy());
        driver.present(); driver.complete_content();
        assert(app.view() == View::Reading && app.saved_record()->anchor.resource.find("chapter2") != std::string::npos);
        assert(app.cache_usage().bitmap_hits == 0);
    }
    for (const bool cached_source : {false, true}) {
        const auto root = card / (cached_source ? "ChapterReturnCached" : "ChapterReturn");
        fs::create_directories(root / "Books");
        fs::copy_file(fixtures / "valid.epub", root / "Books/book.epub");
        std::vector<fs::path> cached_files;
        const auto cache_timestamp = fs::file_time_type::clock::now() - std::chrono::hours(1);
        if (cached_source) {
            EpubDocument source; Chapter content; std::string detail;
            assert(source.open((root / "Books/book.epub").string(), detail));
            for (std::size_t index = 0; index < source.spine().size(); ++index)
                assert(load_content(FileStore(root.string()), source, index, content, detail, ChapterLoad::Reading, 0));
            for (const auto& entry : fs::recursive_directory_iterator(root / "PicoPage/cache/content"))
                if (entry.is_regular_file()) {
                    cached_files.push_back(entry.path());
                    fs::last_write_time(entry.path(), cache_timestamp);
                }
        }
        ReaderApp app(root.string(), "chapter-return", true, true); Driver driver{app};
        assert(app.initialize()); driver.present(); opened(app, "Books/book.epub"); driver.present();
        const auto first = app.saved_record()->anchor;
        const auto first_pixels = app.canvas().pixels();
        const auto opened_count = epub_open_count();
        assert(app.cache_usage().chapter_stream > 0);
        driver.key(Input::K3); driver.present(); driver.complete_content();
        assert(app.saved_record()->anchor.resource != first.resource && epub_open_count() == opened_count);
        const auto opens = epub_open_count();
        for (unsigned turn = 0; turn < 3; ++turn) {
            // No idle/cache-write step occurs between these turns.
            driver.key(Input::K1); driver.present(); driver.complete_content();
            assert(app.saved_record()->anchor == first && epub_open_count() == opens);
            assert(std::equal(first_pixels.begin(), first_pixels.begin() + 342 * 1128, app.canvas().pixels().begin()));
            driver.key(Input::K3); driver.present(); driver.complete_content();
            assert(app.saved_record()->anchor.resource != first.resource && epub_open_count() == opens);
        }
        if (cached_source) {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
            while (app.background_due()) {
                app.background_step(); app.poll_background();
                assert(std::chrono::steady_clock::now() < deadline);
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            for (const auto& path : cached_files) assert(fs::last_write_time(path) == cache_timestamp);
        }
        // The first source load is denied; retrying after evicting optional payloads restores reading.
        static std::uint64_t memory_retry_open;
        memory_retry_open = epub_open_count() + 1;
        set_chapter_memory_probe([](std::size_t) { return epub_open_count() >= memory_retry_open; });
        driver.key(Input::K1); driver.present(); driver.complete_content();
        set_chapter_memory_probe(nullptr);
        assert(app.view() == View::Reading && app.saved_record()->anchor == first && epub_open_count() == memory_retry_open);
    }
    {
        const auto root = card / "ChapterStream";
        fs::create_directories(root / "Books");
        fs::copy_file(fixtures / "multi-part.epub", root / "Books/book.epub");
        ContentJob::Request request;
        request.kind = ContentJob::Kind::Open; request.root = root.string();
        request.relative_path = "Books/book.epub"; request.path = (root / request.relative_path).string();
        request.chapter_session = std::make_shared<ChapterSession>();
        ContentJob opened_job(request); assert(opened_job.start()); opened_job.wait();
        assert(opened_job.result.error.empty() && opened_job.result.chapter.part == 0);
        auto& reader = request.chapter_session->document;
        assert(reader.read_stats().stream_active && opened_job.result.chapter_stream_bytes > 0);
        const auto before = reader.read_stats();
        request.kind = ContentJob::Kind::Chapter; request.part = 1; request.identity = reader.metadata();
        request.chapter_resource = reader.spine()[0].resource;
        const auto opens = epub_open_count();
        ContentJob next(request); assert(next.start()); next.wait();
        assert(next.result.error.empty() && next.result.chapter.part == 1 && epub_open_count() == opens);
        EpubDocument reference; Chapter expected; std::string detail;
        assert(reference.open(request.path, detail));
        assert(reference.load_chapter(0, expected, detail, ChapterLoad::Reading, 1));
        assert(next.result.chapter.text == expected.text && next.result.chapter.runs.size() == expected.runs.size());
        assert(reader.read_stats().xml_feed_bytes - before.xml_feed_bytes < reference.read_stats().xml_feed_bytes);
        // A reverse turn resets only the cursor; a later uncached chapter reuses the same package.
        auto reverse = request;
        reverse.target = ContentJob::Target::End; reverse.part = 0;
        reverse.known_start = 0; reverse.known_end = opened_job.result.chapter.text.size();
        const auto reused_opens = epub_open_count();
        ContentJob previous(reverse); assert(previous.start()); previous.wait();
        assert(previous.result.error.empty() && !reader.read_stats().stream_active);
        reverse.target = ContentJob::Target::Start; reverse.chapter = 1;
        reverse.chapter_resource = reader.spine()[1].resource;
        ContentJob next_chapter(reverse); assert(next_chapter.start()); next_chapter.wait();
        assert(next_chapter.result.error.empty() && next_chapter.result.chapter.resource == reverse.chapter_resource);
        assert(epub_open_count() == reused_opens);
        ContentJob resumed(request); assert(resumed.start()); resumed.wait();
        assert(resumed.result.error.empty() && reader.read_stats().stream_active);
        request.part = 2;
        ContentJob::delay_starts_for_test(ContentJob::Kind::Chapter, std::chrono::seconds(2));
        ContentJob cancelled(request); assert(cancelled.start()); cancelled.cancel(); cancelled.wait();
        ContentJob::delay_starts_for_test(ContentJob::Kind::Chapter, std::chrono::milliseconds{});
        assert(cancelled.result.error == kChapterCancelled && reader.metadata().book_id.empty());
        assert(!reader.read_stats().stream_active && reader.read_stats().zip_allocated_bytes == 0);
    }
    {
        const auto root = card / "ChapterPrefetchCancel";
        fs::create_directories(root / "Books");
        fs::copy_file(fixtures / "valid.epub", root / "Books/book.epub");
        ReaderApp app(root.string(), "chapter-prefetch-cancel", true, true); Driver driver{app};
        assert(app.initialize()); driver.present(); opened(app, "Books/book.epub"); driver.present();
        const auto saved = app.saved_record();
        ContentJob::delay_starts_for_test(ContentJob::Kind::Chapter, std::chrono::seconds(2));
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        while (std::string(app.background_state()) != "chapter-prefetch") {
            app.background_step(); app.poll_background();
            assert(!app.pending_frame() && std::chrono::steady_clock::now() < deadline);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        driver.refresh();
        ContentJob::delay_starts_for_test(ContentJob::Kind::Chapter, std::chrono::milliseconds{});
        assert(app.saved_record() == saved && !app.cache_usage().next_chapter);
        // A cancelled preparation must not delay or replace a requested chapter.
        driver.key(Input::K3); driver.present(); driver.complete_content();
        assert(app.view() == View::Reading && app.saved_record()->anchor.resource != saved->anchor.resource);
        driver.key(Input::K1); driver.present(); driver.complete_content();
        // Refusing optional preparation leaves normal foreground reading available.
        set_chapter_memory_probe([](std::size_t) { return false; });
        for (unsigned i = 0; i < 10; ++i) { app.background_step(); app.poll_background(); }
        assert(!app.cache_usage().next_chapter && app.saved_record()->anchor == saved->anchor);
        set_chapter_memory_probe(nullptr);
        driver.key(Input::K3); driver.present(); driver.complete_content();
        assert(app.view() == View::Reading && app.saved_record()->anchor.resource != saved->anchor.resource);
        driver.key(Input::K1); driver.present(); driver.complete_content();
        while (!app.cache_usage().next_chapter) {
            app.background_step(); app.poll_background();
            assert(!app.pending_frame() && std::chrono::steady_clock::now() < deadline);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        fs::copy_file(fixtures / "long-chapter.epub", root / "Books/other.epub");
        opened(app, "Books/other.epub"); driver.present();
        assert(!app.cache_usage().next_chapter && app.saved_record()->book_id != saved->book_id);
        assert(app.cache_usage().chapter_stream > 0);
        assert(sleep_frame(app)); driver.present();
        assert(app.cache_usage().chapter_stream == 0);
        app.wake(); driver.present(); driver.complete_content();
        assert(app.view() == View::Reading);
        assert(app.prepare_usb_transfer() && app.cache_usage().chapter_stream == 0);
    }
    {
        ReaderApp app(card.string(), "preview-worker", true, true); Driver driver{app};
        assert(app.initialize()); driver.present(); opened(app, "Books/cover.epub"); driver.present();
        driver.complete_content();
        const auto saved = *app.saved_record();
        driver.tap(342, 500); driver.present();
        const auto before = graphics::image_decode_count();
        driver.tap(205, 1140);
        assert(app.view() == View::Loading && graphics::image_decode_count() == before);
        driver.present(); driver.refresh(); driver.tap(342, 400); driver.present();
        driver.complete_content();
        assert(app.view() == View::Tools && *app.saved_record() == saved);
        driver.tap(205, 1140); driver.present(); driver.complete_content();
        assert(app.view() == View::Typography && *app.saved_record() == saved);
        const auto prepared = graphics::image_decode_count();
        driver.tap(580, 1020); driver.present(); driver.complete_content();
        assert(app.view() == View::Typography && graphics::image_decode_count() == prepared);
        driver.close(); driver.present();
        assert(app.view() == View::Reading && *app.saved_record() == saved);
    }
    {
        fs::remove_all(card / "PicoPage/cache/images");
        ReaderApp app(card.string(), "image-worker", true, true); Driver driver{app};
        assert(app.initialize()); driver.present();
        assert(app.open_book("Books/cover.epub") && app.content_busy());
        assert(app.pending_frame()->purpose == FramePurpose::Overlay);
        driver.present();
        const auto saved = app.saved_record();
        driver.refresh();
        app.poll_background();
        driver.tap(580, 1170); driver.present();
        driver.complete_content();
        assert(app.view() == View::Library && app.saved_record() == saved);
        assert(app.open_book("Books/cover.epub")); driver.present(); driver.complete_content();
        assert(app.view() == View::Reading && !app.content_busy());
        const auto pixels = app.canvas().pixels();
        const auto decodes = graphics::image_decode_count();
        const auto verified_scans = identity_hash_count();
        assert(app.open_book("Books/cover.epub")); driver.present(); driver.complete_content();
        assert(graphics::image_decode_count() == decodes && app.canvas().pixels() == pixels);
        assert(identity_hash_count() == verified_scans);
        assert(app.open_book("Books/cover.epub")); driver.present(); app.poll_background();
        opened(app, "Books/book.epub"); driver.present();
        const auto switched = app.canvas().pixels();
        const auto switched_record = app.saved_record();
        driver.complete_content();
        assert(app.canvas().pixels() == switched && app.saved_record() == switched_record);
        assert(sleep_frame(app)); driver.present();
        const auto sleep_scans = identity_hash_count();
        app.wake(); driver.present();
        assert(app.view() == View::Loading && app.content_busy());
        driver.refresh(); driver.complete_content();
        assert(app.view() == View::Reading && app.saved_record()->anchor == switched_record->anchor);
        assert(identity_hash_count() == sleep_scans + 1);
        assert(app.open_book("Books/cover.epub"));
        const auto before_failure = app.saved_record();
        app.complete_frame(false);
        if (app.pending_frame()) driver.present();
        driver.complete_content();
        assert(app.view() == View::Error && app.saved_record() == before_failure);
    }
    {
        fs::create_directories(card / "Worker");
        fs::copy_file(fixtures / "multi-part.epub", card / "Worker/reference.epub");
        fs::copy_file(fixtures / "multi-part.epub", card / "Worker/async.epub");
        EpubDocument source; Chapter part; std::string detail;
        assert(source.open((card / "Worker/reference.epub").string(), detail));
        assert(source.load_chapter(0, part, detail, ChapterLoad::Reading, 1));
        Anchor first;
        std::uint32_t start = 0;
        while (start < part.text.size() && !part.anchor_at(start, first)) ++start;
        assert(start < part.text.size());
        const ReadingRecord seeded{source.metadata().book_id, "epub-native-v1", "fixture", 1, first};
        FileStore store(card.string());
        assert(store.save_progress("Worker/reference.epub", seeded).status == StoreStatus::Ok);
        assert(store.save_progress("Worker/async.epub", seeded).status == StoreStatus::Ok);
        ReaderApp reference(card.string()), async(card.string(), "async-chapter", true, true);
        Driver ref_driver{reference}, driver{async};
        assert(reference.initialize()); ref_driver.present(); opened(reference, "Worker/reference.epub"); ref_driver.present();
        assert(async.initialize()); driver.present(); opened(async, "Worker/async.epub"); driver.present();
        const auto same_body = [&] {
            assert(reference.saved_record()->anchor == async.saved_record()->anchor);
            assert(std::equal(reference.canvas().pixels().begin(), reference.canvas().pixels().begin() + 342 * 1128,
                              async.canvas().pixels().begin()));
        };
        same_body();
        const auto prepare_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        while (!async.cache_usage().next_bitmap) {
            async.background_step(); async.poll_background();
            assert(!async.pending_frame() && std::chrono::steady_clock::now() < prepare_deadline);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        assert(async.cache_usage().next_bitmap == 684U * 1216U / 2);
        driver.refresh();
        const auto bitmap_hits = async.cache_usage().bitmap_hits;
        bool reverse = false;
        auto check_direction = [&](const Frame& frame) {
            if (frame.purpose == FramePurpose::Body) assert(frame.reverse_page_effect == reverse);
            else assert(!frame.reverse_page_effect);
        };
        ref_driver.frame_observer = check_direction;
        driver.frame_observer = check_direction;
        reverse = false;
        ref_driver.key(Input::K3); ref_driver.present();
        driver.key(Input::K3); driver.present();
        assert(async.cache_usage().bitmap_hits == bitmap_hits + 1);
        same_body();
        reverse = true;
        ref_driver.key(Input::K1); ref_driver.present();
        driver.key(Input::K1); driver.present(); driver.complete_content(); same_body();
        reverse = true;
        ref_driver.key(Input::K1); ref_driver.present();
        const auto previous = async.saved_record();
        ContentJob::delay_starts_for_test(ContentJob::Kind::Chapter, std::chrono::milliseconds(1500));
        driver.key(Input::K1);
        assert(!async.pending_frame() && async.saved_record() == previous);
        driver.present();
        assert(async.view() == View::Loading && async.saved_record() == previous);
        driver.refresh(); driver.complete_content(); same_body();
        ContentJob::delay_starts_for_test(ContentJob::Kind::Chapter, std::chrono::milliseconds(0));
        // A completed adjacent part is published by the turn, with the same body and source anchor.
        const auto chapter_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        while (!async.cache_usage().next_chapter) {
            async.background_step(); async.poll_background();
            assert(!async.pending_frame() && std::chrono::steady_clock::now() < chapter_deadline);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        reverse = false;
        ref_driver.key(Input::K3); ref_driver.present();
        const auto chapter_opens = epub_open_count();
        const auto chapter_hits = async.cache_usage().chapter_prefetch_hits;
        driver.key(Input::K3);
        assert(async.view() == View::Reading && async.pending_frame());
        assert(epub_open_count() == chapter_opens && async.cache_usage().chapter_prefetch_hits == chapter_hits + 1);
        driver.present();
        driver.refresh(); driver.complete_content(); same_body();
        assert(async.saved_record()->anchor == first);
        reverse = false;
        for (auto* d : {&ref_driver, &driver}) {
            d->tap(342, 500); d->present(); d->tap(68, 1140); d->present();
            d->tap(300, 752); d->present(); d->complete_content();
        }
        assert(async.saved_record()->anchor.resource.find("chapter2") != std::string::npos);
        same_body();
        reverse = true;
        ref_driver.key(Input::K1); ref_driver.present();
        driver.key(Input::K1); driver.present(); driver.complete_content(); same_body();
        reverse = true;
        ref_driver.key(Input::K1); ref_driver.present();
        driver.key(Input::K1); driver.present(); driver.complete_content(); same_body();
        reverse = false; // Opening another book and following links use the default direction.
        fs::copy_file(fixtures / "multi-part-link.epub", card / "Worker/link-reference.epub");
        fs::copy_file(fixtures / "multi-part-link.epub", card / "Worker/link-async.epub");
        opened(reference, "Worker/link-reference.epub"); ref_driver.present();
        opened(async, "Worker/link-async.epub"); driver.present();
        const auto origin = async.saved_record()->anchor;
        const auto config = store.load_settings("Worker/link-async.epub").settings;
        const auto link_x = config.margin_x_px + config.indent_chars * config.font_size_px + config.font_size_px / 2;
        ref_driver.tap(link_x, 80); ref_driver.present();
        driver.tap(link_x, 80); driver.present(); driver.complete_content(); same_body();
        assert(async.saved_record()->anchor.node.find("/p[4501]/") != std::string::npos);
        ref_driver.tap(150, 50); ref_driver.present();
        driver.tap(150, 50); driver.present(); driver.complete_content(); same_body();
        assert(async.saved_record()->anchor == origin);
    }
    {
        fs::remove(card / "PicoPage/network.toml");
        // Upload over the device access point: the entry only requests a session, the page keeps K2
        // and ignores every control but its own, and a session that never started returns to the
        // network page with the reason.
        ReaderApp app(card.string()); Driver driver{app};
        assert(app.initialize()); driver.present();
        driver.tap(600, 50); driver.present(); assert(app.view() == View::Settings);
        driver.tap(300, 180); driver.present(); assert(app.view() == View::Import);
        driver.tap(342, 660);
        assert(!app.pending_frame() && app.view() == View::Import);
        assert(app.take_portal_action() == PortalAction::Upload && app.take_portal_action() == PortalAction::None);
        driver.portal({PortalPhase::Closed, "", "", "", "设备热点没有启动，请重试。", "", 0, ""}); driver.present();
        assert(app.view() == View::Import && app.status() == "设备热点没有启动，请重试。");
        driver.tap(342, 660);
        assert(app.take_portal_action() == PortalAction::Upload);
        assert(app.prepare_portal()); driver.present(); assert(app.view() == View::Portal);
        const auto starting = app.canvas().pixels();
        PortalStatus status{PortalPhase::Waiting, "PicoPage-TEST", "abcd2345", "http://192.168.4.1", "", "", 0, ""};
        driver.portal(status);
        assert(app.pending_frame() && app.pending_frame()->refresh == RefreshIntent::FullClean);
        driver.present();
        // Top-left finder modules of the access point code (version 3 at 7 px) and the address
        // code (version 2 at 8 px), each inside its white quiet zone.
        assert(app.canvas().pixels() != starting);
        assert(app.canvas().pixel(66, 206) == 0 && app.canvas().pixel(41, 181) == 15);
        assert(app.canvas().pixel(68, 552) == 0 && app.canvas().pixel(41, 525) == 15);
        driver.refresh();
        driver.key(Input::K1); driver.key(Input::K3); driver.tap(60, 50); driver.tap(342, 500);
        assert(!app.pending_frame() && app.view() == View::Portal);
        assert(app.take_portal_action() == PortalAction::None && !app.prepare_sleep());
        // A status change under the same codes updates only the summary band.
        const auto idle = app.canvas().pixels();
        status.phase = PortalPhase::Receiving;
        driver.portal(status);
        assert(app.pending_frame() && app.pending_frame()->refresh == RefreshIntent::Content);
        assert(app.pending_frame()->region && app.pending_frame()->region->y == 820 &&
               app.pending_frame()->region->height == 252 && app.pending_frame()->region->width == 684);
        driver.present();
        const auto stride = app.canvas().stride();
        assert(std::equal(idle.begin(), idle.begin() + 820 * stride, app.canvas().pixels().begin()));
        assert(std::equal(idle.begin() + 1072 * stride, idle.end(), app.canvas().pixels().begin() + 1072 * stride));
        assert(!std::equal(idle.begin() + 820 * stride, idle.begin() + 1072 * stride, app.canvas().pixels().begin() + 820 * stride));
        driver.tap(342, 1120);
        assert(app.take_portal_action() == PortalAction::Stop); driver.present();
        driver.tap(342, 1120);
        assert(app.take_portal_action() == PortalAction::None && !app.pending_frame());
        driver.portal({PortalPhase::Closed, "", "", "", "", "", 0, ""}); driver.present();
        assert(app.view() == View::Library && app.status().empty());
        driver.portal(status);
        assert(!app.pending_frame() && app.view() == View::Library);
        // With a book open the session starts from saved progress and leaves it unchanged.
        opened(app, "Books/book.epub"); driver.present();
        driver.key(Input::K3); driver.present();
        assert(app.prepare_portal()); driver.present();
        assert(app.view() == View::Portal && !app.save_due());
        const auto saved = *app.saved_record();
        driver.portal({PortalPhase::Closed, "", "", "", "", "", 0, ""}); driver.present();
        assert(app.view() == View::Import && *app.saved_record() == saved);
        driver.tap(342, 660);
        assert(app.take_portal_action() == PortalAction::Upload);
        assert(app.prepare_portal()); driver.present();
        driver.portal({PortalPhase::Waiting, "PicoPage-TEST", "abcd2345", "http://192.168.4.1", "", "", 0, ""});
        driver.present(); driver.refresh();
        assert(app.view() == View::Portal && *app.saved_record() == saved && !app.prepare_sleep());
        fs::copy_file(card / "Books/book.epub", card / "Books/wifi-import.epub");
        driver.tap(342, 1120);
        assert(app.take_portal_action() == PortalAction::Stop); driver.present();
        driver.portal({PortalPhase::Closed, "", "", "", "", "", 0, ""}); driver.present();
        assert(app.view() == View::Library && *app.saved_record() == saved);
        fs::remove(card / "Books/wifi-import.epub");

        // With a saved network the page offers both upload paths. A station session that never
        // reaches its page returns to the network page with the reason.
        fs::create_directories(card / "PicoPage");
        { std::ofstream file(card / "PicoPage/network.toml"); file << "schema_version = 2\n[[wifi]]\nssid = \"home\"\npassword = \"preview-password\"\n"; }
        driver.tap(600, 50); driver.present(); assert(app.view() == View::Settings);
        driver.tap(300, 180); driver.present(); assert(app.view() == View::Import);
        driver.tap(342, 880);
        assert(app.take_portal_action() == PortalAction::Upload);
        driver.tap(342, 760);
        assert(app.take_portal_action() == PortalAction::UploadStation);
        assert(app.prepare_portal(true)); driver.present();
        assert(app.view() == View::Portal && !app.prepare_sleep());
        const std::string refused = "无法连接「home」，密码可能不正确。可以改用设备热点传书。";
        driver.portal({PortalPhase::Closed, "", "", "", refused, "", 0, ""}); driver.present();
        assert(app.view() == View::Import && app.status() == refused && *app.saved_record() == saved);
        // A started session shows one code for the joined network and the saved-book summary.
        driver.tap(342, 760);
        assert(app.take_portal_action() == PortalAction::UploadStation);
        assert(app.prepare_portal(true)); driver.present();
        const auto connecting = app.canvas().pixels();
        PortalStatus station{PortalPhase::Waiting, "", "", "http://192.168.1.23/abc234", "", "home", 0, ""};
        driver.portal(station);
        assert(app.pending_frame() && app.pending_frame()->refresh == RefreshIntent::FullClean);
        driver.present();
        // Top-left finder module of the version 2 address code at 11 px inside its quiet zone.
        assert(app.canvas().pixels() != connecting);
        assert(app.canvas().pixel(206, 310) == 0 && app.canvas().pixel(200, 304) == 15);
        const auto waiting = app.canvas().pixels();
        station.saved_books = 2; station.latest_book = "呼兰河传";
        driver.portal(station);
        assert(app.pending_frame() && app.pending_frame()->refresh == RefreshIntent::Content);
        assert(app.pending_frame()->region && app.pending_frame()->region->y == 784 && app.pending_frame()->region->height == 288);
        driver.present();
        assert(app.canvas().pixels() != waiting && app.view() == View::Portal);
        driver.refresh();
        driver.key(Input::K1); driver.key(Input::K3); driver.tap(342, 500);
        assert(!app.pending_frame() && app.take_portal_action() == PortalAction::None);
        const std::string lost = "与已保存的 Wi-Fi 连接已断开。可以改用设备热点传书。";
        driver.portal({PortalPhase::Closed, "", "", "", lost, "", 0, ""}); driver.present();
        assert(app.view() == View::Import && app.status() == lost && *app.saved_record() == saved);
        fs::remove(card / "PicoPage/network.toml");
    }
    {
        fs::create_directories(card / "SaveWorker");
        fs::copy_file(fixtures / "multi-part.epub", card / "SaveWorker/book.epub");
        ReaderApp app(card.string(), "save-worker", true, true);
        Driver driver{app};
        assert(app.initialize()); driver.present(); opened(app, "SaveWorker/book.epub");
        const auto first = app.pending_frame()->anchor;
        app.complete_frame(true);
        app.save_progress();
        assert(app.save_busy() && !app.saved_record());
        driver.key(Input::K3);
        assert(app.pending_frame() && app.pending_frame()->purpose == FramePurpose::Body);
        const auto second = app.pending_frame()->anchor;
        assert(first != second);
        app.complete_frame(true);
        const auto drain = [&] {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
            while (app.save_busy()) {
                app.poll_background();
                assert(std::chrono::steady_clock::now() < deadline);
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        };
        drain();
        assert(app.saved_record()->anchor == first && app.save_due());
        app.save_progress(); drain();
        assert(app.saved_record()->anchor == second && !app.save_due());
        const auto sidecar = card / "SaveWorker/book.epub.picopage";
        fs::rename(sidecar, sidecar.string() + ".held");
        write(sidecar, "not a directory");
        driver.key(Input::K3); app.complete_frame(true);
        app.save_progress(); drain();
        assert(app.saved_record()->anchor == second && app.view() == View::Error);
        assert(app.status().find("进度未保存") != std::string::npos);
        if (app.pending_frame()) app.complete_frame(true);
        const auto hashes = identity_hash_count();
        assert(app.open_book("Books/cover.epub"));
        driver.present(); driver.complete_content();
        assert(app.view() == View::Error && app.saved_record()->anchor == second);
        assert(identity_hash_count() == hashes);
        fs::remove(sidecar); fs::rename(sidecar.string() + ".held", sidecar);
    }
    {
        // A TXT shorter than one 48 KiB part reports its position against its own length.
        std::string text;
        for (int i = 0; i < 300; ++i) text += "短文测试。";
        write(card / "Txt/short.txt", text);
        ReaderApp app(card.string()); Driver driver{app};
        assert(app.initialize()); driver.present(); opened(app, "Txt/short.txt"); driver.present();
        driver.tap(342, 500); driver.present(); driver.tap(60, 50); driver.present();
        assert(app.view() == View::Library);
        int percent = -1;
        for (const auto& item : fs::directory_iterator(card / "PicoPage/cache/positions")) {
            const auto bytes = read(item.path());
            if (bytes.find("\nTxt/short.txt\n") != std::string::npos) percent = std::stoi(bytes.substr(bytes.rfind('\n') + 1));
        }
        assert(percent > 0 && percent < 60);
    }
    {
        // The library position follows the saved record: a page whose commit fails is not cached.
        ReaderApp app(card.string(), "position-worker", true, true);
        Driver driver{app};
        assert(app.initialize()); driver.present(); opened(app, "SaveWorker/book.epub");
        app.complete_frame(true);
        driver.key(Input::K3); assert(app.pending_frame()); app.complete_frame(true);
        const auto sidecar = card / "SaveWorker/book.epub.picopage";
        fs::rename(sidecar, sidecar.string() + ".held");
        write(sidecar, "not a directory");
        assert(app.open_book("Books/cover.epub"));
        driver.present(); driver.complete_content();
        assert(app.view() == View::Error);
        if (fs::exists(card / "PicoPage/cache/positions"))
            for (const auto& item : fs::directory_iterator(card / "PicoPage/cache/positions"))
                assert(read(item.path()).find("\nSaveWorker/book.epub\n") == std::string::npos);
        fs::remove(sidecar); fs::rename(sidecar.string() + ".held", sidecar);
    }
    for (const auto action : {PowerAction::Shutdown, PowerAction::Restart}) {
        const auto home = card / (action == PowerAction::Shutdown ? "PowerShutdown" : "PowerRestart");
        fs::create_directories(home / "Books");
        fs::copy_file(fixtures / "valid.epub", home / "Books/book.epub");
        ReaderApp app(home.string()); Driver driver{app};
        assert(app.initialize()); driver.present(); opened(app, "Books/book.epub"); driver.present();
        const auto before = app.saved_record();
        const auto pixels = app.canvas().pixels();
        assert(app.show_power_menu() && app.pending_frame()->refresh == RefreshIntent::Content);
        driver.present(); driver.refresh();
        driver.key(Input::K1); driver.key(Input::K3);
        assert(!app.pending_frame() && app.take_power_request().action == PowerAction::None);
        driver.tap(100, 1100);
        assert(app.pending_frame() && app.pending_frame()->refresh == RefreshIntent::Content);
        driver.present();
        assert(app.view() == View::Reading && app.canvas().pixels() == pixels && app.saved_record() == before);
        driver.typography();
        const auto draft_view = app.view(); const auto draft_pixels = app.canvas().pixels();
        assert(app.show_power_menu()); driver.present(); driver.tap(100, 1100); driver.present();
        assert(app.view() == draft_view && app.canvas().pixels() == draft_pixels);
        assert(app.show_power_menu()); driver.present();
        const int y = action == PowerAction::Shutdown ? 520 : 650;
        driver.queued(Input::TouchCenter, true, 300, y);
        assert(app.take_power_request().action == PowerAction::None && !app.pending_frame());
        // Crossing any edge cancels, even if the finger moves back before release.
        driver.drag(300, y, {23, y, 300, y});
        driver.drag(300, y, {300, y, 660, y});
        driver.drag(300, y, {300, y - 45, 300, y});
        driver.drag(300, y, {300, y, 300, y + 96});
        assert(app.take_power_request().action == PowerAction::None && !app.pending_frame());
        // Starting outside and ending inside cannot select a power action.
        driver.drag(10, y, {10, y, 300, y});
        assert(app.take_power_request().action == PowerAction::None && !app.pending_frame());
        driver.drag(300, y, {298, y - 2, 303, y + 2});
        const auto request = app.take_power_request();
        assert(request.action == action && !request.without_save && !app.pending_frame());
        assert(app.take_power_request().action == PowerAction::None);
        assert(app.prepare_power(request));
        assert(app.view() == View::PowerOff && app.pending_frame()->refresh == RefreshIntent::FullClean);
        assert(!app.close_power_files());
        // Failed final presentation still permits closing files and never queues a recovery page.
        app.complete_frame(false);
        assert(app.view() == View::PowerOff && !app.pending_frame() && app.close_power_files());
        assert(!app.save_due() && !app.background_due() && !app.take_network_request());
        app.show_power_off_help(); driver.present();
        assert(app.status().find("已保存") != std::string::npos);
        assert(app.status().find("强制关机") != std::string::npos);
        driver.tap(100, 1100); driver.key(Input::K2); driver.key(Input::K3);
        app.poll_background(); app.background_step({});
        assert(app.view() == View::PowerOff && !app.pending_frame() && !app.show_power_menu());
        std::string detail;
        assert(FileStore(home.string()).load_last_book(detail) == "Books/book.epub");
        ReaderApp reopened(home.string()); Driver reloaded{reopened};
        assert(reopened.initialize()); reloaded.present(); opened(reopened, "Books/book.epub"); reloaded.present();
        assert(reopened.saved_record()->anchor == before->anchor);
    }
    for (const bool force : {false, true}) {
        const auto home = card / (force ? "PowerForce" : "PowerReturn");
        fs::create_directories(home / "Books");
        fs::copy_file(fixtures / "valid.epub", home / "Books/book.epub");
        ReaderApp app(home.string()); Driver driver{app};
        assert(app.initialize()); driver.present(); opened(app, "Books/book.epub"); driver.present();
        const auto previous = app.saved_record();
        driver.key(Input::K3); app.complete_frame(true);
        const auto sidecar = home / "Books/book.epub.picopage";
        fs::rename(sidecar, sidecar.string() + ".held"); write(sidecar, "not a directory");
        assert(app.show_power_menu()); driver.present(); driver.tap(300, 520);
        assert(!app.prepare_power(app.take_power_request()));
        assert(app.pending_frame() && app.pending_frame()->refresh == RefreshIntent::Content);
        driver.present();
        assert(app.view() == View::PowerMenu && app.saved_record() == previous);
        assert(app.status().find("TF 卡") != std::string::npos);
        driver.refresh();
        if (force) {
            driver.drag(500, 1100, {353, 1100, 500, 1100});
            driver.queued(Input::TouchCenter, true, 500, 1100);
            assert(app.take_power_request().action == PowerAction::None && !app.pending_frame());
            driver.tap(500, 1100);
            const auto request = app.take_power_request();
            assert(request.action == PowerAction::Shutdown && request.without_save);
            assert(app.prepare_power(request)); app.complete_frame(false);
            assert(app.close_power_files()); app.show_power_off_help();
            app.complete_frame(false);
            assert(app.view() == View::PowerOff && !app.pending_frame());
            assert(app.status().find("没有完整保存") != std::string::npos && app.saved_record() == previous);
            assert(!app.take_network_request());
        } else {
            driver.tap(100, 1100); driver.present();
            assert(app.view() == View::Reading && app.saved_record() == previous);
            fs::remove(sidecar); fs::rename(sidecar.string() + ".held", sidecar);
            assert(app.show_power_menu()); driver.present(); driver.tap(300, 650);
            assert(app.prepare_power(app.take_power_request())); driver.present();
            assert(app.saved_record()->anchor != previous->anchor);
            assert(app.status().find("已保存") != std::string::npos);
        }
    }
    write(card / "PicoPage/network.toml", "schema_version=2\n[[wifi]]\nssid=\"home\"\npassword=\"password\"\n");
    {
        const auto home = card / "PowerNetworkShutdown";
        fs::create_directories(home / "Books");
        fs::copy_file(fixtures / "valid.epub", home / "Books/book.epub");
        write(home / "PicoPage/sync.toml", sync_config);
        write(home / "PicoPage/network.toml", read(card / "PicoPage/network.toml"));
        ReaderApp app(home.string()); Driver driver{app};
        assert(app.initialize()); driver.present(); opened(app, "Books/book.epub"); driver.present();
        const auto saved = app.saved_record();
        driver.sync_view(); const auto request = driver.check_remote();
        app.stop_wifi(true); driver.present(); app.cancel_sleep();
        assert(app.view() == View::NetworkClosing && request.cancelled->load() && !app.show_power_menu());
        pico_page::platform::SleepInput deferred;
        deferred.begin(); deferred.accept({pico_page::platform::EventKind::PowerLong});
        const auto closing_pixels = app.canvas().pixels();
        for (const auto kind : {pico_page::platform::EventKind::Touch, pico_page::platform::EventKind::K1,
                               pico_page::platform::EventKind::K2, pico_page::platform::EventKind::K3,
                               pico_page::platform::EventKind::InputReset}) {
            deferred.accept({kind});
            assert(!deferred.ready(true, false) && deferred.menu());
            assert(app.view() == View::NetworkClosing && app.canvas().pixels() == closing_pixels && app.saved_record() == saved);
        }
        // The reply confirms worker teardown; a late success must not adopt a cancelled candidate.
        app.complete_network(request.id, 200,
            R"({"progress":"/body[1]/DocFragment[1]/body[1]/h1[1]/text()[1].0","percentage":0.1})", "");
        driver.present();
        assert(!deferred.ready(true, false)); // The radio may outlive the response queue entry.
        assert(deferred.ready(!app.content_busy() && !app.pending_frame(), true) && deferred.menu());
        assert(app.show_power_menu()); deferred.clear(); driver.present();
        assert(app.view() == View::PowerMenu && app.saved_record() == saved && !app.take_network_request());
        driver.tap(300, 1120); driver.present();
        assert(app.view() == View::Sync && app.saved_record() == saved);
    }
    // Save failure is before sync teardown: returning preserves both candidate and upload actions.
    for (const bool upload : {false, true}) {
        const auto home = card / (upload ? "PowerSyncUpload" : "PowerSyncCandidate");
        fs::create_directories(home / "Books");
        fs::copy_file(fixtures / "valid.epub", home / "Books/book.epub");
        write(home / "PicoPage/sync.toml", sync_config);
        write(home / "PicoPage/network.toml", read(card / "PicoPage/network.toml"));
        ReaderApp app(home.string()); Driver driver{app};
        assert(app.initialize()); driver.present(); opened(app, "Books/book.epub"); driver.present();
        driver.key(Input::K3); driver.present();
        const auto before = app.saved_record();
        driver.sync_view(); auto request = driver.check_remote();
        assert(!app.show_power_menu());
        app.complete_network(request.id, 200, upload ? "{}" :
            R"({"progress":"/body[1]/DocFragment[1]/body[1]/h1[1]/text()[1].0","percentage":0.1})", "");
        driver.present();
        if (upload) { driver.tap(342, 1090); driver.present(); }
        const auto sync_view = app.view(); const auto pixels = app.canvas().pixels();
        const auto recent = home / "PicoPage/data/last-book.txt";
        fs::rename(recent, recent.string() + ".held"); fs::create_directory(recent);
        assert(app.show_power_menu()); driver.present(); driver.tap(300, 520);
        assert(!app.prepare_power(app.take_power_request())); driver.present();
        driver.tap(100, 1100); driver.present();
        assert(app.view() == sync_view && app.canvas().pixels() == pixels);
        fs::remove(recent); fs::rename(recent.string() + ".held", recent);
        driver.tap(500, upload ? 1120 : 800); driver.present();
        if (upload) {
            const auto sent = app.take_network_request();
            assert(sent && sent->request.method == "PUT");
        } else {
            assert(app.view() == View::Reading && app.saved_record()->anchor != before->anchor);
        }
    }
    {
        ReaderApp app(card.string()); Driver driver{app};
        assert(app.initialize()); driver.present();
        assert(app.show_power_menu()); app.complete_frame(false);
        assert(app.pending_frame() && app.pending_frame()->refresh == RefreshIntent::FullClean);
        driver.present();
        assert(app.view() == View::Library && app.take_power_request().action == PowerAction::None);
        app.power_unavailable("请先结束当前任务，再关机或重启。");
        assert(app.pending_frame() && app.pending_frame()->refresh == RefreshIntent::Content);
        driver.present();
        assert(app.prepare_portal()); driver.present();
        assert(!app.show_power_menu() && app.view() == View::Portal);
        driver.portal({PortalPhase::Closed, "", "", "", "", "", 0, ""}); driver.present();
        assert(app.prepare_usb_transfer()); app.show_usb_transfer(true, ""); driver.present();
        assert(!app.show_power_menu() && app.view() == View::UsbTransfer);
    }
    {
        const auto home = card / "PowerCoverResume";
        fs::create_directories(home / "Books");
        fs::copy_file(fixtures / "image-cover.epub", home / "Books/cover.epub");
        write(home / "PicoPage/settings.toml", "schema_version = 1\n[library]\nview = 'grid'\n");
        ReaderApp app(home.string(), "cover-resume", true, true); Driver driver{app};
        assert(app.initialize()); driver.present();
        assert(app.background_due() && std::string(app.background_state()) == "covers");
        const auto placeholders = app.canvas().pixels();
        app.poll_background();
        assert(app.show_power_menu()); driver.present();
        driver.tap(100, 1100); driver.present();
        assert(app.view() == View::Library && app.background_due());
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (app.background_due()) {
            app.poll_background();
            if (app.pending_frame()) driver.present();
            assert(std::chrono::steady_clock::now() < deadline);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        assert(app.canvas().pixels() != placeholders);
    }
    for (const bool async : {false, true}) {
        // Bookmarks: the tool page toggles the visible page, the reading page shows the ribbon,
        // and the contents panel lists and jumps to them in both content modes.
        const auto root = card / (async ? "BookmarksAsync" : "BookmarksSync");
        const std::string book = "Books/long.epub";
        fs::create_directories(root / "PicoPage");
        fs::create_directories(root / "Books");
        fs::copy_file(fixtures / "long-chapter.epub", root / book);
        const auto marks = root / (book + ".picopage/bookmarks.json");
        const auto progress = root / (book + ".picopage/progress.json");
        FileStore store(root.string());
        const auto ribbon = [](const ReaderApp& app) { return app.canvas().pixel(634, 10) == 1; };
        const auto step = [](Driver& driver, Input key) {
            driver.key(key);
            if (driver.app.pending_frame()) driver.present();
            driver.complete_content();
        };
        const auto tools = [](Driver& driver) {
            driver.tap(342, 500); driver.present(); assert(driver.app.view() == View::Tools);
        };
        const auto bookmark_list = [&](Driver& driver) {
            tools(driver);
            driver.tap(68, 1150); driver.present(); assert(driver.app.view() == View::Contents);
            driver.tap(500, 486); driver.present(); assert(driver.app.view() == View::Contents);
        };
        Anchor marked;
        std::string id;
        {
            ReaderApp app(root.string(), "bookmarks", async, async); Driver driver{app};
            assert(app.initialize()); driver.present(); opened(app, book); driver.present();
            for (int i = 0; i < 3; ++i) step(driver, Input::K3);
            assert(!ribbon(app) && !fs::exists(marks));
            marked = app.saved_record()->anchor;
            id = app.saved_record()->book_id;
            const auto position = *app.saved_record();
            const auto saved_progress = read(progress);
            tools(driver);
            driver.refresh();
            driver.tap(634, 50); driver.present();
            assert(app.view() == View::Tools && *app.saved_record() == position && read(progress) == saved_progress);
            auto list = store.load_bookmarks(book, id, "epub-native-v1");
            assert(list.status == StoreStatus::Ok && list.bookmarks.size() == 1 && list.next_sequence == 2);
            assert(list.bookmarks[0].anchor == marked && !list.bookmarks[0].excerpt.empty());
            assert(!list.bookmarks[0].chapter.empty() && list.bookmarks[0].percent >= 0 && !list.bookmarks[0].created_at);
            driver.close(); driver.present();
            assert(app.view() == View::Reading && ribbon(app));
            step(driver, Input::K3); assert(!ribbon(app));
            step(driver, Input::K1); assert(ribbon(app) && app.saved_record()->anchor == marked);
            for (int i = 0; i < 4; ++i) step(driver, Input::K3);
            assert(app.saved_record()->anchor != marked);

            // The list marks nothing on another page and jumps to the presented bookmark position.
            bookmark_list(driver);
            driver.refresh();
            driver.tap(300, 578);
            if (app.pending_frame()) driver.present();
            driver.complete_content();
            assert(app.view() == View::Reading && app.saved_record()->anchor == marked && ribbon(app));

            // A second tap on the toggle deletes the bookmarks of the visible page only.
            step(driver, Input::K3);
            tools(driver); driver.tap(634, 50); driver.present();
            list = store.load_bookmarks(book, id, "epub-native-v1");
            assert(list.bookmarks.size() == 2 && list.bookmarks[1].sequence == 2);
            driver.close(); driver.present(); step(driver, Input::K1);
            tools(driver); driver.tap(634, 50); driver.present();
            list = store.load_bookmarks(book, id, "epub-native-v1");
            assert(list.bookmarks.size() == 1 && list.bookmarks[0].sequence == 2 && list.next_sequence == 3);
            driver.close(); driver.present(); assert(!ribbon(app));
            tools(driver); driver.tap(634, 50); driver.present();
            list = store.load_bookmarks(book, id, "epub-native-v1");
            assert(list.bookmarks.size() == 2 && list.bookmarks[1].sequence == 3 && list.bookmarks[1].anchor == marked);
            if (async) {
                // The commit runs on a worker: the tool page reports it, a repeated tap is ignored,
                // and the delayed progress save of a new page completes as well.
                driver.close(); driver.present();
                step(driver, Input::K3);  // The page of bookmark 2.
                driver.key(Input::K3);
                while (app.pending_frame()) app.complete_frame(true);
                assert(app.save_due());
                app.save_progress();
                assert(app.save_busy());
                const auto turned = app.saved_record();
                driver.tap(342, 500);
                while (app.pending_frame()) app.complete_frame(true);
                assert(app.view() == View::Tools);
                driver.tap(634, 50);
                assert(app.status() == "正在保存书签…" && app.save_busy());
                while (app.pending_frame()) app.complete_frame(true);
                driver.tap(634, 50);
                assert(!app.pending_frame() && app.save_busy());  // Ignored while the commit is pending.
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
                while (app.save_busy() || app.pending_frame()) {
                    app.poll_background();
                    while (app.pending_frame()) app.complete_frame(true);
                    assert(std::chrono::steady_clock::now() < deadline);
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
                assert(app.status() == "已加书签" && app.view() == View::Tools);
                list = store.load_bookmarks(book, id, "epub-native-v1");
                assert(list.bookmarks.size() == 3 && list.bookmarks[2].sequence == 4);
                const auto stored = store.load_progress(book, id, "epub-native-v1");
                assert(stored.record && app.saved_record() && stored.record->anchor == app.saved_record()->anchor);
                assert(!turned || turned->anchor != marked);
                driver.tap(634, 50); driver.present();
                assert(store.load_bookmarks(book, id, "epub-native-v1").bookmarks.size() == 2);
                // A queued commit finishes before the card is handed to USB.
                driver.tap(634, 50);
                while (app.pending_frame()) app.complete_frame(true);
                assert(app.save_busy() && app.prepare_usb_transfer() && !app.save_busy());
                assert(store.load_bookmarks(book, id, "epub-native-v1").bookmarks.size() == 3);
            }
        }
        if (async) {
            // An image opened while a commit is queued keeps its return page; the page then shows the ribbon.
            const auto viewer_root = card / "BookmarksViewer";
            fs::create_directories(viewer_root / "PicoPage");
            fs::create_directories(viewer_root / "Books");
            fs::copy_file(fixtures / "image-flow.epub", viewer_root / "Books/images.epub");
            ReaderApp app(viewer_root.string(), "bookmarks", true, true); Driver driver{app};
            assert(app.initialize()); driver.present(); opened(app, "Books/images.epub"); driver.present();
            assert(!ribbon(app));
            tools(driver); driver.tap(634, 50);
            while (app.pending_frame()) app.complete_frame(true);
            driver.tap(342, 500);
            while (app.pending_frame()) app.complete_frame(true);
            assert(app.view() == View::Reading && app.save_busy());
            const auto page_pixels = app.canvas().pixels();
            driver.hold(120, 500);
            assert(app.view() == View::Loading);
            driver.present(); driver.complete_content();
            assert(app.view() == View::Reading && app.canvas().pixels() != page_pixels && !app.save_busy());
            driver.tap(100, 50); driver.present();
            assert(app.view() == View::Reading && ribbon(app));
            FileStore viewer_store(viewer_root.string());
            assert(viewer_store.load_bookmarks("Books/images.epub", app.saved_record()->book_id, "epub-native-v1").bookmarks.size() == 1);
        }
        if (async) {
            // A ribbon change redraws an image page; its resumed patch preserves the ribbon and saved position.
            const auto image_root = card / "BookmarksProgressive";
            fs::create_directories(image_root / "Books");
            fs::copy_file(fixtures / "image-flow.epub", image_root / "Books/images.epub");
            ReaderApp app(image_root.string(), "bookmark-image", true, true); Driver driver{app};
            assert(app.initialize()); driver.present(); opened(app, "Books/images.epub");
            while (app.pending_frame()) app.complete_frame(true);
            app.save_progress();
            const auto save_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
            while (app.save_busy()) {
                app.poll_background(true);
                assert(std::chrono::steady_clock::now() < save_deadline);
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            const auto saved = app.saved_record();
            assert(saved && !ribbon(app) && app.content_busy());
            tools(driver); driver.tap(634, 50);
            while (app.pending_frame()) app.complete_frame(true);
            driver.close();
            while (app.pending_frame()) app.complete_frame(true);
            bool patched = false;
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
            while (app.save_busy() || app.content_busy() || app.pending_frame()) {
                app.poll_background();
                while (app.pending_frame()) {
                    if (app.pending_frame()->region) {
                        patched = true;
                        assert(app.pending_frame()->purpose == FramePurpose::Overlay && ribbon(app));
                    }
                    app.complete_frame(true);
                }
                assert(app.saved_record() == saved && std::chrono::steady_clock::now() < deadline);
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            assert(patched);
            assert(ribbon(app));
            assert(!app.save_due());
            FileStore image_store(image_root.string());
            const auto list = image_store.load_bookmarks("Books/images.epub", saved->book_id, "epub-native-v1");
            assert(list.bookmarks.size() == 1 && list.bookmarks.front().anchor == saved->anchor);
            const auto rendered = app.canvas().pixels();
            const auto decodes = graphics::image_decode_count();
            for (const bool marked : {false, true}) {
                tools(driver); driver.tap(634, 50); driver.present();
                driver.close(); driver.present();
                assert(ribbon(app) == marked && app.saved_record() == saved && !app.content_busy());
                assert(graphics::image_decode_count() == decodes);
                // Only the ribbon and the notice/footer may change; every body pixel stays rendered.
                assert(std::equal(rendered.begin() + 64 * 342, rendered.begin() + 1128 * 342,
                                  app.canvas().pixels().begin() + 64 * 342));
            }
            ContentJob::Request request;
            request.kind = ContentJob::Kind::Open;
            request.root = image_root.string(); request.path = (image_root / "Books/images.epub").string();
            request.relative_path = "Books/images.epub"; request.content_revision = "epub-native-v1";
            ContentJob job(std::move(request));
            assert(job.start()); job.wait();
            assert(job.result.error.empty() && job.result.bookmarks.status == StoreStatus::Ok);
            assert(job.result.bookmarks.bookmarks.size() == 1 && job.result.bookmarks.bookmarks.front().anchor == saved->anchor);
        }
        if (async) {
            // A commit that fails while it is drained for a card handoff refuses the handoff and says why.
            ReaderApp app(root.string(), "bookmarks", async, async); Driver driver{app};
            assert(app.initialize()); driver.present(); opened(app, book); driver.present();
            const auto before = read(marks);
            tools(driver); driver.tap(634, 50);
            while (app.pending_frame()) app.complete_frame(true);
            assert(app.save_busy());
            fs::remove(marks); fs::create_directory(marks);
            assert(!app.prepare_usb_transfer() && !app.save_busy());
            assert(app.status() == "书签未删除，请检查 TF 卡后重试" || app.status() == "书签未保存，请检查 TF 卡后重试" ||
                   app.status() == "书签文件已在别处修改，已重新读取，请核对当前书签");
            fs::remove(marks); write(marks, before);
            assert(app.prepare_usb_transfer());
        }
        if (async) {
            // A commit failing after the user moved to a page without a notice line shows an error page.
            ReaderApp app(root.string(), "bookmarks", async, async); Driver driver{app};
            assert(app.initialize()); driver.present(); opened(app, book); driver.present();
            const auto before = read(marks);
            tools(driver); driver.tap(634, 50);
            while (app.pending_frame()) app.complete_frame(true);
            fs::remove(marks); fs::create_directory(marks);
            driver.tap(205, 1140);
            while (app.pending_frame()) app.complete_frame(true);
            assert(app.view() == View::Error && !app.save_busy());
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
            while (app.view() != View::Error) {
                app.poll_background();
                while (app.pending_frame()) app.complete_frame(true);
                assert(std::chrono::steady_clock::now() < deadline);
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            assert(app.status().find("书签") != std::string::npos);
            fs::remove(marks); write(marks, before);
        }
        if (async) {
            // A settings write on another panel waits for a queued commit instead of running beside it.
            ReaderApp app(root.string(), "bookmarks", async, async); Driver driver{app};
            assert(app.initialize()); driver.present(); opened(app, book); driver.present();
            const auto count = store.load_bookmarks(book, id, "epub-native-v1").bookmarks.size();
            const auto book_settings = root / (book + ".picopage/settings.toml");
            const bool settings_existed = fs::exists(book_settings);
            const auto settings_before = read(book_settings);
            tools(driver); driver.tap(634, 50);
            while (app.pending_frame()) app.complete_frame(true);
            driver.tap(479, 1140);
            while (app.pending_frame()) app.complete_frame(true);
            assert(app.view() == View::Refresh && !app.save_busy());
            driver.tap(300, 660);
            assert(!app.save_busy() && store.load_bookmarks(book, id, "epub-native-v1").bookmarks.size() != count);
            while (app.pending_frame()) app.complete_frame(true);
            if (settings_existed) write(book_settings, settings_before); else fs::remove(book_settings);
        }
        if (async) {
            // Entering a card-reading tab completes the commit before it reads configuration.
            const auto marks_before = read(marks), backup_before = read(marks.string() + ".bak");
            const std::pair<int, View> tabs[] = {{205, View::Typography}, {479, View::Refresh}, {342, View::Sync}};
            for (const auto& tab : tabs) {
                ReaderApp app(root.string(), "bookmark-panels", true, true); Driver driver{app};
                assert(app.initialize()); driver.present(); opened(app, book); driver.present();
                tools(driver); driver.tap(634, 50);
                while (app.pending_frame()) app.complete_frame(true);
                assert(app.save_busy());
                driver.tap(tab.first, 1140);
                assert(!app.save_busy() && app.view() == tab.second);
                driver.present();
            }
            write(marks, marks_before); write(marks.string() + ".bak", backup_before);
            // A pending failure must be shown before either tab can save a success notice.
            for (const int tab : {205, 479}) {
                ReaderApp app(root.string(), "bookmark-panel-failure", true, true); Driver driver{app};
                assert(app.initialize()); driver.present(); opened(app, book); driver.present();
                const auto settings_path = root / (book + ".picopage/settings.toml");
                const auto settings_before = read(settings_path), marks_before = read(marks);
                const auto temporary = fs::path(marks.string() + ".tmp");
                fs::remove_all(temporary); fs::create_directory(temporary);
                tools(driver); driver.tap(634, 50);
                while (app.pending_frame()) app.complete_frame(true);
                driver.tap(tab, 1140);
                assert(app.view() == View::Error && !app.save_busy());
                driver.present();
                assert(app.status().find("书签未") == 0 && read(settings_path) == settings_before && read(marks) == marks_before);
                fs::remove_all(temporary);
            }
            // TOC and bookmark-list views both expose a normally polled commit failure.
            for (const bool show_bookmarks : {false, true}) {
                ReaderApp app(root.string(), "bookmark-contents-failure", true, true); Driver driver{app};
                assert(app.initialize()); driver.present(); opened(app, book); driver.present();
                const auto temporary = fs::path(marks.string() + ".tmp");
                fs::remove_all(temporary); fs::create_directory(temporary);
                tools(driver); driver.tap(634, 50);
                while (app.pending_frame()) app.complete_frame(true);
                driver.tap(68, 1140);
                while (app.pending_frame()) app.complete_frame(true);
                assert(app.view() == View::Contents);
                if (show_bookmarks) {
                    driver.tap(500, 486);
                    while (app.pending_frame()) app.complete_frame(true);
                }
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
                while (app.view() != View::Error) {
                    app.poll_background();
                    while (app.pending_frame()) app.complete_frame(true);
                    assert(std::chrono::steady_clock::now() < deadline);
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
                assert(app.status().find("书签未") == 0);
                fs::remove_all(temporary);
            }
        }
        {
            // A reload can already contain the intended add or delete; never instruct another toggle.
            const auto external_root = card / (async ? "BookmarkExternalAsync" : "BookmarkExternalSync");
            fs::create_directories(external_root / "Books");
            fs::copy_file(fixtures / "long-chapter.epub", external_root / book);
            FileStore external(external_root.string());
            for (const bool adding : {true, false}) {
                ReaderApp app(external_root.string(), "bookmark-external", async, async); Driver driver{app};
                assert(app.initialize()); driver.present(); opened(app, book); driver.present();
                const auto identity = app.saved_record()->book_id;
                auto base = external.load_bookmarks(book, identity, "epub-native-v1");
                std::vector<Bookmark> replacement;
                auto next = base.next_sequence;
                if (adding) {
                    Bookmark bookmark; bookmark.sequence = next++; bookmark.anchor = app.saved_record()->anchor;
                    replacement.push_back(bookmark);
                }
                BookmarkList committed;
                assert(external.save_bookmarks(book, identity, "epub-native-v1", base, replacement, next, committed) == StoreStatus::Ok);
                tools(driver); driver.tap(634, 50); driver.present();
                assert(app.status() == "书签文件已在别处修改，已重新读取，请核对当前书签");
                assert(external.load_bookmarks(book, identity, "epub-native-v1").bookmarks.size() == (adding ? 1u : 0u));
                driver.close(); driver.present();
                assert(ribbon(app) == adding);
            }
        }
        if (async) {
            // A failure drained by the power menu is reported by the next handoff, unless a retry succeeds.
            ReaderApp app(root.string(), "bookmarks", async, async); Driver driver{app};
            assert(app.initialize()); driver.present(); opened(app, book); driver.present();
            const auto before = read(marks);
            const auto temporary = fs::path(marks.string() + ".tmp");
            fs::remove_all(temporary); fs::create_directory(temporary);
            tools(driver); driver.tap(634, 50);
            while (app.pending_frame()) app.complete_frame(true);
            assert(app.show_power_menu()); driver.present();
            driver.tap(100, 1100); driver.present();
            fs::remove_all(temporary);
            assert(read(marks) == before && app.view() == View::Tools);
            driver.tap(634, 50); driver.present();
            assert(app.status() == "已删除本页书签" || app.status() == "已加书签");
            assert(app.prepare_usb_transfer());
        }
        {
            // A different font size lays out other pages; the bookmark still opens its paragraph.
            write(root / (book + ".picopage/settings.toml"), "schema_version = 1\n[reader]\nfont_size_px = 30\n");
            ReaderApp app(root.string(), "bookmarks", async, async); Driver driver{app};
            assert(app.initialize()); driver.present(); opened(app, book); driver.present();
            for (int i = 0; i < 6; ++i) step(driver, Input::K3);
            bookmark_list(driver);
            driver.tap(300, 578 + 112);
            if (app.pending_frame()) driver.present();
            driver.complete_content();
            assert(app.view() == View::Reading && app.saved_record()->anchor == marked && ribbon(app));
        }
        const auto original = read(marks);
        {
            // A damaged file is kept and blocks changes; reading progress is still saved.
            write(marks, "{damaged");
            ReaderApp app(root.string(), "bookmarks", async, async); Driver driver{app};
            assert(app.initialize()); driver.present(); opened(app, book);
            assert(app.status() == "书签文件无法读取，已保留原文件；阅读进度不受影响");
            driver.present();
            bookmark_list(driver);
            driver.tap(300, 578); assert(!app.pending_frame() && app.view() == View::Contents);
            driver.close(); driver.present();
            tools(driver); driver.tap(634, 50); driver.present();
            assert(app.view() == View::Tools && read(marks) == "{damaged");
            driver.close(); driver.present();
            const auto before = read(progress);
            step(driver, Input::K3);
            assert(read(progress) != before && read(marks) == "{damaged");
        }
        {
            // Bookmarks of other content are withheld; the first new bookmark archives that file.
            auto foreign = original;
            foreign.replace(foreign.find(id), id.size(), std::string(64, 'f'));
            write(marks, foreign);
            ReaderApp app(root.string(), "bookmarks", async, async); Driver driver{app};
            assert(app.initialize()); driver.present(); opened(app, book);
            assert(app.status() == "书签与当前原书不符，未使用；原文件已保留");
            driver.present();
            bookmark_list(driver);
            driver.tap(300, 578); assert(!app.pending_frame() && app.view() == View::Contents);
            driver.close(); driver.present();
            assert(!ribbon(app));
            tools(driver); driver.tap(634, 50); driver.present();
            assert(read(root / (book + ".picopage/bookmarks.ffffffff.1.json")) == foreign);
            const auto list = store.load_bookmarks(book, id, "epub-native-v1");
            assert(list.status == StoreStatus::Ok && list.bookmarks.size() == 1 && list.bookmarks[0].sequence == 1);
        }
    }
    fs::remove_all(card);
    std::cout << "reader app display, progress protection, typography, cover and stale network checks: passed\n";
}
