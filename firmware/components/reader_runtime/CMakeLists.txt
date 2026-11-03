idf_component_register(SRCS runtime.cpp display_compatibility.cpp INCLUDE_DIRS include
    PRIV_REQUIRES reader_app reader_graphics reader_console reader_input platform_contract
        esp_hw_support esp_timer esp_system heap log freertos)
target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
