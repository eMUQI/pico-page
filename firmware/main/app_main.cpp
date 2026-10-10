#include "pico_page/app.h"
#include "pico_page/console.h"
#include "pico_page/diagnostics.h"
#include "pico_page/platform.h"
#include "pico_page/provision.h"
#include "pico_page/released_tap.h"
#include "pico_page/sleep_input.h"
#include "pico_page/store.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <ctime>
#include <memory>
#include <limits>
#include <utility>

namespace {
constexpr auto tag = "pico_page";
constexpr std::int64_t idle_slow_us = 3'000'000;
// A progress commit blocks action dispatch for its duration, so it waits until the page has
// been left alone this long. Leaving the book, sleeping and USB transfer commit immediately.
constexpr std::int64_t idle_save_us = 2'000'000;
// Whole-book page counting shares the application task with input handling and starts only
// after the page has been left alone this long, so consecutive page turns are not delayed.
constexpr std::int64_t idle_background_us = 1'000'000;
std::uint64_t presentation_sequence = 0;
// When the most recent frame and its follow-up work finished; contacts sampled earlier were
// made while the previous screen was still being replaced.
std::int64_t settled_us = 0;
// An upload session ends itself after this long without an upload; the access point keeps the
// radio powered for as long as it is up.
constexpr std::int64_t portal_idle_us = 600'000'000;
constexpr char manual_paused[] = "手动连接已暂停，点“立即连接”可恢复。";
namespace diag = pico_page::diagnostics;
namespace platform = pico_page::platform;
namespace provision = pico_page::provision;
struct NetworkJob {
    pico_page::NetworkOperation operation;
    std::string card, body, error;
    int status = 0;
    // A book upload session instead of one request: operation.cancelled asks it to end and error
    // returns the closing message. station runs it over a saved network instead of the access point.
    bool portal = false;
    bool station = false;
    std::optional<pico_page::WifiOperation> wifi = std::nullopt;
    std::vector<provision::NearbyWifi> nearby = {};
    // Set when the station link could not be established; error then names the cause.
    pico_page::LinkFailure failure = pico_page::LinkFailure::None;
};
QueueHandle_t requests = nullptr, responses = nullptr;
QueueHandle_t wifi_events = nullptr;
std::atomic_bool radio_active{false};
pico_page::network::Holds holds;
provision::WifiCredentials station_target;
unsigned reconnects = 0;
pico_page::WifiStatus radio_status;
void publish_wifi(pico_page::WifiPhase phase, const std::string& ssid = "", const std::string& detail = "") {
    using pico_page::network::live;
    const bool manual = live(holds.manual) && !holds.manual_suspended;
    if (radio_status.phase == phase && radio_status.ssid == ssid && radio_status.detail == detail && radio_status.manual == manual) return;
    radio_status = {radio_status.sequence + 1, phase, ssid, detail, manual};
#ifdef CONFIG_PICO_PAGE_LEGACY_PERF_LOG
    ESP_LOGI("pico_network", "phase=%u time_ms=%lld internal_free=%u internal_largest=%u psram_free=%u psram_largest=%u",
        static_cast<unsigned>(phase), static_cast<long long>(esp_timer_get_time() / 1000),
        static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
        static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
        static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
        static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)));
#endif
    auto* event = new pico_page::WifiStatus(radio_status);
    if (xQueueSend(wifi_events, &event, 0) != pdTRUE) {
        pico_page::WifiStatus* old = nullptr;
        if (xQueueReceive(wifi_events, &old, 0) == pdTRUE) delete old;
        if (xQueueSend(wifi_events, &event, 0) != pdTRUE) delete event;
    }
}
bool retained_station() {
    return holds.retained();
}
void release_radio(const std::string& detail = "") {
    if (radio_active.load()) {
        publish_wifi(pico_page::WifiPhase::Closing);
        platform::disconnect_network();
        radio_active.store(false);
    }
    station_target = {};
    publish_wifi(pico_page::WifiPhase::Offline, "", detail);
}
// PortalStatus pointers from the network worker; the application task draws and deletes them.
QueueHandle_t portal_events = nullptr;
SemaphoreHandle_t portal_emit_lock = nullptr;
// State of one upload session shared by the network worker and the HTTP server task.
struct PortalSession {
    SemaphoreHandle_t lock = xSemaphoreCreateMutex();
    pico_page::network::IdleTimer upload_idle;
    // The state the device shows, updated as books are saved.
    pico_page::PortalStatus status;
};
void emit(const pico_page::PortalStatus &status) {
    auto *event = new pico_page::PortalStatus(status);
    xSemaphoreTake(portal_emit_lock, portMAX_DELAY);
    if (xQueueSend(portal_events, &event, 0) != pdTRUE) {
        // E-paper refresh can block the consumer; retain the newest cumulative status.
        pico_page::PortalStatus *oldest = nullptr;
        if (xQueueReceive(portal_events, &oldest, 0) == pdTRUE)
            delete oldest;
        if (xQueueSend(portal_events, &event, 0) != pdTRUE)
            delete event;
    }
    xSemaphoreGive(portal_emit_lock);
}
void upload_saved(void *context, const std::string &name) {
    auto &session = *static_cast<PortalSession *>(context);
    xSemaphoreTake(session.lock, portMAX_DELAY);
    ++session.status.saved_books;
    session.status.latest_book = name.substr(0, name.rfind('.'));
    const auto status = session.status;
    xSemaphoreGive(session.lock);
    emit(status);
}
void upload_activity(void* context, bool active) {
    auto& session = *static_cast<PortalSession*>(context);
    xSemaphoreTake(session.lock, portMAX_DELAY);
    session.upload_idle.update(active, esp_timer_get_time());
    session.status.phase = active ? pico_page::PortalPhase::Receiving : pico_page::PortalPhase::Waiting;
    const auto status = session.status;
    xSemaphoreGive(session.lock);
    emit(status);
}
std::string wifi_failure(const std::string &ssid, const platform::ConnectFailure &failure) {
    using Stage = platform::ConnectFailure::Stage;
    const auto kind = failure.stage == Stage::Link      ? provision::WifiFailure::Rejected
                      : failure.stage == Stage::Address ? provision::WifiFailure::NoAddress
                      : failure.stage == Stage::Clock   ? provision::WifiFailure::NoClock
                                                        : provision::WifiFailure::Setup;
    return provision::wifi_problem(ssid, kind, failure.reason);
}
std::vector<provision::NearbyWifi> nearby_wifi(const std::vector<platform::NearbyNetwork> &scanned) {
    std::vector<provision::NearbyWifi> nearby;
    for (const auto &item : scanned)
        nearby.push_back({item.ssid, item.rssi, item.open});
    return nearby;
}
pico_page::LinkFailure link_failure(const platform::ConnectFailure& failure) {
    using pico_page::LinkFailure;
    if (failure.stage != platform::ConnectFailure::Stage::Link) return LinkFailure::Other;
    return provision::network_absent(failure.reason) ? LinkFailure::NotNearby
         : provision::password_rejected(failure.reason) ? LinkFailure::Password : LinkFailure::Other;
}
/**
 * Brings the station link up for one task; the worker is the only driver owner and the
 * credentials were copied before leaving card access. A link that is already up is used as it
 * is unless replace is set. With targets.automatic one scan (up to 10 s) orders the saved
 * networks in range, strongest first, and each is tried until one connects; otherwise only the
 * single listed network is tried. Every attempt blocks for up to 20 s and stops at the next
 * driver call after cancelled is set. A refused password is reported before other causes.
 */
bool ensure_station(const pico_page::WifiTargets& targets, const std::atomic_bool& cancelled, std::string& error,
                    pico_page::LinkFailure& failure, bool replace = false) {
    using pico_page::LinkFailure;
    failure = LinkFailure::Other;
    error = "操作已取消。";
    if (cancelled.load()) return false;
    if (!replace && platform::station_connected()) { failure = LinkFailure::None; error.clear(); return true; }
    auto order = targets.networks;
    radio_active.store(true);
    if (targets.automatic) {
        publish_wifi(pico_page::WifiPhase::Connecting);
        std::vector<platform::NearbyNetwork> scanned;
        std::string scan_error;
        if (!platform::scan_networks(scanned, scan_error, &cancelled)) {
            if (!cancelled.load()) error = scan_error;
            return false;
        }
        order = provision::connection_order(targets.networks, nearby_wifi(scanned));
        if (order.empty()) { failure = LinkFailure::NotNearby; error = "附近没有已保存的网络。"; return false; }
    }
    std::string refused, last = "没有可连接的网络。";
    auto last_kind = LinkFailure::Other;
    for (const auto& target : order) {
        if (cancelled.load()) return false;
        publish_wifi(pico_page::WifiPhase::Connecting, target.ssid);
        platform::ConnectFailure reason;
        std::string detail;
        if (platform::connect_network(target.ssid, target.password, detail, &reason, false, &cancelled)) {
            station_target = target;
            publish_wifi(pico_page::WifiPhase::Connected, target.ssid);
            if (cancelled.load()) return false;
            failure = LinkFailure::None;
            error.clear();
            return true;
        }
        if (cancelled.load()) return false;
        last = wifi_failure(target.ssid, reason);
        last_kind = link_failure(reason);
        if (last_kind == LinkFailure::Password && refused.empty()) refused = last;
    }
    error = refused.empty() ? last : refused;
    failure = refused.empty() ? last_kind : LinkFailure::Password;
    return false;
}
/** Records connection history without changing the explicitly selected target. */
void remember_connection(const std::string &card, const std::string &ssid) {
    std::string error;
    if (!provision::save_connection(card, ssid, std::time(nullptr), error))
        ESP_LOGW(tag, "Connection record not saved: %s", error.c_str());
}
/** Starts the device access point and the upload server on it. */
bool start_hotspot(NetworkJob &job, PortalSession &session) {
    std::uint8_t mac[6]{};
    esp_efuse_mac_get_default(mac);
    char name[20];
    std::snprintf(name, sizeof(name), "PicoPage-%02X%02X", mac[4], mac[5]);
    // Read off the screen and typed on a phone, so look-alike characters are left out. The
    // radio is still off here, which leaves esp_random() pseudo-random; the time of the tap that
    // started the session adds entropy of its own.
    static constexpr char alphabet[] = "abcdefghjkmnpqrstuvwxyz23456789";
    std::string password;
    for (int i = 0; i < 8; ++i)
        password += alphabet[(esp_random() ^ static_cast<std::uint32_t>(esp_timer_get_time())) %
                             (sizeof(alphabet) - 1)];
    std::vector<platform::NearbyNetwork> scanned;
    holds.manual_suspended = pico_page::network::live(holds.manual);
    pico_page::network::release(holds.sync);
    station_target = {};
    radio_active.store(true);
    if (!platform::start_access_point(name, password, scanned, job.error, job.operation.cancelled.get()))
        return false;
    publish_wifi(pico_page::WifiPhase::Hotspot);
    xSemaphoreTake(session.lock, portMAX_DELAY);
    session.status = {pico_page::PortalPhase::Waiting, name, password, "http://192.168.4.1", "", "", 0, "", job.operation.id};
    xSemaphoreGive(session.lock);
    platform::UploadServer server;
    server.root = job.card;
    server.cancelled = job.operation.cancelled.get();
    server.network = name;
    server.saved = upload_saved;
    server.context = &session;
    server.activity = upload_activity;
    return platform::start_upload_server(server, job.error);
}
/**
 * Joins a saved network under the connection mode and serves the upload page at an address that
 * includes a short random path, so the page is reached from the screen rather than by guessing.
 * On failure job.error is the sentence the network page shows.
 */
bool start_station_upload(NetworkJob &job, PortalSession &session) {
    std::string error;
    if (job.operation.targets.networks.empty()) {
        job.error = "还没有可用的已保存网络。请先添加，也可使用设备热点传书。"; return false;
    }
    if (!ensure_station(job.operation.targets, *job.operation.cancelled, error, job.failure)) {
        job.error = error; return false;
    }
    const auto joined = station_target.ssid;
    remember_connection(job.card, joined);
    static constexpr char alphabet[] = "abcdefghjkmnpqrstuvwxyz23456789";
    std::string code;
    for (int i = 0; i < 6; ++i)
        code += alphabet[esp_random() % (sizeof(alphabet) - 1)];
    const auto address = platform::station_address();
    platform::UploadServer server;
    server.root = job.card;
    server.cancelled = job.operation.cancelled.get();
    server.code = code;
    server.network = joined;
    server.saved = upload_saved;
    server.context = &session;
    server.activity = upload_activity;
    if (address.empty() || !platform::start_upload_server(server, error)) {
        ESP_LOGW(tag, "Upload server: %s", error.c_str());
        job.error = "传书服务没有启动，请重试。";
        return false;
    }
    xSemaphoreTake(session.lock, portMAX_DELAY);
    session.status = {pico_page::PortalPhase::Waiting, "", "", "http://" + address + "/" + code, "", joined, 0, "", job.operation.id};
    xSemaphoreGive(session.lock);
    return true;
}
void run_portal(NetworkJob &job) {
    using pico_page::PortalPhase;
    PortalSession session;
    session.upload_idle.update(false, esp_timer_get_time());
    const bool started = session.lock && (job.station ? start_station_upload(job, session) : start_hotspot(job, session));
    if (!started) {
        ESP_LOGW(tag, "Upload session did not start: %s", job.error.c_str());
        if (job.operation.cancelled->load())
            job.error.clear();
        else if (!job.station || job.error.empty())
            job.error = job.station ? "传书服务没有启动，请重试。" : "设备热点没有启动，请重试。";
    } else {
        job.error.clear();
        xSemaphoreTake(session.lock, portMAX_DELAY);
        auto status = session.status;
        xSemaphoreGive(session.lock);
        emit(status);
        while (!job.operation.cancelled->load()) {
            if (job.station && !platform::station_connected()) {
                if (!job.operation.cancelled->load())
                    job.error = "与已保存的 Wi-Fi 连接已断开。可以改用设备热点传书。";
                break;
            }
            xSemaphoreTake(session.lock, portMAX_DELAY);
            const bool idle_expired = session.upload_idle.expired(esp_timer_get_time(), portal_idle_us);
            xSemaphoreGive(session.lock);
            if (idle_expired) {
                job.error = "传书超过 10 分钟没有上传，已自动结束。";
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(200));
        }
    }
    job.operation.cancelled->store(true);
    platform::stop_portal_server();
    if (!job.station || !retained_station())
        release_radio(holds.manual_suspended ? manual_paused : "");
    if (session.lock)
        vSemaphoreDelete(session.lock);
}
/** One request borrows a whole sync transaction, including the bounded decision interval. */
#ifdef CONFIG_PICO_PAGE_PERF_METRICS
void execute_request(NetworkJob &job) {
#else
void run_request(NetworkJob &job) {
#endif
    const auto& cancelled = *job.operation.cancelled;
    if (cancelled.load() || !pico_page::network::live(job.operation.session)) return;
    if (!ensure_station(job.operation.targets, cancelled, job.error, job.failure)) return;
    publish_wifi(pico_page::WifiPhase::Clock, station_target.ssid);
    platform::ConnectFailure failure;
    if (!platform::synchronize_clock(job.error, &failure, &cancelled)) {
        // Without a clock TLS cannot start; the user sees it as a network that does not work.
        job.error = cancelled.load() ? "操作已取消。" : wifi_failure(station_target.ssid, failure);
        job.failure = pico_page::LinkFailure::Other;
        return;
    }
    if (cancelled.load()) return;
    publish_wifi(pico_page::WifiPhase::Request, station_target.ssid);
    const auto &request = job.operation.request;
    const bool okay = platform::http_request(request.method, request.url, request.headers, request.body, job.body, job.status,
                           job.error, job.operation.cancelled.get());
    if (!okay) {
        job.error = request.method == "GET" ? "同步服务不可达，请检查网络或服务地址后重试。" : "上传结果未确认，请重新查询远端。";
        pico_page::network::release(job.operation.session);
    } else if (!cancelled.load()) holds.await_decision(job.operation.session, esp_timer_get_time());
}
#ifdef CONFIG_PICO_PAGE_PERF_METRICS
void run_request(NetworkJob &job) {
    PP_DIAGNOSTIC(const auto measurement = diag::begin(diag::Kind::Network, {0, 0, 0, job.operation.id});)
    execute_request(job);
    PP_DIAGNOSTIC(diag::end(measurement, job.operation.cancelled->load()
        ? diag::Outcome::Cancelled : !job.error.empty() || job.status < 200 || job.status >= 300
        ? diag::Outcome::Failure : diag::Outcome::Success);)
}
#endif
void network_worker(void *) {
    for (;;) {
        NetworkJob *job = nullptr;
        const auto wait = radio_active.load() || retained_station() ? pdMS_TO_TICKS(200) : portMAX_DELAY;
        if (xQueueReceive(requests, &job, wait) == pdTRUE) {
            if (job->portal) run_portal(*job);
            else if (job->wifi) {
                const auto& operation = *job->wifi;
                if (!operation.cancelled->load()) {
                    if (operation.kind == pico_page::WifiOperation::Kind::Connect) {
                        pico_page::network::release(holds.manual);
                        holds.manual = operation.cancelled; holds.manual_suspended = false; reconnects = 0;
                        if (!ensure_station(operation.targets, *operation.cancelled, job->error, job->failure))
                            pico_page::network::release(holds.manual);
                    } else if (operation.kind == pico_page::WifiOperation::Kind::Join) {
                        // Verifies new credentials on a link of their own; nothing keeps it afterwards.
                        ensure_station(operation.targets, *operation.cancelled, job->error, job->failure, true);
                    } else {
                        radio_active.store(true);
                        std::vector<platform::NearbyNetwork> scanned;
                        if (platform::scan_networks(scanned, job->error, operation.cancelled.get())) job->nearby = nearby_wifi(scanned);
                        pico_page::network::release(operation.cancelled);
                    }
                }
            } else {
                run_request(*job);
                if (!job->error.empty() || job->operation.cancelled->load()) pico_page::network::release(job->operation.session);
            }
            if (!retained_station()) release_radio(holds.manual_suspended ? manual_paused : "");
            else if (platform::station_connected()) publish_wifi(pico_page::WifiPhase::Connected, station_target.ssid);
#ifdef CONFIG_PICO_PAGE_LEGACY_PERF_LOG
            ESP_LOGI("network_worker", "phase=request_done stack_free=%u http_status=%d failed=%d wifi_connected=%d internal_free=%u internal_largest=%u internal_min=%u psram_free=%u psram_largest=%u",
                     static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)), job->status, !job->error.empty(),
                     platform::station_connected(),
                     static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
                     static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
                     static_cast<unsigned>(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
                     static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
                     static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)));
#endif
            xQueueSend(responses, &job, portMAX_DELAY);
        }
        holds.expire(esp_timer_get_time());
        if (!pico_page::network::live(holds.manual)) holds.manual_suspended = false;
        if (!retained_station()) { release_radio(holds.manual_suspended ? manual_paused : ""); continue; }
        if (!platform::station_connected()) {
            // One reconnect to the same network; no background scan or unbounded retry loop.
            std::string error;
            pico_page::LinkFailure failure;
            if (!holds.manual_suspended && pico_page::network::live(holds.manual) && reconnects++ == 0 &&
                ensure_station({{station_target}, false}, *holds.manual, error, failure)) continue;
            holds.stop(); release_radio(error.empty() ? "Wi-Fi 连接已断开，请重试。" : error);
        }
    }
}
bool present(pico_page::ReaderApp &app) {
    while (app.pending_frame()) {
        ++presentation_sequence;
        std::string error;
        using pico_page::platform::PageWaveform;
        pico_page::platform::set_page_waveform(app.fast_page_updates() ? PageWaveform::Gray8
                                                                       : PageWaveform::Gray16);
        PP_DIAGNOSTIC(const auto measurement = diag::begin(diag::Kind::Frame,
            {app.pending_frame()->token.session, app.pending_frame()->token.layout,
             presentation_sequence, app.pending_frame()->token.request,
             static_cast<std::uint32_t>(app.pending_frame()->purpose)});)
        const bool okay = pico_page::platform::present(
            app.canvas().data(), app.canvas().pixels().size(),
            app.pending_frame()->refresh == pico_page::RefreshIntent::FullClean, error, app.pending_frame()->region);
        PP_DIAGNOSTIC(diag::end(measurement, okay ? diag::Outcome::Success : diag::Outcome::Failure);)
        pico_page::console::frame_completed(*app.pending_frame(), okay);
#ifdef CONFIG_PICO_PAGE_LEGACY_PERF_LOG
        const auto completed = esp_timer_get_time();
#endif
        app.complete_frame(okay);
#ifdef CONFIG_PICO_PAGE_LEGACY_PERF_LOG
        const auto timing = pico_page::platform::present_timing();
        ESP_LOGI(tag, "Frame complete: copy=%lld power=%lld draw=%lld complete=%lld ms ok=%d",
                 static_cast<long long>(timing.copy_us / 1000),
                 static_cast<long long>(timing.power_us / 1000),
                 static_cast<long long>(timing.draw_us / 1000),
                 static_cast<long long>((esp_timer_get_time() - completed) / 1000), okay);
#endif
        if (!okay) {
            ESP_LOGE(tag, "Display transaction failed: %s", error.c_str());
            settled_us = esp_timer_get_time();
            return false;
        }
        settled_us = esp_timer_get_time();
    }
    return true;
}
} // namespace
extern "C" void app_main() {
    std::string error;
    if (!pico_page::console::initialize(error))
        ESP_LOGW(tag, "Diagnostic console unavailable: %s", error.c_str());
    if (!pico_page::platform::initialize(error)) {
        ESP_LOGE(tag, "Board initialization failed: %s", error.c_str());
        if (pico_page::platform::panel_ready()) {
            pico_page::ReaderApp diagnostic("/sdcard", "pico-page-diagnostic");
            if (diagnostic.initialize()) {
                present(diagnostic);
                diagnostic.show_error("硬件初始化失败，请检查设备后重启。");
                present(diagnostic);
            }
        }
        return;
    }
    std::uint8_t mac[6]{};
    esp_efuse_mac_get_default(mac);
    char writer[48];
    std::snprintf(writer, sizeof(writer), "pico-page-%02x%02x%02x%02x%02x%02x", mac[0], mac[1],
                  mac[2], mac[3], mac[4], mac[5]);
    const auto *card = pico_page::platform::card_root();
    auto app = std::make_unique<pico_page::ReaderApp>(card ? card : "/sdcard", writer, true, true);
    if (!app->initialize()) {
        ESP_LOGE(tag, "Reader initialization failed");
        return;
    }
    present(*app);
    if (!card) {
        app->card_removed();
        present(*app);
    }
    requests = xQueueCreate(1, sizeof(NetworkJob *));
    responses = xQueueCreate(1, sizeof(NetworkJob *));
    portal_events = xQueueCreate(4, sizeof(pico_page::PortalStatus *));
    wifi_events = xQueueCreate(4, sizeof(pico_page::WifiStatus *));
    portal_emit_lock = xSemaphoreCreateMutex();
    const bool worker_ready =
        requests && responses && portal_events && wifi_events && portal_emit_lock &&
        xTaskCreate(network_worker, "pico_network", 16384, nullptr, 3, nullptr) == pdPASS;
    bool usb_mode = false;
    bool network_busy = false, sleep_requested = false, physical_suppressed = false;
    pico_page::PowerRequest pending_power;
    bool pending_usb = false, power_prepared = false;
    // Set while an upload session runs on the network worker; storing true ends it.
    std::shared_ptr<std::atomic_bool> portal_stop;
    std::optional<pico_page::PortalStatus> deferred_portal_failure;
    std::uint64_t input_sequence = 0, physical_sequence = 0, physical_source = 0;
    pico_page::platform::ReleasedTap released_tap;
    std::int64_t last_activity = esp_timer_get_time();
    pico_page::platform::Event sleep_input;
    pico_page::platform::SleepInput resume_input;
    const auto sleep_now = [&] {
        const bool ready = app->prepare_sleep();
        if (!ready && app->sleep_pending()) return false;
        if (ready && present(*app)) {
            // Presentation can outlast a new input; poll both the input queue and PMU before suspension.
            const bool polled = pico_page::platform::poll(sleep_input, error);
            if (!polled) ESP_LOGW(tag, "Sleep input check failed: %s", error.c_str());
            bool slept = false;
            if (polled && sleep_input.kind == pico_page::platform::EventKind::None) {
                if (!pico_page::platform::sleep(error, slept))
                    ESP_LOGW(tag, "Sleep failed: %s", error.c_str());
            }
            // A second short press cancels this request; other input follows the normal dispatcher.
            if (sleep_input.kind == pico_page::platform::EventKind::Power) sleep_input = {};
            if (!slept) resume_input.begin();
            app->wake(slept);
            present(*app);
        } else app->cancel_sleep();
        // The timer keeps counting during light sleep; a failed attempt also waits a full period.
        last_activity = esp_timer_get_time();
        return true;
    };
    for (;;) {
        if (sleep_requested && (physical_source || pico_page::platform::input_waiting_before(
                std::numeric_limits<std::int64_t>::max()))) {
            app->cancel_sleep();
            sleep_requested = false;
        }
        app->poll_background(!resume_input.active() && (physical_source || pico_page::platform::input_waiting_before(
            std::numeric_limits<std::int64_t>::max())));
        const auto battery = pico_page::platform::battery();
        app->set_battery(battery.valid ? std::optional<int>(battery.percent) : std::nullopt);
        if (app->pending_frame())
            present(*app);
        const auto requested_power = app->take_power_request();
        if (requested_power.action != pico_page::PowerAction::None) { pending_power = requested_power; }
        const auto power_request = pending_power;
        if (power_request.action != pico_page::PowerAction::None && !usb_mode && !network_busy &&
            !physical_source && !pico_page::console::transfer_active()) {
            sleep_requested = false;
            if (power_prepared || app->prepare_power(power_request)) {
                if (!power_prepared) {
                    present(*app); // The final image is best effort; it cannot veto shutdown.
                    app->stop_wifi();
                    power_prepared = true;
                }
                if (radio_active.load()) { vTaskDelay(pdMS_TO_TICKS(20)); continue; }
                if (!app->close_power_files()) {
                    ESP_LOGE(tag, "Power teardown did not close file consumers");
                    for (;;) vTaskDelay(pdMS_TO_TICKS(1000));
                }
                if (!pico_page::platform::finish_power(error))
                    ESP_LOGW(tag, "Power teardown: %s", error.c_str());
                if (power_request.action == pico_page::PowerAction::Restart) esp_restart();
                if (!pico_page::platform::power_off(error)) {
                    ESP_LOGE(tag, "PMU power-off: %s", error.c_str());
                    app->show_power_off_help();
                    present(*app);
                    // Drop panel rails after the fallback frame; at most one more release attempt.
                    pico_page::platform::finish_power(error);
                }
                // USB supply can keep the ESP alive. Do not resume app or storage work.
                pico_page::platform::set_performance(false);
                for (;;) vTaskDelay(pdMS_TO_TICKS(1000));
            }
            pending_power = {};
            present(*app);
        }
        auto usb_action = app->take_usb_transfer_action();
        if (usb_action == pico_page::UsbTransferAction::Start && !network_busy) { pending_usb = true; app->stop_wifi(); }
        if (pending_usb) {
            usb_action = radio_active.load() ? pico_page::UsbTransferAction::None : pico_page::UsbTransferAction::Start;
            if (usb_action != pico_page::UsbTransferAction::None) pending_usb = false;
        }
        if (usb_action == pico_page::UsbTransferAction::Start && !usb_mode) {
            if (network_busy || physical_source || sleep_requested ||
                pico_page::console::transfer_active() || app->pending_frame()) {
                app->show_usb_transfer(false, "设备正忙，请稍后重试。");
                present(*app);
            } else {
                app->show_usb_transfer(false, "正在准备 USB 文件传输。");
                if (present(*app) && app->prepare_usb_transfer()) {
                    usb_mode = true;
                    pico_page::platform::set_performance(true);
                    app->show_usb_transfer(false, "正在准备 USB 文件传输。");
                    if (present(*app)) {
                        const bool stopped = pico_page::console::shutdown(error);
                        // USB Serial/JTAG no longer owns the PHY after MSC installation.
                        if (stopped) esp_log_set_vprintf([](const char*, va_list) { return 0; });
                        const bool started = stopped && pico_page::platform::start_usb_storage(error);
                        app->show_usb_transfer(started, started ? "" : "启动失败，请退出并重启。");
                        present(*app);
                    }
                } else {
                    app->show_usb_transfer(false, "准备失败，请确认进度已保存。");
                    present(*app);
                }
            }
        } else if (usb_action == pico_page::UsbTransferAction::Exit && usb_mode) {
            if (pico_page::platform::stop_usb_storage(error)) esp_restart();
            app->show_usb_transfer(false, "设备仍忙，请稍后退出。");
            present(*app);
        }
        const auto portal_action = app->take_portal_action();
        if (portal_action == pico_page::PortalAction::Upload || portal_action == pico_page::PortalAction::UploadStation) {
            const bool station = portal_action == pico_page::PortalAction::UploadStation;
            // Read before the session owns the card.
            const auto targets = station ? app->upload_targets() : pico_page::WifiTargets{};
            if (portal_stop || !worker_ready || usb_mode || network_busy || sleep_requested ||
                pico_page::console::transfer_active() || !app->prepare_portal(station)) {
                pico_page::PortalStatus refused{pico_page::PortalPhase::Closed, "", "", "",
                    "设备正忙或无法开启网络会话，请稍后重试。", "", 0, "", app->portal_generation()};
                if (network_busy || pico_page::console::transfer_active())
                    deferred_portal_failure = std::move(refused);
                else app->show_portal(refused);
            } else {
                portal_stop = std::make_shared<std::atomic_bool>(false);
                auto *job = new NetworkJob{{app->portal_generation(), {}, portal_stop, {}, targets}, card ? card : "/sdcard", {}, {}, 0, true, station};
                // The request queue is empty whenever network_busy is false.
                xQueueSend(requests, &job, portMAX_DELAY);
                network_busy = true;
            }
            present(*app);
        }
        // A session also ends when its page is gone: the card was removed, the display failed
        // or the power key asked for sleep.
        if (portal_stop && (portal_action == pico_page::PortalAction::Stop || sleep_requested ||
                            !app->portal_visible()))
            portal_stop->store(true);
        pico_page::PortalStatus *portal_status = nullptr;
        while (portal_stop && !app->pending_frame() &&
               xQueueReceive(portal_events, &portal_status, 0) == pdTRUE) {
            app->show_portal(*portal_status);
            delete portal_status;
            present(*app);
        }
        NetworkJob *completed = nullptr;
        if (network_busy && !app->pending_frame() &&
            xQueueReceive(responses, &completed, 0) == pdTRUE) {
            if (completed->portal) {
                // States queued after the last drain belong to the session that has ended.
                while (xQueueReceive(portal_events, &portal_status, 0) == pdTRUE)
                    delete portal_status;
                app->show_portal({pico_page::PortalPhase::Closed, "", "", "", completed->error, "", 0, "", completed->operation.id});
                portal_stop.reset();
            } else if (completed->wifi)
                app->complete_wifi(completed->wifi->id, completed->nearby, completed->error, completed->failure);
            else
                app->complete_network(completed->operation.id, completed->status, completed->body,
                                      completed->error, completed->failure);
            delete completed;
            network_busy = false;
            present(*app);
        }
        if (deferred_portal_failure && !usb_mode && !network_busy &&
            !pico_page::console::transfer_active() && !app->pending_frame()) {
            app->show_portal(*deferred_portal_failure);
            deferred_portal_failure.reset();
            present(*app);
        }
        pico_page::WifiStatus *wifi_status = nullptr, *latest_wifi = nullptr;
        while (wifi_events && !app->pending_frame() && xQueueReceive(wifi_events, &wifi_status, 0) == pdTRUE) {
            delete latest_wifi; latest_wifi = wifi_status;
        }
        if (latest_wifi) { app->show_wifi(*latest_wifi); delete latest_wifi; present(*app); }
        if (!usb_mode && !network_busy && !sleep_requested && !resume_input.active() && !pico_page::console::transfer_active()) {
            if (const auto wifi = app->take_wifi_request()) {
                if (worker_ready) {
                    auto* job = new NetworkJob{}; job->wifi = *wifi;
                    if (xQueueSend(requests, &job, 0) == pdTRUE) network_busy = true;
                    else { delete job; app->complete_wifi(wifi->id, {}, "网络任务暂时不可用。"); }
                } else app->complete_wifi(wifi->id, {}, "网络任务创建失败。");
                present(*app);
            }
        }
        if (!usb_mode && !network_busy && !sleep_requested && !resume_input.active() && !pico_page::console::transfer_active()) {
            const auto request = app->take_network_request();
            if (request) {
                if (worker_ready) {
                    auto *job = new NetworkJob{*request, card ? card : "/sdcard", {}, {}, 0, false, false};
                    if (xQueueSend(requests, &job, 0) == pdTRUE)
                        network_busy = true;
                    else {
                        delete job;
                        app->complete_network(request->id, 0, "", "网络任务暂时不可用");
                        present(*app);
                    }
                } else {
                    app->complete_network(request->id, 0, "", "网络任务创建失败");
                    present(*app);
                }
            }
        }
        if (sleep_requested) app->stop_wifi(true);
        if (!usb_mode && sleep_requested && !network_busy && !radio_active.load() && !physical_source &&
            !pico_page::console::transfer_active()) {
            sleep_requested = !sleep_now();
        }
        auto event = std::exchange(sleep_input, pico_page::platform::Event{});
        if (event.kind == pico_page::platform::EventKind::None && !pico_page::platform::poll(event, error))
            ESP_LOGW(tag, "Input read failed: %s", error.c_str());
        using pico_page::platform::EventKind;
        if (event.kind != EventKind::None || pico_page::console::transfer_active()) {
            last_activity = esp_timer_get_time();
            pico_page::platform::set_performance(true);
        }
        const auto dispatch_tap = [&](const pico_page::platform::Event& tap,
                                      const pico_page::platform::Event& end, bool resumed = false) {
            pico_page::AppInput input{
                {physical_sequence, pico_page::Phase::Begin, pico_page::Input::TouchCenter},
                tap.kind == EventKind::Touch, tap.x, tap.y, !resumed && tap.time_us < settled_us,
                pico_page::platform::long_press(tap, end)};
            if (input.touch)
                input.touch_bounds = {end.min_x, end.min_y, end.max_x, end.max_y};
            if (tap.kind == EventKind::K1) input.event.input = pico_page::Input::K1;
            if (tap.kind == EventKind::K2) input.event.input = pico_page::Input::K2;
            if (tap.kind == EventKind::K3) input.event.input = pico_page::Input::K3;
#ifdef CONFIG_PICO_PAGE_LEGACY_PERF_LOG
            const auto dispatched = esp_timer_get_time();
#endif
            PP_DIAGNOSTIC(const auto measurement = diag::begin(diag::Kind::Input, {0, 0, 0, end.sequence});)
            app->input(input);
            input.event.phase = pico_page::Phase::End;
            app->input(input);
            PP_DIAGNOSTIC(diag::end(measurement, diag::Outcome::Success);)
#ifdef CONFIG_PICO_PAGE_LEGACY_PERF_LOG
            ESP_LOGI(tag, "Input dispatch: seq=%llu queued=%d waited=%lld prepare=%lld ms pending=%d",
                     static_cast<unsigned long long>(end.sequence), input.queued,
                     static_cast<long long>((dispatched - end.time_us) / 1000),
                     static_cast<long long>((esp_timer_get_time() - dispatched) / 1000),
                     app->pending_frame().has_value());
#endif
        };
        if (resume_input.active()) {
            if (event.kind == EventKind::SdRemoved || app->view() == pico_page::View::Error) {
                resume_input.clear();
            } else {
                resume_input.accept(event);
                if (event.kind != EventKind::InputReset) event = {};
                if (!app->content_busy() && !app->pending_frame()) {
                    const auto now = esp_timer_get_time();
                    const auto held = resume_input.due(now);
                    if (held && app->can_long_press_at(held->x, held->y) &&
                        !pico_page::platform::input_waiting_before(std::numeric_limits<std::int64_t>::max())) {
                        resume_input.accept({EventKind::Touch, pico_page::platform::EventPhase::End,
                                             held->sequence, held->x, held->y, now});
                    }
                    if (resume_input.menu()) {
                        resume_input.clear();
                        event.kind = EventKind::PowerLong;
                    } else if (resume_input.power()) {
                        resume_input.clear();
                        event.kind = EventKind::Power;
                    } else if (resume_input.tap()) {
                        const auto tap = *resume_input.tap();
                        const auto end = resume_input.end();
                        resume_input.clear();
                        if (input_sequence != std::numeric_limits<std::uint64_t>::max()) {
                            physical_sequence = ++input_sequence;
                            dispatch_tap(tap, end, true);
                            physical_sequence = 0;
                            present(*app);
                        }
                    } else if (!resume_input.held()) resume_input.clear();
                }
            }
        }
        if (event.kind == EventKind::InputReset) {
            released_tap.accept(event, presentation_sequence);
            physical_source = physical_sequence = 0;
            physical_suppressed = false;
        } else if (event.kind == EventKind::SdRemoved) {
            app->card_removed();
            present(*app);
        } else if (event.kind == EventKind::PowerLong) {
            app->cancel_sleep();
            sleep_requested = false;
            if (!usb_mode && !network_busy && !pico_page::console::transfer_active()) {
                released_tap.accept({EventKind::InputReset}, presentation_sequence);
                physical_source = physical_sequence = 0;
                physical_suppressed = false;
                if (!app->show_power_menu()) app->power_unavailable("请先结束当前任务，再关机或重启。");
            } else app->power_unavailable(usb_mode ? "请先弹出设备并结束 USB 传输。" : "请先结束网络或传输任务。");
            present(*app);
        } else if (event.kind == EventKind::Power) {
            if (usb_mode || app->view() == pico_page::View::PowerMenu || app->view() == pico_page::View::PowerOff) continue;
            if (sleep_requested) {
                app->cancel_sleep();
                sleep_requested = false;
            } else {
                sleep_requested = true;
                app->stop_wifi(true);
            }
        } else if (event.kind != EventKind::None) {
#ifdef CONFIG_PICO_PAGE_LEGACY_PERF_LOG
            ESP_LOGI(tag, "Input: seq=%llu kind=%d phase=%d x=%d y=%d",
                     static_cast<unsigned long long>(event.sequence), static_cast<int>(event.kind),
                     static_cast<int>(event.phase), event.x, event.y);
#endif
            if (event.phase == pico_page::platform::EventPhase::Begin) {
                if (sleep_requested) {
                    app->cancel_sleep();
                    sleep_requested = false;
                }
                physical_source = event.sequence;
                physical_suppressed = pico_page::console::transfer_active() && event.kind != EventKind::K2;
                if (physical_suppressed) {
                    physical_sequence = 0;
                    continue;
                }
                if (input_sequence == std::numeric_limits<std::uint64_t>::max()) {
                    ESP_LOGE(tag, "Input sequence exhausted");
                    continue;
                }
                physical_sequence = ++input_sequence;
            } else if (physical_suppressed && physical_source == event.sequence) {
                physical_suppressed = false;
                physical_source = 0;
                continue;
            } else if (!physical_sequence || physical_source != event.sequence) {
                continue;
            }
            const auto tap = released_tap.accept(event, presentation_sequence);
            if (event.phase != pico_page::platform::EventPhase::Begin)
                physical_source = 0;
            // Several contacts made during one busy period collapse into the latest one. The
            // keyboard instead receives every tap in order and draws them in one update.
            if (tap && tap->time_us < settled_us && !app->keyboard_visible() &&
                pico_page::platform::input_waiting_before(settled_us)) {
#ifdef CONFIG_PICO_PAGE_LEGACY_PERF_LOG
                ESP_LOGI(tag, "Input superseded: seq=%llu",
                         static_cast<unsigned long long>(tap->sequence));
#endif
            } else if (tap) dispatch_tap(*tap, event);
            if (!physical_source) physical_sequence = 0;
            present(*app);
        }
        if (event.kind == EventKind::None && physical_source && physical_sequence &&
            !physical_suppressed && !usb_mode && !sleep_requested &&
            !pico_page::console::transfer_active() &&
            !pico_page::platform::input_waiting_before(std::numeric_limits<std::int64_t>::max())) {
            const auto now = esp_timer_get_time();
            const auto held = released_tap.due(now, presentation_sequence);
            if (held && held->sequence == physical_source && held->time_us >= settled_us &&
                app->can_long_press_at(held->x, held->y)) {
                const pico_page::platform::Event end{EventKind::Touch, pico_page::platform::EventPhase::End,
                                                      held->sequence, held->x, held->y, now};
                released_tap.accept(end, presentation_sequence);
                dispatch_tap(*held, end);
                last_activity = now;
                present(*app);
            }
        }
        if (!usb_mode && pico_page::console::poll(*app, pico_page::platform::card_root(), input_sequence,
                                     !network_busy && !sleep_requested && !physical_source && !resume_input.active(),
                                     !sleep_requested && !physical_source && !resume_input.active())) {
            last_activity = esp_timer_get_time();
            pico_page::platform::set_performance(true);
        }
        if (pico_page::console::transfer_active()) app->invalidate_identity_cache();
        const auto idle = esp_timer_get_time() - last_activity;
        if (!usb_mode && !sleep_requested && app->view() != pico_page::View::PowerMenu && app->view() != pico_page::View::PowerOff &&
            idle >= idle_save_us && !physical_source && !app->pending_frame() &&
            app->save_due()) {
            app->save_progress();
            present(*app);
        }
        const bool background = !usb_mode && idle >= idle_background_us && !physical_source && !resume_input.active() &&
                                !network_busy && !sleep_requested && !app->pending_frame() &&
                                !pico_page::console::transfer_active() && app->background_due();
        if (background) {
#ifdef CONFIG_PICO_PAGE_LEGACY_PERF_LOG
            const auto started = esp_timer_get_time();
#endif
            app->background_step([] {
                return pico_page::platform::input_waiting_before(std::numeric_limits<std::int64_t>::max());
            });
#ifdef CONFIG_PICO_PAGE_LEGACY_PERF_LOG
            // Storage, decompression and one page layout can exceed the pagination target.
            const auto elapsed_ms = (esp_timer_get_time() - started) / 1000;
            if (elapsed_ms >= 150)
                ESP_LOGI(tag, "Page counting slice: %lld ms state=%s", static_cast<long long>(elapsed_ms),
                         app->background_state());
#endif
        }
        else if (!usb_mode && !sleep_requested && idle >= idle_slow_us && !app->pending_frame() && !app->content_busy() && !app->save_busy())
            pico_page::platform::set_performance(false);
        const std::int64_t sleep_after = app->auto_sleep_minutes() * 60'000'000LL;
        if (!usb_mode && app->view() != pico_page::View::PowerMenu && app->view() != pico_page::View::PowerOff &&
            !resume_input.active() && sleep_after > 0 && idle >= sleep_after)
            sleep_requested = true;
        if (pico_page::console::transfer_active()) vTaskDelay(pdMS_TO_TICKS(1));
        // A full tick between counting slices lets lower-priority tasks run.
        else pico_page::platform::wait_for_input(background ? portTICK_PERIOD_MS : 20);
    }
}
