/*
 * text.js -- the browser's text backend for src/text.c (linked in with
 * --js-library; the C side declares these as extern functions).
 *
 * libgame.so hands its strings to Java as UTF-8 bytes and reads back an alpha
 * mask (see text.c). On Android that Java side was WrapperRenderer.java:
 * Canvas.drawText with a Paint whose typeface is Typeface.SERIF, letting the
 * platform find glyphs for every script -- which is why the game shows
 * Chinese, Japanese and Korean without shipping a font. This is the same
 * arrangement: Canvas2D, `serif`, and the browser's own per-script fallback.
 *
 * Layout (wrapping, alignment, line height, the buffer stride) stays in C. It
 * asks this file for two things only: the pixel width of a run of text, and
 * the painting of one line into the engine's scratch buffer.
 *
 * The layout measures character by character and sums, so a line is painted as
 * one string but positioned from that sum: kerning (the one thing a per-glyph
 * sum cannot know) can move a glyph a pixel or two inside its line, which is
 * invisible at the 14px this game draws at, and it keeps the width the engine
 * was told and the pixels it reads back from drifting apart. The width cache
 * exists because that per-character walk is the hot path -- it runs for every
 * character of every string on every frame.
 */
mergeInto(LibraryManager.library, {
    $PText: {
        canvas: null,
        ctx: null,
        family: null,
        widths: {},     /* "size|code point" -> advance */
        metrics: {},    /* size -> { ascent, descent, height } */

        ensure: function () {
            if (!PText.ctx) {
                PText.canvas = (typeof OffscreenCanvas !== 'undefined')
                    ? new OffscreenCanvas(8, 8)
                    : document.createElement('canvas');
                PText.ctx = PText.canvas.getContext('2d',
                                                    { willReadFrequently: true });
            }
            if (PText.family === null) {
                /* The page may override it (?font=...); see tools/page-options.js. */
                PText.family = (typeof Module !== 'undefined' &&
                                Module.textFontFamily)
                    ? Module.textFontFamily : 'serif';
            }
            return PText.ctx;
        },

        setFont: function (size) {
            PText.ctx.font = size + 'px ' + PText.family;
        },

        /* Ascent/descent of the resolved face, which is what a line's height
         * is made of -- the same numbers Android's Paint.FontMetrics would
         * report. Falls back to the usual proportion if the browser does not
         * give them (fontBoundingBox* is newish). */
        face: function (size) {
            var m = PText.metrics[size];
            if (m)
                return m;
            PText.setFont(size);
            var t = PText.ctx.measureText('Mg');
            var asc = t.fontBoundingBoxAscent;
            var desc = t.fontBoundingBoxDescent;
            if (!(asc > 0))
                asc = Math.ceil(size * 0.8);
            if (!(desc >= 0))
                desc = Math.ceil(size * 0.25);
            m = { ascent: Math.ceil(asc), descent: Math.ceil(desc),
                  height: Math.ceil(asc + desc) };
            PText.metrics[size] = m;
            return m;
        },

        advance: function (ch, size) {
            var k = size + '|' + ch;
            var w = PText.widths[k];
            if (w === undefined) {
                PText.setFont(size);
                w = PText.ctx.measureText(ch).width;
                PText.widths[k] = w;
            }
            return w;
        }
    },

    text_host_width__deps: ['$PText'],
    text_host_width: function (ptr, len, fontSize) {
        var s = UTF8ToString(ptr, len);
        if (!s)
            return 0;
        PText.ensure();
        if (Array.from(s).length === 1)     /* the layout's per-character path */
            return Math.round(PText.advance(s, fontSize));
        PText.setFont(fontSize);
        return Math.round(PText.ctx.measureText(s).width);
    },

    text_host_line_height__deps: ['$PText'],
    text_host_line_height: function (fontSize) {
        PText.ensure();
        return PText.face(fontSize).height;
    },

    text_host_paint__deps: ['$PText'],
    text_host_paint: function (ptr, len, fontSize, x, y, outPtr, stride, maxW,
                               maxH) {
        var s = UTF8ToString(ptr, len);
        if (!s)
            return 0;
        var ctx = PText.ensure();
        PText.setFont(fontSize);
        var face = PText.face(fontSize);
        var w = Math.max(1, Math.ceil(ctx.measureText(s).width));
        var h = face.height;

        if (PText.canvas.width < w)
            PText.canvas.width = w;
        if (PText.canvas.height < h)
            PText.canvas.height = h;
        ctx = PText.ctx;            /* a resize resets the context's state */
        ctx.clearRect(0, 0, PText.canvas.width, PText.canvas.height);
        PText.setFont(fontSize);
        ctx.fillStyle = '#fff';
        ctx.textBaseline = 'alphabetic';
        ctx.fillText(s, 0, face.ascent);

        /* Only the alpha channel of the scratch buffer is read (the engine
         * uploads it as GL_ALPHA), and Canvas2D antialiasing is exactly the
         * coverage value that channel wants. */
        var d = ctx.getImageData(0, 0, w, h).data;
        var base = outPtr >> 2;
        var ink = 0;
        for (var row = 0; row < h; row++) {
            var yy = y + row;
            if (yy < 0 || yy >= maxH)
                continue;
            var src = (row * w) * 4 + 3;
            var dst = base + yy * stride;
            for (var col = 0; col < w; col++) {
                var a = d[src + col * 4];
                if (!a)
                    continue;
                var xx = x + col;
                if (xx < 0 || xx >= maxW)
                    continue;
                HEAPU32[dst + xx] = (a << 24) | 0x00ffffff;
                ink++;
            }
        }
        return ink;
    }
});
