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
#include "gl.h"
#include "patches.h"
#include "timers.h"
#include "input.h"

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

/* The surface the guest is told it has. WrapperJinterface.GetRealScreenWidth/
 * Height feed these into nativePreInit(), and the engine lays its HUD and menus
 * out against them -- so the same pair has to reach gl4es, or every glOrthox it
 * sets is relative to a different size than the one it thinks it has.
 *
 * 960x544 is the Vita panel. It is also close to the 384x256 surface the engine
 * renders internally (see the texture-upload sizes), so the scale factor stays
 * simple. */
#define SCREEN_W 960
#define SCREEN_H 544

/* WrapperJinterface.setTextViewHeight(1024, 1024, false) is called during the
 * Java onSurfaceCreated() right before nativeInit(), and the .so's pointer
 * arithmetic against the pixel buffer depends on this exact size (see the Vita
 * port's source/main.c). It is 4 MB of guest heap, which the 64 MB arena holds
 * comfortably. */
#define TEXTVIEW_WIDTH  1024
#define TEXTVIEW_HEIGHT 1024

#ifdef WASM_FREESTANDING
/* The wasm build has no filesystem: the driver allocates space in linear
 * memory, copies libgame.so into it, and hands the address here before calling
 * main(). Keeping the file-reading in main() behind this branch means the rest
 * of the runner -- and every option parser -- is identical on both platforms,
 * so a measurement taken under wasm is measuring the same code. */
static const uint8_t *g_wasm_so;
static size_t         g_wasm_so_len;

void spike_set_so(const uint8_t *p, size_t n)
{
    g_wasm_so = p;
    g_wasm_so_len = n;
}
#endif

static guest_image g_img;

/* ---- state that outlives a single tick ----------------------------------
 *
 * The browser drives the engine from requestAnimationFrame, so the run is no
 * longer one synchronous loop and the things that loop carried have to live
 * somewhere. Everything here is set once by spike_load()/spike_boot() and read
 * by spike_tick().
 */
static uint32_t g_native_render;   /* Java_...WrapperRenderer_nativeRender   */
static uint32_t g_native_event;    /* Java_...WrapperRenderer_nativeEvent    */
static int64_t  g_clock;           /* the guest's clock, ms -- sysTime[]      */
static uint64_t g_budget;          /* per-cpu_run instruction budget          */
static int      g_frames_max;      /* 0 = run until something stops it        */
static int      g_frames_done;
static uint64_t g_total_insns;
static int      g_ok_frames;
static int      g_do_profile;
static int      g_trace_on;
static int      g_use_fixed_step = 1;  /* 16 ms per tick, not real elapsed      */
static double   g_last_now_ms;
static int      g_entered_menu;
static int      g_menu_frame;

static int spike_load(const char *path);
static int spike_boot(void);
static int spike_tick(void);
static int spike_tick_browser(void);
static void spike_finish(void);
static uint32_t spike_call1(uint32_t fn, uint32_t a0);


/* Guest address of the jlong sysTime[1] handed to nativeInit(). The guest reads
 * its clock from here (CS_knlCurrentTime), and the timer firetimes arriving
 * from SetTimer are absolute in that same clock -- so the frame loop advances
 * this array rather than keeping a host-side notion of "now". */
uint32_t g_sys_time_addr;

/* Browser debugging: log this many GL entry points by name, then stop. 0 is
 * off. A wasm trap carries no guest PC and no stack, so the last name printed
 * is the only record of which call was running when it happened. */
int g_gl_trace;

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

/* Defined below; the snapshot path needs them here. */
static void dump_guest(const char *path);
static const char *g_snapshot_dump_path;
static const char *g_snapshot_trace_path;
static int g_stop_at_resume;

/* Take the snapshot if cpu_run stopped at the requested address, then carry on
 * from the same instruction with the trace running. Shared by every cpu_run
 * call site -- nativeInit is driven directly rather than through
 * run_entry_arg, and a snapshot that only worked on some of them would be a
 * tool that quietly does nothing.
 *
 * Returns the reason to report. */
static int spike_resume_if_stopped(int reason, uint64_t budget);

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

    r.reason = spike_resume_if_stopped(r.reason, g_budget);

    r.seconds = host_now() - t0;
    r.insns = g_insn_count - before;
    return r;
}

/* A guest entry point taking (JNIEnv*, jclass) and nothing else -- nativeResume
 * and nativeResumeClet are the two. */
static run_result run_entry_env(const char *what, uint32_t entry, uint64_t g_budget)
{
    return run_entry_arg(what, entry, g_jnienv_ptr, g_budget);
}

/* nativeRender(JNIEnv *env, jobject clazz, jint timerID), driven once per timer
 * that came due.
 *
 * This is the call the whole spike exists to measure: it fuses the engine's
 * per-tick game logic and its GLES1 drawing into one function, so instructions
 * retired here are the guest work per frame, and the GL and host-crossing
 * counters reset around it give the other two numbers.
 *
 * `timer_id` is the opaque handle the guest registered through SetTimer and
 * must be passed back unchanged -- see timers.h. */
static run_result run_native_render(uint32_t entry, uint32_t timer_id,
                                    uint64_t budget)
{
    run_result r;
    r.what = "nativeRender";
    r.entry = entry;
    g_cpu.r[13] = GUEST_STACK_TOP - 0x1000;
    g_cpu.r[14] = RETURN_SENTINEL;
    for (int i = 0; i < 13; i++)
        g_cpu.r[i] = 0;
    g_cpu.r[0] = g_jnienv_ptr;
    g_cpu.r[1] = 0;
    g_cpu.r[2] = timer_id;
    g_cpu.cpsr = 0x10;
    uint64_t before = g_insn_count;
    double t0 = host_now();
    r.reason = cpu_run(entry, g_cpu.r[13], g_insn_count + g_budget);
    cpu_trace_final();
    r.insns = g_insn_count - before;
    r.seconds = host_now() - t0;
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
#ifndef WASM_FREESTANDING
/* The guest memory dump feeds the differential test, which needs a filesystem.
 * There is none in the wasm build, and the wasm build exists to measure
 * throughput rather than to be diffed. */
static const struct { uint32_t addr, size; } k_dump_regions[] = {
    { GUEST_IMAGE_BASE, 0x00E00000u },  /* relocated .so, .data, .bss */
    { GUEST_STUB_BASE,  0x00500000u },  /* import stubs + host data   */
    { GUEST_HEAP_BASE,  0x04000000u },  /* the heap: RB-tree nodes live here */
    /* The stack. Without it the oracle starts with a zeroed frame and the
     * divergence it reports is a load of the caller's saved registers --
     * 157,000 instructions of matching execution, then a difference that is
     * the harness's fault, not the decoder's. */
    { GUEST_STACK_TOP - GUEST_STACK_SIZE, GUEST_STACK_SIZE },
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

    /* The import table by index, then name. The oracle needs it to know *what*
     * a stub address means: it is the only way to let the differential run
     * through the crossings both sides can implement identically, instead of
     * stopping at the first one and never seeing inside inflate.
     *
     * Its own magic rather than a count, so a reader that predates it fails
     * cleanly instead of misreading region data. */
    /* Registers as of the dump, so the oracle can start *here* rather than at a
     * function entry with no arguments. Diffing a deep function means reaching
     * it with the state the engine actually had, and neither side can
     * reconstruct that on its own. */
    host_log("  dump: pc=%08x lr=%08x sp=%08x\n", g_cpu.r[15], g_cpu.r[14], g_cpu.r[13]);
    host_log("  dump regs: r13=%08x r14=%08x r15=%08x\n", g_cpu.r[13], g_cpu.r[14], g_cpu.r[15]);
    fwrite("REGS", 1, 4, f);
    fwrite(&g_cpu.r[0], 4, 16, f);
    fwrite(&g_cpu.cpsr, 4, 1, f);

    fwrite("INAM", 1, 4, f);
    uint32_t ni = (uint32_t)g_nimports;
    fwrite(&ni, 4, 1, f);
    for (uint32_t i = 0; i < ni; i++) {
        uint32_t len = (uint32_t)strlen(g_imports[i].name);
        fwrite(&len, 4, 1, f);
        fwrite(g_imports[i].name, 1, len, f);
    }

    fclose(f);
    host_log("guest memory dumped to %s (%u regions)\n", path, n);
}
#else
static void dump_guest(const char *path) { (void)path; }
#endif

static int spike_resume_if_stopped(int reason, uint64_t g_budget)
{
    if (reason != HALT_STOPPED || !g_stop_at_resume)
        return reason;

    host_log("\n=== snapshot at %08x ===\n", g_cpu.r[15]);
    if (g_snapshot_dump_path)
        dump_guest(g_snapshot_dump_path);
    if (g_snapshot_trace_path) {
        host_trace_open(g_snapshot_trace_path);
        g_trace_insn_on = 1;
    }

    uint64_t before = g_insn_count;
    int r = cpu_run(g_cpu.r[15] | 1u, g_cpu.r[13], g_insn_count + g_budget);
    cpu_trace_final();
    host_log("  snapshot: ran %llu more instructions -> %s\n",
             (unsigned long long)(g_insn_count - before), cpu_halt_str(r));
    return r;
}

/* ---- snapshot at a chosen address ----------------------------------------
 *
 * Dumping the whole run's memory is what the oracle needs to start anywhere;
 * dumping it *at* the point the engine first reaches a chosen function is what
 * lets the differential see inside that function. inflate entered with no
 * arguments decides nothing; inflate entered with the z_stream the engine
 * actually built does.
 *
 * The trace is opened here rather than before the run so that it covers only
 * the segment after the snapshot -- both sides then compare from the same
 * instruction with the same state.
 */

/* ---- entry --------------------------------------------------------------- */


/* Everything every entry path needs, and nothing specific to the game boot:
 * the --entry= / --init-array / --jni paths use this and stop there.
 *
 * Return codes are main()'s, preserved: 2 is "no image, or could not read it",
 * 1 is "it is not a usable ELF".
 */
static int spike_load(const char *path)
{
g_mem = (uint8_t *)host_alloc(GUEST_MEM_SIZE);
host_log("guest address space: %u MB at %p\n", GUEST_MEM_SIZE >> 20, (void *)g_mem);

/* Must come after g_mem is known and before the first guest instruction:
 * an unchecked guest access faults as a host SIGSEGV, and the handler's
 * whole job is to translate that back into a guest address. */
host_install_crash_handler();

/* ---- read the .so ---- */
const uint8_t *buf;
size_t len;
#ifdef WASM_FREESTANDING
buf = g_wasm_so;
len = g_wasm_so_len;
if (!buf || !len) {
    host_log("no image supplied: call spike_set_so() first\n");
    return 2;
}
host_log("loaded libgame.so (%u bytes, supplied by the driver)\n",
         (unsigned)len);
#else
FILE *f = fopen(path, "rb");
if (!f) {
    host_log("cannot open %s\n", path);
    return 2;
}
fseek(f, 0, SEEK_END);
long flen = ftell(f);
fseek(f, 0, SEEK_SET);
uint8_t *fread_buf = (uint8_t *)host_alloc((size_t)flen);
if (fread(fread_buf, 1, (size_t)flen, f) != (size_t)flen) {
    host_log("short read on %s\n", path);
    fclose(f);
    return 2;
}
fclose(f);
buf = fread_buf;
len = (size_t)flen;
host_log("loaded %s (%ld bytes)\n", path, flen);
#endif

/* ---- map and relocate it ---- */
if (elf_load(buf, len, &g_img) != 0)
    return 1;
/* Deliberate modifications to the guest binary, before anything runs.
 * Without these the engine boots and then idles: see patches.c. */
patches_apply(&g_img);

/* Must follow elf_load: it registers the JNI table entries as imports and
 * re-stamps the stub page. */
jni_guest_init();

/* After jni_guest_init: this writes the JavaVM's guest address into every
 * .bss slot that a missing subsystem init would have filled. */
patches_fill_vm_caches(&g_img);

gl_state_init();

#ifdef __EMSCRIPTEN__
/* The browser build has a real renderer. The context has to exist before
 * gl4es initialises -- with NOEGL nothing else can tell it the surface
 * size -- and gl4es has to be up before any guest GL call arrives. */
host_browser_set_size(SCREEN_W, SCREEN_H);
host_browser_gl_setup();
gl4es_bridge_init(SCREEN_W, SCREEN_H);
/* Per-call GL tracing is off unless the page asks for it: it is the first
 * thing to reach for when a draw misbehaves, and 600 lines of noise in
 * every ordinary run otherwise. */
{ extern int g_gl_trace; g_gl_trace = g_browser_gl_trace; }
#endif

cpu_init(&g_img);
cpu_set_profile(g_do_profile);
cpu_enable_trace(g_trace_on);
    return 0;
}


/* The game boot proper: src/utils/init.c in the Vita port, in order. Running
 * these in sequence is what puts the guest on the path to its first frame, and
 * the order is load-bearing throughout -- see the comments inline.
 */
static int spike_boot(void)
{
    /* src/utils/init.c in the Vita port, in order: static constructors,
     * then JNI_OnLoad, then nativePreInit. Getting these three to run in
     * sequence is what puts the guest on the path to its first frame. */
    host_log("=== boot sequence ===\n");
    for (uint32_t i = 0; i < g_img.init_array_n; i++) {
        uint32_t raw = LD32(g_img.init_array + i * 4);
        /* The image's .init_array has a trailing NULL entry (ARRAYSZ
         * counts it). Executing it would be exactly the "branch through a
         * NULL function pointer" case the return sentinel exists to
         * distinguish, so skip rather than run. */
        if (!raw) {
            host_log("ctor[%u]                : (null entry, skipped)\n", i);
            continue;
        }
        uint32_t fn = raw | 1u;
        char label[32];
        label[0]='c'; label[1]='t'; label[2]='o'; label[3]='r';
        label[4]='['; label[5]=(char)('0'+(i%10)); label[6]=']'; label[7]=0;
        run_result rr = run_entry_arg(label, fn, 0, g_budget);
        report_run(&rr);
        if (rr.reason == HALT_TRAP)
            break;
    }

    uint32_t jni_entry = find_symbol("JNI_OnLoad");
    run_result rj = run_entry_arg("JNI_OnLoad", jni_entry, g_javavm_ptr, g_budget);
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
        /* LR must be the same sentinel run_entry_arg uses. This was 1,
         * which lands the return at PC 0 -- indistinguishable from a
         * branch through a null function pointer, and reported as "PC left
         * the loaded image" even when the function's epilogue ran
         * correctly. Same class of mistake as the sentinel having been 0. */
        g_cpu.r[14] = RETURN_SENTINEL;
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
        rp.reason = spike_resume_if_stopped(
            cpu_run(pre, g_cpu.r[13], g_insn_count + g_budget), g_budget);
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

    uint32_t init = find_symbol(
        "Java_com_com2us_wrapper_WrapperRenderer_nativeInit");
    if (!init) {
        host_log("nativeInit symbol not found\n");
    } else {
        /* nativeInit(JNIEnv *env, jobject clazz, jlongArray sysTime,
         *            jintArray upTime, jintArray pixel, jfloatArray gyro,
         *            jobject activity) -- seven arguments, so the last
         *            three go on the guest stack in order.
         *
         * The arrays are handles the guest itself calls Get*ArrayElements
         * on; the Java side that would have allocated and filled them is
         * mirrored here. sysTime is wall-clock ms on Android; host_now() is
         * monotonic, so this is "some plausible non-zero ms" rather than a
         * real epoch -- enough to tell whether the engine reads it once or
         * every frame. */
        uint32_t sysTime = jni_host_new_array(8, 1);
        uint32_t sys_addr = jni_host_array_addr(sysTime);
        g_sys_time_addr = sys_addr;
        ST32(sys_addr, (uint32_t)(host_now() * 1000.0));
        ST32(sys_addr + 4, 0);
        uint32_t upTime = jni_host_new_array(4, 1);
        uint32_t pixel  = jni_host_new_array(4, TEXTVIEW_WIDTH * TEXTVIEW_HEIGHT);
        uint32_t gyro   = jni_host_new_array(4, 3);
        uint32_t activity = guest_heap_alloc(16);  /* opaque jobject */

        g_cpu.r[13] = GUEST_STACK_TOP - 0x1000;
        ST32(g_cpu.r[13] + 0, pixel);
        ST32(g_cpu.r[13] + 4, gyro);
        ST32(g_cpu.r[13] + 8, activity);
        g_cpu.r[14] = RETURN_SENTINEL;
        for (int i = 0; i < 13; i++) g_cpu.r[i] = 0;
        g_cpu.r[0] = g_jnienv_ptr;
        g_cpu.r[1] = 0;                  /* clazz: the guest ignores it */
        g_cpu.r[2] = sysTime;
        g_cpu.r[3] = upTime;
        g_cpu.cpsr = 0x10;
        uint64_t ibefore = g_insn_count;
        double it0 = host_now();
        run_result ri;
        ri.what = "nativeInit";
        ri.entry = init;
        ri.reason = spike_resume_if_stopped(
            cpu_run(init, g_cpu.r[13], g_insn_count + g_budget), g_budget);
        cpu_trace_final();
        ri.insns = g_insn_count - ibefore;
        ri.seconds = host_now() - it0;
        report_run(&ri);

        /* --- fire the UserAgree callback ---------------------------
         *
         * nativeInit() bootstraps the game but does not start it. The
         * state machine is entered by STATE_Set(0) (the Init state), whose
         * ONLY call site anywhere in the .so is inside UserAgreeCallback(0)
         * -- and on Android that is reached by the Java-side Com2uS
         * "ActiveUser"/UserAgree module calling back into
         * Java_com_com2us_module_activeuser_ActiveUser_jniUserAgreeNotifier.
         * There is no Java module here, so it is invoked directly.
         *
         * Without it the main timer fires steadily and nothing happens:
         * MainProcess's per-tick check reads STATE_fpProcess, finds it NULL,
         * and returns -- which is exactly the "boots perfectly, does
         * nothing" state this port was in. The parent repository documents
         * the same conclusion (source/patch.c, Corrida 33).
         *
         * Deliberately after nativeInit has fully returned, not inside it:
         * startClet keeps initializing subsystems for several more lines
         * after registering this callback, and firing inside would run the
         * first Enter callback before that finishes -- an ordering Android
         * never produces. */
        uint32_t uac = find_symbol("UserAgreeCallback");
        if (uac) {
            run_result ru = run_entry_arg("UserAgreeCallback(0)", uac, 0, g_budget);
            report_run(&ru);
        } else {
            host_log("UserAgreeCallback symbol not found\n");
        }

        /* The Vita port's order after nativeInit: two resume calls, then
         * the render loop. Both resumes take (JNIEnv*, jclass). */
        uint32_t resume = find_symbol(
            "Java_com_com2us_wrapper_WrapperRenderer_nativeResume");
        uint32_t resume_clet = find_symbol(
            "Java_com_com2us_wrapper_WrapperRenderer_nativeResumeClet");
        if (resume) {
            run_result rr2 = run_entry_env("nativeResume", resume, g_budget);
            report_run(&rr2);
        }
        if (resume_clet) {
            run_result rr3 = run_entry_env("nativeResumeClet", resume_clet, g_budget);
            report_run(&rr3);
        }
    }

    /* The run's clock starts from what nativeInit was handed in sysTime[],
     * because that is what the guest reads back through CS_knlCurrentTime --
     * and therefore what SetTimer's absolute firetimes are comparable to.
     * Starting at zero instead puts every timer ten hours in the future and
     * nothing ever fires, which looks exactly like the engine idling. */
    g_clock = (int64_t)LD32(g_sys_time_addr) |
              ((int64_t)LD32(g_sys_time_addr + 4) << 32);

    /* ---- skip the Com2uS Hub ------------------------------------------
     *
     * TITLE_Event -- the title screen's "any touch to continue" handler --
     * re-decides on every tap where to go:
     *
     *     if (GetHubFirstLoginState() == 1 || HUBINSTALL_Check() != 1)
     *         STATE_Set(5);      // gameplay
     *     else
     *         STATE_Set(3);      // the Hub join screen
     *
     * State 3 is Com2uS's social/login screen. There is no Hub here and no way
     * to complete it, so a tap on the title lands there and the game goes no
     * further -- which is what "Touch to Start does nothing" actually is.
     *
     * The PS Vita port traced this end to end (source/patch.c, Corrida 87) and
     * found the real escape: pressing the Hub's own "Later" button runs
     * STATE_Set(5) and HUBINSTALL_Save(0), persisting "the Hub has been seen".
     * Doing that here before the first frame makes TITLE_Event's gate resolve
     * to state 5 on its own, without ever building the Hub UI. */
    {
        uint32_t save = find_symbol("HUBINSTALL_Save");
        uint32_t check = find_symbol("HUBINSTALL_Check");
        if (save) {
            spike_call1(save, 0);
            host_log("  [hub] HUBINSTALL_Save(0) -> HUBINSTALL_Check()=%u\n",
                     check ? spike_call1(check, 0) : 0u);
        } else {
            host_log("  [hub] HUBINSTALL_Save not found -- the title screen will "
                     "send taps to the Hub join screen\n");
        }
    }

    g_native_render = find_symbol(
        "Java_com_com2us_wrapper_WrapperRenderer_nativeRender");
    if (!g_native_render) {
        host_log("nativeRender symbol not found\n");
        return 1;
    }
    /* Input is optional: without it the game still boots and renders, it just
     * cannot be driven. Say so rather than failing the boot. */
    g_native_event = find_symbol(
        "Java_com_com2us_wrapper_WrapperRenderer_nativeEvent");
    if (!g_native_event)
        host_log("nativeEvent symbol not found -- input will not work\n");
    return 0;
}



/* ---- loader progress ----------------------------------------------------
 *
 * The PS Vita port spent several corridas working out why the game sat on the
 * loading screen forever (source/patch.c, Corrida 45-48): INITSTATE_nState
 * reaches 3, a terminal "Memory error" state with no exit transition, when
 * GAMELOADER_ProcessInstall or ProcessPreLoad fails -- and the engine then
 * redraws the same message every tick forever, which is exactly what "stuck on
 * Now Loading" looks like from outside.
 *
 * Its cause was a sign-bit bug that this port does not have (its heap sat at
 * 0x8xxxxxxx, so `-1 < pointer` was false; ours is at 0x02000000). But the
 * same state machine is running here, so the same numbers answer the same
 * question -- and unlike the Vita build, every one of these globals is in
 * .dynsym, so they can be read by name instead of by hardcoded offset.
 *
 * Logged only when something changes, so a healthy run says nothing.
 */
static struct {
    uint32_t state, load_frame, pool_used, pool_total, ptext;
    int ready;
} g_probe_prev;

/* Call a guest function that takes one integer and returns one. Used only from
 * the frame boundary, like everything else that enters the guest. */
static uint32_t spike_call1(uint32_t fn, uint32_t a0)
{
    if (!fn)
        return 0;
    g_cpu.r[13] = GUEST_STACK_TOP - 0x1000;
    g_cpu.r[14] = RETURN_SENTINEL;
    for (int i = 0; i < 13; i++)
        g_cpu.r[i] = 0;
    g_cpu.r[0] = a0;
    g_cpu.cpsr = 0x10;
    cpu_run(fn | 1u, g_cpu.r[13], g_insn_count + g_budget / 4);
    return g_cpu.r[0];
}

static uint32_t spike_call2(uint32_t fn, uint32_t a0, uint32_t a1)
{
    if (!fn)
        return 0;
    g_cpu.r[13] = GUEST_STACK_TOP - 0x1000;
    g_cpu.r[14] = RETURN_SENTINEL;
    for (int i = 0; i < 13; i++)
        g_cpu.r[i] = 0;
    g_cpu.r[0] = a0;
    g_cpu.r[1] = a1;
    g_cpu.cpsr = 0x10;
    cpu_run(fn | 1u, g_cpu.r[13], g_insn_count + g_budget / 4);
    return g_cpu.r[0];
}

static uint32_t spike_call4(uint32_t fn, uint32_t a0, uint32_t a1, uint32_t a2,
                            uint32_t a3)
{
    if (!fn)
        return 0;
    g_cpu.r[13] = GUEST_STACK_TOP - 0x1000;
    g_cpu.r[14] = RETURN_SENTINEL;
    for (int i = 0; i < 13; i++)
        g_cpu.r[i] = 0;
    g_cpu.r[0] = a0; g_cpu.r[1] = a1; g_cpu.r[2] = a2; g_cpu.r[3] = a3;
    g_cpu.cpsr = 0x10;
    cpu_run(fn | 1u, g_cpu.r[13], g_insn_count + g_budget);
    return g_cpu.r[0];
}

static uint32_t spike_call3(uint32_t fn, uint32_t a0, uint32_t a1, uint32_t a2)
{
    if (!fn)
        return 0;
    g_cpu.r[13] = GUEST_STACK_TOP - 0x1000;
    g_cpu.r[14] = RETURN_SENTINEL;
    for (int i = 0; i < 13; i++)
        g_cpu.r[i] = 0;
    g_cpu.r[0] = a0; g_cpu.r[1] = a1; g_cpu.r[2] = a2;
    g_cpu.cpsr = 0x10;
    cpu_run(fn | 1u, g_cpu.r[13], g_insn_count + g_budget / 4);
    return g_cpu.r[0];
}

/* Ask RES_LoadToHeap for the first install record by name. It is the single
 * gate every install path funnels through, and it reports its failures as
 * -1/-2/-3, so its return value says whether the failure is "the resource is
 * not there" or something further down. */
static void spike_probe_res_load(const char *name)
{
    uint32_t fn = elf_find_symbol(&g_img, "RES_LoadToHeap");
    if (!fn)
        return;
    uint32_t str = guest_heap_alloc(96);
    uint32_t lenp = guest_heap_alloc(8);
    size_t n = 0;
    while (name[n] && n < 95) { ST8(str + n, (uint8_t)name[n]); n++; }
    ST8(str + n, 0);
    ST32(lenp, 0);
    uint32_t r = spike_call2(fn, str, lenp);
    const uint8_t *d = (r && r < GUEST_MEM_SIZE) ? (g_mem + r) : NULL;
    host_log("  [res] RES_LoadToHeap(\"%s\") -> %08x (%d), length=%u, "
             "first bytes %02x %02x %02x %02x %02x %02x %02x %02x\n",
             name, r, (int)r, LD32(lenp),
             d ? d[0] : 0, d ? d[1] : 0, d ? d[2] : 0, d ? d[3] : 0,
             d ? d[4] : 0, d ? d[5] : 0, d ? d[6] : 0, d ? d[7] : 0);
    /* The install path's next step is LZMA_Decode on the payload after the
     * format byte. Calling it here says whether the decode itself is what
     * fails, rather than the file write that follows. */
    uint32_t lz = elf_find_symbol(&g_img, "LZMA_Decode");
    if (lz && d && d[0] == 1) {
        uint32_t outp = guest_heap_alloc(8), lenp2 = guest_heap_alloc(8);
        ST32(outp, 0);
        ST32(lenp2, 0);
        uint32_t ok = spike_call4(lz, r + 1, LD32(lenp) - 1, outp, lenp2);
        host_log("  [res] LZMA_Decode() -> %u, out=%08x len=%u\n",
                 ok, LD32(outp), LD32(lenp2));
    }
}

/* The install table, once. Each 7-byte record is { u16 text id, ..., u8 flags }
 * and GAMELOADER_ProcessInstall reads the name through MEMORYTEXT_GetText_E --
 * so resolving the ids here names the exact record that fails. */
static void spike_dump_install_records(void)
{
    uint32_t base = elf_find_symbol(&g_img, "INSTALLBASE_pData");
    uint32_t cnt  = elf_find_symbol(&g_img, "INSTALLBASE_nRecordCount");
    uint32_t sz   = elf_find_symbol(&g_img, "INSTALLBASE_nRecordSize");
    uint32_t gettext = elf_find_symbol(&g_img, "MEMORYTEXT_GetText_E");
    if (!base || !cnt || !sz)
        return;
    uint32_t p = LD32(base);
    uint32_t n = LD16(cnt) ? ((int16_t)(LD8(cnt) | (LD8(cnt + 1) << 8))) : 0;
    uint32_t stride = LD16(sz) ? ((int16_t)(LD8(sz) | (LD8(sz + 1) << 8))) : 0;
    host_log("  [install] %u records, stride %u, at %08x\n", n, stride, p);
    if (!p || !stride || p >= GUEST_MEM_SIZE)
        return;
    for (uint32_t i = 0; i < n && i < 24; i++) {
        uint32_t r = p + i * stride;
        if (r + stride > GUEST_MEM_SIZE)
            break;
        uint32_t id = LD16(r);
        uint32_t flags = LD8(r + 6);
        uint32_t name = gettext ? spike_call1(gettext, id) : 0;
        host_log("  [install]   [%u] id=%u flags=%02x name=%08x \"%s\"\n",
                 i, id, flags, name,
                 (name && name < GUEST_MEM_SIZE) ? (const char *)(g_mem + name) : "");
    }
}

/* The live UI control table.
 *
 * `UI_listControl` is a struct instance (not a pointer to one): count at +0xc,
 * a pointer to the entry array at +0x10, entries 0x2c bytes apart, and per
 * entry centreX/Y at +0xc/+0x10, width/height at +0x14/+0x18 and the click
 * callback at +0x28. The PS Vita port reads exactly this for its D-pad menu
 * navigation (source/wr_touch.c).
 *
 * Dumping it answers the question a screenshot cannot: where the engine thinks
 * its buttons are, as opposed to where they are drawn.
 */
static void spike_dump_ui_controls(void)
{
    uint32_t lc = elf_find_symbol(&g_img, "UI_listControl");
    if (!lc) { host_log("  [ui] UI_listControl not found\n"); return; }
    uint32_t n = LD32(lc + 0xc);
    uint32_t arr = LD32(lc + 0x10);
    uint32_t ow = elf_find_symbol(&g_img, "jGetOriginalWidth");
    uint32_t oh = elf_find_symbol(&g_img, "jGetOriginalHeight");
    uint32_t sw = elf_find_symbol(&g_img, "SGL_GetScreenWidth");
    uint32_t sh = elf_find_symbol(&g_img, "SGL_GetScreenHeight");
    uint32_t dw = elf_find_symbol(&g_img, "SGL_GetDeviceWidth");
    uint32_t dh = elf_find_symbol(&g_img, "SGL_GetDeviceHeight");
    host_log("  [ui] UI_listControl count=%u entries=%08x  original=%ux%u\n",
             n, arr, ow ? spike_call1(ow, 0) : 0u, oh ? spike_call1(oh, 0) : 0u);
    host_log("  [ui]   SGL screen=%ux%u device=%ux%u orientation=%u\n",
             sw ? spike_call1(sw, 0) : 0u, sh ? spike_call1(sh, 0) : 0u,
             dw ? spike_call1(dw, 0) : 0u, dh ? spike_call1(dh, 0) : 0u,
             elf_find_symbol(&g_img, "SGL_GetScreenOrientation")
                 ? spike_call1(elf_find_symbol(&g_img, "SGL_GetScreenOrientation"), 0) : 0u);
    if (!arr || arr >= GUEST_MEM_SIZE || n > 64)
        return;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t e = arr + i * 0x2cu;
        if (e + 0x2c > GUEST_MEM_SIZE) break;
        host_log("  [ui]   [%u] state=%08x centre=(%d,%d) size=(%d,%d) "
                 "click=%08x\n", i, LD32(e),
                 (int)LD32(e + 0xc), (int)LD32(e + 0x10),
                 (int)LD32(e + 0x14), (int)LD32(e + 0x18),
                 LD32(e + 0x28));
    }
    /* Drive the list's own event handler with a correctly-scaled point. If a
     * control activates, the UI path works and the problem is only the
     * coordinate transform in front of it. */
    {
        uint32_t ev = elf_find_symbol(&g_img, "UICONTROLLIST_Event");
        uint32_t before0 = LD32(arr);
        if (ev) {
            spike_call4(ev, lc, 0x18, 233, 141);
            host_log("  [ui]   UICONTROLLIST_Event(list,0x18,233,141): "
                     "control0 state %08x -> %08x\n",
                     before0, LD32(arr));
        }
    }

    /* Which coordinate space does the hit-test actually live in? Asking the
     * engine's own UICONTROL_IsInEventArea, rather than assuming, settles it:
     * the touch arrives in 960x544, the controls are laid out in 480x320, and
     * the scale between them is not uniform (x2 across, 544/320 = 1.7 down). */
    {
        uint32_t isin = elf_find_symbol(&g_img, "UICONTROL_IsInEventArea");
        uint32_t e0 = arr + 0x0cu;   /* centreX of entry 0 happens to be here */
        if (isin) {
            const struct { const char *what; int x, y; } pts[] = {
                { "raw touch scaled /2,/1.7", 233, 141 },
                { "raw touch",                467, 240 },
                { "control centre",           240, 142 },
                { "centre*2",                 480, 284 },
            };
            for (unsigned k = 0; k < sizeof(pts) / sizeof(pts[0]); k++)
                host_log("  [ui]   hit(%-24s %3d,%3d) = %u\n", pts[k].what,
                         pts[k].x, pts[k].y,
                         spike_call3(isin, arr, (uint32_t)pts[k].x,
                                     (uint32_t)pts[k].y));
            (void)e0;
        }
    }
}

/* Which STATE_* the engine is in, by name.
 *
 * STATE_nState is not exported, but STATE_fpEnter is -- and it holds the
 * current state's entry function. Resolving that to a symbol is the difference
 * between "the screen changed" and "the game moved to state 5 (gameplay)".
 */
static void spike_probe_state(void)
{
    static uint32_t prev;
    static int ready;
    uint32_t slot = ready ? 0 : elf_find_symbol(&g_img, "STATE_fpEnter");
    if (!ready) {
        ready = 1;
        if (!slot) return;
    }
    uint32_t fn = LD32(slot);
    static uint32_t prev_gs;
    static uint32_t gs_slot, proc_slot;
    if (!gs_slot) {
        gs_slot = elf_find_symbol(&g_img, "GAMESTATE_nState");
        proc_slot = elf_find_symbol(&g_img, "STATE_fpProcess");
    }
    uint32_t gs = gs_slot ? LD32(gs_slot) : 0xffffffffu;
    if (fn != prev || gs != prev_gs) {
        prev = fn;
        prev_gs = gs;
        uint32_t at = 0;
        const char *nm = fn ? elf_nearest_sym(&g_img, fn & ~1u, &at) : NULL;
        uint32_t pf = proc_slot ? LD32(proc_slot) : 0;
        const char *pn = pf ? elf_nearest_sym(&g_img, pf & ~1u, &at) : NULL;
        host_log("  [state] GAMESTATE_nState=%u STATE_fpEnter=%s STATE_fpProcess=%s\n",
                 gs, nm ? nm : "-", pn ? pn : "-");
    }
}

static void spike_probe_loader(void)
{
    static uint32_t st, lf, pu, pt, tx, ib_n, ib_p, ib_s, sb_n;
    if (!g_probe_prev.ready) {
        st = elf_find_symbol(&g_img, "INITSTATE_nState");
        lf = elf_find_symbol(&g_img, "INITSTATE_LoadFrameCnt");
        pu = elf_find_symbol(&g_img, "MEM_nUsedPoolSize");
        pt = elf_find_symbol(&g_img, "MEM_nTotalPoolSize");
        tx = elf_find_symbol(&g_img, "GAMELOADER_pText");
        ib_n = elf_find_symbol(&g_img, "INSTALLBASE_nRecordCount");
        ib_s = elf_find_symbol(&g_img, "INSTALLBASE_nRecordSize");
        ib_p = elf_find_symbol(&g_img, "INSTALLBASE_pData");
        sb_n = elf_find_symbol(&g_img, "SYMBOLBASE_nRecordCount");
        g_probe_prev.ready = 1;
        g_probe_prev.state = g_probe_prev.load_frame = 0xffffffffu;
    }
    uint32_t state = st ? LD8(st) : 0xffffffffu;
    uint32_t load_frame = lf ? LD32(lf) : 0;
    uint32_t used = pu ? LD32(pu) : 0;
    uint32_t total = pt ? LD32(pt) : 0;
    uint32_t text = tx ? LD32(tx) : 0;
    /* Must run before the change-detection early-return below, or it only
     * advances on frames where the loader state happened to change -- which is
     * none of them once loading is done. */
    if (g_entered_menu) {
        g_menu_frame++;
        /* Repeat, so a click between two dumps shows up as a changed
         * control state -- which is the difference between "the hit-test
         * missed" and "the event never got that far". */
        if (g_menu_frame % 200 == 0 && g_menu_frame <= 2000)
            spike_dump_ui_controls();
    }
    if (state == g_probe_prev.state && load_frame == g_probe_prev.load_frame &&
        used == g_probe_prev.pool_used && text == g_probe_prev.ptext)
        return;
    if (state == 1 && g_probe_prev.state != 1) {
        spike_dump_install_records();
        spike_probe_res_load("i_character.dat");
    }
    /* Once the install finishes, hand off to the Menu state directly.
     *
     * This is the same thing the PS Vita port does, for the same reason: the
     * title screen's "any touch to continue" handler re-decides on every tap
     * whether to continue or to go to the Com2uS Hub, and the Hub does not
     * exist here. Their fix (source/patch.c, Corrida 87) is to call the real
     * STATE_Set(5) -- STATE_EnterMenu -- at the point the Hub would have been
     * entered, so the title is never on the path at all. Ours does the same at
     * the equivalent point: the load completing.
     *
     * This is not a substitute for making the title's own tap work; it is the
     * step that shows whether everything downstream of it renders. */
    if (state == 2 && g_probe_prev.state != 2 && !g_entered_menu) {
        uint32_t ss = elf_find_symbol(&g_img, "STATE_Set");
        if (ss) {
            g_entered_menu = 1;
            spike_call1(ss, 5);
            host_log("  [menu] STATE_Set(5) -- entered the Menu state\n");
        }
    }
    g_probe_prev.state = state;
    g_probe_prev.load_frame = load_frame;
    g_probe_prev.pool_used = used;
    g_probe_prev.ptext = text;
    host_log("  [loader] state=%u frame=%u pool=%u/%u text=\"%.160s\" "
             "INSTALLBASE(n=%u size=%u p=%08x) SYMBOLBASE(n=%u)\n",
             state, load_frame, used, total,
             (text && text < GUEST_MEM_SIZE) ? (const char *)(g_mem + text) : "",
             ib_n ? LD16(ib_n) : 0u, ib_s ? LD16(ib_s) : 0u,
             ib_p ? LD32(ib_p) : 0u, sb_n ? LD16(sb_n) : 0u);
}

/* ---- input -------------------------------------------------------------
 *
 * nativeEvent(JNIEnv *env, jobject clazz, int type, int p1, int p2,
 *             int touchCount) -- six arguments, so AAPCS puts env/clazz/type/p1
 * in r0-r3 and p2/touchCount on the guest stack. Same shape nativePreInit
 * already needs for its fifth argument.
 *
 * Called only from spike_tick(), between cpu_run() calls. That is not a style
 * preference: g_cpu and g_budget are single globals assigned at cpu_run entry,
 * so a nested cpu_run would corrupt both silently, and the guest is meant to be
 * entered at a frame boundary with a fresh stack.
 */
static void spike_dispatch_event(int type, int p1, int p2, int touch_count)
{
    if (!g_native_event)
        return;
    g_cpu.r[13] = GUEST_STACK_TOP - 0x1000;
    ST32(g_cpu.r[13] + 0, (uint32_t)p2);
    ST32(g_cpu.r[13] + 4, (uint32_t)touch_count);
    g_cpu.r[14] = RETURN_SENTINEL;
    for (int i = 0; i < 13; i++)
        g_cpu.r[i] = 0;
    g_cpu.r[0] = g_jnienv_ptr;      /* env   */
    g_cpu.r[1] = 0;                 /* clazz */
    g_cpu.r[2] = (uint32_t)type;
    g_cpu.r[3] = (uint32_t)p1;
    g_cpu.cpsr = 0x10;
    /* A g_budget of its own: one event must not be able to burn seconds inside a
     * tick with nothing in the log to say why. */
    uint64_t before = g_insn_count;
    int reason = cpu_run(g_native_event, g_cpu.r[13],
                         g_insn_count + g_budget / 4);
    cpu_trace_final();
    /* Logged unconditionally: these are rare -- a few per second, and the drag
     * gate caps the rest -- and "did my click reach the engine" is the first
     * question anyone asks of the input path. */
    host_log("  [event] type=%d slot=%d x=%d y=%d touches=%d  %llu instrs -> %s\n",
             type, p1 - 1, (p2 >> 16) & 0xffff, p2 & 0xffff, touch_count,
             (unsigned long long)(g_insn_count - before), cpu_halt_str(reason));
}


/* End of run: the browser surface dump, then the reports. Called from
 * spike_tick_browser()'s stop path in the browser, and from the tail of main()
 * on the CLI. */
static void spike_finish(void)
{
#ifdef __EMSCRIPTEN__
    /* Dump the engine's own screen surface, base64, in chunks.
     *
     * A black canvas is ambiguous -- the engine may have drawn
     * nothing, or drawn correctly into a buffer that never reached
     * the screen -- and there is no debugger here to tell them
     * apart. This is the buffer the engine last handed to
     * glTexSubImage2D for the screen blit, so it is exactly what
     * the canvas *should* be showing. Chunked because a single
     * 262 KB console line is truncated. */
    if (g_browser_dump_screen) {
        /* Read the *default framebuffer*, not the engine's own
         * surface: this is what the canvas actually holds, after
         * gl4es, the shaders and the browser have had their say.
         * Downsampled 4x -- 960x544 RGBA is 2 MB, and a console
         * line is not -- which is still enough to recognise a
         * frame. */
        extern void host_browser_read_pixels(unsigned char *, int,
                                             int, int);
        static unsigned char px[240 * 136 * 4];
        host_browser_read_pixels(px, SCREEN_W, SCREEN_H, 4);
        static const char b64[] =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"
            "0123456789+/";
        char line[4097];
        uint32_t o = 0;
        host_log("  [dump] framebuffer %dx%d (4x downsample)\n",
                 240, 136);
        for (size_t i = 0; i + 3 < sizeof(px); i += 4) {
            uint32_t v = ((uint32_t)px[i] << 16) |
                         ((uint32_t)px[i + 1] << 8) | px[i + 2];
            line[o++] = b64[(v >> 18) & 63];
            line[o++] = b64[(v >> 12) & 63];
            line[o++] = b64[(v >> 6) & 63];
            line[o++] = b64[v & 63];
            if (o >= 4096) { line[o] = 0; host_log("[dump] %s\n", line); o = 0; }
        }
        if (o) { line[o] = 0; host_log("[dump] %s\n", line); }
        /* And the two surfaces the engine composes with, RGBA,
         * 4x downsampled -- the framebuffer alone cannot say
         * whether a wrong-looking frame is the texture's fault. */
        {
            extern uint32_t g_u480_ptr, g_u600_ptr, g_csfb_ptr;
            if (g_csfb_ptr) {
                host_log("[dump] csfb 384x256\n");
                uint32_t o3 = 0;
                char l3[4097];
                for (uint32_t y = 0; y < 256; y += 2)
                    for (uint32_t x = 0; x < 384; x += 2) {
                        const uint8_t *q = g_mem + g_csfb_ptr +
                                           (y * 384u + x) * 2u;
                        uint32_t v = (uint32_t)q[0] |
                                     ((uint32_t)q[1] << 8);
                        uint32_t r = (v >> 11) & 0x1f,
                                 g2 = (v >> 5) & 0x3f,
                                 b2 = v & 0x1f;
                        uint32_t rgb =
                            (((r << 3) | (r >> 2)) << 16) |
                            (((g2 << 2) | (g2 >> 4)) << 8) |
                            ((b2 << 3) | (b2 >> 2));
                        l3[o3++] = b64[(rgb >> 18) & 63];
                        l3[o3++] = b64[(rgb >> 12) & 63];
                        l3[o3++] = b64[(rgb >> 6) & 63];
                        l3[o3++] = b64[rgb & 63];
                        if (o3 >= 4096) { l3[o3] = 0;
                            host_log("[dump] %s\n", l3); o3 = 0; }
                    }
                if (o3) { l3[o3] = 0; host_log("[dump] %s\n", l3); }
            }
            const struct { const char *nm; uint32_t p, w, h; } u[2] = {
                { "upload480", g_u480_ptr, 480, 320 },
                { "upload600", g_u600_ptr, 600, 320 },
            };
            for (int k = 0; k < 2; k++) {
                if (!u[k].p) continue;
                host_log("[dump] %s %ux%u\n", u[k].nm, u[k].w, u[k].h);
                uint32_t o2 = 0;
                char l2[4097];
                for (uint32_t y = 0; y < u[k].h; y += 4)
                    for (uint32_t x = 0; x < u[k].w; x += 4) {
                        const uint8_t *q = g_mem + u[k].p +
                            (y * u[k].w + x) * 4u;
                        uint32_t v = ((uint32_t)q[0] << 16) |
                                     ((uint32_t)q[1] << 8) | q[2];
                        l2[o2++] = b64[(v >> 18) & 63];
                        l2[o2++] = b64[(v >> 12) & 63];
                        l2[o2++] = b64[(v >> 6) & 63];
                        l2[o2++] = b64[v & 63];
                        if (o2 >= 4096) { l2[o2] = 0;
                            host_log("[dump] %s\n", l2); o2 = 0; }
                    }
                if (o2) { l2[o2] = 0; host_log("[dump] %s\n", l2); }
            }
        }
        host_log("[dump] end\n");
    }
#endif

/* ---- report ---- */
    if (g_do_profile)
        report_profile(60);
    timers_report();
    imports_report(20);
    jni_report(15);
    gl_report(70);

    uint32_t at = 0;
    const char *near = elf_nearest_sym(&g_img, g_cpu.r[15], &at);
    host_log("\nfinal pc=%08x (%s+0x%x)\n", g_cpu.r[15],
             near ? near : "?", near ? g_cpu.r[15] - at : 0);
}


/* One frame. Returns 1 to continue, 0 when the run should stop.
 *
 * The stop condition is the one main()'s loop had, and it is easy to invert:
 * HALT_BUDGET is the *stop* signal. A frame that completes normally ends
 * HALT_BAD_PC with r15 == RETURN_SENTINEL & ~1, which report_run labels
 * "returned to caller". Note also the ndue > 0 guard -- `reason` still holds
 * its stale initial value on a tick where no timer fired, and dropping the
 * guard ends the run on the very first one.
 */
static int spike_tick(void)
{
    /* Input first, then the timers, so the engine sees this frame's input
     * during this frame's render. Same order as the Vita port's loop. */
    input_poll();

    /* ---- the clock ----
     *
     * The guest reads this through GetSystemCurrentTime -> LD32(sysTime), which
     * takes only the low 32 bits, so it has to stay under 2^31 ms and it has to
     * be the same clock the timers are compared against. Nothing else may use
     * it: the input drag gate uses host_now(), not this.
     */
    double dt = 16.0;
    if (!g_use_fixed_step) {
        double now = host_now() * 1000.0;
        double real = now - g_last_now_ms;
        g_last_now_ms = now;
        /* A tick takes hundreds of milliseconds here, and a backgrounded tab
         * takes much longer. Clamp so one stall cannot jump the game clock by
         * an unbounded amount. */
        if (real > 0.0)
            dt = real > 250.0 ? 250.0 : real;
    }
    g_clock += (int64_t)dt;
    ST32(g_sys_time_addr, (uint32_t)g_clock);
    ST32(g_sys_time_addr + 4, (uint32_t)(g_clock >> 32));

    uint32_t due[TIMER_MAX];
    int ndue = timers_due(g_clock, due, TIMER_MAX);
    if (ndue) {
        spike_probe_loader();
        spike_probe_state();
    }

    uint64_t d0 = g_gl_draw_calls;
    uint64_t x0 = 0;
    for (int i = 0; i < g_nimports; i++)
        x0 += g_imports[i].calls;

    uint64_t frame_insns = 0;
    double frame_secs = 0.0;
    int reason = HALT_BUDGET;
    for (int d = 0; d < ndue; d++) {
        run_result rf = run_native_render(g_native_render, due[d], g_budget);
        cpu_trace_final();
        frame_insns += rf.insns;
        frame_secs += rf.seconds;
        reason = rf.reason;
        if (rf.reason == HALT_BUDGET)
            break;
    }

    uint64_t d1 = g_gl_draw_calls;
    uint64_t x1 = 0;
    for (int i = 0; i < g_nimports; i++)
        x1 += g_imports[i].calls;

    if (reason == HALT_BAD_PC) {
        host_log("  frame %3d : pc ran to %08x (lr=%08x)\n",
                 g_frames_done, g_cpu.r[15], g_cpu.r[14]);
        /* A null PC means the guest called through a null function pointer --
         * the signature of a subsystem that was never initialised. LR names the
         * call site, and the stack scan names the chain above it. */
        if (g_cpu.r[15] == 0) {
            uint32_t at = 0;
            const char *nm = elf_nearest_sym(&g_img, g_cpu.r[14] & ~1u, &at);
            host_log("  [null-call] caller %s+0x%x\n",
                     nm ? nm : "?", nm ? (g_cpu.r[14] & ~1u) - at : 0);
            guest_backtrace("null call");
        }
    }
    host_log("  frame %3d : %9llu instrs  %7.2f ms  %6.2f MIPS"
             "  timers=%d draw=%llu host_calls=%llu  -> %s\n",
             g_frames_done, (unsigned long long)frame_insns,
             frame_secs * 1000.0,
             frame_secs > 0 ? (double)frame_insns / frame_secs / 1e6 : 0.0,
             ndue,
             (unsigned long long)(d1 - d0),
             (unsigned long long)(x1 - x0),
             reason == HALT_BUDGET ? "budget exhausted" : cpu_halt_str(reason));
    g_total_insns += frame_insns;
    g_frames_done++;

    if (ndue > 0) {
        if (reason != HALT_BUDGET)
            g_ok_frames++;
        else
            return 0;
    }
    return 1;
}

/* The browser's entry: adds the frame cap, and finishes the run -- report and
 * dump -- from inside the same rAF callback as the last render, because
 * glReadPixels on the default framebuffer from outside one reads an undefined
 * buffer. */
static int spike_tick_browser(void)
{
    if (g_frames_max > 0 && g_frames_done >= g_frames_max) {
        spike_finish();
        return 0;
    }
    if (!spike_tick()) {
        spike_finish();
        return 0;
    }
    return 1;
}

int main(int argc, char **argv)
{
    const char *path = NULL;
    const char *dump_path = NULL;
    const char *trace_path = NULL;
    uint32_t only_entry = 0;
    int frames = 1;
    int timer_id = 0;
    /* Instruction budget for one guest entry. It is a runaway guard, not a
     * frame target -- and GAMELOADER_ProcessInstall legitimately runs the whole
     * install synchronously inside one call: twelve LZMA-decoded data files,
     * several hundred million instructions together. Too small a budget stops
     * the run in the middle of the install, which looks exactly like the engine
     * hanging. */
    uint64_t budget = 3000u * 1000u * 1000u;
    int do_init_array = 0, do_jni = 0, do_profile = 1, trace = 0, do_boot = 0;
    (void)timer_id;   /* parsed but never used: the ids come from timers_due() */

#ifdef __EMSCRIPTEN__
    /* First thing: these are the page's options and the block below consumes
     * them. Reading them further down (where the GL setup lives) left
     * ?frames= doing nothing at all, because `frames` had already been
     * computed from a g_browser_frames that was still zero. */
    host_browser_read_frames();
    host_browser_read_dump();
    host_browser_read_gl_trace();
    host_browser_read_fixed();
#endif
    (void)0;

#if defined(WASM_FREESTANDING) || defined(__EMSCRIPTEN__)
    /* No command line in either wasm build. The browser one still reads its
     * image with fopen -- emscripten gives it a real filesystem, and the .so is
     * preloaded into it -- so only the option parsing is skipped. */
#else
    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "--g_budget=", 9) == 0)
            g_budget = (uint64_t)strtoull(argv[i] + 9, NULL, 10);
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
        else if (strncmp(argv[i], "--frames=", 9) == 0)
            frames = (int)strtol(argv[i] + 9, NULL, 10);
        else if (strncmp(argv[i], "--timer=", 8) == 0)
            timer_id = (int)strtol(argv[i] + 8, NULL, 10);
        else if (strncmp(argv[i], "--stop-at=", 10) == 0)
            g_stop_at = (uint32_t)strtoul(argv[i] + 10, NULL, 16);
        else if (strncmp(argv[i], "--watch=", 8) == 0)
            g_watch_addr = (uint32_t)strtoul(argv[i] + 8, NULL, 16);
        else if (strncmp(argv[i], "--probe=", 8) == 0) {
#ifdef GUEST_CHECK
            char rest[32];
            g_probe_lo = (uint32_t)strtoul(argv[i] + 8, NULL, 16);
            snprintf(rest, sizeof(rest), "%s", strchr(argv[i], ',') + 1);
            g_probe_len = (uint32_t)strtoul(rest, NULL, 10);
#else
            host_log("--probe needs a GUEST_CHECK build\n");
#endif
        }
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
#endif
#if defined(WASM_FREESTANDING) || defined(__EMSCRIPTEN__)
    (void)argc; (void)argv;
    do_boot = 1;
    /* 0 means "no cap". This used to default to 600, which was "under a second
     * of game time" when a tick was a fixed 16 ms -- but the browser now runs
     * on real elapsed time at a few ticks per second, so 600 is minutes. The
     * interactive page wants to run until it is closed; a headless run passes
     * ?frames=N and gets the cap it needs. */
    frames = 0;
#ifdef __EMSCRIPTEN__
    {
        extern int g_browser_frames;
        frames = g_browser_frames;   /* 0 unless the page asked for a cap */
    }
#endif
#ifdef __EMSCRIPTEN__
    /* The browser build reads the image from its own filesystem. */
    if (!path)
        path = "libgame.so";
#endif
#endif
#if !defined(WASM_FREESTANDING)
    if (!path) {
        host_log("usage: spike <libgame.so> [--all] [--g_budget=N] [--no-profile]\n");
        return 2;
    }
#endif


    g_budget = budget;
    g_do_profile = do_profile;
    g_trace_on = trace;
    g_frames_max = frames > 0 ? frames : 0;
#ifdef __EMSCRIPTEN__
    /* Real elapsed time by default in the browser, so the game runs at the
     * right speed despite the interpreter's few ticks per second. ?fixed=1
     * restores the deterministic 16 ms step for headless runs.
     *
     * The CLI keeps the fixed step unconditionally: the differential oracle and
     * every recorded run depend on it. */
    { extern int g_browser_fixed; g_use_fixed_step = !g_browser_fixed; }
#endif

    int rc = spike_load(path);
    if (rc)
        return rc;


    if (g_stop_at) {
        /* A snapshot describes the engine's state at a chosen point, so both
         * the dump and the trace have to start *there* rather than before the
         * run. Deferred to run_entry_arg, which takes the dump when it reaches
         * the address and starts the trace from that instruction. */
        g_snapshot_dump_path = dump_path;
        g_snapshot_trace_path = trace_path;
        g_stop_at_resume = 1;
        dump_path = NULL;
        trace_path = NULL;
    } else if (trace_path) {
        host_trace_open(trace_path);
        g_trace_insn_on = 1;
    }


    /* ---- run it ---- */
    host_log("\n");
    if (do_boot) {
        if (spike_boot() != 0)
            return 1;
        input_init(spike_dispatch_event);
#ifdef __EMSCRIPTEN__
        /* The browser drives the engine one tick per requestAnimationFrame.
         * Input goes in first so the very first tick can already see events.
         *
         * Nothing may follow host_browser_main_loop(): it does not return, and
         * main() returning is what lets the rAF chain own the page. */
        host_browser_input_init();
        host_browser_main_loop(spike_tick_browser);
        return 0;
#else
        host_log("\n=== %d frame(s), timer-driven ===\n", frames);
        if (!timers_active())
            host_log("  no timer registered -- the engine schedules its own\n"
                     "  work, so there is nothing to run yet\n");
        host_log("  clock at frame loop start: %lld (sysTime array at %08x)\n",
                 (long long)g_clock, g_sys_time_addr);
        for (int f = 0; f < frames; f++) {
            if (!spike_tick())
                break;
        }
        if (g_ok_frames)
            host_log("  average over %d frame(s): %llu instructions\n",
                     g_ok_frames,
                     (unsigned long long)(g_total_insns / (uint64_t)g_ok_frames));
#endif

    } else if (only_entry) {
        /* One entry point only, so a differential run covers a single guest
         * function from entry to return rather than concatenating five of them
         * into one trace file. */
        run_result rr = run_entry_arg("entry", only_entry, 0, g_budget);
        report_run(&rr);
    } else if (do_init_array) {
        if (!g_img.init_array) {
            host_log("no DT_INIT_ARRAY in the image\n");
        } else {
            host_log("running %u static constructors\n", g_img.init_array_n);
            for (uint32_t i = 0; i < g_img.init_array_n; i++) {
                /* The constructors are C++ global ctors emitted by GCC for
                 * ARMv5TE, so they are Thumb functions and arrive without the
                 * Thumb bit set in the table -- set it. A NULL entry is
                 * padding, not a constructor: see the boot sequence above. */
                uint32_t raw = LD32(g_img.init_array + i * 4);
                if (!raw)
                    continue;
                uint32_t fn = raw | 1u;
                char label[32];
                /* Labels are built by hand: snprintf into a host buffer is
                 * fine here, this is not guest code. */
                label[0] = 'c'; label[1] = 't'; label[2] = 'o'; label[3] = 'r';
                label[4] = '['; label[5] = (char)('0' + (i % 10)); label[6] = ']';
                label[7] = 0;
                run_result rr = run_entry_arg(label, fn, 0, g_budget);
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
        run_result rr = run_entry_arg("JNI_OnLoad", jni, g_javavm_ptr, g_budget);
        report_run(&rr);
    }

    if (trace_path) {
        g_trace_insn_on = 0;
        host_trace_close();
        host_log("\ninstruction trace written to %s\n", trace_path);
    }
    if (dump_path)
        dump_guest(dump_path);   /* after the run, so it shows the end state */

    spike_finish();
    return 0;
}

#ifdef WASM_FREESTANDING
/* The driver's entry point.
 *
 * clang renames main() to __main_argc_argv when targeting wasm (the real entry
 * would be _start), and that name is a compiler detail rather than an
 * interface. This wrapper gives the JS driver a stable name to call. */
int spike_main(void)
{
    return main(0, 0);
}
#endif