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
tools/build.sh wasm            # -> build/spike_freestanding.wasm

mkdir -p game && cp /path/to/libgame.so game/
./build/spike game/libgame.so --init-array --all --trace
```

Options: `--init-array` run static constructors · `--jni` call `JNI_OnLoad` ·
`--all` both · `--budget=N` instruction cap · `--no-profile` · `--trace` record
the last 96 instructions and dump them on a fault.

## Game files are not in this repository

`game/` is gitignored, as are `*.so`, `*.apk` and `*.obb`. Supply your own
legally-obtained copy. `libgame.so` is Com2uS's copyrighted work and is not
distributed here.

## Status

Working: ELF load and relocation, the full import table, the guest heap, and
Thumb-1 execution to the point where the static constructors run real engine
code. In progress: the ARM decoder, which sits on the path of every host
crossing. Not started: JNI, GLES1, audio, assets, the browser host.

Details, including the five interpreter bugs found so far and how, are in
[`docs/SPIKE.md`](docs/SPIKE.md).
