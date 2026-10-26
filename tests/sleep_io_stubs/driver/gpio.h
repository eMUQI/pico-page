#pragma once
#include "esp_err.h"
using gpio_num_t = int;
constexpr int GPIO_MODE_INPUT = 1, GPIO_PULLDOWN_ONLY = 2;
constexpr int GPIO_NUM_3 = 3;
constexpr int GPIO_NUM_4 = 4;
constexpr int GPIO_NUM_5 = 5;
constexpr int GPIO_NUM_6 = 6;
constexpr int GPIO_NUM_7 = 7;
constexpr int GPIO_NUM_8 = 8;
constexpr int GPIO_NUM_9 = 9;
constexpr int GPIO_NUM_10 = 10;
constexpr int GPIO_NUM_11 = 11;
constexpr int GPIO_NUM_12 = 12;
constexpr int GPIO_NUM_13 = 13;
constexpr int GPIO_NUM_14 = 14;
constexpr int GPIO_NUM_15 = 15;
constexpr int GPIO_NUM_16 = 16;
constexpr int GPIO_NUM_17 = 17;
constexpr int GPIO_NUM_18 = 18;
constexpr int GPIO_NUM_21 = 21;
constexpr int GPIO_NUM_38 = 38;
constexpr int GPIO_NUM_45 = 45;
constexpr int GPIO_NUM_46 = 46;
constexpr int GPIO_NUM_47 = 47;
constexpr int GPIO_NUM_48 = 48;
esp_err_t gpio_sleep_set_direction(gpio_num_t, int);
esp_err_t gpio_sleep_set_pull_mode(gpio_num_t, int);
esp_err_t gpio_sleep_sel_en(gpio_num_t);
