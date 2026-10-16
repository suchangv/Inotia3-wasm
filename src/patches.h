/*
 * patches.h -- deliberate modifications to the guest binary. See patches.c.
 */
#ifndef PATCHES_H
#define PATCHES_H

#include "elf.h"

/* Apply every patch. Call after elf_load() and before any guest code runs. */
void patches_apply(const guest_image *img);

/* Fill in the lazily-populated JavaVM caches. Must run *after*
 * jni_guest_init(), which is what decides the JavaVM's guest address. */
void patches_fill_vm_caches(const guest_image *img);

/* Replace a guest function with a jump to an import's BKPT stub, so the host
 * can implement it in C. The jump is
 *
 *     ldr r3, [pc, #0]     0x4B00
 *     bx  r3               0x4718
 *     .word stub|1
 *
 * and because it is a jump rather than a call, the stub's handler returns
 * straight to the guest function's original caller -- which is what makes this
 * a replacement and not a wrapper. Returns 1 if the symbol was found. */
int patches_replace_with_import(const guest_image *img, const char *symbol,
                                const char *import_name);

#endif /* PATCHES_H */
