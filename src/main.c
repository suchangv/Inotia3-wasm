/*
 * main.c -- the spike runner.
 *
 * Purpose is measurement, not gameplay. It loads libgame.so, runs the image's
 * static constructors and (optionally) JNI_OnLoad under the interpreter, and
 * reports the numbers the AOT-vs-interpreter decision rests on:
 *
 *   1. instructions retired per unit of guest work, and the MIPS the
 *      interpreter actually achieves -- native and (later) under wasm;
 *   2. which functions those instructions are actually in;
 *   3. how often the guest crosses into the host, and through which imports.
 *
 * The point of (3) is that a host crossing costs roughly 50-200 ns in a
 * browser. GL calls are a few dozen per frame and do not matter; if the
 * engine's logic frame turns out to call into the host thousands of times, the
 * browser port's budget changes shape entirely.
 *
 * Usage:
 *   spike <libgame.so> [--budget=N] [--init-array] [--jni] [--profile] [--quiet]
 */

#include "guest.h"
#include "elf.h"
#include "cpu.h"
#include "imports.h"
#include "compat.h"
#include "platform.h"
#include "jni.h"

#ifndef WASM_FREESTANDING
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#endif

/* Returning from an entry function lands the PC here. It must not be 0: a
 * guest that reads a null function pointer and branches to it also arrives at
 * PC 0, and with LR=1 the two are indistinguishable -- a real failure gets
 * reported as a clean return. This value is outside the image (so HALT_BAD_PC
 * fires) and recognisable on sight. The Thumb bit is set, as a `bx lr` return
 * requires. */
#define RETURN_SENTINEL 0x0BADF00Du

static guest_image g_img;

static uint32_t find_symbol(const char *name)
{
    for (int i = 0; i < g_img.nsyms; i++) {
        if (g_img.syms[i].shndx == 0)
            continue;
        const char *n = elf_sym_name(&g_img, &g_img.syms[i]);
        if (n && strcmp(n, name) == 0) {
            /* st_value is an image-relative offset; hand back a guest address,
             * and keep the Thumb bit, which is what tells cpu_run which
             * instruction set to decode. */
            uint32_t v = g_img.syms[i].value;
            return (v & ~1u) + GUEST_IMAGE_BASE + (v & 1u);
        }
    }
    return 0;
}

/* ---- profile report ------------------------------------------------------ */

static void report_profile(int top_n)
{
    uint64_t *counts = cpu_profile_counts();
    if (!counts) {
        host_log("profile: not initialised\n");
        return;
    }
    uint64_t total = 0;
    for (int i = 0; i < g_img.nsorted; i++)
        total += counts[i];
    if (!total) {
        host_log("profile: no instructions attributed\n");
        return;
    }

    /* Partial selection: repeatedly take the largest remaining. n is the
     * symbol count (thousands), top_n is tens, so this is cheap and avoids
     * allocating an index array for a report. */
    int *taken = (int *)host_alloc(sizeof(int) * (size_t)g_img.nsorted);

    host_log("\n--- hottest functions (%d of %d, by instructions retired) ---\n",
             top_n, g_img.nsorted);
    host_log("  %-12s %14s %7s  %s\n", "instrs", "share", "size", "function");
    for (int t = 0; t < top_n; t++) {
        int best = -1;
        for (int i = 0; i < g_img.nsorted; i++) {
            if (taken[i] || !counts[i])
                continue;
            if (best < 0 || counts[i] > counts[best])
                best = i;
        }
        if (best < 0)
            break;
        taken[best] = 1;
        const sym_entry *e = &g_img.sorted[best];
        const char *nm = elf_sym_name(&g_img, &g_img.syms[e->sym_idx]);
        host_log("  %-12llu %6.2f%% %7u  %s\n",
                 (unsigned long long)counts[best],
                 100.0 * (double)counts[best] / (double)total,
                 e->size, nm && *nm ? nm : "(unnamed)");
    }
}

/* ---- running one guest entry point --------------------------------------- */

typedef struct {
    const char *what;
    uint32_t    entry;
    int         reason;
    uint64_t    insns;
    double      seconds;
} run_result;

/* `arg0` is placed in r0 before the entry runs -- JNI_OnLoad expects the
 * JavaVM* there, and the native* entry points expect a JNIEnv*. */
static run_result run_entry_arg(const char *what, uint32_t entry, uint32_t arg0,
                                uint64_t budget)
{
    run_result r;
    r.what = what;
    r.entry = entry;
    r.insns = 0;
    r.seconds = 0;
    r.reason = HALT_BUDGET;
    if (!entry) {
        host_log("%-22s : symbol not found\n", what);
        r.reason = HALT_BAD_PC;
        return r;
    }

    /* Fresh stack per entry, and LR pointing at the sentinel so a normal
     * return is distinguishable from a crash. */
    g_cpu.r[13] = GUEST_STACK_TOP - 0x1000;
    g_cpu.r[14] = RETURN_SENTINEL;
    for (int i = 0; i < 13; i++)
        g_cpu.r[i] = 0;
    g_cpu.r[0] = arg0;
    g_cpu.cpsr = 0x10;   /* user mode, Thumb via the entry's bit0 */

    uint64_t before = g_insn_count;
    double t0 = host_now();
    r.reason = cpu_run(entry, g_cpu.r[13], g_insn_count + budget);
    cpu_trace_final();
    r.seconds = host_now() - t0;
    r.insns = g_insn_count - before;
    return r;
}

static void report_run(const run_result *r)
{
    const char *status;
    if (r->reason == HALT_BAD_PC && g_cpu.r[15] == (RETURN_SENTINEL & ~1u))
        status = "returned to caller";
    else
        status = cpu_halt_str(r->reason);

    host_log("%-22s : entry=%08x  %10llu instrs  %8.3f MIPS  -> %s\n",
             r->what, r->entry, (unsigned long long)r->insns,
             r->seconds > 0 ? (double)r->insns / r->seconds / 1e6 : 0.0, status);

    if (r->reason != HALT_BUDGET &&
        !(r->reason == HALT_BAD_PC && g_cpu.r[15] == (RETURN_SENTINEL & ~1u))) {
        const char *nm = elf_func_name(&g_img, g_fault_pc);
        host_log("    fault at pc=%08x%s%s%s  insn=%08x (width %d)\n",
                 g_fault_pc, nm ? " in " : "", nm ? nm : "",
                 nm ? "" : "", g_fault_insn, g_fault_width);
        if (g_fault_width == 4)
            host_log("    -> this is ARM mode. See the ARM note in cpu.c: the "
                     "decoder is intentionally absent until the worklist from "
                     "a real run says which forms matter.\n");
        /* For HALT_BAD_PC there is no trap() record, so the interesting
         * information is the last few instructions and where the PC went. */
        if (r->reason == HALT_BAD_PC) {
            uint32_t at = 0;
            const char *near = elf_nearest_sym(&g_img, g_cpu.r[15], &at);
            host_log("    pc ran to %08x (%s+0x%x), which is outside "
                     "text %08x-%08x\n",
                     g_cpu.r[15], near ? near : "?", near ? g_cpu.r[15] - at : 0,
                     g_img.text_lo, g_img.text_hi);
            cpu_dump_trace();
        }
    }
}

/* ---- guest memory dump ---------------------------------------------------
 *
 * For the differential test: the oracle maps exactly the memory this process
 * relocated, so a divergence between the two traces cannot be a loader
 * difference. Only regions that can hold non-zero data are written; Unicorn
 * hands out zero-filled pages for everything else, which is what the guest
 * space holds there anyway.
 */
static const struct { uint32_t addr, size; } k_dump_regions[] = {
    { GUEST_IMAGE_BASE, 0x00E00000u },  /* relocated .so, .data, .bss */
    { GUEST_STUB_BASE,  0x00500000u },  /* import stubs + host data   */
};

static void dump_guest(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f)
        host_fatal("cannot open %s for the guest memory dump\n", path);

    uint32_t n = (uint32_t)(sizeof(k_dump_regions) / sizeof(k_dump_regions[0]));
    fwrite("GIMG", 1, 4, f);
    fwrite(&n, 4, 1, f);
    for (uint32_t i = 0; i < n; i++) {
        fwrite(&k_dump_regions[i].addr, 4, 1, f);
        fwrite(&k_dump_regions[i].size, 4, 1, f);
    }
    for (uint32_t i = 0; i < n; i++)
        fwrite(g_mem + k_dump_regions[i].addr, 1, k_dump_regions[i].size, f);

    fclose(f);
    host_log("guest memory dumped to %s (%u regions)\n", path, n);
}

/* ---- entry --------------------------------------------------------------- */

int main(int argc, char **argv)
{
    const char *path = NULL;
    const char *dump_path = NULL;
    const char *trace_path = NULL;
    uint32_t only_entry = 0;
    uint64_t budget = 200u * 1000u * 1000u;
    int do_init_array = 0, do_jni = 0, do_profile = 1, trace = 0, do_boot = 0;
    (void)0;

    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "--budget=", 9) == 0)
            budget = (uint64_t)strtoull(argv[i] + 9, NULL, 10);
        else if (strcmp(argv[i], "--init-array") == 0)
            do_init_array = 1;
        else if (strcmp(argv[i], "--jni") == 0)
            do_jni = 1;
        else if (strcmp(argv[i], "--all") == 0)
            do_init_array = do_jni = 1;
        else if (strcmp(argv[i], "--boot") == 0)
            do_boot = 1;
        else if (strcmp(argv[i], "--no-profile") == 0)
            do_profile = 0;
        else if (strcmp(argv[i], "--trace") == 0)
            trace = 1;
        else if (strcmp(argv[i], "--jni-verbose") == 0)
            g_jni_verbose = 1;
        else if (strcmp(argv[i], "--oracle") == 0)
            g_oracle_mode = 1;
        else if (strncmp(argv[i], "--dump-guest=", 13) == 0)
            dump_path = argv[i] + 13;
        else if (strncmp(argv[i], "--trace-insns=", 14) == 0)
            trace_path = argv[i] + 14;
        else if (strncmp(argv[i], "--entry=", 8) == 0)
            only_entry = (uint32_t)strtoul(argv[i] + 8, NULL, 16);
        else if (argv[i][0] != '-')
            path = argv[i];
    }
    if (!path) {
        host_log("usage: spike <libgame.so> [--all] [--budget=N] [--no-profile]\n");
        return 2;
    }

    g_mem = (uint8_t *)host_alloc(GUEST_MEM_SIZE);
    host_log("guest address space: %u MB at %p\n", GUEST_MEM_SIZE >> 20, (void *)g_mem);

    /* Must come after g_mem is known and before the first guest instruction:
     * an unchecked guest access faults as a host SIGSEGV, and the handler's
     * whole job is to translate that back into a guest address. */
    host_install_crash_handler();

    /* ---- read the .so ---- */
    FILE *f = fopen(path, "rb");
    if (!f) {
        host_log("cannot open %s\n", path);
        return 2;
    }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = (uint8_t *)host_alloc((size_t)len);
    if (fread(buf, 1, (size_t)len, f) != (size_t)len) {
        host_log("short read on %s\n", path);
        fclose(f);
        return 2;
    }
    fclose(f);
    host_log("loaded %s (%ld bytes)\n", path, len);

    /* ---- map and relocate it ---- */
    if (elf_load(buf, (size_t)len, &g_img) != 0)
        return 1;
    /* Must follow elf_load: it registers the JNI table entries as imports and
     * re-stamps the stub page. */
    jni_guest_init();

    cpu_init(&g_img);
    cpu_set_profile(do_profile);
    cpu_enable_trace(trace);

    if (dump_path)
        dump_guest(dump_path);
    if (trace_path) {
        host_trace_open(trace_path);
        g_trace_insn_on = 1;
    }

    /* ---- run it ---- */
    host_log("\n");
    if (do_boot) {
        /* src/utils/init.c in the Vita port, in order: static constructors,
         * then JNI_OnLoad, then nativePreInit. Getting these three to run in
         * sequence is what puts the guest on the path to its first frame. */
        host_log("=== boot sequence ===\n");
        for (uint32_t i = 0; i < g_img.init_array_n; i++) {
            uint32_t fn = LD32(g_img.init_array + i * 4) | 1u;
            char label[32];
            label[0]='c'; label[1]='t'; label[2]='o'; label[3]='r';
            label[4]='['; label[5]=(char)('0'+(i%10)); label[6]=']'; label[7]=0;
            run_result rr = run_entry_arg(label, fn, 0, budget);
            report_run(&rr);
            if (rr.reason == HALT_TRAP)
                break;
        }

        uint32_t jni_entry = find_symbol("JNI_OnLoad");
        run_result rj = run_entry_arg("JNI_OnLoad", jni_entry, g_javavm_ptr, budget);
        report_run(&rj);
        host_log("    JNI_OnLoad returned %d (JNI_VERSION_1_6 is %d)\n",
                 (int)g_cpu.r[0], JNI_VERSION_1_6);

        uint32_t pre = find_symbol(
            "Java_com_com2us_wrapper_WrapperRenderer_nativePreInit");
        if (!pre) {
            host_log("nativePreInit symbol not found\n");
        } else {
            /* nativePreInit(JNIEnv *env, jclass clazz, jintArray parameter,
             *               jint screenW, jint screenH) -- five arguments, so
             *               the last one is on the guest stack, which
             *               run_entry_arg cannot express. Set the register file
             *               directly and push the fifth word. */
            uint32_t array = jni_host_new_int_array(3);
            g_cpu.r[13] = GUEST_STACK_TOP - 0x1000;
            ST32(g_cpu.r[13], 544);          /* screenH, the fifth argument */
            g_cpu.r[14] = 1;
            for (int i = 0; i < 13; i++) g_cpu.r[i] = 0;
            g_cpu.r[0] = g_jnienv_ptr;
            g_cpu.r[1] = 0;                  /* clazz: the guest ignores it */
            g_cpu.r[2] = array;
            g_cpu.r[3] = 960;
            g_cpu.cpsr = 0x10;
            uint64_t before = g_insn_count;
            double t0 = host_now();
            run_result rp;
            rp.what = "nativePreInit";
            rp.entry = pre;
            rp.reason = cpu_run(pre, g_cpu.r[13], g_insn_count + budget);
            cpu_trace_final();
            rp.insns = g_insn_count - before;
            rp.seconds = host_now() - t0;
            report_run(&rp);
            /* getEnv() and JNI_OnLoad() share this globals block: JNI_OnLoad
             * stores the JavaVM* at +0 and getEnv() reads it back before
             * calling vm->GetEnv, which writes the JNIEnv* at +4. Printing it
             * says directly whether the two agree. */
            host_log("    module globals at 0115a36c: vm=%08x env=%08x\n",
                     LD32(0x0115a36c), LD32(0x0115a370));
        }
    } else if (only_entry) {
        /* One entry point only, so a differential run covers a single guest
         * function from entry to return rather than concatenating five of them
         * into one trace file. */
        run_result rr = run_entry_arg("entry", only_entry, 0, budget);
        report_run(&rr);
    } else if (do_init_array) {
        if (!g_img.init_array) {
            host_log("no DT_INIT_ARRAY in the image\n");
        } else {
            host_log("running %u static constructors\n", g_img.init_array_n);
            for (uint32_t i = 0; i < g_img.init_array_n; i++) {
                /* The constructors are C++ global ctors emitted by GCC for
                 * ARMv5TE, so they are Thumb functions and arrive without the
                 * Thumb bit set in the table -- set it. */
                uint32_t fn = LD32(g_img.init_array + i * 4) | 1u;
                char label[32];
                /* Labels are built by hand: snprintf into a host buffer is
                 * fine here, this is not guest code. */
                label[0] = 'c'; label[1] = 't'; label[2] = 'o'; label[3] = 'r';
                label[4] = '['; label[5] = (char)('0' + (i % 10)); label[6] = ']';
                label[7] = 0;
                run_result rr = run_entry_arg(label, fn, 0, budget);
                report_run(&rr);
                if (rr.reason == HALT_TRAP)
                    break;
            }
        }
    }

    if (do_jni) {
        /* JNI_OnLoad(vm). The guest reaches JNI through the JavaVM/JavaEnv
         * function tables rather than through ELF imports, so it needs
         * FalsoJNI behind those tables -- the next step. Calling it with a
         * null VM is still informative: it shows how far the module gets
         * before it first needs the JNI layer. */
        uint32_t jni = find_symbol("JNI_OnLoad");
        run_result rr = run_entry_arg("JNI_OnLoad", jni, g_javavm_ptr, budget);
        report_run(&rr);
    }

    if (trace_path) {
        g_trace_insn_on = 0;
        host_trace_close();
        host_log("\ninstruction trace written to %s\n", trace_path);
    }

    /* ---- report ---- */
    if (do_profile)
        report_profile(20);
    imports_report(20);
    jni_report(15);

    uint32_t at = 0;
    const char *near = elf_nearest_sym(&g_img, g_cpu.r[15], &at);
    host_log("\nfinal pc=%08x (%s+0x%x)\n", g_cpu.r[15],
             near ? near : "?", near ? g_cpu.r[15] - at : 0);
    return 0;
}
