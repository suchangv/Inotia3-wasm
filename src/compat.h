/*
 * compat.h -- the handful of libc calls the core needs, so the same sources
 * build both against the host libc (native, for fast iteration under gdb) and
 * freestanding for wasm32 (where emscripten may not be in the loop at all).
 */
#ifndef COMPAT_H
#define COMPAT_H

#include <stdint.h>
#include <stddef.h>

#ifdef WASM_FREESTANDING

void *memcpy(void *d, const void *s, size_t n);
void *memset(void *d, int c, size_t n);
void *memmove(void *d, const void *s, size_t n);
int   memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
int   strcmp(const char *a, const char *b);
char *strncpy(char *d, const char *s, size_t n);

#else

#include <string.h>

#endif

/* Sort `n` elements of `base` (element size `esz`) by an unsigned key at
 * offset `keyoff` within each element. Plain merge sort: we cannot rely on
 * qsort existing in a freestanding wasm build, and load-time sorting 6k
 * symbols is not a hot path worth anything cleverer for. */
void sort_by_u32(uint8_t *base, size_t n, size_t esz, size_t keyoff);

#endif /* COMPAT_H */
