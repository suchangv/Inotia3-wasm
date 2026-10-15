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

/* Why cpu_run() came back. */
enum {
    HALT_BUDGET = 0,   /* hit the instruction budget -- the normal way out */
    HALT_HALTED,       /* guest executed a deliberate halt (BKPT outside a stub) */
    HALT_UNDEFINED,    /* an encoding the decoder does not implement */
    HALT_TRAP,         /* a valid instruction we deliberately do not emulate */
    HALT_BAD_PC,       /* PC left the loaded image */
    HALT_IMPORT,       /* stopped at a host crossing (oracle mode) */
};

const char *cpu_halt_str(int reason);

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

void cpu_set_profile(int on);
void cpu_dump_state(void);

/* Per-symbol instruction counts, indexed by position in image->sorted.
 * Valid only between cpu_init() and process exit. */
uint64_t *cpu_profile_counts(void);

/* Ring buffer of the last N executed instructions, for bring-up. Safe to call
 * from a signal handler. */
void cpu_enable_trace(int on);
void cpu_dump_trace(void);
void cpu_trace_final(void);

#endif /* CPU_H */
