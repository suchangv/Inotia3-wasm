/*
 * guest.h -- guest address space model shared by the native and wasm32 builds.
 *
 * The guest is ARMv5TE/Thumb-1 running `libgame.so` (Inotia 3). It gets a flat
 * 32-bit address space carved into fixed regions. The *whole* address space is
 * one contiguous buffer, so guest address -> host pointer is a plain add; that
 * is what makes the interpreter equally fast native and under wasm (where the
 * buffer simply lives in linear memory).
 *
 * Layout (192 MB total, chosen to fit comfortably in a wasm32 memory):
 *
 *   0x00000000  +--------------------+  unmapped null guard: a null deref in the
 *               |  (16 MB, no map)   |  guest traps instead of silently reading
 *   0x01000000  +--------------------+  real data, which matters a lot when
 *               |  libgame.so image  |  triaging a crash in 745 KB of unfamiliar
 *               |  (16 MB reserved)  |  code.
 *   0x02000000  +--------------------+
 *               |  guest heap        |  what the guest's malloc() hands out.
 *               |  (64 MB, grows up) |
 *   0x06000000  +--------------------+
 *               |  guest stacks      |  grows down to 0x06800000; one slice per
 *               |  (8 MB)            |  guest thread.
 *   0x06800000  +--------------------+
 *               |  mmap arena        |  mmap()/dlopen() scratch.
 *               |  (32 MB)           |
 *   0x08800000  +--------------------+
 *               |  import stubs      |  4 bytes per imported symbol, each a
 *               |  (4 MB reserved)   |  BKPT marker. See imports.c.
 *   0x09000000  +--------------------+  end (144 MB)
 */
#ifndef GUEST_H
#define GUEST_H

#include <stdint.h>
#include <stddef.h>

#define GUEST_IMAGE_BASE   0x01000000u
#define GUEST_IMAGE_SIZE   0x01000000u  /* 16 MB reserved for the image      */
#define GUEST_HEAP_BASE    0x02000000u
#define GUEST_HEAP_SIZE    0x04000000u  /* 64 MB                             */
#define GUEST_STACK_TOP    0x06800000u
#define GUEST_STACK_SIZE   0x00800000u  /* 8 MB, grows down                  */
#define GUEST_MMAP_BASE    0x06800000u
#define GUEST_MMAP_SIZE    0x02000000u  /* 32 MB                             */
#define GUEST_STUB_BASE    0x08800000u
#define GUEST_STUB_SIZE    0x00400000u  /* 4 MB: room for 1M function stubs  */
#define GUEST_HOSTDATA_BASE 0x08C00000u /* imported *variables* live here,   */
#define GUEST_HOSTDATA_SIZE 0x00100000u /* not in the stub page -- see below */
#define GUEST_JNI_BASE     0x08D00000u  /* JavaVM / JNIEnv structs and their
                                         * function tables. They have to live
                                         * in guest memory because the guest
                                         * indexes them by offset; every slot
                                         * holds a stub address. See jni.c.  */
#define GUEST_JNI_SIZE     0x00001000u
#define GUEST_MEM_SIZE     0x09000000u  /* 144 MB total                      */

/* Guest heap/mmap actually start after their region bases so a zero-ish
 * pointer never lands in a live allocation. */
#define GUEST_HEAP_START   (GUEST_HEAP_BASE + 0x1000u)

/* Where a returning entry function lands. Outside the image on purpose, so
 * HALT_BAD_PC fires: see the note in src/main.c. Must match
 * RETURN_SENTINEL in tools/oracle.py. */
#define RETURN_SENTINEL    0x0BADF00Du

extern uint8_t *g_mem;          /* whole guest address space               */
extern uint32_t g_mem_size;

/* ---- memory access -------------------------------------------------------
 *
 * Deliberately plain array indexing with NO bounds check: this is the hot path
 * of the whole interpreter and the guest never leaves its own address space
 * once the loader has validated the segments. `GUEST_CHECK` builds add a guard
 * for bring-up; the release build drops it.
 */
#ifdef GUEST_CHECK
uint32_t guest_ld32(uint32_t a);
uint16_t guest_ld16(uint32_t a);
uint8_t  guest_ld8 (uint32_t a);
void     guest_st32(uint32_t a, uint32_t v);
void     guest_st16(uint32_t a, uint16_t v);
void     guest_st8 (uint32_t a, uint8_t  v);
#define LD32(a) guest_ld32(a)
#define LD16(a) guest_ld16(a)
#define LD8(a)  guest_ld8(a)
#define ST32(a, v) guest_st32((a), (v))
#define ST16(a, v) guest_st16((a), (v))
#define ST8(a, v)  guest_st8((a), (v))
/* TEMP store probe (see util.c). Set from --probe=ADDR,LEN. */
extern uint32_t g_probe_lo, g_probe_len, g_probe_n, g_probe_on;
#else
#define LD32(a) (*(uint32_t *)(g_mem + (uint32_t)(a)))
#define LD16(a) (*(uint16_t *)(g_mem + (uint32_t)(a)))
#define LD8(a)  (*(uint8_t  *)(g_mem + (uint32_t)(a)))
#define ST32(a, v) (*(uint32_t *)(g_mem + (uint32_t)(a)) = (uint32_t)(v))
#define ST16(a, v) (*(uint16_t *)(g_mem + (uint32_t)(a)) = (uint16_t)(v))
#define ST8(a, v)  (*(uint8_t  *)(g_mem + (uint32_t)(a)) = (uint8_t)(v))
#endif

/* ---- host services every platform must supply ---------------------------- */
void host_log(const char *fmt, ...);

/* Scans the guest stack for return addresses -- see util.c. Heuristic, and
 * named as one, but it is the only call chain either wasm build can produce. */
void guest_backtrace(const char *why);
void *host_alloc(size_t n);          /* zeroed, in host memory (not guest) */
double host_now(void);               /* monotonic seconds                  */
void host_fatal(const char *fmt, ...);

/* Allocate from the guest heap -- the same allocator the guest's own malloc()
 * draws on, so JNI-allocated strings and arrays are ordinary guest memory. */
uint32_t guest_heap_alloc(uint32_t n);

/* Debug: log every host-side write covering this guest address. 0 = off. */
extern uint32_t g_watch_addr;

/* ---- per-instruction trace, for differential testing ---------------------
 *
 * Emitted *after* an instruction has executed, keyed by the PC of the
 * instruction that ran. The oracle (tools/oracle.py, driven by Unicorn)
 * produces the same lines from the same guest memory image; the first line
 * that differs is the first instruction whose semantics are wrong. That turns
 * "the guest crashed somewhere" into a specific address and register.
 */
extern int g_trace_insn_on;
void host_trace_insn(uint32_t pc, const uint32_t *regs, uint32_t cpsr);

#endif /* GUEST_H */
