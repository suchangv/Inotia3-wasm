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
#   tools/build.sh wasm     -> build/spike_freestanding.wasm
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

COMMON="src/util.c src/elf_loader.c src/cpu.c src/imports.c src/host_libc.c src/jni.c src/gl.c src/patches.c src/timers.c src/asset_zip.c src/input.c src/text.c"
COMMON_BROWSER="$COMMON"

build_native() {
    echo "== native =="
    mkdir -p build
    $CC $CFLAGS -g -O2 -I src \
        $COMMON src/platform_native.c src/main.c \
        -lm -o build/spike
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
        -o build/spike_freestanding.wasm
    echo "   -> build/spike_freestanding.wasm"
}

# The browser build: emscripten plus gl4es, which is the real port. See
# docs/REUSE.md. Needs vendor/gl4es/lib/libGL.a from tools/build_gl4es.sh.
#
#   tools/build.sh browser          -> build/spike.html (+ .js, .wasm, .data)
#   ... then serve it and open the page.
build_browser() {
    echo "== browser (emscripten) =="
    EMSDK=${EMSDK:-$HOME/emsdk}
    EMCC=$EMSDK/upstream/emscripten/emcc
    [ -x "$EMCC" ] || { echo "   no emscripten at $EMSDK"; exit 1; }
    GL4ES=vendor/gl4es/lib
    [ -f "$GL4ES/libGL.a" ] || {
        echo "   no gl4es build. Run tools/build_gl4es.sh first."
        exit 1
    }
    mkdir -p build
    # gl4es must be named by path, not with -lGL: emscripten ships its own
    # libGL (the GLES2 -> WebGL mapping) in its sysroot and -lGL finds that.
    #
    # --preload-file puts the game image and assets into the emscripten
    # filesystem, so the loader's fopen sees them as ordinary files.
    #
    # FULL_ES2 supplies client-side vertex arrays, which the engine uses and
    # which the WebGL-compatible GLES2 subset excludes by default.
    # --preload-file keeps the source path, so the image would land at
    # game/libgame.so; the "@" form places it where the loader looks.
    # The .apk is the asset archive the engine reads resources out of, at byte
    # offsets (see src/asset_zip.c). 22 MB, and --preload-file puts it in the
    # emscripten filesystem where fopen finds it.
    PRELOAD="--preload-file game/libgame.so@libgame.so"
    if [ -f game/assets.apk ]; then
        PRELOAD="$PRELOAD --preload-file game/assets.apk@assets.apk"
    else
        echo "   note: game/assets.apk missing -- the engine will find no assets"
    fi
    for d in ${ASSET_DIRS:-}; do
        if [ -d "$d" ]; then PRELOAD="$PRELOAD --preload-file $d"; fi
    done

    # -sSTACK_SIZE: emscripten defaults the *host* stack to 64 KB. gl4es and
    # the shims recurse well past that (the first fault here was "Attempt to
    # set SP to 0x770a0, with stack limits [0x77f30 - 0x87f30]" -- an overflow
    # with no guest involvement at all), and the symptom is a trap that looks
    # like a guest bug. 8 MB, the same as the guest stack.
    #
    # --pre-js: a trap prints only "memory access out of bounds". The Error
    # carries a wasm stack naming the function; tools/trap.js is what prints
    # it. Debug builds add --profiling-funcs so those frames have names.
    DBG=""
    [ -n "$SPIKE_DEBUG" ] && DBG="-g --profiling-funcs -sASSERTIONS=2"
    # The output basename owns the whole browser triple: -o build/spike.html
    # makes emscripten emit build/spike.{js,wasm,data} alongside it. The
    # freestanding target therefore must NOT be called build/spike.wasm, or
    # whichever build ran last silently replaces the other's module and the
    # page dies at instantiation with "Import #2 host: module is not an object
    # or function". See build_wasm(), which writes spike_freestanding.wasm.
    $EMCC -O2 -I src $DBG --pre-js tools/trap.js --pre-js tools/page-options.js \
        $COMMON_BROWSER src/gl4es_bridge.c src/platform_emscripten.c src/main.c \
        "$GL4ES/libGL.a" \
        -sFULL_ES2=1 -sALLOW_MEMORY_GROWTH=1 -sSTACK_SIZE=8388608 \
        -sMIN_WEBGL_VERSION=2 -sMAX_WEBGL_VERSION=2 \
        -sEXIT_RUNTIME=0 \
        -sEXPORTED_RUNTIME_METHODS=ccall,cwrap \
        -sEXPORTED_FUNCTIONS=_main,_spike_set_frames,_spike_set_dump_screen,_spike_set_gl_trace,_spike_set_fixed,_spike_set_frame_ms,_spike_input_down,_spike_input_move,_spike_input_up,_spike_input_release_all \
        $PRELOAD \
        -o build/spike.html
    echo "   -> build/spike.html (serve this directory and open the page)"
}

case "${1:-both}" in
    native)  build_native ;;
    wasm)    build_wasm ;;
    browser) build_browser ;;
    both)    build_native; build_wasm ;;
    all)     build_native; build_wasm; build_browser ;;
    *) echo "usage: $0 [native|wasm|browser|both|all]"; exit 2 ;;
esac
