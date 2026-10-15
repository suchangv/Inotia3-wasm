/*
 * util.c -- guest memory backing, host services, and a dependency-free sort.
 *
 * The native build gets the real libc; the wasm32 build gets enough of it
 * inline that the core needs no emscripten. Keeping both paths in one file
 * makes it obvious what a browser port still has to supply.
 */

#include "compat.h"
#include "guest.h"

/* ---- guest address space ------------------------------------------------- */

uint8_t *g_mem;
uint32_t g_mem_size = GUEST_MEM_SIZE;

/* ---- merge sort by an unsigned 32-bit key --------------------------------- */

static void merge_u32(uint8_t *base, uint8_t *tmp, size_t n, size_t esz,
                      size_t keyoff)
{
    if (n < 2)
        return;
    size_t mid = n / 2;
    merge_u32(base, tmp, mid, esz, keyoff);
    merge_u32(base + mid * esz, tmp, n - mid, esz, keyoff);

    uint8_t *a = base, *b = base + mid * esz, *end = base + n * esz;
    uint8_t *o = tmp;
    uint8_t *bend = end;
    while (a < base + mid * esz && b < bend) {
        uint32_t ka = *(uint32_t *)(a + keyoff);
        uint32_t kb = *(uint32_t *)(b + keyoff);
        if (ka <= kb) { memcpy(o, a, esz); a += esz; }
        else          { memcpy(o, b, esz); b += esz; }
        o += esz;
    }
    while (a < base + mid * esz) { memcpy(o, a, esz); a += esz; o += esz; }
    while (b < bend)             { memcpy(o, b, esz); b += esz; o += esz; }
    memcpy(base, tmp, n * esz);
    (void)end;
}

void sort_by_u32(uint8_t *base, size_t n, size_t esz, size_t keyoff)
{
    if (n < 2)
        return;
    uint8_t *tmp = (uint8_t *)host_alloc(n * esz);
    merge_u32(base, tmp, n, esz, keyoff);
    /* tmp is deliberately leaked: this runs once at load time and the
     * allocation is the same order as the symbol table itself. */
}

#ifdef WASM_FREESTANDING
/* --- freestanding replacements (no libc in the wasm32 build yet) ---------- */

void *memcpy(void *d, const void *s, size_t n)
{
    uint8_t *a = (uint8_t *)d; const uint8_t *b = (const uint8_t *)s;
    while (n--) *a++ = *b++;
    return d;
}
void *memset(void *d, int c, size_t n)
{
    uint8_t *a = (uint8_t *)d;
    while (n--) *a++ = (uint8_t)c;
    return d;
}
void *memmove(void *d, const void *s, size_t n)
{
    uint8_t *a = (uint8_t *)d; const uint8_t *b = (const uint8_t *)s;
    if (a < b) { while (n--) *a++ = *b++; }
    else { a += n; b += n; while (n--) *--a = *--b; }
    return d;
}
int memcmp(const void *a, const void *b, size_t n)
{
    const uint8_t *x = (const uint8_t *)a, *y = (const uint8_t *)b;
    while (n--) { if (*x != *y) return *x - *y; x++; y++; }
    return 0;
}
size_t strlen(const char *s) { const char *p = s; while (*p) p++; return (size_t)(p - s); }
int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (int)(uint8_t)*a - (int)(uint8_t)*b;
}
char *strncpy(char *d, const char *s, size_t n)
{
    size_t i = 0;
    for (; i < n && s[i]; i++) d[i] = s[i];
    for (; i < n; i++) d[i] = 0;
    return d;
}
#endif
