#include "pico_page/store.h"
#include "store_internal.h"

#include "tomlc17.h"

#include <algorithm>
#include <cmath>
#include <charconv>
#include <cstring>
#include <initializer_list>

namespace pico_page {
bool valid_dictionary_id(const std::string& path) {
    if (path.empty() || path.size() > 512 || !store_internal::relative_path(path) ||
        !((path.size() > 4 && path.compare(path.size() - 4, 4, ".ifo") == 0) ||
          (path.size() > 7 && path.compare(path.size() - 7, 7, ".ifo.gz") == 0))) return false;
    bool component_start = true;
    for (unsigned char c : path) {
        if (c < 32 || c == 127 || c == '\\' || c == ':' || (component_start && c == '.')) return false;
        component_start = c == '/';
    }
    return true;
}

namespace store_internal {
void configure_toml() {
    static const bool configured = [] {
        auto options = toml_default_option();
        options.check_utf8 = true;
        toml_set_option(options);
        return true;
    }();
    (void)configured;
}
}
namespace {
constexpr std::size_t config_limit = 32768;
constexpr const char* statistics_languages[] = {"zh-Hans", "zh-Hant", "en"};

bool allowed_fields(toml_datum_t table, std::initializer_list<const char*> allowed,
                    const std::string& prefix, ConfigDiagnostic& diagnostic) {
    if (table.type == TOML_UNKNOWN) return true;
    if (table.type != TOML_TABLE) {
        diagnostic.field = prefix;
        diagnostic.detail = "expected a TOML table";
        return false;
    }
    for (int index = 0; index < table.u.tab.size; ++index) {
        const std::string key(table.u.tab.key[index], table.u.tab.len[index]);
        bool known = false;
        for (const auto* field : allowed) if (key == field) known = true;
        if (!known) {
            diagnostic.field = prefix.empty() ? key : prefix + "." + key;
            diagnostic.detail = "unsupported field";
            return false;
        }
    }
    return true;
}

bool string_value(toml_datum_t datum, std::string& destination) {
    if (datum.type != TOML_STRING || datum.u.str.len <= 0 || datum.u.str.len > 1024 ||
        std::memchr(datum.u.str.ptr, '\0', datum.u.str.len)) return false;
    destination.assign(datum.u.str.ptr, datum.u.str.len);
    return true;
}

bool apply(toml_datum_t table, bool book, ReaderSettings& settings, ConfigDiagnostic& diagnostic) {
    const auto schema = toml_get(table, "schema_version");
    diagnostic.field = "schema_version";
    if (schema.type != TOML_INT64 || schema.u.int64 < 1) {
        diagnostic.detail = "expected schema_version = 1";
        return false;
    }
    if (schema.u.int64 > 1) {
        diagnostic.status = StoreStatus::UnsupportedVersion;
        diagnostic.detail = "unsupported configuration schema_version";
        return false;
    }
    if (!(book ? allowed_fields(table, {"schema_version", "reader", "display"}, "", diagnostic)
               : allowed_fields(table, {"schema_version", "reader", "display", "library", "power", "network", "dictionary", "onboarding", "statistics"}, "", diagnostic)) ||
        !allowed_fields(toml_get(table, "reader"), {"font", "font_weight", "font_size_px", "line_height", "margin_x_px", "paragraph_gap_lines", "indent_chars", "fullscreen_reading", "original_epub"}, "reader", diagnostic) ||
        !allowed_fields(toml_get(table, "display"), {"refresh_preset", "balanced_full_refresh_pages", "fast_full_refresh_pages"}, "display", diagnostic) ||
        !allowed_fields(toml_get(table, "library"), {"roots", "view", "sort", "folders"}, "library", diagnostic) ||
        !allowed_fields(toml_get(table, "dictionary"), {"enabled"}, "dictionary", diagnostic) ||
        !allowed_fields(toml_get(table, "statistics"), {"enabled", "minimum_seconds", "maximum_seconds", "language"}, "statistics", diagnostic) ||
        !allowed_fields(toml_get(table, "onboarding"), {"done"}, "onboarding", diagnostic) ||
        !allowed_fields(toml_get(table, "network"), {"wifi_enabled"}, "network", diagnostic) ||
        !allowed_fields(toml_get(table, "power"), {"auto_sleep_minutes", "sleep_screen", "sleep_image", "sleep_overlay"}, "power", diagnostic)) return false;

    auto value = toml_seek(table, "reader.font");
    diagnostic.field = "reader.font";
    diagnostic.detail = "expected a relative font path";
    if (value.type != TOML_UNKNOWN && (!string_value(value, settings.font) || !store_internal::relative_path(settings.font))) return false;

    value = toml_seek(table, "reader.font_weight");
    if (value.type != TOML_UNKNOWN) {
        diagnostic.field = "reader.font_weight";
        diagnostic.detail = "expected light, regular, medium or bold";
        if (!string_value(value, settings.font_weight) ||
            (settings.font_weight != "light" && settings.font_weight != "regular" && settings.font_weight != "medium" && settings.font_weight != "bold")) return false;
    }

    value = toml_seek(table, "reader.fullscreen_reading");
    if (value.type != TOML_UNKNOWN) {
        diagnostic.field = "reader.fullscreen_reading";
        diagnostic.detail = "expected a boolean";
        if (value.type != TOML_BOOLEAN) return false;
        settings.fullscreen_reading = value.u.boolean;
    }

    value = toml_seek(table, "reader.original_epub");
    if (value.type != TOML_UNKNOWN) {
        diagnostic.field = "reader.original_epub";
        diagnostic.detail = "expected a boolean in a book settings file";
        if (!book || value.type != TOML_BOOLEAN) return false;
        settings.original_epub = value.u.boolean;
    }

    for (const char* field : {"reader.font_size_px", "reader.margin_x_px", "reader.indent_chars"}) {
        value = toml_seek(table, field);
        if (value.type == TOML_UNKNOWN) continue;
        const bool font_size = std::strcmp(field, "reader.font_size_px") == 0;
        const int minimum = font_size ? 12 : 0;
        const bool indent = std::strcmp(field, "reader.indent_chars") == 0;
        const int maximum = font_size ? 96 : indent ? 2 : 160;
        diagnostic.field = field;
        diagnostic.detail = "expected integer in range " + std::to_string(minimum) + ".." + std::to_string(maximum);
        if (value.type != TOML_INT64 || value.u.int64 < minimum || value.u.int64 > maximum) return false;
        (font_size ? settings.font_size_px : indent ? settings.indent_chars : settings.margin_x_px) = static_cast<int>(value.u.int64);
    }

    for (const char* field : {"reader.line_height", "reader.paragraph_gap_lines"}) {
        value = toml_seek(table, field);
        if (value.type == TOML_UNKNOWN) continue;
        const bool line_height = std::strcmp(field, "reader.line_height") == 0;
        const double minimum = line_height ? 1 : 0;
        const double maximum = line_height ? 3 : 1;
        diagnostic.field = field;
        diagnostic.detail = line_height ? "expected a finite number in range 1..3" : "expected a finite number in range 0..1";
        if (value.type != TOML_FP64 && value.type != TOML_INT64) return false;
        const double height = value.type == TOML_FP64 ? value.u.fp64 : static_cast<double>(value.u.int64);
        if (!std::isfinite(height) || height < minimum || height > maximum) return false;
        (line_height ? settings.line_height : settings.paragraph_gap_lines) = height;
    }
    value = toml_seek(table, "display.refresh_preset");
    if (value.type != TOML_UNKNOWN) {
        diagnostic.field = "display.refresh_preset";
        diagnostic.detail = "expected clear, balanced or fast";
        if (!string_value(value, settings.refresh_preset) ||
            (settings.refresh_preset != "clear" && settings.refresh_preset != "balanced" && settings.refresh_preset != "fast")) return false;
    }
    for (const char* field : {"display.balanced_full_refresh_pages", "display.fast_full_refresh_pages"}) {
        value = toml_seek(table, field);
        if (value.type == TOML_UNKNOWN) continue;
        diagnostic.field = field;
        diagnostic.detail = "expected integer in range 0..30";
        if (value.type != TOML_INT64 || value.u.int64 < 0 || value.u.int64 > 30) return false;
        (std::strcmp(field, "display.fast_full_refresh_pages") == 0 ? settings.fast_full_refresh_pages
                                                                 : settings.balanced_full_refresh_pages) = static_cast<int>(value.u.int64);
    }
    if (!book && toml_get(table, "statistics").type != TOML_UNKNOWN) {
        auto& options = settings.statistics;
        value = toml_seek(table, "statistics.enabled");
        if (value.type != TOML_UNKNOWN) {
            diagnostic.field = "statistics.enabled";
            diagnostic.detail = "expected a boolean";
            if (value.type != TOML_BOOLEAN) return false;
            options.enabled = value.u.boolean;
        }
        const auto seconds = [&](const char* field, unsigned low, unsigned high, unsigned& target) {
            const auto number = toml_seek(table, field);
            if (number.type == TOML_UNKNOWN) return true;
            diagnostic.field = field;
            diagnostic.detail = "expected seconds in range " + std::to_string(low) + ".." + std::to_string(high);
            if (number.type != TOML_INT64 || number.u.int64 < low || number.u.int64 > high) return false;
            target = static_cast<unsigned>(number.u.int64);
            return true;
        };
        if (!seconds("statistics.minimum_seconds", 1, 60, options.minimum_seconds) ||
            !seconds("statistics.maximum_seconds", 5, 600, options.maximum_seconds)) return false;
        if (options.minimum_seconds > options.maximum_seconds) {
            diagnostic.field = "statistics.minimum_seconds";
            diagnostic.detail = "minimum_seconds exceeds maximum_seconds";
            return false;
        }
        value = toml_seek(table, "statistics.language");
        if (value.type != TOML_UNKNOWN) {
            diagnostic.field = "statistics.language";
            diagnostic.detail = "expected zh-Hans, zh-Hant or en";
            std::string name;
            if (!string_value(value, name)) return false;
            const auto found = std::find(std::begin(statistics_languages), std::end(statistics_languages), name);
            if (found == std::end(statistics_languages)) return false;
            options.language = static_cast<unsigned>(found - std::begin(statistics_languages));
        }
    }
    value = toml_seek(table, "onboarding.done");
    if (value.type != TOML_UNKNOWN) {
        diagnostic.field = "onboarding.done";
        diagnostic.detail = "expected a boolean";
        if (value.type != TOML_BOOLEAN) return false;
        settings.onboarding_done = value.u.boolean;
    }
    value = toml_seek(table, "network.wifi_enabled");
    if (value.type != TOML_UNKNOWN) {
        diagnostic.field = "network.wifi_enabled";
        diagnostic.detail = "expected a boolean";
        if (value.type != TOML_BOOLEAN) return false;
        settings.wifi_enabled = value.u.boolean;
    }
    value = toml_seek(table, "power.sleep_screen");
    if (value.type != TOML_UNKNOWN) {
        diagnostic.field = "power.sleep_screen";
        diagnostic.detail = "expected cover, reading, simple or custom";
        if (!string_value(value, settings.sleep_screen) ||
            (settings.sleep_screen != "cover" && settings.sleep_screen != "reading" && settings.sleep_screen != "simple" && settings.sleep_screen != "custom")) return false;
    }
    value = toml_seek(table, "power.sleep_overlay");
    if (value.type != TOML_UNKNOWN) {
        diagnostic.field = "power.sleep_overlay";
        diagnostic.detail = "expected a boolean";
        if (value.type != TOML_BOOLEAN) return false;
        settings.sleep_overlay = value.u.boolean;
    }
    value = toml_seek(table, "power.sleep_image");
    if (value.type != TOML_UNKNOWN) {
        diagnostic.field = "power.sleep_image";
        diagnostic.detail = "expected an empty string for rotation or a PNG/JPEG basename";
        if (value.type == TOML_STRING && value.u.str.len == 0) settings.sleep_image.clear();
        else if (!string_value(value, settings.sleep_image) || !valid_lock_screen_name(settings.sleep_image)) return false;
    }
    value = toml_seek(table, "power.auto_sleep_minutes");
    if (value.type != TOML_UNKNOWN) {
        diagnostic.field = "power.auto_sleep_minutes";
        diagnostic.detail = "expected integer in range 0..240";
        if (value.type != TOML_INT64 || value.u.int64 < 0 || value.u.int64 > 240) return false;
        settings.auto_sleep_minutes = static_cast<int>(value.u.int64);
    }
    value = toml_seek(table, "library.view");
    if (value.type != TOML_UNKNOWN) {
        diagnostic.field = "library.view";
        diagnostic.detail = "expected list or grid";
        if (!string_value(value, settings.library_view) || (settings.library_view != "list" && settings.library_view != "grid")) return false;
    }
    value = toml_seek(table, "library.folders");
    if (value.type != TOML_UNKNOWN) {
        diagnostic.field = "library.folders";
        diagnostic.detail = "expected first or recent";
        if (!string_value(value, settings.library_folders) ||
            (settings.library_folders != "first" && settings.library_folders != "recent")) return false;
    }
    value = toml_seek(table, "library.sort");
    if (value.type != TOML_UNKNOWN) {
        diagnostic.field = "library.sort";
        diagnostic.detail = "expected recent, name or name_desc";
        if (!string_value(value, settings.library_sort) ||
            (settings.library_sort != "recent" && settings.library_sort != "name" && settings.library_sort != "name_desc")) return false;
    }
    value = toml_seek(table, "dictionary.enabled");
    if (value.type != TOML_UNKNOWN) {
        diagnostic.field = "dictionary.enabled";
        diagnostic.detail = "expected up to 32 unique metadata paths relative to Dictionaries";
        if (value.type != TOML_ARRAY || value.u.arr.size > 32) return false;
        std::vector<std::string> enabled;
        for (int index = 0; index < value.u.arr.size; ++index) {
            std::string path;
            if (!string_value(value.u.arr.elem[index], path) || !valid_dictionary_id(path) ||
                std::find(enabled.begin(), enabled.end(), path) != enabled.end()) return false;
            enabled.push_back(std::move(path));
        }
        settings.dictionary_enabled = std::move(enabled);
    }
    value = toml_seek(table, "library.roots");
    if (value.type != TOML_UNKNOWN) {
        diagnostic.field = "library.roots";
        diagnostic.detail = "expected 1..16 paths relative to card root";
        if (value.type != TOML_ARRAY || value.u.arr.size < 1 || value.u.arr.size > 16) return false;
        std::vector<std::string> roots;
        for (int index = 0; index < value.u.arr.size; ++index) {
            std::string path;
            if (!string_value(value.u.arr.elem[index], path)) return false;
            if (path.front() == '/') path.erase(0, 1);
            if (!store_internal::relative_path(path)) return false;
            roots.push_back(std::move(path));
        }
        settings.library_roots = std::move(roots);
    }
    return true;
}

bool apply_source(const std::string& bytes, const std::string& path, bool book,
                  ReaderSettings& settings, ConfigDiagnostic& diagnostic) {
    store_internal::configure_toml();
    diagnostic = {StoreStatus::Invalid, path, {}, {}};
    const auto parsed = toml_parse_named(bytes.data(), static_cast<int>(bytes.size()), path.c_str());
    bool success = false;
    if (!parsed.ok) {
        diagnostic.detail = parsed.errmsg;
    } else {
        auto candidate = settings;
        if (apply(parsed.toptab, book, candidate, diagnostic)) {
            settings = std::move(candidate);
            success = true;
        }
    }
    toml_free(parsed);
    return success;
}

void load_layer(const std::string& root, const std::string& path, bool book,
                ConfigResult& output, std::optional<std::string>& source) {
    std::string bytes;
    const auto status = store_internal::read_file(root, path, config_limit, bytes);
    if (status == StoreStatus::Missing) return;
    ConfigDiagnostic diagnostic{status, path, {}, "configuration file cannot be read within size limits"};
    if (status != StoreStatus::Ok) { output.diagnostics.push_back(std::move(diagnostic)); return; }
    source = bytes;
    if (!apply_source(bytes, path, book, output.settings, diagnostic)) output.diagnostics.push_back(std::move(diagnostic));
}

std::string quoted(const std::string& value) {
    std::string result = "\"";
    for (const auto ch : value) {
        if (ch == '\\' || ch == '"') result += '\\';
        result += ch;
    }
    return result + '"';
}

std::string number(double value) {
    char buffer[48];
    const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value);
    return result.ec == std::errc{} ? std::string(buffer, result.ptr) : "nan";
}

using SettingValues = std::vector<std::pair<std::string, std::string>>;
SettingValues setting_values(const ReaderSettings& values, bool global = false) {
    SettingValues fields = {{"reader.font", quoted(values.font)},
            {"reader.font_weight", quoted(values.font_weight)},
            {"reader.font_size_px", std::to_string(values.font_size_px)},
            {"reader.line_height", number(values.line_height)},
            {"reader.margin_x_px", std::to_string(values.margin_x_px)},
            {"reader.paragraph_gap_lines", number(values.paragraph_gap_lines)},
            {"reader.indent_chars", std::to_string(values.indent_chars)},
            {"reader.fullscreen_reading", values.fullscreen_reading ? "true" : "false"},
            {"display.refresh_preset", quoted(values.refresh_preset)},
            {"display.balanced_full_refresh_pages", std::to_string(values.balanced_full_refresh_pages)},
            {"display.fast_full_refresh_pages", std::to_string(values.fast_full_refresh_pages)}};
    if (!global) fields.emplace_back("reader.original_epub", values.original_epub ? "true" : "false");
    if (global) {
        std::string enabled = "[";
        for (const auto& path : values.dictionary_enabled) {
            if (enabled.size() > 1) enabled += ", ";
            enabled += quoted(path);
        }
        fields.emplace_back("dictionary.enabled", enabled + "]");
        const auto& stats = values.statistics;
        fields.emplace_back("statistics.enabled", stats.enabled ? "true" : "false");
        fields.emplace_back("statistics.minimum_seconds", std::to_string(stats.minimum_seconds));
        fields.emplace_back("statistics.maximum_seconds", std::to_string(stats.maximum_seconds));
        fields.emplace_back("statistics.language", quoted(stats.language < std::size(statistics_languages)
                                                            ? statistics_languages[stats.language] : ""));
        fields.emplace_back("onboarding.done", values.onboarding_done ? "true" : "false");
        fields.emplace_back("network.wifi_enabled", values.wifi_enabled ? "true" : "false");
        fields.emplace_back("library.view", quoted(values.library_view));
        fields.emplace_back("library.sort", quoted(values.library_sort));
        fields.emplace_back("library.folders", quoted(values.library_folders));
        fields.emplace_back("power.auto_sleep_minutes", std::to_string(values.auto_sleep_minutes));
        fields.emplace_back("power.sleep_screen", quoted(values.sleep_screen));
        fields.emplace_back("power.sleep_image", quoted(values.sleep_image));
        fields.emplace_back("power.sleep_overlay", values.sleep_overlay ? "true" : "false");
    }
    return fields;
}

std::string trim(const std::string& value) {
    const auto begin = value.find_first_not_of(" \t\r");
    if (begin == std::string::npos) return {};
    return value.substr(begin, value.find_last_not_of(" \t\r") - begin + 1);
}

std::size_t line_offset(const std::string& source, int line) {
    if (line <= 0) return std::string::npos;
    std::size_t offset = 0;
    for (int index = 1; index < line; ++index) {
        offset = source.find('\n', offset);
        if (offset == std::string::npos) return offset;
        ++offset;
    }
    return offset;
}

bool patch_value(std::string& source, const std::string& field,
                 const std::optional<std::string>& replacement) {
    const auto parsed = toml_parse(source.data(), static_cast<int>(source.size()));
    if (!parsed.ok) { toml_free(parsed); return false; }
    const auto value = toml_seek(parsed.toptab, field.c_str());
    const auto separator = field.find('.');
    const auto section = field.substr(0, separator);
    const auto key = field.substr(separator + 1);
    const auto table = toml_get(parsed.toptab, section.c_str());
    bool success = false;
    const auto edit = [&]() {
        if (table.flag & TOML_FLAG_INLINED) return false;
        if (value.type == TOML_UNKNOWN) {
            if (!replacement) return true;
            const std::string assignment = key + " = " + *replacement + "\n";
            if (table.type == TOML_UNKNOWN) {
                if (!source.empty() && source.back() != '\n') source += '\n';
                source += "[" + section + "]\n" + assignment;
                return true;
            }
            const auto header = line_offset(source, table.lineno);
            if (header == std::string::npos) return false;
            const auto header_end = source.find('\n', header);
            const auto header_text = trim(source.substr(header, header_end - header));
            if (!header_text.empty() && header_text.front() == '[') {
                if (header_end == std::string::npos) source += "\n" + assignment;
                else source.insert(header_end + 1, assignment);
            } else {
                source.insert(0, field + " = " + *replacement + "\n");
            }
            return true;
        }
        const auto line = line_offset(source, value.lineno);
        if (line == std::string::npos || value.colno <= 0) return false;
        auto begin = line + static_cast<std::size_t>(value.colno - 1);
        auto line_end = source.find('\n', line);
        if (line_end == std::string::npos) line_end = source.size();
        if (begin >= line_end) return false;
        if (value.type == TOML_STRING) {
            // tomlc17 locates string contents after the opening delimiter.
            if (begin == line || (source[begin - 1] != '\'' && source[begin - 1] != '"')) return false;
            --begin;
        }
        auto prefix = trim(source.substr(line, begin - line));
        if (prefix.empty() || prefix.back() != '=') return false;
        prefix.pop_back();
        prefix = trim(prefix);
        if (prefix != key && prefix != field && prefix != quoted(key) && prefix != "'" + key + "'") return false;
        std::size_t end = begin;
        std::string array_comments;
        const char quote = source[begin];
        if (value.type == TOML_ARRAY) {
            if (quote != '[') return false;
            char delimiter = 0;
            bool escaped = false;
            for (end = begin + 1; end < source.size(); ++end) {
                const char c = source[end];
                if (delimiter) {
                    if (!escaped && c == delimiter) delimiter = 0;
                    if (delimiter == '"' && !escaped && c == '\\') escaped = true;
                    else escaped = false;
                } else if (c == '\'' || c == '"') {
                    if (source.compare(end, 3, std::string(3, c)) == 0) return false;
                    delimiter = c;
                } else if (c == '#') {
                    const auto newline = source.find('\n', end);
                    if (newline == std::string::npos) return false;
                    array_comments += source.substr(end, newline - end) + "\n";
                    end = newline;
                } else if (c == '[') return false;
                else if (c == ']') break;
            }
            if (end == source.size()) return false;
            ++end;
            line_end = source.find('\n', end);
            if (line_end == std::string::npos) line_end = source.size();
        } else if (quote == '\'' || quote == '"') {
            if (source.compare(begin, 3, std::string(3, quote)) == 0) return false;
            bool escaped = false;
            for (end = begin + 1; end < line_end; ++end) {
                if (!escaped && source[end] == quote) break;
                if (quote == '"' && !escaped && source[end] == '\\') escaped = true;
                else escaped = false;
            }
            if (end == line_end) return false;
            ++end;
        } else {
            while (end < line_end && source[end] != '#' && source[end] != ' ' && source[end] != '\t' && source[end] != '\r') ++end;
        }
        const auto suffix = trim(source.substr(end, line_end - end));
        if (!suffix.empty() && suffix.front() != '#') return false;
        if (replacement) {
            auto next = *replacement;
            if (!array_comments.empty() && !next.empty() && next.back() == ']')
                next.insert(next.size() - 1, "\n" + array_comments);
            source.replace(begin, end - begin, next);
        }
        else if (suffix.empty()) source.erase(line, line_end - line + (line_end < source.size() ? 1 : 0));
        else {
            const auto indentation = source.find_first_not_of(" \t", line) - line;
            source.erase(line + indentation, end - line - indentation);
        }
        return true;
    };
    success = edit();
    toml_free(parsed);
    return success;
}

StoreStatus validate_snapshot(const std::string& root, const SettingsSnapshot& snapshot,
                              ReaderSettings& global, ReaderSettings& effective, std::string& error) {
    if (!snapshot.config.diagnostics.empty()) {
        error = snapshot.config.diagnostics.front().detail;
        return snapshot.config.diagnostics.front().status;
    }
    if (!snapshot.book_path.empty() && !store_internal::relative_path(snapshot.book_path)) {
        error = "Expected a card-relative book path";
        return StoreStatus::UnsafePath;
    }
    for (const bool book : {false, true}) {
        if (book && snapshot.book_path.empty()) continue;
        const auto path = book ? snapshot.book_path + ".picopage/settings.toml" : "PicoPage/settings.toml";
        const auto& expected = book ? snapshot.book_source : snapshot.global_source;
        std::string current;
        const auto status = store_internal::read_file(root, path, config_limit, current);
        if (status != StoreStatus::Ok && status != StoreStatus::Missing) {
            error = path + ": cannot compare current configuration";
            return status;
        }
        if ((status == StoreStatus::Ok) != expected.has_value() || (expected && *expected != current)) {
            error = path + ": configuration changed after the preview was opened";
            return StoreStatus::ExternalChange;
        }
        ConfigDiagnostic diagnostic{};
        if (expected && !apply_source(*expected, path, book, effective, diagnostic)) {
            error = path + ": " + diagnostic.detail;
            return diagnostic.status;
        }
        if (!book) global = effective;
    }
    return StoreStatus::Ok;
}

StoreStatus save_settings(const std::string& root, const SettingsSnapshot& snapshot,
                          const ReaderSettings& values, bool book_only, bool reset,
                          std::string& error) {
    error.clear();
    if (book_only && snapshot.book_path.empty()) {
        error = "A book is required for book settings";
        return StoreStatus::Invalid;
    }
    ReaderSettings global, effective;
    const auto status = validate_snapshot(root, snapshot, global, effective, error);
    if (status != StoreStatus::Ok) return status;
    if (book_only && store_internal::safe_path(root, snapshot.book_path) != StoreStatus::Ok) {
        error = "The book is no longer available";
        return StoreStatus::Missing;
    }
    if (values.library_roots != effective.library_roots ||
        (book_only && (values.library_view != effective.library_view ||
                       values.library_sort != effective.library_sort ||
                       values.library_folders != effective.library_folders ||
                       values.dictionary_enabled != effective.dictionary_enabled ||
                       values.statistics != effective.statistics ||
                       values.wifi_enabled != effective.wifi_enabled ||
                       values.onboarding_done != effective.onboarding_done ||
                       values.auto_sleep_minutes != effective.auto_sleep_minutes || values.sleep_screen != effective.sleep_screen || values.sleep_image != effective.sleep_image ||
                       values.sleep_overlay != effective.sleep_overlay))) {
        error = "library.roots is read-only; library.view, library.sort, library.folders, power, network, statistics and dictionary fields are global only";
        return StoreStatus::Invalid;
    }
    const auto desired = setting_values(values, !book_only);
    std::string serialized = "schema_version = 1\n";
    for (const auto& entry : desired) serialized += entry.first + " = " + entry.second + "\n";
    ReaderSettings checked;
    ConfigDiagnostic diagnostic{};
    if (!apply_source(serialized, "settings preview", book_only, checked, diagnostic)) {
        error = diagnostic.field + ": " + diagnostic.detail;
        return diagnostic.status;
    }
    const auto path = book_only ? snapshot.book_path + ".picopage/settings.toml" : "PicoPage/settings.toml";
    const auto& expected = book_only ? snapshot.book_source : snapshot.global_source;
    std::string source = expected.value_or("schema_version = 1\n");
    const auto previous = setting_values(book_only ? effective : global, !book_only);
    const auto inherited = setting_values(global, !book_only);
    auto expected_values = reset ? inherited : desired;
    bool changed = false;
    for (std::size_t index = 0; index < desired.size(); ++index) {
        if (reset && desired[index].first == "reader.original_epub") {
            expected_values[index].second = previous[index].second;
            continue;
        }
        if (!reset && desired[index].second == previous[index].second) continue;
        const auto replacement = reset || (book_only && desired[index].second == inherited[index].second)
            ? std::optional<std::string>{} : std::optional<std::string>{desired[index].second};
        if (!patch_value(source, desired[index].first, replacement)) {
            error = path + ": cannot preserve this TOML syntax while editing " + desired[index].first;
            return StoreStatus::Invalid;
        }
        changed = true;
    }
    if (!changed || (expected && source == *expected)) return StoreStatus::Ok;
    checked = book_only ? global : ReaderSettings{};
    if (source.size() > config_limit || !apply_source(source, path, book_only, checked, diagnostic)) {
        error = path + ": the edited configuration did not validate";
        return StoreStatus::Invalid;
    }
    if (setting_values(checked, !book_only) != expected_values) {
        error = path + ": the edited configuration differs from the requested values";
        return StoreStatus::Invalid;
    }
    return store_internal::commit_file(root, path, expected, source, config_limit, error);
}
}

ConfigResult FileStore::load_settings(const std::string& book_path) const {
    return settings_snapshot(book_path).config;
}

StoreStatus FileStore::load_statistics_options(StatisticsOptions& options) const {
    const auto config = load_settings();
    if (!config.diagnostics.empty()) return config.diagnostics.front().status;
    options = config.settings.statistics;
    return StoreStatus::Ok;
}

StoreStatus FileStore::save_statistics_options(const StatisticsOptions& options, const StatisticsOptions* expected) const {
    const auto snapshot = settings_snapshot();
    if (!snapshot.config.diagnostics.empty()) return snapshot.config.diagnostics.front().status;
    auto values = snapshot.global_settings;
    if (expected && values.statistics != *expected) return StoreStatus::ExternalChange;
    values.statistics = options;
    std::string error;
    return apply_settings(snapshot, values, false, error);
}

SettingsSnapshot FileStore::settings_snapshot(const std::string& book_path) const {
    SettingsSnapshot result;
    result.book_path = book_path;
    load_layer(root_, "PicoPage/settings.toml", false, result.config, result.global_source);
    result.global_settings = result.config.settings;
    if (!book_path.empty()) {
        if (!store_internal::relative_path(book_path)) {
            result.config.diagnostics.push_back({StoreStatus::UnsafePath, book_path, {}, "expected a card-relative book path"});
        } else {
            load_layer(root_, book_path + ".picopage/settings.toml", true, result.config, result.book_source);
        }
    }
    return result;
}

StoreStatus FileStore::apply_settings(const SettingsSnapshot& snapshot, const ReaderSettings& values,
                                     bool book_only, std::string& error) const {
    return save_settings(root_, snapshot, values, book_only, false, error);
}

StoreStatus FileStore::clear_book_settings(const SettingsSnapshot& snapshot, std::string& error) const {
    return save_settings(root_, snapshot, snapshot.global_settings, true, true, error);
}
}
