/*
 * platform_android.c -- host services for the Android build.
 *
 * Fourth platform alongside platform_native.c (desktop), platform_wasm.c
 * (freestanding throughput) and platform_emscripten.c (browser). This one is a
 * NativeActivity: a real Android app whose window, GL context, input queue and
 * storage are the platform's own, with no browser, no WebView and no Java in
 * the frame loop. android_main() lives in main.c, because the engine's loop and
 * its statics are there; this file provides what that loop cannot.
 *
 * The three pieces with a platform shape:
 *
 *   window   EGL + GLES3, because gl4es (built with NOEGL, DEFAULT_ES=2)
 *            generates GLSL ES 3.00 shaders and needs a context that compiles
 *            them. The surface is the device's window, not the 960x544 the
 *            engine believes in, so glViewport is mapped into a centred
 *            letterbox -- see host_adjust_viewport().
 *
 *   input    The AInputQueue from native_app_glue, in window pixels, converted
 *            to the engine's device coordinates on the way into input.c's FIFO.
 *
 *   storage  The app's internal files directory, with libgame.so and
 *            assets.apk copied out of the APK's assets on first run: the
 *            engine reads both through ordinary fopen (see asset_zip.c).
 */

#define _GNU_SOURCE

#include "guest.h"
#include "compat.h"
#include "platform.h"
#include "input.h"
#include "cpu.h"
#include "imports.h"
#include "gl.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <signal.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/system_properties.h>

#include <android/log.h>
#include <android/asset_manager.h>
#include <android/input.h>
#include <android/native_window.h>
#include <android_native_app_glue.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>

#define LOG_TAG "inotia3"

/* The surface the engine is told it has. main.c and jni.c answer
 * GetRealScreenWidth/Height with the same pair, and the engine scales its own
 * 480x320 UI to fit -- so this is the size of the *content*, and the window is
 * whatever the device gives us. */
#define CONTENT_W 960
#define CONTENT_H 544

static struct android_app *g_app;
static int g_created;                 /* EGL context exists */
static int g_have_window;             /* surface exists and is current */
static int g_quit;
static int g_surface_w, g_surface_h;
static int g_gl_inited;               /* gl4es initialised once, after a context */

static EGLDisplay g_dpy = EGL_NO_DISPLAY;
static EGLSurface g_surf = EGL_NO_SURFACE;
static EGLContext g_ctx = EGL_NO_CONTEXT;
static EGLConfig  g_cfg;

/* ---- logging -------------------------------------------------------------
 *
 * logcat, not stdout: an app's stdout is /dev/null, and "the game printed
 * nothing" is the least useful form of a bug report. `adb logcat -s inotia3`
 * is the equivalent of the desktop build's terminal.
 */
static int g_log_ready = 1;

void host_log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    if (g_log_ready)
        __android_log_vprint(ANDROID_LOG_INFO, LOG_TAG, fmt, ap);
    va_end(ap);
    /* Also to stdout, which is where the native build's habits (and any
     * `adb shell run-as` debugging) expect it. */
    va_start(ap, fmt);
    vfprintf(stdout, fmt, ap);
    va_end(ap);
    fflush(stdout);
}

void host_fatal(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    __android_log_vprint(ANDROID_LOG_FATAL, LOG_TAG, fmt, ap);
    va_end(ap);
    /* No exit(): the caller is usually inside the JNI or import layer, and
     * unwinding here would lose the report the run just produced. The engine
     * stops on the first broken invariant anyway. */
}

void *host_alloc(size_t n)
{
    void *p = calloc(1, n ? n : 1);
    if (!p)
        host_fatal("host_alloc(%zu) failed\n", n);
    return p;
}

double host_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

/* ---- guest mmap ---------------------------------------------------------- */
static uint32_t g_mmap_next = GUEST_MMAP_BASE;

uint32_t host_guest_mmap(uint32_t size)
{
    if (!size)
        return 0;
    uint32_t a = (g_mmap_next + 0xFFFu) & ~0xFFFu;
    if (a + size > GUEST_MMAP_BASE + GUEST_MMAP_SIZE) {
        host_log("mmap: arena exhausted (%u requested, %08x used)\n",
                 size, g_mmap_next - GUEST_MMAP_BASE);
        return 0;
    }
    g_mmap_next = a + size;
    return a;
}

/* ---- crash reporting -----------------------------------------------------
 *
 * The guest space is a flat buffer with unchecked accesses, so a bad guest
 * pointer is a host SIGSEGV. Translate it back to a guest PC: on a device there
 * is no debugger attached, and the tombstone alone says nothing about which
 * guest instruction did it.
 */
static void segv_handler(int sig, siginfo_t *info, void *uctx)
{
    (void)sig; (void)uctx;
    uintptr_t bad = (uintptr_t)info->si_addr;
    intptr_t off = (intptr_t)(bad - (uintptr_t)g_mem);
    char buf[512];
    int n = snprintf(buf, sizeof(buf),
                     "\n*** SIGSEGV ***\n"
                     "  fault %p  guest %p..%p  offset %+ld\n"
                     "  guest pc %08x  after %llu instructions\n",
                     (void *)bad, (void *)g_mem, (void *)(g_mem + GUEST_MEM_SIZE),
                     (long)off, g_cpu.r[15],
                     (unsigned long long)g_insn_count);
    for (int i = 0; i < 16 && n > 0 && n < (int)sizeof(buf); i += 4)
        n += snprintf(buf + n, sizeof(buf) - (size_t)n,
                      "  r%-2d=%08x r%-2d=%08x r%-2d=%08x r%-2d=%08x\n",
                      i, g_cpu.r[i], i + 1, g_cpu.r[i + 1],
                      i + 2, g_cpu.r[i + 2], i + 3, g_cpu.r[i + 3]);
    if (n > 0)
        __android_log_write(ANDROID_LOG_FATAL, LOG_TAG, buf);
    cpu_dump_trace();
    host_trace_close();
    _exit(139);
}

void host_install_crash_handler(void)
{
    static struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = segv_handler;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
}

/* ---- storage -------------------------------------------------------------
 *
 * chdir into the app's private files directory once, so every relative path the
 * engine already uses -- "libgame.so", "assets.apk", "./data/<package>/..." --
 * resolves without a single change in the core. That is the same property the
 * desktop build has by accident (its cwd is the repository) and the browser
 * build has by construction (MEMFS).
 */
static char g_data_dir[512];

static int copy_asset(const char *name)
{
    AAsset *a = AAssetManager_open(g_app->activity->assetManager, name,
                                   AASSET_MODE_STREAMING);
    if (!a) {
        host_log("assets: no %s in the APK\n", name);
        return -1;
    }
    off_t len = AAsset_getLength(a);

    /* Already extracted at the right size? Skip. The image is 1.3 MB and the
     * asset archive 22 MB; re-copying them on every launch is wasted work and
     * wasted flash writes. */
    struct stat st;
    if (stat(name, &st) == 0 && st.st_size == len) {
        AAsset_close(a);
        host_log("assets: %s already extracted (%ld bytes)\n", name, (long)len);
        return 0;
    }

    FILE *f = fopen(name, "wb");
    if (!f) {
        host_log("assets: cannot write %s: %s\n", name, strerror(errno));
        AAsset_close(a);
        return -1;
    }
    char buf[64 * 1024];
    off_t total = 0;
    for (;;) {
        int n = AAsset_read(a, buf, sizeof(buf));
        if (n < 0) {
            host_log("assets: read error on %s\n", name);
            fclose(f);
            AAsset_close(a);
            return -1;
        }
        if (n == 0)
            break;
        if (fwrite(buf, 1, (size_t)n, f) != (size_t)n) {
            host_log("assets: write error on %s: %s\n", name, strerror(errno));
            fclose(f);
            AAsset_close(a);
            return -1;
        }
        total += n;
    }
    fclose(f);
    AAsset_close(a);
    host_log("assets: extracted %s (%ld bytes)\n", name, (long)total);
    return 0;
}

int host_android_prepare(void)
{
    const char *dir = g_app->activity->internalDataPath;
    if (!dir) {
        host_log("android: no internalDataPath\n");
        return -1;
    }
    if (chdir(dir) != 0) {
        host_log("android: chdir %s: %s\n", dir, strerror(errno));
        return -1;
    }
    snprintf(g_data_dir, sizeof(g_data_dir), "%s", dir);
    host_log("android: working directory %s\n", dir);

    if (copy_asset("libgame.so") != 0)
        return -1;
    /* The archive is optional in the sense that the engine will boot without
     * it and find no assets; say so rather than failing the launch. */
    copy_asset("assets.apk");
    return 0;
}

const char *host_app_data_dir(const char *package)
{
    static int once;
    if (!once) {
        once = 1;
        mkdir("./data", 0770);
        char pkg[160];
        snprintf(pkg, sizeof(pkg), "./data/%s", package);
        mkdir(pkg, 0770);
    }
    return "./data/";
}

/* ---- locale --------------------------------------------------------------
 *
 * The Android Java layer answered GetLanguage() with Locale.getDefault(), and
 * the engine turns the first two letters into one of its text files. There is
 * no Java here, so ask the platform properties instead: persist.sys.locale is
 * what the user picked ("zh-CN", "ko-KR"), ro.product.locale is the build's
 * default. */
const char *host_language(void)
{
    static char lang[PROP_VALUE_MAX];
    if (!lang[0]) {
        __system_property_get("persist.sys.locale", lang);
        if (!lang[0])
            __system_property_get("ro.product.locale", lang);
        if (!lang[0])
            snprintf(lang, sizeof(lang), "en");
        host_log("android: locale %s\n", lang);
    }
    return lang;
}

/* ---- per-instruction trace ----------------------------------------------- */
static FILE *g_trace_out;

void host_trace_open(const char *path)
{
    g_trace_out = fopen(path, "w");
    if (!g_trace_out) {
        host_log("trace: cannot open %s: %s\n", path, strerror(errno));
        return;
    }
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

/* ---- EGL -----------------------------------------------------------------
 *
 * GLES3 with depth and stencil: the engine clears and masks both, and gl4es
 * emits GLSL ES 3.00. Antialiasing off, because the art is pixel-exact. alpha =
 * 0 so the window is opaque -- the engine's frames carry alpha well below 1 and
 * a translucent window shows the desktop through them.
 */
static int egl_start(void)
{
    if (g_created)
        return 1;

    g_dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (g_dpy == EGL_NO_DISPLAY) {
        host_log("egl: no display\n");
        return 0;
    }
    EGLint major = 0, minor = 0;
    if (!eglInitialize(g_dpy, &major, &minor)) {
        host_log("egl: initialize failed (%04x)\n", eglGetError());
        return 0;
    }
    host_log("egl: %d.%d\n", major, minor);

    /* ES3 first. gl4es' shaders are `#version 300 es`; on an ES2-only device
     * they will not compile and the screen stays empty, so say which we got. */
    const EGLint cfg_attr[] = {
        EGL_SURFACE_TYPE,    EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT_KHR,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_DEPTH_SIZE, 24, EGL_STENCIL_SIZE, 8,
        EGL_NONE
    };
    EGLint n = 0;
    if (!eglChooseConfig(g_dpy, cfg_attr, &g_cfg, 1, &n) || n < 1) {
        host_log("egl: no ES3 config, falling back to ES2\n");
        const EGLint cfg2[] = {
            EGL_SURFACE_TYPE,    EGL_WINDOW_BIT,
            EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
            EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
            EGL_DEPTH_SIZE, 24, EGL_STENCIL_SIZE, 8,
            EGL_NONE
        };
        if (!eglChooseConfig(g_dpy, cfg2, &g_cfg, 1, &n) || n < 1) {
            host_log("egl: no usable config at all\n");
            return 0;
        }
    }

    const EGLint ctx3[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
    g_ctx = eglCreateContext(g_dpy, g_cfg, EGL_NO_CONTEXT, ctx3);
    if (g_ctx == EGL_NO_CONTEXT) {
        host_log("egl: ES3 context failed (%04x), trying ES2\n", eglGetError());
        const EGLint ctx2[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
        g_ctx = eglCreateContext(g_dpy, g_cfg, EGL_NO_CONTEXT, ctx2);
    }
    if (g_ctx == EGL_NO_CONTEXT) {
        host_log("egl: no context\n");
        return 0;
    }
    g_created = 1;
    return 1;
}

static void egl_surface_create(void)
{
    if (!g_created && !egl_start())
        return;
    if (!g_app->window) {
        host_log("egl: window arrived null\n");
        return;
    }
    g_surf = eglCreateWindowSurface(g_dpy, g_cfg, g_app->window, NULL);
    if (g_surf == EGL_NO_SURFACE) {
        host_log("egl: window surface failed (%04x)\n", eglGetError());
        return;
    }
    if (!eglMakeCurrent(g_dpy, g_surf, g_surf, g_ctx)) {
        host_log("egl: makeCurrent failed (%04x)\n", eglGetError());
        return;
    }
    /* Present on vsync: the engine asks for its own frame period (66 ms) and
     * this keeps the loop from spinning. */
    eglSwapInterval(g_dpy, 1);

    EGLint w = 0, h = 0;
    eglQuerySurface(g_dpy, g_surf, EGL_WIDTH, &w);
    eglQuerySurface(g_dpy, g_surf, EGL_HEIGHT, &h);
    g_surface_w = w;
    g_surface_h = h;
    g_have_window = 1;
    host_log("egl: surface %dx%d\n", w, h);

    /* gl4es with NOEGL never sees a context being created, so it has to be told
     * the framebuffer size -- and it uses the engine's, because that is the
     * coordinate space every projection the engine sets is relative to.
     *
     * Nothing may call a GL entry point before this: with STATICLIB gl4es also
     * exports the unprefixed names, the linker binds this file's glGetString to
     * gl4es_glGetString, and that dereferences glstate, which does not exist
     * until initialize_gl4es() has run. Doing it in the other order is a SIGSEGV
     * at 0x1828 with a stack that names no guest code. */
    if (!g_gl_inited) {
        g_gl_inited = 1;
        gl4es_bridge_init(CONTENT_W, CONTENT_H);
        host_log("gl: %s / %s\n",
                 (const char *)glGetString(GL_VERSION),
                 (const char *)glGetString(GL_SHADING_LANGUAGE_VERSION));
    }
    glViewport(0, 0, g_surface_w, g_surface_h);
}

static void egl_surface_destroy(void)
{
    g_have_window = 0;
    if (g_dpy != EGL_NO_DISPLAY) {
        eglMakeCurrent(g_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (g_surf != EGL_NO_SURFACE)
            eglDestroySurface(g_dpy, g_surf);
    }
    g_surf = EGL_NO_SURFACE;
}

/* ---- the letterbox -------------------------------------------------------
 *
 * The engine renders into 960x544 (its own GetRealScreenWidth/Height), the
 * window is the device's. Scale the engine's glViewport into the largest
 * centred 960:544 rectangle, and hand input back through the same mapping. The
 * bars are cleared by the engine's own glClear(COLOR), which covers the whole
 * surface because its scissor is off.
 */
static void letterbox(int *x, int *y, int *w, int *h)
{
    int sw = g_surface_w, sh = g_surface_h;
    if (sw <= 0 || sh <= 0) {
        *x = 0; *y = 0; *w = CONTENT_W; *h = CONTENT_H;
        return;
    }
    /* Integer arithmetic on purpose: the same rect must come back for input
     * mapping, and a float/double round-trip is a way for a touch to land one
     * pixel off the control it visibly hit. */
    int64_t w1 = (int64_t)sh * CONTENT_W / CONTENT_H;
    if (w1 <= sw) {
        *w = (int)w1;
        *h = sh;
        *x = (sw - *w) / 2;
        *y = 0;
    } else {
        int64_t h1 = (int64_t)sw * CONTENT_H / CONTENT_W;
        *w = sw;
        *h = (int)h1;
        *x = 0;
        *y = (sh - *h) / 2;
    }
}

int host_adjust_viewport(int32_t *x, int32_t *y, int32_t *w, int32_t *h)
{
    (void)x; (void)y; (void)w; (void)h;
    int lx, ly, lw, lh;
    letterbox(&lx, &ly, &lw, &lh);
    if (lw <= 0 || lh <= 0)
        return 0;
    /* glViewport's origin is bottom-left, the letterbox is computed top-left
     * (which is how the input coordinates arrive). */
    int vx = lx;
    int vy = g_surface_h - ly - lh;
    glViewport(vx, vy, lw, lh);
    return 1;
}

/* The engine draws nothing outside the letterbox, and an Android surface's
 * buffers are not guaranteed to be preserved: on a 16:10 tablet the top and
 * bottom bars came up as garbage from whatever the buffer held. Clear the whole
 * surface each frame, with the scissor off and the viewport at full size, so the
 * engine's own clear and draws land inside the letterbox on a black field. */
void host_android_begin_frame(void)
{
    if (!g_have_window)
        return;
    glDisable(GL_SCISSOR_TEST);
    /* Full-size viewport for the clear: glClear ignores the viewport, but not
     * the scissor, and this is the only way to be sure the bars are painted on
     * drivers that keep their own scissor. */
    glViewport(0, 0, g_surface_w, g_surface_h);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

    /* Then put the viewport back inside the letterbox even before the engine
     * sets one: a frame that draws without calling glViewport would otherwise
     * fill the whole window and stretch the picture. */
    int lx, ly, lw, lh;
    letterbox(&lx, &ly, &lw, &lh);
    if (lw > 0 && lh > 0)
        glViewport(lx, g_surface_h - ly - lh, lw, lh);
}

void host_android_present(void)
{
    if (g_have_window && g_dpy != EGL_NO_DISPLAY)
        eglSwapBuffers(g_dpy, g_surf);
}

void host_android_window_size(int *w, int *h)
{
    *w = g_surface_w;
    *h = g_surface_h;
}

int host_android_have_window(void) { return g_have_window; }

/* ---- input ---------------------------------------------------------------
 *
 * Window pixels in, engine device coordinates out. The FIFO and the 40 ms /
 * 2 px drag gate live in input.c; this only maps coordinates.
 *
 * Touch ids are the ones the platform gives; input.c maps them onto the five
 * slots the engine indexes by.
 */
static int g_input_down_ids[16];
static int g_input_down_n;

static void to_engine(float fx, float fy, int *ox, int *oy)
{
    int lx, ly, lw, lh;
    letterbox(&lx, &ly, &lw, &lh);
    int x = (int)fx, y = (int)fy;
    x = (x - lx) * CONTENT_W / (lw > 0 ? lw : CONTENT_W);
    y = (y - ly) * CONTENT_H / (lh > 0 ? lh : CONTENT_H);
    if (x < 0) x = 0; else if (x > CONTENT_W) x = CONTENT_W;
    if (y < 0) y = 0; else if (y > CONTENT_H) y = CONTENT_H;
    *ox = x;
    *oy = y;
}

static void push_event(int id, int action, float x, float y, int64_t when_ns)
{
    int ex, ey;
    to_engine(x, y, &ex, &ey);
    double t_ms = (double)when_ns / 1e6;
    if (action == AMOTION_EVENT_ACTION_DOWN ||
        action == AMOTION_EVENT_ACTION_POINTER_DOWN) {
        input_push_down(id, ex, ey, t_ms);
    } else if (action == AMOTION_EVENT_ACTION_UP ||
               action == AMOTION_EVENT_ACTION_POINTER_UP) {
        input_push_up(id, ex, ey, t_ms);
    } else if (action == AMOTION_EVENT_ACTION_MOVE) {
        input_push_move(id, ex, ey, t_ms);
    }
}

static int32_t on_input(struct android_app *app, AInputEvent *ev)
{
    (void)app;
    if (AInputEvent_getType(ev) != AINPUT_EVENT_TYPE_MOTION)
        return 0;
    int32_t action = AMotionEvent_getAction(ev);
    int32_t masked = action & AMOTION_EVENT_ACTION_MASK;
    int64_t when = AMotionEvent_getEventTime(ev) * 1000000LL;   /* ns */
    size_t n = AMotionEvent_getPointerCount(ev);

    if (masked == AMOTION_EVENT_ACTION_CANCEL) {
        input_release_all();
        return 1;
    }

    int idx0 = (action & AMOTION_EVENT_ACTION_POINTER_INDEX_MASK)
             >> AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT;

    for (size_t i = 0; i < n; i++) {
        int32_t id = AMotionEvent_getPointerId(ev, i);
        float x = AMotionEvent_getX(ev, i);
        float y = AMotionEvent_getY(ev, i);
        int32_t act;
        if (masked == AMOTION_EVENT_ACTION_DOWN) {
            act = AMOTION_EVENT_ACTION_DOWN;
        } else if (masked == AMOTION_EVENT_ACTION_UP) {
            act = AMOTION_EVENT_ACTION_UP;
        } else if (masked == AMOTION_EVENT_ACTION_POINTER_DOWN && (int)i == idx0) {
            act = AMOTION_EVENT_ACTION_POINTER_DOWN;
        } else if (masked == AMOTION_EVENT_ACTION_POINTER_UP && (int)i == idx0) {
            act = AMOTION_EVENT_ACTION_POINTER_UP;
        } else if (masked == AMOTION_EVENT_ACTION_MOVE) {
            act = AMOTION_EVENT_ACTION_MOVE;
        } else {
            continue;
        }
        push_event(id, act, x, y, when);
    }
    (void)g_input_down_ids;
    (void)g_input_down_n;
    return 1;
}

/* ---- lifecycle ----------------------------------------------------------- */
static void on_app_cmd(struct android_app *app, int32_t cmd)
{
    switch (cmd) {
    case APP_CMD_INIT_WINDOW:
        host_log("android: window created\n");
        egl_surface_create();
        break;
    case APP_CMD_TERM_WINDOW:
        host_log("android: window destroyed\n");
        egl_surface_destroy();
        break;
    case APP_CMD_GAINED_FOCUS:
        break;
    case APP_CMD_LOST_FOCUS:
        /* A finger that stops existing rather than lifting: the engine would
         * keep it held down forever. */
        input_release_all();
        break;
    case APP_CMD_DESTROY:
        g_quit = 1;
        break;
    }
    (void)app;
}

void host_android_init(struct android_app *app)
{
    g_app = app;
    app->onAppCmd = on_app_cmd;
    app->onInputEvent = on_input;
}

/* Process the looper. `timeout_ms` 0 drains what is pending and returns; a
 * positive value waits up to that long for input or a lifecycle event (which is
 * how the loop sleeps: the display only updates when the engine draws, so there
 * is no vsync blocking on eglSwapBuffers to pace it). With no window it blocks
 * outright, because there is nothing to do until the surface comes back.
 * Returns 0 when the app is going away. */
int host_android_pump(int timeout_ms)
{
    for (;;) {
        if (g_quit || g_app->destroyRequested)
            return 0;
        int events;
        struct android_poll_source *source;
        int timeout = g_have_window ? timeout_ms : -1;
        int id = ALooper_pollOnce(timeout, NULL, &events, (void **)&source);
        if (id == ALOOPER_POLL_TIMEOUT)
            return 1;
        if (source)
            source->process(g_app, source);
        if (id == ALOOPER_POLL_ERROR)
            return 1;
        if (timeout_ms == 0)
            return 1;      /* drain mode: one pass over what is pending */
        timeout_ms = 0;    /* an event woke us: handled, and drain the rest */
    }
}

/* ---- Chrome-style "one frame" hook ---------------------------------------
 *
 * gl4es asks for the framebuffer size before it initialises; the surface is
 * created after that in the lifecycle order, so the size comes from the
 * constants. Kept as a function so the Android build has the same shape as the
 * other hosts that answer this. */
void host_android_main_loop_hint(void) { }
