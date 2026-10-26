#pragma once
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <sys/types.h>
using esp_err_t = int;
constexpr int ESP_OK = 0, ESP_FAIL = -1, ESP_LOG_ERROR = 1;
const char* esp_err_to_name(esp_err_t);
void esp_log_level_set(const char*, int);
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
std::uint32_t esp_random();
using httpd_handle_t = void*;
using httpd_err_code_t = int;
constexpr int HTTP_GET = 0, HTTP_POST = 1, HTTPD_404_NOT_FOUND = 404;
constexpr ssize_t HTTPD_RESP_USE_STRLEN = -1;
struct httpd_req_t {
    const char* uri = "/upload";
    std::size_t content_len = 0;
    std::map<std::string, std::string> headers;
    std::string response, status;
};
using httpd_handler_t = esp_err_t (*)(httpd_req_t*);
using httpd_err_handler_func_t = esp_err_t (*)(httpd_req_t*, httpd_err_code_t);
struct httpd_uri_t { const char* uri{}; int method{}; httpd_handler_t handler{}; };
struct httpd_config_t {
    int stack_size{}, max_open_sockets{}, max_uri_handlers{}, recv_wait_timeout{};
    bool lru_purge_enable{};
};
#define HTTPD_DEFAULT_CONFIG() httpd_config_t{}
esp_err_t httpd_start(httpd_handle_t*, const httpd_config_t*);
esp_err_t httpd_stop(httpd_handle_t);
esp_err_t httpd_register_uri_handler(httpd_handle_t, const httpd_uri_t*);
esp_err_t httpd_register_err_handler(httpd_handle_t, httpd_err_code_t, httpd_err_handler_func_t);
std::size_t httpd_req_get_hdr_value_len(httpd_req_t*, const char*);
esp_err_t httpd_req_get_hdr_value_str(httpd_req_t*, const char*, char*, std::size_t);
esp_err_t httpd_resp_set_status(httpd_req_t*, const char*);
esp_err_t httpd_resp_set_type(httpd_req_t*, const char*);
esp_err_t httpd_resp_set_hdr(httpd_req_t*, const char*, const char*);
esp_err_t httpd_resp_send(httpd_req_t*, const char*, ssize_t);
int httpd_req_recv(httpd_req_t*, char*, std::size_t);
struct esp_netif_ip_info_t { struct { std::uint32_t addr{}; } ip; };
void* esp_netif_get_handle_from_ifkey(const char*);
esp_err_t esp_netif_get_ip_info(void*, esp_netif_ip_info_t*);
using SemaphoreHandle_t = void*;
constexpr unsigned portMAX_DELAY = ~0U;
constexpr int pdPASS = 1;
#define pdMS_TO_TICKS(ms) (ms)
SemaphoreHandle_t xSemaphoreCreateBinary();
int xSemaphoreTake(SemaphoreHandle_t, unsigned);
int xSemaphoreGive(SemaphoreHandle_t);
void vSemaphoreDelete(SemaphoreHandle_t);
int xTaskCreate(void (*)(void*), const char*, unsigned, void*, unsigned, void*);
void vTaskDelay(unsigned);
void vTaskDelete(void*);
unsigned uxTaskGetStackHighWaterMark(void*);
