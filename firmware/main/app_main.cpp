#include "pico_page/app.h"
#include "pico_page/console.h"
#include "pico_page/platform.h"
#include "pico_page/provision.h"
#include "pico_page/released_tap.h"
#include "pico_page/store.h"
#include "esp_log.h"
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
// A phone setup session ends itself after this long without a submission; the access point
// keeps the radio powered for as long as it is up.
constexpr std::int64_t portal_idle_us = 600'000'000;
constexpr char nothing_saved[] = "设备没有保存这次的设置。";
namespace platform = pico_page::platform;
namespace provision = pico_page::provision;
struct NetworkJob {
    pico_page::NetworkOperation operation;
    std::string card, body, error;
    int status = 0;
    // A phone setup session instead of one request: operation.cancelled asks it to end, error
    // returns the closing message and device_id names this device in a new sync.toml.
    bool portal = false;
    std::string device_id;
    // A book upload session; station runs it over a saved network instead of the access point.
    bool upload = false;
    bool station = false;
};
QueueHandle_t requests = nullptr, responses = nullptr;
// PortalStatus pointers from the network worker; the application task draws and deletes them.
QueueHandle_t portal_events = nullptr;
SemaphoreHandle_t portal_emit_lock = nullptr;
// State of one phone session shared by the network worker and the HTTP server task.
struct PortalSession {
    SemaphoreHandle_t lock = xSemaphoreCreateMutex();
    provision::PageState page;
    std::optional<provision::Submission> submitted;
    // The state the device shows; upload sessions update it as books are saved.
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
std::string portal_page(void *context, bool submitted, const std::string &body) {
    auto &session = *static_cast<PortalSession *>(context);
    xSemaphoreTake(session.lock, portMAX_DELAY);
    auto &page = session.page;
    if (submitted && !page.busy) {
        provision::Submission submission;
        std::string problem;
        if (provision::parse_form(body, submission, problem)) {
            page.busy = true;
            page.busy_action = submission.action;
            page.busy_verify_wifi = submission.verify_wifi;
            page.busy_name = submission.network.ssid;
            page.stage = 0;
            page.result.reset();
            session.submitted = std::move(submission);
        } else {
            page.result = provision::Result{submission.action, true, problem};
            page.retry_endpoint = submission.endpoint;
            page.retry_username = submission.username;
        }
    }
    auto html = provision::page(page);
    xSemaphoreGive(session.lock);
    return html;
}
void set_stage(PortalSession &session, std::size_t stage) {
    xSemaphoreTake(session.lock, portMAX_DELAY);
    session.page.stage = stage;
    xSemaphoreGive(session.lock);
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
        nearby.push_back({item.ssid, item.rssi});
    return nearby;
}
/**
 * Tries the networks in order until one accepts. Cancellation stops before the next attempt.
 * joined receives the accepted name; problem receives the first refusal or cancellation.
 */
bool join_saved(const std::vector<provision::SavedWifi> &order, bool clock, std::string &joined,
                std::string &problem, std::string &error, const std::atomic<bool> &cancelled) {
    if (order.empty()) {
        problem = "设备上没有已保存的 Wi-Fi。";
        error = "No saved Wi-Fi network";
        return false;
    }
    for (const auto &wifi : order) {
        if (cancelled.load()) { problem = "操作已取消。"; return false; }
        platform::ConnectFailure failure;
        if (platform::connect_network(wifi.ssid, wifi.password, error, &failure, clock)) {
            if (cancelled.load()) { problem = "操作已取消。"; return false; }
            joined = wifi.ssid;
            return true;
        }
        if (cancelled.load()) { problem = "操作已取消。"; return false; }
        ESP_LOGW(tag, "Wi-Fi: %s", error.c_str());
        if (problem.empty())
            problem = wifi_failure(wifi.ssid, failure);
    }
    return false;
}
/** Records the fallback order and a rate-limited connection time after a successful join. */
void remember_connection(const std::string &card, const std::string &ssid) {
    std::string error;
    if (!provision::save_connection(card, ssid, std::time(nullptr), error))
        ESP_LOGW(tag, "Connection record not saved: %s", error.c_str());
}
/**
 * Applies one setup action; scanned Wi-Fi is verified before saving, while manually entered
 * credentials are saved without connecting. The account is verified before saving, and a
 * failure of one leaves the other unchanged. Returns an empty string on success, otherwise the
 * reason for the user. account is the saved account and receives the new one once it is written.
 */
std::string apply_submission(const NetworkJob &job, PortalSession &session, pico_page::sync::Config &account,
                             const provision::Submission &submission, pico_page::PortalStatus &status) {
    using provision::Action;
    if (job.operation.cancelled->load()) return "操作已取消。";
    const pico_page::FileStore store(job.card);
    std::vector<provision::SavedWifi> saved;
    std::string error;
    if (!provision::load_networks(job.card, saved, error))
        return "PicoPage/network.toml 内容无效，设备没有改动它。";
    const auto &wifi = submission.network;
    const auto known = std::any_of(saved.begin(), saved.end(),
                                   [&](const provision::SavedWifi &entry) { return entry.ssid == wifi.ssid; });
    if (submission.action == Action::WifiDelete) {
        if (!provision::remove_network(saved, wifi.ssid))
            return "没有找到这个网络，它可能已经删除。";
        if (job.operation.cancelled->load()) return "操作已取消。";
        if (!provision::save_network_edit(job.card, wifi, true, 0, error)) {
            ESP_LOGW(tag, "Setup: %s", error.c_str());
            return "保存 Wi-Fi 失败，请检查 PicoPage/network.toml 和 TF 卡。";
        }
        return {};
    }
    if (submission.action == Action::WifiAdd) {
        if (!known && saved.size() >= provision::saved_limit)
            return "已保存 5 个网络，请先删除一个。";
        if (submission.verify_wifi) {
            status.phase = pico_page::PortalPhase::JoiningWifi;
            status.detail = wifi.ssid;
            emit(status);
            platform::ConnectFailure failure;
            if (!platform::connect_network(wifi.ssid, wifi.password, error, &failure, false)) {
                ESP_LOGW(tag, "Setup: %s", error.c_str());
                return wifi_failure(wifi.ssid, failure);
            }
            if (job.operation.cancelled->load()) return "操作已取消。";
            set_stage(session, 1);
        }
        if (job.operation.cancelled->load()) return "操作已取消。";
        const auto connected_at = submission.verify_wifi ? std::time(nullptr) : 0;
        if (!provision::save_network_edit(job.card, wifi, false, connected_at, error)) {
            ESP_LOGW(tag, "Setup: %s", error.c_str());
            return "保存 Wi-Fi 失败，请检查 PicoPage/network.toml 和 TF 卡。";
        }
        return {};
    }
    xSemaphoreTake(session.lock, portMAX_DELAY);
    const auto order = provision::connection_order(saved, session.page.nearby);
    xSemaphoreGive(session.lock);
    status.phase = pico_page::PortalPhase::JoiningWifi;
    status.detail = order.empty() ? "" : order.front().ssid;
    emit(status);
    std::string joined, problem;
    if (!join_saved(order, false, joined, problem, error, *job.operation.cancelled))
        return problem;
    set_stage(session, 1);
    if (!platform::synchronize_clock(error)) {
        ESP_LOGW(tag, "Setup: %s", error.c_str());
        return provision::wifi_problem(joined, provision::WifiFailure::NoClock, 0);
    }
    if (job.operation.cancelled->load()) return "操作已取消。";
    remember_connection(job.card, joined);
    auto next = account;
    next.endpoint = submission.endpoint;
    next.username = submission.username;
    next.userkey = submission.userkey;
    if (next.device_id.empty())
        next.device_id = job.device_id;
    set_stage(session, 2);
    status.phase = pico_page::PortalPhase::CheckingAccount;
    status.detail.clear();
    emit(status);
    pico_page::sync::HttpRequest request;
    std::string body;
    int code = 0;
    if (!pico_page::sync::auth_request(next, request, error))
        return "同步账户的内容无效。";
    const bool transported = platform::http_request(request.method, request.url, request.headers, request.body,
                                                    body, code, error, job.operation.cancelled.get());
    if (!transported || code != 200) {
        ESP_LOGW(tag, "Setup: account check status=%d %s", code, error.c_str());
        return provision::account_problem(transported, code, error);
    }
    if (job.operation.cancelled->load()) return "操作已取消。";
    set_stage(session, 3);
    if (store.save_private("sync.toml", provision::sync_toml(next), error) != pico_page::StoreStatus::Ok) {
        ESP_LOGW(tag, "Setup: %s", error.c_str());
        return "同步账户写入存储卡失败，请检查 TF 卡。";
    }
    account = next;
    return {};
}
/** Starts the device access point and the setup or upload server on it. */
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
    if (!platform::start_access_point(name, password, scanned, job.error))
        return false;
    xSemaphoreTake(session.lock, portMAX_DELAY);
    session.page.nearby = nearby_wifi(scanned);
    session.page.device_name = name;
    session.status = {pico_page::PortalPhase::Waiting, name, password, "http://192.168.4.1", "", "", 0, "", job.operation.id};
    xSemaphoreGive(session.lock);
    if (job.upload) {
        platform::UploadServer server;
        server.root = job.card;
        server.cancelled = job.operation.cancelled.get();
        server.network = name;
        server.saved = upload_saved;
        server.context = &session;
        return platform::start_upload_server(server, job.error);
    }
    return platform::start_portal_server(portal_page, &session, provision::form_limit, job.error);
}
/**
 * Joins the strongest nearby saved network and serves the upload page at an address that
 * includes a short random path, so the page is reached from the screen rather than by guessing.
 * On failure job.error is the sentence the network page shows.
 */
bool start_station_upload(NetworkJob &job, PortalSession &session) {
    const auto saved = session.page.saved;
    std::vector<platform::NearbyNetwork> scanned;
    std::string error, joined, problem;
    if (saved.size() > 1 && !platform::scan_networks(scanned, error))
        ESP_LOGW(tag, "Wi-Fi scan: %s", error.c_str());
    if (job.operation.cancelled->load())
        return false;
    if (!session.page.saved_readable ||
        !join_saved(provision::connection_order(saved, nearby_wifi(scanned)), false, joined, problem, error,
                    *job.operation.cancelled)) {
        job.error = (session.page.saved_readable ? problem : "PicoPage/network.toml 内容无效。") +
                    std::string("可以改用设备热点传书。");
        return false;
    }
    if (job.operation.cancelled->load()) return false;
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
    pico_page::sync::Config account;
    {
        // Values the page shows as current; none is a secret that the page displays.
        std::string ignored;
        session.page.saved_readable = provision::load_networks(job.card, session.page.saved, ignored);
        if (pico_page::sync::load_config(job.card, account, ignored)) {
            session.page.endpoint = account.endpoint;
            session.page.username = account.username;
        } else
            account = {};
    }
    const bool started = session.lock && (job.station ? start_station_upload(job, session) : start_hotspot(job, session));
    if (!started) {
        ESP_LOGW(tag, "Phone session did not start: %s", job.error.c_str());
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
        auto idle_since = esp_timer_get_time();
        while (!job.operation.cancelled->load()) {
            if (job.station && !platform::station_connected()) {
                if (!job.operation.cancelled->load())
                    job.error = "与已保存的 Wi-Fi 连接已断开。可以改用设备热点传书。";
                break;
            }
            if (!job.upload && esp_timer_get_time() - idle_since >= portal_idle_us) {
                job.error = "手机配置超过 10 分钟没有提交，已自动结束。";
                break;
            }
            std::optional<provision::Submission> submission;
            xSemaphoreTake(session.lock, portMAX_DELAY);
            submission.swap(session.submitted);
            xSemaphoreGive(session.lock);
            if (!submission) {
                vTaskDelay(pdMS_TO_TICKS(200));
                continue;
            }
            const auto problem = apply_submission(job, session, account, *submission, status);
            status.phase = problem.empty() ? PortalPhase::Saved : PortalPhase::Failed;
            status.detail = problem.empty() ? provision::saved_summary(*submission) : problem + nothing_saved;
            emit(status);
            std::vector<provision::SavedWifi> saved;
            std::string ignored;
            const bool readable = provision::load_networks(job.card, saved, ignored);
            xSemaphoreTake(session.lock, portMAX_DELAY);
            auto &page = session.page;
            page.busy = false;
            page.result = provision::Result{submission->action, !problem.empty(),
                                            problem.empty() ? provision::saved_summary(*submission) : problem,
                                            problem.empty() && submission->action == provision::Action::WifiAdd &&
                                                submission->verify_wifi};
            page.saved = std::move(saved);
            page.saved_readable = readable;
            page.retry_endpoint = submission->endpoint;
            page.retry_username = submission->username;
            if (problem.empty() && submission->action == provision::Action::Account) {
                page.endpoint = account.endpoint;
                page.username = account.username;
            }
            xSemaphoreGive(session.lock);
            idle_since = esp_timer_get_time();
        }
    }
    platform::stop_portal_server();
    platform::disconnect_network();
    if (session.lock)
        vSemaphoreDelete(session.lock);
}
/** Joins the best saved network with a set clock and performs one sync request. */
void run_request(NetworkJob &job) {
    std::vector<provision::SavedWifi> saved;
    std::vector<platform::NearbyNetwork> scanned;
    std::string joined, problem, ignored;
    const auto &cancelled = *job.operation.cancelled;
    if (cancelled.load() || !provision::load_networks(job.card, saved, job.error))
        return;
    // One saved network needs no scan; connection_order then tries it directly.
    if (saved.size() > 1 && !platform::scan_networks(scanned, ignored))
        ESP_LOGW(tag, "Wi-Fi scan: %s", ignored.c_str());
    if (cancelled.load() || !join_saved(provision::connection_order(saved, nearby_wifi(scanned)), true, joined,
                                        problem, job.error, cancelled) ||
        cancelled.load())
        return;
    remember_connection(job.card, joined);
    const auto &request = job.operation.request;
    platform::http_request(request.method, request.url, request.headers, request.body, job.body, job.status,
                           job.error, job.operation.cancelled.get());
}
void network_worker(void *) {
    for (;;) {
        NetworkJob *job = nullptr;
        if (xQueueReceive(requests, &job, portMAX_DELAY) != pdTRUE)
            continue;
        if (job->portal)
            run_portal(*job);
        else {
            run_request(*job);
            platform::disconnect_network();
        }
        ESP_LOGI("network_worker", "stack_free=%u", static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
        xQueueSend(responses, &job, portMAX_DELAY);
    }
}
bool present(pico_page::ReaderApp &app) {
    while (app.pending_frame()) {
        ++presentation_sequence;
        std::string error;
        using pico_page::platform::PageWaveform;
        pico_page::platform::set_page_waveform(app.fast_page_updates() ? PageWaveform::Gray8
                                                                       : PageWaveform::Gray16);
        const bool okay = pico_page::platform::present(
            app.canvas().data(), app.canvas().pixels().size(),
            app.pending_frame()->refresh == pico_page::RefreshIntent::FullClean, error);
        pico_page::console::frame_completed(*app.pending_frame(), okay);
        const auto completed = esp_timer_get_time();
        app.complete_frame(okay);
        const auto timing = pico_page::platform::present_timing();
        ESP_LOGI(tag, "Frame complete: copy=%lld power=%lld draw=%lld complete=%lld ms ok=%d",
                 static_cast<long long>(timing.copy_us / 1000),
                 static_cast<long long>(timing.power_us / 1000),
                 static_cast<long long>(timing.draw_us / 1000),
                 static_cast<long long>((esp_timer_get_time() - completed) / 1000), okay);
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
    portal_emit_lock = xSemaphoreCreateMutex();
    const bool worker_ready =
        requests && responses && portal_events && portal_emit_lock &&
        xTaskCreate(network_worker, "pico_network", 16384, nullptr, 3, nullptr) == pdPASS;
    bool usb_mode = false;
    bool network_busy = false, sleep_requested = false, physical_suppressed = false;
    // Set while a phone setup session runs on the network worker; storing true ends it.
    std::shared_ptr<std::atomic_bool> portal_stop;
    std::optional<pico_page::PortalStatus> deferred_portal_failure;
    std::uint64_t input_sequence = 0, physical_sequence = 0, physical_source = 0;
    pico_page::platform::ReleasedTap released_tap;
    std::int64_t last_activity = esp_timer_get_time();
    const auto sleep_now = [&] {
        if (app->prepare_sleep() && present(*app)) {
            if (!pico_page::platform::sleep(error))
                ESP_LOGW(tag, "Sleep failed: %s", error.c_str());
            app->wake();
            present(*app);
        }
        // The timer keeps counting during light sleep; a failed attempt also waits a full period.
        last_activity = esp_timer_get_time();
    };
    for (;;) {
        app->poll_background(physical_source || pico_page::platform::input_waiting_before(
            std::numeric_limits<std::int64_t>::max()));
        const auto battery = pico_page::platform::battery();
        app->set_battery(battery.valid ? std::optional<int>(battery.percent) : std::nullopt);
        if (app->pending_frame())
            present(*app);
        const auto power_request = app->take_power_request();
        if (power_request.action != pico_page::PowerAction::None && !usb_mode && !network_busy &&
            !physical_source && !pico_page::console::transfer_active()) {
            sleep_requested = false;
            if (app->prepare_power(power_request)) {
                present(*app); // The final image is best effort; it cannot veto shutdown.
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
            present(*app);
        }
        const auto usb_action = app->take_usb_transfer_action();
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
        if (portal_action == pico_page::PortalAction::Start || portal_action == pico_page::PortalAction::Upload ||
            portal_action == pico_page::PortalAction::UploadStation) {
            const bool upload = portal_action != pico_page::PortalAction::Start;
            const bool station = portal_action == pico_page::PortalAction::UploadStation;
            if (portal_stop || !worker_ready || usb_mode || network_busy || sleep_requested ||
                pico_page::console::transfer_active() || !app->prepare_portal(upload, station)) {
                pico_page::PortalStatus refused{pico_page::PortalPhase::Closed, "", "", "",
                    "设备正忙或无法开启网络会话，请稍后重试。", "", 0, "", app->portal_generation()};
                if (network_busy || pico_page::console::transfer_active())
                    deferred_portal_failure = std::move(refused);
                else app->show_portal(refused);
            } else {
                portal_stop = std::make_shared<std::atomic_bool>(false);
                auto *job = new NetworkJob{{app->portal_generation(), {}, portal_stop}, card ? card : "/sdcard", {}, {}, 0, true, writer, upload, station};
                // The request queue is empty whenever network_busy is false.
                xQueueSend(requests, &job, portMAX_DELAY);
                network_busy = true;
            }
            present(*app);
        }
        // A session also ends when its page is gone: the card was removed, the display failed
        // or the power key asked for sleep.
        if (portal_stop && (portal_action == pico_page::PortalAction::Stop || sleep_requested ||
                            app->view() != pico_page::View::Portal))
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
            } else
                app->complete_network(completed->operation.id, completed->status, completed->body,
                                      completed->error);
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
        if (!usb_mode && !network_busy && !pico_page::console::transfer_active()) {
            const auto request = app->take_network_request();
            if (request) {
                if (worker_ready) {
                    auto *job = new NetworkJob{*request, card ? card : "/sdcard", {}, {}, 0, false, {}, false, false};
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
        if (!usb_mode && sleep_requested && !network_busy && !physical_source &&
            !pico_page::console::transfer_active()) {
            sleep_requested = false;
            sleep_now();
        }
        pico_page::platform::Event event;
        if (!pico_page::platform::poll(event, error))
            ESP_LOGW(tag, "Input read failed: %s", error.c_str());
        using pico_page::platform::EventKind;
        if (event.kind != EventKind::None || network_busy || pico_page::console::transfer_active()) {
            last_activity = esp_timer_get_time();
            pico_page::platform::set_performance(true);
        }
        const auto dispatch_tap = [&](const pico_page::platform::Event& tap,
                                      const pico_page::platform::Event& end) {
            pico_page::AppInput input{
                {physical_sequence, pico_page::Phase::Begin, pico_page::Input::TouchCenter},
                tap.kind == EventKind::Touch, tap.x, tap.y, tap.time_us < settled_us,
                pico_page::platform::long_press(tap, end)};
            if (input.touch)
                input.touch_bounds = {end.min_x, end.min_y, end.max_x, end.max_y};
            if (tap.kind == EventKind::K1) input.event.input = pico_page::Input::K1;
            if (tap.kind == EventKind::K2) input.event.input = pico_page::Input::K2;
            if (tap.kind == EventKind::K3) input.event.input = pico_page::Input::K3;
            const auto dispatched = esp_timer_get_time();
            app->input(input);
            input.event.phase = pico_page::Phase::End;
            app->input(input);
            ESP_LOGI(tag, "Input dispatch: seq=%llu queued=%d waited=%lld prepare=%lld ms pending=%d",
                     static_cast<unsigned long long>(end.sequence), input.queued,
                     static_cast<long long>((dispatched - end.time_us) / 1000),
                     static_cast<long long>((esp_timer_get_time() - dispatched) / 1000),
                     app->pending_frame().has_value());
        };
        if (event.kind == EventKind::InputReset) {
            released_tap.accept(event, presentation_sequence);
            physical_source = physical_sequence = 0;
            physical_suppressed = false;
        } else if (event.kind == EventKind::SdRemoved) {
            app->card_removed();
            present(*app);
        } else if (event.kind == EventKind::PowerLong) {
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
            if (network_busy || physical_source || pico_page::console::transfer_active()) {
                sleep_requested = true;
            } else {
                sleep_now();
            }
        } else if (event.kind != EventKind::None) {
            ESP_LOGI(tag, "Input: seq=%llu kind=%d phase=%d x=%d y=%d",
                     static_cast<unsigned long long>(event.sequence), static_cast<int>(event.kind),
                     static_cast<int>(event.phase), event.x, event.y);
            if (event.phase == pico_page::platform::EventPhase::Begin) {
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
            // Several contacts made during one busy period collapse into the latest one.
            if (tap && tap->time_us < settled_us &&
                pico_page::platform::input_waiting_before(settled_us)) {
                ESP_LOGI(tag, "Input superseded: seq=%llu",
                         static_cast<unsigned long long>(tap->sequence));
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
                app->can_open_image_at(held->x, held->y)) {
                const pico_page::platform::Event end{EventKind::Touch, pico_page::platform::EventPhase::End,
                                                      held->sequence, held->x, held->y, now};
                released_tap.accept(end, presentation_sequence);
                dispatch_tap(*held, end);
                last_activity = now;
                present(*app);
            }
        }
        if (!usb_mode && pico_page::console::poll(*app, pico_page::platform::card_root(), input_sequence,
                                     !network_busy && !sleep_requested && !physical_source)) {
            last_activity = esp_timer_get_time();
            pico_page::platform::set_performance(true);
        }
        if (pico_page::console::transfer_active()) app->invalidate_identity_cache();
        const auto idle = esp_timer_get_time() - last_activity;
        if (!usb_mode && app->view() != pico_page::View::PowerMenu && app->view() != pico_page::View::PowerOff &&
            idle >= idle_save_us && !physical_source && !app->pending_frame() &&
            app->save_due()) {
            app->save_progress();
            present(*app);
        }
        const bool background = !usb_mode && idle >= idle_background_us && !physical_source &&
                                !network_busy && !sleep_requested && !app->pending_frame() &&
                                !pico_page::console::transfer_active() && app->background_due();
        if (background) {
            const auto started = esp_timer_get_time();
            app->background_step([] {
                return pico_page::platform::input_waiting_before(std::numeric_limits<std::int64_t>::max());
            });
            // Storage, decompression and one page layout can exceed the pagination target.
            const auto elapsed_ms = (esp_timer_get_time() - started) / 1000;
            if (elapsed_ms >= 150)
                ESP_LOGI(tag, "Page counting slice: %lld ms state=%s", static_cast<long long>(elapsed_ms),
                         app->background_state());
        }
        else if (!usb_mode && idle >= idle_slow_us && !app->pending_frame() && !app->content_busy() && !app->save_busy())
            pico_page::platform::set_performance(false);
        const std::int64_t sleep_after = app->auto_sleep_minutes() * 60'000'000LL;
        if (!usb_mode && app->view() != pico_page::View::PowerMenu && app->view() != pico_page::View::PowerOff &&
            sleep_after > 0 && idle >= sleep_after)
            sleep_requested = true;
        if (pico_page::console::transfer_active()) vTaskDelay(pdMS_TO_TICKS(1));
        // A full tick between counting slices lets lower-priority tasks run.
        else pico_page::platform::wait_for_input(background ? portTICK_PERIOD_MS : 20);
    }
}
