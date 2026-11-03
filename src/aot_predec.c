/*
 * aot_predec.c -- pre-decoded execution of Thumb-1.
 *
 * The interpreter in cpu.c decodes the same halfword again on every execution:
 * fetch, `switch (ins >> 12)`, a pile of bitfield extractions, then the
 * operation. For a fixed image that work is redundant -- the instruction at a
 * given address has the same encoding for the whole run. This file decodes each
 * guest instruction once, into a struct with its operands already split out and
 * its successors resolved, and executes those structs.
 *
 * What this deliberately keeps, so the measurement is honest:
 *
 *   - the guest register file stays in g_cpu (a load and a store per operand);
 *     this is NOT the register-allocated form of docs/aot-plan.md §7.7;
 *   - the dispatch is still one indirect jump per instruction (a switch over
 *     ~26 opcodes, which GCC compiles to a jump table);
 *   - the per-instruction budget check, the stop-at check and the trace hook are
 *     still per instruction, exactly as the interpreter has them;
 *   - flags are computed with the same helper functions, so the flag cost is
 *     unchanged (the AOT's PSTATE mapping is additional).
 *
 * The difference this measures is therefore *pre-decoding alone*: no fetch, no
 * bit extraction, no re-decoding of the same halfword, plus one indirect jump
 * that the interpreter also pays. Everything docs/aot-plan.md expects from real
 * code generation -- block chaining, register allocation, flags in PSTATE,
 * per-block accounting -- is on top of this, which makes the number a lower
 * bound for the AOT.
 *
 * Scope: Thumb-1 only -- the 19 formats, plus the encodings this binary's
 * compiler emits in the 0xb space (REV*, SXTH/SXTB/UXTH/UXTB, NOP, BKPT). ARM
 * mode and Thumb-2 stop with HALT_TRAP and print the encoding. `libgame.so`
 * needs both (the .plt is ARM and the image contains Thumb-2), which is why this
 * is a measurement target and not a shipping path -- see docs/aot-plan.md §11.
 *
 * Semantics are copied from cpu.c on purpose, including the failure modes its
 * comments record. cpu.c is the specification and the oracle; when the two
 * disagree, the per-instruction trace differential (tools/aotdiff.sh) names the
 * first instruction that differs.
 *
 * Not done here, on purpose: per-function profiling (the interpreter's
 * g_prof charge) and the GUEST_CHECK fault poll. Both belong to the emitter
 * stage, where they are per block rather than per instruction. In a
 * -DGUEST_CHECK build, run with --aot=off.
 */

#include "aot.h"
#include "cpu.h"
#include "guest.h"
#include "imports.h"
#include "host_hooks.h"
#include "platform.h"
#include "compat.h"

/* memset/strcmp come from compat.h: the freestanding wasm32 build has no system
 * headers, and this file is compiled into every build. */

int g_aot_mode = AOT_NONE;

/* ---- flags: verbatim from cpu.c (the specification) --------------------- */

#define F_N (1u << 31)
#define F_Z (1u << 30)
#define F_C (1u << 29)
#define F_V (1u << 28)

static inline void set_nz(uint32_t v)
{
    g_cpu.cpsr = (g_cpu.cpsr & ~(F_N | F_Z)) |
                 (v == 0 ? F_Z : 0) | (v & 0x80000000u ? F_N : 0);
}

static inline uint32_t add_c(uint32_t a, uint32_t b, uint32_t cin)
{
    uint64_t r = (uint64_t)a + (uint64_t)b + cin;
    uint32_t res = (uint32_t)r;
    g_cpu.cpsr = (g_cpu.cpsr & ~(F_N | F_Z | F_C | F_V)) |
                 (res == 0 ? F_Z : 0) |
                 (res & 0x80000000u ? F_N : 0) |
                 ((r >> 32) ? F_C : 0) |
                 (((~(a ^ b) & (a ^ res)) & 0x80000000u) ? F_V : 0);
    return res;
}

static inline uint32_t sub_c(uint32_t a, uint32_t b, uint32_t cin)
{
    return add_c(a, ~b, cin);
}

static inline uint32_t sh_lsl(uint32_t v, uint32_t s, uint32_t *c)
{
    if (s == 0) { *c = g_cpu.cpsr & F_C; return v; }
    if (s < 32) { *c = (v >> (32 - s)) & 1 ? F_C : 0; return v << s; }
    if (s == 32) { *c = (v & 1) ? F_C : 0; return 0; }
    *c = 0;
    return 0;
}

static inline uint32_t sh_lsr(uint32_t v, uint32_t s, uint32_t *c)
{
    if (s == 0) { *c = g_cpu.cpsr & F_C; return v; }
    if (s < 32) { *c = (v >> (s - 1)) & 1 ? F_C : 0; return v >> s; }
    if (s == 32) { *c = (v >> 31) ? F_C : 0; return 0; }
    *c = 0;
    return 0;
}

static inline uint32_t sh_asr(uint32_t v, uint32_t s, uint32_t *c)
{
    int32_t sv = (int32_t)v;
    if (s == 0) { *c = g_cpu.cpsr & F_C; return v; }
    if (s < 32) { *c = (uint32_t)(sv >> (s - 1)) & 1 ? F_C : 0; return (uint32_t)(sv >> s); }
    *c = sv < 0 ? F_C : 0;
    return sv < 0 ? 0xffffffffu : 0;
}

static inline uint32_t sh_ror(uint32_t v, uint32_t s, uint32_t *c)
{
    uint32_t r = s & 31;
    if (s == 0) { *c = g_cpu.cpsr & F_C; return v; }
    if (r == 0) { *c = (v >> 31) ? F_C : 0; return v; }
    *c = (v >> (r - 1)) & 1 ? F_C : 0;
    return (v >> r) | (v << (32 - r));
}

static inline int cond_true(uint32_t cond)
{
    uint32_t f = g_cpu.cpsr;
    int n = (f & F_N) != 0, z = (f & F_Z) != 0;
    int c = (f & F_C) != 0, v = (f & F_V) != 0;
    switch (cond) {
    case 0x0: return z;
    case 0x1: return !z;
    case 0x2: return c;
    case 0x3: return !c;
    case 0x4: return n;
    case 0x5: return !n;
    case 0x6: return v;
    case 0x7: return !v;
    case 0x8: return c && !z;
    case 0x9: return !c || z;
    case 0xa: return n == v;
    case 0xb: return n != v;
    case 0xc: return !z && (n == v);
    case 0xd: return z || (n != v);
    case 0xe: return 1;
    default:  return 0;
    }
}

/* ---- the decoded instruction --------------------------------------------- */

enum {
    PD_UNSUP = 0,   /* an encoding this prototype does not handle: trap       */
    PD_BAD_PC,      /* PC outside .text and the stub page: HALT_BAD_PC        */
    PD_SHIFT,       /* format 1: LSL/LSR/ASR immediate                       */
    PD_ADDSUB,      /* format 2: add/subtract, register or 3-bit immediate   */
    PD_IMM8,        /* format 3: mov/cmp/add/sub immediate                   */
    PD_ALU,         /* format 4: the 16 ALU operations                       */
    PD_HI,          /* format 5: hi-register add/cmp/mov                      */
    PD_BX,          /* format 5, op 3: bx/blx register                       */
    PD_LDRLIT,      /* format 6: PC-relative load                            */
    PD_LDST_REG,    /* formats 7/8: load/store, register offset              */
    PD_LDST_IMM,    /* formats 9/10: load/store, immediate offset            */
    PD_LDST_SP,     /* format 11: load/store, SP-relative                    */
    PD_ADDR,        /* format 12: add rd, pc/sp, #imm                        */
    PD_ADJSP,       /* format 13: add/sub sp, #imm                           */
    PD_PUSH,        /* format 14: push                                       */
    PD_POP,         /* format 14: pop                                        */
    PD_REV,         /* rev / rev16 / revsh                                   */
    PD_EXT,         /* sxth / sxtb / uxth / uxtb                             */
    PD_NOP,         /* nop                                                   */
    PD_BKPT,        /* bkpt: an import stub or a host-hooked function entry   */
    PD_SWI,         /* svc: traps, as in cpu.c                               */
    PD_UNDEF,       /* conditional-branch slot 0xe: undefined, as in cpu.c   */
    PD_LDMIA,       /* format 15: ldmia                                      */
    PD_STMIA,       /* format 15: stmia                                      */
    PD_BCOND,       /* format 16: conditional branch                         */
    PD_B,           /* format 18: unconditional branch                       */
    PD_BL,          /* format 19: bl (stays Thumb)                           */
    PD_BLX         /* format 19: blx (to ARM -- not supported here)          */
};

typedef struct pdec {
    uint32_t key;            /* guest PC of this instruction                  */
    struct pdec *hnext;      /* hash chain                                    */
    struct pdec *fnext;      /* fall-through, resolved on first use           */
    struct pdec *ftaken;     /* branch target, resolved on first use          */
    uint32_t imm;            /* immediate, address or branch displacement     */
    uint16_t insn;           /* the raw halfword, for reports and the trace   */
    uint8_t  op;             /* PD_*                                          */
    uint8_t  rd, rn, rm;
    uint8_t  a, b;           /* per-op selectors, see pd_decode               */
    uint8_t  len;            /* 2 or 4                                        */
    uint8_t  emit;           /* 0 for BKPT: cpu.c suppresses its trace line    */
} pdec;

/* ---- the decode cache ----------------------------------------------------
 *
 * Chained hash buckets and a bump arena, so a decoded instruction is never
 * moved or overwritten: `fnext`/`ftaken` are plain pointers into the arena and
 * stay valid for the life of the process. That is deliberate -- an evicting
 * cache would leave stale successor pointers behind and the failure would be
 * "the guest executes the wrong instruction", a long way from here. The real
 * translator gets the same property from a whole-image pass (a stable code
 * address per guest PC); this is the smallest version of that contract.
 */
#define PD_BUCKETS (1u << 20)          /* 8 MB of pointers                   */
#define PD_CHUNK   16384               /* pdec per arena allocation          */

static pdec  **g_buckets;
static pdec   *g_arena;                /* next free slot                     */
static uint32_t g_arena_left;
static uint32_t g_ndecoded;
static uint64_t g_nlookup;

static pdec *pd_decode(uint32_t pc);

static pdec *pd_alloc(void)
{
    if (!g_arena || g_arena_left == 0) {
        g_arena = (pdec *)host_alloc(sizeof(pdec) * PD_CHUNK);
        g_arena_left = PD_CHUNK;
    }
    g_arena_left--;
    pdec *p = g_arena++;
    memset(p, 0, sizeof(*p));
    return p;
}

static pdec *pd_lookup(uint32_t pc)
{
    g_nlookup++;
    uint32_t h = (pc >> 1) & (PD_BUCKETS - 1u);
    for (pdec *p = g_buckets[h]; p; p = p->hnext)
        if (p->key == pc)
            return p;
    pdec *p = pd_decode(pc);
    p->hnext = g_buckets[h];
    g_buckets[h] = p;
    g_ndecoded++;
    return p;
}

/* ---- decoding ------------------------------------------------------------
 *
 * Every field the execution needs is extracted here and nowhere else, which is
 * the point: the interpreter splits the same bits on every execution.
 */
static pdec *pd_decode(uint32_t pc)
{
    pdec *p = pd_alloc();
    p->key = pc;
    p->len = 2;
    p->emit = 1;

    /* The interpreter checks the PC against .text and the stub page before it
     * fetches, and so must this: decoding a branch target outside the image
     * reads past the end of the guest buffer, which is a host SIGSEGV a long
     * way from the instruction that made the jump. The sentinel that ends a
     * frame (RETURN_SENTINEL) lands here on purpose. */
    uint32_t lo, hi;
    guest_text_bounds(&lo, &hi);
    if (pc < lo || pc >= hi) {
        if (pc >= GUEST_STUB_BASE && pc < GUEST_STUB_BASE + IMPORT_MAX * 4u) {
            p->op = PD_BKPT;            /* a stub is a BKPT, below           */
            p->emit = 0;
            return p;
        }
        p->op = PD_BAD_PC;
        return p;
    }

    uint32_t ins = LD16(pc);
    p->insn = (uint16_t)ins;
    uint32_t top = ins >> 12;

    /* The stub page has no text to decode: a 2-byte BKPT whose index comes
     * from the address. cpu.c's range check allows this page for the same
     * reason. */
    if (pc >= GUEST_STUB_BASE && pc < GUEST_STUB_BASE + IMPORT_MAX * 4u) {
        p->op = PD_BKPT;
        p->emit = 0;
        return p;
    }

    switch (top) {
    case 0x0: case 0x1:
        if ((ins & 0x1800) != 0x1800) {
            /* format 1: move shifted register */
            p->op = PD_SHIFT;
            p->rd = ins & 7;
            p->rm = (ins >> 3) & 7;
            p->a  = (uint8_t)((ins >> 11) & 3);     /* 0 lsl, 1 lsr, 2 asr */
            p->imm = (ins >> 6) & 0x1f;
        } else {
            /* format 2: add/subtract. Rd is both source and destination. */
            p->op = PD_ADDSUB;
            p->rd = ins & 7;
            p->rn = (ins >> 3) & 7;
            p->a  = (ins & 0x0400) ? 1 : 0;         /* 1 = 3-bit immediate */
            p->b  = (ins & 0x0200) ? 1 : 0;         /* 1 = subtract        */
            p->imm = (ins >> 6) & 7;                /* the immediate       */
            p->rm = (uint8_t)((ins >> 6) & 7);      /* the register        */
        }
        break;

    case 0x2: case 0x3:
        p->op = PD_IMM8;
        p->rd = (ins >> 8) & 7;
        p->a  = (uint8_t)((ins >> 11) & 3);         /* 0 mov, 1 cmp, 2 add, 3 sub */
        p->imm = ins & 0xff;
        break;

    case 0x4:
        if ((ins & 0xFC00) == 0x4000) {
            p->op = PD_ALU;
            p->rd = ins & 7;
            p->rm = (ins >> 3) & 7;
            p->a  = (uint8_t)((ins >> 6) & 0xf);    /* the ALU operation   */
        } else if ((ins & 0xFC00) == 0x4400) {
            p->rd = (uint8_t)((ins & 7) | ((ins >> 4) & 8));
            p->rm = (uint8_t)((ins >> 3) & 0xf);
            p->a  = (uint8_t)((ins >> 8) & 3);
            p->op = (p->a == 3) ? PD_BX : PD_HI;
        } else {
            /* format 6: PC-relative load. The address is a constant once the
             * image is relocated, so it is resolved here, not per execution. */
            p->op = PD_LDRLIT;
            p->rd = (ins >> 8) & 7;
            p->imm = ((pc + 4) & ~3u) + ((ins & 0xff) << 2);
        }
        break;

    case 0x5:
        p->op = PD_LDST_REG;
        p->rd = ins & 7;
        p->rn = (ins >> 3) & 7;
        p->rm = (ins >> 6) & 7;
        p->a  = (uint8_t)((ins >> 9) & 7);          /* which access        */
        break;

    case 0x6: case 0x7: case 0x8: case 0x9: {
        uint32_t grp = top;
        if (grp == 0x9) {
            /* format 11: Rd is bits[10:8] and the base is SP. Taking Rd from
             * bits[2:0] here writes a different register -- see cpu.c. */
            p->op = PD_LDST_SP;
            p->rd = (ins >> 8) & 7;
            p->imm = ins & 0xff;
            p->b  = (ins & 0x0800) ? 1 : 0;         /* load?               */
        } else {
            p->op = PD_LDST_IMM;
            p->rd = ins & 7;
            p->rn = (ins >> 3) & 7;
            p->b  = (ins & 0x0800) ? 1 : 0;         /* load?               */
            p->a  = (uint8_t)(grp == 0x8 ? 1 : grp == 0x7 ? 2 : 0);
            /*                         halfword      byte      word         */
            p->imm = (ins >> 6) & 0x1f;
        }
        break;
    }

    case 0xa:
        p->op = PD_ADDR;
        p->rd = (ins >> 8) & 7;
        p->b  = (ins & 0x0800) ? 1 : 0;             /* 1 = sp, 0 = pc      */
        p->imm = (ins & 0xff) << 2;
        break;

    case 0xb:
        if ((ins & 0xFF00) == 0xB000) {
            p->op = PD_ADJSP;
            p->imm = (ins & 0x7f) << 2;
            p->b  = (ins & 0x80) ? 1 : 0;           /* 1 = subtract        */
        } else if ((ins & 0xFE00) == 0xB400) {
            p->op = PD_PUSH;
            p->imm = ins & 0xff;
            p->b  = (ins & 0x100) ? 1 : 0;          /* 1 = push LR too     */
        } else if ((ins & 0xFE00) == 0xBC00) {
            p->op = PD_POP;
            p->imm = ins & 0xff;
            p->b  = (ins & 0x100) ? 1 : 0;          /* 1 = pop PC too      */
        } else if ((ins & 0xFF00) == 0xBA00) {
            p->op = PD_REV;
            p->rm = (ins >> 3) & 7;
            p->rd = ins & 7;
            p->a  = (uint8_t)((ins >> 6) & 3);
        } else if ((ins & 0xFF00) == 0xB200) {
            p->op = PD_EXT;
            p->rm = (ins >> 3) & 7;
            p->rd = ins & 7;
            p->a  = (uint8_t)((ins >> 6) & 3);
        } else if (ins == 0xBF00) {
            p->op = PD_NOP;
        } else if ((ins & 0xFF00) == 0xBE00) {
            p->op = PD_BKPT;            /* a host-hooked function entry      */
            p->emit = 0;
            p->a  = (uint8_t)(ins & 0xff);
        } else {
            p->op = PD_UNSUP;
        }
        break;

    case 0xc:
        p->op = (ins & 0x0800) ? PD_LDMIA : PD_STMIA;
        p->rn = (ins >> 8) & 7;
        p->imm = ins & 0xff;
        break;

    case 0xd: {
        uint32_t cond = (ins >> 8) & 0xf;
        if (cond == 0xf) {
            p->op = PD_SWI;
        } else if (cond == 0xe) {
            p->op = PD_UNDEF;
        } else {
            p->op = PD_BCOND;
            p->a  = (uint8_t)cond;
            p->imm = (uint32_t)(int32_t)((int32_t)(int8_t)(ins & 0xff) << 1);
        }
        break;
    }

    case 0xe:
        if ((ins & 0xF800) != 0xE000) {
            /* Thumb-2: BL/BLX in this space is handled below; anything else is
             * a 32-bit encoding (movw/movt, wide data processing, ...). */
            p->op = PD_UNSUP;
            p->len = 4;
        } else {
            p->op = PD_B;
            int32_t soff = (int32_t)(ins & 0x7ff);
            soff = (soff << 21) >> 21;
            p->imm = (uint32_t)(soff << 1);
        }
        break;

    default: {                              /* 0xf: BL / BLX long           */
        uint32_t lo = LD16(pc + 2);
        if ((lo >> 11) != 0x1fu && (lo >> 11) != 0x1du) {
            p->op = PD_UNSUP;
            p->len = 4;
            break;
        }
        uint32_t S = (ins >> 10) & 1;
        uint32_t imm10 = ins & 0x3ff;
        uint32_t J1 = (lo >> 13) & 1, H = (lo >> 12) & 1;
        uint32_t J2 = (lo >> 11) & 1, imm11 = lo & 0x7ff;
        uint32_t I1 = (~(J1 ^ S)) & 1, I2 = (~(J2 ^ S)) & 1;
        int32_t off = (int32_t)((S << 24) | (I1 << 23) | (I2 << 22) |
                                (imm10 << 12) | (imm11 << 1));
        off = (off << 7) >> 7;
        /* Bit 12 of the second halfword -- not J2 -- tells BL from BLX. */
        p->op = H ? PD_BL : PD_BLX;
        p->imm = (uint32_t)((int32_t)(pc + 4) + off);
        p->len = 4;
        break;
    }
    }
    return p;
}

/* ---- execution ----------------------------------------------------------- */

static inline pdec *fall(pdec *p)
{
    if (!p->fnext)
        p->fnext = pd_lookup(p->key + p->len);
    return p->fnext;
}

static inline pdec *take(pdec *p, uint32_t target)
{
    if (!p->ftaken)
        p->ftaken = pd_lookup(target);
    return p->ftaken;
}

static int pd_trap(pdec *p, int reason)
{
    g_fault_pc = p->key;
    g_fault_insn = p->insn;
    g_fault_width = p->len;
    for (int i = 0; i < 16; i++)
        g_fault_r[i] = g_cpu.r[i];
    return reason;
}

int aot_predec_run(uint64_t budget)
{
    uint32_t *R = g_cpu.r;
    uint32_t pc = R[15];

    if (!g_buckets)
        g_buckets = (pdec **)host_alloc(sizeof(pdec *) * PD_BUCKETS);

    pdec *p = pd_lookup(pc);

    for (;;) {
        /* Same halt contract as cpu.c, in the same place: the check is before
         * the instruction, so a budget of N executes exactly N (counting a
         * 32-bit Thumb encoding as two). */
        if (g_insn_count >= budget)
            return HALT_BUDGET;

        /* One-shot snapshot point: stop *before* the instruction, so a dump
         * describes an unambiguous state. */
        if (g_stop_at && p->key == g_stop_at) {
            g_stop_at = 0;
            R[15] = p->key;
            return HALT_STOPPED;
        }

        pc = p->key;
        if (p->op == PD_BAD_PC) {
            /* Exactly what the interpreter's range check does: report the PC,
             * and do not advance the instruction counter. */
            R[15] = pc;
            return HALT_BAD_PC;
        }
        g_insn_count += p->len >> 1;
        R[15] = pc + p->len;
        pdec *next = NULL;

        switch (p->op) {
        case PD_UNSUP:
            return pd_trap(p, HALT_TRAP);

        case PD_SHIFT: {
            uint32_t c, v;
            switch (p->a) {
            case 0: v = sh_lsl(R[p->rm], p->imm, &c); break;
            case 1: v = sh_lsr(R[p->rm], p->imm ? p->imm : 32, &c); break;
            default: v = sh_asr(R[p->rm], p->imm ? p->imm : 32, &c); break;
            }
            R[p->rd] = v;
            g_cpu.cpsr = (g_cpu.cpsr & ~(F_N | F_Z | F_C)) | c |
                         (v & 0x80000000u ? F_N : 0) | (v == 0 ? F_Z : 0);
            break;
        }
        case PD_ADDSUB: {
            /* Rd = Rs +/- Rn: the source is bits[5:3] (p->rn), the operand is
             * bits[8:6] (p->rm or the 3-bit immediate), and Rd is written. */
            uint32_t op2 = p->a ? p->imm : R[p->rm];
            R[p->rd] = p->b ? sub_c(R[p->rn], op2, 1)
                            : add_c(R[p->rn], op2, 0);
            break;
        }
        case PD_IMM8:
            switch (p->a) {
            case 0: R[p->rd] = p->imm; set_nz(p->imm); break;
            case 1: sub_c(R[p->rd], p->imm, 1); break;
            case 2: R[p->rd] = add_c(R[p->rd], p->imm, 0); break;
            default: R[p->rd] = sub_c(R[p->rd], p->imm, 1); break;
            }
            break;

        case PD_ALU: {
            uint32_t rd = p->rd, rm = p->rm, c;
            switch (p->a) {
            case 0x0: R[rd] &= R[rm]; set_nz(R[rd]); break;
            case 0x1: R[rd] ^= R[rm]; set_nz(R[rd]); break;
            case 0x2: R[rd] = sh_lsl(R[rd], R[rm] & 0xff, &c);
                      g_cpu.cpsr = (g_cpu.cpsr & ~(F_N|F_Z|F_C)) | c |
                                   (R[rd] & 0x80000000u ? F_N : 0) |
                                   (R[rd] == 0 ? F_Z : 0); break;
            case 0x3: R[rd] = sh_lsr(R[rd], R[rm] & 0xff, &c);
                      g_cpu.cpsr = (g_cpu.cpsr & ~(F_N|F_Z|F_C)) | c |
                                   (R[rd] & 0x80000000u ? F_N : 0) |
                                   (R[rd] == 0 ? F_Z : 0); break;
            case 0x4: R[rd] = sh_asr(R[rd], R[rm] & 0xff, &c);
                      g_cpu.cpsr = (g_cpu.cpsr & ~(F_N|F_Z|F_C)) | c |
                                   (R[rd] & 0x80000000u ? F_N : 0) |
                                   (R[rd] == 0 ? F_Z : 0); break;
            case 0x5: R[rd] = add_c(R[rd], R[rm], (g_cpu.cpsr & F_C) ? 1 : 0); break;
            case 0x6: R[rd] = sub_c(R[rd], R[rm], (g_cpu.cpsr & F_C) ? 1 : 0); break;
            case 0x7: R[rd] = sh_ror(R[rd], R[rm] & 0xff, &c);
                      g_cpu.cpsr = (g_cpu.cpsr & ~(F_N|F_Z|F_C)) | c |
                                   (R[rd] & 0x80000000u ? F_N : 0) |
                                   (R[rd] == 0 ? F_Z : 0); break;
            case 0x8: set_nz(R[rd] & R[rm]); break;                     /* TST */
            case 0x9: R[rd] = sub_c(0, R[rm], 1); break;                /* RSB */
            case 0xa: sub_c(R[rd], R[rm], 1); break;                    /* CMP */
            case 0xb: add_c(R[rd], R[rm], 0); break;                    /* CMN */
            case 0xc: R[rd] |= R[rm]; set_nz(R[rd]); break;             /* ORR */
            case 0xd: R[rd] = (uint32_t)((uint64_t)R[rd] * R[rm]);
                      set_nz(R[rd]); break;                             /* MUL */
            case 0xe: R[rd] &= ~R[rm]; set_nz(R[rd]); break;            /* BIC */
            default:  R[rd] = ~R[rm]; set_nz(R[rd]); break;             /* MVN */
            }
            break;
        }
        case PD_HI: {
            /* Reading PC as a source yields pc + 4, not R[15] (which is
             * pc + 2): this is the GOT-base idiom -- see cpu.c. */
            uint32_t src = (p->rm == 15) ? (pc + 4) : R[p->rm];
            switch (p->a) {
            case 0: R[p->rd] += src; break;
            case 1: sub_c(R[p->rd], src, 1); break;
            default: R[p->rd] = src; break;
            }
            /* MOV/ADD to PC is a computed jump that does not interwork. */
            if (p->rd == 15)
                next = pd_lookup(R[15] & ~1u);
            break;
        }
        case PD_BX: {
            uint32_t src = (p->rm == 15) ? (pc + 4) : R[p->rm];
            if (p->insn & 0x0080)                   /* BLX: set LR        */
                R[14] = (pc + 2) | 1u;
            if (!(src & 1u)) {
                R[15] = src & ~1u;
                return pd_trap(p, HALT_TRAP);       /* ARM state: not here */
            }
            next = pd_lookup(src & ~1u);
            break;
        }
        case PD_LDRLIT:
            R[p->rd] = LD32(p->imm);
            break;

        case PD_LDST_REG: {
            uint32_t addr = R[p->rn] + R[p->rm];
            switch (p->a) {
            case 0: ST32(addr, R[p->rd]); break;
            case 1: ST16(addr, (uint16_t)R[p->rd]); break;
            case 2: ST8(addr, (uint8_t)R[p->rd]); break;
            case 3: R[p->rd] = (uint32_t)(int32_t)(int8_t)LD8(addr); break;
            case 4: R[p->rd] = LD32(addr); break;
            case 5: R[p->rd] = LD16(addr); break;
            case 6: R[p->rd] = LD8(addr); break;
            default: R[p->rd] = (uint32_t)(int32_t)(int16_t)LD16(addr); break;
            }
            break;
        }
        case PD_LDST_IMM: {
            uint32_t scale = (p->a == 2) ? 0 : (p->a == 0 ? 2 : 1);
            uint32_t addr = R[p->rn] + (p->imm << scale);
            if (p->a == 0) {                        /* word               */
                if (p->b) R[p->rd] = LD32(addr); else ST32(addr, R[p->rd]);
            } else if (p->a == 1) {                 /* halfword           */
                if (p->b) R[p->rd] = LD16(addr); else ST16(addr, (uint16_t)R[p->rd]);
            } else {                                /* byte               */
                if (p->b) R[p->rd] = LD8(addr); else ST8(addr, (uint8_t)R[p->rd]);
            }
            break;
        }
        case PD_LDST_SP: {
            uint32_t addr = R[13] + (p->imm << 2);
            if (p->b) R[p->rd] = LD32(addr); else ST32(addr, R[p->rd]);
            break;
        }
        case PD_ADDR:
            R[p->rd] = (p->b ? R[13] : ((pc + 4) & ~3u)) + p->imm;
            break;
        case PD_ADJSP:
            R[13] = p->b ? R[13] - p->imm : R[13] + p->imm;
            break;

        case PD_PUSH: {
            /* Registers go to ascending addresses in ascending register order,
             * LR last. Reversing this hands back a data word as the return
             * address -- see cpu.c's note. */
            uint32_t list = p->imm, n = 0;
            for (uint32_t r = 0; r < 8; r++)
                if (list & (1u << r)) n++;
            if (p->b) n++;
            R[13] -= 4 * n;
            uint32_t addr = R[13];
            for (uint32_t r = 0; r < 8; r++)
                if (list & (1u << r)) { ST32(addr, R[r]); addr += 4; }
            if (p->b) ST32(addr, R[14]);
            break;
        }
        case PD_POP: {
            uint32_t list = p->imm;
            for (uint32_t r = 0; r < 8; r++)
                if (list & (1u << r)) { R[r] = LD32(R[13]); R[13] += 4; }
            if (p->b) {
                uint32_t val = LD32(R[13]);
                R[13] += 4;
                if (!(val & 1u)) {
                    R[15] = val & ~1u;
                    return pd_trap(p, HALT_TRAP);   /* ARM state: not here */
                }
                next = pd_lookup(val & ~1u);
            }
            break;
        }
        case PD_REV: {
            uint32_t v = R[p->rm];
            if (p->a == 0)
                R[p->rd] = ((v & 0xff) << 24) | ((v & 0xff00) << 8) |
                           ((v >> 8) & 0xff00) | (v >> 24);
            else if (p->a == 1)
                R[p->rd] = ((v & 0xff) << 8) | ((v >> 8) & 0xff) |
                           ((v & 0xff0000) << 8) | ((v >> 8) & 0xff0000);
            else
                R[p->rd] = (uint32_t)(int32_t)(int16_t)(((v & 0xff) << 8) |
                                                        ((v >> 8) & 0xff));
            break;
        }
        case PD_EXT: {
            uint32_t v = R[p->rm];
            switch (p->a) {
            case 0: R[p->rd] = (uint32_t)(int32_t)(int16_t)v; break;
            case 1: R[p->rd] = (uint32_t)(int32_t)(int8_t)v; break;
            case 2: R[p->rd] = v & 0xffff; break;
            default: R[p->rd] = v & 0xff; break;
            }
            break;
        }
        case PD_NOP:
            break;

        case PD_BKPT: {
            int idx = import_index_of_stub(pc);
            if (idx >= 0 && idx < g_nimports) {
                R[15] = pc + 2;
                if (import_dispatch(idx)) {         /* oracle mode: stop  */
                    R[15] = pc;
                    return HALT_IMPORT;
                }
                uint32_t val = R[14];
                if (!(val & 1u)) {
                    R[15] = val & ~1u;
                    return pd_trap(p, HALT_TRAP);
                }
                next = pd_lookup(val & ~1u);
                break;
            }
            if (host_hook_dispatch(p->a)) {
                uint32_t val = R[14];
                if (!(val & 1u)) {
                    R[15] = val & ~1u;
                    return pd_trap(p, HALT_TRAP);
                }
                next = pd_lookup(val & ~1u);
                break;
            }
            return HALT_HALTED;
        }

        case PD_SWI:
            return pd_trap(p, HALT_TRAP);

        case PD_UNDEF:
            return HALT_UNDEFINED;

        case PD_LDMIA: {
            uint32_t addr = R[p->rn];
            for (uint32_t r = 0; r < 8; r++)
                if (p->imm & (1u << r)) { R[r] = LD32(addr); addr += 4; }
            R[p->rn] = addr;    /* base in the list: the written-back value */
            break;
        }
        case PD_STMIA: {
            uint32_t addr = R[p->rn];
            for (uint32_t r = 0; r < 8; r++)
                if (p->imm & (1u << r)) { ST32(addr, R[r]); addr += 4; }
            R[p->rn] = addr;
            break;
        }

        case PD_BCOND:
            if (cond_true(p->a))
                next = take(p, (uint32_t)((int32_t)(pc + 4) + (int32_t)p->imm));
            break;

        case PD_B:
            next = take(p, (uint32_t)((int32_t)(pc + 4) + (int32_t)p->imm));
            break;

        case PD_BL:
            R[14] = (pc + 4) | 1u;
            next = take(p, p->imm);
            break;

        case PD_BLX:
            /* A call into ARM code (the .plt is ARM in the real image). This
             * build has no ARM mode, so stop with the state cpu.c would have
             * after the call: LR set, PC at the ARM target. */
            R[14] = (pc + 4) | 1u;
            R[15] = p->imm & ~3u;
            return pd_trap(p, HALT_TRAP);

        default:
            break;
        }

        if (g_trace_insn_on && p->emit)
            host_trace_insn(pc, R, g_cpu.cpsr);

        p = next ? next : fall(p);
    }
}

/* ---- mode selection and reporting ---------------------------------------- */

int aot_set_mode(const char *name)
{
    if (!name || !*name || strcmp(name, "off") == 0 || strcmp(name, "0") == 0) {
        g_aot_mode = AOT_NONE;
        return 0;
    }
    if (strcmp(name, "predec") == 0) {
        g_aot_mode = AOT_PREDEC;
        return 0;
    }
#if defined(__aarch64__) && !defined(WASM_FREESTANDING)
    if (strcmp(name, "arm64") == 0 || strcmp(name, "on") == 0 ||
        strcmp(name, "1") == 0) {
        g_aot_mode = AOT_ARM64;
        return 0;
    }
#endif
    return -1;
}

void aot_stats_report(void)
{
    /* Silent when off: every existing run's output has to stay byte-identical,
     * and the regression tools parse these logs. */
    if (g_aot_mode == AOT_ARM64) {
#if defined(__aarch64__) && !defined(WASM_FREESTANDING)
        aot_arm64_stats();
#endif
        return;
    }
    if (g_aot_mode != AOT_PREDEC)
        return;
    host_log("aot: predec -- %u instruction(s) decoded once, %llu lookups "
             "(%.1f per decoded), %.0f KB of pdec\n",
             g_ndecoded, (unsigned long long)g_nlookup,
             g_ndecoded ? (double)g_nlookup / (double)g_ndecoded : 0.0,
             (double)g_ndecoded * (double)sizeof(pdec) / 1024.0);
}
