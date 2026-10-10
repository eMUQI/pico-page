/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * Adapted from Read Pico board bring-up, display, sleep and touch-key mapping
 * at 28cde682a4468a581c278761922724f57d976418. Pico Page changes enforce factory
 * VCOM before panel power-on and expose a single-task application interface.
 */

#include "pico_page/platform.h"
#include "pico_page/power_key.h"
#include "pico_page/text_turn_waveform.h"
#include "esp_attr.h"
#include "ripple.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <limits>

#include "cst836u.h"
#include "driver/gpio.h"
#include "e0470_epaper_waveform.h"
#include "epd_highlevel.h"
#include "epdiy.h"
#include "esp_log.h"
#include "esp_pm.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "ff.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "hal/usb_serial_jtag_ll.h"
#include "read_pico_board.h"
#include "read_pico_epd_timing.h"
#include "read_pico_pmu.h"
#include "read_pico_sd.h"
#include "sc7a20h.h"

namespace pico_page::platform {
namespace {
constexpr char tag[] = "pico_platform";
constexpr int key_area_top = 1300;
constexpr int key_pitch = 160;
constexpr std::int64_t rails_idle_us = 8'000'000;
constexpr std::uint32_t usb_reconnect_delay_ms = 100;
TaskHandle_t owner = nullptr;
bool attempted = false;
bool ready = false;
bool display_ready = false;
bool baseline_valid = false;
bool mounted = false;
bool usb_owns_card = false;
bool suppress_power = true;
bool pending_power_long = false;
PowerKey power_key;
int panel_vcom = 0;
std::int64_t rails_deadline = 0;
std::int64_t last_pmu_poll = 0;
std::int64_t last_card_poll = 0;
std::int64_t power_ignore_until = 0;
std::atomic<std::uint64_t> sequence{0};
EpdiyHighlevelState display{};
cst836u_handle_t touch = nullptr;
sc7a20h_handle_t accelerometer = nullptr;
Battery battery_state{};
// Touch and key events from the sampler task, oldest first.
QueueHandle_t touch_events = nullptr;
// Held by the sampler for each controller read and by sleep() around mode changes.
SemaphoreHandle_t touch_lock = nullptr;
// Asks the sampler to cancel the contact in progress and ignore input until release.
std::atomic<bool> suppress_touch{true};
// Sampling every 5 ms costs the application task about a fifth of its CPU-bound speed, so
// that rate is kept only around a contact. The controller's interrupt line starts it; the
// idle period bounds the delay should that line not respond.
constexpr TickType_t sample_period = pdMS_TO_TICKS(5);
constexpr TickType_t idle_sample_period = pdMS_TO_TICKS(50);
constexpr int settle_samples = 40;
TaskHandle_t sampler = nullptr;
std::atomic<std::uint32_t> input_overflows{0};
// Remains set until the sampler starts a controller read under touch_lock.
portMUX_TYPE touch_pending_lock = portMUX_INITIALIZER_UNLOCKED;
bool touch_pending = true;

void set_touch_pending(bool pending) {
    portENTER_CRITICAL(&touch_pending_lock);
    touch_pending = pending;
    portEXIT_CRITICAL(&touch_pending_lock);
}
bool has_pending_touch() {
    portENTER_CRITICAL(&touch_pending_lock);
    const bool pending = touch_pending;
    portEXIT_CRITICAL(&touch_pending_lock);
    return pending;
}

void IRAM_ATTR touch_interrupt(void*) {
    portENTER_CRITICAL_ISR(&touch_pending_lock);
    touch_pending = true;
    portEXIT_CRITICAL_ISR(&touch_pending_lock);
    BaseType_t woken = pdFALSE;
    vTaskNotifyGiveFromISR(sampler, &woken);
    if (woken) portYIELD_FROM_ISR();
}
std::string mount_error;
esp_pm_lock_handle_t performance_lock = nullptr;
bool performance_high = false;
PageWaveform page_waveform = PageWaveform::Gray16;
PresentTiming timing{};
PageWaveform selected_effect = PageWaveform::Gray16;
// Rebuilt only by the application task between synchronous display transactions.
DRAM_ATTR std::uint8_t text_turn_lut[E0470_GL16_FRAMES * waveform_phase_bytes];
EpdWaveformPhases text_turn_phases{};
const EpdWaveformPhases* text_turn_ranges[] = {&text_turn_phases};
const EpdWaveformMode text_turn_mode{MODE_GL16, 1, text_turn_ranges};
const EpdWaveformMode* text_turn_modes[] = {&text_turn_mode};
EpdWaveform text_turn_table{1, 1, text_turn_modes, nullptr};
static_assert(sizeof(text_turn_lut) == 2368);

bool failure(std::string& error, const char* operation, esp_err_t result) {
    error = std::string(operation) + ": " + esp_err_to_name(result);
    ESP_LOGE(tag, "%s", error.c_str());
    return false;
}

bool on_owner(std::string& error) {
    if (owner == nullptr || owner != xTaskGetCurrentTaskHandle()) {
        error = "Read Pico hardware must be used by its application task";
        return false;
    }
    return true;
}

void copy_portrait(const std::uint8_t* packed) {
    constexpr int source_stride = width / 2;
    constexpr int native_stride = height / 2;
    // Both frames exceed the 64 KiB data cache. A band of columns reads each source
    // line once and keeps its destination rows cached while y advances.
    constexpr int band = 64;
    for (int first = 0; first < width; first += band) {
        const int last = std::min(first + band, width);
        for (int y = 0; y < height; y += 2) {
            const auto* low_row = packed + y * source_stride;
            const auto* high_row = low_row + source_stride;
            for (int x = first; x < last; ++x) {
                const int shift = (x & 1) * 4;
                const auto low = (low_row[x / 2] >> shift) & 15;
                const auto high = (high_row[x / 2] >> shift) & 15;
                display.front_fb[(width - 1 - x) * native_stride + y / 2] =
                    static_cast<std::uint8_t>(low | (high << 4));
            }
        }
    }
}

void observe_card() {
    read_pico_sd_info_t info{};
    const auto result = read_pico_sd_get_info(&info);
    mounted = result == ESP_OK && info.mounted;
    mount_error = mounted ? "" : !info.present ? "TF card not inserted" :
        std::string("TF card unavailable: ") + esp_err_to_name(result);
}

bool await_mount(std::string& error) {
    const auto deadline = esp_timer_get_time() + 5'000'000;
    read_pico_sd_info_t info{};
    while (read_pico_sd_get_info(&info) == ESP_ERR_NOT_FINISHED &&
           esp_timer_get_time() < deadline) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    observe_card();
    error = mount_error;
    return mounted;
}

bool pmu_events(bool& short_press, bool& wake_press, std::string& error, bool* long_press = nullptr) {
    short_press = false;
    wake_press = false;
    if (long_press) *long_press = false;
    for (int i = 0; i <= PMU_EVENT_FIFO_DEPTH; ++i) {
        const auto result = read_pico_pmu_poll();
        const auto* snapshot = read_pico_pmu_get();
        if (result != ESP_OK || !snapshot->identity_ok || !snapshot->status_ok) {
            battery_state.valid = false;
            return failure(error, "PMU status", result != ESP_OK ? result : ESP_ERR_INVALID_RESPONSE);
        }
        battery_state.valid = (snapshot->flags & PMU_STATUS_BATTERY_VALID) && snapshot->soc_permille <= 1000;
        battery_state.percent = snapshot->soc_permille / 10;
        battery_state.millivolts = snapshot->battery_mv;
        battery_state.charging = snapshot->charge_state == PMU_CHARGE_CHARGING;
        if (!snapshot->pending_events) return true;
        if (!snapshot->event_ok || snapshot->event.event_id == 0) {
            return failure(error, "PMU event", ESP_ERR_INVALID_RESPONSE);
        }
        if (i == PMU_EVENT_FIFO_DEPTH) {
            return failure(error, "PMU event queue did not drain", ESP_ERR_TIMEOUT);
        }
        const auto type = snapshot->event.type;
        const auto ack = read_pico_pmu_event_ack(snapshot->event.event_id);
        if (ack != ESP_OK) return failure(error, "PMU event acknowledgement", ack);
        PowerKey::Action action = PowerKey::Action::None;
        if (type == PMU_EVT_KEY_DOWN) action = power_key.accept(PowerKey::Event::Down);
        if (type == PMU_EVT_KEY_SHORT) action = power_key.accept(PowerKey::Event::Short);
        if (type == PMU_EVT_KEY_LONG) action = power_key.accept(PowerKey::Event::Long);
        short_press = short_press || action == PowerKey::Action::Sleep;
        if (long_press && action == PowerKey::Action::Menu) *long_press = true;
        wake_press = wake_press || type == PMU_EVT_KEY_DOWN || type == PMU_EVT_KEY_SHORT;
    }
    return true;
}

/**
 * Clears the expander interrupt and reports whether a later PMU event will produce a new edge.
 *
 * The PMU holds CW_INT (FCA9555 P0.2) low while events are unacknowledged. An event queued
 * after the last acknowledgement but before this clear leaves CW_INT low with the expander
 * latch already reset, so no further edge can wake light sleep. The PMU queue is therefore
 * read after the clear; @p idle also refreshes the snapshot's key_state.
 *
 * @param irq Expander interrupt line, active low.
 * @param idle Set when the line is released and the PMU has no pending events.
 * @return false if the PMU status read fails; @p error then describes the failure.
 */
bool wake_lines_idle(gpio_num_t irq, bool& idle, std::string& error) {
    idle = false;
    read_pico_clear_ioe_int();
    if (!gpio_get_level(irq)) return true;
    const auto result = read_pico_pmu_poll();
    const auto* snapshot = read_pico_pmu_get();
    if (result != ESP_OK || !snapshot->identity_ok || !snapshot->status_ok) {
        return failure(error, "PMU status", result != ESP_OK ? result : ESP_ERR_INVALID_RESPONSE);
    }
    // The poll keeps the previous event count when that register read fails; the
    // CRC-checked STATUS flags reflect the queue and CW_INT as of this poll.
    constexpr auto pending = PMU_STATUS_EVENT_PENDING | PMU_STATUS_CW_INT_ASSERTED;
    idle = snapshot->pending_events == 0 && !(snapshot->flags & pending);
    return true;
}

/**
 * Consumes the PMU events behind a light-sleep wake and re-arms the expander interrupt.
 *
 * Besides key events, the PMU reports a battery sample about every 30 s while the host is
 * powered. Acknowledging events releases CW_INT, and the expander reports that release as
 * a further input change, so its interrupt is cleared only after the events are consumed.
 *
 * @param irq Expander interrupt line, active low.
 * @param wake_press Set when the events include a power-key press.
 * @return false if the PMU fails, or if the line or the PMU queue does not become idle
 *         within 2 s without a key press; @p error then describes the failure.
 */
bool settle_wake_interrupt(gpio_num_t irq, bool& wake_press, std::string& error) {
    const auto deadline = esp_timer_get_time() + 2'000'000;
    for (;;) {
        bool short_press = false;
        if (!pmu_events(short_press, wake_press, error)) return false;
        if (wake_press) return true;
        bool idle = false;
        if (!wake_lines_idle(irq, idle, error)) return false;
        if (idle) return true;
        if (esp_timer_get_time() >= deadline) {
            error = "Wake interrupt or PMU events did not settle without a power-key event";
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

EventKind hit_test(int x, int y) {
    if (y >= key_area_top && x >= 0 && x < 3 * key_pitch) {
        constexpr EventKind keys[] = {EventKind::K1, EventKind::K2, EventKind::K3};
        return keys[x / key_pitch];
    }
    return x >= 0 && x < width && y >= 0 && y < height ? EventKind::Touch : EventKind::None;
}

/**
 * Samples the controller independently of the application task, which blocks for the
 * duration of layout, panel updates and card writes. A contact that begins and ends
 * within such a period would otherwise never be observed.
 */
void sample_touch(void*) {
    Event active{};
    bool suppressed = true;
    bool failing = false;
    bool interrupt_seen = false;
    int fast = settle_samples;
    const auto emit = [&](EventPhase phase, std::int64_t now) {
        Event event = active;
        event.phase = phase;
        event.time_us = now;
        if (xQueueSend(touch_events, &event, 0) == pdTRUE) return;
        // Discard the incomplete backlog before publishing the recovery boundary. This task
        // is the only producer; the consumer can only free space after the reset.
        xQueueReset(touch_events);
        const Event reset{EventKind::InputReset, EventPhase::Cancel, event.sequence, 0, 0, now};
        const auto sent = xQueueSend(touch_events, &reset, 0);
        configASSERT(sent == pdTRUE);
        active = {};
        suppressed = true;
        ESP_LOGW(tag, "Input queue overflow: count=%lu; contacts cancelled",
                 static_cast<unsigned long>(input_overflows.fetch_add(1, std::memory_order_relaxed) + 1));
    };
    const auto cancel = [&](std::int64_t now) {
        if (active.kind != EventKind::None) emit(EventPhase::Cancel, now);
        active = {};
        suppressed = true;
    };
    for (;;) {
        const bool signalled = ulTaskNotifyTake(pdTRUE, fast > 0 ? sample_period : idle_sample_period) != 0;
        if (signalled && !interrupt_seen) {
            interrupt_seen = true;
            ESP_LOGI(tag, "Touch interrupt line is active");
        }
        if (signalled) {
            fast = settle_samples;
            // A burst of edges must not sample more than once per tick.
            vTaskDelay(1);
        } else if (fast > 0) {
            --fast;
        }
        cst836u_touch_t sample{};
        xSemaphoreTake(touch_lock, portMAX_DELAY);
        // Publish the sampled event before sleep can acquire the controller boundary.
        set_touch_pending(false);
        const auto result = cst836u_read(touch, &sample);
        const auto now = esp_timer_get_time();
        if (suppress_touch.exchange(false)) cancel(now);
        if (result != ESP_OK) {
            set_touch_pending(true);
            if (!failing) ESP_LOGW(tag, "Touch/key read: %s", esp_err_to_name(result));
            failing = true;
            cancel(now);
            xSemaphoreGive(touch_lock);
            continue;
        }
        failing = false;
        if (sample.touched) fast = settle_samples;
        if (suppressed) {
            if (!sample.touched) suppressed = false;
            xSemaphoreGive(touch_lock);
            continue;
        }
        if (sample.count > 1) {
            cancel(now);
        } else if (sample.touched) {
            if (active.kind == EventKind::None) {
                const auto kind = hit_test(sample.x, sample.y);
                if (kind == EventKind::None) {
                    suppressed = true;
                    xSemaphoreGive(touch_lock);
                    continue;
                }
                active = {kind, EventPhase::Begin, ++sequence, sample.x, sample.y, now};
                emit(EventPhase::Begin, now);
            } else if (hit_test(sample.x, sample.y) != active.kind) {
                cancel(now);
            } else {
                active.x = sample.x;
                active.y = sample.y;
                active.min_x = std::min<int>(active.min_x, sample.x);
                active.min_y = std::min<int>(active.min_y, sample.y);
                active.max_x = std::max<int>(active.max_x, sample.x);
                active.max_y = std::max<int>(active.max_y, sample.y);
            }
        } else if (active.kind != EventKind::None) {
            emit(EventPhase::End, now);
            active = {};
        }
        xSemaphoreGive(touch_lock);
    }
}

/** Reports a board event; the sampler cancels any contact in progress on its next sample. */
void system_event(EventKind kind, Event& event) {
    suppress_touch = true;
    event = {kind, EventPhase::Begin, ++sequence};
}

bool panel_status(sy7636a_status_t& status, std::string& error) {
    if (!read_pico_rails_on()) return failure(error, "Panel power", ESP_ERR_INVALID_STATE);
    const auto result = sy7636a_read(read_pico_sy7636a(), &status);
    if (result != ESP_OK) return failure(error, "Panel power status", result);
    if (!status.pg || status.fault != SY7636A_FAULT_NONE || !status.on || status.vcom_mv != panel_vcom) {
        return failure(error, "Panel power-good/fault", ESP_ERR_INVALID_STATE);
    }
    return true;
}
} // namespace

void set_performance(bool high) {
    if (!performance_lock || high == performance_high) return;
    const auto result = high ? esp_pm_lock_acquire(performance_lock) : esp_pm_lock_release(performance_lock);
    if (result == ESP_OK) performance_high = high;
}

bool initialize(std::string& error) {
    error.clear();
    if (attempted) {
        if (!on_owner(error)) return false;
        if (!ready) error = "Read Pico initialization failed; restart after correcting the reported hardware error";
        return ready;
    }
    attempted = true;
    owner = xTaskGetCurrentTaskHandle();

    // 80 MHz keeps the PLL and the 80 MHz APB clock, so peripheral timing is unchanged.
    // Without a lock the CPU would also render at the idle frequency; scaling is
    // therefore enabled only together with the lock.
    esp_pm_config_t power{240, 80, false};
    if (esp_pm_configure(&power) == ESP_OK) {
        if (esp_pm_lock_create(ESP_PM_CPU_FREQ_MAX, 0, "pico_active", &performance_lock) == ESP_OK) {
            set_performance(true);
        } else {
            power.min_freq_mhz = power.max_freq_mhz;
            esp_pm_configure(&power);
        }
    }

    // Board initialization keeps SY_EN and VCOM_EN low. Its default VCOM is
    // only a driver RAM value until the first panel power-on.
    epd_init(&epd_board_read_pico, &E0470_DISPLAY, EPD_LUT_1K);
    epd_set_rotation(EPD_ROT_INVERTED_PORTRAIT);
    if (epd_width() != height || epd_height() != width) {
        error = "Unexpected Read Pico panel geometry";
        return false;
    }
    read_pico_pmu_config_t pmu = READ_PICO_PMU_CONFIG_DEFAULT();
    auto result = read_pico_pmu_init(read_pico_i2c_bus(), &pmu);
    if (result != ESP_OK) return failure(error, "PMU initialization; panel remains off", result);
    int factory_vcom = 0;
    for (int attempt = 0; attempt < 3; ++attempt) {
        result = read_pico_pmu_vcom_get(&factory_vcom);
        if (result == ESP_OK) break;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (result != ESP_OK) return failure(error, "Factory VCOM unavailable; panel remains off", result);
    result = sy7636a_set_vcom(read_pico_sy7636a(), factory_vcom);
    if (result != ESP_OK) return failure(error, "Apply factory panel VCOM", result);
    panel_vcom = factory_vcom;

    read_pico_epd_set_pclk(18);
    read_pico_epd_use_scan(READ_PICO_EPD_SCAN_FULL);
    e0470_waveform_init();
    display = epd_hl_init(&E0470_WAVEFORM);
    display_ready = true;
    esp_log_level_set("epdiy", ESP_LOG_WARN);

    sc7a20h_config_t sensor = SC7A20H_DEVICE_CONFIG_DEFAULT();
    result = sc7a20h_init(read_pico_i2c_bus(), &sensor, &accelerometer);
    if (result == ESP_OK) result = sc7a20h_power_down(accelerometer);
    if (result != ESP_OK) ESP_LOGW(tag, "Accelerometer standby: %s", esp_err_to_name(result));

    cst836u_config_t input = CST836U_CONFIG_DEFAULT();
    input.int_gpio = static_cast<gpio_num_t>(READ_PICO_TP_INT_GPIO);
    input.reset_fn = read_pico_touch_reset;
    result = cst836u_init(read_pico_i2c_bus(), &input, &touch);
    if (result != ESP_OK) return failure(error, "Touch/key initialization", result);
    touch_events = xQueueCreate(16, sizeof(Event));
    touch_lock = xSemaphoreCreateMutex();
    // Above the application task so that sampling continues during its CPU-bound work.
    if (!touch_events || !touch_lock ||
        xTaskCreate(sample_touch, "pico_touch", 4096, nullptr, 5, &sampler) != pdPASS) {
        return failure(error, "Touch/key sampler", ESP_ERR_NO_MEM);
    }
    // The line is open-drain and active low. Without it the sampler still polls at its idle period.
    const auto service = gpio_install_isr_service(0);
    if ((service != ESP_OK && service != ESP_ERR_INVALID_STATE) ||
        gpio_set_intr_type(input.int_gpio, GPIO_INTR_NEGEDGE) != ESP_OK ||
        gpio_isr_handler_add(input.int_gpio, touch_interrupt, nullptr) != ESP_OK) {
        ESP_LOGW(tag, "Touch interrupt unavailable; sampling every %d ms when idle",
                 static_cast<int>(pdTICKS_TO_MS(idle_sample_period)));
    }
    bool unused_short = false;
    bool unused_wake = false;
    if (!pmu_events(unused_short, unused_wake, error)) return false;

    result = read_pico_sd_start_probe();
    if (result == ESP_OK || result == ESP_ERR_NOT_FINISHED) {
        std::string card_message;
        await_mount(card_message);
    } else {
        observe_card();
    }
    ready = true;
    ESP_LOGI(tag, "Read Pico initialized; factory VCOM loaded; card %s", mounted ? "mounted" : "unavailable");
    return true;
}

bool panel_ready() { return display_ready; }
void set_page_waveform(PageWaveform waveform) { page_waveform = waveform; }
PageWaveform page_effect_waveform() { return selected_effect; }
bool set_page_effect_waveform(PageWaveform waveform, std::string& error) {
    error.clear();
    if (!on_owner(error)) return false;
    if (waveform == PageWaveform::Ripple) {
        if (!display_ready || !prepare_ripple()) {
            error = "Ripple phase tables unavailable";
            return false;
        }
    } else if (waveform != PageWaveform::Gray16) {
        if (waveform != PageWaveform::TextTurnA && waveform != PageWaveform::TextTurnB &&
            waveform != PageWaveform::TextTurnC) {
            error = "Invalid text-turn variant";
            return false;
        }
        const auto* source = display_ready ? e0470_waveform_phases(&E0470_WAVEFORM, MODE_GL16) : nullptr;
        if (!source || !source->luts || source->phases != E0470_GL16_FRAMES ||
            E0470_WAVEFORM.num_temp_ranges != 1) {
            error = "Text-turn source GL16 unavailable";
            return false;
        }
        const TextTurnOptions options{true, waveform != PageWaveform::TextTurnA,
                                      waveform == PageWaveform::TextTurnC};
        if (!build_text_turn_waveform(source->luts, sizeof(text_turn_lut), source->phases,
                                      options, text_turn_lut, sizeof(text_turn_lut))) {
            error = "Text-turn LUT generation failed";
            return false;
        }
        text_turn_phases = *source;
        text_turn_phases.luts = text_turn_lut;
        text_turn_table.temp_intervals = E0470_WAVEFORM.temp_intervals;
    }
    if (waveform != PageWaveform::Ripple) release_ripple();
    selected_effect = waveform;
    ESP_LOGI(tag, "page_effect=%s", waveform == PageWaveform::Gray16 ? "default" :
             waveform == PageWaveform::Ripple ? "ripple" :
             waveform == PageWaveform::TextTurnA ? "A" : waveform == PageWaveform::TextTurnB ? "B" : "C");
    return true;
}
PresentTiming present_timing() { return timing; }
const char* card_root() { return mounted ? "/sdcard" : nullptr; }
const std::string& card_error() { return mount_error; }
Battery battery() { return battery_state; }

bool finish_power(std::string& error) {
    error.clear();
    if (!on_owner(error) || !ready || usb_owns_card) return false;
    epd_poweroff(); rails_deadline = 0;
    if (mounted) {
        const auto result = read_pico_sd_release();
        if (result != ESP_OK) return failure(error, "SD release", result);
        mounted = false;
    }
    return true;
}
bool power_off(std::string& error) {
    error.clear();
    if (!on_owner(error) || !ready || usb_owns_card) return false;
    const auto* snapshot = read_pico_pmu_get();
    if (!snapshot->identity_ok || !(snapshot->caps & PMU_CAP_HOST_EN_CONTROL))
        return failure(error, "PMU power-off unavailable", ESP_ERR_NOT_SUPPORTED);
    const auto result = read_pico_pmu_power_off();
    if (result != ESP_OK) return failure(error, "PMU power-off", result);
    return true;
}

bool refresh_card(std::string& error) {
    error.clear();
    if (!on_owner(error) || !ready) return false;
    mounted = false;
    const auto result = read_pico_sd_remount();
    if (result != ESP_OK && result != ESP_ERR_NOT_FINISHED) {
        observe_card();
        error = mount_error;
        return false;
    }
    return await_mount(error);
}

bool release_card_for_usb(std::string& error) {
    if (!on_owner(error) || !mounted || usb_owns_card) return false;
    // The TF card is the only mounted FatFs drive and uses drive 0. The host
    // displays its volume label rather than the USB product string.
    char label[64]{};
    const auto read_label = f_getlabel("0:", label, nullptr);
    if (read_label != FR_OK) {
        error = "Read TF volume label: " + std::to_string(read_label);
        return false;
    }
    if (label[0] == '\0' || std::strcmp(label, "NONAME") == 0) {
        const auto set_label = f_setlabel("0:PICO PAGE");
        if (set_label != FR_OK) {
            error = "Set TF volume label: " + std::to_string(set_label);
            return false;
        }
    }
    const auto result = read_pico_sd_release();
    if (result != ESP_OK) return failure(error, "SD release", result);
    mounted = false;
    usb_owns_card = true;
    return true;
}

bool present(const std::uint8_t* packed, std::size_t size, bool full_clean,
             std::string& error, std::optional<RefreshRegion> region) {
    error.clear();
    if (!on_owner(error)) return false;
    if (!display_ready || packed == nullptr || size != framebuffer_bytes ||
        (region && !region->fits(width, height))) {
        return failure(error, "Panel frame", ESP_ERR_INVALID_ARG);
    }
    set_performance(true);
    const auto started = esp_timer_get_time();
    copy_portrait(packed);
    const auto copied = esp_timer_get_time();
    read_pico_epd_use_scan(READ_PICO_EPD_SCAN_FULL);
    epd_lcd_set_prefill_lines(32);
    epd_poweron();
    sy7636a_status_t status{};
    const bool powered = panel_status(status, error);
    const auto drawing = esp_timer_get_time();
    timing = {copied - started, drawing - copied, 0};
    if (!powered) {
        baseline_valid = false;
        epd_poweroff();
        rails_deadline = 0;
        return false;
    }
    const bool clear = full_clean || !baseline_valid;
    baseline_valid = false;
    epd_set_leading_skip(!clear);
    EpdDrawError draw_result;
    // epd_hl_update_area applies EPD_ROT_INVERTED_PORTRAIT, matching copy_portrait.
    const auto update = [&] {
        if (region) return epd_hl_update_area(&display, MODE_GL16, status.temperature_c,
                                             EpdRect{region->x, region->y, region->width, region->height});
        return epd_hl_update_screen_full(&display, MODE_GL16, status.temperature_c);
    };
    if (clear) {
        // Physical black/white pulses establish a new baseline independently
        // of the previous frame, including when all target pixels are equal.
        draw_result = epd_clear_checked();
        if (draw_result == EPD_DRAW_SUCCESS)
            draw_result = epd_hl_update_screen_from_white(&display, MODE_GC16, status.temperature_c);
    } else if (page_waveform == PageWaveform::Ripple && selected_effect == PageWaveform::Ripple) {
        draw_result = present_ripple(display, status.temperature_c);
    } else if (page_waveform == selected_effect && selected_effect != PageWaveform::Gray16) {
        epd_hl_waveform(&display, &text_turn_table);
        draw_result = update();
        epd_hl_waveform(&display, &E0470_WAVEFORM);
    } else if (page_waveform == PageWaveform::Gray8) {
        epd_hl_waveform(&display, &E0470_GRAY8_WAVEFORM);
        draw_result = update();
        epd_hl_waveform(&display, &E0470_WAVEFORM);
    } else {
        draw_result = update();
    }
    bool power_good = panel_status(status, error);
    if (draw_result == EPD_DRAW_EMPTY_LINE_QUEUE && power_good) {
        // The driver has joined its producers and detached scan callbacks before returning.
        // Rebuild the physical baseline once, preserving the immutable target framebuffer.
        ESP_LOGW(tag, "display_recovery attempt=1 error=%u", static_cast<unsigned>(draw_result));
        read_pico_epd_set_pclk(12);
        epd_set_leading_skip(false);
        draw_result = epd_clear_checked();
        if (draw_result == EPD_DRAW_SUCCESS)
            draw_result = epd_hl_update_screen_from_white(&display, MODE_GC16, status.temperature_c);
        power_good = panel_status(status, error);
        ESP_LOGW(tag, "display_recovery result=%u power_good=%d", static_cast<unsigned>(draw_result), power_good);
    }
    timing.draw_us = esp_timer_get_time() - drawing;
    if (draw_result != EPD_DRAW_SUCCESS || !power_good) {
        if (draw_result != EPD_DRAW_SUCCESS) {
            error = "Panel waveform failed: " + std::to_string(static_cast<unsigned>(draw_result));
        }
        if (draw_result & EPD_DRAW_EMPTY_LINE_QUEUE) read_pico_epd_set_pclk(12);
        epd_poweroff();
        rails_deadline = 0;
        return false;
    }
    baseline_valid = true;
    rails_deadline = esp_timer_get_time() + rails_idle_us;
    return true;
}

InputDiagnostics input_diagnostics() {
    return {input_overflows.load(std::memory_order_relaxed),
            sampler ? static_cast<std::uint32_t>(uxTaskGetStackHighWaterMark(sampler)) : 0};
}

bool poll(Event& event, std::string& error) {
    event = {};
    error.clear();
    if (!on_owner(error) || !ready) return false;
    const auto now = esp_timer_get_time();
    if (rails_deadline && now >= rails_deadline) {
        epd_poweroff();
        rails_deadline = 0;
    }
    if (!usb_owns_card && now - last_card_poll >= 500'000) {
        last_card_poll = now;
        const bool was_mounted = mounted;
        observe_card();
        if (was_mounted && !mounted) {
            system_event(EventKind::SdRemoved, event);
            return true;
        }
    }
    if (std::exchange(pending_power_long, false)) {
        system_event(EventKind::PowerLong, event);
        return true;
    }
    if (now - last_pmu_poll >= 200'000) {
        last_pmu_poll = now;
        bool short_press = false;
        bool wake_press = false;
        bool long_press = false;
        if (!pmu_events(short_press, wake_press, error, &long_press)) {
            suppress_touch = true;
            return false;
        }
        if (suppress_power) {
            if (!(read_pico_pmu_get()->key_state & 1) && now >= power_ignore_until) suppress_power = false;
        } else if (long_press || short_press) {
            system_event(long_press ? EventKind::PowerLong : EventKind::Power, event);
            return true;
        }
    }
    xQueueReceive(touch_events, &event, 0);
    return true;
}

void wait_for_input(std::uint32_t timeout_ms) {
    Event next;
    if (touch_events) xQueuePeek(touch_events, &next, pdMS_TO_TICKS(timeout_ms));
    else vTaskDelay(pdMS_TO_TICKS(timeout_ms));
}

bool input_waiting_before(std::int64_t time_us) {
    Event next;
    return touch_events && xQueuePeek(touch_events, &next, 0) == pdTRUE && next.time_us < time_us;
}

static void reconnect_usb_console() {
    if (!usb_serial_jtag_ll_phy_is_pad_enabled()) return;
    decltype(USB_SERIAL_JTAG.conf0) config{};
    config.val = USB_SERIAL_JTAG.conf0.val;
    const usb_serial_jtag_pull_override_vals_t original{
        bool(config.dp_pullup), bool(config.dm_pullup),
        bool(config.dp_pulldown), bool(config.dm_pulldown)};
    const usb_serial_jtag_pull_override_vals_t disconnected{};
    // Clock gating does not guarantee a host-visible disconnect; remove the D+ pull-up.
    usb_serial_jtag_ll_phy_enable_pull_override(&disconnected);
    vTaskDelay(pdMS_TO_TICKS(usb_reconnect_delay_ms));
    usb_serial_jtag_ll_phy_enable_pull_override(&original);
    if (!config.pad_pull_override) usb_serial_jtag_ll_phy_disable_pull_override();
}

bool sleep(std::string& error, bool& entered_sleep) {
    entered_sleep = false;
    error.clear();
    if (!on_owner(error) || !ready) return false;
    if (usb_owns_card) {
        error = "USB storage must stop before sleep";
        return false;
    }
    epd_poweroff();
    rails_deadline = 0;
    // The sampler must not address the controller between standby and its reset on wake.
    xSemaphoreTake(touch_lock, portMAX_DELAY);
    if (input_waiting_before(std::numeric_limits<std::int64_t>::max()) ||
        has_pending_touch() || cst836u_int_asserted(touch)) {
        xSemaphoreGive(touch_lock);
        error = "Sleep cancelled by pending touch input";
        return false;
    }
    auto result = cst836u_set_mode(touch, CST836U_MODE_DEEPSLEEP);
    if (result != ESP_OK) {
        xSemaphoreGive(touch_lock);
        return failure(error, "Touch standby", result);
    }

    const auto irq = static_cast<gpio_num_t>(READ_PICO_IOE_INT_GPIO);
    gpio_config_t config{};
    config.pin_bit_mask = 1ULL << READ_PICO_IOE_INT_GPIO;
    config.mode = GPIO_MODE_INPUT;
    config.pull_up_en = GPIO_PULLUP_ENABLE;
    result = gpio_config(&config);
    bool slept = false;
    const auto idle_deadline = esp_timer_get_time() + 2'000'000;
    while (result == ESP_OK && esp_timer_get_time() < idle_deadline) {
        // An interrupt during the standby command still cancels the handoff.
        if (has_pending_touch()) {
            error = "Sleep cancelled by touch interrupt";
            break;
        }
        bool short_press = false;
        bool wake_press = false;
        bool long_press = false;
        if (!pmu_events(short_press, wake_press, error, &long_press)) break;
        if (short_press || wake_press || long_press) {
            pending_power_long = long_press;
            power_key.cancel_sleep_press();
            error = "Sleep cancelled by power key";
            break;
        }
        bool idle = false;
        if (!wake_lines_idle(irq, idle, error)) break;
        if (idle && !(read_pico_pmu_get()->key_state & 1)) {
            result = gpio_wakeup_enable(irq, GPIO_INTR_LOW_LEVEL);
            if (result == ESP_OK) result = esp_sleep_enable_gpio_wakeup();
            if (result != ESP_OK) break;
            while (result == ESP_OK) {
                // Wake-source setup and non-power wake handling may deliver a pending IRQ.
                if (has_pending_touch() || cst836u_int_asserted(touch)) {
                    error = "Sleep cancelled by touch interrupt";
                    break;
                }
                result = esp_light_sleep_start();
                if (result != ESP_OK) break;
                entered_sleep = true;
                if (!settle_wake_interrupt(irq, wake_press, error)) break;
                if (wake_press) {
                    slept = true;
                    break;
                }
            }
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    gpio_wakeup_disable(irq);
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_GPIO);
    const auto touch_result = cst836u_wake(touch);
    if (entered_sleep) {
        suppress_touch = true;
        xQueueReset(touch_events);
    }
    xSemaphoreGive(touch_lock);
    suppress_power = entered_sleep;
    if (entered_sleep) power_ignore_until = esp_timer_get_time() + 300'000;
    // Reconnect outside touch_lock, including when wake cleanup failed.
    if (entered_sleep) reconnect_usb_console();
    if (touch_result != ESP_OK) return failure(error, "Touch resume", touch_result);
    if (result != ESP_OK) return failure(error, "Light sleep", result);
    if (!slept && error.empty()) error = "Power key or wake interrupt did not become idle";
    return slept;
}

} // namespace pico_page::platform
