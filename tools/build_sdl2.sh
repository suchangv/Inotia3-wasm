#!/bin/sh
# Build SDL2 (the version this port is developed against) for one target.
#
#   tools/build_sdl2.sh native     # x86_64, X11 + offscreen, for the desktop
#   tools/build_sdl2.sh aarch64    # the handheld: KMSDRM + offscreen
#
# SDL2 is not vendored (the tarball is ~7 MB and the repository should not carry
# someone else's tree); this fetches it into vendor/, which is gitignored, like
# tools/build_gl4es.sh does for gl4es.
#
# Why the port brings its own SDL2 instead of using the firmware's: the CFW
# ships whatever version its launcher was built against, and a port that links
# `-lSDL2` from /usr/lib is at the mercy of that. Shipping one known-good
# libSDL2-2.0.so.0 in libs/ is what PortMaster ports do for the same reason.
#
# KMSDRM needs libdrm's headers and library. On the build machine that is
# `libdrm-dev`; without root, fetch the .deb and extract it:
#
#   curl -O http://deb.debian.org/debian/pool/main/libd/libdrm/libdrm-dev_2.4.124-2_amd64.deb
#   curl -O http://deb.debian.org/debian/pool/main/libd/libdrm/libdrm2_2.4.124-2_arm64.deb
#   dpkg-deb -x libdrm-dev_*.deb  /tmp/sysroot-amd64
#   dpkg-deb -x libdrm2_*arm64.deb /tmp/sysroot-arm64
#
# and point SYSROOT_DRM_H / SYSROOT_DRM_LIB at them.
set -e
cd "$(dirname "$0")/.."
ROOT=$(pwd)
SDL="$ROOT/vendor/SDL2"
VER=2.30.9
TARGET=${1:-native}
DRM_H=${SYSROOT_DRM_H:-/usr/include}
DRM_LIB=${SYSROOT_DRM_LIB:-/usr/lib/aarch64-linux-gnu}
# AARCH64_SYSROOT: build the handheld binary against an *older* glibc than the
# host's. PortMaster firmware ships whatever glibc its distro froze years ago,
# and a binary linked against this Debian's 2.41 dies with "GLIBC_2.34 not
# found" on the device. Point this at an extracted Debian bullseye arm64
# sysroot (glibc 2.31) -- see the header for how to get one without root.
SYSROOT=${AARCH64_SYSROOT:-}
# GL/EGL *headers* are not part of the libc sysroot, and SDL2's build and this
# port both compile against them. Copy the host's (they are arch-independent
# declarations) into one directory and point this at it:
#   mkdir -p /tmp/gl && cp -r /usr/include/{GL,GLES,GLES2,GLES3,EGL,KHR} /tmp/gl/
GLINC=${AARCH64_GL_INC:-}
# Optional: an extracted aarch64 libasound2-dev, so SDL2 has an audio backend.
ALSA_INC=${ALSA_INCLUDE_DIR:-}
ALSA_LIB=${ALSA_LIBRARY:-}

mkdir -p "$ROOT/vendor"
if [ ! -d "$SDL" ]; then
    echo "== fetching SDL2 $VER =="
    curl -sSL -o "$ROOT/vendor/sdl2.tar.gz" \
        "https://github.com/libsdl-org/SDL/releases/download/release-$VER/SDL2-$VER.tar.gz"
    tar xzf "$ROOT/vendor/sdl2.tar.gz" -C "$ROOT/vendor"
    mv "$ROOT/vendor/SDL2-$VER" "$SDL"
fi
echo "== SDL2: $(cat "$SDL/include/SDL_version.h" | grep SDL_MINOR_VERSION | head -1) =="

SYSFLAGS=$("$ROOT/tools/cross-aarch64.sh")
case "$TARGET" in
native)
    BUILD="$SDL/build-native"
    INSTALL="$SDL/dist-native"
    cmake -S "$SDL" -B "$BUILD" -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="$INSTALL" \
        -DSDL_STATIC=OFF -DSDL_SHARED=ON -DSDL_TEST=OFF \
        -DSDL_X11=ON -DSDL_WAYLAND=OFF -DSDL_KMSDRM=OFF \
        > /dev/null
    ;;
aarch64)
    BUILD="$SDL/build-aarch64"
    INSTALL="$SDL/dist-aarch64"
    # KMSDRM is gated on pkg-config finding libdrm, gbm and egl. The device
    # dlopens libdrm/gbm at runtime (SDL_KMSDRM_SHARED), so only the *headers*
    # and the .pc files are needed at build time -- generate them rather than
    # depending on aarch64 dev packages that do not exist here.
    mkdir -p "$ROOT/vendor/pkgconfig"
    printf 'prefix=%s\nincludedir=%s\n\nName: libdrm\nDescription: libdrm\nVersion: 2.4.124\nCflags: -I${includedir} -I${includedir}/libdrm\nLibs: -ldrm\n' \
        "${DRM_H%/include}" "$DRM_H" > "$ROOT/vendor/pkgconfig/libdrm.pc"
    printf 'prefix=%s\nincludedir=%s\n\nName: gbm\nDescription: gbm\nVersion: 22.3\nCflags: -I${includedir}\nLibs: -lgbm\n' \
        "${DRM_H%/include}" "$DRM_H" > "$ROOT/vendor/pkgconfig/gbm.pc"
    PKG_CONFIG_PATH="$ROOT/vendor/pkgconfig:$PKG_CONFIG_PATH"
    export PKG_CONFIG_PATH
    # A symlink is enough for the linker; the device only needs libdrm.so.2.
    mkdir -p "$ROOT/vendor/sysroot-aarch64/lib"
    [ -e "$ROOT/vendor/sysroot-aarch64/lib/libdrm.so" ] || \
        ln -sf "$DRM_LIB/libdrm.so.2" "$ROOT/vendor/sysroot-aarch64/lib/libdrm.so"
    cmake -S "$SDL" -B "$BUILD" -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="$INSTALL" \
        -DCMAKE_C_COMPILER=aarch64-linux-gnu-gcc \
        -DCMAKE_C_FLAGS="-I$DRM_H -I$DRM_H/libdrm ${GLINC:+-I$GLINC} $SYSFLAGS" \
        -DCMAKE_EXE_LINKER_FLAGS="$SYSFLAGS -L$ROOT/vendor/sysroot-aarch64/lib" \
        -DCMAKE_SHARED_LINKER_FLAGS="$SYSFLAGS -L$ROOT/vendor/sysroot-aarch64/lib" \
        -DSDL_STATIC=OFF -DSDL_SHARED=ON -DSDL_TEST=OFF \
        -DSDL_X11=OFF -DSDL_WAYLAND=OFF -DSDL_KMSDRM=ON \
        -DSDL_VULKAN=OFF -DSDL_ALSA=ON -DSDL_PULSEAUDIO=OFF \
        -DSDL_JACK=OFF -DSDL_PIPEWIRE=OFF \
        ${ALSA_INC:+-DALSA_INCLUDE_DIR=$ALSA_INC} \
        ${ALSA_LIB:+-DALSA_LIBRARY=$ALSA_LIB} \
        > /dev/null
    ;;
*)
    echo "usage: $0 [native|aarch64]"
    exit 2
    ;;
esac

echo "== building =="
cmake --build "$BUILD" > /dev/null
cmake --install "$BUILD" > /dev/null
ls -la "$INSTALL/lib/libSDL2"*.so* | head -3
