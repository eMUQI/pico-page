set(document_sources css.cpp document.cpp gb18030.cpp third_party/miniz/miniz.c
    third_party/expat/xmlparse.c third_party/expat/xmlrole.c third_party/expat/xmltok.c)
set(document_private_includes third_party/miniz third_party/expat)
set(document_definitions MINIZ_NO_DEFLATE_APIS
    MINIZ_NO_ZLIB_APIS MINIZ_NO_ZLIB_COMPATIBLE_NAMES MINIZ_NO_STDIO MINIZ_NO_TIME)
if(ESP_PLATFORM)
    idf_component_register(SRCS ${document_sources} INCLUDE_DIRS include
        PRIV_INCLUDE_DIRS ${document_private_includes} REQUIRES reader_core mbedtls esp_hw_support)
    target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE ${document_definitions})
    # ESP32-S3 ROM miniz uses an incompatible decompressor layout. PNG shares this API.
    foreach(symbol mz_adler32 mz_crc32 mz_free tinfl_decompress
                   tinfl_decompress_mem_to_callback tinfl_decompress_mem_to_heap
                   tinfl_decompress_mem_to_mem)
        target_compile_definitions(${COMPONENT_LIB} PUBLIC ${symbol}=pico_${symbol})
    endforeach()
else()
    find_package(OpenSSL REQUIRED COMPONENTS Crypto)
    add_library(reader_document ${document_sources})
    target_include_directories(reader_document PUBLIC include PRIVATE ${document_private_includes})
    target_link_libraries(reader_document PUBLIC reader_core PRIVATE OpenSSL::Crypto)
    target_compile_features(reader_document PUBLIC cxx_std_17)
    target_compile_definitions(reader_document PRIVATE ${document_definitions})
    target_compile_options(reader_document PRIVATE -Wall -Wextra)
endif()
