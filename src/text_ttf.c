/*
 * text_ttf.c -- the Java-side text renderer, over the platform's own fonts.
 *
 * (Was text_android.c: nothing in it is Android-specific -- stb_truetype, mmap
 * and dirent -- so the SDL build on the handheld uses the same file with an
 * extra list of font directories.)
 *
 * The engine draws no glyphs. SGL_Font_Android::RenderToTexture calls
 * jDrawText, the *Java* side paints the string into one shared ARGB buffer, and
 * the engine uploads the alpha channel of that buffer as a GL_ALPHA texture.
 * On Android that Java side is Canvas.drawText against the system fonts, which
 * is how the game gets every language it ships (CJK included) without bundling
 * one. This file is that renderer in C: stb_truetype over the phone's own font
 * files. The layout half -- wrapping, alignment, the scratch buffer -- stays in
 * text.c and is shared with the browser.
 *
 * Why not one fixed font path: the fonts are the *device's*. A stock Android has
 * NotoSansCJK-Regular.ttc, a Xiaomi has MiSans, a Huawei has HarmonyOS Sans, and
 * a device can be missing every name this file knows. The first version of this
 * backend tried four hard-coded paths, picked one whose cmap had the language's
 * probe character, and produced blank text on a device whose CJK font had a
 * different name -- Latin still worked, so it read as "the Chinese font does not
 * render". What it does now:
 *
 *   1. try the known paths, fastest first;
 *   2. if none of them can actually *rasterise* the language's probe character,
 *      scan /system/fonts for anything that can, CJK-looking names first;
 *   3. keep every font that loaded and, per character, use the first one that
 *      has a glyph for it -- one font for Latin, another for CJK is fine;
 *   4. say which font is being used and, if none can draw the probe, say that
 *      text will be blank. A blank screen with no log line is the failure mode
 *      this file exists to avoid.
 *
 * Layout contract (text.c's text_host_* declarations):
 *
 *   - measure and paint must agree pixel for pixel. The engine sizes the alpha
 *     texture from the measurement and then paints into it, so an advance that
 *     differs by one accumulates into clipped text at the end of a line. Both
 *     sides call advance_px(): one rounding of the font's advance per character,
 *     summed in integers.
 *   - text is sized the way Android's Paint sizes it: the request is an em size
 *     (stbtt_ScaleForMappingEmToPixels), not an ascent-to-descent box height. A
 *     CJK font's ascent+descent is ~1.45 em, so scaling by that would draw every
 *     character at 70% of the size the engine asked for.
 *   - the buffer is 0xAARRGGBB with the coverage in the alpha byte and white in
 *     the RGB bytes, because only alpha is uploaded.
 *   - `stride` is the rect width, not the buffer width, and rows outside
 *     [0, max_h) or columns outside [0, max_w) are dropped rather than clipped
 *     at the edge of the allocation.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include "guest.h"
#include "compat.h"
#include "platform.h"

#define STB_TRUETYPE_IMPLEMENTATION
#include "../third_party/stb/stb_truetype.h"

/* Fonts kept open at once. One is the normal case; more is the fallback chain
 * (Latin + CJK + symbols). The data is mmap'd, so an unused 19 MB CJK collection
 * costs address space, not memory. */
#define MAX_FONTS 6

typedef struct {
    unsigned char *data;
    size_t len;
    stbtt_fontinfo info;
    char path[192];
} tfont;

static tfont g_fonts[MAX_FONTS];
static int g_nfonts;
static int g_probed;

/* Known-good paths, in preference order. The app's own directory first, so
 * dropping a font there is enough to support a language the device does not. */
static const char *k_known[] = {
    "./font.ttf",
    "./font.ttc",
    "./font.otf",
    "/system/fonts/NotoSansCJK-Regular.ttc",
    "/system/fonts/NotoSansCJKsc-Regular.otf",
    "/system/fonts/NotoSansSC-Regular.otf",
    "/system/fonts/DroidSansFallback.ttf",
    "/system/fonts/MiSans-Regular.ttf",
    "/system/fonts/HarmonyOS_Sans_SC_Regular.ttf",
    "/system/fonts/NotoSansCJK-Regular.otf",
    "/system/fonts/Roboto-Regular.ttf",
    "/system/fonts/DroidSans.ttf",
    /* Linux / PortMaster handhelds: whatever font package the firmware ships. */
    "/usr/share/fonts/truetype/noto/NotoSansCJK-Regular.ttc",
    "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
    "/usr/share/fonts/truetype/wqy/wqy-zenhei.ttc",
    "/usr/share/fonts/truetype/wqy/wqy-microhei.ttc",
    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
    NULL
};

/* ---- loading ------------------------------------------------------------- */

static tfont *load_font(const char *path)
{
    if (g_nfonts >= MAX_FONTS)
        return NULL;
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return NULL;
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size < 1024) {
        close(fd);
        return NULL;
    }
    void *p = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (p == MAP_FAILED)
        return NULL;

    /* A .ttc/.otc is a collection; try every face until one initialises. The
     * faces of NotoSansCJK.ttc are the language variants (SC/TC/JP/KR), and
     * picking face 0 is what the browser-style "first one" assumption would
     * do -- fine, but a face can also fail outright, so try them. */
    int faces = stbtt_GetNumberOfFonts((const unsigned char *)p);
    if (faces <= 0)
        faces = 1;
    for (int i = 0; i < faces && i < 12; i++) {
        int off = faces > 1 ? stbtt_GetFontOffsetForIndex((const unsigned char *)p, i) : 0;
        if (off < 0)
            continue;
        tfont *f = &g_fonts[g_nfonts];
        if (!stbtt_InitFont(&f->info, (const unsigned char *)p, off))
            continue;
        f->data = (unsigned char *)p;
        f->len = (size_t)st.st_size;
        snprintf(f->path, sizeof(f->path), "%s", path);
        g_nfonts++;
        host_log("text: loaded %s (face %d)\n", path, i);
        return f;
    }
    munmap(p, (size_t)st.st_size);
    return NULL;
}

/* ---- character metrics -------------------------------------------------- */

static int utf8_len(const uint8_t *s, int i, int len)
{
    uint8_t c = s[i];
    int n = 1;
    if (c >= 0xf0) n = 4;
    else if (c >= 0xe0) n = 3;
    else if (c >= 0xc0) n = 2;
    if (c >= 0x80 && c < 0xc0) n = 1;
    if (i + n > len) n = 1;
    return n;
}

static uint32_t utf8_code(const uint8_t *s, int n)
{
    if (n == 1) return s[0];
    if (n == 2) return ((uint32_t)(s[0] & 0x1f) << 6) | (s[1] & 0x3f);
    if (n == 3)
        return ((uint32_t)(s[0] & 0x0f) << 12) |
               ((uint32_t)(s[1] & 0x3f) << 6) | (s[2] & 0x3f);
    return ((uint32_t)(s[0] & 0x07) << 18) |
           ((uint32_t)(s[1] & 0x3f) << 12) |
           ((uint32_t)(s[2] & 0x3f) << 6) | (s[3] & 0x3f);
}

/* Android's Paint.textSize is an em size, so map the em square to the requested
 * number of pixels. ScaleForPixelHeight would use ascent-to-descent instead,
 * which for a CJK font is ~1.45x the em -- every character would come out at
 * about 70% of the size the engine asked for. */
static float font_scale(tfont *f, int size)
{
    return stbtt_ScaleForMappingEmToPixels(&f->info, (float)size);
}

static int font_has(tfont *f, uint32_t cp)
{
    if (cp == 0)
        return 0;
    return stbtt_FindGlyphIndex(&f->info, (int)cp) != 0;
}

/* Per-character font choice. The cmap lookup is cheap but this runs for every
 * character of every string of every frame, so cache it. Direct-mapped: a
 * collision just costs one lookup. */
#define CP_CACHE 256
static struct { uint32_t cp; int8_t font; int8_t valid; } g_cp_cache[CP_CACHE];

static tfont *font_for(uint32_t cp)
{
    if (g_nfonts == 0)
        return NULL;
    unsigned slot = cp & (CP_CACHE - 1);
    if (g_cp_cache[slot].valid && g_cp_cache[slot].cp == cp) {
        int i = g_cp_cache[slot].font;
        if (i >= 0 && i < g_nfonts)
            return &g_fonts[i];
        return &g_fonts[0];
    }
    int pick = 0;
    for (int i = 0; i < g_nfonts; i++) {
        if (font_has(&g_fonts[i], cp)) {
            pick = i;
            break;
        }
    }
    g_cp_cache[slot].cp = cp;
    g_cp_cache[slot].font = (int8_t)pick;
    g_cp_cache[slot].valid = 1;
    return &g_fonts[pick];
}

/* A character counts as drawable when the rasteriser produces a non-empty box.
 * The cmap alone is not enough: a font can map a codepoint to a glyph this
 * backend cannot outline, and then the text is blank with no error anywhere. */
static int font_draws(tfont *f, uint32_t cp, int size)
{
    if (!font_has(f, cp))
        return 0;
    if (cp == ' ')
        return 1;
    float s = font_scale(f, size);
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    stbtt_GetCodepointBitmapBox(&f->info, (int)cp, s, s, &x0, &y0, &x1, &y1);
    return (x1 - x0) > 0 && (y1 - y0) > 0;
}

static uint32_t language_probe(void)
{
    const char *l = host_language();
    if (!l) return 'A';
    if (l[0] == 'z' || l[0] == 'Z') return 0x4E2D;   /* 中 */
    if (l[0] == 'k' || l[0] == 'K') return 0xAC00;   /* 가 */
    if (l[0] == 'j' || l[0] == 'J') return 0x3042;   /* あ */
    if (l[0] == 'r' || l[0] == 'R') return 0x0416;   /* Ж, Cyrillic */
    if (l[0] == 'a' || l[0] == 'A') return 0x0627;   /* ا, Arabic */
    return 'A';
}

/* Files that are certainly not what a UI string needs, and names that suggest
 * CJK, for the /system/fonts scan. */
static int name_has(const char *s, const char *what)
{
    size_t n = strlen(what);
    for (const char *p = s; *p; p++) {
        size_t i = 0;
        while (i < n && p[i] &&
               (p[i] == what[i] ||
                (p[i] >= 'A' && p[i] <= 'Z' ? p[i] + 32 : p[i]) == what[i]))
            i++;
        if (i == n)
            return 1;
    }
    return 0;
}

static int font_name_ok(const char *n)
{
    static const char *bad[] = {
        "emoji", "mono", "condensed", "italic", "bold", "black", "thin",
        "light", "medium", "cursive", "serif", "symbol", "dingbat", "music",
        NULL
    };
    for (int i = 0; bad[i]; i++)
        if (name_has(n, bad[i]))
            return 0;
    size_t l = strlen(n);
    return l > 4 && (strcmp(n + l - 4, ".ttf") == 0 ||
                     strcmp(n + l - 4, ".ttc") == 0 ||
                     strcmp(n + l - 4, ".otf") == 0);
}

static int font_name_cjk(const char *n)
{
    return name_has(n, "cjk") || name_has(n, "fallback") || name_has(n, "sc-") ||
           name_has(n, "_sc") || name_has(n, "hans") || name_has(n, "chinese") ||
           name_has(n, "misans") || name_has(n, "harmony") || name_has(n, "han");
}

/* Last resort: ask the directory itself. Two passes so a CJK-looking name is
 * preferred over whatever else might have a glyph for the probe. */
static int scan_system_fonts(uint32_t probe, int size)
{
    static const char *dirs[] = {
        "/system/fonts", "/usr/share/fonts/truetype", "/usr/share/fonts",
        "/usr/share/fonts/opentype", NULL
    };
    for (int di = 0; dirs[di]; di++) {
        for (int pass = 0; pass < 2; pass++) {
            DIR *d = opendir(dirs[di]);
            if (!d)
                break;                  /* no such directory: next one */
            struct dirent *e;
            while ((e = readdir(d)) != NULL) {
                if (!font_name_ok(e->d_name))
                    continue;
                if (pass == 0 && !font_name_cjk(e->d_name))
                    continue;
                if (pass == 1 && font_name_cjk(e->d_name))
                    continue;
                char path[320];
                snprintf(path, sizeof(path), "%s/%s", dirs[di], e->d_name);
                tfont *f = load_font(path);
                if (!f)
                    continue;
                if (font_draws(f, probe, size)) {
                    host_log("text: %s draws U+%04X\n", path, probe);
                    closedir(d);
                    return 1;
                }
            }
            closedir(d);
        }
    }
    return 0;
}

static void ensure_fonts(void)
{
    if (g_probed)
        return;
    g_probed = 1;
    uint32_t probe = language_probe();
    int size = 14;

    for (int i = 0; k_known[i]; i++)
        load_font(k_known[i]);

    /* Which of them can actually draw this language? */
    int primary = -1;
    for (int i = 0; i < g_nfonts; i++) {
        if (font_draws(&g_fonts[i], probe, size)) {
            primary = i;
            break;
        }
    }
    if (primary < 0) {
        host_log("text: no loaded font draws U+%04X, scanning /system/fonts\n",
                 probe);
        if (scan_system_fonts(probe, size)) {
            for (int i = 0; i < g_nfonts; i++)
                if (font_draws(&g_fonts[i], probe, size)) {
                    primary = i;
                    break;
                }
        }
    }
    if (g_nfonts == 0) {
        host_log("text: NO usable font at all -- text will be blank\n");
        return;
    }
    if (primary < 0) {
        host_log("text: NO font on this device draws U+%04X -- text in this "
                 "language will be blank (latin will still work)\n", probe);
        primary = 0;
    } else if (primary > 0) {
        /* The font that can draw the language decides the line metrics: a CJK
         * font's ascent/descent is ~1.45 em, and mixing it with another font's
         * would clip the very glyphs we went to the trouble of finding. */
        tfont tmp = g_fonts[0];
        g_fonts[0] = g_fonts[primary];
        g_fonts[primary] = tmp;
    }
    host_log("text: primary font %s (%d loaded)\n", g_fonts[0].path, g_nfonts);
    memset(g_cp_cache, 0, sizeof(g_cp_cache));
}

/* ---- metrics ------------------------------------------------------------- */

typedef struct {
    int ascent, height;
    float scale;
} tmetrics;

static tmetrics metrics_for(int font_size)
{
    tmetrics m;
    memset(&m, 0, sizeof(m));
    if (g_nfonts == 0 || font_size <= 0) {
        m.ascent = font_size;
        m.height = font_size;
        return m;
    }
    tfont *f = &g_fonts[0];
    m.scale = font_scale(f, font_size);
    int a = 0, d = 0, gap = 0;
    stbtt_GetFontVMetrics(&f->info, &a, &d, &gap);
    m.ascent = (int)ceilf((float)a * m.scale);
    m.height = m.ascent + (int)ceilf((float)-d * m.scale);
    if (m.height < font_size)
        m.height = font_size;
    return m;
}

/* The one rounding of a character's advance, used by both measurement and
 * paint, from the font that will draw it. */
static int advance_px(uint32_t cp, int font_size)
{
    tfont *f = font_for(cp);
    if (!f)
        return font_size * 3 / 7;
    float s = font_scale(f, font_size);
    int adv = 0, lsb = 0;
    stbtt_GetCodepointHMetrics(&f->info, (int)cp, &adv, &lsb);
    if (adv <= 0)
        adv = font_size;            /* blank/unknown: keep the cell square */
    return (int)((float)adv * s + 0.5f);
}

/* ---- text.c's host hooks ------------------------------------------------- */

int text_host_width(const char *s, int len, int font_size)
{
    ensure_fonts();
    const uint8_t *u = (const uint8_t *)s;
    int w = 0;
    for (int i = 0; i < len;) {
        int n = utf8_len(u, i, len);
        w += advance_px(utf8_code(u + i, n), font_size);
        i += n;
    }
    return w;
}

int text_host_line_height(int font_size)
{
    ensure_fonts();
    return metrics_for(font_size).height;
}

int text_host_paint(const char *s, int len, int font_size, int x, int y,
                    uint32_t *out, int stride, int max_w, int max_h)
{
    ensure_fonts();
    if (g_nfonts == 0)
        return 0;
    tmetrics m = metrics_for(font_size);

    /* One glyph bitmap at a time, into a scratch box. The box is sized for the
     * largest glyph this size can produce (an em plus overhang). */
    int boxw = font_size * 2 + 8;
    int boxh = m.height + 2 * font_size + 8;
    if (boxw < 16) boxw = 16;
    if (boxh < 16) boxh = 16;
    unsigned char *tmp = (unsigned char *)host_alloc((size_t)boxw * (size_t)boxh);

    const uint8_t *u = (const uint8_t *)s;
    int pen = 0;
    int ink = 0;

    for (int i = 0; i < len;) {
        int n = utf8_len(u, i, len);
        uint32_t cp = utf8_code(u + i, n);
        tfont *f = font_for(cp);
        int adv = advance_px(cp, font_size);
        if (cp >= 0x20 && f) {
            float sc = font_scale(f, font_size);
            int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
            stbtt_GetCodepointBitmapBox(&f->info, (int)cp, sc, sc,
                                        &x0, &y0, &x1, &y1);
            int gw = x1 - x0, gh = y1 - y0;
            if (gw > 0 && gh > 0 && gw <= boxw && gh <= boxh) {
                stbtt_MakeCodepointBitmap(&f->info, tmp, gw, gh, gw,
                                          sc, sc, (int)cp);
                int ox = x + pen + x0;
                int oy = y + m.ascent + y0;
                for (int row = 0; row < gh; row++) {
                    int yy = oy + row;
                    if (yy < 0 || yy >= max_h)
                        continue;
                    const unsigned char *src = tmp + (size_t)row * (size_t)gw;
                    uint32_t *dst = out + (size_t)yy * (size_t)stride;
                    for (int col = 0; col < gw; col++) {
                        unsigned a = src[col];
                        if (!a)
                            continue;
                        int xx = ox + col;
                        if (xx < 0 || xx >= max_w)
                            continue;
                        dst[xx] = ((uint32_t)a << 24) | 0x00ffffffu;
                        ink++;
                    }
                }
            }
        }
        pen += adv;
        i += n;
    }
    free(tmp);
    return ink;
}
