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

std::string html(const std::string& value) {
    std::string out;
    for (const char c : value) {
        switch (c) {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        case '"': out += "&quot;"; break;
        case '\'': out += "&#39;"; break;
        default: out += c;
        }
    }
    return out;
}

int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    c = static_cast<char>(c | 0x20);
    return c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

bool decode(std::string_view encoded, std::string& out) {
    out.clear();
    for (std::size_t i = 0; i < encoded.size(); ++i) {
        if (encoded[i] == '+') out += ' ';
        else if (encoded[i] != '%') out += encoded[i];
        else {
            if (i + 2 >= encoded.size()) return false;
            const int high = hex_digit(encoded[i + 1]), low = hex_digit(encoded[i + 2]);
            if (high < 0 || low < 0) return false;
            out += static_cast<char>(high * 16 + low);
            i += 2;
        }
    }
    return true;
}

void trim(std::string& value) {
    while (!value.empty() && value.back() == ' ') value.pop_back();
    value.erase(0, value.find_first_not_of(' '));
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

const NearbyWifi* find_nearby(const std::vector<NearbyWifi>& nearby, const std::string& ssid) {
    const auto found = std::find_if(nearby.begin(), nearby.end(), [&](const NearbyWifi& item) { return item.ssid == ssid; });
    return found == nearby.end() ? nullptr : &*found;
}

bool listed(const SavedWifi& entry, const std::vector<SavedWifi>& saved) {
    return std::any_of(saved.begin(), saved.end(), [&](const SavedWifi& item) { return item.ssid == entry.ssid; });
}

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

/** Missing selection uses the legacy fallback until the first compared edit freezes it. */
std::string selected_name(const std::string& source, const std::vector<SavedWifi>& saved,
                          bool* explicit_selection = nullptr) {
    auto parsed = toml_parse(source.data(), static_cast<int>(source.size()));
    const auto value = toml_get(parsed.toptab, "selected_ssid");
    if (explicit_selection) *explicit_selection = value.type == TOML_STRING;
    std::string name = value.type == TOML_STRING ? std::string(value.u.str.ptr, value.u.str.len)
        : saved.empty() ? "" : saved.back().ssid;
    toml_free(parsed);
    return name;
}
bool select_source(std::string& source, const std::string& ssid) {
    auto parsed = toml_parse(source.data(), static_cast<int>(source.size()));
    const auto value = toml_get(parsed.toptab, "selected_ssid");
    bool okay = parsed.ok;
    if (okay && value.type == TOML_UNKNOWN) {
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

/** A form field that only an account submission carries. */
std::string field(const std::string& label, const std::string& input, bool strong = false) {
    return std::string("<label class=\"field\"><span") + (strong ? " class=\"strong\"" : "") + ">" + label +
           "</span><span class=\"input\">" + input + "</span></label>";
}

std::string password_input(const std::string& name, const std::string& id, std::size_t limit,
                           const std::string& placeholder = "") {
    return "<input name=\"" + name + "\" id=\"" + id + "\" type=\"password\" maxlength=\"" + std::to_string(limit) +
           "\" autocomplete=\"off\"" + (placeholder.empty() ? "" : " placeholder=\"" + placeholder + "\"") +
           "><button type=\"button\" class=\"link\" data-show=\"" + id + "\">显示</button>";
}

std::string alert(const std::string& title, const std::string& body) {
    return "<div class=\"alert\" role=\"alert\"><b>" + html(title) + "</b><span>" + html(body) + "</span></div>";
}

std::string busy_block(const PageState& state) {
    std::string out = "<ol class=\"stages\">";
    const auto steps = stages(state.busy_action, state.busy_name, state.busy_verify_wifi);
    for (std::size_t i = 0; i < steps.size(); ++i) {
        const char* kind = i < state.stage ? "done" : i == state.stage ? "now" : "next";
        out += std::string("<li class=\"") + kind + "\">" + html(steps[i]) + (i == state.stage ? "…" : "") + "</li>";
    }
    out += "</ol><p class=\"hint\">结果也会显示在设备屏幕上。本页每 2 秒刷新一次。</p>";
    if (state.busy_action == Action::Account || state.busy_verify_wifi)
        out += "<p class=\"hint\">验证时手机可能暂时断开设备热点，请看设备屏幕上的结果。"
               "继续修改时，重新连接设备热点并打开配置页。</p>";
    return out;
}

const char page_style[] =
    "*{box-sizing:border-box}"
    "body{margin:0 auto;max-width:560px;background:#fff;color:#111;"
    "font:16px/1.5 system-ui,-apple-system,'PingFang SC','Noto Sans SC',sans-serif}"
    "header{padding:18px 20px;border-bottom:3px solid #111}"
    "h1{font-size:22px;margin:0}h2{font-size:18px;margin:0}"
    ".mono{font-family:ui-monospace,Menlo,Consolas,monospace;font-size:13px;color:#333}"
    "section{padding:20px;display:flex;flex-direction:column;gap:14px;border-bottom:3px solid #111}"
    "section:last-of-type{border-bottom:0;padding-bottom:24px}"
    ".head{display:flex;justify-content:space-between;align-items:baseline;gap:12px}"
    ".meta{font-size:14px;color:#333}.dim,.dim h2{color:#555}"
    ".list{display:flex;flex-direction:column;border-bottom:1px solid #cfcfcf}"
    ".row{min-height:60px;display:flex;align-items:center;justify-content:space-between;gap:12px;border-top:1px solid #cfcfcf}"
    ".row>div{display:flex;flex-direction:column;min-width:0;overflow-wrap:anywhere}"
    ".row small,.pick small{font-size:13px;color:#333;font-weight:400}"
    ".pick{min-height:48px;display:flex;align-items:center;gap:12px;border-top:1px solid #cfcfcf;overflow-wrap:anywhere}"
    ".pick span{flex:1}.pick:has(input:checked){font-weight:700}"
    "input[type=radio]{width:18px;height:18px;margin:0;accent-color:#111;flex-shrink:0}"
    ".link{background:none;border:0;padding:0 4px;min-width:64px;min-height:44px;color:#111;"
    "font:inherit;font-size:15px;text-decoration:underline;cursor:pointer;flex-shrink:0}"
    ".btn{display:flex;align-items:center;justify-content:center;width:100%;height:52px;border:2px solid #111;"
    "background:#fff;color:#111;font:inherit;font-size:17px;cursor:pointer}"
    ".primary{background:#111;color:#fff}"
    ".btn:disabled{border:2px dashed #999;background:#fff;color:#555;cursor:default}"
    ".field{display:flex;flex-direction:column;gap:6px;font-size:14px}.strong{font-weight:700}"
    ".input{display:flex;align-items:center;gap:8px;height:46px;border:2px solid #111;padding:0 0 0 12px}"
    ".field.strong .input,.input:focus-within{border-width:3px}"
    ".input input{flex:1;min-width:0;border:0;outline:0;background:transparent;font:inherit;font-size:16px}"
    ".hint{margin:0;font-size:13px;color:#333;line-height:1.6}"
    ".alert{border:3px solid #111;padding:12px 16px;display:flex;flex-direction:column;gap:4px;font-size:14px;line-height:1.6}"
    ".alert b{font-size:17px}.ok{border:2px solid #111;padding:12px 16px;font-size:15px}"
    ".pair{display:grid;grid-template-columns:1fr 2fr;gap:10px}"
    ".stages{margin:0;padding-left:22px;display:flex;flex-direction:column;gap:6px}"
    ".stages .now{font-weight:700}.stages .done{color:#555}.stages .next{color:#555}"
    "[hidden]{display:none!important}";

// Toggles the collapsed blocks, the password visibility and the manual network name, and shows
// connection dates in the phone's own time zone. Without it every block stays usable.
const char page_script[] =
    "<script>"
    "const $=id=>document.getElementById(id),on=(id,f)=>{const e=$(id);if(e)e.onclick=f;};"
    "const swap=(hide,show)=>{for(const id of hide){const e=$(id);if(e)e.hidden=true;}"
    "for(const id of show){const e=$(id);if(e)e.hidden=false;}};"
    "on('add-open',()=>swap(['wifi-view'],['wifi-add']));"
    "on('add-cancel',()=>swap(['wifi-add'],['wifi-view']));"
    "on('account-open',()=>swap(['account-view'],['account-form']));"
    "on('account-cancel',()=>swap(['account-form'],['account-view']));"
    "for(const b of document.querySelectorAll('[data-show]'))b.onclick=()=>{const i=$(b.dataset.show);"
    "const shown=i.type==='password';i.type=shown?'text':'password';b.textContent=shown?'隐藏':'显示';};"
    "const manual=$('manual'),label=$('pw-label'),pick=()=>{const c=document.querySelector('input[name=ssid]:checked');"
    "if(manual)manual.hidden=!!(c&&c.value);"
    "if(label)label.textContent=c&&c.value?'“'+c.value+'”的密码':'密码';"
    "const submit=$('wifi-submit');if(submit)submit.textContent=c&&c.value?'验证并保存 Wi-Fi':'保存 Wi-Fi';};"
    "for(const r of document.querySelectorAll('input[name=ssid]'))r.onchange=pick;pick();"
    "const now=new Date();for(const e of document.querySelectorAll('[data-t]')){const d=new Date(e.dataset.t*1000);"
    "const day=d.toDateString()===now.toDateString()?'今天':' '+(d.getFullYear()===now.getFullYear()?'':d.getFullYear()+' 年 ')"
    "+(d.getMonth()+1)+' 月 '+d.getDate()+' 日';e.textContent=' · 上次连接'+day;}"
    "</script>";

std::string wifi_section(const PageState& state) {
    const auto& saved = state.saved;
    std::string out = "<section id=\"wifi\">";
    if (state.busy && state.busy_action != Action::Account) {
        const auto title = state.busy_action == Action::WifiAdd ? "Wi-Fi · 添加网络" : "Wi-Fi · 删除网络";
        return out + "<div class=\"head\"><h2>" + title + "</h2></div>" + busy_block(state) + "</section>";
    }
    if (!state.saved_readable) {
        return out + "<div class=\"head\"><h2>Wi-Fi</h2><span class=\"meta\">无法读取</span></div>" +
               alert("PicoPage/network.toml 内容无效",
                     "设备不会改动这个文件。请在电脑上检查或删除它，然后重新开启手机配置。") + "</section>";
    }
    const auto& result = state.result;
    const bool wifi_result = result && result->action != Action::Account;
    const bool add_failed = wifi_result && result->failed && result->action == Action::WifiAdd;
    std::string notice;
    if (wifi_result)
        notice = result->failed
            ? alert(result->action == Action::WifiAdd ? "Wi-Fi 未保存" : "Wi-Fi 未删除",
                    result->text + "已保存的网络和同步账户不受影响。")
            : "<div class=\"ok\" role=\"status\">" + html(result->text) + "</div>";
    if (wifi_result && !result->failed && result->action == Action::WifiAdd && !result->wifi_verified)
        notice += "<p class=\"hint\">配置已保存，尚未测试连接。使用手机热点时，请在设备上结束配置，"
                  "再打开手机的 2.4 GHz 热点，然后使用同步或经已保存的 Wi-Fi 传书。</p>";
    if (wifi_result && !result->failed && result->wifi_verified)
        notice += "<p class=\"hint\">设备已连接该网络并取得 IP 地址。验证结果也显示在设备屏幕上。</p>";
    if (state.busy) {
        // Another block is being verified: show the saved networks without controls.
        out += "<div class=\"head\"><h2>Wi-Fi</h2><span class=\"meta\">" +
               (saved.empty() ? std::string("未配置") : "已保存 " + std::to_string(saved.size()) + " 个") + "</span></div>";
        if (const auto* last = last_network(saved))
            out += "<div class=\"ok\">Wi-Fi：" + html(last->ssid) + " · 已保存</div>";
        return out + "</section>";
    }
    const bool collapsed_by_account = result && result->action == Action::Account && result->failed && !saved.empty();
    if (collapsed_by_account) {
        // A refused account keeps the page on the account block; Wi-Fi is summarised.
        const auto* last = last_network(saved);
        return out + "<div class=\"ok\">Wi-Fi：" + html(last->ssid) + " · 已保存</div></section>";
    }

    const bool full = saved.size() >= saved_limit;
    const bool form_notice = !full && (add_failed || saved.empty());
    const bool adding = !full && (saved.empty() || add_failed);
    if (!saved.empty()) {
        // Saved networks: nearby ones first by signal, then the most recently connected.
        auto ordered = saved;
        std::stable_sort(ordered.begin(), ordered.end(), [&](const SavedWifi& a, const SavedWifi& b) {
            const auto* na = find_nearby(state.nearby, a.ssid);
            const auto* nb = find_nearby(state.nearby, b.ssid);
            if (!na != !nb) return na != nullptr;
            if (na && na->rssi != nb->rssi) return na->rssi > nb->rssi;
            return a.last_connected > b.last_connected;
        });
        out += std::string("<div id=\"wifi-view\" class=\"block\"") + (adding ? " hidden" : "") + ">";
        out += "<div class=\"head\"><h2>Wi-Fi</h2><span class=\"meta\">已保存 " + std::to_string(saved.size()) + " 个，最多 " +
               std::to_string(saved_limit) + " 个</span></div>";
        if (!form_notice) out += notice;
        out += "<div class=\"list\">";
        for (std::size_t i = 0; i < ordered.size(); ++i) {
            const auto& entry = ordered[i];
            const bool near = find_nearby(state.nearby, entry.ssid);
            const auto name = html(entry.ssid);
            out += "<div class=\"row\"><div>" + (i == 0 && near ? "<b>" + name + "</b>" : name) + "<small>" +
                   (near ? "正在附近" : "不在附近");
            if (entry.last_connected >= clock_floor)
                out += "<span data-t=\"" + std::to_string(entry.last_connected) + "\"></span>";
            out += "</small></div><form method=\"post\" action=\"/save\"><input type=\"hidden\" name=\"action\" "
                   "value=\"wifi_delete\"><input type=\"hidden\" name=\"ssid\" value=\"" + name +
                   "\"><button class=\"link\">删除</button></form></div>";
        }
        out += "</div><p class=\"hint\">同步或 Wi-Fi 传书时，设备会连接附近信号最强的已保存网络。</p>";
        out += full ? "<button class=\"btn\" disabled>已保存 5 个，删除一个后才能添加</button>"
                    : "<button type=\"button\" class=\"btn\" id=\"add-open\">添加网络</button>";
        out += "</div>";
    }
    if (full) return out + "</section>";

    // Adding a network: scanned names, a manual name and the password.
    out += std::string("<form id=\"wifi-add\" method=\"post\" action=\"/save\" class=\"block\"") + (adding ? "" : " hidden") +
           "><input type=\"hidden\" name=\"action\" value=\"wifi_add\"><div class=\"head\">" +
           (saved.empty() ? "<h2>Wi-Fi</h2><span class=\"meta\">未配置</span>"
                          : "<h2>Wi-Fi · 添加网络</h2><button type=\"button\" class=\"link\" id=\"add-cancel\">取消</button>") +
           "</div>";
    if (form_notice) out += notice;
    out += "<div class=\"list\" role=\"radiogroup\"><span class=\"meta\">设备附近的网络（都是 2.4 GHz）</span>";
    std::size_t shown = 0;
    bool checked = false;
    for (const auto& item : state.nearby) {
        const auto& name = item.ssid;
        if (name.empty() || name.size() > 32 || !printable(name)) continue;
        if (shown++ == network_limit) break;
        const bool known = listed(SavedWifi{{name, ""}, 0}, saved);
        const bool pick = !known && !checked;
        checked = checked || pick;
        out += "<label class=\"pick\"><input type=\"radio\" name=\"ssid\" value=\"" + html(name) + "\"" +
               (pick ? " checked" : "") + (known ? " disabled" : "") + "><span>" + html(name) + "</span>" +
               (known ? "<small>已保存</small>" : "") + "</label>";
    }
    out += std::string("<label class=\"pick\"><input type=\"radio\" name=\"ssid\" value=\"\"") + (checked ? "" : " checked") +
           "><span>手动输入网络名称…</span></label></div>";
    out += "<label class=\"field\" id=\"manual\"><span>网络名称</span><span class=\"input\"><input name=\"manual\" "
           "maxlength=\"32\" autocomplete=\"off\" autocapitalize=\"none\"></span></label>";
    out += "<label class=\"field\"><span id=\"pw-label\">密码</span><span class=\"input\">" +
           password_input("wifi_password", "wifi-password", 64) + "</span></label>";
    out += "<p class=\"hint\">开放网络不填。未开启的手机热点也可手动填写；设备只支持 2.4 GHz Wi-Fi。</p>"
           "<button id=\"wifi-submit\" class=\"btn primary\">" +
           std::string(checked ? "验证并保存 Wi-Fi" : "保存 Wi-Fi") + "</button>"
           "<p class=\"hint\">从列表选择的网络先验证，成功后保存；手动填写的网络直接保存，不测试连接。"
           "验证时手机可能暂时掉线，请看设备屏幕上的结果。继续修改时，重新连接设备热点并打开配置页。</p>"
           "<p class=\"hint\">使用手机热点时，先保存并在设备上结束配置，再打开手机热点。</p>";
    if (!saved.empty())
        out += "<p class=\"hint\">保存后加入网络列表，已有网络和同步账户不受影响。已满 5 个时需先删除一个。</p>";
    return out + "</form></section>";
}

std::string account_section(const PageState& state) {
    const bool configured = !state.username.empty();
    const auto server = [](const std::string& endpoint) {
        const auto scheme = endpoint.find("://");
        return scheme == std::string::npos ? endpoint : endpoint.substr(scheme + 3);
    };
    std::string out = "<section id=\"account\"";
    const bool no_wifi = state.saved.empty() || !state.saved_readable;
    if (no_wifi && !(state.busy && state.busy_action == Action::Account)) {
        out += " class=\"dim\"><div class=\"head\"><h2>KOReader 同步</h2><span class=\"meta\">需要先保存 Wi-Fi</span></div>"
               "<p class=\"hint\">验证账户要连网，Wi-Fi 保存后才能填写。不用同步的话可以不配。</p>"
               "<button class=\"btn\" disabled>验证并保存账户</button>";
        return out + "</section>";
    }
    out += "><div class=\"head\"><h2>KOReader 同步</h2><span class=\"meta\">" +
           (configured ? "已保存：" + html(state.username) : std::string("未配置")) + "</span></div>";
    if (state.busy) {
        if (state.busy_action == Action::Account) return out + busy_block(state) + "</section>";
        if (configured) out += "<p class=\"hint\">" + html(server(state.endpoint)) + " · 账户 " + html(state.username) + "</p>";
        return out + "</section>";
    }
    const auto& result = state.result;
    const bool account_result = result && result->action == Action::Account;
    const bool failed = account_result && result->failed;
    if (account_result && !failed) out += "<div class=\"ok\" role=\"status\">" + html(result->text) + "</div>";
    if (configured) {
        out += std::string("<div id=\"account-view\" class=\"block\"") + (failed ? " hidden" : "") + "><p class=\"hint\">" +
               html(server(state.endpoint)) + " · 账户 " + html(state.username) +
               "</p><button type=\"button\" class=\"btn\" id=\"account-open\">更换同步账户</button></div>";
    }
    out += std::string("<form id=\"account-form\" method=\"post\" action=\"/save\" class=\"block\"") +
           (configured && !failed ? " hidden" : "") + "><input type=\"hidden\" name=\"action\" value=\"account\">";
    if (failed)
        out += alert("账户未保存", result->text + (configured ? "设备仍使用原账户" + quote_name(state.username) + "，Wi-Fi 不受影响。"
                                                            : std::string("设备没有保存同步账户，Wi-Fi 不受影响。")));
    const auto endpoint = failed ? state.retry_endpoint : state.endpoint.empty() ? std::string(default_endpoint) : state.endpoint;
    const auto username = failed ? state.retry_username : state.username;
    out += field("服务地址", "<input name=\"endpoint\" maxlength=\"256\" inputmode=\"url\" autocapitalize=\"none\" "
                            "autocomplete=\"off\" value=\"" + html(endpoint) + "\">");
    out += field("账户", "<input name=\"username\" maxlength=\"256\" autocapitalize=\"none\" autocomplete=\"off\" value=\"" +
                          html(username) + "\">");
    out += field("密码", password_input("password", "account-password", 256, failed ? "重新输入" : ""), failed);
    out += "<p class=\"hint\">设备只保存密码的 MD5，与 KOReader 的做法相同。";
    if (!configured) out += "不用同步的话可以不配。";
    out += "</p>";
    out += configured ? std::string("<div class=\"pair\"><button type=\"button\" class=\"btn\" id=\"account-cancel\">取消</button>"
                                    "<button class=\"btn primary\">") + (failed ? "重新验证" : "验证并保存账户") + "</button></div>"
                      : "<button class=\"btn primary\">" + std::string(failed ? "重新验证" : "验证并保存账户") + "</button>";
    return out + "</form></section>";
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
    const auto name = selected_name(source, saved);
    const auto found = std::find_if(saved.begin(), saved.end(), [&](const SavedWifi& wifi) { return wifi.ssid == name; });
    if (found != saved.end()) selected = *found;
    else if (!name.empty()) { error = "Selected Wi-Fi is no longer saved"; return false; }
    error.clear();
    return true;
}
bool save_selected_network(const std::string& root, const std::string& ssid, std::string& error) {
    std::string source;
    std::vector<SavedWifi> saved;
    if (store_internal::read_file(root, "PicoPage/network.toml", 4096, source) != StoreStatus::Ok ||
        !parse_network_bytes(source, saved, error)) return false;
    if (std::none_of(saved.begin(), saved.end(), [&](const SavedWifi& wifi) { return wifi.ssid == ssid; })) {
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
    const auto target = selected_name(source, saved);
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
    bool explicit_selection = false;
    auto target = selected_name(source, original, &explicit_selection);
    if (remove && target == wifi.ssid) target.clear();
    else if (!remove && target.empty() && !explicit_selection) target = wifi.ssid;
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

bool parse_form(std::string_view body, Submission& submission, std::string& problem) {
    submission = {};
    if (body.size() > form_limit) { problem = "提交的内容过长。"; return false; }
    std::string action, ssid, manual, wifi_password, endpoint, username, password;
    while (!body.empty()) {
        const auto pair = body.substr(0, body.find('&'));
        body.remove_prefix(std::min(body.size(), pair.size() + 1));
        const auto equals = pair.find('=');
        std::string key, value;
        if (!decode(pair.substr(0, equals), key) ||
            !decode(equals == std::string_view::npos ? std::string_view() : pair.substr(equals + 1), value)) {
            problem = "提交的内容格式不正确。";
            return false;
        }
        if (key == "action") action = std::move(value);
        else if (key == "ssid") ssid = std::move(value);
        else if (key == "manual") manual = std::move(value);
        else if (key == "wifi_password") wifi_password = std::move(value);
        else if (key == "endpoint") endpoint = std::move(value);
        else if (key == "username") username = std::move(value);
        else if (key == "password") password = std::move(value);
    }
    if (action == "wifi_add" || action == "wifi_delete") {
        submission.action = action == "wifi_add" ? Action::WifiAdd : Action::WifiDelete;
        submission.verify_wifi = submission.action == Action::WifiAdd && !ssid.empty();
        submission.network = {ssid.empty() && submission.action == Action::WifiAdd ? std::move(manual) : std::move(ssid),
                              submission.action == Action::WifiAdd ? std::move(wifi_password) : std::string()};
        if (submission.network.ssid.empty()) {
            problem = "请选择或填写 Wi-Fi 名称。";
            return false;
        }
        if (submission.action == Action::WifiAdd && !valid_wifi(submission.network)) {
            problem = "Wi-Fi 名称不能超过 32 个字节；密码需要 8 至 63 个字节，开放网络留空。";
            return false;
        }
        return true;
    }
    if (action != "account") {
        problem = "提交的内容格式不正确。";
        return false;
    }
    submission.action = Action::Account;
    trim(endpoint);
    trim(username);
    if (endpoint.empty()) endpoint = default_endpoint;
    while (endpoint.size() > 8 && endpoint.back() == '/') endpoint.pop_back();
    submission.endpoint = std::move(endpoint);
    submission.username = std::move(username);
    if (password.empty()) {
        problem = "请填写同步账户的密码。";
        return false;
    }
    submission.userkey = md5_hex(password);
    // device_id is supplied when the file is written; any valid value serves the check.
    if (!sync::valid_config({submission.endpoint, submission.username, submission.userkey, "device", 20260812})) {
        problem = "同步服务地址必须以 https:// 开头，账户不能为空。";
        return false;
    }
    return true;
}

std::vector<std::string> stages(Action action, const std::string& ssid, bool verify_wifi) {
    switch (action) {
    case Action::WifiAdd:
        if (verify_wifi) return {"连接" + quote_name(ssid), "保存"};
        return {"保存" + quote_name(ssid)};
    case Action::WifiDelete: return {"删除" + quote_name(ssid)};
    case Action::Account: break;
    }
    return {"连接已保存的 Wi-Fi", "取得网络时间", "验证账户", "保存"};
}

std::string page(const PageState& state) {
    std::string out =
        "<!doctype html><html lang=\"zh-CN\"><meta charset=\"utf-8\">"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">";
    if (state.busy) out += "<meta http-equiv=\"refresh\" content=\"2;url=/\">";
    out += "<title>Pico Page 配置</title><style>" + std::string(page_style) + "</style><header><h1>Pico Page 配置</h1>";
    if (!state.device_name.empty()) out += "<div class=\"mono\">" + html(state.device_name) + "</div>";
    out += "</header>" + wifi_section(state) + account_section(state);
    if (!state.busy) out += page_script;
    return out + "</html>";
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
    // WIFI_REASON_NO_AP_FOUND and its security, authentication-mode and signal variants.
    if (reason == 201 || (reason >= 210 && reason <= 212))
        return "找不到名为" + name + "的网络" + code + "设备只支持 2.4 GHz Wi-Fi。";
    // Authentication expiry, MIC failure, both handshake timeouts and authentication failure.
    if (reason == 2 || reason == 14 || reason == 15 || reason == 202 || reason == 204)
        return "无法连接" + name + "，密码可能不正确" + code;
    return "无法连接" + name + code;
}

std::string account_problem(bool transported, int status, const std::string& detail) {
    if (!transported) return "无法访问同步服务（" + detail + "）。请检查服务地址。";
    if (status == 401 || status == 403) return "同步服务拒绝了这个账户或密码。";
    return "同步服务返回 HTTP " + std::to_string(status) + "，无法确认账户。";
}

std::string saved_summary(const Submission& submission) {
    switch (submission.action) {
    case Action::WifiAdd:
        return std::string(submission.verify_wifi ? "已验证并保存 Wi-Fi " : "已保存 Wi-Fi ") +
               quote_name(submission.network.ssid) + (submission.verify_wifi ? "。" : "，未验证连接。");
    case Action::WifiDelete: return "已删除 Wi-Fi " + quote_name(submission.network.ssid) + "。";
    case Action::Account: break;
    }
    return "已保存同步账户" + quote_name(submission.username) + "。";
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
