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

/* The same lookup by *logical* name: the engine names a resource without the
 * ".jpg" the packer appends to files that are already compressed
 * ("MainMenu.png" -> "assets/common/game_res/MainMenu.png.jpg", and a real
 * Ogg Vorbis stream is stored as "BGM1.ogg.jpg"). Both spellings are tried.
 * Returns 0 and fills offset/length on success. Implemented in host_libc.c,
 * next to the path hook that resolves the same names for the engine itself. */
int asset_lookup(const char *name, uint32_t *out_offset, uint32_t *out_length);

/* The one archive every asset lives in. Defined in asset_zip.c; the JNI layer
 * and the File::GetFullPathName hook both need it. */
#define ASSET_ARCHIVE "assets.apk"

/* Where that archive actually is on disk this run. Candidates differ by build
 * (browser preloads at the filesystem root, the desktop build keeps it beside
 * the .so), so the first candidate that opens wins -- and the same answer is
 * what GetApkFilePath hands the engine, so every step of the
 * @offset|length|path chain opens the file it names. Never NULL. */
const char *host_apk_path(void);

#endif /* ASSET_ZIP_H */
