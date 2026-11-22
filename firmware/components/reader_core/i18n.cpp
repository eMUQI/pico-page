#include "pico_page/i18n.h"

#include <algorithm>
#include <iterator>
#include <cstring>
#include <cstdint>

namespace pico_page::i18n {
namespace {
struct Translation { const char* source; const char* translated; };
constexpr Translation traditional[] = {
#include "translations.inc"
};
constexpr Translation english[] = {
#include "translations_en.inc"
};
struct LanguageInfo { Language language; const char* code; const char* name; };
constexpr LanguageInfo languages[] = {
    {Language::SimplifiedChinese, "zh-Hans", "简体中文"},
    {Language::TraditionalChinese, "zh-Hant", "繁體中文"},
    {Language::English, "en", "English"},
};
}

const char* language_code(Language language) {
    for (const auto& entry : languages) if (entry.language == language) return entry.code;
    return "zh-Hans";
}

bool parse_language(std::string_view code, Language& language) {
    for (const auto& entry : languages) {
        if (entry.code == code) {
            language = entry.language;
            return true;
        }
    }
    return false;
}

const char* language_name(Language language) {
    for (const auto& entry : languages) if (entry.language == language) return entry.name;
    return "简体中文";
}

const char* text(Language language, const char* source) {
    const Translation* begin;
    const Translation* end;
    switch (language) {
    case Language::TraditionalChinese: begin = std::begin(traditional); end = std::end(traditional); break;
    case Language::English: begin = std::begin(english); end = std::end(english); break;
    default: return source;
    }
    const auto found = std::lower_bound(begin, end, source,
        [](const Translation& entry, const char* key) { return std::strcmp(entry.source, key) < 0; });
    return found != end && std::strcmp(found->source, source) == 0 ? found->translated : source;
}

std::string format(Language language, const char* source, std::initializer_list<Argument> arguments) {
    const std::string_view message = text(language, source);
    if (message.size() > max_formatted_bytes) return {};
    const auto fallback = [&] { return std::string(message); };
    if (arguments.size() > 32) return fallback();
    for (auto first = arguments.begin(); first != arguments.end(); ++first) {
        if (first->name.empty()) return fallback();
        for (auto other = first + 1; other != arguments.end(); ++other)
            if (first->name == other->name) return fallback();
    }
    std::size_t total = 0;
    const auto visit = [&](auto append) {
        std::uint32_t used_arguments = 0;
        std::size_t begin = 0;
        while (begin < message.size()) {
            const auto opening = message.find('{', begin);
            const auto literal_end = opening == std::string_view::npos ? message.size() : opening;
            if (!append(message.substr(begin, literal_end - begin))) return false;
            if (opening == std::string_view::npos) break;
            const auto closing = message.find('}', opening + 1);
            if (closing == std::string_view::npos) return false;
            const auto name = message.substr(opening + 1, closing - opening - 1);
            const auto argument = std::find_if(arguments.begin(), arguments.end(),
                [&](const Argument& candidate) { return candidate.name == name; });
            if (argument == arguments.end() || !append(argument->value)) return false;
            used_arguments |= std::uint32_t{1} << (argument - arguments.begin());
            begin = closing + 1;
        }
        return arguments.size() == 32 ? used_arguments == UINT32_MAX
            : used_arguments == (std::uint32_t{1} << arguments.size()) - 1;
    };
    if (!visit([&](std::string_view value) {
        if (value.size() > max_formatted_bytes - total) return false;
        total += value.size();
        return true;
    })) return fallback();
    std::string result;
    result.reserve(total);
    visit([&](std::string_view value) { result.append(value); return true; });
    return result;
}
}
