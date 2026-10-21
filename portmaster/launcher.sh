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
# The log is short on purpose: the version that had to be debugged printed
# everything (set -x, directory listings, a probe of every video driver), and
# none of that belongs in a port that works. SPIKE_DEBUG=1 brings it back.

SELFDIR=$(cd "$(dirname "$0")" 2>/dev/null && pwd) || SELFDIR=.
PORTDIR="$SELFDIR/inotia3"
DEBUG=${SPIKE_DEBUG:-}

# A zip unpacked by Windows' "Extract All" lands one level down; look one level
# deep before concluding the install is broken.
if [ ! -f "$PORTDIR/inotia3" ] && [ ! -f "$PORTDIR/inotia3.aarch64" ]; then
    for d in "$SELFDIR"/*/inotia3; do
        if [ -f "$d/inotia3" ] || [ -f "$d/inotia3.aarch64" ]; then
            PORTDIR=$d
            break
        fi
    done
fi

# Log location: next to the game files, and a copy next to the launcher, which
# is where a player looks first.
LOG="$PORTDIR/inotia3.log"
{ : > "$LOG"; } 2>/dev/null || LOG="$SELFDIR/inotia3.log"

# Keep fd 3 on the console so a fatal message is also visible on whatever
# started us, then send everything to the log. exec, not tee: tee loses its tail
# on SIGKILL, which is exactly when the tail matters.
HAVE3=
if ( : >&1 ) 2>/dev/null; then
    exec 3>&1 2>/dev/null && HAVE3=1
fi
exec >> "$LOG" 2>&1

say() {
    printf '%s\n' "$*"
    [ -n "$HAVE3" ] && printf '%s\n' "$*" >&3 2>/dev/null
    return 0
}
# A failure the player has to see: file evidence plus ten seconds on screen.
fail() {
    say "Inotia 3: $*"
    if [ -w /dev/tty0 ]; then
        printf '\033[2J\033[H  Inotia 3: %s\n\n  (details in inotia3.log)\n' \
            "$*" > /dev/tty0 2>/dev/null
    fi
    [ -n "$HAVE3" ] && sleep 10
    exit 1
}

say "--- $(date) ---"
[ -n "$DEBUG" ] && set -x

if [ ! -d "$PORTDIR" ]; then
    fail "no inotia3 directory next to the launcher"
fi
cd "$PORTDIR" || fail "cannot enter $PORTDIR"

ENGINE=
for c in ./inotia3 ./inotia3.aarch64; do
    [ -f "$c" ] && ENGINE=$c && break
done
[ -n "$ENGINE" ] || fail "no engine binary in this directory"

# A zip unpacked on Windows loses the execute bits.
chmod 755 "$ENGINE" 2>/dev/null
chmod 644 libs/*.so* 2>/dev/null

missing=
[ -f libgame.so ] || missing="$missing libgame.so"
[ -f assets.apk ] || missing="$missing assets.apk"
if [ -n "$missing" ]; then
    say "  libgame.so  from lib/armeabi/libgame.so inside the APK"
    say "  assets.apk  the APK itself, renamed"
    fail "missing$missing in $PORTDIR"
fi

# Optional per-install overrides, one KEY=VALUE per line: SPIKE_ROTATE=270 to
# turn the picture the other way, SDL_VIDEODRIVER=wayland to force a backend,
# SPIKE_LANG=zh-CN, SPIKE_DEBUG=1. Editing a text file beats a new download to
# try a value.
if [ -f env.txt ]; then
    while IFS= read -r line; do
        case "$line" in ''|'#'*) continue ;; esac
        export "$line" 2>/dev/null || say "  env.txt: ignoring '$line'"
    done < env.txt
fi

# PortMaster's preamble, in the order its own ports use.
controlfolder="/roms/ports/PortMaster"
[ -d "$controlfolder" ] || controlfolder="/roms/ports/portmaster"
[ -d "$controlfolder" ] || controlfolder="/roms/ports/PortMaster-master"
[ -d "$controlfolder" ] || controlfolder="$SELFDIR/PortMaster"
if [ -f "$controlfolder/control.txt" ]; then
    . "$controlfolder/control.txt"
fi
get_controls 2>/dev/null || true

# The port brings its own SDL2; the firmware's may be any version.
LD_LIBRARY_PATH="$PORTDIR/libs:$LD_LIBRARY_PATH"
export LD_LIBRARY_PATH

say "device: ${DEVICE_NAME:-?}  panel: ${DISPLAY_WIDTH:-?}x${DISPLAY_HEIGHT:-?}  locale: ${LC_ALL:-${LANG:-?}}"

# Pick a video backend: ask SDL which ones can actually produce a window, and
# use the first (wayland, then X11, then KMSDRM). Drivers that need DRM master
# are skipped by the probe itself while a compositor owns the display, so this
# never disturbs weston.
if [ -z "${SDL_VIDEODRIVER:-}" ]; then
    probe=$("$ENGINE" --vdprobe 2>&1)
    SDL_VIDEODRIVER=$(printf '%s\n' "$probe" \
        | grep -E '^[[:space:]]*[A-Za-z0-9_]+[[:space:]]+.*window ok' \
        | head -1 | awk '{print $1}')
    if [ -n "$DEBUG" ]; then
        printf '%s\n' "$probe" | sed 's/^/    /'
    elif [ -z "$SDL_VIDEODRIVER" ]; then
        say "no video backend could create a window:"
        printf '%s\n' "$probe" | sed 's/^/    /'
    fi
    [ -n "$SDL_VIDEODRIVER" ] && export SDL_VIDEODRIVER
fi
say "video: ${SDL_VIDEODRIVER:-auto}"

[ -n "$pm_platform_helper" ] && pm_platform_helper "$PORTDIR/$ENGINE"

say "--- starting ---"
"$ENGINE" "$@"
rc=$?
say "--- exit $rc ---"
[ -n "$pm_finish" ] && pm_finish

if [ -n "$LOG" ] && [ "$LOG" != "$SELFDIR/inotia3.log" ]; then
    cp -f "$LOG" "$SELFDIR/inotia3.log" 2>/dev/null
fi

if [ $rc -ne 0 ]; then
    say "dmesg (tail):"
    dmesg 2>/dev/null | tail -15 | sed 's/^/    /'
    case $rc in
        126) say "126 = not executable (card mounted noexec?)" ;;
        127) say "127 = the loader could not start the binary; its own verdict:"
             ( LD_TRACE_LOADED_OBJECTS=1 "$ENGINE" ) 2>&1 | sed 's/^/    /' ;;
    esac
    fail "the engine exited with code $rc"
fi
exit 0
