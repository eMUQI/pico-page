// Host captures of the real guide UI, with no card files or network connection.
#include "pico_page/app.h"
#include "../../firmware/components/reader_app/help_content.h"
#include <cassert>
#include <filesystem>
#include <fstream>
using namespace pico_page;
namespace fs = std::filesystem;
static void frame(ReaderApp& app) { while (app.pending_frame()) app.complete_frame(true); }
static void capture(ReaderApp& app, const fs::path& path) {
    std::ofstream file(path, std::ios::binary);
    file << "P5\n684 1216\n255\n";
    for (int y = 0; y < 1216; ++y)
        for (int x = 0; x < 684; ++x) file.put(static_cast<char>(app.canvas().pixel(x, y) * 17));
    assert(file.good());
}
int main(int argc, char** argv) {
    assert(argc == 3);
    const fs::path root = argv[1], out = argv[2];
    assert(!fs::exists(root)); fs::create_directories(out);
    ReaderApp app(root.string()); assert(app.initialize()); frame(app);
    std::uint64_t sequence = 0;
    auto tap = [&](int x, int y) {
        ++sequence;
        app.input({{sequence, Phase::Begin, Input::TouchCenter}, true, x, y});
        app.input({{sequence, Phase::End, Input::TouchCenter}, true, x, y});
        frame(app);
    };
    tap(600, 50); assert(app.view() == View::Settings); capture(app, out / "settings.pgm");
    tap(300, 948); assert(app.view() == View::Help);
    for (std::size_t index = 0; index < kHelpPages.size(); ++index) {
        capture(app, out / ("help-" + std::to_string(index + 1) + ".pgm"));
        if (index + 1 < kHelpPages.size()) tap(600, 1160);
    }
    assert(!fs::exists(root) && !app.saved_record() && !app.take_network_request());
}
