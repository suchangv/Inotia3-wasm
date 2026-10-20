/*
 * text_android.c -- the Java-side text renderer, from the platform's own fonts.
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
 * The whole contract is in text.c's text_host_* declarations; the parts that
 * matter here:
 *
 *   - measure and paint must agree pixel for pixel. The engine sizes the alpha
 *     texture from the measurement and then paints into it, so an advance that
 *     differs by one accumulates into clipped text at the end of a line. Both
 *     sides therefore use advance_px(): one rounding of the font's advance per
 *     character, summed in integers.
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

#include "guest.h"
#include "compat.h"
#include "platform.h"

#define STB_TRUETYPE_IMPLEMENTATION
#include "../third_party/stb/stb_truetype.h"

/* Candidate fonts, best coverage first. Order matters: the first one that
 * loads *and* can render a character of the current language wins. The app's
 * own files directory is checked first, so dropping a font.ttf there is enough
 * to support a language the build's device does not ship. */
static const char *k_fonts[] = {
    "./font.ttf",
    "./font.ttc",
    "/system/fonts/NotoSansCJK-Regular.ttc",
    "/system/fonts/DroidSansFallback.ttf",
    "/system/fonts/NotoSansSC-Regular.otf",
    "/system/fonts/Roboto-Regular.ttf",
    "/system/fonts/DroidSans.ttf",
    NULL
};

static unsigned char *g_font_bytes;
static stbtt_fontinfo g_font;
static char g_font_path[256];

/* Per-size metrics, cached: the engine asks for a handful of sizes and the
 * scale is a division on every character otherwise. */
#define METRIC_CACHE 8
typedef struct {
    int size;
    float scale;
    int ascent, height;
} metric;
static metric g_metrics[METRIC_CACHE];
static int g_metrics_n;

static int load_file(const char *path, unsigned char **out, long *len)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return 0;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0) {
        fclose(f);
        return 0;
    }
    unsigned char *buf = (unsigned char *)host_alloc((size_t)n);
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) {
        fclose(f);
        return 0;
    }
    fclose(f);
    *out = buf;
    *len = n;
    return 1;
}

/* The probe character for the language, so a font that loads but draws nothing
 * in this language is not chosen. The engine's language codes are the first two
 * letters of host_language(); CJK is the case that matters (Latin-only Roboto
 * has no 中, 가 or あ). */
static uint32_t language_probe(void)
{
    const char *l = host_language();
    if (!l) return 'A';
    if ((l[0] == 'z' || l[0] == 'Z')) return 0x4E2D;   /* 中 */
    if ((l[0] == 'k' || l[0] == 'K')) return 0xAC00;   /* 가 */
    if ((l[0] == 'j' || l[0] == 'J')) return 0x3042;   /* あ */
    return 'A';
}

static void pick_font(void)
{
    if (g_font_bytes)
        return;
    uint32_t probe = language_probe();
    const char *first_loaded = NULL;

    for (int i = 0; k_fonts[i]; i++) {
        unsigned char *bytes = NULL;
        long len = 0;
        if (!load_file(k_fonts[i], &bytes, &len))
            continue;
        int off = 0;
        /* A .ttc is a collection; the first face is the regular one. */
        int noff = stbtt_GetNumberOfFonts(bytes);
        if (noff > 1)
            off = stbtt_GetFontOffsetForIndex(bytes, 0);
        if (off < 0 || !stbtt_InitFont(&g_font, bytes, off)) {
            host_log("text: %s is not a font stb_truetype can read\n",
                     k_fonts[i]);
            free(bytes);
            continue;
        }
        if (!first_loaded) {
            first_loaded = k_fonts[i];
            g_font_bytes = bytes;
            snprintf(g_font_path, sizeof(g_font_path), "%s", k_fonts[i]);
        }
        if (stbtt_FindGlyphIndex(&g_font, probe)) {
            if (g_font_bytes != bytes) {
                free(g_font_bytes);
                g_font_bytes = bytes;
                snprintf(g_font_path, sizeof(g_font_path), "%s", k_fonts[i]);
            }
            host_log("text: using %s (has U+%04X)\n", k_fonts[i], probe);
            return;
        }
        host_log("text: %s has no U+%04X\n", k_fonts[i], probe);
        if (g_font_bytes != bytes) {
            /* Keep the search going; this one loaded but cannot draw the
             * language. Free it unless it is the fallback we kept. */
            free(bytes);
        }
    }
    if (g_font_bytes)
        host_log("text: falling back to %s (no font covers U+%04X)\n",
                 g_font_path, probe);
    else
        host_log("text: no usable system font; text will be blank\n");
}

static metric *get_metric(int size)
{
    for (int i = 0; i < g_metrics_n; i++)
        if (g_metrics[i].size == size)
            return &g_metrics[i];
    pick_font();
    metric m;
    m.size = size;
    if (!g_font_bytes || size <= 0) {
        m.scale = 0.0f;
        m.ascent = size;
        m.height = size;
    } else {
        /* ScaleForPixelHeight makes (ascent - descent) == size, which is what
         * Android's Paint calls the text size. Ceil the two halves separately,
         * like the browser backend does, so the baseline sits on an integer. */
        m.scale = stbtt_ScaleForPixelHeight(&g_font, (float)size);
        int a = 0, d = 0, gap = 0;
        stbtt_GetFontVMetrics(&g_font, &a, &d, &gap);
        m.ascent = (int)ceilf((float)a * m.scale);
        m.height = m.ascent + (int)ceilf((float)-d * m.scale);
        if (m.height < size)
            m.height = size;
    }
    if (g_metrics_n < METRIC_CACHE) {
        g_metrics[g_metrics_n] = m;
        g_metrics_n++;
        return &g_metrics[g_metrics_n - 1];
    }
    /* Full: overwrite the first slot. Sizes come in a small set, and a wrong
     * metric is only ever a stale one for a size that is no longer used. */
    g_metrics[0] = m;
    return &g_metrics[0];
}

/* One UTF-8 character's length, resynchronising on a stray continuation byte.
 * Same rule as text.c's utf8_len(); duplicated rather than exported because it
 * is four lines and the two are in different translation units. */
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

/* The one rounding of a character's advance, used by both measurement and
 * paint. stb's advance is in font units at the design scale. */
static int advance_px(metric *m, uint32_t cp)
{
    if (!g_font_bytes || m->scale <= 0.0f)
        return m->size * 3 / 7;
    int adv = 0, lsb = 0;
    stbtt_GetCodepointHMetrics(&g_font, (int)cp, &adv, &lsb);
    if (adv <= 0)
        adv = 0;
    return (int)((float)adv * m->scale + 0.5f);
}

/* ---- text.c's host hooks ------------------------------------------------- */

int text_host_width(const char *s, int len, int font_size)
{
    metric *m = get_metric(font_size);
    const uint8_t *u = (const uint8_t *)s;
    int w = 0;
    for (int i = 0; i < len;) {
        int n = utf8_len(u, i, len);
        w += advance_px(m, utf8_code(u + i, n));
        i += n;
    }
    return w;
}

int text_host_line_height(int font_size)
{
    return get_metric(font_size)->height;
}

int text_host_paint(const char *s, int len, int font_size, int x, int y,
                    uint32_t *out, int stride, int max_w, int max_h)
{
    metric *m = get_metric(font_size);
    if (!g_font_bytes || m->scale <= 0.0f)
        return 0;

    /* One glyph bitmap at a time. stb writes coverage, which is exactly the
     * alpha value the engine uploads. */
    static const int PAD = 4;
    int boxw = 4 * font_size + PAD;         /* a generous ceiling for one glyph */
    int boxh = m->height + 2 * PAD;
    unsigned char *tmp = (unsigned char *)host_alloc((size_t)(boxw * boxh));

    const uint8_t *u = (const uint8_t *)s;
    int pen = 0;
    int ink = 0;

    for (int i = 0; i < len;) {
        int n = utf8_len(u, i, len);
        uint32_t cp = utf8_code(u + i, n);
        int adv = advance_px(m, cp);
        if (cp >= 0x20) {
            int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
            int gx = x + pen;
            /* Integer placement: subpixel shifts would have to be part of the
             * measurement to stay consistent, and the result here is already
             * one rounding per character. The box form, not GetCodepointBitmap:
             * that one allocates a bitmap per glyph, which at a few hundred
             * strings a frame is an allocator storm. */
            stbtt_GetCodepointBitmapBox(&g_font, (int)cp, m->scale, m->scale,
                                        &x0, &y0, &x1, &y1);
            int gw = x1 - x0, gh = y1 - y0;
            if (gw > 0 && gh > 0 && gw <= boxw && gh <= boxh) {
                stbtt_MakeCodepointBitmap(&g_font, tmp, gw, gh, gw,
                                          m->scale, m->scale, (int)cp);
                int ox = gx + x0;
                int oy = y + m->ascent + y0;
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
