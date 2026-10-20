/*
 * gl.h -- GLES1 state bookkeeping.
 *
 * This is NOT the renderer. It is the layer that lets the engine believe it has
 * a GL context so its own logic can run: texture object IDs, bindings, the
 * enable bits, and the queries the engine makes about its environment.
 *
 * The distinction matters because it splits the GL work into two very
 * different jobs:
 *
 *   this file     bookkeeping. Bounded, mechanical, and testable by whether
 *                 the guest stops crashing. ~58 entry points, most of them one
 *                 line of state.
 *
 *   the renderer  GLES1 fixed function -> WebGL2. Vertex arrays, the matrix
 *                 stack, lighting, texture environments, the fixed-point
 *                 `x`-suffixed calls, compressed textures, render-to-texture.
 *                 This is the 6-10 week item, and it is where the browser port
 *                 either works or does not.
 *
 * Draw calls are counted rather than performed. That count is one of the two
 * numbers this whole spike exists to produce -- draw calls per frame is what
 * says whether a browser can keep up.
 */
#ifndef GL_H
#define GL_H

#include <stdint.h>

/* Called once, before any guest code. Registers the GL state. */
void gl_state_init(void);

/* True if `name` is a GL entry point this layer answers. */
int gl_is_entry(const char *name);

/* Answer a GL call. `args` is the guest's r0..r3 followed by its stack words. */
void gl_host_call(const char *name, const uint32_t *args, int nargs,
                  uint32_t *ret_lo);

/* Count a crossing into a GL entry point. Called by the dispatcher for every
 * GL name, so the report covers what the guest used rather than what this
 * layer happens to have a case for. */
void gl_note_call(const char *name);

/* Report the GL activity: draw calls, texture objects, and which entry points
 * the guest leans on. */
void gl_report(int top_n);

/* An optional real backend.
 *
 * This file is a state tracker: enough for the engine to run, not enough to
 * draw. A backend that can actually render registers itself here and gets every
 * entry point first -- see gl4es_bridge.c, which forwards to gl4es. The
 * indirection exists so that the browser build can link a renderer while the
 * other two builds (and the differential test) keep working without one.
 *
 * Returns 1 if it handled the call. */
extern int (*g_gl_backend)(const char *name, const uint32_t *args,
                           int nargs, uint32_t *ret);

#if defined(__EMSCRIPTEN__) || defined(__ANDROID__)
/* Browser build only: bring gl4es up. See gl4es_bridge.c and docs/REUSE.md.
 * The Android build needs it for the same reason -- gl4es with NOEGL is told
 * the framebuffer size and initialised by the host. */
void gl4es_bridge_init(int fb_w, int fb_h);
#endif

/* Live counters, for the per-frame measurement. */
extern uint64_t g_gl_draw_calls;
extern uint64_t g_gl_textures_created;
extern uint64_t g_gl_tex_uploads;

#endif /* GL_H */
