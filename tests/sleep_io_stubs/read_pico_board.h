#pragma once
#include <cstdint>
#include "esp_err.h"
using fca9555_handle_t = void*;
#define FCA9555_REG_OUT0 2
#define FCA9555_REG_CFG0 6
bool read_pico_rails_on();
fca9555_handle_t read_pico_fca9555();
esp_err_t fca9555_read_reg(fca9555_handle_t, std::uint8_t, std::uint8_t*);
esp_err_t fca9555_set_output(fca9555_handle_t, int, std::uint8_t);
esp_err_t fca9555_set_config(fca9555_handle_t, int, std::uint8_t);
