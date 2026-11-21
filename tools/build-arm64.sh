#!/bin/sh
# Cross-compile UltraCombo (KillerArm) for AArch64 Linux handhelds (Anbernic
# RG34XX, RG35XX Plus/H/SP, RG40XX, RG CubeXX, Powkiddy RGB30, TrimUI Smart Pro)
# targeting glibc 2.28 + OpenGL ES 2.0 + ARM NEON, and assemble a PortMaster zip.
#
# No copyrighted game files or game code ever enter the package.
#
# Usage:
#   tools/build-arm64.sh              # default target CPU: cortex-a53 (H700 / RK3326)
#   TARGET_CPU=cortex_a55 tools/build-arm64.sh
set -e
cd "$(dirname "$0")/.."

deps=$PWD/deps
out=$PWD/dist/UltraCombo-PortMaster-aarch64
zipout=$PWD/dist/UltraCombo-PortMaster-aarch64.zip
bdir=$PWD/build-arm64
TARGET_CPU=${TARGET_CPU:-cortex_a53}
TARGET_TRIPLE=aarch64-linux-gnu.2.28

ZIG=0.17.0
ZIG_LNX_SHA=1cbe9df9f27e6b78d14ccbca43b6703a404ef79ef1c463de901d7f088d4e2026
SDL=2.32.10
SDL_SRC_SHA=5f5993c530f084535c65a6879e9b26ad441169b3e25d789d83287040a9ca5165
TCC_COMMIT=e23dabed154a8495f3ac1ff3cc9d04fa1c207893

mkdir -p "$deps/dl" "$bdir"

fetch() {
    [ -f "$deps/dl/$2" ] || { curl -fsSL --retry 3 -o "$deps/dl/$2.part" "$1" && mv "$deps/dl/$2.part" "$deps/dl/$2"; }
    echo "$3  $deps/dl/$2" | sha256sum -c --quiet || { echo "checksum mismatch: $deps/dl/$2" >&2; exit 1; }
}

# 1. Host Zig compiler (acts as our hermetic aarch64-linux-gnu.2.28 cross-compiler)
if [ ! -x "$deps/zig-linux/zig" ]; then
    echo "[1/5] Fetching Zig $ZIG..."
    fetch "https://ziglang.org/download/$ZIG/zig-x86_64-linux-$ZIG.tar.xz" "zig-x86_64-linux-$ZIG.tar.xz" "$ZIG_LNX_SHA"
    tar -C "$deps" -xJf "$deps/dl/zig-x86_64-linux-$ZIG.tar.xz"
    rm -rf "$deps/zig-linux" && mv "$deps/zig-x86_64-linux-$ZIG" "$deps/zig-linux"
fi
ZIG_BIN="$deps/zig-linux/zig"
export ZIG_GLOBAL_CACHE_DIR="$deps/zig-cache"
export ZIG_LOCAL_CACHE_DIR="$deps/zig-cache"

# 2. Clean SDL2 2.32.10 headers (for target-independent C compilation without host X11 headers)
if [ ! -d "$deps/sdl2-src/SDL2-$SDL/include" ]; then
    echo "[2/5] Fetching SDL2 $SDL headers..."
    fetch "https://github.com/libsdl-org/SDL/releases/download/release-$SDL/SDL2-$SDL.tar.gz" "SDL2-$SDL.tar.gz" "$SDL_SRC_SHA"
    rm -rf "$deps/sdl2-src" && mkdir -p "$deps/sdl2-src"
    tar -C "$deps/sdl2-src" -xzf "$deps/dl/SDL2-$SDL.tar.gz"
fi
SDL_INC="$bdir/inc"
rm -rf "$SDL_INC" && mkdir -p "$SDL_INC/SDL2"
cp "$deps/sdl2-src/SDL2-$SDL/include"/*.h "$SDL_INC/SDL2/"
# Standard Linux LP64 SDL_config.h for AArch64 (OpenGL ES 2.0 + dynamic GL proc loading)
cat > "$SDL_INC/SDL2/SDL_config.h" <<'EOF'
#ifndef SDL_config_h_
#define SDL_config_h_
#include "SDL_platform.h"
#include <stdarg.h>
#include <stdint.h>
#include <stddef.h>
#define SDL_BYTEORDER 1234
#define SIZEOF_VOIDP 8
#define HAVE_STDIO_H 1
#define HAVE_STDLIB_H 1
#define HAVE_STDDEF_H 1
#define HAVE_STDINT_H 1
#define HAVE_STRING_H 1
#define HAVE_STRINGS_H 1
#define HAVE_INTTYPES_H 1
#define HAVE_MATH_H 1
#define HAVE_SIGNAL_H 1
#define HAVE_SYS_TYPES_H 1
#define HAVE_ALLOCA_H 1
#define HAVE_MALLOC 1
#define HAVE_CALLOC 1
#define HAVE_REALLOC 1
#define HAVE_FREE 1
#define HAVE_MEMSET 1
#define HAVE_MEMCPY 1
#define HAVE_MEMMOVE 1
#define HAVE_MEMCMP 1
#define HAVE_STRLEN 1
#define HAVE_STRCHR 1
#define HAVE_STRRCHR 1
#define HAVE_STRSTR 1
#define HAVE_STRCMP 1
#define HAVE_STRNCMP 1
#define HAVE_STRCASECMP 1
#define HAVE_STRNCASECMP 1
#define HAVE_VSNPRINTF 1
#define HAVE_SNPRINTF 1
#define SDL_VIDEO_OPENGL_ES2 1
#define SDL_VIDEO_OPENGL_EGL 1
#endif
EOF

# 3. Generate embedded headers + version.c
UC_VERSION=$(git describe --tags --always --dirty 2>/dev/null | sed 's/^v//' || echo dev)
mkdir -p "$bdir/obj"
printf 'const char uc_version[] = "%s-arm64";\n' "$UC_VERSION" > "$bdir/version.c"
python3 tools/embed.py "$bdir/rec_headers.c" \
    rec_abi_h=src/rec_abi.h rec_gen_h=src/rec_gen.h adsp_core_h=src/adsp_core.h

# 4. Cross-compile all objects and build a link-time-only libSDL2-2.0.so.0 stub
#    (PortMaster devices supply their own hardware-accelerated Mali/KMSDRM SDL2!)
echo "[3/5] Cross-compiling UltraCombo for $TARGET_TRIPLE ($TARGET_CPU, GLES 2.0, NEON)..."
CHDR=src/third_party/libchdr
CHDR_INC="-I$CHDR/include -I$CHDR/deps/lzma-26.02/include -I$CHDR/deps/miniz-3.1.2 -I$CHDR/deps/zstd-1.5.7"
ARM_CFLAGS="-target $TARGET_TRIPLE -mcpu=$TARGET_CPU -O2 -Wall -Wextra -Wno-unused-parameter -DUC_HANDHELD -DUC_GLES -I$SDL_INC -I$SDL_INC/SDL2 -Isrc"

SRC="src/main.c src/cpu.c src/disasm.c src/mem.c src/ide.c src/dcs.c src/adsp.c src/adsprec.c src/rec.c \
     src/frontend.c src/config.c src/osd.c src/png.c src/state.c src/video.c src/shaders.c src/icon.c \
     src/adjust.c src/recomp.c src/rec_build.c src/training.c src/net.c src/netplay.c \
     src/game.c src/game_ki1.c src/game_ki2.c src/picker.c src/controller_labels.c src/launch.c src/rec_none.c"

OBJS=""
for f in $SRC; do
    o="$bdir/obj/$(basename "$f" .c).o"
    "$ZIG_BIN" cc $ARM_CFLAGS -c -o "$o" "$f"
    OBJS="$OBJS $o"
done
"$ZIG_BIN" cc $ARM_CFLAGS -c -o "$bdir/obj/rec_headers.o" "$bdir/rec_headers.c"
"$ZIG_BIN" cc $ARM_CFLAGS -c -o "$bdir/obj/version.o" "$bdir/version.c"
"$ZIG_BIN" cc -target "$TARGET_TRIPLE" -mcpu="$TARGET_CPU" -O2 -w $CHDR_INC -c -o "$bdir/obj/libchdr.o" src/third_party/libchdr_all.c
OBJS="$OBJS $bdir/obj/rec_headers.o $bdir/obj/version.o $bdir/obj/libchdr.o"

# Extract exact undefined SDL_* symbols from compiled objects and build a clean link-time stub
python3 - "$bdir/sdl2_stub.c" $OBJS <<'PYEOF'
import subprocess, sys
out_c = sys.argv[1]
objs = sys.argv[2:]
syms = set()
for o in objs:
    p = subprocess.run(["nm", "-u", "--defined-only=no", o] if False else ["nm", "-u", o],
                       capture_output=True, text=True, check=True)
    for line in p.stdout.splitlines():
        parts = line.split()
        if len(parts) >= 2 and parts[-2] == "U" and parts[-1].startswith("SDL_"):
            syms.add(parts[-1])
with open(out_c, "w") as f:
    f.write("/* Link-time-only stub for PortMaster system libSDL2-2.0.so.0 */\n")
    for s in sorted(syms):
        f.write(f"void {s}(void) {{}}\n")
print(f"Generated SDL2 link stub with {len(syms)} symbols")
PYEOF

"$ZIG_BIN" cc -target "$TARGET_TRIPLE" -shared -Wl,-soname,libSDL2-2.0.so.0 \
    -o "$bdir/libSDL2-2.0.so.0" "$bdir/sdl2_stub.c"

"$ZIG_BIN" cc -target "$TARGET_TRIPLE" -mcpu="$TARGET_CPU" -O2 \
    -o "$bdir/ultracombo.aarch64" $OBJS \
    -L"$bdir" -l:libSDL2-2.0.so.0 -lm -ldl -lpthread

# 5. Cross-compile TinyCC (libtcc.so for AArch64) as zero-dependency on-device fallback
if [ ! -f "$deps/tcc-arm64/libtcc.so" ]; then
    echo "[4/5] Cross-compiling TinyCC (libtcc.so) for $TARGET_TRIPLE..."
    [ -d "$deps/tinycc" ] || git clone -q https://repo.or.cz/tinycc.git "$deps/tinycc"
    git -C "$deps/tinycc" checkout -q "$TCC_COMMIT"
    rm -rf "$deps/tcc-arm64" && cp -r "$deps/tinycc" "$deps/tcc-arm64"
    (
        cd "$deps/tcc-arm64"
        ./configure --cpu=arm64 --os=Linux \
            --cc="$ZIG_BIN cc -target $TARGET_TRIPLE -mcpu=$TARGET_CPU" \
            --ar="$ZIG_BIN ar" >/dev/null
        # c2str runs on the build host to generate tccdefs_.h
        cc -DC2STR conftest.c -o c2str.exe
        make -j"$(nproc)" libtcc.so
    )
fi

# Verify ELF arch, dynamic dependencies and glibc max version (<= 2.28)
echo "[5/5] Verifying AArch64 binaries and assembling PortMaster package..."
for bin in "$bdir/ultracombo.aarch64" "$deps/tcc-arm64/libtcc.so"; do
    file "$bin" | grep -q "ARM aarch64" || { echo "ERROR: $bin is not ARM aarch64" >&2; exit 1; }
    v=$(objdump -T "$bin" | grep -o 'GLIBC_[0-9.]*' | sed 's/GLIBC_//' | sort -uV | tail -1)
    [ -z "$v" ] || [ "$(printf '%s\n2.28\n' "$v" | sort -V | tail -1)" = "2.28" ] || {
        echo "ERROR: $bin requires GLIBC_$v (max allowed 2.28)" >&2
        exit 1
    }
    echo "  OK: $(basename "$bin") -> AArch64, max GLIBC_${v:-none}"
done
readelf -d "$bdir/ultracombo.aarch64" | grep NEEDED

# Verify LP64 ABI parity between x86_64 host and AArch64 target (critical for PC --build-arm64 cache)
cat > "$bdir/abi_check.c" <<'EOF'
#include "rec_abi.h"
#include "adsp_core.h"
#include <stddef.h>
_Static_assert(sizeof(void*) == 8, "LP64");
int abi_sizes[3] = { sizeof(cpu_t), sizeof(tlbc_t), sizeof(adsp_t) };
EOF
"$ZIG_BIN" cc -target "$TARGET_TRIPLE" -Isrc -c -o "$bdir/abi_arm64.o" "$bdir/abi_check.c"
"$ZIG_BIN" cc -target x86_64-linux-gnu.2.28 -Isrc -c -o "$bdir/abi_x64.o" "$bdir/abi_check.c"
python3 - "$bdir/abi_arm64.o" "$bdir/abi_x64.o" <<'PYEOF'
import re, subprocess, sys, struct
def read_sizes(obj):
    out = subprocess.check_output(["readelf", "-x", ".data", obj], text=True)
    hexbytes = []
    for line in out.splitlines():
        m = re.match(r"^\s*0x[0-9a-fA-F]+\s+([0-9a-fA-F ]{8,})", line)
        if m:
            # take only the 4 hex columns (up to 35 chars) before ASCII column
            cols = line.split()[1:5]
            for c in cols:
                if re.fullmatch(r"[0-9a-fA-F]{2,8}", c):
                    hexbytes.append(bytes.fromhex(c))
    raw = b"".join(hexbytes)
    return struct.unpack("<3I", raw[:12])
a, x = read_sizes(sys.argv[1]), read_sizes(sys.argv[2])
print(f"  ABI check: aarch64={a}  x86_64={x} (cpu_t, tlbc_t, adsp_t)")
assert a == x, f"ABI mismatch between aarch64 {a} and x86_64 {x}!"
PYEOF

# Assemble PortMaster directory & zip (zero game files, zero game code!)
rm -rf "$out" "$zipout"
mkdir -p "$out/ultracombo/run" "$out/ultracombo/cache" "$out/ultracombo/tools/tcc" \
         "$out/ultracombo/libs.aarch64" "$out/ultracombo/licenses"

cp portmaster/UltraCombo.sh "$out/UltraCombo.sh"
chmod +x "$out/UltraCombo.sh"
cp portmaster/ultracombo/port.json "$out/ultracombo/"
cp portmaster/ultracombo/gameinfo.xml "$out/ultracombo/"
cp "$bdir/ultracombo.aarch64" "$out/ultracombo/ultracombo.aarch64"
chmod +x "$out/ultracombo/ultracombo.aarch64"
cp "$deps/tcc-arm64/libtcc.so" "$out/ultracombo/tools/tcc/libtcc.so"
cp "$deps/tcc-arm64/COPYING" "$out/ultracombo/licenses/TinyCC-COPYING.txt"
cp LICENSE "$out/ultracombo/licenses/UltraCombo-LICENSE.txt"
cp "$deps/sdl2-src/SDL2-$SDL/LICENSE.txt" "$out/ultracombo/licenses/SDL2-LICENSE.txt"
cp tools/run-readme.txt "$out/ultracombo/run/PUT-GAME-FILES-HERE.txt"
cp README.md "$out/ultracombo/README.md"

# Bundle lightweight permissive CRT shaders (skip heavy desktop-only Lola Retro & xBR on handheld)
mkdir -p "$out/ultracombo/shaders/crt/shaders"
cp shaders/README.txt "$out/ultracombo/shaders/"
for s in crt-1tap crt-2tap crt-nobody crt-lottes-fast crt-hyllian-fast; do
    [ -f "shaders/crt/$s.glslp" ] && cp "shaders/crt/$s.glslp" "$out/ultracombo/shaders/crt/"
done
cp -r shaders/crt/shaders/* "$out/ultracombo/shaders/crt/shaders/" 2>/dev/null || true

# libchdr licences
{
    cat "$CHDR/SOURCE.txt"
    printf '\n==== libchdr ====\n\n';     cat "$CHDR/LICENSE.txt"
    printf '\n==== zstd ====\n\n';        cat "$CHDR/deps/zstd-1.5.7/LICENSE"
    printf '\n==== LZMA SDK ====\n\n';    cat "$CHDR/deps/lzma-26.02/LICENSE"
} > "$out/ultracombo/licenses/libchdr-and-decompressors.txt"
echo "TinyCC source: https://repo.or.cz/tinycc.git, commit e23dabe (cross-compiled unmodified for aarch64)" > "$out/ultracombo/tools/tcc/SOURCE.txt"

if command -v zip >/dev/null 2>&1; then
    (cd "$out" && zip -qr9 "$zipout" UltraCombo.sh ultracombo)
    echo "Packaged: $zipout ($(du -h "$zipout" | cut -f1))"
else
    python3 -c "import shutil; shutil.make_archive('${zipout%.zip}', 'zip', '$out')"
    echo "Packaged: $zipout ($(du -h "$zipout" | cut -f1))"
fi
echo "Ready! Copy dist/UltraCombo-PortMaster-aarch64/* into your handheld's ports/ folder."
