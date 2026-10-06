#pragma once
#include <freetype/config/ftoption.h>

/* Card fonts are uncompressed outline files; external image/codecs are unused. */
#undef FT_CONFIG_OPTION_USE_ZLIB
#undef FT_CONFIG_OPTION_USE_BZIP2
#undef FT_CONFIG_OPTION_USE_PNG
#undef FT_CONFIG_OPTION_USE_BROTLI
#undef FT_CONFIG_OPTION_USE_HARFBUZZ
#undef FT_CONFIG_OPTION_SVG
#undef TT_CONFIG_OPTION_EMBEDDED_BITMAPS
#undef TT_CONFIG_OPTION_COLOR_LAYERS
