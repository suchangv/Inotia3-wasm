# Spike status

Date: 2026-10-16

## What this spike is for

Two numbers decide whether the browser port goes interpreter-only or pays for a
static translation to WebAssembly:

1. **Instructions retired per unit of guest work**, and the MIPS the interpreter
   actually reaches — native, and under wasm.
2. **How often the guest crosses into the host.** A crossing costs roughly
   50–200 ns in a browser. GL is a few dozen calls per frame and does not
   matter; if the engine's logic frame calls into the host thousands of times,
   the browser budget changes shape entirely.

A third output, unplanned but the most useful so far: the interpreter is a
correctness oracle. Whatever the translated build eventually does, it can be
diffed against this one instruction by instruction.

## Where it stands

| Component | State |
|---|---|
| ELF load + relocation | **done** — 3139 relocations, 0 unhandled, 7713 symbols indexed |
| Import table (183 symbols) | **done** — 178 functions, 4 variables, 84 implemented |
| Guest heap | **done** — first-fit free list with coalescing |
| Thumb-1 decoder | **working** — full format coverage |
| ARM decoder | **working** — verified instruction-for-instruction against Unicorn |
| VFPv2 | not started |
| JNI / GLES1 / audio / assets | not started |
| Differential oracle | **working** — `tools/difftest.sh`, all entries identical |
| JNI environment | **in progress** — tables built, `GetEnv`/`GetIntArrayElements` correct |
| `JNI_OnLoad` | **in progress** — runs, returns -1; see below |
| `nativePreInit` | **runs** — 12 instructions, then reads a null pointer |
| Browser host | not started |

`ctor[3]` now reaches the import stubs and the host crossings count is
non-zero (16 across the constructors: `__aeabi_atexit`, `memcpy`, `malloc`,
`memset`, `pthread_mutex_init`) — see bug 8 below for why it was zero before.

Progress: the four static constructors run real engine code — `SGL_Context`'s
constructor, `operator new`, `jMemory_*` — through the ARM PLT, and their
execution now matches Unicorn instruction for instruction. Coverage is still
shallow (133 instructions) because every import is stubbed to a no-op, so the
guest walks into a NULL soon after; widening it means implementing imports
identically on both sides, which is the next milestone, not a decoder question.

## Findings from the real binary

### The `.plt` is ARM code, and every import goes through it

This is the most consequential discovery so far, and it inverts the usual
reading of "96% of the code is Thumb, so ARM barely matters". `libgame.so`'s
`.plt` is ARM:

```
058990  e28fc600   add  ip, pc, #0x0
058994  e28ccae9   add  ip, ip, #0xE9000
058998  e5bcf3a0   ldr  pc, [ip, #0x3a0]!
```

All 178 imported functions are reached by a Thumb `BLX` into that stub. So the
ARM decoder is on the path of **every host crossing**, and is exercised far more
than its 3.8% share of the code bytes suggests.

Two consequences:

- The ARM decoder cannot be deferred to a later phase; it gates the first
  measurable run.
- The GOT slot must hold `stub|1`, not `stub`. `ldr pc, [ip]` is an
  interworking load on ARMv5: bit 0 selects Thumb state. Without it the stub is
  entered in ARM state and the `BKPT` marker is decoded as half an ARM word.

### `BL` and `BLX` are told apart by bit 12, not by `J2`

The Thumb long-branch encoding is:

```
BL   11110 S imm10   11111 J1 1 J2 imm11     -> stays Thumb
BLX  11110 S imm10   11101 J1 0 J2 imm11     -> switches to ARM
```

The discriminator is **bit 12 of the second halfword (H)**. `J2` is bit 11 and
independent of it. Getting this wrong does not fail at the call — it fails by
running the ARM PLT stub through the Thumb decoder, which turns
`add ip, pc, #0` into an unconditional branch to nowhere.

Verified against observed execution rather than only the ARM ARM:

| call site | halfwords | `bits[15:11]` | target |
|---|---|---|---|
| `0x0ea590` | `f7e4 ff40` | `11111` | `0x0cf414` — Thumb, `operator new` |
| `0x0cf426` | `f789 eaea` | `11101` | `0x0589fe` — ARM, `.plt` |

## The differential oracle

`tools/difftest.sh` runs every guest entry point twice — once under the
hand-written interpreter, once under Unicorn 2.1.4 — and diffs
`tools/difftrace.py`. Both sides start from the *same* relocated memory image
(the interpreter dumps it, the oracle loads it), stub every import to a no-op
returning 0, and stop when PC leaves the image. That leaves pure CPU semantics,
so a mismatch is a decoder bug by construction and the first differing line
names the instruction.

Why Unicorn is an oracle and not the product is in the parent repo's
WASM_PORT_ASSESSMENT.md and the README: its speed comes from QEMU's TCG JIT and
a browser cannot execute generated code, so in wasm it runs TCI — a two-level
interpreter that does strictly more work per guest instruction than a direct
decoder. It is also GPLv2. As a *reference* it is exactly right, and it is the
reason the two bugs below were found in minutes rather than by bisecting
crashes.

Setting it up needs no system packages and no pip: create a venv with
`--without-pip` and unpack the PyPI wheel into its `site-packages`. The
environment is gitignored.

### Harness asymmetries that cost real time

Each of these reported a divergence that had nothing to do with the CPU. They
are recorded because anyone re-running this will hit them in the same order:

1. **CPSR is not comparable verbatim.** The interpreter tracks only N/Z/C/V;
   Unicorn reports the whole architectural word including mode and T bits. Mask
   to `0xF0000000` before comparing, and clear only the flags on the oracle
   side — writing a literal CPSR silently clears the Thumb bit and makes
   Unicorn decode Thumb code as ARM.
2. **`r0`/`r1` must be cleared on both sides at an import.** The interpreter
   cleared them inside `host_import_call`, which oracle mode skips entirely.
3. **The address spaces must match.** The interpreter's space is one flat
   buffer, so a stray write to address 0 is harmless; Unicorn raises
   `UC_ERR_WRITE_UNMAPPED` unless the whole space is mapped. Same memory model,
   or the diff lies.
4. **Both must agree on when a run ends.** The interpreter reports
   `HALT_BAD_PC` when PC leaves the image; with the whole space mapped, Unicorn
   would happily execute the zero bytes at address 0 forever.
5. **The final instruction must be emitted exactly once.** The trace is written
   at the top of the loop for the *previous* instruction, so a run that ends
   before the next top would either miss the last instruction or emit it twice.

## Interpreter bugs found

Each was found by running the real binary and reading the fault report, not by
inspection. They are recorded because they are the class of bug that makes an
interpreter expensive, and because the same traps await the translated build.

1. **`ADD rN, PC` read PC as `pc+2` instead of `pc+4`.** Thumb reads PC as a
   *source* operand as instruction address + 4, but `R[15]` holds `pc+2` (the
   next instruction, which is what `pc = R[15]` wants). The two readings differ
   by 2 and must be reconciled at the read. Not a corner case: `add rN, pc` is
   how GCC materialises the GOT base in position-independent ARM code, so every
   GOT-relative global access goes through it. It surfaced as a bad pointer to
   store into, not as a bad branch.
2. **Symbol index not rebased.** `st_value` is image-relative, but the profiler
   is handed live guest PCs. Comparing the two made every lookup land on the
   highest-addressed symbol — reported as a confident "100% of time is spent in
   `_end`".
3. **`PUSH` stored registers in descending address order.** ARM stores the
   lowest-numbered register at the lowest address, with LR highest. Reversing
   the frame hands back a data word as the return address, so the failure is a
   jump into the middle of `.bss` rather than an immediate crash.
4. **`BL`/`BLX` discriminator** — see above.
5. **Missing interworking bit on import stubs** — see above.
6. **ARM data processing clobbered the flags without S.** `add_c()`/`sub_c()`
   write all four flags unconditionally — they are the flag-setting
   primitives — and `if (S)` only re-wrote them. So `add ip, pc, #0` in the PLT,
   which has no S bit, silently cleared NZCV, and the caller's next conditional
   branch read garbage. Fixed by saving the incoming flags and restoring them
   for the `S == 0` case.
7. **Bit 25 of the ARM load/store offset field is inverted from expectation.**
   `0` selects the 12-bit immediate, `1` selects the register-shifted form —
   the opposite way round from the field's usual name. Getting it backwards
   still produces a structurally valid instruction, so nothing crashes at the
   decode: `0xE5BCF388` decoded as a register offset yields `r8 << 7` and a
   writeback that misses the GOT entirely.

   Both candidates were plausible, so it was settled by fact rather than
   argument: the two addresses differ by exactly `imm12`, and only `ip + 0x388`
   appears in `.rel.plt` as a `R_ARM_JUMP_SLOT` offset. **This single
   instruction is on the path of all 178 imports.**

Two structural notes worth carrying forward:

- **Imported *variables* are not functions.** `__stack_chk_guard`, `__sF`,
  `__errno` and `__page_size` are objects. Pointing their GOT slot at a `BKPT`
  stub faults on first read; they need real guest storage.
- **Only exported symbols have names.** The binary is stripped of `.symtab`, so
  only `.dynsym`'s 5962 functions are nameable; static functions (including the
  `.init_array` constructors) are anonymous. Recovering their ranges is a
  `.ARM.exidx` walk, which is also what an AOT translation wants — the two
  needs coincide.

8. **The PC range check rejected the import stub page.** `cpu_run` bails out
   with `HALT_BAD_PC` when PC is outside `.text`, and that check runs before the
   BKPT at a stub is ever decoded — so every host crossing silently turned into
   "PC left the loaded image" and the crossing count stayed at zero. Visible
   only once the stubs were reachable at all, which the bit-25 fix made
   possible. The executable range is `.text` *plus* the stub page.

## The JNI environment

`src/jni.c` materialises the JavaVM and JNIEnv **inside the guest address
space**, with every slot of both function tables holding a BKPT stub. That is
why `lib/falso_jni` cannot be vendored as-is: on the Vita the guest is native
ARM sharing the loader's address space, so FalsoJNI's host-global `JavaVM jvm`
/ `JNIEnv jni` are valid pointers to hand over. Here they are not. The
*mechanism* had to change; reaching a JNI function is now the same operation as
reaching `malloc`, and JNI calls land in the same crossing histogram.

`src/jni_offsets.h` is generated from `lib/falso_jni/jni.h`. Two things about
it are load-bearing and were both got wrong first:

* **The order is the ABI.** The guest indexes the table by fixed offset, so an
  entry one slot out of place calls its neighbour and the only symptom is a
  wrong value much later.
* **The two tables have different reserved-slot counts.** `JNINativeInterface`
  has 4, `JNIInvokeInterface` has **3**. Assuming 4 for both shifted the whole
  JavaVM table by one slot, and `GetEnv` -- recognisable by its
  `JNI_VERSION_1_6` argument -- arrived as `DetachCurrentThread`. The generator
  now detects the counts instead of taking them as given.

### Where the boot sequence currently stops

```
ctor[0..3]     227, 24, 46, 79 instructions   returned normally
JNI_OnLoad     22 instructions                returns -1
nativePreInit  12 instructions                reads a null pointer
```

The module's own globals are correct after the run --
`0115a36c: vm=08d00400 env=08d00000`, i.e. `JNI_OnLoad` stored the JavaVM and
our `GetEnv` wrote the JNIEnv where `getEnv()` reads it. So the environment is
being reached. What is unresolved is `JNI_OnLoad`'s early-exit test: it calls
`getEnv()` (0x0d8780, 13 instructions, of which 9 ran), and branches to its
`return -1` tail as though `getEnv()` had returned 0 -- while the global that
`getEnv()` returns holds the right pointer. Either the disassembly reading above
is wrong somewhere or `getEnv()` returns before writing it. That is the next
thing to settle, and it is small.

Two harness weaknesses this exposed, both now fixed:

* **The return sentinel was 0**, which is also where a guest lands after
  branching through a null function pointer. A real failure was being reported
  as a clean return. It is now `0x0BADF00C`.
* **JNI arguments were all read as strings.** A `jclass` or `jmethodID` is a
  handle at `0x40000000`, so reading one as a NUL-terminated string
  dereferences a gigabyte outside the guest space. Strings are now read only by
  the functions that take one, and the read is bounds-checked.

## Next steps

1. Widen the differential coverage past the first import. Making both sides
   stub imports to a uniform no-op does work, but the traces then stop being
   symmetric about the stub itself, so the comparison is currently bounded at
   the first crossing — clean rather than deep, which is the right trade while
   the question is "is the decoder right".
2. Let the trap report drive the rest of the ARM work, then VFPv2. Coprocessor
   space and `SWI` should keep trapping with their encodings recorded rather
   than being guessed at — and the first one is checked against the oracle
   immediately, which is the whole point of having built it.
3. Recover function ranges from `.ARM.exidx` so the profile stops under-counting
   and reports anonymous ranges instead of dropping them.
4. Wire in `FalsoJNI` + `java.c` from the Vita port and call `JNI_OnLoad`. The
   guest reaches JNI through the `JavaVM`/`JNIEnv` function tables, not through
   ELF imports, so this is where the existing JNI layer becomes load-bearing.
4. Run to the first real frame and record the two numbers that justify the
   whole exercise.
5. Build for `wasm32`, run under node, and get the wasm/native MIPS ratio on the
   real binary — the synthetic measurement said 58%, and this is the check.

## Reference numbers

Measured on this machine with a hand-written Thumb-1 interpreter and a synthetic
compiled workload (`wasm32` under V8/node 24): **279 MIPS**, i.e. **58% of the
same code native**. The synthetic workload is friendlier than real game code
(more LDR/STR, calls, VFP and PLT stubs), so expect the real figure to be lower.
