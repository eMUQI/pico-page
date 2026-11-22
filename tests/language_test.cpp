#include "pico_page/app.h"
#include "pico_page/store.h"
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <thread>
#include <unistd.h>

using namespace pico_page;
namespace fs = std::filesystem;
namespace {
struct Translation { const char* source; const char* translated; };
constexpr Translation translations[] = {
#include "../firmware/components/reader_core/translations.inc"
};
constexpr Translation english_translations[] = {
#include "../firmware/components/reader_core/translations_en.inc"
};
void put(const fs::path& path, const std::string& value) {
    fs::create_directories(path.parent_path()); std::ofstream(path) << value;
}
std::string get(const fs::path& path) {
    std::ifstream input(path); return {std::istreambuf_iterator<char>(input), {}};
}
struct Driver {
    ReaderApp& app;
    std::uint64_t sequence = 0;
    void present() {
        assert(app.pending_frame());
        while (app.pending_frame()) {
            while (!app.prepare_frame()) app.poll_background();
            app.complete_frame(true);
        }
    }
    void input(Input key, bool touch = false, int x = 0, int y = 0) {
        ++sequence;
        app.input({{sequence, Phase::Begin, key}, touch, x, y});
        app.input({{sequence, Phase::End, key}, touch, x, y});
    }
    void tap(int x, int y) { input(Input::TouchCenter, true, x, y); present(); }
    void settle() {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        while (app.content_busy() || app.pending_frame()) {
            app.poll_background();
            if (app.pending_frame()) present();
            assert(std::chrono::steady_clock::now() < deadline);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    void language() {
        assert(app.view() == View::Library);
        tap(600, 50);
        assert(app.view() == View::Settings);
        tap(300, 370); assert(app.view() == View::LanguageSettings);
    }
};
}
int main() {
    using i18n::Language;
    constexpr auto simplified = Language::SimplifiedChinese, traditional = Language::TraditionalChinese, english = Language::English;
    Language parsed = traditional;
    assert(i18n::parse_language("zh-Hans", parsed) && parsed == simplified);
    assert(i18n::parse_language("zh-Hant", parsed) && parsed == traditional);
    assert(i18n::parse_language("en", parsed) && parsed == english);
    assert(!i18n::parse_language("bogus", parsed) && parsed == english);
    assert(std::strcmp(i18n::language_code(english), "en") == 0);
    assert(std::strcmp(i18n::language_name(english), "English") == 0);
    graphics::Font font; assert(font.open_fallback());
    for (std::size_t i = 0; i < std::size(translations); ++i) {
        const auto& item = translations[i];
        if (i) assert(std::strcmp(translations[i - 1].source, item.source) < 0);
        assert(i18n::text(simplified, item.source) == item.source);
        assert(std::strcmp(i18n::text(traditional, item.source), item.translated) == 0);
        for (const auto* value : {item.source, item.translated})
            for (const auto scalar : graphics::decode_utf8(value)) assert(!font.missing(scalar));
    }
    for (std::size_t i = 0; i < std::size(english_translations); ++i) {
        const auto& item = english_translations[i];
        if (i) assert(std::strcmp(english_translations[i - 1].source, item.source) < 0);
        assert(std::strcmp(i18n::text(english, item.source), item.translated) == 0);
        for (const auto scalar : graphics::decode_utf8(item.translated)) assert(!font.missing(scalar));
    }
    for (auto language : {simplified, traditional, english})
        assert(std::strcmp(i18n::text(language, "not a message"), "not a message") == 0);
    assert(i18n::format(english, "{first}: {second}", {{"second", "{first}"}, {"first", "book"}}) == "book: {first}");
    assert(i18n::format(english, "{first}: {second}", {{"first", "book"}}) == "{first}: {second}");
    assert(i18n::format(english, "{first}", {{"first", "book"}, {"first", "other"}}) == "{first}");
    assert(i18n::format(english, "{first}", {{"first", "book"}, {"second", "extra"}}) == "{first}");
    assert(i18n::format(simplified, "可用 {size}", {{"size", "2 MiB"}}) == "可用 2 MiB");
    assert(i18n::format(traditional, "可用 {size}", {{"size", "2 MiB"}}) == "可用 2 MiB");
    assert(i18n::format(english, "可用 {size}", {{"size", "2 MiB"}}) == "2 MiB available");
    const std::string limit(i18n::max_formatted_bytes, 'x');
    assert(i18n::format(english, "{value}", {{"value", limit}}) == limit);
    assert(i18n::format(english, "x{value}", {{"value", limit}}) == "x{value}");
    assert(i18n::format(english, (limit + "x").c_str(), {}).empty());
    char directory[] = "/tmp/pico-language-XXXXXX"; assert(mkdtemp(directory)); const fs::path base(directory);
    struct TxtFailure {
        const char* name;
        std::string bytes;
        const char* simplified;
        const char* translated;
    };
    const TxtFailure failures[] = {
        {"empty", "", "TXT 文件为空或超过 64 MiB", "TXT 檔案為空或超過 64 MiB"},
        {"blank", "\r\n \t", "TXT 没有可阅读的文字", "TXT 沒有可閱讀的文字"},
        {"nul", std::string("a\0b", 3), "TXT 文件包含 NUL 字节", "TXT 檔案包含 NUL 位元組"},
        {"encoding", "valid\xffinvalid", "TXT 编码不受支持（支持 UTF-8、GB18030、带 BOM 的 UTF-16）",
         "TXT 編碼不受支持（支持 UTF-8、GB18030、帶 BOM 的 UTF-16）"},
        {"utf16-odd", std::string("\xff\xfe", 2) + "x", "TXT UTF-16 长度无效或没有可阅读的文字",
         "TXT UTF-16 長度無效或沒有可閱讀的文字"},
        {"utf16-surrogate", std::string("\xff\xfe\x00\xd8\x61\x00", 6), "TXT UTF-16 包含无效代理对或 NUL 字符",
         "TXT UTF-16 包含無效代理對或 NUL 字符"},
    };
    for (auto language : {simplified, traditional, english}) for (bool async : {false, true}) {
        const auto root = base / (std::string(i18n::language_code(language)) + (async ? "-async" : "-sync"));
        put(root / "PicoPage/settings.toml", std::string("schema_version=1\nonboarding.done=true\nsystem.language=\"") +
            i18n::language_code(language) + "\"\n");
        for (const auto& failure : failures) {
            const auto relative = std::string("Books/") + failure.name + ".txt";
            put(root / relative, failure.bytes);
            ReaderApp app(root.string(), "txt-language-test", true, async); Driver driver{app};
            assert(app.initialize()); driver.present();
            assert(app.open_book(relative) == async); driver.settle();
            assert(app.view() == View::Error && !app.saved_record());
            const std::string expected = language == english ? i18n::text(english, failure.simplified) :
                language == traditional ? failure.translated : failure.simplified;
            const std::string suffix = i18n::text(language, "。原书没有被修改。");
            if (app.status() != expected + suffix)
                std::fprintf(stderr, "%s %s %s: %s\n", i18n::language_code(language), async ? "async" : "sync",
                             failure.name, app.status().c_str());
            assert(app.status() == expected + suffix);
            assert(get(root / relative) == failure.bytes);
        }
    }
    const auto config = base / "PicoPage/settings.toml";
    const auto book = base / "Books/简体书名.txt";
    const std::string original = "第一章 简体正文\n这段正文不应因界面语言变化而改变。\n";
    put(book, original);
    put(config, "schema_version=1\n# keep this comment\nonboarding.done=true\nreader.fullscreen_reading=false\n");
    FileStore store(base.string()); assert(store.load_settings().settings.language == simplified);
    for (auto target : {traditional, english}) for (bool async : {false, true}) {
        const int target_y = target == english ? 480 : 340;
        ReaderApp app(base.string(), "language-test", true, async); Driver driver{app};
        assert(app.initialize() && app.language() == simplified); driver.present();
        if (!async) {
            assert(app.open_book("Books/简体书名.txt")); driver.present();
            app.save_progress(); assert(app.saved_record());
            driver.tap(300, 500); assert(app.view() == View::Tools);
            driver.tap(40, 50); assert(app.view() == View::Library);
        }
        const auto position = app.saved_record();
        driver.language();
        const auto before = app.canvas().pixels();
        fs::create_directory(config.string() + ".tmp");
        driver.tap(300, target_y);
        assert(app.language() == simplified && app.view() == View::LanguageSettings);
        assert(!app.status().empty() && store.load_settings().settings.language == simplified);
        fs::remove(config.string() + ".tmp");
        driver.tap(30, 50); driver.tap(300, 370);
        driver.tap(300, target_y);
        assert(app.language() == target && app.canvas().pixels() != before);
        const auto after = app.canvas().pixels();
        driver.input(Input::K2); assert(app.pending_frame()->refresh == RefreshIntent::FullClean); driver.present();
        assert(app.canvas().pixels() == after);
        assert(store.load_settings().settings.language == target && get(book) == original);
        assert(app.saved_record() == position);
        assert(get(config).find("# keep this comment") != std::string::npos);
        {
            ReaderApp reboot(base.string()); assert(reboot.initialize() && reboot.language() == target);
        }
        auto snapshot = store.settings_snapshot("Books/简体书名.txt");
        auto values = snapshot.config.settings; values.language = simplified;
        std::string error;
        assert(store.apply_settings(snapshot, values, true, error) == StoreStatus::Invalid);
        assert(!fs::exists(book.string() + ".picopage/settings.toml"));
        values = snapshot.config.settings; values.onboarding_in_progress = true;
        assert(store.apply_settings(snapshot, values, true, error) == StoreStatus::Invalid);
        snapshot = store.settings_snapshot(); values = snapshot.config.settings; values.language = simplified;
        put(config, get(config) + "# external change\n");
        assert(store.apply_settings(snapshot, values, false, error) == StoreStatus::ExternalChange);
        driver.tap(300, 200);
        assert(app.language() == target && !app.status().empty());
        driver.tap(30, 50); driver.tap(300, 370); driver.tap(300, 200);
        assert(app.language() == simplified && store.load_settings().settings.language == simplified);
        auto external = get(config);
        const auto language = external.find("zh-Hans"); assert(language != std::string::npos);
        external.replace(language, 7, i18n::language_code(target)); put(config, external);
        driver.tap(30, 50); driver.tap(300, 370); driver.tap(300, 200);
        assert(app.language() == simplified && store.load_settings().settings.language == simplified);
    }
    for (const auto* invalid : {"\"bogus\"", "42", "\"\""}) {
        const auto source = std::string("schema_version=1\nsystem.language=") + invalid + "\n";
        put(config, source);
        const auto result = store.load_settings();
        assert(!result.diagnostics.empty() && result.settings.language == simplified);
        assert(get(config) == source);
    }
    fs::remove_all(base);
}
