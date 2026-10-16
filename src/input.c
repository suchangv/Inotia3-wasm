/*
 * input.c -- the pointer state machine. See input.h for why it is shaped this
 * way; this file is the mechanics.
 */

#include "input.h"

/* ---- the queue ----------------------------------------------------------
 *
 * Small and bounded on purpose. A queue that overflows means the tick loop has
 * stalled for long enough that more than 64 DOM events arrived without a frame
 * boundary, at which point the oldest are the least interesting thing in it --
 * and dropping them is better than growing without limit.
 */
#define QUEUE_MAX 64

typedef struct {
    int    id;
    int    x, y;
    double t_ms;
    int    kind;        /* 0 down, 1 move, 2 up */
} input_event;

static input_event g_q[QUEUE_MAX];
static int g_q_head, g_q_count;

/* ---- the slots ---------------------------------------------------------- */

typedef struct {
    int    id;          /* platform pointer id, or -1 when the slot is free */
    int    x, y;        /* last position handed to the engine */
    double drag_ms;     /* when the last DRAG was sent */
} input_slot;

static input_slot g_slot[INPUT_MAX_POINTERS];
static input_sink_fn g_sink;

void input_init(input_sink_fn sink)
{
    g_sink = sink;
    g_q_head = g_q_count = 0;
    for (int i = 0; i < INPUT_MAX_POINTERS; i++) {
        g_slot[i].id = -1;
        g_slot[i].x = g_slot[i].y = 0;
        g_slot[i].drag_ms = 0;
    }
}

static void push(int kind, int id, int x, int y, double t_ms)
{
    if (g_q_count >= QUEUE_MAX)
        return;                     /* see QUEUE_MAX */
    int i = (g_q_head + g_q_count) % QUEUE_MAX;
    g_q[i].kind = kind;
    g_q[i].id = id;
    g_q[i].x = x;
    g_q[i].y = y;
    g_q[i].t_ms = t_ms;
    g_q_count++;
}

void input_push_down(int id, int x, int y, double t_ms)
{ push(0, id, x, y, t_ms); }

void input_push_move(int id, int x, int y, double t_ms)
{ push(1, id, x, y, t_ms); }

void input_push_up(int id, int x, int y, double t_ms)
{ push(2, id, x, y, t_ms); }

/* ---- slots -------------------------------------------------------------- */

static int slot_find(int id)
{
    for (int s = 0; s < INPUT_MAX_POINTERS; s++)
        if (g_slot[s].id == id)
            return s;
    return -1;
}

static int slot_free(void)
{
    for (int s = 0; s < INPUT_MAX_POINTERS; s++)
        if (g_slot[s].id == -1)
            return s;
    return -1;
}

static int active_count(void)
{
    int n = 0;
    for (int s = 0; s < INPUT_MAX_POINTERS; s++)
        if (g_slot[s].id != -1)
            n++;
    return n;
}

/* MultiTouchListener packs x into the high half and y into the low half, each
 * masked to 16 bits. */
static int makeword(int x, int y)
{
    return (int)((((uint32_t)x & 0xffffu) << 16) | ((uint32_t)y & 0xffffu));
}

/* ---- replay ------------------------------------------------------------- */

static void do_down(int id, int x, int y, double t_ms)
{
    int s = slot_find(id);
    if (s >= 0) {
        /* A second DOWN for a pointer we already hold. The DOM does not
         * produce this, but a platform that lost an UP would, and treating it
         * as a move is better than leaking the slot. */
        g_slot[s].x = x;
        g_slot[s].y = y;
        return;
    }
    s = slot_free();
    if (s < 0)
        return;                     /* more than five pointers -- dropped, as
                                     * the engine's own MAX_TOUCH_POINT would */
    g_slot[s].id = id;
    g_slot[s].x = x;
    g_slot[s].y = y;
    g_slot[s].drag_ms = t_ms;
    /* Counted *after* the slot is occupied, so a first finger reports 1. */
    if (g_sink)
        g_sink(INPUT_DOWN, s + 1, makeword(x, y), active_count());
}

static void do_move(int id, int x, int y, double t_ms)
{
    int s = slot_find(id);
    if (s < 0)
        return;                     /* moved without a down -- ignore */
    int dx = x - g_slot[s].x;
    int dy = y - g_slot[s].y;
    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;
    /* Both gates, strictly greater -- the same test WrapperEventHandler makes
     * before forwarding an ACTION_MOVE. */
    if (t_ms - g_slot[s].drag_ms <= 40.0)
        return;
    if (dx <= 2 && dy <= 2)
        return;
    g_slot[s].drag_ms = t_ms;
    g_slot[s].x = x;
    g_slot[s].y = y;
    if (g_sink)
        g_sink(INPUT_DRAG, s + 1, makeword(x, y), active_count());
}

static void do_up(int id, int x, int y)
{
    int s = slot_find(id);
    if (s < 0)
        return;
    /* Counted *before* the slot is freed: the Java original decrements its
     * PointerCountInGame only after queueing the release event. */
    int n = active_count();
    if (g_sink)
        g_sink(INPUT_UP, s + 1, makeword(x, y), n);
    g_slot[s].id = -1;
}

void input_poll(void)
{
    /* Snapshot the count first: do_*() never push, but draining a queue that
     * something else could append to mid-loop is the kind of thing that works
     * until a platform adds a synthetic event source. */
    int n = g_q_count;
    for (int k = 0; k < n; k++) {
        input_event *e = &g_q[g_q_head];
        g_q_head = (g_q_head + 1) % QUEUE_MAX;
        g_q_count--;
        switch (e->kind) {
        case 0: do_down(e->id, e->x, e->y, e->t_ms); break;
        case 1: do_move(e->id, e->x, e->y, e->t_ms); break;
        default: do_up(e->id, e->x, e->y); break;
        }
    }
}

void input_release_all(void)
{
    /* Drop anything still queued as well: an UP that has not been replayed yet
     * refers to a pointer that is about to be released here, and replaying it
     * afterwards would be a no-op at best. */
    g_q_head = g_q_count = 0;
    for (int s = 0; s < INPUT_MAX_POINTERS; s++) {
        if (g_slot[s].id == -1)
            continue;
        int n = active_count();
        if (g_sink)
            g_sink(INPUT_UP, s + 1, makeword(g_slot[s].x, g_slot[s].y), n);
        g_slot[s].id = -1;
    }
}
