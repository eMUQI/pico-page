// Host-only captures of the real ReaderApp canvas. No network or hardware operations.
#include "pico_page/app.h"
#include <cassert>
#include <filesystem>
#include <fstream>
#include <string>
using namespace pico_page;
namespace fs = std::filesystem;
static void frame(ReaderApp& app) { while (app.pending_frame()) app.complete_frame(true); }
static void capture(ReaderApp& app, const fs::path& out) {
    std::ofstream file(out, std::ios::binary);
    file << "P5\n684 1216\n255\n";
    for (int y = 0; y < 1216; ++y) for (int x = 0; x < 684; ++x)
        file.put(static_cast<char>(app.canvas().pixel(x, y) * 17));
    assert(file.good());
}
int main(int argc, char** argv) {
    assert(argc == 4);
    const fs::path card = argv[1], book = argv[2], out = argv[3];
    fs::create_directories(card / "Books"); fs::create_directories(out);
    fs::copy_file(book, card / "Books/book.epub", fs::copy_options::overwrite_existing);
    ReaderApp app(card.string()); assert(app.initialize()); frame(app);
    assert(app.open_book("Books/book.epub")); frame(app);
    app.save_progress(); frame(app); assert(app.saved_record());
    assert(app.show_power_menu()); frame(app); capture(app, out / "menu.pgm");
    std::uint64_t sequence = 0;
    auto tap = [&](int x, int y) {
        ++sequence;
        app.input({{sequence, Phase::Begin, Input::TouchCenter}, true, x, y});
        app.input({{sequence, Phase::End, Input::TouchCenter}, true, x, y});
    };
    const auto recent = card / "PicoPage/data/last-book.txt";
    fs::remove(recent); fs::create_directory(recent);
    tap(300, 520); assert(!app.prepare_power(app.take_power_request())); frame(app);
    capture(app, out / "save-failed.pgm");
    tap(100, 1100); frame(app); fs::remove(recent);
    assert(app.show_power_menu()); frame(app); tap(300, 520);
    assert(app.prepare_power(app.take_power_request())); frame(app);
    capture(app, out / "shutdown.pgm");
    assert(app.close_power_files()); app.show_power_off_help(); frame(app);
    capture(app, out / "manual-off.pgm");
}
