/*
 * host_hooks.c -- host implementations of the guest's libgcc helpers.
 *
 * See host_hooks.h for why. The calling convention is ARM EABI *soft-float*
 * (armeabi, ARMv5TE): integer arguments in r0-r3, a double as a register pair
 * (low word first), a single-precision float in the low 32 bits of one register.
 * Results follow the same rules.
 *
 * Divide by zero: libgcc calls __div0 and raises SIGFPE; the game never
 * divides by zero (it would have crashed on Android), so the hooks return 0 and
 * log it once rather than letting the host die on SIGFPE.
 */
#include <math.h>
#include <string.h>

#include "host_hooks.h"
#include "cpu.h"
#include "guest.h"
#include "platform.h"

#define MAX_HOOKS 32

enum kind {
    K_DIVSI, K_UDIVSI, K_IDIVMOD, K_UIDIVMOD, K_MODSI, K_UMODSI,
    K_DIVDF, K_ADDDF, K_SUBDF, K_MULDF, K_DIVSF,
    K_FIXDFSI, K_FLOATSIDF, K_EXTENDSFDF, K_TRUNCDFSF
};

static struct {
    const char *name;
    int kind;
} want[] = {
    { "__divsi3",        K_DIVSI },
    { "__aeabi_idiv",    K_DIVSI },
    { "__udivsi3",       K_UDIVSI },
    { "__aeabi_uidiv",   K_UDIVSI },
    { "__aeabi_idivmod", K_IDIVMOD },
    { "__aeabi_uidivmod",K_UIDIVMOD },
    { "__modsi3",        K_MODSI },
    { "__umodsi3",       K_UMODSI },
    { "__divdf3",        K_DIVDF },
    { "__adddf3",        K_ADDDF },
    { "__subdf3",        K_SUBDF },
    { "__muldf3",        K_MULDF },
    { "__divsf3",        K_DIVSF },
    { "__fixdfsi",       K_FIXDFSI },
    { "__floatsidf",     K_FLOATSIDF },
    { "__extendsfdf2",   K_EXTENDSFDF },
    { "__truncdfsf2",    K_TRUNCDFSF },
};

static const char *want_names[MAX_HOOKS];
static int slot_kind[MAX_HOOKS];
static unsigned long slot_calls[MAX_HOOKS];
static unsigned long total_calls;
static uint32_t patched_addr[MAX_HOOKS];
static int nslots;

/* ---- soft-float register plumbing ---------------------------------------- */

static double dget(const uint32_t *R, int i)
{
    uint64_t v = (uint64_t)R[i] | ((uint64_t)R[i + 1] << 32);
    double d;
    memcpy(&d, &v, sizeof(d));
    return d;
}

static void dput(uint32_t *R, int i, double d)
{
    uint64_t v;
    memcpy(&v, &d, sizeof(v));
    R[i] = (uint32_t)v;
    R[i + 1] = (uint32_t)(v >> 32);
}

static float fget(uint32_t w)
{
    float f;
    memcpy(&f, &w, sizeof(f));
    return f;
}

static uint32_t fput(float f)
{
    uint32_t w;
    memcpy(&w, &f, sizeof(w));
    return w;
}

/* ---- the hooks ----------------------------------------------------------- */

static void divsi(uint32_t *R, int want_rem)
{
    int32_t a = (int32_t)R[0], b = (int32_t)R[1];
    if (b == 0) {
        static int once;
        if (!once) { once = 1; host_log("hooks: guest divide by zero\n"); }
        R[0] = 0;
        if (want_rem) R[1] = 0;
        return;
    }
    /* INT_MIN / -1 overflows; libgcc returns INT_MIN rather than trapping. */
    int32_t q = (a == INT32_MIN && b == -1) ? INT32_MIN : a / b;
    R[0] = (uint32_t)q;
    if (want_rem)
        R[1] = (uint32_t)((a == INT32_MIN && b == -1) ? 0 : a % b);
}

static void udivsi(uint32_t *R, int want_rem)
{
    uint32_t a = R[0], b = R[1];
    if (b == 0) {
        static int once;
        if (!once) { once = 1; host_log("hooks: guest divide by zero\n"); }
        R[0] = 0;
        if (want_rem) R[1] = 0;
        return;
    }
    R[0] = a / b;
    if (want_rem)
        R[1] = a % b;
}

int host_hook_dispatch(int idx)
{
    uint32_t *R = g_cpu.r;

    if (idx < 0 || idx >= nslots)
        return 0;
    slot_calls[idx]++;
    total_calls++;
    switch (slot_kind[idx]) {
    case K_DIVSI:     divsi(R, 0); break;
    case K_UDIVSI:    udivsi(R, 0); break;
    case K_IDIVMOD:   divsi(R, 1); break;
    case K_UIDIVMOD:  udivsi(R, 1); break;
    case K_MODSI: {
        int32_t a = (int32_t)R[0], b = (int32_t)R[1];
        R[0] = (b == 0) ? 0 : (uint32_t)((a == INT32_MIN && b == -1) ? 0 : a % b);
        break;
    }
    case K_UMODSI:
        R[0] = (R[1] == 0) ? 0 : (R[0] % R[1]);
        break;
    case K_DIVDF:     dput(R, 0, dget(R, 0) / dget(R, 2)); break;
    case K_ADDDF:     dput(R, 0, dget(R, 0) + dget(R, 2)); break;
    case K_SUBDF:     dput(R, 0, dget(R, 0) - dget(R, 2)); break;
    case K_MULDF:     dput(R, 0, dget(R, 0) * dget(R, 2)); break;
    case K_DIVSF:     R[0] = fput(fget(R[0]) / fget(R[1])); break;
    case K_FIXDFSI:   R[0] = (uint32_t)(int32_t)dget(R, 0); break;
    case K_FLOATSIDF: dput(R, 0, (double)(int32_t)R[0]); break;
    case K_EXTENDSFDF: dput(R, 0, (double)fget(R[0])); break;
    case K_TRUNCDFSF: R[0] = fput((float)dget(R, 0)); break;
    default:
        return 0;
    }
    return 1;
}

/* ---- installation -------------------------------------------------------- */

void host_hooks_report(void)
{
    if (!total_calls)
        return;
    host_log("hooks: %lu calls total\n", total_calls);
    for (int i = 0; i < nslots; i++)
        if (slot_calls[i])
            host_log("hooks:   %-18s %lu\n", want_names[i] ? want_names[i] : "?",
                     slot_calls[i]);
}

int host_hooks_install(const guest_image *img)
{
    int installed = 0;
    if (!img || !g_mem)
        return 0;

    for (unsigned i = 0; i < sizeof(want) / sizeof(want[0]); i++) {
        uint32_t addr = elf_find_symbol(img, want[i].name);
        if (!addr)
            continue;
        int thumb = (addr & 1u) != 0;
        addr &= ~1u;
        if (addr < img->text_lo || addr + 2 > img->text_hi)
            continue;
        /* Two names often share one implementation (__aeabi_idiv is an alias
         * of __divsi3); patching the same address twice would just clobber the
         * first BKPT. */
        int slot = -1;
        for (int k = 0; k < nslots; k++)
            if (patched_addr[k] == addr)
                slot = k;
        if (slot < 0) {
            if (nslots >= MAX_HOOKS)
                break;
            slot = nslots;
            slot_kind[nslots] = want[i].kind;
            want_names[nslots] = want[i].name;
            patched_addr[nslots] = addr;
            nslots++;
        }
        slot_kind[slot] = want[i].kind;
        /* libgcc's helpers are ARM code in this build, so both encodings are
         * needed: Thumb BKPT #imm8 (2 bytes) and ARM BKPT #imm12 (4 bytes).
         * Either way the interpreter reads the index out of the instruction,
         * because the address is not in the stub page. */
        if (thumb) {
            uint16_t bkpt = (uint16_t)(0xBE00 | (slot & 0xFF));
            g_mem[addr] = (uint8_t)(bkpt & 0xFF);
            g_mem[addr + 1] = (uint8_t)(bkpt >> 8);
        } else {
            /* ARM BKPT: cond=1110 0001 0010 imm12 0111 imm4 -- the index goes
             * in imm12 (bits 8..19), not next to the 0x7 marker. */
            uint32_t bkpt = 0xE1200070u | ((uint32_t)slot << 8);
            g_mem[addr] = (uint8_t)(bkpt & 0xFF);
            g_mem[addr + 1] = (uint8_t)((bkpt >> 8) & 0xFF);
            g_mem[addr + 2] = (uint8_t)((bkpt >> 16) & 0xFF);
            g_mem[addr + 3] = (uint8_t)((bkpt >> 24) & 0xFF);
        }
        host_log("hooks: %s at %08x (%s, slot %d)\n", want[i].name, addr,
                 thumb ? "thumb" : "arm", slot);
        installed++;
    }
    host_log("hooks: %d guest function(s) answered by the host\n", installed);
    return installed;
}
