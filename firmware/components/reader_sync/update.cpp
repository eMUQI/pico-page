#include "pico_page/update.h"
#include "cJSON.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstring>
#include <memory>
#include <utility>

namespace pico_page::update {
namespace {
using Json = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;

/** Reads a decimal number without a leading zero; false when text does not start with one. */
bool number(std::string_view& text, std::uint32_t& value) {
    std::size_t length = 0;
    std::uint64_t parsed = 0;
    while (length < text.size() && text[length] >= '0' && text[length] <= '9' && length < 9)
        parsed = parsed * 10 + static_cast<unsigned>(text[length++] - '0');
    if (!length || (length > 1 && text[0] == '0') ||
        (length < text.size() && text[length] >= '0' && text[length] <= '9')) return false;
    value = static_cast<std::uint32_t>(parsed);
    text.remove_prefix(length);
    return true;
}
bool literal(std::string_view& text, std::string_view expected) {
    if (text.substr(0, expected.size()) != expected) return false;
    text.remove_prefix(expected.size());
    return true;
}
/** Major, minor, patch, stage (0 beta, 1 rc, 2 release) and stage number. */
using Precedence = std::array<std::uint32_t, 5>;
bool precedence(std::string_view version, Precedence& order) {
    version = version.substr(0, version.find('+'));
    order = {0, 0, 0, 2, 0};
    if (!number(version, order[0]) || !literal(version, ".") || !number(version, order[1]) ||
        !literal(version, ".") || !number(version, order[2])) return false;
    if (version.empty()) return true;
    order[3] = literal(version, "-beta.") ? 0 : literal(version, "-rc.") ? 1 : 2;
    return order[3] != 2 && number(version, order[4]) && order[4] >= 1 && version.empty();
}
bool text_field(const cJSON* object, const char* key, std::size_t maximum, std::string& value) {
    const auto* field = cJSON_GetObjectItemCaseSensitive(object, key);
    if (!cJSON_IsString(field) || !field->valuestring || std::strlen(field->valuestring) > maximum) return false;
    value = field->valuestring;
    return true;
}
bool integer_field(const cJSON* object, const char* key, double minimum, double maximum, double& value) {
    const auto* field = cJSON_GetObjectItemCaseSensitive(object, key);
    if (!cJSON_IsNumber(field) || !std::isfinite(field->valuedouble) ||
        std::floor(field->valuedouble) != field->valuedouble ||
        field->valuedouble < minimum || field->valuedouble > maximum) return false;
    value = field->valuedouble;
    return true;
}
bool plain_url(const std::string& url) {
    return url.size() > 8 && url.size() <= 512 && url.compare(0, 8, "https://") == 0 && url[8] != '/' &&
           std::none_of(url.begin() + 8, url.end(), [](unsigned char c) {
               return c <= 32 || c >= 127 || c == '@' || c == '#' || c == '\\';
           });
}
/** Cuts at a UTF-8 scalar boundary at or below limit. */
bool truncate(std::string& text, std::size_t limit) {
    if (text.size() <= limit) return false;
    while (limit && (static_cast<unsigned char>(text[limit]) & 0xC0) == 0x80) --limit;
    text.resize(limit);
    return true;
}
/** Splits one line into spans, dropping the inline syntax that the notes page does not draw. */
std::vector<NoteSpan> spans(std::string_view line) {
    std::vector<NoteSpan> result;
    bool bold = false;
    std::string run;
    const auto flush = [&] {
        if (!run.empty()) result.push_back({std::exchange(run, std::string()), bold});
    };
    for (std::size_t i = 0; i < line.size(); ++i) {
        if (line.compare(i, 2, "**") == 0 && (bold || line.find("**", i + 2) != std::string_view::npos)) {
            flush();
            bold = !bold;
            ++i;
        } else if (line[i] == '`') {
            continue;
        } else if (line[i] == '[') {
            const auto close = line.find("](", i + 1);
            const auto end = close == std::string_view::npos ? close : line.find(')', close + 2);
            if (end == std::string_view::npos) { run += line[i]; continue; }
            run.append(line.substr(i + 1, close - i - 1));
            i = end;
        } else run += line[i];
    }
    flush();
    return result;
}
cJSON* manifest_object(const Manifest& manifest) {
    Json object(cJSON_CreateObject(), cJSON_Delete);
    auto* schema = cJSON_AddObjectToObject(object.get(), "data_schema");
    if (!schema || !cJSON_AddStringToObject(object.get(), "version", manifest.version.c_str()) ||
        !cJSON_AddStringToObject(object.get(), "published_at", manifest.published_at.c_str()) ||
        !cJSON_AddStringToObject(object.get(), "url", manifest.url.c_str()) ||
        !cJSON_AddStringToObject(object.get(), "sha256", manifest.sha256.c_str()) ||
        !cJSON_AddStringToObject(object.get(), "notes", manifest.notes.c_str()) ||
        !cJSON_AddNumberToObject(object.get(), "size", manifest.size) ||
        !cJSON_AddBoolToObject(object.get(), "notes_truncated", manifest.notes_truncated) ||
        !cJSON_AddNumberToObject(schema, "progress", manifest.progress_schema) ||
        !cJSON_AddNumberToObject(schema, "config", manifest.config_schema)) return nullptr;
    return object.release();
}
bool read_manifest(const cJSON* object, Manifest& manifest, std::string& error) {
    manifest = {};
    double size = 0, progress = 0, config = 0;
    const auto* schema = cJSON_GetObjectItemCaseSensitive(object, "data_schema");
    Precedence order;
    if (!text_field(object, "version", 31, manifest.version) || !precedence(manifest.version, order) ||
        development_build(manifest.version)) error = "Manifest version is not a release version";
    else if (!text_field(object, "url", 512, manifest.url) || !plain_url(manifest.url)) error = "Manifest url is not an https address";
    else if (!integer_field(object, "size", 1, 4294967295., size)) error = "Manifest size is invalid";
    else if (!text_field(object, "sha256", 64, manifest.sha256) || manifest.sha256.size() != 64 ||
             !std::all_of(manifest.sha256.begin(), manifest.sha256.end(), [](unsigned char c) { return std::isxdigit(c); }))
        error = "Manifest sha256 is invalid";
    else if (!cJSON_IsObject(schema) || !integer_field(schema, "progress", 1, 1000, progress) ||
             !integer_field(schema, "config", 1, 1000, config)) error = "Manifest data_schema is invalid";
    else if (!text_field(object, "published_at", 32, manifest.published_at)) error = "Manifest published_at is invalid";
    else if (!text_field(object, "notes", 32 * 1024, manifest.notes)) error = "Manifest notes are invalid";
    else {
        std::transform(manifest.sha256.begin(), manifest.sha256.end(), manifest.sha256.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        manifest.size = static_cast<std::uint32_t>(size);
        manifest.progress_schema = static_cast<int>(progress);
        manifest.config_schema = static_cast<int>(config);
        const auto* truncated = cJSON_GetObjectItemCaseSensitive(object, "notes_truncated");
        manifest.notes_truncated = truncate(manifest.notes, notes_limit) || cJSON_IsTrue(truncated);
        error.clear();
        return true;
    }
    manifest = {};
    return false;
}
}  // namespace

std::optional<int> compare_versions(std::string_view a, std::string_view b) {
    Precedence left, right;
    if (!precedence(a, left) || !precedence(b, right)) return std::nullopt;
    return left < right ? -1 : left > right ? 1 : 0;
}

bool development_build(std::string_view version) { return version.find('+') != std::string_view::npos; }

std::string release_version(std::string_view version) { return std::string(version.substr(0, version.find('+'))); }

bool parse_manifest(const std::string& json, Manifest& manifest, std::string& error) {
    manifest = {};
    const Json object(cJSON_ParseWithLength(json.data(), json.size()), cJSON_Delete);
    double format = 0;
    std::string target;
    if (!cJSON_IsObject(object.get())) error = "Manifest is not a JSON object";
    else if (!integer_field(object.get(), "manifest", 1, 1, format)) error = "Unsupported manifest format";
    else if (!text_field(object.get(), "board", 32, target) || target != board) error = "Manifest is for another board";
    else return read_manifest(object.get(), manifest, error);
    return false;
}

bool requires_schema_confirmation(const Manifest& manifest) {
    return manifest.progress_schema > progress_schema || manifest.config_schema > config_schema;
}

Offer evaluate(const Manifest& manifest, std::string_view running, std::string_view skipped) {
    const auto order = compare_versions(manifest.version, running);
    const bool development = development_build(running) || !order;
    if (!development && *order <= 0) return Offer::Current;
    if (manifest.size > slot_bytes) return Offer::TooLarge;
    if (manifest.progress_schema < progress_schema || manifest.config_schema < config_schema) return Offer::OlderSchema;
    if (manifest.version == skipped) return Offer::Skipped;
    if (development) return Offer::Development;
    return requires_schema_confirmation(manifest) ? Offer::NewerSchema : Offer::Newer;
}

std::vector<NoteBlock> parse_notes(const std::string& markdown) {
    std::vector<NoteBlock> blocks;
    std::size_t start = 0;
    while (start <= markdown.size()) {
        auto end = markdown.find('\n', start);
        if (end == std::string::npos) end = markdown.size();
        std::string_view line(markdown.data() + start, end - start);
        start = end + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) line.remove_suffix(1);
        while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) line.remove_prefix(1);
        if (line.empty()) continue;
        auto kind = NoteBlock::Kind::Text;
        const auto marks = std::min(line.find_first_not_of('#'), line.size());
        if (marks >= 1 && marks <= 3 && marks < line.size() && line[marks] == ' ') {
            kind = NoteBlock::Kind::Heading;
            line.remove_prefix(marks + 1);
        } else if (line.size() > 2 && (line[0] == '-' || line[0] == '*') && line[1] == ' ') {
            kind = NoteBlock::Kind::Item;
            line.remove_prefix(2);
        }
        while (!line.empty() && line.front() == ' ') line.remove_prefix(1);
        auto parts = spans(line);
        if (!parts.empty()) blocks.push_back({kind, std::move(parts)});
    }
    return blocks;
}

std::string encode_state(const State& state) {
    const Json object(cJSON_CreateObject(), cJSON_Delete);
    auto* manifest = state.manifest ? manifest_object(*state.manifest) : nullptr;
    if (manifest && !cJSON_AddItemToObject(object.get(), "manifest", manifest)) { cJSON_Delete(manifest); return {}; }
    if (!object || (state.manifest && !manifest) ||
        !cJSON_AddNumberToObject(object.get(), "checked_at", static_cast<double>(state.checked_at)) ||
        !cJSON_AddStringToObject(object.get(), "pending_from", state.pending_from.c_str()) ||
        !cJSON_AddStringToObject(object.get(), "pending_to", state.pending_to.c_str()) ||
        !cJSON_AddStringToObject(object.get(), "skipped", state.skipped.c_str())) return {};
    const std::unique_ptr<char, decltype(&cJSON_free)> text(cJSON_PrintUnformatted(object.get()), cJSON_free);
    return text ? std::string(text.get()) : std::string();
}

bool decode_state(const std::string& json, State& state) {
    state = {};
    const Json object(cJSON_ParseWithLength(json.data(), json.size()), cJSON_Delete);
    double checked = 0;
    std::string error;
    if (!cJSON_IsObject(object.get()) || !integer_field(object.get(), "checked_at", 0, 9007199254740991., checked) ||
        !text_field(object.get(), "pending_from", 31, state.pending_from) ||
        !text_field(object.get(), "pending_to", 31, state.pending_to) ||
        !text_field(object.get(), "skipped", 31, state.skipped)) { state = {}; return false; }
    state.checked_at = static_cast<std::int64_t>(checked);
    if (const auto* manifest = cJSON_GetObjectItemCaseSensitive(object.get(), "manifest")) {
        Manifest parsed;
        if (!cJSON_IsObject(manifest) || !read_manifest(manifest, parsed, error)) { state = {}; return false; }
        state.manifest = std::move(parsed);
    }
    return true;
}

}  // namespace pico_page::update
