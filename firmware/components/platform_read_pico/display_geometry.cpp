#include "pico_page/platform_display.h"
#include "display_geometry.h"

namespace pico_page::platform {
DisplayInfo display_info() { return {width, height}; }
}
