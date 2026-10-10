#!/bin/sh
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$repo_root"
mode=${1:-}
case "$mode" in
    "") ;;
    release|performance|diagnostic)
        # Separate generated config and build tree; never reuse a developer's sdkconfig.
        mkdir -p "$repo_root/build/firmware-$mode"
        ;;
    *) printf '%s\n' "Usage: $0 [release|performance|diagnostic]" >&2; exit 2 ;;
esac
sh tools/setup_bsp.sh

if command -v idf.py >/dev/null 2>&1; then
    if [ -n "$mode" ]; then
        exec idf.py -C firmware -B "$repo_root/build/firmware-$mode" \
            -D "SDKCONFIG=$repo_root/build/firmware-$mode/sdkconfig" \
            -D "SDKCONFIG_DEFAULTS=$repo_root/firmware/sdkconfig.defaults;$repo_root/firmware/configs/$mode.defaults" build
    fi
    exec idf.py -C firmware build
elif [ -z "$mode" ] && command -v eim >/dev/null 2>&1; then
    exec eim --log-file .local/eim.log --do-not-track true run "idf.py -C firmware build" v6.1
fi

printf '%s\n' "Activate an existing ESP-IDF 6.1 installation or provide EIM in PATH." >&2
exit 1
