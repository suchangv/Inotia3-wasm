/*
 * platform_wasm.c -- host services for the freestanding wasm32 build.
 *
 * Deliberately dependent on nothing: no emscripten, no libc. The build is
 * clang --target=wasm32 -nostdlib with `--allow-undefined`, so the four things
 * that genuinely need the outside world -- logging, the clock, the ability to
 * size the heap -- come in as imports the JS driver supplies. Everything else
 * is implemented here.
 *
 * This exists to answer one question with a measurement rather than an
 * estimate: instructions retired per frame is now known (~8,963, see
 * docs/HANDOFF.md), so the browser's frame cost is that number divided by the
 * interpreter's throughput under wasm. The synthetic benchmark said 58% of
 * native; this build checks it against the real binary.
 *
 * It is not the browser port. There is no GL, no audio, no asset loading and no
 * DOM -- those are the renderer and host-layer items, and they will want
 * emscripten. What is here is the runner plus the clock, which is all a
 * throughput number needs.
 */

#include "guest.h"
#include "compat.h"
#include "platform.h"

#include <stdarg.h>   /* compiler-provided, so available without libc */

/* ---- what the JS driver must provide ------------------------------------- */
__attribute__((import_module("host"), import_name("log")))
extern void js_log(const char *s, int len);

__attribute__((import_module("host"), import_name("now")))
extern double js_now(void);

/* ---- logging --------------------------------------------------------------
 *
 * A printf for the host side. Unlike the guest-facing one in host_libc.c --
 * which has to read arguments out of a flat word array because a guest call
 * carries them in registers and on the guest stack -- this is an ordinary
 * variadic C function, so arguments arrive with their real types and va_arg
 * works normally. That is the whole reason it is a separate implementation
 * rather than a shared one.
 */
#define LOG_BUF 2048

typedef struct {
    char  *dst;
    size_t cap;
    size_t n;
} sink;

static void s_ch(sink *s, char c)
{
    if (s->n + 1 < s->cap)
        s->dst[s->n] = c;
    s->n++;
}

static void s_str(sink *s, const char *p, int width, int left)
{
    if (!p)
        p = "(null)";
    size_t len = strlen(p);
    if (!left)
        for (size_t i = len; i < (size_t)(width > 0 ? width : 0); i++)
            s_ch(s, ' ');
    for (size_t i = 0; i < len; i++)
        s_ch(s, p[i]);
    if (left)
        for (size_t i = len; i < (size_t)(width > 0 ? width : 0); i++)
            s_ch(s, ' ');
}

static void s_num(sink *s, uint64_t v, int base, int upper, int width, int zero,
                  int negative, int precision)
{
    char tmp[24];
    int n = 0;
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    if (!v && precision != 0)
        tmp[n++] = '0';
    while (v) {
        tmp[n++] = digits[v % (unsigned)base];
        v /= (unsigned)base;
    }
    while (n < precision)
        tmp[n++] = '0';

    int total = n + (negative ? 1 : 0);
    if (!zero && !negative)
        for (int i = total; i < width; i++)
            s_ch(s, ' ');
    if (negative)
        s_ch(s, '-');
    if (zero && !negative)
        for (int i = total; i < width; i++)
            s_ch(s, '0');
    while (n--)
        s_ch(s, tmp[n]);
}

/* Floats to a fixed precision. Only used for timings and percentages, so a
 * simple scale-and-print is enough; no exponent, no shortest-round-trip. */
static void s_float(sink *s, double d, int width, int precision, int left,
                    int zero)
{
    char tmp[64];
    int n = 0;
    if (d != d) { s_str(s, "nan", width, left); return; }
    int neg = d < 0;
    if (neg) d = -d;
    if (precision < 0) precision = 6;

    uint64_t ip = (uint64_t)d;
    double fp = d - (double)ip;
    char frac[16];
    for (int i = 0; i < precision && i < 15; i++) {
        fp *= 10.0;
        int digit = (int)fp;
        frac[i] = (char)('0' + digit);
        fp -= digit;
    }

    char intbuf[24];
    int in = 0;
    if (!ip) intbuf[in++] = '0';
    while (ip) { intbuf[in++] = (char)('0' + (int)(ip % 10)); ip /= 10; }
    if (neg) tmp[n++] = '-';
    while (in--) tmp[n++] = intbuf[in];
    if (precision > 0) {
        tmp[n++] = '.';
        for (int i = 0; i < precision && i < 15; i++) tmp[n++] = frac[i];
    }
    if (!left)
        for (int i = n; i < width; i++) s_ch(s, zero ? '0' : ' ');
    for (int i = 0; i < n; i++) s_ch(s, tmp[i]);
    if (left)
        for (int i = n; i < width; i++) s_ch(s, ' ');
}

static int mini_vsnprintf(char *dst, size_t cap, const char *fmt, va_list ap)
{
    sink s = { dst, cap, 0 };

    for (const char *p = fmt; *p; p++) {
        if (*p != '%') { s_ch(&s, *p); continue; }
        p++;
        int left = 0, zero = 0;
        for (;; p++) {
            if (*p == '-') left = 1;
            else if (*p == '0') zero = 1;
            else if (*p == '+' || *p == ' ' || *p == '#') { }
            else break;
        }
        int width = 0;
        while (*p >= '0' && *p <= '9') width = width * 10 + (*p++ - '0');
        int precision = -1;
        if (*p == '.') {
            p++;
            precision = 0;
            while (*p >= '0' && *p <= '9') precision = precision * 10 + (*p++ - '0');
        }
        int lng = 0;
        while (*p == 'l' || *p == 'h' || *p == 'z' || *p == 'j') {
            if (*p == 'l' || *p == 'z' || *p == 'j') lng++;
            p++;
        }

        switch (*p) {
        case 'd': case 'i': {
            int64_t v = (lng >= 2) ? va_arg(ap, int64_t)
                                   : (int64_t)va_arg(ap, int);
            int neg = v < 0;
            uint64_t u = neg ? (uint64_t)(-v) : (uint64_t)v;
            s_num(&s, u, 10, 0, width, zero, neg, precision);
            break;
        }
        case 'u':
            s_num(&s, (lng >= 2) ? va_arg(ap, uint64_t)
                                 : (uint64_t)va_arg(ap, unsigned),
                  10, 0, width, zero, 0, precision);
            break;
        case 'x':
            s_num(&s, (lng >= 2) ? va_arg(ap, uint64_t)
                                 : (uint64_t)va_arg(ap, unsigned),
                  16, 0, width, zero, 0, precision);
            break;
        case 'X':
            s_num(&s, (lng >= 2) ? va_arg(ap, uint64_t)
                                 : (uint64_t)va_arg(ap, unsigned),
                  16, 1, width, zero, 0, precision);
            break;
        case 'p':
            s_str(&s, "0x", 0, 0);
            s_num(&s, (uint64_t)(uintptr_t)va_arg(ap, void *), 16, 0, 8, 1, 0, -1);
            break;
        case 'c':
            s_ch(&s, (char)va_arg(ap, int));
            break;
        case 's':
            s_str(&s, va_arg(ap, const char *), width, left);
            break;
        case 'f':
            s_float(&s, va_arg(ap, double), width, precision, left, zero);
            break;
        case '%':
            s_ch(&s, '%');
            break;
        case 0:
            goto done;
        default:
            s_ch(&s, '%');
            s_ch(&s, *p);
            break;
        }
    }
done:
    if (s.cap)
        s.dst[s.n < s.cap ? s.n : s.cap - 1] = 0;
    return (int)s.n;
}

void host_log(const char *fmt, ...)
{
    char buf[LOG_BUF];
    va_list ap;
    va_start(ap, fmt);
    int n = mini_vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > (int)sizeof(buf) - 1)
        n = (int)sizeof(buf) - 1;
    js_log(buf, n);
}

void host_fatal(const char *fmt, ...)
{
    char buf[LOG_BUF];
    va_list ap;
    va_start(ap, fmt);
    int n = mini_vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > (int)sizeof(buf) - 1)
        n = (int)sizeof(buf) - 1;
    js_log(buf, n);
    /* No exit() in a freestanding build; the driver stops when the export
     * returns. Trapping here would lose the report the run just produced. */
}

/* ---- clock --------------------------------------------------------------- */

double host_now(void) { return js_now(); }

/* ---- allocation ----------------------------------------------------------
 *
 * A bump allocator over wasm linear memory, growing as needed. Enough for what
 * the loader asks of it: the 144 MB guest address space, the symbol tables and
 * the profile counters. Nothing here is ever freed, which is true of the native
 * build's use of host_alloc as well.
 */
extern unsigned char __heap_base;

static uintptr_t g_heap_top;
static uintptr_t g_heap_end;

static void heap_grow_to(uintptr_t want)
{
    uintptr_t have = (uintptr_t)__builtin_wasm_memory_size(0) * 65536u;
    if (want <= have) {
        g_heap_end = have;
        return;
    }
    uintptr_t need_pages = (want - have + 65535u) / 65536u;
    if (__builtin_wasm_memory_grow(0, need_pages) == (uintptr_t)-1) {
        js_log("host_alloc: cannot grow wasm memory\n", 34);
        g_heap_end = have;
        return;
    }
    g_heap_end = (uintptr_t)__builtin_wasm_memory_size(0) * 65536u;
}

void *host_alloc(size_t n)
{
    if (!g_heap_top) {
        g_heap_top = (uintptr_t)&__heap_base;
        g_heap_top = (g_heap_top + 15u) & ~(uintptr_t)15u;
        heap_grow_to(g_heap_top);
    }
    uintptr_t p = (g_heap_top + 15u) & ~(uintptr_t)15u;
    uintptr_t end = p + n;
    if (end > g_heap_end)
        heap_grow_to(end + 65536u);
    if (end > g_heap_end)
        return 0;
    g_heap_top = end;
    memset((void *)p, 0, n);
    return (void *)p;
}

/* Exported for the JS driver: it needs somewhere in linear memory to put the
 * .so before calling main(). */
void *spike_alloc(size_t n) { return host_alloc(n); }

/* ---- the rest of the platform surface ------------------------------------ */

uint32_t host_guest_mmap(uint32_t size)
{
    static uint32_t next = GUEST_MMAP_BASE;
    if (!size)
        return 0;
    uint32_t a = (next + 0xFFFu) & ~0xFFFu;
    if (a + size > GUEST_MMAP_BASE + GUEST_MMAP_SIZE)
        return 0;
    next = a + size;
    return a;
}

/* Nothing to install: a bad guest access faults the wasm instance, and the
 * driver reports the guest PC from the exported state. */
const char *host_app_data_dir(const char *package)
{ (void)package; return "data/"; }

void host_install_crash_handler(void) { }

/* Instruction tracing writes to a file, which does not exist here. Kept as
 * no-ops so the interpreter's trace hooks stay unconditional. */
void host_trace_open(const char *path) { (void)path; }
void host_trace_close(void) { }

void host_trace_insn(uint32_t pc, const uint32_t *r, uint32_t cpsr)
{
    (void)pc; (void)r; (void)cpsr;
}
