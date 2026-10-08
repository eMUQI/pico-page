// Host-only captures of the real ReaderApp canvas. No network or hardware operations.
#include "pico_page/app.h"
#include <cassert>
#include <filesystem>
#include <fstream>
#include <string>
using namespace pico_page;
namespace fs = std::filesystem;
static void frame(ReaderApp& app) {
    while (app.pending_frame()) app.complete_frame(true);
    if (app.save_due()) app.save_progress();
}
static void capture(ReaderApp& app, const fs::path& out) {
    std::ofstream file(out, std::ios::binary);
    file << "P5\n684 1216\n255\n";
    for (int y = 0; y < 1216; ++y) for (int x = 0; x < 684; ++x)
        file.put(static_cast<char>(app.canvas().pixel(x, y) * 17));
    assert(file.good());
}
int main(int argc, char** argv) {
    assert(argc == 5);
    const fs::path card = argv[1], book = argv[2], font = argv[3], out = argv[4];
    fs::create_directories(card / "Books"); fs::create_directories(card / "Fonts"); fs::create_directories(out);
    fs::copy_file(book, card / "Books/book.epub", fs::copy_options::overwrite_existing);
    fs::copy_file(font, card / "Fonts/LXGWWenKai-Regular.ttf", fs::copy_options::overwrite_existing);
    ReaderApp app(card.string()); assert(app.initialize()); frame(app);
    assert(app.open_book("Books/book.epub")); frame(app);
    std::uint64_t sequence = 0;
    auto tap = [&](int x, int y) {
        ++sequence;
        app.input({{sequence, Phase::Begin, Input::TouchCenter}, true, x, y});
        app.input({{sequence, Phase::End, Input::TouchCenter}, true, x, y});
        frame(app);
    };
    auto key = [&](Input input) {
        ++sequence;
        app.input({{sequence, Phase::Begin, input}, false, 0, 0});
        app.input({{sequence, Phase::End, input}, false, 0, 0});
        frame(app);
    };
    key(Input::K3);
    tap(342, 500); capture(app, out / "tools-add.pgm");
    tap(634, 50); capture(app, out / "tools-remove.pgm");
    tap(342, 500); capture(app, out / "reading-marked.pgm");
    for (int i = 0; i < 3; ++i) key(Input::K3);
    tap(342, 500); tap(634, 50);
    tap(68, 1150); capture(app, out / "contents-toc.pgm");
    tap(500, 486); capture(app, out / "contents-bookmarks.pgm");
}
