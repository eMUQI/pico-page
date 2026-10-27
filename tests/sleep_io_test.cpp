#include "sleep_io.h"
#include "read_pico_board.h"
#include "driver/gpio.h"
#include <array>
#include <cassert>
#include <set>
#include <string>
#include <vector>

using pico_page::platform::SleepIo;
namespace {
constexpr int injected_error = 7;
void* const device = reinterpret_cast<void*>(1);
constexpr std::uint16_t original_output = 0x5aa5, original_config = 0xff64;
std::uint16_t output = original_output, config = original_config;
bool rails_on = false, missing = false, asleep = false, apply_on_error = false;
bool sd_busy = false;
int fail_at = -1, calls = 0;
std::vector<std::string> operations;
std::array<std::set<int>, 3> gpio_pins;
int step(const std::string& name) {
    operations.push_back(name);
    return calls++ == fail_at ? injected_error : ESP_OK;
}
void reset() {
    output = original_output; config = original_config;
    rails_on = missing = asleep = apply_on_error = sd_busy = false;
    fail_at = -1; calls = 0; operations.clear();
    for (auto& pins : gpio_pins) pins.clear();
}
void restored(SleepIo& io) {
    fail_at = -1;
    assert(io.resume() == ESP_OK && !io.pending());
    assert(output == original_output && config == original_config && !asleep);
    const auto count = calls;
    assert(io.resume() == ESP_OK && calls == count);
}
int gpio_step(int phase, int pin) {
    // Neither PMU wake GPIO41, touch IRQ43, nor the I2C bus may be pulled down.
    assert(pin != 41 && pin != 43 && pin != 39 && pin != 40);
    gpio_pins[phase].insert(pin);
    return step("gpio" + std::to_string(phase));
}
}
bool read_pico_rails_on() { return rails_on; }
bool read_pico_sd_busy() { return sd_busy; }
fca9555_handle_t read_pico_fca9555() { return missing ? nullptr : device; }
// Single-register reads only: a burst from IN0 on the real expander repeats the input pair.
esp_err_t fca9555_read_reg(fca9555_handle_t handle, std::uint8_t reg, std::uint8_t* value) {
    assert(handle == device && reg >= FCA9555_REG_OUT0 && reg <= FCA9555_REG_CFG0 + 1 && reg != 4 && reg != 5);
    const auto result = step("read" + std::to_string(reg));
    const auto pair = reg >= FCA9555_REG_CFG0 ? config : output;
    if (result == ESP_OK) *value = static_cast<std::uint8_t>(pair >> ((reg & 1) * 8));
    return result;
}
esp_err_t fca9555_set_output(fca9555_handle_t handle, int port, std::uint8_t value) {
    assert(handle == device && (port == 0 || port == 1));
    // A high CW_INT output would contend with the PMU's open-drain interrupt.
    if (port == 0 && !(config & 4)) assert(!(value & 4));
    const auto result = step("output" + std::to_string(port));
    if (result == ESP_OK || apply_on_error) {
        const auto shift = port * 8;
        output = (output & ~(0xff << shift)) | (value << shift);
    }
    return result;
}
esp_err_t fca9555_set_config(fca9555_handle_t handle, int port, std::uint8_t value) {
    assert(handle == device && (port == 0 || port == 1));
    if (port == 0) {
        assert((value & 0x60) == 0x60); // PGOOD and card detect stay inputs.
        if (!(value & 4)) assert(!(output & 4));
    }
    const auto result = step("config" + std::to_string(port));
    if (result == ESP_OK || apply_on_error) {
        const auto shift = port * 8;
        config = (config & ~(0xff << shift)) | (value << shift);
    }
    return result;
}
esp_err_t cst836u_set_mode(cst836u_handle_t handle, int mode) {
    assert(handle == device && mode == CST836U_MODE_DEEPSLEEP);
    const auto result = step("touch_sleep");
    if (result == ESP_OK || apply_on_error) asleep = true;
    return result;
}
esp_err_t cst836u_wake(cst836u_handle_t handle) {
    // The BSP touch reset modifies Port0 using its runtime output cache.
    assert(handle == device && (output & ~0x80) == (original_output & ~0x80) && config == original_config);
    const auto result = step("touch_wake");
    if (result == ESP_OK) { asleep = false; output = original_output; }
    else if (apply_on_error) output &= ~0x80; // Failed reset may leave TP_RST asserted.
    return result;
}
esp_err_t gpio_sleep_set_direction(gpio_num_t pin, int value) {
    assert(value == GPIO_MODE_INPUT); return gpio_step(0, pin);
}
esp_err_t gpio_sleep_set_pull_mode(gpio_num_t pin, int value) {
    assert(value == GPIO_PULLDOWN_ONLY); return gpio_step(1, pin);
}
esp_err_t gpio_sleep_sel_en(gpio_num_t pin) { return gpio_step(2, pin); }

int main() {
    const std::set<int> expected_pins{3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,21,38,45,46,47,48};
    for (const bool host_off : {false, true}) {
        reset(); SleepIo io;
        assert(io.resume() == ESP_OK && calls == 0);
        assert(io.suspend(device, host_off) == ESP_OK && io.pending());
        assert(asleep && !(output & 1) && (output >> 8) == 0xff && (config >> 8) == 0);
        assert((config & 4) == (host_off ? 0 : 4));
        if (host_off) assert(gpio_pins[0].empty());
        else for (const auto& pins : gpio_pins) assert(pins == expected_pins);
        const auto suspend_calls = calls;
        assert(io.suspend(device, !host_off) == ESP_ERR_INVALID_STATE && calls == suspend_calls);
        restored(io);
        // Repeat with the same instance after wake, and after an aborted attempt.
        assert(io.suspend(device, host_off) == ESP_OK); restored(io);

        // Fail every suspension step, both before and after a write takes effect.
        for (int failure = 0; failure < suspend_calls; ++failure) {
            for (const bool applied : {false, true}) {
                reset(); SleepIo failing;
                fail_at = failure; apply_on_error = applied;
                assert(failing.suspend(device, host_off) == injected_error);
                restored(failing);
                assert(failing.suspend(device, host_off) == ESP_OK); restored(failing);
            }
        }
        // Fail each restore write and the touch reset. A retry must use the saved
        // running snapshot rather than taking the parked registers as its baseline.
        for (int failure = 0; failure < 5; ++failure) {
            for (const bool applied : {false, true}) {
                reset(); SleepIo failing;
                assert(failing.suspend(device, host_off) == ESP_OK);
                fail_at = calls + failure; apply_on_error = applied;
                assert(failing.resume() == injected_error && failing.pending());
                if (failure < 4) assert(operations.back() != "touch_wake");
                restored(failing);
            }
        }
    }
    reset(); SleepIo io;
    sd_busy = true;
    assert(io.suspend(device, false) == ESP_ERR_NOT_FINISHED && calls == 0 && !io.pending());
    // Terminal power-off retains the existing policy after a TF release failure;
    // unlike light sleep, it does not switch TF CLK to its sleep configuration.
    assert(io.suspend(device, true) == ESP_OK && gpio_pins[0].empty()); restored(io);
    reset();
    rails_on = true;
    assert(io.suspend(device, false) == ESP_ERR_INVALID_STATE && calls == 0);
    rails_on = false; missing = true;
    assert(io.suspend(device, false) == ESP_ERR_INVALID_STATE && calls == 0);
    missing = false;
    assert(io.suspend(nullptr, false) == ESP_ERR_INVALID_STATE && calls == 0);
    config &= ~4;
    assert(io.suspend(device, false) == ESP_ERR_INVALID_STATE && !io.pending());
    config = original_config; output |= 8;
    assert(io.suspend(device, false) == ESP_ERR_INVALID_STATE && !io.pending());
    reset(); assert(io.suspend(device, false) == ESP_OK);
    missing = true; assert(io.resume() == ESP_ERR_INVALID_STATE && io.pending());
    missing = false; restored(io);
}
