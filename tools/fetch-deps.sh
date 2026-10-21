#!/bin/sh
# Fetch the cross-build dependencies this port needs, without root.
#
#   tools/fetch-deps.sh
#
# Downloads a handful of Debian packages and unpacks them into vendor/deps/
# (gitignored). Nothing here is redistributed: only *headers* and the aarch64
# libraries the linker needs to resolve SDL2's dependencies are used at build
# time; the device supplies the real libraries at run time.
#
# Why packages instead of a sysroot: the host has no aarch64 dev packages, and
# adding a full Debian arm64 sysroot dragged the *old* glibc headers in as well
# (which then fights this host's glibc). Each of these packages ships only its
# own headers -- alsa/, wayland-*.h, xkbcommon/, drm/, gbm.h -- so mixing them
# into one include directory is safe, and it does not shadow libc.
#
# aarch64 side:
#   libasound2-dev, libasound2      ALSA is what SDL2's audio backend uses
#   libwayland-dev, libwayland-{client,cursor,egl}1, libxkbcommon{,-dev}
#                                  weston holds DRM master on this handheld, so
#                                  SDL2 must be built with the wayland backend
# amd64 side (headers only -- KMSDRM's build-time check):
#   libdrm-dev, libgbm-dev
set -e
cd "$(dirname "$0")/.."
ROOT=$(pwd)
DEPS="$ROOT/vendor/deps"
DEBS="$DEPS/.debs"
mkdir -p "$DEBS"
cd "$DEBS"

fetch() {   # fetch <pool-path> <pattern>
    url="http://deb.debian.org/debian/pool/main/$1/"
    name=$(curl -s "$url" | grep -oE "$2" | sort -uV | tail -1)
    [ -n "$name" ] || { echo "cannot find $2 in $url" >&2; exit 1; }
    if [ ! -f "$name" ]; then
        echo "   get $name"
        curl -sSLO "$url$name"
    fi
    dpkg-deb -x "$name" "$DEPS"
}

fetch a/alsa-lib        'libasound2-dev_[^"]*arm64\.deb'
fetch a/alsa-lib        'libasound2_[^"]*arm64\.deb'
fetch w/wayland         'libwayland-dev_[^"]*arm64\.deb'
fetch w/wayland         'libwayland-client0_[^"]*arm64\.deb'
fetch w/wayland         'libwayland-cursor0_[^"]*arm64\.deb'
fetch w/wayland         'libwayland-egl1_[^"]*arm64\.deb'
fetch libx/libxkbcommon 'libxkbcommon-dev_[^"]*arm64\.deb'
fetch libx/libxkbcommon 'libxkbcommon0_[^"]*arm64\.deb'
fetch libd/libdrm       'libdrm-dev_[^"]*amd64\.deb'
fetch m/mesa            'libgbm-dev_[^"]*amd64\.deb'

# GL/EGL/GLES headers are arch-independent, so take them from the host rather
# than guessing an aarch64 package. They go in their own directory: pointing the
# compiler at /usr/include would shadow the aarch64 libc headers with the
# host's, which is how "bits/sys_errlist.h: No such file" happens.
GLINC="$DEPS/gl-include"
mkdir -p "$GLINC"
for d in EGL GLES2 GLES3 KHR; do
    [ -d "/usr/include/$d" ] && cp -r "/usr/include/$d" "$GLINC/" 2>/dev/null || true
done

echo
echo "   -> $DEPS"
echo "      headers: $DEPS/usr/include"
echo "      aarch64: $DEPS/usr/lib/aarch64-linux-gnu"
