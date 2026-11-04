#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>

namespace pico_page::document::css {

/** Operation outcome. Unsupported syntax is counted in Report, not a fatal error. */
enum class Result : uint8_t { Complete, Limit, Cancelled, NoMemory, InvalidInput };
enum class Phase : uint8_t { Parse, Match, Cascade, Complete };
/** Checkpoint returns true to cancel. It must not mutate this stylesheet or borrowed elements. */
struct Checkpoint {
    bool (*cancelled)(void*) = nullptr;
    void* context = nullptr;
    /** Optional measurement hook; must not allocate, block or mutate borrowed state. */
    void (*phase_changed)(void*, Phase) = nullptr;
};
/** Per-stylesheet capacities; values above the documented hard ceilings are rejected. */
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
/** Borrowed UTF-8 XML names, case sensitive; classes are separated by ASCII whitespace. */
struct Element {
    std::string_view tag, id, classes;
    const Element* parent = nullptr;
};
enum class Display : uint8_t { Inline, Block, None };
enum class Align : uint8_t { Start, Left, Right, Center, Justify };
enum class Break : uint8_t { Auto, Always, Avoid };
/** Resolved lengths use CSS px. A negative line_height_factor denotes an absolute inherited height. */
struct ComputedStyle {
    float font_px = 16, line_height_px = 19.2f, line_height_factor = 1.2f, indent_px = 0;
    std::array<float, 4> margin_px{}, padding_px{}; // Top, right, bottom, left.
    Display display = Display::Inline;
    Align align = Align::Start;
    Break break_before = Break::Auto, break_after = Break::Auto, break_inside = Break::Auto;
    uint8_t margin_auto = 0;
    bool bold = false, italic = false, underline = false, hidden = false;
};
/** Initial/root sizes and safety bounds use px; percentage spacing uses containing_width_px. */
struct Context {
    float initial_font_px = 16, root_font_px = 16, containing_width_px = 600;
    float minimum_font_px = 6, maximum_font_px = 192;
};
/** Reuse across a batch. Each examined byte, selector step and declaration consumes work. */
struct WorkBudget {
    uint32_t maximum_work = 1000000, maximum_styles = 4096;
    uint32_t work = 0, styles = 0;
};

/**
 * Bounded CSS author-style prototype, owned by one executor (normally content_worker).
 * No filesystem, ZIP, import loader, task or product-rendering side effects.
 * All @ rules, including @import and @media, are skipped in their entirety.
 * Hard ceilings: 256 KiB source, 512 rules, 1024 selectors, 4096 steps,
 * 8192 atoms, 4096 declarations, 32 KiB names. Tokens: 256 bytes;
 * nesting: 32; selector: 8 compounds / 16 atoms per compound; ancestry: 64.
 * Allocation uses nothrow fixed-capacity pools. Input and elements remain caller-owned.
 */
class StyleSheet {
public:
    explicit StyleSheet(Limits limits = {});
    ~StyleSheet();
    StyleSheet(const StyleSheet&) = delete;
    StyleSheet& operator=(const StyleSheet&) = delete;
    /** Append in author source order. Limit/cancellation retains only complete preceding rules. */
    Result append(std::string_view source, Checkpoint checkpoint = {});
    /**
     * Compute one element using its already computed parent (nullptr for root).
     * inline_style is a declaration list, bounded by 4096 bytes and the same token limits.
     * With a valid context, failure returns the initial/inherited style without partial matches.
     * Invalid context returns a default-constructed output.
     * Work/styles budgets are cumulative; invalid context or excessive ancestry is rejected.
     * No allocations occur during compute. The owner must keep all borrowed views valid.
     */
    Result compute(const Element& element, const ComputedStyle* parent, const Context& context,
                   std::string_view inline_style, WorkBudget& budget, ComputedStyle& output,
                   Checkpoint checkpoint = {}) const;
    Report report() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace pico_page::document::css
