#include "pico_page/app.h"
#include "pico_page/console.h"
#include "pico_page/platform.h"
#include "pico_page/released_tap.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <cstdio>
#include <memory>
#include <limits>

namespace {
constexpr auto tag = "pico_page";
constexpr std::int64_t idle_slow_us = 3'000'000;
// A progress commit blocks input sampling for its duration, so it waits until the page has
// been left alone this long. Leaving the book, sleeping and USB transfer commit immediately.
constexpr std::int64_t idle_save_us = 2'000'000;
std::uint64_t presentation_sequence = 0;
// When the most recent frame and its follow-up work finished; contacts sampled earlier were
// made while the previous screen was still being replaced.
std::int64_t settled_us = 0;
struct NetworkJob {
    pico_page::NetworkOperation operation;
    std::string card, body, error;
    int status = 0;
};
QueueHandle_t requests = nullptr, responses = nullptr;
void network_worker(void *) {
    for (;;) {
        NetworkJob *job = nullptr;
        if (xQueueReceive(requests, &job, portMAX_DELAY) != pdTRUE)
            continue;
        if (!job->operation.cancelled->load() &&
            pico_page::platform::connect_network(job->card, job->error) &&
            !job->operation.cancelled->load()) {
            const auto &request = job->operation.request;
            pico_page::platform::http_request(request.method, request.url, request.headers,
                                              request.body, job->body, job->status, job->error,
                                              job->operation.cancelled.get());
        }
        pico_page::platform::disconnect_network();
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
        ESP_LOGI(tag, "Frame complete: copy=%lld power=%lld draw=%lld save=%lld ms ok=%d",
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
    auto app = std::make_unique<pico_page::ReaderApp>(card ? card : "/sdcard", writer);
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
    const bool worker_ready =
        requests && responses &&
        xTaskCreate(network_worker, "pico_network", 16384, nullptr, 3, nullptr) == pdPASS;
    bool usb_mode = false;
    bool network_busy = false, sleep_requested = false, physical_suppressed = false;
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
        NetworkJob *completed = nullptr;
        if (network_busy && !app->pending_frame() &&
            xQueueReceive(responses, &completed, 0) == pdTRUE) {
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
                    auto *job = new NetworkJob{*request, card ? card : "/sdcard", {}, {}, 0};
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
        if (event.kind == EventKind::SdRemoved) {
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
                    tap->kind == EventKind::Touch, tap->x, tap->y, tap->time_us < settled_us};
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
        const auto idle = esp_timer_get_time() - last_activity;
        if (!usb_mode && idle >= idle_save_us && !physical_source && !app->pending_frame() &&
            app->save_due()) {
            app->save_progress();
            present(*app);
        }
        if (!usb_mode && idle >= idle_slow_us && !app->pending_frame())
            pico_page::platform::set_performance(false);
        const std::int64_t sleep_after = app->auto_sleep_minutes() * 60'000'000LL;
        if (!usb_mode && sleep_after > 0 && idle >= sleep_after)
            sleep_requested = true;
        if (pico_page::console::transfer_active()) vTaskDelay(pdMS_TO_TICKS(1));
        else pico_page::platform::wait_for_input(20);
    }
}
