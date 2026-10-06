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

std::string quote_name(const std::string& name) { return "“" + name + "”"; }
}  // namespace

bool valid_wifi(const WifiCredentials& credentials) {
    const auto& password = credentials.password;
    if (credentials.ssid.empty() || credentials.ssid.size() > 32 || !printable(credentials.ssid) ||
        password.size() > 64 || !printable(password)) return false;
    if (password.size() == 64) return std::all_of(password.begin(), password.end(), [](char c) { return hex_digit(c) >= 0; });
    return password.empty() || password.size() >= 8;
}

bool load_wifi(const std::string& card_root, WifiCredentials& credentials, std::string& error) {
    credentials = {};
    std::string bytes;
    if (store_internal::read_file(card_root, "PicoPage/network.toml", 4096, bytes) != StoreStatus::Ok) {
        error = "Cannot read local PicoPage/network.toml";
        return false;
    }
    store_internal::configure_toml();
    auto parsed = toml_parse(bytes.data(), static_cast<int>(bytes.size()));
    const auto schema = toml_get(parsed.toptab, "schema_version");
    const auto table = toml_get(parsed.toptab, "wifi");
    const auto ssid = toml_get(table, "ssid");
    const auto password = toml_get(table, "password");
    WifiCredentials candidate;
    const bool shaped = parsed.ok && only_keys(parsed.toptab, {"schema_version", "wifi"}) &&
        only_keys(table, {"ssid", "password"}) && schema.type == TOML_INT64 && schema.u.int64 == 1 &&
        ssid.type == TOML_STRING && password.type == TOML_STRING;
    if (shaped) {
        candidate.ssid.assign(ssid.u.str.ptr, ssid.u.str.len);
        candidate.password.assign(password.u.str.ptr, password.u.str.len);
    }
    toml_free(parsed);
    if (!shaped || !valid_wifi(candidate)) {
        error = "Invalid network.toml; expected schema_version=1 and wifi.ssid/password";
        return false;
    }
    credentials = std::move(candidate);
    error.clear();
    return true;
}

std::string network_toml(const WifiCredentials& credentials) {
    return "schema_version = 1\n[wifi]\nssid = " + quoted(credentials.ssid) +
           "\npassword = " + quoted(credentials.password) + "\n";
}

std::string sync_toml(const sync::Config& config) {
    return "schema_version = 1\nendpoint = " + quoted(config.endpoint) + "\nusername = " + quoted(config.username) +
           "\nuserkey = " + quoted(config.userkey) + "\ndevice_id = " + quoted(config.device_id) +
           "\ndom_version = " + std::to_string(config.dom_version) + "\n";
}

bool parse_form(std::string_view body, Submission& submission, std::string& problem) {
    submission = {};
    if (body.size() > form_limit) { problem = "提交的内容过长。"; return false; }
    std::string ssid, wifi_password, endpoint, username, password;
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
        if (key == "ssid") ssid = std::move(value);
        else if (key == "wifi_password") wifi_password = std::move(value);
        else if (key == "endpoint") endpoint = std::move(value);
        else if (key == "username") username = std::move(value);
        else if (key == "password") password = std::move(value);
    }
    submission.wifi = !ssid.empty();
    submission.account = !password.empty();
    if (!submission.wifi && !submission.account) {
        problem = "没有可保存的内容。请填写 Wi-Fi 名称，或填写同步账户的密码。";
        return false;
    }
    if (submission.wifi) {
        submission.network = {std::move(ssid), std::move(wifi_password)};
        if (!valid_wifi(submission.network)) {
            problem = "Wi-Fi 名称不能超过 32 个字节；密码需要 8 至 63 个字节，开放网络留空。";
            return false;
        }
    }
    if (submission.account) {
        trim(endpoint);
        trim(username);
        if (endpoint.empty()) endpoint = default_endpoint;
        while (endpoint.size() > 8 && endpoint.back() == '/') endpoint.pop_back();
        submission.endpoint = std::move(endpoint);
        submission.username = std::move(username);
        submission.userkey = md5_hex(password);
        // device_id is supplied when the file is written; any valid value serves the check.
        if (!sync::valid_config({submission.endpoint, submission.username, submission.userkey, "device", 20260812})) {
            problem = "同步服务地址必须以 https:// 开头，账户不能为空。";
            return false;
        }
    }
    return true;
}

std::string page(const PageState& state) {
    std::string out =
        "<!doctype html><html lang=\"zh-CN\"><meta charset=\"utf-8\">"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">";
    if (state.busy) out += "<meta http-equiv=\"refresh\" content=\"5;url=/\">";
    out +=
        "<title>Pico Page 配置</title><style>"
        "body{font:16px/1.5 system-ui,sans-serif;margin:0 auto;max-width:28em;padding:16px}"
        "h1{font-size:20px}fieldset{border:1px solid #999;margin:0 0 16px;padding:12px}"
        "label{display:block;margin-top:8px}"
        "input{box-sizing:border-box;width:100%;font:inherit;padding:8px;border:1px solid #666}"
        "button{width:100%;font:inherit;padding:12px;background:#000;color:#fff;border:0}"
        ".notice{border:2px solid #000;padding:8px;margin-bottom:16px}small{color:#555}"
        "</style><h1>Pico Page 配置</h1>";
    if (!state.notice.empty()) out += "<p class=\"notice\">" + html(state.notice) + "</p>";
    if (state.busy) return out + "<p>设备正在验证，结果显示在设备屏幕上。本页每 5 秒刷新一次。</p>";
    out += "<form method=\"post\" action=\"/save\"><fieldset><legend>Wi-Fi</legend>"
           "<label>网络名称<input name=\"ssid\" list=\"networks\" maxlength=\"32\" autocomplete=\"off\" "
           "autocapitalize=\"none\"></label><datalist id=\"networks\">";
    std::size_t listed = 0;
    for (const auto& name : state.networks) {
        if (name.empty() || name.size() > 32 || !printable(name)) continue;
        if (listed++ == network_limit) break;
        out += "<option value=\"" + html(name) + "\">";
    }
    out += "</datalist><label>密码<input name=\"wifi_password\" type=\"password\" maxlength=\"64\"></label><small>";
    if (!state.current_ssid.empty()) out += "当前使用" + html(quote_name(state.current_ssid)) + "。";
    out += "只支持 2.4 GHz 网络。网络名称留空表示不修改 Wi-Fi；开放网络的密码留空。</small></fieldset>"
           "<fieldset><legend>KOReader 同步</legend>"
           "<label>服务地址<input name=\"endpoint\" maxlength=\"256\" inputmode=\"url\" autocapitalize=\"none\" value=\"" +
           html(state.endpoint.empty() ? default_endpoint : state.endpoint) +
           "\"></label><label>账户<input name=\"username\" maxlength=\"256\" autocomplete=\"off\" "
           "autocapitalize=\"none\" value=\"" + html(state.username) +
           "\"></label><label>密码<input name=\"password\" type=\"password\" maxlength=\"256\"></label>"
           "<small>密码留空表示不修改同步账户。设备只保存密码的 MD5，与 KOReader 的做法相同。</small></fieldset>"
           "<button>验证并保存</button></form>"
           "<p><small>设备先连接 Wi-Fi 并验证账户，成功后才保存。结果显示在设备屏幕上。</small></p>";
    return out;
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
    std::string out = "已保存";
    if (submission.wifi) out += " Wi-Fi " + quote_name(submission.network.ssid);
    if (submission.wifi && submission.account) out += "和";
    if (submission.account) out += "同步账户" + quote_name(submission.username);
    return out + "。";
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
