#pragma once

#include <cstdint>
#include <string_view>

namespace pico_page::graphics {

enum class EnglishHyphenation : std::uint8_t { None, Us, Gb };

/** Returns a mask with bit i set for an allowed break before word[i].
 * Accepts 5–63 ASCII letters, including uppercase. Other inputs return zero.
 */
std::uint64_t english_hyphenation_breaks(std::u32string_view word, EnglishHyphenation language);

}
