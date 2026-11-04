#pragma once

#include "pico_page/document_style.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
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
struct Context {
    float initial_font_px = 16, root_font_px = 16, containing_width_px = 600;
    float minimum_font_px = 6, maximum_font_px = 192;
    bool defer_box_percentages = false;
};

struct WorkBudget {
    uint32_t maximum_work = 1000000, maximum_styles = 4096;
    uint32_t work = 0, styles = 0;
};
using FontFamilies = std::array<std::string_view, kEmbeddedFontCount>;

class StyleSheet {
public:
    explicit StyleSheet(Limits limits = {});
    ~StyleSheet();
    StyleSheet(const StyleSheet&) = delete;
    StyleSheet& operator=(const StyleSheet&) = delete;
    Result append(std::string_view source, Checkpoint checkpoint = {}, const FontFamilies* families = nullptr);
    Result collect_fonts(std::string_view source, Checkpoint checkpoint,
                        const std::function<bool(std::string_view, std::string_view)>& accept);
    Result compute(const Element& element, const ComputedStyle* parent, const Context& context,
                   std::string_view inline_style, WorkBudget& budget, ComputedStyle& output,
                   Checkpoint checkpoint = {}, const ComputedStyle* defaults = nullptr,
                   const FontFamilies* families = nullptr) const;
    Report report() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
