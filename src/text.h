/*
 * text.h -- the Java-side text renderer the engine expects.
 *
 * libgame.so does not draw text itself. SGL_Font_Android::RenderToTexture and
 * RenderToTextureInRect call jDrawText/jDrawTextInRect, which hand the string
 * to *Java* as a byte[] (UTF-8 -- see text.c), and then read the result back
 * out of one shared ARGB scratch buffer (jGetPixelBufferPointer), keeping only
 * the alpha byte of each pixel. On Android that Java side is Canvas.drawText
 * with the platform's own fonts, which is how the game gets every language's
 * glyphs, CJK included, without shipping a font of its own.
 *
 * There is no Java here, so the port is that renderer: in the browser the
 * platform's text stack again (Canvas2D in tools/text.js, `serif` -- the
 * typeface the game's own WrapperRenderer.java asks for), and in the native
 * runner a stand-in built from the engine's bitmap font, which is good enough
 * to debug layout with but is not what a player sees.
 */
#ifndef TEXT_H
#define TEXT_H

#include <stdint.h>

/* Register the guest address of the `FONT_pFontData` *variable* (not its
 * value: that is only set once FONT_Load runs, well after boot). The native
 * stand-in font reads it; the browser backend does not. */
void text_init(uint32_t font_data_var);

/* Register the shared scratch buffer the engine reads text back from -- the
 * `pixel[]` array handed to nativeInit(), which the guest caches the payload
 * pointer of itself. Called once, right where that array is made. */
void text_set_buffer(uint32_t guest_addr, int width, int height);

/* WrapperJinterface.GetStringWidthHeightInRect(String, int fontSize, int
 * fontStyle, int rectWidth, int lineBreak, int[] values): int -- writes
 * {max line width, total height} into `values` and returns the width. */
int text_measure_width_height(const char *s, int font_size, int rect_width,
                              int *out_w, int *out_h);

/* WrapperJinterface.GetStringWidth(String, int fontSize, int fontStyle): int
 * -- flat, no wrap. */
int text_measure_width(const char *s, int font_size);

/* WrapperJinterface.DrawTextInRect(byte[], int fontSize, int fontStyle, int
 * align, int rectWidth, int isBreak): int -- wraps at `rect_width`, aligns
 * within it, and rasterises into the shared buffer. */
int text_draw_in_rect(const uint8_t *s, int len, int font_size, int align,
                      int rect_width);

/* WrapperJinterface.DrawText(byte[], int fontSize, int stride): int -- the
 * single-line path (SGL_RenderFontToAlphaImage). */
int text_draw(const uint8_t *s, int len, int font_size, int stride);

#endif /* TEXT_H */
