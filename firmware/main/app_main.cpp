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
#include <cstdio>
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
struct NetworkJob {
    pico_page::NetworkOperation operation;
    std::string card, body, error;
    int status = 0;
    // A phone setup session instead of one request: operation.cancelled asks it to end, error
    // returns the closing message and device_id names this device in a new sync.toml.
    bool portal = false;
    std::string device_id;
    bool upload = false;
};
QueueHandle_t requests = nullptr, responses = nullptr;
// PortalStatus pointers from the network worker; the application task draws and deletes them.
QueueHandle_t portal_events = nullptr;
// State of one phone setup session shared by the network worker and the HTTP server task.
struct PortalSession {
    SemaphoreHandle_t lock = xSemaphoreCreateMutex();
    pico_page::provision::PageState page;
    std::optional<pico_page::provision::Submission> submitted;
};
std::string portal_page(void *context, bool submitted, const std::string &body) {
    auto &session = *static_cast<PortalSession *>(context);
    xSemaphoreTake(session.lock, portMAX_DELAY);
    if (submitted && !session.page.busy) {
        pico_page::provision::Submission submission;
        if (pico_page::provision::parse_form(body, submission, session.page.notice)) {
            session.submitted = std::move(submission);
            session.page.busy = true;
            session.page.notice = "已收到。";
        }
    }
    auto html = pico_page::provision::page(session.page);
    xSemaphoreGive(session.lock);
    return html;
}
void emit(const pico_page::PortalStatus &status) {
    auto *event = new pico_page::PortalStatus(status);
    if (xQueueSend(portal_events, &event, pdMS_TO_TICKS(1000)) != pdTRUE)
        delete event;
}
/**
 * Joins the submitted or saved network, checks a submitted account with the sync server and
 * only then writes the files. Returns an empty string on success, otherwise the reason for the
 * user. account is the saved account and receives the new one once it is written.
 */
std::string apply_submission(const NetworkJob &job, pico_page::sync::Config &account,
                             const pico_page::provision::Submission &submission,
                             pico_page::PortalStatus &status) {
    namespace platform = pico_page::platform;
    namespace provision = pico_page::provision;
    auto wifi = submission.network;
    std::string error;
    if (!submission.wifi && !provision::load_wifi(job.card, wifi, error))
        return std::string("设备上还没有可用的 Wi-Fi 配置，请同时填写 Wi-Fi。") + nothing_saved;
    status.phase = pico_page::PortalPhase::JoiningWifi;
    status.detail = wifi.ssid;
    emit(status);
    platform::ConnectFailure failure;
    if (!platform::connect_network(wifi.ssid, wifi.password, error, &failure)) {
        ESP_LOGW(tag, "Setup: %s", error.c_str());
        using Stage = platform::ConnectFailure::Stage;
        const auto kind = failure.stage == Stage::Link      ? provision::WifiFailure::Rejected
                          : failure.stage == Stage::Address ? provision::WifiFailure::NoAddress
                          : failure.stage == Stage::Clock   ? provision::WifiFailure::NoClock
                                                            : provision::WifiFailure::Setup;
        return provision::wifi_problem(wifi.ssid, kind, failure.reason) + nothing_saved;
    }
    auto next = account;
    if (submission.account) {
        next.endpoint = submission.endpoint;
        next.username = submission.username;
        next.userkey = submission.userkey;
        if (next.device_id.empty())
            next.device_id = job.device_id;
        status.phase = pico_page::PortalPhase::CheckingAccount;
        status.detail.clear();
        emit(status);
        pico_page::sync::HttpRequest request;
        std::string body;
        int code = 0;
        if (!pico_page::sync::auth_request(next, request, error))
            return std::string("同步账户的内容无效。") + nothing_saved;
        const bool transported =
            platform::http_request(request.method, request.url, request.headers, request.body,
                                   body, code, error, job.operation.cancelled.get());
        if (!transported || code != 200) {
            ESP_LOGW(tag, "Setup: account check status=%d %s", code, error.c_str());
            return provision::account_problem(transported, code, error) + nothing_saved;
        }
    }
    const pico_page::FileStore store(job.card);
    if (submission.wifi &&
        store.save_private("network.toml", provision::network_toml(wifi), error) != pico_page::StoreStatus::Ok) {
        ESP_LOGW(tag, "Setup: %s", error.c_str());
        return std::string("写入存储卡失败，请检查 TF 卡。") + nothing_saved;
    }
    if (submission.account &&
        store.save_private("sync.toml", provision::sync_toml(next), error) != pico_page::StoreStatus::Ok) {
        ESP_LOGW(tag, "Setup: %s", error.c_str());
        return std::string("同步账户写入存储卡失败，请检查 TF 卡。") +
               (submission.wifi ? "Wi-Fi 配置已保存。" : nothing_saved);
    }
    if (submission.account)
        account = next;
    return {};
}
void run_portal(NetworkJob &job) {
    namespace platform = pico_page::platform;
    namespace provision = pico_page::provision;
    using pico_page::PortalPhase;
    PortalSession session;
    pico_page::sync::Config account;
    {
        // Values the page shows as current; neither is a secret.
        provision::WifiCredentials wifi;
        std::string ignored;
        if (provision::load_wifi(job.card, wifi, ignored))
            session.page.current_ssid = wifi.ssid;
        if (pico_page::sync::load_config(job.card, account, ignored)) {
            session.page.endpoint = account.endpoint;
            session.page.username = account.username;
        } else
            account = {};
    }
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
    if (!session.lock ||
        !platform::start_access_point(name, password, session.page.networks, job.error) ||
        !platform::start_portal_server(portal_page, &session, provision::form_limit, job.error,
                                       job.upload ? job.card : "", job.operation.cancelled.get())) {
        ESP_LOGW(tag, "Setup session did not start: %s", job.error.c_str());
        job.error = "设备热点没有启动，请重试。";
    } else {
        job.error.clear();
        pico_page::PortalStatus status{PortalPhase::Waiting, name, password, "http://192.168.4.1", ""};
        emit(status);
        auto idle_since = esp_timer_get_time();
        while (!job.operation.cancelled->load()) {
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
            const auto problem = apply_submission(job, account, *submission, status);
            status.phase = problem.empty() ? PortalPhase::Saved : PortalPhase::Failed;
            status.detail = problem.empty() ? provision::saved_summary(*submission) : problem;
            emit(status);
            xSemaphoreTake(session.lock, portMAX_DELAY);
            session.page.busy = false;
            session.page.notice = status.detail;
            if (problem.empty() && submission->wifi)
                session.page.current_ssid = submission->network.ssid;
            if (problem.empty() && submission->account) {
                session.page.endpoint = account.endpoint;
                session.page.username = account.username;
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
void network_worker(void *) {
    for (;;) {
        NetworkJob *job = nullptr;
        if (xQueueReceive(requests, &job, portMAX_DELAY) != pdTRUE)
            continue;
        if (job->portal)
            run_portal(*job);
        else {
            pico_page::provision::WifiCredentials wifi;
            if (!job->operation.cancelled->load() &&
                pico_page::provision::load_wifi(job->card, wifi, job->error) &&
                pico_page::platform::connect_network(wifi.ssid, wifi.password, job->error) &&
                !job->operation.cancelled->load()) {
                const auto &request = job->operation.request;
                pico_page::platform::http_request(request.method, request.url, request.headers,
                                                  request.body, job->body, job->status, job->error,
                                                  job->operation.cancelled.get());
            }
            pico_page::platform::disconnect_network();
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
    const bool worker_ready =
        requests && responses && portal_events &&
        xTaskCreate(network_worker, "pico_network", 16384, nullptr, 3, nullptr) == pdPASS;
    bool usb_mode = false;
    bool network_busy = false, sleep_requested = false, physical_suppressed = false;
    // Set while a phone setup session runs on the network worker; storing true ends it.
    std::shared_ptr<std::atomic_bool> portal_stop;
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
        if (portal_action == pico_page::PortalAction::Start || portal_action == pico_page::PortalAction::Upload) {
            if (portal_stop || !worker_ready || usb_mode || network_busy || sleep_requested ||
                pico_page::console::transfer_active() || !app->prepare_portal(portal_action == pico_page::PortalAction::Upload)) {
                app->show_portal({pico_page::PortalPhase::Closed, "", "", "", "无法开启设备热点，请稍后重试。"});
            } else {
                portal_stop = std::make_shared<std::atomic_bool>(false);
                auto *job = new NetworkJob{{0, {}, portal_stop}, card ? card : "/sdcard", {}, {}, 0, true, writer, portal_action == pico_page::PortalAction::Upload};
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
                app->show_portal({pico_page::PortalPhase::Closed, "", "", "", completed->error});
                portal_stop.reset();
            } else
                app->complete_network(completed->operation.id, completed->status, completed->body,
                                      completed->error);
            delete completed;
            network_busy = false;
            present(*app);
        }
        if (!usb_mode && !network_busy && !pico_page::console::transfer_active()) {
            const auto request = app->take_network_request();
            if (request) {
                if (worker_ready) {
                    auto *job = new NetworkJob{*request, card ? card : "/sdcard", {}, {}, 0, false, {}};
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
        if (event.kind == EventKind::InputReset) {
            released_tap.accept(event, presentation_sequence);
            physical_source = physical_sequence = 0;
            physical_suppressed = false;
        } else if (event.kind == EventKind::SdRemoved) {
            app->card_removed();
            present(*app);
        } else if (event.kind == EventKind::Power) {
            if (usb_mode) continue;
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
            } else if (tap) {
                pico_page::AppInput input{
                    {physical_sequence, pico_page::Phase::Begin, pico_page::Input::TouchCenter},
                    tap->kind == EventKind::Touch, tap->x, tap->y, tap->time_us < settled_us,
                    pico_page::platform::long_press(*tap, event)};
                if (tap->kind == EventKind::K1) input.event.input = pico_page::Input::K1;
                if (tap->kind == EventKind::K2) input.event.input = pico_page::Input::K2;
                if (tap->kind == EventKind::K3) input.event.input = pico_page::Input::K3;
                const auto dispatched = esp_timer_get_time();
                app->input(input);
                input.event.phase = pico_page::Phase::End;
                app->input(input);
                ESP_LOGI(tag, "Input dispatch: seq=%llu queued=%d waited=%lld prepare=%lld ms pending=%d",
                         static_cast<unsigned long long>(event.sequence), input.queued,
                         static_cast<long long>((dispatched - event.time_us) / 1000),
                         static_cast<long long>((esp_timer_get_time() - dispatched) / 1000),
                         app->pending_frame().has_value());
            }
            if (!physical_source) physical_sequence = 0;
            present(*app);
        }
        if (!usb_mode && pico_page::console::poll(*app, pico_page::platform::card_root(), input_sequence,
                                     !network_busy && !sleep_requested && !physical_source)) {
            last_activity = esp_timer_get_time();
            pico_page::platform::set_performance(true);
        }
        if (pico_page::console::transfer_active()) app->invalidate_identity_cache();
        const auto idle = esp_timer_get_time() - last_activity;
        if (!usb_mode && idle >= idle_save_us && !physical_source && !app->pending_frame() &&
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
        if (!usb_mode && sleep_after > 0 && idle >= sleep_after)
            sleep_requested = true;
        if (pico_page::console::transfer_active()) vTaskDelay(pdMS_TO_TICKS(1));
        // A full tick between counting slices lets lower-priority tasks run.
        else pico_page::platform::wait_for_input(background ? portTICK_PERIOD_MS : 20);
    }
}
