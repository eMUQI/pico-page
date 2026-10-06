cmake_minimum_required(VERSION 3.22)

set(IDF_TARGET esp32s3)
set(READ_PICO_REVISION 28cde682a4468a581c278761922724f57d976418)
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

foreach(component read_pico read_pico_pmu epdiy e0470_epaper_waveform
                  cst836u sc7a20h fca9555 sy7636a pwm_audio)
    list(APPEND EXTRA_COMPONENT_DIRS "${READ_PICO_SOURCE}/components/${component}")
endforeach()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(MINIMAL_BUILD ON)
project(pico_page)
if(NOT IDF_VERSION_MAJOR EQUAL 6 OR NOT IDF_VERSION_MINOR EQUAL 1)
    message(FATAL_ERROR "This build baseline requires ESP-IDF 6.1.")
endif()
