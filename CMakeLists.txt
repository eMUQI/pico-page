cmake_minimum_required(VERSION 3.20)
project(pico_page_host LANGUAGES C CXX)
include(tools/project_version.cmake)
include(firmware/board/profile.cmake)

include(CTest)
add_executable(display_network_gate_test tests/display_network_gate_test.cpp
    firmware/components/platform_read_pico/presentation_gate.cpp)
target_link_libraries(display_network_gate_test PRIVATE platform_contract)
target_include_directories(display_network_gate_test PRIVATE firmware/components/platform_read_pico)
target_compile_features(display_network_gate_test PRIVATE cxx_std_17)
target_compile_options(display_network_gate_test PRIVATE -Wall -Wextra -Werror -UNDEBUG)
add_test(NAME display_network_gate COMMAND display_network_gate_test)
add_executable(firmware_resume_test tests/firmware_resume_test.cpp)
target_include_directories(firmware_resume_test PRIVATE firmware/components/platform_read_pico)
target_compile_features(firmware_resume_test PRIVATE cxx_std_17)
target_compile_options(firmware_resume_test PRIVATE -Wall -Wextra -Werror -UNDEBUG)
add_test(NAME firmware_resume COMMAND firmware_resume_test)
add_executable(power_key_test tests/power_key_test.cpp)
target_include_directories(power_key_test PRIVATE firmware/components/platform_read_pico/include)
target_compile_features(power_key_test PRIVATE cxx_std_17)
target_compile_options(power_key_test PRIVATE -Wall -Wextra -Werror -UNDEBUG)
add_test(NAME power_key COMMAND power_key_test)
add_subdirectory(firmware/components/reader_core)
add_subdirectory(firmware/components/platform_contract)
add_subdirectory(firmware/components/reader_input)
set(contract_headers build_profile platform platform_input platform_display platform_storage
    platform_power platform_system platform_network platform_update)
foreach(header IN LISTS contract_headers)
    set(source "${CMAKE_CURRENT_BINARY_DIR}/contract_${header}.cpp")
    file(WRITE "${source}" "#include \"pico_page/${header}.h\"\n")
    list(APPEND contract_sources "${source}")
endforeach()
add_library(platform_contract_headers OBJECT ${contract_sources})
target_link_libraries(platform_contract_headers PRIVATE platform_contract)
target_compile_options(platform_contract_headers PRIVATE -Wall -Wextra -Werror)
add_executable(core_test tests/core_test.cpp)
target_link_libraries(core_test PRIVATE reader_core reader_input)
target_compile_options(core_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME core COMMAND core_test)

add_executable(reading_statistics_test tests/reading_statistics_test.cpp)
target_link_libraries(reading_statistics_test PRIVATE reader_core)
target_compile_options(reading_statistics_test PRIVATE -Wall -Wextra -Werror -UNDEBUG)
add_test(NAME reading_statistics COMMAND reading_statistics_test)

add_subdirectory(firmware/components/reader_store)
add_executable(store_test tests/store_test.cpp)
target_link_libraries(store_test PRIVATE reader_store)
target_compile_options(store_test PRIVATE -Wall -Wextra -Werror -UNDEBUG)
add_test(NAME store COMMAND store_test)

add_executable(statistics_store_test tests/statistics_store_test.cpp)
target_link_libraries(statistics_store_test PRIVATE reader_store)
target_compile_options(statistics_store_test PRIVATE -Wall -Wextra -Werror -UNDEBUG)
add_test(NAME statistics_store COMMAND statistics_store_test)

add_executable(store_sync_test tests/store_sync_test.cpp)
target_link_libraries(store_sync_test PRIVATE reader_store)
target_include_directories(store_sync_test PRIVATE firmware/components/reader_store/third_party/cjson)
target_compile_options(store_sync_test PRIVATE -Wall -Wextra -Werror -UNDEBUG)
add_test(NAME store_sync COMMAND store_sync_test)

add_executable(reader_flow_test tests/reader_flow_test.cpp)
target_link_libraries(reader_flow_test PRIVATE reader_store)
target_compile_options(reader_flow_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME reader_flow COMMAND reader_flow_test)

find_package(Python3 3.11 REQUIRED COMPONENTS Interpreter)
add_test(NAME architecture COMMAND ${CMAKE_COMMAND} -E env "PICO_PAGE_CMAKE=${CMAKE_COMMAND}"
    ${Python3_EXECUTABLE} -m unittest discover
    -s ${CMAKE_CURRENT_SOURCE_DIR}/tests -p test_architecture.py -v)
add_test(NAME css_survey COMMAND ${Python3_EXECUTABLE} -m unittest discover
    -s ${CMAKE_CURRENT_SOURCE_DIR}/tests -p test_css_survey.py -v)
add_test(NAME epub_probe COMMAND ${Python3_EXECUTABLE} -m unittest discover
    -s ${CMAKE_CURRENT_SOURCE_DIR}/tests -p test_epub_probe.py -v)
set(tool_card ${CMAKE_BINARY_DIR}/setup-network-card)
add_test(NAME setup_network COMMAND ${CMAKE_COMMAND} -E env PICO_PAGE_TOOL_CARD=${tool_card}
    ${Python3_EXECUTABLE} -m unittest discover -s ${CMAKE_CURRENT_SOURCE_DIR}/tests -p test_setup_network.py -v)
set_tests_properties(setup_network PROPERTIES FIXTURES_SETUP tool_card)

add_subdirectory(firmware/components/reader_document)
add_executable(css_test tests/css_test.cpp)
target_link_libraries(css_test PRIVATE reader_document)
target_include_directories(css_test PRIVATE firmware/components/reader_document)
target_compile_options(css_test PRIVATE -Wall -Wextra -Werror -UNDEBUG)
add_test(NAME css COMMAND css_test)
add_executable(css_probe EXCLUDE_FROM_ALL tools/css_probe.cpp)
target_link_libraries(css_probe PRIVATE reader_document)
target_include_directories(css_probe PRIVATE firmware/components/reader_document)
target_compile_options(css_probe PRIVATE -Wall -Wextra -Werror)


add_subdirectory(firmware/components/reader_dictionary)
set(dictionary_fixtures ${CMAKE_CURRENT_BINARY_DIR}/dictionary-fixtures)
add_custom_target(dictionary_fixtures_target
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/make_dictionary_fixtures.py ${dictionary_fixtures}
    VERBATIM)
add_executable(dictionary_test tests/dictionary_test.cpp)
add_dependencies(dictionary_test dictionary_fixtures_target)
target_link_libraries(dictionary_test PRIVATE reader_dictionary)
target_compile_options(dictionary_test PRIVATE -Wall -Wextra -Werror -UNDEBUG)
add_test(NAME dictionary COMMAND dictionary_test ${dictionary_fixtures})
add_executable(dictionary_probe EXCLUDE_FROM_ALL tools/dictionary_probe.cpp)
target_link_libraries(dictionary_probe PRIVATE reader_dictionary)
target_compile_options(dictionary_probe PRIVATE -Wall -Wextra -Werror)
add_subdirectory(firmware/components/reader_graphics)
add_executable(display_compatibility_test tests/display_compatibility_test.cpp
    firmware/components/reader_runtime/display_compatibility.cpp
    firmware/components/platform_read_pico/display_geometry.cpp)
target_include_directories(display_compatibility_test PRIVATE firmware/components/reader_runtime)
target_link_libraries(display_compatibility_test PRIVATE reader_graphics platform_contract)
# This startup guard must still execute when release builds remove assert().
target_compile_definitions(display_compatibility_test PRIVATE NDEBUG)
target_compile_options(display_compatibility_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME display_compatibility COMMAND display_compatibility_test)
add_subdirectory(firmware/components/reader_sync)
add_subdirectory(firmware/components/reader_app)
target_compile_definitions(reader_app PRIVATE PICO_PAGE_VERSION="${PROJECT_VER}")
add_test(NAME project_version COMMAND ${Python3_EXECUTABLE} -m unittest discover
    -s ${CMAKE_CURRENT_SOURCE_DIR}/tests -p test_project_version.py -v)
add_test(NAME ota_manifest COMMAND ${Python3_EXECUTABLE} -m unittest discover
    -s ${CMAKE_CURRENT_SOURCE_DIR}/tests -p test_ota_manifest.py -v)
add_executable(content_worker_test tests/content_worker_test.cpp firmware/components/reader_app/content_job.cpp firmware/components/reader_app/dictionary_job.cpp)
target_compile_definitions(content_worker_test PRIVATE ESP_PLATFORM)
target_include_directories(content_worker_test PRIVATE tests/rtos_stubs)
target_link_libraries(content_worker_test PRIVATE reader_app)
target_compile_options(content_worker_test PRIVATE -Wall -Wextra -Werror -UNDEBUG)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_options(content_worker_test PRIVATE "LINKER:--wrap=calloc" "LINKER:--wrap=fopen")
    target_compile_definitions(content_worker_test PRIVATE PICO_PAGE_WRAP_CALLOC_TEST=1)
endif()
add_dependencies(content_worker_test dictionary_fixtures_target)
add_test(NAME content_worker COMMAND content_worker_test ${CMAKE_CURRENT_BINARY_DIR}/document-fixtures/image-cover.epub ${dictionary_fixtures})
add_executable(graphics_test tests/graphics_test.cpp)
target_link_libraries(graphics_test PRIVATE reader_graphics)
target_include_directories(graphics_test PRIVATE firmware/components/reader_graphics/third_party/qrcodegen)
target_compile_options(graphics_test PRIVATE -Wall -Wextra -Werror -UNDEBUG)
add_test(NAME graphics COMMAND graphics_test)

set(document_fixtures ${CMAKE_CURRENT_BINARY_DIR}/document-fixtures)
add_custom_target(document_fixtures_target
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/make_document_fixtures.py ${document_fixtures}
    VERBATIM)
add_dependencies(content_worker_test document_fixtures_target)
add_executable(chapter_stream_probe EXCLUDE_FROM_ALL tools/chapter_stream_probe.cpp)
target_link_libraries(chapter_stream_probe PRIVATE reader_document)
target_compile_options(chapter_stream_probe PRIVATE -Wall -Wextra -Werror)
add_executable(document_test tests/document_test.cpp)
add_dependencies(document_test document_fixtures_target)
target_link_libraries(document_test PRIVATE reader_document)
target_compile_options(document_test PRIVATE -Wall -Wextra -Werror -UNDEBUG)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_options(document_test PRIVATE "LINKER:--wrap=calloc")
    target_compile_definitions(document_test PRIVATE PICO_PAGE_WRAP_CALLOC_TEST=1)
endif()
add_test(NAME document COMMAND document_test ${document_fixtures}/valid.epub
    ${document_fixtures}/malformed.epub ${document_fixtures}/oversized-chapter.epub ${document_fixtures}/styled.epub
    ${document_fixtures}/image-flow.epub ${document_fixtures}/long-chapter.epub ${document_fixtures}/nested-toc.epub
    ${document_fixtures}/multi-part.epub)

find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)
find_package(CURL REQUIRED)
add_executable(pico_page tools/host/main.cpp)
target_link_libraries(pico_page PRIVATE reader_app PkgConfig::SDL2 CURL::libcurl)
target_compile_options(pico_page PRIVATE -Wall -Wextra -Werror)

add_executable(image_cache_bench EXCLUDE_FROM_ALL tools/image_cache_bench.cpp)
add_dependencies(image_cache_bench document_fixtures_target)
target_link_libraries(image_cache_bench PRIVATE reader_app)
target_compile_options(image_cache_bench PRIVATE -Wall -Wextra -Werror)

add_executable(onboarding_test tests/onboarding_test.cpp)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_executable(reading_statistics_bench EXCLUDE_FROM_ALL tools/reading_statistics_bench.cpp)
    target_link_libraries(reading_statistics_bench PRIVATE reader_app)
    target_link_options(reading_statistics_bench PRIVATE "LINKER:--wrap=fsync" "LINKER:--wrap=rename")
    target_compile_options(reading_statistics_bench PRIVATE -Wall -Wextra -Werror)

    add_executable(large_epub_probe tests/large_epub_probe.cpp tests/host_allocations.cpp)
    add_dependencies(large_epub_probe document_fixtures_target)
    target_link_libraries(large_epub_probe PRIVATE reader_app)
    target_compile_definitions(large_epub_probe PRIVATE PICO_PAGE_VERSION="${PROJECT_VER}")
    target_include_directories(large_epub_probe PRIVATE firmware/components/reader_document/third_party/miniz)
    target_compile_options(large_epub_probe PRIVATE -Wall -Wextra -Werror -UNDEBUG)
    foreach(symbol malloc calloc realloc free posix_memalign mz_zip_reader_get_filename mz_zip_reader_extract_to_callback)
        target_link_options(large_epub_probe PRIVATE "LINKER:--wrap=${symbol}")
    endforeach()
    add_test(NAME large_epub_work COMMAND large_epub_probe ${document_fixtures})
endif()
target_link_libraries(onboarding_test PRIVATE reader_app)
target_compile_options(onboarding_test PRIVATE -Wall -Wextra -Werror -UNDEBUG)
add_test(NAME onboarding COMMAND onboarding_test)

add_executable(app_test tests/app_test.cpp)
add_dependencies(app_test document_fixtures_target dictionary_fixtures_target)
target_link_libraries(app_test PRIVATE reader_app reader_input)
target_compile_options(app_test PRIVATE -Wall -Wextra -Werror -UNDEBUG)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_options(app_test PRIVATE "LINKER:--wrap=malloc" "LINKER:--wrap=fopen" "LINKER:--wrap=readdir" "LINKER:--wrap=lstat")
    target_compile_definitions(app_test PRIVATE PICO_PAGE_WRAP_MALLOC_TEST=1 PICO_PAGE_WRAP_LIBRARY_IO_TEST=1)
endif()
add_test(NAME app COMMAND app_test ${document_fixtures}
    ${CMAKE_CURRENT_SOURCE_DIR}/.local/card/Fonts/LXGWWenKai-Regular.ttf)

add_executable(chapter_jump_test tests/chapter_jump_test.cpp)
add_dependencies(chapter_jump_test document_fixtures_target)
target_include_directories(chapter_jump_test PRIVATE firmware/components/reader_app)
target_link_libraries(chapter_jump_test PRIVATE reader_app)
target_compile_options(chapter_jump_test PRIVATE -Wall -Wextra -Werror -UNDEBUG)
add_test(NAME chapter_jump COMMAND chapter_jump_test ${document_fixtures})

add_executable(statistics_app_test tests/statistics_app_test.cpp)
add_dependencies(statistics_app_test document_fixtures_target)
target_link_libraries(statistics_app_test PRIVATE reader_app)
target_compile_options(statistics_app_test PRIVATE -Wall -Wextra -Werror -UNDEBUG)
add_test(NAME statistics_app COMMAND statistics_app_test ${document_fixtures}
    ${CMAKE_CURRENT_SOURCE_DIR}/.local/card/Fonts/LXGWWenKai-Regular.ttf)

add_executable(provision_test tests/provision_test.cpp)
target_link_libraries(provision_test PRIVATE reader_sync)
target_compile_options(provision_test PRIVATE -Wall -Wextra -Werror -UNDEBUG)
add_test(NAME provision COMMAND provision_test)

add_executable(update_test tests/update_test.cpp)
target_link_libraries(update_test PRIVATE reader_sync)
target_compile_options(update_test PRIVATE -Wall -Wextra -Werror -UNDEBUG)
add_test(NAME update COMMAND update_test)

find_program(NODE_EXECUTABLE node)
if(NODE_EXECUTABLE)
    add_test(NAME wifi_upload_page COMMAND ${NODE_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/wifi_upload_page_test.mjs)
endif()

add_executable(sync_test tests/sync_test.cpp)
add_dependencies(sync_test document_fixtures_target)
target_link_libraries(sync_test PRIVATE reader_sync)
target_compile_options(sync_test PRIVATE -Wall -Wextra -Werror -UNDEBUG)
add_test(NAME sync COMMAND sync_test ${document_fixtures}/valid.epub ${tool_card})
set_tests_properties(sync PROPERTIES FIXTURES_REQUIRED tool_card)

find_package(Threads REQUIRED)
foreach(mode release performance diagnostic)
    add_executable(diagnostics_${mode}_test tests/diagnostics_test.cpp
        firmware/components/reader_core/diagnostics.cpp)
    target_include_directories(diagnostics_${mode}_test PRIVATE firmware/components/reader_core/include)
    target_compile_features(diagnostics_${mode}_test PRIVATE cxx_std_17)
    target_link_libraries(diagnostics_${mode}_test PRIVATE Threads::Threads)
    target_compile_options(diagnostics_${mode}_test PRIVATE -Wall -Wextra -Werror -UNDEBUG)
    if(NOT mode STREQUAL "release")
        target_compile_definitions(diagnostics_${mode}_test PRIVATE CONFIG_PICO_PAGE_PERF_METRICS=1)
    endif()
    if(mode STREQUAL "diagnostic")
        target_compile_definitions(diagnostics_${mode}_test PRIVATE CONFIG_PICO_PAGE_TRACE_EVENTS=1)
    endif()
    add_test(NAME diagnostics_${mode} COMMAND diagnostics_${mode}_test)
endforeach()
add_test(NAME device_workbench COMMAND ${Python3_EXECUTABLE} -m unittest discover
    -s ${CMAKE_CURRENT_SOURCE_DIR}/tests -p test_device_workbench.py -v)
add_test(NAME usb_reset_stress COMMAND ${Python3_EXECUTABLE} -m unittest discover
    -s ${CMAKE_CURRENT_SOURCE_DIR}/tests -p test_usb_reset_stress.py -v)

add_executable(changed_pixels_waveform_test tests/changed_pixels_waveform_test.cpp
    firmware/components/platform_read_pico/changed_pixels_waveform.cpp)
target_include_directories(changed_pixels_waveform_test PRIVATE firmware/components/platform_read_pico/include)
target_compile_features(changed_pixels_waveform_test PRIVATE cxx_std_17)
target_compile_options(changed_pixels_waveform_test PRIVATE -Wall -Wextra -Werror -UNDEBUG)
add_test(NAME changed_pixels_waveform COMMAND changed_pixels_waveform_test)

option(PICO_PAGE_TEST_BSP_WAVEFORM "Test footer drive against the pinned BSP" OFF)
if(PICO_PAGE_TEST_BSP_WAVEFORM)
    set(bsp_source "${CMAKE_CURRENT_SOURCE_DIR}/.local/read_pico_firmware")
    execute_process(COMMAND sh "${CMAKE_CURRENT_SOURCE_DIR}/tools/setup_bsp.sh"
        RESULT_VARIABLE bsp_result)
    if(NOT bsp_result EQUAL 0)
        message(FATAL_ERROR "Cannot prepare pinned BSP waveform tests")
    endif()
    set(waveform_source "${bsp_source}/components/e0470_epaper_waveform")
    add_executable(footer_waveform_test tests/footer_waveform_test.cpp
        "${waveform_source}/e0470_epaper_waveform.c" "${waveform_source}/e0470_waveform_trim.c"
        firmware/components/platform_read_pico/changed_pixels_waveform.cpp)
    target_include_directories(footer_waveform_test PRIVATE tests/waveform_stubs
        "${waveform_source}/include" "${waveform_source}/waveforms"
        "${bsp_source}/components/epdiy/include" "${bsp_source}/components/epdiy/src"
        firmware/components/platform_read_pico/include)
    target_compile_features(footer_waveform_test PRIVATE cxx_std_17)
    target_compile_options(footer_waveform_test PRIVATE -Wall -Wextra -Werror -UNDEBUG)
    add_test(NAME footer_waveform COMMAND footer_waveform_test)
endif()

add_executable(page_sweep_test tests/page_sweep_test.cpp firmware/components/platform_read_pico/page_sweep.cpp)
target_include_directories(page_sweep_test PRIVATE tests/page_sweep_stubs firmware/components/platform_read_pico
    firmware/components/platform_read_pico/include firmware/components/platform_contract/include)
target_link_libraries(page_sweep_test PRIVATE reader_core)
target_compile_options(page_sweep_test PRIVATE -Wall -Wextra -Werror -UNDEBUG)
add_test(NAME page_sweep COMMAND page_sweep_test)

add_executable(rtc_clock_test tests/rtc_clock_test.cpp firmware/components/platform_read_pico/rtc_clock.cpp)
target_include_directories(rtc_clock_test PRIVATE tests/rtc_stubs firmware/components/platform_read_pico
    firmware/components/reader_sync/include firmware/components/reader_document/include firmware/components/reader_core/include)
target_compile_features(rtc_clock_test PRIVATE cxx_std_17)
target_compile_definitions(rtc_clock_test PRIVATE time=rtc_test_time settimeofday=rtc_test_settimeofday)
target_compile_options(rtc_clock_test PRIVATE -Wall -Wextra -Werror -UNDEBUG)
add_test(NAME rtc_clock COMMAND rtc_clock_test)

add_executable(sleep_io_test tests/sleep_io_test.cpp firmware/components/platform_read_pico/sleep_io.cpp)
target_include_directories(sleep_io_test PRIVATE tests/sleep_io_stubs firmware/components/platform_read_pico)
target_compile_features(sleep_io_test PRIVATE cxx_std_17)
target_compile_options(sleep_io_test PRIVATE -Wall -Wextra -Werror -UNDEBUG)
add_test(NAME sleep_io COMMAND sleep_io_test)

add_executable(sleep_usb_console_test tests/sleep_usb_console_test.cpp
    firmware/components/platform_read_pico/sleep_usb_console.cpp)
target_include_directories(sleep_usb_console_test PRIVATE tests/sleep_io_stubs firmware/components/platform_read_pico)
target_compile_features(sleep_usb_console_test PRIVATE cxx_std_17)
target_compile_options(sleep_usb_console_test PRIVATE -Wall -Wextra -Werror -UNDEBUG)
add_test(NAME sleep_usb_console COMMAND sleep_usb_console_test)

add_executable(usb_storage_test tests/usb_storage_test.cpp firmware/components/platform_read_pico/usb_storage.cpp)
target_include_directories(usb_storage_test PRIVATE tests/usb_storage_stubs firmware/components/platform_read_pico)
target_compile_features(usb_storage_test PRIVATE cxx_std_17)
target_compile_options(usb_storage_test PRIVATE -Wall -Wextra -Werror -UNDEBUG)
add_test(NAME usb_storage COMMAND usb_storage_test)

add_executable(portal_server_test tests/portal_server_test.cpp
    firmware/components/platform_read_pico/presentation_gate.cpp
    firmware/components/platform_read_pico/portal_server.cpp)
target_include_directories(portal_server_test PRIVATE tests/portal_stubs
    firmware/components/platform_read_pico/include)
target_link_libraries(portal_server_test PRIVATE reader_store platform_contract)
target_compile_options(portal_server_test PRIVATE -Wall -Wextra -Werror -UNDEBUG)
add_test(NAME portal_server COMMAND portal_server_test)

add_executable(language_test tests/language_test.cpp)
target_link_libraries(language_test PRIVATE reader_app)
target_compile_options(language_test PRIVATE -Wall -Wextra -Werror -UNDEBUG)
add_test(NAME language COMMAND language_test)
add_test(NAME translations COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/generate_translations.py --check)
