#include "pico_page/graphics.h"

extern "C" {
#include "qrcodegen.h"
}

#include <string>
#include <vector>

namespace pico_page::graphics {
namespace {
constexpr int max_version = 10;
constexpr int quiet_modules = 4;
constexpr int min_module_px = 3;
}

bool draw_qr(Canvas& canvas, std::string_view text, int x, int y, int size) {
    // The encoder needs a terminated string and two working buffers of this length.
    const std::string terminated(text);
    if (terminated.find('\0') != std::string::npos) return false;
    std::vector<std::uint8_t> work(qrcodegen_BUFFER_LEN_FOR_VERSION(max_version)), code(work.size());
    if (!qrcodegen_encodeText(terminated.c_str(), work.data(), code.data(), qrcodegen_Ecc_MEDIUM,
                              1, max_version, qrcodegen_Mask_AUTO, false)) return false;
    const int modules = qrcodegen_getSize(code.data());
    const int module_px = size / (modules + 2 * quiet_modules);
    if (module_px < min_module_px) return false;
    const int origin = (size - modules * module_px) / 2;
    canvas.fill_rect(x, y, size, size, 15);
    for (int row = 0; row < modules; ++row)
        for (int column = 0; column < modules; ++column)
            if (qrcodegen_getModule(code.data(), column, row))
                canvas.fill_rect(x + origin + column * module_px, y + origin + row * module_px, module_px, module_px, 0);
    return true;
}

}
