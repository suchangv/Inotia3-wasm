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
 * Glyphs come from the engine's own bitmap font rather than a TrueType
 * rasteriser. The lookup is exactly GRP_PutAsciiChar's (guest 0x6b580):
 *
 *   record = FONT_pFontData + 0xb10 + (ch - 0x21) * 0xc      ch in 0x21..0x7e
 *
 * 12 bytes per glyph, 5 columns from bits 7..3 of each byte, 10 rows, bit
 * CLEAR means ink (GRP_PutAsciiChar skips a pixel whose bit is set, and the
 * empty glyph is memset to 0xff). FONT_pFontData itself is a 4-byte pointer
 * variable, filled in by FONT_Load() -> RES_LoadToPool(), so it is read at
 * draw time and not at init.
 *
 * The per-byte advance is FONT_GetSubStringWidth's (guest 0x6a0ec), which is
 * also the metric the engine's own native text drawing uses: a space is 4px,
 * any byte with bit 7 set starts a 2-byte glyph (EUC-KR and friends) 9px wide,
 * anything else is 6px. Two-byte glyphs have no entry in this ASCII table, so
 * they are advanced over and drawn blank -- visible as correctly-spaced gaps,
 * not as garbage. Making them real means porting GRP_PutHanChar's Johab
 * composition; see the note at the end of this file.
 *
 * The wrap and the line height (16px) are replicated from the parent port,
 * which tuned them against the same engine; the engine measures with one call
 * and draws with the next assuming they agree, so both here go through the
 * same wrap function.
 */
#include <stdint.h>

#include "compat.h"    /* the string helpers, declared for freestanding */
#include "guest.h"
#include "jni.h"
#include "text.h"

/* ---- the engine's bitmap font ------------------------------------------- */

#define FONT_GLYPH_OFFSET  0x0b10u   /* first printable glyph, '!'           */
#define FONT_GLYPH_STRIDE  0x0c      /* bytes per glyph record               */
#define FONT_FIRST_CHAR    0x21      /* '!'                                  */
#define FONT_LAST_CHAR     0x7e      /* '~'                                  */
#define FONT_GLYPH_COLS    5
#define FONT_GLYPH_ROWS    10

/* ---- metrics (FONT_GetSubStringWidth) ----------------------------------- */

#define METRIC_SPACE   4
#define METRIC_NARROW  6
#define METRIC_WIDE    9             /* one 2-byte (high-bit) glyph          */
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

/* One character's advance and width. Same table FONT_GetSubStringWidth uses:
 * space 4, high-bit lead byte 9 and two bytes, everything else 6 and one. */
static int char_advance(const uint8_t *s, int i, int len, int *width)
{
    if (s[i] == ' ') {
        *width = METRIC_SPACE;
        return 1;
    }
    if (s[i] & 0x80) {
        *width = METRIC_WIDE;
        return (i + 1 < len) ? 2 : 1;
    }
    *width = METRIC_NARROW;
    return 1;
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
 * and the natural single-line width is what comes back. */
static int unlimited(int rect_width)
{
    return rect_width <= 0;
}

/* Greedy wrap, preferring the last whitespace seen on the line, which is what
 * the real Java-side word wrap does. A single pass, and the exact same walk
 * measures and draws, so the height the engine is told and the rows it reads
 * back cannot disagree. */
static void wrap_lines(const uint8_t *s, int len, int rect_width,
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

        int w, adv = char_advance(s, i, len, &w);

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

int text_measure_width_height(const char *s, int rect_width, int *out_w,
                              int *out_h)
{
    measure_ctx ctx;
    ctx.max_width = 0;
    ctx.lines = 0;
    wrap_lines((const uint8_t *) s, (int) strlen(s), rect_width, measure_line,
               &ctx);

    int w = ctx.max_width;
    if (!unlimited(rect_width) && w > rect_width)
        w = rect_width;
    if (out_w)
        *out_w = w;
    if (out_h)
        *out_h = ctx.lines * LINE_HEIGHT;
    return w;
}

int text_measure_width(const char *s)
{
    const uint8_t *p = (const uint8_t *) s;
    int len = (int) strlen(s);
    int w = 0;
    for (int i = 0; i < len;) {
        int cw, adv = char_advance(p, i, len, &cw);
        w += cw;
        i += adv;
    }
    return w;
}

/* The width the same string measures at with no limit -- what a
 * rectWidth<=0 call above handed back, and therefore the width of the texture
 * this draw is being read into. */
static int natural_width(const uint8_t *s, int len)
{
    int w = 0;
    for (int i = 0; i < len;) {
        int cw, adv = char_advance(s, i, len, &cw);
        w += cw;
        i += adv;
    }
    return w;
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

/* The blit, GRP_PutAsciiChar's loop in the host's own terms: rows 0..9 read
 * glyph bytes 1..10, columns 0..4 read bits 7..3, bit clear = ink. */
static void put_glyph(unsigned ch, int x, int y, int stride)
{
    const uint8_t *g = glyph_for(ch);
    if (!g)
        return;

    for (int row = 0; row < FONT_GLYPH_ROWS; row++) {
        uint8_t bits = g[1 + row];
        for (int col = 0; col < FONT_GLYPH_COLS; col++) {
            if ((bits >> (7 - col)) & 1u)
                continue;                       /* bit set = background */
            uint32_t *px = pixel_at(x + col, y + row, stride);
            if (px) {
                *px = INK;
                g_ink++;
            }
        }
    }
}

typedef struct {
    const uint8_t *s;
    int rect_width;
    int align;                                  /* 0 left, 1 centre, 2 right */
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

    int x = x0;
    for (int i = start; i < end;) {
        int cw, adv = char_advance(ctx->s, i, end, &cw);
        put_glyph(ctx->s[i], x, ctx->y + LINE_TOP, ctx->rect_width);
        x += cw;
        i += adv;
    }
    ctx->y += LINE_HEIGHT;
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
    if (!g_font_data_var || !LD32(g_font_data_var))
        host_log("  [text]   font data pointer is null\n");
}

int text_draw_in_rect(const uint8_t *s, int len, int align, int rect_width)
{
    if (!g_buf_ready || !s || len <= 0)
        return 0;
    if (unlimited(rect_width))
        rect_width = natural_width(s, len);
    if (rect_width <= 0)
        rect_width = 1;

    log_text("DrawTextInRect", s, len, align, rect_width, 0);

    /* The buffer is one shared scratch area reused by every call, and both a
     * shorter string and a previous longer one start at index 0, so the region
     * this call is about to be read back over has to be cleared first or old
     * ink bleeds through. Measure first so the cleared region is exactly the
     * one about to be drawn. */
    measure_ctx m;
    m.max_width = 0;
    m.lines = 0;
    wrap_lines(s, len, rect_width, measure_line, &m);
    clear_rows(rect_width, m.lines * LINE_HEIGHT);

    draw_ctx ctx;
    ctx.s = s;
    ctx.rect_width = rect_width;
    ctx.align = align;
    ctx.y = 0;
    g_ink = 0;
    wrap_lines(s, len, rect_width, draw_line, &ctx);

    if (g_jni_verbose) {
        static int nd;
        if (nd < 6) {
            nd++;
            host_log("  [text]   %d ink pixels, stride %d, %d line(s)\n", g_ink,
                     rect_width, m.lines);
        }
    }
    return 0;
}

int text_draw(const uint8_t *s, int len, int stride)
{
    if (!g_buf_ready || !s || len <= 0)
        return 0;

    /* SGL_RenderFontToAlphaImage's own texture is one line wide and as tall as
     * the text, so the stride it reads back with is the text's own width. The
     * caller passes that width; fall back to measuring if it did not. */
    if (stride <= 0)
        stride = natural_width(s, len);
    if (stride <= 0)
        return 0;

    log_text("DrawText", s, len, stride, 0, 0);

    clear_rows(stride, LINE_HEIGHT);

    draw_ctx ctx;
    ctx.s = s;
    ctx.rect_width = stride;
    ctx.align = 0;
    ctx.y = 0;
    wrap_lines(s, len, stride, draw_line, &ctx);
    return 0;
}

/*
 * Not done: two-byte (high-bit) glyphs. The metric already accounts for them
 * (9px, 2 bytes) so layout is right, but the ASCII table at FONT_pFontData+0xb10
 * has no entry for them and they draw blank. The engine's own Korean path is
 * GRP_PutHanChar (guest 0x6b601, 496 bytes): split the 2-byte code into
 * initial/medial/final jamo with FONT_ConvertKSToJohab (0x6a07d) and the
 * FONT_nCho/Jung/Jong tables, then blit three jamo glyphs from the same font
 * data. Worth doing only if the text this build draws turns out to be
 * multi-byte -- the port answers "en" for GetLanguage, and DrawTextInRect's
 * own arguments are logged under --jni-verbose, so the run says which.
 */
