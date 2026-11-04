get_filename_component(repo_root "${CMAKE_CURRENT_LIST_DIR}/../../../.." ABSOLUTE)
idf_component_register(SRCS bench.cpp "${repo_root}/firmware/components/reader_document/css.cpp"
    INCLUDE_DIRS "${repo_root}/firmware/components/reader_document"
    REQUIRES esp_timer heap)
target_compile_features(${COMPONENT_LIB} PRIVATE cxx_std_17)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
if(CSS_BENCH_DATA)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE CSS_BENCH_DATA="${CSS_BENCH_DATA}")
endif()
