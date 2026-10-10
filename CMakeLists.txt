cmake_minimum_required(VERSION 3.20)
project(pico_page_host LANGUAGES C CXX)

include(CTest)
add_executable(power_key_test tests/power_key_test.cpp)
target_include_directories(power_key_test PRIVATE firmware/components/platform_read_pico/include)
target_compile_options(power_key_test PRIVATE -Wall -Wextra -Werror -UNDEBUG)
add_test(NAME power_key COMMAND power_key_test)
add_subdirectory(firmware/components/reader_core)
add_executable(core_test tests/core_test.cpp)
target_link_libraries(core_test PRIVATE reader_core)
target_compile_options(core_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME core COMMAND core_test)

add_subdirectory(firmware/components/reader_store)
add_executable(store_test tests/store_test.cpp)
target_link_libraries(store_test PRIVATE reader_store)
target_compile_options(store_test PRIVATE -Wall -Wextra -Werror -UNDEBUG)
add_test(NAME store COMMAND store_test)

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
add_test(NAME epub_probe COMMAND ${Python3_EXECUTABLE} -m unittest discover
    -s ${CMAKE_CURRENT_SOURCE_DIR}/tests -p test_epub_probe.py -v)
set(tool_card ${CMAKE_BINARY_DIR}/setup-network-card)
add_test(NAME setup_network COMMAND ${CMAKE_COMMAND} -E env PICO_PAGE_TOOL_CARD=${tool_card}
    ${Python3_EXECUTABLE} -m unittest discover -s ${CMAKE_CURRENT_SOURCE_DIR}/tests -p test_setup_network.py -v)
set_tests_properties(setup_network PROPERTIES FIXTURES_SETUP tool_card)

add_subdirectory(firmware/components/reader_document)
add_subdirectory(firmware/components/reader_graphics)
add_subdirectory(firmware/components/reader_sync)
add_subdirectory(firmware/components/reader_app)
# Compile the firmware worker path with controlled queue, allocation and scheduling failures.
add_executable(content_worker_test tests/content_worker_test.cpp firmware/components/reader_app/content_job.cpp)
target_compile_definitions(content_worker_test PRIVATE ESP_PLATFORM)
target_include_directories(content_worker_test PRIVATE tests/rtos_stubs)
target_link_libraries(content_worker_test PRIVATE reader_app)
target_compile_options(content_worker_test PRIVATE -Wall -Wextra -Werror -UNDEBUG)
add_test(NAME content_worker COMMAND content_worker_test ${CMAKE_CURRENT_BINARY_DIR}/document-fixtures/image-cover.epub)
add_executable(graphics_test tests/graphics_test.cpp)
target_link_libraries(graphics_test PRIVATE reader_graphics)
target_include_directories(graphics_test PRIVATE firmware/components/reader_graphics/third_party/qrcodegen)
target_compile_options(graphics_test PRIVATE -Wall -Wextra -Werror -UNDEBUG)
add_test(NAME graphics COMMAND graphics_test)

set(document_fixtures ${CMAKE_CURRENT_BINARY_DIR}/document-fixtures)
add_custom_command(OUTPUT ${document_fixtures}/valid.epub
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/make_document_fixtures.py ${document_fixtures}
    DEPENDS tests/make_document_fixtures.py tests/make_epub_fixture.py)
# Segmented reading and cross-part link fixtures share the same deterministic generator.
add_custom_command(OUTPUT ${document_fixtures}/multi-part.epub ${document_fixtures}/multi-part-link.epub
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/make_document_fixtures.py ${document_fixtures}
    DEPENDS tests/make_document_fixtures.py tests/make_epub_fixture.py)
add_custom_target(document_fixtures_target DEPENDS ${document_fixtures}/valid.epub ${document_fixtures}/multi-part.epub ${document_fixtures}/multi-part-link.epub)
add_dependencies(content_worker_test document_fixtures_target)
add_executable(document_test tests/document_test.cpp)
add_dependencies(document_test document_fixtures_target)
target_link_libraries(document_test PRIVATE reader_document)
target_compile_options(document_test PRIVATE -Wall -Wextra -Werror -UNDEBUG)
add_test(NAME document COMMAND document_test ${document_fixtures}/valid.epub
    ${document_fixtures}/malformed.epub ${document_fixtures}/oversized-chapter.epub ${document_fixtures}/styled.epub
    ${document_fixtures}/image-flow.epub ${document_fixtures}/long-chapter.epub ${document_fixtures}/nested-toc.epub
    ${document_fixtures}/multi-part.epub)

find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)
find_package(CURL REQUIRED)
add_executable(pico_page host/main.cpp)
target_link_libraries(pico_page PRIVATE reader_app PkgConfig::SDL2 CURL::libcurl)
target_compile_options(pico_page PRIVATE -Wall -Wextra -Werror)

add_executable(app_test tests/app_test.cpp)
add_dependencies(app_test document_fixtures_target)
target_link_libraries(app_test PRIVATE reader_app)
target_compile_options(app_test PRIVATE -Wall -Wextra -Werror -UNDEBUG)
add_test(NAME app COMMAND app_test ${document_fixtures}
    ${CMAKE_CURRENT_SOURCE_DIR}/.local/card/Fonts/LXGWWenKai-Regular.ttf)

add_executable(provision_test tests/provision_test.cpp)
target_link_libraries(provision_test PRIVATE reader_sync)
target_compile_options(provision_test PRIVATE -Wall -Wextra -Werror -UNDEBUG)
add_test(NAME provision COMMAND provision_test)

# The transfer page script runs in the phone browser; Node executes it against a DOM stand-in.
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
