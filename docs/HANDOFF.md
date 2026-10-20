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
loose ends (audio, the save round-trip, and the budgeted coverage of code that
only runs with real arguments).

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
- **The opening event's two SKIP presses are the game's own logic.** The class
  confirm drops into `GAMESTATE_nState` 13 (a black screen with a **SKIP**
  control at the top right, UI centre `(449,17)`), one press is not enough, and
  a second press reaches the play state `GAMESTATE_nState` 2. Confirmed against
  a device by the user: not a port artefact, and not something to chase.
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

*(Superseded the next day: the bitmap font described above is what the native
runner still draws with. The browser renders real fonts as of 2026-10-19.)*

---

### Progress since that (2026-10-18, the sound)

**The game has audio.** The menu's looping music and the UI effects play in the
browser, from the game's own Ogg Vorbis files, with the engine's own loop and
volume arguments.

The path is the same shape as the text one, and it is *not* the one the .so
looks like it has. libgame.so carries a native C2S media layer
(`CS_mdaClipCreate` and friends, reached from `SOUNDPLAYER_Play`), but in this
build every `CS_mda*` entry point is a two-instruction stub returning 0, so
that path dies at the first call. What the engine actually uses is the Java
surface: `SOUNDSYSTEM_Create` -> `CS_mdaSoundCreate` -> `jSoundCreate` ->
`WrapperJinterface.SoundCreate`, and eight more methods for
prepare/play/pause/resume/stop/volume/destroy -- which on Android is
MediaPlayer and SoundPool. `src/sound.c` is that player, and
`tools/sound.js` (linked with `--js-library`) is its backend in the browser:
WebAudio, with the browser's own Vorbis decoder.

Nothing is decoded in the port. The engine preloads **160** sounds at boot
(~10 MB of Vorbis, one handle per asset, kept for the life of the process), so
the bytes stay compressed and are decoded only when played -- at
`SoundPrepare` for the small effects, at `SoundPlay` for the background music
-- with a play that arrives mid-decode remembered and started when the decode
lands. That single deferral is the one behavioural difference from Android,
where SoundPool hands out an already-decoded sample. The native runner gets the
same handle bookkeeping with a null backend, so the engine behaves identically
in both hosts; only the audio hardware differs.

Two details worth knowing:

* **The assets resolve through the engine's own logical names.** `SoundCreate`
  is called with `"BGM1.ogg"`, not a path, and the file is stored as
  `assets/common/game_res/BGM1.ogg.jpg` -- the packer renames
  already-compressed files so aapt stores them instead of deflating them
  again. `asset_lookup()` (host_libc.c) already knew that convention for
  images and now resolves sounds through it too.
* **The volume is 0..100, not a fraction** -- confirmed from the guest:
  `SoundSetVolume(160 "Title.ogg", 100)` on the first music play.

---

### Progress since that (2026-10-19, the fonts)

**The text is a real font now, in every language the game ships.** The first
text pass drew with the engine's own bitmap font (`FONT_pFontData`, the 5x10
glyph records `GRP_PutAsciiChar` blits) and that was the wrong font twice over:
at the sizes the game asks for it reads as a dot matrix once the engine scales
it onto a 960x544 screen, and it has no glyphs above U+007F at all, so every
localized string came out as correctly-spaced blanks.

The bytes handed over are UTF-8, not a legacy codepage. Confirmed by asking for
another language (SPIKE_LANG=ko) and reading the draw: the class description
starts `ea b1 b0 ec b9 a0 ea b3 a0` = "거칠고" ("rough"). So there was nothing to
transcode -- the renderer just had to be a real one:

* **Browser**: `tools/text.js`, Canvas2D with `serif` -- the family
  `WrapperRenderer.java` asks Android for (`Typeface.SERIF`, still a string in
  the APK's classes.dex) -- and the browser's own per-script fallback behind it.
  That is Android's arrangement exactly, and it is why Chinese, Japanese and
  Korean come out right without the port shipping or downloading a font: the
  platform had the fonts all along, on both sides.
* **Native**: still the engine's bitmap font, ASCII only. It is the debugging
  host; what a player sees is the browser.

Layout (wrapping, alignment, line height) stayed in `text.c`, which asks the
host for two primitives only -- the width of a run of text and the painting of
one line -- so the size the engine is told and the pixels it reads back cannot
come from different metrics. The wrap is one greedy pass for every script:
prefer the last whitespace on the line, and when there is none break at the
character that overflowed, which is how CJK wraps anyway.

**The language is the host's locale.** `host_language()` (platform_*.c)
answers with `navigator.language` in the browser -- Android's
`Locale.getDefault()`, which is what the Java side answered -- so a Chinese or
Korean browser gets the game in its own language with no setting at all.
`?lang=zh-CN` (browser) and `SPIKE_LANG=zh-CN` (native) force one for testing;
`ko-KR`, `zh-CN`, `zh-TW`, `ja-JP` and `fr-FR` all select their file.

For the record, the retired bitmap path is what the parent Vita port does too
(`source/java.c` sums the engine's own per-byte metric), so the two ports now
differ here -- deliberately: the browser has the fonts, and the Vita does not.

---

### Progress since that (2026-10-19, the freeze)

**The freeze was the CPU decoder missing Thumb-2's `movw`/`movt`.** The
symptom: walking around, the moment the character picked an item up, the world
stopped for good -- the engine kept ticking frames (33 instructions each,
`draw=0`, `host_calls=0`) but nothing advanced, so the page was alive and the
game was dead.

The trail, once the port could see it:

* Every occurrence left the guest at the *same* address: `pc=016d11ec
  lr=0108c095`, and `016d11ec` is `MEM_pCurrentPool+0x176804` -- inside the
  engine's own memory pool. The backtrace named the path:
  `CHAR_PickItemAll -> INVEN_SaveItem -> INVEN_GetMoney`.
* The instruction ring (`?trace=1`, below) showed the last instruction as
  `0108c090 f245`. That is not a branch: it is the first half of
  `movw r0, #0x52ad; movt r0, #0x0181; bx lr` -- i.e. "return 0x018152ad, the
  address of the money field". The decoder's `11110` space handled only BL/BLX,
  so it computed a branch offset out of the `movw` bits and jumped into the
  pool.

This is bigger than one function: **the .so is Thumb-2 code.** The APK files
it under `lib/armeabi`, but it is built for ARMv7, while the interpreter is
ARMv5TE/Thumb-1. Any ARMv7-only instruction on a path a player can reach was
being mis-executed; this was the first one that mattered. `movw`/`movt` are
implemented now, and the BL/BLX path *validates* its second halfword and traps
with the full encoding instead of inventing a target out of whatever the bits
happen to say.

The unit test is a single call, because `INVEN_GetMoney` *is* that sequence:

```
./build/spike game/libgame.so --entry=108c091
# before: 2 instrs -> pc 016d11ec (MEM_pCurrentPool+0x176804)
# after:  5 instrs -> returned to caller, r0 = 018152ad
```

Getting there needed a diagnostic worth keeping: **when the guest PC leaves the
image and is not the return sentinel, the run stops and prints a backtrace, the
register file, and -- with `?trace=1` (browser) or `--trace` (native) -- the
last 96 instructions.** That ring is what named the instruction; without it the
freeze looked like a mysterious jump into the engine's heap, and the run kept
limping, which is why it presented as a hang.

Two unrelated bugs were found while chasing it, both worth having:

* **The GL bridge's texture table could be written past its end.** `g_tex[64]`
  records id -> size/format for the draw trace. A new id with the table full
  fell through to `g_tex[i]` with `i == 64` -- a write past the array, into
  whatever static the linker put after it. Every map brings its own atlases, so
  the length of the session decided when. It holds 256 entries now and recycles
  them (a one-line log says when).
* **Decoded audio was never released.** `decodeAudioData` returns PCM at roughly
  20x the compressed size, the engine plays a different track per map, and every
  handle lives for the whole process. It is a 96 MB LRU now. Two supporting
  fixes came out of testing it: `decodeAudioData` *detaches* the buffer it is
  given, so it now gets a copy (otherwise an evicted sound could never be
  decoded twice), and a looping sound that is already playing is no longer
  restarted (the engine re-issues BGM plays as its state machine re-evaluates).

A freeze now also leaves evidence: `tools/page-options.js` reports uncaught
exceptions, a lost WebGL context, and main-thread stalls (the heartbeat prints
how long the thread was blocked once it comes back).

---

### Progress since that (2026-10-19, the rest of Thumb-2, the save, the portrait)

**The decoder now covers everything the binary actually uses.** Disassembling
every byte covered by a FUNC symbol in .text, minus the encodings already
handled, left four groups, and they are implemented: the `.w` data-processing
forms (`and/orr/eor/bic/orn/add/sub/rsb/adc/sbc/cmp/tst/teq`, shifted operands
including RRX, and the S-flag rules the ARM path already established); wide
`ldm/stm/ldmdb/stmdb`; `push.w`/`pop.w`; and `ldrd`/`strd`. Everything else in
that space still traps, and a trap now prints the encoding and the last 96
instructions.

**Save works.** The engine composes its save path as
`<MD5(HubSave key)>/saveN.dat` -- a subdirectory -- and the only code that
would create that subdirectory runs after the Hub/social login, which an
offline port does not have. The native run shows it plainly:

```
[io] open ./data/com.com2us.../e133100100b953020500100100000000/save0.dat FAILED
```

so the write failed with ENOENT and the game reported "failed". The host
`open()` shim now creates the parent directories for a creating open (the same
fix the parent port made in its `CS_fsOpen` hook, applied one layer down where
every write already goes through).

**The portrait is fixed.** Its draw and its texture were never the problem: the
texture reaches GL as **64** and its pixels are exactly `portrait4.png`
(checksummed every 512x512 upload in a run against the same checksum over each
portrait PNG in the archive, and only one matches). The problem was the
**multi-textured draw** it goes through, which the OpenGL ES 1.1 client-array
conversion in `src/gl4es_bridge.c` did not model.

The chain is `UI_CharacterState_Draw -> UI_CharacterDraw_Icon ->
GRPX_DrawPartExWithMask -> SGL_DrawTexturePartWithMask`, and it samples two
texture units: unit 0 the portrait atlas, unit 1 a circular alpha mask that was
generated once at start-up. `SGL_InitState` sets the two client arrays with
`glClientActiveTexture`:

```
glClientActiveTexture(GL_TEXTURE0); glTexCoordPointer(2, GL_FIXED, 0, portraitUVs);
...
glClientActiveTexture(GL_TEXTURE1); glEnable(GL_TEXTURE_2D);
glTexCoordPointer(2, GL_FIXED, 0, maskUVs);
```

The port had **one** shared `fixed_array` for texture coordinates, so the second
call overwrote the first, and at draw time it converted only that one and
installed it on whichever unit happened to be client-active. Unit 1 kept its raw
`GL_FIXED` words -- WebGL reads those as garbage -- so the mask evaluated to
alpha 0 across the quad and the portrait was masked out completely: an empty
ring, with the frame drawn around it by a separate, unaffected sprite. That is
exactly the reported symptom.

The texture-coordinate array is the one client array that is per *unit*, and
gl4es already stores it that way (`ATT_MULTITEXCOORD0 + glstate->texture.client`),
so the port now records one `fixed_array` per unit, notes which unit
`glClientActiveTexture` selected, and reinstalls each array on its own unit
before the draw (restoring the engine's client-active unit afterwards). Vertex
and colour arrays are not per unit and are unchanged.

Two things worth keeping from the search: the earlier "texture 54" reading was a
misreading (54 is a different 512x512 atlas drawn at the same corner with a
different rect), and `PortraitInfo_Process` draws a 212x281 portrait overlay
every frame at (0,38) that the map covers -- it is a "new portrait" popup, not
the HUD icon, and chasing it wasted a lot of time.

The regression run is unchanged at `average over 1271 frame(s): 390385
instructions`, and the face now renders in the oval in the browser.

**Linux / Android.** The browser is the host with the full stack and the native
build is the debugging/measurement host, deliberately windowless.
`tools/play-linux.sh` is the Linux answer: it builds the browser target if
needed, serves it on localhost and opens it. Android is a real app now -- see
below.

### Progress since that (2026-10-20, the differential covers the whole image)

**The oracle now runs every function in the image, not four constructors.**
`tools/difftest.py` starts both sides from one pre-run dump of the loader's own
image and compares one line per retired instruction. `--all` derives an entry
address from every FUNC symbol in the `.so` -- 5904 of them -- and the result is
**all entries match**:

```
5904 entries, 3951988 instructions compared
  identical  5861
  capped     25
  bad-mem    18
```

75 s wall on eight workers. `capped` is the per-entry instruction budget: the
interpreter counts a 32-bit Thumb-2 instruction as two (it counts instruction
*words*), so a capped run retires about 5% fewer instructions than the oracle's
budget and the two traces end at different lengths without anything being wrong.
`bad-mem` is the harness calling a function with zeroed arguments so it
dereferences a null pointer: both sides stop at the same instruction, and what
differs is the destination register of the aborted access. Named, counted, not
counted as a decoder mismatch.

`--log=FILE` writes one line per failure with the first divergent PC and the
registers that differ, and the summary groups failures by that signature. After
a fix the *group counts* are what say whether the sweep got better or just moved.

Five real defects came out of it, four in the CPU and one in the loader:

- **ARM `BLX <register>` was not decoded.** `blx r12` -- how the compiler calls a
  function held in a register, and how this image reaches every function pointer
  -- fell through to the data-processing decoder and ran as `teq r15, r12`: the
  call never happened, the flags were clobbered and the PC walked on. Five
  entries reported it.
- **ARM `LDRH/STRH/LDRSB/LDRSH/LDRD/STRD` were not decoded either.** The block
  claiming to handle them was gated on `bits[7:4] == 1001`, which is the
  *multiply* encoding; the halfword forms are `1011/1101/1111`, so that branch
  could never run. Every one of these instructions executed as whatever the
  data-processing decoder made of it: `strd r2, r3, [r12]` -- how
  `__gnu_ldivmod_helper` ends every 64-bit division -- became
  `bic r2, r12, r0`, a store that silently did nothing and a register that
  changed for no reason. It surfaced twice, the second time as a `memcmp`
  disagreement, because the two sides had written different bytes to the stack.
- **The ARM unconditional space (`cond == 0xF`) was skipped as if it had failed
  its condition.** `cond_true(0xF)` is false, so four `ldc2` in
  `__gnu_Unwind_Restore_WMMXC` "executed" as no-ops. A `blx` immediate there
  would have been a silently dropped call. It now decodes BLX immediate (a real
  branch), treats PLD/PLI as the hints they are, and traps on the coprocessor-2
  forms and SVC with the encoding printed.
- **`R_ARM_ABS32` against an imported symbol was applied as a base-relative
  add.** The only two in the binary are `jsonp_malloc`/`jsonp_free`'s stored
  `&malloc`/`&free` in `.data`; both were written as the image base, so `blx r3`
  jumped to `0x01000000` and executed the ELF header. Fixed to S + A, with S
  resolved through the import stubs (or the symbol's own address when it is
  defined in this image).

The fifth is one layer up, in the shims, and it is the same class of lie:
**`memcmp` was the host's, and glibc's AVX2 path returns `-1` where the byte
difference is `-137`.** The oracle compared bytes, the interpreter asked glibc,
and the two disagreed about a value no code is allowed to depend on. `strcmp`
had the same problem plus a worse one: `guest_str()` substitutes the *text*
`"(null)"` for a null pointer, so `strcmp(NULL, "PHONE...")` returned
`'(' - 'P' = -40`. Both now compare guest bytes directly, under one documented
contract (see `do_reproducible()`'s docstring and `guest_strcmp()`): unsigned
byte difference, 0 when equal, and **an argument range outside the 144 MB space
means do nothing and return 0** -- which a flat address space makes possible and
which both sides must do, or every entry that hands `strlen` a null pointer looks
like a decoder bug. `strncmp` also stopped copying both operands into an 8 KB
scratch buffer first; the engine passes lengths like `0x0a0f6a` through it, and
the loop then read past the copy.

**The harness had to become trustworthy before any of this meant anything.**
Each of these had already wasted a session:

- `--budget` did not exist. The option was `--g_budget`, and even that was
  parsed into a global that the next line overwrote with the 3-billion default,
  so *every* budget was a no-op. One entry that loops wrote a 5 GB trace and
  filled the disk -- the sweep is what found it, and unknown options are now an
  error rather than a shrug.
- The oracle needs `UC_CPU_ARM_CORTEX_A7`. Unicorn's default ARM CPU rejects
  Thumb-2, and reports "0 instructions compared" for every entry.
- Returning from an import stub switches instruction set through **`R15`, with
  bit 0**; writing the CPSR's T bit from a code hook does not stick.
- Both traces must end at the same place: the oracle has to emit the last
  instruction the interpreter flushes (`cpu_trace_final`), and must *not* flush
  again when a hook already wrote that line.
- The dump has to contain everything the guest can read: the JNI/JavaVM structs
  at `GUEST_JNI_BASE` (the engine caches `(*vm)->functions`, and a dump that
  stopped at the stub page left the oracle reading zeros -- 15 entries reported
  "r3 = 0 vs a pointer" on otherwise matching traces), the text bounds (so the
  oracle knows what "left the image" means without a constant that goes stale
  when the `.so` is rebuilt), and the null page.

`tools/build.sh check` builds the same interpreter with `GUEST_CHECK`: a sweep
calls each function with no arguments, so plenty of them dereference a null
pointer, and the bounds-checked build stops at the same instruction the oracle
does instead of taking a host SIGSEGV. `difftest.py` prefers that binary when it
exists. The release build is unchanged -- the check is a test build, about half
speed.

### Progress since that (2026-10-20, the Android build)

**There is an APK, and it plays the game.** A NativeActivity with no Java, no
`classes.dex` and no WebView: the frame loop, input, GL and audio are native,
and the guest runs under the same interpreter as every other build. Because the
guest is interpreted, the APK carries `arm64-v8a` and `x86_64` and the *same*
guest image runs on both -- the x86_64 library is what runs in an emulator, not a
second port.

```
tools/build-android.sh                 # -> build/android/inotia3.apk
tools/build-android.sh --install       # ... adb install -r
```

No Gradle: the script compiles with the NDK's clang, packages with `aapt2` +
`jar` (from the JDK, so `zip` is not needed), aligns with `zipalign` and signs
with a generated debug keystore.

The four pieces that are actually new, and what each had to solve:

- **`src/platform_android.c`** -- EGL with a GLES3 context (gl4es generates GLSL
  ES 3.00), the `AInputQueue` mapped from window pixels to the engine's 960×544,
  assets unpacked out of the APK into the app's files dir with a `chdir` so every
  relative path in the engine keeps working, and a pump over `ALooper_pollOnce`
  for the lifecycle. The engine still believes the screen is 960×544: every
  `glViewport` it issues is mapped into the largest centred 960:544 rectangle,
  and input comes back through the same mapping, so a 20:9 phone letterboxes
  instead of stretching.
- **`src/text_android.c`** -- stb_truetype over the phone's own fonts. The
  engine never draws glyphs; the Java side did, with Canvas and the platform
  fonts, and that is why the game ships no font and still has CJK. This is that
  renderer in C: `/system/fonts/NotoSansCJK-Regular.ttc`, DroidSansFallback or
  Roboto, picked by whether it has a glyph for the *language's* probe character,
  with `font.ttf` in the app's files dir as an override. Measurement and paint
  share one integer advance per character so the texture the engine sizes from
  the measurement is exactly as wide as what is painted into it.
- **`src/sound_android.c`** -- stb_vorbis + OpenSL ES. Everything shipped is Ogg
  Vorbis; decoding is lazy and cached with a 48 MB LRU, because the engine
  creates 160 handles at boot and decoding them all up front is tens of megabytes
  for sounds the player may never hear. Looping is a re-enqueue from the
  buffer-queue callback (OpenSL has no loop flag).
- **`android_main()` in `src/main.c`** -- pump, tick, swap; the engine's own
  66 ms period decides the speed, as in the browser.

Two traps cost most of the bring-up, and both were silent:

1. **gl4es makes `initialize_gl4es()` a `constructor(101)`.** On Android that
   runs at `dlopen` time, before the app has set the backend lookup, so
   `GetHardwareExtensions()` calls a null `gles_glGetString` and the process dies
   in the loader with `SIGSEGV at 0x0` and a stack naming nothing. Fixed by
   building gl4es with `NO_INIT_CONSTRUCTOR` and doing the initialisation in
   `gl4es_bridge_init()`, after `set_getprocaddress()`/`set_getmainfbsize()`.
2. **`STATICLIB` gl4es also exports the unprefixed GL names**, and a static
   archive wins at link time: the bridge's `glShaderSource` call resolved back
   into gl4es' own, dereferencing a `glstate` that does not exist yet. The bridge
   now resolves the *driver's* entry point through `eglGetProcAddress`/`dlsym`.

A third was self-inflicted: the port's `src/jni.h` (its own guest-JNI surface)
shadowed the NDK's `<jni.h>` for `android/native_activity.h`, which then could
not declare `JavaVM`. It is `src/falsojni.h` now.

**Verified** on an Android 13 x86_64 emulator: install, first-run asset unpack
(23 MB), title screen, character creation (the description and stat text are the
new TTF backend), the opening dialogue, the first map, letterboxing and touch
mapping, and an OpenSL player starting. **Not verified**: audio output (the
emulator ran `-no-audio`), a real GPU, background/rotation, and the save
round-trip. docs/ANDROID.md has the setup, the layer map and that list.

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
  text.c           the Java-side text rasteriser (the engine draws no glyphs)
  sound.c          the Java-side sound player (the .so's own one is stubbed)
  input.c          touch, as device coordinates
  platform_*.c     host services: native / wasm-freestanding / emscripten / android
  text_android.c   the text renderer on the phone's own fonts (stb_truetype)
  sound_android.c  the sound player on OpenSL ES (stb_vorbis)
  falsojni.h       the guest's JNI surface (renamed: it shadowed the NDK's jni.h)
  main.c           the runner
android/
  AndroidManifest.xml  NativeActivity: no Java, no dex, no WebView
tools/
  build.sh             native | check | wasm | browser (check = GUEST_CHECK)
  build-android.sh     the APK: compile, package, sign (no Gradle)
  gen_gl4es_protos.sh  regenerate gl4es_protos.h
  build_gl4es.sh       fetch and build gl4es (emscripten or NDK); see below
  sound.js             the browser's WebAudio backend for sound.c
  text.js              the browser's Canvas2D backend for text.c
  page-options.js      browser CPU/GL options (they have to run before init)
  trap.js              unpack a wasm trap into named frames
  run_browser.sh       serve build/ and screenshot with headless Chrome
  run_wasm.mjs         node driver for the freestanding build
  oracle.py            Unicorn differential oracle
  difftrace.py         first divergent instruction, with a register diff
  difftest.py          the sweep: both sides, every entry, one verdict each
  difftest.sh          the sweep over the ctors, or every FUNC symbol
  callers.py           who calls this function (finds direct BL/BLX only)
third_party/
  stb/             stb_truetype.h + stb_vorbis.c (public domain, Android text/audio)
docs/
  HANDOFF.md       this file
  ANDROID.md       the Android build: setup, layers, what is verified
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
`--entry=ADDR` (run one function and stop) · `--budget=N` · `--stop-at=ADDR` ·
`--dump-guest=FILE` · `--trace-insns=FILE` · `--watch=ADDR`

`--budget` is a runaway guard, not a target: the default is three billion
instructions, and the differential passes its own. An option this build does not
know is an error, because a silently ignored one is how a 3-billion-instruction
run fills a disk.

```sh
tools/build.sh browser            # -> build/spike.html + .js + .wasm + .data
tools/run_browser.sh 60 /tmp/x.png
```

In a browser **the page needs a WebGL2 context or it stops with a clear line**
(`gl: no WebGL2 context ...`). A Chrome with hardware acceleration off -- which
is the norm under WSL -- will not fall back to software WebGL unless it is
started with `--enable-unsafe-swiftshader`:

```sh
chrome --enable-unsafe-swiftshader http://localhost:8080/spike.html
```

Without it the context request returns 0, and before that message existed the
page died in emscripten's glue with a `TypeError` inside
`getSupportedExtensions` that named nothing at all.

```sh
tools/build-android.sh            # -> build/android/inotia3.apk (arm64 + x86_64)
tools/build-android.sh --install  # ... and install it with adb
```

The Android build is a NativeActivity: no Java, no dex, no WebView. It needs the
SDK's build-tools and platform, the NDK and a JDK, and no Gradle. See
docs/ANDROID.md for the one-time setup, for what each layer is, and for the two
gl4es traps that cost the most time (`NO_INIT_CONSTRUCTOR`, and the unprefixed
GL symbols a static gl4es also exports).

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
the map, the character, the HUD, the minimap, the text in every language it
ships and the sound. What is left is adjacent:

- **A save slot.** After entering play the log still shows `save0.dat` missing.
  That may be correct (nothing is saved until the player saves), but confirm a
  new-game save round-trips.
- **Coverage of functions that need real arguments.** The differential sweep
  starts every function with zeroed registers, so a function that immediately
  dereferences an argument stops at its first access (`bad-mem`): the prologue
  is checked, the body is not. The way to reach the body is a snapshot at the
  call site -- `--stop-at=ADDR --dump-guest=FILE` -- and `oracle.py` from that
  image, which is the same mechanism `--entry` already uses.
- **`--watch=ADDR` names host-side writes**, which is the one thing the
  instruction trace cannot see. Worth wiring into the sweep if a future
  divergence is a memory difference with matching registers.

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
| Thumb-2 decoder | everything the image contains (`movw/movt`, wide data-processing, wide `ldm/stm`, `push.w/pop.w`, `ldrd/strd`); unknown encodings trap with the word |
| ARM decoder | full coverage of the image: data processing, loads/stores incl. halfword/doubleword, block transfer, branches, multiply, BX **and BLX**, unconditional space (BLX imm, PLD/PLI, coprocessor-2 traps) |
| Oracle | `tools/difftest.sh --all`: 5904 entries, ~4M instructions, all match |
| Android | `tools/build-android.sh` -> `build/android/inotia3.apk` (NativeActivity, arm64 + x86_64); menu, character creation, intro and map confirmed on an API 33 emulator |
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

`tools/build.sh wasm` builds, and `node tools/run_wasm.mjs` now gets past
instantiation. Two symbols used to leave the link as `env.*` imports --
`host_file_get_full_path_name` and `host_parse_offset_len_path`: the dispatcher
that calls them (`host_import_call`) is compiled in both builds, but their
definitions sat inside the desktop-only block -- and with `--allow-undefined`
an undefined symbol becomes an import from `env` rather than a link error, so
the only sign was `Import #0 "env": module is not an object or function` at
instantiation, with nothing naming the symbol. They have freestanding stubs now.

It stops later instead, inside the guest heap: `memset` from
`host_import_call`'s `calloc` faults "memory access out of bounds" during boot,
which says the freestanding arena reaches past the memory the link reserves.
That is the remaining work. It only affects the throughput-measurement target;
the numbers above were measured before any of this and remain valid. Note also
that `build.sh wasm` used to write `build/spike.wasm`, the same name the browser
build gives its emscripten module, so the two silently clobbered each other --
the freestanding target now writes `build/spike_freestanding.wasm`, and the
browser triple keeps the `spike.*` names the page URL depends on.

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
   The same family, one layer up: **the texture-coordinate array is per texture
   unit** (`glClientActiveTexture`). One shared record for it means the engine's
   second `glTexCoordPointer` overwrites the first and only one array is ever
   converted, on whichever unit happened to be selected — the other unit keeps
   its raw `GL_FIXED` pointer and WebGL reads garbage from it. It hid for a long
   time because almost every draw in this engine uses a single unit, so the
   symptom was local: the top-left HUD portrait is `SGL_DrawTexturePartWithMask`,
   the only multi-textured draw, and with the mask sampling garbage it evaluated
   to alpha 0 and erased the face behind an otherwise perfect circular frame.
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

**Instruction-set semantics, second pass** (2026-10-20, found by the sweep)

16. **A decode block whose gate can never match is dead code that reads as
    coverage.** The ARM halfword/doubleword load-store branch was inside a
    `bits[7:4] == 1001` gate -- the *multiply* encoding -- while the forms it
    was written for are `1011/1101/1111`. It never ran, and the comment above it
    ("LDRH/STRH/LDRSB/LDRSH") was enough to make it look handled. The check that
    catches this is the differential, not reading the code.
17. **`BLX <register>` is not `BX <register>` with an extra bit.** Bits[7:4] are
    `0001` for BX and `0011` for BLX; matching only `0001` sent the BLX into the
    data-processing decoder, where it became `teq r15, r12` -- a call that never
    happens, plus clobbered flags. Every indirect call in the image goes through
    it.
18. **Condition field `1111` is not "always".** `cond_true(0xF)` is false by
    construction (the flag tests only define 0x0-0xE), so a decoder that tests
    the condition before testing for `0xF` skips the entire unconditional space
    as if the condition had failed: `ldc2`, `mcr2`, SVC and -- the one that
    matters -- `BLX <immediate>`. Handle `cond == 0xF` first, implement BLX
    there, treat PLD/PLI as hints and trap on the rest.
19. **The host C library is not a specification.** `memcmp` and `strcmp` may
    return *any* value with the right sign, and glibc's AVX2 paths do: the same
    four bytes came back as `-1` from the shim and `-137` from the byte loop.
    Anything whose exact result reaches a register the differential compares has
    to be implemented explicitly. The same mistake in the other direction is
    `guest_str()`'s `"(null)"` placeholder: a string used for logging must never
    be the string an import compares.

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
32. **A flag that is parsed and then overwritten is worse than a flag that is
    rejected.** `--g_budget=` wrote a global that `main()` overwrote two lines
    later, and `--budget=` was never a flag at all, so both were silently
    ignored and a 3-billion-instruction default applied. The symptom was a 5 GB
    trace file and a full disk. Unknown options are an error now.
33. **A dump that leaves out what the *host* wrote into the guest space makes
    the oracle disagree with itself.** The JNI/JavaVM structs live at
    `GUEST_JNI_BASE`, the engine caches `(*vm)->functions` from there, and the
    dump stopped at the stub page: 15 entries reported `r3 = 0` against a
    pointer on traces that matched line for line. Dump everything the guest can
    read, including the null page and the text bounds.
34. **Two traces that end at different places are a harness bug until proven
    otherwise.** The interpreter flushes the instruction that ended the run
    (`cpu_trace_final`) and the oracle has to do the same -- but only when no
    hook already wrote that line, or every "PC left the image" stop gains a
    duplicate. A sweep with a 50%-false-positive rate is a sweep nobody reads.

**Test coverage** — `tools/difftest.sh --all` runs every FUNC symbol in the
image from its own entry: 5904 entries, ~4M instructions, 75 s on eight workers.
That is the check to run before trusting a decoder change, and it is what found
the ARM `BLX`, the ARM halfword/doubleword load-stores, the unconditional-space
skip and the `R_ARM_ABS32` loader bug. The four constructors remain the fast
check (a second or two, `tools/difftest.sh` with no arguments).

What the sweep cannot see:

- Code that never runs because the function needs real arguments. A function
  that is only reached with a live object stops at its first dereference
  (`bad-mem`), so the sweep covers its prologue and not its body. Diffing a deep
  function needs a snapshot at the point the engine calls it --
  `--stop-at=ADDR --dump-guest=FILE` on the C side, then `oracle.py` from that
  image, exactly as `--entry` works.
- Memory the two sides write differently while their registers agree. The trace
  has no memory operands; the `strd` bug showed up twice for that reason, the
  second time only because a later `memcmp` disagreed about bytes.
- Anything above the per-entry budget (`capped`). Raise `--budget` for a
  specific entry; it is also the trace-file size limit.

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
- Run `tools/difftest.sh --all` before committing anything that touches
  `src/cpu.c`, `src/elf_loader.c` or the `mem*`/`str*` shims. It takes about
 75 s and it is the only check that covers the whole image; the constructor
  run (`tools/difftest.sh`, no arguments) is the quick one, and it does not
  exercise the ARM paths at all.
