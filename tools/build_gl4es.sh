#!/bin/sh
# Build gl4es, the GLES1 -> GLES2 layer this port renders through.
#
#   tools/build_gl4es.sh                # emscripten, for the browser build
#   tools/build_gl4es.sh android        # the NDK, for the APK (arm64 + x86_64)
#   ABIS="arm64-v8a" tools/build_gl4es.sh android
#   tools/build_gl4es.sh --clean
#
# gl4es is not vendored: a checkout is ~104 MB and the repository should not
# carry someone else's tree. This script fetches and builds it into vendor/,
# which is gitignored, so a build is one command and the result is reproducible.
# See docs/REUSE.md for why gl4es and what was rejected.
#
# Everything gl4es needs from its host is in src/gl4es_bridge.c: a current GL
# context (created by the platform, because gl4es is built with NOEGL) and a
# getprocaddress that can resolve the GLES2 entry points gl4es calls.
set -e
cd "$(dirname "$0")/.."
ROOT=$(pwd)
VENDOR="$ROOT/vendor"
GL4ES="$VENDOR/gl4es"
REPO="https://github.com/ptitSeb/gl4es.git"
TARGET=${1:-emscripten}
EMSDK=${EMSDK:-$HOME/emsdk}
SDK=${ANDROID_SDK_ROOT:-$HOME/android/sdk}
ABIS=${ABIS:-"arm64-v8a x86_64"}

if [ "$TARGET" = "--clean" ]; then
    rm -rf "$GL4ES"
    echo "removed $GL4ES"
    exit 0
fi

mkdir -p "$VENDOR"
if [ ! -d "$GL4ES" ]; then
    echo "== fetching gl4es =="
    git clone --depth 1 -q "$REPO" "$GL4ES"
fi
echo "== gl4es: $(git -C "$GL4ES" rev-parse --short HEAD) =="

case "$TARGET" in
emscripten)
    if [ ! -d "$EMSDK/upstream/emscripten" ]; then
        echo "no emscripten SDK at $EMSDK."
        echo "  git clone https://github.com/emscripten-core/emsdk.git ~/emsdk"
        echo "  cd ~/emsdk && ./emsdk install latest && ./emsdk activate latest"
        exit 1
    fi
    EMCMAKE_TOOLCHAIN="$EMSDK/upstream/emscripten/cmake/Modules/Platform/Emscripten.cmake"
    export PATH="$EMSDK/upstream/emscripten:$EMSDK/node/22.16.0_64bit/bin:$PATH"

    # Its own build directory and its own copy of the archive: gl4es' CMake
    # writes lib/libGL.a, and the Android builds write the same path. Sharing it
    # is how the browser build ended up linking an x86_64 archive
    # ("undefined symbol: gl4es_glViewport" from wasm-ld).
    mkdir -p "$GL4ES/build-emscripten"
    cd "$GL4ES/build-emscripten"
    # DEFAULT_ES=2 is the one flag that differs from gl4es' own documented
    # emscripten recipe. Its default backend is GLES 1.1 (for hardware that only
    # has that); under emscripten the backend *is* WebGL, i.e. GLES2.
    #
    # NOEGL=ON means gl4es does not create its own context -- the host's GL
    # setup does that -- which is also why the host must call
    # set_getmainfbsize() before initialize_gl4es(): with no EGL there is
    # nothing to ask for the surface size.
    echo "== configuring =="
    cmake .. \
        -DCMAKE_TOOLCHAIN_FILE="$EMCMAKE_TOOLCHAIN" \
        -DCMAKE_BUILD_TYPE=RelWithDebInfo \
        -DNOX11=ON -DNOEGL=ON -DSTATICLIB=ON -DDEFAULT_ES=2 \
        > /dev/null
    echo "== building =="
    make -j"$(nproc)" > /dev/null
    if [ ! -f "$GL4ES/lib/libGL.a" ]; then
        echo "build produced no libGL.a -- check $GL4ES/build-emscripten"
        exit 1
    fi
    mkdir -p "$GL4ES/lib/emscripten"
    cp "$GL4ES/lib/libGL.a" "$GL4ES/lib/emscripten/libGL.a"
    echo "   -> vendor/gl4es/lib/emscripten/libGL.a ($(du -h "$GL4ES/lib/emscripten/libGL.a" | cut -f1))"
    ;;

android)
    NDK=${ANDROID_NDK_HOME:-$(ls -d "$SDK"/ndk/* 2>/dev/null | tail -1)}
    if [ -z "$NDK" ] || [ ! -d "$NDK" ]; then
        echo "no Android NDK. Install it with:"
        echo "  sdkmanager --install 'ndk;26.3.11579264'"
        echo "and set ANDROID_SDK_ROOT (default ~/android/sdk)."
        exit 1
    fi
    TOOLCHAIN="$NDK/build/cmake/android.toolchain.cmake"
    [ -f "$TOOLCHAIN" ] || { echo "no $TOOLCHAIN"; exit 1; }
    echo "== NDK: $NDK =="

    for abi in $ABIS; do
        echo "== gl4es: $abi =="
        mkdir -p "$GL4ES/build-android-$abi"
        cd "$GL4ES/build-android-$abi"
        # ANDROID=ON is gl4es' own switch: it adds -DNOX11 -DNO_GBM -DDEFAULT_ES=2.
        # NOEGL keeps gl4es out of the context business (the app creates the
        # context), USE_ANDROID_LOG sends its errors to logcat.
        # NO_INIT_CONSTRUCTOR is not optional here. gl4es makes
        # initialize_gl4es() a constructor(101) by default, which on Android runs
        # when the .so is dlopen'd -- before this app has set the backend lookup
        # or the framebuffer size, so GetHardwareExtensions() calls through a
        # still-null gles_glGetString and the process dies in the loader with
        # "SIGSEGV at 0x0" and a stack that names no guest code at all. The
        # port calls initialize_gl4es() itself, after set_getprocaddress() and
        # set_getmainfbsize(); see gl4es_bridge_init().
        cmake .. \
            -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" \
            -DANDROID_ABI="$abi" \
            -DANDROID_PLATFORM=android-24 \
            -DANDROID=ON -DNOX11=ON -DNOEGL=ON -DSTATICLIB=ON -DDEFAULT_ES=2 \
            -DNO_INIT_CONSTRUCTOR=ON -DUSE_ANDROID_LOG=ON \
            -DCMAKE_BUILD_TYPE=Release \
            > /dev/null
        make -j"$(nproc)" > /dev/null
        mkdir -p "$GL4ES/lib/$abi"
        cp "$GL4ES/lib/libGL.a" "$GL4ES/lib/$abi/libGL.a"
        echo "   -> vendor/gl4es/lib/$abi/libGL.a ($(du -h "$GL4ES/lib/$abi/libGL.a" | cut -f1))"
    done
    ;;

*)
    echo "usage: $0 [emscripten|android|--clean]"
    exit 2
    ;;
esac
