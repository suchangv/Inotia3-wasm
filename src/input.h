/*
 * input.h -- pointer input, from DOM events to the engine's event protocol.
 *
 * Inotia 3's Java layer (WrapperEventHandler's MultiTouchListener, since
 * WrapperActivity.IsMultiTouch defaults to true and MainActivity never
 * overrides it) forwards every touch as
 *
 *     nativeEvent(type, PointID + 1, MAKEWORD(x, y), touchCount)
 *
 * and the PS Vita port's wr_touch.c already worked out what each field has to
 * be. This module is that state machine with the device layer removed: the
 * platform pushes pointer events in, the guest-facing sink is called back out,
 * and nothing here knows what a mouse or a SceTouchReport is.
 *
 * The one place the Vita model does not transfer is *how the input arrives*.
 * wr_touch.c polls: each frame it peeks the hardware and diffs the result
 * against its slots, and a pointer that stopped appearing is released. That
 * works because the poll and the panel both run at ~60 Hz, so a finger that
 * starts and ends between two polls is a ~16 ms window -- impossible.
 *
 * Browser events are edges, and a tick here runs for 200-300 ms because the
 * interpreter is slow, while a real tap is 50-100 ms. A tap that begins and
 * ends inside one tick would be added to and removed from a level snapshot
 * before the tick ever read it, and the engine would see nothing -- not a race,
 * the normal case. So events go into a FIFO and are replayed at the frame
 * boundary, which is what makes a fast tap produce a DOWN/UP pair.
 *
 * The engine's own protocol is what fixes the rest of the design:
 *
 *   - `PointID` must be a *stable* slot 0..4, never a device id. The engine
 *     indexes fixed-size per-touch arrays by it, so a monotonic id corrupts
 *     them. Hence slots here, and hence ids >= 1000 for anything synthetic.
 *   - `touchCount` is counted after inserting on DOWN and before clearing on
 *     UP, matching the Java original.
 *   - A move is only forwarded after 40 ms *and* more than 2 px, the same gate
 *     WrapperEventHandler applies. Without it the engine is flooded with
 *     sub-pixel jitter.
 */
#ifndef INPUT_H
#define INPUT_H

#include <stdint.h>

/* MultiTouchListener's CS_APP_EVENT_*_EX constants (0x80000004/5/6), as the
 * signed ints the guest receives. */
#define INPUT_DOWN (-2147483644)
#define INPUT_DRAG (-2147483643)
#define INPUT_UP   (-2147483642)

/* WrapperEventHandler.MAX_TOUCH_POINT. */
#define INPUT_MAX_POINTERS 5

/* The engine side. `type` is one of the three above, `p1` is slot + 1, `p2` is
 * (x << 16) | y, and `touch_count` is the number of occupied slots. */
typedef void (*input_sink_fn)(int type, int p1, int p2, int touch_count);

/* Hand over the sink. Must be called before the first input_poll(). */
void input_init(input_sink_fn sink);

/* ---- platform -> input --------------------------------------------------
 *
 * Called from DOM event handlers. These only append to a queue: they must not
 * call the guest, because the guest is entered at frame boundaries only (see
 * main.c) and a DOM handler runs at an arbitrary point in the page's task
 * queue.
 *
 * `id` is whatever the platform uses to tell pointers apart -- a pointerId, a
 * touch identifier. It is never passed to the engine; it only maps a stream of
 * events onto one of the five slots. `t_ms` is when the event happened, in the
 * same units the platform passes to input_poll().
 */
void input_push_down(int id, int x, int y, double t_ms);
void input_push_move(int id, int x, int y, double t_ms);
void input_push_up(int id, int x, int y, double t_ms);

/* Release every held pointer at its last known position.
 *
 * This is the one case the FIFO cannot cover: a pointer that stops existing
 * rather than lifting -- the tab losing focus, the window being hidden. The
 * platform calls it from `blur` / `visibilitychange`. Without it the engine
 * would keep a finger held down forever. */
void input_release_all(void);

/* ---- input -> guest -----------------------------------------------------
 *
 * Replay everything queued, in order, through the sink. Called once per tick
 * from the guest's frame boundary, before the timers fire, so the engine sees
 * this frame's input during this frame's render.
 */
void input_poll(void);

#endif /* INPUT_H */
