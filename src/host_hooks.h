/*
 * host_hooks.h -- replace selected *internal* guest functions with host code.
 *
 * The import mechanism (imports.c) covers the 183 symbols libgame.so takes
 * from libc: relocation points them at a BKPT stub in the stub page. But the
 * expensive helpers in this game are not imported -- they are linked into the
 * guest binary from libgcc, and the interpreter runs them one guest
 * instruction at a time:
 *
 *     __divsi3   39.6% of the steady-state frame     <- ARMv5TE has no divide
 *     __divdf3    6.6%
 *     __udivsi3   5.6%
 *
 * ARMv5TE has no divide instruction, so the SDK calls libgcc's software
 * routines for every division the engine performs. Hooking their entry costs
 * one branch through a BKPT and answers with the host's 64-bit divider.
 *
 * How: the loader resolves the symbol, the first halfword of the function is
 * replaced with a Thumb BKPT carrying a hook index, and the interpreter's BKPT
 * decode (which already exists for imports) hands it to host_hook_dispatch().
 * The hook writes the result into r0/r1 and returns through LR, exactly like
 * the import path does.
 *
 * Only Thumb functions are patched (bit 0 of the symbol value); an ARM-state
 * function would decode the halfword as something else entirely.
 *
 * The differential tests run without hooks (the oracle is the plain
 * interpreter), so this is enabled only by the game host and can be turned off
 * with --no-hooks.
 */
#ifndef HOST_HOOKS_H
#define HOST_HOOKS_H

#include <stdint.h>

#include "elf.h"

/* Patch every known hot function present in the image. Returns how many were
 * installed; logs each one. */
int host_hooks_install(const guest_image *img);

/* Called from the interpreter's BKPT decode for a BKPT outside the stub page.
 * Returns 1 if the index names a hook (the hook has already written its
 * results into g_cpu.r). */
int host_hook_dispatch(int idx);

/* Call counts per hook, for the debug build's report at exit. */
void host_hooks_report(void);

#endif
