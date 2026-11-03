#include "portal_sdk.h"
#include "pico_page/platform.h"
#include <cassert>
#include <cstring>
#include <functional>
#include <memory>

namespace {
struct Server { std::map<std::string, httpd_handler_t> handlers; };
Server* active = nullptr;
unsigned stop_failures = 0, stop_attempts = 0, waits = 0;
bool fail_start = false, fail_registration = false;
std::function<void()> stop_probe;
struct SessionState { unsigned starts = 0, ends = 0; };
void activity(void* context, bool start) {
    auto& state = *static_cast<SessionState*>(context);
    if (start) ++state.starts; else ++state.ends;
}
std::string token(const char* route) {
    httpd_req_t request; request.uri = route;
    assert(active->handlers.at(route)(&request) == ESP_OK);
    const auto at = request.response.find("const token='");
    assert(at != std::string::npos);
    return request.response.substr(at + 13, 32);
}
void rejected_upload(const std::string& value) {
    httpd_req_t request;
    request.headers["X-Pico-Token"] = value;
    request.headers["X-Pico-Name"] = "bad";
    assert(active->handlers.at("/upload")(&request) == ESP_FAIL);
}
}
const char* esp_err_to_name(esp_err_t) { return "injected failure"; }
void esp_log_level_set(const char*, int) {}
std::uint32_t esp_random() { static std::uint32_t value = 0; return ++value; }
esp_err_t httpd_start(httpd_handle_t* server, const httpd_config_t*) {
    assert(!active);
    if (fail_start) { *server = nullptr; return ESP_FAIL; }
    active = new Server; *server = active; return ESP_OK;
}
esp_err_t httpd_stop(httpd_handle_t server) {
    assert(server == active && active);
    ++stop_attempts;
    if (stop_probe) stop_probe();
    if (stop_failures) { --stop_failures; return ESP_FAIL; }
    delete active; active = nullptr; return ESP_OK;
}
esp_err_t httpd_register_uri_handler(httpd_handle_t server, const httpd_uri_t* uri) {
    assert(server == active);
    active->handlers[uri->uri] = uri->handler;
    return ESP_OK;
}
esp_err_t httpd_register_err_handler(httpd_handle_t, httpd_err_code_t, httpd_err_handler_func_t) {
    return fail_registration ? ESP_FAIL : ESP_OK;
}
std::size_t httpd_req_get_hdr_value_len(httpd_req_t* request, const char* name) {
    const auto found = request->headers.find(name);
    return found == request->headers.end() ? 0 : found->second.size();
}
esp_err_t httpd_req_get_hdr_value_str(httpd_req_t* request, const char* name, char* out, std::size_t size) {
    const auto& value = request->headers.at(name);
    assert(size > value.size()); std::memcpy(out, value.c_str(), value.size() + 1); return ESP_OK;
}
esp_err_t httpd_resp_set_status(httpd_req_t* request, const char* status) { request->status = status; return ESP_OK; }
esp_err_t httpd_resp_set_type(httpd_req_t*, const char*) { return ESP_OK; }
esp_err_t httpd_resp_set_hdr(httpd_req_t*, const char*, const char*) { return ESP_OK; }
esp_err_t httpd_resp_send(httpd_req_t* request, const char* bytes, ssize_t size) {
    request->response.assign(bytes, size == HTTPD_RESP_USE_STRLEN ? std::strlen(bytes) : static_cast<std::size_t>(size));
    return ESP_OK;
}
int httpd_req_recv(httpd_req_t*, char*, std::size_t) { assert(false); return -1; }
void* esp_netif_get_handle_from_ifkey(const char*) { return nullptr; }
esp_err_t esp_netif_get_ip_info(void*, esp_netif_ip_info_t*) { return ESP_FAIL; }
SemaphoreHandle_t xSemaphoreCreateBinary() { assert(false); return nullptr; }
int xSemaphoreTake(SemaphoreHandle_t, unsigned) { assert(false); return 0; }
int xSemaphoreGive(SemaphoreHandle_t) { assert(false); return 0; }
void vSemaphoreDelete(SemaphoreHandle_t) { assert(false); }
int xTaskCreate(void (*)(void*), const char*, unsigned, void*, unsigned, void*) { assert(false); return 0; }
void vTaskDelay(unsigned ticks) { assert(ticks == 1000); assert(++waits < 128); }
void vTaskDelete(void*) { assert(false); }
unsigned uxTaskGetStackHighWaterMark(void*) { return 0; }

int main() {
    using namespace pico_page::platform;
    auto first = std::make_unique<SessionState>();
    std::atomic_bool cancelled{false};
    UploadServer session;
    session.root = "/unused"; session.code = "one"; session.cancelled = &cancelled;
    session.context = first.get(); session.activity = activity;
    std::string error;
    assert(start_upload_server(session, error));
    const auto old_token = token("/one");
    assert(old_token.size() == 32);
    rejected_upload(""); rejected_upload(std::string(32, 'x'));
    assert(first->starts == 0 && first->ends == 0);
    rejected_upload(old_token);
    assert(first->starts == 1 && first->ends == 1);
    cancelled.store(true); rejected_upload(old_token);
    assert(first->starts == 1 && first->ends == 1);
    cancelled.store(false);

    // A restart must finish the old server before changing any borrowed session state.
    SessionState second;
    auto next = session; next.code = "two"; next.context = &second;
    stop_failures = 2;
    stop_probe = [&] {
        assert(token("/one") == old_token);
        rejected_upload(old_token); // Stopping rejects even authenticated new requests.
        assert(first->starts == 1 && second.starts == 0);
        // Model an already-entered handler's late completion callback.
        if (stop_attempts == 2) session.activity(session.context, false);
    };
    assert(start_upload_server(next, error));
    assert(stop_attempts == 3 && waits == 2 && first->ends == 2);
    stop_probe = {};
    first.reset(); // No server now borrows this context.
    const auto new_token = token("/two");
    assert(new_token != old_token);
    rejected_upload(old_token);
    assert(second.starts == 0 && second.ends == 0);
    rejected_upload(new_token);
    assert(second.starts == 1 && second.ends == 1);
    stop_failures = 1;
    stop_portal_server();
    assert(!active && stop_attempts == 5 && waits == 3);
    stop_portal_server();
    assert(stop_attempts == 5);

    // Registration failure must also finish cleanup before returning the borrowed context.
    fail_registration = true; stop_failures = 2;
    assert(!start_upload_server(next, error) && !error.empty());
    assert(!active && stop_attempts == 8 && waits == 5);
    fail_registration = false; fail_start = true;
    assert(!start_upload_server(next, error) && !active);
    assert(stop_attempts == 8);
    fail_start = false;
    assert(start_upload_server(next, error));
    stop_portal_server();
    assert(!active && stop_attempts == 9);

    // A prolonged outage must not surrender ownership or admit a replacement server.
    assert(start_upload_server(next, error));
    const auto retained_token = token("/two");
    const auto starts_before = second.starts, ends_before = second.ends;
    stop_failures = 100;
    cancelled.store(true);
    stop_probe = [&] {
        assert(token("/two") == retained_token);
        rejected_upload(retained_token);
        assert(second.starts == starts_before && second.ends == ends_before);
        assert(cancelled.load());
    };
    stop_portal_server();
    assert(!active && stop_attempts == 110 && waits == 105);
    stop_probe = {};
    cancelled.store(false);
    assert(start_upload_server(next, error));
    assert(token("/two") != retained_token);
    stop_portal_server();
    assert(!active && stop_attempts == 111);
}
