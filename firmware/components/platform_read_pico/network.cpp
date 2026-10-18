#include "pico_page/platform.h"
#include "pico_page/diagnostics.h"
#include "rtc_clock.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <limits>

#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_image_format.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "lwip/netdb.h"
#include "psa/crypto.h"

namespace pico_page::platform {
namespace {
constexpr EventBits_t has_address = 1;
constexpr EventBits_t connection_failed = 2;
constexpr EventBits_t scan_complete = 4;
constexpr EventBits_t associated = 8;
constexpr std::size_t response_limit = 32768;
constexpr std::size_t request_limit = 16384;
TaskHandle_t network_owner = nullptr;
EventGroupHandle_t events = nullptr;
esp_netif_t* interface = nullptr;
esp_netif_t* access_interface = nullptr;
esp_event_handler_instance_t wifi_handler = nullptr;
esp_event_handler_instance_t ip_handler = nullptr;
bool netif_initialized = false;
bool wifi_initialized = false;
bool wifi_started = false;
bool access_point = false;
bool connected = false;
bool sntp_initialized = false;
bool clock_synchronized = false;
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
        xEventGroupClearBits(events, has_address | associated);
        xEventGroupSetBits(events, connection_failed);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_CONNECTED) {
        xEventGroupSetBits(events, associated);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_SCAN_DONE) {
        xEventGroupSetBits(events, scan_complete);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(events, has_address);
    }
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

// A pending frame takes priority between SDK steps. An in-flight SDK step retains
// its own DNS, TCP and cryptographic bounds; the application retries without waiting.
template <typename Operation, typename Result>
Result http_step(Operation operation, Result pending, std::int64_t deadline, const std::atomic_bool* cancelled) {
    for (;;) {
        if ((cancelled && cancelled->load(std::memory_order_acquire)) || esp_timer_get_time() >= deadline)
            return pending;
        if (presentation_gate.try_network()) {
            const auto result = operation();
            presentation_gate.finish_network();
            if (result != pending) return result;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

// http_step returns its pending value only after cancellation or its deadline.
esp_err_t step_error(std::int64_t value) { return value == -ESP_ERR_HTTP_EAGAIN ? ESP_ERR_TIMEOUT : ESP_FAIL; }

// The open step keeps the full client timeout: esp-tls waits in select() for up to that
// timeout while TCP connects and does not re-arm its descriptor sets after a select() timeout.
// IDF closes the connection when a request header write fails, so a retry opens a new request.
// Later steps poll the socket in 50 ms intervals.
esp_err_t http_open(esp_http_client_handle_t client, int length,
                    std::int64_t deadline, const std::atomic_bool* cancelled) {
    esp_http_client_set_timeout_ms(client, 10000);
    const auto result = http_step([&] {
        errno = 0;
        const auto value = esp_http_client_open(client, length);
        return value != ESP_OK && (errno == EAGAIN || errno == EWOULDBLOCK) ? ESP_ERR_HTTP_EAGAIN : value;
    }, ESP_ERR_HTTP_EAGAIN, deadline, cancelled);
    if (result == ESP_OK) esp_http_client_set_timeout_ms(client, 50);
    return result == ESP_ERR_HTTP_EAGAIN ? ESP_ERR_TIMEOUT : result;
}

std::int64_t http_headers(esp_http_client_handle_t client, int& status,
                          std::int64_t deadline, const std::atomic_bool* cancelled) {
    status = 0;
    return http_step([&] {
        const auto length = esp_http_client_fetch_headers(client);
        // IDF 6.1 resets its status field on every fetch, even when resuming partial headers.
        const auto parsed_status = esp_http_client_get_status_code(client);
        if (parsed_status > 0) status = parsed_status;
        return length;
    }, static_cast<std::int64_t>(-ESP_ERR_HTTP_EAGAIN), deadline, cancelled);
}

void http_close(esp_http_client_handle_t client, bool destroy) {
    // Cancellation cannot abandon a live TLS context; release it between panel frames.
    http_step([&] {
        esp_http_client_close(client);
        if (destroy) esp_http_client_cleanup(client);
        return true;
    }, false, std::numeric_limits<std::int64_t>::max(), nullptr);
}

void stop_clock() {
    if (sntp_initialized) esp_netif_sntp_deinit();
    sntp_initialized = false;
    clock_synchronized = false;
}

void start_clock() {
    if (sntp_initialized) return;
    esp_sntp_config_t configuration = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    configuration.sync_cb = rtc_clock::synchronized;
    sntp_initialized = esp_netif_sntp_init(&configuration) == ESP_OK;
}

/** Creates the interfaces and starts the driver; access selects access point plus station. */
esp_err_t start_wifi(wifi_config_t* access) {
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
    if (access && !(access_interface = esp_netif_create_default_wifi_ap())) return ESP_ERR_NO_MEM;
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    init.nvs_enable = false;
    result = esp_wifi_init(&init);
    if (result != ESP_OK) return result;
    wifi_initialized = true;
    if ((result = esp_wifi_set_storage(WIFI_STORAGE_RAM)) != ESP_OK ||
        (result = esp_wifi_set_mode(access ? WIFI_MODE_APSTA : WIFI_MODE_STA)) != ESP_OK ||
        (access && (result = esp_wifi_set_config(WIFI_IF_AP, access)) != ESP_OK)) return result;
    if ((result = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, network_event, nullptr, &wifi_handler)) != ESP_OK ||
        (result = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, network_event, nullptr, &ip_handler)) != ESP_OK) return result;
    esp_log_level_set("wifi", ESP_LOG_WARN);
    // Every 50 ms socket poll timeout is an SDK warning; callers report request failures.
    esp_log_level_set("HTTP_CLIENT", ESP_LOG_ERROR);
    result = esp_wifi_start();
    if (result == ESP_OK) wifi_started = true;
    return result;
}

/** Ends a station attempt; without an access point the whole session is released. */
void abandon_station() {
    connected = false;
    if (!access_point) {
        disconnect_network();
        return;
    }
    stop_clock();
    esp_wifi_disconnect();
}

/** Scans once on the running driver; networks receives distinct names, strongest first. */
bool scan(std::vector<NearbyNetwork>& networks, std::string& error, const std::atomic_bool* cancelled) {
    networks.clear();
    wifi_scan_config_t configuration{};
    xEventGroupClearBits(events, scan_complete);
    const auto result = esp_wifi_scan_start(&configuration, false);
    if (result != ESP_OK) return fail(error, "Wi-Fi scan", result);
    const auto deadline = esp_timer_get_time() + 10'000'000;
    while (!(xEventGroupWaitBits(events, scan_complete, pdFALSE, pdFALSE, pdMS_TO_TICKS(100)) & scan_complete)) {
        if ((cancelled && cancelled->load()) || esp_timer_get_time() >= deadline) {
            esp_wifi_scan_stop(); esp_wifi_clear_ap_list();
            error = cancelled && cancelled->load() ? "操作已取消。" : "搜索网络超时，请重试。";
            return false;
        }
    }
    std::uint16_t count = 20;
    std::vector<wifi_ap_record_t> records(count);
    if (esp_wifi_scan_get_ap_records(&count, records.data()) != ESP_OK) {
        esp_wifi_clear_ap_list();
        error = "无法读取网络搜索结果，请重试。";
        return false;
    }
    for (std::uint16_t index = 0; index < count; ++index) {
        const auto* raw = reinterpret_cast<const char*>(records[index].ssid);
        std::string name(raw, strnlen(raw, sizeof(records[index].ssid)));
        if (name.empty()) continue;
        const auto found = std::find_if(networks.begin(), networks.end(),
                                        [&](const NearbyNetwork& item) { return item.ssid == name; });
        const bool open = records[index].authmode == WIFI_AUTH_OPEN;
        if (found == networks.end()) networks.push_back({std::move(name), records[index].rssi, open});
        else if (records[index].rssi > found->rssi) { found->rssi = records[index].rssi; found->open = open; }
    }
    std::stable_sort(networks.begin(), networks.end(),
                     [](const NearbyNetwork& a, const NearbyNetwork& b) { return a.rssi > b.rssi; });
    return true;
}

/** True when Wi-Fi runs and belongs to the calling task. */
bool owned_and_running() {
    return wifi_started && network_owner == xTaskGetCurrentTaskHandle();
}
} // namespace

void disconnect_network() {
    if (network_owner && network_owner != xTaskGetCurrentTaskHandle()) return;
    connected = false;
    access_point = false;
    stop_clock();
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
    if (access_interface) esp_netif_destroy_default_wifi(access_interface);
    access_interface = nullptr;
    if (events) vEventGroupDelete(events);
    events = nullptr;
    network_owner = nullptr;
}

bool start_access_point(const std::string& ssid, const std::string& password,
                        std::vector<NearbyNetwork>& networks, std::string& error, const std::atomic_bool* cancelled) {
    error.clear();
    networks.clear();
    if (network_owner && network_owner != xTaskGetCurrentTaskHandle()) {
        error = "Network must be used by its owning worker task";
        return false;
    }
    wifi_config_t access{};
    if (ssid.empty() || ssid.size() > sizeof(access.ap.ssid) || password.size() < 8 ||
        password.size() >= sizeof(access.ap.password)) {
        error = "Invalid access point name or password";
        return false;
    }
    disconnect_network();
    network_owner = xTaskGetCurrentTaskHandle();
    std::memcpy(access.ap.ssid, ssid.data(), ssid.size());
    access.ap.ssid_len = static_cast<std::uint8_t>(ssid.size());
    std::memcpy(access.ap.password, password.data(), password.size());
    access.ap.channel = 1;
    access.ap.authmode = WIFI_AUTH_WPA2_PSK;
    access.ap.max_connection = 2;
    const auto result = start_wifi(&access);
    erase_bytes(&access, sizeof(access));
    if (result != ESP_OK) {
        disconnect_network();
        return fail(error, "Access point initialization", result);
    }
    access_point = true;
    // Scanning leaves the access point channel for a moment, so it runs once, before a phone joins.
    const bool scanned = scan(networks, error, cancelled);
    if (cancelled && cancelled->load()) {
        error = "操作已取消。";
        return false;
    }
    // Nearby networks are optional for manual setup and hotspot file transfer.
    if (!scanned) {
        ESP_LOGW("pico_network", "Hotspot scan unavailable: %s", error.c_str());
        networks.clear();
        error.clear();
    }
    return true;
}

bool scan_networks(std::vector<NearbyNetwork>& networks, std::string& error, const std::atomic_bool* cancelled) {
    error.clear();
    networks.clear();
    if (network_owner && network_owner != xTaskGetCurrentTaskHandle()) {
        error = "Network must be used by its owning worker task";
        return false;
    }
    if (!owned_and_running()) {
        disconnect_network();
        network_owner = xTaskGetCurrentTaskHandle();
        const auto result = start_wifi(nullptr);
        if (result != ESP_OK) {
            disconnect_network();
            return fail(error, "Wi-Fi initialization", result);
        }
    }
    return scan(networks, error, cancelled);
}

bool connect_network(const std::string& ssid, const std::string& password, std::string& error,
                     ConnectFailure* failure, bool clock, const std::atomic_bool* cancelled) {
    error.clear();
    ConnectFailure ignored;
    auto& outcome = failure ? *failure : ignored;
    outcome = {ConnectFailure::Stage::Setup, 0};
    if (network_owner && network_owner != xTaskGetCurrentTaskHandle()) {
        error = "Network must be used by its owning worker task";
        return false;
    }
    wifi_config_t configuration{};
    if (ssid.empty() || ssid.size() > sizeof(configuration.sta.ssid) ||
        password.size() > sizeof(configuration.sta.password)) {
        error = "Invalid Wi-Fi name or password";
        return false;
    }
    std::memcpy(configuration.sta.ssid, ssid.data(), ssid.size());
    std::memcpy(configuration.sta.password, password.data(), password.size());
    configuration.sta.threshold.authmode = password.empty() ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;
    configuration.sta.pmf_cfg.capable = true;
    esp_err_t result = ESP_OK;
    if (owned_and_running()) {
        connected = false;
        stop_clock();
        // Leaving an established link reports a disconnect; it must arrive before this attempt
        // starts, or it would be taken for the attempt's own failure.
        const bool linked = xEventGroupGetBits(events) & has_address;
        xEventGroupClearBits(events, connection_failed);
        esp_wifi_disconnect();
        if (linked) xEventGroupWaitBits(events, connection_failed, pdFALSE, pdFALSE, pdMS_TO_TICKS(2000));
        xEventGroupClearBits(events, has_address | associated | connection_failed);
    } else {
        disconnect_network();
        network_owner = xTaskGetCurrentTaskHandle();
        result = start_wifi(nullptr);
    }
    disconnect_reason = 0;
    if (result == ESP_OK) result = esp_wifi_set_config(WIFI_IF_STA, &configuration);
    if (result == ESP_OK) result = esp_wifi_connect();
    erase_bytes(&configuration, sizeof(configuration));
    if (result != ESP_OK) {
        abandon_station();
        return fail(error, "Wi-Fi initialization", result);
    }
    EventBits_t bits = 0;
    const auto deadline = esp_timer_get_time() + 20'000'000;
    do {
        if (cancelled && cancelled->load()) { abandon_station(); error = "操作已取消。"; return false; }
        bits = xEventGroupWaitBits(events, has_address | connection_failed, pdFALSE, pdFALSE, pdMS_TO_TICKS(100));
    } while (!(bits & (has_address | connection_failed)) && esp_timer_get_time() < deadline);
    if (!(bits & has_address) || (bits & connection_failed)) {
        if (bits & connection_failed) {
            outcome = {ConnectFailure::Stage::Link, disconnect_reason};
            error = "Wi-Fi disconnected (driver reason " + std::to_string(disconnect_reason) + ")";
        } else {
            outcome.stage = (xEventGroupGetBits(events) & associated) ? ConnectFailure::Stage::Address : ConnectFailure::Stage::Link;
            error = outcome.stage == ConnectFailure::Stage::Address ? "Wi-Fi address acquisition timed out" : "Wi-Fi association timed out";
        }
        abandon_station();
        return false;
    }
    outcome.stage = ConnectFailure::Stage::None;
    connected = true;
    start_clock();
    if (clock && !synchronize_clock(error, &outcome, cancelled)) {
        abandon_station();
        return false;
    }
    return true;
}

bool synchronize_clock(std::string& error, ConnectFailure* failure, const std::atomic_bool* cancelled, bool fresh) {
    error.clear();
    ConnectFailure ignored;
    auto& outcome = failure ? *failure : ignored;
    outcome = {ConnectFailure::Stage::Clock, 0};
    if (!connected || network_owner != xTaskGetCurrentTaskHandle() || !events ||
        !(xEventGroupGetBits(events) & has_address)) {
        outcome.stage = ConnectFailure::Stage::Link;
        error = "Wi-Fi is not connected";
        return false;
    }
    if (cancelled && cancelled->load()) { error = "操作已取消。"; return false; }
    if (fresh) stop_clock();
    start_clock();
    const auto deadline = esp_timer_get_time() + 10'000'000;
    while (sntp_initialized && !clock_synchronized && esp_timer_get_time() < deadline) {
        if (cancelled && cancelled->load()) { error = "操作已取消。"; return false; }
        clock_synchronized = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(100)) == ESP_OK;
    }
    if (cancelled && cancelled->load()) { error = "操作已取消。"; return false; }
    if (fresh && !clock_synchronized) {
        error = "时间未校准，请检查互联网连接后重试。";
        return false;
    }
    if (std::time(nullptr) < 1704067200) {
        error = "A valid clock is required for TLS; time synchronization failed";
        return false;
    }
    outcome.stage = ConnectFailure::Stage::None;
    return true;
}

std::string station_address() {
    esp_netif_ip_info_t address{};
    if (!station_connected() || !interface || esp_netif_get_ip_info(interface, &address) != ESP_OK || !address.ip.addr) return {};
    char text[16];
    std::snprintf(text, sizeof(text), IPSTR, IP2STR(&address.ip));
    return text;
}

bool station_connected() {
    return connected && events && (xEventGroupGetBits(events) & has_address);
}

bool resolve_host(const std::string& url, std::string& error, const std::atomic_bool* cancelled) {
    error.clear();
    if (!valid_url(url)) { error = "Invalid HTTPS URL"; return false; }
    if (!connected || network_owner != xTaskGetCurrentTaskHandle() || !events ||
        !(xEventGroupGetBits(events) & has_address)) {
        error = "Wi-Fi is not connected";
        return false;
    }
    const auto host = url.substr(8, url.find_first_of("/?#:", 8) - 8);
    const int result = http_step([&] {
        addrinfo hints{};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo* found = nullptr;
        const int status = getaddrinfo(host.c_str(), nullptr, &hints, &found);
        if (found) freeaddrinfo(found);
        return status == 0 ? 0 : 1;
    }, -1, esp_timer_get_time() + 25'000'000, cancelled);
    if (result == 0) return true;
    error = result < 0 ? "Name lookup cancelled or timed out" : "Name lookup failed for " + host;
    return false;
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
    configuration.is_async = true;
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
        auto result = http_open(client, static_cast<int>(body.size()),
                                std::min(deadline, esp_timer_get_time() + 10'000'000), cancelled);
        if (cancel_requested()) return false;
        if (result != ESP_OK) return fail(error, "HTTPS connection", result);
        std::size_t sent = 0;
        while (sent < body.size()) {
            if (cancel_requested()) return false;
            if (esp_timer_get_time() >= deadline) return fail(error, "HTTP request", ESP_ERR_TIMEOUT);
            const int count = http_step([&] {
                errno = 0;
                const auto value = esp_http_client_write(client, body.data() + sent, static_cast<int>(body.size() - sent));
                return value == 0 || (value < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) ? -ESP_ERR_HTTP_EAGAIN : value;
            }, -ESP_ERR_HTTP_EAGAIN, std::min(deadline, esp_timer_get_time() + 10'000'000), cancelled);
            if (count <= 0) return fail(error, "HTTP request write", step_error(count));
            sent += static_cast<std::size_t>(count);
        }
        if (cancel_requested()) return false;
        const auto length = http_headers(client, status,
                                     std::min(deadline, esp_timer_get_time() + 10'000'000), cancelled);
        if (length < 0) return fail(error, "HTTP response headers", step_error(length));
        if (cancel_requested()) return false;
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
            const int count = http_step([&] { return esp_http_client_read(client, buffer, sizeof(buffer)); },
                                       -ESP_ERR_HTTP_EAGAIN, std::min(deadline, esp_timer_get_time() + 5'000'000), cancelled);
            if (cancel_requested()) return false;
            if (count < 0) return fail(error, "HTTP response read", step_error(count));
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
    http_close(client, true);
    if (!complete) {
        response.clear();
        if (request_started && method != "GET") error += "; remote write outcome is unknown";
    }
    return complete;
}

namespace {
// The slot that the last completed download_firmware() verified; cleared when another begins.
std::atomic<const esp_partition_t*> verified_slot{nullptr};
// Image header, first segment header and application description: the start of every app image.
constexpr std::size_t image_prefix = sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t);
constexpr std::size_t firmware_block = 4096;
constexpr std::size_t firmware_erase_block = 64 * 1024;
constexpr std::int64_t firmware_deadline_us = 1'200'000'000;

bool expected_image(const std::uint8_t* prefix, const std::string& version) {
    esp_image_header_t header;
    esp_app_desc_t description;
    std::memcpy(&header, prefix, sizeof(header));
    std::memcpy(&description, prefix + sizeof(header) + sizeof(esp_image_segment_header_t), sizeof(description));
    const auto* running = esp_app_get_description();
    return header.magic == ESP_IMAGE_HEADER_MAGIC && header.chip_id == ESP_CHIP_ID_ESP32S3 &&
           description.magic_word == ESP_APP_DESC_MAGIC_WORD &&
           strnlen(description.project_name, sizeof(description.project_name)) < sizeof(description.project_name) &&
           std::strcmp(description.project_name, running->project_name) == 0 &&
           strnlen(description.version, sizeof(description.version)) < sizeof(description.version) &&
           version == description.version;
}
} // namespace

bool download_firmware(const FirmwareImage& image, const FirmwareHooks& hooks, FirmwareFailure& failure,
                       std::string& error, const std::atomic_bool* cancelled) {
    error.clear();
    verified_slot.store(nullptr);
    const auto stop = [&](FirmwareFailure reason, const char* operation, esp_err_t result = ESP_FAIL) {
        failure = reason;
        return fail(error, operation, result);
    };
    const auto cancel_requested = [&] {
        if (!cancelled || !cancelled->load(std::memory_order_acquire)) return false;
        failure = FirmwareFailure::Cancelled;
        error = "Download cancelled";
        return true;
    };
    if (cancel_requested()) return false;
    if (!connected || network_owner != xTaskGetCurrentTaskHandle() || !events ||
        !(xEventGroupGetBits(events) & has_address)) return stop(FirmwareFailure::Network, "Wi-Fi is not connected");
    const auto* slot = esp_ota_get_next_update_partition(nullptr);
    if (!slot) return stop(FirmwareFailure::Flash, "No update slot", ESP_ERR_NOT_FOUND);
    if (!valid_url(image.url) || image.sha256.size() != 64 || image.size < image_prefix)
        return stop(FirmwareFailure::Mismatch, "Invalid firmware description", ESP_ERR_INVALID_ARG);
    if (image.size > slot->size) return stop(FirmwareFailure::TooLarge, "Firmware exceeds the update slot", ESP_ERR_INVALID_SIZE);
    struct ActiveRadio {
        wifi_ps_type_t previous = WIFI_PS_MIN_MODEM;
        bool restore = esp_wifi_get_ps(&previous) == ESP_OK && esp_wifi_set_ps(WIFI_PS_NONE) == ESP_OK;
        ~ActiveRadio() {
            if (restore && esp_wifi_set_ps(previous) != ESP_OK)
                ESP_LOGW("pico_network", "Could not restore Wi-Fi power saving");
        }
    } active_radio;
    // Flash writes take their data from internal RAM without an intermediate copy.
    auto* block = static_cast<std::uint8_t*>(heap_caps_malloc(firmware_block, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (!block) return stop(FirmwareFailure::Flash, "Download buffer", ESP_ERR_NO_MEM);
    esp_http_client_config_t configuration{};
    configuration.url = image.url.c_str();
    configuration.method = HTTP_METHOD_GET;
    configuration.timeout_ms = 10000;
    configuration.is_async = true;
    configuration.disable_auto_redirect = true;
    configuration.crt_bundle_attach = esp_crt_bundle_attach;
    configuration.buffer_size = firmware_block;
    PP_DIAGNOSTIC(
        wifi_ap_record_t ap{};
        wifi_ps_type_t ps = WIFI_PS_MIN_MODEM;
        esp_wifi_sta_get_ap_info(&ap);
        esp_wifi_get_ps(&ps);
        ESP_LOGI("pico_ota_perf", "link rssi=%d channel=%u ps=%d tcp_window=%d", ap.rssi, ap.primary, ps, CONFIG_LWIP_TCP_WND_DEFAULT);
    )
    auto* client = esp_http_client_init(&configuration);
    if (!client) { heap_caps_free(block); return stop(FirmwareFailure::Flash, "HTTP allocation", ESP_ERR_NO_MEM); }
    esp_ota_handle_t handle = 0;
    bool writing = false;
    std::uint32_t written = 0;
    PP_DIAGNOSTIC(
        const auto timing_start = esp_timer_get_time();
        std::int64_t read_us = 0, read_max_us = 0, write_us = 0, write_max_us = 0;
        std::int64_t wait_us = 0, begin_us = 0, digest_us = 0, verify_us = 0, connect_us = 0, headers_us = 0;
        std::int64_t erase_max_us = 0;
        std::uint32_t read_calls = 0, write_calls = 0, logged_step = 0;
        ESP_LOGI("pico_ota_perf", "start at_us=%lld size=%lu url=%s", static_cast<long long>(timing_start),
                 static_cast<unsigned long>(image.size), image.url.c_str());
    )
    psa_hash_operation_t hash = PSA_HASH_OPERATION_INIT;
    const auto flash = [&](bool begin) {
        PP_DIAGNOSTIC(const auto started = esp_timer_get_time();)
        if (hooks.flash) hooks.flash(hooks.context, begin);
        PP_DIAGNOSTIC(if (begin) wait_us += esp_timer_get_time() - started;)
    };
    const auto deadline = esp_timer_get_time() + firmware_deadline_us;
    const auto open_response = [&]() -> bool {
        PP_DIAGNOSTIC(const auto connecting = esp_timer_get_time();)
        auto result = http_open(client, 0, esp_timer_get_time() + 10'000'000, cancelled);
        PP_DIAGNOSTIC(connect_us += esp_timer_get_time() - connecting;)
        if (cancel_requested()) return false;
        if (result != ESP_OK) return stop(FirmwareFailure::Network, "HTTPS connection", result);
        PP_DIAGNOSTIC(const auto headers_started = esp_timer_get_time();)
        int status = 0;
        const auto length = http_headers(client, status, esp_timer_get_time() + 10'000'000, cancelled);
        PP_DIAGNOSTIC(headers_us += esp_timer_get_time() - headers_started;)
        if (cancel_requested()) return false;
        if (length < 0) return stop(FirmwareFailure::Network, "HTTP response headers", step_error(length));
        if (status != 200) return stop(FirmwareFailure::Server, "HTTP status");
        if (length > 0 && length != static_cast<std::int64_t>(image.size))
            return stop(FirmwareFailure::Mismatch, "Firmware length", ESP_ERR_INVALID_SIZE);
        return true;
    };
    auto perform = [&]() -> bool {
        if (!open_response()) return false;
        esp_err_t result;
        if (psa_hash_setup(&hash, PSA_ALG_SHA_256) != PSA_SUCCESS) return stop(FirmwareFailure::Flash, "Digest setup");
        std::size_t held = 0;
        bool complete = false;
        while (!complete) {
            if (cancel_requested()) return false;
            if (esp_timer_get_time() >= deadline) return stop(FirmwareFailure::Network, "Firmware download", ESP_ERR_TIMEOUT);
            PP_DIAGNOSTIC(const auto reading = esp_timer_get_time();)
            const int count = http_step([&] {
                return esp_http_client_read(client, reinterpret_cast<char*>(block) + held,
                                            static_cast<int>(firmware_block - held));
            }, -ESP_ERR_HTTP_EAGAIN, std::min(deadline, esp_timer_get_time() + 10'000'000), cancelled);
            PP_DIAGNOSTIC(
                const auto elapsed_read = esp_timer_get_time() - reading;
                read_us += elapsed_read; read_max_us = std::max(read_max_us, elapsed_read); ++read_calls;
            )
            if (cancel_requested()) return false;
            if (count < 0) return stop(FirmwareFailure::Network, "Firmware read", step_error(count));
            if (count == 0) {
                if (!esp_http_client_is_complete_data_received(client)) return stop(FirmwareFailure::Network, "Incomplete firmware");
                complete = true;
            }
            held += static_cast<std::size_t>(count);
            // Nothing is written until the start of the image names this chip, project and version.
            if (!written && held < image_prefix && !complete) continue;
            if (!held) break;
            if (written + held > image.size) return stop(FirmwareFailure::Mismatch, "Firmware length", ESP_ERR_INVALID_SIZE);
            if (!written && (held < image_prefix || !expected_image(block, image.version)))
                return stop(FirmwareFailure::WrongImage, "Firmware identity", ESP_ERR_INVALID_VERSION);
            if (!writing) {
                // No firmware packets may arrive while erasure disables the Flash cache.
                // The full transfer reconnects and revalidates its prefix after erasure.
                http_close(client, false);
                if (cancel_requested()) return false;
                flash(true);
                PP_DIAGNOSTIC(const auto beginning = esp_timer_get_time();)
                // Erase one hardware block at a time, retaining cancellation between blocks.
                std::size_t erased = std::min<std::size_t>(firmware_erase_block, slot->size);
                result = esp_ota_begin(slot, erased, &handle);
                PP_DIAGNOSTIC(begin_us = esp_timer_get_time() - beginning;)
                PP_DIAGNOSTIC(erase_max_us = begin_us;)
                flash(false);
                writing = handle != 0;
                if (result != ESP_OK) return stop(FirmwareFailure::Flash, "Update start", result);
                // Complete erasure before streaming writes so repeated cache-off erase
                // intervals do not interrupt each new window of incoming Wi-Fi packets.
                const auto erase_end = std::min<std::size_t>(slot->size,
                    ((image.size + firmware_erase_block - 1) / firmware_erase_block) * firmware_erase_block);
                while (erased < erase_end) {
                    // Let the application publish input and cancellation between erase blocks.
                    vTaskDelay(1);
                    if (cancel_requested()) return false;
                    const auto length = std::min<std::size_t>(firmware_erase_block, erase_end - erased);
                    flash(true);
                    PP_DIAGNOSTIC(const auto erasing = esp_timer_get_time();)
                    result = esp_partition_erase_range(slot, erased, length);
                    PP_DIAGNOSTIC(
                        const auto elapsed = esp_timer_get_time() - erasing;
                        begin_us += elapsed;
                        erase_max_us = std::max(erase_max_us, elapsed);
                    )
                    flash(false);
                    if (result != ESP_OK) return stop(FirmwareFailure::Flash, "Firmware erase", result);
                    erased += length;
                }
                if (cancel_requested() || !open_response()) return false;
                held = 0;
                complete = false;
                continue;
            }
            PP_DIAGNOSTIC(const auto digest_started = esp_timer_get_time();)
            const auto digest_result = psa_hash_update(&hash, block, held);
            PP_DIAGNOSTIC(digest_us += esp_timer_get_time() - digest_started;)
            if (digest_result != PSA_SUCCESS) return stop(FirmwareFailure::Flash, "Digest update");
            flash(true);
            PP_DIAGNOSTIC(const auto writing_started = esp_timer_get_time();)
            result = esp_ota_write(handle, block, held);
            PP_DIAGNOSTIC(
                const auto elapsed_write = esp_timer_get_time() - writing_started;
                write_us += elapsed_write; write_max_us = std::max(write_max_us, elapsed_write); ++write_calls;
            )
            flash(false);
            if (result != ESP_OK) return stop(FirmwareFailure::Flash, "Firmware write", result);
            written += static_cast<std::uint32_t>(held);
            held = 0;
            if (hooks.progress) hooks.progress(hooks.context, written);
            PP_DIAGNOSTIC(
                const auto step = static_cast<std::uint32_t>(20ull * written / image.size);
                if (step != logged_step) {
                    logged_step = step;
                    ESP_LOGI("pico_ota_perf", "progress at_us=%lld bytes=%lu read_us=%lld write_us=%lld wait_us=%lld",
                             static_cast<long long>(esp_timer_get_time()), static_cast<unsigned long>(written),
                             static_cast<long long>(read_us), static_cast<long long>(write_us), static_cast<long long>(wait_us));
                }
            )
        }
        if (cancel_requested()) return false;
        std::uint8_t digest[32];
        std::size_t digest_length = 0;
        if (psa_hash_finish(&hash, digest, sizeof(digest), &digest_length) != PSA_SUCCESS || digest_length != sizeof(digest))
            return stop(FirmwareFailure::Flash, "Digest");
        char text[65];
        for (std::size_t i = 0; i < sizeof(digest); ++i) std::snprintf(text + i * 2, 3, "%02x", digest[i]);
        if (written != image.size || image.sha256 != text)
            return stop(FirmwareFailure::Mismatch, "Firmware digest", ESP_ERR_INVALID_CRC);
        // Reads the slot back and checks the digest appended to the image. Header and digest
        // reads use the SPI1 flash driver and mappings update the MMU; both suspend the cache.
        flash(true);
        PP_DIAGNOSTIC(const auto verifying = esp_timer_get_time();)
        result = esp_ota_end(handle);
        PP_DIAGNOSTIC(verify_us = esp_timer_get_time() - verifying;)
        flash(false);
        writing = false;
        if (result != ESP_OK) return stop(FirmwareFailure::Mismatch, "Firmware validation", result);
        return true;
    };
    const bool okay = perform();
    PP_DIAGNOSTIC(
        ESP_LOGI("pico_ota_perf", "end at_us=%lld ok=%d bytes=%lu total_us=%lld read_us=%lld read_max_us=%lld read_calls=%lu write_us=%lld write_max_us=%lld write_calls=%lu wait_us=%lld begin_us=%lld digest_us=%lld verify_us=%lld connect_us=%lld headers_us=%lld erase_max_us=%lld",
                 static_cast<long long>(esp_timer_get_time()), okay, static_cast<unsigned long>(written),
                 static_cast<long long>(esp_timer_get_time() - timing_start), static_cast<long long>(read_us),
                 static_cast<long long>(read_max_us), static_cast<unsigned long>(read_calls), static_cast<long long>(write_us),
                 static_cast<long long>(write_max_us), static_cast<unsigned long>(write_calls), static_cast<long long>(wait_us),
                 static_cast<long long>(begin_us), static_cast<long long>(digest_us), static_cast<long long>(verify_us),
                 static_cast<long long>(connect_us), static_cast<long long>(headers_us), static_cast<long long>(erase_max_us));
    )
    psa_hash_abort(&hash);
    if (writing) esp_ota_abort(handle);
    http_close(client, true);
    heap_caps_free(block);
    if (okay) { failure = FirmwareFailure::None; verified_slot.store(slot); }
    return okay;
}

bool activate_firmware(std::string& error) {
    const auto* slot = verified_slot.load();
    if (!slot) return fail(error, "No verified firmware", ESP_ERR_INVALID_STATE);
    const auto result = esp_ota_set_boot_partition(slot);
    return result == ESP_OK || fail(error, "Boot slot", result);
}

bool firmware_unconfirmed() {
    esp_ota_img_states_t state;
    return esp_ota_get_state_partition(esp_ota_get_running_partition(), &state) == ESP_OK &&
           state == ESP_OTA_IMG_PENDING_VERIFY;
}

bool confirm_firmware() {
    const auto result = esp_ota_mark_app_valid_cancel_rollback();
    if (result != ESP_OK) ESP_LOGE("pico_network", "Firmware confirmation: %s", esp_err_to_name(result));
    return result == ESP_OK;
}

void reject_firmware() {
    esp_ota_mark_app_invalid_rollback_and_reboot();
}

} // namespace pico_page::platform
