# Handoff

Everything needed to continue this work. Read this first; `docs/SPIKE.md` has the
running engineering log and `docs/REUSE.md` the dependency research.

**State as of 2026-10-16 (later): the engine boots, finds its assets, opens the
archive, and the PNG decoder now gets *into* `inflate` and past the zlib header
check — but decompression still fails, currently with `libpng error: invalid bit
length repeat`.** The first real frame has not been seen yet. The blocker is now
a decode divergence inside zlib's `inflate`, described below.

### Progress since the previous handoff

- **`@assets.ap` is fixed.** The guest's own `ParseOffsetAndLengthAndPath`
  mangles the path, so the host replaces it (`host/ParseOffsetAndLengthAndPath`,
  see `patches.c` / `host_libc.c`). `File::GetFullPathName` also returns the
  *resolved* archive path, so the `mmap` that follows finds the file. The log now
  reads `[path] mode=0 MainMenu.png -> @6490388|148229|game/assets.apk`, then
  `[io] open game/assets.apk ok`, then the archive bytes at that offset.
- **Three decoder bugs fixed** (all required `tools/difftest.sh` to stay green):
  `ARCHIVE-PATH`, ARM `CLZ`, ARM data-processing S-flag carry. See the bug list.
- **`MUL`/`MLA` destination-register bug fixed** — this was the one blocking the
  zlib header check. See the bug list; it is the most instructive of the set.
- **`snprintf`/`sprintf`/`vsnprintf`/`vsprintf` argument conventions fixed** (the
  format is at a different argument for the `n`-suffixed forms) and `fprintf`'s
  first-vararg off-by-one fixed. These were silently producing literal `%s` in
  built paths and hiding error text.
- `setjmp`/`longjmp` are now implemented host-side so libpng can unwind out of
  `png_error` instead of spinning.

The previous handoff's "immediate next task" (the `@assets.ap` parse) is **done**;
its text is kept below only for the history of how it was found.

---

## What this is

A browser port of Inotia 3 that runs the original ARM `libgame.so` — no Android,
no Dalvik, no reimplementation of the game.

```
guest GLES1 → gl4es (MIT) → GLES2 → emscripten → WebGL
```

The decision that shapes everything: **a hand-written ARMv5TE/Thumb-1
interpreter**, because the binary makes a static translation to wasm realistic
later and the interpreter is what will prove that translation correct.

The measured numbers that justified the approach:

| | |
|---|---|
| interpreter, native | 151.9 MIPS |
| interpreter, wasm | **77.7 MIPS = 51% of native** |
| idle/menu-ish frame | 8,963 instructions |
| extrapolated busy frame | 15–30M instructions → **2.8–5.2 FPS** |

So the interpreter is a correctness oracle and a demo, not the shipping path. A
static translation is what makes it playable — which is why the interpreter
matters: it is the thing the translated build gets diffed against.

---

## Repository layout

`Inotia3-vita/wasm/` is its own git repository. The parent repo is public and
gitignores `/wasm/`. Nothing has been pushed anywhere; there is no remote.

```
src/
  guest.h          fixed 144 MB guest address space, one flat host buffer
  elf.h/.c         ELF32/ARM reader, relocation, address-ordered symbol index
  cpu.h/.c         ARMv5TE / Thumb-1 interpreter
  imports.h/.c     the 183 imports: BKPT stubs, dispatch, crossing counters
  jni.h/.c         the JNI environment, built in guest memory
  jni_offsets.h    GENERATED from lib/falso_jni/jni.h — the table order is the ABI
  gl.h/.c          GLES1 state tracker (the fallback when no backend is linked)
  gl4es_bridge.c   the real GL backend: typed adapters into gl4es
  gl4es_protos.h   GENERATED from gl4es' GLES1 header
  asset_zip.c      find an asset inside the .apk
  host_libc.c      bionic shims, the guest heap, file I/O, the path hook
  patches.c        deliberate modifications to the guest binary
  timers.c         the Java-side software timer table
  platform_*.c     host services: native / wasm-freestanding / emscripten
  main.c           the runner
tools/
  build.sh             native | wasm | browser
  build_gl4es.sh       fetch and build gl4es into vendor/ (gitignored)
  gen_gl4es_protos.sh  regenerate gl4es_protos.h
  run_browser.sh       serve build/ and screenshot with headless Chrome
  run_wasm.mjs         node driver for the freestanding build
  oracle.py            Unicorn differential oracle
  difftrace.py         first divergent instruction, with a register diff
  difftest.sh          run the oracle over every entry point
  callers.py           who calls this function (finds direct BL/BLX only)
docs/
  HANDOFF.md       this file
  SPIKE.md         the engineering log: every bug, every finding
  REUSE.md         what off-the-shelf code exists and why each was chosen
```

`game/` is gitignored and must be supplied locally:

```
game/libgame.so      the ARM binary from the APK
game/assets.apk      the original .apk (the engine reads assets out of it)
```

---

## Build and run

```sh
tools/build_gl4es.sh              # once: fetch + build gl4es into vendor/
tools/build.sh native             # -> build/spike        (fast iteration)

./build/spike game/libgame.so --boot --frames=200
```

Flags: `--boot` (constructors → JNI_OnLoad → nativePreInit → nativeInit →
UserAgreeCallback → resumes → nativeRender frames) · `--frames=N` · `--trace`
(last 96 instructions, dumped on a fault) · `--jni-verbose` · `--oracle` ·
`--dump-guest=FILE` · `--trace-insns=FILE` · `--watch=ADDR`

```sh
tools/build.sh browser            # -> build/spike.html + .js + .wasm + .data
tools/run_browser.sh 60 /tmp/x.png
```

**Work on the native build.** It iterates in seconds, has a working crash
handler that translates a host SIGSEGV back into a guest PC and register dump,
and has the differential oracle. The browser build is for validating, not for
finding things.

### Debugging aids, and where they bit

- `--trace` ring + crash handler: the main tool. A host fault reports the guest
  PC, registers and the last 96 instructions.
- `--watch=ADDR` logs host-side writes covering a guest address. Guest stores
  are visible in the trace; a *shim's* writes are not, and that asymmetry is
  what made a canary corruption hard to place.
- The `[imp]` trace in `host_import_call` names every crossing, so a fault
  inside one has a location.
- The `[glcall]` trace in `gl4es_bridge.c` does the same for GL. A wasm trap
  carries no PC and no stack, so the last name printed is the only record.
- `GUEST_CHECK=1 tools/build.sh browser` turns the interpreter's memory access
  into checked accessors that report the address and the instruction.

**Three debugging aids have caused more work than they found.** The texture-dump
block in `gl.c` ran for *every* GL entry point and read `args[8]` as a pixel
pointer — for `glTexCoordPointer(2, GL_FIXED, 0, ptr)` that is a stack word,
and the resulting host fault looked exactly like an engine bug. A third, an
`inflate` probe in `cpu.c`, dereferenced `next_in` out of a NULL `z_stream` and
produced a phantom `SIGSEGV` that looked like a real fault (see the note on
probes below). The first two are now bounded to the calls they are about; the
third is deleted. **When adding one, ask what conditions it runs under, not what
conditions you meant it to run under.**

---

## The immediate next task

**`inflate` fails with `libpng error: invalid bit length repeat`.**

The chain is now: `File::Open` → `MMappedFile::Open` → `mmap` (reads the archive
at the offset) → PNG decode → `inflate`. The header check passes (`0x78da % 31 ==
0`, verified from the division helper's trace), so the failure is in the
Huffman/deflate body of `inflate`, not the wrapper. The error string is one of
zlib 1.2.3's `strm->msg` assignments; `invalid bit length repeat` has two sites,
both in the `CODELENS` loop of `inflate.c` (the `state->have == 0` case for a
repeat code 16, and the `state->have + copy > nlen + ndist` overflow case).

Method that found the last bug, and the one to repeat:

1. `inflate` is **Thumb** and lives at runtime `0x01103774` (verified from
   `.dynsym`; `tools/callers.py`-style lookups are in `/tmp/opencode/funcdis.py`).
   The failing check site is at `0x01103d6c`–`0x01103d7e` (it computes
   `(BITS(8)<<8)+(hold>>8)`, calls `__aeabi_uidivmod` at `0x010d1bd4`, and tests
   the remainder in `r1`).
2. **Add a bounded register-only probe at the suspect PC** in `cpu.c`'s
   interpreter loop. Never dereference a guest pointer from a probe without a
   range check — that is exactly what produced the phantom SIGSEGV (see the
   cautionary note in the state section). Print registers, then remove the probe.
3. The zlib sources are in `/tmp/opencode/zlib_inflate.c` (1.2.3) and
   `/tmp/opencode/pngread124.c` (libpng 1.2.24) if the scratch dir survives.

The likely shape is another wrong-bitfield / wrong-format decode, given `MUL`
was. **The differential oracle would find it in one step if it could run past
the first import** — `inflate` calls `memcpy`/`memset`, and the oracle stops at
the first host crossing (oracle.py's `on_code`). Extending the oracle to
implement `memcpy`/`memset`/`memcmp`/`strlen`/`strncpy` faithfully (a handful of
lines over `uc.mem_read`/`uc.mem_write`) is the highest-leverage change here; it
turns every decoder question from "probe and guess" into "diff and point".

Also worth fixing: **`tools/difftest.sh` coverage is four constructors running
9–102 instructions.** `MUL` was wrong the whole time and never exercised. A
synthetic difftest entry that runs each decoded instruction family once (or the
`inflate` prologue, whose memory image can be captured) would have caught it.

### Superseded: the `@assets.ap` parse (fixed)

Kept for the history of how it was found. The engine used to receive
`@6490388|148229|assets.apk` and open `@assets.ap`.

Everything upstream of it works. From a run:

```
[assets] values[] <- offset=6490388 length=148229        ← the offset reaches the engine
[path]   mode=0 MainMenu.png -> @6490388|148229|assets.apk  ← the host's path hook
[io]     open @assets.ap   FAILED                          ← and this is what it opens
```

`@assets.ap` is ten characters — one short of `assets.apk`, which reads like a
truncating destination buffer. **It is not truncation in this port**: the same
string passed through *unchanged* when it did not begin with `@`
(`assets.apk|6490388|148229` → `open assets.apk|6490388|148229`). So the `@` is
a real marker, something parses it, and its output is what needs matching.

**Where the parse is not:** `File::Open(char const*, int)` — disassembled, it
has no compare against `0x40` and no call to `ParseOffsetAndLengthAndPath`.
`tools/callers.py` finds no direct caller of `File::GetFullPathName` either, so
the path goes through a function pointer.

Cheapest ways in, in order:

1. **Use `patches_replace_with_import` to instrument.** It can point any symbol
   at a host handler, and a handler that logs its arguments and then forwards is
   a log point with no guest-address unknown. `ParseOffsetAndLengthAndPath`
   (`_Z27ParseOffsetAndLengthAndPathPiPci`, 202 bytes) is a good target: log the
   buffer it receives and the one it leaves.
2. **Read `ParseOffsetAndLengthAndPath` properly.** Its disassembly is in
   `docs/SPIKE.md`'s neighbourhood: it scans for `|` (`0x7C`), and there is a
   compare against `-` (`0x2D`) suggesting signed parsing. The offsets may not
   be plain decimal.
3. **Try formats.** The harness makes this cheap: change the `snprintf` in
   `host_file_get_full_path_name`, rebuild (seconds), and read what
   `[io] open` reports. Three formats are already ruled out and recorded in the
   commit `08fc5fc`.

**Read the parent repository's `source/patch.c` first.** Its
`File_GetFullPathName_hook` is the same fix, it works, and its comments explain
the failure mode this port is hitting ("that branch is the ONLY place File::Open's
offset/length fields get written at all -- skip it and those fields keep whatever
was already sitting in that stack slot"). Corridas 34, 35, 37 are all this area.

### What happens once it works

The chain from there: `File::Open` → `MMappedFile::Open` → `mmap` (implemented
and reads the archive at the offset) → PNG decode → `glTexImage2D` with real
pixels. Only the PNG decode is left.

Verify with the framebuffer dump in `gl.c`: it writes the engine's 384×256
RGB565 surface to `fbNNN.raw`. Today those are 0/98304 or a single flat colour;
**a dump with many distinct colours is the first frame.** (`fb001.raw` is now
dumped from the resume path, so there is a file to watch; it is still
`0/98304`.)

### A cautionary note on probes

A temporary probe added to find the zlib failure dereferenced `next_in` read out
of the `z_stream`, and when `inflate` was entered with `strm == NULL` that
pointer was garbage — so the **probe itself** faulted, and the reported
`SIGSEGV at pc=0110378e` was an artefact of the diagnostic, not the emulator.
Building with `-DGUEST_CHECK` turned the wild read into a clean `guest: load8 at
bad address …` and made that obvious. Lesson: a guest-pointer read inside a
diagnostic needs the same range check a shim does, and `-DGUEST_CHECK` is the
fast way to tell a probe artefact from a real fault.

---

## Where things stand, precisely

Working:

| | |
|---|---|
| ELF load + relocation | 3139 relocations, 0 unhandled, 7713 symbols |
| Import table | 183 ELF + 234 JNI entries, interworking stubs |
| Thumb-1 decoder | full coverage, oracle-verified |
| ARM decoder | verified against Unicorn (plus the new MUL/MLA and carry fixes) |
| Oracle | `tools/difftest.sh`, all four constructors instruction-identical |
| Boot sequence | ctors → JNI_OnLoad → nativePreInit → nativeInit → UserAgreeCallback → resumes → frames |
| Timer table | engine clock unified with `GetSystemCurrentTime` |
| Assets | found in the .apk, offsets delivered, the archive **opens** and its bytes are mmapped |
| PNG header | zlib wrapper (`78 da`) decoded and validated |
| GL | gl4es linked and live in the browser; GL_FIXED converted by hand |
| Browser build | boots in Chrome, frames run, traps in `gl4es_glLoadIdentity` |

Not working yet: **PNG `inflate` body** (`invalid bit length repeat`) · VFPv2 ·
the browser host proper (canvas loop, input, audio, saves) · AOT translation ·
threads.

### Known break: the freestanding wasm target

`tools/build.sh wasm` builds, but `node tools/run_wasm.mjs` fails with
`Import #0 "env": module is not an object or function`. The cause is the pattern
this port keeps meeting: **with `--allow-undefined` an undefined symbol becomes
an import from `env` rather than a link error**, and the failure surfaces at
instantiation with no hint of which symbol. `asset_zip.c` was guarded for
`WASM_FREESTANDING` after `jni.c` started calling into it; a stub was added, and
one more undefined reference evidently remains. Dump the wasm's import section
(parse section id 2) to name it.

This only affects the throughput-measurement target. The numbers above were
measured before the break and remain valid, but re-measuring needs this fixed.

Also note: **`build.sh wasm` and `build.sh browser` both write
`build/spike.wasm`** and clobber each other's output.

---

## The bug classes, because they recur

Fifteen-plus bugs, all found by running. Grouped by what they teach.

**Instruction-set semantics** (full detail in `docs/SPIKE.md`)

1. `ADD rN, PC` reads PC as `pc+4`, but `R[15]` holds `pc+2`. This is how GCC
   materialises the GOT base, so every GOT-relative global access goes through it.
2. `PUSH` stores the lowest register at the lowest address. Reversing the frame
   returns a data word as the return address.
3. `BL` vs `BLX`: **bit 12 of the second halfword**, not `J2`.
4. Bit 25 of the ARM load/store offset field is **inverted** from expectation.
5. ARM data processing clobbered the flags when `S` was clear.
6. **Thumb `BLX Rm` did not set LR** — treated as a plain `BX`. Invisible until a
   stubbed import is reached through `blx`, then the callee returns one frame up.
7. The PC range check rejected the import **stub page**.
8. Thumb format 11's `Rd` is bits[10:8], not bits[2:0].
9. The ARM branch target is **PC + 8**, not PC + 4.
10. A `va_list` is a plain pointer to the argument list — not the AAPCS struct,
    and not the flat array the other formatting entry points take.
11. **ARM `MUL`/`MLA`: the destination is bits[19:16], not the data-processing
    `Rd` (bits[15:12]).** bits[15:12] is the MLA *accumulator*, and zero for
    `MUL`. Because the field is almost always zero in compiler output, the old
    code wrote every product into `r0` and left the real destination stale.
    This went unseen until `__aeabi_uidivmod` (`mul r3,r2,r0`) computed the
    remainder: `r0` was clobbered with the dividend, so `0x78da % 31` returned
    `0x78d6` instead of `0`, and zlib rejected the valid PNG header. Fixing this
    is what moved the failure from "incorrect header check" to the real deflate
    error. **The long forms (`UMULL` etc.) were already correct** — there
    bits[15:12] *is* `RdLo` — which is why the bug was easy to miss.
12. **ARM `CLZ` was not decoded at all** — it shares the data-processing space
    and fell through to `CMN r15,Rm`, a silent no-op. `__udivsi3` is built on it,
    so every unsigned division was wrong. Now matched explicitly
    (bits[27:20]=0x16, rn=15, bits[11:4]=0xf1).
13. **ARM `S`-flag carry.** For data processing with `S=1`, `C` is the *shifter*
    carry for the logical ops (AND/EOR/TST/TEQ/ORR/MOV/BIC/MVN) but the *ALU*
    carry for the arithmetic ops (SUB/RSB/ADD/ADC/SBC/RSC/CMP/CMN). The old code
    overwrote `C` with the shifter carry unconditionally, so `subs r2,r1,#1`
    carried wrong and `__udivsi3`'s first `blo` fired.
14. **`snprintf`/`sprintf`/`vsnprintf`/`vsprintf` do not share an argument
    layout.** The `n`-suffixed forms carry a size between the buffer and the
    format (`snprintf(buf, size, fmt, …)`), the others do not. Treating them as
    one shape meant `snprintf` read its *size* (`0x400`) as the format pointer,
    so the format resolved to an empty string and path builders produced literal
    `%s` in file names like `/sdcard/Android/data/%s/files/%s`.
15. **`fprintf`'s first vararg is argument 2**, not 1 (the stream is 0, the
    format is 1). Passing `&args[1]` fed the *format pointer* to the first
    conversion, so `"libpng error: %s"` printed a literal `%s` and hid the real
    message for a whole debugging session.

**The boundary between guest and host** (this is the fertile ground)

16. **A `jstring` is a handle, not a pointer.** *And the same mistake was made
    twice more with a `jintArray`.* A handle is `0x400000xx`, above
    `GUEST_MEM_SIZE`, so a range check silently rejects it and nothing is
    written. The engine then reads zeros and the whole path reports success.
17. **`__errno` is a function**, not a variable — and the test is where the
    symbol is reached from: `.rel.plt` (called) vs `.rel.dyn` (addressed).
18. **A stub that returns success and does nothing is worse than one that
    fails.** `fstat` was that, and it hid the asset chain completely.
19. **Imported variables need storage, not stubs** — `__sF`, `__stack_chk_guard`.
20. `mmap` has to actually read the file; and its arena check has to be
    overflow-safe, or a size near 4 GB wraps past it.
21. **The path hook must return a path the host can actually `open`, and it must
    carry the `"@offset|length|path"` form** or `File::Open` never fills in its
    offset/length fields. `File::GetFullPathName` now also treats the *resolved*
    archive path as "already resolved" (the second re-resolve that
    `ParseOffsetAndLengthAndPath` triggers), or it rebuilds a
    `<userdir>/game/assets.apk` that does not exist.
22. **`setjmp`/`longjmp` are load-bearing for libpng.** Stubbed to "return 0",
    `png_error` returned to its caller which called `longjmp` again, forever.
    Implemented host-side (save r4-r11/sp/lr, restore and land back at the
    `setjmp` call site). See `host_setjmp`/`host_longjmp`.

**Patches and hooks**

23. The engine contains anti-tamper that fails closed. `excuteDirectly()` and
    `jIsCracked()` shell out to `ps`, find nothing, and answer "tampered" — which
    **silently skips `startClet()`** and leaves the game booting perfectly and
    doing nothing. See `patches.c`.
24. Subsystems cache the JavaVM in `.bss` and expect a Java-side init to fill it.
    `patches.c` *finds* the copies by scanning for the idiom rather than
    hardcoding addresses.
25. `SetTimer`'s "timer id" is an **opaque handle** (this build passes
    `&g_MainTimer`), not a table index.
26. The engine's state machine is entered from a callback nothing fires.
27. **The guest's `ParseOffsetAndLengthAndPath` is broken in this build** (leaves
    `@assets.ap` from `@6490388|148229|assets.apk`), so the host owns it, the
    same way it owns `File::GetFullPathName`. `patches_replace_with_import`.

**Harness defects — the ones that lie**

28. The return sentinel was 0, which is also where a guest lands after branching
    through a null pointer.
29. The instruction trace ring did not reset per entry.
30. **Three debugging aids read guest memory they had no business reading.** Two
    in `gl.c`, one in `cpu.c` (an `inflate` probe dereferenced `next_in` from a
    NULL `strm`). The resulting SIGSEGV looked exactly like an emulator bug. See
    the cautionary note above.
31. A comment was left unterminated by an edit, so the build failed and runs used
    a stale binary — which made two real fixes look ineffective.

**Test coverage** — `tools/difftest.sh` runs four constructors totalling ~130
instructions. `MUL` was wrong from the start and none of them touch it. Widen it
(a synthetic entry per instruction family, or a captured `inflate` prologue)
before trusting any decoder change.

---

## Conventions

- **Comments say why, not what.** Every non-obvious line records the failure it
  prevents. Several bugs above are only findable because the comment says which
  symptom it caused.
- **Fail loudly and precisely.** Unimplemented things report their name and
  count; `trap()` records the raw encoding. The worklist is generated by
  running, never guessed — same for the Java-method report in `jni.c`.
- **Let the report drive the work.** Run, read what actually traps, implement
  that. The same at every level: the import worklist, the Java-method worklist,
  the GL entry points in use.
- **Verify against the oracle after any decoder change.**
- **Diagnostics must be bounded to the calls they are about.**
- No premature abstraction: the interpreter is a switch statement on purpose.

## What was considered and rejected

Do not re-litigate without new evidence; `docs/REUSE.md` has the detail.

- **QEMU-in-wasm + Linux** — replaces the CPU layer with a guest OS, an Android
  userspace and GPU emulation, and saves no JNI, GL or asset work. For an
  aarch32 guest it only gets TCI, so it would be *slower*.
- **Decompiling and recompiling `libgame.so`** — Ghidra's C is not compilable and
  the `.data`/`.bss` are referenced by absolute addresses.
- **Unicorn as the product** — TCI under wasm, and GPLv2. It is a test oracle
  only.
- **Emscripten's `-sLEGACY_GL_EMULATION`** — emulates legacy *desktop* GL 1.x,
  not the GLES1 API surface; the `x`-suffixed fixed-point calls are absent.
- **Writing a GLES1 renderer** — gl4es has the complete API surface, an
  emscripten build target and an MIT licence.
- **The Vita port's GPU code** — `vitaGL` implements GL on GXM, which is
  Vita-only. What *is* portable is `source/patch.c`'s accumulated knowledge, and
  that has been used repeatedly.

## Ground rules

- **`libgame.so` and the assets are Com2uS's copyrighted work.** `game/`, `*.so`,
  `*.apk`, `*.obb` are gitignored. Never commit or publish them — a browser build
  is far more visible than a Vita homebrew.
- Run `tools/difftest.sh` before committing anything that touches `src/cpu.c`.
