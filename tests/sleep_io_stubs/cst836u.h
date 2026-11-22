#pragma once
#include "esp_err.h"
using cst836u_handle_t = void*;
constexpr int CST836U_MODE_DEEPSLEEP = 1;
esp_err_t cst836u_set_mode(cst836u_handle_t, int);
esp_err_t cst836u_wake(cst836u_handle_t);
