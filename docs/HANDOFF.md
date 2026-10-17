# Handoff

Everything needed to continue this work. Read this first; `docs/SPIKE.md` has the
running engineering log and `docs/REUSE.md` the dependency research.

**State as of 2026-10-17 (evening): the game reaches its main menu in the
browser.**

```
boot -> Google Play splash -> com2uS splash -> "Now Loading" -> title -> MAIN MENU
```

The main menu is fully rendered — NEW GAME / CONTINUE / OST & CREDIT / MORE
GAMES, the NOTICE and HIVE corners, every button's ornate frame. That means the
whole chain below it works: asset install, LZMA unpacking, PNG decode, fonts,
UI rendering, the state machine, and the renderer.

**The one blocker left: touches reach the engine but the UI does not act on
them.** See "The immediate next task".

### What is already proven

- **Rendering is correct.** The early "washed out" frames were the com2uS
  splash mid-fade, not a colour bug — the menu renders in full colour. That
  thread can be closed.
- **Input plumbing works.** A pointer event arrives as
  `nativeEvent(0x80000004, slot+1, (x<<16)|y, count)`, and the chain inside the
  engine is `jGetEventType` (a **constant** in this .so: `movs r0,#2` — always
  `EventHandler_Wrapper`, not a JNI call) → `EventHandler_Wrapper` (scales the
  coordinates) → `handleCletEvent(0x18, ...)`, which is the same event id the
  Vita port documents for the title screen's handler.
- **The loader completes.** `INITSTATE_nState` goes 0 → 1 → 2 and the twelve
  data files are installed into the app directory.
- **The CLI baseline is intact.** `tools/build.sh native &&
  ./build/spike game/libgame.so --boot --frames=600 --no-profile` still prints
  `average over 119 frame(s): 8701 instructions` and `final pc=0badf00c`, byte
  for byte as before the runner was split.

### The browser host

The page is a real host: one engine tick per `requestAnimationFrame`, pointer
input, no frame cap unless asked for.

```
tools/build.sh browser
tools/run_browser.sh 60 /tmp/shot.png            # headless, 600 frames
QUERY="frames=200&gltrace=200&dump=1" tools/run_browser.sh 60 /tmp/shot.png
node tools/watch_run.mjs http://127.0.0.1:8080/spike.html 150   # real time + screenshots
node tools/click_at.mjs  http://127.0.0.1:8080/spike.html 467 240 60 /tmp/out.png
node tools/click_test.mjs http://127.0.0.1:8080/spike.html 70   # input protocol
```

Page options: `frames=N` (stop after N; unset runs until closed), `fixed=1`
(16 ms per tick instead of real elapsed time), `gltrace=N`, `dump=1`.
`click_at.mjs` also takes `PRE=<seconds>` or `FRAME=<n>` to wait before
clicking, and `HOLD=<ms>` to separate press from release.

**Headless `--screenshot` cannot reach the menu**: `--virtual-time-budget`
expires after a couple of dozen engine ticks. Use `watch_run.mjs`, which drives
a real browser and screenshots at intervals. At the current interpreter speed
the menu arrives around frame 2800, i.e. ~2.5 minutes in.

`main()` is split into `spike_load` / `spike_boot` / `spike_tick` /
`spike_finish`. The CLI keeps a synchronous loop over `spike_tick`; the browser
hands `spike_tick_browser` to `host_browser_main_loop`. The CLI frame log is
byte-identical to the pre-split build — that comparison is the regression test
for any further change to the split.

Two things about the stop condition and the clock that are easy to get wrong:

- `HALT_BUDGET` is the *stop* signal, not the continue signal. A frame that
  completes normally ends `HALT_BAD_PC` with `r15 == RETURN_SENTINEL & ~1`.
  Inverting this makes the browser run exactly one frame.
- `g_clock` must be seeded from the guest's `sysTime[]`, because that is what
  `SetTimer`'s absolute firetimes are comparable to. Starting at zero puts every
  timer ten hours in the future and the engine just looks idle.

Input lives in `src/input.c`: the Vita port's five-stable-slot state machine
with the device layer removed. The one place its model does not transfer is
*how events arrive* — it polls hardware at ~60 Hz, so a finger that starts and
ends between two polls is impossible, while here a tick runs for 200-300 ms and
a tap is 50-100 ms. A level snapshot would drop the tap entirely, so events go
into a FIFO and are replayed at the frame boundary.

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

### Progress since the browser host (2026-10-17, later)

The game now reaches its main menu. Six bugs stood between "renders the title
screen" and that, and every one of them was found by reading the PS Vita port
first rather than re-deriving the engine:

- **The JNI handle table ran out at frame 600.** `GetStaticMethodID` minted a
  new handle on every call with no dedupe, and the engine resolves the same
  handful of methods every frame. JNI guarantees the id is the same for the same
  (class, name, signature), so the lookup now reuses it.
- **A freed block still read as live.** `TM_Run` frees a dead timer node and
  then re-examines that node's id. On bionic the allocator has clobbered the
  first word, so the walk ends; ours left the payload intact, so it freed the
  same node again — 165 million instructions of it. `heap_free` now poisons the
  block's first word.
- **A cached JavaVM slot was still null.** `patch_vm_caches` required the vtable
  load and the call through it to be adjacent, but the JavaVM idiom loads the
  call's arguments in between. That left `0x0115a34c` null and the guest
  branched through a null `GetEnv`.
- **`open()` ignored its flags** and opened every path read-only, so nothing
  could ever be created. This is the one that stalled the install.
- **`GetUserDir` had no trailing slash** (the engine concatenates the package
  name onto it) and its directories did not exist.
- **`File::GetFullPathName` did not know mode 4**, so a create fell through to
  the "not on disk, so it must be a shipped asset" fallback and was handed a
  read-only archive offset.

Plus `sin`/`cos`/`pow`, which were a placeholder that did not even return 0 —
it left the result register alone.

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

**Make the UI act on a touch.** Everything around it now has a measured answer;
what is left is the last hop.

What the probe in `spike_dump_ui_controls()` established (all of it callable
again by setting nothing — it runs 20 frames after the menu appears):

```
[ui] UI_listControl count=9 entries=019d311c  original=480x320
[ui]   [0] state=0 centre=(240,142) size=(233,48) click=010ba031   <- NEW GAME
[ui]   [1] ...                                                     <- CONTINUE
[ui]   SGL screen=480x320 device=960x544 orientation=0
[ui]   hit(233,141) = 1     hit(467,240) = 0
```

So:

- **The UI lives in 480x320**, the canvas is 960x544. The scale is not uniform
  — x2 across, 544/320 = 1.7 down.
- **A correctly-scaled touch hits.** `UICONTROL_IsInEventArea` — the engine's own
  hit-test — returns 1 for (233,141) and 0 for the raw (467,240). The control
  rectangles and the hit-test are therefore fine.
- **The scaling is not being applied.** `SGL_ChangeOrientationPosition` branches
  on `SGL_GetScreenOrientation() == 1` to decide whether to rescale, and that
  returns **0** here. Neither this port nor the Vita port implements an
  orientation getter, so 0 is the engine's own default — which suggests the
  non-scaling branch is correct for this build and the touch is simply expected
  to arrive already in UI space.
- **The controls' callbacks are named**, and directly resolvable:
  `MAINMENU_EventNewgame` / `_Continue` / `_Credit` / `_TermsOfUse`. Calling
  `MAINMENU_EventNewgame()` from the frame boundary returns cleanly but does
  **not** advance the screen, so the button's full action is not the callback
  alone — it is callback plus whatever `MAINMENU_Process` does on the next tick,
  presumably driven by a latch the callback sets.

One more measurement, and it narrows the last hop a lot. `handleCletEvent`
reaches `UICONTROLLIST_Event` for event 0x18 only through this chain:

```
cmp  r7, #0x18            ; event is 0x18
ldrb r3, [globalA]        ; must be 0
bl   IsIfreeEvent         ; measured: returns 1, so this passes
ldrb r3, [globalB]
ldrh r3, [r3]
cmp  r3, #0x5
bne  -> UICONTROLLIST_Event      ; *globalB != 5: straight there
beq  -> counter <= 9 ? swallow the event and set the counter to 10 : UICONTROLLIST_Event
```

`IsIfreeEvent()` returning 1 rules out the popup gate. What is left is the
`*globalB == 5` branch and its counter: if that is the path taken, the first
tap is swallowed by design and the second should work — but two taps in a row
were tried and neither registered, so either the counter is reset between ticks
or `*globalB` is something else entirely. Reading `*globalB` and that counter at
runtime is the next single measurement, and both are reachable by the same
`spike_call*` technique already in the file.

The two implementation options, cheapest first:

1. **Send touches in UI space.** Scale x by 480/960 and y by 320/544 in the
   input layer before dispatching, so the engine's own hit-test sees the point
   it expects. Cheap, and it is what the numbers above say the engine wants —
   but verify that nothing else reads the touch in device space first.
2. **Do the hit-test ourselves**, the way the Vita port does for its D-pad menu
   navigation: on a tap, walk `UI_listControl`, call `UICONTROL_IsInEventArea`
   for each entry, and invoke the matching callback. This bypasses whatever the
   engine's own routing is missing, and the probe already proves the hit-test
   returns the right answer.

Either way, watch `state` at +0x0 of the entry under the tap: it stayed 0
through every click tried so far, so a non-zero value is the signal that the
press actually registered.

Method that has worked on this codebase, and the one to repeat:

1. **Read the PS Vita port first.** `../source/patch.c` documents ~100 corridas
   of exactly these problems, and `../source/wr_touch.c` is the input state
   machine — its menu navigation reads this same `UI_listControl`. Its offsets
   are for a different APK build, but our `.so` exports most of these globals in
   `.dynsym` (see below), so they resolve by name.
2. **Probe by symbol, log on change only.** `spike_probe_loader()` is the model:
   it reads `INITSTATE_nState` / `LoadFrameCnt` / `MEM_nUsedPoolSize` /
   `GAMELOADER_pText` and logs only when one changes. That turned "stuck on a
   screen" into the literal string `"File installation error (0)"` in one run.
3. **Call guest functions from the frame boundary.** `spike_call1()` /
   `spike_call2()` / `spike_call3()` / `spike_call4()` do this; they are how
   `RES_LoadToHeap("i_character.dat")` and `LZMA_Decode()` were each cleared in
   one step, how the state machine was driven directly, and how the hit-test was
   checked against candidate coordinates. Never from a DOM callback — `g_cpu`
   and `g_budget` are single globals.
4. **Screenshot with `watch_run.mjs`, not `--screenshot`.** Headless
   `--virtual-time-budget` cannot get far enough into a run to see anything
   past the first splash.

### The `.so` exports its own globals

Worth knowing before disassembling anything: `game/libgame.so` puts its loader
state in `.dynsym` as OBJECT symbols, so `elf_find_symbol()` reaches it by name
and survives a different APK build — which the Vita port's hardcoded offsets do
not. Confirmed present: `INITSTATE_nState`, `INITSTATE_LoadFrameCnt`,
`GAMELOADER_pText`, `GAMELOADER_bInstalled`, `MEM_nUsedPoolSize`,
`MEM_nTotalPoolSize`, `INSTALLBASE_pData`/`nRecordCount`/`nRecordSize`,
`SYMBOLBASE_*`, `FILEPACK_*`, `GAMESTATE_nState`, `STATE_fpEnter`/`fpProcess`,
and every `GAMELOADER_*` / `EXCELDATA_*` / `RES_*` / `LZMA_*` function.

### What the previous session found, in case it recurs

The four bugs between "boots and draws" and a picture were: gl4es' GL state
never allocated (`NewGLState` is only called from the EGL path, which `NOEGL`
skips); emscripten's 64 KB default host stack; `patch_vm_caches` writing the
JavaVM into 844 globals whose pattern also matched intrusive list heads; and
GL_FIXED client arrays converted once and sharing one buffer between vertex,
texcoord and colour. All four are described in the bug-class list below.

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

The chain is already proven end to end: `File::Open` → `MMappedFile::Open` →
`mmap` → PNG decode → `glTexImage2D` with real pixels → gl4es → WebGL2 → the
canvas. The main menu is the evidence.

The `fbNNN.raw` dumps that used to be the only way to see the engine's own
surface are now behind `SPIKE_DUMP_FB=1` in `src/gl.c` — they wrote up to 80
files of 192 KB into the working directory on every large texture upload, and
44 of them had been committed by accident. `?dump=1` on the page is the better
tool now: it reads the framebuffer back with `glReadPixels` and prints it, along
with the engine's own surfaces, as base64 images.

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

Twenty-plus bugs, all found by running. Grouped by what they teach.

**Getting past the loading screen** (2026-10-17, later)

These are the ones the PS Vita port had already solved; the lesson is to read
`../source/patch.c` before reverse-engineering anything game-logic shaped.

0f. **A shim that ignores its arguments is invisible until something needs
   them.** `open()` opened every path `"rb"`. The port only ever read for
   months, so nothing showed; the first writer — `GAMELOADER_ProcessInstall`,
   creating twelve data files with `O_CREAT|O_WRONLY` — failed on all twelve and
   the engine reported "File installation error (0)".
0g. **A path with no trailing separator is a different path.** `GetUserDir`
   returned `"/data"`; the engine concatenates the package name straight onto it
   and every path became `/datacom.com2us...`. The Vita port returns
   `"ux0:data/"`. The directories also have to exist — Android creates them, and
   nothing did here.
0h. **An unknown mode value falls into whatever the catch-all was written for.**
   `File::GetFullPathName` handled 0/2/3; mode 4 (a create) fell through to the
   "file is not on disk, so it must be a shipped asset" fallback, which returned
   a read-only archive offset *for a write*. Write intent has to skip that
   fallback.
0i. **A budget sized for a frame is too small for a loader.** The instruction
   budget is a runaway guard, but the install runs twelve LZMA decodes inside
   one guest call. 200M stopped the run mid-install, which is indistinguishable
   from a hang.
0j. **A JNI handle table needs dedupe, not more slots.** Method and field ids
   are stable per (class, name, signature); minting a fresh handle per lookup
   fills any fixed table, and the failure is a null id rather than an error.
0k. **`free()` must not leave the payload readable.** Code that re-reads a freed
   node's first field gets away with it on Android because dlmalloc puts the
   free-list link there. Leaving the data intact turns a benign use-after-free
   into an infinite loop.
0l. **A pattern scan that requires strict adjacency misses the real idiom.**
   `patch_vm_caches` needed the vtable load and the `blx` through it back to
   back, but the JavaVM idiom loads the call's own arguments in between. Scan a
   small window instead — and keep the `blx` requirement, which is what still
   separates a JavaVM cache from an intrusive list head.

**Host and loader** (the browser build's bring-up, 2026-10-17)

0a. **gl4es' GL state was never allocated.** `NewGLState()` is only called from
   the EGL context-creation path, which `NOEGL` skips, so `glstate` stayed on the
   zero-filled default and every matrix call dereferenced a null sub-struct. The
   symptom is a wasm trap with no guest PC and no stack, in a function named
   `gl4es_glLoadIdentity`. Call `NewGLState(DEFAULT_STATE, 0)` yourself.
0b. **Emscripten's host stack defaults to 64 KB.** gl4es and the shims recurse
   past it, and the resulting trap reads "memory access out of bounds" — the
   message names the wrong subsystem entirely. `-sSTACK_SIZE=8388608`.
0c. **A pattern scan that is one instruction short of its idiom matches
   everything.** `patch_vm_caches` looked for "load a global, read a field of
   what it points to" and called it a cached JavaVM; that is also how every
   intrusive linked list is walked, so it matched 844 sites and wrote the JavaVM
   into two list heads — the network worker then read the JNI struct as
   `node->next` and freed it. Requiring the call through the vtable is what
   makes the pattern mean what it claims.
0d. **A GLES1 client array is read at draw time, and its record has to survive
   the draw.** Converting once and clearing the record means only the first quad
   of a run sees the engine's data. And each array needs its own conversion
   buffer: one shared scratch buffer is aliased by all three pointers, so by draw
   time the vertex array holds the colour data.
0e. **A one-character shortcut on a name is not a name test.** `name[2] == 'n'`
   to tell `glEnable` from `glDisable` is never true — `name[2]` is `'E'` — so
   `glEnable` became a no-op and the screen went white with no error at all.

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
