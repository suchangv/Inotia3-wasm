/*
 * imports.h -- the 183 imported symbols of libgame.so, and how the guest calls
 * them without a dynamic linker.
 *
 * Mechanism: every imported symbol gets a 4-byte stub in the guest address
 * space at GUEST_STUB_BASE + index*4. The stub's first halfword is BKPT, which
 * is a real Thumb instruction the guest never legitimately executes. Relocation
 * writes the stub address into the symbol's GOT slot, so a guest call through
 * the PLT lands on the stub, the interpreter decodes BKPT, and derives the
 * import index from the address.
 *
 * Why this shape: it makes host crossings countable on the *existing* decode
 * path. There is no per-instruction "is this an import?" branch in the hot
 * loop -- BKPT is just one more Thumb opcode -- and every crossing is a single
 * counter bump. Measuring the crossing rate is one of the two numbers this
 * spike exists to produce.
 */
#ifndef IMPORTS_H
#define IMPORTS_H

#include <stdint.h>

#include "guest.h"

/* 182 ELF imports plus 234 JNI table entries, with headroom. */
#define IMPORT_MAX 1024

/* Implementation ids. import_register() maps an imported *name* to one of
 * these; -1 means "known to the .so but not provided by this host yet", which
 * is the worklist the first real run produces. Switching on an id rather than
 * on the import index keeps dispatch independent of the order symbols happen
 * to appear in the .so's dynamic symbol table. */
enum {
    IMPL_NONE = -1,
    IMPL_malloc = 0, IMPL_free, IMPL_calloc, IMPL_realloc,
    IMPL_memcpy, IMPL_memset, IMPL_memmove, IMPL_memcmp,
    IMPL_strlen, IMPL_strcmp, IMPL_strcpy, IMPL_strncpy, IMPL_strcat,
    IMPL_strncat, IMPL_strchr, IMPL_strrchr, IMPL_strstr, IMPL_strncmp,
    IMPL_strdup, IMPL_strerror, IMPL_strtok, IMPL_strtod,
    IMPL_atoi, IMPL_atoll, IMPL_strtoll, IMPL_qsort,
    IMPL_abort, IMPL_assert2, IMPL_stack_chk_fail,
    IMPL_android_log_write,
    IMPL_cxa_atexit, IMPL_aeabi_atexit, IMPL_cxa_finalize,
    IMPL_gettimeofday, IMPL_time, IMPL_nanosleep, IMPL_usleep, IMPL_sleep,
    IMPL_sin, IMPL_cos, IMPL_pow, IMPL_isinf,
    IMPL_setjmp, IMPL_longjmp,
    IMPL_pthread_mutex_init, IMPL_pthread_mutex_lock, IMPL_pthread_mutex_unlock,
    IMPL_pthread_mutex_trylock, IMPL_pthread_mutex_destroy,
    IMPL_pthread_mutexattr_init, IMPL_pthread_mutexattr_settype,
    IMPL_pthread_mutexattr_destroy,
    IMPL_pthread_once, IMPL_pthread_self, IMPL_pthread_equal,
    IMPL_pthread_key_create, IMPL_pthread_getspecific, IMPL_pthread_setspecific,
    IMPL_pthread_create, IMPL_pthread_join, IMPL_pthread_exit, IMPL_pthread_detach,
    IMPL_pthread_cond_init, IMPL_pthread_cond_wait, IMPL_pthread_cond_signal,
    IMPL_pthread_cond_broadcast, IMPL_pthread_cond_destroy,
    IMPL_mmap, IMPL_munmap,
    IMPL_getpid, IMPL_errno_addr,
    IMPL_mkdir, IMPL_stat,
    IMPL_dlopen, IMPL_dlsym, IMPL_dlclose,
    IMPL_fopen, IMPL_fclose, IMPL_fread, IMPL_fwrite, IMPL_fseek, IMPL_fstat,
    IMPL_open, IMPL_close, IMPL_read, IMPL_write, IMPL_lseek,
    IMPL_snprintf, IMPL_sprintf, IMPL_vsnprintf, IMPL_vsprintf, IMPL_fprintf,
    /* Directory walking and file deletion/rename. The engine's save-slot list,
     * its delete/discard paths and the Hub backup code all reach these, and
     * they were absent from the table entirely -- so they were reported as
     * "never seen" rather than "known and not provided", and every one of them
     * returned a silent 0 that reads as success. */
    IMPL_opendir, IMPL_readdir, IMPL_closedir,
    IMPL_remove, IMPL_rename, IMPL_lstat,
    /* statfs(struct statfs *, ...) -- fills a bionic-shaped struct; the engine
     * asks for free space before it writes. */
    IMPL_statfs,
    /* lrand48/srand48: the 48-bit LCG every Android build has. The engine's
     * only source of randomness. */
    IMPL_lrand48, IMPL_srand48,
    /* fsync/raise: completion signals with no host behaviour worth modelling. */
    IMPL_fsync, IMPL_raise,
    /* JNI table entries. They are not dispatched by this switch -- host_import
     * call routes anything named "jni/" to jni_host_call first -- but they need
     * an impl id so the report distinguishes "handled by the JNI layer" from
     * "the guest called something nobody provides". Without it every JNI call
     * was listed under "unimplemented imports were actually called". */
    IMPL_jni,
    /* GL entries are dispatched by name through gl_host_call() before this
     * switch is reached (gl.c), for the same reason as the JNI ones: there are
     * ~130 of them and a name table reads better than a 130-case switch. They
     * need an id all the same, or the report lists every GL call under
     * "unimplemented imports were actually called" -- which it did, making a
     * working GL layer look like a missing one. */
    IMPL_gl,
    IMPL_IMPL_COUNT
};

typedef struct {
    char     name[64];
    uint32_t stub;      /* guest address of this import's stub        */
    uint32_t interwork; /* stub|1: the value written into the GOT slot,
                         * so the ARM PLT's `ldr pc` lands in Thumb     */
    uint64_t calls;     /* how many times the guest called it         */
    uint64_t ns;        /* host time spent inside it, in ns           */
    int      impl;      /* IMPL_* id, or IMPL_NONE                    */
    int      is_object; /* imported variable, not function: the GOT
                         * slot must hold a real guest address rather
                         * than a BKPT stub, or the first read faults  */
} guest_import;

extern guest_import g_imports[IMPORT_MAX];
extern int          g_nimports;

/* Register an import, returning its index. Reuses an existing entry if the same
 * name is already known (the .so imports some symbols from several objects).
 * `is_object` marks an imported *variable* (__stack_chk_guard, __sF, __errno)
 * as opposed to a function. */
int  import_register(const char *name, int is_object);

/* Stamp BKPT stubs for every registered function import, and point object
 * imports at real guest storage. Call once, after all names are known and
 * before running any guest code. */
void imports_build_stubs(void);

/* Index for a stub address, or -1. */
static inline int import_index_of_stub(uint32_t addr)
{
    uint32_t off = addr - GUEST_STUB_BASE;
    if (off >= (uint32_t)IMPORT_MAX * 4u)
        return -1;
    if (off & 3u)
        return -1;
    return (int)(off >> 2);
}

/* Call the host implementation for import `idx` with the arguments the guest
 * placed in r0-r3 and on the stack. The return value is written back to r0
 * (and r1, for the 64-bit-returning libc functions).
 *
 * Returns non-zero if the interpreter should stop. In oracle mode (see
 * g_oracle_mode) it stops at the first crossing, which bounds a differential
 * test against Unicorn to pure CPU semantics: neither side's shims can be the
 * explanation for a difference in the traces. */
int import_dispatch(int idx);

/* When set, the interpreter only runs imports that the oracle can reproduce
 * exactly, and stops at any other. Used by the differential test. */
extern int g_oracle_mode;

/* Imports whose behaviour is fully determined by their arguments and guest
 * memory -- so both sides can perform them and the comparison can continue
 * *through* the crossing instead of ending at it.
 *
 * This list is the difference between a differential that can see inside
 * inflate and one that stops at its first memcpy, which is the difference
 * between finding a Huffman-decoding bug and never reaching it. Anything whose
 * result depends on the host (malloc, file I/O, time, JNI) stays out: the two
 * sides would disagree for reasons that are not the decoder. */
int import_is_reproducible(const char *name);

/* The implementation switch, in host_libc.c. Kept separate from the stub and
 * bookkeeping machinery above so that "what the guest can call" and "how the
 * guest reaches it" can be read independently. */
void host_import_call(guest_import *im);

/* Report the crossing histogram, worst offenders first. */
void imports_report(int top_n);

#endif /* IMPORTS_H */
