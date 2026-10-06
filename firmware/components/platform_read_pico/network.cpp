#include "pico_page/platform.h"

#include "store_internal.h"
#include "tomlc17.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <ctime>

#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

namespace pico_page::platform {
namespace {
constexpr EventBits_t has_address = 1;
constexpr EventBits_t connection_failed = 2;
constexpr std::size_t response_limit = 32768;
constexpr std::size_t request_limit = 16384;
TaskHandle_t network_owner = nullptr;
EventGroupHandle_t events = nullptr;
esp_netif_t* interface = nullptr;
esp_event_handler_instance_t wifi_handler = nullptr;
esp_event_handler_instance_t ip_handler = nullptr;
bool netif_initialized = false;
bool wifi_initialized = false;
bool wifi_started = false;
bool connected = false;
bool sntp_initialized = false;
std::uint16_t disconnect_reason = 0;

bool fail(std::string& error, const char* operation, esp_err_t result) {
    error = std::string(operation) + ": " + esp_err_to_name(result);
    return false;
}

void erase_bytes(void* data, std::size_t size) {
    auto* bytes = static_cast<volatile unsigned char*>(data);
    while (size--) *bytes++ = 0;
}

void network_event(void*, esp_event_base_t base, std::int32_t id, void* data) {
    if (!events) return;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        disconnect_reason = static_cast<wifi_event_sta_disconnected_t*>(data)->reason;
        xEventGroupClearBits(events, has_address);
        xEventGroupSetBits(events, connection_failed);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(events, has_address);
    }
}

bool only_fields(toml_datum_t table, std::initializer_list<const char*> fields) {
    if (table.type != TOML_TABLE) return false;
    for (int index = 0; index < table.u.tab.size; ++index) {
        bool found = false;
        const std::string key(table.u.tab.key[index], table.u.tab.len[index]);
        for (const auto* field : fields) found = found || key == field;
        if (!found) return false;
    }
    return true;
}

bool scalar_string(toml_datum_t value, std::size_t maximum, bool empty_allowed) {
    if (value.type != TOML_STRING || value.u.str.len < (empty_allowed ? 0 : 1) ||
        static_cast<std::size_t>(value.u.str.len) > maximum) return false;
    for (int index = 0; index < value.u.str.len; ++index) {
        const auto ch = static_cast<unsigned char>(value.u.str.ptr[index]);
        if (ch < 0x20 || ch == 0x7f) return false;
    }
    return true;
}

bool wifi_configuration(const std::string& root, wifi_config_t& configuration, std::string& error) {
    std::string bytes;
    const auto status = store_internal::read_file(root, "PicoPage/network.toml", 4096, bytes);
    if (status != StoreStatus::Ok) {
        error = "Cannot read local PicoPage/network.toml";
        return false;
    }
    store_internal::configure_toml();
    auto parsed = toml_parse(bytes.data(), static_cast<int>(bytes.size()));
    const auto table = toml_get(parsed.toptab, "wifi");
    const auto schema = toml_get(parsed.toptab, "schema_version");
    const auto ssid = toml_get(table, "ssid");
    const auto password = toml_get(table, "password");
    bool valid = parsed.ok && only_fields(parsed.toptab, {"schema_version", "wifi"}) &&
        only_fields(table, {"ssid", "password"}) && schema.type == TOML_INT64 && schema.u.int64 == 1 &&
        scalar_string(ssid, sizeof(configuration.sta.ssid), false) &&
        scalar_string(password, sizeof(configuration.sta.password), true);
    if (valid && password.u.str.len != 0 && password.u.str.len < 8) valid = false;
    if (valid && password.u.str.len == 64) {
        for (int index = 0; index < password.u.str.len; ++index) {
            if (!std::isxdigit(static_cast<unsigned char>(password.u.str.ptr[index]))) valid = false;
        }
    }
    if (valid) {
        std::memcpy(configuration.sta.ssid, ssid.u.str.ptr, ssid.u.str.len);
        std::memcpy(configuration.sta.password, password.u.str.ptr, password.u.str.len);
        configuration.sta.threshold.authmode = password.u.str.len ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
        configuration.sta.pmf_cfg.capable = true;
    }
    toml_free(parsed);
    erase_bytes(bytes.data(), bytes.size());
    if (!valid) error = "Invalid network.toml; expected schema_version=1 and wifi.ssid/password";
    return valid;
}

bool valid_header(const std::pair<std::string, std::string>& header) {
    if (header.first.empty() || header.first.size() > 64 || header.second.size() > 2048) return false;
    for (const auto value : header.first) {
        const auto ch = static_cast<unsigned char>(value);
        if (!std::isalnum(ch) && ch != '-') return false;
    }
    for (const auto value : header.second) {
        const auto ch = static_cast<unsigned char>(value);
        if (ch < 0x20 || ch == 0x7f) return false;
    }
    return true;
}

bool valid_url(const std::string& url) {
    if (url.compare(0, 8, "https://") != 0 || url.size() > 2048 || url.size() <= 8) return false;
    const auto authority_end = url.find_first_of("/?#", 8);
    const auto authority = url.substr(8, authority_end - 8);
    if (authority.empty() || authority.find('@') != std::string::npos) return false;
    for (const auto value : url) {
        const auto ch = static_cast<unsigned char>(value);
        if (ch <= 0x20 || ch == 0x7f || ch == '\\') return false;
    }
    return url.find('#') == std::string::npos;
}
} // namespace

void disconnect_network() {
    if (network_owner && network_owner != xTaskGetCurrentTaskHandle()) return;
    connected = false;
    if (sntp_initialized) {
        esp_netif_sntp_deinit();
        sntp_initialized = false;
    }
    if (wifi_handler) esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_handler);
    if (ip_handler) esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, ip_handler);
    wifi_handler = nullptr;
    ip_handler = nullptr;
    if (wifi_started) esp_wifi_stop();
    wifi_started = false;
    if (wifi_initialized) esp_wifi_deinit();
    wifi_initialized = false;
    if (interface) esp_netif_destroy_default_wifi(interface);
    interface = nullptr;
    if (events) vEventGroupDelete(events);
    events = nullptr;
    network_owner = nullptr;
}

bool connect_network(const std::string& card_root, std::string& error) {
    error.clear();
    if (network_owner && network_owner != xTaskGetCurrentTaskHandle()) {
        error = "Network must be used by its owning worker task";
        return false;
    }
    disconnect_network();
    network_owner = xTaskGetCurrentTaskHandle();
    wifi_config_t configuration{};
    if (!wifi_configuration(card_root, configuration, error)) return false;
    auto initialize = [&]() -> esp_err_t {
        if (!netif_initialized) {
            const auto result = esp_netif_init();
            if (result != ESP_OK) return result;
            netif_initialized = true;
        }
        auto result = esp_event_loop_create_default();
        if (result != ESP_OK && result != ESP_ERR_INVALID_STATE) return result;
        events = xEventGroupCreate();
        if (!events) return ESP_ERR_NO_MEM;
        interface = esp_netif_create_default_wifi_sta();
        if (!interface) return ESP_ERR_NO_MEM;
        wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
        init.nvs_enable = false;
        result = esp_wifi_init(&init);
        if (result != ESP_OK) return result;
        wifi_initialized = true;
        if ((result = esp_wifi_set_storage(WIFI_STORAGE_RAM)) != ESP_OK ||
            (result = esp_wifi_set_mode(WIFI_MODE_STA)) != ESP_OK ||
            (result = esp_wifi_set_config(WIFI_IF_STA, &configuration)) != ESP_OK) return result;
        if ((result = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, network_event, nullptr, &wifi_handler)) != ESP_OK ||
            (result = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, network_event, nullptr, &ip_handler)) != ESP_OK) return result;
        result = esp_wifi_start();
        if (result != ESP_OK) return result;
        wifi_started = true;
        return esp_wifi_connect();
    };
    esp_log_level_set("wifi", ESP_LOG_WARN);
    esp_log_level_set("HTTP_CLIENT", ESP_LOG_WARN);
    disconnect_reason = 0;
    const auto result = initialize();
    erase_bytes(&configuration, sizeof(configuration));
    if (result != ESP_OK) {
        disconnect_network();
        return fail(error, "Wi-Fi initialization", result);
    }
    const auto bits = xEventGroupWaitBits(events, has_address | connection_failed, pdFALSE, pdFALSE, pdMS_TO_TICKS(20000));
    if (!(bits & has_address) || (bits & connection_failed)) {
        error = bits & connection_failed ? "Wi-Fi disconnected (driver reason " + std::to_string(disconnect_reason) + ")" :
            "Wi-Fi address acquisition timed out";
        disconnect_network();
        return false;
    }
    esp_sntp_config_t time_configuration = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    const auto time_result = esp_netif_sntp_init(&time_configuration);
    if (time_result == ESP_OK) {
        sntp_initialized = true;
        esp_netif_sntp_sync_wait(pdMS_TO_TICKS(10000));
    }
    if (std::time(nullptr) < 1704067200) {
        error = "A valid clock is required for TLS; time synchronization failed";
        disconnect_network();
        return false;
    }
    connected = true;
    return true;
}

bool http_request(const std::string& method, const std::string& url,
                  const std::vector<std::pair<std::string, std::string>>& headers,
                  const std::string& body, std::string& response, int& status,
                  std::string& error, const std::atomic_bool* cancelled) {
    response.clear();
    status = 0;
    error.clear();
    bool request_started = false;
    const auto cancel_requested = [&]() {
        if (!cancelled || !cancelled->load(std::memory_order_acquire)) return false;
        error = request_started ? "Request cancelled; verify remote state before retrying" : "Request cancelled before send";
        return true;
    };
    if (cancel_requested()) return false;
    if (!connected || network_owner != xTaskGetCurrentTaskHandle() || !events ||
        !(xEventGroupGetBits(events) & has_address)) {
        error = "Wi-Fi is not connected";
        return false;
    }
    if (!valid_url(url) || headers.size() > 16 || body.size() > request_limit ||
        !std::all_of(headers.begin(), headers.end(), valid_header) ||
        (method != "GET" && method != "PUT" && method != "POST") || (method == "GET" && !body.empty())) {
        error = "Invalid HTTPS request or request exceeds limits";
        return false;
    }
    esp_http_client_config_t configuration{};
    configuration.url = url.c_str();
    configuration.method = method == "GET" ? HTTP_METHOD_GET : method == "PUT" ? HTTP_METHOD_PUT : HTTP_METHOD_POST;
    configuration.timeout_ms = 10000;
    configuration.disable_auto_redirect = true;
    configuration.crt_bundle_attach = esp_crt_bundle_attach;
    configuration.buffer_size = 1024;
    configuration.buffer_size_tx = 4096;
    auto* client = esp_http_client_init(&configuration);
    if (!client) return fail(error, "HTTP allocation", ESP_ERR_NO_MEM);
    const auto deadline = esp_timer_get_time() + 25'000'000;
    auto perform = [&]() -> bool {
        for (const auto& header : headers) {
            const auto result = esp_http_client_set_header(client, header.first.c_str(), header.second.c_str());
            if (result != ESP_OK) return fail(error, "HTTP header", result);
        }
        if (cancel_requested()) return false;
        request_started = true;
        auto result = esp_http_client_open(client, static_cast<int>(body.size()));
        if (result != ESP_OK) return fail(error, "HTTPS connection", result);
        if (cancel_requested()) return false;
        std::size_t sent = 0;
        while (sent < body.size()) {
            if (cancel_requested()) return false;
            if (esp_timer_get_time() >= deadline) return fail(error, "HTTP request", ESP_ERR_TIMEOUT);
            const int count = esp_http_client_write(client, body.data() + sent, static_cast<int>(body.size() - sent));
            if (count <= 0) return fail(error, "HTTP request write", ESP_FAIL);
            sent += static_cast<std::size_t>(count);
        }
        if (cancel_requested()) return false;
        const auto length = esp_http_client_fetch_headers(client);
        if (length < 0) return fail(error, "HTTP response headers", ESP_FAIL);
        if (cancel_requested()) return false;
        status = esp_http_client_get_status_code(client);
        if (length > static_cast<std::int64_t>(response_limit)) {
            error = "HTTP response exceeds 32 KiB";
            return false;
        }
        char buffer[1024];
        // Headers can arrive together with the entire body. The driver's
        // complete flag includes buffered bytes that still need to be read.
        while (true) {
            if (cancel_requested()) return false;
            const auto remaining = deadline - esp_timer_get_time();
            if (remaining <= 0) return fail(error, "HTTP response", ESP_ERR_TIMEOUT);
            esp_http_client_set_timeout_ms(client, static_cast<int>(std::min<std::int64_t>(5000, (remaining + 999) / 1000)));
            const int count = esp_http_client_read(client, buffer, sizeof(buffer));
            if (cancel_requested()) return false;
            if (count < 0) return fail(error, "HTTP response read", ESP_FAIL);
            if (count == 0) {
                if (esp_http_client_is_complete_data_received(client)) break;
                return fail(error, "Incomplete HTTP response", ESP_FAIL);
            }
            if (response.size() + static_cast<std::size_t>(count) > response_limit) {
                error = "HTTP response exceeds 32 KiB";
                return false;
            }
            response.append(buffer, static_cast<std::size_t>(count));
        }
        return !cancel_requested();
    };
    const bool complete = perform();
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    if (!complete) {
        response.clear();
        if (request_started && method != "GET") error += "; remote write outcome is unknown";
    }
    return complete;
}

} // namespace pico_page::platform
