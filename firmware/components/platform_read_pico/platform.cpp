/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * Adapted from Read Pico board bring-up, display, sleep and touch-key mapping
 * at 28cde682a4468a581c278761922724f57d976418. Pico Page changes enforce factory
 * VCOM before panel power-on and expose a single-task application interface.
 */

#include "pico_page/platform.h"
#include "display_geometry.h"
#include "touch_moves.h"
#include "pico_page/power_key.h"
#include "pico_page/changed_pixels_waveform.h"
#include "esp_attr.h"
#include "page_sweep.h"
#include "rtc_clock.h"
#include "sleep_io.h"
#include "sleep_usb_console.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
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
TaskHandle_t owner = nullptr;
bool attempted = false;
bool ready = false;
bool display_ready = false;
bool baseline_valid = false;
bool mounted = false;
pico_page::CardIssue card_issue = pico_page::CardIssue::NotInserted;
bool usb_owns_card = false;
bool suppress_power = true;
bool pending_power_long = false;
PowerKey power_key;
int panel_vcom = 0;
std::int64_t rails_deadline = 0;
std::int64_t last_pmu_poll = 0;
std::int64_t last_card_poll = 0;
std::int64_t power_ignore_until = 0;
constexpr std::int64_t sleep_report_delay_us = 1'000'000;
struct SleepReport {
    std::int64_t report_after_us = 0;
    int wakes = 0;
    int rejected = 0;
    int pmu_retries = 0;
    int touch_edges = 0;
    int touch_low = 0;
    std::string first_pmu_error;
    std::string result;
};
SleepReport sleep_report;
std::atomic<std::uint64_t> sequence{0};
EpdiyHighlevelState display{};
cst836u_handle_t touch = nullptr;
sc7a20h_handle_t accelerometer = nullptr;
Battery battery_state{};
QueueHandle_t touch_events = nullptr;
TouchMoves touch_moves;
SemaphoreHandle_t touch_lock = nullptr;
SleepIo sleep_io;
std::atomic<bool> suppress_touch{true};
constexpr TickType_t sample_period = pdMS_TO_TICKS(5);
constexpr TickType_t idle_sample_period = pdMS_TO_TICKS(50);
constexpr int settle_samples = 40;
TaskHandle_t sampler = nullptr;
std::atomic<std::uint32_t> input_overflows{0};
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
bool reverse_page_effect = false;
PageTurnOrigin page_turn_origin = PageTurnOrigin::Middle;
PresentTiming timing{};
PageWaveform selected_effect = PageWaveform::Gray16;
template<std::size_t Phases>
struct ChangedPixelsTable {
    std::uint8_t lut[Phases * waveform_phase_bytes]{};
    EpdWaveformPhases phases{};
    const EpdWaveformPhases* ranges[1]{&phases};
    const EpdWaveformMode mode{MODE_GL16, 1, ranges};
    const EpdWaveformMode* modes[1]{&mode};
    EpdWaveform table{1, 1, modes, nullptr};
    bool ready = false;

    const EpdWaveform* prepare(const EpdWaveform& source) {
        if (!ready) {
            const auto* original = e0470_waveform_phases(&source, MODE_GL16);
            if (source.num_temp_ranges != 1 || !original || !original->luts ||
                original->phases != static_cast<int>(Phases) ||
                !build_changed_pixels_waveform(original->luts, sizeof(lut), Phases,
                                          lut, sizeof(lut))) return nullptr;
            phases = *original;
            phases.luts = lut;
            table.temp_intervals = source.temp_intervals;
            ready = true;
        }
        return &table;
    }
};
DRAM_ATTR ChangedPixelsTable<E0470_GL16_FRAMES> changed_gray16;
DRAM_ATTR ChangedPixelsTable<E0470_GRAY8_GL16_FRAMES> changed_gray8;

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

bool restore_sleep_io_locked(std::string& error) {
    const auto result = sleep_io.resume();
    if (result == ESP_OK) return true;
    std::string detail;
    failure(detail, "Sleep I/O resume", result);
    if (!error.empty()) error += "; ";
    error += detail;
    return false;
}

bool restore_sleep_io(std::string& error) {
    if (!sleep_io.pending()) return true;
    xSemaphoreTake(touch_lock, portMAX_DELAY);
    const bool restored = restore_sleep_io_locked(error);
    xSemaphoreGive(touch_lock);
    return restored;
}

bool suspend_sleep_io_locked(bool host_off, std::string& error) {
    if (!restore_sleep_io_locked(error)) return false;
    suppress_touch = true;
    xQueueReset(touch_events);
    touch_moves.clear();
    const auto result = sleep_io.suspend(touch, host_off);
    if (result == ESP_OK) return true;
    failure(error, "Sleep I/O suspend", result);
    restore_sleep_io_locked(error);
    return false;
}

void copy_portrait(const std::uint8_t* packed) {
    constexpr int source_stride = width / 2;
    constexpr int native_stride = height / 2;
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
    card_issue = mounted ? pico_page::CardIssue::Removed : !info.present ? pico_page::CardIssue::NotInserted :
        info.needs_format ? pico_page::CardIssue::NoFilesystem : pico_page::CardIssue::Unreadable;
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
            battery_state = {};
            return failure(error, "PMU status", result != ESP_OK ? result : ESP_ERR_INVALID_RESPONSE);
        }
        battery_state.valid = (snapshot->flags & PMU_STATUS_BATTERY_VALID) && snapshot->soc_permille <= 1000;
        battery_state.percent = snapshot->soc_permille / 10;
        battery_state.millivolts = snapshot->battery_mv;
        battery_state.charging = snapshot->charge_state == PMU_CHARGE_CHARGING;
        if (!snapshot->pending_events) return true;
        if (!snapshot->event_ok || snapshot->event.event_id == 0) {
            // id and type read as zero when the count or the event register could not be
            // read; a non-zero id with ok=0 is a CRC mismatch.
            char detail[96];
            std::snprintf(detail, sizeof(detail),
                          "PMU event unreadable (pending=%u ok=%d id=%u type=0x%02x flags=0x%04lx)",
                          static_cast<unsigned>(snapshot->pending_events), snapshot->event_ok ? 1 : 0,
                          static_cast<unsigned>(snapshot->event.event_id),
                          static_cast<unsigned>(snapshot->event.type),
                          static_cast<unsigned long>(snapshot->flags));
            return failure(error, detail, ESP_ERR_INVALID_RESPONSE);
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

bool wake_lines_idle(gpio_num_t irq, bool& idle, std::string& error) {
    idle = false;
    // The PMU holds CW_INT low until acknowledgement. A new event can leave it low
    // when the expander latch is cleared, so a released expander IRQ alone is insufficient.
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

bool settle_wake_interrupt(gpio_num_t irq, bool& wake_press, std::string& error) {
    wake_press = false;
    const auto deadline = esp_timer_get_time() + 2'000'000;
    for (;;) {
        bool short_press = false;
        bool press = false;
        bool idle = false;
        // A key event acknowledged before a later read failed is not reported again.
        bool read = pmu_events(short_press, press, error);
        wake_press = wake_press || press;
        // Releasing CW_INT on acknowledgement also changes the expander input.
        if (!wake_press) read = read && wake_lines_idle(irq, idle, error);
        if (wake_press || (read && idle)) {
            error.clear();
            return true;
        }
        if (!read) {
            if (sleep_report.first_pmu_error.empty()) sleep_report.first_pmu_error = error;
            ++sleep_report.pmu_retries;
        }
        if (esp_timer_get_time() >= deadline) {
            if (read) error = "Wake interrupt or PMU events did not settle without a power-key event";
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

void sample_touch(void*) {
    Event active{};
    bool move_pending = false;
    bool suppressed = true;
    bool failing = false;
    bool interrupt_seen = false;
    int fast = settle_samples;
    const auto emit = [&](EventPhase phase, std::int64_t now) -> bool {
        Event event = active;
        event.phase = phase;
        event.time_us = now;
        if (phase == EventPhase::Move && !touch_moves.publish(event)) return touch_moves.pending_for(event.sequence);
        if (xQueueSend(touch_events, &event, 0) == pdTRUE) return true;
        xQueueReset(touch_events);
        touch_moves.clear();
        const Event reset{EventKind::InputReset, EventPhase::Cancel, event.sequence, 0, 0, now};
        const auto sent = xQueueSend(touch_events, &reset, 0);
        configASSERT(sent == pdTRUE);
        active = {};
        suppressed = true;
        ESP_LOGW(tag, "Input queue overflow: count=%lu; contacts cancelled",
                 static_cast<unsigned long>(input_overflows.fetch_add(1, std::memory_order_relaxed) + 1));
        return false;
    };
    const auto cancel = [&](std::int64_t now) {
        if (active.kind != EventKind::None) emit(EventPhase::Cancel, now);
        active = {};
        move_pending = false;
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
            vTaskDelay(1);
        } else if (fast > 0) {
            --fast;
        }
        cst836u_touch_t sample{};
        xSemaphoreTake(touch_lock, portMAX_DELAY);
        if (sleep_io.pending()) {
            xSemaphoreGive(touch_lock);
            continue;
        }
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
                move_pending = false;
                emit(EventPhase::Begin, now);
            } else if (hit_test(sample.x, sample.y) != active.kind) {
                cancel(now);
            } else {
                const bool moved = active.x != sample.x || active.y != sample.y;
                active.x = sample.x;
                active.y = sample.y;
                active.min_x = std::min<int>(active.min_x, sample.x);
                active.min_y = std::min<int>(active.min_y, sample.y);
                active.max_x = std::max<int>(active.max_x, sample.x);
                active.max_y = std::max<int>(active.max_y, sample.y);
                move_pending = move_pending || moved;
                if (move_pending && active.kind == EventKind::Touch) move_pending = !emit(EventPhase::Move, now);
            }
        } else if (active.kind != EventKind::None) {
            emit(EventPhase::End, now);
            active = {};
        }
        xSemaphoreGive(touch_lock);
    }
}

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
}

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

    // On ESP32-S3, an 80 MHz CPU minimum keeps the APB clock at 80 MHz.
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
    rtc_clock::restore();
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
void set_page_waveform(PageWaveform waveform, bool reverse, PageTurnOrigin origin) {
    page_waveform = waveform;
    reverse_page_effect = reverse;
    page_turn_origin = origin;
}
PageWaveform page_effect_waveform() { return selected_effect; }
bool set_page_effect_waveform(PageWaveform waveform, std::string& error) {
    error.clear();
    if (!on_owner(error)) return false;
    if (waveform == PageWaveform::Ripple || waveform == PageWaveform::Diagonal || waveform == PageWaveform::TouchPosition) {
        if (!display_ready || !prepare_page_sweep(waveform)) {
            error = "Page sweep phase tables unavailable";
            return false;
        }
    } else if (waveform != PageWaveform::Gray16) {
        error = "Invalid page effect";
        return false;
    }
    if (waveform == PageWaveform::Gray16) release_page_sweep();
    selected_effect = waveform;
    ESP_LOGI(tag, "page_effect=%s", waveform == PageWaveform::TouchPosition ? "touch" :
             waveform == PageWaveform::Diagonal ? "diagonal" :
             waveform == PageWaveform::Ripple ? "ripple" : "default");
    return true;
}
PresentTiming present_timing() { return timing; }
const char* card_mount_path() { return "/sdcard"; }
const char* card_root() { return mounted ? card_mount_path() : nullptr; }
pico_page::CardIssue card_start_issue() { return card_issue; }
const std::string& card_error() { return mount_error; }
Battery battery() { return battery_state; }

bool finish_power(std::string& error) {
    error.clear();
    if (!on_owner(error) || !ready || usb_owns_card) return false;
    if (!restore_sleep_io(error)) return false;
    epd_poweroff(); rails_deadline = 0;
    if (mounted) {
        const auto result = read_pico_sd_release();
        if (result != ESP_OK) return failure(error, "SD release", result);
        mounted = false;
    }
    return true;
}
void persist_clock() {
    if (ready && owner == xTaskGetCurrentTaskHandle()) rtc_clock::persist();
}
std::int64_t take_clock_sync() { return rtc_clock::take_synchronized_at(); }

bool power_off(std::string& error) {
    error.clear();
    if (!on_owner(error) || !ready || usb_owns_card) return false;
    const auto* snapshot = read_pico_pmu_get();
    if (!snapshot->identity_ok || !(snapshot->caps & PMU_CAP_HOST_EN_CONTROL))
        return failure(error, "PMU power-off unavailable", ESP_ERR_NOT_SUPPORTED);
    xSemaphoreTake(touch_lock, portMAX_DELAY);
    if (!suspend_sleep_io_locked(true, error)) {
        xSemaphoreGive(touch_lock);
        return false;
    }
    const auto result = read_pico_pmu_power_off();
    if (result != ESP_OK) {
        failure(error, "PMU power-off", result);
        restore_sleep_io_locked(error);
    }
    xSemaphoreGive(touch_lock);
    return result == ESP_OK;
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
    // USB hosts display the FAT volume label independently of the USB product string.
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
             std::string& error, std::optional<RefreshRegion> region, bool changed_pixels_only,
             bool hold_unchanged_footer) {
    error.clear();
    if (!on_owner(error)) return false;
    if (!display_ready || packed == nullptr || size != framebuffer_bytes ||
        (region && !region->fits(width, height))) {
        return failure(error, "Panel frame", ESP_ERR_INVALID_ARG);
    }
    if (!restore_sleep_io(error)) {
        baseline_valid = false;
        return false;
    }
    const bool clear = full_clean || !baseline_valid;
    set_performance(true);
    const auto started = esp_timer_get_time();
    copy_portrait(packed);
    const bool unchanged = !clear && std::memcmp(display.front_fb, display.back_fb, framebuffer_bytes) == 0;
    const auto copied = esp_timer_get_time();
    timing = {copied - started, 0, 0};
    if (unchanged) return true;
    const EpdWaveform* pixel_table = nullptr;
    if (changed_pixels_only && !clear) {
        pixel_table = page_waveform == PageWaveform::Gray8 ? changed_gray8.prepare(E0470_GRAY8_WAVEFORM)
                                                         : changed_gray16.prepare(E0470_WAVEFORM);
        if (!pixel_table) {
            baseline_valid = false;
            return failure(error, "Changed-pixel GL16 unavailable", ESP_ERR_INVALID_STATE);
        }
    }
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
    baseline_valid = false;
    epd_set_leading_skip(!clear && (page_waveform == PageWaveform::Gray16 ||
                                  page_waveform == PageWaveform::Gray8));
    EpdDrawError draw_result;
    constexpr int footer_first_native_column = 1128;
    epd_set_unchanged_pixel_hold(hold_unchanged_footer && !clear ? footer_first_native_column : -1);
    // epd_hl_update_area applies EPD_ROT_INVERTED_PORTRAIT, matching copy_portrait.
    const auto update = [&] {
        if (region) return epd_hl_update_area(&display, MODE_GL16, status.temperature_c,
                                             EpdRect{region->x, region->y, region->width, region->height});
        return epd_hl_update_screen_full(&display, MODE_GL16, status.temperature_c);
    };
    if (clear) {
        draw_result = epd_clear_checked();
        if (draw_result == EPD_DRAW_SUCCESS)
            draw_result = epd_hl_update_screen_from_white(&display, MODE_GC16, status.temperature_c);
    } else if (pixel_table) {
        epd_hl_waveform(&display, pixel_table);
        draw_result = update();
        epd_hl_waveform(&display, &E0470_WAVEFORM);
    } else if (page_waveform == selected_effect && selected_effect != PageWaveform::Gray16) {
        draw_result = present_page_sweep(display, status.temperature_c, reverse_page_effect, page_turn_origin);
    } else if (page_waveform == PageWaveform::Gray8) {
        epd_hl_waveform(&display, &E0470_GRAY8_WAVEFORM);
        draw_result = update();
        epd_hl_waveform(&display, &E0470_WAVEFORM);
    } else {
        draw_result = update();
    }
    epd_set_unchanged_pixel_hold(-1);
    bool power_good = panel_status(status, error);
    if (draw_result == EPD_DRAW_EMPTY_LINE_QUEUE && power_good) {
        // The driver joins its producers and detaches scan callbacks before returning.
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

void guard_flash_io(bool begin) {
    if (begin) xSemaphoreTake(touch_lock, portMAX_DELAY);
    else xSemaphoreGive(touch_lock);
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
    struct InputGuard {
        InputGuard() { xSemaphoreTake(touch_lock, portMAX_DELAY); }
        ~InputGuard() { xSemaphoreGive(touch_lock); }
    } input_guard;
    if (!restore_sleep_io_locked(error)) return false;
    if (sleep_report.report_after_us && now >= sleep_report.report_after_us) {
        sleep_report.report_after_us = 0;
        ESP_LOGI(tag, "Light sleep: wakes=%d rejected=%d pmu_retries=%d touch_edges=%d touch_low=%d "
                 "first_pmu_error=%s result=%s",
                 sleep_report.wakes, sleep_report.rejected, sleep_report.pmu_retries,
                 sleep_report.touch_edges, sleep_report.touch_low,
                 sleep_report.first_pmu_error.empty() ? "none" : sleep_report.first_pmu_error.c_str(),
                 sleep_report.result.empty() ? "power key" : sleep_report.result.c_str());
    }
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
    if (xQueueReceive(touch_events, &event, 0) == pdTRUE) touch_moves.receive(event);
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

bool sleep(std::string& error, bool& entered_sleep) {
    entered_sleep = false;
    error.clear();
    if (!on_owner(error) || !ready) return false;
    if (usb_owns_card) {
        error = "USB storage must stop before sleep";
        return false;
    }
    if (!restore_sleep_io(error)) return false;
    epd_poweroff();
    rails_deadline = 0;
    sleep_report = {};
    xSemaphoreTake(touch_lock, portMAX_DELAY);
    if (!suspend_sleep_io_locked(false, error)) {
        xSemaphoreGive(touch_lock);
        return false;
    }

    const auto irq = static_cast<gpio_num_t>(READ_PICO_IOE_INT_GPIO);
    gpio_config_t config{};
    config.pin_bit_mask = 1ULL << READ_PICO_IOE_INT_GPIO;
    config.mode = GPIO_MODE_INPUT;
    config.pull_up_en = GPIO_PULLUP_ENABLE;
    auto result = gpio_config(&config);
    SleepUsbConsole usb_console;
    bool slept = false;
    const auto idle_deadline = esp_timer_get_time() + 2'000'000;
    while (result == ESP_OK && esp_timer_get_time() < idle_deadline) {
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
                if (has_pending_touch()) {
                    set_touch_pending(false);
                    ++sleep_report.touch_edges;
                }
                if (cst836u_int_asserted(touch)) ++sleep_report.touch_low;
                result = usb_console.enter_light_sleep();
                entered_sleep = usb_console.entered_sleep();
                // The request is rejected when the wake line is already low. The pending
                // event is then handled as a wake; the delay bounds the rate of repeats.
                if (result == ESP_ERR_SLEEP_REJECT) {
                    result = ESP_OK;
                    ++sleep_report.rejected;
                    vTaskDelay(pdMS_TO_TICKS(20));
                } else if (result == ESP_OK) {
                    ++sleep_report.wakes;
                }
                if (result != ESP_OK) break;
                if (!settle_wake_interrupt(irq, wake_press, error)) break;
                if (wake_press) {
                    slept = entered_sleep;
                    if (!slept) {
                        power_key.cancel_sleep_press();
                        error = "Sleep cancelled by power key";
                    }
                    break;
                }
            }
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    gpio_wakeup_disable(irq);
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_GPIO);
    if (result != ESP_OK) failure(error, "Light sleep", result);
    const bool restored = restore_sleep_io_locked(error);
    if (entered_sleep) {
        suppress_touch = true;
        xQueueReset(touch_events);
        touch_moves.clear();
    }
    xSemaphoreGive(touch_lock);
    suppress_power = entered_sleep;
    if (entered_sleep) power_ignore_until = esp_timer_get_time() + 300'000;
    usb_console.resume();
    if (!slept && error.empty()) error = "Power key or wake interrupt did not become idle";
    if (entered_sleep) {
        sleep_report.result = error;
        sleep_report.report_after_us = esp_timer_get_time() + sleep_report_delay_us;
    }
    return slept && restored;
}

}
