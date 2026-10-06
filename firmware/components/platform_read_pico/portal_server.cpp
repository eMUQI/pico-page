#include "pico_page/platform.h"

#include <atomic>
#include <cstring>

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

namespace pico_page::platform {
namespace {
httpd_handle_t server = nullptr;
PortalHandler portal_handler = nullptr;
void* portal_context = nullptr;
std::size_t portal_body_limit = 0;
std::atomic_bool resolver_running{false};
SemaphoreHandle_t resolver_stopped = nullptr;
std::uint32_t portal_address = 0;  // Network byte order.

esp_err_t serve(httpd_req_t* request) {
    const bool submitted = request->method == HTTP_POST;
    std::string body;
    if (submitted) {
        if (request->content_len > portal_body_limit) {
            httpd_resp_send_err(request, HTTPD_413_CONTENT_TOO_LARGE, nullptr);
            return ESP_FAIL;  // The unread body makes the connection unusable.
        }
        body.resize(request->content_len);
        std::size_t received = 0;
        while (received < body.size()) {
            const int count = httpd_req_recv(request, body.data() + received, body.size() - received);
            if (count == HTTPD_SOCK_ERR_TIMEOUT) continue;
            if (count <= 0) return ESP_FAIL;
            received += static_cast<std::size_t>(count);
        }
    }
    const auto page = portal_handler(portal_context, submitted, body);
    // A submitted body carries passwords.
    for (auto& byte : body) static_cast<volatile char&>(byte) = 0;
    httpd_resp_set_type(request, "text/html; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return httpd_resp_send(request, page.data(), static_cast<ssize_t>(page.size()));
}

// Phones probe a well-known address after joining a network. Sending every unknown address to
// the setup page is what makes them offer the page as the network's sign-in page.
esp_err_t redirect(httpd_req_t* request, httpd_err_code_t) {
    httpd_resp_set_status(request, "303 See Other");
    httpd_resp_set_hdr(request, "Location", "http://192.168.4.1/");
    // iOS ignores a redirect without a body.
    return httpd_resp_send(request, "Pico Page", HTTPD_RESP_USE_STRLEN);
}

/**
 * Answers every DNS A query with the access point address, so that a probe for any host name
 * reaches the setup page. Other query types get an empty answer. The receive timeout bounds
 * how long a stop request waits.
 */
void resolver(void*) {
    const int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_port = htons(53);
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    timeval timeout{0, 500000};
    if (sock >= 0 && bind(sock, reinterpret_cast<sockaddr*>(&local), sizeof(local)) == 0 &&
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0) {
        constexpr int header = 12, answer = 16;
        std::uint8_t packet[512 + answer];
        while (resolver_running.load()) {
            sockaddr_in peer{};
            socklen_t peer_size = sizeof(peer);
            const int size = recvfrom(sock, packet, 512, 0, reinterpret_cast<sockaddr*>(&peer), &peer_size);
            // A standard query: not a response, opcode 0, at least one question.
            if (size < header + 5 || (packet[2] & 0xf8) || !(packet[4] | packet[5])) continue;
            int end = header;
            while (end < size && packet[end] && packet[end] < 64) end += packet[end] + 1;
            if (end + 5 > size || packet[end]) continue;  // Truncated, or a compressed name.
            const bool address_query = packet[end + 1] == 0 && packet[end + 2] == 1;
            end += 5;
            packet[2] |= 0x80;  // Response; the opcode and recursion-desired bits are echoed.
            packet[3] = 0x80;   // Recursion available, no error.
            const std::uint8_t counts[] = {0, 1, 0, static_cast<std::uint8_t>(address_query), 0, 0, 0, 0};
            std::memcpy(packet + 4, counts, sizeof(counts));
            if (address_query) {
                // Name pointer to the question, type A, class IN, 60 s lifetime, four address bytes.
                const std::uint8_t record[] = {0xc0, 0x0c, 0, 1, 0, 1, 0, 0, 0, 60, 0, 4};
                std::memcpy(packet + end, record, sizeof(record));
                std::memcpy(packet + end + sizeof(record), &portal_address, 4);
                end += answer;
            }
            sendto(sock, packet, end, 0, reinterpret_cast<sockaddr*>(&peer), peer_size);
        }
    } else {
        ESP_LOGW("pico_portal", "Name service unavailable: errno %d", errno);
    }
    if (sock >= 0) close(sock);
    xSemaphoreGive(resolver_stopped);
    vTaskDelete(nullptr);
}
} // namespace

bool start_portal_server(PortalHandler handler, void* context, std::size_t body_limit,
                         std::string& error) {
    stop_portal_server();
    portal_handler = handler;
    portal_context = context;
    portal_body_limit = body_limit;
    // Redirected probes arrive for paths the server does not have; those are expected.
    esp_log_level_set("httpd_uri", ESP_LOG_ERROR);
    esp_log_level_set("httpd_txrx", ESP_LOG_ERROR);
    esp_log_level_set("httpd_parse", ESP_LOG_ERROR);
    httpd_config_t configuration = HTTPD_DEFAULT_CONFIG();
    // The handler assembles the page with std::string calls; the default 4 KiB stack leaves
    // little margin for that call depth.
    configuration.stack_size = 8192;
    configuration.max_open_sockets = 3;
    configuration.max_uri_handlers = 2;
    configuration.lru_purge_enable = true;
    auto result = httpd_start(&server, &configuration);
    if (result == ESP_OK) {
        httpd_uri_t page{}, save{};
        page.uri = "/";
        page.method = HTTP_GET;
        save.uri = "/save";
        save.method = HTTP_POST;
        page.handler = save.handler = serve;
        if ((result = httpd_register_uri_handler(server, &page)) == ESP_OK &&
            (result = httpd_register_uri_handler(server, &save)) == ESP_OK)
            result = httpd_register_err_handler(server, HTTPD_404_NOT_FOUND, redirect);
    }
    if (result != ESP_OK) {
        stop_portal_server();
        error = std::string("Setup page server: ") + esp_err_to_name(result);
        return false;
    }
    // Without the name service the page still answers at its address; only the automatic
    // sign-in prompt is lost, so a failure here does not fail the session.
    esp_netif_ip_info_t address{};
    auto* access = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (access && esp_netif_get_ip_info(access, &address) == ESP_OK &&
        (resolver_stopped = xSemaphoreCreateBinary())) {
        portal_address = address.ip.addr;
        resolver_running.store(true);
        if (xTaskCreate(resolver, "pico_portal_dns", 4096, nullptr, 3, nullptr) != pdPASS) {
            resolver_running.store(false);
            vSemaphoreDelete(resolver_stopped);
            resolver_stopped = nullptr;
        }
    }
    error.clear();
    return true;
}

void stop_portal_server() {
    if (resolver_stopped) {
        resolver_running.store(false);
        xSemaphoreTake(resolver_stopped, portMAX_DELAY);
        vSemaphoreDelete(resolver_stopped);
        resolver_stopped = nullptr;
    }
    if (server) httpd_stop(server);
    server = nullptr;
}

} // namespace pico_page::platform
