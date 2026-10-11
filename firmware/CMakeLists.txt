cmake_minimum_required(VERSION 3.22)

set(IDF_TARGET esp32s3)
set(READ_PICO_REVISION 5f3406deec33304de8801046db583dec7c49e55b)
set(READ_PICO_SOURCE "${CMAKE_CURRENT_LIST_DIR}/../.local/read_pico_firmware")
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

# Patch a build-local copy, preserving the verified upstream checkout.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_LIST_DIR}/patches/epdiy-row-phases.patch"
    "${CMAKE_CURRENT_LIST_DIR}/patches/read-pico-rtc-response.patch")
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
list(APPEND EXTRA_COMPONENT_DIRS "${READ_PICO_ADAPTED}/epdiy")
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

include(../tools/project_version.cmake)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(MINIMAL_BUILD ON)
project(pico_page)
if(CONFIG_APP_PROJECT_VER_FROM_CONFIG OR CONFIG_APP_EXCLUDE_PROJECT_VER_VAR)
    message(FATAL_ERROR "The application version must come from firmware/version.txt.")
endif()
if(NOT IDF_VERSION_MAJOR EQUAL 6 OR NOT IDF_VERSION_MINOR EQUAL 1)
    message(FATAL_ERROR "This build baseline requires ESP-IDF 6.1.")
endif()

# Use TinyUSB's MSC class with synchronous project callbacks. The convenience
# storage layer queues writes and can remount FAT automatically on USB events.
idf_component_get_property(usb_component espressif__esp_tinyusb COMPONENT_LIB)
get_target_property(usb_sources ${usb_component} SOURCES)
list(FILTER usb_sources EXCLUDE REGEX "(tinyusb_msc|storage_sdmmc|storage_spiflash|storage_blockdev)\\.c$")
set_property(TARGET ${usb_component} PROPERTY SOURCES "${usb_sources}")
