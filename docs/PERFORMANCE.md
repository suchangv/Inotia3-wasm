# Performance: what was actually slow, and where it stands

The target is the engine's own rate. Inotia 3 paces itself: `MainProcess`, the
callback `g_MainTimer` dispatches, is one whole frame — update, draw, re-arm —
and it asks to be called back 66 ms later, so the game is a 15 fps game. A port
that runs it slower is broken and a port that runs it faster has accelerated the
game.

Both of those were true of this port, in the browser, at the same time. What it
is *not* is interpreter-bound: a gameplay frame is ~2.15 M guest instructions,
12.8 ms of browser time, and the engine only wants one every 66.8 ms.

Everything here is measured, with `tools/run_browser.sh` (headless Chrome,
swiftshader) or the native build, which stays on a deterministic 16 ms step so
its numbers are reproducible.

## 1. The host log sink (fixed: 47.6 ms/frame -> 3.4 ms)

Emscripten's generated page defines `Module.print` as "console.log the line,
then append it to `<textarea id="output">`, then set
`scrollTop = scrollHeight`". The engine logs one line per frame, so that is a
forced DOM reflow *per frame* on a string that grows for the whole run -- the
cost per line grows with the run. Two frames into gameplay the textarea held
237,364 characters.

Measured on the idle main menu, where the engine itself does 0.26 ms of work per
frame:

| | requestAnimationFrame callback | page |
|---|---|---|
| as shipped | 47.6 ms | 17 fps |
| `#output` detached mid-run | 3.4 ms | 59.7 fps |

Boot to the menu went from 89 s to 23 s with the textarea detached. Silencing
`console.log` *alone* changed nothing (0.081 ms per call), which is what proves
the cost was the textarea write and not the logging.

The fix is in `tools/page-options.js`: emscripten binds its stdout sink exactly
once, at module init (`if(Module["print"])out=Module["print"]`), and `--pre-js`
runs before that, so overriding `Module.print` there routes stdout to the
console and nowhere else.

## 2. The port's own clock ran the game 20% slow (fixed: 12.0 -> 15.0 fps)

With the log sink fixed and the engine's frame period untouched, gameplay in the
browser ran **12.0 engine frames per second** where the engine had asked for
66 ms, i.e. 15.15 fps. Two port bugs, compounding:

* `g_use_fixed_step = !g_browser_fixed` **inverted the `?fixed=1` flag**. The
  comment above it says the browser runs on real elapsed time and `?fixed=1`
  restores the deterministic 16 ms step, but the code did the opposite: with no
  query string the browser was pinned to a 16 ms tick.
* `g_clock += (int64_t)dt` **truncated** each tick to whole milliseconds. A real
  tick in the browser is ~16.7 ms, so the guest clock ran 4% slow on top.

The frame rate is sensitive to exactly that, because the engine re-arms itself
at 66 ms and the timer fires on the first tick whose clock has passed that:

| tick step | 66 ms of game clock lands on | engine fps |
|---|---|---|
| 16 ms (fixed, mistakenly the default) | 5th tick = 80 ms | 12.5 |
| ~16.7 ms (real elapsed, with the fraction carried) | 4th tick = 66.8 ms | 15.0 |

Measured after the fix, in gameplay: **15.0 engine fps** (300 engine frames per
1200 ticks, i.e. one every 4 ticks), the page still at **60.0 ticks/s**, the
menu up in ~17 s.

The native CLI keeps the fixed step unconditionally -- the differential oracle
and every recorded run depend on it -- so this changes nothing about the numbers
in this file. The check is that `--frames=6500` still reports
`average over 1271 frame(s): 390385 instructions`.

## 3. The engine's frame period: deliberately not touched

`APPINFO_nTimer` (guest `0xd5bfe8`) is the engine's frame period. It is written
in exactly one place, `APPINFO_SetTimer`, and `APPINFO_Initialize` sets it at
`0x7256c` on every path through that function:

    7256c: 2042   movs r0, #0x42        ; 66 ms
    7256e: f7ff.. bl   APPINFO_SetTimer

Overriding that value at runtime shows the whole frame rate is this one
constant -- 1271 engine frames over a 6500-tick (104 s) run at 66 ms, 2087 at
33 ms, 6165 at 16 ms, for ~16% more total instructions. `patches.c` can rewrite
that immediate, and `--frame-ms=N` / `?framems=N` sets it.

**It is off by default, and it should stay off.** The world advances once per
`MainProcess` frame and that same frame is what draws, so there is no way to
redraw more often without the game running faster: 16 ms frames are a 4.8x
speed-up of the game, not a smoother version of it. The knob exists to measure
that, which is what the numbers above are for.

Whether the engine's world is *inherently* clock-driven (so that a faster frame
period would only redraw more often) is not settled. For it: `MainProcess`
stamps `CHECKTIME_nCurrentTime` every frame for the rest of the engine to read,
the item/buff deadlines are `now + 15 ms` / `now + 30 ms`, and the loader's
stages advance on the game clock rather than on a frame count (8 ticks, 128 ms,
per stage at both 16 ms and 66 ms frames). Against it: a gameplay frame costs
the same 2.1 M instructions at 66 ms as at 16 ms, which is what per-frame logic
looks like. Either way the default is the engine's own pacing.

Also worth correcting, since it misled an earlier session: the 120 ms branch in
`MainProcess` is **not** an empty input queue. It is `STATE_nState == 0` -- no
state set yet. During gameplay (`STATE_nState != 0`) the interval is the
computed `66 - work`, floored at 10 ms.

## 4. Where the interpreter stands

At the engine's own pacing, in the browser, in gameplay:

| | |
|---|---|
| tick rate | 60.0/s (input is polled every tick) |
| engine frames | 15.0 fps -- the engine's own 66 ms |
| per engine frame | 2,150,383 instructions, 12.77 ms interpreter, 34 draw calls |
| interpreter | 168 MIPS |
| share of wall clock | 19% |

So the game's own rate is met with about **5x headroom**, and the worst observed
gameplay frame does not threaten it either: 4.4 M instructions is ~26 ms, inside
a 66.8 ms budget.

If the frame period were ever moved to 16 ms the budget would become 16.67 ms a
frame, and then the average is 1.31x headroom with the worst frames over budget
(4.4 M instructions = 26 ms, ~40 fps). That is the work Tier 1 below describes;
it is not needed at 15 fps.

## 5. What is left

Not performance:

* **Audio is not wired up.** `SOUNDSYSTEM_Process` is called every frame from
  `MainProcess`; there is no host audio backend behind it.
* **Text does not render** -- the opening event's dialogue box comes up empty.
  Whether that is a font-loading failure or a glyph-decode bug is not known, and
  it is the next thing to look at.

Performance, in order:

* **Boot (~17 s to the menu).** LZMA + inflate + PNG are ~37% of all
  instructions in a full run and are one-time cost. They are portable C
  libraries rather than engine logic, so the host can do the work instead of the
  interpreter: `patches_replace_with_import` already exists for this (see
  `File::GetFullPathName`). This trades a faithful-guest ideal for boot time.
* **The profiler is on by default in the browser.** `g_do_profile` defaults to 1
  and the page has no way to pass `--no-profile`, so every guest instruction also
  does profiler bookkeeping. Unmeasured, and cheap to measure.
* **Only if 60 fps is ever wanted**: `-O3 -flto` for the browser target (it is
  `-O2`), then the per-instruction host checks in `cpu.c` that can be hoisted
  into loop-local flags, then the `GRP_*` software blitter
  (`GRP_FilterAlpha` 10.6%, `GRP_FillRect` 8.7%, `GRP_GetPixelFromRGB_565` 8.7%,
  `GRP_DrawVLine` 8.2% of all instructions). Rerun `tools/difftest.sh` after any
  change to `cpu.c` -- noting that it currently fails before it compares
  anything, because the oracle side runs 0 instructions; that failure predates
  this work and is an environment problem.

## How to measure

Native, deterministic, with the profile:

```sh
./build/spike game/libgame.so --boot --frames=6500 \
    --tap=1500,467,240 --tap=2000,480,151 --tap=2400,136,459 \
    --tap=2800,480,359 --tap=5000,860,60 --tap=5600,860,60
```

Browser, bounded, console visible:

```sh
QUERY='frames=600' tools/run_browser.sh 30 /tmp/out.png
```

What to read, and what it should say at the engine's own pacing: the tick rate
(60/s), engine frames per second (**15.0** -- 12.5 means something is back on a
16 ms step), instructions and interpreter ms per engine frame (~2.15 M, ~12.8 ms
in the browser), and the interpreter's share of wall clock (~19%).

A gameplay run that reaches play, for reference: `--tap=1500,467,240`,
`--tap=2000,480,151`, `--tap=2400,136,459`, `--tap=2800,480,359`, then two
presses at device `(898,29)` to leave the opening event.
