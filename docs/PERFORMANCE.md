# Performance: what was actually slow, and what is left

Everything here is measured, either with the native build (which is the baseline
the numbers are quoted against) or with headless Chrome
(`tools/run_browser.sh`, swiftshader, `--disable-gpu`). Reproduce any line with
the command given next to it.

Two things made the browser port unplayable, and they were not the same thing.
The first was the port's own fault and cost more than everything else combined.
The second is the shipped engine's design, and moving it is a decision, not a
fix.

## 1. The host log sink (fixed: ~3.5x on the tick loop, 7x on boot)

Emscripten's generated page defines `Module.print` as "console.log the line,
then append it to `<textarea id="output">`, then set
`scrollTop = scrollHeight`". The engine logs one line per frame, so that is a
forced DOM reflow *per frame*, on a string that grows for the whole run -- the
cost per line grows with the run. Two frames into gameplay the textarea held
237,364 characters.

Measured on the idle main menu, where the engine itself does 0.26 ms of work per
frame:

| | requestAnimationFrame callback | page |
|---|---|---|
| as shipped | 47.6 ms | 17 fps |
| `#output` detached mid-run | 3.4 ms | 59.7 fps |

Boot to the menu went from 89 s to 23 s with the textarea detached. Silencing
`console.log` *alone* changed nothing (0.081 ms per call), which is what proved
the cost was the textarea write and not the logging.

The fix is in `tools/page-options.js`: emscripten binds its stdout sink exactly
once, at module init (`if(Module["print"])out=Module["print"]`), and `--pre-js`
runs before that, so overriding `Module.print` there routes stdout to the
console and nowhere else.

## 2. The engine's frame period (fixed, but this one is a judgement call)

`MainProcess` (guest `0x6de34`) is the whole engine frame: it is the callback
`g_MainTimer` dispatches. It stamps `CHECKTIME_nCurrentTime`, does the scene's
update and draw, and re-arms itself with

    interval = APPINFO_nTimer - (time this frame took)      floor 10 ms

(120 ms when no input is queued; 100 ms while `gAppSleepInfo` is set, which is
the backgrounded case). `APPINFO_nTimer` (guest `0xd5bfe8`) is set once, by
`APPINFO_Initialize` at `0x7256c`:

    7256c: 2042   movs r0, #0x42        ; 66 ms
    7256e: f7ff.. bl   APPINFO_SetTimer

So the engine asks for a 66 ms frame, and gets one. Nothing in the port needed
that: a gameplay frame is ~2.1 M guest instructions, which the interpreter
retires in ~7.6 ms natively and ~12.2 ms in the browser.

Overriding the value at runtime (temporarily, via a debug poke) shows the whole
frame rate is this one constant:

| `APPINFO_nTimer` | engine frames over a 6500-tick (104 s) run | ≈ fps |
|---|---|---|
| 66 ms (as shipped) | 1271 | 12.2 |
| 33 ms | 2087 | 20.1 |
| 16 ms | 6165 | 59.3 |

Total instructions grew only ~16% for 4.8x the frames: the engine is doing the
same work, just at the rate a 60 Hz display expects.

`patches.c` now scans for that `movs r0, #0x42` + `bl APPINFO_SetTimer` pair and
rewrites the immediate. It scans rather than hardcodes an address so another APK
build reports itself instead of getting the wrong instruction patched, and it
leaves the 200 ms cloud-backup throttle alone.

**This is the one change here that is a behaviour change rather than a repair**,
so it is a knob:

```sh
./build/spike game/libgame.so --boot --frame-ms=66          # the engine's own 15 fps
# http://localhost:8080/spike.html?framems=66               # same, in the browser
```

### What to check after moving it

The patch is correct only while the game's world advances with the clock rather
than once per frame. If it advances per frame, 16 ms frames run the game at
roughly 4x speed and the knob has to go back to 66.

The evidence points the other way:

* `MainProcess` stamps `CHECKTIME_nCurrentTime` every frame *for the rest of the
  engine to read*, and that is the only place it is written.
* The engine's timed systems are deadlines in game-clock ms --
  `CHECKTIME_Initialize` seeds `nFreeWillTick`/`nPotionTick`/`nBuffItemTick`/
  `nLifeTick`/`nSiegeItemTick` from `now + 15` and `now + 30`.
* The loader advances its stages on the clock, not on a frame count: in a
  fixed-step run every stage takes 8 ticks (128 ms) per stage at *both* 16 ms
  and 66 ms frames. A frame-counted stage would have taken 5 ticks at 16 ms and
  40 at 66 ms.
* A gameplay engine frame costs 2.17 M instructions at 66 ms and 2.02 M at
  16 ms -- flat per frame. That is what a fixed-cost render plus a small
  clock-driven simulation looks like; it is also what per-frame simulation looks
  like, so it does not decide the question either way, but it does say the
  simulation is only ~2% of the frame.

That is suggestive rather than proof, so: play it. Anomalies would be characters
and NPCs sliding noticeably faster than on a phone, or the opening event running
4x short, and the fix is `--frame-ms=66` / `?framems=66`.

## 3. Where the interpreter stands at 60 fps

Measured in the browser, in gameplay, with the log sink fixed and the frame
period at 16 ms:

| | |
|---|---|
| tick rate | 58.5/s, and **every tick runs the engine** (100%) |
| engine frames | 58.5 fps |
| per frame | 2,143,247 instructions, 12.21 ms interpreter, 30 draw calls |
| interpreter | 176 MIPS |
| share of wall clock | 71% |
| headroom in a 16.67 ms budget | 1.37x |

The same frames natively cost 7.55-7.75 ms (~265 MIPS), so the desktop build has
~2.2x headroom and the browser is the tight one.

The average is not the number that decides whether 60 fps holds, though. Over the
same gameplay the worst frame was 4.4 M instructions (5.09 M natively): at 176
MIPS that is 25 ms, so those frames come in at ~40 fps. **That, not the average,
is the remaining work.**

## The plan, in tiers

**Tier 0 -- done, verified.**

1. Stop stdout going into the shell's textarea (`tools/page-options.js`). This
   was the largest single win by a wide margin.
2. Raise the engine's frame period to 16 ms (`patch_frame_period` in
   `patches.c`), with `--frame-ms=` / `?framems=` to put it back.
3. Turn the profiler off in the browser. `g_do_profile` defaults to 1, and the
   page has no way to pass `--no-profile`; every guest instruction currently
   also does profiler bookkeeping. Unmeasured, and cheap to measure.

**Tier 1 -- the worst frames, if 60 fps has to hold through combat.**

The hot list (native `--frames=6500`, whole run, % of all instructions):
`LzmaDecode` 22.5, `GRP_FilterAlpha` 10.6, `GRP_FillRect` 8.7,
`GRP_GetPixelFromRGB_565` 8.7, `inflate_fast` 8.2, `GRP_DrawVLine` 8.2. The
loader/decompression entries are one-time; the `GRP_*` software blitter is the
per-frame cost and is where a 4.4 M-instruction frame goes.

- Build `-O3 -flto` for the browser target (currently `-O2`). Free, and the
  interpreter is a big switch, which is exactly what inlining helps.
- Profiler off (Tier 0.3) if it shows up.
- Then interpreter micro-work on the block dispatcher in `cpu.c`, in the order
  the profile says: the per-instruction host checks (budget/trace/stop-at) that
  can be hoisted into loop-local flags, and the flag shuffles in `GRP_*`.
- Regenerate the profile after each step; do not touch `cpu.c` before the
  measurement says to, and rerun `tools/difftest.sh` after any change to it.

**Tier 2 -- boot, if 12 s is still too slow.**

LZMA + inflate + PNG are ~37% of all instructions in a full run and are one-time
boot cost. They are libraries with portable C sources, not engine logic, so the
host can do the work instead of the interpreter: implement `LzmaDecode`,
`inflate*` and the `png_*` entry points as host imports (the same
`patches_replace_with_import` mechanism already used for
`File::GetFullPathName`). This trades a faithful-guest ideal for boot time; the
39 s the textarea fix already recovered is the cheaper win, which is why this is
Tier 2.

**Tier 3 -- AOT, only if the interpreter cannot get there.**

130 MIPS average and 305 MIPS peak are what 60 fps of gameplay costs (native
measurement, worst frame included). The interpreter does 176 MIPS in the browser
and 265 natively, so it is close, and it is the browser that would need the AOT.
That is a large piece of work; the tiers above should be spent first.

## How to measure

Native, the whole run -- engine frames, instructions/frame, and the profile:

```sh
./build/spike game/libgame.so --boot --frames=6500 \
    --tap=1500,467,240 --tap=2000,480,151 --tap=2400,136,459 \
    --tap=2800,480,359 --tap=5000,860,60 --tap=5600,860,60
```

Browser, bounded, with the console visible:

```sh
QUERY='frames=600' tools/run_browser.sh 30 /tmp/out.png
```

What to read: the tick rate, what fraction of ticks run the engine, average
instructions and interpreter ms per engine frame, and the *max* frame. A 16.67 ms
budget means ~2.9 M instructions per frame at 176 MIPS; anything above that is a
dropped frame.
