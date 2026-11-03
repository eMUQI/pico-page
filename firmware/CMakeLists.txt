cmake_minimum_required(VERSION 3.22)

include("${CMAKE_CURRENT_LIST_DIR}/../boards/read_pico/board.cmake")

include(../tools/project_version.cmake)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(pico_page)
if(CONFIG_APP_PROJECT_VER_FROM_CONFIG OR CONFIG_APP_EXCLUDE_PROJECT_VER_VAR)
    message(FATAL_ERROR "The application version must come from firmware/version.txt.")
endif()
if(NOT IDF_VERSION_MAJOR EQUAL 6 OR NOT IDF_VERSION_MINOR EQUAL 1)
    message(FATAL_ERROR "This build baseline requires ESP-IDF 6.1.")
endif()

pico_page_board_configure_components()
