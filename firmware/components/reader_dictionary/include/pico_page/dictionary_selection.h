#pragma once

#include "pico_page/dictionary.h"
#include <string_view>

namespace pico_page::dictionary {

/** Unicode scalar offsets into the current chapter part; end is exclusive. */
struct Selection { std::size_t begin = 0, end = 0; };
struct Candidate {
    Selection selection;
    std::string word;
    bool inflection = false;
};

/** Selects one Han character or a Latin word around hit, within visible.
 * Soft hyphens remain in the source range. Does not cross paragraph separators
 * or silently shorten words exceeding the 64-scalar query limit.
 */
Result initial_selection(std::u32string_view text, Selection visible,
                         std::size_t hit, Selection& selection);
/** Produces at most 36 bounded UTF-8 candidates; no file or font access.
 * Automatic Han lookup considers all substrings up to eight scalars containing
 * hit, longest first. Latin lookup preserves the original before case and
 * inflection alternatives. Query exact/case candidates across dictionaries
 * before the inflection round. Manual selection produces only its exact text.
 * Failure clears candidates; every word is at most 64 scalars and 255 bytes.
 */
Result selection_candidates(std::u32string_view text, Selection visible,
                            Selection selection, std::size_t hit, bool manual,
                            std::vector<Candidate>& candidates);

enum class SelectionEdge { Begin, End };
/** Moves one edge of selection by one unit within visible; direction is -1 or 1.
 * A unit is a Latin word, another Unicode letter, mark or number, or with by_scalar one Unicode
 * scalar. Spaces and punctuation between units are crossed with the
 * step and never left at the moved edge. Shrinking never empties the selection.
 * Returns NotFound when the edge cannot move, ResourceLimit when the result would
 * exceed the 64-scalar or 255-byte query limit; selection is unchanged on failure.
 */
Result adjust_selection(std::u32string_view text, Selection visible, SelectionEdge edge,
                        int direction, bool by_scalar, Selection& selection);

}
