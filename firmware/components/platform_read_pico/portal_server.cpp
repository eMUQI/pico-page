#include "pico_page/platform.h"
#include "pico_page/store.h"
#include "upload_page.h"
#include "esp_random.h"
#include <algorithm>
#include <charconv>
#include <cstdio>

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
UploadServer upload_session;
std::string upload_token, upload_path;
std::atomic_bool resolver_running{false};
SemaphoreHandle_t resolver_stopped = nullptr;
std::uint32_t portal_address = 0;  // Network byte order.

std::string header(httpd_req_t* request, const char* name, std::size_t limit) {
    const auto size = httpd_req_get_hdr_value_len(request, name);
    if (!size || size > limit) return {};
    std::string value(size + 1, '\0');
    if (httpd_req_get_hdr_value_str(request, name, value.data(), value.size()) != ESP_OK) return {};
    value.resize(size);
    return value;
}
int hex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
/** A JavaScript string literal that stays inert inside an HTML script element. */
std::string script_string(const std::string& value) {
    std::string out = "\"";
    for (const char c : value) {
        const auto byte = static_cast<unsigned char>(c);
        if (byte < 0x20 || c == '"' || c == '\\' || c == '<' || c == '>' || c == '&' || c == '\'' || byte == 0x7f) {
            char escaped[7];
            std::snprintf(escaped, sizeof(escaped), "\\u%04x", byte);
            out += escaped;
        } else
            out += c;
    }
    return out + '"';
}

/**
 * Rejection statuses the transfer page tells apart: 400, 409, 413 and 415 concern only the
 * file named in the request, so the page continues with the next file; any other status stops
 * the batch.
 */
struct Refusal {
    const char* status;
    const char* message;
};
constexpr Refusal session_ended{"403 Forbidden", "会话无效，请重新打开传书页。"};
constexpr Refusal bad_name{"400 Bad Request", "文件名无效或过长。"};
constexpr Refusal bad_characters{"400 Bad Request", "文件名包含不支持的字符。"};
constexpr Refusal bad_type{"415 Unsupported Media Type", "仅支持 EPUB 或 TXT 文件。"};
constexpr Refusal bad_checksum{"400 Bad Request", "文件校验值无效。"};
constexpr Refusal bad_size{"413 Payload Too Large", "文件大小必须为 1 字节至 64 MiB。"};
constexpr Refusal exists{"409 Conflict", "书库里已有同名文件，未上传。已有书籍不会被覆盖。"};
constexpr Refusal not_prepared{"500 Internal Server Error", "无法准备上传或清理上次的临时文件，请检查存储卡后重试。"};
constexpr Refusal cleanup_failed{"500 Internal Server Error", "临时文件清理失败，请检查存储卡后重试。"};
constexpr Refusal closed{"503 Service Unavailable", "传输已结束。"};
constexpr Refusal interrupted{"500 Internal Server Error", "接收中断或超时，未完成的文件未导入。"};
constexpr Refusal write_failed{"507 Insufficient Storage", "写入失败，请检查存储卡空间。"};
constexpr Refusal commit_failed{"500 Internal Server Error", "保存或校验失败，请在书库核对后重试。已有同名文件不会被覆盖。"};

esp_err_t upload(httpd_req_t* request) {
    std::unique_ptr<FileImport> transaction;
    auto fail = [&](Refusal refusal) {
        std::string cleanup_error;
        if (transaction && transaction->cancel(cleanup_error) != StoreStatus::Ok) {
            ESP_LOGW("pico_upload", "Cleanup failed: %s", cleanup_error.c_str());
            refusal = {"500 Internal Server Error", "传输未完成，临时文件清理失败。请检查存储卡后重试。"};
        }
        ESP_LOGW("pico_upload", "Rejected: %s %s", refusal.status, refusal.message);
        httpd_resp_set_status(request, refusal.status);
        httpd_resp_set_type(request, "text/plain; charset=utf-8");
        httpd_resp_set_hdr(request, "Connection", "close");
        httpd_resp_send(request, refusal.message, HTTPD_RESP_USE_STRLEN);
        return ESP_FAIL; // An unread request body requires closing the connection.
    };
    const auto* cancelled = upload_session.cancelled;
    if (header(request, "X-Pico-Token", 32) != upload_token) return fail(session_ended);
    const auto encoded = header(request, "X-Pico-Name", 720);
    std::string name;
    for (std::size_t i = 0; i < encoded.size(); ++i) {
        char c = encoded[i];
        if (c == '%') {
            if (i + 2 >= encoded.size() || hex(encoded[i + 1]) < 0 || hex(encoded[i + 2]) < 0)
                return fail(bad_name);
            c = static_cast<char>(hex(encoded[i + 1]) * 16 + hex(encoded[i + 2])); i += 2;
        }
        if (static_cast<unsigned char>(c) < 32 || c == 127 || std::strchr("/\\:*?\"<>|", c))
            return fail(bad_characters);
        name += c;
    }
    if (name.size() < 5 || name.size() > 240 || name.front() == '.' || name.back() == ' ')
        return fail(bad_name);
    const auto dot = name.rfind('.');
    if (dot == std::string::npos) return fail(bad_type);
    auto extension = name.substr(dot);
    for (auto& c : extension) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    if (extension != ".epub" && extension != ".txt") return fail(bad_type);
    const auto checksum = header(request, "X-Pico-CRC", 8);
    if (checksum.empty()) return fail(bad_checksum);
    std::uint32_t crc = 0;
    for (const auto c : checksum) {
        if (hex(c) < 0) return fail(bad_checksum);
        crc = crc * 16 + hex(c);
    }
    const bool checking = std::strcmp(request->uri, "/check") == 0;
    std::uint64_t size = request->content_len;
    if (checking) {
        const auto value = header(request, "X-Pico-Size", 10);
        const auto parsed = std::from_chars(value.data(), value.data() + value.size(), size);
        if (request->content_len || parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size())
            return fail(bad_size);
    }
    if (!size || size > 64U * 1024 * 1024) return fail(bad_size);
    std::string error;
    transaction = FileStore(upload_session.root).begin_import("Books/" + name, size, crc, error);
    if (!transaction) {
        ESP_LOGW("pico_upload", "Prepare failed: %s", error.c_str());
        return fail(error == "destination_exists_or_unsafe" ? exists : not_prepared);
    }
    if (checking) {
        if (transaction->cancel(error) != StoreStatus::Ok) return fail(cleanup_failed);
        transaction.reset();
        httpd_resp_set_type(request, "text/plain; charset=utf-8");
        return httpd_resp_send(request, "可以上传", HTTPD_RESP_USE_STRLEN);
    }
    ESP_LOGI("pico_upload", "Receiving bytes=%u stack_free=%u", static_cast<unsigned>(size),
             static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
    char block[2048];
    while (transaction->received() < request->content_len) {
        if (cancelled && cancelled->load()) return fail(closed);
        const auto count = httpd_req_recv(request, block, std::min<std::size_t>(sizeof(block), request->content_len - transaction->received()));
        if (count <= 0) return fail(interrupted);
        if (transaction->append(transaction->received(), block, count, error) != StoreStatus::Ok)
            return fail(write_failed);
    }
    if (cancelled && cancelled->load()) return fail(closed);
    ESP_LOGI("pico_upload", "Verifying bytes=%u", static_cast<unsigned>(size));
    if (transaction->finish(error) != StoreStatus::Ok) {
        ESP_LOGW("pico_upload", "Commit failed: %s", error.c_str());
        return fail(commit_failed);
    }
    ESP_LOGI("pico_upload", "Saved bytes=%u stack_free=%u", static_cast<unsigned>(size),
             static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
    if (upload_session.saved) upload_session.saved(upload_session.context, name);
    httpd_resp_set_type(request, "text/plain; charset=utf-8");
    return httpd_resp_send(request, "已保存", HTTPD_RESP_USE_STRLEN);
}

esp_err_t transfer_page(httpd_req_t* request) {
    auto page = std::string(upload_page);
    page.insert(page.find("<script>") + 8,
                "const token='" + upload_token + "',route={hotspot:" + (upload_session.code.empty() ? "true" : "false") +
                    ",network:" + script_string(upload_session.network) + "};");
    httpd_resp_set_type(request, "text/html; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    httpd_resp_set_hdr(request, "X-Frame-Options", "DENY");
    return httpd_resp_send(request, page.data(), page.size());
}

// On a joined network the page is reachable only through the address on the device screen.
esp_err_t address_required(httpd_req_t* request, httpd_err_code_t) {
    static constexpr char page[] =
        "<!doctype html><html lang=\"zh-CN\"><meta charset=\"utf-8\">"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\"><title>Pico Page 传书</title>"
        "<body style=\"font:16px/1.6 system-ui,sans-serif;max-width:560px;margin:0 auto;padding:20px\">"
        "<h1 style=\"font-size:22px\">Wi-Fi 传书</h1><p>请用手机扫描设备屏幕上的二维码，"
        "或在浏览器输入屏幕上显示的完整地址（包括斜杠后的字母和数字）。</p></body></html>";
    httpd_resp_set_status(request, "404 Not Found");
    httpd_resp_set_type(request, "text/html; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return httpd_resp_send(request, page, HTTPD_RESP_USE_STRLEN);
}

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

/** Starts the HTTP server with the given handlers; access_point adds the name service. */
bool start_server(const std::vector<httpd_uri_t>& handlers, httpd_err_handler_func_t missing, bool access_point,
                  std::string& error) {
    // Redirected probes arrive for paths the server does not have; those are expected.
    esp_log_level_set("httpd_uri", ESP_LOG_ERROR);
    esp_log_level_set("httpd_txrx", ESP_LOG_ERROR);
    esp_log_level_set("httpd_parse", ESP_LOG_ERROR);
    httpd_config_t configuration = HTTPD_DEFAULT_CONFIG();
    // The handler assembles the page with std::string calls; the default 4 KiB stack leaves
    // little margin for that call depth.
    configuration.stack_size = 8192;
    configuration.max_open_sockets = 3;
    configuration.max_uri_handlers = 3;
    configuration.lru_purge_enable = true;
    configuration.recv_wait_timeout = 5;
    auto result = httpd_start(&server, &configuration);
    for (std::size_t i = 0; result == ESP_OK && i < handlers.size(); ++i)
        result = httpd_register_uri_handler(server, &handlers[i]);
    if (result == ESP_OK) result = httpd_register_err_handler(server, HTTPD_404_NOT_FOUND, missing);
    if (result != ESP_OK) {
        stop_portal_server();
        error = std::string("Setup page server: ") + esp_err_to_name(result);
        return false;
    }
    error.clear();
    if (!access_point) return true;
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
    return true;
}
} // namespace

bool start_portal_server(PortalHandler handler, void* context, std::size_t body_limit, std::string& error) {
    stop_portal_server();
    portal_handler = handler;
    portal_context = context;
    portal_body_limit = body_limit;
    httpd_uri_t page{}, save{};
    page.uri = "/";
    page.method = HTTP_GET;
    page.handler = serve;
    save.uri = "/save";
    save.method = HTTP_POST;
    save.handler = serve;
    return start_server({page, save}, redirect, true, error);
}

bool start_upload_server(const UploadServer& session, std::string& error) {
    stop_portal_server();
    upload_session = session;
    upload_token.clear();
    for (int i = 0; i < 4; ++i) {
        char part[9]; std::snprintf(part, sizeof(part), "%08lx", static_cast<unsigned long>(esp_random()));
        upload_token += part;
    }
    const bool access_point = session.code.empty();
    upload_path = "/" + session.code;
    httpd_uri_t page{}, save{}, check{};
    page.uri = upload_path.c_str();
    page.method = HTTP_GET;
    page.handler = transfer_page;
    save.uri = "/upload";
    save.method = HTTP_POST;
    save.handler = upload;
    check.uri = "/check";
    check.method = HTTP_POST;
    check.handler = upload;
    return start_server({page, save, check}, access_point ? redirect : address_required, access_point, error);
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
