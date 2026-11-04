#include "display_compatibility.h"
#include "pico_page/graphics.h"
#include <cstdlib>
#include <iostream>

#ifndef NDEBUG
#error This test must exercise the release startup guard.
#endif

#define CHECK(condition) do { if (!(condition)) { std::cerr << #condition << '\n'; std::exit(1); } } while (false)

int main() {
    pico_page::graphics::Canvas canvas;
    const auto display = pico_page::platform::display_info();
    std::string error;
    CHECK(pico_page::runtime::validate_display_geometry(canvas, display, error));
    CHECK(error.empty());
    CHECK(!pico_page::runtime::validate_display_geometry(canvas, {display.height, display.width}, error));
    CHECK(!error.empty());
    CHECK(!pico_page::runtime::validate_display_geometry(canvas, {display.width - 1, display.height}, error));
    CHECK(!pico_page::runtime::validate_display_geometry(canvas, {display.width, display.height - 1}, error));
    CHECK(!pico_page::runtime::validate_display_geometry(canvas, {0, display.height}, error));
    CHECK(!pico_page::runtime::validate_display_geometry(canvas, {display.width, -1}, error));
    CHECK(pico_page::runtime::validate_display_geometry(canvas, display, error));
    CHECK(error.empty());
}
