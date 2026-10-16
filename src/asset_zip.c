/*
 * asset_zip.c -- find an asset inside the game's .apk.
 *
 * The engine's resource access is Android's AssetManager.openFd() shape: ask
 * the Java layer for a file descriptor and an offset, then read the asset
 * straight out of the archive at that offset. Reproducing it is what makes the
 * engine's own resource code work unchanged:
 *
 *     GetFileDescriptor(name, values, true)   -> values = {offset, length}
 *     GetApkFilePath()                        -> the archive's path
 *     File::Open("@<offset>|<length>|<path>") -> mmap, read
 *
 * The alternative the Vita port took -- pre-extracted loose files -- needs a
 * hook on File::GetFullPathName, because jGetApkFilePath caches its answer
 * forever after the first call and would hand every later asset the first
 * asset's path (source/patch.c and source/java.c document that in full).
 * Pointing at one archive sidesteps the caching entirely: the path is a
 * constant and the offset is what varies.
 *
 * Only STORED entries are usable this way, which is the point -- Android's
 * openFd() requires them too, so an .apk built for this API has its assets
 * uncompressed. A DEFLATE entry here means the archive was rebuilt with
 * compression on, and the caller is told rather than handed garbage.
 */

#include "asset_zip.h"
#include "guest.h"
#include "compat.h"

#include <stdio.h>
#include <string.h>

/* --- little-endian readers over a byte buffer ----------------------------- */
static uint32_t rd16(const uint8_t *p) { return (uint32_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* The end-of-central-directory record is at the end of the file, possibly
 * followed by a comment, so it is searched backwards. */
static long find_eocd(FILE *f, long file_size)
{
    long window = file_size < 66000 ? file_size : 66000;
    long start = file_size - window;
    uint8_t buf[66000];
    if (fseek(f, start, SEEK_SET) != 0)
        return -1;
    size_t got = fread(buf, 1, (size_t)window, f);
    for (long i = (long)got - 22; i >= 0; i--) {
        if (buf[i] == 0x50 && buf[i + 1] == 0x4b &&
            buf[i + 2] == 0x05 && buf[i + 3] == 0x06)
            return start + i;
    }
    return -1;
}

/*
 * Find `entry_name` (a full path inside the archive, e.g.
 * "assets/common/game_res/MainMenu.jpg") and return its raw offset and stored
 * length. Returns 0 on success, non-zero otherwise.
 */
int asset_zip_find(FILE *f, const char *entry_name, uint32_t *out_offset,
                   uint32_t *out_length)
{
    if (fseek(f, 0, SEEK_END) != 0)
        return 1;
    long file_size = ftell(f);
    if (file_size < 22)
        return 1;

    long eocd = find_eocd(f, file_size);
    if (eocd < 0) {
        host_log("assets: no end-of-central-directory record -- not a zip\n");
        return 1;
    }

    uint8_t e[22];
    if (fseek(f, eocd, SEEK_SET) != 0 || fread(e, 1, 22, f) != 22)
        return 1;
    uint32_t count = rd16(e + 10);
    uint32_t cd_size = rd32(e + 12);
    uint32_t cd_off = rd32(e + 16);
    if (cd_off + cd_size > (uint32_t)file_size) {
        host_log("assets: central directory runs past the end of the file\n");
        return 1;
    }

    /* Walk the central directory. Its entries carry the name and, in the
     * "local header offset" field, the position of the entry's own header --
     * the data starts a variable distance after that, so the local header has
     * to be read to get the true start of the data. */
    size_t name_len = strlen(entry_name);
    uint8_t *cd = (uint8_t *)host_alloc(cd_size);
    if (fseek(f, cd_off, SEEK_SET) != 0 ||
        fread(cd, 1, cd_size, f) != cd_size) {
        host_log("assets: cannot read the central directory\n");
        return 1;
    }

    host_log("assets: zip has %u entries, central directory at %u (%u bytes)\n",
             count, cd_off, cd_size);
    static int dumped;
    int result = 1;
    uint32_t p = 0;
    for (uint32_t i = 0; i < count && p + 46 <= cd_size; i++) {
        if (rd32(cd + p) != 0x02014b50u) {
            host_log("assets: bad central directory signature at entry %u\n", i);
            break;
        }
        uint32_t method   = rd16(cd + p + 10);
        uint32_t csize    = rd32(cd + p + 20);
        uint32_t usize    = rd32(cd + p + 24);
        uint32_t nlen     = rd16(cd + p + 28);
        uint32_t elen     = rd16(cd + p + 30);
        uint32_t clen     = rd16(cd + p + 32);
        uint32_t lho      = rd32(cd + p + 42);
        const char *nm    = (const char *)(cd + p + 46);

        if (dumped < 4 && i < 3) {
            host_log("assets:   entry[%u] '%.*s' (method=%u lho=%u)\n",
                     i, (int)(nlen < 60 ? nlen : 60), nm, method, lho);
            if (i == 2) dumped = 4;
        }
        if (nlen == name_len && memcmp(nm, entry_name, nlen) == 0) {
            if (method != 0) {
                host_log("assets: %s is DEFLATE-compressed (method %u); "
                         "openFd-style access needs STORED entries\n",
                         entry_name, method);
                break;
            }
            /* Read the local header to find where the data really begins: its
             * own name and extra field lengths are independent of the
             * central directory's. */
            uint8_t lh[30];
            if (fseek(f, (long)lho, SEEK_SET) != 0 || fread(lh, 1, 30, f) != 30)
                break;
            uint32_t lnlen = rd16(lh + 26);
            uint32_t lelen = rd16(lh + 28);
            *out_offset = lho + 30 + lnlen + lelen;
            *out_length = usize ? usize : csize;
            result = 0;
            break;
        }
        p += 46 + nlen + elen + clen;
    }

    if (result)
        host_log("assets: %s not found in the archive\n", entry_name);
    return result;
}
