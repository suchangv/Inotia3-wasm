# The PortMaster build (RK3566 handhelds)

An SDL2 application that runs the same engine as every other build: the guest
`libgame.so` from the player's own APK, executed by the ARM interpreter, with
gl4es doing GLES1 → GLES2 and SDL2 owning the window, the pad and the clock.

**The interpreter stays.** RK3566 (4× Cortex-A55 @ 1.8–2.0 GHz) does not need
native execution: gameplay frames are 2.1–2.7 M guest instructions and 15 fps is
66 ms, so ~80 MIPS of interpreter — a third of what this one already achieves on
a desktop core — leaves the frame with room to spare. Dropping the interpreter
would mean running an *armeabi* (32-bit) Android `.so` under an armhf userspace
with a C reimplementation of bionic + JNI + GLES1: the Vita port's glue layer,
from scratch, for a speed-up the game does not need. See the table in
`docs/ANDROID.md` for the same engine's instruction counts.

## Layout

```
/roms/ports/
  K_艾诺迪亚3[中].sh            launcher the Ports menu shows
  inotia3/                     the port's own directory
    inotia3                    aarch64 binary, gl4es linked in
    libs/libSDL2-2.0.so.0      the SDL2 this port is built against
    port.json                  PortMaster metadata
    README.txt
    data/                      created on first run: saves + installed data
    inotia3.log                everything the port prints (no console)
    libgame.so                 ← the player supplies this
    assets.apk                 ← and this (the APK itself, renamed)
```

`tools/build-portmaster.sh` produces both (and `--zip` a
`dist/K_艾诺迪亚3[中].zip`). The two game files are never packaged: the launcher
checks for them and says what is missing.

The launcher follows the PortMaster conventions the neighbouring ports use: the
four-way `controlfolder` discovery, `get_controls` (which is what fills
`$DISPLAY_WIDTH`/`$DISPLAY_HEIGHT`), logging through `exec >` rather than `tee`
(tee loses its tail on SIGKILL), `pm_platform_helper`/`pm_finish` when they
exist, a `dmesg` tail on exit as OOM evidence, and **`XDG_RUNTIME_DIR` left
alone** -- on this firmware a weston session owns it, and pointing it elsewhere
breaks the compositor (black screen, 90 degree rotation).

## Building

Three steps, once each (the toolchains are ordinary Debian packages):

```sh
tools/build_sdl2.sh native            # development machine
tools/build_sdl2.sh aarch64           # the handheld (KMSDRM + offscreen)
tools/build_gl4es.sh native           # desktop window / measurement
tools/build_gl4es.sh aarch64
tools/build.sh sdl                    # -> build/inotia3        (x86_64)
tools/build.sh handheld               # -> build/inotia3-aarch64
tools/build-portmaster.sh --zip       # -> dist/…
```

The aarch64 builds need `aarch64-linux-gnu-gcc`. Two things about SDL2's
KMSDRM backend, both easy to get wrong:

- it is gated on **pkg-config** finding `libdrm`, `gbm` *and* `egl`, and on
  `HAVE_OPENGL_EGL`. Without those the build succeeds and the binary simply has
  no video driver -- the fault shows up on the device as "no available video
  device". `tools/build_sdl2.sh aarch64` generates the two `.pc` files and puts
  them on `PKG_CONFIG_PATH`; check the result with
  `nm vendor/SDL2/dist-aarch64/lib/libSDL2-2.0.so.0 | grep -c kmsdrm`
  (124 symbols on the current build) and `strings … | grep -x kmsdrm`.
- the device `dlopen`s libdrm and gbm at runtime (`SDL_KMSDRM_SHARED`), so the
  package does not ship them; only headers are needed at build time. ALSA is
  linked (`libasound.so.2` is on every CFW), which is why the build wants an
  aarch64 `libasound` at link time.

**glibc.** This build links against the build machine's glibc (Debian trixie,
2.41) and therefore needs **GLIBC_2.38 or newer** on the device -- Ubuntu 24.04
or newer. On an older firmware the loader stops with `GLIBC_2.34 not found`
(visible in `inotia3.log`). The fix is to rebuild SDL2 and this binary against
an older sysroot; `tools/cross-aarch64.sh` documents the recipe and the four
non-obvious flags it needs (`-nostdinc`+`-isystem` because Debian's cross gcc
ignores `--sysroot` for includes, `-B` for the old crt objects, `-Wl,--sysroot`
for the linker scripts' absolute paths, and the multiarch lib symlinks). Check
the device first: `ldd --version` in an SSH shell.

## Running it on the development machine

The same binary runs in a window, which is where the input mapping is tuned:

```sh
cd game                      # libgame.so + assets.apk live here
SDL_VIDEODRIVER=x11 ../build/inotia3 --so=libgame.so --windowed
```

Headless, for screenshots (no X server needed — SDL's offscreen driver plus
Mesa's llvmpipe):

```sh
SDL_VIDEODRIVER=offscreen ../build/inotia3 --so=libgame.so \
    --frames=1050 --shot=/tmp/out.ppm@1030 --cursor=300,200 \
    --tap=1500,467,240 --tap=2000,480,151
```

`--frames` counts *engine* frames (what the game calls a frame), `--shot=FILE@N`
writes a PPM after N presented frames, `--cursor=x,y` holds the pointer at a
content position so a screenshot can check that it draws, and `--tap=frame,x,y`
drives the menu with synthetic touches (device pixels, the same numbers the
Vita port uses).

The screenshot path was how the cursor was verified: the PPM showed a proper
arrow at content (300,200) over the game's map, and earlier frames showed it
absent — the 3-second hide doing its job.

## Input

**Now:** the left stick is a virtual mouse pointer over the letterboxed picture.

| Input | Effect |
|---|---|
| left stick | moves the pointer, 620 px/s at full deflection, deadzone 0.18, quadratic near the centre |
| no input for 3 s | the pointer hides (and any button press brings it back) |
| **L3** (stick press) | mouse button: press + release is a click, holding drags |
| mouse (desktop) | the same, directly |
| arrows / space (desktop) | pointer / click, for testing without a pad |
| Escape | quit |

The pointer is drawn by the port, not the engine (the engine has no notion of
one): a 16×16 arrow in a generated texture, drawn after the engine's frame with
the depth/cull/scissor state disabled, in window pixels, scaled with the
letterbox.

**Next:** the buttons, using the Vita port's mapping — the engine is touch-only,
so each button is a synthetic tap at a fixed HUD position. These are the
coordinates the Vita port uses, in the engine's 960×544 space:

| Vita | position | what it is | planned here |
|---|---|---|---|
| ✕ CROSS | 880,470 | main attack | A |
| ○ CIRCLE | 917,365 | skill 1 | B |
| □ SQUARE | 757,427 | skill 2 | X |
| △ TRIANGLE | 827,365 | item / shield | Y |
| ↑ ↓ ← → | 140,341 · 145,507 · 42,417 · 245,417 | movement | d-pad |
| L / R | 540,499 · 640,499 | item quick slots | L1 / R1 |
| SELECT | 735,499 | third quick slot | L2 |
| START | 42,272 | pause | START |

and, when the engine's UI control list is non-empty, the d-pad moves a cursor
between controls and **A** confirms (the Vita port's `wr_process_menu_cursor`,
same idea: the list lives at `UI_listControl`, which `spike_dump_ui_controls`
already reads).

## What is verified

On the development machine, with Mesa's GLES2 and the SDL offscreen driver: the
engine boots, the letterbox is correct, the title/character-creation/opening
screens render, text goes through the same TTF backend as Android, scripted taps
drive the menus, and the pointer draws at the right place and hides when idle.
Frames are presented only when the engine drew — the same flicker fix as the
Android build.

Not verified yet: the handheld itself (KMSDRM, Mali-G52, the real pad), sound
(the audio backend is the next step), and everything below "Next" above.
