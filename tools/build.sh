#!/bin/sh
# Build the spike.
#
# Native first, always: the desktop build is the one you debug under gdb, and
# it is the baseline the wasm32 MIPS number is quoted against. Only once a
# change works native is it worth building wasm.
#
# No emscripten required. clang can target wasm32 directly and lld supplies
# wasm-ld; the core is freestanding (see compat.h), so this stays a one-line
# toolchain dependency instead of a multi-GB SDK install. Emscripten comes back
# into the picture only when the browser host layer (fetch, WebGL, IDBFS) lands.
#
#   tools/build.sh native   -> build/spike
#   tools/build.sh wasm     -> build/spike.wasm
#   tools/build.sh both

set -e
cd "$(dirname "$0")/.."

CC=${CC:-clang}
CFLAGS="-std=c11 -O2 -Wall -Wextra -Wno-unused-parameter -Wno-unused-function"

# lld is not always on PATH; look where Debian puts it and where we stage it.
find_wasm_ld() {
    for c in wasm-ld wasm-ld-19 "$HOME/.local/bin/wasm-ld" \
             /usr/lib/llvm-19/bin/wasm-ld /usr/bin/wasm-ld-19; do
        if command -v "$c" >/dev/null 2>&1 || [ -x "$c" ]; then
            echo "$c"; return 0
        fi
    done
    return 1
}

COMMON="src/util.c src/elf_loader.c src/cpu.c src/imports.c src/host_libc.c"

build_native() {
    echo "== native =="
    mkdir -p build
    $CC $CFLAGS -g -O2 -I src \
        $COMMON src/platform_native.c src/main.c \
        -o build/spike
    echo "   -> build/spike"
}

build_wasm() {
    echo "== wasm32 =="
    LD=$(find_wasm_ld) || {
        echo "   wasm-ld not found. Install lld, or stage it:"
        echo "     cp \$(dirname \$(command -v ld.lld))/{wasm-ld,lld} ~/.local/bin/"
        exit 1
    }
    mkdir -p build
    # WASM_FREESTANDING drops the libc includes; util.c supplies mem*/str*.
    # The entry is main(), exported so a node driver can call it.
    $CC --target=wasm32 -nostdlib $CFLAGS -DWASM_FREESTANDING -I src \
        -Wl,--no-entry -Wl,--export=main -Wl,--export=memory \
        -Wl,--allow-undefined \
        -Wl,--initial-memory=$((144 * 1024 * 1024)) \
        -Wl,--max-memory=$((512 * 1024 * 1024)) \
        -fuse-ld="$LD" \
        $COMMON src/platform_wasm.c src/main.c \
        -o build/spike.wasm
    echo "   -> build/spike.wasm"
}

case "${1:-both}" in
    native) build_native ;;
    wasm)   build_wasm ;;
    both)   build_native; build_wasm ;;
    *) echo "usage: $0 [native|wasm|both]"; exit 2 ;;
esac
