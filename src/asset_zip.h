/*
 * asset_zip.h -- find an asset inside the game's .apk. See asset_zip.c.
 */
#ifndef ASSET_ZIP_H
#define ASSET_ZIP_H

#include <stdint.h>

#ifndef WASM_FREESTANDING
#include <stdio.h>
#else
/* The freestanding wasm build -- the throughput measurement -- has no libc, and
 * the archive lookup is never called there. Only the FILE handle's type is
 * needed for the declaration. */
typedef struct _IO_FILE FILE;
#endif

/* Locate `entry_name` in the open archive. Returns 0 and fills the offset and
 * length of its stored data on success. */
int asset_zip_find(FILE *f, const char *entry_name, uint32_t *out_offset,
                   uint32_t *out_length);

#endif /* ASSET_ZIP_H */
