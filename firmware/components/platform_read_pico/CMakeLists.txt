idf_component_register(
    SRCS "rtc_clock.cpp" "sleep_io.cpp" "sleep_usb_console.cpp" "ripple.cpp" "changed_pixels_waveform.cpp" "platform.cpp" "network.cpp" "portal_server.cpp" "usb_storage.cpp"
    INCLUDE_DIRS "include"
    REQUIRES read_pico reader_core
    PRIV_REQUIRES reader_sync reader_store espressif__esp_tinyusb esp_driver_sdmmc esp_hal_usb esp_timer esp_pm esp_hw_support freertos esp_wifi esp_event esp_netif esp_http_client esp_http_server lwip mbedtls app_update esp_app_format bootloader_support
)
target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
