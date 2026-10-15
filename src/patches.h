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

#endif /* PATCHES_H */
