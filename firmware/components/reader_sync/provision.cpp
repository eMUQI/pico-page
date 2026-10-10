#include "pico_page/provision.h"
#include "store_internal.h"
#include "tomlc17.h"
#include <algorithm>
#include <cctype>
#include <initializer_list>

namespace pico_page::provision {
namespace {
bool printable(const std::string& value) {
    return std::none_of(value.begin(), value.end(), [](unsigned char c) { return c < 0x20 || c == 0x7f; });
}

// The validated values contain no control characters, so only these two need escaping in a
// TOML basic string.
std::string quoted(const std::string& value) {
    std::string out = "\"";
    for (const char c : value) {
        if (c == '"' || c == '\\') out += '\\';
        out += c;
    }
    return out + '"';
}

int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    c = static_cast<char>(c | 0x20);
    return c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

void trim(std::string& value) {
    while (!value.empty() && value.back() == ' ') value.pop_back();
    value.erase(0, value.find_first_not_of(' '));
}

bool listed(const SavedWifi& entry, const std::vector<SavedWifi>& saved) {
    return std::any_of(saved.begin(), saved.end(), [&](const SavedWifi& item) { return item.ssid == entry.ssid; });
}

bool only_keys(toml_datum_t table, std::initializer_list<const char*> keys) {
    if (table.type != TOML_TABLE) return false;
    for (int i = 0; i < table.u.tab.size; ++i) {
        const std::string key(table.u.tab.key[i], table.u.tab.len[i]);
        if (std::none_of(keys.begin(), keys.end(), [&](const char* allowed) { return key == allowed; })) return false;
    }
    return true;
}

/** Reads one [wifi] table; with_time accepts the schema 2 connection time. */
bool read_entry(toml_datum_t table, bool with_time, SavedWifi& entry) {
    const auto ssid = toml_get(table, "ssid");
    const auto password = toml_get(table, "password");
    const auto time = toml_get(table, "last_connected");
    const bool shaped = with_time ? only_keys(table, {"ssid", "password", "last_connected"})
                                  : only_keys(table, {"ssid", "password"});
    if (!shaped || ssid.type != TOML_STRING || password.type != TOML_STRING ||
        (time.type != TOML_UNKNOWN && (time.type != TOML_INT64 || time.u.int64 < 0)))
        return false;
    entry.ssid.assign(ssid.u.str.ptr, ssid.u.str.len);
    entry.password.assign(password.u.str.ptr, password.u.str.len);
    entry.last_connected = time.type == TOML_INT64 ? time.u.int64 : 0;
    return valid_wifi(entry);
}

std::string quote_name(const std::string& name) { return "“" + name + "”"; }

bool parse_network_bytes(const std::string& bytes, std::vector<SavedWifi>& networks, std::string& error) {
    networks.clear();
    store_internal::configure_toml();
    auto parsed = toml_parse(bytes.data(), static_cast<int>(bytes.size()));
    const auto schema = toml_get(parsed.toptab, "schema_version");
    const auto wifi = toml_get(parsed.toptab, "wifi");
    std::vector<SavedWifi> candidate;
    bool shaped = parsed.ok && only_keys(parsed.toptab, {"schema_version", "wifi", "selected_ssid"}) && schema.type == TOML_INT64;
    if (shaped && schema.u.int64 == 1) {
        candidate.emplace_back();
        shaped = read_entry(wifi, false, candidate.back());
    } else if (shaped && schema.u.int64 == 2 && wifi.type == TOML_ARRAY) {
        shaped = wifi.u.arr.size <= static_cast<int>(saved_limit);
        for (int i = 0; shaped && i < wifi.u.arr.size; ++i) {
            SavedWifi entry;
            shaped = read_entry(wifi.u.arr.elem[i], true, entry) && !listed(entry, candidate);
            candidate.push_back(std::move(entry));
        }
    } else
        shaped = shaped && schema.u.int64 == 2 && wifi.type == TOML_UNKNOWN;
    const auto selected = toml_get(parsed.toptab, "selected_ssid");
    shaped = shaped && (selected.type == TOML_UNKNOWN ||
        (selected.type == TOML_STRING && selected.u.str.len <= 32 &&
         printable(std::string(selected.u.str.ptr, selected.u.str.len))));
    toml_free(parsed);
    if (!shaped) {
        for (auto& entry : candidate) std::fill(entry.password.begin(), entry.password.end(), '\0');
        error = "Invalid network.toml; expected schema_version=2 with up to 5 [[wifi]] ssid/password entries";
        return false;
    }
    networks = std::move(candidate);
    error.clear();
    return true;
}

std::size_t line_offset(const std::string& source, int number) {
    if (number < 1) return std::string::npos;
    std::size_t offset = 0;
    for (int line = 1; line < number; ++line) {
        offset = source.find('\n', offset);
        if (offset == std::string::npos) return offset;
        ++offset;
    }
    return offset;
}

std::size_t leading_comments(const std::string& source, std::size_t header) {
    auto begin = header;
    while (begin > 0) {
        const auto end = begin - 1;
        const auto separator = end ? source.rfind('\n', end - 1) : std::string::npos;
        const auto previous = separator == std::string::npos ? 0 : separator + 1;
        auto line = source.substr(previous, end - previous);
        trim(line);
        if (!line.empty() && line.front() != '#') break;
        begin = previous;
    }
    return begin;
}

/** Moves a connected table while retaining each table's source text and attached comments. */
bool edit_connection_source(const std::string& source, const std::vector<SavedWifi>& desired,
                            std::string& output) {
    if (desired.empty()) return false;
    auto parsed = toml_parse(source.data(), static_cast<int>(source.size()));
    const bool edited = [&]() {
        if (!parsed.ok) return false;
        const auto schema = toml_get(parsed.toptab, "schema_version");
        const auto wifi = toml_get(parsed.toptab, "wifi");
        if (schema.type != TOML_INT64) return false;
        if (schema.u.int64 == 1 && wifi.type == TOML_TABLE) {
            const auto version_line = line_offset(source, schema.lineno);
            const auto header = line_offset(source, wifi.lineno);
            if (schema.colno < 1 || version_line == std::string::npos || header == std::string::npos) return false;
            const auto version = version_line + static_cast<std::size_t>(schema.colno - 1);
            if (version >= source.size() || source[version] != '1') return false;
            const auto end = source.find('\n', header);
            const auto left = source.find('[', header);
            const auto right = source.find(']', left);
            if (left == std::string::npos || right == std::string::npos ||
                (end != std::string::npos && right >= end)) return false;
            auto converted = source;
            converted[version] = '2';
            converted.insert(right + 1, 1, ']');
            converted.insert(left, 1, '[');
            return edit_connection_source(converted, desired, output);
        }
        if (schema.u.int64 != 2 || wifi.type != TOML_ARRAY ||
            wifi.u.arr.size != static_cast<int>(desired.size())) return false;
        std::vector<std::size_t> starts;
        starts.reserve(desired.size());
        std::size_t selected = desired.size();
        for (int i = 0; i < wifi.u.arr.size; ++i) {
            const auto table = wifi.u.arr.elem[i];
            const auto header = line_offset(source, table.lineno);
            if (header == std::string::npos) return false;
            const auto start = leading_comments(source, header);
            if (!starts.empty() && start <= starts.back()) return false;
            starts.push_back(start);
            const auto name = toml_get(table, "ssid");
            if (name.type == TOML_STRING && std::string(name.u.str.ptr, name.u.str.len) == desired.back().ssid)
                selected = static_cast<std::size_t>(i);
        }
        if (selected == desired.size()) return false;
        std::vector<std::string> blocks;
        for (std::size_t i = 0; i < starts.size(); ++i)
            blocks.push_back(source.substr(starts[i], (i + 1 == starts.size() ? source.size() : starts[i + 1]) - starts[i]));
        const auto timestamp = toml_get(wifi.u.arr.elem[selected], "last_connected");
        if (desired.back().last_connected != (timestamp.type == TOML_INT64 ? timestamp.u.int64 : 0)) {
            const auto value = std::to_string(desired.back().last_connected);
            if (timestamp.type == TOML_INT64) {
                const auto value_line = line_offset(source, timestamp.lineno);
                if (timestamp.colno < 1 || value_line == std::string::npos) return false;
                const auto absolute = value_line + static_cast<std::size_t>(timestamp.colno - 1);
                if (absolute < starts[selected] ||
                    absolute >= starts[selected] + blocks[selected].size()) return false;
                const auto begin = absolute - starts[selected];
                auto end = begin;
                while (end < blocks[selected].size() && !std::isspace(static_cast<unsigned char>(blocks[selected][end])) &&
                       blocks[selected][end] != '#') ++end;
                blocks[selected].replace(begin, end - begin, value);
            } else if (timestamp.type == TOML_UNKNOWN) {
                if (!blocks[selected].empty() && blocks[selected].back() != '\n') blocks[selected] += '\n';
                blocks[selected] += "last_connected = " + value + "\n";
            } else return false;
        }
        output = source.substr(0, starts.front());
        for (std::size_t i = 0; i < blocks.size(); ++i)
            if (i != selected) output += blocks[i];
        output += blocks[selected];
        std::vector<SavedWifi> checked;
        std::string error;
        if (output.size() > 4096 || !parse_network_bytes(output, checked, error) || checked.size() != desired.size())
            return false;
        for (std::size_t i = 0; i < checked.size(); ++i)
            if (checked[i].ssid != desired[i].ssid || checked[i].password != desired[i].password ||
                checked[i].last_connected != desired[i].last_connected) return false;
        return true;
    }();
    toml_free(parsed);
    return edited;
}

bool same_networks(const std::string& source, const std::vector<SavedWifi>& desired) {
    if (source.size() > 4096) return false;
    std::vector<SavedWifi> checked;
    std::string error;
    if (!parse_network_bytes(source, checked, error) || checked.size() != desired.size()) return false;
    for (std::size_t i = 0; i < checked.size(); ++i)
        if (checked[i].ssid != desired[i].ssid || checked[i].password != desired[i].password ||
            checked[i].last_connected != desired[i].last_connected) return false;
    return true;
}

/** Replaces one basic or literal TOML string without changing its surrounding text. */
bool replace_string_value(std::string& source, toml_datum_t value, const std::string& replacement) {
    const auto line = line_offset(source, value.lineno);
    if (value.colno < 1 || line == std::string::npos) return false;
    const auto content = line + static_cast<std::size_t>(value.colno - 1);
    if (content == 0 || content >= source.size()) return false;
    // tomlc17 reports the first character inside the opening string delimiter.
    const auto begin = content - 1;
    if (source[begin] != '"' && source[begin] != '\'') return false;
    const char quote = source[begin];
    if (begin + 2 < source.size() && source[begin + 1] == quote && source[begin + 2] == quote) return false;
    std::size_t end = begin + 1;
    for (; end < source.size() && source[end] != '\n'; ++end) {
        if (quote == '"' && source[end] == '\\') { ++end; continue; }
        if (source[end] == quote) break;
    }
    if (end >= source.size() || source[end] != quote) return false;
    source.replace(begin, end - begin + 1, quoted(replacement));
    return true;
}

/** Preserves existing tables and comments when a phone form changes one network. */
bool edit_network_source(const std::string& source, bool missing, const WifiCredentials& wifi,
                         bool remove, std::int64_t now, std::string& output, std::string& error) {
    std::vector<SavedWifi> current;
    if (!missing && !parse_network_bytes(source, current, error)) return false;
    auto desired = current;
    const bool valid_change = remove ? remove_network(desired, wifi.ssid) : add_network(desired, wifi, now);
    if (!valid_change) { error = "Network is absent or saved network limit was reached"; return false; }
    std::string working = missing ? "schema_version = 2\n" : source;
    if (!missing && !current.empty()) {
        auto parsed = toml_parse(working.data(), static_cast<int>(working.size()));
        const auto schema = toml_get(parsed.toptab, "schema_version");
        const bool legacy = parsed.ok && schema.type == TOML_INT64 && schema.u.int64 == 1;
        toml_free(parsed);
        if (legacy && !edit_connection_source(source, current, working)) {
            error = "Legacy network.toml syntax cannot be preserved";
            return false;
        }
    }
    auto parsed = toml_parse(working.data(), static_cast<int>(working.size()));
    const bool edited = [&]() {
        if (!parsed.ok) return false;
        const auto entries = toml_get(parsed.toptab, "wifi");
        const auto found = std::find_if(current.begin(), current.end(),
                                        [&](const SavedWifi& entry) { return entry.ssid == wifi.ssid; });
        if (found == current.end() && !remove) {
            output = working;
            if (!output.empty() && output.back() != '\n') output += '\n';
            output += "\n[[wifi]]\nssid = " + quoted(wifi.ssid) + "\npassword = " + quoted(wifi.password) + "\n";
            if (desired.back().last_connected > 0)
                output += "last_connected = " + std::to_string(desired.back().last_connected) + "\n";
            return same_networks(output, desired);
        }
        if (entries.type != TOML_ARRAY || entries.u.arr.size != static_cast<int>(current.size())) return false;
        const auto index = static_cast<int>(found - current.begin());
        if (index < 0 || index >= entries.u.arr.size) return false;
        const auto table = entries.u.arr.elem[index];
        const auto header = line_offset(working, table.lineno);
        if (header == std::string::npos) return false;
        if (remove) {
            const auto begin = leading_comments(working, header);
            const auto end = index + 1 < entries.u.arr.size
                ? leading_comments(working, line_offset(working, entries.u.arr.elem[index + 1].lineno))
                : working.size();
            if (end == std::string::npos || end <= begin) return false;
            output = working;
            output.erase(begin, end - begin);
            return same_networks(output, desired);
        }
        const auto password = toml_get(table, "password");
        if (password.type != TOML_STRING) return false;
        auto changed = working;
        if (found->password != wifi.password && !replace_string_value(changed, password, wifi.password)) {
            error = "Network password syntax cannot be preserved";
            return false;
        }
        if (!edit_connection_source(changed, desired, output)) {
            error = "Network order or time syntax cannot be preserved";
            return false;
        }
        return same_networks(output, desired);
    }();
    toml_free(parsed);
    if (!edited && error.empty()) error = "network.toml syntax cannot be preserved for this edit";
    return edited;
}

/** The pinned network name; empty, also for a missing key, selects automatic connection. */
std::string selected_name(const std::string& source) {
    auto parsed = toml_parse(source.data(), static_cast<int>(source.size()));
    const auto value = toml_get(parsed.toptab, "selected_ssid");
    std::string name = value.type == TOML_STRING ? std::string(value.u.str.ptr, value.u.str.len) : "";
    toml_free(parsed);
    return name;
}
bool select_source(std::string& source, const std::string& ssid) {
    auto parsed = toml_parse(source.data(), static_cast<int>(source.size()));
    const auto value = toml_get(parsed.toptab, "selected_ssid");
    bool okay = parsed.ok;
    if (okay && value.type == TOML_UNKNOWN && ssid.empty()) {
        // Automatic connection needs no key.
    } else if (okay && value.type == TOML_UNKNOWN) {
        const auto schema = toml_get(parsed.toptab, "schema_version");
        const auto line = line_offset(source, schema.lineno);
        const auto end = line == std::string::npos ? std::string::npos : source.find('\n', line);
        if (end == std::string::npos) { source += "\nselected_ssid = " + quoted(ssid) + "\n"; }
        else source.insert(end + 1, "selected_ssid = " + quoted(ssid) + "\n");
    }
    else if (okay) okay = replace_string_value(source, value, ssid);
    toml_free(parsed);
    return okay && source.size() <= 4096;
}

}  // namespace

bool valid_wifi(const WifiCredentials& credentials) {
    const auto& password = credentials.password;
    if (credentials.ssid.empty() || credentials.ssid.size() > 32 || !printable(credentials.ssid) ||
        password.size() > 64 || !printable(password)) return false;
    if (password.size() == 64) return std::all_of(password.begin(), password.end(), [](char c) { return hex_digit(c) >= 0; });
    return password.empty() || password.size() >= 8;
}

bool load_networks(const std::string& card_root, std::vector<SavedWifi>& networks, std::string& error) {
    networks.clear();
    std::string bytes;
    const auto status = store_internal::read_file(card_root, "PicoPage/network.toml", 4096, bytes);
    if (status == StoreStatus::Missing) {
        error.clear();
        return true;
    }
    if (status != StoreStatus::Ok) {
        error = "Cannot read local PicoPage/network.toml";
        return false;
    }
    const bool shaped = parse_network_bytes(bytes, networks, error);
    std::fill(bytes.begin(), bytes.end(), '\0');
    return shaped;
}

bool load_selected_network(const std::string& root, std::optional<SavedWifi>& selected, std::string& error) {
    selected.reset();
    std::string source;
    const auto status = store_internal::read_file(root, "PicoPage/network.toml", 4096, source);
    if (status == StoreStatus::Missing) { error.clear(); return true; }
    std::vector<SavedWifi> saved;
    if (status != StoreStatus::Ok || !parse_network_bytes(source, saved, error)) {
        if (error.empty()) error = "Cannot read local PicoPage/network.toml";
        return false;
    }
    const auto name = selected_name(source);
    const auto found = std::find_if(saved.begin(), saved.end(), [&](const SavedWifi& wifi) { return wifi.ssid == name; });
    if (found != saved.end()) selected = *found;
    error.clear();
    return true;
}
bool save_selected_network(const std::string& root, const std::string& ssid, std::string& error) {
    std::string source;
    std::vector<SavedWifi> saved;
    if (store_internal::read_file(root, "PicoPage/network.toml", 4096, source) != StoreStatus::Ok ||
        !parse_network_bytes(source, saved, error)) return false;
    if (!ssid.empty() && std::none_of(saved.begin(), saved.end(), [&](const SavedWifi& wifi) { return wifi.ssid == ssid; })) {
        error = "Selected Wi-Fi is not saved"; return false;
    }
    auto updated = source;
    if (!select_source(updated, ssid)) { error = "Cannot preserve network selection syntax"; return false; }
    return store_internal::commit_file(root, "PicoPage/network.toml", std::optional<std::string>{source},
        updated, 4096, error, true) == StoreStatus::Ok;
}

std::string network_toml(const std::vector<SavedWifi>& networks) {
    std::string out = "schema_version = 2\n";
    for (const auto& entry : networks) {
        out += "\n[[wifi]]\nssid = " + quoted(entry.ssid) + "\npassword = " + quoted(entry.password) + "\n";
        if (entry.last_connected > 0) out += "last_connected = " + std::to_string(entry.last_connected) + "\n";
    }
    return out;
}

std::string sync_toml(const sync::Config& config) {
    return "schema_version = 1\nendpoint = " + quoted(config.endpoint) + "\nusername = " + quoted(config.username) +
           "\nuserkey = " + quoted(config.userkey) + "\ndevice_id = " + quoted(config.device_id) +
           "\ndom_version = " + std::to_string(config.dom_version) + "\n";
}

const SavedWifi* last_network(const std::vector<SavedWifi>& saved) {
    return saved.empty() ? nullptr : &saved.back();
}

bool record_connection(std::vector<SavedWifi>& saved, const std::string& ssid, std::int64_t now) {
    const auto found = std::find_if(saved.begin(), saved.end(),
                                    [&](const SavedWifi& entry) { return entry.ssid == ssid; });
    if (found == saved.end()) return false;
    bool changed = false;
    if (now >= clock_floor &&
        !(now - found->last_connected < 3600 && found->last_connected - now < 3600)) {
        found->last_connected = now;
        changed = true;
    }
    if (found + 1 != saved.end()) {
        std::rotate(found, found + 1, saved.end());
        changed = true;
    }
    return changed;
}

bool save_connection(const std::string& card_root, const std::string& ssid, std::int64_t now,
                     std::string& error) {
    std::string source;
    const auto status = store_internal::read_file(card_root, "PicoPage/network.toml", 4096, source);
    if (status != StoreStatus::Ok) {
        error = "PicoPage/network.toml cannot be read for a connection update";
        return false;
    }
    std::vector<SavedWifi> saved;
    if (!parse_network_bytes(source, saved, error)) return false;
    if (std::none_of(saved.begin(), saved.end(), [&](const SavedWifi& entry) { return entry.ssid == ssid; })) {
        error = "Connected Wi-Fi is absent from PicoPage/network.toml";
        return false;
    }
    const auto target = selected_name(source);
    if (!record_connection(saved, ssid, now)) {
        error.clear();
        return true;
    }
    std::string updated;
    if (!edit_connection_source(source, saved, updated) ||
        !select_source(updated, target)) {
        error = "PicoPage/network.toml syntax cannot be preserved for a connection update";
        return false;
    }
    return store_internal::commit_file(card_root, "PicoPage/network.toml",
                                       std::optional<std::string>{source}, updated, 4096, error, true) == StoreStatus::Ok;
}

bool save_network_edit(const std::string& card_root, const WifiCredentials& wifi, bool remove,
                       std::int64_t now, std::string& error) {
    std::string source;
    const auto status = store_internal::read_file(card_root, "PicoPage/network.toml", 4096, source);
    if (status != StoreStatus::Ok && status != StoreStatus::Missing) {
        error = "PicoPage/network.toml cannot be read for editing";
        return false;
    }
    std::string updated;
    if (!edit_network_source(source, status == StoreStatus::Missing, wifi, remove, now, updated, error))
        return false;
    std::vector<SavedWifi> original;
    if (status != StoreStatus::Missing && !parse_network_bytes(source, original, error)) return false;
    auto target = selected_name(source);
    // Forgetting the pinned network returns to automatic connection; adding never pins. A name
    // left behind by a hand edit already means automatic and must not pin a network added later.
    if ((remove && target == wifi.ssid) ||
        std::none_of(original.begin(), original.end(), [&](const SavedWifi& entry) { return entry.ssid == target; }))
        target.clear();
    if (!select_source(updated, target)) { error = "Cannot preserve network selection syntax"; return false; }
    const auto expected = status == StoreStatus::Missing ? std::optional<std::string>{}
                                                          : std::optional<std::string>{source};
    return store_internal::commit_file(card_root, "PicoPage/network.toml", expected, updated,
                                       4096, error, true) == StoreStatus::Ok;
}

bool add_network(std::vector<SavedWifi>& saved, const WifiCredentials& credentials, std::int64_t now) {
    const auto time = now >= clock_floor ? now : 0;
    for (auto entry = saved.begin(); entry != saved.end(); ++entry) {
        if (entry->ssid != credentials.ssid) continue;
        entry->password = credentials.password;
        if (time) entry->last_connected = time;
        std::rotate(entry, entry + 1, saved.end());
        return true;
    }
    if (saved.size() >= saved_limit) return false;
    SavedWifi entry;
    static_cast<WifiCredentials&>(entry) = credentials;
    entry.last_connected = time;
    saved.push_back(std::move(entry));
    return true;
}

bool remove_network(std::vector<SavedWifi>& saved, const std::string& ssid) {
    const auto found = std::find_if(saved.begin(), saved.end(), [&](const SavedWifi& entry) { return entry.ssid == ssid; });
    if (found == saved.end()) return false;
    saved.erase(found);
    return true;
}

std::vector<WifiCredentials> connection_order(const std::vector<WifiCredentials>& saved,
                                              const std::vector<NearbyWifi>& nearby) {
    std::vector<WifiCredentials> order;
    // nearby is strongest first, so the scan order is the attempt order.
    for (const auto& found : nearby) {
        const auto named = [&](const WifiCredentials& wifi) { return wifi.ssid == found.ssid; };
        const auto entry = std::find_if(saved.begin(), saved.end(), named);
        if (entry != saved.end() && std::none_of(order.begin(), order.end(), named)) order.push_back(*entry);
    }
    return order;
}

// WIFI_REASON_NO_AP_FOUND and its security, authentication-mode and signal variants.
bool network_absent(int reason) { return reason == 201 || (reason >= 210 && reason <= 212); }
// Authentication expiry, MIC failure, both handshake timeouts and authentication failure.
bool password_rejected(int reason) {
    return reason == 2 || reason == 14 || reason == 15 || reason == 202 || reason == 204;
}

std::string wifi_problem(const std::string& ssid, WifiFailure failure, int reason) {
    const auto name = quote_name(ssid);
    switch (failure) {
    case WifiFailure::Setup: return "设备的 Wi-Fi 没有启动。";
    case WifiFailure::NoAddress: return "连接" + name + "超时，没有取得网络地址。";
    case WifiFailure::NoClock: return "已连接" + name + "，但没有取得网络时间。这个网络可能无法访问互联网。";
    case WifiFailure::Rejected: break;
    }
    const auto code = "（驱动原因 " + std::to_string(reason) + "）。";
    if (network_absent(reason))
        return "找不到名为" + name + "的网络" + code + "设备只支持 2.4 GHz Wi-Fi。";
    if (password_rejected(reason))
        return "无法连接" + name + "，密码可能不正确" + code;
    return "无法连接" + name + code;
}

std::string wifi_qr(const WifiCredentials& access) {
    const auto escaped = [](const std::string& value) {
        std::string out;
        for (const char c : value) {
            if (c == '\\' || c == ';' || c == ',' || c == ':' || c == '"') out += '\\';
            out += c;
        }
        return out;
    };
    return "WIFI:T:WPA;S:" + escaped(access.ssid) + ";P:" + escaped(access.password) + ";;";
}

}  // namespace pico_page::provision
