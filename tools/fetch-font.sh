#!/bin/sh
# Fetch the font the port ships (and that the engine's text needs).
#
#   tools/fetch-font.sh
#
# Why a font has to be shipped at all: the Android build draws text with
# Canvas.drawText against the *phone's* system fonts, and text_ttf.c reimplements
# that with stb_truetype over the device's font directories. This firmware
# (Buildroot) has no fonts at all, so every string came out blank --
# "text: NO usable font at all -- text will be blank" in the log. text_ttf.c
# already looks for ./font.ttf first, so putting one next to the binary is enough;
# no code change is involved.
#
# The font: Noto Sans SC, instanced to a static weight and subset to Latin-1,
# punctuation, fullwidth forms and the CJK ideographs (U+4E00-U+9FA5) -- about
# 7 MB instead of 17, and covers ordinary Chinese game text. License: SIL OFL
# 1.1, so it can be redistributed with the port (see portmaster/FONT-LICENSE.txt).
#
# Needs fonttools (pip install --user fonttools) only if it has to build the
# subset from a variable font; if a suitable .ttf is already in vendor/fonts,
# nothing happens.
set -e
cd "$(dirname "$0")/.."
OUT="vendor/fonts/font.ttf"

if [ -f "$OUT" ]; then
    echo "   $OUT already present"
    exit 0
fi
mkdir -p vendor/fonts

# 1. a static CJK font already on this machine
for f in /usr/share/fonts/truetype/wqy/wqy-microhei.ttc \
         /usr/share/fonts/opentype/noto/NotoSansCJKsc-Regular.otf \
         /usr/share/fonts/font/NotoSansSC-VF.ttf; do
    [ -f "$f" ] && SRC=$f && break
done

# 2. otherwise download one
if [ -z "${SRC:-}" ]; then
    echo "   downloading Noto Sans SC"
    curl -sSL -o vendor/fonts/NotoSansSC-VF.ttf \
        "https://github.com/notofonts/noto-cjk/raw/main/Sans/Variable/TTF/Subset/NotoSansSC-VF.ttf"
    SRC=vendor/fonts/NotoSansSC-VF.ttf
fi

case "$SRC" in
*.ttc)
    # Bitmap-bearing collections do not subset cleanly; take the first face.
    python3 -m fontTools.ttLib.ttCollection --help >/dev/null 2>&1 || true
    cp "$SRC" "$OUT"
    ;;
*VF.ttf)
    echo "   instancing $SRC to wght=400 and subsetting"
    python3 -m fontTools.varLib.instancer "$SRC" wght=400 \
        -o vendor/fonts/NotoSansSC-Regular.ttf > /dev/null
    python3 -m fontTools.subset vendor/fonts/NotoSansSC-Regular.ttf \
        --unicodes="U+0020-007E,U+00A0-00FF,U+2000-206F,U+3000-303F,U+FF00-FFEF,U+4E00-9FA5" \
        --layout-features= --drop-tables+=DSIG --output-file="$OUT" > /dev/null
    rm -f vendor/fonts/NotoSansSC-Regular.ttf
    ;;
*)
    cp "$SRC" "$OUT"
    ;;
esac

echo "   -> $OUT ($(du -h "$OUT" | cut -f1))"
