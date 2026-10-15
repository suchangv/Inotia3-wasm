/*
 * host_libc.c -- what libgame.so expects from bionic, plus the guest heap.
 *
 * The guest is a C++ engine: it allocates and frees constantly, so it needs a
 * real allocator rather than a bump pointer, and it needs one that lives
 * *inside* the guest address space because guest code holds those addresses in
 * its own pointers and data structures.
 *
 * Everything here is written against the guest address space directly. The
 * native and browser builds differ only in how file I/O and threads are
 * answered, which is why this file is split from the platform glue.
 */

#include "imports.h"
#include "guest.h"
#include "cpu.h"
#include "compat.h"
#include "platform.h"
#include "jni.h"

/* ========================================================================== */
/* Guest heap: first-fit free list with forward coalescing.                    */
/* ========================================================================== */

typedef struct {
    uint32_t size;      /* payload bytes, low bit flags the block as free */
    uint32_t prev_size; /* to walk backwards while coalescing             */
} blk_hdr;

#define BLK_FREE 1u
#define HDR_SZ   ((uint32_t)((sizeof(blk_hdr) + 7u) & ~7u))
#define ALIGN8(n) (((n) + 7u) & ~7u)

static uint32_t heap_start;   /* first block header                     */
static uint32_t heap_break;   /* one past the last address carved out    */

static void heap_init(void)
{
    if (heap_start)
        return;
    heap_start = GUEST_HEAP_START;
    heap_break = heap_start;
    blk_hdr *h = (blk_hdr *)(g_mem + heap_start);
    uint32_t sz = GUEST_HEAP_SIZE - (heap_start - GUEST_HEAP_BASE) - HDR_SZ;
    /* The free flag is part of the size word, not separate state, and the
     * allocator's first act is to look for a block that has it. Forgetting it
     * here makes the whole heap invisible and every allocation fail with
     * "out of memory" on an empty 64 MB arena. */
    h->size = sz | BLK_FREE;
    h->prev_size = 0;
    heap_break = heap_start + HDR_SZ + sz;
}

static uint32_t heap_alloc(uint32_t want)
{
    heap_init();
    want = ALIGN8(want ? want : 8);

    uint32_t at = heap_start;
    while (at + HDR_SZ <= heap_break) {
        blk_hdr *h = (blk_hdr *)(g_mem + at);
        uint32_t sz = h->size & ~BLK_FREE;
        if ((h->size & BLK_FREE) && sz >= want) {
            /* Split when the remainder can hold a header plus a sane payload;
             * otherwise hand over the whole block to avoid a run of unusable
             * slivers. */
            if (sz >= want + HDR_SZ + 16u) {
                uint32_t rest = at + HDR_SZ + want;
                blk_hdr *n = (blk_hdr *)(g_mem + rest);
                n->size = (sz - want - HDR_SZ) | BLK_FREE;
                n->prev_size = want;
                h->size = want;
                blk_hdr *after = (blk_hdr *)(g_mem + rest + HDR_SZ + n->size);
                if (rest + HDR_SZ + n->size + HDR_SZ <= heap_break)
                    after->prev_size = n->size;
            }
            h->size &= ~BLK_FREE;
            return at + HDR_SZ;
        }
        if (!sz)
            break;
        at += HDR_SZ + sz;
    }
    host_log("heap: out of memory requesting %u bytes (heap %08x..%08x)\n",
             want, heap_start, heap_break);
    return 0;
}

static void heap_free(uint32_t ptr)
{
    if (!ptr)
        return;
    uint32_t at = ptr - HDR_SZ;
    blk_hdr *h = (blk_hdr *)(g_mem + at);
    h->size |= BLK_FREE;

    /* Coalesce forward. */
    for (;;) {
        uint32_t nxt = at + HDR_SZ + (h->size & ~BLK_FREE);
        if (nxt + HDR_SZ > heap_break)
            break;
        blk_hdr *n = (blk_hdr *)(g_mem + nxt);
        if (!(n->size & BLK_FREE))
            break;
        h->size = (h->size & ~BLK_FREE) + HDR_SZ + (n->size & ~BLK_FREE);
        h->size |= BLK_FREE;
    }
    /* Coalesce backward using prev_size. */
    if (h->prev_size) {
        blk_hdr *p = (blk_hdr *)(g_mem + at - HDR_SZ - h->prev_size);
        if (p->size & BLK_FREE) {
            p->size = (p->size & ~BLK_FREE) + HDR_SZ + (h->size & ~BLK_FREE);
            p->size |= BLK_FREE;
        }
    }
}

uint32_t guest_heap_alloc(uint32_t n) { return heap_alloc(n); }

static uint32_t heap_usable(uint32_t ptr)
{
    if (!ptr)
        return 0;
    blk_hdr *h = (blk_hdr *)(g_mem + ptr - HDR_SZ);
    return h->size & ~BLK_FREE;
}

/* ========================================================================== */
/* Argument plumbing                                                          */
/* ========================================================================== */

/* The guest has no threads yet in this build, so a single shadow stack of
 * scratch buffers for string formatting is enough. When thread support lands
 * these become per-thread. */
#define SCRATCH_N 8
#define SCRATCH_SZ 8192
static char  g_scratch[SCRATCH_N][SCRATCH_SZ];
static int   g_scratch_i;

static char *scratch_next(void)
{
    char *p = g_scratch[g_scratch_i];
    g_scratch_i = (g_scratch_i + 1) % SCRATCH_N;
    p[0] = 0;
    return p;
}

/* Read the guest's string argument into a host buffer. Reasons: a guest string
 * is not NUL-terminated in the way a host string is expected to be (the guest
 * can and does pass unterminated buffers to strncmp-like calls), and reading
 * it lazily during a host_log() from inside the interpreter risks walking off
 * into unmapped guest memory. */
static const char *guest_str(uint32_t addr, size_t max)
{
    char *out = scratch_next();
    if (!addr)
        return "(null)";
    size_t i = 0;
    for (; i < max - 1 && i < GUEST_MEM_SIZE; i++) {
        char c = (char)g_mem[addr + i];
        out[i] = c;
        if (!c)
            return out;
    }
    out[i] = 0;
    return out;
}

/* ========================================================================== */
/* A small printf, because snprintf/sprintf/vsprintf/fprintf are all imported  */
/* ========================================================================== */

typedef struct {
    char  *dst;      /* host buffer, or NULL when writing to the log  */
    size_t cap;
    size_t n;
} sink;

static void sink_ch(sink *s, char c)
{
    if (s->dst) {
        if (s->n + 1 < s->cap)
            s->dst[s->n] = c;
    } else {
        char b[2] = { c, 0 };
        host_log("%s", b);
    }
    s->n++;
}

static void sink_str(sink *s, const char *p)
{
    if (!p)
        p = "(null)";
    while (*p)
        sink_ch(s, *p++);
}

static void sink_uint(sink *s, uint32_t v, uint32_t base, int upper, int width,
                      char pad)
{
    char tmp[36];
    int n = 0;
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    if (v == 0)
        tmp[n++] = '0';
    while (v) {
        tmp[n++] = digits[v % base];
        v /= base;
    }
    for (int i = n; i < width; i++)
        sink_ch(s, pad);
    while (n--)
        sink_ch(s, tmp[n]);
}

static void sink_int(sink *s, int32_t v, int width, char pad)
{
    if (v < 0) {
        sink_ch(s, '-');
        uint32_t u = (uint32_t)(-(int64_t)v);
        sink_uint(s, u, 10, 0, width ? width - 1 : 0, pad);
    } else {
        sink_uint(s, (uint32_t)v, 10, 0, width, pad);
    }
}

static void format(sink *s, const char *fmt, uint32_t *args, int nargs)
{
    int ai = 0;
    for (const char *p = fmt; *p; p++) {
        if (*p != '%') {
            sink_ch(s, *p);
            continue;
        }
        p++;
        char pad = ' ';
        int width = 0;
        if (*p == '0') { pad = '0'; p++; }
        while (*p >= '0' && *p <= '9') { width = width * 10 + (*p - '0'); p++; }
        int is_long = 0;
        while (*p == 'l' || *p == 'h' || *p == 'z') {
            if (*p == 'l') is_long++;
            p++;
        }
        uint32_t a = (ai < nargs) ? args[ai] : 0;
        switch (*p) {
        case 'd': case 'i':
            if (is_long >= 2) {
                int64_t v = (int64_t)(((uint64_t)args[ai + 1] << 32) | a);
                ai += 2;
                if (v < 0) { sink_ch(s, '-'); v = -v; }
                char tmp[24]; int n = 0;
                if (!v) tmp[n++] = '0';
                while (v) { tmp[n++] = (char)('0' + (int)(v % 10)); v /= 10; }
                for (int i = n; i < width; i++) sink_ch(s, pad);
                while (n--) sink_ch(s, tmp[n]);
            } else {
                ai++;
                sink_int(s, (int32_t)a, width, pad);
            }
            break;
        case 'u':
            ai++;
            sink_uint(s, a, 10, 0, width, pad);
            break;
        case 'x':
            ai++;
            sink_uint(s, a, 16, 0, width, pad);
            break;
        case 'X':
            ai++;
            sink_uint(s, a, 16, 1, width, pad);
            break;
        case 'p':
            ai++;
            sink_str(s, "0x");
            sink_uint(s, a, 16, 0, 8, '0');
            break;
        case 'c':
            ai++;
            sink_ch(s, (char)a);
            break;
        case 's':
            ai++;
            sink_str(s, guest_str(a, 4096));
            break;
        case 'f': {
            ai++;
            /* 32-bit guest floats are promoted to double by AAPCS varargs;
             * a float bit pattern in a single register is the other common
             * shape, so handle both by checking whether it looks like a
             * double. */
            double d;
            uint64_t bits = ((uint64_t)args[ai] << 32) | a;
            (void)bits;
            union { uint32_t u; float f; } cv;
            cv.u = a;
            d = (double)cv.f;
            ai++;
            if (d < 0) { sink_ch(s, '-'); d = -d; }
            uint64_t ip = (uint64_t)d;
            sink_uint(s, (uint32_t)ip, 10, 0, 0, ' ');
            sink_ch(s, '.');
            double fp = d - (double)ip;
            for (int i = 0; i < 6; i++) {
                fp *= 10.0;
                int digit = (int)fp;
                sink_ch(s, (char)('0' + digit));
                fp -= digit;
            }
            break;
        }
        case '%':
            sink_ch(s, '%');
            break;
        case 0:
            return;
        default:
            sink_ch(s, '%');
            sink_ch(s, *p);
            break;
        }
    }
}

/* Count the arguments a printf-style format consumes, so the caller can pull
 * them off the guest's register/stack argument area correctly. */
static int count_args(const char *fmt)
{
    int n = 0;
    for (const char *p = fmt; *p; p++) {
        if (*p != '%')
            continue;
        p++;
        if (*p == '%') continue;
        while (*p == 'l' || *p == 'h' || *p == 'z' || *p == '0' ||
               (*p >= '1' && *p <= '9'))
            p++;
        if (!*p) break;
        if (*p == 'd' || *p == 'i' || *p == 'u' || *p == 'x' || *p == 'X' ||
            *p == 'c' || *p == 's' || *p == 'p')
            n++;
        else if (*p == 'f')
            n += 2;   /* a double arrives as two 32-bit words */
    }
    return n;
}

/* ========================================================================== */
/* The dispatch                                                               */
/* ========================================================================== */

void host_import_call(guest_import *im)
{
    uint32_t *R = g_cpu.r;
    uint32_t a0 = R[0], a1 = R[1], a2 = R[2], a3 = R[3];
    /* AAPCS: arguments past the fourth live on the guest stack. */
    uint32_t sp = R[13];
    uint32_t extra[24];
    int nextra = (int)(sizeof(extra) / sizeof(extra[0]));
    for (int i = 0; i < nextra; i++)
        extra[i] = LD32(sp + (uint32_t)i * 4u);

    /* Only the guest's own args matter here; a0..a3 followed by the stack. */
    uint32_t args[32];
    for (int i = 0; i < 4; i++)
        args[i] = R[i];
    for (int i = 0; i < nextra; i++)
        args[4 + i] = extra[i];
    int nargs = 4 + nextra;

    R[0] = 0;
    R[1] = 0;

    /* JNI table entries arrive through the same mechanism as everything else:
     * the guest branched to a BKPT stub and the interpreter derived its index
     * from the stub address. Nothing here needs to know that. */
    if (strncmp(im->name, "jni/", 4) == 0) {
        uint32_t lo = 0, hi = 0;
        jni_host_call(im->name, args, nargs, &lo, &hi);
        R[0] = lo;
        R[1] = hi;
        return;
    }

    switch (im->impl) {

    /* ---- memory ---- */
    case IMPL_malloc:  R[0] = heap_alloc(a0); break;
    case IMPL_free:    heap_free(a0); break;
    case IMPL_calloc: {
        uint32_t n = a0 * a1;
        R[0] = heap_alloc(n);
        if (R[0]) memset(g_mem + R[0], 0, n);
        break;
    }
    case IMPL_realloc: {
        if (!a0) { R[0] = heap_alloc(a1); break; }
        if (!a1) { heap_free(a0); R[0] = 0; break; }
        uint32_t old = heap_usable(a0);
        R[0] = heap_alloc(a1);
        if (R[0]) memcpy(g_mem + R[0], g_mem + a0, old < a1 ? old : a1);
        heap_free(a0);
        break;
    }

    /* ---- strings ---- */
    case IMPL_memcpy:  if (a0 && a1) memcpy(g_mem + a0, g_mem + a1, a2); R[0] = a0; break;
    case IMPL_memset:  if (a0) memset(g_mem + a0, (int)a1, a2); R[0] = a0; break;
    case IMPL_memmove: if (a0 && a1) memmove(g_mem + a0, g_mem + a1, a2); R[0] = a0; break;
    case IMPL_memcmp:  R[0] = a0 && a1 ? (uint32_t)(int32_t)memcmp(g_mem + a0, g_mem + a1, a2) : 0; break;
    case IMPL_strlen:  R[0] = a0 ? (uint32_t)strlen((const char *)(g_mem + a0)) : 0; break;
    case IMPL_strcmp:  R[0] = (uint32_t)(int32_t)strcmp(guest_str(a0, 4096), guest_str(a1, 4096)); break;
    case IMPL_strncmp: R[0] = (uint32_t)(int32_t)memcmp(guest_str(a0, a2 + 1), guest_str(a1, a2 + 1), a2); break;
    case IMPL_strcpy: {
        const char *s = guest_str(a1, 4096);
        size_t n = strlen(s) + 1;
        if (a0) memcpy(g_mem + a0, s, n);
        R[0] = a0;
        break;
    }
    case IMPL_strncpy: {
        const char *s = guest_str(a1, a2 + 1);
        size_t n = strlen(s);
        if (n > a2) n = a2;
        if (a0) { memcpy(g_mem + a0, s, n); memset(g_mem + a0 + n, 0, a2 - n); }
        R[0] = a0;
        break;
    }
    case IMPL_strcat: {
        const char *s = guest_str(a1, 4096);
        size_t l = a0 ? strlen((const char *)(g_mem + a0)) : 0;
        if (a0) memcpy(g_mem + a0 + l, s, strlen(s) + 1);
        R[0] = a0;
        break;
    }
    case IMPL_strncat: {
        const char *s = guest_str(a1, a2 + 1);
        size_t l = a0 ? strlen((const char *)(g_mem + a0)) : 0;
        size_t n = strlen(s);
        if (n > a2) n = a2;
        if (a0) { memcpy(g_mem + a0 + l, s, n); g_mem[a0 + l + n] = 0; }
        R[0] = a0;
        break;
    }
    case IMPL_strchr:
    case IMPL_strrchr: {
        if (!a0) { R[0] = 0; break; }
        const char *s = (const char *)(g_mem + a0);
        uint32_t found = 0;
        for (uint32_t i = 0; ; i++) {
            if (s[i] == (char)a1) {
                found = a0 + i;
                if (im->impl == IMPL_strchr) break;
            }
            if (!s[i]) break;
        }
        R[0] = found;
        break;
    }
    case IMPL_strstr: {
        const char *h = guest_str(a0, 8192);
        const char *n = guest_str(a1, 4096);
        if (!*n) { R[0] = a0; break; }
        uint32_t res = 0;
        for (size_t i = 0; h[i]; i++) {
            size_t j = 0;
            while (n[j] && h[i + j] == n[j]) j++;
            if (!n[j]) { res = a0 + (uint32_t)i; break; }
        }
        R[0] = res;
        break;
    }
    case IMPL_strdup: {
        const char *s = guest_str(a0, 4096);
        size_t n = strlen(s) + 1;
        uint32_t p = heap_alloc((uint32_t)n);
        if (p) memcpy(g_mem + p, s, n);
        R[0] = p;
        break;
    }
    case IMPL_strerror: R[0] = a0; break;   /* the guest's code, echoed back */
    case IMPL_strtok: {
        static uint32_t save;
        const char *delim = guest_str(a1, 256);
        uint32_t start = a0 ? a0 : save;
        if (!start) { R[0] = 0; break; }
        uint8_t *m = g_mem + start;
        while (*m && strchr(delim, (char)*m)) m++;
        if (!*m) { save = 0; R[0] = 0; break; }
        uint32_t tok = (uint32_t)(m - g_mem);
        while (*m && !strchr(delim, (char)*m)) m++;
        if (*m) { *m = 0; save = (uint32_t)(m - g_mem) + 1; }
        else save = 0;
        R[0] = tok;
        break;
    }
    case IMPL_strtod: {
        const char *s = guest_str(a0, 256);
        int sign = 1; double v = 0;
        while (*s == ' ') s++;
        if (*s == '-') { sign = -1; s++; } else if (*s == '+') s++;
        while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; }
        if (*s == '.') {
            s++;
            double f = 0.1;
            while (*s >= '0' && *s <= '9') { v += (*s - '0') * f; f *= 0.1; s++; }
        }
        union { float f; uint32_t u; } cv;
        cv.f = (float)(sign * v);
        R[0] = cv.u;
        break;
    }
    case IMPL_atoi: {
        const char *s = guest_str(a0, 64);
        int sign = 1;
        while (*s == ' ' || *s == '\t') s++;
        if (*s == '-') { sign = -1; s++; } else if (*s == '+') s++;
        int32_t v = 0;
        while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; }
        R[0] = (uint32_t)(v * sign);
        break;
    }
    case IMPL_atoll:
    case IMPL_strtoll: {
        const char *s = guest_str(a0, 64);
        int sign = 1;
        while (*s == ' ') s++;
        if (*s == '-') { sign = -1; s++; }
        int64_t v = 0;
        while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; }
        v *= sign;
        R[0] = (uint32_t)v;
        R[1] = (uint32_t)((uint64_t)v >> 32);
        break;
    }
    case IMPL_qsort:
        /* Needs to re-enter the interpreter to run the guest's comparator.
         * Deferred until the first run says whether the engine actually sorts
         * anything on the boot path -- it costs nothing to leave it on the
         * worklist, and re-entrancy is a design decision worth making once,
         * deliberately, rather than inline here. */
        host_log("host: qsort() not implemented yet (base=%08x n=%u size=%u)\n",
                 a0, a1, a2);
        break;

    /* ---- runtime ---- */
    case IMPL_abort:
    case IMPL_stack_chk_fail:
        host_log("guest: %s() called -- this is a guest-side bug\n", im->name);
        g_cpu.r[0] = 0;
        /* Do not kill the host: the spike wants the run to continue and report
         * the profile so far, not to abort the whole session. */
        break;
    case IMPL_assert2:
        host_log("guest assert: %s:%d: %s\n", guest_str(a1, 256), (int)a2,
                 guest_str(a3, 512));
        break;
    case IMPL_android_log_write: {
        int prio = (int)a0;
        const char *tag = guest_str(a1, 128);
        const char *msg = guest_str(a2, 1024);
        host_log("[android/%d %s] %s\n", prio, tag, msg);
        break;
    }
    case IMPL_cxa_atexit:
    case IMPL_aeabi_atexit:
        R[0] = 0;   /* nothing here ever needs the destructor to run */
        break;
    case IMPL_cxa_finalize:
        break;

    /* ---- variables ---- */
    /* __errno is an imported *object*, so it never reaches this switch: the
     * loader pointed its GOT slot at real guest storage instead. See
     * imports_build_stubs(). */

    /* ---- time ---- */
    case IMPL_gettimeofday: {
        /* struct timeval { long tv_sec; long tv_usec; } -- and on a 32-bit
         * Android target "long" is 32 bits. */
        if (a0) {
            double now = host_now();
            ST32(a0, (uint32_t)now);
            ST32(a0 + 4, (uint32_t)((now - (double)(uint32_t)now) * 1e6));
        }
        R[0] = 0;
        break;
    }
    case IMPL_time: {
        uint32_t t = (uint32_t)host_now();
        if (a0) ST32(a0, t);
        R[0] = t;
        break;
    }
    case IMPL_nanosleep:
    case IMPL_usleep:
    case IMPL_sleep:
        R[0] = 0;   /* the host drives the frame clock; sleeping only stalls */
        break;

    /* ---- math ----
     * Gated on the same reasoning as file I/O: sin/cos/pow are for the engine's
     * own effects maths, not the boot path. host_sin/host_cos/host_pow are
     * declared in platform.h and left for whoever lands the host that needs
     * them; until then these report rather than silently return 0.0, which
     * would look like a game bug instead of a missing shim. */
    case IMPL_sin: case IMPL_cos: case IMPL_pow:
        host_log("host: %s() not implemented yet (arg=%08x)\n", im->name, a0);
        break;
    case IMPL_isinf: {
        union { uint32_t u; float f; } x;
        x.u = a0;
        uint32_t e = (x.u >> 23) & 0xff;
        R[0] = (e == 0xff && !(x.u & 0x7fffff)) ? 1u : 0u;
        break;
    }

    /* ---- threads ---- */
    /* Single-threaded for now: mutexes succeed and do nothing, and a thread
     * create is refused loudly rather than silently ignored, because a game
     * thread that never runs would look like a hang much later. */
    case IMPL_pthread_mutex_init:
    case IMPL_pthread_mutex_lock:
    case IMPL_pthread_mutex_unlock:
    case IMPL_pthread_mutex_trylock:
    case IMPL_pthread_mutex_destroy:
    case IMPL_pthread_mutexattr_init:
    case IMPL_pthread_mutexattr_settype:
    case IMPL_pthread_mutexattr_destroy:
    case IMPL_pthread_cond_init:
    case IMPL_pthread_cond_signal:
    case IMPL_pthread_cond_broadcast:
    case IMPL_pthread_cond_destroy:
        R[0] = 0;
        break;
    case IMPL_pthread_cond_wait:
        R[0] = 0;
        break;
    case IMPL_pthread_self:  R[0] = 1; break;
    case IMPL_pthread_equal: R[0] = (a0 == a1); break;
    case IMPL_pthread_key_create: {
        static uint32_t next_key = 1;
        if (a0) ST32(a0, next_key++);
        R[0] = 0;
        break;
    }
    case IMPL_pthread_getspecific: R[0] = 0; break;
    case IMPL_pthread_setspecific: R[0] = 0; break;
    case IMPL_pthread_create:
        host_log("host: pthread_create refused (single-threaded host); "
                 "entry=%08x arg=%08x\n", a2, a3);
        R[0] = 11;   /* EAGAIN */
        break;
    case IMPL_pthread_join:
    case IMPL_pthread_exit:
    case IMPL_pthread_detach:
        R[0] = 0;
        break;

    /* ---- loader / misc ---- */
    case IMPL_getpid: R[0] = 4242; break;
    case IMPL_dlopen: R[0] = 0; break;
    case IMPL_dlsym:  R[0] = 0; break;
    case IMPL_dlclose: R[0] = 0; break;
    case IMPL_mmap:   R[0] = host_guest_mmap(a1); break;
    case IMPL_munmap: R[0] = 0; break;

    /* ---- file I/O: the asset path ----
     * This is where the game reads its 926 asset files, so it is required to
     * reach a title screen -- but it is not required to measure the boot path,
     * and the browser host will answer it from fetch()/IDBFS rather than from
     * a filesystem. Left on the worklist deliberately.
     */

    /* ---- formatting ---- */
    case IMPL_snprintf:
    case IMPL_sprintf: {
        const char *fmt = guest_str(a1, 4096);
        int n = count_args(fmt);
        sink s;
        s.dst = (im->impl == IMPL_snprintf) ? (char *)(g_mem + a0) : (char *)(g_mem + a0);
        s.cap = (im->impl == IMPL_snprintf) ? a2 : 1u << 30;
        s.n = 0;
        format(&s, fmt, &args[1], n + 4);
        sink_ch(&s, 0);
        R[0] = (uint32_t)s.n - 1;
        break;
    }
    case IMPL_vsnprintf:
    case IMPL_vsprintf: {
        const char *fmt = guest_str(a1, 4096);
        int n = count_args(fmt);
        sink s;
        s.dst = (char *)(g_mem + a0);
        s.cap = (im->impl == IMPL_vsnprintf) ? a2 : 1u << 30;
        s.n = 0;
        format(&s, fmt, &args[2], n + 2);
        sink_ch(&s, 0);
        R[0] = (uint32_t)s.n - 1;
        break;
    }
    case IMPL_fprintf: {
        const char *fmt = guest_str(a1, 4096);
        int n = count_args(fmt);
        sink s;
        s.dst = NULL;   /* straight to the host log */
        s.cap = 0;
        s.n = 0;
        format(&s, fmt, &args[1], n + 4);
        break;
    }

    default:
        /* Called, but not provided. import_dispatch() has already counted it;
         * the report lists these as the worklist. Returning 0 keeps the guest
         * running so one run yields as many distinct gaps as possible. */
        break;
    }
}
