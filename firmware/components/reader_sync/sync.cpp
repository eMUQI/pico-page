#include "pico_page/sync.h"
#include "cJSON.h"
#include "tomlc17.h"
#include "store_internal.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>

namespace pico_page::sync {
namespace {
bool plain_header(const std::string& value, std::size_t limit = 256) {
    return !value.empty() && value.size() <= limit &&
        std::none_of(value.begin(), value.end(), [](unsigned char c) { return c < 32 || c == 127; });
}
bool endpoint_valid(const std::string& value) {
    if (value.size() < 9 || value.size() > 1024 || value.compare(0, 8, "https://") != 0) return false;
    return std::none_of(value.begin() + 8, value.end(), [](unsigned char c) {
        return c <= 32 || c >= 127 || c == '@' || c == '#' || c == '?' || c == '\\';
    }) && value[8] != '/';
}
bool digest_valid(const std::string& digest) {
    return digest.size() == 32 && std::all_of(digest.begin(), digest.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}
bool config_string(toml_datum_t root, const char* key, std::string& target) {
    const auto value = toml_get(root, key);
    if (value.type != TOML_STRING || value.u.str.len < 1 || value.u.str.len > 1024 ||
        std::memchr(value.u.str.ptr, '\0', value.u.str.len)) return false;
    target.assign(value.u.str.ptr, value.u.str.len);
    return true;
}
bool begin_request(const Config& config, const std::string& digest, HttpRequest& request,
                   std::string& error) {
    request = {};
    if (!valid_config(config) || !digest_valid(digest)) {
        error = "Invalid sync configuration or document identity"; return false;
    }
    request.headers = {{"accept", "application/vnd.koreader.v1+json"},
                       {"x-auth-user", config.username}, {"x-auth-key", config.userkey}};
    request.scope = account_scope(config);
    error.clear();
    return true;
}
std::string json_string(const cJSON* object, const char* key, std::size_t maximum) {
    const auto* field = cJSON_GetObjectItemCaseSensitive(object, key);
    if (!cJSON_IsString(field) || !field->valuestring || std::strlen(field->valuestring) > maximum) return {};
    return field->valuestring;
}
}  // namespace

bool valid_config(const Config& config) {
    return endpoint_valid(config.endpoint) && plain_header(config.username) &&
           plain_header(config.userkey) && plain_header(config.device_id) &&
           (config.dom_version == 20240114 || config.dom_version == 20260812);
}

bool load_config(const std::string& card_root, Config& config, std::string& error) {
    config = {};
    std::string bytes;
    if (store_internal::read_file(card_root, "PicoPage/sync.toml", 8192, bytes) != StoreStatus::Ok) {
        error = "PicoPage/sync.toml is unavailable or invalid"; return false;
    }
    if (bytes.find('\0') != std::string::npos) { error = "Invalid sync configuration"; return false; }
    store_internal::configure_toml();
    auto parsed = toml_parse(bytes.data(), static_cast<int>(bytes.size()));
    if (!parsed.ok) { toml_free(parsed); error = "Invalid TOML in PicoPage/sync.toml"; return false; }
    Config candidate;
    const auto schema = toml_get(parsed.toptab, "schema_version");
    const auto version = toml_get(parsed.toptab, "dom_version");
    bool valid = schema.type == TOML_INT64 && schema.u.int64 == 1 &&
                 version.type == TOML_INT64 && (version.u.int64 == 20240114 || version.u.int64 == 20260812) &&
                 config_string(parsed.toptab, "endpoint", candidate.endpoint) &&
                 config_string(parsed.toptab, "username", candidate.username) &&
                 config_string(parsed.toptab, "userkey", candidate.userkey) &&
                 config_string(parsed.toptab, "device_id", candidate.device_id);
    if (valid) candidate.dom_version = static_cast<int>(version.u.int64);
    for (int i = 0; valid && i < parsed.toptab.u.tab.size; ++i) {
        const std::string key(parsed.toptab.u.tab.key[i], parsed.toptab.u.tab.len[i]);
        if (key != "schema_version" && key != "dom_version" && key != "endpoint" &&
            key != "username" && key != "userkey" && key != "device_id") valid = false;
    }
    toml_free(parsed);
    while (candidate.endpoint.size() > 8 && candidate.endpoint.back() == '/') candidate.endpoint.pop_back();
    if (!valid || !valid_config(candidate)) { error = "Invalid sync fields; HTTPS and supported DOM version are required"; return false; }
    config = std::move(candidate);
    error.clear();
    return true;
}

std::string account_scope(const Config& config) { return config.endpoint + "\n" + config.username + "\n" + config.device_id; }

bool build_auth_request(const Config& config, HttpRequest& request, std::string& error) {
    request = {};
    if (!valid_config(config)) { error = "Invalid sync configuration"; return false; }
    request.method = "GET";
    request.url = config.endpoint + "/users/auth";
    request.headers = {{"accept", "application/vnd.koreader.v1+json"},
                       {"x-auth-user", config.username}, {"x-auth-key", config.userkey}};
    request.scope = account_scope(config);
    error.clear();
    return true;
}

bool build_get_request(const Config& config, const std::string& partial_md5,
                       HttpRequest& request, std::string& error) {
    if (!begin_request(config, partial_md5, request, error)) return false;
    request.method = "GET";
    request.url = config.endpoint + "/syncs/progress/" + partial_md5;
    return true;
}

bool build_put_request(const Config& config, const std::string& partial_md5,
                       const std::string& xpointer, double progress_fraction, const ReadingRecord& saved_record,
                       HttpRequest& request, std::string& error) {
    if (!begin_request(config, partial_md5, request, error)) return false;
    if (saved_record.book_id.size() != 64 || !saved_record.local_revision || saved_record.anchor.node.empty() ||
        xpointer.empty() || xpointer.size() > 4096 || xpointer.front() != '/' ||
        !std::isfinite(progress_fraction) || progress_fraction < 0 || progress_fraction > 1) {
        request = {}; error = "A saved position and an exact mapped XPointer are required"; return false;
    }
    std::unique_ptr<cJSON, decltype(&cJSON_Delete)> json(cJSON_CreateObject(), &cJSON_Delete);
    if (!json || !cJSON_AddStringToObject(json.get(), "document", partial_md5.c_str()) ||
        !cJSON_AddStringToObject(json.get(), "progress", xpointer.c_str()) ||
        !cJSON_AddNumberToObject(json.get(), "percentage", progress_fraction) ||
        !cJSON_AddStringToObject(json.get(), "device", "Pico Page") ||
        !cJSON_AddStringToObject(json.get(), "device_id", config.device_id.c_str())) {
        request = {}; error = "Cannot allocate sync request"; return false;
    }
    char* encoded = cJSON_PrintUnformatted(json.get());
    if (!encoded) { request = {}; error = "Cannot encode sync request"; return false; }
    request.body = encoded;
    cJSON_free(encoded);
    request.method = "PUT";
    request.url = config.endpoint + "/syncs/progress";
    request.headers.emplace_back("content-type", "application/json");
    request.uploaded_record = saved_record;
    return true;
}

RemoteResult parse_remote(int http_status, const std::string& body) {
    RemoteResult result;
    if (http_status == 401 || http_status == 403) {
        result.status = RemoteStatus::Unauthorized; result.error = "Sync authentication failed"; return result;
    }
    if (http_status != 200) { result.error = "Sync server returned HTTP " + std::to_string(http_status); return result; }
    if (body.size() > 32768 || body.find('\0') != std::string::npos ||
        body.find("\\u0000") != std::string::npos) { result.error = "Invalid sync response length or NUL character"; return result; }
    std::unique_ptr<cJSON, decltype(&cJSON_Delete)> json(cJSON_ParseWithOpts(body.c_str(), nullptr, true), &cJSON_Delete);
    if (!json || !cJSON_IsObject(json.get()) || cJSON_GetArraySize(json.get()) > 32) {
        result.error = "Invalid sync response JSON"; return result;
    }
    for (auto* a = json->child; a; a = a->next)
        for (auto* b = a->next; b; b = b->next)
            if (a->string && b->string && std::strcmp(a->string, b->string) == 0) {
                result.error = "Duplicate sync response field"; return result;
            }
    const auto* progress = cJSON_GetObjectItemCaseSensitive(json.get(), "progress");
    const auto* percentage = cJSON_GetObjectItemCaseSensitive(json.get(), "percentage");
    if (!progress && !percentage && cJSON_GetArraySize(json.get()) == 0) {
        result.status = RemoteStatus::NoRecord; return result;
    }
    result.progress = json_string(json.get(), "progress", 4096);
    if (result.progress.empty() || !cJSON_IsNumber(percentage) || !std::isfinite(percentage->valuedouble) ||
        percentage->valuedouble < 0 || percentage->valuedouble > 1) {
        result.error = "Sync response has no valid position"; return result;
    }
    result.progress_fraction = percentage->valuedouble;
    result.device = json_string(json.get(), "device", 256);
    result.device_id = json_string(json.get(), "device_id", 256);
    result.status = RemoteStatus::Found;
    return result;
}

bool upload_confirmed(int http_status, std::string& error) {
    if (http_status == 200) { error.clear(); return true; }
    error = http_status == 202 ? "Upload outcome is pending; read back before retrying" :
            http_status == 401 || http_status == 403 ? "Sync authentication failed" :
            "Upload was not confirmed; read back before retrying";
    return false;
}

}  // namespace pico_page::sync
