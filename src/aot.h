/*
 * aot.h -- pre-translation of the guest: pre-decoded instructions now, native
 * code later. See docs/aot-plan.md.
 *
 * The guest is a fixed, immutable image (BIND_NOW, no self-modifying code, no
 * mprotect import), so every instruction it will ever execute can be decoded
 * once and executed many times. That is the whole idea; how far the decoded
 * form is taken is a staging decision:
 *
 *   AOT_NONE    the interpreter in cpu.c, unchanged. The reference.
 *   AOT_PREDEC  each guest instruction is decoded once into a struct in a
 *               stable hash table, and execution dispatches on that struct
 *               (src/aot_predec.c). Thumb-1 only; ARM and Thumb-2 encodings
 *               stop with HALT_TRAP, because the first version exists to
 *               measure what pre-decoding is worth, not to ship.
 *
 * The contract is cpu_run()'s: same g_cpu, same g_insn_count, same HALT_*
 * reasons, same trace lines. tools/aotdiff.sh checks it by diffing
 * --trace-insns files from both modes.
 */
#ifndef AOT_H
#define AOT_H

#include <stdint.h>

enum { AOT_NONE = 0, AOT_PREDEC = 1, AOT_ARM64 = 2 };

/* Returned by aot_arm64_run() when this build/host cannot produce executable
 * code: the caller falls through to the interpreter, unchanged. */
#define AOT_NOT_AVAILABLE 0x7fffffff

/* Internal: aot_enter() returned the verify stopper, i.e. the block ended. */
#define AOT_VERIFY_STOP  0x7ffffffe

/* Set from --aot=off|predec, and from the AOT environment variable so a device
 * can flip it from env.txt without a rebuild. */
extern int g_aot_mode;

/* Parses "off" / "predec". Returns 0 on success, -1 for an unknown name. */
int aot_set_mode(const char *name);

/* One line: what was decoded, how many lookups, how much the cache cost. */
void aot_stats_report(void);

/* Continuation of cpu_run() for AOT_PREDEC: enters with g_cpu, g_budget and
 * g_insn_count already set up by cpu_run, Thumb state, and R[15] holding the
 * first instruction's address. Returns a HALT_* code. */
int aot_predec_run(uint64_t budget);

/* The same contract, executing translated aarch64 instead of pre-decoded
 * instructions. Defined in src/aot_arm64.c, compiled only for aarch64 hosts. */
int aot_arm64_run(uint32_t entry, uint32_t sp, uint64_t budget);
void aot_arm64_stats(void);
void aot_arm64_save_cache(void);
int  aot_arm64_available(void);

#endif /* AOT_H */
