#!/bin/sh
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$repo_root"
mode=${1:-}
case "$mode" in
    "") ;;
    release|performance|diagnostic|stages) ;;
    *) printf '%s\n' "Usage: $0 [release [absolute-build-dir]|performance|diagnostic|stages]" >&2; exit 2 ;;
esac
if [ "$#" -gt 1 ]; then
    case "$mode:$#:$2" in
        release:2:/*) ;;
        *) printf '%s\n' "A custom build directory requires release mode and an absolute path." >&2; exit 2 ;;
    esac
fi
sh tools/setup_bsp.sh
build_dir="$repo_root/firmware/build"
sdkconfig="$repo_root/firmware/sdkconfig"
if [ -n "$mode" ]; then
    build_dir="${2:-$repo_root/build/firmware-$mode}"
    sdkconfig="$build_dir/sdkconfig"
fi
reject_bbpll_recalibration() {
    if grep -q '^CONFIG_ESP_SYSTEM_BBPLL_RECALIB=y' "$sdkconfig" 2>/dev/null; then
        printf '%s\n' "$sdkconfig keeps CONFIG_ESP_SYSTEM_BBPLL_RECALIB=y, which can stall USB Serial/JTAG after a reset. Delete it so the board defaults apply, then rebuild." >&2
        exit 1
    fi
}
reject_bbpll_recalibration
python3 tools/firmware_provenance.py begin "$build_dir" "$mode"

# EIM exposes idf.py as a shell function, which child shells do not inherit.
if [ -n "${IDF_PATH:-}" ] && [ -f "$IDF_PATH/tools/idf.py" ]; then
    PATH="$IDF_PATH/tools:$PATH"
    export PATH
fi
if command -v idf.py >/dev/null 2>&1; then
    if [ -n "$mode" ]; then
        idf.py -C firmware -B "$build_dir" \
            -D "SDKCONFIG=$sdkconfig" \
            -D "SDKCONFIG_DEFAULTS=$repo_root/firmware/board/sdkconfig.defaults;$repo_root/firmware/configs/$mode.defaults" reconfigure build
    else
        idf.py -C firmware reconfigure build
    fi
elif [ -z "$mode" ] && command -v eim >/dev/null 2>&1; then
    eim --log-file .local/eim.log --do-not-track true run "idf.py -C firmware reconfigure build" v6.1
else
    printf '%s\n' "Activate an existing ESP-IDF 6.1 installation or provide EIM in PATH." >&2
    exit 1
fi
reject_bbpll_recalibration
python3 tools/firmware_provenance.py finish "$build_dir" "$mode"
