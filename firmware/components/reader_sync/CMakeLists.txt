if(ESP_PLATFORM)
    idf_component_register(SRCS sync.cpp mapping.cpp provision.cpp update.cpp INCLUDE_DIRS include
        PRIV_INCLUDE_DIRS ../reader_store ../reader_store/third_party/cjson ../reader_store/third_party/tomlc17
        REQUIRES reader_core reader_document reader_store)
    target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
else()
    add_library(reader_sync sync.cpp mapping.cpp provision.cpp update.cpp)
    target_include_directories(reader_sync PUBLIC include PRIVATE
        ../reader_store ../reader_store/third_party/cjson ../reader_store/third_party/tomlc17)
    target_link_libraries(reader_sync PUBLIC reader_document reader_store)
    target_compile_features(reader_sync PUBLIC cxx_std_17)
    target_compile_options(reader_sync PRIVATE -Wall -Wextra -Werror)
endif()
