#!/bin/sh
# Build the Android APK.
#
#   tools/build-android.sh                # arm64-v8a + x86_64, debug-signed
#   ABI=arm64-v8a tools/build-android.sh  # one ABI (arm64-v8a, x86_64, armeabi-v7a)
#   tools/build-android.sh --install      # ... and adb install -r it
#
# Output: build/android/inotia3.apk
#
# A NativeActivity with no Java, no dex and no WebView: the C engine is the app.
# The guest libgame.so is *interpreted*, so the same guest image runs on every
# ABI -- x86_64 is not a second port, it is the same binary running in an
# emulator.
#
# Needs, from the Android SDK/NDK (no Gradle):
#   build-tools (aapt2, zipalign, apksigner), platform-34, ndk, and a JDK for
#   apksigner and keytool. ANDROID_SDK_ROOT defaults to ~/android/sdk;
#   JAVA_HOME defaults to ~/android/jdk. See docs/ANDROID.md for the one-time
#   setup.
#
# gl4es (vendor/gl4es/lib/<abi>/libGL.a) is built by tools/build_gl4es.sh
# android; this script does that for you if it is missing.
set -e
cd "$(dirname "$0")/.."
ROOT=$(pwd)

SDK=${ANDROID_SDK_ROOT:-$HOME/android/sdk}
NDK=${ANDROID_NDK_HOME:-$(ls -d "$SDK"/ndk/* 2>/dev/null | tail -1)}
JDK=${JAVA_HOME:-$HOME/android/jdk}
BT=$(ls -d "$SDK"/build-tools/* 2>/dev/null | tail -1)
PLATFORM=$(ls -d "$SDK"/platforms/android-* 2>/dev/null | tail -1)
ABIS=${ABI:-"arm64-v8a x86_64"}
MIN_API=24
OUT=build/android
PKG=inotia3
DO_INSTALL=0
[ "$1" = "--install" ] && DO_INSTALL=1

die() { echo "$*" >&2; exit 1; }

[ -d "$SDK" ] || die "no Android SDK at $SDK (set ANDROID_SDK_ROOT)"
[ -n "$NDK" ] && [ -d "$NDK" ] || die "no Android NDK under $SDK/ndk (sdkmanager --install 'ndk;26.3.11579264')"
[ -n "$BT" ] && [ -x "$BT/aapt2" ] || die "no build-tools under $SDK/build-tools"
[ -n "$PLATFORM" ] && [ -f "$PLATFORM/android.jar" ] || die "no platforms/android-* under $SDK/platforms"
[ -x "$JDK/bin/java" ] || die "no JDK at $JDK (set JAVA_HOME)"
[ -x "$JDK/bin/keytool" ] || die "no keytool in $JDK (a full JDK, not a JRE)"

HOST_TAG=linux-x86_64
TOOLCHAIN="$NDK/toolchains/llvm/prebuilt/$HOST_TAG"
[ -d "$TOOLCHAIN" ] || die "no NDK toolchain at $TOOLCHAIN"
GLUE="$NDK/sources/android/native_app_glue"
[ -f "$GLUE/android_native_app_glue.c" ] || die "no native_app_glue at $GLUE"

# ---- the guest game, which is not in the repository ----------------------
[ -f game/libgame.so ] || die "game/libgame.so missing: this build needs the
guest ELF from your own copy of the APK (see README). It is copyrighted and
never committed."

mkdir -p "$OUT" android/assets
cp -f game/libgame.so android/assets/libgame.so
if [ -f game/assets.apk ]; then
    cp -f game/assets.apk android/assets/assets.apk
else
    echo "   note: game/assets.apk missing -- the engine will boot with no assets"
fi

CC_FLAGS="-std=c11 -O2 -fPIC -Wall -Wextra -Wno-unused-parameter \
-Wno-unused-function -Isrc -Ithird_party/stb -Iandroid -I$GLUE"
SOURCES="src/util.c src/elf_loader.c src/cpu.c src/imports.c src/host_libc.c \
src/jni.c src/gl.c src/gl4es_bridge.c src/patches.c src/timers.c \
src/asset_zip.c src/input.c src/text.c src/text_ttf.c src/sound.c \
src/sound_android.c src/platform_android.c src/main.c \
$GLUE/android_native_app_glue.c"

for abi in $ABIS; do
    case "$abi" in
    arm64-v8a)   CC="$TOOLCHAIN/bin/aarch64-linux-android$MIN_API-clang" ;;
    armeabi-v7a) CC="$TOOLCHAIN/bin/armv7a-linux-androideabi$MIN_API-clang" ;;
    x86_64)      CC="$TOOLCHAIN/bin/x86_64-linux-android$MIN_API-clang" ;;
    *) die "unknown ABI $abi" ;;
    esac
    [ -x "$CC" ] || die "no compiler $CC"

    GL4ES="vendor/gl4es/lib/$abi/libGL.a"
    if [ ! -f "$GL4ES" ]; then
        echo "== gl4es for $abi is missing, building it =="
        ABIS="$abi" tools/build_gl4es.sh android
    fi

    echo "== compiling $abi =="
    mkdir -p "$OUT/$abi"
    # 16 KB page alignment: Android 15+ requires it for the native library, and
    # it costs nothing on older releases.
    $CC $CC_FLAGS -shared -o "$OUT/$abi/lib$PKG.so" $SOURCES "$GL4ES" \
        -Wl,-z,max-page-size=16384 \
        -lEGL -lGLESv2 -lOpenSLES -llog -landroid -ldl -lm
    echo "   -> $OUT/$abi/lib$PKG.so ($(du -h "$OUT/$abi/lib$PKG.so" | cut -f1))"
done

# ---- package -------------------------------------------------------------
echo "== packaging =="
BASE="$OUT/base.apk"
"$BT/aapt2" link -o "$BASE" \
    -I "$PLATFORM/android.jar" \
    --manifest android/AndroidManifest.xml \
    -A android/assets

# The native libraries go in at lib/<abi>/, which is where the loader looks for
# android.app.lib_name. Stored uncompressed for the emulator's sake; zipalign
# fixes the alignment afterwards either way.
STAGE="$OUT/stage"
rm -rf "$STAGE"
for abi in $ABIS; do
    mkdir -p "$STAGE/lib/$abi"
    cp -f "$OUT/$abi/lib$PKG.so" "$STAGE/lib/$abi/"
done
# jar, from the JDK the script already needs, is a ZIP writer. -u *updates* the
# APK aapt2 produced (a plain -c would replace it and lose the manifest), -0
# stores the entries (a compressed library still works with
# extractNativeLibs=true, but stored ones are what a page-aligned one wants), -M
# keeps a manifest out of an APK.
"$JDK/bin/jar" -uf0M "$BASE" -C "$STAGE" lib

ALIGNED="$OUT/aligned.apk"
"$BT/zipalign" -f 4 "$BASE" "$ALIGNED"

# A debug keystore, made once. `adb install` accepts it without any setup; this
# is not a release signing key and the build script says so.
KS="$OUT/debug.keystore"
if [ ! -f "$KS" ]; then
    "$JDK/bin/keytool" -genkeypair -v -keystore "$KS" \
        -alias androiddebugkey -keyalg RSA -keysize 2048 -validity 10000 \
        -storepass android -keypass android \
        -dname "CN=Android Debug,O=Android,C=US" > /dev/null 2>&1
fi

APK="$OUT/$PKG.apk"
"$BT/apksigner" sign --ks "$KS" --ks-pass pass:android --key-pass pass:android \
    --out "$APK" "$ALIGNED"
"$BT/apksigner" verify "$APK"

echo
echo "   APK: $APK ($(du -h "$APK" | cut -f1))"
echo "   install: adb install -r $APK"

if [ "$DO_INSTALL" = 1 ]; then
    ADB="$SDK/platform-tools/adb"
    [ -x "$ADB" ] || ADB=adb
    "$ADB" install -r "$APK"
    "$ADB" shell am start -n io.github.inotia3.native/android.app.NativeActivity
fi
