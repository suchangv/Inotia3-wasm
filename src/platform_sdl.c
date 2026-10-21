/*
 * platform_sdl.c -- host services for the handheld (and for a desktop window).
 *
 * Fifth platform alongside native (headless measurement), wasm (throughput),
 * emscripten (browser) and android (NativeActivity). This one is an SDL2
 * application: SDL2 owns the window and the GL context, the gamepad and the
 * clock, and the port supplies the same host_* interfaces every other platform
 * does. gl4es still does GLES1 -> GLES2 (it is the renderer that is proven by
 * the browser and Android builds), and the engine still believes the screen is
 * 960x544, letterboxed into whatever the device has.
 *
 * Why SDL2 and not KMS/EGL directly: PortMaster firmware already ships SDL2 and
 * routes every port through it -- input, audio and the video driver (KMSDRM) all
 * come for free, and the same binary runs in a window on the development
 * machine, which is where the input mapping is tuned.
 *
 * Input, for a handheld with a stick, a d-pad and ABXY:
 *
 *   left stick     a virtual mouse cursor over the letterboxed picture
 *   L3             mouse button (press/release; holding drags)
 *   everything     the engine is touch-only, so buttons are synthesised as taps
 *   else           at fixed HUD positions -- see the table in docs/PORTMASTER.md
 *
 * The cursor hides itself after a few seconds of no input: the engine draws the
 * game, this file draws the pointer, and a pointer that is always visible over
 * a game that has no notion of one gets in the way.
 */

/* POSIX (sigaction, dlopen) rather than ISO C; ask for it explicitly, and
 * before any include that pulls in the libc feature-test machinery -- the same
 * note as platform_native.c. */
#define _POSIX_C_SOURCE 200809L

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
#include <math.h>
#include <sys/stat.h>
#include <dlfcn.h>

#include <SDL.h>
#include <GLES2/gl2.h>

/* The engine's screen. main.c/jni.c answer GetRealScreenWidth/Height with the
 * same pair; everything here works in that space and scales at the last step. */
#define CONTENT_W 960
#define CONTENT_H 544

/* gl4es' GLES1 entry points, for the cursor sprite. The rest of the GL work is
 * the engine's, through gl4es_bridge.c. */
#define GL4ES(name, ret, params) extern ret gl4es_##name params;
#include "gl4es_protos.h"
#undef GL4ES

/* gl4es needs to be told where GLES2 lives (built with NOEGL). */
extern void initialize_gl4es(void);
extern void set_getmainfbsize(void (*fn)(int *, int *));
extern void set_getprocaddress(void *(*fn)(const char *));

static SDL_Window   *g_win;
static SDL_GLContext g_ctx;
static SDL_GameController *g_pad;
static int g_surface_w, g_surface_h;

/* Rotation. KMSDRM on this panel reports a 720x960 portrait drawable because
 * that is the connector's native mode; the compositor normally rotates it, and
 * SDL2's KMSDRM backend cannot. So when the drawable is portrait and the
 * content is landscape, the engine renders into an FBO at content size and the
 * present pass blits it rotated. 0 = off (the normal path, unchanged). */
static int g_rotate;            /* 0, 90, 180, 270 */
static int g_rotate_given;      /* --rotate=/SPIKE_ROTATE overrides the guess */
static unsigned g_fbo, g_fbo_tex, g_fbo_rb;
static unsigned g_copy_tex;     /* FBO -> texture copy, drawn rotated */
static int g_fbo_ok;
static int g_running = 1;
static int g_gl_inited;

/* Command line (parsed in main.c and handed over). */
int  g_sdl_windowed;          /* windowed instead of fullscreen-desktop */
int  g_sdl_frames;            /* stop after N engine frames, 0 = forever */
const char *g_sdl_shot;       /* write a PPM once this is set */
int  g_sdl_shot_frame;
int  g_sdl_cursor_x = -1, g_sdl_cursor_y = -1;   /* --cursor=x,y (debug) */

/* ---- logging -------------------------------------------------------------
 *
 * stdout when there is a terminal, and every line also goes to inotia3.log when
 * the launcher asked for one (a handheld has no console: the launcher redirects
 * stdout, which is enough, so this file only keeps the plain version).
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
    fprintf(stdout, "fatal: ");
    vfprintf(stdout, fmt, ap);
    va_end(ap);
    fflush(stdout);
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
    static uint64_t freq;
    if (!freq)
        freq = SDL_GetPerformanceFrequency();
    return (double)SDL_GetPerformanceCounter() / (double)freq;
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
 * Same idea as the desktop build: the guest space is a flat buffer with
 * unchecked accesses, so translate a host SIGSEGV back into a guest PC. On a
 * handheld the report is the only debugging channel there is.
 */
static void segv_handler(int sig, siginfo_t *info, void *uctx)
{
    (void)sig; (void)uctx;
    uintptr_t bad = (uintptr_t)info->si_addr;
    intptr_t off = (intptr_t)(bad - (uintptr_t)g_mem);
    fprintf(stderr, "\n*** SIGSEGV ***\n  fault %p  guest %p..%p  offset %+ld\n"
                    "  guest pc %08x  after %llu instructions\n",
            (void *)bad, (void *)g_mem, (void *)(g_mem + GUEST_MEM_SIZE),
            (long)off, g_cpu.r[15],
            (unsigned long long)g_insn_count);
    for (int i = 0; i < 16; i += 4)
        fprintf(stderr, "  r%-2d=%08x r%-2d=%08x r%-2d=%08x r%-2d=%08x\n",
                i, g_cpu.r[i], i + 1, g_cpu.r[i + 1],
                i + 2, g_cpu.r[i + 2], i + 3, g_cpu.r[i + 3]);
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
 * cwd is the port directory (the launcher cd's there), so the engine's own
 * relative paths -- ./data/<package>/... -- work unchanged, which is the same
 * property every other build has.
 */
const char *host_app_data_dir(const char *package)
{
    static int once;
    if (!once) {
        once = 1;
        mkdir("./data", 0775);
        char pkg[200];
        snprintf(pkg, sizeof(pkg), "./data/%s", package);
        mkdir(pkg, 0775);
    }
    return "./data/";
}

/* The locale, as the engine wants it ("zh-CN", "ko-KR", ...). SPIKE_LANG wins so
 * a port can be pinned to one language; otherwise LANG/LC_ALL, which is what the
 * firmware's locale settings end up in. */
const char *host_language(void)
{
    static char lang[32];
    static int once;
    if (once)
        return lang;
    once = 1;
    const char *l = getenv("SPIKE_LANG");
    if (!l || !*l)
        l = getenv("LC_ALL");
    if (!l || !*l)
        l = getenv("LANG");
    if (!l || !*l) {
        snprintf(lang, sizeof(lang), "en");
        return lang;
    }
    /* "zh_CN.UTF-8" -> "zh-CN"; "en_GB" -> "en-GB"; "C" -> "en" */
    if (l[0] == 'C' && (l[1] == 0 || l[1] == '.')) {
        snprintf(lang, sizeof(lang), "en");
        return lang;
    }
    lang[0] = (char)((l[0] >= 'A' && l[0] <= 'Z') ? l[0] + 32 : l[0]);
    lang[1] = (char)((l[1] >= 'A' && l[1] <= 'Z') ? l[1] + 32 : l[1]);
    int n = 2;
    if (l[2] == '_' || l[2] == '-') {
        lang[n++] = '-';
        for (int i = 3; i < 5 && l[i] && l[i] != '.' && l[i] != '@'; i++)
            lang[n++] = (char)((l[i] >= 'a' && l[i] <= 'z') ? l[i] - 32 : l[i]);
    }
    lang[n] = 0;
    return lang;
}

/* ---- per-instruction trace (differential runs) --------------------------- */
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

/* ---- gl4es --------------------------------------------------------------- */

/* One GL entry point from the driver. SDL already loaded it, so this is the
 * right source -- and it is also the only one: the binary links gl4es
 * statically, which means the unprefixed gl* symbols in this file belong to
 * gl4es, not to the driver. */
void *host_gl_proc(const char *name)
{
    void *p = (void *)(uintptr_t)SDL_GL_GetProcAddress(name);
    if (!p) {
        /* The driver's handle, for the entry points SDL's lookup does not
         * return on every firmware (glShaderSource is the one that matters). */
        static void *gles;
        if (!gles)
            gles = dlopen("libGLESv2.so.2", RTLD_NOW | RTLD_LOCAL);
        if (!gles)
            gles = dlopen("libGLESv2.so", RTLD_NOW | RTLD_LOCAL);
        if (gles)
            p = dlsym(gles, name);
    }
    return p;
}

void *host_gl_proc(const char *name);

static void fb_size(int *w, int *h) { *w = CONTENT_W; *h = CONTENT_H; }

static int gl_start(void)
{
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8);

    g_ctx = SDL_GL_CreateContext(g_win);
    if (!g_ctx) {
        /* ES3 refused: gl4es emits GLSL ES 3.00, so this will not render, but
         * saying so is better than dying inside SDL. */
        host_log("gl: no ES3 context (%s), trying ES2\n", SDL_GetError());
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
        g_ctx = SDL_GL_CreateContext(g_win);
    }
    if (!g_ctx) {
        host_log("gl: cannot create a GLES context: %s\n", SDL_GetError());
        return 0;
    }
    SDL_GL_MakeCurrent(g_win, g_ctx);
    SDL_GL_SetSwapInterval(1);

    int w = 0, h = 0;
    SDL_GL_GetDrawableSize(g_win, &w, &h);
    g_surface_w = w;
    g_surface_h = h;
    /* Through the driver's entry point, not the unprefixed symbol: with gl4es
     * linked statically, `glGetString` in this file resolves to gl4es'
     * implementation, which dereferences glstate -- and glstate does not exist
     * until initialize_gl4es() below. */
    {
        const unsigned char *(*get_string)(unsigned) =
            (const unsigned char *(*)(unsigned))(uintptr_t)
                SDL_GL_GetProcAddress("glGetString");
        host_log("gl: %s / %s, drawable %dx%d\n",
                 get_string ? (const char *)get_string(0x1F02 /* GL_VERSION */) : "?",
                 get_string ? (const char *)get_string(0x8B8C /* GLSL */) : "?",
                 w, h);
    }

    set_getmainfbsize(fb_size);
    set_getprocaddress(host_gl_proc);
    initialize_gl4es();
    {
        /* With NOEGL nothing called NewGLState() -- see gl4es_bridge.c. */
        extern void *NewGLState(void *, int);
        NewGLState((void *)(uintptr_t)~(uintptr_t)0, 0);
    }
    gl4es_bridge_init(CONTENT_W, CONTENT_H);
    g_gl_inited = 1;

    /* A portrait drawable with landscape content means the panel's native mode
     * is rotated and nobody rotated it for us: render offscreen and blit
     * rotated. Guessing 90 is what every one of these panels needs; --rotate or
     * SPIKE_ROTATE overrides it (0 disables). */
    if (!g_rotate_given && g_surface_h > g_surface_w)
        g_rotate = 90;
    if (g_rotate == 90 || g_rotate == 270 || g_rotate == 180) {
        glGenTextures(1, &g_fbo_tex);
        glBindTexture(GL_TEXTURE_2D, g_fbo_tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, CONTENT_W, CONTENT_H, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        /* 960x544 is not a power of two: ES2 only allows NPOT textures with
         * CLAMP_TO_EDGE, otherwise the texture is incomplete and samples as
         * black -- which is exactly what a missing line here looks like. */
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, 0x812F /* CLAMP_TO_EDGE */);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, 0x812F);
        glGenTextures(1, &g_copy_tex);
        glBindTexture(GL_TEXTURE_2D, g_copy_tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, CONTENT_W, CONTENT_H, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, 0x812F);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, 0x812F);
        glGenFramebuffers(1, &g_fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, g_fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_2D, g_fbo_tex, 0);
        glGenRenderbuffers(1, &g_fbo_rb);
        glBindRenderbuffer(GL_RENDERBUFFER, g_fbo_rb);
        glRenderbufferStorage(GL_RENDERBUFFER, 0x88F0 /* GL_DEPTH24_STENCIL8 */,
                              CONTENT_W, CONTENT_H);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, 0x821A /* GL_DEPTH_STENCIL_ATTACHMENT */,
                                  GL_RENDERBUFFER, g_fbo_rb);
        g_fbo_ok = (glCheckFramebufferStatus(GL_FRAMEBUFFER) ==
                    GL_FRAMEBUFFER_COMPLETE);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        if (!g_fbo_ok)
            host_log("gl: rotation requested (%d) but the FBO is incomplete; "
                     "falling back to letterbox\n", g_rotate);

        else
            host_log("gl: drawable %dx%d, rendering %dx%d into an FBO and "
                     "rotating %d degrees\n", g_surface_w, g_surface_h,
                     CONTENT_W, CONTENT_H, g_rotate);
    }
    return 1;
}

/* ---- the letterbox -------------------------------------------------------
 * The engine draws 960x544; the panel is whatever the handheld has. Same mapping
 * as the Android build: the biggest centred 960:544 rectangle, and input goes
 * back through it.
 */
static void letterbox(int *x, int *y, int *w, int *h)
{
    int sw = g_surface_w, sh = g_surface_h;
    if (sw <= 0 || sh <= 0) {
        *x = 0; *y = 0; *w = CONTENT_W; *h = CONTENT_H;
        return;
    }
    int64_t w1 = (int64_t)sh * CONTENT_W / CONTENT_H;
    if (w1 <= sw) {
        *w = (int)w1; *h = sh;
        *x = (sw - *w) / 2; *y = 0;
    } else {
        int64_t h1 = (int64_t)sw * CONTENT_H / CONTENT_W;
        *w = sw; *h = (int)h1;
        *x = 0; *y = (sh - *h) / 2;
    }
}

static int rotating(void) { return g_rotate != 0 && g_fbo_ok; }

/* Where the engine draws: the window, letterboxed -- or the whole FBO when the
 * picture is rotated, because then the FBO *is* the content. */
static void content_target(int *x, int *y, int *w, int *h)
{
    if (rotating()) {
        *x = 0; *y = 0; *w = CONTENT_W; *h = CONTENT_H;
        return;
    }
    letterbox(x, y, w, h);
}

static void target_size(int *w, int *h)
{
    if (rotating()) { *w = CONTENT_W; *h = CONTENT_H; }
    else { *w = g_surface_w; *h = g_surface_h; }
}

/* The rotated picture's footprint: the content's axes are swapped, so the
 * aspect ratio to fit is CONTENT_H x CONTENT_W. */
static void letterbox_rot(int *x, int *y, int *w, int *h)
{
    int sw = g_surface_w, sh = g_surface_h;
    int rw = CONTENT_H, rh = CONTENT_W;
    if (sw <= 0 || sh <= 0) { *x = 0; *y = 0; *w = rw; *h = rh; return; }
    int64_t w1 = (int64_t)sh * rw / rh;
    if (w1 <= sw) {
        *w = (int)w1; *h = sh; *x = (sw - *w) / 2; *y = 0;
    } else {
        int64_t h1 = (int64_t)sw * rh / rw;
        *w = sw; *h = (int)h1; *x = 0; *y = (sh - *h) / 2;
    }
}

int host_adjust_viewport(int32_t *x, int32_t *y, int32_t *w, int32_t *h)
{
    (void)x; (void)y; (void)w; (void)h;
    int lx, ly, lw, lh, tw, th;
    content_target(&lx, &ly, &lw, &lh);
    target_size(&tw, &th);
    if (lw <= 0 || lh <= 0)
        return 0;
    glViewport(lx, th - ly - lh, lw, lh);
    return 1;
}

static void cursor_set(int x, int y);

static struct {
    float x, y;                /* content coordinates */
    int visible;
    double last_input;         /* ms, host clock */
    int down;                  /* button held */
} cur = { CONTENT_W / 2.0f, CONTENT_H / 2.0f, 1, 0, 0 };

/* ---- video driver probe --------------------------------------------------
 * "sdl: video driver offscreen" on the device, with no explanation, is one
 * round trip that a log can save: SDL falls back silently when a driver's init
 * fails, so the reason it *refused* KMSDRM never reaches the log. Ask each
 * driver directly and print its own error.
 *
 *   ./inotia3 --vdprobe
 *
 * Harmless everywhere: it opens and closes the video subsystem once per driver
 * and never starts the engine.
 */
/* ---- dlopen probe ---------------------------------------------------------
 * SDL's own message ("Could not initialize OpenGL / GLES library") is set
 * *after* it has thrown away the reason the loader gave. Ask the loader
 * directly: the string dlerror() returns is the whole diagnosis -- a missing
 * object, an undefined symbol, or "cannot allocate memory in static TLS
 * block", which is not a missing file at all and has a different fix.
 */
static void probe_dlopen(const char *name, int global)
{
    void *h = dlopen(name, RTLD_NOW | (global ? RTLD_GLOBAL : RTLD_LOCAL));
    if (!h) {
        const char *e = dlerror();
        printf("  dlopen %-24s FAILED: %s\n", name, e ? e : "(no error)");
        return;
    }
    printf("  dlopen %-24s ok%s\n", name, global ? " (global)" : "");
    dlclose(h);
}

int host_sdl_vdprobe(void)
{
    /* Unbuffered: when a driver's GL path takes the process down, everything
     * printed up to that point must already be in the log. */
    setvbuf(stdout, NULL, _IONBF, 0);

    int n = SDL_GetNumVideoDrivers();
    printf("video drivers: %d\n", n);
    for (int i = 0; i < n; i++)
        printf("  [%d] %s\n", i, SDL_GetVideoDriver(i));
    printf("env: XDG_RUNTIME_DIR=%s WAYLAND_DISPLAY=%s DISPLAY=%s\n",
           getenv("XDG_RUNTIME_DIR") ? getenv("XDG_RUNTIME_DIR") : "(unset)",
           getenv("WAYLAND_DISPLAY") ? getenv("WAYLAND_DISPLAY") : "(unset)",
           getenv("DISPLAY") ? getenv("DISPLAY") : "(unset)");

    /* The libraries the GL path needs, loaded one at a time so their
     * individual verdicts cannot hide behind each other. */
    static const char *libs[] = {
        "libGLESv2.so.2", "libGLESv2.so", "libGLESv1_CM.so.1",
        "libEGL.so.1", "libEGL.so", "libmali.so.1", "libmali-hook.so.1",
        "libgbm.so.1", "libdrm.so.2", "libwayland-client.so.0",
        "libwayland-egl.so.1", "libxkbcommon.so.0",
    };
    printf("libraries:\n");
    for (unsigned i = 0; i < sizeof(libs) / sizeof(libs[0]); i++)
        probe_dlopen(libs[i], 0);

    /* RTLD_GLOBAL first, i.e. what LD_PRELOAD would have done: a vendor GL
     * library that needs initial-exec TLS can only come in before the main
     * thread's TLS block is sized, and the launcher can do exactly this. */
    printf("preload attempt (RTLD_GLOBAL):\n");
    probe_dlopen("libmali.so.1", 1);
    probe_dlopen("libGLESv2.so.2", 1);

    for (int i = 0; i < n; i++) {
        const char *name = SDL_GetVideoDriver(i);
        if (SDL_VideoInit(name) != 0) {
            printf("%-9s init FAILED: %s\n", name, SDL_GetError());
            continue;
        }
        SDL_DisplayMode dm;
        if (SDL_GetDesktopDisplayMode(0, &dm) != 0)
            dm.w = dm.h = 0;
        printf("%-9s init ok (active=%s) desktop %dx%d, %d display(s)\n",
               name, SDL_GetCurrentVideoDriver(), dm.w, dm.h,
               SDL_GetNumVideoDisplays());

        /* A window and a GL context are the two things that actually fail on
         * a handheld: weston holding DRM master shows up here, not in init.
         * Everything is torn down between drivers so one driver's leftovers
         * cannot poison the next report. */
        SDL_Window *w = SDL_CreateWindow(
            "probe", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
            320, 240, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
        if (!w) {
            printf("%-9s   window FAILED: %s\n", name, SDL_GetError());
        } else {
            SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,
                                SDL_GL_CONTEXT_PROFILE_ES);
            SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
            SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
            SDL_GLContext ctx = SDL_GL_CreateContext(w);
            if (!ctx) {
                printf("%-9s   window ok, GL FAILED: %s\n", name,
                       SDL_GetError());
            } else {
                /* No glGetString: this binary has gl4es linked in and its
                 * strings go through gl4es' own init. The real run logs them. */
                printf("%-9s   window ok, GL context created\n", name);
                SDL_GL_DeleteContext(ctx);
                /* The rest are not tested: on this class of device probing
                 * KMSDRM means taking DRM master from the compositor for a
                 * moment, and doing that on every launch is how a port breaks
                 * the compositor it is trying to live inside. */
                SDL_DestroyWindow(w);
                SDL_VideoQuit();
                printf("stopping here: %s works, and the others would take the "
                       "display\n", name);
                return 0;
            }
            SDL_DestroyWindow(w);
        }
        SDL_VideoQuit();
    }
    return 0;
}

/* ---- start-up ------------------------------------------------------------ */

void host_sdl_set_rotation(int deg)
{
    if (deg == 0 || deg == 90 || deg == 180 || deg == 270) {
        g_rotate = deg;
        g_rotate_given = 1;
    }
}

int host_sdl_init(void)
{
    {
        const char *rot = getenv("SPIKE_ROTATE");
        if (rot && *rot)
            host_sdl_set_rotation(atoi(rot));
    }
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_EVENTS) != 0) {
        host_log("sdl: init failed: %s\n", SDL_GetError());
        return 1;
    }
    const char *drv = SDL_GetCurrentVideoDriver();
    host_log("sdl: video driver %s, %d joystick(s)\n",
             drv, SDL_NumJoysticks());
    host_log("sdl: XDG_RUNTIME_DIR=%s WAYLAND_DISPLAY=%s\n",
             getenv("XDG_RUNTIME_DIR") ? getenv("XDG_RUNTIME_DIR") : "(unset)",
             getenv("WAYLAND_DISPLAY") ? getenv("WAYLAND_DISPLAY") : "(unset)");
    /* SDL falls back to a headless driver without saying why the real ones
     * refused. Say it here, once, instead of making the next log reader guess. */
    if (drv && (strcmp(drv, "offscreen") == 0 || strcmp(drv, "dummy") == 0)) {
        host_log("sdl: WARNING headless driver (%d available): "
                 "rerun with --vdprobe to see why the others refused\n",
                 SDL_GetNumVideoDrivers());
    }

    /* Before CreateWindow, not after: SDL loads the GL library while creating an
     * OPENGL window, and without the ES profile set here it would load desktop
     * GL (libGL.so.1). Firmware images ship libGLESv2/libEGL and no libGL, so
     * that ordering makes every window fail with "cannot initialize OpenGL". */
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8);

    Uint32 flags = SDL_WINDOW_OPENGL | SDL_WINDOW_ALLOW_HIGHDPI;
    if (!g_sdl_windowed)
        flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
    g_win = SDL_CreateWindow("Inotia 3", SDL_WINDOWPOS_CENTERED,
                             SDL_WINDOWPOS_CENTERED,
                             g_sdl_windowed ? CONTENT_W : 0,
                             g_sdl_windowed ? CONTENT_H : 0, flags);
    if (!g_win) {
        host_log("sdl: cannot create a window: %s\n", SDL_GetError());
        return 1;
    }
    if (!gl_start())
        return 1;

    /* The first controller that looks like a gamepad. Handhelds expose exactly
     * one; hot-plug is handled in the pump. */
    for (int i = 0; i < SDL_NumJoysticks(); i++) {
        if (SDL_IsGameController(i)) {
            g_pad = SDL_GameControllerOpen(i);
            if (g_pad) {
                host_log("sdl: controller: %s\n",
                         SDL_GameControllerName(g_pad));
                break;
            }
        }
    }
    SDL_ShowCursor(SDL_DISABLE);      /* the port draws its own pointer */
    return 0;
}

void host_sdl_finish(void)
{
    if (g_pad) {
        SDL_GameControllerClose(g_pad);
        g_pad = NULL;
    }
    if (g_ctx) {
        SDL_GL_DeleteContext(g_ctx);
        g_ctx = NULL;
    }
    if (g_win) {
        SDL_DestroyWindow(g_win);
        g_win = NULL;
    }
    SDL_Quit();
}

/* ---- the virtual mouse ---------------------------------------------------
 * The engine's only input is touch, so the cursor exists to place one. It moves
 * at a fixed speed scaled by stick deflection (a linear ramp from the deadzone
 * so small pushes are controllable), and it is the *content* coordinates that
 * count: the pointer is mapped into the same 960x544 box the engine draws into.
 */
#define CURSOR_SPEED   620.0f   /* content px/s at full deflection */
#define CURSOR_DEADZONE 0.18f
#define CURSOR_HIDE_MS 3000.0

static void cursor_nudge(double dt_ms)
{
    if (!g_pad || !cur.visible)
        return;
    Sint16 ax = SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_LEFTX);
    Sint16 ay = SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_LEFTY);
    float fx = ax / 32767.0f, fy = ay / 32767.0f;
    float mag = sqrtf(fx * fx + fy * fy);
    if (mag < CURSOR_DEADZONE)
        return;
    /* Rescale so the first pixel past the deadzone starts slow. */
    float k = (mag - CURSOR_DEADZONE) / (1.0f - CURSOR_DEADZONE);
    if (k > 1.0f) k = 1.0f;
    k = k * k;                       /* gentle near the centre */
    float step = CURSOR_SPEED * k * (float)(dt_ms / 1000.0);
    cur.x += fx / mag * step;
    cur.y += fy / mag * step;
    if (cur.x < 0) cur.x = 0;
    if (cur.x > CONTENT_W - 1) cur.x = CONTENT_W - 1;
    if (cur.y < 0) cur.y = 0;
    if (cur.y > CONTENT_H - 1) cur.y = CONTENT_H - 1;
    cur.last_input = host_now() * 1000.0;
    cur.visible = 1;
}

/* Screen (window) pixels of the cursor, or -1 if it is off the picture. */
static void cursor_screen(int *sx, int *sy)
{
    int lx, ly, lw, lh;
    content_target(&lx, &ly, &lw, &lh);
    *sx = lx + (int)(cur.x * (float)lw / CONTENT_W);
    *sy = ly + (int)(cur.y * (float)lh / CONTENT_H);
}

static void cursor_hide_if_idle(void)
{
    if (!cur.visible)
        return;
    double now = host_now() * 1000.0;
    if (now - cur.last_input > CURSOR_HIDE_MS && !cur.down)
        cur.visible = 0;
}

/* A generated 16x16 arrow: white with a dark outline, so it reads on any
 * background without shipping an image. */
static unsigned g_cursor_tex;

static void cursor_texture(void)
{
    static const char *art[16] = {
        "X               ",
        "XX              ",
        "X.X             ",
        "X..X            ",
        "X...X           ",
        "X....X          ",
        "X.....X         ",
        "X......X        ",
        "X.......X       ",
        "X........X      ",
        "X.....XXXXX     ",
        "X..X..X         ",
        "X.X X..X        ",
        "XX  X..X        ",
        "X    X..X       ",
        "     XXXX       "
    };
    static unsigned char px[16 * 16 * 4];
    for (int y = 0; y < 16; y++)
        for (int x = 0; x < 16; x++) {
            char c = art[y][x];
            unsigned char *p = &px[(y * 16 + x) * 4];
            if (c == 'X') { p[0] = p[1] = p[2] = 20;  p[3] = 255; }
            else if (c == '.') { p[0] = p[1] = p[2] = 255; p[3] = 235; }
            else { p[0] = p[1] = p[2] = 0; p[3] = 0; }
        }
    gl4es_glGenTextures(1, &g_cursor_tex);
    gl4es_glBindTexture(0x0DE1 /* GL_TEXTURE_2D */, g_cursor_tex);
    gl4es_glTexParameteri(0x0DE1, 0x2801, 0x2600 /* LINEAR */);
    gl4es_glTexParameteri(0x0DE1, 0x2800, 0x2600);
    gl4es_glTexImage2D(0x0DE1, 0, 0x1908 /* RGBA */, 16, 16, 0,
                       0x1908, 0x1401 /* UNSIGNED_BYTE */, px);
}

static void cursor_draw(void)
{
    static int have_tex;
    if (!cur.visible || !g_gl_inited)
        return;
    if (!have_tex) {              /* once, after gl4es is up */
        have_tex = 1;
        cursor_texture();
    }
    int sx, sy, tw, th;
    cursor_screen(&sx, &sy);
    int lx, ly, lw, lh;
    content_target(&lx, &ly, &lw, &lh);
    target_size(&tw, &th);
    float scale = (float)lw / CONTENT_W;
    if (scale <= 0.0f) scale = 1.0f;
    float w = 16.0f * scale, h = 16.0f * scale;

    /* glViewport is the whole window here: this pass is raw GLES1 through
     * gl4es, in window pixels, on top of the engine's finished frame. */
    gl4es_glViewport(0, 0, tw, th);
    gl4es_glMatrixMode(0x1701 /* GL_PROJECTION */);
    gl4es_glLoadIdentity();
    gl4es_glOrthof(0.0f, (float)tw, (float)th, 0.0f, -1.0f, 1.0f);
    gl4es_glMatrixMode(0x1700 /* GL_MODELVIEW */);
    gl4es_glLoadIdentity();
    /* The engine leaves depth testing, culling, alpha test and scissor in
     * whatever state its last draw wanted; this pass is 2D on top of a finished
     * frame, so all four have to be off or the quad is silently rejected. */
    gl4es_glDisable(0x0B71 /* GL_DEPTH_TEST */);
    gl4es_glDisable(0x0B44 /* GL_CULL_FACE */);
    gl4es_glDisable(0x0BC0 /* GL_ALPHA_TEST */);
    gl4es_glDisable(0x0C11 /* GL_SCISSOR_TEST */);
    gl4es_glEnable(0x0BE2 /* GL_BLEND */);
    gl4es_glBlendFunc(0x0302 /* GL_SRC_ALPHA */, 0x0303 /* GL_ONE_MINUS_SRC_ALPHA */);
    /* Unit 0 explicitly: the engine's last draw is a *multi-textured* one (the
     * HUD portrait samples unit 1), and the active unit is global state -- bind
     * this texture on the unit the fixed-function pipeline actually samples. */
    gl4es_glActiveTexture(0x84C0 /* GL_TEXTURE0 */);
    gl4es_glEnable(0x0DE1 /* GL_TEXTURE_2D */);
    gl4es_glBindTexture(0x0DE1, g_cursor_tex);
    /* REPLACE, not MODULATE: the engine leaves a texture environment behind
     * (and a vertex colour), and the pointer must not pick either up. */
    gl4es_glTexEnvi(0x2300 /* GL_TEXTURE_ENV */, 0x2200 /* GL_TEXTURE_ENV_MODE */,
                    0x1E01 /* GL_REPLACE */);
    gl4es_glEnable(0x8074 /* GL_VERTEX_ARRAY */);
    gl4es_glEnable(0x8078 /* GL_TEXTURE_COORD_ARRAY */);
    gl4es_glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
    float x0 = (float)sx, y0 = (float)sy;
    float verts[8] = { x0, y0, x0 + w, y0, x0, y0 + h, x0 + w, y0 + h };
    float uv[8] = { 0, 0, 1, 0, 0, 1, 1, 1 };
    gl4es_glVertexPointer(2, 0x1406 /* GL_FLOAT */, 0, verts);
    gl4es_glTexCoordPointer(2, 0x1406, 0, uv);
    gl4es_glDrawArrays(0x0005 /* GL_TRIANGLE_STRIP */, 0, 4);
}

/* ---- input --------------------------------------------------------------- */

static void push_tap(int down, double t_ms)
{
    if (down)
        input_push_down(0, (int)cur.x, (int)cur.y, t_ms);
    else
        input_push_up(0, (int)cur.x, (int)cur.y, t_ms);
    input_push_move(0, (int)cur.x, (int)cur.y, t_ms);
}

/* Move the cursor to a content position (mouse, or the debug keyboard). */
static void cursor_set(int x, int y)
{
    cur.x = (float)x;
    cur.y = (float)y;
    if (cur.x < 0) cur.x = 0;
    if (cur.x > CONTENT_W - 1) cur.x = CONTENT_W - 1;
    if (cur.y < 0) cur.y = 0;
    if (cur.y > CONTENT_H - 1) cur.y = CONTENT_H - 1;
    cur.last_input = host_now() * 1000.0;
    cur.visible = 1;
}

/* Window pixels -> content coordinates. */
static void window_to_content(int wx, int wy, int *cx, int *cy)
{
    int lx, ly, lw, lh;
    if (rotating()) {
        int dx, dy, dw, dh;
        letterbox_rot(&dx, &dy, &dw, &dh);
        if (dw <= 0 || dh <= 0) { *cx = 0; *cy = 0; return; }
        float u = (float)(wx - dx) / (float)dw;   /* across the rotated picture */
        float v = (float)(wy - dy) / (float)dh;   /* down it */
        switch (g_rotate) {
        case 90:  *cx = (int)(v * CONTENT_W);
                  *cy = (int)((1.0f - u) * CONTENT_H); break;
        case 180: *cx = (int)((1.0f - u) * CONTENT_W);
                  *cy = (int)(v * CONTENT_H); break;
        default:  *cx = (int)((1.0f - v) * CONTENT_W);
                  *cy = (int)(u * CONTENT_H); break;
        }
        return;
    }
    letterbox(&lx, &ly, &lw, &lh);
    *cx = (wx - lx) * CONTENT_W / (lw > 0 ? lw : CONTENT_W);
    *cy = (wy - ly) * CONTENT_H / (lh > 0 ? lh : CONTENT_H);
}

static void input_events(double dt_ms)
{
    SDL_Event e;
    double now = host_now() * 1000.0;
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
        case SDL_QUIT:
            g_running = 0;
            break;
        case SDL_KEYDOWN:
            if (e.key.keysym.sym == SDLK_ESCAPE)
                g_running = 0;
            if (e.key.keysym.sym == SDLK_F12 && !g_sdl_shot)
                g_sdl_shot = "shot.ppm";
            /* Keyboard cursor, for testing without a pad. */
            if (e.key.keysym.sym == SDLK_LEFT)  cursor_set((int)cur.x - 8, (int)cur.y);
            if (e.key.keysym.sym == SDLK_RIGHT) cursor_set((int)cur.x + 8, (int)cur.y);
            if (e.key.keysym.sym == SDLK_UP)    cursor_set((int)cur.x, (int)cur.y - 8);
            if (e.key.keysym.sym == SDLK_DOWN)  cursor_set((int)cur.x, (int)cur.y + 8);
            if (e.key.keysym.sym == SDLK_SPACE) { cur.down = 1; push_tap(1, now); }
            break;
        case SDL_KEYUP:
            if (e.key.keysym.sym == SDLK_SPACE) { cur.down = 0; push_tap(0, now); }
            break;
        case SDL_MOUSEMOTION: {
            int cx, cy;
            window_to_content(e.motion.x, e.motion.y, &cx, &cy);
            cursor_set(cx, cy);
            break;
        }
        case SDL_MOUSEBUTTONDOWN:
            if (e.button.button == SDL_BUTTON_LEFT) {
                int cx, cy;
                window_to_content(e.button.x, e.button.y, &cx, &cy);
                cursor_set(cx, cy);
                cur.down = 1;
                push_tap(1, now);
            }
            break;
        case SDL_MOUSEBUTTONUP:
            if (e.button.button == SDL_BUTTON_LEFT) {
                cur.down = 0;
                push_tap(0, now);
            }
            break;
        case SDL_CONTROLLERDEVICEADDED:
            if (!g_pad && SDL_IsGameController(e.cdevice.which)) {
                g_pad = SDL_GameControllerOpen(e.cdevice.which);
                host_log("sdl: controller: %s\n",
                         g_pad ? SDL_GameControllerName(g_pad) : "(failed)");
            }
            break;
        case SDL_CONTROLLERDEVICEREMOVED:
            if (g_pad && e.cdevice.which ==
                    SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(g_pad))) {
                SDL_GameControllerClose(g_pad);
                g_pad = NULL;
            }
            break;
        case SDL_CONTROLLERBUTTONDOWN:
            if (e.cbutton.button == SDL_CONTROLLER_BUTTON_LEFTSTICK) {
                cur.down = 1;
                push_tap(1, now);
            }
            break;
        case SDL_CONTROLLERBUTTONUP:
            if (e.cbutton.button == SDL_CONTROLLER_BUTTON_LEFTSTICK) {
                cur.down = 0;
                push_tap(0, now);
            }
            break;
        default:
            break;
        }
    }
    cursor_nudge(dt_ms);
    cursor_hide_if_idle();
}

/* ---- the frame ----------------------------------------------------------- */

int host_sdl_pump(int timeout_ms)
{
    /* Wait for an event (or the timeout) without burning a core: with no engine
     * frame to present there is no swap to block on. Sleeping here is also what
     * keeps a 15 fps engine off 100% of the CPU. */
    if (timeout_ms > 0) {
        if (SDL_WaitEventTimeout(NULL, timeout_ms) == 0)
            return g_running;
    }
    input_events(1000.0 / 60.0);
    return g_running;
}

void host_sdl_begin_frame(void)
{
    if (g_sdl_cursor_x >= 0) {
        /* --cursor=x,y holds the pointer there (and visible) for a screenshot.
         * SDL delivers a synthetic mouse move at (0,0) on the first poll, so
         * placing it once during start-up is not enough. */
        cursor_set(g_sdl_cursor_x, g_sdl_cursor_y);
        cur.last_input = host_now() * 1000.0 + 3600.0 * 1000.0;
    }
    if (!g_gl_inited)
        return;
    glBindFramebuffer(GL_FRAMEBUFFER,
                      (rotating() && !getenv("SPIKE_SHOT_FB")) ? g_fbo : 0);
    int tw, th;
    target_size(&tw, &th);
    glDisable(GL_SCISSOR_TEST);
    glViewport(0, 0, tw, th);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    int lx, ly, lw, lh;
    content_target(&lx, &ly, &lw, &lh);
    if (lw > 0 && lh > 0)
        glViewport(lx, th - ly - lh, lw, lh);
}

/* Read the framebuffer into a PPM (P6). PPM because a screenshot tool is not
 * something a handheld has, and this needs no library to write. */
static void screenshot(const char *path, int from_fbo)
{
    if (!g_gl_inited)
        return;
    /* from_fbo reads the engine's own frame (upright 960x544); otherwise the
     * window buffer as it stands. Binding happens here only for the FBO, so the
     * window read uses whatever is actually current. */
    if (from_fbo)
        glBindFramebuffer(GL_FRAMEBUFFER, g_fbo);
    int w = g_surface_w, h = g_surface_h;
    if (from_fbo) { w = CONTENT_W; h = CONTENT_H; }
    unsigned char *rgb = (unsigned char *)host_alloc((size_t)w * h * 3);
    unsigned char *row = (unsigned char *)host_alloc((size_t)w * 4);
    for (int y = 0; y < h; y++) {
        glReadPixels(0, h - 1 - y, w, 1, GL_RGBA, GL_UNSIGNED_BYTE, row);
        for (int x = 0; x < w; x++) {
            rgb[((size_t)y * w + x) * 3 + 0] = row[x * 4 + 0];
            rgb[((size_t)y * w + x) * 3 + 1] = row[x * 4 + 1];
            rgb[((size_t)y * w + x) * 3 + 2] = row[x * 4 + 2];
        }
    }
    FILE *f = fopen(path, "wb");
    if (f) {
        fprintf(f, "P6\n%d %d\n255\n", w, h);
        fwrite(rgb, 3, (size_t)w * h, f);
        fclose(f);
        host_log("shot: %s (%dx%d)\n", path, w, h);
    } else {
        host_log("shot: cannot write %s\n", path);
    }
    free(row);
    free(rgb);
}

/* Present the FBO rotated. The rotation lives in the texture coordinates:
 * the quad always covers the letterboxed (axis-swapped) footprint, and which
 * content corner lands on which screen corner decides the direction. */
static void present_rotated(void)
{
    int dx, dy, dw, dh;
    letterbox_rot(&dx, &dy, &dw, &dh);

    /* Copy the frame into a texture gl4es knows about, then draw that rotated.
     * A texture *rendered into* cannot be sampled through gl4es (its cached copy
     * stays empty); a texture filled by the copy inside gl4es can. */
    glActiveTexture(0x84C0 /* GL_TEXTURE0 */);
    glBindTexture(0x0DE1, g_copy_tex);
    glCopyTexSubImage2D(0x0DE1, 0, 0, 0, 0, 0, CONTENT_W, CONTENT_H);

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDisable(GL_SCISSOR_TEST);
    glViewport(0, 0, g_surface_w, g_surface_h);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    gl4es_glMatrixMode(0x1701);
    gl4es_glLoadIdentity();
    gl4es_glOrthof(0.0f, (float)g_surface_w, (float)g_surface_h, 0.0f, -1.0f, 1.0f);
    gl4es_glMatrixMode(0x1700);
    gl4es_glLoadIdentity();
    gl4es_glDisable(0x0B71 /* DEPTH_TEST */);
    gl4es_glDisable(0x0B44);
    gl4es_glDisable(0x0B90);
    gl4es_glDisable(0x0BC0);
    gl4es_glDisable(0x0BE2);
    gl4es_glDisable(0x0C11);
    gl4es_glEnable(0x0DE1);
    gl4es_glBindTexture(0x0DE1, g_copy_tex);
    gl4es_glTexEnvi(0x2300, 0x2200, 0x1E01 /* REPLACE */);
    gl4es_glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
    gl4es_glEnable(0x8074);
    gl4es_glEnable(0x8078);

    float x0 = (float)dx, y0 = (float)dy;
    float x1 = (float)(dx + dw), y1 = (float)(dy + dh);
    float verts[8] = { x0, y0, x1, y0, x0, y1, x1, y1 };
    float uv[8];
    switch (g_rotate) {
    case 90:  uv[0]=0; uv[1]=0; uv[2]=0; uv[3]=1; uv[4]=1; uv[5]=0; uv[6]=1; uv[7]=1; break;
    case 180: uv[0]=1; uv[1]=1; uv[2]=0; uv[3]=1; uv[4]=1; uv[5]=0; uv[6]=0; uv[7]=0; break;
    default:  uv[0]=1; uv[1]=1; uv[2]=1; uv[3]=0; uv[4]=0; uv[5]=1; uv[6]=0; uv[7]=0; break;
    }
    gl4es_glVertexPointer(2, 0x1406, 0, verts);
    gl4es_glTexCoordPointer(2, 0x1406, 0, uv);
    gl4es_glDrawArrays(0x0005, 0, 4);
}

void host_sdl_present(void)
{
    if (!g_gl_inited)
        return;
    cursor_draw();
    if (rotating())
        present_rotated();
    /* SPIKE_SHOT_FB=1: capture what is about to be shown, i.e. the window
     * buffer, instead of the content. The only way to check the rotation
     * without a portrait panel on the bench. */
    if (g_sdl_shot && getenv("SPIKE_SHOT_FB") && g_sdl_shot_frame <= 0) {
        g_sdl_shot_frame = -1;
        screenshot(g_sdl_shot, 0);   /* the window buffer, as drawn */
        g_sdl_shot = NULL;
    }
    SDL_GL_SwapWindow(g_win);
    if (g_sdl_shot) {
        /* --shot=FILE[@FRAME]: FRAME presented frames first, then once. */
        if (g_sdl_shot_frame > 0)
            g_sdl_shot_frame--;
        else {
            screenshot(g_sdl_shot, rotating());
            g_sdl_shot = NULL;
        }
    }
}
