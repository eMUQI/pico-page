#include "pico_page/store.h"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <csignal>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

namespace fs = std::filesystem;
using namespace pico_page;

static void put(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary);
    output << text;
    assert(output.good());
}

static std::string get(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

int main() {
    char temporary[] = "/tmp/pico-store-XXXXXX";
    const char* directory = mkdtemp(temporary);
    assert(directory);
    const fs::path root(directory);
    FileStore store(root.string());
    const std::string book = "Books/中文.epub";
    const fs::path sidecar = root / (book + ".picopage");
    const fs::path progress = sidecar / "progress.json";
    put(root / book, "original epub");
    ReadingRecord first{"sha256:one", "source-v1", "device-a", 1, {"chapter.xhtml", "/p[1]", 9}};
    auto saved = store.save_progress(book, first);
    assert(saved.status == StoreStatus::Ok && saved.record && *saved.record == first);
    auto loaded = store.load_progress(book, first.book_id, first.content_revision);
    assert(loaded.record && *loaded.record == first);

    const std::string first_bytes = get(progress);
    std::string invalid_text = first_bytes;
    invalid_text.replace(invalid_text.find("chapter.xhtml"), 1, 1, char(0xff));
    put(progress, invalid_text);
    assert(!store.load_progress(book, first.book_id, first.content_revision).record);
    put(progress, first_bytes);
    ReadingRecord second = first;
    second.local_revision = 9007199254740993ULL;
    second.anchor.offset = 15;
    saved = store.save_progress(book, second);
    assert(saved.record && *saved.record == second);
    assert(store.load_progress(book, first.book_id, first.content_revision).record->local_revision == second.local_revision);
    put(progress.string() + ".tmp", first_bytes);
    assert(store.load_progress(book, first.book_id, first.content_revision).record->local_revision == second.local_revision);

    put(progress, "{partial");
    loaded = store.load_progress(book, first.book_id, first.content_revision);
    assert(loaded.recovered_backup && loaded.record && *loaded.record == first);
    fs::remove(progress);
    assert(store.load_progress(book, first.book_id, first.content_revision).recovered_backup);
    assert(store.save_progress(book, second).record);
    assert(get(progress.string() + ".bak") == first_bytes);
    put(progress, "{partial");
    fs::remove(progress.string() + ".bak");
    loaded = store.load_progress(book, first.book_id, first.content_revision);
    assert(!loaded.record && loaded.status == StoreStatus::Invalid);
    put(progress, first_bytes);
    assert(store.load_progress(book, "sha256:other", first.content_revision).status == StoreStatus::IdentityMismatch);
    ReadingRecord other = second;
    other.book_id = "sha256:other";
    assert(!store.save_progress(book, other).record && get(progress) == first_bytes);

    fs::remove(progress.string() + ".tmp");
    fs::create_directory(progress.string() + ".tmp");
    assert(!store.save_progress(book, second).record && get(progress) == first_bytes);
    fs::remove(progress.string() + ".tmp");
    fs::create_directory(progress.string() + ".bak.tmp");
    assert(!store.save_progress(book, second).record && get(progress) == first_bytes);
    fs::remove(progress.string() + ".bak.tmp");
    fs::create_directory(progress.string() + ".bak");
    assert(!store.save_progress(book, second).record && get(progress) == first_bytes);
    fs::remove(progress.string() + ".bak");

    const pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
        std::signal(SIGXFSZ, SIG_IGN);
        struct rlimit limit {64, 64};
        assert(setrlimit(RLIMIT_FSIZE, &limit) == 0);
        assert(!store.save_progress(book, second).record);
        _exit(0);
    }
    int child_status = 0;
    assert(waitpid(child, &child_status, 0) == child && WIFEXITED(child_status) && WEXITSTATUS(child_status) == 0);
    assert(get(progress) == first_bytes);

    std::string future = first_bytes;
    const auto schema = future.find("\"schema_version\":1");
    assert(schema != std::string::npos);
    future.replace(schema, 18, "\"schema_version\":2");
    put(progress, future);
    put(progress.string() + ".bak", first_bytes);
    assert(store.load_progress(book, first.book_id, first.content_revision).status == StoreStatus::UnsupportedVersion);
    assert(!store.save_progress(book, second).record && get(progress) == future);
    assert(store.save_progress("../outside.epub", first).status == StoreStatus::UnsafePath);

    const fs::path global = root / "PicoPage/settings.toml";
    const std::string global_text = "# retain this comment\nschema_version=1\n[reader]\nfont='Fonts/custom.ttf'\nfont_size_px=38\nline_height=1.6\n[library]\nroots=['/Books']\n";
    put(global, global_text);
    const auto book_settings = sidecar / "settings.toml";
    const std::string book_settings_text = "schema_version=1\n# book preferences\n[reader]\nfont_size_px=42 # preferred size\n";
    put(book_settings, book_settings_text);
    auto config = store.load_settings(book);
    assert(config.diagnostics.empty());
    assert(config.settings.font == "Fonts/custom.ttf" && config.settings.font_size_px == 42);
    assert(config.settings.library_roots == std::vector<std::string>{"Books"});
    assert(config.settings.paragraph_gap_lines == 0 && config.settings.indent_chars == 2);
    std::string error;
    auto snapshot = store.settings_snapshot(book);
    auto preview = snapshot.config.settings;
    preview.font_size_px = 44;
    preview.paragraph_gap_lines = 0.6;
    preview.indent_chars = 1;
    assert(store.apply_settings(snapshot, preview, true, error) == StoreStatus::Ok);
    assert(get(global) == global_text);
    assert(get(book_settings).find("# book preferences") != std::string::npos);
    assert(get(book_settings).find("font_size_px=44 # preferred size") != std::string::npos);
    assert(get(book_settings).find("paragraph_gap_lines = 0.6") != std::string::npos);
    config = store.load_settings(book);
    assert(config.diagnostics.empty() && config.settings.font_size_px == 44);
    assert(config.settings.paragraph_gap_lines == 0.6 && config.settings.indent_chars == 1);
    assert(store.apply_settings(snapshot, preview, true, error) == StoreStatus::ExternalChange);

    snapshot = store.settings_snapshot(book);
    const auto applied_book = get(book_settings);
    put(global, global_text + "# external edit\n");
    preview.font_size_px = 46;
    assert(store.apply_settings(snapshot, preview, true, error) == StoreStatus::ExternalChange);
    assert(get(book_settings) == applied_book);
    put(global, global_text);
    snapshot = store.settings_snapshot(book);
    fs::create_directory(book_settings.string() + ".tmp");
    assert(store.apply_settings(snapshot, preview, true, error) == StoreStatus::IoError);
    assert(get(book_settings) == applied_book);
    fs::remove(book_settings.string() + ".tmp");
    assert(store.clear_book_settings(snapshot, error) == StoreStatus::Ok);
    config = store.load_settings(book);
    assert(config.diagnostics.empty() && config.settings.font_size_px == 38 && config.settings.indent_chars == 2);
    assert(get(book_settings).find("# preferred size") != std::string::npos);

    put(book_settings, "schema_version=1\nreader={font_size_px=42}\n");
    snapshot = store.settings_snapshot(book);
    assert(snapshot.config.diagnostics.empty());
    preview = snapshot.config.settings;
    preview.font_size_px = 48;
    const auto inline_settings = get(book_settings);
    assert(store.apply_settings(snapshot, preview, true, error) == StoreStatus::Invalid);
    assert(get(book_settings) == inline_settings);
    for (const std::string quote : {"'", "\""}) {
        const std::string original = "schema_version=1\n[reader]\nfont=" + quote +
            "Fonts/old.ttf" + quote + " # font note\n[display]\nrefresh_preset=" + quote +
            "clear" + quote + " # refresh note\n";
        put(book_settings, original);
        snapshot = store.settings_snapshot(book);
        assert(snapshot.config.diagnostics.empty());
        preview = snapshot.config.settings;
        preview.font = "Fonts/new.ttf";
        preview.refresh_preset = "fast";
        assert(store.apply_settings(snapshot, preview, true, error) == StoreStatus::Ok);
        config = store.load_settings(book);
        assert(config.settings.font == "Fonts/new.ttf" && config.settings.refresh_preset == "fast");
        assert(get(book_settings).find(" # font note\n") != std::string::npos);
        assert(get(book_settings).find(" # refresh note\n") != std::string::npos);
        assert(get(global) == global_text);
        put(book_settings, "schema_version=1\n[display]\nrefresh_preset=" + quote + quote + quote +
            "clear" + quote + quote + quote + " # preserved multiline syntax\n");
        snapshot = store.settings_snapshot(book);
        assert(snapshot.config.diagnostics.empty());
        preview = snapshot.config.settings;
        preview.refresh_preset = "fast";
        const auto multiline = get(book_settings);
        assert(store.apply_settings(snapshot, preview, true, error) == StoreStatus::Invalid);
        assert(get(book_settings) == multiline);
    }
    put(book_settings, book_settings_text);
    snapshot = store.settings_snapshot(book);
    preview = snapshot.global_settings;
    preview.font_size_px = 40;
    assert(store.apply_settings(snapshot, preview, false, error) == StoreStatus::Ok);
    assert(store.load_settings().settings.font_size_px == 40);
    assert(store.load_settings(book).settings.font_size_px == 42 && get(book_settings) == book_settings_text);
    snapshot = store.settings_snapshot(book);
    preview = snapshot.config.settings;
    preview.font_size_px = 40;
    assert(store.apply_settings(snapshot, preview, true, error) == StoreStatus::Ok);
    assert(get(book_settings).find("font_size_px") == std::string::npos);
    assert(store.load_settings(book).settings.font_size_px == 40);
    put(global, global_text);
    put(book_settings, book_settings_text);

    snapshot = store.settings_snapshot(book);
    preview = snapshot.config.settings;
    preview.paragraph_gap_lines = 2;
    assert(store.apply_settings(snapshot, preview, true, error) == StoreStatus::Invalid);
    assert(get(book_settings) == book_settings_text);
    fs::remove(book_settings);
    snapshot = store.settings_snapshot(book);
    preview = snapshot.config.settings;
    preview.indent_chars = 0;
    assert(store.apply_settings(snapshot, preview, true, error) == StoreStatus::Ok);
    assert(store.load_settings(book).settings.indent_chars == 0);
    put(book_settings, book_settings_text);

    assert(store.save_last_book(book, error) == StoreStatus::Ok);
    assert(store.load_last_book(error) == std::optional<std::string>(book));
    assert(store.save_last_book("../outside.epub", error) == StoreStatus::UnsafePath);
    assert(store.save_last_book(book, error) == StoreStatus::Ok);
    put(root / "PicoPage/data/last-book.txt", "../outside.epub\n");
    assert(store.load_last_book(error) == std::optional<std::string>(book) && !error.empty());
    assert(store.save_last_book(book, error) == StoreStatus::Ok);
    fs::create_directory(root / "PicoPage/data/last-book.txt.tmp");
    assert(store.save_last_book(book, error) == StoreStatus::IoError);
    fs::remove(root / "PicoPage/data/last-book.txt.tmp");
    put(global, "schema_version=1\n[reader]\nfont='Fonts/new.ttf'\n");
    config = store.load_settings(book);
    assert(config.settings.font == "Fonts/new.ttf" && config.settings.font_size_px == 42);
    const std::string malformed = "schema_version=1\n[reader\nfont_size_px=48\n";
    put(global, malformed);
    config = store.load_settings();
    assert(config.settings.font_size_px == 38 && !config.diagnostics.empty());
    assert(config.diagnostics[0].path == "PicoPage/settings.toml" && get(global) == malformed);
    put(global, "schema_version=1\n[reader]\nfont_size_px='big'\n");
    config = store.load_settings();
    assert(config.settings.font_size_px == 38 && config.diagnostics[0].field == "reader.font_size_px");
    assert(config.settings.auto_sleep_minutes == 10);
    put(global, "schema_version=1\n[power]\nauto_sleep_minutes=0\n");
    assert(store.load_settings().settings.auto_sleep_minutes == 0);
    put(global, "schema_version=1\n[power]\nauto_sleep_minutes=241\n");
    config = store.load_settings();
    assert(config.settings.auto_sleep_minutes == 10 && config.diagnostics[0].field == "power.auto_sleep_minutes");
    put(book_settings, "schema_version=1\n[power]\nauto_sleep_minutes=5\n");
    put(global, "schema_version=1\n");
    config = store.load_settings(book);
    assert(config.settings.auto_sleep_minutes == 10 && config.diagnostics[0].field == "power");
    put(book_settings, "schema_version=1\n");
    put(global, "schema_version=1\n# user comment\n[power]\nauto_sleep_minutes=10 # idle\n");
    snapshot = store.settings_snapshot(); preview = snapshot.config.settings;
    preview.sleep_screen = "reading"; preview.auto_sleep_minutes = 15;
    preview.fast_full_refresh_pages = 3;
    assert(store.apply_settings(snapshot, preview, false, error) == StoreStatus::Ok);
    assert(store.load_settings().settings.sleep_screen == "reading" && store.load_settings().settings.auto_sleep_minutes == 15);
    assert(store.load_settings().settings.fast_full_refresh_pages == 3 && get(global).find("# idle") != std::string::npos);
    snapshot = store.settings_snapshot(); preview = snapshot.config.settings;
    preview.library_view = "grid";
    assert(store.apply_settings(snapshot, preview, false, error) == StoreStatus::Ok);
    assert(store.load_settings().settings.library_view == "grid" && get(global).find("# user comment") != std::string::npos);
    assert(store.load_settings().settings.auto_sleep_minutes == 15);
    preview.library_roots = {"Elsewhere"};
    assert(store.apply_settings(store.settings_snapshot(), preview, false, error) == StoreStatus::Invalid);
    snapshot = store.settings_snapshot(book); preview = snapshot.config.settings;
    preview.library_view = "list";
    assert(store.apply_settings(snapshot, preview, true, error) == StoreStatus::Invalid);
    std::uint64_t card_total = 0, card_free = 0;
    assert(store.card_space(card_total, card_free) && card_total >= card_free && card_total > 0);
    snapshot = store.settings_snapshot(book); preview = snapshot.config.settings;
    preview.sleep_screen = "simple";
    assert(store.apply_settings(snapshot, preview, true, error) == StoreStatus::Invalid);
    preview = snapshot.config.settings; preview.fast_full_refresh_pages = 31;
    assert(store.apply_settings(snapshot, preview, true, error) == StoreStatus::Invalid);
    preview = snapshot.config.settings; preview.fast_full_refresh_pages = 1;
    assert(store.apply_settings(snapshot, preview, true, error) == StoreStatus::Ok);
    snapshot = store.settings_snapshot(book);
    assert(store.clear_book_settings(snapshot, error) == StoreStatus::Ok);
    assert(store.load_settings(book).settings.fast_full_refresh_pages == 3);
    assert(store.load_settings().settings.sleep_screen == "reading");
    put(book_settings, book_settings_text);
    put(global, "schema_version=2\n[reader]\nfont_size_px=52\n");
    config = store.load_settings();
    assert(config.settings.font_size_px == 38 && config.diagnostics[0].status == StoreStatus::UnsupportedVersion);
    put(global, std::string("schema_version=1\n[reader]\nfont='Fonts/") + char(0xff) + ".ttf'\n");
    config = store.load_settings();
    assert(!config.diagnostics.empty() && config.settings.font == "Fonts/LXGWWenKai-Regular.ttf" &&
           config.settings.library_roots == std::vector<std::string>{"Books"});

    put(sidecar / "prepared/map", "required");
    put(sidecar / "cover.png", "manual");
    put(root / "PicoPage/cache/books/one/layout", "disposable");
    put(root / "outside/keep", "keep");
    fs::create_directory_symlink(root / "outside", root / "PicoPage/cache/external");
    assert(store.clear_cache() == StoreStatus::Ok);
    assert(fs::is_empty(root / "PicoPage/cache"));
    assert(get(root / book) == "original epub" && get(sidecar / "prepared/map") == "required");
    assert(get(sidecar / "cover.png") == "manual" && get(progress) == future);
    assert(get(root / "outside/keep") == "keep" && fs::exists(global));
    assert(store.load_last_book(error) == std::optional<std::string>(book));
    fs::remove(root / "PicoPage/cache");
    fs::create_directory_symlink(root / "outside", root / "PicoPage/cache");
    assert(store.clear_cache() == StoreStatus::UnsafePath && get(root / "outside/keep") == "keep");
    fs::create_directory_symlink(root / "outside", root / "Linked");
    assert(store.save_progress("Linked/book.epub", first).status == StoreStatus::UnsafePath);

    // A known CRC vector exercises streaming import and committed readback.
    constexpr std::uint32_t import_crc = 0xcbf43926u;
    auto importing = store.begin_import("Books/Imported/check.epub", 9, import_crc, error);
    assert(importing);
    assert(importing->append(0, "1234", 4, error) == StoreStatus::Ok);
    assert(importing->append(4, "56789", 5, error) == StoreStatus::Ok);
    assert(!fs::exists(root / "Books/Imported/check.epub"));
    assert(importing->finish(error) == StoreStatus::Ok);
    importing.reset();
    std::uint64_t asset_size = 0;
    std::uint32_t asset_crc = 0;
    assert(store.inspect_asset("Books/Imported/check.epub", asset_size, asset_crc, error) == StoreStatus::Ok);
    assert(asset_size == 9 && asset_crc == import_crc);
    auto txt_import = store.begin_import("Books/Imported/note.txt", 9, import_crc, error);
    assert(txt_import && txt_import->append(0, "123456789", 9, error) == StoreStatus::Ok);
    assert(txt_import->finish(error) == StoreStatus::Ok);
    txt_import.reset();
    assert(!store.begin_import("Books/Imported/check.epub", 9, import_crc, error));
    assert(get(root / "Books/Imported/check.epub") == "123456789");
    assert(!store.begin_import("../outside.epub", 9, import_crc, error));
    assert(!store.begin_import("/Books/outside.epub", 9, import_crc, error));
    assert(!store.begin_import("PicoPage/network.toml", 9, import_crc, error));
    assert(!store.begin_import("Books/zero.epub", 0, 0, error));
    assert(!store.begin_import("Books/large.epub", 64 * 1024 * 1024 + 1, 0, error));
    fs::create_directory_symlink(root / "outside", root / "Books/linked");
    assert(!store.begin_import("Books/linked/book.epub", 9, import_crc, error));
    assert(store.inspect_asset("PicoPage/network.toml", asset_size, asset_crc, error) == StoreStatus::UnsafePath);
    importing = store.begin_import("Fonts/reader.ttf", 9, import_crc, error);
    assert(importing && importing->append(0, "123456789", 9, error) == StoreStatus::Ok);
    assert(importing->finish(error) == StoreStatus::Ok);
    importing.reset();
    assert(!store.begin_import("Articles/old.epub", 9, import_crc, error));
    assert(!store.begin_import("PicoPage/fonts/reader.ttf", 9, import_crc, error));
    assert(!store.begin_import("PicoPage/images/cover.png", 9, import_crc, error));
    importing = store.begin_import("Books/Imported/bad.epub", 9, import_crc, error);
    assert(importing && importing->append(0, "123456788", 9, error) == StoreStatus::Ok);
    assert(importing->finish(error) == StoreStatus::Invalid);
    assert(importing->append(9, "9", 1, error) == StoreStatus::Invalid);
    importing.reset();
    assert(!fs::exists(root / "Books/Imported/bad.epub") && !fs::exists(root / "Books/Imported/bad.epub.import.tmp"));
    importing = store.begin_import("Books/interrupted.epub", 9, import_crc, error);
    assert(importing && importing->append(1, "1", 1, error) == StoreStatus::Invalid);
    assert(importing->finish(error) == StoreStatus::Invalid);
    importing.reset();
    assert(!fs::exists(root / "Books/interrupted.epub.import.tmp"));
    importing = store.begin_import("Books/readback.epub", 9, import_crc, error);
    assert(importing && importing->append(0, "123456789", 9, error) == StoreStatus::Ok);
    put(root / "Books/readback.epub.import.tmp", "987654321");
    assert(importing->finish(error) == StoreStatus::IoError);
    importing.reset();
    assert(!fs::exists(root / "Books/readback.epub"));
    put(root / "Books/stale.epub.import.tmp", "incomplete transfer");
    importing = store.begin_import("Books/stale.epub", 9, import_crc, error);
    assert(importing && fs::file_size(root / "Books/stale.epub.import.tmp") == 0);
    assert(importing->append(0, "123456789", 9, error) == StoreStatus::Ok);
    assert(importing->finish(error) == StoreStatus::Ok);
    importing.reset();
    assert(get(root / "Books/stale.epub") == "123456789");
    fs::create_directory_symlink(root / "outside", root / "Books/unsafe.epub.import.tmp");
    assert(!store.begin_import("Books/unsafe.epub", 9, import_crc, error));
    assert(fs::is_symlink(root / "Books/unsafe.epub.import.tmp"));
    importing = store.begin_import("Books/cancel.epub", 9, import_crc, error);
    assert(importing && importing->append(0, "1234", 4, error) == StoreStatus::Ok);
    assert(importing->cancel(error) == StoreStatus::Ok);
    assert(importing->cancel(error) == StoreStatus::Ok);
    assert(!fs::exists(root / "Books/cancel.epub.import.tmp"));
    importing.reset();
    importing = store.begin_import("Books/race.epub", 9, import_crc, error);
    assert(importing && importing->append(0, "123456789", 9, error) == StoreStatus::Ok);
    put(root / "Books/race.epub", "external file");
    assert(importing->finish(error) == StoreStatus::ExternalChange);
    importing.reset();
    assert(get(root / "Books/race.epub") == "external file");

    const std::string managed = "Books/manage.epub";
    const auto managed_progress = root / (managed + ".picopage/progress.json");
    put(root / managed, "managed original");
    assert(store.save_progress(managed, first).record);
    assert(store.save_progress(managed, second).record);
    put(root / (managed + ".picopage/settings.toml"), "preserved settings");
    BookSnapshot book_state;
    assert(store.book_snapshot(managed, book_state) == StoreStatus::Ok);
    put(managed_progress, first_bytes);
    assert(store.manage_book(book_state, false, first.book_id, first.content_revision) == StoreStatus::ExternalChange);
    assert(get(managed_progress) == first_bytes);
    assert(store.book_snapshot(managed, book_state) == StoreStatus::Ok);
    put(root / managed, "external replacement");
    assert(store.manage_book(book_state, true, first.book_id, first.content_revision) == StoreStatus::ExternalChange);
    assert(get(root / managed) == "external replacement");
    put(root / managed, "managed original");
    assert(store.book_snapshot(managed, book_state) == StoreStatus::Ok);
    assert(store.manage_book(book_state, false, first.book_id, first.content_revision) == StoreStatus::Ok);
    assert(store.load_progress(managed, first.book_id, first.content_revision).status == StoreStatus::Cleared);
    const auto cleared = get(managed_progress);
    assert(cleared == get(managed_progress.string() + ".bak"));
    fs::remove(managed_progress);
    assert(store.load_progress(managed, first.book_id, first.content_revision).status == StoreStatus::Cleared);
    put(managed_progress, "{partial");
    assert(store.load_progress(managed, first.book_id, first.content_revision).status == StoreStatus::Cleared);
    assert(store.save_progress(managed, first).record);
    assert(get(managed_progress.string() + ".bak") == cleared);
    fs::remove(managed_progress);
    assert(store.load_progress(managed, first.book_id, first.content_revision).status == StoreStatus::Cleared);
    assert(store.save_progress(managed, first).record);
    assert(store.book_snapshot(managed, book_state) == StoreStatus::Ok);
    assert(store.manage_book(book_state, true, first.book_id, first.content_revision) == StoreStatus::Ok);
    assert(!fs::exists(root / managed));
    assert(get(managed_progress) == first_bytes && get(root / (managed + ".picopage/settings.toml")) == "preserved settings");
    assert(store.book_snapshot("Books/linked/book.epub", book_state) == StoreStatus::UnsafePath);
    assert(store.book_snapshot("PicoPage/config.epub", book_state) == StoreStatus::UnsafePath);

    fs::remove_all(root);
    std::cout << "store: committed recovery, configuration and cache boundaries passed\n";
}
