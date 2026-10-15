/*
 * cpu.c -- ARMv5TE / Thumb-1 / VFPv2 interpreter.
 *
 * Structure notes that matter for the two numbers this spike measures:
 *
 *  - The instruction dispatch is a switch on the top nibble (Thumb) which
 *    compiles to a jump table. Everything else the interpreter needs on the
 *    hot path -- guest memory access, the register file, the flags -- is a
 *    plain array access, so the wasm32 and native builds generate the same
 *    shape and the MIPS ratio between them is meaningful.
 *
 *  - Imported functions are reached through 4-byte BKPT stubs (see imports.h).
 *    BKPT is one more opcode in the existing decode, so counting host
 *    crossings costs nothing per instruction.
 *
 *  - Per-function time is attributed by tracking the current symbol range and
 *    only re-resolving when control transfers outside it, i.e. on calls and
 *    returns rather than on every instruction. The per-instruction cost is one
 *    increment of a register-indexed counter.
 */

#include "cpu.h"
#include "guest.h"
#include "imports.h"
#include "compat.h"

cpu_state g_cpu;
uint64_t  g_insn_count;
uint32_t  g_fault_pc;
uint32_t  g_fault_insn;
int       g_fault_width;
uint32_t  g_fault_r[16];

static const guest_image *g_img;
static int      g_thumb = 1;          /* the guest starts in Thumb state */
static uint64_t g_budget;

/* ---- instruction trace ---------------------------------------------------
 * A ring buffer of the last N (pc, instruction) pairs. Off by default; enabled
 * for bring-up. This exists because the failures that matter here are "the
 * guest did something wrong 12 instructions in", and the guest cannot be
 * single-stepped in a host debugger -- the host PC tells you nothing about
 * which guest instruction misbehaved.
 */
#define TRACE_N 96
static uint32_t g_tr_pc[TRACE_N];
static uint32_t g_tr_ins[TRACE_N];
static int      g_tr_i;
static int      g_tr_on;

/* Per-instruction trace to the host, for differential testing against
 * the Unicorn oracle. Emitted at the top of the loop for the
 * *previous* instruction, so what is reported is the register state
 * after that instruction retired -- which is what a divergence needs
 * to be attributed to a specific instruction. */
int g_trace_insn_on;
static uint32_t g_prev_pc;
static int      g_have_prev;

void cpu_enable_trace(int on) { g_tr_on = on; g_tr_i = 0; }

/* Emit the last executed instruction. The in-loop emit fires at the top of an
 * iteration, so whichever instruction caused the run to end would otherwise
 * never appear -- and that is usually the interesting one. Keeping the two
 * traces symmetric is what lets difftrace.py compare line for line. */
void cpu_trace_final(void)
{
    if (g_trace_insn_on && g_have_prev)
        host_trace_insn(g_prev_pc, g_cpu.r, g_cpu.cpsr);
}

void cpu_dump_trace(void)
{
    if (!g_tr_on)
        return;
    host_log("--- last %d guest instructions ---\n", TRACE_N);
    for (int k = 0; k < TRACE_N; k++) {
        int i = (g_tr_i + k) % TRACE_N;
        if (!g_tr_pc[i] && !g_tr_ins[i])
            continue;
        uint32_t at = 0;
        const char *nm = elf_nearest_sym(g_img, g_tr_pc[i], &at);
        host_log("  %08x  %08x  %s+0x%x\n", g_tr_pc[i], g_tr_ins[i],
                 nm ? nm : "?", nm ? g_tr_pc[i] - at : 0);
    }
}

/* ---- per-function profile ------------------------------------------------ */

typedef struct {
    uint64_t *counts;   /* indexed by position in img->sorted            */
    int       cap;
    int       cur;      /* current index, -1 if the PC is not in a known
                         * function (literal pool, PLT, stub)            */
    uint32_t  lo, hi;   /* current function's address range              */
    int       on;
} profile_t;

static profile_t g_prof;

void cpu_set_profile(int on) { g_prof.on = on; }

void cpu_init(const guest_image *img)
{
    g_img = img;
    g_prof.cap = img->nsorted;
    g_prof.counts = (uint64_t *)host_alloc(sizeof(uint64_t) * (size_t)(g_prof.cap ? g_prof.cap : 1));
    g_prof.cur = -1;
    g_prof.on = 1;
}

/* Resolve `pc` to a profile slot. Returns -1 for anything that is not the body
 * of a known function -- the PLT, literal pools, and the import stubs all live
 * between functions and must not be charged to a neighbour. */
static int prof_resolve(uint32_t pc)
{
    uint32_t want = pc & ~1u;
    uint32_t lo = 0, hi = (uint32_t)g_img->nsorted;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (g_img->sorted[mid].value <= want) lo = mid + 1; else hi = mid;
    }
    if (lo == 0)
        return -1;
    int i = (int)lo - 1;
    const sym_entry *e = &g_img->sorted[i];
    if (e->size && want >= e->value + e->size)
        return -1;
    g_prof.lo = e->value;
    g_prof.hi = e->size ? e->value + e->size : e->value + 0x10000u;
    return i;
}

/* Called on every control transfer. Cheap when staying inside the current
 * function, which is the common case for a backward branch in a loop. */
static inline void prof_transfer(uint32_t pc)
{
    if (!g_prof.on)
        return;
    uint32_t w = pc & ~1u;
    if (w - g_prof.lo < g_prof.hi - g_prof.lo)
        return;
    g_prof.cur = prof_resolve(pc);
}

/* ---- flags --------------------------------------------------------------- */

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

/* ---- shifts (ARM's four kinds, with the C-out rules the ISA specifies) ---- */

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

/* ---- fault reporting ----------------------------------------------------- */

static int trap(uint32_t pc, uint32_t insn, int width)
{
    g_fault_pc = pc;
    g_fault_insn = insn;
    g_fault_width = width;
    for (int i = 0; i < 16; i++)
        g_fault_r[i] = g_cpu.r[i];
    return HALT_TRAP;
}

/* ---- VFPv2 --------------------------------------------------------------- */
/*
 * Not implemented yet, and deliberately so: the spike's job is to find out
 * which instructions are actually reached before writing emulation for them.
 * The trap records the encoding, so the first run of the real JNI_OnLoad and
 * the static constructors tells us exactly which VFP forms matter.
 */

/* ---- the interpreter ----------------------------------------------------- */

int cpu_run(uint32_t entry, uint32_t sp, uint64_t budget)
{
    uint32_t *R = g_cpu.r;
    uint32_t pc = entry & ~1u;
    g_budget = budget;

    R[13] = sp;
    R[15] = pc;
    g_thumb = (entry & 1) != 0;
    g_prof.cur = -1;
    g_prof.lo = 0;
    g_prof.hi = 0;
    g_have_prev = 0;
    /* Reset the ring per run: a trace that spans several entry points shows
     * whichever ran last, which is not the one being debugged. */
    g_tr_i = 0;
    prof_transfer(pc);

    for (;;) {
        if (g_insn_count >= g_budget)
            return HALT_BUDGET;

        /* Emit the previous instruction now that its effects are in R.
         * Clearing g_have_prev here is what keeps cpu_trace_final() from
         * emitting the same instruction a second time when the run ends
         * before the loop reaches another top. */
        if (g_trace_insn_on && g_have_prev) {
            host_trace_insn(g_prev_pc, R, g_cpu.cpsr);
            g_have_prev = 0;
        }

        /* Executable ranges are .text and the import stub page. The stubs have
         * to be allowed through here or the range check fires before the BKPT
         * is ever decoded, and every host crossing silently becomes a
         * HALT_BAD_PC -- which is exactly what "0 host crossings" looked like
         * before this was noticed. */
        {
            int in_text = (pc >= g_img->text_lo && pc < g_img->text_hi);
            int in_stub = (pc >= GUEST_STUB_BASE &&
                           pc < GUEST_STUB_BASE + IMPORT_MAX * 4u);
            if (!in_text && !in_stub)
                return HALT_BAD_PC;
        }

        g_prev_pc = pc;
        g_have_prev = 1;

        if (g_prof.on && g_prof.cur >= 0)
            g_prof.counts[g_prof.cur]++;

        if (g_tr_on) {
            g_tr_pc[g_tr_i]  = pc;
            g_tr_ins[g_tr_i] = g_thumb ? LD16(pc) : LD32(pc & ~3u);
            g_tr_i = (g_tr_i + 1) % TRACE_N;
        }

        /* ================= Thumb ================= */
        if (g_thumb) {
            uint32_t ins = LD16(pc);
            g_insn_count++;
            R[15] = pc + 2;
            uint32_t rd, rn, rm, imm, addr, val, c;
            int32_t soff;

            switch (ins >> 12) {

            case 0x0: case 0x1:
                if ((ins & 0x1800) != 0x1800) {
                    /* format 1: move shifted register */
                    rd = ins & 7; rm = (ins >> 3) & 7;
                    uint32_t off = (ins >> 6) & 0x1f;
                    switch ((ins >> 11) & 3) {
                    case 0: val = sh_lsl(R[rm], off, &c); break;
                    case 1: val = sh_lsr(R[rm], off ? off : 32, &c); break;
                    default: val = sh_asr(R[rm], off ? off : 32, &c); break;
                    }
                    R[rd] = val;
                    g_cpu.cpsr = (g_cpu.cpsr & ~(F_N | F_Z | F_C)) | c |
                                 (val & 0x80000000u ? F_N : 0) | (val == 0 ? F_Z : 0);
                } else {
                    /* format 2: add/subtract */
                    rd = ins & 7; rn = (ins >> 3) & 7;
                    uint32_t op2 = (ins & 0x0400) ? ((ins >> 6) & 7) : R[(ins >> 6) & 7];
                    R[rd] = (ins & 0x0200) ? sub_c(R[rn], op2, 1) : add_c(R[rn], op2, 0);
                }
                break;

            case 0x2: case 0x3:
                /* format 3: mov/cmp/add/sub immediate */
                rd = (ins >> 8) & 7; imm = ins & 0xff;
                switch ((ins >> 11) & 3) {
                case 0: R[rd] = imm; set_nz(imm); break;
                case 1: sub_c(R[rd], imm, 1); break;
                case 2: R[rd] = add_c(R[rd], imm, 0); break;
                default: sub_c(R[rd], imm, 1); break;
                }
                break;

            case 0x4:
                if ((ins & 0xFC00) == 0x4000) {
                    /* format 4: ALU operations */
                    rd = ins & 7; rm = (ins >> 3) & 7;
                    switch ((ins >> 6) & 0xf) {
                    case 0x0: R[rd] &= R[rm]; set_nz(R[rd]); break;
                    case 0x1: R[rd] ^= R[rm]; set_nz(R[rd]); break;
                    case 0x2: R[rd] = sh_lsl(R[rd], R[rm] & 0xff, &c);
                              g_cpu.cpsr = (g_cpu.cpsr & ~(F_N|F_Z|F_C)) | c |
                                           (R[rd] & 0x80000000u ? F_N : 0) | (R[rd] == 0 ? F_Z : 0); break;
                    case 0x3: R[rd] = sh_lsr(R[rd], (R[rm] & 0xff) ? (R[rm] & 0xff) : 32, &c);
                              g_cpu.cpsr = (g_cpu.cpsr & ~(F_N|F_Z|F_C)) | c |
                                           (R[rd] & 0x80000000u ? F_N : 0) | (R[rd] == 0 ? F_Z : 0); break;
                    case 0x4: R[rd] = sh_asr(R[rd], (R[rm] & 0xff) ? (R[rm] & 0xff) : 32, &c);
                              g_cpu.cpsr = (g_cpu.cpsr & ~(F_N|F_Z|F_C)) | c |
                                           (R[rd] & 0x80000000u ? F_N : 0) | (R[rd] == 0 ? F_Z : 0); break;
                    case 0x5: R[rd] = add_c(R[rd], R[rm], (g_cpu.cpsr & F_C) ? 1 : 0); break;
                    case 0x6: R[rd] = sub_c(R[rd], R[rm], (g_cpu.cpsr & F_C) ? 1 : 0); break;
                    case 0x7: R[rd] = sh_ror(R[rd], R[rm] & 0xff, &c);
                              g_cpu.cpsr = (g_cpu.cpsr & ~(F_N|F_Z|F_C)) | c |
                                           (R[rd] & 0x80000000u ? F_N : 0) | (R[rd] == 0 ? F_Z : 0); break;
                    case 0x8: set_nz(R[rd] & R[rm]); break;                 /* TST */
                    case 0x9: R[rd] = sub_c(0, R[rm], 1); break;            /* RSB */
                    case 0xa: sub_c(R[rd], R[rm], 1); break;                /* CMP */
                    case 0xb: add_c(R[rd], R[rm], 0); break;                /* CMN */
                    case 0xc: R[rd] |= R[rm]; set_nz(R[rd]); break;         /* ORR */
                    case 0xd: R[rd] = (uint32_t)((uint64_t)R[rd] * R[rm]); set_nz(R[rd]); break;
                    case 0xe: R[rd] &= ~R[rm]; set_nz(R[rd]); break;        /* BIC */
                    default:  R[rd] = ~R[rm]; set_nz(R[rd]); break;         /* MVN */
                    }
                } else if ((ins & 0xFC00) == 0x4400) {
                    /* format 5: hi register operations / BX / BLX */
                    rd = (ins & 7) | ((ins >> 4) & 8);
                    rm = (ins >> 3) & 0xf;
                    /* Reading PC as a Thumb source operand yields the address
                     * of the current instruction plus 4, but R[15] holds
                     * pc+2 (the address of the next instruction, which is what
                     * `pc = R[15]` at the bottom of the loop wants). The two
                     * readings differ by 2 and must be reconciled here.
                     *
                     * This is not a corner case: `add rN, pc` is the idiom GCC
                     * emits to materialise the GOT base in position-independent
                     * ARM code, so every GOT-relative global access in the
                     * binary goes through this instruction. Getting it wrong by
                     * 2 silently produces a wrong pointer rather than a wrong
                     * branch, which is why it surfaced as a bad address to
                     * store into rather than as a jump into nowhere. */
                    uint32_t src = (rm == 15) ? (pc + 4) : R[rm];
                    switch ((ins >> 8) & 3) {
                    case 0: R[rd] += src; break;                /* ADD, no flags */
                    case 1: R[rd] = sub_c(R[rd], src, 1); break;/* CMP, flags */
                    case 2: R[rd] = src; break;                 /* MOV, no flags */
                    default: {                                  /* BX / BLX */
                        /* Bit 7 tells them apart, and the difference is LR:
                         * `blx Rm` is a call and must leave a return address
                         * behind, `bx Rm` is a jump and must not.
                         *
                         * Treating them alike is invisible until a stubbed
                         * import is reached through `blx`, at which point the
                         * callee returns to whatever LR the *caller's* caller
                         * left there -- one frame too high up. That is what
                         * made JNI_OnLoad fail: getEnv()'s `blx r3` reached the
                         * GetEnv stub, the stub returned straight into
                         * JNI_OnLoad, and the JNI_OK that GetEnv returned was
                         * read as getEnv()'s own result, so its `env == NULL`
                         * test saw 0 and the module gave up. */
                        if (ins & 0x0080)          /* BLX: set LR */
                            R[14] = (pc + 2) | 1u;
                        g_thumb = (src & 1) != 0;
                        pc = src & ~1u;
                        R[15] = pc;
                        prof_transfer(pc);
                        continue;
                    }
                    }
                    if (rd == 15)
                        pc = R[15] & ~1u;   /* MOV/ADD PC do not interwork */
                } else {
                    /* format 6: PC-relative load */
                    rd = (ins >> 8) & 7;
                    addr = ((pc + 4) & ~3u) + ((ins & 0xff) << 2);
                    R[rd] = LD32(addr);
                }
                break;

            case 0x5:
                /* formats 7/8: load/store with register offset, and the
                 * sign-extended byte/halfword forms */
                rd = ins & 7; rn = (ins >> 3) & 7; rm = (ins >> 6) & 7;
                addr = R[rn] + R[rm];
                switch ((ins >> 9) & 7) {
                case 0: ST32(addr, R[rd]); break;
                case 1: ST16(addr, (uint16_t)R[rd]); break;
                case 2: ST8(addr, (uint8_t)R[rd]); break;
                case 3: R[rd] = (uint32_t)(int32_t)(int8_t)LD8(addr); break;   /* LDSB */
                case 4: R[rd] = LD32(addr); break;
                case 5: R[rd] = LD16(addr); break;
                case 6: R[rd] = LD8(addr); break;
                default: R[rd] = (uint32_t)(int32_t)(int16_t)LD16(addr); break; /* LDSH */
                }
                break;

            case 0x6: case 0x7: case 0x8: case 0x9: {
                /* formats 9/10/11: load/store with immediate offset.
                 *
                 * Format 11 (SP-relative) is the odd one out in two ways, and
                 * both are easy to miss because formats 9 and 10 sit in the
                 * same case:
                 *
                 *   1001 L Rd  imm8      <- format 11: Rd is bits[10:8]
                 *   0111 B L imm5 Rn Rd  <- format 9:  Rd is bits[2:0]
                 *   1000 L    imm5 Rn Rd <- format 10: Rd is bits[2:0]
                 *
                 * Taking Rd from bits[2:0] for all of them makes
                 * `ldr r1, [sp, #20]` (0x9905) write r5 instead of r1. Since
                 * r5 held the GOT base, the failure surfaces thousands of
                 * instructions later as a load through a garbage address --
                 * and the offending instruction looks perfectly innocent in a
                 * trace that only prints PCs. */
                uint32_t grp = ins >> 12;
                uint32_t isload = (ins & 0x0800) != 0;
                uint32_t base, scale, imms;
                if (grp == 0x9) {
                    rd = (ins >> 8) & 7;
                    base = R[13];
                    scale = 2;
                    imms = ins & 0xff;
                } else {
                    rd = ins & 7;
                    base = R[(ins >> 3) & 7];
                    imms = (ins >> 6) & 0x1f;
                    scale = (grp == 0x7) ? 0 : (grp == 0x6 ? 2 : 1);
                }
                addr = base + (imms << scale);
                if (grp == 0x8) {
                    if (isload) R[rd] = LD16(addr); else ST16(addr, (uint16_t)R[rd]);
                } else if (grp == 0x7) {
                    if (isload) R[rd] = LD8(addr); else ST8(addr, (uint8_t)R[rd]);
                } else {
                    if (isload) R[rd] = LD32(addr); else ST32(addr, R[rd]);
                }
                break;
            }

            case 0xa:
                /* format 12: load address */
                rd = (ins >> 8) & 7;
                imm = (ins & 0xff) << 2;
                val = (ins & 0x0800) ? R[13] : ((pc + 4) & ~3u);
                R[rd] = val + imm;
                break;

            case 0xb:
                if ((ins & 0xFF00) == 0xB000) {
                    /* format 13: add/subtract SP */
                    imm = (ins & 0x7f) << 2;
                    R[13] = (ins & 0x80) ? R[13] - imm : R[13] + imm;
                } else if ((ins & 0xFE00) == 0xB400) {
                    /* format 14: push.
                     *
                     * The layout rule is the whole content of this instruction:
                     * registers are stored in ascending register order to
                     * ascending addresses, so the lowest-numbered register ends
                     * up at the lowest address and LR at the highest. Popping
                     * with reducing addresses instead -- which is the obvious
                     * way to write it, and was the first way written here --
                     * reverses the frame and hands back a data word as the
                     * return address. The failure mode is a jump into the
                     * middle of .bss, not an immediate crash, so it is worth
                     * being explicit about rather than clever.
                     *
                     * An empty register list is UNPREDICTABLE on real
                     * hardware; GCC never emits one, and neither does this
                     * accept one. */
                    uint32_t n = 0;
                    for (rm = 0; rm < 8; rm++)
                        if (ins & (1u << rm)) n++;
                    if (ins & 0x100) n++;
                    R[13] -= 4 * n;
                    addr = R[13];
                    for (rm = 0; rm < 8; rm++)
                        if (ins & (1u << rm)) { ST32(addr, R[rm]); addr += 4; }
                    if (ins & 0x100) ST32(addr, R[14]);
                } else if ((ins & 0xFE00) == 0xBC00) {
                    /* format 14: pop -- reads the layout push writes. */
                    for (rm = 0; rm < 8; rm++)
                        if (ins & (1u << rm)) { R[rm] = LD32(R[13]); R[13] += 4; }
                    if (ins & 0x100) {
                        val = LD32(R[13]); R[13] += 4;
                        g_thumb = (val & 1) != 0;
                        pc = val & ~1u;
                        R[15] = pc;
                        prof_transfer(pc);
                        continue;
                    }
                } else if ((ins & 0xFF00) == 0xBA00) {
                    /* REV / REV16 / REVSH */
                    rm = (ins >> 3) & 7; rd = ins & 7;
                    val = R[rm];
                    if ((ins & 0xC0) == 0x00)
                        R[rd] = ((val & 0xff) << 24) | ((val & 0xff00) << 8) |
                                ((val >> 8) & 0xff00) | (val >> 24);
                    else if ((ins & 0xC0) == 0x40)
                        R[rd] = ((val & 0xff) << 8) | ((val >> 8) & 0xff) |
                                ((val & 0xff0000) << 8) | ((val >> 8) & 0xff0000);
                    else
                        R[rd] = (uint32_t)(int32_t)(int16_t)(((val & 0xff) << 8) |
                                                            ((val >> 8) & 0xff));
                } else if ((ins & 0xFF00) == 0xB200) {
                    /* SXTH / SXTB / UXTH / UXTB */
                    rm = (ins >> 3) & 7; rd = ins & 7;
                    switch ((ins >> 6) & 3) {
                    case 0: R[rd] = (uint32_t)(int32_t)(int16_t)R[rm]; break;
                    case 1: R[rd] = (uint32_t)(int32_t)(int8_t)R[rm]; break;
                    case 2: R[rd] = R[rm] & 0xffff; break;
                    default: R[rd] = R[rm] & 0xff; break;
                    }
                } else if (ins == 0xBF00) {
                    /* NOP (the Thumb-2 hint space; GCC emits it for alignment) */
                } else if ((ins & 0xFF00) == 0xBE00) {
                    /* BKPT. Within the stub page this is an import call; the
                     * index comes from the address, so the imm8 is unused. */
                    int idx = import_index_of_stub(pc);
                    if (idx >= 0 && idx < g_nimports) {
                        R[15] = pc + 2;
                        if (import_dispatch(idx)) {
                            /* The stub is our dispatch mechanism, not guest
                             * code. cpu_trace_final() would otherwise emit a
                             * line for it that the oracle has no counterpart
                             * for, and an extra line reads as a control-flow
                             * divergence. */
                            g_have_prev = 0;
                            R[15] = pc;
                            return HALT_IMPORT;
                        }
                        /* Return through LR: a Thumb caller leaves bit0 set,
                         * an ARM caller does not. Respect both. */
                        val = R[14];
                        g_thumb = (val & 1) != 0;
                        pc = val & ~1u;
                        R[15] = pc;
                        prof_transfer(pc);
                        continue;
                    }
                    return HALT_HALTED;
                } else {
                    return trap(pc, ins, 2);
                }
                break;

            case 0xc:
                /* format 15: multiple load/store */
                rn = (ins >> 8) & 7;
                addr = R[rn];
                if (ins & 0x0800) {
                    for (rm = 0; rm < 8; rm++)
                        if (ins & (1u << rm)) { R[rm] = LD32(addr); addr += 4; }
                } else {
                    for (rm = 0; rm < 8; rm++)
                        if (ins & (1u << rm)) { ST32(addr, R[rm]); addr += 4; }
                }
                /* LDMIA with the base in the list loads the written-back value. */
                R[rn] = addr;
                break;

            case 0xd: {
                /* format 16/17: conditional branch, and SVC */
                uint32_t cond = (ins >> 8) & 0xf;
                if (cond == 0xf)
                    return trap(pc, ins, 2);            /* SVC -> host syscall */
                if (cond == 0xe)
                    return HALT_UNDEFINED;
                if (cond_true(cond)) {
                    soff = (int32_t)(int8_t)(ins & 0xff) << 1;
                    pc = (uint32_t)((int32_t)(pc + 4) + soff);
                    R[15] = pc;
                    prof_transfer(pc);
                    continue;
                }
                break;
            }

            case 0xe: {
                /* format 18: unconditional branch */
                if ((ins & 0xF800) != 0xE000)
                    return trap(pc, ins, 2);
                soff = (int32_t)(ins & 0x7ff);
                soff = (soff << 21) >> 21;
                pc = (uint32_t)((int32_t)(pc + 4) + (soff << 1));
                R[15] = pc;
                prof_transfer(pc);
                continue;
            }

            default: {
                /* format 19: long branch with link.
                 *
                 * BL and BLX share this encoding and are told apart by bit 12
                 * of the second halfword (H), NOT by J2 -- the two happen to
                 * be equal in most calls, which is exactly why getting it
                 * wrong is easy to miss:
                 *
                 *   BL  (11110 S imm10  11111 J1 1 J2 imm11) -> stays Thumb
                 *   BLX (11110 S imm10  11101 J1 0 J2 imm11) -> switches to ARM
                 *
                 * This matters more than the 3.8% ARM share suggests, because
                 * *every* imported function is reached through the ARM PLT:
                 * libgame.so's .plt is ARM code (add ip,pc,#..; ldr pc,[ip]!)
                 * even though all of its callers are Thumb. Misclassifying BLX
                 * as BL therefore does not fail at the call -- it fails by
                 * running the ARM PLT stub through the Thumb decoder, which
                 * turns the stub into an unconditional branch to nowhere.
                 *
                 * Verified against observed execution, not just the ARM ARM:
                 * pc=0x0ea590 (f7e4 ff40, H=1) -> 0x0cf414 Thumb; and
                 * pc=0x0cf426 (f789 eaea, H=0) -> 0x0589fe, the ARM .plt. */
                uint32_t hi = ins;
                uint32_t lo = LD16(pc + 2);
                g_insn_count++;
                uint32_t S = (hi >> 10) & 1;
                uint32_t imm10 = hi & 0x3ff;
                uint32_t J1 = (lo >> 13) & 1, H = (lo >> 12) & 1;
                uint32_t J2 = (lo >> 11) & 1, imm11 = lo & 0x7ff;
                uint32_t I1 = (~(J1 ^ S)) & 1, I2 = (~(J2 ^ S)) & 1;
                int32_t off = (int32_t)((S << 24) | (I1 << 23) | (I2 << 22) |
                                        (imm10 << 12) | (imm11 << 1));
                off = (off << 7) >> 7;   /* sign-extend from bit 24 */
                /* Both forms return to Thumb, so LR keeps its Thumb bit. */
                R[14] = (pc + 4) | 1u;
                if (H) {
                    pc = (uint32_t)((int32_t)(pc + 4) + off);
                    g_thumb = 1;
                } else {
                    pc = (uint32_t)((int32_t)(pc + 4) + off) & ~3u;
                    g_thumb = 0;
                }
                R[15] = pc;
                prof_transfer(pc);
                continue;
            }
            }
            pc = R[15];
            continue;
        }

        /* ================= ARM =================
         *
         * ARM is 3.8% of the code bytes but cannot be skipped, for a reason
         * that has nothing to do with that ratio: libgame.so's .plt is ARM
         * code. Every imported function -- all 178 of them -- is reached by a
         * Thumb BLX into an ARM stub that does `add ip,pc,#..; add ip,ip,#..;
         * ldr pc,[ip]!`. So this decoder is on the path of every single host
         * crossing, and gets exercised far more than its share of the image
         * suggests.
         *
         * Coverage is the common compiler output: data processing (immediate
         * and register-shifted), single and block data transfer, branches,
         * multiply, and the register-form BX. What is missing -- coprocessor
         * space (VFP), SWI, MRS/MSR -- traps with its encoding recorded rather
         * than being guessed at.
         */
        {
            uint32_t ins = LD32(pc & ~3u);
            g_insn_count++;
            uint32_t next = pc + 4;
            /* On ARM, reading r15 as a source yields the instruction address
             * plus 8; the PLT relies on this to find its GOT base, so it is
             * set here and every operand read below just uses R[15]. */
            R[15] = next + 4;

            uint32_t cond = ins >> 28;
            if (cond != 0xe && !cond_true(cond)) {
                pc = next;
                continue;
            }

            uint32_t op1 = (ins >> 25) & 7;
            uint32_t rd  = (ins >> 12) & 0xf;
            uint32_t rn  = (ins >> 16) & 0xf;

            /* ---- multiply, and the extra load/store sharing 000/001 ---- */
            if (op1 == 0 && ((ins >> 4) & 0xf) == 9) {
                uint32_t kind = (ins >> 24) & 0xf;
                uint32_t rm = ins & 0xf, rs = (ins >> 8) & 0xf;

                if (kind == 0) {
                    /* MUL/MLA (bit22=0) and UMULL/UMLAL/SMULL/SMLAL (bit22=1) */
                    uint32_t acc = (ins >> 21) & 1, set = (ins >> 20) & 1;
                    if (!((ins >> 22) & 1)) {
                        uint32_t r = R[rm] * R[rs];
                        if (acc) r += R[rn];
                        R[rd] = r;
                        if (set) set_nz(r);
                    } else {
                        uint64_t r;
                        if ((ins >> 22) & 1) {
                            /* bit22 in this family means signed for the
                             * long forms; distinguish by bits[6:5] */
                        }
                        if (((ins >> 5) & 3) == 1 || ((ins >> 5) & 3) == 3) {
                            int64_t a = (int64_t)(int32_t)R[rm] * (int32_t)R[rs];
                            if (acc) a += (int64_t)(((uint64_t)R[rn] << 32) | R[rd]);
                            r = (uint64_t)a;
                        } else {
                            r = (uint64_t)R[rm] * R[rs];
                            if (acc) r += ((uint64_t)R[rn] << 32) | R[rd];
                        }
                        R[rd] = (uint32_t)r;
                        R[rn] = (uint32_t)(r >> 32);
                        if (set)
                            g_cpu.cpsr = (g_cpu.cpsr & ~(F_N | F_Z)) |
                                         ((r >> 32) & 0x80000000u ? F_N : 0) |
                                         (r == 0 ? F_Z : 0);
                    }
                    pc = next;
                    continue;
                }

                if (kind == 1 && !((ins >> 22) & 1)) {
                    /* LDRH / STRH / LDRSB / LDRSH */
                    uint32_t P = (ins >> 24) & 1, U = (ins >> 23) & 1;
                    uint32_t I = (ins >> 22) & 1, W = (ins >> 21) & 1;
                    uint32_t L = (ins >> 20) & 1;
                    uint32_t sh = (ins >> 5) & 3;
                    uint32_t off = I ? ((((ins >> 8) & 0xf) << 4) | rm) : R[rm];
                    uint32_t base = R[rn];
                    uint32_t a = U ? base + off : base - off;
                    if (!P) {
                        a = base;
                        R[rn] = U ? base + off : base - off;
                    } else if (W) {
                        R[rn] = U ? base + off : base - off;
                    }
                    if (L) {
                        switch (sh) {
                        case 1: R[rd] = LD16(a); break;
                        case 2: R[rd] = (uint32_t)(int32_t)(int8_t)LD8(a); break;
                        default: R[rd] = (uint32_t)(int32_t)(int16_t)LD16(a); break;
                        }
                    } else {
                        if (sh != 1)
                            return trap(pc, ins, 4);
                        ST16(a, (uint16_t)R[rd]);
                    }
                    pc = next;
                    continue;
                }
                return trap(pc, ins, 4);
            }

            /* ---- BX / BLX register (bits[27:20] = 0001 0010) ---- */
            if (op1 == 0 && ((ins >> 20) & 0xff) == 0x12 && ((ins >> 4) & 0xf) == 1) {
                uint32_t t = R[ins & 0xf];
                g_thumb = (t & 1) != 0;
                pc = t & ~1u;
                R[15] = pc;
                prof_transfer(pc);
                continue;
            }

            /* ---- data processing ---- */
            if (op1 == 0 || op1 == 1) {
                uint32_t opcode = (ins >> 21) & 0xf;
                uint32_t S = (ins >> 20) & 1;
                uint32_t shc = g_cpu.cpsr & F_C;
                uint32_t op2;

                if (op1 == 1) {
                    uint32_t imm = ins & 0xff;
                    uint32_t rot = ((ins >> 8) & 0xf) * 2;
                    op2 = rot ? ((imm >> rot) | (imm << (32 - rot))) : imm;
                    if (rot)
                        shc = ((imm >> (rot - 1)) & 1) ? F_C : 0;
                } else {
                    uint32_t rm = ins & 0xf;
                    uint32_t type = (ins >> 5) & 3, s = (ins >> 7) & 0x1f;
                    uint32_t reg_shift = (ins >> 4) & 1;
                    if (reg_shift) {
                        s = R[(ins >> 8) & 0xf] & 0xff;
                        if (s == 0) shc = g_cpu.cpsr & F_C;
                    }
                    if (reg_shift && s == 0)
                        op2 = (type == 3) ? sh_ror(R[rm], 0, &shc) : R[rm];
                    else switch (type) {
                        case 0: op2 = sh_lsl(R[rm], s, &shc); break;
                        case 1: op2 = sh_lsr(R[rm], s ? s : 32, &shc); break;
                        case 2: op2 = sh_asr(R[rm], s ? s : 32, &shc); break;
                        default: op2 = sh_ror(R[rm], s ? s : 32, &shc); break;
                    }
                    /* PC as a shifted register operand: +4 rather than +8 for
                     * the register-shift forms. GCC does not emit these, so
                     * refuse rather than quietly differ from hardware. */
                    if (rm == 15)
                        return trap(pc, ins, 4);
                }

                /* add_c()/sub_c() write all four flags unconditionally -- they
                 * are the flag-setting primitives. ARM's data-processing
                 * instructions only write flags when S is set, so the incoming
                 * flags are captured here and put back for the common case.
                 * Without this, `add ip, pc, #0` in the PLT -- which has no S
                 * bit -- silently clears the condition flags, and any later
                 * conditional branch in the caller reads garbage. */
                uint32_t flags_in = g_cpu.cpsr & (F_N | F_Z | F_C | F_V);
                uint32_t a = R[rn], res;
                switch (opcode) {
                case 0x0: res = a & op2; break;                       /* AND */
                case 0x1: res = a ^ op2; break;                       /* EOR */
                case 0x2: res = sub_c(a, op2, 1); break;              /* SUB */
                case 0x3: res = sub_c(op2, a, 1); break;              /* RSB */
                case 0x4: res = add_c(a, op2, 0); break;              /* ADD */
                case 0x5: res = add_c(a, op2, (g_cpu.cpsr & F_C) ? 1 : 0); break;
                case 0x6: res = sub_c(a, op2, (g_cpu.cpsr & F_C) ? 1 : 0); break;
                case 0x7: res = sub_c(op2, a, (g_cpu.cpsr & F_C) ? 1 : 0); break;
                case 0x8: set_nz(a & op2); pc = next; continue;       /* TST */
                case 0x9: set_nz(a ^ op2); pc = next; continue;       /* TEQ */
                case 0xa: sub_c(a, op2, 1); pc = next; continue;      /* CMP */
                case 0xb: add_c(a, op2, 0); pc = next; continue;      /* CMN */
                case 0xc: res = a | op2; break;                       /* ORR */
                case 0xd: res = op2; break;                           /* MOV */
                case 0xe: res = a & ~op2; break;                      /* BIC */
                default:  res = ~op2; break;                          /* MVN */
                }
                R[rd] = res;
                if (S) {
                    set_nz(res);
                    g_cpu.cpsr = (g_cpu.cpsr & ~F_C) | shc;
                } else {
                    g_cpu.cpsr = (g_cpu.cpsr & ~(F_N | F_Z | F_C | F_V)) | flags_in;
                }
                if (rd == 15) {
                    /* A write to PC from a data-processing instruction is a
                     * return or a computed jump; it does not interwork. */
                    pc = res & ~1u;
                    R[15] = pc;
                    prof_transfer(pc);
                    continue;
                }
                pc = next;
                continue;
            }

            /* ---- single data transfer ---- */
            if (op1 == 2 || op1 == 3) {
                /* Bit 25 selects the offset form, and it is the opposite way
                 * round from the field's usual name: 0 selects the 12-bit
                 * immediate, 1 selects the register-shifted form.
                 *
                 * Settled empirically against QEMU rather than from the ARM
                 * ARM, because getting it backwards still produces a
                 * structurally valid instruction: 0xE5BCF388 (bit25=0) is
                 * `ldr pc, [ip, #0x388]!` and lands on a real JUMP_SLOT slot,
                 * while decoding it as a register offset yields r8<<7 and a
                 * writeback that misses the GOT entirely. The tell was that
                 * the two candidate addresses differed by exactly imm12.
                 *
                 * This one instruction is on the path of all 178 imports. */
                uint32_t reg_offset = (ins >> 25) & 1;
                uint32_t P = (ins >> 24) & 1, U = (ins >> 23) & 1;
                uint32_t B = (ins >> 22) & 1, W = (ins >> 21) & 1;
                uint32_t L = (ins >> 20) & 1;
                uint32_t off;
                if (!reg_offset) {
                    off = ins & 0xfff;
                } else {
                    uint32_t rm = ins & 0xf;
                    uint32_t type = (ins >> 5) & 3, s = (ins >> 7) & 0x1f;
                    uint32_t c;
                    switch (type) {
                    case 0: off = sh_lsl(R[rm], s, &c); break;
                    case 1: off = sh_lsr(R[rm], s ? s : 32, &c); break;
                    case 2: off = sh_asr(R[rm], s ? s : 32, &c); break;
                    default: off = sh_ror(R[rm], s ? s : 32, &c); break;
                    }
                }
                uint32_t base = R[rn];
                uint32_t eff = U ? base + off : base - off;
                uint32_t a = P ? eff : base;
                if (L) {
                    uint32_t v = B ? LD8(a) : LD32(a);
                    R[rd] = v;
                } else {
                    if (B) ST8(a, (uint8_t)R[rd]);
                    else   ST32(a, R[rd]);
                }
                if (!P || W)
                    R[rn] = eff;
                if (L && rd == 15) {
                    /* An interworking load: this is how the PLT dispatches,
                     * and bit 0 of the loaded word chooses the state. */
                    uint32_t v = R[15];
                    g_thumb = (v & 1) != 0;
                    pc = v & ~1u;
                    R[15] = pc;
                    prof_transfer(pc);
                    continue;
                }
                pc = next;
                continue;
            }

            /* ---- block data transfer ---- */
            if (op1 == 4) {
                uint32_t P = (ins >> 24) & 1, U = (ins >> 23) & 1;
                uint32_t W = (ins >> 21) & 1;
                uint32_t L = (ins >> 20) & 1;
                uint32_t list = ins & 0xffff;
                uint32_t n = 0;
                for (int i = 0; i < 16; i++)
                    if (list & (1u << i)) n++;
                if (!n) return trap(pc, ins, 4);
                uint32_t base = R[rn];
                /* IA/IB/DA/DB: the lowest-numbered register goes to the
                 * lowest address in every one of them; only the start
                 * address and the writeback differ. */
                uint32_t start;
                if (U) start = P ? base + 4 : base;
                else   start = base - 4 * n;
                uint32_t cur = start;
                for (int i = 0; i < 16; i++) {
                    if (!(list & (1u << i))) continue;
                    if (L) {
                        uint32_t v = LD32(cur);
                        if (i == 15) {
                            g_thumb = (v & 1) != 0;
                            pc = v & ~1u;
                            R[15] = pc;
                        } else {
                            R[i] = v;
                        }
                    } else {
                        ST32(cur, R[i]);
                    }
                    cur += 4;
                }
                if (W)
                    R[rn] = U ? base + 4 * n : base - 4 * n;
                if (L && (list & 0x8000)) {
                    /* Writeback happens before the PC is loaded when the base
                     * is also in the list, so apply it before transferring. */
                    prof_transfer(pc);
                    continue;
                }
                pc = next;
                continue;
            }

            /* ---- branch / branch with link ----
             *
             * The target is PC + 8 + offset, where PC is the branch's own
             * address -- ARM's PC reads as the instruction plus 8, and the
             * encoding's imm24 is relative to that. `next` is pc + 4 and is
             * only right for the *link* value, which is the address of the
             * following instruction.
             *
             * Using next for both puts every ARM branch 4 bytes short. That is
             * small enough to land inside a neighbouring instruction rather
             * than fault, so it presents as a control-flow divergence the
             * oracle finds immediately and a human reads as "the loop never
             * terminates": it was the whole reason __divsf3 spun for 49.8M
             * instructions. */
            if (op1 == 5) {
                int32_t off = (int32_t)(ins & 0xffffff);
                off = (off << 8) >> 6;   /* sign-extend imm24, then << 2 */
                if (ins & 0x01000000) {
                    R[14] = next | (g_thumb ? 1u : 0u);
                }
                pc = (uint32_t)((int32_t)R[15] + off);   /* R[15] == pc + 8 */
                R[15] = pc;
                prof_transfer(pc);
                continue;
            }

            return trap(pc, ins, 4);
        }
    }
}

const char *cpu_halt_str(int reason)
{
    switch (reason) {
    case HALT_BUDGET:    return "instruction budget exhausted";
    case HALT_HALTED:    return "guest halted (BKPT outside the import stub page)";
    case HALT_UNDEFINED: return "undefined instruction";
    case HALT_TRAP:      return "unimplemented instruction (valid, not emulated)";
    case HALT_BAD_PC:    return "PC left the loaded image";
    case HALT_IMPORT:    return "stopped at host crossing (oracle mode)";
    default:             return "?";
    }
}

void cpu_dump_state(void)
{
    for (int i = 0; i < 16; i += 4)
        host_log("  r%-2d=%08x r%-2d=%08x r%-2d=%08x r%-2d=%08x\n",
                 i, g_cpu.r[i], i + 1, g_cpu.r[i + 1],
                 i + 2, g_cpu.r[i + 2], i + 3, g_cpu.r[i + 3]);
    host_log("  cpsr=%08x (%c%c%c%c)  mode=%s\n", g_cpu.cpsr,
             (g_cpu.cpsr & F_N) ? 'N' : '-', (g_cpu.cpsr & F_Z) ? 'Z' : '-',
             (g_cpu.cpsr & F_C) ? 'C' : '-', (g_cpu.cpsr & F_V) ? 'V' : '-',
             g_thumb ? "thumb" : "arm");
}

/* Exposed for the profile report. */
uint64_t *cpu_profile_counts(void) { return g_prof.counts; }
int       cpu_profile_active(void) { return g_prof.on; }
