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

XDG_DATA_HOME=${XDG_DATA_HOME:-$HOME/.local/share}

# PortMaster's own preamble: the folder has moved between releases, so try the
# known names and fall back to one next to this script. Not being able to find
# it is not fatal -- the port runs without it, it just loses the hotkeys and the
# shared logging.
controlfolder="/roms/ports/PortMaster"
[ -d "$controlfolder" ] || controlfolder="/roms/ports/portmaster"
[ -d "$controlfolder" ] || controlfolder="/roms/ports/PortMaster-master"
[ -d "$controlfolder" ] || controlfolder="$(dirname "$0")/PortMaster"
if [ -f "$controlfolder/control.txt" ]; then
    . "$controlfolder/control.txt"
fi

# Fills in $DISPLAY_WIDTH/$DISPLAY_HEIGHT for this panel and, when a .gptk is
# present, starts gptokeyb. This port reads the pad through SDL2 itself, so the
# .gptk only carries the exit hotkey.
get_controls 2>/dev/null || true

GAMEDIR="$(dirname "$0")/inotia3"
cd "$GAMEDIR" || exit 1

# Logging goes through exec, not tee: on SIGKILL tee loses its last few KB and
# those are exactly the lines a crash needs. dmesg on the way out turns "it just
# closed" into OOM evidence.
exec > "$GAMEDIR/inotia3.log" 2>&1
trap 'echo "--- dmesg (tail) ---"; dmesg 2>/dev/null | tail -20' EXIT

echo "--- $(date) ---"
echo "device: ${DEVICE:-?}  panel: ${DISPLAY_WIDTH:-?}x${DISPLAY_HEIGHT:-?}"
echo "video: ${SDL_VIDEODRIVER:-auto}  audio: ${SDL_AUDIODRIVER:-auto}"
echo "locale: ${LC_ALL:-${LANG:-?}}"

missing=
[ -f "$GAMEDIR/libgame.so" ] || missing="$missing libgame.so"
[ -f "$GAMEDIR/assets.apk" ] || missing="$missing assets.apk"
if [ -n "$missing" ]; then
    echo "Inotia 3: missing$missing -- put them in $GAMEDIR"
    echo "  libgame.so  from lib/armeabi/libgame.so inside the APK"
    echo "  assets.apk  the APK itself, renamed"
    sleep 5
    exit 1
fi

# The port brings its own SDL2: the firmware's may be any version, and the
# kmsdrm/GL pairing is the one thing a port cannot guess.
export LD_LIBRARY_PATH="$GAMEDIR/libs:$LD_LIBRARY_PATH"

# A language override for testing: SPIKE_LANG=zh-CN ./K_艾诺迪亚3[中].sh
# Otherwise the engine follows LC_ALL/LANG, which is the firmware's locale.
# XDG_RUNTIME_DIR is deliberately left alone: on this firmware a weston session
# owns it and overwriting it breaks the compositor (black screen / 90 degrees).

[ -n "$pm_platform_helper" ] && pm_platform_helper "$GAMEDIR/inotia3"

echo "--- starting ---"
./inotia3 "$@"
rc=$?
echo "--- exit $rc ---"
[ -n "$pm_finish" ] && pm_finish
exit $rc
