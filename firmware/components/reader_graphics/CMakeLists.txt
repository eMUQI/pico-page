get_filename_component(PICO_PAGE_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
set(ft_source "${PICO_PAGE_ROOT}/.local/freetype-526ec5c47b9ebccc4754c85ac0c0cdf7c85a5e9b")
if(NOT EXISTS "${ft_source}/include/ft2build.h")
    message(FATAL_ERROR "Run python3 tools/setup_fonts.py to fetch the pinned graphics dependencies.")
endif()

set(ft_sources)
foreach(source base/ftbase.c base/ftinit.c base/ftsystem.c base/ftdebug.c
               base/ftbitmap.c base/ftglyph.c base/ftmm.c
               truetype/truetype.c cff/cff.c sfnt/sfnt.c psaux/psaux.c
               psnames/psnames.c pshinter/pshinter.c smooth/smooth.c raster/raster.c)
    list(APPEND ft_sources "${ft_source}/src/${source}")
endforeach()

# The original subset font remains independently inspectable with its OFL notice.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_LIST_DIR}/fonts/PicoPageFallback.otf")
file(READ "${CMAKE_CURRENT_LIST_DIR}/fonts/PicoPageFallback.otf" fallback_hex HEX)
string(REGEX REPLACE "(..)" "0x\\1," fallback_bytes "${fallback_hex}")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/fallback_font.h"
    "static const unsigned char kFallbackFont[] = {${fallback_bytes}};\nstatic const unsigned long kFallbackFontSize = sizeof(kFallbackFont);\n")

set(image_sources image.cpp third_party/pngle/pngle.c third_party/tjpgd/tjpgd.c)
set(image_includes third_party/pngle third_party/tjpgd ../reader_document/third_party/miniz)
if(ESP_PLATFORM)
    idf_component_register(SRCS graphics.cpp ${image_sources} ${ft_sources}
        INCLUDE_DIRS include
        PRIV_INCLUDE_DIRS "${ft_source}/include" "." "${CMAKE_CURRENT_BINARY_DIR}" ${image_includes}
        REQUIRES reader_document freertos)
    set(graphics_target ${COMPONENT_LIB})
else()
    add_library(reader_graphics graphics.cpp ${image_sources} ${ft_sources})
    target_include_directories(reader_graphics PUBLIC include
        PRIVATE "${ft_source}/include" "." "${CMAKE_CURRENT_BINARY_DIR}" ${image_includes})
    target_link_libraries(reader_graphics PRIVATE reader_document)
    set(graphics_target reader_graphics)
    set_source_files_properties(graphics.cpp image.cpp PROPERTIES COMPILE_OPTIONS "-Wall;-Wextra;-Werror")
endif()
target_compile_features(${graphics_target} PUBLIC cxx_std_17)
target_compile_definitions(${graphics_target} PRIVATE FT2_BUILD_LIBRARY
    FT_CONFIG_MODULES_H="freetype_modules.h" FT_CONFIG_OPTIONS_H="freetype_options.h"
    PNGLE_NO_GAMMA_CORRECTION MINIZ_NO_ZLIB_COMPATIBLE_NAMES)
