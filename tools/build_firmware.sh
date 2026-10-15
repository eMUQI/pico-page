#!/bin/sh
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$repo_root"
mode=${1:-}
case "$mode" in
    "") ;;
    release|performance|diagnostic|stages)
        # Separate generated config and build tree; never reuse a developer's sdkconfig.
        mkdir -p "$repo_root/build/firmware-$mode"
        ;;
    *) printf '%s\n' "Usage: $0 [release|performance|diagnostic|stages]" >&2; exit 2 ;;
esac
sh tools/setup_bsp.sh
build_dir="$repo_root/firmware/build"
if [ -n "$mode" ]; then build_dir="$repo_root/build/firmware-$mode"; fi
python3 tools/firmware_provenance.py begin "$build_dir"

# EIM exposes idf.py as a shell function, which child shells do not inherit.
if [ -n "${IDF_PATH:-}" ] && [ -f "$IDF_PATH/tools/idf.py" ]; then
    PATH="$IDF_PATH/tools:$PATH"
    export PATH
fi
if command -v idf.py >/dev/null 2>&1; then
    if [ -n "$mode" ]; then
        idf.py -C firmware -B "$repo_root/build/firmware-$mode" \
            -D "SDKCONFIG=$repo_root/build/firmware-$mode/sdkconfig" \
            -D "SDKCONFIG_DEFAULTS=$repo_root/firmware/sdkconfig.defaults;$repo_root/firmware/configs/$mode.defaults" reconfigure build
    else
        idf.py -C firmware reconfigure build
    fi
elif [ -z "$mode" ] && command -v eim >/dev/null 2>&1; then
    eim --log-file .local/eim.log --do-not-track true run "idf.py -C firmware reconfigure build" v6.1
else
    printf '%s\n' "Activate an existing ESP-IDF 6.1 installation or provide EIM in PATH." >&2
    exit 1
fi
python3 tools/firmware_provenance.py finish "$build_dir"
