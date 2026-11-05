include("${CMAKE_CURRENT_LIST_DIR}/profile.cmake")
set(COMPONENTS main platform_read_pico)
set(IDF_TARGET esp32s3)
if(NOT SDKCONFIG_DEFAULTS AND "$ENV{SDKCONFIG_DEFAULTS}" STREQUAL "")
    set(SDKCONFIG_DEFAULTS "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults")
endif()
file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/bsp-revision.txt" READ_PICO_REVISION)
set(READ_PICO_SOURCE "${CMAKE_CURRENT_LIST_DIR}/../../.local/read_pico_firmware")
execute_process(COMMAND git -C "${READ_PICO_SOURCE}" rev-parse HEAD
    OUTPUT_VARIABLE read_pico_head OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET RESULT_VARIABLE read_pico_result)
if(NOT read_pico_result EQUAL 0 OR NOT read_pico_head STREQUAL READ_PICO_REVISION)
    message(FATAL_ERROR "Run tools/setup_bsp.sh to obtain the pinned Read Pico BSP.")
endif()
execute_process(COMMAND git -C "${READ_PICO_SOURCE}" diff --quiet HEAD -- components
    RESULT_VARIABLE read_pico_modified)
if(NOT read_pico_modified EQUAL 0)
    message(FATAL_ERROR "The pinned Read Pico components contain local modifications.")
endif()
execute_process(COMMAND git -C "${READ_PICO_SOURCE}" ls-files --others --exclude-standard -- components
    OUTPUT_VARIABLE read_pico_untracked OUTPUT_STRIP_TRAILING_WHITESPACE)
if(read_pico_untracked)
    message(FATAL_ERROR "The pinned Read Pico components contain untracked files.")
endif()

set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_LIST_DIR}/bsp-revision.txt"
    "${CMAKE_CURRENT_LIST_DIR}/patches/read-pico-sd-release.patch"
    "${CMAKE_CURRENT_LIST_DIR}/patches/epdiy-underrun-diagnostics.patch"
    "${CMAKE_CURRENT_LIST_DIR}/patches/epdiy-row-phases.patch"
    "${CMAKE_CURRENT_LIST_DIR}/patches/epdiy-feed-start.patch"
    "${CMAKE_CURRENT_LIST_DIR}/patches/epdiy-footer-hold.patch"
    "${CMAKE_CURRENT_LIST_DIR}/patches/epdiy-unchanged-pixels.h"
    "${CMAKE_CURRENT_LIST_DIR}/patches/read-pico-rtc-response.patch"
    "${CMAKE_CURRENT_LIST_DIR}/patches/read-pico-sd-busy.patch")
set(READ_PICO_ADAPTED "${CMAKE_CURRENT_BINARY_DIR}/pico_bsp")
file(REMOVE_RECURSE "${READ_PICO_ADAPTED}/read_pico")
file(MAKE_DIRECTORY "${READ_PICO_ADAPTED}")
file(COPY "${READ_PICO_SOURCE}/components/read_pico" DESTINATION "${READ_PICO_ADAPTED}")
execute_process(COMMAND git apply --unsafe-paths
    "${CMAKE_CURRENT_LIST_DIR}/patches/read-pico-sd-release.patch"
    WORKING_DIRECTORY "${READ_PICO_ADAPTED}/read_pico" RESULT_VARIABLE patch_result)
if(NOT patch_result EQUAL 0)
    message(FATAL_ERROR "Cannot apply checked SD ownership patch")
endif()
execute_process(COMMAND git apply --unsafe-paths
    "${CMAKE_CURRENT_LIST_DIR}/patches/read-pico-sd-busy.patch"
    WORKING_DIRECTORY "${READ_PICO_ADAPTED}/read_pico" RESULT_VARIABLE sd_busy_patch_result)
if(NOT sd_busy_patch_result EQUAL 0)
    message(FATAL_ERROR "Cannot apply SD busy-state patch")
endif()
list(APPEND EXTRA_COMPONENT_DIRS "${READ_PICO_ADAPTED}/read_pico")
file(REMOVE_RECURSE "${READ_PICO_ADAPTED}/epdiy")
file(COPY "${READ_PICO_SOURCE}/components/epdiy" DESTINATION "${READ_PICO_ADAPTED}")
execute_process(COMMAND git apply --unsafe-paths
    "${CMAKE_CURRENT_LIST_DIR}/patches/epdiy-underrun-diagnostics.patch"
    WORKING_DIRECTORY "${READ_PICO_ADAPTED}/epdiy" RESULT_VARIABLE epdiy_patch_result)
if(NOT epdiy_patch_result EQUAL 0)
    message(FATAL_ERROR "Cannot apply EPD underrun diagnostics patch")
endif()
execute_process(COMMAND git apply --unsafe-paths
    "${CMAKE_CURRENT_LIST_DIR}/patches/epdiy-row-phases.patch"
    WORKING_DIRECTORY "${READ_PICO_ADAPTED}/epdiy" RESULT_VARIABLE row_patch_result)
if(NOT row_patch_result EQUAL 0)
    message(FATAL_ERROR "Cannot apply EPD row-phase patch")
endif()
execute_process(COMMAND git apply --unsafe-paths
    "${CMAKE_CURRENT_LIST_DIR}/patches/epdiy-feed-start.patch"
    WORKING_DIRECTORY "${READ_PICO_ADAPTED}/epdiy" RESULT_VARIABLE feed_start_patch_result)
if(NOT feed_start_patch_result EQUAL 0)
    message(FATAL_ERROR "Cannot apply EPD feeder start barrier patch")
endif()
list(APPEND EXTRA_COMPONENT_DIRS "${READ_PICO_ADAPTED}/epdiy")
file(COPY "${CMAKE_CURRENT_LIST_DIR}/patches/epdiy-unchanged-pixels.h"
    DESTINATION "${READ_PICO_ADAPTED}/epdiy/src/output_common")
execute_process(COMMAND git apply --unsafe-paths
    "${CMAKE_CURRENT_LIST_DIR}/patches/epdiy-footer-hold.patch"
    WORKING_DIRECTORY "${READ_PICO_ADAPTED}/epdiy" RESULT_VARIABLE footer_patch_result)
if(NOT footer_patch_result EQUAL 0)
    message(FATAL_ERROR "Cannot apply EPD footer hold patch")
endif()
file(REMOVE_RECURSE "${READ_PICO_ADAPTED}/read_pico_pmu")
file(COPY "${READ_PICO_SOURCE}/components/read_pico_pmu" DESTINATION "${READ_PICO_ADAPTED}")
execute_process(COMMAND git apply --unsafe-paths
    "${CMAKE_CURRENT_LIST_DIR}/patches/read-pico-rtc-response.patch"
    WORKING_DIRECTORY "${READ_PICO_ADAPTED}/read_pico_pmu" RESULT_VARIABLE rtc_patch_result)
if(NOT rtc_patch_result EQUAL 0)
    message(FATAL_ERROR "Cannot apply RTC response validation patch")
endif()
list(APPEND EXTRA_COMPONENT_DIRS "${READ_PICO_ADAPTED}/read_pico_pmu")
foreach(component e0470_epaper_waveform
                  cst836u sc7a20h fca9555 sy7636a pwm_audio)
    list(APPEND EXTRA_COMPONENT_DIRS "${READ_PICO_SOURCE}/components/${component}")
endforeach()

function(pico_page_board_configure_components)
    # ESP-IDF component selection alone does not order static archives after their consumers.
    idf_component_add_link_dependency(FROM reader_runtime TO platform_read_pico)
    idf_component_add_link_dependency(FROM reader_console TO platform_read_pico)
    # Use TinyUSB's MSC class with synchronous project callbacks. The convenience
    # storage layer queues writes and can remount FAT automatically on USB events.
    idf_component_get_property(usb_component espressif__esp_tinyusb COMPONENT_LIB)
    get_target_property(usb_sources ${usb_component} SOURCES)
    list(FILTER usb_sources EXCLUDE REGEX "(tinyusb_msc|storage_sdmmc|storage_spiflash|storage_blockdev)\\.c$")
    set_property(TARGET ${usb_component} PROPERTY SOURCES "${usb_sources}")
endfunction()
