/*
 * platform_emscripten.c -- host services for the browser build.
 *
 * Third platform alongside platform_native.c (desktop, for development and the
 * oracle) and platform_wasm.c (freestanding, for the pure throughput number).
 * This one is the real thing: emscripten gives it libc, a filesystem, a clock,
 * an event loop, and the GL-to-WebGL mapping, so unlike platform_wasm.c almost
 * nothing has to be hand-written.
 *
 * The core is unchanged between the three -- that is the point of having kept
 * the interpreter, loader and JNI free of platform assumptions.
 */

#include "guest.h"
#include "compat.h"
#include "platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <emscripten.h>
#include <emscripten/html5.h>
#include <GLES2/gl2.h>

/* ---- logging -------------------------------------------------------------
 * Straight to stdout. Emscripten turns that into a console.log line, which the
 * browser's own devtools then make searchable and filterable -- better than
 * anything this file could do with the output itself.
 */
void host_log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stdout, fmt, ap);
    va_end(ap);
    fflush(stdout);
}

void host_fatal(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fputs("fatal: ", stdout);
    vfprintf(stdout, fmt, ap);
    va_end(ap);
    fflush(stdout);
    /* No exit(): the caller is inside an emscripten callback, and unwinding
     * through the JS stack would lose the report the run just produced. */
}

void *host_alloc(size_t n)
{
    void *p = calloc(1, n ? n : 1);
    if (!p)
        host_fatal("host_alloc(%u) failed\n", (unsigned)n);
    return p;
}

double host_now(void)
{
    /* emscripten_get_now() is a double of milliseconds from performance.now(),
     * which is what the frame timings want: the same clock the browser
     * schedules animation on, not wall time. */
    return emscripten_get_now() / 1000.0;
}

/* ---- guest mmap ----------------------------------------------------------
 * The guest's own arena, inside the guest address space -- not the host heap.
 * See guest.h.
 */
uint32_t host_guest_mmap(uint32_t size)
{
    static uint32_t next = GUEST_MMAP_BASE;
    if (!size)
        return 0;
    /* Overflow-safe: a caller-supplied size near 4 GB would wrap `a + size`
     * past the check and hand back an address outside the arena, which the
     * interpreter then writes through. An out-of-bounds wasm trap is how that
     * surfaces -- a long way from the mmap that caused it. */
    if (size > GUEST_MMAP_SIZE) {
        host_log("mmap: refusing %u bytes\n", size);
        return 0;
    }
    uint32_t a = (next + 0xFFFu) & ~0xFFFu;
    if (a < next || a + size < a || a + size > GUEST_MMAP_BASE + GUEST_MMAP_SIZE) {
        host_log("mmap: arena exhausted (%u requested)\n", size);
        return 0;
    }
    next = a + size;
    return a;
}

/* ---- crash reporting -----------------------------------------------------
 * The native build installs a SIGSEGV handler that translates a host fault back
 * into a guest PC, which is what makes a bad guest pointer diagnosable. There
 * is no equivalent here: a fault traps the wasm instance and the browser
 * reports a wasm stack, not a guest one. What survives is the state the
 * interpreter exports (g_cpu, g_fault_pc), so the JS side reads those on a trap
 * -- see tools/run_browser.mjs.
 */
void host_install_crash_handler(void) { }

/* ---- the WebGL context ---------------------------------------------------
 *
 * gl4es is built with NOEGL, so it never creates a context -- something outside
 * it has to. Emscripten does that here, on the canvas the generated HTML
 * provides. Attributes are set for what this engine actually uses: depth and
 * stencil because it clears and masks both, no antialiasing because the art is
 * pixel-exact and MSAA would blur it, and WebGL 1 because gl4es targets GLES2
 * and asking for 2 buys nothing.
 *
 * The default framebuffer *is* the canvas, and the browser presents it when the
 * JS task ends, so there is no swap call anywhere -- rendering during a
 * requestAnimationFrame callback is the whole protocol.
 */
static int g_fb_w = 960, g_fb_h = 544;

/* How many frames to run, overridable from the page (Module.spikeFrames).
 * Reading it needs a value in wasm memory the JS side can write, since
 * exported globals are not directly addressable from the shell. */
int g_browser_frames;
EM_JS(void, spike_read_frames_js, (void), {
    if (typeof Module !== 'undefined' && Module.spikeFrames)
        _spike_set_frames(Module.spikeFrames | 0);
});
void spike_set_frames(int n) { g_browser_frames = n; }

void host_browser_read_frames(void) { spike_read_frames_js(); }

/* Dump the engine's own screen surface to the console, base64, at the end of
 * the run -- see main.c. Off unless the page asks (Module.spikeDumpScreen).
 *
 * It exists because "the canvas is black" is ambiguous: the engine may have
 * drawn nothing, or drawn correctly into a surface that never reached the
 * screen, and a wasm build has no debugger to tell them apart. Decoding this
 * gives the picture the canvas *should* be showing. */
int g_browser_dump_screen;

/* How many GL entry points to name in the log. Zero by default; a misbehaving
 * draw is diagnosed by turning it up, not by reading it on every run. */
int g_browser_gl_trace;
EM_JS(void, spike_read_gl_trace_js, (void), {
    if (typeof Module !== 'undefined' && Module.spikeGlTrace)
        _spike_set_gl_trace(Module.spikeGlTrace | 0);
});
void spike_set_gl_trace(int n) { g_browser_gl_trace = n; }
void host_browser_read_gl_trace(void) { spike_read_gl_trace_js(); }
EM_JS(void, spike_read_dump_js, (void), {
    if (typeof Module !== 'undefined' && Module.spikeDumpScreen)
        _spike_set_dump_screen(1);
});
void spike_set_dump_screen(int n) { g_browser_dump_screen = n; }
void host_browser_read_dump(void) { spike_read_dump_js(); }

void host_browser_set_size(int w, int h) { g_fb_w = w; g_fb_h = h; }

int host_browser_gl_setup(void)
{
    EmscriptenWebGLContextAttributes attr;
    emscripten_webgl_init_context_attributes(&attr);
    attr.alpha = 1;
    attr.depth = 1;
    attr.stencil = 1;
    attr.antialias = 0;
    /* WebGL2, not WebGL1. gl4es generates GLSL ES 3.00 shaders (it emits
     * `#version 300 es`, `layout(...)` and `textureCubeLod`), which only
     * compile against a GLES3 context -- under a WebGL1 context every shader
     * fails with "'#' : unexpected token" and "'layout' : syntax error". The
     * GLES2 calls gl4es makes are a subset of WebGL2, so nothing is lost. */
    attr.majorVersion = 2;
    attr.minorVersion = 0;
    attr.preserveDrawingBuffer = 1;   /* lets a screenshot be taken after a frame */

    EMSCRIPTEN_WEBGL_CONTEXT_HANDLE ctx =
        emscripten_webgl_create_context("#canvas", &attr);
    if (ctx <= 0) {
        host_log("gl: could not create a WebGL context (%d)\n", (int)ctx);
        return 0;
    }
    /* The canvas must be the size the engine thinks the screen is. Emscripten's
     * generated shell sizes it to a default (a few hundred pixels), while
     * WrapperJinterface.GetRealScreenWidth/Height tell the engine 960x544 --
     * so glViewport(0, 0, 960, 544) would draw almost entirely outside the
     * canvas and the page would stay black however correct the rendering is. */
    emscripten_set_canvas_element_size("#canvas", g_fb_w, g_fb_h);

    emscripten_webgl_make_context_current(ctx);
    /* Ask the context what it actually is. Creating it with majorVersion 2 is
     * a request, not a guarantee -- and the failure mode when it is not
     * honoured is silent: gl4es's ES3 shader probe fails, gl4es concludes ES3
     * is unavailable, and then every fixed-function draw compiles against a
     * dialect mismatch and produces nothing. The canvas stays black with no
     * error anywhere near the draw. */
    {
        EmscriptenWebGLContextAttributes got;
        emscripten_webgl_get_context_attributes(ctx, &got);
        host_log("gl: context is WebGL%d.%d (asked for %d.%d)\n",
                 got.majorVersion, got.minorVersion,
                 attr.majorVersion, attr.minorVersion);
    }
    host_log("gl: GL_VERSION=%s\n", (const char *)glGetString(GL_VERSION));
    host_log("gl: GL_SHADING_LANGUAGE_VERSION=%s\n",
             (const char *)glGetString(GL_SHADING_LANGUAGE_VERSION));
    host_log("gl: WebGL context ready, canvas %dx%d\n", g_fb_w, g_fb_h);
    return 1;
}

/* ---- instruction tracing -------------------------------------------------
 * Emscripten has a filesystem (MEMFS by default), so unlike the freestanding
 * build this can actually record a trace -- which matters because the
 * differential test is the main correctness tool and the browser is where the
 * interesting bugs will be.
 */
static FILE *g_trace_out;

void host_trace_open(const char *path)
{
    g_trace_out = fopen(path, "w");
    if (g_trace_out)
        setvbuf(g_trace_out, NULL, _IOFBF, 1 << 20);
}

void host_trace_close(void)
{
    if (g_trace_out) {
        fclose(g_trace_out);
        g_trace_out = NULL;
    }
}

void host_trace_insn(uint32_t pc, const uint32_t *r, uint32_t cpsr)
{
    if (!g_trace_out)
        return;
    fprintf(g_trace_out,
            "%08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x "
            "%08x %08x %08x %08x %08x\n",
            pc, r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7],
            r[8], r[9], r[10], r[11], r[12], r[13], r[14], cpsr);
}
