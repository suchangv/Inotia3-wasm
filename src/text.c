/*
 * text.c -- be the Java side of the engine's text drawing.
 *
 * The call chain, for the record, is the one the parent Vita port traced
 * (source/java.c, "Corrida 59"):
 *
 *   GRPX_DrawString
 *     -> SGL_grpDrawStringWithFont / SGL_grpDrawStringInRectWithFontEx
 *       -> SGL_grpCreateStringImageInRect
 *         -> SGL_Texture_Text::SetTextInRect
 *           -> SGL_Font_Android::RenderToTextureInRect
 *             -> SGL_RenderFontToAlphaImageInRect
 *                  jDrawTextInRect(text, len, fontSize, 0, align, rectWidth, ...)
 *                  puVar = jGetPixelBufferPointer();
 *                  alpha_dest[i] = (byte)(puVar[i] >> 24);   // ARGB -> A
 *
 * and the mirror-image single-line path ends at SGL_RenderFontToAlphaImage ->
 * jDrawText + jGetPixelBufferPointer.
 *
 * jGetPixelBufferPointer is not a JNI call at all: it returns a raw pointer the
 * engine cached during nativeInit() from GetIntArrayElements() on the `pixel[]`
 * array the loader handed it (guest 0x025a36c + 0x34 -- see nativeInit's own
 * `str r2, [r7, #0x34]`). That array is this port's own guest memory, so
 * rasterising into it *is* the render; there is no second JNI round trip to
 * invent.
 *
 * The bytes are UTF-8 -- confirmed, not assumed: with GetLanguage answering
 * "ko" the first draw of the class description is
 * `ea b1 b0 ec b9 a0 ea b3 a0` = "거카고", and the English locale draws plain
 * ASCII. So the text needs a real font stack, per script:
 *
 *   - In the browser the host does it (tools/text.js): Canvas2D, with `serif`
 *     as the family and the browser's own per-script fallback behind it. That
 *     is the same arrangement Android has -- WrapperRenderer.java asks for
 *     Typeface.SERIF and lets the platform find the glyph -- and it is what
 *     gives antialiased Latin *and* Chinese/Korean/Japanese without the port
 *     shipping or downloading a font of its own.
 *   - In the native runner there is no such stack, so the fallback rasteriser
 *     below blits the engine's own bitmap font (FONT_pFontData, the same glyph
 *     records GRP_PutAsciiChar reads). It covers ASCII only, with a
 *     font-size-proportional advance for everything else, and exists to debug
 *     *layout* -- what a player sees is the browser.
 *
 * Layout (wrapping, alignment, line height) stays here in both cases, and asks
 * the host only for two primitives -- the pixel width of a run of text, and
 * the painting of one line -- so the measurement the engine is handed and the
 * pixels it reads back always come from the same source.
 */
#include <stdint.h>

#include "compat.h"    /* the string helpers, declared for freestanding */
#include "guest.h"
#include "falsojni.h"
#include "platform.h"
#include "text.h"

/* ---- the engine's bitmap font (the native stand-in) --------------------- */

#define FONT_GLYPH_OFFSET  0x0b10u   /* first printable glyph, '!'           */
#define FONT_GLYPH_STRIDE  0x0c      /* bytes per glyph record               */
#define FONT_FIRST_CHAR    0x21      /* '!'                                  */
#define FONT_LAST_CHAR     0x7e      /* '~'                                  */
#define FONT_GLYPH_COLS    5
#define FONT_GLYPH_ROWS    10

/* ---- metrics (FONT_GetSubStringWidth) -- the stand-in's only source of a
 * per-character advance. Kept proportional to the font size so a 14px and a
 * 12px call still lay out differently; at 14px these are the engine's own
 * numbers: space 4, narrow 6, wide 9. -------------------------------------- */
#define LINE_HEIGHT    16
#define LINE_TOP       3             /* glyph row 0 sits this far into a line */

static uint32_t g_font_data_var;     /* guest addr of FONT_pFontData         */
static uint32_t g_buffer;            /* guest addr of pixel[]                */
static int      g_buf_w, g_buf_h;
static int      g_buf_ready;

void text_init(uint32_t font_data_var)
{
    g_font_data_var = font_data_var;
}

void text_set_buffer(uint32_t guest_addr, int width, int height)
{
    g_buffer  = guest_addr;
    g_buf_w   = width;
    g_buf_h   = height;
    g_buf_ready = guest_addr != 0;
}

/* ---- the host's text stack (the browser) ---------------------------------
 *
 * Implemented in tools/text.js. `s`/`len` are a UTF-8 run of text that is not
 * NUL-terminated (it is a slice of the engine's own buffer), the guest buffers
 * are passed as host pointers because the tooling writes straight into them. */
#if defined(__EMSCRIPTEN__) || defined(__ANDROID__)
extern int  text_host_width(const char *s, int len, int font_size);
extern int  text_host_line_height(int font_size);
extern int  text_host_paint(const char *s, int len, int font_size, int x, int y,
                            uint32_t *out, int stride, int max_w, int max_h);

static int host_text = 1;
#endif

/* ---- metrics ------------------------------------------------------------- */

/* One UTF-8 character's byte length, resynchronising on a stray continuation
 * byte rather than walking off the end. */
static int utf8_len(const uint8_t *s, int i, int len)
{
    uint8_t c = s[i];
    int n = 1;
    if (c >= 0xf0)
        n = 4;
    else if (c >= 0xe0)
        n = 3;
    else if (c >= 0xc0)
        n = 2;
    if (c >= 0x80 && c < 0xc0)
        n = 1;                                  /* continuation: resync */
    if (i + n > len)
        n = 1;
    return n;
}

/* The stand-in's advance for one character: the engine's own metrics scaled
 * with the requested size (a 14px request reproduces them exactly). Anything
 * multi-byte gets the wide advance and, in the stand-in, no ink. */
static int fallback_advance(const uint8_t *s, int i, int len, int font_size)
{
    int n = utf8_len(s, i, len);
    if (s[i] == ' ')
        return font_size / 3;
    if (n > 1)
        return font_size * 2 / 3;
    return font_size * 3 / 7;
}

/* The pixel width of s[start..end) as one line. The host measures it when
 * there is one (real font metrics, every script); otherwise the stand-in's
 * per-character advances are summed. */
static int run_width(const uint8_t *s, int start, int end, int font_size)
{
    if (end <= start)
        return 0;
#if defined(__EMSCRIPTEN__) || defined(__ANDROID__)
    if (host_text)
        return text_host_width((const char *) (s + start), end - start,
                               font_size);
#endif
    int w = 0;
    for (int i = start; i < end;)
        w += fallback_advance(s, i, end, font_size), i = i + utf8_len(s, i, end);
    return w;
}

/* One line's height. The engine sizes the alpha texture from the height the
 * measurement reports, so this has to be the same number the painting uses. */
static int line_height(int font_size)
{
#if defined(__EMSCRIPTEN__) || defined(__ANDROID__)
    if (host_text)
        return text_host_line_height(font_size);
#endif
    (void) font_size;
    return LINE_HEIGHT;
}

/* ---- wrapping ------------------------------------------------------------ */

typedef void (*line_fn)(void *ctx, int start, int end, int width);

/* A rectWidth of 0 is NOT "a 1px rect". The engine passes 0 when it has no
 * limit and wants the text's natural size: SGL_grpCreateStringImageInRect
 * measures with 0 to decide how big a texture to make, then hands that same
 * width back down as the draw's own rectWidth (SGL_Font_Android::
 * RenderToTextureInRect passes one value to both). Treating 0 as 1px wraps
 * every string one character per line and the texture comes out one pixel
 * wide -- coloured, positioned, and completely blank. So: 0 means unlimited,
 * and the natural single-line width is what comes back.
 *
 * The same greedy wrap serves every script: it prefers the last whitespace on
 * the line, and when there is none it breaks at the character that would have
 * overflowed -- which is exactly how CJK wraps, one character at a time. */
static int unlimited(int rect_width)
{
    return rect_width <= 0;
}

static void wrap_lines(const uint8_t *s, int len, int rect_width, int font_size,
                       line_fn fn, void *ctx)
{
    if (unlimited(rect_width))
        rect_width = 0x7fffffff;

    int i = 0, line_start = 0, line_width = 0;
    int last_space = -1, width_at_space = 0;

    while (i < len) {
        if (s[i] == '\n') {
            fn(ctx, line_start, i, line_width);
            i++;
            line_start = i;
            line_width = 0;
            last_space = -1;
            continue;
        }

        int adv = utf8_len(s, i, len);
        int w   = run_width(s, i, i + adv, font_size);

        if (line_width + w > rect_width && i > line_start) {
            int have_space = last_space > line_start;
            int break_at   = have_space ? last_space : i;
            int used       = have_space ? width_at_space : line_width;
            fn(ctx, line_start, break_at, used);
            i = break_at;
            while (i < len && s[i] == ' ')
                i++;
            line_start = i;
            line_width = 0;
            last_space = -1;
            continue;
        }

        if (s[i] == ' ') {
            last_space = i;
            width_at_space = line_width;
        }
        line_width += w;
        i += adv;
    }
    if (i > line_start)
        fn(ctx, line_start, i, line_width);
}

typedef struct {
    int max_width;
    int lines;
} measure_ctx;

static void measure_line(void *ctx_, int start, int end, int width)
{
    (void) start;
    (void) end;
    measure_ctx *ctx = (measure_ctx *) ctx_;
    if (width > ctx->max_width)
        ctx->max_width = width;
    ctx->lines++;
}

int text_measure_width_height(const char *s, int font_size, int rect_width,
                              int *out_w, int *out_h)
{
    measure_ctx ctx;
    ctx.max_width = 0;
    ctx.lines = 0;
    wrap_lines((const uint8_t *) s, (int) strlen(s), rect_width, font_size,
               measure_line, &ctx);

    int w = ctx.max_width;
    if (!unlimited(rect_width) && w > rect_width)
        w = rect_width;
    if (out_w)
        *out_w = w;
    if (out_h)
        *out_h = ctx.lines * line_height(font_size);
    return w;
}

int text_measure_width(const char *s, int font_size)
{
    return run_width((const uint8_t *) s, 0, (int) strlen(s), font_size);
}

/* The width the same string measures at with no limit -- what a rectWidth<=0
 * call above handed back, and therefore the width of the texture this draw is
 * being read into. */
static int natural_width(const uint8_t *s, int len, int font_size)
{
    return run_width(s, 0, len, font_size);
}

/* ---- drawing ------------------------------------------------------------- */

/* ARGB white. The engine only ever reads byte 31-24 (the alpha), and the real
 * Java side paints pure white too; the colour on screen comes from the
 * engine's own tinting of the resulting alpha texture. */
#define INK 0xffffffffu

static uint32_t *pixel_at(int x, int y, int stride)
{
    if (x < 0 || y < 0 || x >= stride || x >= g_buf_w || y >= g_buf_h)
        return 0;
    uint32_t idx = (uint32_t) y * (uint32_t) stride + (uint32_t) x;
    if (idx >= (uint32_t) g_buf_w * (uint32_t) g_buf_h)
        return 0;
    return (uint32_t *) (void *) (g_mem + g_buffer) + idx;
}

/* Ink pixels blitted by the current call -- reported under --jni-verbose so
 * "the text methods ran" and "glyphs actually landed" are distinguishable. */
static int g_ink;

static const uint8_t *glyph_for(unsigned ch)
{
    if (ch < FONT_FIRST_CHAR || ch > FONT_LAST_CHAR)
        return 0;
    uint32_t base = g_font_data_var ? LD32(g_font_data_var) : 0;
    if (!base)
        return 0;
    return (const uint8_t *) (void *) (g_mem + base + FONT_GLYPH_OFFSET +
                                      (ch - FONT_FIRST_CHAR) * FONT_GLYPH_STRIDE);
}

/* The stand-in's blit, GRP_PutAsciiChar's loop in the host's own terms: rows
 * 0..9 read glyph bytes 1..10, columns 0..4 read bits 7..3, bit clear = ink. */
static int put_glyph(unsigned ch, int x, int y, int stride)
{
    const uint8_t *g = glyph_for(ch);
    if (!g)
        return 0;
    int n = 0;

    for (int row = 0; row < FONT_GLYPH_ROWS; row++) {
        uint8_t bits = g[1 + row];
        for (int col = 0; col < FONT_GLYPH_COLS; col++) {
            if ((bits >> (7 - col)) & 1u)
                continue;                       /* bit set = background */
            uint32_t *px = pixel_at(x + col, y + row, stride);
            if (px) {
                *px = INK;
                n++;
            }
        }
    }
    return n;
}

/* Paint s[start..end) as one line whose left edge and top edge are (x, y).
 * `stride` is the row length the engine reads the buffer back with, which is
 * the rect width, not the buffer's own width. */
static void paint_run(const uint8_t *s, int start, int end, int font_size, int x,
                      int y, int stride)
{
    if (end <= start)
        return;
#if defined(__EMSCRIPTEN__) || defined(__ANDROID__)
    if (host_text) {
        int max_w = stride < g_buf_w ? stride : g_buf_w;
        g_ink += text_host_paint((const char *) (s + start), end - start,
                                 font_size, x, y,
                                 (uint32_t *) (void *) (g_mem + g_buffer),
                                 stride, max_w, g_buf_h);
        return;
    }
#endif
    int at = x;
    for (int i = start; i < end;) {
        int adv = utf8_len(s, i, end);
        if (adv == 1) {
            g_ink += put_glyph(s[i], at, y + LINE_TOP, stride);
            at += fallback_advance(s, i, end, font_size);
        } else {
            /* No stand-in glyph for a multi-byte character: advance over it so
             * the ASCII around it still lands where the engine measured it. */
            at += fallback_advance(s, i, end, font_size);
        }
        i += adv;
    }
}

typedef struct {
    const uint8_t *s;
    int rect_width;
    int align;                                  /* 0 left, 1 centre, 2 right */
    int font_size;
    int y;
} draw_ctx;

static void draw_line(void *ctx_, int start, int end, int width)
{
    draw_ctx *ctx = (draw_ctx *) ctx_;

    int x0 = 0;
    if (ctx->align == 1)
        x0 = (ctx->rect_width - width) / 2;
    else if (ctx->align == 2)
        x0 = ctx->rect_width - width;
    if (x0 < 0)
        x0 = 0;

    paint_run(ctx->s, start, end, ctx->font_size, x0, ctx->y, ctx->rect_width);
    ctx->y += line_height(ctx->font_size);
}

static void clear_rows(int rect_width, int rows)
{
    if (!g_buf_ready || rows <= 0)
        return;
    long want = (long) rect_width * (long) rows;
    long have = (long) g_buf_w * (long) g_buf_h;
    if (want > have)
        want = have;
    if (want > 0)
        memset(g_mem + g_buffer, 0, (size_t) want * sizeof(uint32_t));
}

static void log_text(const char *what, const uint8_t *s, int len, int a, int b,
                     int c)
{
    if (!g_jni_verbose)
        return;
    static int n;
    if (n >= 12)
        return;
    n++;

    char shown[80];
    int  k = 0;
    for (int i = 0; i < len && k < (int) sizeof(shown) - 1; i++)
        shown[k++] = (s[i] >= 0x20 && s[i] < 0x7f) ? (char) s[i] : '.';
    shown[k] = 0;

    int hi = 0;
    for (int i = 0; i < len; i++)
        if (s[i] & 0x80)
            hi++;

    host_log("  [text] %s len=%d args=%d,%d,%d high=%d \"%s\"\n", what, len, a,
             b, c, hi, shown);
}

int text_draw_in_rect(const uint8_t *s, int len, int font_size, int align,
                      int rect_width)
{
    if (!g_buf_ready || !s || len <= 0)
        return 0;
    if (unlimited(rect_width))
        rect_width = natural_width(s, len, font_size);
    if (rect_width <= 0)
        rect_width = 1;

    log_text("DrawTextInRect", s, len, font_size, align, rect_width);

    /* The buffer is one shared scratch area reused by every call, and both a
     * shorter string and a previous longer one start at index 0, so the region
     * this call is about to be read back over has to be cleared first or old
     * ink bleeds through. Measure first so the cleared region is exactly the
     * one about to be drawn. */
    measure_ctx m;
    m.max_width = 0;
    m.lines = 0;
    wrap_lines(s, len, rect_width, font_size, measure_line, &m);
    clear_rows(rect_width, m.lines * line_height(font_size));

    draw_ctx ctx;
    ctx.s = s;
    ctx.rect_width = rect_width;
    ctx.align = align;
    ctx.font_size = font_size;
    ctx.y = 0;
    g_ink = 0;
    wrap_lines(s, len, rect_width, font_size, draw_line, &ctx);

    if (g_jni_verbose) {
        static int nd;
        if (nd < 6) {
            nd++;
            host_log("  [text]   %d ink pixels, stride %d, %d line(s), %dpx "
                     "#%d\n", g_ink, rect_width, m.lines,
                     line_height(font_size), font_size);
        }
    }
    return 0;
}

int text_draw(const uint8_t *s, int len, int font_size, int stride)
{
    if (!g_buf_ready || !s || len <= 0)
        return 0;

    /* SGL_RenderFontToAlphaImage's own texture is one line wide and as tall as
     * the text, so the stride it reads back with is the text's own width. The
     * caller passes that width; fall back to measuring if it did not. */
    if (stride <= 0)
        stride = natural_width(s, len, font_size);
    if (stride <= 0)
        return 0;

    log_text("DrawText", s, len, font_size, stride, 0);

    clear_rows(stride, line_height(font_size));

    draw_ctx ctx;
    ctx.s = s;
    ctx.rect_width = stride;
    ctx.align = 0;
    ctx.font_size = font_size;
    ctx.y = 0;
    wrap_lines(s, len, stride, font_size, draw_line, &ctx);
    return 0;
}
