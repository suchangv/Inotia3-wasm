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
tools/build-android.sh         # -> build/android/inotia3.apk (see docs/ANDROID.md)

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
on a fault · `--frame-ms=N` the frame period the engine is asked to run at. The
engine hardcodes 66 ms (15 fps) and that is the game's speed, so this is off by
default and is a measurement tool, not a setting -- see
[docs/PERFORMANCE.md](docs/PERFORMANCE.md). In the browser the same knob is
`?framems=N`, and `?fixed=1` forces the deterministic 16 ms tick that headless
verification uses.

## Game files are not in this repository

`game/`, `data/`, `*.so`, `*.apk` and `*.obb` are gitignored. Supply your own
legally-obtained copy. `libgame.so` and everything the engine unpacks out of it
are Com2uS's copyrighted work and are not distributed here. This holds for the
**whole history**, not just the current tree: the commits that once carried
`data/com.com2us.inotia3.google.global/` and the `fb*.raw` frame dumps were
rewritten before this repository was first published, so no blob of game
content exists in any reachable commit.

## Standing on other people's work

Three projects made this one possible. None of them is vendored here; each is a
dependency you fetch or install yourself (see [docs/REUSE.md](docs/REUSE.md)).

**[unicorn-engine](https://github.com/unicorn-engine/unicorn)** — it is what
makes the CPU interpreter *verifiable*. `tools/difftest.sh` runs the same
relocated `libgame.so` twice: once under `src/cpu.c`, once under Unicorn's ARM
core with a per-instruction code hook, and `tools/difftrace.py` diffs the two
traces. Both sides start from identical guest memory, so the first differing
line is a decoder bug by construction — it names the instruction, the register
file and the guest PC. Every Thumb-1/ARM/VFP fix this port needed was found that
way, and `--all` sweeps the entry point of all 5962 functions in `.text`. An
interpreter this size cannot be trusted without a second implementation to
disagree with; this is that implementation.

**[gl4es](https://github.com/ptitSeb/gl4es)** — it is what makes the GPU part
possible at all. The game speaks only GLES1 fixed-function with the fixed-point
entry points (`glVertexPointerx`, `glClearColorx`, `glOrthox`, ...), which no
current driver implements; gl4es translates that pipeline to GLES2.
`src/gl4es_bridge.c` is then a thin adapter: the guest's ARM soft-float ABI on
one side, `gl4es_gl*` on the other, plus the `GL_FIXED` → `float` conversion
WebGL requires. The GL work here is adapter code, not a renderer.

**[Inotia3-vita](https://github.com/MetalSyntax/Inotia3-vita)** — the native
PS Vita port of this same engine, and the reason this took weeks instead of
months. It is where the guest lifecycle came from (`nativePreInit` →
`nativeInit` → per-tick `nativeEvent`/`nativeRender`, and the three-step
pause/resume), along with the JNI offsets, the touch-target coordinates, the
finding that `UI_listControl` is the one hookable hit-test registry, and the
binary patches the engine needs to run offline. `src/patches.c` here is that
port's decisions re-applied through a simpler mechanism, and
[docs/SPIKE.md](docs/SPIKE.md) records where the two ports agree and where this
one had to differ. Reverse-engineering an unfamiliar 745 KB binary is the
longest, least rewarding part of a port; that work was already done there.

## Status

The game boots to its main menu and, with a tap, into gameplay — asset install,
the game loader's 34 stages, LZMA unpacking, PNG decode, fonts, UI rendering and
the renderer all run. The interpreter covers Thumb-1 and ARM; the import table,
the JNI environment, the guest heap and the GLES1 state layer are in place.
Audio is not wired up: `SOUNDSYSTEM_Process` is called every frame and there is
no host backend behind it. In the browser, gameplay now holds the engine's own
rate — 15.0 engine frames per second, on a page running at 60 ticks/s, with the
interpreter using 19% of the wall clock — where it was 11.9 before, and text
still does not render (the opening event's dialogue box comes up empty). See
[`docs/PERFORMANCE.md`](docs/PERFORMANCE.md) for the measurements and for what is
left.

Details — the interpreter bugs found and how, plus the current state — are in
[`docs/HANDOFF.md`](docs/HANDOFF.md) and [`docs/SPIKE.md`](docs/SPIKE.md).
