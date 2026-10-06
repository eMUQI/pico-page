#include "pico_page/app.h"
#include "pico_page/store.h"

#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <unistd.h>

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
    void present() { assert(app.pending_frame()); app.complete_frame(true); }
    void sync_view() {
        tap(342, 500); assert(app.view() == View::Tools); present();
        tap(342, 1140); assert(app.view() == View::Sync); present();
    }
    NetworkOperation check_remote() {
        tap(342, 620); present();
        auto request = app.take_network_request();
        assert(request && request->request.method == "GET");
        return *request;
    }
    void typography() {
        tap(342, 500); assert(app.view() == View::Tools); present();
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
    fs::copy_file(fixtures / "image-cover.epub", card / "Books/cover.epub");
    fs::copy_file(fixtures / "image-flow.epub", card / "Books/images.epub");
    const auto progress = card / "Books/book.epub.picopage/progress.json";
    const auto settings = card / "Books/book.epub.picopage/settings.toml";
    ReadingRecord confirmed;
    {
        ReaderApp app(card.string()); Driver driver{app};
        assert(app.initialize()); driver.present();
        assert(!app.open_book("Books/book.epub") && app.view() == View::Error);
        driver.present(); assert(!app.saved_record() && !fs::exists(progress));
        driver.key(Input::K2); driver.present();
        assert(app.view() == View::Library);
        opened(app, "Books/cover.epub");
        assert(app.canvas().pixel(100, 400) == 0 && app.canvas().pixel(100, 800) == 5);
        driver.present(); const auto cover = *app.saved_record();
        driver.key(Input::K3); assert(app.view() == View::Error); driver.present();
        assert(*app.saved_record() == cover);
        driver.key(Input::K2); driver.present();
        assert(app.view() == View::Reading && *app.saved_record() == cover);
    }
    fs::create_directories(card / "PicoPage/fonts");
    fs::copy_file(argv[2], card / "PicoPage/fonts/LXGWWenKai-Regular.ttf");
    {
        ReaderApp app(card.string()); Driver driver{app};
        assert(app.initialize()); driver.present();
        opened(app, "Books/book.epub"); assert(!app.saved_record()); driver.present();
        assert(app.saved_record()); const auto first = *app.saved_record();
        driver.key(Input::K3); assert(app.pending_frame());
        const auto failed_anchor = app.pending_frame()->anchor;
        assert(failed_anchor != first.anchor);
        app.complete_frame(false);
        assert(app.saved_record() && *app.saved_record() == first && app.view() == View::Error);
        driver.present();
        driver.key(Input::K3); assert(!app.pending_frame() && *app.saved_record() == first);
        driver.key(Input::K2); assert(app.pending_frame() && app.pending_frame()->anchor == first.anchor); driver.present();
        driver.key(Input::K3); assert(app.pending_frame() && app.pending_frame()->anchor == failed_anchor); driver.present();
        confirmed = *app.saved_record(); const auto before_refresh = read(progress);
        driver.key(Input::K2);
        assert(app.pending_frame() && app.pending_frame()->purpose == FramePurpose::ManualRefresh && app.pending_frame()->refresh == RefreshIntent::FullClean);
        driver.present(); assert(*app.saved_record() == confirmed && read(progress) == before_refresh);
        driver.typography(); driver.tap(580, 948); driver.present();
        driver.key(Input::K2); assert(app.view() == View::Reading); driver.present();
        assert(!fs::exists(settings) && *app.saved_record() == confirmed);
    }
    {
        ReaderApp app(card.string()); Driver driver{app};
        assert(app.initialize()); driver.present(); opened(app, "Books/book.epub");
        assert(app.pending_frame()->anchor == confirmed.anchor);
        app.complete_frame(false); driver.present();
        assert(*app.saved_record() == confirmed);
        driver.key(Input::K2);
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
        driver.present(); driver.key(Input::K2); driver.present();
        assert(read(progress) == invalid);
    }
    write(progress, valid_progress);
    {
        ReaderApp app(card.string()); Driver driver{app};
        assert(app.initialize()); driver.present(); opened(app, "Books/cover.epub"); driver.present();
        assert(app.saved_record() && app.saved_record()->anchor.node == "/html[1]/body[1]/img[1]" && app.saved_record()->anchor.offset == 0);
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
        driver.key(Input::K2); driver.present(); assert(*app.saved_record() == image_record);
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
        assert(cancelled.request.url.find("https://example.invalid/") == 0);
        driver.key(Input::K2); driver.present();
        app.complete_network(cancelled.id, 200, remote, "");
        assert(!app.pending_frame() && !app.take_network_request() && *app.saved_record() == before);

        driver.sync_view(); const auto candidate = driver.check_remote();
        app.complete_network(candidate.id, 200, remote, ""); driver.present();
        auto switched_account = sync_config;
        switched_account.replace(switched_account.find("test-user"), 9, "other-user");
        write(card / "PicoPage/sync.toml", switched_account);
        driver.tap(342, 752);
        assert(app.view() == View::Sync && app.pending_frame() && app.pending_frame()->purpose == FramePurpose::Overlay);
        driver.present(); assert(*app.saved_record() == before && !app.take_network_request());
        driver.key(Input::K2); driver.present();
        write(card / "PicoPage/sync.toml", sync_config);

        driver.sync_view(); const auto stale = driver.check_remote();
        opened(app, "Books/cover.epub"); driver.present(); const auto cover = *app.saved_record();
        driver.sync_view(); const auto current = driver.check_remote();
        assert(current.id != stale.id);
        app.complete_network(stale.id, 200, remote, "");
        assert(!app.pending_frame() && !app.take_network_request() && *app.saved_record() == cover);
        app.complete_network(current.id, 200, "{}", ""); driver.present();
        assert(*app.saved_record() == cover && !app.take_network_request());
        driver.tap(342, 884); assert(app.view() == View::Confirmation); driver.present();
        driver.key(Input::K2); driver.present();
        assert(app.view() == View::Reading && !app.take_network_request() && *app.saved_record() == cover);
    }
    {
        ReaderApp app(card.string()); Driver driver{app};
        assert(app.initialize()); driver.present(); opened(app, "Books/book.epub"); driver.present();
        const auto saved = *app.saved_record();
        const std::string external = "schema_version=1\n[reader]\nfont_size_px=48\nline_height=1.6\nmargin_x_px=44\n[display]\nrefresh_preset='clear'\n";
        write(settings, external);
        driver.typography(); driver.tap(580, 948); driver.present();
        driver.key(Input::K2); driver.present();
        assert(read(settings) == external && *app.saved_record() == saved);
        driver.typography(); driver.tap(580, 948); driver.present(); driver.tap(500, 1130); driver.present();
        auto actual = FileStore(card.string()).load_settings("Books/book.epub").settings;
        assert(actual.font_size_px == 50 && actual.line_height == 1.6 && actual.margin_x_px == 44 && actual.refresh_preset == "clear");
        assert(*app.saved_record() == saved);

        const auto body_before_refresh = app.canvas().pixels();
        write(settings, "schema_version=1\n[reader]\nfont_size_px=54\nline_height=1.55\nmargin_x_px=32\n[display]\nrefresh_preset='balanced'\n");
        driver.tap(342, 500); driver.present(); driver.tap(480, 1140); driver.present();
        assert(app.view() == View::Refresh);
        driver.tap(342, 700); driver.present();
        actual = FileStore(card.string()).load_settings("Books/book.epub").settings;
        if (actual.refresh_preset != "fast") std::cerr << "Refresh edit failed: " << app.status() << '\n';
        assert(actual.font_size_px == 54 && actual.line_height == 1.55 && actual.margin_x_px == 32 && actual.refresh_preset == "fast");
        driver.key(Input::K2); driver.present();
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
        driver.key(Input::K2); driver.present();
        assert(std::equal(applied_pixels.begin(), applied_pixels.begin() + 342 * 1128,
                          app.canvas().pixels().begin()) && *app.saved_record() == saved);

        const std::string invalid = "schema_version=1\n[reader]\nfont_size_px='invalid'\n";
        write(settings, invalid);
        driver.tap(342, 500); driver.present(); driver.tap(205, 1140);
        assert(app.view() == View::Error); driver.present();
        assert(read(settings) == invalid && *app.saved_record() == saved);
        driver.key(Input::K2); driver.present();
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
    fs::remove_all(card);
    std::cout << "reader app display, progress protection, typography, cover and stale network checks: passed\n";
}
