#pragma once
#include <cstdint>
#include "esp_err.h"
using fca9555_handle_t = void*;
struct read_pico_status_t { std::uint16_t ioe_output, ioe_config; };
bool read_pico_rails_on();
fca9555_handle_t read_pico_fca9555();
esp_err_t read_pico_get_ioe_status_full(read_pico_status_t*);
esp_err_t fca9555_set_output(fca9555_handle_t, int, std::uint8_t);
esp_err_t fca9555_set_config(fca9555_handle_t, int, std::uint8_t);
