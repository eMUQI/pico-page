#include "pico_page/app.h"
#include "pico_page/document.h"
#include "pico_page/store.h"

#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;
using namespace pico_page;

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
    assert(app.pending_frame() && app.pending_frame()->purpose == FramePurpose::Body);
}
struct Driver {
    ReaderApp& app;
    std::uint64_t sequence = 0;
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
    // Mirrors the firmware loop: every frame queued by deferred work, then the idle-time save.
    // A contact that began while the previous frame was still in flight.
    void queued(Input key, bool touch = false, int x = 0, int y = 0) {
        ++sequence;
        app.input({{sequence, Phase::Begin, key}, touch, x, y, true});
        app.input({{sequence, Phase::End, key}, touch, x, y, true});
    }
    void present() {
        assert(app.pending_frame());
        while (app.pending_frame()) app.complete_frame(true);
        if (app.save_due()) app.save_progress();
    }
    void close() {
        if (app.view() == View::Tools) tap(342, 500);
        else if (app.view() == View::Typography) tap(628, 700);
        else if (app.view() == View::Contents) tap(628, 150);
        else if (app.view() == View::Refresh) tap(628, 550);
        else if (app.view() == View::Sync) {
            tap(628, 550);
            if (!app.pending_frame()) tap(100, 950);
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
        const auto token = app.pending_frame()->token;
        key(Input::K2);
        assert(app.pending_frame()->token == token);
        present();
        assert(app.view() == view && app.canvas().pixels() == pixels && app.saved_record() == record);
        assert(!app.pending_frame());
    }
    void sync_view() {
        tap(342, 500); assert(app.view() == View::Tools); present();
        tap(342, 1140); assert(app.view() == View::Sync); present();
    }
    NetworkOperation check_remote() {
        tap(342, 960); present();
        auto request = app.take_network_request();
        assert(request && request->request.method == "GET");
        return *request;
    }
    void typography() {
        tap(342, 500); assert(app.view() == View::Tools); present(); refresh();
        tap(205, 1140); assert(app.view() == View::Typography); present();
    }
};

int main(int argc, char** argv) {
    assert(argc == 3);
    char temporary[] = "/tmp/pico-app-XXXXXX";
    const char* directory = mkdtemp(temporary); assert(directory);
    const fs::path card(directory), fixtures(argv[1]);
    fs::create_directories(card / "Books");
    fs::create_directories(card / "PicoPage");
    fs::copy_file(fixtures / "valid.epub", card / "Books/book.epub");
    fs::copy_file(fixtures / "nested-toc.epub", card / "Books/nested.epub");
    fs::copy_file(fixtures / "image-cover.epub", card / "Books/cover.epub");
    fs::copy_file(fixtures / "image-flow.epub", card / "Books/images.epub");
    const auto progress = card / "Books/book.epub.picopage/progress.json";
    const auto settings = card / "Books/book.epub.picopage/settings.toml";
    ReadingRecord confirmed;
    {
        ReaderApp app(card.string()); Driver driver{app};
        assert(app.initialize()); driver.present();
        driver.tap(240, 482); driver.present();
        // Undecoded covers first appear as titles; the cover replaces the title in a second frame.
        driver.tap(610, 384);
        assert(app.canvas().pixel(270, 490) == 15);
        app.complete_frame(true);
        assert(app.pending_frame() && app.view() == View::Library);
        assert(app.canvas().pixel(270, 490) == 0 && app.canvas().pixel(270, 610) == 5);
        driver.present();
        driver.refresh();
        const auto original_cover = read(card / "Books/cover.epub");
        write(card / "Books/cover.epub", "broken");
        driver.tap(520, 384); driver.present(); driver.tap(610, 384); driver.present();
        assert(app.canvas().pixel(270, 490) == 15);
        write(card / "Books/cover.epub", original_cover);
        driver.tap(520, 384); driver.present(); driver.tap(610, 384); driver.present();
        assert(app.canvas().pixel(270, 490) == 0 && app.canvas().pixel(270, 610) == 5);
        driver.tap(520, 384); driver.present();
        assert(!app.open_book("Books/book.epub") && app.view() == View::Error);
        driver.present(); assert(!app.saved_record() && !fs::exists(progress));
        driver.close(); driver.present();
        assert(app.view() == View::Library);
        opened(app, "Books/cover.epub");
        assert(app.canvas().pixel(100, 400) == 0 && app.canvas().pixel(100, 800) == 5);
        driver.present(); const auto cover = *app.saved_record();
        driver.key(Input::K3); assert(app.view() == View::Error); driver.present();
        assert(*app.saved_record() == cover);
        driver.close(); driver.present();
        assert(app.view() == View::Reading && *app.saved_record() == cover);
    }
    {
        // A cached thumbnail survives a restart; an entry that fails validation is rebuilt from the EPUB.
        constexpr std::size_t thumbnail_bytes = 82 * 184;
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
            driver.tap(240, 482); driver.present();
            driver.tap(610, 384); driver.present();
            return app.canvas().pixel(x, y);
        };
        assert(grid_pixel(270, 490) == 15);
        write(cached, "stale");
        assert(grid_pixel(270, 490) == 0 && grid_pixel(270, 610) == 5);
        assert(read(cached).size() > thumbnail_bytes);
    }
    fs::create_directories(card / "PicoPage/fonts");
    fs::copy_file(argv[2], card / "PicoPage/fonts/LXGWWenKai-Regular.ttf");
    {
        ReaderApp app(card.string()); Driver driver{app};
        assert(app.initialize()); driver.present(); driver.refresh();
        opened(app, "Books/book.epub"); assert(!app.saved_record()); driver.present();
        assert(app.saved_record()); const auto first = *app.saved_record();
        driver.key(Input::K3); assert(app.pending_frame());
        const auto failed_anchor = app.pending_frame()->anchor;
        assert(failed_anchor != first.anchor);
        app.complete_frame(false);
        assert(app.saved_record() && *app.saved_record() == first && app.view() == View::Error);
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
        driver.tap(580, 948); driver.present();
        assert(app.canvas().pixels() != applied_preview);
        driver.refresh(); assert(!fs::exists(settings));
        driver.close(); assert(app.view() == View::Reading); driver.present();
        assert(!fs::exists(settings) && *app.saved_record() == confirmed);
        driver.tap(342, 500); driver.present(); driver.tap(68, 1150); driver.present();
        assert(app.view() == View::Contents); driver.refresh();
        driver.tap(205, 150); driver.present(); assert(app.view() == View::Typography);
        driver.close(); driver.present();
        driver.tap(342, 500); driver.present(); driver.tap(60, 50);
        assert(app.pending_frame() && app.pending_frame()->refresh == RefreshIntent::FullClean);
        driver.present();
        assert(app.view() == View::Library); driver.refresh();
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
        driver.typography(); driver.tap(580, 948); driver.present(); driver.tap(500, 1130);
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
        ReaderApp app(card.string()); Driver driver{app};
        assert(app.initialize()); driver.present(); opened(app, "Books/cover.epub"); driver.present();
        assert(app.saved_record() && app.saved_record()->anchor.node == "/html[1]/body[1]/img[1]" && app.saved_record()->anchor.offset == 0);
        const auto saved = *app.saved_record();
        assert(app.prepare_sleep()); driver.present();
        assert(app.view() == View::Sleeping && *app.saved_record() == saved);
        assert(app.canvas().pixel(100, 400) == 0 && app.canvas().pixel(100, 800) == 5);
        app.wake(); driver.present(); assert(*app.saved_record() == saved);
    }
    {
        fs::copy_file(fixtures / "long-chapter.epub", card / "Books/long.epub");
        ReaderApp app(card.string()); Driver driver{app};
        assert(app.initialize()); driver.present();
        // Books sorts book, cover, images, long: the fourth list row opens after a status frame.
        driver.tap(240, 482); driver.present();
        driver.tap(240, 432 + 3 * 126 + 40);
        assert(app.view() == View::Library && app.pending_frame()->purpose == FramePurpose::Overlay && !app.saved_record());
        app.complete_frame(true);
        assert(app.view() == View::Reading && app.pending_frame()->purpose == FramePurpose::Body);
        driver.present();
        driver.tap(342, 500); driver.present();
        driver.tap(68, 1140); assert(app.view() == View::Contents); driver.present();
        driver.tap(300, 204 + 88 + 44); assert(app.view() == View::Reading); driver.present();
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
        assert(app.view() == View::Tools && app.saved_record()->local_revision == second.local_revision + 1);
        driver.present(); driver.close(); driver.present();
        driver.key(Input::K3); app.complete_frame(true);
        const auto unsaved = app.saved_record()->local_revision;
        assert(app.prepare_sleep() && app.saved_record()->local_revision == unsaved + 1);
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
    ReadingRecord image_record, after_image;
    {
        ReaderApp app(card.string()); Driver driver{app};
        assert(app.initialize()); driver.present(); opened(app, "Books/images.epub"); driver.present();
        const auto before_image = *app.saved_record();
        assert(before_image.anchor.node.find("/p[1]/") != std::string::npos);
        driver.key(Input::K3);
        assert(app.pending_frame()->anchor.node == "/html[1]/body[1]/img[1]");
        assert(app.canvas().pixel(100, 400) == 0 && app.canvas().pixel(100, 800) == 5);
        assert(*app.saved_record() == before_image);
        driver.present(); image_record = *app.saved_record();
        const auto image_pixels = app.canvas().pixels();
        driver.key(Input::K2);
        assert(app.pending_frame()->purpose == FramePurpose::ManualRefresh && app.pending_frame()->refresh == RefreshIntent::FullClean);
        driver.present(); assert(*app.saved_record() == image_record && app.canvas().pixels() == image_pixels);
        driver.typography();
        assert(app.canvas().pixel(100, 100) == 0);
        driver.tap(580, 948); driver.present();
        assert(app.canvas().pixel(100, 100) == 0 && *app.saved_record() == image_record);
        driver.close(); driver.present(); assert(*app.saved_record() == image_record);
        driver.key(Input::K3); driver.present(); after_image = *app.saved_record();
        assert(after_image.anchor.node.find("/p[2]/") != std::string::npos);
        driver.key(Input::K3);
        assert(app.pending_frame()->anchor.node == "/html[1]/body[1]/img[2]");
        assert(app.canvas().pixel(36, 76) == 1);
        driver.present(); const auto missing_image = *app.saved_record();
        driver.key(Input::K3); driver.present(); const auto trailing_text = *app.saved_record();
        assert(trailing_text.anchor.node.find("/p[3]/") != std::string::npos);
        driver.key(Input::K3); driver.present();
        assert(app.saved_record()->anchor.resource != trailing_text.anchor.resource);
        driver.key(Input::K1); driver.present(); assert(app.saved_record()->anchor == trailing_text.anchor);
        driver.key(Input::K1); driver.present(); assert(app.saved_record()->anchor == missing_image.anchor);
        driver.key(Input::K1); driver.present(); assert(app.saved_record()->anchor == after_image.anchor);
    }
    {
        ReaderApp app(card.string()); Driver driver{app};
        assert(app.initialize()); driver.present(); opened(app, "Books/images.epub"); driver.present();
        assert(app.saved_record()->anchor == after_image.anchor);
        driver.key(Input::K1); driver.present();
        assert(app.saved_record()->anchor == image_record.anchor && app.canvas().pixel(100, 400) == 0);
        image_record = *app.saved_record();
    }
    {
        ReaderApp app(card.string()); Driver driver{app};
        assert(app.initialize()); driver.present(); opened(app, "Books/images.epub"); driver.present();
        assert(*app.saved_record() == image_record && app.canvas().pixel(100, 800) == 5);
        driver.key(Input::K1); driver.present();
        assert(app.saved_record()->anchor.node.find("/p[1]/") != std::string::npos);
    }
    const std::string sync_config = "schema_version = 1\ndom_version = 20260812\nendpoint = \"https://example.invalid\"\nusername = \"test-user\"\nuserkey = \"test-only-placeholder\"\ndevice_id = \"app-test\"\n";
    write(card / "PicoPage/sync.toml", sync_config);
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
        app.complete_network(cancelled.id, 200, remote, "");
        assert(!app.pending_frame() && !app.take_network_request() && *app.saved_record() == before);

        driver.sync_view(); const auto candidate = driver.check_remote();
        app.complete_network(candidate.id, 200, remote, ""); driver.present();
        auto switched_account = sync_config;
        switched_account.replace(switched_account.find("test-user"), 9, "other-user");
        write(card / "PicoPage/sync.toml", switched_account);
        driver.tap(500, 950);
        assert(app.view() == View::Sync && app.pending_frame() && app.pending_frame()->purpose == FramePurpose::Overlay);
        driver.present(); assert(*app.saved_record() == before && !app.take_network_request());
        driver.close(); driver.present();
        write(card / "PicoPage/sync.toml", sync_config);

        driver.sync_view(); const auto stale = driver.check_remote();
        opened(app, "Books/cover.epub"); driver.present(); const auto cover = *app.saved_record();
        driver.sync_view(); const auto current = driver.check_remote();
        assert(current.id != stale.id);
        app.complete_network(stale.id, 200, remote, "");
        assert(!app.pending_frame() && !app.take_network_request() && *app.saved_record() == cover);
        app.complete_network(current.id, 200, "{}", ""); driver.present();
        assert(*app.saved_record() == cover && !app.take_network_request());
        driver.tap(342, 1075); assert(app.view() == View::Confirmation); driver.present();
        driver.refresh(); assert(!app.take_network_request());
        driver.close(); driver.present();
        assert(app.view() == View::Reading && !app.take_network_request() && *app.saved_record() == cover);
        opened(app, "Books/book.epub"); driver.present();
        const auto sync_saved = *app.saved_record();
        driver.sync_view(); const auto no_record = driver.check_remote();
        app.complete_network(no_record.id, 200, "{}", ""); driver.present();
        driver.tap(342, 1075); driver.present(); driver.tap(500, 1120); driver.present();
        auto upload = app.take_network_request(); assert(upload && upload->request.method == "PUT");
        driver.close(); driver.present();
        assert(upload->cancelled->load());
        app.complete_network(upload->id, 200, "{}", "");
        assert(!app.pending_frame() && *app.saved_record() == sync_saved);
        driver.sync_view(); const auto recheck = driver.check_remote();
        app.complete_network(recheck.id, 401, "{}", ""); driver.present(); driver.refresh();
        assert(!app.take_network_request() && *app.saved_record() == sync_saved);
        const auto retry = driver.check_remote();
        app.complete_network(retry.id, 200, "{}", ""); driver.present();
        driver.tap(342, 1075); driver.present(); driver.tap(500, 1120); driver.present();
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
        driver.typography(); driver.tap(580, 948); driver.present();
        driver.close(); driver.present();
        assert(read(settings) == external && *app.saved_record() == saved);
        driver.typography(); driver.tap(580, 948); driver.present(); driver.tap(500, 1130); driver.present();
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

        driver.typography(); driver.tap(566, 1038); driver.present();
        driver.tap(580, 948); driver.present(); driver.tap(500, 1130); driver.present();
        actual = FileStore(card.string()).load_settings("Books/book.epub").settings;
        const auto global = FileStore(card.string()).load_settings().settings;
        assert(actual.font_size_px == global.font_size_px + 2 && actual.line_height == global.line_height && actual.margin_x_px == global.margin_x_px);
        assert(*app.saved_record() == saved);

        const auto applied_pixels = app.canvas().pixels();
        const auto book_overrides = read(settings);
        driver.typography(); driver.tap(580, 948); driver.present(); driver.tap(180, 1130);
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
        driver.typography(); driver.tap(100, 948); driver.present(); driver.tap(180, 1130);
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
        app.show_usb_transfer(true, ""); driver.present();
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
        ReaderApp app(card.string()); Driver driver{app};
        assert(app.initialize()); driver.present(); opened(app, "Books/nested.epub"); driver.present();
        const auto saved = *app.saved_record();
        driver.tap(342, 500); driver.present(); driver.tap(68, 1150); driver.present();
        const auto collapsed = app.canvas().pixels();
        driver.tap(615, 248); driver.present();
        assert(app.view() == View::Contents && app.canvas().pixels() != collapsed && *app.saved_record() == saved);
        driver.refresh();
        driver.tap(615, 248); driver.present();
        assert(app.canvas().pixels() == collapsed && *app.saved_record() == saved);
        driver.tap(615, 248); driver.present();
        driver.tap(200, 340); driver.present();
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
        assert(FileStore(card.string()).load_settings("Books/book.epub").settings.balanced_full_refresh_pages == 9);
        assert(*app.saved_record() == saved);
        driver.close(); driver.present();
        driver.tap(340, 500); driver.present(); driver.tap(50, 50); driver.present();
        driver.tap(100, 480); driver.present();
        driver.tap(600, 480); driver.present();
        assert(app.view() == View::BookDetails && *app.saved_record() == saved);
        driver.refresh(); driver.close(); driver.present();
        assert(app.view() == View::Library && *app.saved_record() == saved);
        driver.tap(610, 384); driver.present();
        driver.tap(100, 680); driver.present();
        assert(app.view() == View::BookDetails && *app.saved_record() == saved);
        driver.refresh(); driver.close(); driver.present();
        driver.tap(600, 50); driver.present();
        driver.tap(340, 710); driver.present();
        assert(app.view() == View::PowerSettings); driver.refresh();
        const auto global_before = read(card / "PicoPage/settings.toml");
        driver.tap(340, 390); driver.present(); driver.tap(600, 750); driver.present();
        driver.refresh(); driver.tap(150, 1120); driver.present();
        assert(read(card / "PicoPage/settings.toml") == global_before && app.auto_sleep_minutes() == 10);
        driver.tap(340, 710); driver.present(); driver.tap(340, 390); driver.present();
        driver.tap(600, 750); driver.present(); driver.tap(500, 1120); driver.present();
        assert(app.auto_sleep_minutes() == 15 && FileStore(card.string()).load_settings().settings.sleep_screen == "reading");
        assert(app.prepare_sleep());
        assert(app.view() == View::Sleeping && app.pending_frame()->refresh == RefreshIntent::FullClean);
        driver.present(); assert(*app.saved_record() == saved);
        driver.key(Input::K3); assert(!app.pending_frame());
        app.wake(); driver.present(); assert(*app.saved_record() == saved);
        driver.tap(340, 500); driver.present(); driver.tap(50, 50); driver.present();
        driver.tap(600, 50); driver.present(); driver.tap(340, 840); driver.present();
        assert(app.view() == View::StorageSettings); driver.refresh();
        write(card / "PicoPage/cache/layout", "cache");
        const auto book_before = read(card / "Books/book.epub");
        const auto progress_before = read(progress);
        driver.tap(340, 1080); driver.present();
        assert(!fs::exists(card / "PicoPage/cache/layout") && read(progress) == progress_before);
        assert(read(card / "Books/book.epub") == book_before && *app.saved_record() == saved);
        driver.close(); driver.present(); driver.tap(340, 710); driver.present();
        driver.tap(340, 250); driver.present();
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
        assert(app.prepare_sleep()); driver.present();
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
        driver.tap(240, 480); driver.present();
        driver.tap(610, 480); driver.present();
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
        driver.tap(342, 500); driver.present(); driver.tap(60, 50); driver.present(); driver.tap(610, 480); driver.present();
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
            driver.tap(588, 948); driver.present(); driver.tap(500, 1136); driver.present();
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
        driver.tap(300, 840); driver.present(); assert(app.view() == View::StorageSettings); driver.refresh();
        driver.tap(60, 50); driver.present(); assert(app.view() == View::Settings);
        driver.tap(300, 580); driver.present(); assert(app.view() == View::InputSettings);
        assert(FileStore(card.string()).load_settings().settings.library_view == "list");
        driver.tap(300, 150); driver.present();
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
            driver.tap(300, 336); driver.present();
            assert(app.view() == View::Reading);
            late = app.saved_record()->anchor;
            late_page = app.book_pages()->page;
            assert(late.node.find("/p[4501]/") != std::string::npos && late_page > total / 2);
        }
        {
            ReaderApp app(card.string()); Driver driver{app};
            assert(app.initialize()); driver.present(); opened(app, "Parts/long.epub"); driver.present();
            assert(app.saved_record()->anchor == late && app.book_pages() && app.book_pages()->page == late_page);
            driver.key(Input::K1); driver.present();
            assert(app.view() == View::Reading && app.book_pages()->page + 1 >= late_page);
        }
    }
    {
        // Phone setup: both entry points only request a session, the page keeps K2 and ignores
        // every control but its own, and a closed session returns to the settings list.
        ReaderApp app(card.string()); Driver driver{app};
        assert(app.initialize()); driver.present();
        driver.tap(600, 50); driver.present(); assert(app.view() == View::Settings);
        driver.tap(300, 180); driver.present(); assert(app.view() == View::Import);
        driver.tap(342, 750);
        assert(!app.pending_frame() && app.view() == View::Import);
        assert(app.take_portal_action() == PortalAction::Start && app.take_portal_action() == PortalAction::None);
        app.show_portal({PortalPhase::Closed, "", "", "", "无法开启手机配置，请稍后重试。"}); driver.present();
        assert(app.view() == View::Settings && app.status() == "无法开启手机配置，请稍后重试。");
        driver.tap(300, 310); driver.present(); assert(app.view() == View::SyncSettings);
        driver.tap(342, 800);
        assert(app.take_portal_action() == PortalAction::Start);
        assert(app.prepare_portal()); driver.present(); assert(app.view() == View::Portal);
        const auto starting = app.canvas().pixels();
        PortalStatus status{PortalPhase::Waiting, "PicoPage-TEST", "abcd2345", "http://192.168.4.1", ""};
        app.show_portal(status);
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
        status.phase = PortalPhase::JoiningWifi; status.detail = "home";
        app.show_portal(status);
        assert(app.pending_frame() && app.pending_frame()->refresh == RefreshIntent::Content);
        driver.present();
        status.phase = PortalPhase::Failed; status.detail = "无法连接“home”（驱动原因 8）。设备没有保存这次的设置。";
        app.show_portal(status); driver.present();
        const auto failed = app.canvas().pixels();
        status.phase = PortalPhase::Saved; status.detail = "已保存 Wi-Fi “home”。";
        app.show_portal(status); driver.present();
        assert(app.canvas().pixels() != failed && app.view() == View::Portal); driver.refresh();
        driver.tap(342, 1120);
        assert(app.take_portal_action() == PortalAction::Stop); driver.present();
        driver.tap(342, 1120);
        assert(app.take_portal_action() == PortalAction::None && !app.pending_frame());
        app.show_portal({PortalPhase::Closed, "", "", "", ""}); driver.present();
        assert(app.view() == View::Settings && app.status().empty());
        app.show_portal(status);
        assert(!app.pending_frame() && app.view() == View::Settings);
        // With a book open the session starts from saved progress and leaves it unchanged.
        opened(app, "Books/book.epub"); driver.present();
        driver.key(Input::K3); driver.present();
        assert(app.prepare_portal()); driver.present();
        assert(app.view() == View::Portal && !app.save_due());
        const auto saved = *app.saved_record();
        app.show_portal({PortalPhase::Closed, "", "", "", ""}); driver.present();
        assert(app.view() == View::Settings && *app.saved_record() == saved);
    }
    fs::remove_all(card);
    std::cout << "reader app display, progress protection, typography, cover and stale network checks: passed\n";
}
