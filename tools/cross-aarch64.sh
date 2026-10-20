#!/bin/sh
# Print the flags that make the aarch64 build link against an *older* glibc.
#
# PortMaster firmware is frozen: a binary built against this machine's glibc
# (2.41) dies on the device with "GLIBC_2.34 not found". Point
# AARCH64_SYSROOT at an extracted Debian bullseye arm64 sysroot (glibc 2.31) and
# this prints the flags to use it -- which is less obvious than it sounds:
#
#   -nostdinc + -isystem     Debian's cross gcc ignores --sysroot for its
#                            *include* search, so the host's 2.41 headers leak in
#                            and `strtol` becomes `__isoc23_strtol` (a 2.38
#                            symbol). The sysroot's includes have to be forced,
#                            including the compiler's own headers.
#   -B<crt dir>              otherwise crt1.o comes from the host and asks for
#                            __libc_start_main@GLIBC_2.34.
#   -Wl,--sysroot            the sysroot's libc.so is a linker script with
#                            absolute paths (/lib/aarch64-linux-gnu/libc.so.6);
#                            only ld's --sysroot prefixes those.
#
# The sysroot needs one fixup: the multiarch lib symlinks (libdl.so, libm.so,
# libpthread.so, ...) point at absolute /lib paths, which ld cannot prefix, so
# it falls back to the static .a. Make them relative:
#
#   cd $S/usr/lib/aarch64-linux-gnu
#   for l in libdl libpthread librt libm libresolv libutil libanl; do
#       t=$(readlink $l.so); case "$t" in /lib/*) base=$(basename $t); rm -f $l.so;
#           ln -s ../../../lib/aarch64-linux-gnu/$base $l.so;; esac; done
#
# Getting the sysroot without root:
#   curl -O http://deb.debian.org/debian/pool/main/g/glibc/libc6_2.31-13+deb11u11_arm64.deb
#   curl -O http://deb.debian.org/debian/pool/main/g/glibc/libc6-dev_2.31-13+deb11u11_arm64.deb
#   mkdir -p /tmp/sysroot-bullseye && for f in libc6*.deb; do
#       dpkg-deb -x $f /tmp/sysroot-bullseye; done
#
# GL and EGL *headers* are not part of a libc sysroot; point AARCH64_GL_INC at a
# directory holding them (the host's are arch-independent):
#   mkdir -p /tmp/gl-headers && cp -r /usr/include/{GL,GLES,GLES2,GLES3,EGL,KHR} /tmp/gl-headers/

S=${AARCH64_SYSROOT:-}
[ -n "$S" ] || exit 0
GCCINC=$(aarch64-linux-gnu-gcc -print-file-name=include)
echo "-nostdinc -isystem $S/usr/include -isystem $S/usr/include/aarch64-linux-gnu" \
     "-isystem $GCCINC -B$S/usr/lib/aarch64-linux-gnu" \
     "-Wl,--sysroot=$S -L$S/usr/lib/aarch64-linux-gnu -L$S/lib/aarch64-linux-gnu"
