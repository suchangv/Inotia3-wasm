/*
 * platform_native.c -- host services for the desktop build.
 *
 * The desktop build exists to make iteration cheap: it runs the same
 * interpreter, loader and import table as the wasm32 build, but under gdb with
 * a real filesystem and a real clock. The wasm build supplies counterparts in
 * platform_wasm.c.
 */

/* clock_gettime and friends are POSIX, not ISO C; ask for them explicitly
 * rather than relying on a particular libc's default feature set. Must precede
 * every include, including the project headers, since those pull in stdint.h
 * and thereby the libc feature-test machinery. */
#define _POSIX_C_SOURCE 200809L

#include "guest.h"
#include "compat.h"
#include "platform.h"
#include "cpu.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <time.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>

/* ---- crash reporting -----------------------------------------------------
 *
 * The guest address space is one flat buffer with deliberately unchecked
 * accesses (see guest.h): that is what keeps the interpreter's inner loop
 * tight, and it is the right trade for a static binary whose loader has
 * already validated every segment. The cost is that a bad guest pointer
 * becomes a host SIGSEGV, which is useless on its own -- the faulting address
 * is a host pointer, not a guest one.
 *
 * So catch it and translate: report the *guest* PC and registers, which is
 * what actually identifies the bug. Without this, every bring-up crash is a
 * manual bisect.
 */
static void segv_handler(int sig, siginfo_t *info, void *uctx)
{
    (void)sig;
    uintptr_t bad = (uintptr_t)info->si_addr;
    intptr_t off = (intptr_t)(bad - (uintptr_t)g_mem);
    (void)uctx;

    fprintf(stderr,
            "\n*** SIGSEGV ***\n"
            "  host fault address : %p\n"
            "  guest address space: %p .. %p\n",
            (void *)bad, (void *)g_mem, (void *)(g_mem + GUEST_MEM_SIZE));

    if (off >= 0 && off < (intptr_t)GUEST_MEM_SIZE)
        fprintf(stderr, "  => guest address %08lx (INSIDE the space)\n",
                (unsigned long)off);
    else
        fprintf(stderr, "  => %+ld bytes OUTSIDE the guest space\n", (long)off);

    fprintf(stderr, "  guest pc           : %08x\n", g_cpu.r[15]);
    fprintf(stderr, "  instructions run   : %llu\n",
            (unsigned long long)g_insn_count);
    for (int i = 0; i < 16; i += 4)
        fprintf(stderr, "  r%-2d=%08x r%-2d=%08x r%-2d=%08x r%-2d=%08x\n",
                i, g_cpu.r[i], i + 1, g_cpu.r[i + 1],
                i + 2, g_cpu.r[i + 2], i + 3, g_cpu.r[i + 3]);
    cpu_dump_trace();
    _exit(139);
}

static void install_segv_handler(void)
{
    static struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = segv_handler;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
}

/* Called from main() before any guest code runs. */
void host_install_crash_handler(void);
void host_install_crash_handler(void) { install_segv_handler(); }

/* ---- logging -------------------------------------------------------------
 * Everything the loader and interpreter report goes through here. In the
 * browser this becomes console.log; keeping the core free of stdio is what
 * lets the same sources build for wasm32 without emscripten.
 */
void host_log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

void host_fatal(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "fatal: ");
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    exit(1);
}

/* ---- host-side allocation ------------------------------------------------
 * For the loader's own bookkeeping (symbol tables, profile counters). Not the
 * guest heap -- that lives inside the guest address space, see host_libc.c.
 */
void *host_alloc(size_t n)
{
    void *p = calloc(1, n ? n : 1);
    if (!p)
        host_fatal("host_alloc(%zu) failed\n", n);
    return p;
}

double host_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

/* ---- guest mmap ----------------------------------------------------------
 * The guest asks for scratch memory it will never free until exit. A forward
 * bump allocator over the dedicated arena is enough, and its failure mode is
 * loud rather than silent.
 */
static uint32_t g_mmap_next = GUEST_MMAP_BASE;

uint32_t host_guest_mmap(uint32_t size)
{
    if (!size)
        return 0;
    uint32_t a = (g_mmap_next + 0xFFFu) & ~0xFFFu;
    if (a + size > GUEST_MMAP_BASE + GUEST_MMAP_SIZE) {
        host_log("mmap: arena exhausted (%u requested, %08x used)\n",
                 size, g_mmap_next - GUEST_MMAP_BASE);
        return 0;
    }
    g_mmap_next = a + size;
    return a;
}
