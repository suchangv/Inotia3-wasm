# Handoff

Everything a fresh agent needs to continue this work. Read this first, then
`docs/SPIKE.md` for the running engineering log.

## The goal, and why it is shaped this way

Run the Android build of **Inotia 3** in a browser by executing its original
ARM `libgame.so` — no Android, no Dalvik, no reimplementation of the game.

The binary decides the approach. From the real `libgame.so` (1.36 MB):

| Property | Value | Consequence |
|---|---|---|
| `Tag_CPU_arch` | **v5TE** | no ARMv7 semantics |
| `Tag_THUMB_ISA_use` | **Thumb-1** | no Thumb-2, no IT blocks; 16-bit fixed width |
| `Tag_FP_arch` | **VFPv2** | no NEON |
| Dynamic flags | `BIND_NOW` + `SYMBOLIC`, no `mprotect` | no JIT, no self-modifying code |
| Relocations | 2954 `R_ARM_RELATIVE`, 178 `R_ARM_JUMP_SLOT` | all resolvable at load |
| Imports | 183 (58 GLES1 fixed-function, rest libc/libm/pthread) | small, closed host surface |
| `.dynsym` | 5962 functions in `.text` | every entry point enumerable |

That last row is why a static translation to WebAssembly is realistic later:
code that cannot change itself has a recoverable call graph.

**The interpreter is stage one, not the destination.** Two numbers decide
whether the project stays an interpreter or pays for a translation:

1. instructions retired per unit of guest work, at what MIPS, native and under wasm;
2. host crossings per frame — a crossing costs ~50–200 ns in a browser.

Getting to the first real frame is what produces those numbers. That is the
current objective.

## Repository layout

This is a **separate git repository** inside `Inotia3-vita/wasm/`. The parent
repo is public and `.gitignore`s `/wasm/`, so nothing here is published.
Nothing has been pushed anywhere; there is no remote. See "Where things stand"
at the end for the one open question about that.

```
Inotia3-vita/            (public repo, the PS Vita port — the reference)
  source/ lib/           the Vita loader: FalsoJNI, so_util, java.c, patch.c
  PORTING_PLAN.md        104 Corridas of Vita debugging; answers many questions
  WASM_PORT_ASSESSMENT.md  the interpreter-vs-AOT analysis
  inotia3-wasm repo ↓
```

```
wasm/
  README.md              design rationale, build/run, what is and is not done
  docs/SPIKE.md          engineering log: every bug, every finding, harness gotchas
  docs/HANDOFF.md        this file
  src/
    guest.h              fixed guest address space, host service declarations
    elf.h/.c             ELF32/ARM reader, relocation, address-ordered symbol index
    cpu.h/.c             the ARMv5TE/Thumb-1 interpreter
    imports.h/.c         the 183 ELF imports: BKPT stubs, dispatch, crossing counters
    jni.h/.c             the JNI environment, built in guest memory
    jni_offsets.h        GENERATED — the JNI function table order (the ABI)
    host_libc.c          bionic shims + the guest heap
    platform.h/_native.c host services for the desktop build
    main.c               the spike runner
  tools/
    build.sh             native and wasm32 builds
    oracle.py            Unicorn differential oracle
    difftrace.py         first divergent instruction, with register diff
    difftest.sh          runs the oracle over every entry point
  game/                  GITIGNORED — put libgame.so here
  .venv/                 GITIGNORED — the oracle's Unicorn install
```

## Build and run

```sh
tools/build.sh native        # -> build/spike
mkdir -p game && cp /path/to/libgame.so game/

./build/spike game/libgame.so --boot    # the interesting one: the full boot sequence
./build/spike game/libgame.so --init-array
./build/spike game/libgame.so --jni
./build/spike game/libgame.so --jni --jni-verbose
./build/spike game/libgame.so --entry=010b6a09 --trace
```

Flags: `--boot` (constructors → `JNI_OnLoad` → `nativePreInit`) · `--init-array`
· `--jni` · `--all` · `--entry=HEX` (one guest function) · `--budget=N`
instruction cap · `--trace` last 96 instructions, dumped on a fault ·
`--jni-verbose` first 3 calls per JNI function with arguments · `--oracle` ·
`--dump-guest=FILE` · `--trace-insns=FILE` · `--no-profile`.

**No emscripten and no sudo are needed.** clang 19 targets wasm32 directly and
lld supplies `wasm-ld`; the core is freestanding (`compat.h`). Emscripten comes
back only for the browser host layer.

## The differential oracle

The single most valuable tool here. `tools/difftest.sh` runs each guest entry
point twice — once under the hand-written interpreter, once under Unicorn — and
`tools/difftrace.py` names the first instruction whose semantics differ.

```sh
tools/difftest.sh
  entry 010b6a09 : traces are identical for all 102 instructions
  entry 010d03c5 : traces are identical for all 9 instructions
  entry 010d0b3d : traces are identical for all 9 instructions
  entry 010ea589 : traces are identical for all 13 instructions
all entries match
```

Both sides start from the *same* relocated memory image (the interpreter dumps
it with `--dump-guest`), stub every import to a no-op returning 0, and stop when
PC leaves the image. That leaves pure CPU semantics, so a mismatch is a decoder
bug by construction. **Run it after every interpreter change** — it has already
caught three bugs that no amount of reading would have.

Setting up the oracle needs no pip and no system Python:

```sh
python3 -m venv --without-pip .venv
# then download the unicorn wheel from PyPI and unzip it into
# .venv/lib/python*/site-packages
.venv/bin/python3 -c "import unicorn; print(unicorn.__version__)"
```

Unicorn is an **oracle, not the product**: its speed is QEMU's TCG JIT, which a
browser cannot use, so under wasm it runs TCI (a two-level interpreter doing
strictly more work per guest instruction than a direct decoder). It is also
GPLv2. Read `docs/SPIKE.md` §"The differential oracle" for the five harness
asymmetries that each reported a divergence having nothing to do with the CPU —
you will hit them in the same order if you re-derive this.

## Where things stand

```
ctor[0]                entry=010b6a09    227 instrs  -> returned to caller
ctor[1]                entry=010d03c5     24 instrs  -> returned to caller
ctor[2]                entry=010d0b3d     46 instrs  -> returned to caller
ctor[3]                entry=010ea589     79 instrs  -> returned to caller
JNI_OnLoad             entry=010d9bad     99 instrs  -> returned to caller
    JNI_OnLoad returned 65542 (JNI_VERSION_1_6)          <-- correct
nativePreInit          entry=010db621     57 instrs  -> PC left the loaded image
    module globals at 0115a36c: vm=08d00400 env=08d00000 <-- both correct
```

Working: ELF load and relocation (3139 relocations, 0 unhandled, 7713 symbols
indexed), the full 183-symbol import table with interworking stubs, the guest
heap, the Thumb-1 decoder, an ARM subset decoder, the JNI environment (229 + 5
table entries, all 12 calls the boot makes are implemented), per-function
profiling, crash triage that translates a host `SIGSEGV` back into a guest
address and register dump, and the oracle.

Not started: **VFPv2**, the browser host, GLES1, audio, assets.

## The immediate next task

**`nativePreInit` returns through its own epilogue but to a corrupted return
address.**

Its trace ends:

```
010db66c  47b0   nativePreInit+0x4c    BLX r3  -> stub 088005d4
088005d4  be00                          (import stub)
010db66e  bdf8   nativePreInit+0x4e    POP {r3,r4,r5,r6,r7,pc}
```

`bdf8` pops PC from the frame, and the value popped is 0 (or 1) rather than the
return sentinel `0x0BADF00D` that `main.c` put in LR. So either the frame was
corrupted, or the function never pushed LR, or the import it called last
clobbered the guest stack.

Things worth checking, cheapest first:

1. **`host_import_call` reads 24 words from the guest stack** (`extra[24]`) for
   every import, regardless of the callee's arity. It only reads, so it cannot
   corrupt anything — but confirm that is still true.
2. `--trace` the few instructions before the `POP`. The ring holds 96 and the
   function is only 57 instructions long, so the whole run is there.
3. Check whether the last import (`088005d4`, index 373) is a JNI entry and
   whether its implementation writes anywhere it should not. `jni_host_call`
   writes into guest memory for `GetIntArrayElements`-style calls.
4. Derive the PUSH at the function's entry (mirror of `bdf8`: `b5f8`) and check
   the prologue actually ran, and that SP at the `POP` equals SP at the `PUSH`.
   `cpu_dump_state()` prints the register file; the crash handler prints it
   automatically on a real fault.

Once `nativePreInit` returns cleanly, the next milestones are `nativeInit`
(seven arguments, one of them a `float*` — watch for VFPv2 being needed the
moment the engine touches floating point) and then the first `nativeRender`
tick, which is where the two numbers get measured.

## A map of the bugs found so far

Worth reading as a list of *classes*, because they recur. All are in
`docs/SPIKE.md` with full detail.

**Instruction-set semantics** (`src/cpu.c`)

1. `ADD rN, PC` read PC as `pc+2`; Thumb reads PC as a *source* operand as
   `pc+4`. Not a corner case: this is how GCC materialises the GOT base, so
   every GOT-relative global access goes through it.
2. `PUSH` stored registers in descending address order. ARM stores the
   lowest-numbered register at the lowest address. Reversing the frame hands
   back a data word as the return address.
3. `BL` vs `BLX` are told apart by **bit 12 of the second halfword**, not by
   `J2`. Getting it wrong runs the ARM PLT through the Thumb decoder.
4. Bit 25 of the ARM load/store offset field is **inverted** from expectation:
   0 selects the 12-bit immediate. This one instruction is on the path of all
   178 imports.
5. ARM data processing clobbered the flags when `S` was clear (`add_c`/`sub_c`
   write all four flags unconditionally).
6. **Thumb `BLX Rm` did not set LR** — treated as a plain `BX`. Invisible until
   a stubbed import is reached through `blx`, at which point the callee returns
   one frame too high. This was the whole reason `JNI_OnLoad` failed.
7. The PC range check rejected the import **stub page**, so the BKPT at a stub
   was never decoded and every host crossing became `HALT_BAD_PC` — which read
   as "the constructors call nothing".

**Loader / host** (`src/elf_loader.c`, `src/host_libc.c`, `src/jni.c`)

8. Symbol index not rebased to the load address, so the profiler reported a
   confident "100% of time in `_end`".
9. Imported **variables** (`__stack_chk_guard`, `__sF`, `__errno`) are not
   functions; pointing their GOT slot at a code stub faults on first read.
10. Import stubs need the **interworking bit** (`stub|1`): the ARM PLT
    dispatches with `ldr pc, [ip]`, and bit 0 selects Thumb state.
11. `JNIInvokeInterface` has **3** reserved slots, `JNINativeInterface` has
    **4**. Assuming 4 for both shifted the whole JavaVM table one slot, so
    `GetEnv` arrived as `DetachCurrentThread`.
12. The guest heap's initial block was never marked free, so an empty 64 MB
    arena reported "out of memory" on the first allocation.
13. JNI arguments were read as strings before dispatch — a `jclass` is a handle
    at `0x40000000`, so that dereferences a gigabyte outside the guest space.
14. The `jni/env/` prefix was not stripped, so no `JNINativeInterface` entry was
    ever routed — while the report blamed the functions rather than the
    dispatcher.

**Harness** (the ones that cost the most time, because they lie)

15. The "returned to caller" sentinel was 0, which is also where a guest lands
    after branching through a null function pointer. A real failure was
    reported as a clean return.
16. The instruction trace ring did not reset per entry, so `--trace` on a
    multi-entry run reported whichever entry ran last.
17. The oracle's copy of the sentinel has to move whenever `main.c`'s does.

## Conventions this codebase follows

Worth preserving; they are load-bearing for the next person.

* **Comments say why, not what.** Every non-obvious line records the failure it
  prevents. Several of the above are only findable because the comment says
  which symptom it caused.
* **Fail loudly and precisely.** Unimplemented things report their name and
  count rather than returning a plausible zero; `trap()` records the raw
  encoding. The worklist is generated by running, never guessed.
* **Let the trap report drive the work.** Do not implement a whole instruction
  class speculatively — run, read what actually traps, implement that.
* **Verify against the oracle immediately** after any decoder change.
* **No premature abstraction.** The interpreter is a switch statement on
  purpose; the guest address space is one flat buffer with unchecked access
  because that is what keeps the inner loop tight.

## What has been considered and rejected

Do not re-litigate these without new evidence; the reasoning is in
`WASM_PORT_ASSESSMENT.md` and the conversation that produced it.

* **QEMU-in-wasm + Linux.** It replaces the ~1–2 month CPU layer (which a
  static translation does better for this binary) with a much larger problem —
  a guest OS, an Android userspace, and GPU emulation — while saving none of
  the JNI, GL or asset work. For an aarch32 guest it also only gets TCI, so it
  would be *slower* than the hand-written interpreter.
* **Decompiling `libgame.so` and recompiling it.** Ghidra's C is not
  compilable, and the `.so`'s `.data`/`.bss` are referenced by absolute
  addresses from its own relocations. Read it for reference; do not try to
  build it.
* **Unicorn as the product.** See above: TCI under wasm, and GPLv2.
* **The Vita port's GPU code.** `vitaGL` is an external library implementing GL
  on **GXM**, Sony's Vita-only GPU API — it is not in this repo and cannot run
  anywhere else. `source/utils/glutil.c` is vitaGL-shaped glue full of
  `psp2/*` and NEON headers. What *is* portable: `source/patch.c`'s GL patches
  (they act on the guest's call stream) and the accumulated knowledge of what
  the game does that is unusual.

## Roadmap and rough effort

| Item | Remaining | Notes |
|---|---|---|
| **VFPv2** | 1–2 weeks | not started; needed as soon as the engine does float maths |
| Deepen oracle coverage | 1 week | currently only the prologue path |
| `nativeInit` → first frame | 1–2 weeks | **produces the two numbers** |
| JNI semantics (`WrapperJinterface`, fonts, Sound) | 2–4 weeks | `source/java.c` in the Vita repo has the logic |
| **GLES1 → WebGL2 fixed function** | **6–10 weeks** | the big one; 58 entry points, no reuse from this repo. Consider `gl4es` or emscripten's `LEGACY_GL_EMULATION` before writing it |
| Audio (WebAudio + OGG) | 1–3 weeks | |
| Assets (926 files) | 2–3 weeks | |
| Browser host (emscripten, threads, saves) | 2–4 weeks | threads are the variable: COOP/COEP + SharedArrayBuffer, or cooperative fibres |

Total for a playable browser build: **roughly 3.5–6 months** of focused work.
That estimate is dominated by GL and the browser host, not the CPU.

## One open question for the user

The repository is local only — **nothing has been pushed to a remote**, per the
original instruction to leave GitHub out of it for now. When it is time:

* `gh` is not installed, there is no PAT, and the GitHub CLI cannot create a
  repository without one. SSH authentication **does** work as `suchangv`, so
  pushing to an already-existing repository needs nothing more.
* The parent `Inotia3-vita` repo is **public**; this one was to be private.

## Ground rules

* **`game/libgame.so` and the assets are Com2uS's copyrighted work.** `game/`,
  `*.so`, `*.apk` and `*.obb` are gitignored. Never commit them, and do not
  publish them — a browser build is far more visible than a Vita homebrew.
* Run `tools/difftest.sh` before committing anything that touches `src/cpu.c`.
