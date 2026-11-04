#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
if [ "$#" -lt 2 ]; then
    echo "Usage: sh tools/check_public_books.sh FONT EPUB..." >&2
    exit 2
fi
build=${PICO_PAGE_HOST_BUILD:-build/host}
cmake --build "$build" --target reader_document reader_graphics -j 4
mkdir -p .local/public-books
${CXX:-c++} -std=c++17 -Wall -Wextra -Werror \
    -I firmware/components/reader_document/include \
    -I firmware/components/reader_core/include \
    -I firmware/components/reader_graphics/include \
    tools/public_book_probe.cpp \
    "$build/firmware/components/reader_document/libreader_document.a" \
    "$build/firmware/components/reader_graphics/libreader_graphics.a" \
    $(pkg-config --libs openssl) -o .local/public-books/probe
exec .local/public-books/probe "$@"
