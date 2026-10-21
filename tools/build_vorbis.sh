#!/bin/sh
# Build the standalone Ogg Vorbis decoder (build/vorbis.wasm).
#
# Separate from tools/build.sh on purpose: this module is not part of the game
# and must never become part of it. It is fetched by the page only after
# decodeAudioData has failed on a sound, which on Chrome and Firefox never
# happens. See src/vorbis_wasm.c for why it is not compiled in.
#
# Standalone-wasm mode rather than --target=wasm32 -nostdlib: stb_vorbis needs
# ldexp/exp/log/pow/sin/cos for its IMDCT window, and emscripten's standalone
# mode brings musl's libm (and malloc) without bringing a JS runtime.
#
#   tools/build_vorbis.sh   -> build/vorbis.wasm
set -e
cd "$(dirname "$0")/.."

EMSDK=${EMSDK:-$HOME/emsdk}
EMCC=$EMSDK/upstream/emscripten/emcc
[ -x "$EMCC" ] || { echo "no emscripten at $EMSDK"; exit 1; }

mkdir -p build
# 32 MB up front, growable: the peak is one decoded track (a few minutes of
# stereo float32) plus stb_vorbis' working set.
$EMCC -O2 --no-entry -sSTANDALONE_WASM=1 \
      -sALLOW_MEMORY_GROWTH=1 -sINITIAL_MEMORY=33554432 \
      -sERROR_ON_UNDEFINED_SYMBOLS=0 \
      -sEXPORTED_FUNCTIONS=_spike_decode,_spike_scratch,_spike_out \
      -I third_party/stb \
      src/vorbis_wasm.c -o build/vorbis.wasm

ls -l build/vorbis.wasm
