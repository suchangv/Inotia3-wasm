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
#include "input.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <sys/stat.h>
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

/* Read the default framebuffer, 4x downsampled, as tightly packed RGB.
 * glReadPixels is the only thing here that sees what the canvas really holds
 * -- everything else reads the engine's own surfaces, which is a different
 * question. */
void host_browser_read_pixels(unsigned char *out, int w, int h, int step)
{
    int dw = w / step, dh = h / step;
    unsigned char *row = (unsigned char *)host_alloc((size_t)w * 4);
    for (int y = 0; y < dh; y++) {
        glReadPixels(0, (h - 1) - y * step, w, 1, GL_RGBA, GL_UNSIGNED_BYTE, row);
        for (int x = 0; x < dw; x++) {
            out[(y * dw + x) * 4 + 0] = row[x * step * 4 + 0];
            out[(y * dw + x) * 4 + 1] = row[x * step * 4 + 1];
            out[(y * dw + x) * 4 + 2] = row[x * step * 4 + 2];
            out[(y * dw + x) * 4 + 3] = row[x * step * 4 + 3];
        }
    }
}
int g_browser_fixed;
EM_JS(void, spike_read_fixed_js, (void), {
    if (typeof Module !== 'undefined' && Module.spikeFixed)
        _spike_set_fixed(1);
});
void spike_set_fixed(int n) { g_browser_fixed = n; }
void host_browser_read_fixed(void) { spike_read_fixed_js(); }

/* ?trace=1 turns on the interpreter's 96-instruction ring. It costs a little
 * on every instruction, so it is off by default -- but it is what turns "the
 * guest jumped into data" into "this instruction did it", and the dump only
 * prints if something goes wrong. */
int g_browser_trace;
EM_JS(void, spike_read_trace_js, (void), {
    if (typeof Module !== 'undefined' && Module.spikeTrace)
        _spike_set_trace(1);
});
void spike_set_trace(int n) { g_browser_trace = n; }
void host_browser_read_trace(void) { spike_read_trace_js(); }

EM_JS(void, spike_read_dump_js, (void), {
    if (typeof Module !== 'undefined' && Module.spikeDumpScreen)
        _spike_set_dump_screen(1);
});
void spike_set_dump_screen(int n) { g_browser_dump_screen = n; }
void host_browser_read_dump(void) { spike_read_dump_js(); }

/* The language the engine should draw in. Android's Java side answered this
 * from Locale.getDefault(); navigator.language is the same notion here, so a
 * Chinese or Korean browser gets the game in Chinese or Korean without a
 * setting. ?lang=ko overrides it, which is how a script the machine's own
 * locale would never reach gets tested. */
static char g_lang_code[32];

EM_JS(void, spike_read_language_js, (char *buf, int cap), {
    var s = (typeof Module !== 'undefined' && Module.spikeLang)
        ? String(Module.spikeLang)
        : (typeof navigator !== 'undefined' && navigator.language)
            ? String(navigator.language) : 'en';
    stringToUTF8(s, buf, cap);
});

const char *host_language(void)
{
    if (!g_lang_code[0])
        spike_read_language_js(g_lang_code, sizeof(g_lang_code));
    return g_lang_code;
}

/* The engine's frame period, overridable from the page (Module.spikeFrameMs).
 * Off by default: moving it changes how fast the world moves -- see patches.c. */
extern int g_frame_period_ms;
EM_JS(void, spike_read_frame_ms_js, (void), {
    if (typeof Module !== 'undefined' && Module.spikeFrameMs)
        _spike_set_frame_ms(Module.spikeFrameMs | 0);
});
void spike_set_frame_ms(int n) { g_frame_period_ms = n; }
void host_browser_read_frame_ms(void) { spike_read_frame_ms_js(); }

void host_browser_set_size(int w, int h) { g_fb_w = w; g_fb_h = h; }

int host_browser_gl_setup(void)
{
    EmscriptenWebGLContextAttributes attr;
    emscripten_webgl_init_context_attributes(&attr);
    /* alpha = 0, not 1. With an alpha channel the canvas is *composited* over
     * the page, and the engine's frames carry alpha well below 1 -- so the
     * whole picture came out washed toward the white page behind it, which
     * reads as "the colours are wrong" rather than "the canvas is
     * translucent". The game never wants a see-through screen. */
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


/* ---- app storage ---------------------------------------------------------
 *
 * MEMFS starts with almost nothing, so the directory has to be made before the
 * engine writes into it. Only the base is created here; the engine mkdirs the
 * per-package directory itself, through the shim.
 */
const char *host_app_data_dir(const char *package)
{
    static int once;
    if (!once) {
        once = 1;
        mkdir("/data", 0777);                       /* EEXIST is fine */
        char pkg[128];
        snprintf(pkg, sizeof(pkg), "/data/%s", package);
        mkdir(pkg, 0777);
    }
    return "/data/";
}

/* ---- durable storage -----------------------------------------------------
 *
 * /data is IDBFS on this platform (tools/idbfs.js mounts it before main runs),
 * so a write there is invisible to the browser until FS.syncfs pushes it to
 * IndexedDB. Before this existed, a reload came back to an empty /data: saves
 * were gone and the engine reinstalled its 12 asset files every time.
 *
 * The loader tells us what changed; this decides when that becomes durable.
 * Three triggers, and they are not redundant:
 *
 *   fsync         the engine's own "the save is committed" point, so sync now;
 *   the frame tick  catches writes that never get an fsync, coalesced to at
 *                 most one sync per SPIKE_SYNC_MS so a burst of small writes
 *                 costs one IndexedDB transaction and not fifty;
 *   pagehide / visibilitychange  in the pre-js, because the page can go away
 *                 between frames and C never gets another turn.
 */
#define SPIKE_SYNC_MS 500.0

EM_JS(void, spike_storage_sync, (void), {
    if (typeof FS === 'undefined') return;
    FS.syncfs(false, function (err) {
        if (err) console.error('spike: syncfs failed: ' + err);
    });
});

static int    g_storage_dirty;
static int    g_storage_changes;    /* writes since the last sync, for the log */
static double g_storage_since;      /* when the current dirty burst started */

void host_storage_dirty(const char *path)
{
    /* Only the app's own directory is persisted. The engine also writes
     * scratch files next to the trace output, and those live in MEMFS on
     * purpose -- syncing them would put a per-frame log into IndexedDB. */
    if (!path || strncmp(path, "/data/", 6) != 0)
        return;
    if (!g_storage_dirty)
        g_storage_since = emscripten_get_now();
    g_storage_dirty = 1;
    g_storage_changes++;
    /* Naming the first few paths is what turns "the save is not there after a
     * reload" into an answer: either the path never appears here (the engine
     * wrote somewhere this layer does not watch) or it does and the sync is the
     * thing to look at. */
    if (g_storage_changes <= 12)
        host_log("storage: changed %s\n", path);
}

void host_storage_flush(void)
{
    if (!g_storage_dirty)
        return;
    g_storage_dirty = 0;
    g_storage_since = emscripten_get_now();
    /* MEMFS is only told about bytes that reached it, and libc's stdio buffer
     * sits between the engine's write() and the filesystem. Without this a sync
     * triggered by fsync() would copy the file as it was before the last write. */
    fflush(NULL);
    host_log("storage: syncfs (%d change%s)\n", g_storage_changes,
             g_storage_changes == 1 ? "" : "s");
    g_storage_changes = 0;
    spike_storage_sync();
}

void host_storage_tick(void)
{
    if (g_storage_dirty && emscripten_get_now() - g_storage_since >= SPIKE_SYNC_MS)
        host_storage_flush();
}

/* ---- the main loop -------------------------------------------------------
 *
 * The engine runs one tick per requestAnimationFrame rather than in a
 * synchronous for-loop, so the page stays responsive and the canvas updates
 * continuously.
 *
 * emscripten_set_main_loop_arg takes void(*)(void*), so the tick needs a thunk
 * -- casting an int(*)(void) to it would happen to work at runtime and be
 * undefined behaviour.
 *
 * simulate_infinite_loop = 0, which means main() returns normally and the rAF
 * chain outlives it. That is safe here and was checked rather than assumed: the
 * generated glue has noExitRuntime=true (from -sEXIT_RUNTIME=0), so
 * keepRuntimeAlive() is a constant true and _proc_exit never sets ABORT. The
 * throwing variant exists to unwind a stack that would otherwise exit, which is
 * not the situation.
 */
static int (*g_tick)(void);

static void spike_tick_thunk(void *arg)
{
    (void)arg;
    host_storage_tick();
    if (!g_tick())
        emscripten_cancel_main_loop();
}

void host_browser_main_loop(int (*tick)(void))
{
    g_tick = tick;
    emscripten_set_main_loop_arg(spike_tick_thunk, NULL, 0, 0);
}

/* ---- pointer input -------------------------------------------------------
 *
 * Listeners are installed from JS rather than through
 * emscripten_set_touch*_callback / emscripten_set_mouse*_callback, for three
 * concrete reasons:
 *
 *   - emscripten's touch struct is the union of e.touches and e.changedTouches,
 *     so a touch that has *ended* is still in it, and isChanged/onTarget do not
 *     cleanly separate ended from moved. Driving off the event type is simpler
 *     and correct.
 *   - a touch callback registered on the document gets Chrome's default
 *     *passive* listener, where preventDefault() is ignored and the page
 *     scrolls instead of dragging.
 *   - returning false from a touch callback lets Chrome synthesise
 *     compatibility mouse events after every tap, so the same finger would
 *     arrive twice, on two different slots. Pointer events have no such
 *     duplication.
 *
 * One path covers mouse, touch and pen, which is all the engine needs -- it
 * only ever sees five anonymous slots.
 *
 * Coordinates: pointer positions are CSS pixels relative to the canvas, and the
 * canvas backing store is 960x544, so they are scaled by
 * canvas.width / rect.width. That ratio already accounts for devicePixelRatio;
 * multiplying by it as well would double-apply it. It is computed per event
 * rather than cached because the shell's Fullscreen button CSS-resizes the
 * canvas.
 *
 * move/up are registered on the document, not the canvas: a drag that leaves
 * the canvas would otherwise never deliver its release and the engine would
 * keep a finger held down forever. blur/visibilitychange release everything,
 * for the case where the pointer stops existing rather than lifting.
 */
int g_browser_fixed;

EM_JS(void, spike_input_install_js, (void), {
    var canvas = document.getElementById('canvas');
    if (!canvas) { console.error('[input] no #canvas'); return; }
    /* Without this the browser claims touch gestures for scrolling/zooming and
     * the drags never reach us. */
    canvas.style.touchAction = 'none';

    function toEngine(e) {
        var r = canvas.getBoundingClientRect();
        if (!r.width || !r.height) return null;
        var x = (e.clientX - r.left) * (canvas.width  / r.width);
        var y = (e.clientY - r.top)  * (canvas.height / r.height);
        if (x < 0) x = 0; else if (x > 960) x = 960;
        if (y < 0) y = 0; else if (y > 544) y = 544;
        return [x | 0, y | 0];
    }

    function send(kind, e) {
        var p = toEngine(e);
        if (!p) return;
        var t = e.timeStamp || performance.now();
        if (kind === 0)      _spike_input_down(e.pointerId, p[0], p[1], t);
        else if (kind === 1) _spike_input_move(e.pointerId, p[0], p[1], t);
        else                 _spike_input_up(e.pointerId, p[0], p[1], t);
    }

    canvas.addEventListener('pointerdown', function (e) {
        if (e.button !== 0 && e.pointerType === 'mouse') return;
        e.preventDefault();
        if (canvas.setPointerCapture) canvas.setPointerCapture(e.pointerId);
        send(0, e);
    }, { passive: false });

    document.addEventListener('pointermove', function (e) {
        e.preventDefault();
        send(1, e);
    }, { passive: false });

    document.addEventListener('pointerup', function (e) { send(2, e); });
    document.addEventListener('pointercancel', function (e) { send(2, e); });

    function releaseAll() { _spike_input_release_all(); }
    window.addEventListener('blur', releaseAll);
    document.addEventListener('visibilitychange', function () {
        if (document.hidden) releaseAll();
    });
    console.log('[input] pointer listeners installed');
});

void host_browser_input_init(void)
{
    spike_input_install_js();
}

/* The JS side calls these; EM_JS reaches C by symbol name, so they must be
 * exported too (see -sEXPORTED_FUNCTIONS in tools/build.sh).
 *
 * They only queue -- the guest is entered at frame boundaries, from spike_tick,
 * never from a DOM handler. */
void spike_input_down(int id, int x, int y, double t_ms);
void spike_input_move(int id, int x, int y, double t_ms);
void spike_input_up(int id, int x, int y, double t_ms);
void spike_input_release_all(void);
void spike_input_down(int id, int x, int y, double t_ms) { input_push_down(id, x, y, t_ms); }
void spike_input_move(int id, int x, int y, double t_ms) { input_push_move(id, x, y, t_ms); }
void spike_input_up(int id, int x, int y, double t_ms)   { input_push_up(id, x, y, t_ms); }
void spike_input_release_all(void)                       { input_release_all(); }

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
