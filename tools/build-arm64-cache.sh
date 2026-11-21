#!/bin/sh
# Pre-build -O2 Cortex-A53 native AArch64 game modules on your PC from your own
# MAME ROMs and CHD/raw disk images, so an Anbernic RG34XX (1 GB RAM) boots
# straight into locked 60 FPS without compiling on-device.
#
# Usage:
#   tools/build-arm64-cache.sh /path/to/ports/ultracombo [cortex-a53]
#
# Expects your own game files inside <data-dir>/run/ (kinst and/or kinst2).
# Writes cache/rec-<hash>-aarch64.so into <data-dir>/cache/.
set -e
cd "$(dirname "$0")/.."

DATA_DIR=${1:-$PWD}
CPU=${2:-cortex-a53}

if [ ! -x ./ultracombo ]; then
    echo "Building native host ultracombo first..."
    make -j"$(nproc)" ultracombo
fi

if [ ! -x deps/zig-linux/zig ] && ! command -v zig >/dev/null 2>&1 && ! command -v aarch64-linux-gnu-gcc >/dev/null 2>&1; then
    echo "Need Zig (run tools/build-arm64.sh once to fetch deps/zig-linux/zig) or aarch64-linux-gnu-gcc." >&2
    exit 1
fi

BUILT=0
for g in ki1 ki2; do
    if [ "$g" = ki1 ]; then
        CHECK="$DATA_DIR/run/ki-l15d.u98"
        CHECK2="$DATA_DIR/run/ki-l14.u98"
    else
        CHECK="$DATA_DIR/run/ki2-l14.u98"
        CHECK2=""
    fi
    if [ -f "$CHECK" ] || { [ -n "$CHECK2" ] && [ -f "$CHECK2" ]; }; then
        echo "=== Cross-building AArch64 ($CPU, -O2) cache for $g ==="
        ./ultracombo --game "$g" --data-dir "$DATA_DIR" --build-arm64 --build-arm64-cpu "$CPU"
        BUILT=$((BUILT + 1))
    fi
done

if [ "$BUILT" -eq 0 ]; then
    echo "No game ROMs found in $DATA_DIR/run/." >&2
    echo "Put your own MAME kinst and/or kinst2 ROMs + CHD into $DATA_DIR/run/ and re-run." >&2
    exit 1
fi

echo "Done! Copy $DATA_DIR/cache/rec-*-aarch64.so to ports/ultracombo/cache/ on your handheld SD card."
