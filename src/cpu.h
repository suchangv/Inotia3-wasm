/*
 * cpu.h -- the ARMv5TE / Thumb-1 / VFPv2 interpreter.
 *
 * Scope is dictated by the real binary, not by the ARM ARM: libgame.so is
 * Tag_CPU_arch v5TE, Tag_THUMB_ISA_use Thumb-1, Tag_FP_arch VFPv2, 96.0% of
 * its code bytes are Thumb. So there is no Thumb-2, no IT block, no NEON, and
 * no unaligned-access semantics to worry about -- the awkward quarter of the
 * ISA simply is not present.
 */
#ifndef CPU_H
#define CPU_H

#include <stdint.h>
#include "elf.h"

typedef struct {
    uint32_t r[16];
    uint32_t cpsr;
} cpu_state;

extern cpu_state g_cpu;
extern uint64_t   g_insn_count;
/* When non-zero, cpu_run returns HALT_STOPPED on reaching this address. Used to
 * take a dump with the engine's real state at a chosen point. */
extern uint32_t   g_stop_at;

/* g_stop_at makes cpu_run return HALT_STOPPED, with r15 set to the stop
 * address. The runner dumps memory and registers there and resumes from the
 * same instruction, so the dump describes the engine's real state at a chosen
 * point rather than at a function entry with no arguments. */


/* Why cpu_run() came back. */
enum {
    HALT_BUDGET = 0,   /* hit the instruction budget -- the normal way out */
    HALT_HALTED,       /* guest executed a deliberate halt (BKPT outside a stub) */
    HALT_UNDEFINED,    /* an encoding the decoder does not implement */
    HALT_TRAP,         /* a valid instruction we deliberately do not emulate */
    HALT_BAD_PC,       /* PC left the loaded image */
    HALT_IMPORT,       /* stopped at a host crossing (oracle mode) */
    HALT_BAD_MEM,      /* GUEST_CHECK only: an access left the guest space */
    HALT_STOPPED,      /* reached g_stop_at */
};

const char *cpu_halt_str(int reason);
/* One word, for tools: budget, import, returned, bad-pc, trap, undefined,
 * halted, stopped, bad-mem. */
const char *cpu_halt_slug(int reason);

/* PC of the last fault, plus the raw instruction and its width in bytes, for
 * the "tell me exactly what is missing" report. */
extern uint32_t g_fault_pc;
extern uint32_t g_fault_insn;
extern int      g_fault_width;
extern uint32_t g_fault_r[16];

/* Hand the interpreter the symbol table so it can attribute time per function
 * and name addresses in fault reports. */
void cpu_init(const guest_image *img);

/* Run from `entry` with stack pointer `sp` until an instruction budget runs
 * out or something traps. Returns the halt reason. */
int cpu_run(uint32_t entry, uint32_t sp, uint64_t budget);

/* Call a guest function from *inside* a host import, and come back with the
 * interpreter in the state it was in. Only for shims that cannot be written
 * without running guest code -- qsort's comparator is the one that exists.
 *
 * cpu_run() is not re-entrant as written: it owns g_cpu, g_budget, g_thumb and
 * the profiler's current-function range, and it resets the instruction trace
 * ring. A plain nested call would return to the outer interpreter with a
 * different register file, the wrong instruction set and a trace that starts
 * mid-function -- the first symptom would be a guest crash somewhere unrelated,
 * which is exactly the class of bug this port keeps paying for. So this saves
 * all of that, runs, and puts it back.
 *
 * The callee gets its own stack below the current frame (`sp` is the caller's
 * current SP minus a safety margin) so it cannot overwrite the host import's
 * arguments, which live on the guest stack.
 *
 * g_insn_count is deliberately NOT restored: it is the run's cumulative total
 * and the caller's budget is measured against it, so a nested call has to
 * advance it or the outer loop would never notice the work that was done. */
uint32_t cpu_call_guest(uint32_t entry, const uint32_t *args, int nargs,
                        uint64_t budget);

void cpu_set_profile(int on);
void cpu_dump_state(void);

/* Per-symbol instruction counts, indexed by position in image->sorted.
 * Valid only between cpu_init() and process exit. */
uint64_t *cpu_profile_counts(void);
void      cpu_profile_clear(void);

/* Ring buffer of the last N executed instructions, for bring-up. Safe to call
 * from a signal handler. */
void cpu_enable_trace(int on);
int  cpu_trace_is_on(void);
void cpu_dump_trace(void);
void cpu_trace_final(void);

void guest_text_bounds(uint32_t *lo, uint32_t *hi);

#endif /* CPU_H */
