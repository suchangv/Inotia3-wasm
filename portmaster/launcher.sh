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
# This script is written so that it *always* leaves a log behind, including when
# it dies before it can cd, and so that the reason is also printed to whatever
# console the launcher was started from (fd 3). A port that vanishes without a
# trace is undebuggable; that is the whole point of the first thirty lines.

# ---- where we are, without needing a successful cd -------------------------
case "$0" in
    */*) SELFDIR=${0%/*} ;;
    *)   SELFDIR=. ;;
esac
[ -d "$SELFDIR" ] || SELFDIR=.
SELFDIR=$(cd "$SELFDIR" 2>/dev/null && pwd) || SELFDIR=.
PORTDIR="$SELFDIR/inotia3"

# ---- keep the console, then redirect everything into a log -----------------
# fd 3 stays on the original stdout: the firmware shows that on screen (or drops
# it, which is fine), while the log keeps the full transcript.
# Test first in a subshell: a failed "exec 3>&1" with no command exits a
# non-interactive shell, and a launcher killed by its own logging would be a
# spectacular way to lose a debugging round.
HAVE3=
if ( : >&1 ) 2>/dev/null; then
    exec 3>&1 2>/dev/null && HAVE3=1
fi

say() {
    # to the log (fd 1) and, when there is one, to the console
    printf '%s\n' "$*"
    [ -n "$HAVE3" ] && printf '%s\n' "$*" >&3 2>/dev/null
    return 0
}

# Three candidates, in the order the player is likely to look: next to the game
# files, next to the launcher, then /tmp (for a read-only card).
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

say "--- $(date) ---"
say "launcher: $0"
say "selfdir:  $SELFDIR"
say "log:      ${LOG:-<none writable>}"

# ---- the facts a remote debugging round always ends up asking for ----------
# uname/glibc answer "which firmware is this and is my binary even loadable",
# which is exactly what a silent exit usually comes down to.
say "kernel:   $(uname -srm 2>/dev/null)"
say "glibc:    $(ldd --version 2>&1 | head -1)"
say "os:       $(head -3 /etc/os-release 2>/dev/null | tr '\n' ' ')"
say "device:   ${DEVICE:-?}  panel: ${DISPLAY_WIDTH:-?}x${DISPLAY_HEIGHT:-?}"
say "locale:   ${LC_ALL:-${LANG:-?}}  video: ${SDL_VIDEODRIVER:-auto}"

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

# ---- the port directory -----------------------------------------------------
if [ ! -f "$PORTDIR/inotia3" ]; then
    say "Inotia 3: $PORTDIR/inotia3 is missing."
    say "The archive must unpack to:"
    say "    <ports>/K_艾诺迪亚3[中].sh"
    say "    <ports>/inotia3/inotia3"
    say "found next to the launcher:"
    ls -la "$SELFDIR" 2>&1 | sed 's/^/    /'
    [ -d "$PORTDIR" ] && ls -la "$PORTDIR" 2>&1 | sed 's/^/    /'
    [ -n "$HAVE3" ] && sleep 5
    exit 1
fi

cd "$PORTDIR" || {
    say "Inotia 3: cannot cd to $PORTDIR"
    exit 1
}
say "cwd:      $(pwd)"

# A zip unpacked on Windows arrives without the execute bits; the loader also
# needs the libraries readable. Fixing our own files is harmless when they are
# already right.
chmod 755 inotia3 2>/dev/null
chmod 644 libs/*.so* 2>/dev/null

missing=
[ -f libgame.so ] || missing="$missing libgame.so"
[ -f assets.apk ] || missing="$missing assets.apk"
if [ -n "$missing" ]; then
    say "Inotia 3: missing$missing in $(pwd)"
    say "  libgame.so  from lib/armeabi/libgame.so inside the APK"
    say "  assets.apk  the APK itself, renamed"
    [ -n "$HAVE3" ] && sleep 5
    exit 1
fi

# The port brings its own SDL2: the firmware's may be any version, and the
# kmsdrm/GL pairing is the one thing a port cannot guess.
LD_LIBRARY_PATH="$PORTDIR/libs:$LD_LIBRARY_PATH"
export LD_LIBRARY_PATH

# Resolve the binary's libraries without running it. This prints the loader's
# own verdict -- "GLIBC_2.38 not found", "libSDL2-2.0.so.0 => not found" -- into
# the log, which is the one thing we cannot otherwise see from the Ports menu.
say "--- loader check ---"
( LD_TRACE_LOADED_OBJECTS=1 ./inotia3 ) 2>&1 | sed 's/^/    /'
say "--- files ---"
ls -l inotia3 libs/libSDL2-2.0.so.0 2>&1 | sed 's/^/    /'

# XDG_RUNTIME_DIR is deliberately left alone: on this firmware a weston session
# owns it, and overwriting it breaks the compositor (black screen, 90 degree
# rotation).
[ -n "$pm_platform_helper" ] && pm_platform_helper "$PORTDIR/inotia3"

say "--- starting ---"
./inotia3 "$@"
rc=$?
say "--- exit $rc ---"
case $rc in
    126) say "126 = not executable (the card may be mounted noexec)" ;;
    127) say "127 = the loader could not start the binary: look at the loader check above" ;;
esac
[ -n "$pm_finish" ] && pm_finish

# Keep a copy next to the launcher too: that is where a player looks first.
if [ -n "$LOG" ] && [ "$LOG" != "$SELFDIR/inotia3.log" ]; then
    cp -f "$LOG" "$SELFDIR/inotia3.log" 2>/dev/null
fi

if [ $rc -ne 0 ]; then
    say "dmesg (tail):"
    dmesg 2>/dev/null | tail -20 | sed 's/^/    /'
    [ -n "$HAVE3" ] && sleep 5
fi
exit $rc
