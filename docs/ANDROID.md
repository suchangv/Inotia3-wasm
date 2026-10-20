# The Android build

A **NativeActivity**: the app is the C engine. No Java code, no `classes.dex`, no
WebView — `android:hasCode="false"` in the manifest is literal. The frame loop,
input, GL (through gl4es) and audio (OpenSL ES) are all native, and the guest
`libgame.so` runs under the same interpreter the browser build uses.

```
tools/build-android.sh                 # -> build/android/inotia3.apk
tools/build-android.sh --install       # ... and adb install -r it
ABI=arm64-v8a tools/build-android.sh   # one ABI
```

Because the guest is *interpreted*, there is no second port per CPU: the same
APK carries `arm64-v8a` and `x86_64` and the same guest image runs on both. The
x86_64 library is what runs in an emulator; it is not a separate effort.

## One-time setup

No Gradle, no Android Studio. The build needs the SDK's build-tools and
platform, the NDK, and a JDK (for `apksigner`/`keytool` — the JDK's `jar` also
does the packaging, so `zip` is not required):

```sh
SDK=$HOME/android/sdk
sdkmanager --install "platform-tools" "build-tools;34.0.0" \
                       "platforms;android-34" "ndk;26.3.11579264"
export ANDROID_SDK_ROOT=$SDK
export ANDROID_NDK_HOME=$SDK/ndk/26.3.11579264
export JAVA_HOME=$HOME/android/jdk          # a full JDK, not a JRE
```

`tools/build-android.sh` uses those three variables (defaulting to
`~/android/sdk` and `~/android/jdk`) and says which one is missing.

The game itself is **not** in the repository — it is Com2uS's copyrighted work.
The build copies `game/libgame.so` and `game/assets.apk` into the APK's assets,
and the app unpacks them into its private files directory on first launch:

```
/data/data/io.github.inotia3.native/files/
    libgame.so        the guest ELF, 1.3 MB
    assets.apk        the asset archive the engine reads by offset, 22 MB
    data/…            what the engine writes (saves, install)
```

## What each layer is

| | |
|---|---|
| window | `src/platform_android.c`: EGL + a GLES3 context on the `ANativeWindow`, `eglSwapBuffers` once per tick |
| GL | gl4es, built with `NOEGL`, `DEFAULT_ES=2`, `NO_INIT_CONSTRUCTOR` — see the two traps below |
| input | the `AInputQueue` from `native_app_glue`, mapped from window pixels to the engine's 960×544 |
| storage | `chdir` into the app's files dir; relative paths then behave exactly as on the desktop |
| text | `src/text_android.c`: stb_truetype over the phone's own fonts (`/system/fonts/NotoSansCJK-Regular.ttc`, DroidSansFallback, Roboto), so CJK works without bundling a font |
| audio | `src/sound_android.c`: stb_vorbis (everything shipped is Ogg Vorbis) + OpenSL ES, with a 48 MB LRU on decoded PCM |
| loop | `android_main()` in `src/main.c`: `host_android_pump()` (looper, lifecycle, input) → `spike_tick()` → `host_android_present()` |

The engine is still told its screen is **960×544**. The window is the device's,
so every `glViewport` the engine issues is mapped into the largest centred
960:544 rectangle (`host_adjust_viewport`), and touch coordinates go back
through the same mapping. Nothing in the engine or the port's core knows the
phone's resolution, and the letterbox is why a 20:9 device shows bars instead of
a stretched picture.

## Two Android-specific traps, both fatal and both silent

1. **gl4es' `initialize_gl4es()` is a `constructor(101)` by default.** On
   Android that runs when the `.so` is `dlopen`ed — before the app has set the
   backend lookup or the framebuffer size — and `GetHardwareExtensions()` then
   calls through a null `gles_glGetString`. The symptom is a SIGSEGV at `0x0`
   during library loading, with a stack that names no guest code. Build gl4es
   with `-DNO_INIT_CONSTRUCTOR=ON`; the port calls `initialize_gl4es()` itself,
   in `gl4es_bridge_init()`, after `set_getprocaddress()` and
   `set_getmainfbsize()`.

2. **With `STATICLIB` gl4es also exports the unprefixed GL names** (`nm libGL.a`
   shows `T glShaderSource` next to `T gl4es_glShaderSource`), and a static
   archive beats the shared library at link time. A call to `glShaderSource`
   from the bridge resolved back into gl4es' own entry point, which dereferences
   `glstate` — which does not exist during `initialize_gl4es()`. The bridge now
   resolves the *driver's* `glShaderSource` through
   `eglGetProcAddress`/`dlsym`. Anything else that must reach the driver rather
   than gl4es needs the same treatment.

A third, self-inflicted one worth remembering: `src/jni.h` (the port's guest-JNI
surface) shadowed the NDK's `<jni.h>` for every NDK header that includes it, so
`android/native_activity.h` could not even declare `JavaVM`. The header is now
`src/falsojni.h`.

## Fixed after the first device test

Three things a real 16:10 phone showed that the emulator did not:

**The letterbox bars were garbage, not black.** The engine only covers its
960x544 region, and an Android window surface's buffers are not guaranteed to
come back preserved: whatever the last user of that buffer left in the top and
bottom bars is what the compositor shows. `host_android_begin_frame()` now
clears the whole surface black (scissor off, full-size viewport) before each
tick, then restores the letterbox viewport so a frame that draws without setting
one still lands inside the bars.

**The screen flickered badly.** The loop presented on every tick -- 60 a second
on a phone -- while the engine only draws when one of its timers is due, 15
times a second. Presenting an untouched back buffer puts the *previous* frame on
screen for one vsync, then the next tick puts this one back: flicker at the
display's rate, three times per engine frame. The loop now presents only when
`spike_tick()` actually ran the engine (`g_tick_drew`), and sleeps on
`timers_next_in()` otherwise, through the looper so input still arrives. Measured
on the emulator: 15.1 presents/second, and the app's CPU use halved.

**Chinese text was blank on the device.** The first version of the text backend
tried four hard-coded font paths and chose the first whose cmap had the
language's probe character. A device whose CJK font has a different name (a
vendor font: MiSans, HarmonyOS Sans, ...) fell back to a Latin font -- Latin
worked, Chinese was blank, and nothing was logged. The backend now (a) tries the
known paths, (b) if none can actually *rasterise* the probe, scans
`/system/fonts` -- CJK-looking names first -- for one that can, (c) keeps every
font that loaded and picks per character, so one font can serve Latin and another
CJK, and (d) logs which font it is using, or that no font on the device can draw
the language. Text is also sized the way `Paint.textSize` sizes it now
(`stbtt_ScaleForMappingEmToPixels`, not ascent-to-descent), which matters for CJK
where the two differ by ~45%.

If a device genuinely has no font that can draw the language, dropping a
`font.ttf` (or `.ttc`/`.otf`) into the app's files directory overrides
everything:

```sh
adb push myfont.ttf /data/data/io.github.inotia3.native/files/font.ttf
```

## Fixed after the second device test

**A skill's sound stuttered.** The OpenSL backend created and destroyed an
`AudioPlayer` on every `SoundPlay`. On a phone that is a round trip to the
audioserver -- 10-13 ms measured on the emulator -- taken *inside* the frame that
fired the skill. A voice pool (ten realised players, one per PCM format, reused)
plus "a play request for a sound that is already sounding is a no-op" (the Java
side it was written against, `MediaPlayer`/`SoundPool`, behaves that way) brings
a repeat play down to **0.75 ms**, of which decoding is zero (cached) and player
setup is zero (reused). The one remaining cost is the first play of a new
*format*: ~3 ms to realise one player, once.

Two smaller things in the same pass: the queue callback now frees the voice it
finished with (it used to leave it "owned but silent", so the pool filled up with
unusable voices and every play built a new player -- the 10 ms came back), and
`SoundCreate`'s 160 handles still decode lazily, with a 48 MB LRU.

**Saving said it worked and the slot stayed empty.** Not Android-specific: the
browser had it too, and the cause is in the guest's own save path. `SAVE_Save`
writes `./data/<package>/<folder>/saveN.dat`, where `<folder>` is
`HubSave_GetFolderName()`'s answer for the "hub key" -- an account id the
engine's online service supplies. Offline nothing supplies it, so the folder is
named after whatever its heap block held, and the block is reused: the save wrote
`<garbage A>/save0.dat` while the slot list looked in `<garbage B>/save0.dat`.
The file was real, the reported success was real, and the slot never appeared.

`patches.c` now replaces `HubSave_GetFolderName` with "there is no subfolder"
(write an empty string, return success), so save, list, load and delete all use
`./data/<package>/saveN.dat` -- the path the engine already probes first for
every slot. Verified in the native build end to end: a save writes
`save0.dat` (1427 bytes) and the next run's slot scan opens it successfully
(`[io] open .../save0.dat ok`). The browser shares the patch, so its "saved but
the slot is empty" is fixed by the same change.

## What has been verified, and how

On an `x86_64` Android 13 emulator (`-gpu swiftshader_indirect`, no window;
screenshots via `adb exec-out screencap`), with the settings above:

- the APK installs and launches; assets unpack on first run (23 MB, then cached);
- the title screen, the character-creation screen (description text, stat block,
  YES/NO), the opening dialogue and the first map all render correctly, with the
  letterbox and correct touch mapping;
- text goes through the TTF backend: the class description, the stat labels and
  the dialogue lines are all drawn by it;
- OpenSL ES starts a player (`libOpenSLES: Emulating old channel mask behavior`,
  i.e. a stereo buffer was queued).

Not verified yet:

- **Audio output.** The emulator was run with `-no-audio`; the player starts,
  but nobody has heard it. `stb_vorbis` + OpenSL is the one part with no
  end-to-end listening test.
- **A real device.** The emulator's GLES3 is SwiftShader. The shaders gl4es
  generates are the same ones the browser build uses on a real GPU, but a
  driver-specific failure here would be new information.
- **Saving.** Same status as the browser: the save path is fixed
  (`ensure_parent_dirs`), and the write has not been round-tripped on Android.
- **Lifecycle.** Home/return, rotation and the surface being destroyed have code
  paths (`APP_CMD_TERM_WINDOW`, blocking pump) but have only been exercised by
  launching, not by backgrounding mid-game.
- **Performance** on a real phone. The emulator is not a benchmark.

## Where the pieces live

```
android/AndroidManifest.xml    NativeActivity, landscape, GLES3, no code
src/platform_android.c         EGL, input, assets, storage, letterbox, lifecycle
src/text_android.c             stb_truetype + the system fonts
src/sound_android.c            stb_vorbis + OpenSL ES
src/main.c                     android_main(): pump -> tick -> present
third_party/stb/               stb_truetype.h, stb_vorbis.c (public domain)
tools/build-android.sh         compile + package + sign, no Gradle
tools/build_gl4es.sh android   gl4es for the NDK (also builds the browser one)
```
