#!/bin/sh
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$repo_root"
sh tools/setup_bsp.sh

if command -v idf.py >/dev/null 2>&1; then
    exec idf.py -C firmware build
elif command -v eim >/dev/null 2>&1; then
    exec eim --log-file .local/eim.log --do-not-track true run "idf.py -C firmware build" v6.1
fi

printf '%s\n' "Activate an existing ESP-IDF 6.1 installation or provide EIM in PATH." >&2
exit 1
