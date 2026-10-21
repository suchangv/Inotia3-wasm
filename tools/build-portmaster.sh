#!/bin/sh
# Package the SDL build as a PortMaster port.
#
#   tools/build-portmaster.sh            # aarch64 binary + libs + launcher
#   tools/build-portmaster.sh --zip      # ... and a .zip for /roms/ports
#
# Output, matching the layout the firmware expects (and the Hollow Knight port
# next to it):
#
#   dist/K_艾诺迪亚3[中].sh          the launcher the Ports menu shows
#   dist/inotia3/                    the port's own directory
#       inotia3                      aarch64 binary, gl4es linked in
#       libs/libSDL2-2.0.so.0        the SDL2 it is built against
#       port.json  README.txt  data/
#
# The game's two files are NOT packaged (they are Com2uS' work): the launcher
# checks for them and says where they go.
set -e
cd "$(dirname "$0")/.."
ROOT=$(pwd)
NAME='K_艾诺迪亚3[中]'
PORTDIR=inotia3
OUT="dist/$PORTDIR"

[ -x build/inotia3-aarch64 ] || {
    echo "build the handheld binary first: tools/build.sh handheld"; exit 1; }
[ -f vendor/SDL2/dist-aarch64/lib/libSDL2-2.0.so.0 ] || {
    echo "no aarch64 SDL2: tools/build_sdl2.sh aarch64"; exit 1; }

rm -rf "$OUT" "dist/$NAME.sh"
mkdir -p "$OUT/libs" "$OUT/data"

cp build/inotia3-aarch64 "$OUT/inotia3"
strip "$OUT/inotia3" 2>/dev/null || true
# As a *file* named with the SONAME the binary asks for, not a symlink: a zip
# does not preserve symlinks, and the ld.so name is the only one that matters at
# runtime.
cp -f vendor/SDL2/dist-aarch64/lib/libSDL2-2.0.so.0.3000.9 \
      "$OUT/libs/libSDL2-2.0.so.0"

# The engine draws its text through a font; this firmware has none.
if [ -f vendor/fonts/font.ttf ]; then
    cp -f vendor/fonts/font.ttf "$OUT/font.ttf"
else
    echo "   warning: no vendor/fonts/font.ttf -- text will be blank (tools/fetch-font.sh)"
fi
cp -f portmaster/FONT-LICENSE.txt "$OUT/FONT-LICENSE.txt" 2>/dev/null || true

cp portmaster/port.json "$OUT/port.json"
cp portmaster/README.txt "$OUT/README.txt"

# The launcher is installed next to the directory, and its @PORTDIR@ becomes the
# real directory name.
sed "s/@PORTDIR@/$PORTDIR/g" portmaster/launcher.sh > "dist/$NAME.sh"
chmod +x "dist/$NAME.sh"

echo "   -> dist/$NAME.sh"
echo "   -> $OUT/  ($(du -sh "$OUT" | cut -f1))"
echo
echo "   Copy dist/$NAME.sh and $OUT/ into /roms/ports/, then put"
echo "   libgame.so and assets.apk in /roms/ports/$PORTDIR/ ."

if [ "$1" = "--zip" ]; then
    if command -v zip > /dev/null 2>&1; then
        ( cd dist && zip -qr "$NAME.zip" "$NAME.sh" "$PORTDIR" )
    else
        # python3 is a build dependency of this repo already (tools/difftest.py).
        python3 - "$NAME" "$PORTDIR" <<'PYZIP'
import os, sys, zipfile
name, portdir = sys.argv[1], sys.argv[2]
os.chdir("dist")
with zipfile.ZipFile(name + ".zip", "w", zipfile.ZIP_DEFLATED) as z:
    z.write(name + ".sh")
    for root, _dirs, files in os.walk(portdir):
        for f in files:
            z.write(os.path.join(root, f))
PYZIP
    fi
    echo "   -> dist/$NAME.zip ($(du -h "dist/$NAME.zip" | cut -f1))"
fi
