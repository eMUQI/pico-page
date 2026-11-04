#pragma once

#include <cstddef>

namespace pico_page::platform {
inline constexpr int width = 684;
inline constexpr int height = 1216;
inline constexpr std::size_t framebuffer_bytes = width * height / 2;
}
