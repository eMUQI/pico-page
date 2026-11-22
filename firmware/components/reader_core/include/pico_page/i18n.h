#pragma once

#include <initializer_list>
#include <string>
#include <string_view>

namespace pico_page::i18n {
enum class Language { SimplifiedChinese, TraditionalChinese, English };

const char* language_code(Language language);
bool parse_language(std::string_view code, Language& language);
const char* language_name(Language language);
/** Translates a project-owned message key. Unknown keys retain their source text. */
const char* text(Language language, const char* source);

struct Argument { std::string_view name; std::string_view value; };
inline constexpr std::size_t max_formatted_bytes = 4096;
/** Substitutes named arguments without interpreting their values. Invalid arguments or output over
 * max_formatted_bytes return the translated template unchanged; an oversized template returns empty. */
std::string format(Language language, const char* source, std::initializer_list<Argument> arguments);
}
