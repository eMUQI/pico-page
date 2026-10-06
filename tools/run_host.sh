#!/bin/sh
set -eu
repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$repo_root"
python3 tools/setup_fonts.py
python3 tools/prepare_demo.py
cmake -S . -B build/host -DCMAKE_BUILD_TYPE=Debug
cmake --build build/host --target pico_page --parallel 4
exec build/host/pico_page --card "$repo_root/.local/card" "$@"
