#!/bin/sh
# Build gl4es for emscripten.
#
# gl4es is the GLES1 -> GLES2 compatibility layer that this port uses instead of
# writing a renderer. See docs/REUSE.md for why, and for what was rejected.
#
# It is not vendored: a checkout is ~104 MB and the repository should not carry
# someone else's tree. This script fetches and builds it into vendor/, which is
# gitignored, so a build is one command and the result is reproducible.
#
#   tools/build_gl4es.sh            # clone if needed, then build
#   tools/build_gl4es.sh --clean    # start over
#
# Requires the emscripten SDK at $EMSDK (default ~/emsdk).
set -e
cd "$(dirname "$0")/.."
ROOT=$(pwd)
VENDOR="$ROOT/vendor"
GL4ES="$VENDOR/gl4es"
REPO="https://github.com/ptitSeb/gl4es.git"
EMSDK=${EMSDK:-$HOME/emsdk}

if [ "$1" = "--clean" ]; then
    rm -rf "$GL4ES"
    echo "removed $GL4ES"
    exit 0
fi

if [ ! -d "$EMSDK/upstream/emscripten" ]; then
    echo "no emscripten SDK at $EMSDK."
    echo "  git clone https://github.com/emscripten-core/emsdk.git ~/emsdk"
    echo "  cd ~/emsdk && ./emsdk install latest && ./emsdk activate latest"
    exit 1
fi
EMCMAKE_TOOLCHAIN="$EMSDK/upstream/emscripten/cmake/Modules/Platform/Emscripten.cmake"
export PATH="$EMSDK/upstream/emscripten:$EMSDK/node/22.16.0_64bit/bin:$PATH"

mkdir -p "$VENDOR"
if [ ! -d "$GL4ES" ]; then
    echo "== fetching gl4es =="
    git clone --depth 1 -q "$REPO" "$GL4ES"
fi
echo "== gl4es: $(git -C "$GL4ES" rev-parse --short HEAD) =="

mkdir -p "$GL4ES/build"
cd "$GL4ES/build"

# DEFAULT_ES=2 is the one flag that differs from gl4es' own documented emscripten
# recipe. Its default backend is GLES 1.1 (for hardware that only has that);
# under emscripten the backend *is* WebGL, i.e. GLES2.
#
# NOEGL=ON means gl4es does not create its own context -- emscripten's GL setup
# does that -- which is also why the host must call set_getmainfbsize() before
# initialize_gl4es(): with no EGL there is nothing to ask for the surface size.
echo "== configuring =="
cmake .. \
    -DCMAKE_TOOLCHAIN_FILE="$EMCMAKE_TOOLCHAIN" \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DNOX11=ON -DNOEGL=ON -DSTATICLIB=ON -DDEFAULT_ES=2 \
    > /dev/null

echo "== building =="
make -j"$(nproc)" > /dev/null

if [ ! -f "$GL4ES/lib/libGL.a" ]; then
    echo "build produced no libGL.a -- check $GL4ES/build"
    exit 1
fi
echo "   -> vendor/gl4es/lib/libGL.a ($(du -h "$GL4ES/lib/libGL.a" | cut -f1))"
