/*
 * util.c -- guest memory backing, host services, and a dependency-free sort.
 *
 * The native build gets the real libc; the wasm32 build gets enough of it
 * inline that the core needs no emscripten. Keeping both paths in one file
 * makes it obvious what a browser port still has to supply.
 */

#include "compat.h"
#include "guest.h"
#include "cpu.h"

/* ---- guest address space ------------------------------------------------- */

uint8_t *g_mem;
uint32_t g_mem_size = GUEST_MEM_SIZE;

/* ---- guest backtrace -----------------------------------------------------
 *
 * There is no frame-pointer chain to walk: this is Thumb code compiled with
 * -fomit-frame-pointer, which is why the desktop crash handler can only name
 * the faulting PC. But the guest stack is contiguous and single-threaded, and
 * every call leaves a return address in it -- an address into .text with the
 * Thumb bit set. Scanning for those recovers the call chain well enough to
 * answer "which subsystem called this", which is the question every fault
 * actually asks.
 *
 * Heuristic, and presented as one: a stale return address from a function that
 * has already returned is still on the stack and still looks valid, so the
 * chain can carry extra entries. It never invents one that was not a real
 * call site, though, which is what makes it worth printing.
 */

void guest_backtrace(const char *why)
{
    uint32_t text_lo = 0, text_hi = 0, sp = g_cpu.r[13];
    guest_text_bounds(&text_lo, &text_hi);
    if (sp < GUEST_STACK_TOP - GUEST_STACK_SIZE || sp >= GUEST_STACK_TOP) {
        host_log("  [bt] (%s) sp %08x is not on the guest stack\n", why, sp);
        return;
    }
    host_log("  [bt] (%s) pc=%08x lr=%08x sp=%08x\n", why, g_cpu.r[15],
             g_cpu.r[14], sp);
    int shown = 0;
    for (uint32_t a = sp; a + 4 <= GUEST_STACK_TOP && shown < 24; a += 4) {
        uint32_t w = *(uint32_t *)(g_mem + a);
        if (!(w & 1))
            continue;                       /* Thumb return addresses are odd */
        uint32_t t = w & ~1u;
        if (t < text_lo || t >= text_hi)
            continue;
        host_log("  [bt]   [sp+%03x] %08x\n", a - sp, t);
        shown++;
    }
    if (!shown)
        host_log("  [bt]   no return addresses found on the stack\n");
}

/* ---- merge sort by an unsigned 32-bit key --------------------------------- */

static void merge_u32(uint8_t *base, uint8_t *tmp, size_t n, size_t esz,
                      size_t keyoff)
{
    if (n < 2)
        return;
    size_t mid = n / 2;
    merge_u32(base, tmp, mid, esz, keyoff);
    merge_u32(base + mid * esz, tmp, n - mid, esz, keyoff);

    uint8_t *a = base, *b = base + mid * esz, *end = base + n * esz;
    uint8_t *o = tmp;
    uint8_t *bend = end;
    while (a < base + mid * esz && b < bend) {
        uint32_t ka = *(uint32_t *)(a + keyoff);
        uint32_t kb = *(uint32_t *)(b + keyoff);
        if (ka <= kb) { memcpy(o, a, esz); a += esz; }
        else          { memcpy(o, b, esz); b += esz; }
        o += esz;
    }
    while (a < base + mid * esz) { memcpy(o, a, esz); a += esz; o += esz; }
    while (b < bend)             { memcpy(o, b, esz); b += esz; o += esz; }
    memcpy(base, tmp, n * esz);
    (void)end;
}

void sort_by_u32(uint8_t *base, size_t n, size_t esz, size_t keyoff)
{
    if (n < 2)
        return;
    uint8_t *tmp = (uint8_t *)host_alloc(n * esz);
    merge_u32(base, tmp, n, esz, keyoff);
    /* tmp is deliberately leaked: this runs once at load time and the
     * allocation is the same order as the symbol table itself. */
}

#ifdef WASM_FREESTANDING
/* --- freestanding replacements (no libc in the wasm32 build yet) ---------- */

void *memcpy(void *d, const void *s, size_t n)
{
    uint8_t *a = (uint8_t *)d; const uint8_t *b = (const uint8_t *)s;
    while (n--) *a++ = *b++;
    return d;
}
void *memset(void *d, int c, size_t n)
{
    uint8_t *a = (uint8_t *)d;
    while (n--) *a++ = (uint8_t)c;
    return d;
}
void *memmove(void *d, const void *s, size_t n)
{
    uint8_t *a = (uint8_t *)d; const uint8_t *b = (const uint8_t *)s;
    if (a < b) { while (n--) *a++ = *b++; }
    else { a += n; b += n; while (n--) *--a = *--b; }
    return d;
}
int memcmp(const void *a, const void *b, size_t n)
{
    const uint8_t *x = (const uint8_t *)a, *y = (const uint8_t *)b;
    while (n--) { if (*x != *y) return *x - *y; x++; y++; }
    return 0;
}
size_t strlen(const char *s) { const char *p = s; while (*p) p++; return (size_t)(p - s); }
int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (int)(uint8_t)*a - (int)(uint8_t)*b;
}
int strncmp(const char *a, const char *b, size_t n)
{
    while (n--) {
        if (*a != *b) return (int)(uint8_t)*a - (int)(uint8_t)*b;
        if (!*a) break;
        a++; b++;
    }
    return 0;
}
char *strchr(const char *s, int c)
{
    for (; *s; s++)
        if (*s == (char)c)
            return (char *)s;
    return (c == 0) ? (char *)s : 0;
}
char *strstr(const char *h, const char *n)
{
    if (!*n)
        return (char *)h;
    for (; *h; h++) {
        const char *a = h, *b = n;
        while (*b && *a == *b) { a++; b++; }
        if (!*b)
            return (char *)h;
    }
    return 0;
}
char *strncpy(char *d, const char *s, size_t n)
{
    size_t i = 0;
    for (; i < n && s[i]; i++) d[i] = s[i];
    for (; i < n; i++) d[i] = 0;
    return d;
}
#endif

#ifdef GUEST_CHECK
/*
 * Bounds-checked guest access.
 *
 * The release build indexes the guest buffer directly -- that is what keeps the
 * interpreter's inner loop tight, and the loader has already validated every
 * segment. The cost is that a bad guest pointer becomes a fault at *host*
 * level: a SIGSEGV on the desktop build (translated back to a guest PC by the
 * crash handler) and an opaque "memory access out of bounds" trap under wasm,
 * with no PC and no trace.
 *
 * Enabling this turns that into an interpreter-level trap that names the
 * address and the instruction, which is the difference between "something
 * dereferenced garbage" and a place to look. It slows the interpreter by
 * roughly half, so it is a debugging build.
 */
static void guest_bad(uint32_t addr, const char *what)
{
    host_log("guest: %s at bad address %08x (pc=%08x, insn=%08x) -- guest "
             "memory is %u MB\n", what, addr, g_cpu.r[15],
             g_fault_insn ? g_fault_insn : (uint32_t)g_cpu.r[15],
             GUEST_MEM_SIZE >> 20);
    /* Stop the interpreter at the instruction that did it, so the trace and
     * the register dump describe the fault and not what followed. */
    extern int g_guest_check_fault;
    g_guest_check_fault = 1;
}

#define GUEST_INRANGE(a, n) ((uint32_t)(a) <= GUEST_MEM_SIZE - (n))

uint32_t guest_ld32(uint32_t a)
{
    if (!GUEST_INRANGE(a, 4)) { guest_bad(a, "load32"); return 0; }
    return *(uint32_t *)(g_mem + a);
}
uint16_t guest_ld16(uint32_t a)
{
    if (!GUEST_INRANGE(a, 2)) { guest_bad(a, "load16"); return 0; }
    return *(uint16_t *)(g_mem + a);
}
uint8_t guest_ld8(uint32_t a)
{
    if (!GUEST_INRANGE(a, 1)) { guest_bad(a, "load8"); return 0; }
    return *(uint8_t *)(g_mem + a);
}
/* TEMP probe (see guest_st32): names the stores that paint the composite
 * buffer. 32-bit stores log individually; 16/8-bit ones aggregate into a
 * tiny per-PC table and flush it when the budget runs out. */
uint32_t g_probe_lo, g_probe_len, g_probe_n, g_probe_on = 1;
static struct { uint32_t pc; int n; } g_probe_pcs[8];

static void probe_note(uint32_t pc)
{
    for (int i = 0; i < 8; i++) {
        if (!g_probe_pcs[i].n) { g_probe_pcs[i].pc = pc; g_probe_pcs[i].n = 1; return; }
        if (g_probe_pcs[i].pc == pc) { g_probe_pcs[i].n++; return; }
    }
}

static void probe_flush(void)
{
    g_probe_on = 0;
    for (int i = 0; i < 8 && g_probe_pcs[i].n; i++)
        host_log("  [str8/16] pc=%08x stores=%d\n", g_probe_pcs[i].pc,
                 g_probe_pcs[i].n);
    memset(g_probe_pcs, 0, sizeof(g_probe_pcs));
}

void guest_st32(uint32_t a, uint32_t v)
{
    if (!GUEST_INRANGE(a, 4)) { guest_bad(a, "store32"); return; }
    if (g_probe_lo && g_probe_on && (uint32_t)(a - g_probe_lo) < g_probe_len) {
        if (g_probe_n < 32) {
            g_probe_n++;
            host_log("  [str32] %08x = %08x pc=%08x lr=%08x\n", a, v,
                     g_cpu.r[15], g_cpu.r[14]);
            if (g_probe_n >= 32) probe_flush();
        }
    }
    *(uint32_t *)(g_mem + a) = v;
}
void guest_st16(uint32_t a, uint16_t v)
{
    if (!GUEST_INRANGE(a, 2)) { guest_bad(a, "store16"); return; }
    if (g_probe_lo && g_probe_on && (uint32_t)(a - g_probe_lo) < g_probe_len) {
        if (g_probe_n < 65536) { g_probe_n++; probe_note(g_cpu.r[15]);
            if (g_probe_n >= 65536) probe_flush(); }
    }
    *(uint16_t *)(g_mem + a) = v;
}
void guest_st8(uint32_t a, uint8_t v)
{
    if (!GUEST_INRANGE(a, 1)) { guest_bad(a, "store8"); return; }
    if (g_probe_lo && g_probe_on && (uint32_t)(a - g_probe_lo) < g_probe_len) {
        if (g_probe_n < 65536) { g_probe_n++; probe_note(g_cpu.r[15]);
            if (g_probe_n >= 65536) probe_flush(); }
    }
    *(uint8_t *)(g_mem + a) = v;
}
#endif
