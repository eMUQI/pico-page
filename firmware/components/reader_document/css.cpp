#include "css.h"

#include <cstdlib>
#include <tuple>
#include <type_traits>
#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#endif

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <new>
#include <utility>

namespace pico_page::document::css {
namespace {
constexpr size_t max_token_bytes = 256, max_tokens = 128, max_nesting = 32;
constexpr size_t max_selector_steps = 8, max_compound_atoms = 16, max_ancestors = 64;
struct PhaseScope {
    Checkpoint checkpoint;
    explicit PhaseScope(Checkpoint value, Phase phase) : checkpoint(value) { change(phase); }
    ~PhaseScope() { change(Phase::Complete); }
    void change(Phase phase) const {
        if (checkpoint.phase_changed) checkpoint.phase_changed(checkpoint.context, phase);
    }
};
bool space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }
bool digit(char c) { return c >= '0' && c <= '9'; }
bool alpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
char lower(char c) { return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c; }
int hex(char c) {
    if (digit(c)) return c - '0';
    c = lower(c);
    return c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}
bool name_start(char c) { return alpha(c) || c == '_' || static_cast<unsigned char>(c) >= 128 || c == '\0'; }
bool name_char(char c) { return name_start(c) || digit(c) || c == '-'; }
struct Text {
    char bytes[max_token_bytes + 1]{};
    size_t size = 0;
    bool push(char c) {
        if (size == max_token_bytes) return false;
        bytes[size++] = c;
        return true;
    }
    bool scalar(uint32_t value) {
        if (!value || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff)) value = 0xfffd;
        if (value < 0x80) return push(static_cast<char>(value));
        if (value < 0x800) return push(static_cast<char>(0xc0 | (value >> 6))) && push(static_cast<char>(0x80 | (value & 63)));
        if (value < 0x10000) return push(static_cast<char>(0xe0 | (value >> 12))) &&
            push(static_cast<char>(0x80 | ((value >> 6) & 63))) && push(static_cast<char>(0x80 | (value & 63)));
        return push(static_cast<char>(0xf0 | (value >> 18))) && push(static_cast<char>(0x80 | ((value >> 12) & 63))) &&
            push(static_cast<char>(0x80 | ((value >> 6) & 63))) && push(static_cast<char>(0x80 | (value & 63)));
    }
    std::string_view view() const { return {bytes, size}; }
};
bool decode(std::string_view raw, Text& output) {
    for (size_t i = 0; i < raw.size();) {
        unsigned char c = raw[i++];
        if (c == '\\') {
            if (i == raw.size()) return false;
            if (hex(raw[i]) >= 0) {
                uint32_t value = 0;
                unsigned count = 0;
                while (i < raw.size() && count < 6 && hex(raw[i]) >= 0) {
                    value = value * 16 + hex(raw[i++]);
                    ++count;
                }
                if (i < raw.size() && space(raw[i])) {
                    const char first = raw[i++];
                    if (first == '\r' && i < raw.size() && raw[i] == '\n') ++i;
                }
                if (!output.scalar(value)) return false;
            } else {
                c = raw[i++];
                if (c == '\n' || c == '\f' || c == '\r') {
                    if (c == '\r' && i < raw.size() && raw[i] == '\n') ++i;
                    continue;
                }
                if (!output.push(static_cast<char>(c))) return false;
            }
        } else if (!c) {
            if (!output.scalar(0xfffd)) return false;
        } else if (!output.push(static_cast<char>(c))) return false;
    }
    return true;
}
enum class Kind : uint8_t { End, Space, Ident, Number, Dimension, Percent, String, Url, Bad, Delimiter };
struct Token {
    Kind kind = Kind::End;
    char delimiter = 0;
    std::string_view raw, unit;
    float number = 0;
};
bool is(const Token& token, char value) { return token.kind == Kind::Delimiter && token.delimiter == value; }
bool keyword(const Token& token, std::string_view value) {
    if (token.kind != Kind::Ident) return false;
    Text text;
    if (!decode(token.raw, text) || text.size != value.size()) return false;
    for (size_t i = 0; i < text.size; ++i) if (lower(text.bytes[i]) != value[i]) return false;
    return true;
}
class Scanner {
public:
    Scanner(std::string_view source, Checkpoint checkpoint) : source_(source), checkpoint_(checkpoint) {
        if (source_.substr(0, 3) == "\xef\xbb\xbf") position_ = 3;
        check();
    }
    Result result = Result::Complete;
    Token next() {
        for (;;) {
            if (position_ == source_.size() || result != Result::Complete) return {};
            if (peek() == '/' && peek(1) == '*') {
                advance(); advance();
                while (position_ < source_.size() && !(peek() == '*' && peek(1) == '/')) advance();
                if (position_ < source_.size()) { advance(); advance(); }
            } else break;
        }
        Token token;
        const size_t begin = position_;
        const char c = peek();
        if (space(c)) {
            do { advance(); } while (position_ < source_.size() && space(peek()));
            token.kind = Kind::Space;
        } else if (c == '\'' || c == '"') {
            advance();
            const size_t start = position_;
            token.kind = Kind::String;
            while (position_ < source_.size() && peek() != c) {
                if (peek() == '\n' || peek() == '\r' || peek() == '\f') { token.kind = Kind::Bad; break; }
                if (peek() == '\\') {
                    advance();
                    if (position_ < source_.size()) {
                        const char escaped = peek();
                        advance();
                        if (escaped == '\r' && peek() == '\n') advance();
                    }
                } else advance();
            }
            token.raw = source_.substr(start, position_ - start);
            if (peek() == c && position_ < source_.size()) advance();
            return bounded(token);
        } else if (starts_number()) {
            if (peek() == '+' || peek() == '-') advance();
            while (position_ < source_.size() && digit(peek())) advance();
            if (peek() == '.' && digit(peek(1))) {
                advance();
                while (position_ < source_.size() && digit(peek())) advance();
            }
            if (lower(peek()) == 'e' && (digit(peek(1)) || ((peek(1) == '+' || peek(1) == '-') && digit(peek(2))))) {
                advance();
                if (peek() == '+' || peek() == '-') advance();
                while (position_ < source_.size() && digit(peek())) advance();
            }
            const size_t number_end = position_;
            if (number_end - begin <= max_token_bytes) {
                Text number;
                for (size_t i = begin; i < number_end; ++i) number.push(source_[i]);
                token.number = std::strtof(number.bytes, nullptr);
            }
            token.kind = Kind::Number;
            if (starts_ident()) { token.kind = Kind::Dimension; token.unit = consume_name(); }
            else if (peek() == '%') { token.kind = Kind::Percent; advance(); }
            token.raw = source_.substr(begin, position_ - begin);
            return bounded(token);
        } else if (starts_ident()) {
            token.kind = Kind::Ident;
            token.raw = consume_name();
            if (keyword(token, "url") && peek() == '(') {
                size_t lookahead = position_ + 1;
                while (lookahead < source_.size() && space(source_[lookahead])) {
                    ++lookahead;
                    if (lookahead % 256 == 0) check();
                    if (result != Result::Complete) return {};
                }
                if (lookahead == source_.size() || (source_[lookahead] != '\'' && source_[lookahead] != '"')) {
                    token.kind = consume_url() ? Kind::Url : Kind::Bad;
                    token.raw = source_.substr(begin, position_ - begin);
                }
            }
            return bounded(token);
        } else {
            token.kind = Kind::Delimiter;
            token.delimiter = c;
            advance();
        }
        token.raw = source_.substr(begin, position_ - begin);
        return bounded(token);
    }
private:
    std::string_view source_;
    Checkpoint checkpoint_;
    size_t position_ = 0, next_check_ = 256;
    char peek(size_t offset = 0) const { return offset < source_.size() - position_ ? source_[position_ + offset] : '\0'; }
    void check() { if (checkpoint_.cancelled && checkpoint_.cancelled(checkpoint_.context)) result = Result::Cancelled; }
    void advance() {
        if (position_ < source_.size()) ++position_;
        if (position_ >= next_check_) { check(); next_check_ = position_ + 256; }
        if (result != Result::Complete) position_ = source_.size();
    }
    bool escape(size_t offset = 0) const {
        return peek(offset) == '\\' && position_ + offset + 1 < source_.size() &&
            peek(offset + 1) != '\n' && peek(offset + 1) != '\r' && peek(offset + 1) != '\f';
    }
    bool starts_ident() const {
        return (position_ < source_.size() && name_start(peek())) || escape() ||
            (peek() == '-' && (name_start(peek(1)) || peek(1) == '-' || escape(1)) && position_ + 1 < source_.size());
    }
    bool starts_number() const {
        if (digit(peek())) return true;
        if (peek() == '.') return digit(peek(1));
        return (peek() == '+' || peek() == '-') && (digit(peek(1)) || (peek(1) == '.' && digit(peek(2))));
    }
    void consume_escape() {
        advance();
        unsigned count = 0;
        while (position_ < source_.size() && count < 6 && hex(peek()) >= 0) { advance(); ++count; }
        if (count && position_ < source_.size() && space(peek())) {
            const char whitespace = peek();
            advance();
            if (whitespace == '\r' && peek() == '\n') advance();
        } else if (!count) advance();
    }
    bool consume_url() {
        advance();
        while (position_ < source_.size() && space(peek())) advance();
        bool valid = true;
        while (position_ < source_.size()) {
            const unsigned char c = peek();
            if (c == ')') { advance(); break; }
            if (c == '\\' && escape()) { consume_escape(); continue; }
            if (valid && space(static_cast<char>(c))) {
                while (position_ < source_.size() && space(peek())) advance();
                if (position_ < source_.size() && peek() != ')') valid = false;
                continue;
            }
            if (c == '\'' || c == '"' || c == '(' || c == '\\' || (c && c < 32) || c == 127) valid = false;
            // CSS URL tokens contain comment markers and block delimiters as ordinary data.
            advance();
        }
        return valid;
    }
    std::string_view consume_name() {
        const size_t start = position_;
        while (position_ < source_.size()) {
            if (name_char(peek())) advance();
            else if (escape()) consume_escape();
            else break;
        }
        return source_.substr(start, position_ - start);
    }
    Token bounded(Token token) {
        if (token.raw.size() > max_token_bytes) token.kind = Kind::Bad;
        return token;
    }
};

template<class T> struct PoolDelete {
    void operator()(T* data) const {
#ifdef ESP_PLATFORM
        std::free(data);
#else
        delete[] data;
#endif
    }
};

template<class T> T* allocate_pool(std::size_t count) {
    static_assert(std::is_trivially_destructible_v<T>);
#ifdef ESP_PLATFORM
    void* storage = heap_caps_malloc(count * sizeof(T), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return storage ? new(storage) T[count] : nullptr;
#else
    return new(std::nothrow) T[count];
#endif
}

template<class T> struct Pool {
    std::unique_ptr<T[], PoolDelete<T>> data;
    uint32_t size = 0, capacity = 0;
    bool allocate(uint32_t count) {
        capacity = count;
        if (!count) return true;
        data.reset(allocate_pool<T>(count));
        return data != nullptr;
    }
    bool push(const T& value) {
        if (size == capacity) return false;
        data[size++] = value;
        return true;
    }
    T& operator[](size_t index) { return data[index]; }
    const T& operator[](size_t index) const { return data[index]; }
};
enum class Property : uint8_t {
    FontSize, LineHeight, Indent, MarginTop, MarginRight, MarginBottom, MarginLeft,
    PaddingTop, PaddingRight, PaddingBottom, PaddingLeft, Weight, FontStyle, Align, Hyphens,
    Display, Visibility, Decoration, BreakBefore, BreakAfter, BreakInside, Width, MaxWidth, FontFamily, Bleed, Count
};
constexpr size_t property_count = static_cast<size_t>(Property::Count);
enum class Unit : uint8_t { Number, Px, Em, Rem, Percent, Pt, Keyword };
enum class Word : uint8_t { None, Initial, Inherit, Unset, Normal, Auto, Bold, Italic,
    Start, Left, Right, Center, Justify, Inline, Block, Hidden, Visible, Underline, Always, Avoid, Manual };
struct Value { float number = 0; Unit unit = Unit::Number; Word word = Word::None; };
struct Declaration { Value value; Property property = Property::FontSize; bool important = false; };
struct Atom { uint16_t offset = 0, bytes = 0; char kind = 0; };
struct Step { uint16_t first = 0; uint8_t count = 0; char relation = 0; };
struct Selector { uint32_t specificity = 0; uint16_t first = 0; uint8_t count = 0; };
struct Rule { uint16_t first_selector = 0, selectors = 0, first_declaration = 0, declarations = 0; };
struct TokenList {
    std::array<Token, max_tokens> values;
    size_t size = 0;
    bool valid = true;
    void clear() { size = 0; valid = true; }
};

bool opening(char c) { return c == '{' || c == '(' || c == '['; }
char closing(char c) { return c == '{' ? '}' : c == '(' ? ')' : ']'; }
Token collect(Scanner& scanner, Token first, TokenList& list, bool declaration) {
    std::array<char, max_nesting> stack{};
    size_t depth = 0;
    Token token = first;
    while (token.kind != Kind::End) {
        if (!depth && ((declaration && (is(token, ';') || is(token, '}'))) ||
                       (!declaration && (is(token, '{') || is(token, ';') || is(token, '}'))))) return token;
        if (token.kind == Kind::Delimiter) {
            const char c = token.delimiter;
            if (opening(c)) {
                if (depth == stack.size()) { scanner.result = Result::Limit; return {}; }
                stack[depth++] = closing(c);
            } else if (c == '}' || c == ')' || c == ']') {
                if (!depth || stack[depth - 1] != c) list.valid = false;
                else --depth;
            }
        }
        if (token.kind == Kind::Bad) list.valid = false;
        if (list.size < list.values.size()) list.values[list.size++] = token;
        else list.valid = false;
        token = scanner.next();
    }
    if (depth) list.valid = false;
    return token;
}
void skip_block(Scanner& scanner) {
    std::array<char, max_nesting> stack{};
    size_t depth = 1;
    stack[0] = '}';
    while (depth && scanner.result == Result::Complete) {
        const auto token = scanner.next();
        if (token.kind == Kind::End) return;
        if (token.kind != Kind::Delimiter) continue;
        const char c = token.delimiter;
        if (opening(c)) {
            if (depth == stack.size()) { scanner.result = Result::Limit; return; }
            stack[depth++] = closing(c);
        } else if (c == stack[depth - 1]) --depth;
    }
}
Token collect_declaration(Scanner& scanner, Token first, TokenList& list) {
    const bool at_rule = is(first, '@');
    const auto terminator = collect(scanner, first, list, !at_rule);
    if (at_rule) {
        list.clear();
        if (is(terminator, '{')) skip_block(scanner);
    }
    return terminator;
}
Token nonspace(Scanner& scanner) {
    Token token;
    do { token = scanner.next(); } while (token.kind == Kind::Space);
    return token;
}

std::size_t compact_tokens(TokenList& list) {
    std::size_t count = 0;
    for (std::size_t i = 0; i < list.size; ++i)
        if (list.values[i].kind != Kind::Space) list.values[count++] = list.values[i];
    return count;
}
bool family_name(const Token* tokens, std::size_t count, Text& family) {
    if (!count) return false;
    if (count == 1 && tokens[0].kind == Kind::String) {
        if (!decode(tokens[0].raw, family)) return false;
    } else for (std::size_t i = 0; i < count; ++i) {
        if (tokens[i].kind != Kind::Ident || (i && !family.push(' ')) || !decode(tokens[i].raw, family)) return false;
    }
    for (std::size_t i = 0; i < family.size; ++i) family.bytes[i] = lower(family.bytes[i]);
    return family.size != 0;
}
bool generic_family(std::string_view family) {
    constexpr std::string_view generic[] = {"serif", "sans-serif", "monospace", "cursive", "fantasy",
        "system-ui", "ui-serif", "ui-sans-serif", "ui-monospace", "ui-rounded", "math", "emoji", "fangsong"};
    return std::find(std::begin(generic), std::end(generic), family) != std::end(generic);
}
bool family_value(const Token* tokens, std::size_t count, const FontFamilies& families, Value& value) {
    if (count == 1) {
        for (const auto word : {Word::Initial, Word::Inherit, Word::Unset}) {
            const auto name = word == Word::Initial ? "initial" : word == Word::Inherit ? "inherit" : "unset";
            if (keyword(tokens[0], name)) { value = {0, Unit::Keyword, word}; return true; }
        }
    }
    std::uint16_t slots = 0;
    unsigned shift = 0;
    bool generic = false;
    for (std::size_t first = 0;;) {
        std::size_t end = first;
        while (end < count && !is(tokens[end], ',')) ++end;
        Text family;
        if (!family_name(tokens + first, end - first, family)) return false;
        generic |= tokens[first].kind != Kind::String && generic_family(family.view());
        if (!generic && shift < 16) {
            const auto found = std::find(families.begin(), families.end(), family.view());
            if (found != families.end()) {
                slots |= static_cast<std::uint16_t>((found - families.begin() + 1) << shift);
                shift += 4;
            }
        }
        if (end == count) break;
        first = end + 1;
    }
    value.number = slots;
    return true;
}

Value bleed_value(const Token* tokens, std::size_t count) {
    unsigned sides = 0;
    for (std::size_t i = 0; i < count; ++i) {
        if (tokens[i].kind != Kind::Ident) continue;
        Text name;
        if (!decode(tokens[i].raw, name)) continue;
        for (std::size_t j = 0; j < name.size; ++j) name.bytes[j] = lower(name.bytes[j]);
        const auto view = name.view();
        constexpr std::string_view side_names[] = {"top", "right", "bottom", "left"};
        for (unsigned side = 0; side < 4; ++side)
            if (view.find(side_names[side]) != std::string_view::npos) sides |= 1U << side;
    }
    return {static_cast<float>(sides)};
}
constexpr std::string_view property_names[] = {
    "font-size", "line-height", "text-indent", "margin-top", "margin-right", "margin-bottom", "margin-left",
    "padding-top", "padding-right", "padding-bottom", "padding-left", "font-weight", "font-style", "text-align", "hyphens",
    "display", "visibility", "text-decoration", "page-break-before", "page-break-after", "page-break-inside", "width", "max-width", "font-family"
};
constexpr std::string_view word_names[] = {
    "none", "initial", "inherit", "unset", "normal", "auto", "bold", "italic", "start", "left", "right",
    "center", "justify", "inline", "block", "hidden", "visible", "underline", "always", "avoid", "manual"
};
bool length_value(const Token& token, Value& value) {
    value.number = token.number;
    if (!std::isfinite(value.number) || std::fabs(value.number) > 1000000) return false;
    if (token.kind == Kind::Number) { value.unit = Unit::Number; return true; }
    if (token.kind == Kind::Percent) { value.unit = Unit::Percent; return true; }
    if (token.kind != Kind::Dimension) return false;
    Token unit; unit.kind = Kind::Ident; unit.raw = token.unit;
    constexpr std::string_view units[] = {"px", "em", "rem", "pt"};
    constexpr Unit values[] = {Unit::Px, Unit::Em, Unit::Rem, Unit::Pt};
    for (size_t i = 0; i < 4; ++i) if (keyword(unit, units[i])) { value.unit = values[i]; return true; }
    return false;
}
bool property_value(Property property, const Token& token, Value& value) {
    if (token.kind == Kind::Ident) {
        for (size_t i = 0; i < std::size(word_names); ++i) {
            if (!keyword(token, word_names[i])) continue;
            value.unit = Unit::Keyword; value.word = static_cast<Word>(i);
            if (value.word == Word::Initial || value.word == Word::Inherit || value.word == Word::Unset) return true;
            switch (property) {
            case Property::LineHeight: case Property::FontStyle: return value.word == Word::Normal ||
                (property == Property::FontStyle && value.word == Word::Italic);
            case Property::Weight: return value.word == Word::Bold || value.word == Word::Normal;
            case Property::Align: return value.word >= Word::Start && value.word <= Word::Justify;
            case Property::Hyphens: return value.word == Word::None || value.word == Word::Manual || value.word == Word::Auto;
            case Property::Display: return value.word == Word::None || value.word == Word::Inline || value.word == Word::Block;
            case Property::Visibility: return value.word == Word::Hidden || value.word == Word::Visible;
            case Property::Decoration: return value.word == Word::None || value.word == Word::Underline;
            case Property::BreakBefore: case Property::BreakAfter:
                return value.word == Word::Auto || value.word == Word::Always || value.word == Word::Avoid;
            case Property::BreakInside: return value.word == Word::Auto || value.word == Word::Avoid;
            case Property::Width: return value.word == Word::Auto;
            case Property::MaxWidth: return value.word == Word::None;
            default: return property >= Property::MarginTop && property <= Property::MarginLeft && value.word == Word::Auto;
            }
        }
        return false;
    }
    if (property == Property::Weight) {
        if (token.kind != Kind::Number || token.number < 100 || token.number > 900 ||
            std::fmod(token.number, 100) != 0) return false;
        value = {0, Unit::Keyword, token.number >= 600 ? Word::Bold : Word::Normal};
        return true;
    }
    if ((property > Property::PaddingLeft && property < Property::Width) || !length_value(token, value)) return false;
    if (value.unit == Unit::Number && value.number != 0 && property != Property::LineHeight) return false;
    if ((property == Property::FontSize || property == Property::LineHeight || property >= Property::PaddingTop) && value.number < 0) return false;
    return property != Property::FontSize || value.number > 0;
}
size_t declarations(TokenList& list, std::array<Declaration, 4>& output, const FontFamilies* families) {
    if (!list.valid) return 0;
    auto& tokens = list.values;
    size_t count = compact_tokens(list);
    if (count < 3 || tokens[0].kind != Kind::Ident || !is(tokens[1], ':')) return 0;
    bool important = false;
    if (count >= 5 && is(tokens[count - 2], '!') && keyword(tokens[count - 1], "important")) { count -= 2; important = true; }
    if (keyword(tokens[0], "bleed") || keyword(tokens[0], "duokan-bleed") || keyword(tokens[0], "qrbleed")) {
        output[0] = {bleed_value(tokens.data() + 2, count - 2), Property::Bleed, important};
        return 1;
    }
    if (families && keyword(tokens[0], "font-family")) {
        output[0].property = Property::FontFamily;
        output[0].important = important;
        return family_value(tokens.data() + 2, count - 2, *families, output[0].value) ? 1 : 0;
    }
    const bool margin = keyword(tokens[0], "margin"), padding = keyword(tokens[0], "padding");
    if (margin || padding) {
        const size_t values = count - 2;
        if (!values || values > 4) return 0;
        const auto first = margin ? Property::MarginTop : Property::PaddingTop;
        std::array<Value, 4> parsed{};
        for (size_t i = 0; i < values; ++i) if (!property_value(first, tokens[i + 2], parsed[i])) return 0;
        if (values > 1) for (size_t i = 0; i < values; ++i)
            if (parsed[i].unit == Unit::Keyword && parsed[i].word != Word::Auto) return 0;
        const size_t indices[] = {0, values > 1 ? 1U : 0U, values > 2 ? 2U : 0U, values > 3 ? 3U : (values > 1 ? 1U : 0U)};
        for (size_t i = 0; i < 4; ++i) output[i] = {parsed[indices[i]], static_cast<Property>(static_cast<size_t>(first) + i), important};
        return 4;
    }
    if (count != 3) return 0;
    for (size_t i = 0; i < static_cast<size_t>(Property::FontFamily); ++i) {
        if (keyword(tokens[0], property_names[i]) && property_value(static_cast<Property>(i), tokens[2], output[0].value)) {
            output[0].property = static_cast<Property>(i); output[0].important = important; return 1;
        }
    }
    return 0;
}
bool inherited(Property property) {
    return property == Property::FontSize || property == Property::LineHeight || property == Property::Indent ||
        property == Property::Weight || property == Property::FontStyle || property == Property::Align ||
        property == Property::Visibility || property == Property::FontFamily || property == Property::Bleed ||
        property == Property::Hyphens;
}
ComputedStyle base_style(const ComputedStyle* parent, const Context& context) {
    ComputedStyle result;
    result.font_px = context.initial_font_px;
    if (parent) {
        result.font_px = parent->font_px; result.line_height_factor = parent->line_height_factor;
        result.font_families = parent->font_families; result.bleed = parent->bleed;
        result.line_height_px = parent->line_height_px; result.indent_px = parent->indent_px;
        result.box_percentages = parent->box_percentages & ComputedStyle::indent_percent;
        result.bold = parent->bold; result.italic = parent->italic; result.align = parent->align; result.hidden = parent->hidden;
        result.align_declared = parent->align_declared;
        result.hyphens = parent->hyphens;
        result.language = parent->language;
    }
    result.font_px = std::clamp(result.font_px, context.minimum_font_px, context.maximum_font_px);
    if (result.line_height_factor >= 0) result.line_height_px = result.font_px * result.line_height_factor;
    return result;
}
float pixels(Value value, float em, float root, float percent) {
    switch (value.unit) {
    case Unit::Em: return value.number * em;
    case Unit::Rem: return value.number * root;
    case Unit::Percent: return value.number * percent / 100;
    case Unit::Pt: return value.number * (96.0f / 72.0f);
    default: return value.number;
    }
}
struct Winner { Value value; uint32_t specificity = 0; bool set = false, important = false; };
void consider(std::array<Winner, property_count>& winners, const Declaration& declaration, uint32_t specificity) {
    auto& winner = winners[static_cast<size_t>(declaration.property)];
    if (!winner.set || declaration.important > winner.important ||
        (declaration.important == winner.important && specificity >= winner.specificity))
        winner = {declaration.value, specificity, true, declaration.important};
}
Value computed_value(Property property, const ComputedStyle& style) {
    if (property == Property::FontSize) return {style.font_px, Unit::Px};
    if (property == Property::FontFamily) return {static_cast<float>(style.font_families)};
    if (property == Property::Bleed) return {static_cast<float>(style.bleed)};
    if (property == Property::LineHeight) return style.line_height_factor >= 0 ? Value{style.line_height_factor} : Value{style.line_height_px, Unit::Px};
    const auto box_value = [&](float value, unsigned bit) {
        return Value{value, style.box_percentages & bit ? Unit::Percent : Unit::Px};
    };
    if (property == Property::Indent) return box_value(style.indent_px, ComputedStyle::indent_percent);
    if (property == Property::Width) return style.width_px < 0 ? Value{0, Unit::Keyword, Word::Auto}
        : box_value(style.width_px, ComputedStyle::width_percent);
    if (property == Property::MaxWidth) return style.max_width_px < 0 ? Value{0, Unit::Keyword, Word::None}
        : box_value(style.max_width_px, ComputedStyle::max_width_percent);
    const auto index = static_cast<size_t>(property);
    if (property >= Property::MarginTop && property <= Property::MarginLeft) {
        const auto side = index - static_cast<size_t>(Property::MarginTop);
        return (style.margin_auto & (1U << side)) ? Value{0, Unit::Keyword, Word::Auto} : box_value(style.margin_px[side], ComputedStyle::margin_percent << side);
    }
    if (property >= Property::PaddingTop && property <= Property::PaddingLeft) {
        const auto side = index - static_cast<size_t>(Property::PaddingTop);
        return box_value(style.padding_px[side], ComputedStyle::padding_percent << side);
    }
    Word word = Word::None;
    switch (property) {
    case Property::Weight: word = style.bold ? Word::Bold : Word::Normal; break;
    case Property::FontStyle: word = style.italic ? Word::Italic : Word::Normal; break;
    case Property::Align: word = static_cast<Word>(static_cast<unsigned>(Word::Start) + static_cast<unsigned>(style.align)); break;
    case Property::Hyphens: word = style.hyphens == Hyphens::None ? Word::None : style.hyphens == Hyphens::Auto ? Word::Auto : Word::Manual; break;
    case Property::Display: word = style.display == Display::None ? Word::None : style.display == Display::Block ? Word::Block : Word::Inline; break;
    case Property::Visibility: word = style.hidden ? Word::Hidden : Word::Visible; break;
    case Property::Decoration: word = style.underline ? Word::Underline : Word::None; break;
    case Property::BreakBefore: case Property::BreakAfter: case Property::BreakInside: {
        const auto rule = property == Property::BreakBefore ? style.break_before : property == Property::BreakAfter ? style.break_after : style.break_inside;
        word = rule == Break::Always ? Word::Always : rule == Break::Avoid ? Word::Avoid : Word::Auto;
        break;
    }
    default: break;
    }
    return {0, Unit::Keyword, word};
}
void resolve(const std::array<Winner, property_count>& winners, const ComputedStyle* parent, const Context& context, ComputedStyle& result) {
    const auto initial = base_style(nullptr, context);
    const auto box_length = [&](Value value, unsigned bit) {
        result.box_percentages &= ~bit;
        if (context.defer_box_percentages && value.unit == Unit::Percent) {
            result.box_percentages |= bit;
            return value.number;
        }
        return pixels(value, result.font_px, context.root_font_px, context.containing_width_px);
    };
    for (size_t i = 0; i < property_count; ++i) {
        if (!winners[i].set) continue;
        const auto property = static_cast<Property>(i);
        auto value = winners[i].value;
        if (property == Property::Align) {
            const bool inherits_alignment = value.word == Word::Inherit || value.word == Word::Unset;
            result.align_declared = !inherits_alignment || (parent && parent->align_declared);
        }
        if (value.unit == Unit::Keyword && (value.word == Word::Initial || value.word == Word::Inherit || value.word == Word::Unset)) {
            const bool use_parent = value.word == Word::Inherit || (value.word == Word::Unset && inherited(property));
            value = computed_value(property, use_parent && parent ? *parent : initial);
        }
        if (property == Property::FontSize) {
            result.font_px = std::clamp(pixels(value, parent ? parent->font_px : context.initial_font_px,
                                              context.root_font_px, parent ? parent->font_px : context.initial_font_px),
                                       context.minimum_font_px, context.maximum_font_px);
            if (result.line_height_factor >= 0) result.line_height_px = result.font_px * result.line_height_factor;
        } else if (property == Property::LineHeight) {
            result.line_height_factor = value.unit == Unit::Number ? value.number : value.unit == Unit::Keyword ? 1.2f : -1;
            result.line_height_px = result.line_height_factor >= 0 ? result.font_px * result.line_height_factor :
                pixels(value, result.font_px, context.root_font_px, result.font_px);
        } else if (property == Property::Indent) result.indent_px = box_length(value, ComputedStyle::indent_percent);
        else if (property == Property::Width) result.width_px = value.unit == Unit::Keyword ? -1 : box_length(value, ComputedStyle::width_percent);
        else if (property == Property::MaxWidth) result.max_width_px = value.unit == Unit::Keyword ? -1 : box_length(value, ComputedStyle::max_width_percent);
        else if (property >= Property::MarginTop && property <= Property::MarginLeft) {
            const auto side = i - static_cast<size_t>(Property::MarginTop);
            result.margin_px[side] = box_length(value, ComputedStyle::margin_percent << side);
            if (value.unit == Unit::Keyword && value.word == Word::Auto) result.margin_auto |= 1U << side;
        } else if (property >= Property::PaddingTop && property <= Property::PaddingLeft) {
            const auto side = i - static_cast<size_t>(Property::PaddingTop);
            result.padding_px[side] = box_length(value, ComputedStyle::padding_percent << side);
        } else {
            switch (property) {
            case Property::FontFamily: result.font_families = static_cast<std::uint16_t>(value.number); break;
            case Property::Bleed: result.bleed = static_cast<std::uint8_t>(value.number); break;
            case Property::Weight: result.bold = value.word == Word::Bold; break;
            case Property::FontStyle: result.italic = value.word == Word::Italic; break;
            case Property::Align: result.align = static_cast<Align>(static_cast<unsigned>(value.word) - static_cast<unsigned>(Word::Start)); break;
            case Property::Hyphens: result.hyphens = value.word == Word::None ? Hyphens::None : value.word == Word::Auto ? Hyphens::Auto : Hyphens::Manual; break;
            case Property::Display: result.display = value.word == Word::None ? Display::None : value.word == Word::Block ? Display::Block : Display::Inline; break;
            case Property::Visibility: result.hidden = value.word == Word::Hidden; break;
            case Property::Decoration: result.underline = value.word == Word::Underline; break;
            case Property::BreakBefore: case Property::BreakAfter: case Property::BreakInside: {
                const auto rule = value.word == Word::Always ? Break::Always : value.word == Word::Avoid ? Break::Avoid : Break::Auto;
                if (property == Property::BreakBefore) result.break_before = rule;
                else if (property == Property::BreakAfter) result.break_after = rule;
                else result.break_inside = rule;
                break;
            }
            default: break;
            }
        }
    }
}
class Matching {
public:
    Matching(WorkBudget& budget, Checkpoint checkpoint) : budget_(budget), checkpoint_(checkpoint) { take(0); }
    Result result = Result::Complete;
    bool take(uint32_t work = 1) {
        if (result != Result::Complete) return false;
        if (budget_.work > budget_.maximum_work || work > budget_.maximum_work - budget_.work) { result = Result::Limit; return false; }
        budget_.work += work;
        if (checkpoint_.cancelled && (work == 0 || budget_.work - checked_ >= 64)) {
            checked_ = budget_.work;
            if (checkpoint_.cancelled(checkpoint_.context)) result = Result::Cancelled;
        }
        return result == Result::Complete;
    }
    bool equal(std::string_view a, std::string_view b) {
        if (!take()) return false;
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); ++i) if (!take() || a[i] != b[i]) return false;
        return true;
    }
    bool has_class(std::string_view classes, std::string_view name) {
        size_t start = 0;
        while (start < classes.size()) {
            if (!take()) return false;
            if (space(classes[start])) { ++start; continue; }
            size_t end = start;
            while (end < classes.size() && !space(classes[end])) { if (!take()) return false; ++end; }
            if (equal(classes.substr(start, end - start), name)) return true;
            start = end;
        }
        return false;
    }
private:
    WorkBudget& budget_;
    Checkpoint checkpoint_;
    uint32_t checked_ = 0;
};
}

struct StyleSheet::Impl {
    Limits limits;
    Report report;
    Result ready = Result::Complete;
    Pool<Rule> rules;
    Pool<Selector> selectors;
    Pool<Step> steps;
    Pool<Atom> atoms;
    Pool<Declaration> values;
    Pool<char> names;
    std::unique_ptr<TokenList, PoolDelete<TokenList>> scratch;
    explicit Impl(Limits value) : limits(value) {
        if (limits.source_bytes > 262144 || limits.rules > 512 || limits.selectors > 1024 || limits.steps > 4096 ||
            limits.atoms > 8192 || limits.declarations > 4096 || limits.identifier_bytes > 32768) { ready = Result::InvalidInput; return; }
        if (!rules.allocate(limits.rules) || !selectors.allocate(limits.selectors) || !steps.allocate(limits.steps) ||
            !atoms.allocate(limits.atoms) || !values.allocate(limits.declarations) || !names.allocate(limits.identifier_bytes)) {
            ready = Result::NoMemory; return;
        }
        scratch.reset(allocate_pool<TokenList>(1));
        if (!scratch) { ready = Result::NoMemory; return; }
        report.allocated_bytes = sizeof(Impl) + sizeof(TokenList) + limits.rules * sizeof(Rule) +
            limits.selectors * sizeof(Selector) + limits.steps * sizeof(Step) + limits.atoms * sizeof(Atom) +
            limits.declarations * sizeof(Declaration) + limits.identifier_bytes;
    }
    std::string_view name(const Atom& atom) const { return {names.data.get() + atom.offset, atom.bytes}; }
    bool atom(Token token, char kind) {
        Text text;
        if (!decode(token.raw, text)) return false;
        if (text.size > names.capacity - names.size || atoms.size == atoms.capacity) { ready = Result::Limit; return false; }
        const auto offset = names.size;
        for (size_t i = 0; i < text.size; ++i) names.push(text.bytes[i]);
        return atoms.push({static_cast<uint16_t>(offset), static_cast<uint16_t>(text.size), kind});
    }
    bool selector(const TokenList& list, size_t begin, size_t end) {
        while (begin < end && list.values[begin].kind == Kind::Space) ++begin;
        while (end > begin && list.values[end - 1].kind == Kind::Space) --end;
        if (begin == end) return false;
        Selector selector;
        selector.first = static_cast<uint16_t>(steps.size);
        char relation = 0;
        size_t cursor = begin;
        while (cursor < end) {
            if (selector.count == max_selector_steps) return false;
            Step step;
            step.first = static_cast<uint16_t>(atoms.size); step.relation = relation;
            if (list.values[cursor].kind == Kind::Ident) {
                if (!atom(list.values[cursor++], 't')) return false;
                ++step.count; ++selector.specificity;
            } else if (is(list.values[cursor], '*')) ++cursor;
            while (cursor < end && (is(list.values[cursor], '.') || is(list.values[cursor], '#'))) {
                if (step.count == max_compound_atoms) return false;
                const char kind = list.values[cursor++].delimiter;
                if (cursor == end || list.values[cursor].kind != Kind::Ident || !atom(list.values[cursor++], kind)) return false;
                ++step.count; selector.specificity += kind == '#' ? 65536 : 256;
            }
            if (!step.count && !(cursor > begin && is(list.values[cursor - 1], '*'))) return false;
            if (!steps.push(step)) { ready = Result::Limit; return false; }
            ++selector.count;
            bool whitespace = false;
            while (cursor < end && list.values[cursor].kind == Kind::Space) { ++cursor; whitespace = true; }
            if (cursor == end) break;
            if (is(list.values[cursor], '>')) {
                ++cursor;
                while (cursor < end && list.values[cursor].kind == Kind::Space) ++cursor;
                relation = '>';
                if (cursor == end) return false;
            } else if (whitespace) relation = ' ';
            else return false;
        }
        if (!selectors.push(selector)) { ready = Result::Limit; return false; }
        return true;
    }
    bool selector_group(const TokenList& list) {
        if (!list.valid) return false;
        size_t begin = 0;
        for (size_t i = 0; i <= list.size; ++i) {
            if (i == list.size || is(list.values[i], ',')) {
                if (!selector(list, begin, i)) return false;
                begin = i + 1;
            }
        }
        return true;
    }
    bool matches_step(const Step& step, const Element& element, Matching& matching) const {
        if (!matching.take()) return false;
        for (size_t i = 0; i < step.count; ++i) {
            const auto& atom = atoms[step.first + i];
            const auto text = name(atom);
            if (atom.kind == '.' ? !matching.has_class(element.classes, text) :
                !matching.equal(atom.kind == '#' ? element.id : element.tag, text)) return false;
        }
        return true;
    }
    bool matches(const Selector& selector, size_t step_index, const Element* const* ancestry,
                 size_t depth, size_t element_index, Matching& matching) const {
        if (element_index == depth || !matches_step(steps[selector.first + step_index], *ancestry[element_index], matching)) return false;
        if (!step_index) return true;
        if (steps[selector.first + step_index].relation == '>')
            return matches(selector, step_index - 1, ancestry, depth, element_index + 1, matching);
        for (size_t i = element_index + 1; i < depth && matching.result == Result::Complete; ++i)
            if (matches(selector, step_index - 1, ancestry, depth, i, matching)) return true;
        return false;
    }
};

bool ComputedStyle::operator==(const ComputedStyle& other) const {
    const auto fields = [](const ComputedStyle& style) {
        return std::tie(style.font_px, style.line_height_px, style.line_height_factor, style.indent_px,
            style.margin_px, style.padding_px, style.width_px, style.max_width_px, style.box_percentages, style.display, style.align, style.break_before,
            style.break_after, style.break_inside, style.margin_auto, style.font_families, style.bleed,
            style.hyphens, style.language);
    };
    return bold == other.bold && italic == other.italic && underline == other.underline &&
        hidden == other.hidden && align_declared == other.align_declared && fields(*this) == fields(other);
}

StyleSheet::StyleSheet(Limits limits) : impl_(new(std::nothrow) Impl(limits)) {}
StyleSheet::~StyleSheet() = default;
Report StyleSheet::report() const {
    if (!impl_) return {};
    auto result = impl_->report;
    result.rules = impl_->rules.size; result.selectors = impl_->selectors.size; result.declarations = impl_->values.size;
    return result;
}
Result StyleSheet::collect_fonts(std::string_view source, Checkpoint checkpoint,
                                 const std::function<bool(std::string_view, std::string_view)>& accept) {
    if (!impl_) return Result::NoMemory;
    if (impl_->ready != Result::Complete) return impl_->ready;
    if (source.size() > impl_->limits.source_bytes) return Result::Limit;
    auto& list = *impl_->scratch;
    Scanner scanner(source, checkpoint);
    while (scanner.result == Result::Complete) {
        const auto first = nonspace(scanner);
        if (first.kind == Kind::End) break;
        if (is(first, ';') || is(first, '}')) continue;
        list.clear();
        const auto end = collect(scanner, first, list, false);
        if (!is(end, '{')) continue;
        if (!is(first, '@') || compact_tokens(list) != 2 || !keyword(list.values[1], "font-face")) {
            skip_block(scanner);
            continue;
        }
        Text family;
        std::string_view urls;
        bool normal = true, closed = false;
        while (scanner.result == Result::Complete) {
            const auto token = nonspace(scanner);
            if (token.kind == Kind::End || is(token, '}')) { closed = is(token, '}'); break; }
            list.clear();
            const auto terminator = collect_declaration(scanner, token, list);
            const auto count = compact_tokens(list);
            if (list.valid && count >= 3 && is(list.values[1], ':')) {
                const auto& value = list.values[2];
                if (keyword(token, "font-family")) {
                    family.size = 0;
                    if (!family_name(list.values.data() + 2, count - 2, family)) family.size = 0;
                } else if (keyword(token, "src") && terminator.kind != Kind::End) {
                    const char* begin = list.values[1].raw.data() + 1;
                    urls = {begin, static_cast<std::size_t>(terminator.raw.data() - begin)};
                } else if (keyword(token, "font-style") || keyword(token, "font-weight") ||
                           keyword(token, "font-stretch") || keyword(token, "unicode-range")) {
                    normal &= keyword(value, "normal") || (keyword(token, "font-weight") &&
                        value.kind == Kind::Number && value.number == 400);
                }
            }
            if (terminator.kind == Kind::End || is(terminator, '}')) { closed = is(terminator, '}'); break; }
        }
        if (!closed || !normal || !family.size || urls.empty()) continue;
        Scanner sources(urls, checkpoint);
        bool first_source_token = true;
        unsigned depth = 0;
        for (auto token = nonspace(sources); token.kind != Kind::End; token = nonspace(sources)) {
            if (is(token, ',') && !depth) { first_source_token = true; continue; }
            if (first_source_token) {
                Text url;
                if (token.kind == Kind::Url && token.raw.back() == ')') {
                    auto raw = token.raw.substr(token.raw.find('(') + 1);
                    raw.remove_suffix(1);
                    while (!raw.empty() && space(raw.front())) raw.remove_prefix(1);
                    while (!raw.empty() && space(raw.back())) raw.remove_suffix(1);
                    if (!decode(raw, url)) url.size = 0;
                } else if (keyword(token, "url") && is(nonspace(sources), '(')) {
                    const auto argument = nonspace(sources);
                    if (argument.kind == Kind::String && is(nonspace(sources), ')') && !decode(argument.raw, url)) url.size = 0;
                }
                if (url.size && accept(family.view(), url.view())) break;
            }
            first_source_token = false;
            if (is(token, '(')) ++depth;
            if (is(token, ')') && depth) --depth;
        }
        if (sources.result != Result::Complete) return sources.result;
    }
    return scanner.result;
}
Result StyleSheet::append(std::string_view source, Checkpoint checkpoint, const FontFamilies* families) {
    PhaseScope phase(checkpoint, Phase::Parse);
    if (!impl_) return Result::NoMemory;
    auto& data = *impl_;
    if (data.ready != Result::Complete) return data.ready;
    if (source.size() > data.limits.source_bytes - data.report.source_bytes) return Result::Limit;
    data.report.source_bytes += static_cast<uint32_t>(source.size());
    Scanner scanner(source, checkpoint);
    while (scanner.result == Result::Complete) {
        const auto first = nonspace(scanner);
        if (first.kind == Kind::End) break;
        if (is(first, ';') || is(first, '}')) continue;
        auto& list = *data.scratch;
        list.clear();
        const auto terminator = collect(scanner, first, list, false);
        if (is(first, '@')) {
            ++data.report.skipped_at_rules;
            if (is(terminator, '{')) skip_block(scanner);
            continue;
        }
        if (!is(terminator, '{')) { ++data.report.rejected_rules; continue; }
        const auto selectors_start = data.selectors.size, steps_start = data.steps.size,
                   atoms_start = data.atoms.size, names_start = data.names.size, values_start = data.values.size;
        const bool valid = data.selector_group(list);
        Rule rule{static_cast<uint16_t>(selectors_start), static_cast<uint16_t>(data.selectors.size - selectors_start),
                  static_cast<uint16_t>(values_start), 0};
        if (valid) {
            for (;;) {
                const auto token = nonspace(scanner);
                if (token.kind == Kind::End || is(token, '}')) break;
                list.clear();
                const auto end = collect_declaration(scanner, token, list);
                if (is(token, '@')) ++data.report.skipped_at_rules;
                std::array<Declaration, 4> parsed{};
                const size_t count = declarations(list, parsed, families);
                if (!count && list.size) ++data.report.rejected_declarations;
                if (count > data.values.capacity - data.values.size) data.ready = Result::Limit;
                if (data.ready != Result::Complete || scanner.result != Result::Complete) break;
                for (size_t i = 0; i < count; ++i) data.values.push(parsed[i]);
                if (end.kind == Kind::End || is(end, '}')) break;
            }
            rule.declarations = static_cast<uint16_t>(data.values.size - values_start);
        } else { ++data.report.rejected_rules; skip_block(scanner); }
        if (!valid || scanner.result != Result::Complete || data.ready != Result::Complete || !data.rules.push(rule)) {
            data.selectors.size = selectors_start; data.steps.size = steps_start;
            data.atoms.size = atoms_start; data.names.size = names_start; data.values.size = values_start;
            if (valid && scanner.result == Result::Complete && data.ready == Result::Complete) data.ready = Result::Limit;
        }
        if (data.ready != Result::Complete) return data.ready;
    }
    return scanner.result;
}
Result StyleSheet::compute(const Element& element, const ComputedStyle* parent, const Context& context,
                           std::string_view inline_style, WorkBudget& budget, ComputedStyle& output,
                           Checkpoint checkpoint, const ComputedStyle* defaults, const FontFamilies* families) const {
    PhaseScope phase(checkpoint, Phase::Match);
    output = {};
    const float fields[] = {context.initial_font_px, context.root_font_px, context.containing_width_px,
                            context.minimum_font_px, context.maximum_font_px};
    for (const auto value : fields) if (!std::isfinite(value) || value <= 0 || value > 16384) return Result::InvalidInput;
    if (context.minimum_font_px > context.maximum_font_px) return Result::InvalidInput;
    output = base_style(parent, context);
    if (inline_style.size() > 4096) return Result::InvalidInput;
    if (!impl_) return Result::NoMemory;
    if (impl_->ready == Result::NoMemory || impl_->ready == Result::InvalidInput) return impl_->ready;
    if (budget.styles >= budget.maximum_styles) return Result::Limit;
    ++budget.styles;
    Matching matching(budget, checkpoint);
    std::array<const Element*, max_ancestors> ancestry{};
    size_t depth = 0;
    for (const Element* current = &element; current; current = current->parent) {
        if (depth == ancestry.size() || current->tag.size() > 256 || current->id.size() > 256 || current->classes.size() > 4096)
            return Result::InvalidInput;
        if (!matching.take()) return matching.result;
        ancestry[depth++] = current;
    }
    std::array<Winner, property_count> winners{};
    const auto& data = *impl_;
    for (size_t i = 0; i < data.rules.size && matching.result == Result::Complete; ++i) {
        if (!matching.take()) break;
        const auto& rule = data.rules[i];
        bool matched = false;
        uint32_t specificity = 0;
        for (size_t j = 0; j < rule.selectors && matching.result == Result::Complete; ++j) {
            const auto& selector = data.selectors[rule.first_selector + j];
            if (data.matches(selector, selector.count - 1, ancestry.data(), depth, 0, matching)) {
                matched = true; specificity = std::max(specificity, selector.specificity);
            }
        }
        if (matched) for (size_t j = 0; j < rule.declarations; ++j) {
            if (!matching.take()) break;
            consider(winners, data.values[rule.first_declaration + j], specificity);
        }
    }
    if (matching.result != Result::Complete) return matching.result;
    if (!matching.take(static_cast<uint32_t>(inline_style.size()))) return matching.result;
    Scanner scanner(inline_style, checkpoint);
    auto& list = *data.scratch;
    for (;;) {
        const auto token = nonspace(scanner);
        if (token.kind == Kind::End) break;
        list.clear();
        const auto end = collect_declaration(scanner, token, list);
        std::array<Declaration, 4> parsed{};
        const auto count = declarations(list, parsed, families);
        for (size_t i = 0; i < count; ++i) {
            if (!matching.take()) return matching.result;
            consider(winners, parsed[i], 0x1000000);
        }
        if (end.kind == Kind::End) break;
        if (is(end, '}')) return Result::InvalidInput;
    }
    if (scanner.result != Result::Complete) return scanner.result;
    phase.change(Phase::Cascade);
    if (defaults) output = *defaults;
    resolve(winners, parent, context, output);
    return Result::Complete;
}
}
