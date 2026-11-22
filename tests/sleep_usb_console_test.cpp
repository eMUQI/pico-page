#include "sleep_usb_console.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "freertos/task.h"
#include "hal/usb_serial_jtag_ll.h"
#include <cassert>
#include <initializer_list>

using pico_page::platform::SleepUsbConsole;
UsbSerialJtagRegisters USB_SERIAL_JTAG{};
namespace {
std::int64_t now_us = 0, sleep_duration_us = 0;
std::int64_t first_delay_shortfall_us = 0;
esp_err_t sleep_result = ESP_OK;
int writes = 0, pad_enables = 0;
TickType_t waited_ms = 0;
void reset(std::uint32_t config) {
    USB_SERIAL_JTAG.conf0.val = config;
    now_us = sleep_duration_us = 0;
    first_delay_shortfall_us = 0;
    sleep_result = ESP_OK;
    writes = pad_enables = 0;
    waited_ms = 0;
}
}
std::int64_t esp_timer_get_time() { return now_us; }
bool usb_serial_jtag_ll_phy_is_pad_enabled() { return USB_SERIAL_JTAG.conf0.usb_pad_enable; }
void usb_serial_jtag_ll_phy_enable_pad(bool enable) {
    if (enable && !USB_SERIAL_JTAG.conf0.usb_pad_enable) ++pad_enables;
    USB_SERIAL_JTAG.conf0.usb_pad_enable = enable;
    ++writes;
}
void usb_serial_jtag_ll_phy_enable_pull_override(const usb_serial_jtag_pull_override_vals_t* values) {
    USB_SERIAL_JTAG.conf0.dp_pullup = values->dp_pu;
    USB_SERIAL_JTAG.conf0.dm_pullup = values->dm_pu;
    USB_SERIAL_JTAG.conf0.dp_pulldown = values->dp_pd;
    USB_SERIAL_JTAG.conf0.dm_pulldown = values->dm_pd;
    USB_SERIAL_JTAG.conf0.pad_pull_override = 1;
    ++writes;
}
void usb_serial_jtag_ll_phy_disable_pull_override() {
    USB_SERIAL_JTAG.conf0.pad_pull_override = 0;
    ++writes;
}
void vTaskDelay(TickType_t ticks) {
    assert(!usb_serial_jtag_ll_phy_is_pad_enabled());
    waited_ms += ticks;
    assert(first_delay_shortfall_us < ticks * 1000LL);
    now_us += ticks * 1000LL - first_delay_shortfall_us;
    first_delay_shortfall_us = 0;
}
esp_err_t esp_light_sleep_start() {
    // ESP-IDF disables the pad for sleep and restores the state seen on entry.
    const bool saved_pad = usb_serial_jtag_ll_phy_is_pad_enabled();
    assert(!saved_pad);
    usb_serial_jtag_ll_phy_enable_pad(false);
    now_us += sleep_duration_us;
    usb_serial_jtag_ll_phy_enable_pad(saved_pad);
    return sleep_result;
}

int main() {
    for (std::uint32_t bits = 0; bits < 64; ++bits) {
        const auto original = 0xa5000000U | bits;
        const bool enabled = bits & 32;
        reset(original);
        SleepUsbConsole cancelled;
        cancelled.resume();
        assert(writes == 0 && !cancelled.entered_sleep());

        for (const auto result : {ESP_ERR_SLEEP_REJECT, ESP_ERR_INVALID_STATE}) {
            reset(original);
            SleepUsbConsole rejected;
            sleep_result = result;
            assert(rejected.enter_light_sleep() == result && !rejected.entered_sleep());
            rejected.resume();
            assert(waited_ms == 0 && USB_SERIAL_JTAG.conf0.val == original);
        }

        reset(original);
        SleepUsbConsole sleep;
        sleep_result = ESP_ERR_SLEEP_REJECT;
        assert(sleep.enter_light_sleep() == ESP_ERR_SLEEP_REJECT && !sleep.entered_sleep());
        sleep_result = ESP_OK;
        sleep_duration_us = 5'000;
        assert(sleep.enter_light_sleep() == ESP_OK && sleep.entered_sleep());
        assert(pad_enables == 0);
        assert(sleep.enter_light_sleep() == ESP_OK);
        sleep_result = ESP_ERR_SLEEP_REJECT;
        sleep_duration_us = 0;
        assert(sleep.enter_light_sleep() == ESP_ERR_SLEEP_REJECT && sleep.entered_sleep());
        assert(pad_enables == 0);
        sleep.resume();
        assert(USB_SERIAL_JTAG.conf0.val == original && pad_enables == int(enabled));
        assert(waited_ms == (enabled ? 90U : 0U));
        const auto restored_writes = writes;
        sleep.resume();
        assert(writes == restored_writes);

        reset(original);
        SleepUsbConsole long_sleep;
        sleep_duration_us = 30'000'000;
        assert(long_sleep.enter_light_sleep() == ESP_OK);
        sleep_result = ESP_ERR_INVALID_STATE;
        sleep_duration_us = 0;
        assert(long_sleep.enter_light_sleep() == ESP_ERR_INVALID_STATE && long_sleep.entered_sleep());
        long_sleep.resume();
        assert(waited_ms == 0 && USB_SERIAL_JTAG.conf0.val == original);
    }
    // FreeRTOS delays count tick boundaries; a near-boundary first tick can shorten the wait.
    reset(32);
    SleepUsbConsole short_sleep;
    sleep_duration_us = 1;
    assert(short_sleep.enter_light_sleep() == ESP_OK);
    first_delay_shortfall_us = 999;
    short_sleep.resume();
    assert(now_us >= 100'000 && waited_ms == 101 && USB_SERIAL_JTAG.conf0.val == 32);
}
