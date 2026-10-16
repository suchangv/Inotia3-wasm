/*
 * patches.c -- deliberate modifications to the guest binary.
 *
 * The Vita port needs 196 KB of these (source/patch.c), applied through a hook
 * mechanism it built for the purpose. Here they are simpler, because the loader
 * owns the guest's memory: a function that must return a constant is four bytes
 * of overwrite, not a trampoline.
 *
 * Every patch here is a *port decision* rather than a fix to the game, so each
 * one records what it changes and why. The rule is that a patch must be
 * explainable in terms of "this check cannot succeed in this environment, and
 * succeeding is the correct answer for an offline single-player port" -- not
 * "this was in the way".
 */

#include "patches.h"
#include "guest.h"
#include "elf.h"
#include "cpu.h"
#include "jni.h"
#include "imports.h"
#include "compat.h"

/* Overwrite a function's prologue so that it returns `value` immediately.
 *
 * Thumb:  MOVS r0, #0   BX lr        (0x2000, 0x4770)
 * ARM:    MOV  r0, #0   BX lr        (0xE3A00000, 0xE12FFF1E)
 *
 * Only sound for a function with no side effects the caller depends on and no
 * arguments to preserve. Every use below satisfies that, and each says so.
 */
static void patch_return_constant(uint32_t addr, uint32_t value)
{
    int thumb = (addr & 1) != 0;
    uint32_t at = addr & ~1u;

    if (thumb) {
        ST16(at, 0x2000);          /* MOVS r0, #0 */
        if (value != 0)
            host_log("patches: %08x returns %u, but only 0 is encoded here\n",
                     addr, value);
        ST16(at + 2, 0x4770);      /* BX lr */
    } else {
        ST32(at, 0xE3A00000);      /* MOV r0, #0 */
        ST32(at + 4, 0xE12FFF1E);  /* BX lr */
    }
    host_log("patches: %08x (%s) -> return %u\n", at,
             thumb ? "Thumb" : "ARM", value);
}

/* Look up a symbol and patch it. Returns 1 if it was found and patched. */
static int patch_symbol_return(const guest_image *img, const char *name,
                               uint32_t value)
{
    uint32_t addr = elf_find_symbol(img, name);
    if (!addr) {
        host_log("patches: %s not found -- not patched\n", name);
        return 0;
    }
    patch_return_constant(addr, value);
    return 1;
}

/* --- lazily-populated JavaVM caches ---------------------------------------
 *
 * Several Com2uS subsystems (Hub, ActiveUser, ads) keep their own copy of the
 * JavaVM pointer in .bss, each meant to be filled in lazily by that
 * subsystem's own init function calling
 *
 *     GetJavaVM(env, &its_own_copy)      -- env->functions[215]
 *
 * On Android those init functions are called by the Java layer. Nothing here
 * calls them, so every copy stays NULL forever, and any CS_* function that
 * reads its copy crashes the moment it is reached:
 *
 *     ldr r0, [r5]        ; r0 = *copy      <- 0
 *     ldr r3, [r0]        ; dereference NULL
 *
 * which is what killed CS_UserAgreeSetCallback here -- reached from startClet,
 * which is itself only reached once the anti-tamper patch above lets the boot
 * proceed.
 *
 * The parent repository hit this too and documents it (source/patch.c, Corrida
 * 38: "we must manually initialize every copy ... otherwise any CS_ or ad-related
 * function reading its own copy crashes the same way"). It hardcodes seven
 * addresses because its hook mechanism needs them; that list is tied to one APK
 * build and would silently rot against another. Since the loader here owns the
 * guest image, the copies can be *found* instead of listed:
 *
 * scan .text for the idiom
 *
 *     ldr  rB, [pc, #lit]     ; PIC offset
 *     add  rB, pc             ; rB = &some_global
 *     ldr  rX, [rB, #0]       ; rX = *some_global
 *     ldr  rY, [rX, #0]       ; dereference it
 *
 * and every base it yields is a cached JavaVM pointer. That is
 * version-independent, which the hardcoded list is not.
 */
static int patch_vm_caches(const guest_image *img)
{
    uint32_t lo = img->text_lo, hi = img->text_hi;
    int found = 0, patched = 0;

    for (uint32_t pc = lo; pc + 8 < hi; pc += 2) {
        uint32_t h = LD16(pc);
        if ((h & 0xF800) != 0x4800)          /* ldr rB, [pc, #imm8*4] */
            continue;
        uint32_t b = (h >> 8) & 7;
        uint32_t lit = ((pc + 4) & ~3u) + ((h & 0xff) << 2);

        /* The matching `add rB, pc` follows within a few instructions. */
        for (uint32_t k = 2; k <= 8; k += 2) {
            uint32_t h2 = LD16(pc + k);
            if ((h2 & 0xFF00) != 0x4400 || ((h2 >> 8) & 3) != 0)
                continue;
            uint32_t rd = (h2 & 7) | ((h2 >> 4) & 8);
            uint32_t rm = (h2 >> 3) & 0xf;
            if (rd != b || rm != 15)
                continue;

            uint32_t base = LD32(lit) + pc + k + 4;
            /* Then it must be read and immediately dereferenced. */
            uint32_t h3 = LD16(pc + k + 2);
            if ((h3 & 0xF800) != 0x6800 || ((h3 >> 3) & 7) != b)
                continue;
            uint32_t x = h3 & 7;
            uint32_t h4 = LD16(pc + k + 4);
            if ((h4 & 0xF800) != 0x6800 || ((h4 >> 3) & 7) != x)
                continue;

            if (base < img->base || base >= img->base + 0x01000000u)
                continue;
            found++;
            /* Write the JavaVM where the missing init function would have. */
            if (LD32(base) == 0) {
                ST32(base, g_javavm_ptr);
                patched++;
            }
            break;
        }
    }
    host_log("patches: %d cached JavaVM slot(s) found, %d filled in\n",
             found, patched);
    return patched;
}

int patches_replace_with_import(const guest_image *img, const char *symbol,
                                const char *import_name)
{
    uint32_t addr = elf_find_symbol(img, symbol);
    if (!addr) {
        host_log("patches: %s not found -- not replaced\n", symbol);
        return 0;
    }
    if (!(addr & 1)) {
        host_log("patches: %s is ARM code; the jump below is Thumb\n", symbol);
        return 0;
    }
    int idx = import_register(import_name, 0);
    imports_build_stubs();   /* the stub has to exist before the jump names it */
    uint32_t stub = g_imports[idx].stub | 1u;
    uint32_t at = addr & ~1u;
    ST16(at + 0, 0x4B00);                       /* ldr r3, [pc, #0] */
    ST16(at + 2, 0x4718);                       /* bx  r3           */
    ST32(at + 4, stub);
    host_log("patches: %s (%08x) -> %s stub %08x\n", symbol, at,
             import_name, stub);
    return 1;
}

void patches_apply(const guest_image *img)
{
    /* --- anti-tamper ---------------------------------------------------
     *
     * nativeInit() bootstraps the game only when BOTH of these return 0:
     *
     *     startClet()  ->  registers g_MainTimer  ->  the whole
     *                      timer-driven game loop
     *
     * Both detect memory-hack and cracking tools by running
     * `system("ps > <file>")` and scanning the captured process list. There is
     * no shell here, so the file is never created, fopen() returns NULL, and
     * both fall through to their "tampered" return value of 1 -- which
     * *silently skips startClet()*. No main timer, no frame work, nothing
     * renders. It presents as a clean boot with an idle engine, which is
     * exactly what this spike was reporting before these two patches.
     *
     * Returning 0 is the correct answer for an offline single-player port, and
     * there is no other way to satisfy the check: the JNI-side knob
     * jGetMemoryHackCheckType() is hardcoded to 7 inside the .so, and
     * excuteDirectly() also peeks at bionic FILE internals
     * (__stream->_IO_read_base) whose layout does not match ours, so faking the
     * file is not reliable either. The Vita port reached the same conclusion --
     * see source/patch.c "Anti-tamper bypass" in the parent repository, whose
     * comment documents this exact failure ("black screen") and these exact two
     * functions.
     *
     * Both are pure predicates: they take no arguments the caller needs back
     * and have no side effects anything depends on, so overwriting the
     * prologue is sound. */
    patch_symbol_return(img, "excuteDirectly", 0);
    patch_symbol_return(img, "jIsCracked", 0);

    /* --- File::GetFullPathName -------------------------------------------
     *
     * The function every resource path goes through, replaced with a host
     * implementation. This is the same decision the parent repository made
     * (source/patch.c's File_GetFullPathName_hook, Corrida 34/37) and for the
     * same reason: the real one builds an "@offset|length|path" string for
     * assets, and that "@" form is the *only* place File::Open's offset and
     * length fields are written. Get it wrong and those fields keep whatever
     * was in that stack slot from an unrelated earlier call.
     *
     * Doing it here rather than trusting the original means the pass-through
     * case is also formatted the same way, which is what the parent repository
     * found it had to fix: on Android every resource resolves through the apk
     * branch, so the bug of a bare path never taking the "@" branch is
     * unreachable there and only shows up in a port. */
    patches_replace_with_import(img, "_ZN4File15GetFullPathNameEPKcPcmi",
                                "host/File::GetFullPathName");
    patches_replace_with_import(img, "_ZN4File15GetFullPathNameEPKcPcm",
                                "host/File::GetFullPathName");
}

/* Separate from patches_apply() because it depends on jni_guest_init(), which
 * is what decides the JavaVM's guest address. */
void patches_fill_vm_caches(const guest_image *img)
{
    patch_vm_caches(img);
}
