#include "display_compatibility.h"
#include "pico_page/graphics.h"

namespace pico_page::runtime {
bool validate_display_geometry(const graphics::Canvas& canvas, platform::DisplayInfo display, std::string& error) {
    error.clear();
    if (display.width > 0 && display.height > 0 &&
        canvas.width() == display.width && canvas.height() == display.height) return true;
    error = "Application canvas dimensions do not match the display";
    return false;
}
} // namespace pico_page::runtime
