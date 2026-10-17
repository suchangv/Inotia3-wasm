/*
 * text.h -- the Java-side text rasteriser the engine expects.
 *
 * libgame.so does not draw text itself. SGL_Font_Android::RenderToTexture and
 * RenderToTextureInRect call jDrawText/jDrawTextInRect, which hand the string
 * to *Java* as a byte[], and then read the result back out of one shared ARGB
 * scratch buffer (jGetPixelBufferPointer) -- keeping only the alpha byte of
 * each pixel. On Android that Java side is Canvas.drawText with the system
 * font, which is also how the game gets CJK glyphs without shipping a
 * rasteriser.
 *
 * There is no Java here, so the port has to be that renderer. It uses the
 * engine's own bitmap font (FONT_pFontData, loaded by FONT_Load) rather than a
 * TrueType rateriser: same glyph lookup the engine's own native text drawing
 * (GRP_PutAsciiChar) uses, so the shapes and metrics match what the rest of
 * the engine already draws. See text.c for the layout and the caveats.
 */
#ifndef TEXT_H
#define TEXT_H

#include <stdint.h>

/* Register the guest address of the `FONT_pFontData` *variable* (not its
 * value: that is only set once FONT_Load runs, well after boot). */
void text_init(uint32_t font_data_var);

/* Register the shared scratch buffer the engine reads text back from -- the
 * `pixel[]` array handed to nativeInit(), which the guest caches the payload
 * pointer of itself. Called once, right where that array is made. */
void text_set_buffer(uint32_t guest_addr, int width, int height);

/* WrapperJinterface.GetStringWidthHeightInRect(String, int, int, int, int,
 * int[]): int -- writes {max line width, total height} into `out` and returns
 * the width. */
int text_measure_width_height(const char *s, int rect_width, int *out_w,
                              int *out_h);

/* WrapperJinterface.GetStringWidth(String, int, int): int -- flat, no wrap. */
int text_measure_width(const char *s);

/* WrapperJinterface.DrawTextInRect(byte[], i, i, i, i, i): int -- wraps at
 * `rect_width`, aligns within it, and rasterises into the shared buffer. */
int text_draw_in_rect(const uint8_t *s, int len, int align, int rect_width);

/* WrapperJinterface.DrawText(byte[], int, int): int -- the single-line path
 * (SGL_RenderFontToAlphaImage). */
int text_draw(const uint8_t *s, int len, int stride);

#endif /* TEXT_H */
