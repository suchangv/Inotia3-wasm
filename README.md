<p align="right">
  <a href="https://suchangv.github.io/inotia3-wasm/"><b>▶ 在浏览器里试玩 · Play in the browser</b></a>
</p>

# Inotia 3 → WebAssembly

[English](README.md) · [中文](README.zh-CN.md)

Runs the Android build of **Inotia 3** (Com2uS) in a browser and on handhelds by
executing its original ARM `libgame.so` — no Android, no Dalvik, no
reimplementation of the game. The binary is interpreted instruction by
instruction; everything it asks of the system (183 imports and the whole JNI
surface) is answered by this repository.

**This repository contains no game data.** Supply your own legally obtained APK.

---

## Play it

<https://suchangv.github.io/inotia3-wasm/> — import your APK once (it stays in
your browser's IndexedDB and is never uploaded), then press start. Saves move in
and out as zip files. It installs as a PWA and runs offline after the first load.

## Build it

```sh
tools/build.sh native      # desktop runner, no window: measurement + oracle
tools/build.sh check       # same, with bounds-checked guest memory
tools/build.sh wasm        # freestanding wasm32: the throughput number
tools/build.sh browser     # emscripten + gl4es, game files preloaded (dev)
tools/build.sh web         # emscripten, NO game data: what the page ships
tools/build.sh sdl         # SDL2 window, gamepad, ALSA/PipeWire audio
tools/build.sh handheld    # aarch64 cross build for PortMaster-style devices
tools/build_vorbis.sh      # the separate Ogg Vorbis decoder module

tools/build-android.sh     # the APK: arm64-v8a + x86_64, debug-signed
tools/build-android.sh --install
ABI=arm64-v8a tools/build-android.sh
```

The browser targets need emscripten (`$HOME/emsdk` by default) and a gl4es built
for the host (`tools/build_gl4es.sh`); `vendor/` is gitignored. A fresh clone is
therefore not buildable until you supply an APK and run those two scripts — see
[`docs/HANDOFF.md`](docs/HANDOFF.md).

The Android target needs the SDK's build-tools and platform, the NDK and a JDK
(`ANDROID_SDK_ROOT`, `ANDROID_NDK_HOME`, `JAVA_HOME`; defaults are `~/android/sdk`
and `~/android/jdk`), plus `ABIS="arm64-v8a x86_64" tools/build_gl4es.sh android`
for the static gl4es. No Gradle. It packages `android/AndroidManifest.xml`
(a `NativeActivity` with `hasCode="false"`) and copies the guest files into
`android/assets/` — `tools/build-android.sh` is the whole build system, and
[`docs/ANDROID.md`](docs/ANDROID.md) is the long version.

## Why this shape

The binary decides the approach. From the real `libgame.so` (1,359,908 B):

| Property | Value | Consequence |
|---|---|---|
| `Tag_CPU_arch` | **v5TE**, not v6/v7 | no ARMv7-only semantics to model |
| `Tag_THUMB_ISA_use` | **Thumb-1** | no Thumb-2, no IT blocks; 16-bit fixed width |
| `Tag_FP_arch` | **VFPv2**, soft-float ABI | no NEON; doubles in r0/r1 pairs |
| `.text` | 96% Thumb | one decoder covers almost everything |
| Dynamic flags | `BIND_NOW` + `SYMBOLIC`, no `mprotect` | no JIT, no self-modifying code |
| Relocations | 3,139, all resolvable at load time | the loader owns the address space |
| Imports | 183 (58 GLES1, rest libc/libm/pthread/JNI) | a small, closed host surface |
| `.dynsym` | 5,962 functions inside `.text` | every entry point is enumerable |

That last row is why the interpreter is verifiable: there is a finite, known set
of functions to prove and to diff against.

## How it works

```
libgame.so ──▶ ELF32/ARM loader ──▶ relocation + sorted symbol index
                    │
                    ▼
              ARMv5TE/Thumb-1 interpreter ──── BKPT stubs ──▶ host imports
                    │                                └──────▶ libgcc helpers
                    ▼
              FalsoJNI-style JavaVM/JNIEnv (built in guest memory)
                    │
                    ▼
              GLES1 fixed-function ──▶ gl4es ──▶ GLES2 ──▶ WebGL2 / native GL
                    │
                    ▼
              host layer: emscripten · SDL2 · freestanding wasm · Android
```

* **Guest memory** is one flat 144 MB arena, so guest address → host pointer is a
  plain add. On native builds a fault handler maps a `SIGSEGV` back to guest PC,
  address and registers.
* **Imports** get a four-byte `BKPT` stub each; the interpreter recovers the
  index from the address, so counting host crossings costs nothing in the inner
  loop. Internal libgcc helpers (`__divsi3`, `__divdf3`, …) are hooked the same
  way — those are 52% of the steady-state instruction count on ARMv5TE, which has
  no divide instruction at all.
* **Java** is replaced by a hand-built `JavaVM`/`JNIEnv` in guest memory with the
  real offsets the engine indexes by; native methods call back into the engine's
  own entry points.
* **GL** is fixed-point GLES1 (`glVertexPointerx`, `glClearColorx`, …), which no
  current driver implements. gl4es translates it to GLES2; the port's own code is
  only the ABI adapter and the `GL_FIXED` → `float` conversion.

## What works

| | |
|---|---|
| Boot | asset install (12 files, LZMA), the loader's 34 stages, main menu |
| Gameplay | menus, UI, maps, characters, combat — the interpreter is complete for Thumb-1, ARM and VFPv2 that this binary uses |
| Text | real fonts, all shipped languages (not the engine's bitmap font) |
| Saves | `/data` lives in IDBFS in the browser (survives reload), export/import as zip, delete with confirmation |
| Audio | WebAudio backend for BGM and effects; Ogg Vorbis decoded by the browser, or by `vorbis.wasm` where the browser has no Vorbis decoder (Safari) |
| Input | pointer/touch, gamepad on SDL targets, on-screen mapping of keys to engine touch coordinates |
| Offline | service worker precaches the shell and the module |
| Devices | desktop browsers, iPhone/iPad (PWA), RK3566 handhelds, and the native PS Vita port this one borrows its reverse engineering from |

Known gaps are tracked in [`docs/REMAINING.zh-CN.md`](docs/REMAINING.zh-CN.md)
(Chinese) and the measurement notes in [`docs/PERFORMANCE.md`](docs/PERFORMANCE.md).

## Verification, not hope

* **Differential oracle.** `tools/difftest.sh` runs the same relocated image
  twice — once under this interpreter, once under Unicorn's ARM core with a
  per-instruction hook — and diffs the traces (`tools/difftrace.py`). Both sides
  start from identical guest memory, so a mismatch is a decoder bug by
  construction and the first differing line names the instruction, the register
  file and the PC. `--all` sweeps the entry point of all 5,962 functions.
* **Pixel A/B.** The same frame index with a change on and off is byte-compared;
  the interpreter is deterministic, so this catches behavioural regressions the
  oracle cannot see (a wrong value that is consistently wrong).
* **Guest-side counters.** Every import crossing, every frame's instruction count
  and interpreter time are measured and logged, which is how the armv5 divide
  helpers were identified as the hottest thing in the game.

Current numbers, desktop Chrome, in gameplay: **2,150,383 instructions and
12.8 ms of interpreter per engine frame, 34 draw calls, 19% of the wall clock, at
the engine's own 15 fps** — about 5× headroom. On an RK3566 handheld the same
frame costs ~84 ms at ~27 MIPS, which is what the libgcc host hooks and the
`inotia3-aarch64` build exist for. See [`docs/PERFORMANCE.md`](docs/PERFORMANCE.md).

## Layout

```
web/                 the published page: index.html, sw.js, manifest, icons
android/             AndroidManifest.xml — the APK is a NativeActivity, no Java
src/  guest.h        one flat 144 MB guest address space
      elf.h/.c       ELF32/ARM reader, relocation, symbol index
      cpu.h/.c       ARMv5TE / Thumb-1 / VFPv2 interpreter
      imports.h/.c   the 183 imports: BKPT stubs, dispatch, crossing counters
      host_hooks.c   libgcc helpers answered from the host (divide, soft-float)
      host_libc.c    bionic shims + the guest heap allocator
      jni.c           JavaVM/JNIEnv, built in guest memory
      gl.c, gl4es_bridge.c   GLES1 state + fixed-function → gl4es adapter
      platform_*.c   hosts: native, SDL2, emscripten, freestanding wasm, Android
      main.c         the runners
tools/               build scripts, the oracle, the difftests, the page's JS side
docs/                SPIKE, PERFORMANCE, HANDOFF, REUSE, ANDROID, PORTMASTER, 需求清单
third_party/stb/     stb_vorbis (audio), stb_truetype (fonts)
```

## Game files are not in this repository

`game/`, `data/`, `*.so`, `*.apk` and `*.obb` are gitignored, and this holds for
the **whole history**: the commits that once carried installed assets or
framebuffer dumps were rewritten before the repository was first published, so no
blob of game content exists in any reachable commit. The deployed page ships no
game data either — it asks for the player's own APK and keeps it in their
browser.

## Standing on other people's work

**[unicorn-engine](https://github.com/unicorn-engine/unicorn)** — the correctness
oracle. Every decoder fix in this port was found by disagreeing with a second,
independent implementation, instruction by instruction. GPLv2, used as a test
tool only.

**[gl4es](https://github.com/ptitSeb/gl4es)** — the GPU half. GLES1
fixed-function with fixed-point entry points has no modern implementation;
gl4es translates the pipeline to GLES2, so this port is an adapter, not a
renderer.

**[Inotia3-vita](https://github.com/MetalSyntax/Inotia3-vita)** — the native PS
Vita port of the same engine, and the reason this took weeks instead of months:
the guest lifecycle, the JNI offsets, the binary patches a port needs, the touch
coordinates and the `UI_listControl` discovery all came from there.

## Docs

| | |
|---|---|
| [`docs/SPIKE.md`](docs/SPIKE.md) | what was measured, and what the numbers decided |
| [`docs/PERFORMANCE.md`](docs/PERFORMANCE.md) | the frame budget, the clock bugs, where the time goes |
| [`docs/HANDOFF.md`](docs/HANDOFF.md) | the long version: every bug found and how |
| [`docs/REUSE.md`](docs/REUSE.md) | the dependencies, their licences and their gotchas |
| [`docs/ANDROID.md`](docs/ANDROID.md) / [`docs/PORTMASTER.md`](docs/PORTMASTER.md) | the two packaged targets |
| [`docs/REMAINING.zh-CN.md`](docs/REMAINING.zh-CN.md) | 剩余工作清单(中文) |
