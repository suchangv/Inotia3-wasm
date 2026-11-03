/*
 * aot_arm64.c -- translating the guest's Thumb/ARM code to aarch64 machine code.
 *
 * See docs/aot-plan.md. The guest image is fixed, relocated and patched before
 * the first instruction runs, so every instruction it will execute can be
 * translated once and then executed natively. This file is that translator and
 * its runtime.
 *
 * Shape of the generated code
 * ---------------------------
 * One guest basic block becomes one straight-line run of aarch64. A conditional
 * branch does *not* end a block: it is emitted as a real `b.cond` out of the
 * block, and the fall-through continues in the same block. That is what keeps
 * flags in PSTATE across `cmp; bgt L1; blt L2` sequences instead of forcing them
 * through a register.
 *
 * Pinned registers (all callee-saved under AAPCS64, so a host call preserves
 * them for free):
 *
 *   x19  .text low address            (indirect-branch range check)
 *   x20  dispatch table               (guest PC -> translated code)
 *   x21  dispatch table entry count   (the other half of the check)
 *   x22  the run's return address     (host `bl` clobbers x30, so keep it here)
 *   x24  instruction budget for this run
 *   x26  &g_insn_count
 *   x27  g_mem                        (guest address space)
 *   x28  &g_cpu.r[0]                  (guest register file, r[16] = cpsr)
 *
 * Guest addresses are 32-bit, so a guest load/store is
 * `ldr wT, [x27, wAddr, uxtw]` -- one instruction, no bounds check, exactly the
 * flat-memory model in src/guest.h.
 *
 * Flags
 * -----
 * The canonical form is the CPSR flag nibble in w23 (bits 31..28), because
 * `MRS Xt, NZCV` needs FEAT_FlagM and Cortex-A55 does not have it. Conditions
 * are tested with tbz/tbnz and, where PSTATE already holds the answer from an
 * exact `adds`/`subs`, with `b.cond` -- see aot_plan §8.4. Blocks that would
 * need a flag the block did not produce are not translated at all: they are
 * counted and interpreted, which is a precise fallback rather than a guess.
 *
 * Not here yet: cross-block direct chaining (every branch goes through
 * aot_enter), and the disk cache. Both are optimisations; correctness first.
 */

#include "aot.h"
#include "cpu.h"
#include "guest.h"
#include "imports.h"
#include "host_hooks.h"
#include "elf.h"
#include "platform.h"
#include "compat.h"

#ifndef WASM_FREESTANDING
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

/* One entry per 2 bytes of .text, covering the whole image. libgame.so text
 * is 1.3 MB (0x01000000..0x0113dac4), so 2^19 entries covered only the first
 * megabyte and every block above it was re-translated on each arrival --
 * 131k blocks and an exhausted arena. */
#define AOT_TABLE_ENTRIES (1u << 21)        /* one entry per 2 bytes, 8 MB    */
#define AOT_CODE_WORDS    (14u << 20)       /* 56 MB of code                  */
#define AOT_MAX_BLOCK     96                /* instructions per block         */

/* AOT_CHECK=1 compares every translated instruction against the interpreter,
 * one instruction at a time: the shadow is the interpreter keeper. */
static int       g_check;           /* AOT_CHECK=1: check every instruction  */
static cpu_state g_shadow;          /* the interpreter's copy, one insn ahead */
static uint32_t  g_shadow_pc;
static uint64_t  g_shadow_n;
static uint64_t  g_checks;
static int       g_seq;            /* AOT_SEQ=1: log the executed stream  */
static int       g_blkchk;         /* AOT_BLKCHECK=1: validate every branch */
/* AOT_SEQ=1 logs the executed stream so it can be diffed against the
 * interpreter's --trace-insns output; SPIKE_TRACE_LIMIT bounds it the same way
 * (an unbounded boot log is hundreds of megabytes and fills the disk). */
static void seq_log(uint32_t pc)
{
    if (!g_seq) return;
    static long limit = -1, written;
    if (limit < 0) { const char *e = getenv("SPIKE_TRACE_LIMIT"); limit = e ? atol(e) : 0; }
    if (limit && written >= limit) return;
    written++;
    host_log("aotseq %08x\n", pc & ~1u);
}

/* coverage counters, reported by aot_arm64_stats() */
uint64_t g_aot_blocks, g_aot_insns, g_aot_fallback_blocks;
uint32_t g_aot_why_count[4];

/* ------------------------------------------------------------------------- */
/* aarch64 encoder                                                           */
/* ------------------------------------------------------------------------- */

#define R_XZR 31
#define R_WZR 31

static uint32_t *g_emit;          /* write cursor into the arena             */
static uint32_t *g_emit_end;

static inline void emit(uint32_t w)
{
    if (g_emit >= g_emit_end) {
        host_fatal("aot: code arena exhausted (%u words)\n",
                   AOT_CODE_WORDS);
    }
    *g_emit++ = w;
}

/* add/subtract (immediate): sf op S 100010 sh imm12 Rn Rd */
static uint32_t enc_addsub_imm(int sf, int op, int S, int rd, int rn, uint32_t imm12)
{
    return (uint32_t)((sf << 31) | (op << 30) | (S << 29) | (0x11u << 24) |
                      ((imm12 & 0xfffu) << 10) | ((uint32_t)rn << 5) | (uint32_t)rd);
}

/* add/subtract (shifted register): sf op S 01011 shift(2) 0 imm6 Rm Rn Rd */
static uint32_t enc_addsub_reg(int sf, int op, int S, int rd, int rn, int rm,
                               int shift, int amount)
{
    return (uint32_t)((sf << 31) | (op << 30) | (S << 29) | (0x0bu << 24) |
                      ((uint32_t)shift << 22) | ((uint32_t)(amount & 63) << 10) |
                      ((uint32_t)rm << 16) | ((uint32_t)rn << 5) | (uint32_t)rd);
}

/* logical (shifted register): sf opc 01010 shift N imm6 Rm Rn Rd
 * opc: 0 and, 1 orr, 2 eor, 3 ands.  N=1 inverts Rm (bic/orn/eon/bics). */
static uint32_t enc_logic_reg(int sf, int opc, int N, int rd, int rn, int rm,
                              int shift, int amount)
{
    return (uint32_t)((sf << 31) | ((uint32_t)opc << 29) | (0x0au << 24) |
                      ((uint32_t)shift << 22) | ((uint32_t)N << 21) |
                      ((uint32_t)(amount & 63) << 10) | ((uint32_t)rm << 16) |
                      ((uint32_t)rn << 5) | (uint32_t)rd);
}

/* movz/movn/movk: sf opc(2) 100101 hw(2) imm16 Rd */
static uint32_t enc_movwide(int sf, int opc, int hw, uint32_t imm16, int rd)
{
    return (uint32_t)((sf << 31) | ((uint32_t)opc << 29) | (0x25u << 23) |
                      ((uint32_t)hw << 21) | ((imm16 & 0xffffu) << 5) | (uint32_t)rd);
}

/* move the 32-bit constant `v` into rd (0-2 instructions). */
static void emit_mov_const(int rd, uint32_t v)
{
    if (v == 0) { emit(enc_movwide(0, 2, 0, 0, rd)); return; }         /* movz */
    if ((v & 0xffff0000u) == 0) {
        emit(enc_movwide(0, 2, 0, v, rd));
    } else if ((v & 0xffffu) == 0) {
        emit(enc_movwide(0, 2, 1, v >> 16, rd));
    } else {
        emit(enc_movwide(0, 2, 0, v & 0xffffu, rd));
        emit(enc_movwide(0, 3, 1, v >> 16, rd));                       /* movk */
    }
}

/* ldr/str (unsigned offset): sf 111 0 01 opc(2) imm12 Rn Rt
 * size: 0 byte, 1 halfword, 2 word, 3 doubleword.  opc 1 = load, 0 = store,
 * 2 = sign-extending load (ldrsb/ldrsh/ldrsw). */
static uint32_t enc_ldst_imm(int size, int opc, int rt, int rn, uint32_t imm12)
{
    return (uint32_t)(((uint32_t)size << 30) | (7u << 27) | (1u << 24) |
                      ((uint32_t)opc << 22) | ((imm12 & 0xfffu) << 10) |
                      ((uint32_t)rn << 5) | (uint32_t)rt);
}

/* ldr/str (register offset): size 111 0 00 opc 1 Rm option(3) S 10 Rn Rt
 * option 0b010 = UXTW (zero-extend the 32-bit guest address). */
/* MRS Xt, NZCV / MSR NZCV, Xt. Reading NZCV from EL0 is UNDEFINED without
 * FEAT_FlagM, which Cortex-A55 does not have, so these are used only by the
 * AOT_SEQ trace, which runs under qemu on the host. */
static uint32_t enc_mrs_nzcv(int rt) { return 0xd53b4200u | (uint32_t)rt; }
static uint32_t enc_msr_nzcv(int rt) { return 0xd51b4200u | (uint32_t)rt; }

static uint32_t enc_ldst_reg(int size, int opc, int rt, int rn, int rm,
                             int option, int S, int amount)
{
    return (uint32_t)(((uint32_t)size << 30) | (7u << 27) | (1u << 21) |
                      ((uint32_t)opc << 22) | ((uint32_t)rm << 16) |
                      ((uint32_t)option << 13) | ((uint32_t)S << 12) |
                      (2u << 10) |                 /* fixed "10"            */
                      ((uint32_t)rn << 5) | (uint32_t)rt);
}

/* branch (immediate): 000101 imm26  (offset in instructions, +/-128 MB) */
static uint32_t enc_b(int32_t off_words)
{
    return 0x14000000u | ((uint32_t)off_words & 0x03ffffffu);
}
static uint32_t enc_bl(int32_t off_words)
{
    return 0x94000000u | ((uint32_t)off_words & 0x03ffffffu);
}

/* b.cond: 0101010 0 imm19 0 cond */
static uint32_t enc_bcond(int cond, int32_t off_words)
{
    return 0x54000000u | (((uint32_t)off_words & 0x7ffffu) << 5) |
           (uint32_t)(cond & 0xf);
}

/* cbz/cbnz: sf 011010 op imm19 Rt */
static uint32_t enc_cb(int sf, int nz, int rt, int32_t off_words)
{
    return (uint32_t)((sf << 31) | (0x34u << 24) | ((uint32_t)nz << 24) |
                      (((uint32_t)off_words & 0x7ffffu) << 5) | (uint32_t)rt);
}

/* tbz/tbnz: b5 011011 op b40 imm14 Rt */
static uint32_t enc_tb(int nz, int rt, int bit, int32_t off_words)
{
    return (uint32_t)(((uint32_t)(rt >> 5) << 31) | (0x36u << 24) |
                      ((uint32_t)nz << 24) | ((uint32_t)(bit & 31) << 19) |
                      (((uint32_t)off_words & 0x3fffu) << 5) | (uint32_t)(rt & 31));
}

/* br/blr/ret: 1101011 0 0 1 11111 000000 Rn 00000 */
static uint32_t enc_br(int rn)  { return 0xd61f0000u | ((uint32_t)rn << 5); }
static uint32_t enc_blr(int rn) { return 0xd63f0000u | ((uint32_t)rn << 5); }
static uint32_t enc_ret(int rn) { return 0xd65f0000u | ((uint32_t)rn << 5); }

/* bitfield: sf opc(2) 100110 N immr imms Rn Rd
 * opc 0 = sbfm, 1 = bfm, 2 = ubfm. */
static uint32_t enc_bitfield(int sf, int opc, int rd, int rn, int immr, int imms)
{
    return (uint32_t)((sf << 31) | ((uint32_t)opc << 29) | (0x26u << 23) |
                      ((uint32_t)(immr & 63) << 16) | ((uint32_t)(imms & 63) << 10) |
                      ((uint32_t)rn << 5) | (uint32_t)rd);
}

/* data processing (2 source): sf 0 0 11010110 Rm opcode(6) Rn Rd */
static uint32_t enc_dp2(int sf, int rm, int opcode, int rn, int rd)
{
    return (uint32_t)((sf << 31) | (0x1ACu << 20) | ((uint32_t)rm << 16) |
                      ((uint32_t)opcode << 10) | ((uint32_t)rn << 5) | (uint32_t)rd);
}
#define DP2_UDIV 0x02
#define DP2_SDIV 0x03
#define DP2_LSLV 0x08
#define DP2_LSRV 0x09
#define DP2_ASRV 0x0a
#define DP2_RORV 0x0b

/* mul/madd/msub: sf 00 11011 000 Rm 0 Ra Rn Rd */
/* wide multiply-accumulate: UMULL/UMLAL/SMULL/SMLAL all land here, with the
 * accumulator 64-bit register pair as the "addend". Ra = 31 means "no addend". */
static uint32_t enc_lmul(int is_signed, int rd, int rn, int rm, int ra)
{
    return (uint32_t)((is_signed ? 0x9B200000u : 0x9BA00000u) |
                      ((uint32_t)rm << 16) | ((uint32_t)ra << 10) |
                      ((uint32_t)rn << 5) | (uint32_t)rd);
}

static uint32_t enc_madd(int sf, int rd, int rn, int rm, int ra)
{
    return (uint32_t)((sf << 31) | (0x1bu << 24) | ((uint32_t)rm << 16) |
                      ((uint32_t)ra << 10) | ((uint32_t)rn << 5) | (uint32_t)rd);
}

/* csel: sf 0 0 11010100 Rm cond 00 Rn Rd */
static uint32_t enc_csel(int sf, int rd, int rn, int rm, int cond)
{
    return (uint32_t)((sf << 31) | (0x1A8u << 20) | ((uint32_t)rm << 16) |
                      ((uint32_t)cond << 12) | (0x2u << 10) |
                      ((uint32_t)rn << 5) | (uint32_t)rd);
}

/* add/subtract with carry: sf op S 11010000 Rm 000000 Rn Rd
 * op 0 = adc, 1 = sbc; S = 1 sets the flags. The carry chain is part of the
 * instruction, which is exactly what ARM's ADC/SBC do. */
static uint32_t enc_adcsbc(int sf, int op, int S, int rd, int rn, int rm)
{
    return (uint32_t)((sf << 31) | ((uint32_t)op << 30) | ((uint32_t)S << 29) |
                      (0xd0u << 21) | ((uint32_t)rm << 16) |
                      ((uint32_t)rn << 5) | (uint32_t)rd);
}

/* cset: csinc Rd, zr, zr, invert(cond) */
static uint32_t enc_cset(int rd, int cond)
{
    return (uint32_t)((0x1A8u << 20) | ((uint32_t)R_WZR << 16) |
                      (((uint32_t)(cond ^ 1) & 0xf) << 12) | (0x1u << 10) |
                      ((uint32_t)R_WZR << 5) | (uint32_t)rd);
}

/* rev/rev16/rbit: sf 1 0 11010110 00000 opc(2) 10 Rn Rd */
static uint32_t enc_rev(int sf, int opc, int rn, int rd)
{
    return (uint32_t)(((uint32_t)sf << 31) | (1u << 30) | (0xD6u << 21) |
                      ((uint32_t)opc << 10) | ((uint32_t)rn << 5) | (uint32_t)rd);
}
#define REV_REV16 1
#define REV_REV32 2
#define REV_RBIT  0
#define REV_REV64 3
#define LDS_REG   3     /* sign-extending load, register-offset form */

/* ldrsb/ldrsh/ldrsw use enc_ldst_imm with opc 2 (size 0/1/2) */

/* ------------------------------------------------------------------------- */
/* kernel: the arena, the dispatch table, translation on demand               */
/* ------------------------------------------------------------------------- */

static uint32_t *g_code;                    /* arena base                     */
static uint64_t *g_disp;                    /* dispatch table (host addresses) */
static uint32_t  g_text_lo, g_text_hi;
static const guest_image *g_img;

/* Per-block translation result. */
typedef struct {
    uint32_t pc;            /* guest address of the block's first insn       */
    uint32_t *code;         /* host address of its first instruction         */
    uint32_t insns;         /* guest instructions in the block               */
} block_t;

/* Static exits that could not be linked yet: their placeholder `b` is rewritten
 * when the target block is translated. A block has at most a handful of these,
 * so a flat array with an append-only cursor is enough; when it fills up the
 * only consequence is that later exits stay on the slow path, which is exactly
 * what the code did before linking existed. */
struct pend_link { uint32_t *slot; uint32_t target; };
#define AOT_MAX_LINKS (1u << 15)
static struct pend_link g_link[AOT_MAX_LINKS];
static uint32_t g_nlink;

static void disp_set(uint32_t pc, uint32_t *code);

/* Guest PC -> host code, or 0. Indexed by (pc - text_lo) / 2 for Thumb. */
static inline uint32_t *disp_get(uint32_t pc)
{
    uint32_t i = (pc - g_text_lo) >> 1;
    if (i >= AOT_TABLE_ENTRIES) return NULL;
    return (uint32_t *)(uintptr_t)g_disp[i];
}
static inline void disp_set(uint32_t pc, uint32_t *code)
{
    uint32_t i = (pc - g_text_lo) >> 1;
    if (i < AOT_TABLE_ENTRIES) g_disp[i] = (uint64_t)(uintptr_t)code;

    /* Link every exit that was waiting for this block. The slot holds a `b` to
     * its own slow path; rewriting it makes the next arrival a single branch.
     * Compiled lazily, so this is the only place that knows a block appeared. */
    for (uint32_t k = 0; k < g_nlink; k++) {
        if (g_link[k].target != pc) continue;
        uint32_t *slot = g_link[k].slot;
        *slot = enc_b((int32_t)(code - slot));
        __builtin___clear_cache((char *)slot, (char *)(slot + 1));
        g_link[k].target = 0;                    /* done, skipped from now on */
    }
}

/* ------------------------------------------------------------------------- */
/* guest registers: kept in g_cpu (x28), one ldr/str per access              */
/* ------------------------------------------------------------------------- */

/* ---- guest register cache ------------------------------------------------
 *
 * Most instructions write a guest register and the next one reads it straight
 * back, and a store followed by a load of the same address is a
 * store-to-load-forwarding stall. Four host registers mirror guest registers,
 * so the read becomes a register move.
 *
 * The store is DEFERRED: the value waits in the slot until something outside
 * G_LD/G_ST can observe g_cpu. A guest register written twice in a block costs
 * one store, and the very common write-then-read pair never touches memory --
 * which is the point, because a store immediately followed by a load of the same
 * address is a store-to-load-forwarding stall. Measured: the store is 44% of the
 * run (replacing it with a register move takes the bench from 908 to 1636 MIPS)
 * and the load is free (894), so deferring the store is where the money is.
 *
 * Correctness rests on one rule: rc_sync() publishes and forgets the mapping
 * before anything that can read or write g_cpu from outside G_LD/G_ST. The list
 * is short and every entry was found by a failing gate:
 *
 *   - a host call (emit_call);
 *   - the block's own PC write (emit_guest_pc);
 *   - every exit, including the budget check's halt stub, which returns to a
 *     runner that reads g_cpu;
 *   - a conditional branch, whose taken path leaves for an out-of-line stub;
 *   - a conditional body, whose store clears the slot's dirty flag at
 *     translation time -- so a deferred store from before the guard would vanish
 *     from the publish list even on the path where the body never ran;
 *   - a translation rollback, which discards the emitted code the slots describe.
 *
 * r13/r15 are written to g_cpu directly (the block's PC, the exit's stack) and
 * r14 is the return address the exit reads, so those three are never deferred.
 *
 * w5..w8 are the slots: the emitters use w0, w1 and w9..w15 as scratch, w23 for
 * exit targets, and pin x19..x28. Checked by grepping every encoder call. */
#define RC_N 4
static const int rc_host[RC_N] = { 5, 6, 7, 8 };
static int8_t rc_slot[16];
static int8_t rc_of[RC_N];

/* Forget the mapping. g_cpu is already correct, so nothing has to be published.
 * Called before anything that can write g_cpu behind the mapping's back: a host
 * call, the block's own PC write, a block boundary, and a translation rollback
 * (which discards the emitted code the slots describe). */

static uint8_t rc_dirty[RC_N];
static int g_rc_cond;      /* inside a conditionally executed body */

/* Publish every dirty slot, keeping the mapping: both paths of a branch, and
 * the fall-through, continue to use the same slots. */
static void rc_flush(void)
{
    for (int i = 0; i < RC_N; i++)
        if (rc_of[i] >= 0 && rc_dirty[i]) {
            emit(enc_ldst_imm(2, 0, rc_host[i], 28, (uint32_t)rc_of[i]));
            rc_dirty[i] = 0;
        }
}

static void rc_sync(void)
{
    rc_flush();
    for (int i = 0; i < RC_N; i++) rc_of[i] = -1;
    for (int i = 0; i < 16; i++) rc_slot[i] = -1;
}

static int rc_alloc(int g)
{
    for (int i = 0; i < RC_N; i++)
        if (rc_of[i] == g) return i;
    int i = -1;
    for (int k = 0; k < RC_N; k++)
        if (rc_of[k] < 0) { i = k; break; }
    if (i < 0) {
        static int rr;
        i = rr; rr = (rr + 1) % RC_N;
        if (rc_dirty[i])
            emit(enc_ldst_imm(2, 0, rc_host[i], 28, (uint32_t)rc_of[i]));
        rc_slot[(int)rc_of[i]] = -1;
        rc_dirty[i] = 0;
    }
    rc_of[i] = (int8_t)g;
    rc_slot[g] = (int8_t)i;
    return i;
}

#define G_LD(w, g) \
    do { \
        int _s = rc_slot[(g)]; \
        if (_s >= 0) { \
            if (rc_host[_s] != (w)) \
                emit(enc_logic_reg(0, 1, 0, (w), R_WZR, rc_host[_s], 0, 0)); \
        } else \
            emit(enc_ldst_imm(2, 1, (w), 28, (uint32_t)(g))); \
    } while (0)

#define G_ST(g, w) \
    do { \
        int _s = rc_alloc((g)); \
        if (rc_host[_s] != (w)) \
            emit(enc_logic_reg(0, 1, 0, rc_host[_s], R_WZR, (w), 0, 0)); \
        if (g_rc_cond) { \
            emit(enc_ldst_imm(2, 0, (w), 28, (uint32_t)(g))); \
            rc_dirty[_s] = 0; \
        } else \
            rc_dirty[_s] = 1; \
    } while (0)


/* guest load/store through the flat address space: [x27, wIdx, uxtw] */
#define M_LD(size, w, idx)  emit(enc_ldst_reg(size, 1, (w), 27, (idx), 2, 0, 0))
#define M_ST(size, w, idx)  emit(enc_ldst_reg(size, 0, (w), 27, (idx), 2, 0, 0))
#define M_LDS(size, w, idx) emit(enc_ldst_reg(size, LDS_REG, (w), 27, (idx), 2, 0, 0))

/* mov wD, wS */
#define MOVW(d, s) emit(enc_logic_reg(0, 1, 0, (d), R_WZR, (s), 0, 0))
/* and wD, wS, #imm -> and with a materialised constant (bitmask immediates are
 * their own encoding, and getting one wrong is a silent wrong answer) */
static void emit_and_imm(int rd, int rs, uint32_t imm)
{
    emit_mov_const(9, imm);
    emit(enc_logic_reg(0, 0, 0, rd, rs, 9, 0, 0));
}

/* ------------------------------------------------------------------------- */
/* decoded instruction                                                       */
/* ------------------------------------------------------------------------- */

enum {
    AOP_UNSUP = 0, AOP_SHIFT, AOP_ADDSUB, AOP_IMM8, AOP_ALU, AOP_HI, AOP_BX,
    AOP_LDRLIT, AOP_LDST_REG, AOP_LDST_IMM, AOP_LDST_SP, AOP_ADDR, AOP_ADJSP,
    AOP_PUSH, AOP_POP, AOP_REV, AOP_EXT, AOP_NOP, AOP_BKPT, AOP_SWI,
    AOP_UNDEF, AOP_LDMIA, AOP_STMIA, AOP_BCOND, AOP_B, AOP_BL, AOP_BLX
};

typedef struct {
    uint32_t pc;
    uint32_t imm;       /* immediate, address or displacement               */
    uint16_t insn;      /* raw halfword (or low half of the ARM word)       */
    uint8_t  op;
    uint8_t  rd, rn, rm;
    uint8_t  a, b;      /* per-op selectors                                 */
    uint8_t  len;       /* 2 or 4                                           */
    uint8_t  cnt;       /* instructions retired: 1, or 2 for 32-bit Thumb    */
    uint8_t  cond;      /* ARM condition, 0xe (AL) for anything Thumb        */
} air;

/* Does this instruction read the flags the previous one produced? The checker
 * cannot follow a flag producer, because its call into C clobbers PSTATE, which
 * is where those flags live. So a producer whose consumer is the next
 * instruction goes unchecked -- and the consumer's own check covers it, because
 * a flag is only observable through the branch or the carry-in it feeds. */


/* flag providers within the current block. Bits: N=1, Z=2, C=4, V=8. */
enum { CC_NONE = 0, CC_EXACT, CC_NZ };

static int cc_kind;
static uint32_t g_block_pc;      /* guest PC of the block being emitted */

/* The last flag-setting instruction, if it was one the flags can be recomputed
 * from exactly (add/sub/cmp/cmn). Used when a block exit lands on a block that
 * reads flags but cannot be translated. */

static int cond_bits(int cond)
{
    /* which flags the condition reads */
    switch (cond) {
    case 0x0: case 0x1: return 2;          /* EQ NE: Z    */
    case 0x2: case 0x3: return 4;          /* CS CC: C    */
    case 0x4: case 0x5: return 1;          /* MI PL: N    */
    case 0x6: case 0x7: return 8;          /* VS VC: V    */
    case 0x8: case 0x9: return 4 | 2;      /* HI LS: C Z  */
    case 0xa: case 0xb: return 1 | 8;      /* GE LT: N V  */
    case 0xc: case 0xd: return 1 | 8 | 2;  /* GT LE: N Z V */
    default:  return 0;                    /* AL          */
    }
}

/* What the current provider can be trusted for. An exact `adds`/`subs`/`cmp`
 * leaves all four flags in PSTATE; a partial setter is materialised with a
 * `tst`, which gets N and Z right and clobbers C and V -- so a consumer that
 * needs C or V after one makes the whole block untranslatable, and it is
 * interpreted instead. That is the precise fallback the plan asks for. */
static int cc_provides(void)
{
    return cc_kind == CC_EXACT ? 15 : cc_kind == CC_NZ ? 3 : 0;
}

/* ------------------------------------------------------------------------- */
/* decode: Thumb-1 first, 32-bit encodings to follow                         */
/* ------------------------------------------------------------------------- */

static void dec_thumb(uint32_t pc, air *o)
{
    uint32_t ins = LD16(pc);
    uint32_t top = ins >> 12;

    o->pc = pc;
    o->insn = (uint16_t)ins;
    o->len = 2;
    o->cnt = 1;
    o->cond = 0xe;
    o->op = AOP_UNSUP;
    o->imm = 0; o->rd = o->rn = o->rm = 0; o->a = o->b = 0;

    /* an import stub, or a host-hooked function entry: a BKPT whose index comes
     * from the address (stub page) or from imm8 (hooked entry) */
    if (pc >= GUEST_STUB_BASE && pc < GUEST_STUB_BASE + IMPORT_MAX * 4u) {
        o->op = AOP_BKPT;
        return;
    }

    switch (top) {
    case 0x0: case 0x1:
        if ((ins & 0x1800) != 0x1800) {
            o->op = AOP_SHIFT;
            o->rd = ins & 7; o->rm = (ins >> 3) & 7;
            o->a = (uint8_t)((ins >> 11) & 3);
            o->imm = (ins >> 6) & 0x1f;
        } else {
            o->op = AOP_ADDSUB;
            o->rd = ins & 7; o->rn = (ins >> 3) & 7;
            o->a = (ins & 0x0400) ? 1 : 0;
            o->b = (ins & 0x0200) ? 1 : 0;
            o->imm = (ins >> 6) & 7;
            o->rm = (uint8_t)((ins >> 6) & 7);
        }
        break;
    case 0x2: case 0x3:
        o->op = AOP_IMM8;
        o->rd = (ins >> 8) & 7;
        o->a = (uint8_t)((ins >> 11) & 3);
        o->imm = ins & 0xff;
        break;
    case 0x4:
        if ((ins & 0xFC00) == 0x4000) {
            o->op = AOP_ALU;
            o->rd = ins & 7; o->rm = (ins >> 3) & 7;
            o->a = (uint8_t)((ins >> 6) & 0xf);
        } else if ((ins & 0xFC00) == 0x4400) {
            o->rd = (uint8_t)((ins & 7) | ((ins >> 4) & 8));
            o->rm = (uint8_t)((ins >> 3) & 0xf);
            o->a = (uint8_t)((ins >> 8) & 3);
            o->op = (o->a == 3) ? AOP_BX : AOP_HI;
        } else {
            o->op = AOP_LDRLIT;
            o->rd = (ins >> 8) & 7;
            o->imm = ((pc + 4) & ~3u) + ((ins & 0xff) << 2);
        }
        break;
    case 0x5:
        o->op = AOP_LDST_REG;
        o->rd = ins & 7; o->rn = (ins >> 3) & 7; o->rm = (ins >> 6) & 7;
        o->a = (uint8_t)((ins >> 9) & 7);
        break;
    case 0x6: case 0x7: case 0x8: case 0x9:
        if (top == 0x9) {
            o->op = AOP_LDST_SP;
            o->rd = (ins >> 8) & 7;
            o->imm = ins & 0xff;
            o->b = (ins & 0x0800) ? 1 : 0;
        } else {
            o->op = AOP_LDST_IMM;
            o->rd = ins & 7; o->rn = (ins >> 3) & 7;
            o->b = (ins & 0x0800) ? 1 : 0;
            o->a = (uint8_t)(top == 0x8 ? 1 : top == 0x7 ? 2 : 0);
            o->imm = (ins >> 6) & 0x1f;
        }
        break;
    case 0xa:
        o->op = AOP_ADDR;
        o->rd = (ins >> 8) & 7;
        o->b = (ins & 0x0800) ? 1 : 0;
        o->imm = (ins & 0xff) << 2;
        break;
    case 0xb:
        if ((ins & 0xFF00) == 0xB000) {
            o->op = AOP_ADJSP;
            o->imm = (ins & 0x7f) << 2;
            o->b = (ins & 0x80) ? 1 : 0;
        } else if ((ins & 0xFE00) == 0xB400) {
            o->op = AOP_PUSH;
            o->imm = ins & 0xff;
            o->b = (ins & 0x100) ? 1 : 0;
        } else if ((ins & 0xFE00) == 0xBC00) {
            o->op = AOP_POP;
            o->imm = ins & 0xff;
            o->b = (ins & 0x100) ? 1 : 0;
        } else if ((ins & 0xFF00) == 0xBA00) {
            o->op = AOP_REV;
            o->rm = (ins >> 3) & 7; o->rd = ins & 7;
            o->a = (uint8_t)((ins >> 6) & 3);
        } else if ((ins & 0xFF00) == 0xB200) {
            o->op = AOP_EXT;
            o->rm = (ins >> 3) & 7; o->rd = ins & 7;
            o->a = (uint8_t)((ins >> 6) & 3);
        } else if (ins == 0xBF00) {
            o->op = AOP_NOP;
        } else if ((ins & 0xFF00) == 0xBE00) {
            o->op = AOP_BKPT;
            o->a = (uint8_t)(ins & 0xff);
        }
        break;
    case 0xc:
        o->op = (ins & 0x0800) ? AOP_LDMIA : AOP_STMIA;
        o->rn = (ins >> 8) & 7;
        o->imm = ins & 0xff;
        break;
    case 0xd: {
        uint32_t cond = (ins >> 8) & 0xf;
        if (cond == 0xf) {
            o->op = AOP_SWI;
        } else if (cond == 0xe) {
            o->op = AOP_UNDEF;
        } else {
            o->op = AOP_BCOND;
            o->a = (uint8_t)cond;
            o->imm = (uint32_t)(int32_t)((int32_t)(int8_t)(ins & 0xff) << 1);
        }
        break;
    }
    case 0xe:
        if ((ins & 0xF800) != 0xE000) {
            o->len = 4;
            o->cnt = 2;
        } else {
            int32_t soff = (int32_t)(ins & 0x7ff);
            soff = (soff << 21) >> 21;
            o->op = AOP_B;
            o->imm = (uint32_t)(soff << 1);
        }
        break;
    default: {
        uint32_t lo = LD16(pc + 2);
        if ((lo >> 11) != 0x1fu && (lo >> 11) != 0x1du)
            break;
        uint32_t S = (ins >> 10) & 1;
        uint32_t imm10 = ins & 0x3ff;
        uint32_t J1 = (lo >> 13) & 1, H = (lo >> 12) & 1;
        uint32_t J2 = (lo >> 11) & 1, imm11 = lo & 0x7ff;
        uint32_t I1 = (~(J1 ^ S)) & 1, I2 = (~(J2 ^ S)) & 1;
        int32_t off = (int32_t)((S << 24) | (I1 << 23) | (I2 << 22) |
                                (imm10 << 12) | (imm11 << 1));
        off = (off << 7) >> 7;
        o->op = H ? AOP_BL : AOP_BLX;
        /* Base address: BL is relative to PC, i.e. the instruction address + 4,
         * while BLX (immediate) is relative to Align(PC, 4) -- its targets have
         * to be word aligned. The two differ by 2 for a 32-bit instruction at a
         * 2-mod-4 address, which is every other one. Using PC+4 for both made
         * `blx __aeabi_i2d` land 2 bytes into the function. */
        uint32_t base = H ? (pc + 4) : ((pc + 4) & ~3u);
        o->imm = (uint32_t)((int32_t)base + off);
        o->len = 4;
        o->cnt = 2;
        break;
    }
    }
}

/* ------------------------------------------------------------------------- */
/* ARM mode, and the Thumb-2 encodings the image uses                        */
/* ------------------------------------------------------------------------- */

/* Extra opcodes for the 32-bit instruction set. Kept apart from the Thumb-1
 * ones so a mistake in one cannot silently change the other. */
enum {
    AOP_ARM_DP = 0x40,   /* data processing, immediate or register-shifted     */
    AOP_ARM_LDST,        /* single data transfer, imm or register offset       */
    AOP_ARM_LDM,         /* ldm/stm                                            */
    AOP_ARM_MUL,         /* mul/mla                                            */
    AOP_ARM_CLZ,         /* clz                                                */
    AOP_ARM_B,           /* b/bl immediate                                     */
    AOP_ARM_BX,          /* bx/blx register                                    */
    AOP_ARM_T2_MOVW,     /* Thumb-2 movw/movt                                  */
    AOP_ARM_T2_DP,       /* Thumb-2 wide data processing                       */
    AOP_ARM_LMUL         /* umull/umlal/smull/smlal                            */
};



/* ARM data processing opcodes (the ARM ARM's table, bits 24..21). */
#define DP_AND 0x0
#define DP_EOR 0x1
#define DP_SUB 0x2
#define DP_RSB 0x3
#define DP_ADD 0x4
#define DP_ADC 0x5
#define DP_SBC 0x6
#define DP_TST 0x8
#define DP_TEQ 0x9
#define DP_CMP 0xa
#define DP_CMN 0xb
#define DP_ORR 0xc
#define DP_MOV 0xd
#define DP_BIC 0xe
#define DP_MVN 0xf

static air g_last_cc;
static int g_last_cc_valid;

/* Publish PSTATE into g_cpu.cpsr, exactly.
 *
 * AOT code keeps the flags in PSTATE while the interpreter reads g_cpu.cpsr, so
 * an exit into a block that starts with a conditional instruction has to write
 * them out. This used to recompute them from the remembered setter's operands,
 * which is wrong as soon as the setter is itself conditional: a `cmnmi` leaves
 * the previous flags in place when MI is false, and the recomputation published
 * the ones it would have set.
 *
 * cset reads PSTATE without writing it, so four of them copy the nibble out
 * with no assumption about how it got there. */
static void emit_flags_to_cpsr(void)
{
    emit(enc_cset(12, 4));                       /* N: cset w12, mi */
    emit(enc_logic_reg(0, 1, 0, 12, 12, 12, 0, 31));
    emit(enc_cset(13, 0));                       /* Z: cset w13, eq */
    emit(enc_logic_reg(0, 1, 0, 12, 12, 13, 0, 30));
    emit(enc_cset(13, 2));                       /* C: cset w13, cs */
    emit(enc_logic_reg(0, 1, 0, 12, 12, 13, 0, 29));
    emit(enc_cset(13, 6));                       /* V: cset w13, vs */
    emit(enc_logic_reg(0, 1, 0, 12, 12, 13, 0, 28));
    emit(enc_addsub_imm(0, 0, 0, 12, 12, 0x10)); /* user mode bit */
    emit(enc_ldst_imm(2, 0, 12, 28, 16));        /* str w12, [x28,#64] */
    /* Not a guest register: the slot is untouched. */
}


static int insn_reads_flags(const air *i)
{
    if (i->op == AOP_BCOND) return 1;
    if (i->op == AOP_ALU && (i->a == 0x5 || i->a == 0x6)) return 1;  /* adc/sbc */
    /* ARM and Thumb-2 instructions can be conditional, and a condition is read
     * from the flags -- which the checker's call would destroy. Missing this
     * made every conditional instruction in __divsi3 branch on garbage. */
    if (i->op >= AOP_ARM_DP && i->cond != 0xe) return 1;
    if (i->op == AOP_ARM_DP && (i->a == DP_ADC || i->a == DP_SBC)) return 1;
    return 0;
}


/* Decode one 32-bit ARM instruction at `pc`. Anything not implemented here
 * comes back as AOP_UNSUP and is interpreted, one instruction at a time, with
 * the encoding printed once so the worklist comes from running rather than
 * guessing. */
static void dec_arm(uint32_t pc, air *o)
{
    uint32_t ins = LD32(pc & ~3u);
    uint32_t cond = ins >> 28;
    o->pc = pc;
    o->insn = (uint16_t)(ins >> 16);
    o->len = 4;
    o->cnt = 1;
    o->cond = (uint8_t)cond;
    o->op = AOP_UNSUP;
    o->imm = 0; o->rd = o->rn = o->rm = 0; o->a = o->b = 0;
    if (cond == 0xf) {
        /* the unconditional space: BLX immediate, PLD, and coprocessor 2 */
        if (((ins >> 25) & 7) == 5) {                       /* BLX <imm> */
            int32_t off = (int32_t)(ins & 0xffffff);
            off = (off << 8) >> 6;
            o->op = AOP_ARM_B;
            o->b = 2;                                       /* BLX to ARM */
            o->imm = (uint32_t)((int32_t)pc + 8 + off + (((ins >> 24) & 1) << 1));
            return;
        }
        if ((ins & 0x0f7f0000u) == 0x055f0000u) return;     /* PLD: no-op */
        return;                                             /* trap */
    }
    uint32_t op1 = (ins >> 25) & 7;
    uint32_t rd  = (ins >> 12) & 0xf;
    uint32_t rn  = (ins >> 16) & 0xf;

    /* long multiply: cond 00001 U A S RdHi(19..16) RdLo(15..12) Rs(11..8)
     * 1001 Rm(3..0). Its bits 27..23 are 00001, which the 32-bit multiply test
     * below (bits 27..22 = 000000) does not accept -- so without this it fell
     * through to the data-processing class and every 64-bit multiply in the
     * soft-float helpers became nonsense. */
    if ((ins & 0x0f800090u) == 0x00800090u) {
        o->op = AOP_ARM_LMUL;
        o->a = (uint8_t)((ins >> 22) & 1);                  /* 1 = signed   */
        o->b = (uint8_t)((ins >> 21) & 1);                  /* accumulate   */
        if ((ins >> 20) & 1) { o->op = AOP_UNSUP; return; }  /* S: rare      */
        o->rd = (uint8_t)((ins >> 12) & 0xf);               /* RdLo         */
        o->imm = (uint32_t)((ins >> 16) & 0xf);             /* RdHi         */
        o->rn = (uint8_t)(ins & 0xf);                       /* Rm           */
        o->rm = (uint8_t)((ins >> 8) & 0xf);                /* Rs           */
        return;
    }

    /* multiply: cond 000000 A S Rd(19..16) Ra(15..12) Rs(11..8) 1001 Rm(3..0).
     * Rd and Ra are NOT where the data-processing Rd is, and taking Rd from
     * bits 15..12 here makes `mul r3, r2, r0` write r0 instead. */
    if ((ins & 0x0fc000f0u) == 0x00000090u) {
        o->op = AOP_ARM_MUL;
        o->rd = (uint8_t)((ins >> 16) & 0xf);               /* Rd */
        o->rn = (uint8_t)(ins & 0xf);                       /* Rm */
        o->rm = (uint8_t)((ins >> 8) & 0xf);                /* Rs */
        o->a = (uint8_t)((ins >> 21) & 1);                  /* accumulate */
        o->imm = (uint32_t)((ins >> 12) & 0xf);             /* Ra */
        o->b = (uint8_t)((ins >> 20) & 1);                  /* S */
        return;
    }
    /* clz: 00010110 1111 Rd 1111 0001 Rm */
    if ((ins & 0x0fff0ff0u) == 0x016f0f10u) {
        o->op = AOP_ARM_CLZ;
        o->rd = (uint8_t)rd;
        o->rm = (uint8_t)(ins & 0xf);
        return;
    }
    /* BX / BLX register: 0001 0010 1111 1111 1111 001x Rm. Bit 5 is the
     * BX/BLX discriminator and the register is the low nibble, so one mask
     * matches both forms. This has to be tested before the data-processing
     * class: with the old mask (which demanded bit 5 clear) a BLX register
     * instruction decoded as TEQ, so the call never happened, LR was never
     * updated and the flags were quietly clobbered. */
    if ((ins & 0x0fffffd0u) == 0x012fff10u) {
        o->op = AOP_ARM_BX;
        o->rm = (uint8_t)(ins & 0xf);
        o->b = (uint8_t)((ins >> 5) & 1);                    /* BLX */
        return;
    }
    /* MRS/MSR and the coprocessor space: not implemented, and loudly so. */
    if ((ins & 0x0f000000u) == 0x0f000000u) return;

    /* single data transfer: 01 I P U B W L Rn Rd offset */
    if (op1 == 2 || op1 == 3) {
        uint32_t I = (ins >> 25) & 1;
        o->op = AOP_ARM_LDST;
        o->rd = (uint8_t)rd;
        o->rn = (uint8_t)rn;
        o->a = (uint8_t)((ins >> 22) & 1);                  /* B: byte? */
        o->b = (uint8_t)(((ins >> 20) & 1) |                 /* load      */
                         (((ins >> 24) & 1) << 1) |          /* pre       */
                         (((ins >> 23) & 1) << 2) |          /* up        */
                         (((ins >> 21) & 1) << 3) |          /* writeback */
                         (I << 4));                          /* register  */
        if (I) {
            o->rm = (uint8_t)(ins & 0xf);
            o->imm = (ins >> 7) & 0x1f;                      /* shift amount */
            o->a = (uint8_t)(o->a | (((ins >> 5) & 3) << 2));/* shift type */
        } else {
            o->imm = ins & 0xfff;
        }
        return;
    }
    /* halfword/doubleword transfer: 000 P U I W L Rn Rd 1 S H 1 Rm */
    if ((ins & 0x0e000090u) == 0x00000090u) {
        uint32_t S = (ins >> 6) & 1, H = (ins >> 5) & 1;
        uint32_t L = (ins >> 20) & 1;
        o->op = AOP_ARM_LDST;
        o->rd = (uint8_t)rd;
        o->rn = (uint8_t)rn;
        o->a = (uint8_t)(L ? (H ? (S ? 4 : 3) : 2) : 2);     /* LDRH/LDRSB/LDRSH/STRH */
        if (L == 0 && H == 0) o->a = 5;                      /* LDRD/STRD */
        o->b = (uint8_t)((L << 0) | ((((ins >> 24) & 1)) << 1) |
                         ((((ins >> 23) & 1)) << 2) | ((((ins >> 21) & 1)) << 3) |
                         ((((ins >> 22) & 1)) << 4));        /* I: register */
        if ((ins >> 22) & 1) {
            o->rm = (uint8_t)(ins & 0xf);
        } else {
            o->imm = (uint32_t)(((ins >> 8) & 0xf) << 4) | (ins & 0xf);
        }
        return;
    }
    /* block transfer: 100 P U S W L Rn reglist */
    if (op1 == 4) {
        o->op = AOP_ARM_LDM;
        o->rn = (uint8_t)rn;
        o->a = (uint8_t)((ins >> 20) & 1);                   /* load */
        o->b = (uint8_t)((((ins >> 24) & 1) << 0) |          /* pre  */
                         ((((ins >> 23) & 1)) << 1) |        /* up   */
                         ((((ins >> 21) & 1)) << 2));        /* writeback */
        o->imm = ins & 0xffff;
        return;
    }
    /* branch: 101 L offset24 */
    if (op1 == 5) {
        int32_t off = (int32_t)(ins & 0xffffff);
        off = (off << 8) >> 6;
        uint32_t target = (uint32_t)((int32_t)pc + 8 + off);
        if (cond != 0xe) {
            /* A conditional ARM branch is a superblock exit like Thumb's: the
             * taken path leaves, the fall-through stays in this block. Ending
             * the block here (as the first version did) dropped the
             * fall-through onto whatever followed in the arena. */
            o->op = AOP_BCOND;
            o->a = (uint8_t)cond;
            o->cond = 0xe;
            o->imm = (uint32_t)((int32_t)target - (int32_t)(pc + 4));
            return;
        }
        o->op = AOP_ARM_B;
        o->b = (uint8_t)((ins >> 24) & 1);                   /* link */
        o->imm = target;
        return;
    }
    /* data processing: bits 27..25 are 000 (register) or 001 (immediate).
     * Taking only 000 here left every `add rX, pc, #imm` -- the PLT, i.e. every
     * host crossing -- undecoded. */
    if (op1 == 0 || op1 == 1) {
        uint32_t opc = (ins >> 21) & 0xf;
        uint32_t S = (ins >> 20) & 1;
        uint32_t I = (ins >> 25) & 1;
        o->op = AOP_ARM_DP;
        o->rd = (uint8_t)rd;
        o->rn = (uint8_t)rn;
        o->a = (uint8_t)opc;
        o->b = (uint8_t)((S) | (I << 1) | 0);
        if (I) {
            /* rotated immediate: the rotate count is part of the operand */
            uint32_t imm8 = ins & 0xff, rot = ((ins >> 8) & 0xf) * 2;
            o->imm = (rot == 0) ? imm8
                    : ((imm8 >> rot) | (imm8 << (32 - rot)));
            o->rm = 0xff;
        } else {
            o->rm = (uint8_t)(ins & 0xf);
            if (ins & 0x10u) {
                /* shifted by a register: Rs is bits 11..8, and the 5-bit field
                 * is not an amount at all */
                o->imm = ((uint32_t)((ins >> 5) & 3) << 8) | (ins >> 8 & 0xfu);
                o->b = (uint8_t)(o->b | 8);
            } else {
                o->imm = ((uint32_t)((ins >> 5) & 3) << 8) | ((ins >> 7) & 0x1f);
            }
        }
        return;
    }
    /* the ARM condition code travels in the `a` field for every form above
     * except DP/LDST/LDM/MUL, which use `b` for their own bits */
    o->a = (o->a & 0x3f);
    o->imm = o->imm;
}

/* Thumb-2: movw/movt and the wide data-processing forms the image uses. */
static int dec_thumb2(uint32_t pc, uint32_t h1, uint32_t h2, air *o)
{
    o->pc = pc;
    o->insn = (uint16_t)h1;
    o->len = 4;
    o->cnt = 2;
    o->cond = 0xe;
    o->op = AOP_UNSUP;
    o->imm = 0; o->rd = o->rn = o->rm = 0; o->a = o->b = 0;

    /* movw/movt: 11110 i 10 x100 imm4 0 imm3 Rd imm8 */
    uint32_t op6 = (h1 >> 4) & 0x3fu;
    if ((h1 & 0xfb00u) == 0xf200u && (op6 == 0x24u || op6 == 0x2cu) &&
        (h2 & 0x8000u) == 0) {
        uint32_t i    = (h1 >> 10) & 1u;
        uint32_t imm4 = h1 & 0xfu;
        uint32_t imm3 = (h2 >> 12) & 7u;
        uint32_t rd   = (h2 >> 8) & 0xfu;
        uint32_t imm8 = h2 & 0xffu;
        o->op = AOP_ARM_T2_MOVW;
        o->rd = (uint8_t)rd;
        o->imm = (imm4 << 12) | (i << 11) | (imm3 << 8) | imm8;
        o->a = (uint8_t)((h1 & 0x80u) ? 1 : 0);              /* 1 = movt */
        return 1;
    }
    /* wide data processing (register): 11101 01 op 0 Rn 0 imm3 Rd imm2 type Rm
     *   h1: 1110101 op(4) S Rn    h2: 0 imm3 Rd imm2 type Rm
     * If S is set the flags come from the shifted operand, which the register
     * form cannot carry here, so only S=0 is translated. */
    if ((h1 & 0xfe00u) == 0xea00u) {
        uint32_t opc = (h1 >> 5) & 0xf;
        uint32_t S   = (h1 >> 4) & 1;
        uint32_t rn  = h1 & 0xf;
        uint32_t rd  = (h2 >> 8) & 0xf;
        uint32_t rm  = h2 & 0xf;
        uint32_t typ = (h2 >> 4) & 3;
        uint32_t imm2 = (h2 >> 6) & 3;
        uint32_t imm3 = (h2 >> 12) & 7;
        uint32_t amount = (imm3 << 2) | imm2;
        uint32_t setflags = (h2 >> 4) & 1;   /* bit 4 of h2: S in this form */
        if (S) return 0;
        (void)setflags;
        o->op = AOP_ARM_T2_DP;
        o->rd = (uint8_t)rd;
        o->rn = (uint8_t)rn;
        o->rm = (uint8_t)rm;
        o->a = (uint8_t)opc;
        o->b = (uint8_t)((typ << 1) | (S ? 1 : 0));
        o->imm = amount;
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------------- */
/* emitting one instruction                                                  */
/* ------------------------------------------------------------------------- */

static void emit_tst(int reg)
{
    emit(enc_logic_reg(0, 3, 0, R_WZR, reg, reg, 0, 0));   /* ands wzr, w, w */
}

static void emit_mov_reg(int d, int s) { MOVW(d, s); }

static void emit_add_imm(int d, int s, uint32_t imm)
{
    if (imm) emit(enc_addsub_imm(0, 0, 0, d, s, imm));
    else if (d != s) emit_mov_reg(d, s);
}
static void emit_sub_imm(int d, int s, uint32_t imm)
{
    if (imm) emit(enc_addsub_imm(0, 1, 0, d, s, imm));
    else if (d != s) emit_mov_reg(d, s);
}

/* the flags of a partial setter: N and Z are what a `tst` can give */
static void emit_flags_nz(int reg) { emit_tst(reg); cc_kind = CC_NZ; }

/* Store R[15] first, so a block always names itself in crash reports and in
 * aot_verify(). */
static void emit_guest_pc(uint32_t pc)
{
    rc_sync();
    emit_mov_const(9, pc);
    emit(enc_ldst_imm(2, 0, 9, 28, 15));
}

static void seq_log(uint32_t pc);
static uint32_t g_seq_pc;

/* Called at a block entry that was split out because it is the --stop-at
 * address. Returns 1 when this is that address (and consumes the one-shot). */
static int aot_stop_check(uint32_t pc)
{
    return cpu_stop_hit(pc);
}

static int aot_stop_check(uint32_t pc);

static void aot_block_log(uint32_t pc)
{
    host_log("aot: block %08x\n", pc);
}

/* ---- runtime entry points the generated code calls ---------------------- */

static uint32_t *aot_enter(uint32_t pcraw);
static uint32_t *aot_step(uint32_t pcraw);
static uint32_t *aot_host_call(uint32_t pc);
static int       aot_halt_reason(void);
static int       aot_check_one(uint32_t pc, uint32_t prev);
static int       aot_check_branch(uint32_t pc, uint32_t prev);
static int       aot_check_step(uint32_t pc, uint32_t prev);
static int       shadow_step(uint32_t pcraw);
extern uint32_t  g_expect_next;
static void      emit_term_check(uint32_t pc);
static void      emit_cbnz_w0_to_abort(void);

static uint32_t *g_abort_stub;        /* arena prologue: call reason; exit    */
static uint32_t *g_halt_budget_stub;  /* arena prologue: w0 = HALT_BUDGET     */
static uint32_t *g_halt_stop_stub;    /* arena prologue: w0 = HALT_STOPPED    */
static uint32_t *g_verify_stop_stub;  /* arena prologue: w0 = AOT_VERIFY_STOP */
static int       g_halt;              /* HALT_* recorded by a fallback/host   */
static int       g_trace_blocks;      /* AOT_TRACE=1: log every block entered */
static const char *g_dump_code;       /* AOT_DUMP_CODE=path: append each block */
static int       g_verify, g_verify_active;
static uint32_t  g_verify_next;

/* the entry stub's per-run context (pinned registers are loaded from here) */
struct aot_ctx {
    uint64_t text_lo;
    uint64_t disp;
    uint64_t entries;
    uint64_t insn_count;
    uint64_t mem;
    uint64_t cpu;
};
extern int aot_exec(uint32_t *code, uint64_t budget, struct aot_ctx *ctx);

/* ---- block state -------------------------------------------------------- */

static int      g_block_thumb;        /* mode of the block being translated   */
static uint32_t g_last_block_pc;      /* for the no-decode report             */
static int      g_fail;               /* block cannot be translated           */
static int      g_fail_why;           /* 1 = op, 2 = flags, 3 = limit         */
static uint32_t g_fail_pc;
static uint8_t  g_fail_op;
static uint16_t g_fail_insn;
static const char *g_why[4];

struct fixup { uint32_t *at; uint32_t pc; uint32_t check_pc; uint32_t prev_pc;
               uint32_t pend[8]; int npend; int cond; };
static struct fixup g_fix[AOT_MAX_BLOCK];
static int g_nfix;

/* A flag producer whose per-instruction check was skipped: its consumer's
 * check steps it on the interpreter instead (see aot_check_one). */
static uint32_t g_pending_prev;      /* kept for the single-producer case */
static uint32_t g_pend[8];           /* skipped producers, in order        */
static int      g_npend;

/* ---- constants, calls, and the block prologue --------------------------- */

/* 64-bit host address into xd (movz + up to three movk). */
static void emit_mov_addr64(int rd, uint64_t v)
{
    int first = 1;
    for (int hw = 0; hw < 4; hw++) {
        uint32_t part = (uint32_t)((v >> (hw * 16)) & 0xffffu);
        if (first) {
            if (!part && hw < 3) continue;
            emit(enc_movwide(1, 2, hw, part, rd));
            first = 0;
        } else {
            emit(enc_movwide(1, 3, hw, part, rd));
        }
    }
}

/* Call a C function at `fn`. A direct `bl` when the arena and the binary are
 * within reach of a 26-bit branch, otherwise movz/movk + blr. */
static void emit_call(void *fn)
{
    rc_sync();              /* the callee sees g_cpu, not the slot */
    intptr_t here = (intptr_t)g_emit;
    intptr_t off = ((intptr_t)fn - here);
    if (off > -(128 * 1024 * 1024) && off < (128 * 1024 * 1024) && (off & 3) == 0) {
        emit(enc_bl((int32_t)(off / 4)));
    } else {
        emit_mov_addr64(9, (uint64_t)(uintptr_t)fn);
        emit(enc_blr(9));
    }
}

static void emit_term_check(uint32_t pc)
{
    if (!g_check) return;   /* g_block_thumb is set by translate() */
    emit_mov_const(0, pc | (g_block_thumb ? 1u : 0u));
    emit_mov_const(1, g_pending_prev);
    emit_call((void *)aot_check_step);
    emit_cbnz_w0_to_abort();
    g_pending_prev = 0;
}

/* Step every skipped flag producer on the checker's copy, in order. Each call
 * carries a pc, so the arrival check runs for each one. */
static void emit_pending_steps(void)
{
    if (!g_check) return;
    for (int i = 0; i < g_npend; i++) {
        emit_mov_const(0, g_pend[i] | (uint32_t)(g_block_thumb ? 1 : 0));
        emit_mov_const(1, 0);
        emit_call((void *)aot_check_step);
        emit_cbnz_w0_to_abort();
    }
    g_npend = 0;
    g_pending_prev = 0;
}

/* Branch to the shared abort stub when w0 is non-zero. cbz/cbnz has a +/-1 MB
 * range and the arena is 56 MB, so a block near the end encoded an offset that
 * wrapped: the guest jumped into arena memory that was never written and the
 * host faulted fetching zeroes. A short inverted branch over a `b` (which has
 * +/-128 MB) is one word longer and cannot wrap. */
static void emit_cbnz_w0_to_abort(void)
{
    emit(enc_cb(0, 0, 0, 2));                       /* cbz w0, +2 (skip the b) */
    intptr_t off = ((intptr_t)g_abort_stub - (intptr_t)g_emit) / 4;
    emit(enc_b((int32_t)off));
}

/* One guest instruction retired. A superblock only executes the instructions
 * before its first taken conditional branch, so the pre-scanned block length is
 * NOT how many retired: counting per instruction is what makes g_insn_count
 * exact. */
static void emit_count(uint32_t n)
{
    emit(enc_addsub_imm(1, 0, 0, 25, 25, n));       /* add x25, x25, #n */
}

/* The budget is checked on the way out of a block, so the count is exact and the
 * halt point is a block start. */
static void emit_budget_check(void)
{
    rc_flush();     /* the halt stub leaves the run; g_cpu must be current */
    emit(enc_addsub_reg(1, 1, 1, R_WZR, 25, 24, 0, 0));   /* cmp x25, x24 */
    intptr_t off = ((intptr_t)g_halt_budget_stub - (intptr_t)g_emit) / 4;
    emit(enc_bcond(2, (int32_t)off));                     /* b.hs halt    */
}

static void emit_fail(const air *i, int why)
{
    if (!g_fail) {
        g_fail = 1; g_fail_why = why; g_fail_pc = i->pc;
        g_fail_op = i->op; g_fail_insn = i->insn;
    }
}

/* A structured exit: x0 gets the guest address, aot_enter turns it into code. */
/* For the interworking forms: the target mode is part of the instruction. */
static void emit_exit_to_raw(uint32_t target)
{
    emit_flags_to_cpsr();
    rc_sync();              /* the target block reads g_cpu */
    emit_budget_check();
    emit_mov_const(0, target);
    emit_call((void *)aot_enter);
    {
        /* cbz/cbnz has +/-1 MB of range and the arena is 56 MB: a block near
         * the end encoded an offset that wrapped and jumped into arena memory
         * that was never written. Inverted branch over a b. */
        emit(enc_cb(1, 1, 0, 2));                   /* cbnz x0, +2 */
        intptr_t off = ((intptr_t)g_abort_stub - (intptr_t)g_emit) / 4;
        emit(enc_b((int32_t)off));
    }
    emit(enc_br(0));
}

/* An indirect exit: the target is only known at run time (`bx lr`, a return
 * through `pop {pc}`, a call through a function pointer). These are the most
 * common exits in real code -- every function return is one -- and they used to
 * take a host call into aot_enter for a dispatch-table lookup the generated
 * code can do itself: x20 is the table, x21 its entry count, x19 the text base,
 * all three pinned and otherwise unused.
 *
 * The lookup is the same expression disp_get() evaluates:
 *   index = ((pc & ~1) - text_lo) / 2;  index < entries;  code = table[index]
 * Bit 0 is the mode bit, and shifting it out is exactly what disp_get's >>1
 * does for an even address, so the shift is all that is needed. */
static void emit_exit_indirect(int wreg, int force_thumb)
{
    /* Flags first: the budget check below is a cmp and would destroy PSTATE. */
    emit_flags_to_cpsr();
    rc_sync();              /* the target block reads g_cpu */
    emit_budget_check();
    emit_mov_reg(0, wreg);
    if (force_thumb)
        emit(enc_addsub_imm(0, 0, 0, 0, 0, 1));     /* add w0, w0, #1 */

    emit(enc_bitfield(0, 2, 11, 0, 1, 30));         /* ubfx w11, w0, #1, #31 */

    uint32_t *b_slow = g_emit;
    emit(enc_bcond(2, 0));                          /* b.hs slow, patched later */
    emit(enc_addsub_reg(0, 0, 0, 11, 11, 19, 0, 0));/* sub w11, w11, w19       */
    emit(enc_addsub_reg(0, 1, 1, R_WZR, 11, 21, 0, 0)); /* cmp w11, w21        */
    {
        uint32_t *g2 = g_emit;
        emit(enc_bcond(2, 0));                      /* b.hs slow, patched later */
        emit(enc_ldst_reg(3, 1, 9, 20, 11, 2, 0, 3)); /* ldr x9,[x20,w11,uxtw #3] */
        uint32_t *nul = g_emit;
        emit(enc_cb(1, 0, 9, 0));                   /* cbz x9, slow, patched    */
        emit(enc_br(9));
        uint32_t *slow = g_emit;
        *b_slow = enc_bcond(2, (int32_t)(slow - b_slow));
        *g2     = enc_bcond(2, (int32_t)(slow - g2));
        *nul    = enc_cb(1, 0, 9, (int32_t)(slow - nul));
    }
    emit_call((void *)aot_enter);
    {
        emit(enc_cb(1, 1, 0, 2));                   /* cbnz x0, +2 */
        intptr_t off = ((intptr_t)g_abort_stub - (intptr_t)g_emit) / 4;
        emit(enc_b((int32_t)off));
    }
    emit(enc_br(0));
}

static void decode_any(uint32_t pc, int thumb, air *a);

/* Does the block at target begin by reading the flags? If it does and it
 * cannot be translated, the interpreter runs it, and the interpreter reads
 * g_cpu.cpsr -- which AOT code never maintains. So materialise it first. */
static int target_reads_flags(uint32_t target, int thumb)
{
    air t;
    decode_any(target, thumb, &t);
    return insn_reads_flags(&t);
}

/* The slow path taken by an exit whose target block does not exist yet: ask
 * aot_enter for it, and if it hands back code (which also translates it, and so
 * patches `slot` to branch straight there) continue in it. The slot is one
 * word: a `b` to the next instruction until it is patched, which is why the
 * block still runs correctly on the very first arrival. */
static void emit_exit_slow(uint32_t *slot, uint32_t target, uint32_t raw,
                           int needs_flags)
{
    *slot = enc_b(1);                               /* b +1: fall into the slow path */
    if (g_nlink < AOT_MAX_LINKS) {
        g_link[g_nlink].slot = slot;
        g_link[g_nlink].target = target;
        g_nlink++;
    }
    /* Before the budget check: `cmp x25, x24` overwrites NZCV, and this is the
     * last point where the block's own flags are still in PSTATE. */
    if (needs_flags) emit_flags_to_cpsr();
    emit_mov_const(0, raw);
    emit_call((void *)aot_enter);
    {
        /* cbz/cbnz has +/-1 MB of range and the arena is 56 MB: a block near
         * the end encoded an offset that wrapped and jumped into arena memory
         * that was never written. Inverted branch over a b. */
        emit(enc_cb(1, 1, 0, 2));                   /* cbnz x0, +2 */
        intptr_t off = ((intptr_t)g_abort_stub - (intptr_t)g_emit) / 4;
        emit(enc_b((int32_t)off));
    }
    emit(enc_br(0));
}

/* A static exit. Once the target has been translated this is a single `b`:
 * no host call, no dispatch-table lookup, no range check.
 *
 * A direct `b` keeps PSTATE live, which is exactly what a translated target
 * wants -- but a target whose first instruction *reads* flags can never be
 * translated (it would need a flag the block did not produce), so it stays a
 * fallback block and the interpreter reads g_cpu.cpsr instead. Those, and only
 * those, still publish the flags and take the slow path. */
static void emit_exit_to(uint32_t target)
{
    int needs_flags = target_reads_flags(target, g_block_thumb);
    if (needs_flags) emit_flags_to_cpsr();
    rc_sync();              /* the target block reads g_cpu */
    /* The budget is checked on *backward* edges only: an unconditional walk
     * forward cannot be unbounded, and the whole point of the check is to stop
     * a guest loop. Every loop has a backward edge, so the run still stops
     * within one block of the budget -- which the gates measure. */
    if (target <= g_block_pc) emit_budget_check();
    uint32_t *known = needs_flags ? NULL : disp_get(target);
    if (known) {
        emit(enc_b((int32_t)(known - g_emit)));
        return;
    }
    uint32_t *slot = g_emit;
    emit(enc_b(1));                                 /* placeholder, see above */
    emit_exit_slow(slot, target, target | (g_block_thumb ? 1u : 0u), 0);
}

/* AOT_DUMP_CODE=path: append each translated block so a fault inside generated
 * code can be disassembled. */
static void aot_dump_block(const uint32_t *from, const uint32_t *to, uint32_t pc)
{
    if (!g_dump_code) return;
    FILE *f = fopen(g_dump_code, "ab");
    if (!f) return;
    uint32_t hdr[2] = { pc, (uint32_t)(to - from) };
    fwrite(hdr, sizeof hdr, 1, f);
    fwrite(from, 4, (size_t)(to - from), f);
    fclose(f);
}

/* ---- one instruction ---------------------------------------------------- */

#define SZ_B 0
#define SZ_H 1
#define SZ_W 2

/* Park an exit target in w23: it is callee-saved, so the checker call on the
 * way out cannot clobber it (w0-w18 are all caller-saved). */
static void emit_park_target(int wreg) { emit_mov_reg(23, wreg); }

static int emit_arm_insn(const air *i);

static int emit_one(const air *i)
{
    if (i->op >= AOP_ARM_DP)
        return emit_arm_insn(i);

    switch (i->op) {
    case AOP_SHIFT: {
        G_LD(9, i->rm);
        uint32_t s = i->imm;
        if (i->a == 0) {
            if (s) emit(enc_bitfield(0, 2, 9, 9, (int)((32 - s) & 31), 31 - (int)s));
        } else if (i->a == 1) {
            if (s) emit(enc_bitfield(0, 2, 9, 9, (int)s, 31));
            else emit_mov_const(9, 0);
        } else {
            if (s) emit(enc_bitfield(0, 0, 9, 9, (int)s, 31));
            else emit(enc_bitfield(0, 0, 9, 9, 31, 31));
        }
        G_ST(i->rd, 9);
        emit_flags_nz(9);
        break;
    }

    case AOP_ADDSUB: {
        G_LD(9, i->rn);
        if (i->a) emit_mov_const(10, i->imm);
        else G_LD(10, i->rm);
        emit(enc_addsub_reg(0, i->b, 1, 9, 9, 10, 0, 0));
        G_ST(i->rd, 9);
        cc_kind = CC_EXACT;
        break;
    }

    case AOP_IMM8:
        switch (i->a) {
        case 0:                                     /* mov */
            emit_mov_const(9, i->imm);
            G_ST(i->rd, 9);
            emit_flags_nz(9);
            break;
        case 1:                                     /* cmp */
            G_LD(9, i->rd);
            emit_mov_const(10, i->imm);
            emit(enc_addsub_reg(0, 1, 1, R_WZR, 9, 10, 0, 0));
            cc_kind = CC_EXACT;
            break;
        case 2:                                     /* add */
            G_LD(9, i->rd);
            emit(enc_addsub_imm(0, 0, 1, 9, 9, i->imm));
            G_ST(i->rd, 9);
            cc_kind = CC_EXACT;
            break;
        default:                                    /* sub */
            G_LD(9, i->rd);
            emit(enc_addsub_imm(0, 1, 1, 9, 9, i->imm));
            G_ST(i->rd, 9);
            cc_kind = CC_EXACT;
            break;
        }
        break;

    case AOP_ALU: {
        int rd = i->rd, rm = i->rm;
        switch (i->a) {
        case 0x0: G_LD(9, rd); G_LD(10, rm);
                  emit(enc_logic_reg(0, 0, 0, 9, 9, 10, 0, 0)); break;
        case 0x1: G_LD(9, rd); G_LD(10, rm);
                  emit(enc_logic_reg(0, 2, 0, 9, 9, 10, 0, 0)); break;
        case 0x2: G_LD(9, rd); G_LD(10, rm);
                  emit(enc_dp2(0, 10, DP2_LSLV, 9, 9)); break;
        case 0x3: G_LD(9, rd); G_LD(10, rm);
                  emit(enc_dp2(0, 10, DP2_LSRV, 9, 9)); break;
        case 0x4: G_LD(9, rd); G_LD(10, rm);
                  emit(enc_dp2(0, 10, DP2_ASRV, 9, 9)); break;
        case 0x5:                                   /* ADC: Rd+Rm+C  */
        case 0x6: {                                 /* SBC: Rd-Rm-!C */
            /* Both read C, so they need flags from an exact setter. aarch64
             * has ADC/SBC with the carry chain built in (`adcs`/`sbcs`);
             * emulating them with cset plus two subtracts loses the first
             * subtract's borrow, which is what made 0-1 with C=1 come out with
             * C still set and every 64-bit compare branch the wrong way. */
            if (cc_kind != CC_EXACT) { emit_fail(i, 2); return -1; }
            G_LD(9, rd); G_LD(10, rm);
            emit(enc_adcsbc(0, i->a == 0x6, 1, 9, 9, 10));
            cc_kind = CC_EXACT;
            G_ST(rd, 9);
            goto done;
        }
        case 0x7: G_LD(9, rd); G_LD(10, rm);
                  emit(enc_dp2(0, 10, DP2_RORV, 9, 9)); break;
        case 0x8: G_LD(9, rd); G_LD(10, rm);        /* TST */
                  emit(enc_logic_reg(0, 3, 0, R_WZR, 9, 10, 0, 0));
                  cc_kind = CC_NZ; goto done;
        case 0x9: G_LD(10, rm);                     /* RSB */
                  emit(enc_addsub_reg(0, 1, 1, 9, R_WZR, 10, 0, 0));
                  G_ST(rd, 9); cc_kind = CC_EXACT; goto done;
        case 0xa: G_LD(9, rd); G_LD(10, rm);        /* CMP */
                  emit(enc_addsub_reg(0, 1, 1, R_WZR, 9, 10, 0, 0));
                  cc_kind = CC_EXACT; goto done;
        case 0xb: G_LD(9, rd); G_LD(10, rm);        /* CMN */
                  emit(enc_addsub_reg(0, 0, 1, R_WZR, 9, 10, 0, 0));
                  cc_kind = CC_EXACT; goto done;
        case 0xc: G_LD(9, rd); G_LD(10, rm);        /* ORR */
                  emit(enc_logic_reg(0, 1, 0, 9, 9, 10, 0, 0)); break;
        case 0xd: G_LD(9, rd); G_LD(10, rm);        /* MUL */
                  emit(enc_madd(0, 9, 9, 10, R_WZR)); break;
        case 0xe: G_LD(9, rd); G_LD(10, rm);        /* BIC */
                  emit(enc_logic_reg(0, 0, 1, 9, 9, 10, 0, 0)); break;
        default:  G_LD(10, rm);                     /* MVN */
                  emit(enc_logic_reg(0, 1, 1, 9, R_WZR, 10, 0, 0)); break;
        }
        G_ST(rd, 9);
        emit_flags_nz(9);
    done:
        break;
    }

    case AOP_HI: {
        int rd = i->rd, rm = i->rm;
        if (i->a == 1) {                            /* cmp rH, rH: flags */
            G_LD(9, rd);
            if (rm == 15) emit_mov_const(10, i->pc + 4); else G_LD(10, rm);
            emit(enc_addsub_reg(0, 1, 1, R_WZR, 9, 10, 0, 0));
            cc_kind = CC_EXACT;
            break;
        }
        G_LD(9, rd);
        if (rm == 15) emit_mov_const(10, i->pc + 4); else G_LD(10, rm);
        if (i->a == 0) emit(enc_addsub_reg(0, 0, 0, 9, 9, 10, 0, 0));
        else emit_mov_reg(9, 10);
        G_ST(rd, 9);
        if (rd == 15) {                             /* computed jump */
            emit_park_target(9);
            emit_term_check(i->pc);
            /* A Thumb `mov pc, rX` keeps the state: it is not an interworking
             * branch, so the target is entered as Thumb even when bit 0 is
             * clear -- which is exactly what a jump table produces. Entering
             * libpng's switch tables in ARM mode decoded `mov r0, sl` as an
             * ARM instruction and halted at the first switch. */
            emit_exit_indirect(23, 1);
            return 1;
        }
        break;
    }

    case AOP_BX: {
        if (i->rm == 15) emit_mov_const(23, i->pc + 4); else G_LD(23, i->rm);
        if (i->insn & 0x0080) {                     /* BLX: set LR */
            emit_mov_const(10, (i->pc + 2) | 1u);
            G_ST(14, 10);
        }
        emit_term_check(i->pc);
        emit_exit_indirect(23, 0);
        return 1;
    }

    case AOP_LDRLIT:
        emit_mov_const(9, i->imm);
        M_LD(SZ_W, 10, 9);
        G_ST(i->rd, 10);
        break;

    case AOP_LDST_REG: {
        G_LD(9, i->rn);
        G_LD(10, i->rm);
        emit(enc_addsub_reg(0, 0, 0, 9, 9, 10, 0, 0));
        switch (i->a) {
        case 0: G_LD(11, i->rd); M_ST(SZ_W, 11, 9); break;
        case 1: G_LD(11, i->rd); M_ST(SZ_H, 11, 9); break;
        case 2: G_LD(11, i->rd); M_ST(SZ_B, 11, 9); break;
        case 3: M_LDS(SZ_B, 11, 9);
                emit(enc_bitfield(0, 0, 11, 11, 0, 7));
                G_ST(i->rd, 11); break;
        case 4: M_LD(SZ_W, 11, 9); G_ST(i->rd, 11); break;
        case 5: M_LD(SZ_H, 11, 9); G_ST(i->rd, 11); break;
        case 6: M_LD(SZ_B, 11, 9); G_ST(i->rd, 11); break;
        default: M_LD(SZ_H, 11, 9);
                 emit(enc_bitfield(0, 0, 11, 11, 0, 15));
                 G_ST(i->rd, 11); break;
        }
        break;
    }

    case AOP_LDST_IMM: {
        uint32_t scale = (i->a == 2) ? 0 : (i->a == 0 ? 2 : 1);
        G_LD(9, i->rn);
        emit_add_imm(9, 9, i->imm << scale);
        if (i->a == 0) {
            if (i->b) { M_LD(SZ_W, 11, 9); G_ST(i->rd, 11); }
            else { G_LD(11, i->rd); M_ST(SZ_W, 11, 9); }
        } else if (i->a == 1) {
            if (i->b) { M_LD(SZ_H, 11, 9); G_ST(i->rd, 11); }
            else { G_LD(11, i->rd); M_ST(SZ_H, 11, 9); }
        } else {
            if (i->b) { M_LD(SZ_B, 11, 9); G_ST(i->rd, 11); }
            else { G_LD(11, i->rd); M_ST(SZ_B, 11, 9); }
        }
        break;
    }

    case AOP_LDST_SP: {
        G_LD(9, 13);
        emit_add_imm(9, 9, i->imm << 2);
        if (i->b) { M_LD(SZ_W, 11, 9); G_ST(i->rd, 11); }
        else { G_LD(11, i->rd); M_ST(SZ_W, 11, 9); }
        break;
    }

    case AOP_ADDR:
        if (i->b) G_LD(9, 13);
        else emit_mov_const(9, (i->pc + 4) & ~3u);
        emit_add_imm(9, 9, i->imm);
        G_ST(i->rd, 9);
        break;

    case AOP_ADJSP:
        G_LD(9, 13);
        if (i->b) emit_sub_imm(9, 9, i->imm);
        else emit_add_imm(9, 9, i->imm);
        G_ST(13, 9);
        break;

    case AOP_PUSH: {
        uint32_t list = i->imm, n = 0;
        for (uint32_t r = 0; r < 8; r++) if (list & (1u << r)) n++;
        if (i->b) n++;
        G_LD(9, 13);
        emit_sub_imm(9, 9, 4 * n);
        G_ST(13, 9);
        for (uint32_t r = 0; r < 8; r++)
            if (list & (1u << r)) { G_LD(10, (int)r); M_ST(SZ_W, 10, 9); emit_add_imm(9, 9, 4); }
        if (i->b) { G_LD(10, 14); M_ST(SZ_W, 10, 9); }
        break;
    }

    case AOP_POP: {
        uint32_t list = i->imm;
        G_LD(9, 13);
        for (uint32_t r = 0; r < 8; r++)
            if (list & (1u << r)) {
                M_LD(SZ_W, 10, 9);
                G_ST((int)r, 10);
                emit_add_imm(9, 9, 4);
            }
        if (i->b) {
            M_LD(SZ_W, 11, 9);
            emit_add_imm(9, 9, 4);
            G_ST(13, 9);
            emit_park_target(11);
            emit_term_check(i->pc);
            emit_exit_indirect(23, 0);
            return 1;
        }
        G_ST(13, 9);
        break;
    }

    case AOP_REV:
        G_LD(9, i->rm);
        switch (i->a) {
        case 0: emit(enc_rev(0, REV_REV32, 9, 10)); break;
        case 1: emit(enc_rev(0, REV_REV16, 9, 10)); break;
        default:
            emit(enc_rev(0, REV_REV16, 9, 10));
            emit(enc_bitfield(0, 0, 10, 10, 0, 15));
            break;
        }
        G_ST(i->rd, 10);
        break;

    case AOP_EXT:
        G_LD(9, i->rm);
        switch (i->a) {
        case 0: emit(enc_bitfield(0, 0, 10, 9, 0, 15)); break;   /* sxth */
        case 1: emit(enc_bitfield(0, 0, 10, 9, 0, 7)); break;    /* sxtb */
        case 2: emit(enc_bitfield(0, 2, 10, 9, 0, 15)); break;   /* uxth */
        default: emit(enc_bitfield(0, 2, 10, 9, 0, 7)); break;   /* uxtb */
        }
        G_ST(i->rd, 10);
        break;

    case AOP_NOP:
        break;

    case AOP_LDMIA:
    case AOP_STMIA: {
        int load = (i->op == AOP_LDMIA);
        G_LD(9, i->rn);
        for (uint32_t r = 0; r < 8; r++)
            if (i->imm & (1u << r)) {
                if (load) { M_LD(SZ_W, 10, 9); G_ST((int)r, 10); }
                else { G_LD(10, (int)r); M_ST(SZ_W, 10, 9); }
                emit_add_imm(9, 9, 4);
            }
        G_ST(i->rn, 9);
        break;
    }

    case AOP_BCOND: {
        if (cond_bits(i->a) & ~cc_provides()) { emit_fail(i, 2); return -1; }
        if (g_nfix >= AOT_MAX_BLOCK) { emit_fail(i, 3); return -1; }
        /* Publish the deferred stores BEFORE g_fix[].at is captured. The fixup
         * rewrites the word at that address into the branch, so anything emitted
         * between the capture and the branch would be overwritten -- the branch
         * then became 'b .' (offset zero) and the guest hung. */
        rc_flush();
        uint32_t target = (uint32_t)((int32_t)(i->pc + 4) + (int32_t)i->imm);
        g_fix[g_nfix].at = g_emit;
        g_fix[g_nfix].pc = target;
        g_fix[g_nfix].cond = i->a;
        g_fix[g_nfix].check_pc = i->pc;
        g_fix[g_nfix].prev_pc = g_pending_prev;   /* the taken path needs it too */
        g_fix[g_nfix].npend = g_npend;
        for (int i = 0; i < g_npend; i++) g_fix[g_nfix].pend[i] = g_pend[i];
        g_nfix++;
        emit(enc_bcond(0, 0));                      /* patched after the body */
        if (g_check) {
            /* Both paths run the checker, and it has to be *after* the branch:
             * the call is C, and PSTATE is where the condition lives. The
             * taken path's stub does the same before it dispatches. */
            emit_pending_steps();
            emit_mov_const(0, i->pc | (g_block_thumb ? 1u : 0u));
            emit_mov_const(1, 0);
            emit_call((void *)aot_check_branch);
            emit_cbnz_w0_to_abort();
        }
        break;
    }

    case AOP_B:
        emit_term_check(i->pc);
        emit_exit_to((uint32_t)((int32_t)(i->pc + 4) + (int32_t)i->imm));
        return 1;

    case AOP_BL:
        emit_term_check(i->pc);
        emit_mov_const(10, (i->pc + 4) | 1u);
        G_ST(14, 10);
        emit_exit_to(i->imm);
        return 1;

    case AOP_BLX:
        /* Thumb BLX: a call into ARM. LR keeps the Thumb bit (the return lands
         * in Thumb code), the target does not. */
        emit_term_check(i->pc);
        emit_mov_const(10, (i->pc + 4) | 1u);
        G_ST(14, 10);
        emit_exit_to_raw(i->imm & ~3u);
        return 1;

    case AOP_BKPT: {
        /* No emit_term_check here: aot_host_call() steps the checker's copy
         * across the stub itself (it is the one place that knows where the
         * return address came from), and stepping it twice desynchronised
         * every comparison after the first import. */
        emit_budget_check();
        emit(enc_ldst_imm(3, 0, 25, 26, 0));        /* str x25, [x26] */
        emit_mov_const(0, i->pc);
        emit_call((void *)aot_host_call);
        emit(enc_ldst_imm(3, 1, 25, 26, 0));        /* ldr x25, [x26] */
    {
        /* cbz/cbnz has +/-1 MB of range and the arena is 56 MB: a block near
         * the end encoded an offset that wrapped and jumped into arena memory
         * that was never written. Inverted branch over a b. */
        emit(enc_cb(1, 1, 0, 2));                   /* cbnz x0, +2 */
        intptr_t off = ((intptr_t)g_abort_stub - (intptr_t)g_emit) / 4;
        emit(enc_b((int32_t)off));
    }
        emit(enc_br(0));
        return 1;
    }

    default:                                        /* SWI / UNDEF / BLX / T2 */
        emit_fail(i, 1);
        return -1;
    }
    return 0;
}

/* ---- a block ------------------------------------------------------------ */

/* A block we could not translate: interpret one instruction and come back. */
static uint32_t *emit_fallback_block(uint32_t pcraw)
{
    uint32_t *start = g_emit;
    emit_budget_check();
    emit(enc_ldst_imm(3, 0, 25, 26, 0));            /* str x25, [x26] */
    if (g_seq) {
        /* A fallback instruction runs on the interpreter, so without this the
         * AOT_SEQ stream silently skips it and a stream comparison reports a
         * divergence that is only a log artefact. */
        emit_mov_const(0, pcraw);
        emit_call((void *)seq_log);
    }
    emit_mov_const(0, pcraw);
    emit_call((void *)aot_step);
    /* The interpreter owned the counter while that instruction ran (and a shim
     * may have run a nested one), so the AOT copy is stale. */
    emit(enc_ldst_imm(3, 1, 25, 26, 0));            /* ldr x25, [x26] */
    {
        /* cbz/cbnz has +/-1 MB of range and the arena is 56 MB: a block near
         * the end encoded an offset that wrapped and jumped into arena memory
         * that was never written. Inverted branch over a b. */
        emit(enc_cb(1, 1, 0, 2));                   /* cbnz x0, +2 */
        intptr_t off = ((intptr_t)g_abort_stub - (intptr_t)g_emit) / 4;
        emit(enc_b((int32_t)off));
    }
    emit(enc_br(0));
    __builtin___clear_cache((char *)start, (char *)g_emit);
    return start;
}

/* ---- emitting the 32-bit instruction set -------------------------------- */

/* operand2 for a data-processing instruction: the value goes to w10, and the
 * shifter carry (for the logical-with-S forms) is not modelled -- those make the
 * block untranslatable when a consumer follows, which is exact and rare. */
static int emit_arm_op2(const air *i, int *used_rm)
{
    if (i->b & 2) {                                 /* rotated immediate */
        emit_mov_const(10, i->imm);
        *used_rm = 0;
        return 0;
    }
    uint32_t type = (i->imm >> 8) & 3;
    if (i->rm == 15) return -1;
    G_LD(12, i->rm);
    *used_rm = i->rm;
    if (i->b & 8) {
        /* shift amount from a register. The variable-shift instructions mask
         * the count to 5 bits, while ARM's LSL/LSR by >= 32 yields 0 -- the
         * difference only shows up for counts the compiler does not emit here. */
        G_LD(11, (int)(i->imm & 0xf));
        switch (type) {
        case 0: emit(enc_dp2(0, 11, DP2_LSLV, 12, 10)); break;
        case 1: emit(enc_dp2(0, 11, DP2_LSRV, 12, 10)); break;
        case 2: emit(enc_dp2(0, 11, DP2_ASRV, 12, 10)); break;
        default: emit(enc_dp2(0, 11, DP2_RORV, 12, 10)); break;
        }
        return 0;
    }
    uint32_t amount = i->imm & 0x1f;
    if (type == 0 && amount == 0) { MOVW(10, 12); return 0; }
    emit_mov_const(11, amount ? amount : 32);
    switch (type) {
    case 0: emit(enc_dp2(0, 11, DP2_LSLV, 12, 10)); break;
    case 1: emit(enc_dp2(0, 11, DP2_LSRV, 12, 10)); break;
    case 2: emit(enc_dp2(0, 11, DP2_ASRV, 12, 10)); break;
    default: emit(enc_dp2(0, 11, DP2_RORV, 12, 10)); break;
    }
    return 0;
}

static int emit_arm_insn(const air *i)
{
    uint32_t cond = i->cond;
    uint32_t *skip_at = NULL;

    if (cond != 0xe) {
        if (cond_bits((int)cond) & ~cc_provides()) { emit_fail(i, 2); return -1; }
        /* A conditionally executed body runs on one path only, so a deferred
         * store inside it describes a value the other path never wrote; and
         * rc_alloc may have taken the slot from another guest register, so
         * publishing at the merge would write one register's value into
         * another. Inside such a body the store is immediate. */
        /* Publish and forget BEFORE the guard. A conditionally executed body
         * may write a guest register, and that write clears the slot's dirty
         * flag at *translation* time -- so a deferred store from before the
         * guard would be dropped from the publish list even on the path where
         * the body never runs. Seen as: `movs r3, r0` deferred, `rsbmi r3, r0,
         * #0` immediate inside the body, and the fall-through reading r3 from
         * memory where it still held the old value. */
        rc_sync();
        g_rc_cond++;
        skip_at = g_emit;
        emit(enc_bcond((int)(cond ^ 1u), 0));       /* b.!cond over the body */
    }

    int rc = 0;
    switch (i->op) {
    case AOP_ARM_DP: {
        uint32_t opc = i->a;
        int logical = (opc == DP_AND || opc == DP_EOR || opc == DP_ORR ||
                       opc == DP_BIC || opc == DP_MVN || opc == DP_MOV ||
                       opc == DP_TST || opc == DP_TEQ);
        int sets = (i->b & 1) != 0;
        int rm_used = 0;
        if (emit_arm_op2(i, &rm_used)) { emit_fail(i, 1); return -1; }

        if (opc == DP_MOV || opc == DP_MVN) {
            if (opc == DP_MOV) MOVW(9, 10);
            else emit(enc_logic_reg(0, 1, 1, 9, R_WZR, 10, 0, 0));
        } else if (i->rn == 15) {
            emit_mov_const(9, i->pc + 8);       /* PC as a source operand */
        } else {
            G_LD(9, i->rn);
        }
        switch (opc) {
        case DP_AND: emit(enc_logic_reg(0, 0, 0, 9, 9, 10, 0, 0)); break;
        case DP_EOR: emit(enc_logic_reg(0, 2, 0, 9, 9, 10, 0, 0)); break;
        case DP_ORR: emit(enc_logic_reg(0, 1, 0, 9, 9, 10, 0, 0)); break;
        case DP_BIC: emit(enc_logic_reg(0, 0, 1, 9, 9, 10, 0, 0)); break;
        case DP_MOV: case DP_MVN: break;
        case DP_TST: emit(enc_logic_reg(0, 3, 0, R_WZR, 9, 10, 0, 0)); break;
        case DP_TEQ: /* AArch64 has no EORS: eor then ands to get N,Z */
                     emit(enc_logic_reg(0, 2, 0, 13, 9, 10, 0, 0));
                     emit(enc_logic_reg(0, 3, 0, R_WZR, 13, 13, 0, 0));
                     break;
        case DP_ADD: emit(enc_addsub_reg(0, 0, sets, 9, 9, 10, 0, 0)); break;
        case DP_SUB: emit(enc_addsub_reg(0, 1, sets, 9, 9, 10, 0, 0)); break;
        case DP_RSB: emit(enc_addsub_reg(0, 1, sets, 9, 10, 9, 0, 0)); break;
        case DP_CMP: emit(enc_addsub_reg(0, 1, 1, R_WZR, 9, 10, 0, 0)); break;
        case DP_CMN: emit(enc_addsub_reg(0, 0, 1, R_WZR, 9, 10, 0, 0)); break;
        case DP_ADC: case DP_SBC: {
            /* Reads C, so the block needs an exact provider. `sbcs` does the
             * arithmetic and the flags in one instruction -- the cset plus two
             * subtracts this used to emit lost the first subtract's borrow. */
            if (cc_kind != CC_EXACT) { emit_fail(i, 2); return -1; }
            /* S comes from the instruction: an ARM adc without S must leave the
             * flags alone. Forcing S=1 clobbered the carry the next branch read. */
            emit(enc_adcsbc(0, opc == DP_SBC, sets, 9, 9, 10));
            if (sets) cc_kind = CC_EXACT;
            break;
        }
        default: emit_fail(i, 1); return -1;
        }
        /* write back (MOV/MVN already left it in w9), then the flags */
        if (opc != DP_TST && opc != DP_TEQ && opc != DP_CMP && opc != DP_CMN)
            G_ST(i->rd, 9);
        if (sets) {
            if (logical) {
                /* N,Z come from the result; C would have to come from the
                 * shifter and V must survive, which PSTATE cannot express, so a
                 * block whose consumer needs those is interpreted instead. The
                 * tst is what actually *computes* N and Z: without it a
                 * MOVS/LSRS left PSTATE holding the previous instruction's
                 * flags, and the bne that followed looped on a stale Z. */
                if (opc != DP_TST && opc != DP_TEQ)
                    emit_flags_nz(9);
                else
                    cc_kind = CC_NZ;
            } else {
                cc_kind = CC_EXACT;
            }
        }
        break;
    }

    case AOP_ARM_LDST: {
        int reg_offset = (i->b >> 4) & 1;
        int pre = (i->b >> 1) & 1, up = (i->b >> 2) & 1;
        /* Post-indexed forms (P=0) always write the base back; the W bit only
         * means anything when P=1. Treating W as the only source of writeback
         * left a post-indexed store writing to the same slot every time, so the
         * value another frame expected had been overwritten by the next one. */
        int wb = ((i->b >> 3) & 1) | !pre;
        int load = i->b & 1;
        uint32_t kind = i->a;   /* 0 word, 1 byte, 2 half, 3 ldsb, 4 ldrsh, 5 dword */

        /* base in w14, offset in w12, address in w9. The base is preserved
         * because the writeback must add the offset to the base, not to the
         * address -- adding it to w9 (which already has it) doubled every
         * pre-indexed writeback, and that is what made the ARM PLT leave a
         * different ip behind than the interpreter. */
        if (i->rn == 15) emit_mov_const(14, (i->pc + 8) & ~3u);
        else G_LD(14, i->rn);
        if (reg_offset) G_LD(12, i->rm);

        if (pre) {
            emit_mov_reg(9, 14);
            if (reg_offset) emit(enc_addsub_reg(0, up ? 0 : 1, 0, 9, 9, 12, 0, 0));
            else if (i->imm) emit(enc_addsub_imm(0, up ? 0 : 1, 0, 9, 9, i->imm & 0xfff));
        } else {
            emit_mov_reg(9, 14);            /* post-indexed: access at the base */
        }

        int is_dword = (kind == 5);
        if (is_dword) {
            if (load) {
                M_LD(SZ_W, 10, 9);
                G_ST(i->rd, 10);
                emit_add_imm(9, 9, 4);
                M_LD(SZ_W, 10, 9);
                G_ST(i->rd + 1, 10);
            } else {
                G_LD(10, i->rd); M_ST(SZ_W, 10, 9);
                emit_add_imm(9, 9, 4);
                G_LD(10, i->rd + 1); M_ST(SZ_W, 10, 9);
            }
        } else if (load) {
            if (kind == 0) M_LD(SZ_W, 10, 9);
            else if (kind == 1) M_LD(SZ_B, 10, 9);
            else if (kind == 2) M_LD(SZ_H, 10, 9);
            else if (kind == 3) { M_LDS(SZ_B, 10, 9); emit(enc_bitfield(0, 0, 10, 10, 0, 7)); }
            else { M_LDS(SZ_H, 10, 9); emit(enc_bitfield(0, 0, 10, 10, 0, 15)); }
            if (i->rd == 15) {
                /* LDR pc: the ARM PLT's last instruction and every import
                 * crossing. The target's bit 0 picks ARM or Thumb. */
                G_ST(15, 10);
                if (wb) {
                    if (reg_offset) emit(enc_addsub_reg(0, up ? 0 : 1, 0, 14, 14, 12, 0, 0));
                    else if (i->imm) emit(enc_addsub_imm(0, up ? 0 : 1, 0, 14, 14, i->imm & 0xfff));
                    G_ST(i->rn, 14);
                }
                emit_park_target(10);
                emit_term_check(i->pc);
                emit_exit_indirect(23, 0);
                rc = 1;
                break;
            }
            G_ST(i->rd, 10);
        } else {
            G_LD(10, i->rd);
            if (kind == 0) M_ST(SZ_W, 10, 9);
            else if (kind == 1) M_ST(SZ_B, 10, 9);
            else M_ST(SZ_H, 10, 9);
        }
        if (wb) {
            if (reg_offset) emit(enc_addsub_reg(0, up ? 0 : 1, 0, 14, 14, 12, 0, 0));
            else if (i->imm) emit(enc_addsub_imm(0, up ? 0 : 1, 0, 14, 14, i->imm & 0xfff));
            G_ST(i->rn, 14);
        }
        break;
    }

    case AOP_ARM_LDM: {
        int load = i->a & 1;
        int pre = (i->b >> 0) & 1, up = (i->b >> 1) & 1, wb = (i->b >> 2) & 1;
        uint32_t n = 0;
        for (uint32_t r = 0; r < 16; r++) if (i->imm & (1u << r)) n++;
        if (!n) { emit_fail(i, 1); return -1; }

        /* base in w14, address in w9. The writeback is computed from the base,
         * not from the running address: for stmdb/ldmdb the address walks up
         * from base-4n, so reusing it yields the original base again and a
         * "push" of six registers leaves sp 24 bytes too high. That is the bug
         * that made the guest stack drift and a later malloc receive a garbage
         * size. */
        G_LD(14, i->rn);
        emit_mov_reg(9, 14);
        /* IA/IB/DA/DB: only DB starts below the base (base-4n). "!pre && up"
         * is LDMIA/STMIA, which starts *at* the base -- adding 4 there made
         * every ARM pop read four bytes too high, and the caller then loaded a
         * zero argument off its own stack. */
        if (pre && !up) emit_sub_imm(9, 9, 4 * n);

        int returned = 0;
        for (uint32_t r = 0; r < 16; r++) {
            if (!(i->imm & (1u << r))) continue;
            if (load) { M_LD(SZ_W, 10, 9); G_ST((int)r, 10); }
            else { G_LD(10, (int)r); M_ST(SZ_W, 10, 9); }
            emit_add_imm(9, 9, 4);
            if (r == 15 && load) returned = 1;
        }
        if (wb) {
            if (up) emit_add_imm(14, 14, 4 * n);
            else    emit_sub_imm(14, 14, 4 * n);
            G_ST(i->rn, 14);
        }
        if (returned) {
            G_LD(23, 15);
            emit_term_check(i->pc);
            emit_exit_indirect(23, 0);
            rc = 1;
        }
        break;
    }

    case AOP_ARM_LMUL: {
        G_LD(9, i->rn);                 /* w9  = Rm */
        G_LD(10, i->rm);                /* w10 = Rs */
        int ra;
        if (i->b) {
            G_LD(11, i->rd);            /* RdLo */
            G_LD(12, (int)i->imm);      /* RdHi */
            emit(0xAA0C0000u | (11u << 16) | (12u << 5) | 11u);   /* orr x11, x11, x12, lsl #32 */
            ra = 11;
        } else {
            ra = 31;                    /* xzr */
        }
        emit(enc_lmul(i->a, 13, 9, 10, ra));
        G_ST(i->rd, 13);                /* RdLo */
        emit(0xD34CFC00u | (13u << 5) | 14u);  /* lsr x14, x13, #32 */
        G_ST((int)i->imm, 14);          /* RdHi */
        break;
    }

    case AOP_ARM_MUL:
        G_LD(9, i->rn);
        G_LD(10, i->rm);
        if (i->a) {
            G_LD(11, (int)i->imm);
            emit(enc_madd(0, 9, 9, 10, 11));
        } else {
            emit(enc_madd(0, 9, 9, 10, R_WZR));
        }
        G_ST(i->rd, 9);
        break;

    case AOP_ARM_CLZ:
        /* clz counts in a *host* register: the source is w9 after the load, not
         * the guest register number. Using the guest index there read an
         * unrelated host register (and re-ran clz on its own result). */
        G_LD(9, i->rm);
        emit(0x5ac01000u | (9u << 5) | 10u);              /* clz w10, w9 (sf=0) */
        G_ST(i->rd, 10);
        break;

    case AOP_ARM_B: {
        /* LR is pc+4 with no Thumb bit: an ARM call returns to ARM code. BLX
         * immediate (b == 2) is the one place ARM code reaches Thumb. Only a
         * call writes LR, though: an unconditional `b` that overwrote it broke
         * the tail-call idiom at the end of __aeabi_cdrcmple, where the callee
         * `pop {r0, pc}` then returned to the `b` itself instead of to the
         * caller (r14 diverged by exactly the branch address). */
        if (i->b) {
            emit_mov_const(10, i->pc + 4);
            G_ST(14, 10);
        }
        emit_term_check(i->pc);
        if (i->b == 2) emit_exit_to_raw(i->imm | 1u);
        else           emit_exit_to(i->imm);
        rc = 1;
        break;
    }

    case AOP_ARM_BX: {
        G_LD(23, i->rm);
        if (i->b) {                                 /* BLX: LR = pc+4 */
            emit_mov_const(10, i->pc + 4);
            G_ST(14, 10);
        }
        emit_term_check(i->pc);
        emit_exit_indirect(23, 0);
        rc = 1;
        break;
    }

    case AOP_ARM_T2_MOVW:
        if (i->a) {                                 /* movt */
            G_LD(9, i->rd);
            emit(enc_movwide(0, 3, 1, i->imm, 9));
        } else {
            emit(enc_movwide(0, 2, 0, i->imm, 9));
        }
        G_ST(i->rd, 9);
        break;

    case AOP_ARM_T2_DP: {
        uint32_t opc = i->a, typ = (i->b >> 1) & 3;
        int rd = i->rd, rn = i->rn, rm = i->rm;
        G_LD(9, rn);
        G_LD(10, rm);
        if (typ || i->imm) {
            emit_mov_const(11, i->imm);
            switch (typ) {
            case 0: emit(enc_dp2(0, 11, DP2_LSLV, 10, 10)); break;
            case 1: emit(enc_dp2(0, 11, DP2_LSRV, 10, 10)); break;
            case 2: emit(enc_dp2(0, 11, DP2_ASRV, 10, 10)); break;
            default: emit(enc_dp2(0, 11, DP2_RORV, 10, 10)); break;
            }
        }
        switch (opc) {
        case DP_AND: emit(enc_logic_reg(0, 0, 0, 9, 9, 10, 0, 0)); break;
        case DP_EOR: emit(enc_logic_reg(0, 2, 0, 9, 9, 10, 0, 0)); break;
        case DP_ORR: emit(enc_logic_reg(0, 1, 0, 9, 9, 10, 0, 0)); break;
        case DP_BIC: emit(enc_logic_reg(0, 0, 1, 9, 9, 10, 0, 0)); break;
        case DP_ADD: emit(enc_addsub_reg(0, 0, 0, 9, 9, 10, 0, 0)); break;
        case DP_SUB: emit(enc_addsub_reg(0, 1, 0, 9, 9, 10, 0, 0)); break;
        default: emit_fail(i, 1); return -1;
        }
        G_ST(rd, 9);
        break;
    }

    default:
        emit_fail(i, 1);
        return -1;
    }

    /* Where the not-taken path continues. Normally that is simply the next
     * instruction; for a conditional *terminator* (pople, bxeq, ...) there is no
     * next instruction in this block, so the exit to pc+4 has to be emitted and
     * the skip must land *on* it -- translate() adds no fall-through exit for a
     * block that ended with a terminator, and landing one past the emitted code
     * is what made the CPU execute zeroes and trap as SIGILL. */
    uint32_t *skip_to = g_emit;
    if (rc == 1 && cond != 0xe) {
        skip_to = g_emit;
        emit_exit_to(i->pc + 4);
    }

    if (skip_at) {
        /* The body above only runs when the condition holds, so on the other
         * path the slot still holds whatever was there before. `rc_g` is a
         * translation-time fact about the emitted code, and it would otherwise
         * claim a value that one of the two paths never wrote. */
        rc_sync();
        g_rc_cond--;
        intptr_t off = skip_to - skip_at;           /* words to skip over */
        *skip_at = enc_bcond((int)(cond ^ 1u), (int32_t)off);
    }
    return rc;
}

/* Thumb or ARM, and within Thumb the 32-bit encodings first. */
static void decode_any(uint32_t pc, int thumb, air *a)
{
    if (!thumb) {
        dec_arm(pc, a);
        return;
    }
    uint32_t h1 = LD16(pc);
    if ((h1 & 0xf800u) == 0xe800u || (h1 & 0xf800u) == 0xf000u) {
        uint32_t h2 = LD16(pc + 2);
        if (dec_thumb2(pc, h1, h2, a))
            return;
    }
    dec_thumb(pc, a);
}


/* ---- crash reporting ---------------------------------------------------- */

/* A fault inside generated code reports a host PC. Turning it back into the
 * guest PC of the block is the difference between "something stored through a
 * bad pointer" and "this instruction's base register or write-back is wrong".
 * AOT_DUMP_CODE records the mapping; without a dump the answer is "unknown"
 * and the register file is all there is. */
static struct { uint32_t *start; uint32_t *end; uint32_t pc; } g_blkmap[1 << 16];
static int g_nblkmap;

/* The nearest block at or below the host PC, whether or not the PC is inside
 * it: a jump into the middle of the arena is exactly the case that matters, and
 * an "inside" test reports nothing for it. */
uint32_t *aot_host_pc_block(uintptr_t hp)
{
    uint32_t *best = NULL;
    for (int i = 0; i < g_nblkmap; i++)
        if ((uintptr_t)g_blkmap[i].start <= hp &&
            (!best || g_blkmap[i].start > best))
            best = g_blkmap[i].start;
    return best;
}

uint32_t aot_host_pc_block_end(uint32_t *start)
{
    for (int i = 0; i < g_nblkmap; i++)
        if (g_blkmap[i].start == start) return (uint32_t)(uintptr_t)g_blkmap[i].end;
    return 0;
}

uint32_t aot_block_guest_pc(uint32_t *start)
{
    for (int i = 0; i < g_nblkmap; i++)
        if (g_blkmap[i].start == start)
            return g_blkmap[i].pc;
    return 0;
}

static uint32_t *translate(uint32_t pc, int thumb)
{
    g_block_thumb = thumb;
    g_block_pc = pc;
    air blk[AOT_MAX_BLOCK];
    int n = 0;
    uint32_t p = pc;

    for (; n < AOT_MAX_BLOCK; ) {
        air a;
        if (n && g_stop_at && (g_stop_at & ~1u) == p)
            break;                          /* the stop is an exact boundary */
        decode_any(p, thumb, &a);
        if (a.op == AOP_UNSUP)
            break;
        blk[n++] = a;
        p += a.len;
        if (a.op == AOP_B || a.op == AOP_BL || a.op == AOP_BLX || a.op == AOP_BX ||
            a.op == AOP_BKPT || a.op == AOP_SWI || a.op == AOP_UNDEF ||
            a.op == AOP_ARM_B || a.op == AOP_ARM_BX ||
            (a.op == AOP_POP && a.b) || (a.op == AOP_HI && a.rd == 15) ||
            (a.op == AOP_ARM_LDM && (a.imm & 0x8000u)) ||
            (a.op == AOP_ARM_LDST && a.rd == 15))
            break;
    }

    if (n == 0) {
        air a0;
        decode_any(pc, thumb, &a0);
        host_log("aot: no decode at %08x (thumb %d): op=%02x insn=%08x "
                 "(previous block %08x)\n",
                 pc, thumb, a0.op, LD32(pc & ~3u), g_last_block_pc);
        g_aot_fallback_blocks++;
        uint32_t *c = emit_fallback_block(pc | (uint32_t)thumb);
        disp_set(pc, c);
        return c;
    }

    g_last_block_pc = pc;
    uint32_t *start = g_emit;
    rc_sync();              /* a block never inherits a slot */
    emit_guest_pc(pc);
    /* The --stop-at check is a host call per block, so it is emitted only when
     * the run actually asked for one. --stop-at is parsed before any guest code
     * runs, so g_stop_at is already final here; a block translated without it
     * simply cannot be a stop point. */
    if (g_stop_at) {
        emit_mov_const(0, pc);
        emit_call((void *)aot_stop_check);
        /* w0 != 0 -> halt with HALT_STOPPED, so the runner can dump and resume */
        intptr_t off = ((intptr_t)g_halt_stop_stub - (intptr_t)g_emit) / 4;
        emit(enc_cb(0, 1, 0, (int32_t)off));
    }
    if (g_trace_blocks) {
        emit_mov_const(0, pc);
        emit_call((void *)aot_block_log);
    }

    cc_kind = CC_NONE;
    g_nfix = 0;
    g_fail = 0;
    int ended = 0;
    g_pending_prev = 0;
    g_npend = 0;
    for (int k = 0; k < n; k++) {
        emit_count(blk[k].cnt);
        if (g_seq) {
            /* The trace call is a host call and destroys PSTATE, which the next
             * instruction in the block may still need (cmp; bcond is emitted as
             * one block). Without this the trace perturbs what it measures: it
             * reported a divergence at `cmp r0,#0; beq` that only existed
             * because seq_log itself had cleared Z. */
            emit(0xf81f0fe9);                        /* str x9, [sp, #-16]! */
            emit(enc_mrs_nzcv(9));                   /* mrs x9, nzcv        */
            emit(0xf81f0fe9);                        /* str x9, [sp, #-16]! */
            emit_mov_const(0, blk[k].pc | (uint32_t)(thumb ? 1 : 0));
            emit_call((void *)seq_log);
            emit(0xf84107e9);                        /* ldr x9, [sp], #16   */
            emit(enc_msr_nzcv(9));                   /* msr nzcv, x9        */
            emit(0xf84107e9);                        /* ldr x9, [sp], #16   */
        }
        /* A terminator ends the block, so the "flush the pending producers
         * before their consumer is checked" step that normally happens on the
         * *next* instruction would land after the branch -- dead code, and the
         * checker's shadow then stops advancing at that instruction. Flushing
         * here keeps the shadow in step with the generated code. */
        if (g_check && g_npend > 0 &&
            (blk[k].op == AOP_B || blk[k].op == AOP_BL || blk[k].op == AOP_BLX ||
             blk[k].op == AOP_BX || blk[k].op == AOP_ARM_B ||
             blk[k].op == AOP_ARM_BX || blk[k].op == AOP_BKPT ||
             blk[k].op == AOP_SWI))
            emit_pending_steps();
        int r = emit_one(&blk[k]);
        if (g_check && r == 0 && blk[k].op != AOP_BCOND) {
            int next_reads = (k + 1 < n) && insn_reads_flags(&blk[k + 1]);
            if (next_reads) {
                /* The next instruction consumes these flags and the call below
                 * would destroy them: remember this one and let the consumer's
                 * check step it on the shadow first. */
                if (g_npend < 8) g_pend[g_npend++] = blk[k].pc;
                g_pending_prev = blk[k].pc;
            } else {
                emit_pending_steps();
                emit_mov_const(0, blk[k].pc | (g_block_thumb ? 1u : 0u));
                emit_mov_const(1, 0);
                emit_call((void *)aot_check_one);
                emit_cbnz_w0_to_abort();
            }
        }
        if (r < 0) break;
        if (r > 0) { ended = 1; break; }
    }

    if (g_fail) {
        /* Roll the block back: a half-emitted block is not a thing. The
         * instruction that failed is interpreted, and the counter says how
         * often, so coverage is a number rather than a hope. */
        g_emit = start;
        /* The rollback discards the emitted code, so the register cache
         * must forget its mapping too: those slots describe instructions
         * that no longer exist. Missing this left the fallback reading a
         * register that was never written. */
        rc_sync();
        g_aot_fallback_blocks++;
        if (g_aot_why_count[g_fail_why] < 8) {
            host_log("aot: no translation for %08x op=%02x insn=%04x (%s) "
                     "-- interpreting it\n", g_fail_pc, g_fail_op, g_fail_insn,
                     g_why[g_fail_why] ? g_why[g_fail_why] : "?");
            g_aot_why_count[g_fail_why]++;
        }
        uint32_t *c = emit_fallback_block(pc | (uint32_t)thumb);
        disp_set(pc, c);
        return c;
    }

    emit_pending_steps();      /* the block ended before the consumer did */

    if (!ended)
        emit_exit_to(p | (uint32_t)thumb);          /* fall off the end */

    rc_sync();              /* nothing may survive the block */
    /* out-of-line branch stubs, then the patch */
    if (getenv("AOT_FIXDBG") && pc == 0x010d1fcc)
        host_log("aot fixdbg: block %08x, %d fixup(s), end at +%ld\n",
                 pc, g_nfix, (long)(g_emit - start));
    for (int f = 0; f < g_nfix; f++) {
        uint32_t *stub = g_emit;
        if (g_check) {
            for (int i = 0; i < g_fix[f].npend; i++) {
                emit_mov_const(0, g_fix[f].pend[i] | (uint32_t)(g_block_thumb ? 1 : 0));
                emit_mov_const(1, 0);
                emit_call((void *)aot_check_step);
                emit_cbnz_w0_to_abort();
            }
            emit_mov_const(0, g_fix[f].check_pc | (g_block_thumb ? 1u : 0u));
            emit_mov_const(1, 0);
            emit_call((void *)aot_check_branch);
            emit_cbnz_w0_to_abort();
        }
        /* Publish the flags as they were at the branch, not at the end of the
         * block: the target may be interpreted (a block whose first instruction
         * is conditional cannot be translated), and the interpreter reads
         * g_cpu.cpsr. */
        if (target_reads_flags(g_fix[f].pc, g_block_thumb))
            emit_flags_to_cpsr();
        emit_exit_to(g_fix[f].pc);
        intptr_t off = stub - g_fix[f].at;
        if (getenv("AOT_FIXDBG") && pc == 0x010d1fcc)
            host_log("aot fixdbg:   fix %d at +%ld -> stub +%ld, after +%ld\n",
                     f, (long)(g_fix[f].at - start), (long)(stub - start),
                     (long)(g_emit - start));
        *g_fix[f].at = enc_bcond(g_fix[f].cond, (int32_t)off);
    }

    if (g_blkchk) {
        /* Every branch emitted into this block must land inside it. A mistyped
         * offset here is a crash three blocks later with no clue; a named
         * block and target is the whole diagnosis. */
        for (uint32_t *q = start; q < g_emit; q++) {
            uint32_t ins = *q;
            int32_t off;
            int is_b = 0;
            if ((ins & 0x7c000000u) == 0x14000000u) {          /* b / bl */
                off = (int32_t)(ins << 6) >> 8; is_b = 1;
            } else if ((ins & 0xff000010u) == 0x54000000u) {    /* b.cond */
                off = (int32_t)((ins >> 5) & 0x7ffffu); off = (off << 13) >> 13; is_b = 1;
            } else if ((ins & 0x7e000000u) == 0x34000000u) {    /* cbz/cbnz */
                off = (int32_t)((ins >> 5) & 0x7ffffu); off = (off << 13) >> 13; is_b = 1;
            }
            if (!is_b) continue;
            uint32_t *t = q + off;
            if (t < g_code || t >= g_emit)
                host_log("aot blkcheck: block %08x at +%ld branches to %p, "
                         "outside the arena %p..%p\n", pc, (long)(q - start),
                         (void *)t, (void *)g_code, (void *)g_emit);
        }
    }
    if (g_nblkmap < (int)(sizeof(g_blkmap)/sizeof(g_blkmap[0]))) {
        g_blkmap[g_nblkmap].start = start;
        g_blkmap[g_nblkmap].end = g_emit;
        g_blkmap[g_nblkmap].pc = pc;
        g_nblkmap++;
    }
    aot_dump_block(start, g_emit, pc);
    __builtin___clear_cache((char *)start, (char *)g_emit);
    g_aot_blocks++;
    g_aot_insns += (uint64_t)n;
    disp_set(pc, start);
    return start;
}

/* ---- host crossings and the interpreter fallback ------------------------ */

static uint32_t *g_stub_code[IMPORT_MAX];

uint32_t *aot_enter_inner(uint32_t pcraw);

static uint32_t *aot_host_call(uint32_t pc)
{
    /* The checker's copy has to cross this stub as well, or it stops advancing
     * at every import and every later comparison is against the wrong
     * instruction. */
    if (g_check && shadow_step(pc | 1u)) {
        g_halt = HALT_TRAP;
        return NULL;
    }
    int idx = import_index_of_stub(pc);
    if (getenv("SPIKE_TRACE_AOTIMP") && idx >= 0 && idx < g_nimports)
        host_log("  [aotimp] %08x idx %d %s\n", pc, idx, g_imports[idx].name);
    if (idx >= 0 && idx < g_nimports) {
        g_cpu.r[15] = pc + 2;
        if (import_dispatch(idx)) {                 /* oracle mode stops here */
            g_halt = HALT_IMPORT;
            return NULL;
        }
    } else {
        uint32_t ins = LD16(pc);
        if (!host_hook_dispatch((int)(ins & 0xff))) {
            g_halt = HALT_HALTED;
            return NULL;
        }
    }
    /* Every shim may rewrite LR, SP and the registers (longjmp does), so the
     * next instruction always comes from g_cpu.r[14]. Flags are dead across a
     * call: a block that would need them afterwards is not translated. */
    if (g_check) {
        /* The checker must NOT replay host calls: a shim's return value depends
         * on state the replay would advance twice (malloc hands out two blocks,
         * a JNI handle counter ticks twice), which looks exactly like a
         * translation bug. So the shadow adopts the generated code's state
         * across a crossing and the comparison resumes on the other side; a
         * real divergence inside the next block is still caught. */
        g_shadow = g_cpu;
        g_shadow_n = g_insn_count;
        g_shadow.r[15] = g_cpu.r[14] & ~1u;
        g_expect_next = 0;
        g_pending_prev = 0;
    }
    return aot_enter(g_cpu.r[14]);
}

static uint32_t *aot_step(uint32_t pcraw)
{
    int saved = g_aot_mode;
    g_aot_mode = AOT_NONE;                  /* or cpu_run hands it back here */
    int r = cpu_run(pcraw, g_cpu.r[13], g_insn_count + 1);
    g_aot_mode = saved;
    if (r != HALT_BUDGET) {
        g_halt = r;
        return NULL;
    }
    return aot_enter(cpu_last_pc() | (uint32_t)cpu_thumb_state());
}

/* AOT_CHECK=1: called after every translated instruction with that
 * instruction's address. The interpreter runs the same instruction from its own
 * copy of the state and the two are compared register by register -- the first
 * mismatch names the guest PC, the register and both values. This is the gate
 * the plan calls "block self-verification", taken down to one instruction
 * because a block-level check cannot localise inside a block. */
uint32_t g_expect_next;
static uint32_t g_last_branch_pc;

/* Step the interpreter's copy by one instruction and keep it there. */
static int shadow_step(uint32_t pcraw)
{
    /* pcraw carries the mode bit: an ARM instruction must be stepped as ARM.
     * The shadow must be exactly where the generated code is about to run,
     * otherwise a branch went the other way earlier and every later comparison
     * would be measuring two different paths. */
    if ((g_shadow.r[15] & ~1u) != (pcraw & ~1u)) {
        host_log("aot check: path diverged -- code at %08x, interpreter at %08x\n",
                 pcraw & ~1u, g_shadow.r[15] & ~1u);
        host_log("   mode code=%d shadow=%d\n", (int)(pcraw & 1u),
                 (int)(g_shadow.r[15] & 1u));
        host_log("   code   : r0=%08x r1=%08x r2=%08x r3=%08x lr=%08x "
                 "sp=%08x cpsr=%08x\n", g_cpu.r[0], g_cpu.r[1], g_cpu.r[2],
                 g_cpu.r[3], g_cpu.r[14], g_cpu.r[13], g_cpu.cpsr);
        host_log("   interp : r0=%08x r1=%08x r2=%08x r3=%08x lr=%08x "
                 "sp=%08x cpsr=%08x\n", g_shadow.r[0], g_shadow.r[1],
                 g_shadow.r[2], g_shadow.r[3], g_shadow.r[14], g_shadow.r[13],
                 g_shadow.cpsr);
        return 1;
    }
    g_cpu = g_shadow;
    g_insn_count = g_shadow_n;
    int saved = g_aot_mode;
    g_aot_mode = AOT_NONE;
    int r = cpu_run(pcraw, g_cpu.r[13], g_shadow_n + 1);
    g_aot_mode = saved;
    if (r != HALT_BUDGET) {
        host_log("aot check: the interpreter halted at %08x (%s)\n",
                 pcraw & ~1u, cpu_halt_slug(r));
        return 1;
    }
    g_shadow = g_cpu;
    g_shadow_n = g_insn_count;
    g_shadow.r[15] = cpu_last_pc();     /* R[15] is instruction+8 for ARM */
    return 0;
}

/* Validate the path a branch promised, then consume the promise. The check is
 * against the instruction that is about to be stepped, not against the one the
 * checker happens to be called for: when a flag producer's own check is
 * skipped, the producer is stepped from the consumer's call. */
static int expect_arrival(uint32_t pcraw)
{
    if (!g_expect_next) return 0;
    if ((g_expect_next & ~1u) != (pcraw & ~1u)) {
        host_log("aot check: went to %08x, the interpreter went to %08x "
                 "(branch at %08x)\n", pcraw & ~1u, g_expect_next & ~1u,
                 g_last_branch_pc);
        return 1;
    }
    g_expect_next = 0;
    return 0;
}
/** Run the same instructions under the interpreter and compare. `prev` is a
 * flag producer whose own check was skipped because a C call would have
 * clobbered PSTATE before its consumer ran; stepping it here keeps the
 * interpreter's flags authoritative. */
static int aot_check_one(uint32_t pc, uint32_t prev)
{
    g_checks++;
    seq_log(prev ? prev : pc);
    if (prev) {
        seq_log(pc);
        if (expect_arrival(prev)) return 1;
        if (shadow_step(prev)) return 1;
    }

    cpu_state aot = g_cpu;
    uint64_t  aot_n = g_insn_count;
    if (expect_arrival(pc)) return 1;
    if (shadow_step(pc)) return 1;
    g_cpu = aot;
    g_insn_count = aot_n;

    /* r15 is excluded: the generated code keeps the block address there and
     * the interpreter the next instruction address; the next arrival check
     * carries that information. */
    for (int k = 0; k < 15; k++) {
        if (aot.r[k] == g_shadow.r[k]) continue;
        host_log("aot check: %08x differs in r%d: aot %08x, interpreter %08x\n",
                 pc, k, aot.r[k], g_shadow.r[k]);
        for (int j = 0; j < 15; j++)
            host_log("   r%-2d aot %08x  interp %08x\n", j, aot.r[j], g_shadow.r[j]);
        return 1;
    }
    return 0;
}

/* A conditional branch has two successors, so there is no single post-state
 * to compare: run it on the interpreter, record where it went, and let the
 * next arrival check compare that with the path the generated code took.
 * Called *after* the branch, since the call clobbers PSTATE. */
static int aot_check_branch(uint32_t pc, uint32_t prev)
{
    seq_log(prev ? prev : pc);
    if (prev) {
        seq_log(pc);
        if (expect_arrival(prev)) return 1;
        if (shadow_step(prev)) return 1;
    }
    if (expect_arrival(pc)) return 1;
    if (shadow_step(pc)) return 1;
    g_expect_next = (g_shadow.r[15] & ~1u) | 1u;
    g_last_branch_pc = pc;
    return 0;
}

static int aot_check_step(uint32_t pc, uint32_t prev)
{
    seq_log(prev ? prev : pc);
    /* The steps leave g_cpu holding the interpreter's state; the generated
     * code must continue with its own, so both are saved around them. */
    cpu_state aot = g_cpu;
    uint64_t  aot_n = g_insn_count;
    if (prev) {
        seq_log(pc);
        if (expect_arrival(prev)) return 1;
        if (shadow_step(prev)) return 1;
    }
    if (expect_arrival(pc)) return 1;
    if (shadow_step(pc)) return 1;
    g_cpu = aot;
    g_insn_count = aot_n;
    return 0;
}

static uint32_t *aot_enter(uint32_t pcraw)
{
    uint32_t pc = pcraw & ~1u;
    uint32_t *code;

    {
        /* tail call through the real work, then a range check: an address
         * outside the emitted code means the CPU would run zeros, which traps
         * as SIGILL with no clue where it came from. */
        code = aot_enter_inner(pcraw);
        if (code && (code < g_code || code >= g_emit)) {
            host_log("aot: BLOCK ADDRESS OUT OF RANGE: guest %08x -> %p "
                     "(arena %p..%p)\n", pc, (void *)code, (void *)g_code,
                     (void *)g_emit);
            g_halt = HALT_BAD_PC;
            return NULL;
        }
        return code;
    }
}

uint32_t *aot_enter_inner(uint32_t pcraw)
{
    uint32_t pc = pcraw & ~1u;

    if (g_verify_active) {
        g_verify_next = pcraw;
        return g_verify_stop_stub;
    }

    if (pc >= g_text_lo && pc < g_text_hi) {
        uint32_t *c = disp_get(pc);
        return c ? c : translate(pc, (int)(pcraw & 1u));
    }
    if (pc >= GUEST_STUB_BASE && pc < GUEST_STUB_BASE + IMPORT_MAX * 4u) {
        uint32_t i = (pc - GUEST_STUB_BASE) / 4u;
        if (!g_stub_code[i])
            g_stub_code[i] = translate(pc, 1);           /* decoded as BKPT */
        return g_stub_code[i];
    }
    /* Outside the image and outside the stub page. The frame sentinel lands
     * here on purpose: see RETURN_SENTINEL in src/guest.h. */
    g_halt = HALT_BAD_PC;
    g_cpu.r[15] = pc;
    host_log("aot: PC outside the image: %08x (text %08x-%08x, stub page %08x)\n",
             pc, g_text_lo, g_text_hi, GUEST_STUB_BASE);
    return NULL;
}

#if defined(__aarch64__)
/* The entry stub. x0 = code, x1 = budget, x2 = ctx. Everything the generated
 * code pins is loaded here, once per run, and restored on the way out -- so a
 * nested run (qsort's comparator, via cpu_call_guest) gets its own set. */
__asm__(
".text\n"
".global aot_exec\n"
".type aot_exec, %function\n"
"aot_exec:\n"
"    stp x19, x20, [sp, #-96]!\n"
"    stp x21, x22, [sp, #16]\n"
"    stp x23, x24, [sp, #32]\n"
"    stp x25, x26, [sp, #48]\n"
"    stp x27, x28, [sp, #64]\n"
"    stp x29, x30, [sp, #80]\n"
"    adr  x22, .Laot_exit\n"

"    mov  x24, x1\n"
"    mov  x25, x2\n"
"    ldr  x19, [x25, #0]\n"
"    ldr  x20, [x25, #8]\n"
"    ldr  x21, [x25, #16]\n"
"    ldr  x26, [x25, #24]\n"
"    ldr  x27, [x25, #32]\n"
"    ldr  x28, [x25, #40]\n"
"    ldr  x25, [x26]\n"
"    blr  x0\n"
".Laot_exit:\n"
"    str  x25, [x26]\n"
"    ldp  x29, x30, [sp, #80]\n"
"    ldp  x27, x28, [sp, #64]\n"
"    ldp  x25, x26, [sp, #48]\n"
"    ldp  x23, x24, [sp, #32]\n"
"    ldp  x21, x22, [sp, #16]\n"
"    ldp  x19, x20, [sp], #96\n"
"    ret\n"
".size aot_exec, .-aot_exec\n"
);
#else
/* Non-aarch64 hosts only compile this file for the encoder self-test, which
 * compares the encoders against the assembler (see the header comment), so the
 * trampoline has no body here. */
int aot_exec(uint32_t *code, uint64_t budget, struct aot_ctx *ctx)
{
    (void)code; (void)budget; (void)ctx;
    return 0;
}
#endif

static int aot_halt_reason(void) { return g_halt; }

/* ------------------------------------------------------------------------- */
/* init, run, stats                                                          */
/* ------------------------------------------------------------------------- */

static int g_ready;

static int aot_init(void)
{
    g_ready = 0;
#ifndef WASM_FREESTANDING
    long ps = sysconf(_SC_PAGESIZE);
    if (ps <= 0) ps = 4096;
    size_t code_bytes = (size_t)AOT_CODE_WORDS * 4u;
    size_t disp_bytes = (size_t)AOT_TABLE_ENTRIES * sizeof(uint64_t);

    void *code = mmap(NULL, code_bytes, PROT_READ | PROT_WRITE | PROT_EXEC,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (code == MAP_FAILED) {
        host_log("aot: cannot map executable code (%u bytes) -- the kernel "
                 "refused RWX; staying on the interpreter\n", (unsigned)code_bytes);
        return -1;
    }
    g_code = (uint32_t *)code;
    g_emit = g_code;
    g_emit_end = g_code + AOT_CODE_WORDS;

    g_disp = (uint32_t *)host_alloc(disp_bytes);
    guest_text_bounds(&g_text_lo, &g_text_hi);

    /* The shared abort path: every generated branch stub jumps here when
     * aot_enter() has no code to give it, with the reason already recorded. */
    g_abort_stub = g_emit;
    emit_call((void *)aot_halt_reason);              /* w0 = g_halt        */
    emit(enc_br(22));

    g_halt_budget_stub = g_emit;
    emit_mov_const(0, HALT_BUDGET);
    emit(enc_br(22));

    g_halt_stop_stub = g_emit;
    emit_mov_const(0, HALT_STOPPED);
    emit(enc_br(22));

    g_verify_stop_stub = g_emit;
    emit_mov_const(0, AOT_VERIFY_STOP);
    emit(enc_br(22));

    __builtin___clear_cache((char *)g_code, (char *)g_emit);

    { const char *t = getenv("AOT_TRACE"); g_trace_blocks = t && *t && *t != '0'; }
    g_dump_code = getenv("AOT_DUMP_CODE");
    { const char *v = getenv("AOT_VERIFY"); g_verify = v && *v && *v != '0'; }
    { const char *v = getenv("AOT_CHECK");  g_check  = v && *v && *v != '0'; }
    { const char *v = getenv("AOT_SEQ");    g_seq    = v && *v && *v != '0'; }
    { const char *v = getenv("AOT_BLKCHECK"); g_blkchk = v && *v && *v != '0'; }
    host_log("aot: verify=%d trace=%d &g_halt %p\n",
             g_verify, g_trace_blocks, (void *)&g_halt);
    host_log("aot: arena %p, abort %p, budget %p, verify %p\n",
             (void *)g_code, (void *)g_abort_stub,
             (void *)g_halt_budget_stub, (void *)g_verify_stop_stub);
    g_why[1] = "no encoder for this encoding";
    g_why[2] = "flags live across a call or a partial setter";
    g_why[3] = "block/arena limit";
    g_ready = 1;
    return 0;
#else
    return -1;      /* no executable memory in the freestanding wasm build */
#endif
}

static void fill_ctx(struct aot_ctx *ctx)
{
    ctx->text_lo    = g_text_lo;
    ctx->disp       = (uint64_t)(uintptr_t)g_disp;
    ctx->entries    = AOT_TABLE_ENTRIES;
    ctx->insn_count = (uint64_t)(uintptr_t)&g_insn_count;
    ctx->mem        = (uint64_t)(uintptr_t)g_mem;
    ctx->cpu        = (uint64_t)(uintptr_t)&g_cpu;
}

/* One interpreter instruction, from a raw PC with its Thumb bit. Returns the
 * next raw PC. */
static uint32_t interp_one(uint32_t pcraw, int *halted)
{
    int saved = g_aot_mode;
    g_aot_mode = AOT_NONE;
    int r = cpu_run(pcraw, g_cpu.r[13], g_insn_count + 1);
    g_aot_mode = saved;
    if (getenv("AOT_DEBUG_STEP"))
        host_log("aot: step from %08x budget %llu -> r=%d pc=%08x count=%llu\n",
                 pcraw, (unsigned long long)(g_insn_count + 1), r,
                 g_cpu.r[15], (unsigned long long)g_insn_count);
    if (r != HALT_BUDGET) { g_halt = r; *halted = 1; return 0; }
    return (g_cpu.r[15] & ~1u) | (uint32_t)cpu_thumb_state();
}

/* AOT_VERIFY: run a block, then run the same block under the interpreter from a
 * snapshot and compare the whole register file. The first block that differs is
 * named, with the register and both values -- which is the only practical way to
 * localise a code generator bug from 54 million instructions. */
static int aot_verify_run(uint32_t entry, uint64_t budget)
{
    struct aot_ctx ctx;
    fill_ctx(&ctx);
    uint32_t pcraw = entry;

    while (g_insn_count < budget) {
        uint32_t *code = aot_enter(pcraw);
        if (!code) return g_halt;

        cpu_state before = g_cpu;
        uint64_t  n_before = g_insn_count;

        g_verify_active = 1;
        int r = aot_exec(code, budget, &ctx);
        g_verify_active = 0;

        cpu_state aot = g_cpu;
        uint64_t  n_aot = g_insn_count;
        uint32_t  next = g_verify_next;

        if (r != AOT_VERIFY_STOP) {
            /* The block halted inside itself (budget, trap, bad PC). Nothing to
             * compare -- but the interpreter must agree it halts there too.
             * check_block() below covers the normal case; halts are rare and
             * are covered by the full-run comparison. */
            return r;
        }

        uint64_t steps = n_aot - n_before;
        if (getenv("AOT_VERBOSE"))
            host_log("aot verify: block %08x steps %llu -> %08x\n",
                     pcraw & ~1u, (unsigned long long)steps, next & ~1u);
        if (getenv("AOT_VERBOSE"))
            host_log("aot verify: block %08x -> %08x, %llu insns\n",
                     pcraw & ~1u, next & ~1u, (unsigned long long)steps);
        g_cpu = before;
        g_insn_count = n_before;
        uint32_t replay = pcraw;
        for (uint64_t i = 0; i < steps; i++) {
            int halted = 0;
            uint32_t np = interp_one(replay, &halted);
            if (halted) {
                host_log("aot verify: the interpreter halted inside a block the "
                         "AOT completed: guest pc %08x\n", replay & ~1u);
                return HALT_TRAP;
            }
            replay = np;
        }

        /* r15 is excluded on purpose: the interpreter keeps it pointing at the
         * next instruction, the generated code keeps the block's own address
         * (so crash reports name a real boundary). The next PC is compared
         * separately, which is the same information. */
        int bad = -1;
        for (int k = 0; k < 15; k++)
            if (g_cpu.r[k] != aot.r[k]) { bad = k; break; }
        if (bad >= 0 || (replay & ~1u) != (next & ~1u)) {
            host_log("aot verify: block at %08x diverges (r%d: code %08x vs interp %08x;"
                     " next %08x vs %08x)\n", pcraw & ~1u, bad,
                     bad >= 0 ? g_cpu.r[bad] : 0, bad >= 0 ? aot.r[bad] : 0,
                     replay & ~1u, next & ~1u);
            if (bad >= 0)
                host_log("  r%d: interpreter %08x, aot %08x\n",
                         bad, g_cpu.r[bad], aot.r[bad]);
            host_log("  next pc: interpreter %08x, aot %08x\n",
                     replay & ~1u, next & ~1u);
            host_log("  instructions: interpreter %llu, aot %llu\n",
                     (unsigned long long)(g_insn_count - n_before),
                     (unsigned long long)steps);
            cpu_dump_trace();
            return HALT_TRAP;
        }

        g_cpu = aot;
        g_insn_count = n_aot;
        pcraw = next;
    }
    return HALT_BUDGET;
}

int aot_arm64_run(uint32_t entry, uint32_t sp, uint64_t budget)
{
    (void)sp;                       /* already in g_cpu.r[13] via cpu_run */
    if (!g_ready && aot_init() != 0)
        return AOT_NOT_AVAILABLE;

    if (g_check) {
        /* The checker keeps its own copy of the machine, one instruction
         * behind, and the interpreter is the authority on what each
         * instruction should have done. */
        g_shadow = g_cpu;
        g_shadow_n = g_insn_count;
        g_shadow_pc = entry;
        g_expect_next = 0;      /* a new entry point starts a new path */
        g_pending_prev = 0;
    }

    if (g_verify)
        return aot_verify_run(entry, budget);

    uint32_t *code = aot_enter(entry);
    if (!code)
        return g_halt;

    struct aot_ctx ctx;
    fill_ctx(&ctx);
    return aot_exec(code, budget, &ctx);
}

int aot_arm64_available(void) { return g_ready; }

void aot_arm64_stats(void)
{
    if (!g_ready)
        return;
    if (g_checks)
        host_log("aot: %llu instruction(s) checked against the interpreter\n",
                 (unsigned long long)g_checks);
    host_log("aot: %llu block(s), %llu guest insn(s) translated, %llu fallback "
             "block(s), %u word(s) of code\n",
             (unsigned long long)g_aot_blocks, (unsigned long long)g_aot_insns,
             (unsigned long long)g_aot_fallback_blocks,
             (unsigned)(g_emit - g_code));
}

/* Is a host PC inside the emitted code, and if so which block? Used by the
 * crash handler; printing the arena bounds makes "inside the arena but never
 * written" (executing zeroes) distinguishable from "outside the arena". */
void aot_report_code_range(uintptr_t hp);
void aot_report_code_range(uintptr_t hp)
{
    host_log("  arena is %p..%p (emit cursor %p); pc %s the arena\n",
             (void *)g_code, (void *)g_emit_end, (void *)g_emit,
             (hp >= (uintptr_t)g_code && hp < (uintptr_t)g_emit_end)
                 ? "IS inside" : "is OUTSIDE");
    uint32_t *b = aot_host_pc_block(hp);
    if (!b) { host_log("  host pc is below the first block\n"); return; }
    uint32_t *e = (uint32_t *)(uintptr_t)aot_host_pc_block_end(b);
    host_log("  block for guest %08x: %p..%p, pc offset %+ld words%s\n",
             aot_block_guest_pc(b), (void *)b, (void *)e,
             (long)((hp - (uintptr_t)b) / 4),
             (hp >= (uintptr_t)b && hp < (uintptr_t)e) ? "" : "  (OUTSIDE it)");
}
