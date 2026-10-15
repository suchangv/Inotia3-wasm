/*
 * jni.c -- the JNI environment, materialised inside the guest address space.
 *
 * Shape of the thing:
 *
 *   guest memory
 *   ┌──────────────────────────────────────────────┐
 *   │ JNIEnv        { ptr -> table }               │   what the guest gets
 *   │ JNINativeInterface[4 + 229]  all BKPT stubs  │   indexed by offset
 *   │ JavaVM        { ptr -> table }               │
 *   │ JNIInvokeInterface[4 + 5]    all BKPT stubs  │
 *   └──────────────────────────────────────────────┘
 *
 * The guest calls a JNI function by loading a slot from one of those tables and
 * branching to it, so the only thing that has to be true is that the slot holds
 * a Thumb address the interpreter recognises. A BKPT stub is exactly that, and
 * the existing import machinery already derives an index from a stub address
 * and counts the crossing.
 *
 * Fake Java objects -- jclass, jobject, jmethodID -- are small integer handles
 * rather than pointers, because the guest treats them as opaque and never
 * dereferences them. Only strings and arrays have to be real guest memory,
 * since the guest reads and writes their contents directly.
 */

#include "jni.h"
#include "jni_offsets.h"
#include "guest.h"
#include "cpu.h"
#include "imports.h"
#include "compat.h"

/* ---- guest layout -------------------------------------------------------- */

#define JNIENV_STRUCT (GUEST_JNI_BASE + 0x000u)
#define JNIENV_TABLE  (GUEST_JNI_BASE + 0x010u)
#define JAVAVM_STRUCT (GUEST_JNI_BASE + 0x400u)
#define JAVAVM_TABLE  (GUEST_JNI_BASE + 0x410u)

int g_jni_verbose;
uint32_t g_jnienv_ptr;
uint32_t g_javavm_ptr;

/* ---- handles -------------------------------------------------------------
 *
 * Handles are `JNI_HANDLE_BASE | index`. The base is high enough that a
 * truncated or default-initialised handle cannot be mistaken for a valid one,
 * and the low bits are an index rather than a rotated hash so that a handle
 * printed in hex reads directly as its slot.
 */
#define JNI_HANDLE_BASE 0x40000000u
#define JNI_MAX_HANDLES 512

enum {
    H_FREE = 0, H_CLASS, H_OBJECT, H_METHOD, H_FIELD, H_STRING, H_ARRAY,
};

typedef struct {
    int      kind;
    char     cls[96];    /* for methods: the class it was resolved against */
    char     name[96];
    char     sig[96];
    uint32_t guest;      /* strings/arrays: guest address of the payload */
    uint32_t len;        /* payload length in elements or bytes */
    uint32_t elem;       /* arrays: element size */
} jni_handle;

static jni_handle g_handles[JNI_MAX_HANDLES];
static int        g_nhandles;

static uint32_t handle_new(int kind)
{
    for (int i = 1; i < JNI_MAX_HANDLES; i++) {
        if (g_handles[i].kind == H_FREE) {
            memset(&g_handles[i], 0, sizeof(g_handles[i]));
            g_handles[i].kind = kind;
            if (i >= g_nhandles)
                g_nhandles = i + 1;
            return JNI_HANDLE_BASE | (uint32_t)i;
        }
    }
    host_fatal("jni: out of handles (%d)\n", JNI_MAX_HANDLES);
    return 0;
}

static jni_handle *handle_of(uint32_t h)
{
    if ((h & 0xF0000000u) != JNI_HANDLE_BASE)
        return NULL;
    uint32_t i = h & 0x0FFFFFFFu;
    if (i == 0 || i >= JNI_MAX_HANDLES)
        return NULL;
    return &g_handles[i];
}

/* Copy a NUL-terminated guest string into a host buffer.
 *
 * Bounds-checked, and only ever called for arguments that are known to be
 * strings. Reading every argument as a string instead -- which is what the
 * first version did -- turns a jclass or a jmethodID into an address, and since
 * handles live at 0x40000000 that is a dereference a gigabyte outside the guest
 * space rather than a wrong answer. The guest never faults on a bad pointer, so
 * neither may the host. */
static void read_guest_str(uint32_t addr, char *out, size_t cap)
{
    size_t i = 0;
    out[0] = 0;
    if (!addr || addr >= GUEST_MEM_SIZE)
        return;
    if (cap > GUEST_MEM_SIZE - addr)
        cap = GUEST_MEM_SIZE - addr;
    for (; i + 1 < cap; i++) {
        char c = (char)g_mem[addr + i];
        out[i] = c;
        if (!c)
            return;
    }
    out[cap - 1] = 0;
}

/* ---- table construction -------------------------------------------------- */

/* Register one table entry: give it a name, get its stub, write the stub into
 * the slot. The `|1` is the interworking bit -- the guest reaches these
 * through a register branch, and the stub is Thumb code. */
static void install_slot(uint32_t table, int index, const char *name,
                         int is_vm)
{
    char full[96];
    size_t n = 0;
    const char *pfx = is_vm ? "jni/vm/" : "jni/env/";
    while (*pfx && n + 1 < sizeof(full)) full[n++] = *pfx++;
    for (const char *p = name; *p && n + 1 < sizeof(full); p++) full[n++] = *p;
    full[n] = 0;

    int idx = import_register(full, 0);
    ST32(table + (uint32_t)(index + (is_vm ? JNI_VM_RESERVED : JNI_ENV_RESERVED)) * 4u,
         g_imports[idx].stub | 1u);
}

void jni_guest_init(void)
{
    memset(g_mem + GUEST_JNI_BASE, 0, GUEST_JNI_SIZE);

    g_jnienv_ptr = JNIENV_STRUCT;
    g_javavm_ptr = JAVAVM_STRUCT;

    ST32(JNIENV_STRUCT, JNIENV_TABLE);
    ST32(JAVAVM_STRUCT, JAVAVM_TABLE);

    for (int i = 0; i < JNI_ENV_COUNT; i++)
        install_slot(JNIENV_TABLE, i, k_jni_env_names[i], 0);
    for (int i = 0; i < JNI_VM_COUNT; i++)
        install_slot(JAVAVM_TABLE, i, k_jni_vm_names[i], 1);

    /* elf_load() already stamped the ELF imports' stubs; these were registered
     * afterwards, so the stub page needs re-stamping. imports_build_stubs() is
     * idempotent -- it re-derives every stub from its index -- so running it
     * again is cheaper than threading a "register JNI first" ordering through
     * the loader. It must happen before any guest code runs. */
    imports_build_stubs();

    host_log("jni: JavaVM at %08x, JNIEnv at %08x (%d + %d table entries)\n",
             g_javavm_ptr, g_jnienv_ptr, JNI_ENV_COUNT, JNI_VM_COUNT);
}

/* ---- call counters ------------------------------------------------------- */

typedef struct {
    const char *name;
    uint64_t calls;
    int      implemented;
} jni_stat;

static jni_stat g_stats[300];
static int     g_nstats;

static jni_stat *stat_for(const char *name)
{
    for (int i = 0; i < g_nstats; i++)
        if (strcmp(g_stats[i].name, name) == 0)
            return &g_stats[i];
    if (g_nstats >= (int)(sizeof(g_stats) / sizeof(g_stats[0])))
        return &g_stats[0];
    jni_stat *s = &g_stats[g_nstats++];
    s->name = name;
    s->calls = 0;
    s->implemented = 0;
    return s;
}

/* ---- implementations ------------------------------------------------------
 *
 * A first tranche, chosen to get JNI_OnLoad through. The rest report
 * themselves on the way past, so the worklist is whatever the guest actually
 * reaches rather than a guess -- the same approach that has worked for the
 * decoder.
 */

static uint32_t do_FindClass(const char *name)
{
    uint32_t h = handle_new(H_CLASS);
    jni_handle *o = handle_of(h);
    size_t n = 0;
    while (name[n] && n < sizeof(o->cls) - 1) { o->cls[n] = name[n]; n++; }
    o->cls[n] = 0;
    /* The guest writes class names in the JNI slash form. */
    return h;
}

int jni_host_call(const char *name, uint32_t *args, int nargs, uint32_t *ret_lo,
                  uint32_t *ret_hi)
{
    if (strncmp(name, "jni/", 4) != 0)
        return 0;

    const char *fn = name + 4;
    int is_vm = strncmp(fn, "vm/", 3) == 0;
    if (is_vm)
        fn += 3;

    jni_stat *st = stat_for(name);
    st->calls++;

    /* Bring-up aid: the first few crossings, with their arguments, so a table
     * offset that is wrong by a slot shows up as "the guest called the wrong
     * function" rather than as a mysterious result. Silenced once the
     * environment is known good. */
    extern int g_jni_verbose;
    if (g_jni_verbose && st->calls <= 3)
        host_log("  [jni] %-34s arg0=%08x arg1=%08x arg2=%08x arg3=%08x\n",
                 name, nargs > 0 ? args[0] : 0, nargs > 1 ? args[1] : 0,
                 nargs > 2 ? args[2] : 0, nargs > 3 ? args[3] : 0);

    /* Argument 0 of every JNI call is the JNIEnv* (or JavaVM* for the invoke
     * interface); it is the guest's copy of our pointer and carries no
     * information we do not already have. */
    uint32_t a1 = nargs > 1 ? args[1] : 0;
    uint32_t a2 = nargs > 2 ? args[2] : 0;
    uint32_t a3 = nargs > 3 ? args[3] : 0;
    uint32_t a4 = nargs > 4 ? args[4] : 0;
    /* Strings are read at the point of use, by the functions that take one.
     * See read_guest_str(). */
    char     s1[128], s2[128], s3[128];

    /* ---- JNIInvokeInterface ---- */
    if (is_vm) {
        if (strcmp(fn, "GetEnv") == 0) {
            /* jint GetEnv(JavaVM *vm, void **penv, jint version) */
            if (a1)
                ST32(a1, g_jnienv_ptr);
            *ret_lo = JNI_OK;
            st->implemented = 1;
            return 1;
        }
        if (strcmp(fn, "AttachCurrentThread") == 0) {
            if (a1)
                ST32(a1, g_jnienv_ptr);
            *ret_lo = JNI_OK;
            st->implemented = 1;
            return 1;
        }
        if (strcmp(fn, "DetachCurrentThread") == 0 ||
            strcmp(fn, "DestroyJavaVM") == 0) {
            *ret_lo = JNI_OK;
            st->implemented = 1;
            return 1;
        }
        *ret_lo = JNI_OK;
        return 1;
    }

    /* ---- JNINativeInterface ---- */
    if (strcmp(fn, "GetVersion") == 0) {
        *ret_lo = JNI_VERSION_1_6;
        st->implemented = 1;
        return 1;
    }
    if (strcmp(fn, "FindClass") == 0) {
        read_guest_str(a1, s1, sizeof(s1));
        *ret_lo = do_FindClass(s1);
        st->implemented = 1;
        return 1;
    }
    if (strcmp(fn, "GetObjectClass") == 0 ||
        strcmp(fn, "GetSuperclass") == 0) {
        *ret_lo = do_FindClass("java/lang/Object");
        st->implemented = 1;
        return 1;
    }
    if (strcmp(fn, "GetMethodID") == 0 || strcmp(fn, "GetStaticMethodID") == 0) {
        jni_handle *c = handle_of(a1);
        uint32_t h = handle_new(H_METHOD);
        jni_handle *m = handle_of(h);
        read_guest_str(a2, s2, sizeof(s2));
        read_guest_str(a3, s3, sizeof(s3));
        if (c) memcpy(m->cls, c->cls, sizeof(m->cls));
        size_t n = 0;
        while (s2[n] && n < sizeof(m->name) - 1) { m->name[n] = s2[n]; n++; }
        n = 0;
        while (s3[n] && n < sizeof(m->sig) - 1) { m->sig[n] = s3[n]; n++; }
        *ret_lo = h;
        st->implemented = 1;
        return 1;
    }
    if (strcmp(fn, "GetFieldID") == 0 || strcmp(fn, "GetStaticFieldID") == 0) {
        jni_handle *c = handle_of(a1);
        uint32_t h = handle_new(H_FIELD);
        jni_handle *f = handle_of(h);
        read_guest_str(a2, s2, sizeof(s2));
        read_guest_str(a3, s3, sizeof(s3));
        if (c) memcpy(f->cls, c->cls, sizeof(f->cls));
        size_t n = 0;
        while (s2[n] && n < sizeof(f->name) - 1) { f->name[n] = s2[n]; n++; }
        n = 0;
        while (s3[n] && n < sizeof(f->sig) - 1) { f->sig[n] = s3[n]; n++; }
        *ret_lo = h;
        st->implemented = 1;
        return 1;
    }
    if (strcmp(fn, "NewStringUTF") == 0) {
        /* The returned jstring is opaque, but GetStringUTFChars has to hand
         * back memory the guest can read, so the bytes are copied into the
         * guest heap up front and the handle remembers where. */
        read_guest_str(a1, s1, sizeof(s1));
        size_t len = 0;
        while (s1[len] && len < sizeof(s1) - 1) len++;
        uint32_t buf = guest_heap_alloc((uint32_t)len + 1);
        memcpy(g_mem + buf, s1, len + 1);
        uint32_t h = handle_new(H_STRING);
        jni_handle *o = handle_of(h);
        o->guest = buf;
        o->len = (uint32_t)len;
        *ret_lo = h;
        st->implemented = 1;
        return 1;
    }
    if (strcmp(fn, "GetStringUTFChars") == 0 ||
        strcmp(fn, "GetStringChars") == 0) {
        jni_handle *o = handle_of(a1);
        if (a2)          /* jboolean *isCopy */
            ST8(a2, 0);
        *ret_lo = o ? o->guest : 0;
        st->implemented = 1;
        return 1;
    }
    if (strcmp(fn, "ReleaseStringUTFChars") == 0 ||
        strcmp(fn, "ReleaseStringChars") == 0) {
        st->implemented = 1;
        return 1;
    }
    if (strcmp(fn, "GetStringUTFLength") == 0 ||
        strcmp(fn, "GetStringLength") == 0) {
        jni_handle *o = handle_of(a1);
        *ret_lo = o ? o->len : 0;
        st->implemented = 1;
        return 1;
    }
    if (strcmp(fn, "NewIntArray") == 0 || strcmp(fn, "NewByteArray") == 0 ||
        strcmp(fn, "NewShortArray") == 0 || strcmp(fn, "NewFloatArray") == 0) {
        uint32_t elem = 4;
        if (strcmp(fn, "NewByteArray") == 0) elem = 1;
        else if (strcmp(fn, "NewShortArray") == 0) elem = 2;
        uint32_t buf = guest_heap_alloc(a1 * elem + 4);
        memset(g_mem + buf, 0, a1 * elem + 4);
        uint32_t h = handle_new(H_ARRAY);
        jni_handle *o = handle_of(h);
        o->guest = buf;
        o->len = a1;
        o->elem = elem;
        *ret_lo = h;
        st->implemented = 1;
        return 1;
    }
    if (strcmp(fn, "GetArrayLength") == 0) {
        jni_handle *o = handle_of(a1);
        *ret_lo = o ? o->len : 0;
        st->implemented = 1;
        return 1;
    }
    if (strcmp(fn, "GetIntArrayElements") == 0 ||
        strcmp(fn, "GetByteArrayElements") == 0 ||
        strcmp(fn, "GetShortArrayElements") == 0 ||
        strcmp(fn, "GetFloatArrayElements") == 0) {
        jni_handle *o = handle_of(a1);
        if (a2)
            ST8(a2, 0);
        *ret_lo = o ? o->guest : 0;
        st->implemented = 1;
        return 1;
    }
    if (strcmp(fn, "ReleaseIntArrayElements") == 0 ||
        strcmp(fn, "ReleaseByteArrayElements") == 0 ||
        strcmp(fn, "ReleaseShortArrayElements") == 0 ||
        strcmp(fn, "ReleaseFloatArrayElements") == 0) {
        st->implemented = 1;
        return 1;
    }
    if (strcmp(fn, "GetIntArrayRegion") == 0) {
        jni_handle *o = handle_of(a1);
        if (o && a2)
            memcpy(g_mem + a2, g_mem + o->guest + a3 * 4, a4 * 4);
        st->implemented = 1;
        return 1;
    }
    if (strcmp(fn, "SetIntArrayRegion") == 0) {
        jni_handle *o = handle_of(a1);
        if (o && a2)
            memcpy(g_mem + o->guest + a3 * 4, g_mem + a2, a4 * 4);
        st->implemented = 1;
        return 1;
    }
    if (strcmp(fn, "DeleteLocalRef") == 0 ||
        strcmp(fn, "DeleteGlobalRef") == 0) {
        jni_handle *o = handle_of(a1);
        if (o)
            o->kind = H_FREE;
        st->implemented = 1;
        return 1;
    }
    if (strncmp(fn, "Exception", 9) == 0) {
        /* Nothing in the guest throws across this boundary; reporting "no
         * pending exception" is both true and what the guest expects. */
        if (strcmp(fn, "ExceptionOccurred") == 0)
            *ret_lo = 0;
        st->implemented = 1;
        return 1;
    }
    if (strcmp(fn, "IsSameObject") == 0) {
        *ret_lo = (a1 == a2);
        st->implemented = 1;
        return 1;
    }
    if (strcmp(fn, "GetJavaVM") == 0) {
        if (a1)
            ST32(a1, g_javavm_ptr);
        st->implemented = 1;
        return 1;
    }
    if (strcmp(fn, "RegisterNatives") == 0) {
        /* The guest is registering its own native methods with a VM that will
         * never call them: on Android it is the Java side that invokes them,
         * and here the loader drives those entry points directly. Succeeding
         * without recording anything is the correct behaviour, not a stub. */
        st->implemented = 1;
        return 1;
    }

    /* Not implemented. Return a null/zero result and let the report name it --
     * a wrong-looking zero is much cheaper to diagnose when the report says
     * which function produced it. */
    *ret_lo = 0;
    return 1;
}

uint32_t jni_host_new_int_array(int n)
{
    uint32_t buf = guest_heap_alloc((uint32_t)n * 4u);
    memset(g_mem + buf, 0, (size_t)n * 4u);
    uint32_t h = handle_new(H_ARRAY);
    jni_handle *o = handle_of(h);
    o->guest = buf;
    o->len = (uint32_t)n;
    o->elem = 4;
    return h;
}

void jni_report(int top_n)
{
    uint64_t total = 0;
    int      n_missing = 0;
    for (int i = 0; i < g_nstats; i++) {
        total += g_stats[i].calls;
        if (!g_stats[i].implemented && g_stats[i].calls)
            n_missing++;
    }
    if (!g_nstats) {
        host_log("\n--- JNI: nothing called ---\n");
        return;
    }

    host_log("\n--- JNI calls (%llu total across %d functions) ---\n",
             (unsigned long long)total, g_nstats);

    /* Selection over a copy of the indices, largest first. The table is a few
     * hundred entries and this runs once, so an O(n^2) scan beats carrying an
     * index array through the sort. */
    int order[300];
    int n = g_nstats < 300 ? g_nstats : 300;
    for (int i = 0; i < n; i++)
        order[i] = i;
    for (int i = 0; i < n; i++)
        for (int j = i + 1; j < n; j++)
            if (g_stats[order[j]].calls > g_stats[order[i]].calls) {
                int t = order[i]; order[i] = order[j]; order[j] = t;
            }

    int shown = 0;
    for (int i = 0; i < n && shown < top_n; i++) {
        const jni_stat *st = &g_stats[order[i]];
        if (!st->calls)
            break;
        host_log("  %-34s %8llu  %s\n", st->name,
                 (unsigned long long)st->calls,
                 st->implemented ? "" : "<-- NOT IMPLEMENTED");
        shown++;
    }

    if (n_missing) {
        host_log("\n--- %d JNI functions called but not implemented "
                 "(the worklist) ---\n", n_missing);
        for (int i = 0; i < n; i++)
            if (!g_stats[i].implemented && g_stats[i].calls)
                host_log("  %-34s %8llu\n", g_stats[i].name,
                         (unsigned long long)g_stats[i].calls);
    }
}
