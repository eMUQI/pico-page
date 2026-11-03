idf_component_register(SRCS "console.cpp" INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "../reader_store"
    REQUIRES reader_app
    PRIV_REQUIRES reader_store platform_contract esp_timer esp_rom heap freertos esp_driver_usb_serial_jtag)
target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
