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

#define IMPORT_MAX 512

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
    IMPL_dlopen, IMPL_dlsym, IMPL_dlclose,
    IMPL_fopen, IMPL_fclose, IMPL_fread, IMPL_fwrite, IMPL_fseek, IMPL_fstat,
    IMPL_open, IMPL_close, IMPL_read, IMPL_write, IMPL_lseek,
    IMPL_snprintf, IMPL_sprintf, IMPL_vsnprintf, IMPL_vsprintf, IMPL_fprintf,
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
 * (and r1, for the 64-bit-returning libc functions). */
void import_dispatch(int idx);

/* The implementation switch, in host_libc.c. Kept separate from the stub and
 * bookkeeping machinery above so that "what the guest can call" and "how the
 * guest reaches it" can be read independently. */
void host_import_call(guest_import *im);

/* Report the crossing histogram, worst offenders first. */
void imports_report(int top_n);

#endif /* IMPORTS_H */
