#include "sleep_usb_console.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hal/usb_serial_jtag_ll.h"

namespace pico_page::platform {

esp_err_t SleepUsbConsole::enter_light_sleep() {
    if (!pending_ && usb_serial_jtag_ll_phy_is_pad_enabled()) {
        original_config_ = USB_SERIAL_JTAG.conf0.val;
        pending_ = true;
        const usb_serial_jtag_pull_override_vals_t disconnected{};
        usb_serial_jtag_ll_phy_enable_pull_override(&disconnected);
        // ESP-IDF snapshots this disabled pad state, so periodic PMU wakeups cannot reattach USB.
        usb_serial_jtag_ll_phy_enable_pad(false);
        disconnected_at_us_ = esp_timer_get_time();
    }
    const auto result = esp_light_sleep_start();
    if (result == ESP_OK) entered_sleep_ = true;
    return result;
}

void SleepUsbConsole::resume() {
    if (!pending_) return;
    constexpr std::int64_t minimum_disconnect_us = 100'000;
    auto remaining_us = minimum_disconnect_us - (esp_timer_get_time() - disconnected_at_us_);
    while (entered_sleep_ && remaining_us > 0) {
        const auto ticks = pdMS_TO_TICKS((remaining_us + 999) / 1000);
        vTaskDelay(ticks ? ticks : 1);
        remaining_us = minimum_disconnect_us - (esp_timer_get_time() - disconnected_at_us_);
    }
    decltype(USB_SERIAL_JTAG.conf0) config{};
    config.val = original_config_;
    const usb_serial_jtag_pull_override_vals_t pulls{
        bool(config.dp_pullup), bool(config.dm_pullup),
        bool(config.dp_pulldown), bool(config.dm_pulldown)};
    usb_serial_jtag_ll_phy_enable_pull_override(&pulls);
    if (!config.pad_pull_override) usb_serial_jtag_ll_phy_disable_pull_override();
    usb_serial_jtag_ll_phy_enable_pad(config.usb_pad_enable);
    pending_ = false;
}

} // namespace pico_page::platform
