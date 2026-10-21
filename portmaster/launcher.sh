#!/bin/sh
# Inotia 3 -- Children of Carnia. PortMaster launcher.
#
# Installed as /roms/ports/K_艾诺迪亚3[中].sh with the port's own directory
# (inotia3/) next to it. The two game files are supplied by the player and never
# shipped:
#
#     inotia3/libgame.so     the guest ELF from the APK (lib/armeabi/libgame.so)
#     inotia3/assets.apk     the APK itself, renamed
#
# Everything is native: the engine is the C interpreter in ./inotia3, gl4es does
# the GLES1 work over the firmware's GLES2 driver, SDL2 owns the window, the pad
# and the sound, and no Java or WebView is involved.
#
# This is a *debug* build of the launcher: it trace-logs (set -x), it writes a
# breadcrumb before it does anything else, and when something goes wrong it
# prints the reason to the console (and to /dev/tty0, which on this class of
# firmware is the screen) and waits ten seconds so the message can be read.
# Silence on a failure is what costs round trips; noise is cheap.

# ---- 1. prove the script ran at all ----------------------------------------
# No variables, no functions, no cd: absolute paths only, and every statement
# tolerated to fail. If neither of these files appears after launching the port,
# the shell never executed this file -- which is a completely different bug
# (execute bit, CRLF line endings, or the menu never finding it).
printf 'inotia3 launcher ran %s\n' "$(date 2>/dev/null)" >>/roms/ports/inotia3-boot.log 2>/dev/null
printf 'inotia3 launcher ran %s\n' "$(date 2>/dev/null)" >>/tmp/inotia3-boot.log 2>/dev/null

# ---- 2. where are we, without needing a successful cd ----------------------
case "$0" in
    */*) SELFDIR=${0%/*} ;;
    *)   SELFDIR=. ;;
esac
[ -d "$SELFDIR" ] || SELFDIR=.
SELFDIR=$(cd "$SELFDIR" 2>/dev/null && pwd) || SELFDIR=.

# A zip unpacked by Windows' "Extract All" lands one level down, and the menu
# entry then sits outside the directory it needs. Look one level deep before
# giving up -- that mistake should not look like a crash.
find_portdir() {
    for d in "$SELFDIR/inotia3" "$SELFDIR"/*/inotia3 "$SELFDIR/inotia3.aarch64"; do
        [ -d "$d" ] || continue
        [ -f "$d/inotia3" ] || [ -f "$d/inotia3.aarch64" ] || continue
        printf '%s\n' "$d"
        return 0
    done
    return 1
}
PORTDIR=$(find_portdir) || PORTDIR="$SELFDIR/inotia3"

# ---- 3. keep the console, then send everything to a log --------------------
HAVE3=
if ( : >&1 ) 2>/dev/null; then
    exec 3>&1 2>/dev/null && HAVE3=1
fi

# The reason, on the screen, for ten seconds. /dev/tty0 is the text console on
# these firmwares; if it cannot be opened this is a no-op. It is written before
# and after sleeping so a repaint cannot swallow it.
show() {
    [ -n "$HAVE3" ] && printf '%s\n' "$*" >&3 2>/dev/null
    if [ -w /dev/tty0 ]; then
        printf '\033[2J\033[H  Inotia 3: %s\n\n  (this message stays for 10 s;\n   a log was written to inotia3-boot.log / inotia3.log)\n' \
            "$*" > /dev/tty0 2>/dev/null
    fi
    return 0
}
fail() {
    show "$*"
    printf '%s\n' "$*"
    [ -n "$HAVE3" ] && sleep 10
    return 1
}

LOG=
for candidate in "$PORTDIR/inotia3.log" "$SELFDIR/inotia3.log" "/tmp/inotia3.log"; do
    if ( : > "$candidate" ) 2>/dev/null; then
        LOG=$candidate
        break
    fi
done
if [ -n "$LOG" ]; then
    exec >> "$LOG" 2>&1
else
    exec > /dev/null 2>&1
fi

printf '%s\n' "--- $(date) ---"
printf '%s\n' "launcher: $0"
printf '%s\n' "selfdir:  $SELFDIR"
printf '%s\n' "portdir:  $PORTDIR"
printf '%s\n' "log:      ${LOG:-<none writable>}"
printf '%s\n' "args:     $*"

# ---- 4. the facts a remote round trip always ends up asking for ------------
printf '%s\n' "kernel:   $(uname -srm 2>/dev/null)"
printf '%s\n' "glibc:    $(ldd --version 2>&1 | head -1)"
printf '%s\n' "os:       $(head -3 /etc/os-release 2>/dev/null | tr '\n' ' ')"
printf '%s\n' "device:   ${DEVICE:-?}  panel: ${DISPLAY_WIDTH:-?}x${DISPLAY_HEIGHT:-?}"
printf '%s\n' "locale:   ${LC_ALL:-${LANG:-?}}  video: ${SDL_VIDEODRIVER:-auto}"
printf '%s\n' "disk:     $(df -h . 2>/dev/null | tail -1)"

# PortMaster's own preamble: the folder has moved between releases, so try the
# known names and fall back to one next to this script. Not being able to find
# it is not fatal -- the port runs without it, it just loses the hotkeys and the
# shared logging.
controlfolder="/roms/ports/PortMaster"
[ -d "$controlfolder" ] || controlfolder="/roms/ports/portmaster"
[ -d "$controlfolder" ] || controlfolder="/roms/ports/PortMaster-master"
[ -d "$controlfolder" ] || controlfolder="$SELFDIR/PortMaster"
if [ -f "$controlfolder/control.txt" ]; then
    . "$controlfolder/control.txt"
fi

# Fills in $DISPLAY_WIDTH/$DISPLAY_HEIGHT for this panel. This port reads the
# pad through SDL2 itself, so no .gptk is shipped and gptokeyb stays out of the
# way (it would grab the controller and inject keys the game cannot use).
get_controls 2>/dev/null || true

# Trace from here on: when the script dies somewhere unexpected, the last
# traced line says where.
set -x

# ---- 5. the port directory ------------------------------------------------
if [ ! -d "$PORTDIR" ]; then
    fail "no port directory next to the launcher (looked for $PORTDIR)"
    printf '%s\n' 'what is next to the launcher:'
    ls -la "$SELFDIR" 2>&1
    printf '%s\n' 'one level down:'
    ls -la "$SELFDIR"/*/ 2>&1 | head -40
    exit 1
fi
cd "$PORTDIR" || { fail "cannot cd to $PORTDIR"; exit 1; }
printf '%s\n' "cwd:      $(pwd)"
printf '%s\n' 'contents:'
ls -la 2>&1
printf '%s\n' 'libs:'
ls -la libs 2>&1

# The binary is shipped as 'inotia3' (the doc's layout says 'inotia3.aarch64');
# accept either, so a rename while testing does not read as a crash.
ENGINE=
for c in ./inotia3 ./inotia3.aarch64; do
    [ -f "$c" ] && ENGINE=$c && break
done
[ -n "$ENGINE" ] || { fail "no engine binary in $PORTDIR (expected ./inotia3)"; exit 1; }

# A zip unpacked on Windows arrives without the execute bits; the loader also
# needs the libraries readable. Fixing our own files is harmless when they are
# already right.
chmod 755 "$ENGINE" 2>/dev/null
chmod 644 libs/*.so* 2>/dev/null

missing=
[ -f libgame.so ] || missing="$missing libgame.so"
[ -f assets.apk ] || missing="$missing assets.apk"
if [ -n "$missing" ]; then
    fail "missing$missing in $PORTDIR"
    printf '%s\n' '  libgame.so  from lib/armeabi/libgame.so inside the APK'
    printf '%s\n' '  assets.apk  the APK itself, renamed'
    exit 1
fi

# The port brings its own SDL2: the firmware's may be any version, and the
# kmsdrm/GL pairing is the one thing a port cannot guess.
LD_LIBRARY_PATH="$PORTDIR/libs:$LD_LIBRARY_PATH"
export LD_LIBRARY_PATH

# Resolve the binary's libraries without running it. This prints the loader's
# own verdict -- "GLIBC_2.38 not found", "libSDL2-2.0.so.0 => not found" -- into
# the log, which is the one thing we cannot otherwise see from the Ports menu.
printf '%s\n' '--- loader check ---'
( LD_TRACE_LOADED_OBJECTS=1 "$ENGINE" ) 2>&1 | sed 's/^/    /'
# The video stack, in one place. On this CFW weston owns the display, so the
# question is always "is there a wayland socket, and can EGL be found".
printf '%s\n' "wayland:  XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR:-unset} WAYLAND_DISPLAY=${WAYLAND_DISPLAY:-unset}"
printf '%s\n' "sockets:  $(ls "${XDG_RUNTIME_DIR:-/run/user/0}" 2>/dev/null | tr '\n' ' ')"
printf '%s\n' "dri:      $(ls /dev/dri 2>/dev/null | tr '\n' ' ')"
printf '%s\n' "gl:       $(ls /usr/lib/libEGL* /usr/lib/libGLES* /usr/lib/libmali* /usr/lib/libwayland* 2>/dev/null | tr '\n' ' ')"
printf '%s\n' "--- video driver probe ---"
"$ENGINE" --vdprobe 2>&1 | sed 's/^/    /'
printf '%s\n' '--- files ---'
ls -l "$ENGINE" libs/libSDL2-2.0.so.0 2>&1

# XDG_RUNTIME_DIR is deliberately left alone: on this firmware a weston session
# owns it, and overwriting it breaks the compositor (black screen, 90 degree
# rotation).
[ -n "$pm_platform_helper" ] && pm_platform_helper "$PORTDIR/$ENGINE"

printf '%s\n' '--- starting ---'
"$ENGINE" "$@"
rc=$?
printf '%s\n' "--- exit $rc ---"
case $rc in
    126) printf '%s\n' '126 = not executable (the card may be mounted noexec)' ;;
    127) printf '%s\n' '127 = the loader could not start the binary: see the loader check above' ;;
esac
[ -n "$pm_finish" ] && pm_finish

# Keep a copy next to the launcher too: that is where a player looks first.
if [ -n "$LOG" ] && [ "$LOG" != "$SELFDIR/inotia3.log" ]; then
    cp -f "$LOG" "$SELFDIR/inotia3.log" 2>/dev/null
fi

if [ $rc -ne 0 ]; then
    printf '%s\n' '--- dmesg (tail) ---'
    dmesg 2>/dev/null | tail -20 | sed 's/^/    /'
    fail "the engine exited with code $rc (details in $LOG)"
fi
exit $rc
