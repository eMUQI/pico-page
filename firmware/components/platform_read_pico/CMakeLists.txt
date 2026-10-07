idf_component_register(
    SRCS "platform.cpp" "network.cpp" "portal_server.cpp" "usb_storage.cpp"
    INCLUDE_DIRS "include"
    REQUIRES read_pico
    PRIV_REQUIRES reader_store espressif__esp_tinyusb esp_driver_sdmmc esp_timer esp_pm esp_hw_support freertos esp_wifi esp_event esp_netif esp_http_client esp_http_server lwip mbedtls
)
target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
