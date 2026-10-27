/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * Adapted from Read Pico board sleep handling at a2a63a6a2c606540be72156a6567392d948d65fc.
 * Pico Page preserves runtime register snapshots and retries incomplete restoration.
 */
#include "sleep_io.h"
#include "driver/gpio.h"
#include "read_pico_board.h"
#include "read_pico_sd.h"

namespace pico_page::platform {
namespace {
constexpr std::uint8_t panel_mode = 1U << 0;
constexpr std::uint8_t pmu_interrupt = 1U << 2;
constexpr std::uint8_t input_pins = (1U << 2) | (1U << 5) | (1U << 6);
constexpr std::uint8_t panel_power = (1U << 3) | (1U << 4);

esp_err_t configure_sleep_pins() {
    // Read Pico panel clock/control/data and TF CLK. Only sleep registers change;
    // active SDMMC signals remain under the driver's ownership.
    constexpr gpio_num_t pins[] = {
        GPIO_NUM_21, GPIO_NUM_48, GPIO_NUM_3, GPIO_NUM_46, GPIO_NUM_47,
        GPIO_NUM_4, GPIO_NUM_5, GPIO_NUM_6, GPIO_NUM_7, GPIO_NUM_8,
        GPIO_NUM_9, GPIO_NUM_10, GPIO_NUM_11, GPIO_NUM_12, GPIO_NUM_13,
        GPIO_NUM_14, GPIO_NUM_15, GPIO_NUM_16, GPIO_NUM_17, GPIO_NUM_18,
        GPIO_NUM_45, GPIO_NUM_38,
    };
    for (const auto pin : pins) {
        auto result = gpio_sleep_set_direction(pin, GPIO_MODE_INPUT);
        if (result == ESP_OK) result = gpio_sleep_set_pull_mode(pin, GPIO_PULLDOWN_ONLY);
        if (result == ESP_OK) result = gpio_sleep_sel_en(pin);
        if (result != ESP_OK) return result;
    }
    return ESP_OK;
}

// The FCA9555 register pointer only alternates within a register pair during a burst read, so
// a burst from IN0 returns the inputs in place of OUT/CFG. Each register is read on its own.
esp_err_t read_register_pair(fca9555_handle_t ioe, std::uint8_t first, std::uint16_t& value) {
    std::uint8_t low = 0, high = 0;
    auto result = fca9555_read_reg(ioe, first, &low);
    if (result == ESP_OK) result = fca9555_read_reg(ioe, static_cast<std::uint8_t>(first + 1), &high);
    if (result == ESP_OK) value = static_cast<std::uint16_t>(low | (high << 8));
    return result;
}
}

esp_err_t SleepIo::suspend(cst836u_handle_t touch, bool host_off) {
    if (pending() || !touch || read_pico_rails_on()) return ESP_ERR_INVALID_STATE;
    if (!host_off && read_pico_sd_busy()) return ESP_ERR_NOT_FINISHED;
    const auto ioe = read_pico_fca9555();
    if (!ioe) return ESP_ERR_INVALID_STATE;
    std::uint16_t running_output = 0, running_config = 0;
    auto result = read_register_pair(ioe, FCA9555_REG_OUT0, running_output);
    if (result == ESP_OK) result = read_register_pair(ioe, FCA9555_REG_CFG0, running_config);
    if (result != ESP_OK) return result;
    if ((running_config & input_pins) != input_pins) return ESP_ERR_INVALID_STATE;
    if (running_output & panel_power) return ESP_ERR_INVALID_STATE;
    if (!host_off) {
        result = configure_sleep_pins();
        if (result != ESP_OK) return result;
    }
    output_ = running_output;
    config_ = running_config;
    ioe_pending_ = true;
    // A failed command can still have reached the controller; reset it on cleanup.
    touch_ = touch;
    result = cst836u_set_mode(touch_, CST836U_MODE_DEEPSLEEP);
    if (result != ESP_OK) return result;
    // Preload the output latch before changing Port1 from input to output.
    result = fca9555_set_output(ioe, 1, 0xff);
    if (result == ESP_OK) result = fca9555_set_config(ioe, 1, 0x00);
    auto output = static_cast<std::uint8_t>(output_) & ~panel_mode;
    if (host_off) output &= ~pmu_interrupt;
    if (result == ESP_OK) result = fca9555_set_output(ioe, 0, output);
    // CW32 is open-drain. Output-low is reserved for terminal host power-off;
    // light sleep must keep this input so PMU events can wake GPIO41.
    if (result == ESP_OK && host_off)
        result = fca9555_set_config(ioe, 0, static_cast<std::uint8_t>(config_) & ~pmu_interrupt);
    return result;
}

esp_err_t SleepIo::resume() {
    if (!pending()) return ESP_OK;
    const auto ioe = read_pico_fca9555();
    if (!ioe) return ESP_ERR_INVALID_STATE;
    if (ioe_pending_) {
        // Release CW_INT before restoring its output latch: never drive the PMU line high.
        auto result = fca9555_set_config(ioe, 0, static_cast<std::uint8_t>(config_));
        if (result == ESP_OK) result = fca9555_set_output(ioe, 0, static_cast<std::uint8_t>(output_));
        // Restore Port1 directions before its latch, avoiding an output-low transient.
        const auto direction = fca9555_set_config(ioe, 1, static_cast<std::uint8_t>(config_ >> 8));
        const auto latch = direction == ESP_OK
            ? fca9555_set_output(ioe, 1, static_cast<std::uint8_t>(output_ >> 8)) : direction;
        if (result == ESP_OK) result = latch;
        if (result != ESP_OK) return result;
        ioe_pending_ = false;
    }
    // Touch reset uses the BSP output cache, so it must follow register restoration.
    if (touch_) {
        const auto result = cst836u_wake(touch_);
        if (result != ESP_OK) return result;
        touch_ = nullptr;
    }
    return ESP_OK;
}

} // namespace pico_page::platform
