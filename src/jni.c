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
#include "timers.h"
#include "asset_zip.h"
#include "platform.h"
#include "text.h"

/* The archive the engine reads its assets out of, and where inside them they
 * live. The archive's *path* comes from host_apk_path() -- see asset_zip.c --
 * because which of the candidate locations exists varies by build. */
#define ASSET_PREFIX  "assets/common/game_res/"
#define ASSET_SUFFIX  ".jpg"

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
    int      refs;       /* JNI references outstanding: local + global */
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
            g_handles[i].refs = 1;
            if (i >= g_nhandles)
                g_nhandles = i + 1;
            return JNI_HANDLE_BASE | (uint32_t)i;
        }
    }
    host_fatal("jni: out of handles (%d)\n", JNI_MAX_HANDLES);
    return 0;
}

/* Reuse an existing handle for the same thing, or 0.
 *
 * JNI guarantees that the methodID for a given (class, name, signature) is the
 * same id every time, and the engine leans on that: it resolves the same
 * handful of methods on every frame, thousands of times over a run. Minting a
 * fresh handle per call fills the 512-entry table, and then the guest gets a
 * null methodID -- which is what "the game stops on some screen" turned out to
 * be. Any field left NULL is not matched on. */
static uint32_t handle_find(int kind, const char *cls, const char *name,
                            const char *sig)
{
    for (int i = 1; i < JNI_MAX_HANDLES; i++) {
        jni_handle *o = &g_handles[i];
        if (o->kind != kind)
            continue;
        if (cls && strcmp(o->cls, cls))
            continue;
        if (name && strcmp(o->name, name))
            continue;
        if (sig && strcmp(o->sig, sig))
            continue;
        return JNI_HANDLE_BASE | (uint32_t)i;
    }
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

/* The text methods take the string either as a jstring handle (the engine
 * makes one with NewStringUTF immediately before the call) or as a byte[]
 * handle (the Draw* variants). Both are payloads in the guest heap, so this
 * resolves either shape to a C string. A byte[] payload is not NUL-terminated
 * by contract, hence the explicit terminator. TEXT_MAX truncates rather than
 * faults; a rectangle this engine draws in never holds more than a few hundred
 * bytes, and measurement only needs what the wrap will walk. */
#define TEXT_MAX 1024

static void jni_read_text_arg(uint32_t arg, char *out, size_t cap)
{
    jni_handle *h = handle_of(arg);
    if (h && (h->kind == H_STRING || h->kind == H_ARRAY) && h->guest) {
        size_t n = h->len;
        if (n > cap - 1)
            n = cap - 1;
        memcpy(out, g_mem + h->guest, n);
        out[n] = 0;
        return;
    }
    read_guest_str(arg, out, cap);
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

/* Called Java methods, keyed by "Class.method" rather than by the JNI entry
 * point that invoked them.
 *
 * The distinction matters: "CallStaticVoidMethod was called 400 times" says
 * nothing about what the host has to implement, whereas
 * "WrapperJinterface.SetTimer" is a work item. The guest reaches the Java side
 * only through these, so this list *is* the specification for the Java
 * semantics the port needs -- the same role the import table plays for libc. */
typedef struct { char key[160]; uint64_t calls; } jni_method_stat;
static jni_method_stat g_methods[200];
static int             g_nmethods;

static void note_method(uint32_t method_handle)
{
    jni_handle *m = handle_of(method_handle);
    if (!m || (m->kind != H_METHOD && m->kind != H_FIELD))
        return;
    const char *cls = m->cls;
    /* Strip the JNI slash form's leading L and trailing ; if present, so the
     * key reads as a name rather than as a type descriptor. */
    if (cls[0] == 'L') cls++;
    char key[160];
    size_t n = 0;
    for (const char *p = cls; *p && *p != ';' && n + 1 < sizeof(key); p++)
        key[n++] = *p;
    if (n + 1 < sizeof(key)) key[n++] = '.';
    for (const char *p = m->name; *p && n + 1 < sizeof(key); p++)
        key[n++] = *p;
    key[n] = 0;

    for (int i = 0; i < g_nmethods; i++)
        if (strcmp(g_methods[i].key, key) == 0) { g_methods[i].calls++; return; }
    if (g_nmethods < (int)(sizeof(g_methods) / sizeof(g_methods[0]))) {
        size_t i = 0;
        for (; key[i] && i < sizeof(g_methods[0].key) - 1; i++)
            g_methods[g_nmethods].key[i] = key[i];
        g_methods[g_nmethods].key[i] = 0;
        g_methods[g_nmethods].calls = 1;
        g_nmethods++;
    }
}

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
    uint32_t found = handle_find(H_CLASS, name, NULL, NULL);
    if (found)
        return found;
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

    /* Names are "jni/env/Foo" and "jni/vm/Foo"; both prefixes have to come off
     * before the comparison below. Stripping only "vm/" left every
     * JNINativeInterface entry named "env/Foo", matching no case, falling
     * through to the unimplemented tail -- while the report blamed the
     * functions for not being implemented rather than the dispatcher for not
     * routing them. */
    const char *fn = name + 4;
    int is_vm = strncmp(fn, "vm/", 3) == 0;
    if (is_vm)
        fn += 3;
    else if (strncmp(fn, "env/", 4) == 0)
        fn += 4;

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
        read_guest_str(a2, s2, sizeof(s2));
        read_guest_str(a3, s3, sizeof(s3));
        if (c) {
            uint32_t found = handle_find(H_METHOD, c->cls, s2, s3);
            if (found) {
                *ret_lo = found;
                st->implemented = 1;
                return 1;
            }
        }
        uint32_t h = handle_new(H_METHOD);
        jni_handle *m = handle_of(h);
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
        read_guest_str(a2, s2, sizeof(s2));
        read_guest_str(a3, s3, sizeof(s3));
        if (c) {
            uint32_t found = handle_find(H_FIELD, c->cls, s2, s3);
            if (found) {
                *ret_lo = found;
                st->implemented = 1;
                return 1;
            }
        }
        uint32_t h = handle_new(H_FIELD);
        jni_handle *f = handle_of(h);
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
        strcmp(fn, "NewShortArray") == 0 || strcmp(fn, "NewFloatArray") == 0 ||
        strcmp(fn, "NewLongArray") == 0 || strcmp(fn, "NewDoubleArray") == 0 ||
        strcmp(fn, "NewCharArray") == 0 || strcmp(fn, "NewBooleanArray") == 0) {
        /* Element size by type. Getting this wrong does not fault -- the
         * payload is flat guest memory and the guest indexes it itself -- it
         * just makes the guest read past what was allocated, or read a
         * neighbouring element, so it is worth being explicit. */
        int elem = 4;
        if (strcmp(fn, "NewByteArray") == 0 ||
            strcmp(fn, "NewBooleanArray") == 0)      elem = 1;
        else if (strcmp(fn, "NewShortArray") == 0 ||
                 strcmp(fn, "NewCharArray") == 0)        elem = 2;
        else if (strcmp(fn, "NewLongArray") == 0 ||
                 strcmp(fn, "NewDoubleArray") == 0)      elem = 8;
        *ret_lo = jni_host_new_array(elem, (int)a1);
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
        strcmp(fn, "GetFloatArrayElements") == 0 ||
        strcmp(fn, "GetLongArrayElements") == 0 ||
        strcmp(fn, "GetDoubleArrayElements") == 0 ||
        strcmp(fn, "GetCharArrayElements") == 0 ||
        strcmp(fn, "GetBooleanArrayElements") == 0) {
        /* Hands back the payload itself, not a copy: every caller in this
         * guest writes through the pointer expecting Java to see it, and there
         * is no Java. isCopy is reported false accordingly. */
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
        strcmp(fn, "ReleaseFloatArrayElements") == 0 ||
        strcmp(fn, "ReleaseLongArrayElements") == 0 ||
        strcmp(fn, "ReleaseDoubleArrayElements") == 0 ||
        strcmp(fn, "ReleaseCharArrayElements") == 0 ||
        strcmp(fn, "ReleaseBooleanArrayElements") == 0) {
        /* Nothing to commit: Get*ArrayElements handed out the payload. */
        st->implemented = 1;
        return 1;
    }
    if (strncmp(fn, "Get", 3) == 0 && strstr(fn, "ArrayRegion")) {
        /* jint Get<T>ArrayRegion(env, array, start, len, T *buf): copies out
         * of the array into guest memory. The element size comes from the
         * handle rather than the function name, so this covers every element
         * type with one body.
         *
         * The JNI order is (env, array, start, len, buf) -- args[2] is the
         * start, args[3] the length and args[4] the destination. Reading it
         * any other way silently copies nothing (or from the wrong place),
         * which is how the engine's text string arrived as zeros. */
        jni_handle *o = handle_of(a1);
        uint32_t start = a2, len = a3, buf = a4;
        if (o && buf && o->elem && start + len <= o->len) {
            uint32_t n = len * o->elem;
            memcpy(g_mem + buf, g_mem + o->guest + start * o->elem, n);
        }
        st->implemented = 1;
        return 1;
    }
    if (strncmp(fn, "Set", 3) == 0 && strstr(fn, "ArrayRegion")) {
        /* jint Set<T>ArrayRegion(env, array, start, len, const T *buf) --
         * same order, opposite direction. jDrawTextInRect fills its byte[]
         * through this, so it is on the text path. */
        jni_handle *o = handle_of(a1);
        uint32_t start = a2, len = a3, buf = a4;
        if (o && buf && o->elem && start + len <= o->len) {
            uint32_t n = len * o->elem;
            memcpy(g_mem + o->guest + start * o->elem, g_mem + buf, n);
        }
        st->implemented = 1;
        return 1;
    }
    if (strcmp(fn, "NewGlobalRef") == 0 ||
        strcmp(fn, "NewLocalRef") == 0) {
        /* Handles have no local/global distinction here: the same handle is
         * returned with one more reference on it. Keeping the count is what
         * makes the engine's `NewIntArray(); NewGlobalRef(); DeleteLocalRef()`
         * cache idiom work -- without it the delete freed the payload and the
         * slot came back as something else (a methodID, in the text-measurement
         * case), so the engine's cached array handle silently stopped being an
         * array and every measurement came back as (0,0). */
        jni_handle *o = handle_of(a1);
        if (o)
            o->refs++;
        *ret_lo = a1;
        st->implemented = 1;
        return 1;
    }
    if (strcmp(fn, "DeleteLocalRef") == 0 ||
        strcmp(fn, "DeleteGlobalRef") == 0) {
        /* Deleting one reference must not invalidate the object while another
         * reference (typically a cached global ref) still names it. JNI counts
         * references, not objects; so does this table now. */
        jni_handle *o = handle_of(a1);
        if (o && --o->refs <= 0)
            o->kind = H_FREE;
        st->implemented = 1;
        return 1;
    }
    /* Any Call*Method / Call*MethodV / Call*MethodA names the Java method in
     * its second argument; Get*Field names a field. Recording them here is what
     * produces the worklist. */
    if ((strncmp(fn, "Call", 4) == 0 || strncmp(fn, "Get", 3) == 0) &&
        (strstr(fn, "Method") || strstr(fn, "Field"))) {
        note_method(a2);

        /* The handful of Java methods that have to return something real.
         *
         * Everything else answers zero, which is fine for a getter nothing acts
         * on -- but not for these. GetUserDir and GetPackageName are the inputs
         * to every resource path the engine builds: with them empty, mode 0 and
         * mode 3 produce "/sdcard/Android/data//sdcard/Android/data/%s/files"
         * and the engine can never find a file. They are also the reason the
         * path formatting shows literal %s -- the format string is fine, its
         * arguments are not.
         *
         * The values are the ones this build actually shipped with; the package
         * name comes from the APK, and the Vita port records the same one
         * ("com.com2us.inotia3...google.global"). */
        {
            jni_handle *m = handle_of(a2);
            if (m && m->kind == H_METHOD) {
                const char *n = m->name;
                if (strcmp(n, "GetPackageName") == 0) {
                    *ret_lo = jni_host_new_string("com.com2us.inotia3.google.global");
                    st->implemented = 1; return 1;
                }
                if (strcmp(n, "GetUserDir") == 0) {
                    /* Where the engine installs its files and puts saves. The
                     * trailing slash is load-bearing: the engine concatenates
                     * the package name straight onto it, so "/data" gives
                     * "/datacom.com2us...", every GAMELOADER_Install fails, and
                     * the game parks on "File installation error (0)" forever
                     * (INITSTATE_nState==3, a terminal state). The Vita port
                     * returns "ux0:data/" for exactly this reason. */
                    *ret_lo = jni_host_new_string(host_app_data_dir("com.com2us.inotia3.google.global"));
                    st->implemented = 1; return 1;
                }
                if (strcmp(n, "GetApkFilePath") == 0 ||
                    strcmp(n, "GetBundlePath") == 0 ||
                    strcmp(n, "GetResourcePath") == 0) {
                    /* The engine reads its assets out of this file at byte
                     * offsets (mode 2's "@offset|length|path"), so it has to
                     * be the real archive, not a directory. One constant path
                     * for every asset is exactly what Android does and what
                     * makes jGetApkFilePath's own caching harmless. The path
                     * must also be one this host can open: File::Open's next
                     * fopen is on the bare path, after
                     * ParseOffsetAndLengthAndPath has stripped the "@off|len|"
                     * prefix, so an openable path here is what makes that
                     * fopen succeed. */
                    *ret_lo = jni_host_new_string(host_apk_path());
                    st->implemented = 1; return 1;
                }
                if (strcmp(n, "GetFileDescriptor") == 0) {
                    /* GetFileDescriptor(String name, int[] values,
                     *                    boolean closeFDdirectly)
                     *
                     * This is Android's AssetManager.openFd(): fill values[0]
                     * with the offset and values[1] with the length of the
                     * asset inside the archive, and return the descriptor. The
                     * engine reads those two and mmaps the archive at that
                     * offset -- which is why the archive has to be the real
                     * .apk and the entries have to be STORED.
                     *
                     * A missing asset is reported as {-1, -1}, which is what
                     * the Java side does; the engine checks for it. */
                    /* The name arrives as a jstring *handle*, not a guest
                     * string pointer -- every jstring the engine holds came
                     * from NewStringUTF, and a handle is the only thing that
                     * identifies it. Reading it as a pointer gives an empty
                     * string, which looks exactly like "the asset is missing".
                     * See jni_handle: strings keep their payload address. */
                    char name[192];
                    name[0] = 0;
                    {
                        jni_handle *sh = handle_of(a3);
                        if (sh && sh->kind == H_STRING && sh->guest)
                            read_guest_str(sh->guest, name, sizeof(name));
                        else
                            read_guest_str(a3, name, sizeof(name));
                    }
                    uint32_t values = args[4];
                    char entry[256];
                    int off = 0;
                    const char *p = ASSET_PREFIX;
                    while (*p && off < (int)sizeof(entry) - 1) entry[off++] = *p++;
                    for (const char *q = name; *q && off < (int)sizeof(entry) - 40; q++)
                        entry[off++] = *q;
                    p = ASSET_SUFFIX;
                    while (*p && off < (int)sizeof(entry) - 1) entry[off++] = *p++;
                    entry[off] = 0;

#ifdef WASM_FREESTANDING
                    int found = 1;   /* no filesystem: nothing to look up */
                    uint32_t asset_off = 0, asset_len = 0;
#else
                    uint32_t asset_off = 0, asset_len = 0;
                    /* One place decides where the archive is; see asset_zip.c. */
                    FILE *apk = fopen(host_apk_path(), "rb");
                    int found = apk ? asset_zip_find(apk, entry, &asset_off,
                                                     &asset_len) : 1;
                    if (apk)
                        fclose(apk);
#endif

                    static int logged;
                    if (logged < 20) {
                        logged++;
                        host_log("  [assets] %-40s %s%s caller=%08x\n", entry,
                                 found ? "NOT FOUND" : "offset",
                                 found ? "" : "", g_cpu.r[14]);
                        if (!found)
                            host_log("           offset=%u length=%u\n",
                                     asset_off, asset_len);
                    }

                    /* `values` is a jintArray HANDLE, not a guest pointer. Same
                     * mistake as the `name` argument two lines up, and it fails
                     * the same silent way: the handle is 0x400000xx, which is
                     * above GUEST_MEM_SIZE, so the range check rejected it and
                     * nothing was ever written. The engine then read {0, 0} as
                     * the asset's offset and length, opened nothing, and drew a
                     * black frame -- with every function on the path reporting
                     * that it had run.
                     *
                     * The handle resolves to the array's payload, which is real
                     * guest memory (see jni_handle, and jni_host_new_array). */
                    uint32_t payload = 0;
                    {
                        jni_handle *ah = handle_of(values);
                        if (ah && ah->kind == H_ARRAY)
                            payload = ah->guest;
                    }
                    if (payload && payload < GUEST_MEM_SIZE - 8) {
                        ST32(payload, found ? 0xFFFFFFFFu : asset_off);
                        ST32(payload + 4, found ? 0xFFFFFFFFu : asset_len);
                        static int n;
                        if (n < 6) {
                            n++;
                            host_log("  [assets] values[] <- offset=%u length=%u\n",
                                     found ? 0xFFFFFFFFu : asset_off,
                                     found ? 0xFFFFFFFFu : asset_len);
                        }
                    } else {
                        host_log("  [assets] cannot resolve the values array "
                                 "(handle %08x)\n", values);
                    }
                    *ret_lo = 0;
                    st->implemented = 1;
                    return 1;
                }
                if (strcmp(n, "GetLanguage") == 0) {
                    *ret_lo = jni_host_new_string("en");
                    st->implemented = 1; return 1;
                }
                if (strcmp(n, "GetCountry") == 0) {
                    *ret_lo = jni_host_new_string("US");
                    st->implemented = 1; return 1;
                }
                if (strcmp(n, "GetUDID") == 0 || strcmp(n, "GetCom2usUDID") == 0) {
                    *ret_lo = jni_host_new_string("0000000000000000");
                    st->implemented = 1; return 1;
                }
                if (strcmp(n, "GetPhoneNumber") == 0) {
                    *ret_lo = jni_host_new_string("");
                    st->implemented = 1; return 1;
                }
                if (strcmp(n, "GetRealScreenWidth") == 0 ||
                    strcmp(n, "GetScreenWidth") == 0) {
                    *ret_lo = 960;
                    st->implemented = 1; return 1;
                }
                if (strcmp(n, "GetRealScreenHeight") == 0 ||
                    strcmp(n, "GetScreenHeight") == 0) {
                    *ret_lo = 544;
                    st->implemented = 1; return 1;
                }
                if (strcmp(n, "GetSystemCurrentTime") == 0) {
                    /* This must be the SAME clock the engine schedules its
                     * timers against, and that clock is the sysTime[] array
                     * handed to nativeInit() -- WrapperRenderer computes the
                     * one from the other, so on Android they are identical.
                     *
                     * Returning host_now() instead made them two different
                     * clocks: SetTimer registered firetime=6915768 (measured in
                     * this one) while the frame loop advanced the array from
                     * near zero, so no timer was ever due and the engine sat at
                     * "timers=0" for the whole run. */
                    extern uint32_t g_sys_time_addr;
                    if (g_sys_time_addr) {
                        *ret_lo = LD32(g_sys_time_addr);
                    } else {
                        *ret_lo = (uint32_t)(host_now() * 1000.0);
                    }
                    st->implemented = 1; return 1;
                }
                if (strcmp(n, "GetAppProperty") == 0) {
                    *ret_lo = jni_host_new_string("");
                    st->implemented = 1; return 1;
                }

                /* ---- the text rasteriser --------------------------------
                 *
                 * The engine does not draw text itself: SGL_Font_Android hands
                 * the string to Java as a byte[]/String and reads the result
                 * back as an alpha mask out of the pixel[] buffer nativeInit()
                 * was given (see text.c). All four of these were answering
                 * zero, which is why every dialogue box, menu label and item
                 * name in the port came up empty.
                 *
                 * Argument indexing is this file's usual one: args[0..2] are
                 * env/clazz/methodID, so the Java varargs start at args[3] and
                 * follow the literal signatures from the .so's .rodata --
                 * DrawText "([BII)I", DrawTextInRect "([BIIIII)I",
                 * GetStringWidthHeightInRect "(Ljava/lang/String;IIII[I)I",
                 * GetStringWidth "(Ljava/lang/String;II)I". */
                if (strcmp(n, "GetStringWidthHeightInRect") == 0) {
                    char text[TEXT_MAX];
                    jni_read_text_arg(args[3], text, sizeof(text));
                    int w = 0, h = 0;
                    text_measure_width_height(text, (int) args[6], &w, &h);
                    if (g_jni_verbose) {
                        static int nn;
                        if (nn < 12) {
                            nn++;
                            host_log("  [text] measure len=%d args=%d,%d,%d,%d "
                                     "-> w=%d h=%d \"%s\"\n", (int) strlen(text),
                                     (int) args[4], (int) args[5], (int) args[6],
                                     (int) args[7], w, h, text);
                        }
                    }
                    /* values[0]=width, values[1]=height: the engine sizes the
                     * alpha texture it is about to read from these. */
                    jni_handle *ah = handle_of(args[8]);
                    if (ah && ah->kind == H_ARRAY && ah->guest) {
                        ST32(ah->guest, (uint32_t) w);
                        ST32(ah->guest + 4, (uint32_t) h);
                    }
                    *ret_lo = (uint32_t) w;
                    st->implemented = 1; return 1;
                }
                if (strcmp(n, "GetStringWidth") == 0) {
                    char text[TEXT_MAX];
                    jni_read_text_arg(args[3], text, sizeof(text));
                    *ret_lo = (uint32_t) text_measure_width(text);
                    st->implemented = 1; return 1;
                }
                if (strcmp(n, "DrawTextInRect") == 0) {
                    jni_handle *bh = handle_of(args[3]);
                    if (bh && bh->kind == H_ARRAY && bh->guest && bh->elem == 1)
                        text_draw_in_rect(g_mem + bh->guest, (int) bh->len,
                                          (int) args[6], (int) args[7]);
                    *ret_lo = 0;
                    st->implemented = 1; return 1;
                }
                if (strcmp(n, "DrawText") == 0) {
                    jni_handle *bh = handle_of(args[3]);
                    if (bh && bh->kind == H_ARRAY && bh->guest && bh->elem == 1)
                        text_draw(g_mem + bh->guest, (int) bh->len, (int) args[5]);
                    *ret_lo = 0;
                    st->implemented = 1; return 1;
                }
            }
        }

        /* The timer table is the one other Java-side facility the engine cannot
         * run without: it is how the game loop schedules itself (see timers.h).
         *
         * CallStaticVoidMethod(env, clazz, methodID, id, firetime, interval):
         * the varargs start at args[3], and each jlong occupies two words. */
        {
            jni_handle *m = handle_of(a2);
            if (m && m->kind == H_METHOD && nargs >= 8) {
                if (strcmp(m->name, "SetTimer") == 0) {
                    timers_set(args[3],
                               (int64_t)(((uint64_t)args[5] << 32) | args[4]),
                               (int64_t)(((uint64_t)args[7] << 32) | args[6]));
                    st->implemented = 1;
                    *ret_lo = 0;
                    return 1;
                }
                if (strcmp(m->name, "UnsetTimer") == 0) {
                    timers_unset(args[3]);
                    st->implemented = 1;
                    *ret_lo = 0;
                    return 1;
                }
            }
        }
        /* The Java side is not there, so every call answers with a zero of the
         * right width. That is enough for the engine to keep running, and the
         * report says what would have to exist for it to be correct. */
        *ret_lo = 0;
        *ret_hi = 0;
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

uint32_t jni_host_new_array(int elem, int n)
{
    uint32_t bytes = (uint32_t)n * (uint32_t)elem;
    if (!bytes)
        bytes = 1;             /* a zero-length array still needs a non-NULL
                                * payload: the guest stores the pointer and may
                                * add to it before ever reading */
    uint32_t buf = guest_heap_alloc(bytes);
    memset(g_mem + buf, 0, bytes);
    uint32_t h = handle_new(H_ARRAY);
    jni_handle *o = handle_of(h);
    o->guest = buf;
    o->len = (uint32_t)n;
    o->elem = (uint32_t)elem;
    return h;
}

uint32_t jni_host_new_string(const char *s)
{
    size_t len = strlen(s);
    uint32_t buf = guest_heap_alloc((uint32_t)len + 1);
    memcpy(g_mem + buf, s, len + 1);
    uint32_t h = handle_new(H_STRING);
    jni_handle *o = handle_of(h);
    o->guest = buf;
    o->len = (uint32_t)len;
    return h;
}

uint32_t jni_host_new_int_array(int n)
{
    return jni_host_new_array(4, n);
}

uint32_t jni_host_array_addr(uint32_t handle)
{
    jni_handle *o = handle_of(handle);
    return o ? o->guest : 0;
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

    if (g_nmethods) {
        host_log("\n--- Java methods the guest calls (the real worklist) ---\n");
        int order2[200];
        for (int i = 0; i < g_nmethods; i++) order2[i] = i;
        for (int i = 0; i < g_nmethods; i++)
            for (int j = i + 1; j < g_nmethods; j++)
                if (g_methods[order2[j]].calls > g_methods[order2[i]].calls) {
                    int t = order2[i]; order2[i] = order2[j]; order2[j] = t;
                }
        for (int i = 0; i < g_nmethods; i++)
            host_log("  %-52s %8llu\n", g_methods[order2[i]].key,
                     (unsigned long long)g_methods[order2[i]].calls);
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
