/*
 * timers.h -- the Java-side software timer table.
 *
 * Inotia 3's game loop is timer-driven, not frame-driven. The native code
 * schedules work by asking the Java layer for a timer:
 *
 *     startClet -> CS_knlSetTimer -> TM_Set -> jSetTimer
 *         -> WrapperJinterface.SetTimer(id, firetime, interval)
 *
 * and the Java layer fires it back by calling nativeRender(id) once `firetime`
 * is reached, at which point the guest's TM_Run(id) dispatches the matching
 * callback -- MainProcess for g_MainTimer, which is what actually advances the
 * game state.
 *
 * There is no Java here, so this table stands in for it: same fields, same
 * logic. Without it the render loop can only ever call nativeRender(0), no
 * timer with a non-zero id fires, and the game stays on a black screen while
 * booting perfectly -- which is exactly what this port was doing.
 *
 * The clock is the `sysTime[]` array handed to nativeInit(): the guest reads it
 * through CS_knlCurrentTime(), so the absolute `firetime` values arriving from
 * SetTimer are directly comparable to whatever is written there. Both sides
 * must use that array and not their own notion of "now", or the timers fire at
 * the wrong rate.
 */
#ifndef TIMERS_H
#define TIMERS_H

#include <stdint.h>

/* How many timers can be registered at once. WrapperRenderer has a 32-slot
 * table, but its slots are indexed by id while ours are a list keyed by an
 * opaque handle -- see below -- so this is a capacity, not an index bound. */
#define TIMER_MAX 64

/* WrapperRenderer.SetTimer(int timerID, long firetime, long intervaltime).
 *
 * `timer_id` is opaque and must be handed back to the guest unchanged. It is
 * NOT an index: in this build the guest passes the address of its own timer
 * object (the first one registered is &g_MainTimer), and the guest's TM_Run(id)
 * uses that address to find the timer. Treating it as an index into a 32-slot
 * table discards every registration, which is exactly what happened first. */
void timers_set(uint32_t timer_id, int64_t firetime, int64_t intervaltime);

/* WrapperRenderer.UnsetTimer(int timerID). */
void timers_unset(uint32_t timer_id);

/* Reset to the constructed (all-empty) state. */
void timers_reset(void);

/* Ids whose firetime has arrived at `now`, in ascending id order. Returns how
 * many were written to `out`; each is advanced by its interval so the next
 * call does not return it again. */
int timers_due(int64_t now, uint32_t *out, int max);

/* Report the table. */
void timers_report(void);

/* How many timers are currently registered. */
int timers_active(void);

#endif /* TIMERS_H */
