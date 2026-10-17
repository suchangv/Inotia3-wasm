# Inotia 3 → WebAssembly

Running the Android build of Inotia 3 in a browser, by executing its original ARM
`libgame.so` — no Android, no Dalvik, no reimplementation of the game.

This is a **spike**: a measurement rig first, a port second. See
[`docs/SPIKE.md`](docs/SPIKE.md) for what it does and does not yet prove.

## Why this shape

The binary decides the approach. From the real `libgame.so` (1.36 MB):

| Property | Value | Consequence |
|---|---|---|
| `Tag_CPU_arch` | **v5TE**, not v6/v7 | no ARMv7 semantics to model |
| `Tag_THUMB_ISA_use` | **Thumb-1** | no Thumb-2, no IT blocks; 16-bit fixed width |
| `Tag_FP_arch` | **VFPv2** | no NEON |
| `.text` | 745,696 B, 96.0% Thumb | one decoder covers almost everything |
| Dynamic flags | `BIND_NOW` + `SYMBOLIC`, no `mprotect` | no JIT, no self-modifying code |
| Relocations | 2954 `R_ARM_RELATIVE`, 178 `R_ARM_JUMP_SLOT` | all resolvable at load time |
| Imports | 183 (58 GLES1 fixed-function, rest libc/libm/pthread) | a small, closed host surface |
| `.dynsym` | 5962 functions inside `.text` | every entry point is enumerable |

That last column is why a static translation to WebAssembly is realistic later:
the code cannot change itself, and its call graph is recoverable. The interpreter
in `src/` is the first stage — it is the correctness oracle the translated
version will be diffed against, and it produces the numbers that decide whether
the translation is worth building.

## Layout

```
src/
  guest.h          fixed guest address space (144 MB, one flat buffer)
  elf.h/.c         ELF32/ARM reader, relocation, symbol index
  cpu.h/.c         ARMv5TE / Thumb-1 interpreter
  imports.h/.c     the 183 imports: BKPT stubs, dispatch, crossing counters
  host_libc.c      bionic shims + the guest heap allocator
  platform_*.c     host services (native now, browser later)
  main.c           the spike runner
tools/build.sh     native and wasm32 builds
```

### Two design choices worth knowing

**Import stubs are `BKPT` markers.** Each imported function gets 4 bytes at
`GUEST_STUB_BASE + i*4` whose first halfword is `BKPT`; the index is recovered
from the address. The GOT slot holds `stub|1`, because `libgame.so`'s `.plt` is
*ARM* code that dispatches with `ldr pc, [ip]`, and bit 0 is what selects Thumb
state. Net effect: counting host crossings costs nothing in the inner loop,
because `BKPT` is already one more opcode in the existing decode.

**Guest memory is one flat buffer with unchecked access.** The inner loop is
plain array indexing. On native that turns a bad guest pointer into a host
`SIGSEGV`, so `platform_native.c` installs a handler that translates the fault
back into guest address, guest PC and registers — without it, every bring-up
crash is a manual bisect. `-DGUEST_CHECK` swaps in bounds-checked accessors.

## Building and running

The core is freestanding: `clang` targets `wasm32` directly and `lld` supplies
`wasm-ld`. No emscripten, no multi-GB SDK. (Emscripten returns when the browser
host layer — fetch, WebGL, IDBFS — lands.)

```sh
tools/build.sh native          # -> build/spike
tools/build.sh browser         # -> build/spike.{html,js,wasm,data}

mkdir -p game && cp /path/to/libgame.so game/
./build/spike game/libgame.so --boot --frames=4200 --no-profile

# drive the menus headlessly -- device (canvas) pixels, one tap per line
./build/spike game/libgame.so --boot --frames=4200 --no-profile \
    --tap=1500,467,240 --tap=2000,480,151 \
    --tap=2400,136,459 --tap=2800,480,359
```

Options: `--boot` run the game's own startup (JNI, assets, frames) ·
`--frames=N` stop after N frames · `--tap=frame,x,y` inject a tap at a frame ·
`--init-array` run static constructors · `--jni` call `JNI_OnLoad` · `--all`
both · `--no-profile` · `--trace` record the last 96 instructions and dump them
on a fault · `--frame-ms=N` the frame period the engine is asked to run at (the
engine hardcodes 66 ms = 15 fps; the port asks for 16 ms = 60 fps -- see
[docs/PERFORMANCE.md](docs/PERFORMANCE.md) before moving it). In the browser the
same knob is `?framems=N`.

## Game files are not in this repository

`game/` is gitignored, as are `*.so`, `*.apk` and `*.obb`. Supply your own
legally-obtained copy. `libgame.so` is Com2uS's copyrighted work and is not
distributed here.

## Status

The game boots to its main menu and, with a tap, into gameplay — asset install,
the game loader's 34 stages, LZMA unpacking, PNG decode, fonts, UI rendering and
the renderer all run. The interpreter covers Thumb-1 and ARM; the import table,
the JNI environment, the guest heap and the GLES1 state layer are in place.
Audio is not wired up. The browser host builds and reaches the menu, and in
gameplay it now runs one engine frame per animation frame — 58.5 fps measured,
up from 13.5 — after fixing the host log sink and raising the engine's own frame
period; see [`docs/PERFORMANCE.md`](docs/PERFORMANCE.md) for the measurements and
for what is left in the worst frames.

Details — the interpreter bugs found and how, plus the current state — are in
[`docs/HANDOFF.md`](docs/HANDOFF.md) and [`docs/SPIKE.md`](docs/SPIKE.md).
