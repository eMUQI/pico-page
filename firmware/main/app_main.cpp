#include "pico_page/app.h"
#include "pico_page/console.h"
#include "pico_page/platform.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <cstdio>
#include <memory>
#include <limits>

namespace {
constexpr auto tag = "pico_page";
constexpr std::int64_t idle_slow_us = 3'000'000;
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
        std::string error;
        using pico_page::platform::PageWaveform;
        pico_page::platform::set_page_waveform(app.fast_page_updates() ? PageWaveform::Gray8
                                                                       : PageWaveform::Gray16);
        const bool okay = pico_page::platform::present(
            app.canvas().data(), app.canvas().pixels().size(),
            app.pending_frame()->refresh == pico_page::RefreshIntent::FullClean, error);
        pico_page::console::frame_completed(*app.pending_frame(), okay);
        app.complete_frame(okay);
        if (!okay) {
            ESP_LOGE(tag, "Display transaction failed: %s", error.c_str());
            return false;
        }
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
    bool network_busy = false, sleep_requested = false, physical_suppressed = false;
    std::uint64_t input_sequence = 0, physical_sequence = 0, physical_source = 0;
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
        if (app->pending_frame())
            present(*app);
        NetworkJob *completed = nullptr;
        if (network_busy && !app->pending_frame() &&
            xQueueReceive(responses, &completed, 0) == pdTRUE) {
            app->complete_network(completed->operation.id, completed->status, completed->body,
                                  completed->error);
            delete completed;
            network_busy = false;
            present(*app);
        }
        if (!network_busy && !pico_page::console::transfer_active()) {
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
        if (sleep_requested && !network_busy && !physical_source &&
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
            if (network_busy || physical_source || pico_page::console::transfer_active()) {
                sleep_requested = true;
            } else {
                sleep_now();
            }
        } else if (event.kind != EventKind::None) {
            if (event.phase == pico_page::platform::EventPhase::Begin) {
                physical_source = event.sequence;
                physical_suppressed = pico_page::console::transfer_active();
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
            pico_page::AppInput input{
                {physical_sequence, pico_page::Phase::Begin, pico_page::Input::TouchCenter},
                event.kind == EventKind::Touch,
                event.x,
                event.y};
            if (event.phase == pico_page::platform::EventPhase::End)
                input.event.phase = pico_page::Phase::End;
            if (event.phase == pico_page::platform::EventPhase::Cancel)
                input.event.phase = pico_page::Phase::Cancel;
            if (event.kind == EventKind::K1)
                input.event.input = pico_page::Input::K1;
            if (event.kind == EventKind::K2)
                input.event.input = pico_page::Input::K2;
            if (event.kind == EventKind::K3)
                input.event.input = pico_page::Input::K3;
            app->input(input);
            if (input.event.phase != pico_page::Phase::Begin) {
                physical_sequence = 0;
                physical_source = 0;
            }
            present(*app);
        }
        if (pico_page::console::poll(*app, pico_page::platform::card_root(), input_sequence,
                                     !network_busy && !sleep_requested && !physical_source)) {
            last_activity = esp_timer_get_time();
            pico_page::platform::set_performance(true);
        }
        const auto idle = esp_timer_get_time() - last_activity;
        if (idle >= idle_slow_us && !app->pending_frame())
            pico_page::platform::set_performance(false);
        const std::int64_t sleep_after = app->auto_sleep_minutes() * 60'000'000LL;
        if (sleep_after > 0 && idle >= sleep_after)
            sleep_requested = true;
        vTaskDelay(pdMS_TO_TICKS(pico_page::console::transfer_active() ? 1 : 20));
    }
}
