#pragma once

#include "pico_page/platform_display.h"

namespace pico_page::graphics { class Canvas; }
namespace pico_page::runtime {
/** Check before application initialization or presenting a diagnostic frame; valid in Release. */
bool validate_display_geometry(const graphics::Canvas& canvas, platform::DisplayInfo display, std::string& error);
} // namespace pico_page::runtime
