# Small OFL WenKai subset used only by the offline welcome subtitle.
file(READ "${CMAKE_CURRENT_LIST_DIR}/../reader_graphics/fonts/PicoPageWelcome.ttf" welcome_hex HEX)
string(REGEX REPLACE "(..)" "0x\\1," welcome_bytes "${welcome_hex}")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/onboarding_font.h"
    "static const unsigned char kOnboardingFont[] = {${welcome_bytes}};\n")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_LIST_DIR}/../reader_graphics/fonts/PicoPageWelcome.ttf")

set(sleep_cover_asset "${CMAKE_CURRENT_LIST_DIR}/assets/default_sleep_cover.zlib")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${sleep_cover_asset}")
file(READ "${sleep_cover_asset}" sleep_cover_hex HEX)
string(REGEX REPLACE "(..)" "0x\\1," sleep_cover_bytes "${sleep_cover_hex}")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/default_sleep_cover.h"
    "static constexpr unsigned char kDefaultSleepCover[] = {${sleep_cover_bytes}};\n")

set(app_private_includes "${CMAKE_CURRENT_BINARY_DIR}" ../reader_document/third_party/miniz)

set(app_sources
    app.cpp
    app_bookmarks.cpp
    app_content.cpp
    app_drawing.cpp
    app_dictionary.cpp
    app_input.cpp
    app_library.cpp
    app_onboarding.cpp
    app_power.cpp
    app_reading.cpp
    app_settings.cpp
    app_statistics.cpp
    app_statistics_runtime.cpp
    app_sync.cpp
    app_transfer.cpp
    app_update.cpp
    app_wifi.cpp
    content_cache.cpp content_job.cpp dictionary_job.cpp page_cache.cpp)

if(ESP_PLATFORM)
    idf_component_register(SRCS ${app_sources} INCLUDE_DIRS include REQUIRES reader_core reader_store reader_document reader_dictionary reader_graphics reader_sync
        PRIV_REQUIRES esp_app_format
        PRIV_INCLUDE_DIRS ${app_private_includes})
    target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
else()
    find_package(Threads REQUIRED)
    add_library(reader_app ${app_sources})
    target_include_directories(reader_app PUBLIC include PRIVATE ${app_private_includes})
    target_link_libraries(reader_app PUBLIC reader_core reader_store reader_document reader_dictionary reader_graphics reader_sync Threads::Threads)
    target_compile_features(reader_app PUBLIC cxx_std_17)
    target_compile_options(reader_app PRIVATE -Wall -Wextra -Werror)
endif()
