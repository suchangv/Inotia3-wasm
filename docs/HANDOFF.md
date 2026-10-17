# Handoff

Everything needed to continue this work. Read this first; `docs/SPIKE.md` has the
running engineering log and `docs/REUSE.md` the dependency research.

**State as of 2026-10-17 (night): the game reaches gameplay — world map, character
and HUD — in the native runner and in the browser.**

```
boot -> splash -> "Now Loading" -> MAIN MENU
     -> NEW GAME -> save-slot screen -> class screen
     -> opening event (SKIP) -> GAMEPLAY
```

The main menu is fully rendered — NEW GAME / CONTINUE / OST & CREDIT / MORE
GAMES, the NOTICE and HIVE corners, every button's ornate frame — and gameplay
draws the village map, the character, an NPC, the minimap and the button ring.
The chain above runs end to end in both hosts: asset install, **the game
loader's 34 stages**, LZMA unpacking, PNG decode, fonts, UI rendering, the state
machine, and the renderer.

Reaching gameplay is two steps. The class confirm drops into the opening event
(`GAMESTATE_nState` 13 — a black screen with a **SKIP** control at the top
right), and two presses on SKIP land in the play state (`GAMESTATE_nState` 2,
where every "return to gameplay" path lands), which draws the map. One SKIP
press is not enough, and taps elsewhere never leave the event.

**Nothing blocks reaching gameplay.** See "The immediate next task" for the
loose ends (audio, the save round-trip, and the unusable `difftest.sh`).

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
the menu arrives around frame 990, i.e. ~1.5 minutes in.

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

### Progress since that (2026-10-17, the touch bug)

**The UI acts on a touch.** A tap on NEW GAME advances the screen: the 9-control
main menu is replaced by the 5-control slot screen (`MAINMENU_EventSlot`
callbacks), while a no-tap control run stays on the menu. The blocker was in the
*interpreter*, not the engine. `cpu.c` split `MUL`/`MLA` from the long
multiplies on bit22 of the ARM encoding, where the split bit is **bit23** — bit22
is only the *signed* flag of `UMULL`/`UMLAL`/`SMULL`/`SMLAL`, which are zero for
`UMULL`/`UMLAL` exactly like `MUL`/`MLA`. Every `umull`/`umlal` therefore ran
down the 32-bit path and wrote only the low word, so `__muldf3` lost the high
half of its product. `EventHandler_Wrapper` scales a tap's y by
`g_zoom_rate_vertical_inv` (0.588); `240 * 0.588` came back as `2^-506`, the y of
every tap collapsed to 0, and no control was ever hit. Full write-up in
`docs/SPIKE.md`, "the touch that did nothing".

That invalidates the engine-side theories recorded further down (`IsIfreeEvent`,
the orientation getter, "the engine wants touches already in UI space"): the
engine state was correct throughout. The two measurements that found it were the
`__fixdfsi` results of the two scales — x gave 233, y gave 0 — and a direct
`__muldf3` unit call from the frame boundary.

### Progress since that (2026-10-17, the loader-skip hang)

**The game enters gameplay.** Tapping a save slot then hung the run — one frame
burned the whole 3e9-instruction budget — and the loop turned out to be a
consequence of the *runner*, not the engine. The forced handoff to the menu in
`spike_probe_loader` fired on the transition into `INITSTATE_nState == 2`, but 2
is the **loading** state from its first frame, so the handoff skipped all 34
`GAMELOADER_ProcessLoad` stages — including `CHARSYSTEM_Create` at stage 5. That
left `CHARSYSTEM_pPool` NULL, and the first save-slot tap died in
`CHARSYSTEM_RemoveMap`, which walks that pool unconditionally: a null base made
it read the zero page as characters, and `CHAR_Destroy` then drained a bogus
two-node actor list in `ACTPOOL_FreeList` forever.

The fix waits for the counter the loader stops at (`INITSTATE_LoadFrameCnt ==
0x22`) — the same point the engine itself leaves the load, where the Vita port's
Corrida 87 puts its handoff. The chain then runs `NEW GAME → MAINMENU_EventSlot →
STATEMAINMENU_EnterSelectClass → STATE_Set(7) → STATE_ProcessGame` (the opening
event, `GAMESTATE_nState` 13) → two SKIP presses → the play state
(`GAMESTATE_nState` 2), which draws the map. Full write-up in `docs/SPIKE.md`,
"the slot screen that never left".

Two things worth knowing from this: the `frame=N` in the `[loader]` log line
**is** the load stage (`INITSTATE_LoadFrameCnt` doubles as the GAMELOADER stage
counter), and `--tap=frame,x,y` (device pixels) drives the whole chain in a
headless native run.

### Progress since that (2026-10-18, the frame rate)

**Gameplay now holds the engine's own rate in the browser: 15.0 engine frames per
second, up from 11.9.** Three causes, none of them the interpreter:

* **The host log sink.** The generated page's `Module.print` appended every
  engine log line to `<textarea id="output">` and then forced
  `scrollTop = scrollHeight` — a DOM reflow per frame on a string that only
  grows (237,364 characters by the time gameplay started). It cost 47.6 ms of
  every 60 ms animation frame on the idle menu, where the engine itself does
  0.26 ms. `tools/page-options.js` now routes `Module.print`/`printErr` to the
  console: emscripten binds its stdout sink once, at module init, and `--pre-js`
  runs before that. Boot to the menu went 89 s -> 23 s, and the *page* went from
  17 fps to its full 60.
* **The port's own clock was 20% slow.** `g_use_fixed_step = !g_browser_fixed`
  inverted the `?fixed=1` flag, so the browser was pinned to a deterministic
  16 ms tick even though the comment next to it says the browser runs on real
  elapsed time; and `g_clock += (int64_t)dt` then truncated each ~16.7 ms tick to
  16 ms. That matters because the engine re-arms itself at 66 ms and the timer
  fires on the first tick whose clock has passed it: 16 ms steps put the engine's
  frame on the 5th tick (80 ms, 12.5 fps), ~16.7 ms steps on the 4th (66.8 ms,
  15.0 fps). Fixed both; the native CLI keeps the fixed step, so every recorded
  measurement is unchanged (`--frames=6500` still gives 1271 frames).
* **The engine's own frame period — not touched.** `MainProcess`, the callback
  `g_MainTimer` dispatches, re-arms at `APPINFO_nTimer - (work it just did)`, and
  `APPINFO_Initialize` hardcodes `APPINFO_nTimer = 66 ms`: 15 fps by design, on
  every path through that function. Raising it was measured (16 ms gives 6165
  engine frames instead of 1271 over the same 104 s, for ~16% more instructions)
  and the machinery is in `patches.c` behind `--frame-ms=N` / `?framems=N`, but
  it is **off by default**: the world advances once per `MainProcess` frame and
  that same frame is what draws, so a faster frame period accelerates the game
  rather than smoothing it.

The engine's rate is now met with ~5x headroom: 2,150,383 instructions and
12.77 ms per engine frame in the browser, 168 MIPS, 19% of the wall clock, and a
66.8 ms budget. The measurements, the 60 fps experiment, and what is left (boot
time; the profiler being on by default) are in
[`docs/PERFORMANCE.md`](PERFORMANCE.md).

At that point text did not render yet — the opening event's dialogue box came up
empty — and audio was not wired up. Both were next, and both are separate from
the frame rate.

---

### Progress since that (2026-10-18, the text)

**Text renders.** The class description (word-wrapped to its rect), the stat
panel (`STR 30 / DEX 20 / … / LUK ??`), the `Create a new character` title and
the save slots' `NO DATA` all draw — same code and same result in the browser
and in the native runner.

The engine does not draw text itself. `SGL_Font_Android::RenderToTextureInRect`
hands the string to *Java* (`jDrawTextInRect`, as a `byte[]`) and reads the
result back as an alpha mask out of the `pixel[]` buffer `nativeInit()` was
given, keeping only the alpha byte (`>> 24`). There is no Java here, so the port
is that renderer: `src/text.c` lays the string out with the engine's own bitmap
font — `FONT_pFontData`, the same glyph records the engine's native
`GRP_PutAsciiChar` reads, so the shapes and metrics match everything else it
draws — and writes ARGB into that buffer. `src/jni.c` supplies the four methods
it calls (`GetStringWidthHeightInRect`, `GetStringWidth`, `DrawTextInRect`,
`DrawText`).

Three things sat between "the bytes are right" and "the picture is right":

* **`Get/Set<T>ArrayRegion` had the JNI argument order wrong.** `(env, array,
  start, len, buf)` was read as `(env, array, buf, len, start)`, so the
  `SetByteArrayRegion` that fills `jDrawTextInRect`'s `byte[]` copied nothing
  and the engine's string arrived as zeros. The `start == 0` case was skipped
  outright, which is every whole-array copy.
* **The handle table freed objects, not references** — bug class 16b below.
  This is the one that made the text *invisible*: the engine's cached `int[]`
  was freed at init and its slot came back as a methodID, so
  `SGL_GetTextSizeInRect` read `(0,0)` through a handle that was no longer an
  array. `RenderToTextureInRect` sizes its texture from those two numbers, so
  every string became a **1x1** `GL_ALPHA` texture while the `glTexSubImage2D`
  still uploaded the full 64x16 mask into it — a blit that reports success and
  draws nothing.
* **A diagnostic that prints a *derived* value can hide the input.** The
  rasteriser substitutes the natural width when `rect_width <= 0` (0 means
  "unlimited" here), and its log printed the substituted value — so the
  engine's `0` read as `40` in every log line, and the search went to the GL
  layer for a while when the texture size was the real signal. Log the argument
  and the interpretation separately.

The GL side needed no change at all. The text is a `GL_ALPHA` texture modulated
by a white vertex colour, and gl4es/WebGL resolves that to white with the mask's
alpha, exactly as the engine expects on Android. The bisect that established
this (a solid fill of the same rect, then a trace of the draw) is removed again,
per the ground rule that diagnostics do not stay.

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
UserAgreeCallback → resumes → nativeRender frames) · `--frames=N` ·
`--tap=frame,x,y` (inject a tap at a frame; device pixels) · `--trace`
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

**Nothing blocks reaching gameplay. These are the loose ends.**

Gameplay is confirmed in both hosts — the native runner and the browser — with
the map, the character, the HUD, the minimap and the text drawn. What is left is
adjacent:

- **Audio.** The engine opens its audio files; nothing is mixed yet.
- **The opening event leaves only on SKIP, and it needs two presses.** The class
  confirm drops into `GAMESTATE_nState` 13 (a black screen with a **SKIP**
  control at the top right, UI centre `(449,17)`). One SKIP press is not enough:
  two SKIP presses (any spacing tried, ~200..1000 frames apart) reach the play
  state `GAMESTATE_nState` 2, which draws the map; taps elsewhere never leave it.
  A device probably also ends it on a timer or on a tap anywhere; if the port
  needs that, the event system (`EVTSYSTEM_*`) is where to look.
- **A save slot.** After entering play the log still shows `save0.dat` missing.
  That may be correct (nothing is saved until the player saves), but confirm a
  new-game save round-trips.
- **`tools/difftest.sh`** — the oracle is still unusable; see "The differential
  oracle" below.

To reproduce the native chain (device pixels, one tap per line):

```
tools/build.sh native
./build/spike game/libgame.so --boot --frames=6500 --no-profile \
    --tap=1500,467,240 \
    --tap=2000,480,151 \
    --tap=2400,136,459 \
    --tap=2800,480,359 \
    --tap=5000,860,60 \
    --tap=5600,860,60
```

And the screen geometry, straight from `spike_dump_ui_controls()`:

```
main menu     (9 controls): [0]     centre=(240,142)   size=(233,48)  click=010ba031  <- NEW GAME
slot screen   (5 controls): [0..2]  centre=(240,89/171/253) size=(288,80) click=010bb779
class screen (11 controls): [0..5]  centre=(68..408,270) size=(68,50)  click=010bc671
                            [9]     centre=(240,211)   size=(100,40)  click=010bc665  <- confirm
opening event (2 controls): [1]     centre=(449,17)    size=(80,50)                  <- SKIP
```

- **The UI lives in 480x320**, the canvas is 960x544, and the split is
  non-uniform — x2 across, 544/320 = 1.7 down. The engine applies that itself in
  `EventHandler_Wrapper` from the `g_zoom_rate_*` globals, so the input layer must
  **not** pre-scale. Device coords are `2 * UI.x` across and `UI.y / 0.588` down,
  which is what `--tap` takes and what `tools/click_at.mjs` maps the browser
  clicks onto.
- **A tap has to go through `nativeEvent`, not the callback.** Calling
  `MAINMENU_EventNewgame()` from the frame boundary returns cleanly but does not
  advance the screen: the advance needs the callback *plus* the next `Process`
  tick. `--tap` injects `INPUT_DOWN` then `INPUT_UP` through the real path.
- **The handoff fires on `INITSTATE_LoadFrameCnt == 0x22`**, not on the enter of
  the loading state — see the progress note. `INITSTATE_LoadFrameCnt` is the
  loader stage, so a change to the loader length would move this number.
- **A game screen is not the outer state.** `STATE_fpProcess` stays
  `STATE_ProcessGame` throughout; the screens are `GAMESTATE_nState`, switched by
  `GAMESTATE_SetState`. `2` is the play state (the target of every "return to
  gameplay" path); `13` is the opening event.

The register probes used to find the loader-skip hang were removed again, per the
ground rule that diagnostics do not stay; `src/cpu.c` is byte-identical to
`530571b`.

### Superseded: the engine-side theories (all wrong)

Kept only so the reasoning is not repeated. The `handleCletEvent` chain that was
being blamed is real — event 0x18 passes through `*globalA == 0`, `IsIfreeEvent()`
(measured 1) and the `*globalB == 5` counter branch on the way to
`UICONTROLLIST_Event` — but the event reached the list the whole time. Nothing
swallowed it; the y coordinate it arrived with was 0 by the time the `__muldf3`
inside `EventHandler_Wrapper` was done. Reading `*globalB` and the counter would
have explained nothing, and the "two implementation options" below were both
unnecessary.

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

Also note: `build.sh wasm` used to write `build/spike.wasm`, the same name the
browser build gives its emscripten module, so the two silently clobbered each
other -- running the throughput build after the browser build left the page
dying at instantiation with `Import #2 "host": module is not an object or
function`. The freestanding target now writes `build/spike_freestanding.wasm`;
the browser triple keeps the `spike.*` names the page URL depends on.

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
11. **ARM multiply: two independent field mistakes, both silent.**
    `MUL`/`MLA`'s destination is bits[19:16] (the data-processing `Rn` field),
    not `Rd` (bits[15:12]) — bits[15:12] is the MLA *accumulator* and zero for
    `MUL`. Because that field is almost always zero in compiler output, the old
    code wrote every product into `r0` and left the real destination stale; it
    went unseen until `__aeabi_uidivmod` (`mul r3,r2,r0`) computed the remainder,
    where `r0` was clobbered with the dividend, so `0x78da % 31` returned `0x78d6`
    instead of `0` and zlib rejected the valid PNG header.
    The 64-bit forms were **not** in fact fine. The block split `MUL`/`MLA` from
    `UMULL`/`UMLAL`/`SMULL`/`SMLAL` on **bit22**, but the ARM encoding splits
    them on **bit23**; bit22 is only the *signed* flag of the long forms, and it
    is 0 for `UMULL`/`UMLAL` exactly as for `MUL`/`MLA`. So every long multiply
    ran the 32-bit path, wrote only its low word to bits[19:16] and left the high
    word untouched, corrupting every `__muldf3`/`__fixdfsi` result whose product
    needed its high half. It was invisible until input: it made
    `EventHandler_Wrapper`'s y scale (`240 * 0.588`) come out as `2^-506`, so
    every tap's y collapsed to 0 and no control could be hit.
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
16b. **`DeleteLocalRef` deletes a *reference*, not the object.** These handles
    conflate the two: `NewGlobalRef` handed the same handle back and
    `DeleteLocalRef` set it `H_FREE` outright, so the engine's cache idiom —
    `NewIntArray(2)`, `NewGlobalRef`, `DeleteLocalRef` — killed its long-lived
    array at init. The slot was then recycled by a later `handle_new` for a
    cached *methodID*, and `SGL_GetTextSizeInRect` went on reading its two
    measurements out of that method handle: `(0,0)`. Everything downstream was
    consistent with a font that measures zero — text textures came out 1x1 and
    every label was drawn into one and thrown away. The table now carries a
    reference count, and a slot only becomes reusable when it reaches zero.
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

As of 2026-10-17 it does not even get that far: every entry reports DIVERGENCE
with the oracle running 0 instructions and stopping at the first import it cannot
reproduce (`__aeabi_atexit`). It resumes from the C side's dump point, which for
these entries is an import stub rather than the entry, so it compares nothing.
This reproduces with the multiply fix reverted (pre-existing, not the decoder),
and fixing it is the next harness task. Until then, verify a decoder change by
calling the guest function directly with known operands — which is how the
`umull`/`umlal` routing fix above was verified (`__muldf3` against six operand
pairs).

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
