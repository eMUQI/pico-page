#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>

namespace pico_page::document::css {

enum class Result : uint8_t { Complete, Limit, Cancelled, NoMemory, InvalidInput };
enum class Phase : uint8_t { Parse, Match, Cascade, Complete };

struct Checkpoint {
    bool (*cancelled)(void*) = nullptr;
    void* context = nullptr;

    void (*phase_changed)(void*, Phase) = nullptr;
};
struct Limits {
    uint32_t source_bytes = 262144;
    uint16_t rules = 512, selectors = 1024, steps = 4096, atoms = 8192, declarations = 4096;
    uint32_t identifier_bytes = 32768;
};
struct Report {
    uint32_t source_bytes = 0, rules = 0, selectors = 0, declarations = 0;
    uint32_t rejected_rules = 0, rejected_declarations = 0, skipped_at_rules = 0;
    size_t allocated_bytes = 0;
};

struct Element {
    std::string_view tag, id, classes;
    const Element* parent = nullptr;
};
enum class Display : uint8_t { Inline, Block, None };
enum class Align : uint8_t { Start, Left, Right, Center, Justify };
enum class Break : uint8_t { Auto, Always, Avoid };

struct ComputedStyle {
    float font_px = 16, line_height_px = 19.2f, line_height_factor = 1.2f, indent_px = 0;
    std::array<float, 4> margin_px{}, padding_px{};
    Display display = Display::Inline;
    Align align = Align::Start;
    Break break_before = Break::Auto, break_after = Break::Auto, break_inside = Break::Auto;
    uint8_t margin_auto = 0;
    bool bold = false, italic = false, underline = false, hidden = false;
};

struct Context {
    float initial_font_px = 16, root_font_px = 16, containing_width_px = 600;
    float minimum_font_px = 6, maximum_font_px = 192;
};

struct WorkBudget {
    uint32_t maximum_work = 1000000, maximum_styles = 4096;
    uint32_t work = 0, styles = 0;
};

class StyleSheet {
public:
    explicit StyleSheet(Limits limits = {});
    ~StyleSheet();
    StyleSheet(const StyleSheet&) = delete;
    StyleSheet& operator=(const StyleSheet&) = delete;

    Result append(std::string_view source, Checkpoint checkpoint = {});

    Result compute(const Element& element, const ComputedStyle* parent, const Context& context,
                   std::string_view inline_style, WorkBudget& budget, ComputedStyle& output,
                   Checkpoint checkpoint = {}) const;
    Report report() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
