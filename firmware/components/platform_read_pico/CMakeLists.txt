idf_component_register(
    SRCS "platform.cpp" "network.cpp"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "../reader_store" "../reader_store/third_party/tomlc17"
    REQUIRES read_pico
    PRIV_REQUIRES esp_timer esp_pm esp_hw_support freertos reader_store esp_wifi esp_event esp_netif esp_http_client mbedtls
)
target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
