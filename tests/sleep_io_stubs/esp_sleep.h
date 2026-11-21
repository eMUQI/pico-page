#pragma once
#include "esp_err.h"
constexpr esp_err_t ESP_ERR_SLEEP_REJECT = 3;
esp_err_t esp_light_sleep_start();
