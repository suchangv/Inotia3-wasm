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
# clang resolves the wasm linker as "wasm-ld-<major>" and looks for it on PATH,
# so the directory holding lld has to be on PATH rather than passed through
# -fuse-ld (which does not accept a path to a differently-named binary).
find_wasm_ld() {
    for c in wasm-ld wasm-ld-19 "$HOME/.local/bin/wasm-ld" \
             /usr/lib/llvm-19/bin/wasm-ld /usr/bin/wasm-ld-19; do
        if command -v "$c" >/dev/null 2>&1 || [ -x "$c" ]; then
            dirname "$(command -v "$c" 2>/dev/null || echo "$c")"
            return 0
        fi
    done
    return 1
}

COMMON="src/util.c src/elf_loader.c src/cpu.c src/imports.c src/host_libc.c src/jni.c src/gl.c src/patches.c src/timers.c"

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
    LLDIR=$(find_wasm_ld) || {
        echo "   wasm-ld not found. Install lld, or stage it:"
        echo "     cp \$(dirname \$(command -v ld.lld))/{wasm-ld,lld} ~/.local/bin/"
        echo "     ln -sf wasm-ld ~/.local/bin/wasm-ld-19"
        exit 1
    }
    # clang wants the versioned name, so make sure it exists next to the plain one
    [ -e "$LLDIR/wasm-ld-19" ] || ln -sf "$LLDIR/wasm-ld" "$LLDIR/wasm-ld-19" 2>/dev/null
    PATH="$LLDIR:$PATH"
    export PATH
    mkdir -p build
    # WASM_FREESTANDING drops the libc includes; util.c supplies mem*/str*.
    # The entry is main(), exported so a node driver can call it.
    # The guest address space is 144 MB and is itself a host_alloc(), so the
    # initial memory has to cover that plus the C stack and the loader's own
    # tables. Growing from a smaller start would work but costs a realloc
    # mid-run, which is exactly what a throughput measurement does not want.
    $CC --target=wasm32 -nostdlib $CFLAGS -DWASM_FREESTANDING -I src \
        -Wl,--no-entry \
        -Wl,--export=spike_main -Wl,--export=memory \
        -Wl,--export=spike_alloc -Wl,--export=spike_set_so \
        -Wl,--allow-undefined \
        -Wl,-z,stack-size=$((1024 * 1024)) \
        -Wl,--initial-memory=$((192 * 1024 * 1024)) \
        -Wl,--max-memory=$((1024 * 1024 * 1024)) \
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
