/*
 * asset_zip.h -- find an asset inside the game's .apk. See asset_zip.c.
 */
#ifndef ASSET_ZIP_H
#define ASSET_ZIP_H

#include <stdint.h>
#include <stdio.h>

/* Locate `entry_name` in the open archive. Returns 0 and fills the offset and
 * length of its stored data on success. */
int asset_zip_find(FILE *f, const char *entry_name, uint32_t *out_offset,
                   uint32_t *out_length);

#endif /* ASSET_ZIP_H */
