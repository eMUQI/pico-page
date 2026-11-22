#pragma once
#include "pico_page/dictionary.h"
#include <array>

namespace pico_page::dictionary {

/** Copyable decoder position, bound to immutable fields for one definition.
 * No source or output pointers are retained. Resume only with the same fields.
 */
struct TextCursor {
    std::size_t field = 0, byte = 0;
    std::size_t scalars = 0;  // Decoded Unicode scalar offset from the definition start.
    std::array<std::uint32_t, 64> tags{};
    std::array<bool, 64> emphasis{};
    unsigned depth = 0;
    std::uint32_t suppressed = 0;
    unsigned pending = 0;
    char32_t last = 0;
    bool degraded = false;
};
struct TextChunk {
    std::u32string text;
    std::vector<bool> emphasis;
    bool end = false;
};
/** Decode up to limit (1..4096) scalars, preserving plain m/t and converting
 * h/g/x to paragraphs, lists and emphasis. Scripts/styles/resources are never
 * executed or loaded. Unknown entities remain literal and mark degraded.
 * Failure leaves cursor unchanged and clears the chunk. Cancellation is checked
 * during tag and text scanning. Chunk boundaries preserve markup state.
 */
Result text_chunk(const std::vector<Field>& fields, TextCursor& cursor,
                  std::size_t limit, TextChunk& chunk, const Stop& stop = {});

}
