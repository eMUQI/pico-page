#pragma once
template<class... Args> void test_esp_log(Args...) {}
#define ESP_LOGI(...) test_esp_log(__VA_ARGS__)
#define ESP_LOGW(...) test_esp_log(__VA_ARGS__)
