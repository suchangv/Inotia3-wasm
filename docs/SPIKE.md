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
| Thumb-1 decoder | **working** — full format coverage, oracle-verified |
| ARM decoder | **working** — verified against Unicorn |
| VFPv2 | **not started** — traps with its encoding recorded |
| JNI environment | **done** — 229 + 5 table entries in guest memory; 12 boot calls implemented |
| Java-method worklist | **done** — the report names every method the guest calls |
| GLES1 state layer | **done** — texture IDs, bindings, enable bits, queries (not a renderer) |
| GLES1 renderer → WebGL2 | **not started** — the 6-10 week item |
| Differential oracle | **working** — `tools/difftest.sh`, all entries identical |
| Boot sequence | **completes** — ctors, JNI_OnLoad, nativePreInit, nativeInit, both resumes, nativeRender |
| Assets | **working** — offsets delivered, `File::Open` opens the archive, bytes are mmapped |
| PNG decode | **done** — menu art and world textures decode |
| First real frame | **done** — the menu and gameplay both render |
| Browser host | **working** — reaches the menu and gameplay; see HANDOFF |

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

---

## Session log — 2026-10-16, the PNG decode push

The engine now gets all the way to decompressing a PNG. Getting there fixed a
batch of small, high-consequence bugs; recorded here because the *shape* of each
repeats.

**Path / archive**

- The guest's `ParseOffsetAndLengthAndPath` leaves the wrong buffer contents in
  this build (`@6490388|148229|assets.apk` → `@assets.ap`), so the host replaces
  it and returns the bare, openable path. `File::GetFullPathName` returns the
  *resolved* archive path and must recognise that path on the second re-resolve,
  or it rebuilds a path under `<userdir>` that does not exist. Result:
  `[path] MainMenu.png -> @6490388|148229|game/assets.apk`, then
  `[io] open game/assets.apk ok`.
- `setjmp`/`longjmp` were stubbed to no-ops; libpng's `png_error` path then spun
  forever. Now implemented host-side.

**Decode**

- **ARM `CLZ`**: not decoded; fell through to `CMN r15,Rm`. `__udivsi3` is built
  on it, so unsigned division was broken.
- **ARM `S`-flag carry**: logical ops take the shifter carry, arithmetic ops take
  the ALU carry. Overwriting `C` with the shifter carry broke `subs` and thus
  `__udivsi3`'s first `blo`.
- **ARM `MUL`/`MLA` destination**: bits[19:16], not the data-processing
  `Rd` (bits[15:12]). Because that field is zero for `MUL`, every product went to
  `r0`. `__aeabi_uidivmod`'s `mul r3,r2,r0` then clobbered the quotient, so the
  remainder of `0x78da % 31` was wrong and zlib rejected the valid header.
  The long-multiply forms (`RdLo`/`RdHi` *are* bits[15:12] and [19:16]) hid
  this at the time, but they were **not** in fact correct — see the 2026-10-17
  entry below. The claim that they were "already correct" is the note that
  kept the next bug hidden for a day.

**Formatting**

- `snprintf`/`sprintf` and `vsnprintf`/`vsprintf` do not share an argument
  layout: the `n`-suffixed forms carry a size before the format. Reading the
  format from `a1` for all four meant `snprintf` treated its size as a pointer.
  Built paths came out with literal `%s`.
- `fprintf`'s first vararg is argument 2, not 1; the old index made the format
  pointer the first conversion, hiding libpng's error text.

**Harness**

- `tools/difftest.sh` was green throughout because it exercises four constructors
  totalling ~130 instructions and none of them use `MUL`. **Coverage, not the
  oracle, is the gap.**
- A temporary `inflate` probe dereferenced `next_in` from a NULL `z_stream` and
  faulted *itself*; the resulting `SIGSEGV at pc=0110378e` was an artefact.
  `-DGUEST_CHECK` would have labelled it immediately (`guest: load8 at bad
  address …`). All probes are now removed.

### Where it stops now

`inflate` (runtime `0x01103774`, Thumb) passes the zlib header check and then
fails building the dynamic Huffman tables: `libpng error: invalid bit length
repeat`. The message is one of two sites in zlib 1.2.3's `CODELENS` loop. The
check site is `0x01103d6c`–`0x01103d7e`; the next step is either a bounded probe
there, or — better — teaching `tools/oracle.py` to perform `memcpy`/`memset`/
`memcmp` so the differential compares *inside* `inflate` instead of stopping at
its first host crossing.

## Session log — 2026-10-17, the touch that did nothing

**Symptom.** The game reaches its main menu and draws it correctly, but no touch
ever activates a control: `state` at +0x0 of the entry under the tap stayed 0
through every click, and the screen never changed.

**What it was not.** The instrumentation (a synthetic pointer DOWN+UP injected
through the real `nativeEvent` path, plus a few register probes) showed the whole
input chain working: the tap reaches `handleCletEvent`, the `0x106e5ce` gate
(`*globalA == 0 && IsIfreeEvent()`) *is* taken, and `UICONTROLLIST_Event` and
`UICONTROL_IsInEventArea` run. The engine's own hit-test even returns 1 for the
correctly-scaled point (233,141). So the routing, the callbacks and the control
rectangles were all fine; the coordinate the engine fed its own hit-test was not.

**Root cause.** `EventHandler_Wrapper` (`0x010daff4`) scales the tap before the
hit-test: x by `g_zoom_rate_horizontal_inv` (0.5) and y by
`g_zoom_rate_vertical_inv`. A probe at the two `__fixdfsi` results read `r0=0xe9`
(233) for x and **`r0=0` for y**, while the two doubles going into `__muldf3`
were correct — `0.5` and `0x3FE2D2D2D3A88913` (0.588, i.e. 320/544). That is the
shape that matters: the *state* was right and the *arithmetic* was wrong.

`__muldf3(240.0, 0.588)` really did return `0x2050000000000000` — a denormal-ish
`2^-506` — instead of `0x4061A5A5A66E0082`. Feeding the guest's own `__muldf3`
six operand pairs directly (a temporary ARM-mode `spike_call*`, since the libgcc
helpers are the ARM-mode exceptions to this otherwise-Thumb-2 image) showed the
pattern: `467*0.5`, `240*0.5`, `240*1.0` were exact, but `240*0.75` became
`0x4040000000000000` (32.0) and `240*0.588` collapsed. The failures were exactly
the pairs whose mantissas needed both halves of a 64-bit product.

- **ARM long-multiply routing.** The multiply block split `MUL`/`MLA` from
  `UMULL`/`UMLAL`/`SMULL`/`SMLAL` on **bit22**, but the ARM encoding uses
  **bit23** for that split; bit22 is only the *signed* flag of the long forms
  (`UMULL`/`UMLAL` have bit22 = 0, like `MUL`/`MLA`). Every `umull`/`umlal`
  therefore ran down the 32-bit path: it wrote only the low word to
  bits[19:16] and left the high word untouched, so all of `__muldf3`,
  `__divdf3` and `__fixdfsi` computed garbage whenever a product needed its high
  half. `-W` nothing; it is a silent wrong-answer, which is why it survived a
  menu, rendering, and (uselessly) `difftest.sh`.

Note the shape, because it repeats: **the engine state was correct and the
interpreter was wrong.** Two prior sessions had attributed the dead touch to the
engine (a popup gate, a missing orientation getter, "the engine expects touches
already in UI space"); a single register probe at the multiply's result settled
it in one line.

**Also corrected here.** The 2026-10-16 entry asserted the long-multiply forms
were "already correct". They were not; only the `MUL`/`MLA` destination field
had been fixed, and the routing bug above meant the long branch was dead code
for `UMULL`/`UMLAL`. That false note is what let this one hide.

**After the fix.** A tap at (467,240) → (233,141) in UI space now fires
`MAINMENU_EventNewgame`: the UI list changes from the 9-entry main menu to the
5-entry slot screen (`MAINMENU_EventSlot` callbacks), while a no-tap control run
stays on the 9-entry menu. The UI acts on touch.

## Session log — 2026-10-17, the slot screen that never left

**Symptom.** With the touch fix in, tapping a save slot hung the run: one frame
consumed the entire 3e9-instruction budget and stopped the loop. No trap, no
output change — the menu was still the last thing drawn.

**Where the time went.** Dropping `--no-profile` answered it in one run:
`ACT_Initialize` 673M instructions, `ACTPOOL_PopHead` and `ACTPOOL_Free` 551M
each, `ACTPOOL_AddHead` and `ACTPOOL_FreeList` 367M each. A PC histogram (one
sample per ~4M instructions, temp) put the whole loop inside 18 instructions of
the actor-pool API. A guest backtrace at the stop named the chain:
`STATEMAINMENU_EnterSelectClass → MAP_Load → CHARSYSTEM_RemoveMap →
CHARSYSTEM_Free → CHAR_Destroy → ACTPOOL_FreeList`.

**Root cause.** `ACTPOOL_FreeList` pops nodes off a list until the head is NULL.
A probe at its loop point showed the head oscillating between `0x448` and
`0x17f0` and never NULL — both are in the **null-guard** region, not the heap.
`CHARSYSTEM_RemoveMap` reads the character array through `*CHARSYSTEM_pPool`
with no NULL check, and `CHARSYSTEM_pPool` was **0**, so the walk started at
guest address 0, read the zero page as characters, and called `CHARSYSTEM_Free`
on garbage whose `+0x1e0` "actor list" was two low-memory words that
`ACTPOOL_Free` kept re-linking into a two-node cycle. A GUEST_CHECK write-watch
on `CHARSYSTEM_pPool` confirmed it was **never** written.

**The real bug: the runner skipped the game loader.** `CHARSYSTEM_pPool` is
created by `GAMELOADER_ProcessLoad`, stage 5 of 34, driven from the init state.
The runner's forced handoff to the menu (`spike_probe_loader`) fired on the
transition into `INITSTATE_nState == 2` — but 2 is the *loading* state from its
very first frame, so the handoff happened before a single stage ran. (The
counter in the `[loader]` line, `INITSTATE_LoadFrameCnt`, **is** the loader's
own stage: it climbs 1..34, and the engine leaves the load by calling
`STATE_Set(4)` when it reaches 0x22.) The fix waits for `0x22`.

**After the fix.** `NEW GAME → slot → class → confirm` enters the game
(`GAMESTATE_nState` 13, the opening event) and the run no longer trips the
budget. `--tap=frame,x,y` drives that headlessly; see HANDOFF.

**Then the event.** The class confirm lands on the opening *event*: a black
screen with a **SKIP** control at the top right (`GAMESTATE_nState` 13). It is
`GAMESTATE_EnterEvent`, and it builds a two-entry `UICONTROLLIST` — a
full-screen "advance" control and the SKIP button at UI centre `(449,17)`, size
`(80,50)`. Only SKIP leaves it, and it takes **two** SKIP presses: one press
alone does nothing, taps elsewhere do nothing, but two SKIPs (spacings from
~200 to ~1000 frames all worked) move to `GAMESTATE_nState` 2 and the draw count
jumps from 5 to 32 quads per frame. `GAMESTATE` 2 is the play state —
`SetState(2)` is what every "return to gameplay" path calls — and
`SGL_FrameBufferToScreen` then composites the offscreen frame each tick at ~98%
painted. The browser needed the same SKIP at device `(898,29)`; my first attempt
used `(805,30)`, which is just outside the button's left edge at UI x 409, and it
did nothing.

**Note the shape, again.** The engine's own code was correct; the *harness* was
wrong (it took a policy shortcut at the wrong moment). Two sessions' worth of
"is the interpreter broken?" is a reminder to check what the runner did before
suspecting the guest.

---

## Session log — 2026-10-18, the frame rate that was not the interpreter

The complaint was "no sound, broken fonts, and performance bad enough that it is
almost unplayable". Only the third was investigated here, and it turned out to
have two causes, neither of them the interpreter.

**The measurement that made the difference was an A/B, not a profile.** The CPU
profile of the *idle menu* put ~81% of samples inside emscripten's `print`, in
the generated HTML shell, and 18% in `(program)`. The shell's `Module.print`
appends each line to `<textarea id="output">` and then forces
`scrollTop = scrollHeight`, so the engine's one-log-line-per-frame was one forced
DOM reflow per frame on a string that grows all run. Detaching the textarea
mid-run (from the page, via `Page.addScriptToEvaluateOnNewDocument`) took the
animation-frame callback from 47.6 ms to 3.4 ms and the page from 17 to 59.7 fps;
boot to the menu went 89 s -> 23 s. Silencing `console.log` alone changed
*nothing*, at 0.081 ms per call, which is what separates "logging is slow" from
"this particular sink is slow" — worth remembering, because the wrong conclusion
here would have been to throttle the log lines instead of fixing the sink.

**Then the engine's own pacing.** With the host fixed, gameplay still ran 11.9
engine frames per second out of 60 ticks: `[settimer]` traces showed the engine
asking for `interval = 66` every time, and disassembly of `MainProcess` gave the
formula — `deadline = now + APPINFO_nTimer`, `interval = deadline - now2` floored
at 10 (120 when no input is queued, 100 while `gAppSleepInfo` is set).
`APPINFO_nTimer` is written only by `APPINFO_SetTimer`, and
`APPINFO_Initialize` calls it with `movs r0, #0x42` (66 ms = 15 fps) on every
path, right after reading its environment file. The other two callers pass 0xc8
(200 ms) for cloud backup.

Two dead ends are worth recording, because both looked promising:

* The read is `APPINFO_nTimer` at guest `0xd5bfe8`, reached through GOT slot
  `0x140600`. Reading that slot by its *address*, though, gives garbage: `.got`,
  `.data` and `.data.rel.ro` have `Off = Addr - 0x1000`, unlike `.text` and
  `.rodata` where they are equal. That cost a round of "the frame period is a
  code byte?" confusion.
* A temporary poke of the value in the runner settled it in one run: 66 -> 1271
  engine frames per 6500-tick run, 33 -> 2087, 16 -> 6165. One `movs` immediate,
  4.8x the frame rate, 16% more instructions in total.

**Locating the 16 ms knob was the easy part; trusting it is the open question.**
The engine's world may advance with the clock or once per frame, and if it is per
frame then 16 ms frames run the game at ~4x speed. The loader's stage timing
argues for the clock: in a fixed-step run each stage takes 8 ticks (128 ms) at
*both* 16 ms and 66 ms frames, where a frame-counted stage would have taken 5
ticks at one and 40 at the other. `MainProcess` also stamps `CHECKTIME_nCurrentTime`
each frame for the rest of the engine to read, and `CHECKTIME_Initialize` seeds
the item/buff/life deadlines at `now + 15` / `now + 30`. Suggestive, not proof —
so the patch takes its value from `--frame-ms=N` / `?framems=N` and
`docs/PERFORMANCE.md` says what playing it should look like.

**Where the interpreter actually stands at 60 fps** (browser, gameplay): 58.5
engine frames/s, 2,143,247 instructions and 12.21 ms per frame, 176 MIPS, 71% of
wall clock — 1.37x headroom in a 16.67 ms budget, and 2.2x natively (7.6 ms).
The average is fine; the worst frame was 4.4 M instructions (25 ms), and that is
what Tier 1 in `docs/PERFORMANCE.md` is for.

**Two process notes.** `tools/difftest.sh` currently fails before it compares
anything — the oracle side runs 0 instructions — and it fails identically with
this session's changes stashed, so it is an environment problem, not a
regression. And `data/.../hubcheck.sav` is rewritten by the game on every run;
restore it before committing anything else.
