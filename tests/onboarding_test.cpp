#include "pico_page/app.h"
#include "pico_page/store.h"
#include <cassert>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include <unistd.h>
using namespace pico_page;
namespace fs = std::filesystem;
static void put(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path()); std::ofstream(path) << text;
}
struct Driver {
    ReaderApp& app;
    std::uint64_t sequence = 0;
    void present() { assert(app.pending_frame()); app.complete_frame(true); assert(!app.pending_frame()); }
    void input(Input key, bool touch = false, int x = 0, int y = 0, bool queued = false) {
        ++sequence;
        app.input({{sequence, Phase::Begin, key}, touch, x, y, queued});
        app.input({{sequence, Phase::End, key}, touch, x, y, queued});
    }
    void tap(int x, int y) { input(Input::TouchCenter, true, x, y); }
    void swipe(bool next) { input(next ? Input::Next : Input::Previous, false, 300, 500); }
    void capture(const std::string& path) {
        std::ofstream out(path, std::ios::binary);
        out << "P5\n684 1216\n255\n";
        for (int y = 0; y < 1216; ++y) for (int x = 0; x < 684; ++x) out.put(app.canvas().pixel(x, y) * 17);
    }
    void refresh() {
        const auto pixels = app.canvas().pixels(); input(Input::K2);
        assert(app.view() == View::Onboarding && app.pending_frame()->refresh == RefreshIntent::FullClean);
        assert(app.canvas().pixels() == pixels); present();
    }
    void sleep() {
        const auto pixels = app.canvas().pixels();
        assert(app.begin_sleep()); if (app.pending_frame()) present();
        assert(app.prepare_sleep()); present(); assert(app.view() == View::Sleeping);
        app.wake(); assert(app.view() == View::Onboarding); present();
        assert(app.canvas().pixels() == pixels);
    }
};
int main(int argc, char** argv) {
    char temp[] = "/tmp/pico-onboarding-XXXXXX"; assert(mkdtemp(temp)); const fs::path base(temp);
    const fs::path shots = argc > 1 ? argv[1] : temp;
    fs::create_directories(shots);
    for (bool async : {false, true}) {
        const auto root = base / (async ? "async" : "sync"); fs::create_directories(root / "Books");
        FileStore store(root.string()); assert(store.needs_onboarding());
        {
            ReaderApp app(root.string(), "onboarding", true, async); Driver d{app};
            assert(app.initialize() && app.view() == View::Onboarding);
            assert(app.pending_frame()->refresh == RefreshIntent::FullClean); d.present();
            d.capture((shots / "language.pgm").string()); d.refresh(); d.sleep();
            assert(!fs::exists(root / "PicoPage/settings.toml"));
            d.input(Input::K1); d.tap(300, 700); assert(!app.pending_frame());
            d.tap(300, 1120); d.present();
            d.capture((shots / "welcome.pgm").string()); d.refresh(); d.sleep();
            const auto welcome = app.canvas().pixels();
            d.input(Input::K1); d.present(); d.input(Input::K3); d.present();
            assert(app.canvas().pixels() == welcome && !fs::exists(root / "PicoPage/settings.toml"));
            d.tap(300, 700); assert(!app.pending_frame());
            d.tap(300, 1000); d.present(); d.capture((shots / "practice.pgm").string());
            const auto practice = app.canvas().pixels(); d.refresh(); d.sleep();
            d.tap(100, 500); d.input(Input::K1); d.input(Input::Previous); assert(!app.pending_frame());
            d.tap(300, 500); d.present(); d.capture((shots / "practice-toolbar.pgm").string());
            const auto toolbar = app.canvas().pixels(); assert(toolbar != practice);
            d.input(Input::K1); d.input(Input::K3); d.input(Input::Next); assert(!app.pending_frame());
            d.tap(60, 1150); d.present(); d.capture((shots / "practice-tool.pgm").string());
            assert(app.canvas().pixels() != toolbar);
            d.tap(60, 1150); d.tap(300, 50); assert(!app.pending_frame());
            d.tap(600, 700); d.present(); assert(app.canvas().pixels() == practice);
            d.input(Input::K3, false, 0, 0, true); assert(!app.pending_frame());
            d.tap(600, 500); const auto turned = app.canvas().pixels(); assert(turned != practice);
            app.input({{d.sequence, Phase::End, Input::TouchCenter}, true, 600, 500});
            assert(app.canvas().pixels() == turned); d.present();
            d.capture((shots / "practice-previous.pgm").string()); d.sleep();
            d.input(Input::K1); d.present(); d.capture((shots / "practice-open.pgm").string());
            d.tap(300, 500); d.present(); d.tap(300, 900); d.present();
            d.capture((shots / "practice-done.pgm").string()); d.refresh();
            const auto done = app.canvas().pixels();
            d.tap(300, 500); d.present(); d.tap(300, 1150); d.present();
            assert(app.view() == View::Onboarding && app.canvas().pixels() != done);
            d.tap(300, 700); d.present(); assert(app.canvas().pixels() == done);
            d.swipe(true); d.present(); d.swipe(false); d.present(); assert(app.canvas().pixels() == done);
            d.tap(300, 1130); d.present(); d.capture((shots / "tools.pgm").string()); d.refresh(); d.sleep();
            d.tap(600, 50); d.tap(300, 420); assert(!app.pending_frame());
            d.tap(300, 1120); d.present(); assert(app.view() == View::Library);
            assert(store.load_settings().settings.onboarding_done && !store.needs_onboarding());
        }
        ReaderApp upgraded(root.string()); Driver d{upgraded}; assert(upgraded.initialize()); d.present();
        assert(upgraded.view() == View::Library);
        d.tap(600, 50); d.present(); d.tap(300, 970); d.present(); assert(upgraded.view() == View::Help);
        const auto before = store.settings_snapshot().global_source;
        d.capture((shots / "help.pgm").string());
        d.tap(300, 1060); d.present(); assert(upgraded.view() == View::Onboarding);
        d.tap(30, 50); d.present(); assert(upgraded.view() == View::Help);
        d.tap(300, 1060); d.present(); d.input(Input::K3); d.present();
        for (int i = 0; i < 4; ++i) { d.input(Input::Next); d.present(); }
        d.input(Input::K3); d.present(); assert(upgraded.view() == View::Help);
        assert(store.settings_snapshot().global_source == before);
        d.tap(300, 1060); d.present(); upgraded.show_error("测试错误"); d.present();
        d.tap(300, 1120); d.present(); assert(upgraded.view() == View::Help);
    }
    for (bool practice : {false, true}) {
        const auto root = base / (practice ? "skip-practice" : "skip-welcome"); fs::create_directories(root);
        ReaderApp app(root.string()); Driver d{app}; assert(app.initialize()); d.present();
        d.input(Input::K3); d.present();
        if (practice) { d.input(Input::K3); d.present(); }
        d.tap(practice ? 600 : 300, practice ? 50 : 1120); d.present();
        assert(app.view() == View::Library && FileStore(root.string()).load_settings().settings.onboarding_done);
    }
    for (auto language : {i18n::Language::TraditionalChinese, i18n::Language::English}) for (bool async : {false, true}) {
        const std::string tag = language == i18n::Language::English ? "english" : "traditional";
        const int language_y = language == i18n::Language::English ? 480 : 340;
        const auto root = base / (tag + (async ? "-async" : "-sync")); fs::create_directories(root);
        FileStore store(root.string());
        {
            ReaderApp app(root.string(), "onboarding-language", true, async); Driver d{app};
            assert(app.initialize() && app.view() == View::Onboarding); d.present();
            d.tap(300, 200); assert(!app.pending_frame());
            assert(!fs::exists(root / "PicoPage/settings.toml"));
            fs::create_directories(root / "PicoPage/settings.toml.tmp");
            d.tap(300, language_y); d.present();
            assert(app.language() == i18n::Language::SimplifiedChinese && !app.status().empty());
            assert(!fs::exists(root / "PicoPage/settings.toml"));
            fs::remove(root / "PicoPage/settings.toml.tmp");
            d.tap(300, language_y); d.present();
            assert(app.language() == language);
            const auto saved = store.load_settings();
            assert(saved.diagnostics.empty() && saved.settings.onboarding_in_progress && !saved.settings.onboarding_done);
            assert(store.needs_onboarding());
            d.capture((shots / ("language-" + tag + ".pgm")).string()); d.refresh(); d.sleep();
            d.input(Input::TouchCenter, true, 300, 200, true); assert(!app.pending_frame());
            assert(app.language() == language);
            fs::create_directory(root / "PicoPage/settings.toml.tmp");
            d.tap(300, 200); d.present();
            assert(app.language() == language && !app.status().empty());
            assert(store.load_settings().settings.language == language);
            d.capture((shots / ("language-save-failed-" + tag + ".pgm")).string());
            fs::remove(root / "PicoPage/settings.toml.tmp");
            d.tap(300, 200); d.present(); assert(app.language() == i18n::Language::SimplifiedChinese);
            d.tap(300, language_y); d.present(); assert(store.needs_onboarding());
            d.tap(300, 1120); d.present(); d.capture((shots / ("welcome-" + tag + ".pgm")).string());
            d.tap(300, 200); d.tap(300, language_y); assert(!app.pending_frame());
            assert(app.language() == language);
            d.tap(30, 50); d.present();
            d.capture((shots / ("language-return-" + tag + ".pgm")).string());
        }
        {
            ReaderApp reboot(root.string(), "onboarding-language", true, async); Driver d{reboot};
            assert(reboot.initialize() && reboot.view() == View::Onboarding);
            assert(reboot.language() == language); d.present();
            d.input(Input::K3); d.present(); d.sleep();
            d.input(Input::K3); d.present(); d.sleep();
            d.tap(600, 50); d.present(); assert(reboot.view() == View::Library);
            assert(store.load_settings().settings.onboarding_done && !store.load_settings().settings.onboarding_in_progress);
            assert(!store.needs_onboarding());
        }
        {
            ReaderApp replay(root.string()); Driver d{replay}; assert(replay.initialize()); d.present();
            d.tap(600, 50); d.present(); d.tap(300, 970); d.present(); d.tap(300, 1060); d.present();
            assert(replay.view() == View::Onboarding);
            d.tap(300, 200); d.present(); assert(replay.language() == i18n::Language::SimplifiedChinese);
            assert(store.load_settings().settings.onboarding_done && !store.load_settings().settings.onboarding_in_progress);
            assert(!store.needs_onboarding()); d.sleep();
            d.tap(300, 1120); d.present(); d.tap(30, 50); d.present();
            d.capture((shots / ("language-replay-" + tag + ".pgm")).string());
            d.tap(30, 50); d.present(); assert(replay.view() == View::Help);
        }
    }
    const auto interrupted = base / "interrupted"; fs::create_directories(interrupted);
    std::vector<std::uint8_t> language_pixels;
    { ReaderApp app(interrupted.string()); Driver d{app}; assert(app.initialize()); d.present(); language_pixels = app.canvas().pixels(); d.input(Input::K3); d.present(); }
    { ReaderApp app(interrupted.string()); Driver d{app}; assert(app.initialize()); d.present(); assert(app.view() == View::Onboarding && app.canvas().pixels() == language_pixels); d.tap(300,1120); d.present(); }
    for (bool absent : {false, true}) {
        const auto root = base / (absent ? "absent" : "blocked");
        if (!absent) fs::create_directories(root / "PicoPage/settings.toml.tmp");
        for (int boot = 0; boot < 2; ++boot) {
            ReaderApp app(root.string()); Driver d{app}; assert(app.initialize()); d.present();
            assert(app.view() == View::Onboarding);
            if (absent) { app.card_unavailable(CardIssue::Removed); d.present(); assert(app.view() == View::Onboarding); }
            for (int i = 0; i < 6; ++i) { d.input(Input::K3); d.present(); }
            assert(app.view() == View::Library && !FileStore(root.string()).load_settings().settings.onboarding_done);
        }
    }
    for (const auto* evidence : {"PicoPage/settings.toml", "PicoPage/settings.toml.bak", "PicoPage/network.toml", "PicoPage/sync.toml",
            "PicoPage/data/last-book.txt", "PicoPage/data/recent-books.txt.bak", "Books/deep/book.epub.picopage/progress.json",
            "Books/deep/book.epub.picopage/progress.json.bak", "Books/deep/book.epub.picopage/settings.toml"}) {
        const auto root = base / "legacy"; fs::remove_all(root); put(root / evidence, "legacy or damaged content");
        assert(!FileStore(root.string()).needs_onboarding());
        ReaderApp app(root.string()); assert(app.initialize() && app.view() != View::Onboarding); app.complete_frame(true);
    }
    const auto limits = base / "limits";
    fs::create_directories(limits / "Books/a/b/c/d/e/f");
    assert(!FileStore(limits.string()).needs_onboarding());
    fs::remove_all(limits);
    fs::create_directories(limits / "Books");
    for (int i = 0; i < 2001; ++i) put(limits / "Books" / std::to_string(i), "");
    assert(!FileStore(limits.string()).needs_onboarding());
    fs::remove_all(limits);
    fs::create_directories(limits);
    fs::create_directory_symlink(base, limits / "Books");
    assert(!FileStore(limits.string()).needs_onboarding());
    const auto config = base / "config";
    put(config / "PicoPage/settings.toml", "schema_version=1\n[onboarding]\ndone='yes'\n");
    assert(!FileStore(config.string()).load_settings().diagnostics.empty());
    for (const auto* invalid : {"1", "'yes'"}) {
        put(config / "PicoPage/settings.toml", std::string("schema_version=1\nonboarding.in_progress=") + invalid + "\n");
        assert(!FileStore(config.string()).load_settings().diagnostics.empty());
        assert(!FileStore(config.string()).needs_onboarding());
    }
    put(config / "PicoPage/settings.toml", "schema_version=1\nonboarding.in_progress=true\nonboarding.done=true\n");
    assert(!FileStore(config.string()).needs_onboarding());
    const auto failed = base / "display-failure"; fs::create_directories(failed);
    ReaderApp app(failed.string()); Driver d{app}; assert(app.initialize());
    app.complete_frame(false); assert(app.view() == View::Error); d.present();
    d.tap(300, 1120); d.present(); assert(app.view() == View::Library);
    assert(FileStore(failed.string()).needs_onboarding());
    fs::remove_all(base);
}
