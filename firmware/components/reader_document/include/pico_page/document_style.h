#pragma once

#include <array>
#include <cstdint>

namespace pico_page::document::css {
enum class Display : uint8_t { Inline, Block, None };
enum class Align : uint8_t { Start, Left, Right, Center, Justify };
enum class Break : uint8_t { Auto, Always, Avoid };
enum class Hyphens : uint8_t { None, Manual, Auto };
enum class TextLanguage : uint8_t { Unknown, Other, EnglishUS, EnglishGB };
inline constexpr std::size_t kEmbeddedFontCount = 4;
/** Resolved lengths use CSS px. A negative line_height_factor denotes an absolute inherited height. */
struct ComputedStyle {
    static constexpr unsigned indent_percent = 1U << 0, margin_percent = 1U << 1, padding_percent = 1U << 5;
    static constexpr unsigned width_percent = 1U << 9, max_width_percent = 1U << 10;
    float font_px = 16, line_height_px = 19.2f, line_height_factor = 1.2f, indent_px = 0;
    std::array<float, 4> margin_px{}, padding_px{}; // Top, right, bottom, left.
    float width_px = -1, max_width_px = -1;  // Negative denotes an unspecified constraint.
    std::uint16_t box_percentages = 0;  // Selected lengths are percentages of the containing width.
    std::uint16_t font_families = 0;  // One-based face slots in successive low-to-high nibbles.
    Display display = Display::Inline;
    Align align = Align::Start;
    Break break_before = Break::Auto, break_after = Break::Auto, break_inside = Break::Auto;
    uint8_t margin_auto = 0;
    uint8_t bleed = 0;  // Image sides extended to the screen edge, in margin side bit order.
    Hyphens hyphens = Hyphens::Manual;
    TextLanguage language = TextLanguage::Unknown;
    constexpr ComputedStyle() : bold(false), italic(false), underline(false), hidden(false), align_declared(false) {}
    bool operator==(const ComputedStyle& other) const;
    bool bold : 1, italic : 1, underline : 1, hidden : 1, align_declared : 1;
};
}
