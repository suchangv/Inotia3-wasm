/*
 * imports.c -- resolve libgame.so's 183 imports against host implementations,
 * and count every crossing.
 *
 * Two kinds of import, handled differently and for a real reason:
 *
 *   functions  get a 4-byte BKPT stub. A guest call through the PLT lands on
 *              the stub, the Thumb decoder sees BKPT, and the import index is
 *              derived from the stub address. No dynamic linker, and the cost
 *              of counting is one increment already on the decode path.
 *
 *   variables  (__stack_chk_guard, __sF, __errno, __page_size) get real guest
 *              storage. These are read and written directly by guest code, so
 *              pointing their GOT slot at a stub would fault on first access
 *              rather than call anything.
 */

#include "imports.h"
#include "guest.h"
#include "cpu.h"
#include "compat.h"

guest_import g_imports[IMPORT_MAX];
int          g_nimports;

/* ---- name -> implementation mapping -------------------------------------- */

typedef struct {
    const char *name;
    int         impl;
    int         is_object;
    uint32_t    obj_size;   /* for objects: bytes of guest storage to reserve */
} known_import;

/* Everything the .so actually imports (verified against its .dynsym: 183
 * undefined symbols). Entries with a NULL-ish impl are still listed so the
 * report can distinguish "we know about this and chose not to implement it"
 * from "we have never seen this symbol". */
static const known_import g_known[] = {
    /* --- memory --- */
    { "malloc",                IMPL_malloc,  0, 0 },
    { "free",                  IMPL_free,    0, 0 },
    { "calloc",                IMPL_calloc,  0, 0 },
    { "realloc",               IMPL_realloc, 0, 0 },
    { "mmap",                  IMPL_mmap,    0, 0 },
    { "munmap",                IMPL_munmap,  0, 0 },

    /* --- strings --- */
    { "memcpy",                IMPL_memcpy,  0, 0 },
    { "memset",                IMPL_memset,  0, 0 },
    { "memmove",               IMPL_memmove, 0, 0 },
    { "memcmp",                IMPL_memcmp,  0, 0 },
    { "strlen",                IMPL_strlen,  0, 0 },
    { "strcmp",                IMPL_strcmp,  0, 0 },
    { "strcpy",                IMPL_strcpy,  0, 0 },
    { "strncpy",               IMPL_strncpy, 0, 0 },
    { "strcat",                IMPL_strcat,  0, 0 },
    { "strncat",               IMPL_strncat, 0, 0 },
    { "strchr",                IMPL_strchr,  0, 0 },
    { "strrchr",               IMPL_strrchr, 0, 0 },
    { "strstr",                IMPL_strstr,  0, 0 },
    { "strncmp",               IMPL_strncmp, 0, 0 },
    { "strdup",                IMPL_strdup,  0, 0 },
    { "strerror",              IMPL_strerror,0, 0 },
    { "strtok",                IMPL_strtok,  0, 0 },
    { "strtod",                IMPL_strtod,  0, 0 },
    { "atoi",                  IMPL_atoi,    0, 0 },
    { "atoll",                 IMPL_atoll,   0, 0 },
    { "strtoll",               IMPL_strtoll, 0, 0 },
    { "qsort",                 IMPL_qsort,   0, 0 },

    /* --- runtime / diagnostics --- */
    { "abort",                 IMPL_abort,          0, 0 },
    { "__assert2",             IMPL_assert2,        0, 0 },
    { "__stack_chk_fail",      IMPL_stack_chk_fail, 0, 0 },
    { "__android_log_write",   IMPL_android_log_write, 0, 0 },
    { "__cxa_atexit",          IMPL_cxa_atexit,     0, 0 },
    { "__aeabi_atexit",        IMPL_aeabi_atexit,   0, 0 },
    { "__cxa_finalize",        IMPL_cxa_finalize,   0, 0 },
    { "__gnu_Unwind_Find_exidx", IMPL_NONE,         0, 0 },

    /* --- imported *variables* --- */
    { "__stack_chk_guard",     IMPL_NONE, 1, 4 },
    { "__sF",                  IMPL_NONE, 1, 0x200 },  /* bionic stdio array */
    { "__errno",               IMPL_errno_addr, 1, 4 },
    { "__page_size",           IMPL_NONE, 1, 4 },

    /* --- time / math --- */
    { "gettimeofday",          IMPL_gettimeofday, 0, 0 },
    { "time",                  IMPL_time,         0, 0 },
    { "nanosleep",             IMPL_nanosleep,    0, 0 },
    { "usleep",                IMPL_usleep,       0, 0 },
    { "sleep",                 IMPL_sleep,        0, 0 },
    { "sin",                   IMPL_sin,          0, 0 },
    { "cos",                   IMPL_cos,          0, 0 },
    { "pow",                   IMPL_pow,          0, 0 },
    { "__isinf",               IMPL_isinf,        0, 0 },

    /* --- threads: real on native, cooperative in the browser (see docs) --- */
    { "pthread_mutex_init",        IMPL_pthread_mutex_init,    0, 0 },
    { "pthread_mutex_lock",        IMPL_pthread_mutex_lock,    0, 0 },
    { "pthread_mutex_unlock",      IMPL_pthread_mutex_unlock,  0, 0 },
    { "pthread_mutex_trylock",     IMPL_pthread_mutex_trylock, 0, 0 },
    { "pthread_mutex_destroy",     IMPL_pthread_mutex_destroy, 0, 0 },
    { "pthread_mutexattr_init",    IMPL_pthread_mutexattr_init,    0, 0 },
    { "pthread_mutexattr_settype", IMPL_pthread_mutexattr_settype, 0, 0 },
    { "pthread_mutexattr_destroy", IMPL_pthread_mutexattr_destroy, 0, 0 },
    { "pthread_self",              IMPL_pthread_self,          0, 0 },
    { "pthread_equal",             IMPL_pthread_equal,         0, 0 },
    { "pthread_key_create",        IMPL_pthread_key_create,    0, 0 },
    { "pthread_getspecific",       IMPL_pthread_getspecific,   0, 0 },
    { "pthread_setspecific",       IMPL_pthread_setspecific,   0, 0 },
    { "pthread_create",            IMPL_pthread_create,        0, 0 },
    { "pthread_join",              IMPL_pthread_join,          0, 0 },
    { "pthread_exit",              IMPL_pthread_exit,          0, 0 },
    { "pthread_detach",            IMPL_pthread_detach,        0, 0 },
    { "pthread_cond_init",         IMPL_pthread_cond_init,     0, 0 },
    { "pthread_cond_wait",         IMPL_pthread_cond_wait,     0, 0 },
    { "pthread_cond_signal",       IMPL_pthread_cond_signal,   0, 0 },
    { "pthread_cond_broadcast",    IMPL_pthread_cond_broadcast,0, 0 },
    { "pthread_cond_destroy",      IMPL_pthread_cond_destroy,  0, 0 },

    /* --- loader / misc --- */
    { "getpid",                IMPL_getpid,   0, 0 },
    { "dlopen",                IMPL_dlopen,   0, 0 },
    { "dlsym",                 IMPL_dlsym,    0, 0 },
    { "dlclose",               IMPL_dlclose,  0, 0 },

    /* --- file I/O (asset access) --- */
    { "fopen",                 IMPL_fopen,    0, 0 },
    { "fclose",                IMPL_fclose,   0, 0 },
    { "fread",                 IMPL_fread,    0, 0 },
    { "fwrite",                IMPL_fwrite,   0, 0 },
    { "fseek",                 IMPL_fseek,    0, 0 },
    { "fstat",                 IMPL_fstat,    0, 0 },
    { "open",                  IMPL_open,     0, 0 },
    { "close",                 IMPL_close,    0, 0 },
    { "read",                  IMPL_read,     0, 0 },
    { "write",                 IMPL_write,    0, 0 },
    { "lseek",                 IMPL_lseek,    0, 0 },
    { "snprintf",              IMPL_snprintf, 0, 0 },
    { "sprintf",               IMPL_sprintf,  0, 0 },
    { "vsnprintf",             IMPL_vsnprintf,0, 0 },
    { "vsprintf",              IMPL_vsprintf, 0, 0 },
    { "fprintf",               IMPL_fprintf,  0, 0 },
};

#define N_KNOWN ((int)(sizeof(g_known) / sizeof(g_known[0])))

static const known_import *lookup_known(const char *name)
{
    for (int i = 0; i < N_KNOWN; i++)
        if (g_known[i].name[0] == name[0] && strcmp(g_known[i].name, name) == 0)
            return &g_known[i];
    return NULL;
}

int import_register(const char *name, int is_object)
{
    for (int i = 0; i < g_nimports; i++) {
        if (strcmp(g_imports[i].name, name) == 0)
            return i;
    }
    if (g_nimports >= IMPORT_MAX) {
        host_fatal("imports: more than %d imports\n", IMPORT_MAX);
        return 0;
    }
    int i = g_nimports++;
    guest_import *im = &g_imports[i];
    memset(im, 0, sizeof(*im));

    size_t n = strlen(name);
    if (n >= sizeof(im->name))
        n = sizeof(im->name) - 1;
    memcpy(im->name, name, n);
    im->name[n] = 0;

    const known_import *k = lookup_known(im->name);
    im->impl = k ? k->impl : IMPL_NONE;
    /* The JNI function tables are registered through the same mechanism as the
     * ELF imports (see jni.c), so they need the same accounting; jni_host_call
     * is what actually services them. */
    if (strncmp(im->name, "jni/", 4) == 0)
        im->impl = IMPL_jni;
    else if (im->name[0] == 'g' && im->name[1] == 'l')
        im->impl = IMPL_gl;
    /* Trust the symbol table over the table above: what matters is whether the
     * .so's own symbol is a variable, since that decides stub vs storage. */
    im->is_object = is_object || (k && k->is_object);
    im->stub = GUEST_STUB_BASE + (uint32_t)i * 4u;
    return i;
}

void imports_build_stubs(void)
{
    uint32_t obj_off = 0;
    for (int i = 0; i < g_nimports; i++) {
        guest_import *im = &g_imports[i];
        if (im->is_object) {
            const known_import *k = lookup_known(im->name);
            uint32_t sz = (k && k->obj_size) ? k->obj_size : 8;
            sz = (sz + 7u) & ~7u;
            im->stub = GUEST_HOSTDATA_BASE + obj_off;
            obj_off += sz;
            if (obj_off > GUEST_HOSTDATA_SIZE)
                host_fatal("imports: host data region exhausted\n");
            memset(g_mem + im->stub, 0, sz);
        } else {
            /* BKPT in the first halfword; the decoder derives the index from
             * the address, so the imm8 is free. Second halfword is padding to
             * keep stubs 4-byte aligned, which is what import_index_of_stub
             * relies on. */
            ST16(im->stub, 0xBE00);
            ST16(im->stub + 2, 0x0000);
            /* The GOT slot value is what the ARM PLT loads into PC, and an
             * interworking load takes its instruction set from bit 0. Without
             * this the PLT would enter the stub in ARM state and decode the
             * BKPT marker as the low half of an ARM word. See elf_loader.c,
             * where the slot is written. */
            im->interwork = im->stub | 1u;
        }
    }
    int nfn = 0, nimpl = 0, nobj = 0;
    for (int i = 0; i < g_nimports; i++) {
        if (g_imports[i].is_object) { nobj++; continue; }
        nfn++;
        if (g_imports[i].impl != IMPL_NONE)
            nimpl++;
    }
    host_log("imports: %d total -- %d functions (%d implemented, %d still "
             "missing), %d variables\n",
             g_nimports, nfn, nimpl, nfn - nimpl, nobj);
}

/* ---- dispatch ------------------------------------------------------------ */

int g_oracle_mode;

int import_dispatch(int idx)
{
    guest_import *im = &g_imports[idx];
    im->calls++;
    if (g_oracle_mode) {
        /* Deterministic no-op returning 0, matching the oracle's stub exactly.
         * Two different shim implementations would make any later divergence
         * ambiguous -- was it the CPU or the shim? -- so in this mode neither
         * side implements anything, and the comparison stays about the
         * decoder. The guest will usually walk into a NULL return soon after,
         * but it does so identically on both sides, which is the point.
         *
         * r0/r1 are cleared here rather than left to host_import_call(): that
         * function is skipped entirely in this mode, and the oracle does clear
         * them, so leaving them alone shows up as a phantom divergence. */
        g_cpu.r[0] = 0;
        g_cpu.r[1] = 0;
        /* Stop at the first crossing. Carrying on with every import stubbed
         * to NULL is possible but the traces stop being symmetric about the
         * stub itself, and the comparison is worth more clean than deep: what
         * is being tested here is the decoder, and the run to the first import
         * already exercises the whole PLT path. */
        return 1;
    }
    host_import_call(im);
    return 0;
}

void imports_report(int top_n)
{
    host_log("\n--- host crossings by import (top %d) ---\n", top_n);
    /* Selection sort over a copy of the indices; the import count is small
     * enough that this is not worth a heap. */
    int order[IMPORT_MAX];
    for (int i = 0; i < g_nimports; i++)
        order[i] = i;
    for (int i = 0; i < g_nimports; i++) {
        for (int j = i + 1; j < g_nimports; j++) {
            if (g_imports[order[j]].calls > g_imports[order[i]].calls) {
                int t = order[i]; order[i] = order[j]; order[j] = t;
            }
        }
    }
    uint64_t total = 0;
    for (int i = 0; i < g_nimports; i++)
        total += g_imports[i].calls;

    int shown = 0;
    for (int i = 0; i < g_nimports && shown < top_n; i++) {
        guest_import *im = &g_imports[order[i]];
        if (!im->calls)
            break;
        host_log("  %-28s %12llu calls  %6.2f%%  %8.1f us total  %s\n",
                 im->name, (unsigned long long)im->calls,
                 total ? 100.0 * (double)im->calls / (double)total : 0.0,
                 (double)im->ns / 1000.0,
                 im->is_object ? "(variable)"
                               : (im->impl == IMPL_NONE ? "NOT IMPLEMENTED" : ""));
        shown++;
    }
    host_log("  %-28s %12llu\n", "TOTAL", (unsigned long long)total);

    /* The worklist: imports the guest called that we do not provide yet. */
    int n_missing = 0;
    for (int i = 0; i < g_nimports; i++)
        if (!g_imports[i].is_object && g_imports[i].impl == IMPL_NONE &&
            g_imports[i].calls)
            n_missing++;
    if (n_missing) {
        host_log("\n--- %d unimplemented imports were actually called ---\n",
                 n_missing);
        for (int i = 0; i < g_nimports; i++) {
            guest_import *im = &g_imports[i];
            if (!im->is_object && im->impl == IMPL_NONE && im->calls)
                host_log("  %-28s %llu calls\n", im->name,
                         (unsigned long long)im->calls);
        }
    }
}
