#pragma once
void rtc_test_log(const char*, const char*);
#define ESP_LOGW(tag, message) rtc_test_log(tag, message)
#define ESP_LOGI(tag, message) rtc_test_log(tag, message)
