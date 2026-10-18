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
#include "asset_zip.h"
#include "jni.h"
#include "gl.h"

#ifndef WASM_FREESTANDING
#include <math.h>
#endif

#ifndef WASM_FREESTANDING
/* The file I/O shims below are real stdio. The freestanding wasm build -- the
 * throughput measurement -- has no filesystem and does not need one; the
 * browser and desktop builds both do. */
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/types.h>
#endif

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

/* Written into a freed block's first word. See heap_free(). */
#define HEAP_POISON 0xDEADBEEFu

/* The import currently being serviced. A shim that rejects its arguments has
 * the caller's LR to report, but that names the guest instruction and not the
 * API -- and "free() was called with a bad pointer" reads very differently
 * from "realloc() was". */
const char *g_current_import = "?";

/* The last few host crossings, as (name, arg0, arg1, caller).
 *
 * A rejected pointer is only half a diagnosis: the question is always where it
 * came from. The value was usually handed to the guest by an import a few
 * calls earlier -- a JNI string, a file buffer, an allocation -- and the ring
 * answers that without a debugger, which matters because the browser build has
 * none. */
#define RECENT_MAX 24
static struct {
    const char *name;
    uint32_t a0, a1, lr;
} g_recent[RECENT_MAX];
static int g_recent_n, g_recent_w;

static void recent_dump(void)
{
    int n = g_recent_n < RECENT_MAX ? g_recent_n : RECENT_MAX;
    host_log("  [heap] last %d host calls before this one:\n", n);
    for (int i = 0; i < n; i++) {
        int k = (g_recent_w - n + i + RECENT_MAX * 2) % RECENT_MAX;
        host_log("  [heap]   %-24s a0=%08x a1=%08x caller=%08x\n",
                 g_recent[k].name, g_recent[k].a0, g_recent[k].a1,
                 g_recent[k].lr);
    }
}

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

/* A block header is only meaningful inside the arena. `free` on anything else
 * -- a pointer the guest never got from here, an offset into a struct, a
 * double free that has already been coalesced -- makes `at = ptr - HDR_SZ`
 * land outside the buffer and the first read of `h->size` becomes an opaque
 * wasm trap with no PC. Checking first turns that into a line naming the
 * pointer and the caller, which is the difference between "the heap is
 * corrupt" and "this call site passed garbage". */
static int heap_owns(uint32_t at)
{
    return at >= heap_start && at + HDR_SZ <= heap_break;
}

/* Keep the block after `at` pointing back at it.
 *
 * `prev_size` is what makes backward coalescing O(1), and it is only correct
 * if every write to a size word writes the neighbour's back-pointer too. It
 * was maintained in the split path only, and there from the *flagged* size
 * word, which is one byte too large -- so the neighbour's `prev_size` named an
 * address inside its own payload. A free then walked back into the middle of a
 * live block, read a garbage size, and (before this was guarded) followed it
 * off the end of the arena. */
static void heap_set_next_prev(uint32_t at, uint32_t sz)
{
    uint32_t nxt = at + HDR_SZ + sz;
    if (nxt <= heap_break - HDR_SZ)
        ((blk_hdr *)(g_mem + nxt))->prev_size = sz;
}

static uint32_t heap_alloc(uint32_t want)
{
    heap_init();
    want = ALIGN8(want ? want : 8);

    uint32_t at = heap_start;
    while (heap_owns(at)) {
        blk_hdr *h = (blk_hdr *)(g_mem + at);
        uint32_t sz = h->size & ~BLK_FREE;
        /* A block has to fit between its own header and the break, or the
         * header is not a header and the walk is following garbage. Report it
         * rather than stepping to whatever address the size word names. */
        if (sz > heap_break - at - HDR_SZ) {
            host_log("  [heap] corrupt header at %08x: size %08x (heap "
                     "%08x..%08x, caller %08x)\n", at, h->size, heap_start,
                     heap_break, g_cpu.r[14]);
            break;
        }
        if ((h->size & BLK_FREE) && sz >= want) {
            /* Split when the remainder can hold a header plus a sane payload;
             * otherwise hand over the whole block to avoid a run of unusable
             * slivers. */
            if (sz >= want + HDR_SZ + 16u) {
                uint32_t rest_sz = sz - want - HDR_SZ;
                uint32_t rest = at + HDR_SZ + want;
                blk_hdr *n = (blk_hdr *)(g_mem + rest);
                n->size = rest_sz | BLK_FREE;
                n->prev_size = want;
                h->size = want;
                heap_set_next_prev(rest, rest_sz);
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

static void heap_bad_free(uint32_t ptr, const char *why)
{
    static int warned;
    if (warned >= 20)
        return;
    warned++;
    host_log("  [heap] free(%08x): %s (from %s, caller %08x)\n", ptr, why,
             g_current_import, g_cpu.r[14]);
    /* The first couple also get the history: the question a rejected pointer
     * raises is where it came from, and that is a few host calls back. */
    if (warned <= 2) {
        recent_dump();
        guest_backtrace(why);
    }
}

static void heap_free(uint32_t ptr)
{
    if (!ptr)
        return;
    uint32_t at = ptr - HDR_SZ;
    if (!heap_owns(at)) {
        heap_bad_free(ptr, "not a heap address");
        return;
    }
    blk_hdr *h = (blk_hdr *)(g_mem + at);
    if (h->size & BLK_FREE) {
        /* Freeing twice is harmless to ignore but it does mean the caller's
         * bookkeeping is off, and the coalescing below would merge a block
         * into itself. */
        heap_bad_free(ptr, "already free");
        return;
    }
    h->size |= BLK_FREE;
    /* Clobber the first word of the payload.
     *
     * A freed block is not the caller's memory any more, and leaving it
     * readable makes a use-after-free silently return stale data -- which is
     * worse than crashing, because it looks like a wrong answer from live code.
     * Real allocators do this too: dlmalloc's smallest bins put the free-list
     * link in the first word of the payload, which is why code that reads a
     * freed node's first field usually gets away with it on Android and not
     * here.
     *
     * The engine has exactly such a read: TM_Run removes a dead timer, frees
     * the node, and then re-examines that node's id. Left intact, the id still
     * matches, so it frees the same node again -- forever. */
    *(uint32_t *)(g_mem + ptr) = HEAP_POISON;

    /* Coalesce forward.
     *
     * `nxt` is derived from the header's own size word, so a corrupted size --
     * the guest writing past the end of its block, or a pointer into the
     * middle of one -- puts it anywhere at all. The check has to be written so
     * it still holds when the addition wraps: `nxt + HDR_SZ > heap_break` is
     * false for a `nxt` just under 2^32, and the loop would then dereference
     * it. Same for walking back with `prev_size`. */
    for (;;) {
        uint32_t sz = h->size & ~BLK_FREE;
        if (sz > heap_break - at - HDR_SZ)
            break;
        uint32_t nxt = at + HDR_SZ + sz;
        if (!heap_owns(nxt))
            break;
        blk_hdr *n = (blk_hdr *)(g_mem + nxt);
        if (!(n->size & BLK_FREE))
            break;
        h->size = (sz + HDR_SZ + (n->size & ~BLK_FREE)) | BLK_FREE;
    }
    /* The merge moved this block's end, so the block that follows it now has a
     * stale back-pointer unless it is told. */
    heap_set_next_prev(at, h->size & ~BLK_FREE);

    /* Coalesce backward using prev_size. */
    if (h->prev_size) {
        if (h->prev_size > at - HDR_SZ - heap_start) {
            heap_bad_free(ptr, "prev_size runs off the front of the heap");
            return;
        }
        blk_hdr *p = (blk_hdr *)(g_mem + at - HDR_SZ - h->prev_size);
        if (p->size & BLK_FREE) {
            p->size = ((p->size & ~BLK_FREE) + HDR_SZ +
                       (h->size & ~BLK_FREE)) | BLK_FREE;
            /* Same again, from the other end: `p` now ends where `h` did, so
             * the block after `h` points back at `p`'s new size. */
            heap_set_next_prev(at - HDR_SZ - h->prev_size,
                               p->size & ~BLK_FREE);
        }
    }
}

uint32_t guest_heap_alloc(uint32_t n) { return heap_alloc(n); }

/* ========================================================================== */
/* setjmp / longjmp                                                            */
/* ========================================================================== */

/*
 * These are imported by the guest, so the guest calls them through the PLT
 * like any bionic function. The interpreter already hands a normal import call
 * back to the caller through R[14], and that is all longjmp needs: rewrite the
 * callee-saved registers, SP and R[14], and the existing return path resumes
 * at the setjmp call site with R[0] holding the value.
 *
 * What it pins down: the C library's setjmp/longjmp are the only way libpng
 * can unwind out of png_error(). With them stubbed to "return 0" the error
 * handler returned into png_error(), which called longjmp() again, forever --
 * three billion instructions inside a 300-byte function. The saved contexts
 * live on the host, not in the guest's jmp_buf: the guest only ever passes the
 * buffer's address around, and bionic's private layout is not something this
 * port needs to reproduce.
 */
#define JMP_MAX 64
typedef struct {
    uint32_t buf;
    uint32_t r[8];      /* r4-r11, the AAPCS callee-saved set */
    uint32_t sp;
    uint32_t lr;        /* return address, thumb bit included */
} jmp_ctx;
static jmp_ctx g_jmp[JMP_MAX];
static int     g_jmp_n;

static uint32_t host_setjmp(uint32_t buf)
{
    if (g_jmp_n >= JMP_MAX) {
        /* Losing a frame here would be a silent unwind into the wrong place,
         * so say so instead of overwriting the oldest one. */
        host_fatal("setjmp: %d nested contexts, cannot save another\n", JMP_MAX);
        return 0;
    }
    jmp_ctx *c = &g_jmp[g_jmp_n++];
    c->buf = buf;
    for (int i = 0; i < 8; i++)
        c->r[i] = g_cpu.r[4 + i];
    c->sp = g_cpu.r[13];
    c->lr = g_cpu.r[14];
    return 0;
}

static void host_longjmp(uint32_t buf, uint32_t value)
{
    int i;
    for (i = g_jmp_n - 1; i >= 0; i--)
        if (g_jmp[i].buf == buf)
            break;
    if (i < 0)
        host_fatal("longjmp: no setjmp for buffer %08x\n", buf);
    jmp_ctx *c = &g_jmp[i];
    for (int k = 0; k < 8; k++)
        g_cpu.r[4 + k] = c->r[k];
    g_cpu.r[13] = c->sp;
    /* The return path in cpu.c takes the next PC from R[14], so restoring it
     * is what makes longjmp land back at the setjmp call site. */
    g_cpu.r[14] = c->lr;
    g_cpu.r[0] = value ? value : 1;
    /* The context is consumed: everything newer than it was abandoned by the
     * unwind. */
    g_jmp_n = i;
}

static uint32_t heap_usable(uint32_t ptr)
{
    if (!ptr || !heap_owns(ptr - HDR_SZ))
        return 0;
    blk_hdr *h = (blk_hdr *)(g_mem + ptr - HDR_SZ);
    return h->size & ~BLK_FREE;
}

/* Implemented below; declared here because the dispatcher calls it long before
 * its definition. See the comment there. */
void host_file_get_full_path_name(uint32_t name_ptr, uint32_t out_ptr,
                                  uint32_t out_size, uint32_t mode);
void host_parse_offset_len_path(uint32_t out_ptr, uint32_t buf_ptr, uint32_t len);

/* ========================================================================== */
/* Guest pointer validation                                                    */
/* ========================================================================== */

/* Every shim that takes a guest pointer checks it first.
 *
 * The interpreter's own accesses are checked in a GUEST_CHECK build (see
 * util.c), but the shims are ordinary C reading `g_mem + addr` directly -- and
 * a bad pointer there walks off the end of linear memory, which under wasm is
 * an opaque "memory access out of bounds" with no PC and no caller. `strlen`
 * is the worst of them: one bad pointer and it scans until it happens to find
 * a zero byte or leaves the buffer.
 *
 * Failing the call and naming it turns that into a location. */
static int guest_ok(uint32_t addr, uint32_t len, const char *what)
{
    if (!addr)
        return 0;
    if (len > GUEST_MEM_SIZE || addr > GUEST_MEM_SIZE - len) {
        static int warned;
        if (warned < 20) {
            warned++;
            host_log("  [shim] %s: bad guest range %08x+%u (caller %08x)\n",
                     what, addr, len, g_cpu.r[14]);
        }
        return 0;
    }
    return 1;
}

/* For the C-string shims, whose length is unknown up front: the bytes are
 * scanned here so a missing terminator cannot run past the buffer. */
static int guest_str_ok(uint32_t addr, uint32_t max, const char *what)
{
    if (!guest_ok(addr, 1, what))
        return 0;
    uint32_t lim = GUEST_MEM_SIZE - addr;
    if (lim > max) lim = max;
    for (uint32_t i = 0; i < lim; i++)
        if (!g_mem[addr + i])
            return 1;
    host_log("  [shim] %s: unterminated string at %08x (caller %08x)\n",
             what, addr, g_cpu.r[14]);
    return 0;
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
    if (!addr || addr >= GUEST_MEM_SIZE) {
        out[0] = 0;
        return addr ? "(bad ptr)" : "(null)";
    }

    /* The scan is bounded by the *buffer*, not by the caller's `max`: an
     * address near the top of the guest space plus an offset would otherwise
     * read past the end of linear memory. A host fault outside the guest space
     * is what that looks like, several frames away from the call that caused
     * it -- `strncmp` alone is 700 crossings a run and every one of them comes
     * through here. */
    size_t lim = GUEST_MEM_SIZE - addr;
    if (lim > max)
        lim = max;
    if (lim > SCRATCH_SZ - 1)
        lim = SCRATCH_SZ - 1;

    size_t i = 0;
    for (; i + 1 < lim; i++) {
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
/* File I/O                                                                    */
/* ========================================================================== */
#ifndef WASM_FREESTANDING

/* The guest's FILE* is a 32-bit value it treats as opaque, so it gets a small
 * handle table rather than a host pointer. The host side is real stdio: under
 * emscripten the engine's files are preloaded into MEMFS, so fopen finds them
 * as ordinary files, and on the desktop build they are ordinary files.
 *
 * This matters more than it looks. Without it the engine boots, runs its state
 * machine and draws -- with nothing to draw, because every asset it asks for
 * comes back NULL. A black screen is the *correct* output of a working engine
 * with no data, which is exactly why it has to be told apart from a broken
 * renderer before chasing the renderer.
 */
#define FIOMAX 64
static FILE *g_files[FIOMAX];
static char  g_file_path[FIOMAX][256];
static char  g_path_buf[512];

/* Create every missing directory on the way to `path`'s parent.
 *
 * The engine composes its save file name as "<MD5(HubSave key)>/saveN.dat" --
 * always a subdirectory -- and the only code that would ever create that
 * subdirectory is HubSave_MakeFolder, which runs only after
 * HubSave_LoadKey() succeeds, i.e. after the Hub/social login. An offline port
 * has no such login (GetAppProperty and GetUDID are not registered, so the key
 * stays empty), so the directory is never made, CS_fsOpen's open(O_CREAT) on
 * "<dir>/saveN.dat" fails with ENOENT, and the game reports "Save failed".
 * The parent port fixed this the same way (its patch.c, Corrida 62) -- this is
 * that fix in the one place every write in this .so goes through.
 *
 * Idempotent, and only ever called for a creating open; a directory that
 * already exists, or a parent that is read-only, just leaves mkdir's error
 * unread. */
static void ensure_parent_dirs(const char *path)
{
    char tmp[512];
    size_t n = strlen(path);
    if (n == 0 || n >= sizeof(tmp))
        return;
    memcpy(tmp, path, n + 1);
    for (size_t i = 1; i < n; i++) {
        if (tmp[i] != '/')
            continue;
        tmp[i] = 0;
        if (mkdir(tmp, 0777) == 0) {
            static int said;
            if (said++ < 8)
                host_log("  [fs] created directory %s\n", tmp);
        }
        tmp[i] = '/';
    }
}

static void guest_path(uint32_t addr, char *out, size_t cap)
{
    size_t i = 0;
    out[0] = 0;
    if (!addr || addr >= GUEST_MEM_SIZE) return;
    if (cap > GUEST_MEM_SIZE - addr) cap = GUEST_MEM_SIZE - addr;
    for (; i + 1 < cap; i++) {
        char c = (char)g_mem[addr + i];
        out[i] = c;
        if (!c) return;
    }
    out[cap - 1] = 0;
}

static uint32_t file_handle(FILE *f, const char *path)
{
    for (int i = 1; i < FIOMAX; i++) {
        if (!g_files[i]) {
            g_files[i] = f;
            size_t n = strlen(path);
            if (n > 255) n = 255;
            memcpy(g_file_path[i], path, n);
            g_file_path[i][n] = 0;
            return (uint32_t)i;
        }
    }
    return 0;
}

static FILE *file_of(uint32_t h)
{
    return (h > 0 && h < FIOMAX) ? g_files[h] : NULL;
}

/* Log the first few opens so the worklist is the engine's own, not a guess --
 * the same principle as the import and Java-method reports. */
static void note_open(const char *path, int ok)
{
    static int logged;
    if (logged < 400) {
        logged++;
        host_log("  [io] open %-52s %s\n", path, ok ? "ok" : "FAILED");
    }
}

#endif /* !WASM_FREESTANDING */

/* ========================================================================== */
/* Guest-write watchpoint                                                      */
/* ========================================================================== */

/* Set to a guest address to log every host-side write that covers it.
 *
 * This exists because the two sides of the boundary fail very differently. A
 * guest store that goes out of bounds is visible in the instruction trace; a
 * *host* store is invisible, because the shim is C running on the host and the
 * interpreter never sees it. When guest memory is corrupted and no guest
 * instruction did it, this is how to find the culprit.
 *
 * Enabled by --watch=ADDR; off (and free) otherwise. */
uint32_t g_watch_addr;

static void watch_check_fmt(const char *what, uint32_t dst, uint32_t len,
                            const char *fmt)
{
    if (!g_watch_addr || !len)
        return;
    if (dst <= g_watch_addr && g_watch_addr < dst + len)
        host_log("  WATCH: %s() wrote %u bytes to %08x..%08x covering %08x"
                 "  (return address %08x)\n    format: \"%s\"\n",
                 what, len, dst, dst + len, g_watch_addr, g_cpu.r[14],
                 fmt ? fmt : "?");
}

static void watch_check(const char *what, uint32_t dst, uint32_t len)
{
    /* TEMP: bounded while diagnosing the white composite; the unbounded
     * variant floods every per-frame row copy. */
    static int watched;
    if (!g_watch_addr || !len || watched >= 64)
        return;
    if (dst <= g_watch_addr && g_watch_addr < dst + len) {
        watched++;
        host_log("  WATCH: %s() wrote %u bytes to %08x..%08x covering %08x"
                 "  (return address %08x)\n",
                 what, len, dst, dst + len, g_watch_addr, g_cpu.r[14]);
    }
}

/* ========================================================================== */
/* The dispatch                                                               */
/* ========================================================================== */

/* Arguments past the fourth live in `args[]` (registers then guest stack). */
#define extra_arg(n) ((n) < nargs ? args[n] : 0)

void host_import_call(guest_import *im)
{
    uint32_t *R = g_cpu.r;
    g_current_import = im->name;
    g_recent[g_recent_w].name = im->name;
    g_recent[g_recent_w].a0 = R[0];
    g_recent[g_recent_w].a1 = R[1];
    g_recent[g_recent_w].lr = R[14];
    g_recent_w = (g_recent_w + 1) % RECENT_MAX;
    if (g_recent_n < RECENT_MAX)
        g_recent_n++;
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

    /* Name every crossing, so a host fault inside one has a location. The same
     * reason the browser build traces GL entry points by name: a SIGSEGV in a
     * shim reports the guest PC of the *call*, not which import was running. */
    {
        static int n;
        if (n < 4000) { n++; host_log("  [imp] %s\n", im->name); }
    }

    /* The one guest function the host replaces outright. See patches.c. */
    if (strcmp(im->name, "host/File::GetFullPathName") == 0) {
        host_file_get_full_path_name(a0, a1, a2, extra_arg(4));
        return;
    }
    if (strcmp(im->name, "host/ParseOffsetAndLengthAndPath") == 0) {
        host_parse_offset_len_path(a0, a1, a2);
        return;
    }

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

    /* GL is recognised by name rather than by an IMPL_* id: there are ~130
     * entry points, most of them one line of state, and a name table keeps them
     * in one readable place instead of a 130-case switch. gl_is_entry is the
     * single source of truth for "is this GL", so a name cannot be dispatched
     * without also being counted. */
    if (gl_is_entry(im->name)) {
        gl_note_call(im->name);
        gl_host_call(im->name, args, nargs, &R[0]);
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
    case IMPL_memcpy:  watch_check("memcpy", a0, a2);
        /* TEMP probe: the per-frame composite is ~294 memcpy crossings per
         * active frame, and the uploaded 384x256 surface comes out all-white.
         * Trace the big ones to learn what the composite reads. */
        static int cpy_traced;
        if (cpy_traced < 60 && a2 >= 512)
            cpy_traced++,
            host_log("  [cpy] %08x <- %08x n=%u caller=%08x\n", a0, a1, a2, g_cpu.r[14]);
        if (guest_ok(a0, a2, "memcpy dst") && guest_ok(a1, a2, "memcpy src"))
            memcpy(g_mem + a0, g_mem + a1, a2);
        R[0] = a0; break;
    case IMPL_memset:  watch_check("memset", a0, a2);
        if (guest_ok(a0, a2, "memset")) memset(g_mem + a0, (int)a1, a2);
        R[0] = a0; break;
    case IMPL_memmove: watch_check("memmove", a0, a2);
        if (guest_ok(a0, a2, "memmove dst") && guest_ok(a1, a2, "memmove src"))
            memmove(g_mem + a0, g_mem + a1, a2);
        R[0] = a0; break;
    case IMPL_memcmp:  R[0] = (guest_ok(a0, a2, "memcmp a") && guest_ok(a1, a2, "memcmp b"))
            ? (uint32_t)(int32_t)memcmp(g_mem + a0, g_mem + a1, a2) : 0; break;
    case IMPL_strlen:  R[0] = guest_str_ok(a0, GUEST_MEM_SIZE - a0, "strlen")
            ? (uint32_t)strlen((const char *)(g_mem + a0)) : 0; break;
    case IMPL_strcmp:  R[0] = (uint32_t)(int32_t)strcmp(guest_str(a0, 4096), guest_str(a1, 4096)); break;
    case IMPL_strncmp: {
        /* Stopping at the first difference or NUL is strncmp's whole contract,
         * and it is also what keeps this in bounds: memcmp'ing a caller-supplied
         * n over two 8 KB scratch buffers reads straight past them for any n
         * above 8192. The engine passes sizes like 0x0a0f6a here. */
        const char *x = guest_str(a0, a2 + 1);
        const char *y = guest_str(a1, a2 + 1);
        size_t n = a2, i = 0;
        R[0] = 0;
        for (; i < n; i++) {
            if (x[i] != y[i]) { R[0] = (uint32_t)(int32_t)((uint8_t)x[i] - (uint8_t)y[i]); break; }
            if (!x[i]) break;
        }
        break;
    }
    case IMPL_strcpy: {
        const char *s = guest_str(a1, 4096);
        size_t n = strlen(s) + 1;
        watch_check("strcpy", a0, (uint32_t)n);
        if (a0) memcpy(g_mem + a0, s, n);
        R[0] = a0;
        break;
    }
    case IMPL_strncpy: {
        const char *s = guest_str(a1, a2 + 1);
        size_t n = strlen(s);
        if (n > a2) n = a2;
        watch_check("strncpy", a0, a2);
        if (a0 && guest_ok(a0, a2, "strncpy"))
            { memcpy(g_mem + a0, s, n); memset(g_mem + a0 + n, 0, a2 - n); }
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
        host_log("guest: abort() called\n");
        g_cpu.r[0] = 0;
        /* Do not kill the host: the spike wants the run to continue and report
         * the profile so far, not to abort the whole session. */
        break;
    case IMPL_stack_chk_fail:
        /* A canary mismatch means guest memory was written out of bounds. The
         * two candidates are worth telling apart, because they point at
         * completely different culprits:
         *
         *   the saved copy changed -> something overwrote the guest's stack,
         *                             most often a local buffer whose length
         *                             came from a shim that answered wrongly
         *   the guard itself changed -> our own host-data storage for
         *                             __stack_chk_guard was clobbered, which
         *                             would be a bug in the loader
         *
         * The guest's check is `cmp r2, r3` with r2 the value it saved at entry
         * and r3 the guard read back, so r2 and r3 are exactly those two
         * numbers at this point. */
        {
            uint32_t guard_addr = 0;
            for (int i = 0; i < g_nimports; i++)
                if (g_imports[i].is_object &&
                    strcmp(g_imports[i].name, "__stack_chk_guard") == 0)
                    guard_addr = g_imports[i].stub;
            host_log("guest: __stack_chk_fail -- stack canary mismatch\n"
                     "  saved canary (r2)   : %08x\n"
                     "  guard read  (r3)    : %08x\n"
                     "  __stack_chk_guard @ : %08x = %08x\n"
                     "  return address      : %08x\n"
                     "  stack pointer       : %08x\n",
                     a2, a3, guard_addr,
                     guard_addr ? LD32(guard_addr) : 0, R[14], R[13]);
            if (a2 != a3 && guard_addr && LD32(guard_addr) == a3)
                host_log("  => the guard is intact, so the SAVED copy was "
                         "overwritten: something wrote past a local buffer on "
                         "the guest stack.\n");
            else if (guard_addr && LD32(guard_addr) != a3)
                host_log("  => the guard itself moved: the loader's storage for "
                         "__stack_chk_guard was written by something else.\n");
        }
        g_cpu.r[0] = 0;
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
     *
     * These matter more than "effects maths" suggested: the engine calls them
     * while composing the screen and during loading, and a shim that leaves
     * the result register alone returns whatever was there -- so anything
     * driven by an angle or a power silently reads garbage. Implemented below
     * rather than left as a report.
     *
     * The guest is soft-float, so a double is two words: the low word first,
     * in r0:r1 (and r2:r3 for pow's second argument), with the result going
     * back the same way. */
    case IMPL_sin: case IMPL_cos: case IMPL_pow: {
        union { uint32_t u[2]; double d; } x, y, r;
        x.u[0] = a0; x.u[1] = a1;
        r.d = 0.0;
#ifndef WASM_FREESTANDING
        if (im->impl == IMPL_sin)
            r.d = sin(x.d);
        else if (im->impl == IMPL_cos)
            r.d = cos(x.d);
        else {
            y.u[0] = a2; y.u[1] = a3;
            r.d = pow(x.d, y.d);
        }
#endif
        R[0] = r.u[0];
        R[1] = r.u[1];
        break;
    }
    case IMPL_isinf: {
        union { uint32_t u; float f; } x;
        x.u = a0;
        uint32_t e = (x.u >> 23) & 0xff;
        R[0] = (e == 0xff && !(x.u & 0x7fffff)) ? 1u : 0u;
        break;
    }
    case IMPL_setjmp:  R[0] = host_setjmp(a0); break;
    case IMPL_longjmp: host_longjmp(a0, a1); break;

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
#ifndef WASM_FREESTANDING
    case IMPL_mmap: {
        /* mmap(void *addr, size_t length, int prot, int flags, int fd,
         *      off_t offset)
         *
         * The engine reads its resources through MMappedFile::Open, which mmaps
         * the asset archive -- so this has to actually produce the file's bytes,
         * not scratch memory. A bump allocator that returns zeros makes every
         * resource decode as empty, which surfaces as "not a PNG file" far from
         * the cause.
         *
         * The guest address space is a plain buffer, so "mapping" is: reserve
         * guest memory and read the file into it. There is no unmap to do --
         * munmap already does nothing and the guest never reuses the range. */
        uint32_t len = a1;
        int fd = (int)extra_arg(4);
        long off = (long)(int32_t)extra_arg(5);
        FILE *f = file_of((uint32_t)fd);
        static int logged;
        if (logged < 6) {
            logged++;
            host_log("  [mmap] len=%u fd=%d off=%ld file=%s\n", len, fd, off,
                     f ? "open" : "NO HANDLE");
        }
        uint32_t dst = host_guest_mmap(len ? len : 1);
        if (!dst) { R[0] = (uint32_t)-1; break; }
        if (f && len) {
            memset(g_mem + dst, 0, len);
            if (fseek(f, off, SEEK_SET) == 0)
                fread(g_mem + dst, 1, len, f);
        }
        R[0] = dst;
        break;
    }
    case IMPL_mkdir: {
        /* The engine creates /sdcard/Android/data/<package>/files before using
         * it. One level at a time is all it asks for. */
        guest_path(a0, g_path_buf, sizeof(g_path_buf));
        R[0] = (mkdir(g_path_buf, 0777) == 0) ? 0 : (uint32_t)-1;
        break;
    }
    case IMPL_fstat: {
        /* fstat(int fd, struct stat *buf) -- the same bionic layout as stat.
         * Left unimplemented until now, which meant it returned success and
         * filled nothing: the engine opened the asset archive, read whatever
         * happened to be in that struct, closed the file and moved on without
         * ever reading the asset. A stub that reports success is worse than one
         * that fails, because the caller has no reason to look further. */
        FILE *f = file_of(a0);
        if (!f) { R[0] = (uint32_t)-1; break; }
        long here = ftell(f);
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, here, SEEK_SET);
        if (a1 && guest_ok(a1, 64, "fstat")) {
            memset(g_mem + a1, 0, 64);
            ST32(a1 + 8, (uint32_t)sz);       /* st_size (bionic arm: +8)  */
            ST32(a1 + 16, 0100644);           /* st_mode (bionic arm: +16) */
        }
        R[0] = 0;
        break;
    }
    case IMPL_stat: {
        /* struct stat is bionic's, and the engine only reads st_size and
         * st_mode from it. Writing the whole host struct would be wrong -- the
         * layouts differ -- so only those two fields are filled, at bionic's
         * offsets. */
        guest_path(a0, g_path_buf, sizeof(g_path_buf));
        FILE *f = fopen(g_path_buf, "rb");
        if (!f) { R[0] = (uint32_t)-1; break; }
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fclose(f);
        if (a1) {
            if (!guest_ok(a1, 64, "stat")) { R[0] = (uint32_t)-1; break; }
            memset(g_mem + a1, 0, 64);
            ST32(a1 + 8, (uint32_t)sz);       /* st_size  (bionic arm: +8) */
            ST32(a1 + 16, 0100644);           /* st_mode  (bionic arm: +16) */
        }
        R[0] = 0;
        break;
    }
#endif
    case IMPL_dlopen: R[0] = 0; break;
    case IMPL_dlsym:  R[0] = 0; break;
    case IMPL_dlclose: R[0] = 0; break;
    case IMPL_munmap: R[0] = 0; break;

    /* ---- file I/O ---- */
#ifndef WASM_FREESTANDING
    case IMPL_fopen: {
        guest_path(a0, g_path_buf, sizeof(g_path_buf));
        const char *mode = a1 ? (const char *)g_mem + a1 : "rb";
        FILE *f = fopen(g_path_buf, mode);
        note_open(g_path_buf, f != NULL);
        R[0] = f ? file_handle(f, g_path_buf) : 0;
        break;
    }
    case IMPL_fclose: {
        FILE *f = file_of(a0);
        if (f) { fclose(f); g_files[a0] = NULL; }
        R[0] = 0;
        break;
    }
    case IMPL_fread: {
        /* fread(void *ptr, size_t size, size_t nmemb, FILE *stream) */
        FILE *f = file_of(a3);
        static int n;
        if (n < 10) {
            n++;
            host_log("  [io] fread ptr=%08x size=%u nmemb=%u -> %s\n",
                     a0, a1, a2, f ? "ok" : "BAD HANDLE");
        }
        R[0] = (f && a0) ? (uint32_t)fread(g_mem + a0, a1, a2, f) : 0;
        break;
    }
    case IMPL_fwrite: {
        FILE *f = file_of(a3);
        R[0] = (f && a0) ? (uint32_t)fwrite(g_mem + a0, a1, a2, f) : 0;
        break;
    }
    case IMPL_fseek: {
        FILE *f = file_of(a0);
        R[0] = f ? (uint32_t)fseek(f, (long)(int32_t)a1, (int)a2) : (uint32_t)-1;
        break;
    }
    case IMPL_open: {
        guest_path(a0, g_path_buf, sizeof(g_path_buf));
        /* Honour the flags. This used to open "rb" unconditionally, which was
         * invisible while the port only ever read: the first thing that writes
         * is GAMELOADER_ProcessInstall, and with every create turned into a
         * read-only open it could not produce a single file. The install then
         * reports "File installation error (0)", INITSTATE_nState goes to 3 -- a
         * terminal state with no exit -- and the game sits on the loading screen
         * forever.
         *
         * O_* values are the ARM Linux ones the guest was compiled against. */
        uint32_t flags = a1;
        int wr  = (flags & 3u) != 0;        /* O_WRONLY / O_RDWR */
        int app = (flags & 0x400u) != 0;    /* O_APPEND */
        int cre = (flags & 0x40u) != 0;     /* O_CREAT  */
        int tr  = (flags & 0x200u) != 0;    /* O_TRUNC  */
        /* A creating open must not fail just because its parent directory was
         * never made -- that is exactly the "Save failed" bug. See
         * ensure_parent_dirs(). */
        if (cre)
            ensure_parent_dirs(g_path_buf);
        const char *mode;
        if (app)              mode = wr ? "ab" : "ab";
        else if (cre && wr)   mode = (tr || (flags & 0x80u)) ? "wb" : "r+b";
        else if (tr)          mode = "wb";
        else if (wr)          mode = "r+b";
        else                  mode = "rb";
        FILE *f = fopen(g_path_buf, mode);
        /* A create whose "r+b" did not find an existing file still has to
         * create one; fopen cannot express "create if missing, else update". */
        if (!f && cre) {
            f = fopen(g_path_buf, "wb");
            if (f) { fclose(f); f = fopen(g_path_buf, "r+b"); }
        }
        note_open(g_path_buf, f != NULL);
        /* Returns 0 on failure, not -1. That is wrong for open(2) -- 0 is a
         * valid descriptor -- but it is what this port did before there was a
         * file layer at all, and the engine's boot depends on it: with a
         * correct -1 the engine takes its "file missing" branch and dies
         * calling through a data address. Left as 0 deliberately, and flagged,
         * until that branch is understood. */
        R[0] = f ? file_handle(f, g_path_buf) : 0;
        break;
    }
    case IMPL_close: {
        FILE *f = file_of(a0);
        if (f) { fclose(f); g_files[a0] = NULL; }
        R[0] = 0;
        break;
    }
    case IMPL_read: {
        FILE *f = file_of(a0);
        static int n;
        if (n < 10) {
            n++;
            host_log("  [io] read fd=%u buf=%08x len=%u -> %s\n", a0, a1, a2,
                     f ? "ok" : "BAD FD");
        }
        R[0] = (f && a1) ? (uint32_t)fread(g_mem + a1, 1, a2, f) : (uint32_t)-1;
        break;
    }
    case IMPL_write: {
        FILE *f = file_of(a0);
        R[0] = (f && a1) ? (uint32_t)fwrite(g_mem + a1, 1, a2, f) : (uint32_t)-1;
        break;
    }
    case IMPL_lseek: {
        FILE *f = file_of(a0);
        if (!f) { R[0] = (uint32_t)-1; break; }
        int whence = (a2 == 0) ? SEEK_SET : (a2 == 1) ? SEEK_CUR : SEEK_END;
        if (fseek(f, (long)(int32_t)a1, whence) != 0) { R[0] = (uint32_t)-1; break; }
        R[0] = (uint32_t)ftell(f);
        break;
    }
#endif

    /* ---- formatting ---- */
    case IMPL_snprintf:
    case IMPL_sprintf: {
        /* The two are not the same shape. snprintf takes a size between the
         * buffer and the format -- snprintf(buf, size, fmt, ...) -- while
         * sprintf has no size: sprintf(buf, fmt, ...). Reading the format
         * from a1 for both fed snprintf's *size* (a small integer) to
         * guest_str() as if it were a pointer, so every snprintf came back
         * empty; then the path builders that glue those fragments together
         * produced names like "/sdcard/Android/data/%s/files/%s" with the
         * conversions never applied. */
        int is_sn = (im->impl == IMPL_snprintf);
        const char *fmt = guest_str(is_sn ? a2 : a1, 4096);
        int n = count_args(fmt);
        sink s;
        s.dst = (char *)(g_mem + a0);
        s.cap = is_sn ? a2 : 1u << 30;
        s.n = 0;
        format(&s, fmt, &args[is_sn ? 3 : 2], n + 4);
        sink_ch(&s, 0);
        watch_check(im->name, a0, (uint32_t)s.n);
        R[0] = (uint32_t)s.n - 1;
        break;
    }
    case IMPL_vsnprintf:
    case IMPL_vsprintf: {
        /* Same split as snprintf/sprintf above: vsnprintf carries a size,
         * vsprintf does not. */
        int is_vsn = (im->impl == IMPL_vsnprintf);
        const char *fmt = guest_str(is_vsn ? a2 : a1, 4096);
        int n = count_args(fmt);
        sink s;
        s.dst = (char *)(g_mem + a0);
        s.cap = is_vsn ? a1 : 1u << 30;
        s.n = 0;
        (void)n;
        uint32_t ap = is_vsn ? a3 : a2;

        /* A va_list here is a plain pointer to the argument list in the
         * caller's frame -- NOT the `struct { void *__ap; }` the AAPCS
         * describes, and emphatically not the flat r0-r3-then-stack array the
         * other formatting entry points take.
         *
         * Passing &args[2] made the va_list's own *pointer* the first
         * argument, shifting every conversion by one. `%s` then read a string
         * from whatever integer happened to be there and copied it until it
         * hit a NUL, which is how a call meant to produce "1.3.2" wrote 14
         * bytes over a 12-byte stack buffer and tripped the guest's stack
         * canary -- thousands of instructions after the actual mistake.
         *
         * Settled by reading the guest's own frames: for
         * `"Android Wrapper v%s #%d"`, [va_list] held a pointer to a valid
         * string, which is only true if the arguments start *at* va_list. */
        uint32_t va_args[24];
        for (int i = 0; i < 24; i++)
            va_args[i] = LD32(ap + (uint32_t)i * 4u);

        format(&s, fmt, va_args, 24);
        sink_ch(&s, 0);
        watch_check_fmt(im->name, a0, (uint32_t)s.n, fmt);
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
        /* The stream is argument 0 and the format is argument 1, so the
         * conversions begin at argument 2. Starting at &args[1] fed the
         * format pointer itself to the first `%…`, so a diagnostic like
         * "libpng error: %s" printed the literal "%s" instead of the message. */
        format(&s, fmt, &args[2], n + 4);
        break;
    }

    default:
        /* Called, but not provided. import_dispatch() has already counted it;
         * the report lists these as the worklist. Returning 0 keeps the guest
         * running so one run yields as many distinct gaps as possible. */
        break;
    }
}

#ifndef WASM_FREESTANDING
/* Offset and stored length of a logical asset name inside the archive. The
 * engine names resources without the ".jpg" the archive stores them under
 * ("MainMenu.png" -> "assets/common/game_res/MainMenu.png.jpg"), so both are
 * tried. Returns 0 on success. The archive's location is host_apk_path() in
 * asset_zip.c -- one place decides it, so the path handed back to the engine
 * in the "@offset|length|path" form names the same file the lookup opened. */
int asset_lookup(const char *name, uint32_t *out_offset, uint32_t *out_length)
{
    FILE *f = fopen(host_apk_path(), "rb");
    if (!f)
        return 1;
    char entry[256];
    int r;
    snprintf(entry, sizeof(entry), "assets/common/game_res/%s.jpg", name);
    r = asset_zip_find(f, entry, out_offset, out_length);
    if (r) {
        snprintf(entry, sizeof(entry), "assets/common/game_res/%s", name);
        r = asset_zip_find(f, entry, out_offset, out_length);
    }
    fclose(f);
    return r;
}

/*
 * ParseOffsetAndLengthAndPath(int *out, char *buf, int len) -- the host's
 * replacement.
 *
 * File::Open calls this on a resolved "@<offset>|<length>|<path>" path. Its
 * job is to read the two numbers and rewrite `buf` in place to hold only the
 * path, which File::Open then open()s and mmaps at <offset> for <length>
 * bytes. The guest implementation does exactly that on Android -- and this
 * port needs it to, because those offset/length fields are the only reason
 * the path is wrapped in the "@" form at all.
 *
 * The guest's own copy in this build does not: given
 * "@6490388|148229|assets.apk" it leaves "@assets.ap" (verified from the open
 * call), so the engine opens the wrong name and the mmap has no file. Since
 * the guest function is exercised on every resource load and the transform is
 * three lines, the host owns it here, the same way File::GetFullPathName
 * above is owned.
 *
 * The log is bounded like every other diagnostic: this runs once per resource.
 */
void host_parse_offset_len_path(uint32_t out_ptr, uint32_t buf_ptr, uint32_t len)
{
    (void)len;
    long off = 0, ln = 0;
    char path[512];
    path[0] = 0;

    if (buf_ptr && buf_ptr < GUEST_MEM_SIZE) {
        const char *s = (const char *)(g_mem + buf_ptr);
        if (s[0] == '@') {
            char *end;
            off = strtol(s + 1, &end, 10);
            if (*end == '|') {
                ln = strtol(end + 1, &end, 10);
                if (*end == '|') {
                    size_t n = strlen(end + 1);
                    if (n >= sizeof(path)) n = sizeof(path) - 1;
                    memcpy(path, end + 1, n);
                    path[n] = 0;
                }
            }
        }
    }

    static int logged;
    if (logged < 64) {
        logged++;
        host_log("  [parse] @%ld|%ld -> open \"%s\"\n", off, ln, path);
    }

    if (out_ptr) { ST32(out_ptr, (uint32_t)off); ST32(out_ptr + 4, (uint32_t)ln); }
    if (buf_ptr && path[0]) {
        size_t n = strlen(path);
        memcpy(g_mem + buf_ptr, path, n + 1);
    }
    g_cpu.r[0] = path[0] ? 1 : 0;
}

/*
 * File::GetFullPathName(const char *name, char *out, unsigned out_size,
 *                       int mode) -- the host's replacement.
 *
 * The engine resolves every path through here, and for assets it has to
 * produce the "@offset|length|path" form: that is the only thing that makes
 * File::Open write its own offset and length fields, and those fields are what
 * the mmap that follows uses. A bare path leaves them holding whatever was in
 * that stack slot from an earlier call, which is a truncated read and a CRC
 * failure far from the cause.
 *
 * The modes are the parent repository's (source/patch.c, Corrida 34):
 *   0  user dir      <userdir><package>/<name>       saves and profile data
 *   2  apk           @<offset>|<length>|<archive>    assets
 *   3  external      /sdcard/Android/data/<pkg>/files/<name>
 *
 * Mode 2 does the archive lookup directly rather than calling the engine's own
 * CS_fsGetFileDescriptor: that is a guest function, and calling back into the
 * interpreter from inside an import handler is re-entrancy this port has
 * deliberately avoided. The lookup is the same work GetFileDescriptor does, so
 * the answer is identical.
 */
void host_file_get_full_path_name(uint32_t name_ptr, uint32_t out_ptr,
                                  uint32_t out_size, uint32_t mode)
{
    char name[512];
    guest_path(name_ptr, name, sizeof(name));
    if (!out_ptr || out_size == 0 || out_size > 4096) {
        if (out_ptr)
            ST8(out_ptr, 0);
        return;
    }

    char buf[1024];
    buf[0] = 0;

    const char *apk = host_apk_path();

    if (name[0] == '/' || strchr(name, ':') != NULL ||
        (apk && apk[0] && strcmp(name, apk) == 0)) {
        /* Already resolved -- one of our own stubs' paths, or the archive
         * path the preceding ParseOffsetAndLengthAndPath left in the buffer
         * for MMappedFile::Open to re-resolve. That second case is the one
         * that bites: the resolved archive path is relative and has no ':',
         * so without this test it falls through to the mode branches, which
         * rebuild a path (<userdir>/game/assets.apk) that does not exist, and
         * the mmap that follows has no file behind it -- libpng then decodes
         * a buffer that was never filled and every texture comes out blank.
         * It still has to carry the "@" form, or File::Open will not fill in
         * its offset and length. The parent repository's Corrida 34 is this
         * fix. */
        FILE *f = fopen(name, "rb");
        long len = 0;
        if (f) { fseek(f, 0, SEEK_END); len = ftell(f); fclose(f); }
        snprintf(buf, sizeof(buf), "@0|%ld|%s", len, name);
    } else if (mode == 2) {
        uint32_t off = 0, len = 0;
        if (asset_lookup(name, &off, &len) == 0)
            snprintf(buf, sizeof(buf), "@%u|%u|%s", off, len, apk ? apk : "");
        else
            snprintf(buf, sizeof(buf), "@0|0|%s", apk ? apk : "");
    } else if (mode == 3) {
        snprintf(buf, sizeof(buf),
                 "/sdcard/Android/data/%s/files/%s",
                 "com.com2us.inotia3.google.global", name);
    } else if (mode == 0 || mode == 4) {
        /* Mode 4 is the install target: RES_MoveToFileEx opens the
         * destination with FILE_Open(name, 4, 1) before writing it, and
         * GAMELOADER_Install then reopens the same name with mode 8. Both are
         * "somewhere under the app's writable directory", the same place mode
         * 0 means -- built from the host's own user dir rather than a
         * hardcoded prefix, because the engine concatenates the package name
         * onto that directly and the separator has to come from somewhere. */
        snprintf(buf, sizeof(buf), "%s%s/%s",
                 host_app_data_dir("com.com2us.inotia3.google.global"),
                 "com.com2us.inotia3.google.global", name);
    }

    /* If whatever that produced does not exist, the engine is asking for a
     * shipped asset whatever mode it used to ask.
     *
     * The mode numbering is not taken on faith: the parent repository's build
     * resolves assets through mode 2, and this build asks for "MainMenu.png"
     * in mode 0 -- the same APK version difference that moved every other
     * offset in this port. Rather than encode an assumption about which mode
     * means "asset", the fallback is the archive lookup itself, which is
     * correct in every mode and costs one stat.
     *
     * It has to produce the "@" form regardless of which branch got here: that
     * is the only thing that makes File::Open write its offset and length
     * fields, and those are what the mmap that follows reads. And the path in
     * that form is what File::Open then open()s, so it has to name a file the
     * host can actually find -- the resolved archive path, not a bare
     * "assets.apk" that only exists when the working directory happens to be
     * the game directory. */
    /* Write intent must not be redirected into the archive. Modes 4 and 8 are
     * how the engine installs its data files; falling back to an archive
     * offset for those hands File::Open a read-only path, the create fails,
     * and GAMELOADER_ProcessInstall reports "File installation error" -- which
     * parks the game on the loading screen forever. */
    int write_intent = (mode == 4 || mode == 8);
    if (!write_intent && !strchr(buf, '@')) {
        FILE *probe = fopen(buf, "rb");
        if (probe) {
            fclose(probe);
        } else {
            uint32_t off = 0, len = 0;
            if (apk && asset_lookup(name, &off, &len) == 0)
                snprintf(buf, sizeof(buf), "@%u|%u|%s", off, len, apk);
        }
    }

    static int logged;
    if (logged < 200) {
        logged++;
        host_log("  [path] mode=%u %-34s -> %s\n", mode, name, buf);
    }

    size_t n = strlen(buf);
    if (n + 1 > out_size) {
        ST8(out_ptr, 0);
        return;
    }
    memcpy(g_mem + out_ptr, buf, n + 1);
    g_cpu.r[0] = 1;   /* the real function returns nonzero on success */
}
#else /* WASM_FREESTANDING */

/* The throughput build has no filesystem, and does not want one: it runs the
 * same instruction stream as the other hosts with every path resolution
 * failing, which is what a device with no storage looks like. These two still
 * have to exist, though -- host_import_call() (compiled in both builds) calls
 * them, so without stubs they leave the link as `env.*` imports (the
 * freestanding link passes --allow-undefined), and the only sign of that is
 * `Import #0 "env": module is not an object or function` at instantiation,
 * with nothing naming the symbol. */
void host_parse_offset_len_path(uint32_t out_ptr, uint32_t buf_ptr, uint32_t len)
{
    (void) out_ptr; (void) buf_ptr; (void) len;
}

void host_file_get_full_path_name(uint32_t name_ptr, uint32_t out_ptr,
                                 uint32_t out_size, uint32_t mode)
{
    (void) name_ptr; (void) mode;
    if (out_size > 0)
        ST8(out_ptr, 0);
    g_cpu.r[0] = 0;
}

#endif /* WASM_FREESTANDING */
