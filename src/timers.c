/*
 * timers.c -- see timers.h.
 *
 * Field-for-field mirror of WrapperRenderer's timer arrays, which the parent
 * repository also reimplements host-side (source/wr_timer.c, "Faithful port of
 * WrapperRenderer's timer bookkeeping"). Two differences from that one, both
 * deliberate:
 *
 *  - it is driven by the guest's own clock (the sysTime array) rather than a
 *    host clock, because the firetime values it receives are absolute in that
 *    clock and comparing them against anything else would fire them at the
 *    wrong rate;
 *
 *  - catch-up is bounded. A timer whose interval elapsed N times while the
 *    host was busy fires once and its firetime is advanced to the next future
 *    slot, rather than being run N times to "catch up". Running the game logic
 *    N times in one frame is not what Android does either -- it drops the
 *    missed ticks -- and doing it here would make a slow host spiral.
 */

#include "timers.h"
#include "guest.h"

typedef struct {
    int      used;
    uint32_t id;          /* opaque, straight from the guest */
    int64_t  firetime;    /* absolute, in the sysTime clock */
    int64_t  interval;    /* 0 for a one-shot */
    uint64_t fires;
} wr_timer;

static wr_timer g_timers[TIMER_MAX];

void timers_reset(void)
{
    for (int i = 0; i < TIMER_MAX; i++) {
        g_timers[i].used = 0;
        g_timers[i].firetime = 0;
        g_timers[i].interval = 0;
        g_timers[i].fires = 0;
    }
}

void timers_set(uint32_t timer_id, int64_t firetime, int64_t intervaltime)
{
    wr_timer *t = NULL;
    for (int i = 0; i < TIMER_MAX; i++) {
        if (g_timers[i].used && g_timers[i].id == timer_id) { t = &g_timers[i]; break; }
    }
    if (!t) {
        for (int i = 0; i < TIMER_MAX; i++)
            if (!g_timers[i].used) { t = &g_timers[i]; break; }
    }
    if (!t) {
        host_log("timers: table full, SetTimer(%08x) dropped\n", timer_id);
        return;
    }
    if (!t->used)
        host_log("timers: SetTimer(id=%08x, firetime=%lld, interval=%lld)\n",
                 timer_id, (long long)firetime, (long long)intervaltime);
    t->used = 1;
    t->id = timer_id;
    t->firetime = firetime;
    t->interval = intervaltime;
}

void timers_unset(uint32_t timer_id)
{
    for (int i = 0; i < TIMER_MAX; i++)
        if (g_timers[i].used && g_timers[i].id == timer_id)
            g_timers[i].used = 0;
}

int timers_due(int64_t now, uint32_t *out, int max)
{
    int n = 0;
    for (int i = 0; i < TIMER_MAX && n < max; i++) {
        wr_timer *t = &g_timers[i];
        if (!t->used || now < t->firetime)
            continue;
        out[n++] = t->id;
        t->fires++;
        if (t->interval > 0) {
            /* Advance to the next future slot rather than by one interval, so
             * a host that fell behind does not queue up a burst. */
            int64_t next = t->firetime + t->interval;
            if (next <= now) {
                int64_t behind = (now - next) / t->interval + 1;
                next += behind * t->interval;
            }
            t->firetime = next;
        } else {
            t->used = 0;   /* one-shot */
        }
    }
    return n;
}

int timers_active(void)
{
    int n = 0;
    for (int i = 0; i < TIMER_MAX; i++)
        if (g_timers[i].used)
            n++;
    return n;
}

void timers_report(void)
{
    host_log("\n--- timers (%d active) ---\n", timers_active());
    for (int i = 0; i < TIMER_MAX; i++) {
        const wr_timer *t = &g_timers[i];
        if (!t->fires && !t->used)
            continue;
        host_log("  id=%08x %s firetime=%-12lld interval=%-8lld fired=%llu\n",
                 t->id, t->used ? "on " : "off",
                 (long long)t->firetime, (long long)t->interval,
                 (unsigned long long)t->fires);
    }
}
