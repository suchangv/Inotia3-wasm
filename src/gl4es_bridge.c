/*
 * gl4es_bridge.c -- the guest's GLES1 calls, forwarded to gl4es.
 *
 * See docs/REUSE.md for why this exists instead of a renderer. In short: gl4es
 * is a MIT-licensed GLES1 -> GLES2 layer that already implements the fixed
 * pipeline, the matrix stack, the texture environment and the fixed-point
 * entry points, and under emscripten its GLES2 output becomes WebGL. Writing
 * that by hand was the 6-10 week item and the project's biggest risk.
 *
 * What is left for this port is the boundary, and there are two reasons it
 * cannot be a cast:
 *
 *  1. **The guest's arguments arrive as a flat word array.** ARM AAPCS puts the
 *     first four in r0-r3 and the rest on the guest stack; the interpreter
 *     hands them over as `args[]`. A C function cannot be called with a word
 *     array of unknown types.
 *
 *  2. **WebAssembly's function signatures are typed, and i32 and f32 are
 *     different.** Casting `void (*)(int)` to `void (*)(float)` and calling it
 *     does not reinterpret the bits -- it produces a signature mismatch the
 *     engine rejects at instantiation. So the float-taking entry points
 *     genuinely need their own adapters, not a generic shim.
 *
 * Guest pointers do not need per-function logic though: the guest address space
 * is a single host buffer, so `guest_addr` -> `g_mem + guest_addr` and the
 * result is a pointer gl4es can read directly.
 *
 * Coverage is deliberately the set the engine actually calls (see the GL
 * activity report from a real run), not all 241. Adding one is a two-line
 * adapter and a line in the dispatch.
 */

#include "gl.h"
#include "platform.h"
#include <stdio.h>
#include "guest.h"
#include "cpu.h"
#include "compat.h"

/* gl4es' core entry points carry a prefix; the unprefixed public names are
 * generated only for its shared-library build. Calling gl4es_gl* directly is
 * both simpler and type-safe -- we declare the prototypes we use. */
#define GL4ES(name, ret, params) extern ret gl4es_##name params;
#include "gl4es_protos.h"
#undef GL4ES

extern void initialize_gl4es(void);
extern void set_getmainfbsize(void (*fn)(int *, int *));
extern void set_getprocaddress(void *(*fn)(const char *));

#ifdef __EMSCRIPTEN__
#include <emscripten/html5.h>
#endif
#if defined(__EMSCRIPTEN__) || defined(__ANDROID__)
/* For the backend prototypes the wrapper below calls. gl4es never sees this
 * header -- it resolves its own pointers -- so there is no aliasing. */
#include <GLES2/gl2.h>
#endif
#ifdef __ANDROID__
#include <EGL/egl.h>
#include <dlfcn.h>
#endif

/* ---- argument helpers ---------------------------------------------------- */

/* A guest address as a host pointer. Address 0 stays NULL: several GL calls
 * treat a null pointer as meaningful (glTexImage2D with NULL allocates storage
 * without uploading), and handing gl4es g_mem+0 instead would make it read the
 * guard region. */
/* Every guest pointer that crosses into gl4es goes through here, and an
 * out-of-range one is checked rather than followed. Without the check a bad
 * pointer is read by gl4es at host level -- an opaque wasm trap with no PC --
 * while with it the address and the calling GL entry point are named. */
/* The engine's own screen surface, remembered so a run can dump it. See
 * gl4es_dump_last_screen() -- it is the ground truth for "did the engine draw
 * anything", which a black canvas cannot answer on its own. */
/* Which texture each draw actually samples.
 *
 * "The splash is drawn and comes out washed out" and "the splash is drawn and
 * something else is sampled" look identical from the canvas. Tracking the
 * binding is what tells them apart, and it is the only way to attribute a
 * wrong-looking draw to a texture in a build with no debugger. */
static uint32_t g_cur_tex;
/* What a texture was created as (id -> w/h/format), for the draw trace only.
 *
 * A session outgrows any fixed table -- every map brings its own atlases, and
 * a long one runs through hundreds of ids -- so a new id with the table full
 * has to *replace* an entry. It used to fall through to `g_tex[i]` with
 * i == the limit, which is a write past the end of this array: it corrupted
 * whatever static followed it in the wasm data segment, and the length of a
 * session decided when. Diagnostics must never be able to do that. */
#define GTEX_MAX 256
static struct { uint32_t id, w, h, fmt; } g_tex[GTEX_MAX];
static int g_ntex, g_tex_next;

/* The last RGBA upload at each of the sizes the engine composes with, so a
 * run can show what a draw is really sampling. */
uint32_t g_u480_ptr, g_u600_ptr;
/* The engine's own screen surface: 384x256 RGB565, blitted every frame. */
uint32_t g_csfb_ptr;

uint32_t g_last_screen_ptr, g_last_screen_w, g_last_screen_h;
uint32_t g_last_screen_fmt, g_last_screen_type;

static inline const void *G(uint32_t guest_addr)
{
    if (!guest_addr)
        return (const void *)0;
    if (guest_addr >= GUEST_MEM_SIZE) {
        static int warned;
        if (warned < 10) {
            warned++;
            host_log("gl: bad guest pointer %08x from caller %08x -- passing "
                     "NULL\n", guest_addr, g_cpu.r[14]);
        }
        return (const void *)0;
    }
    return (const void *)(g_mem + guest_addr);
}
static inline void *GW(uint32_t guest_addr)
{
    if (!guest_addr)
        return (void *)0;
    if (guest_addr >= GUEST_MEM_SIZE) {
        static int warned;
        if (warned < 10) {
            warned++;
            host_log("gl: bad guest pointer %08x (write) from caller %08x -- "
                     "passing NULL\n", guest_addr, g_cpu.r[14]);
        }
        return (void *)0;
    }
    return (void *)(g_mem + guest_addr);
}

/* GLfloat arguments cross the boundary as their bit pattern in a word; the
 * adapter reinterprets rather than converts, because that is what the guest
 * put there. */
static inline float F(uint32_t bits)
{
    union { uint32_t u; float f; } c;
    c.u = bits;
    return c.f;
}

/* GLfixed is 16.16. */
static inline float X(uint32_t fixed)
{
    return (float)(int32_t)fixed / 65536.0f;
}

/* A GLfixed parameter whose meaning depends on pname: the GLES1 fixed-point
 * calls take *enum* parameters too (GL_LINEAR for GL_TEXTURE_MIN_FILTER,
 * GL_MODULATE for GL_TEXTURE_ENV_MODE), and the engine passes those as the
 * enum, not scaled into 16.16. Scaling one gives 0, which the driver rejects
 * with GL_INVALID_ENUM and which silently leaves the state unset. */
static int is_enum_pname(uint32_t pname)
{
    switch (pname) {
    case 0x2800: case 0x2801: case 0x2802: case 0x2803:  /* filter / wrap */
    case 0x2200: case 0x2201:                            /* texture env   */
    case 0x0DE0:                                         /* GL_TEXTURE_2D */
        return 1;
    default:
        return 0;
    }
}

static inline float XP(uint32_t pname, uint32_t fixed)
{
    return is_enum_pname(pname) ? (float)(int32_t)fixed : X(fixed);
}

/* glLoadMatrixx / glMultMatrixx take 16 GLfixed values from guest memory. */
static const float *fixed_matrix(const void *src)
{
    static float m[16];
    const uint32_t *v = (const uint32_t *)src;
    for (int i = 0; i < 16; i++)
        m[i] = X(v[i]);
    return m;
}

/* ---- GL_FIXED client arrays ----------------------------------------------
 *
 * GLES1's most common vertex format, and WebGL's one hard exclusion: the
 * WebGL-friendly GLES2 subset drops GL_FIXED, so `glVertexPointer(..., GL_FIXED,
 * ...)` reaches the driver as GL_INVALID_ENUM and the array is left unbound --
 * which is why every draw then fails with "an enabled vertex array has no
 * buffer" and the screen stays empty while the engine draws happily.
 *
 * gl4es does not convert it. It only has GL_FIXED in its enum name table; its
 * array code never mentions it, because a real GLES2 driver accepts the type
 * and WebGL does not. So the conversion belongs here: record the array when it
 * is set, and convert it at draw time, because GLES1 client arrays are read
 * when the draw happens, not when the pointer is set.
 *
 * Two consequences of "read at draw time" that this got wrong at first, both
 * of which produce a black screen with no error anywhere:
 *
 *   - The record must survive the draw. Clearing it after converting means
 *     only the first draw of a run sees the engine's data; the engine sets the
 *     pointer once, rewrites the buffer before every quad (SGL_grpFillRect
 *     stores eight words and draws), and every later draw reads whatever the
 *     first one left behind.
 *
 *   - Each array needs its OWN conversion buffer. One shared scratch buffer is
 *     aliased by all three pointers -- glVertexPointer, glTexCoordPointer and
 *     glColorPointer all end up naming it -- and since gl4es reads the arrays
 *     at draw time, the vertex array contains the colour data by then. That
 *     draws geometry made of colour values.
 */
#define GL_FIXED_ 0x140C

typedef struct {
    int      active;      /* the engine set this array as GL_FIXED */
    int      size;
    uint32_t stride;      /* bytes; 0 means tightly packed */
    uint32_t ptr;         /* guest address */
    int      logged;
    float   *conv;        /* this array's own float copy, host memory */
    size_t   conv_cap;
} fixed_array;

/* The texture-coordinate array is the one array that is per texture *unit*:
 * GLES1 has glClientActiveTexture, and the engine uses it -- the HUD character
 * portrait is drawn with multi-texturing (unit 0 samples the portrait atlas,
 * unit 1 samples a circular alpha mask). One shared record made the second
 * glTexCoordPointer overwrite the first, so at draw time only one array was
 * converted and installed, on the one unit that happened to be selected. Unit
 * 1 kept its raw GL_FIXED words, WebGL read them as garbage, the mask evaluated
 * to alpha 0 everywhere, and the portrait was masked out completely -- an empty
 * ring where the face should be. gl4es keeps the arrays per unit itself
 * (ATT_MULTITEXCOORD0 + glstate->texture.client), so the port only has to
 * remember them per unit and reinstall each on its own unit before the draw. */
#define FIXED_TEX_UNITS 4
static fixed_array g_fx_vertex, g_fx_color;
static fixed_array g_fx_texcoord[FIXED_TEX_UNITS];

/* Which unit glActiveTexture/glClientActiveTexture last selected. The engine
 * uses only units 0 and 1, but the state is tracked for all four so an
 * unexpected unit cannot silently index past the array. */
static int g_client_tex_unit;
static int g_active_tex_unit;

static fixed_array *fx_texcoord(void)
{
    return &g_fx_texcoord[g_client_tex_unit];
}

static int tex_unit_index(uint32_t e)
{
    if (e < 0x84c0u || e >= 0x84c0u + FIXED_TEX_UNITS)
        return -1;
    return (int)(e - 0x84c0u);
}

/* Convert `count` vertices starting at `first` into this array's own float
 * buffer and hand that to gl4es. Called before every draw, because that is
 * when a GLES1 client array is read -- the record is deliberately not cleared
 * here; only glVertexPointer/glTexCoordPointer/glColorPointer clear it. */
static void fixed_install(const char *what, fixed_array *fx, int first, int count)
{
    if (!fx->active || count <= 0)
        return;
    int size = fx->size > 0 && fx->size <= 4 ? fx->size : 4;
    uint32_t stride = fx->stride ? fx->stride : (uint32_t)size * 4u;

    if (!fx->logged) {
        fx->logged = 1;
        host_log("gl: %s is GL_FIXED (size=%d stride=%u ptr=%08x) -- converting "
                 "to float, WebGL has no GL_FIXED\n",
                 what, size, stride, fx->ptr);
    }

    size_t need = (size_t)(first + count) * (size_t)size * sizeof(float);
    if (need > fx->conv_cap) {
        fx->conv = (float *)host_alloc(need);
        fx->conv_cap = need;
    }
    /* Check the WHOLE range, not just its start: G() validates one address, and
     * the loop below reads `count` vertices of `size` words each, so an array
     * ending near the top of guest memory walks off the end of linear memory --
     * an opaque wasm trap, from a call that had already returned successfully
     * from gl4es' own point of view. */
    uint32_t start = fx->ptr + (uint32_t)first * stride;
    uint32_t span = (uint32_t)(count > 0 ? (count - 1) : 0) * stride +
                    (uint32_t)size * 4u;
    if (start >= GUEST_MEM_SIZE || span > GUEST_MEM_SIZE - start) {
        static int warned;
        if (warned < 8) {
            warned++;
            host_log("gl: GL_FIXED array %08x+%u exceeds guest memory (%u MB) "
                     "-- skipping the draw\n", start, span, GUEST_MEM_SIZE >> 20);
        }
        return;
    }
    const uint32_t *src = (const uint32_t *)(g_mem + start);
    size_t n = (size_t)count * (size_t)size;
    for (size_t i = 0; i < n; i++)
        fx->conv[i] = (float)(int32_t)src[i] / 65536.0f;

    /* Host memory, not guest: the float copy lives in this process, and gl4es
     * hands the pointer straight to the driver. */
    if (strcmp(what, "glVertexPointer") == 0)
        gl4es_glVertexPointer(size, 0x1406 /* GL_FLOAT */, size * 4, fx->conv);
    else if (strcmp(what, "glTexCoordPointer") == 0)
        gl4es_glTexCoordPointer(size, 0x1406, size * 4, fx->conv);
    else if (strcmp(what, "glColorPointer") == 0)
        gl4es_glColorPointer(size, 0x1406, size * 4, fx->conv);
}

/* ---- initialisation ------------------------------------------------------ */

int gl4es_dispatch(const char *name, const uint32_t *args, int nargs,
                   uint32_t *ret);


static int g_fb_w = 960, g_fb_h = 544;

static void fb_size(int *w, int *h)
{
    *w = g_fb_w;
    *h = g_fb_h;
}

void gl4es_bridge_set_fb_size(int w, int h)
{
    g_fb_w = w;
    g_fb_h = h;
}

int g_gl4es_ready;

/* Hand the dispatch to gl.c, which calls it before its own state tracking. */
void gl4es_bridge_attach(void)
{
    extern int (*g_gl_backend)(const char *name, const uint32_t *args,
                               int nargs, uint32_t *ret);
    g_gl_backend = gl4es_dispatch;
}

#if defined(__EMSCRIPTEN__) || defined(__ANDROID__)
/* gl4es resolves its backend GLES2 entry points through this. With NOEGL there
 * is no EGL to ask, so without it every gles_gl* pointer stays NULL and the
 * first call prints "gles_glXxx is NULL" and does nothing -- the engine runs
 * but the screen stays empty. The host's own lookup is the right source:
 * emscripten's for the page, eglGetProcAddress for Android (plus dlsym, because
 * eglGetProcAddress is only required to return *extension* entry points and
 * some drivers return NULL for the core ones). */
/* The shader source gl4es hands the backend. Wrapping it here is the only place
 * to see it: gl4es generates the fixed-pipeline shader internally and calls the
 * backend's glShaderSource directly, so nothing in this port sees it otherwise.
 * A GLSL error with no source to look at is unguessable. */
/* The *driver's* glShaderSource, not the unprefixed symbol.
 *
 * With STATICLIB gl4es also exports the unprefixed names (nm libGL.a shows
 * `T glShaderSource` next to `T gl4es_glShaderSource`), and a static archive
 * beats the shared library at link time: the call below would resolve back into
 * gl4es' own entry point, which dereferences glstate. During
 * initialize_gl4es() glstate does not exist yet -- testGLSL() runs first -- so
 * that is a SIGSEGV at 0x2660 inside the loader, naming no guest code at all.
 * Android resolves it through EGL/dlsym; the browser's libGL is a JS library
 * and has no such alias, so there the plain symbol is already the driver's. */
typedef void (*shader_source_fn)(unsigned, int, const char *const *,
                                 const int *);

static shader_source_fn real_shader_source(void)
{
    static shader_source_fn fn;
    static int tried;
    if (!tried) {
        tried = 1;
#ifdef __ANDROID__
        fn = (shader_source_fn)(uintptr_t)eglGetProcAddress("glShaderSource");
        if (!fn) {
            void *h = dlopen("libGLESv2.so", RTLD_NOW | RTLD_LOCAL);
            if (h)
                fn = (shader_source_fn)(uintptr_t)dlsym(h, "glShaderSource");
        }
#else
        fn = (shader_source_fn)(uintptr_t)glShaderSource;
#endif
    }
    return fn;
}

static void gl_shader_source_wrap(unsigned shader, int count,
                                  const char *const *str, const int *len)
{
    static int logged;
    if (logged < 1) {
        logged++;
        host_log("  [glsl] %d source string(s)\n", count);
        for (int i = 0; i < count; i++) {
            int n = (len && len[i] >= 0) ? len[i] : (int)strlen(str[i]);
            host_log("  [glsl %d len=%d]\n%.*s\n  [end %d]\n", i, n, n, str[i], i);
        }
    }
    shader_source_fn fn = real_shader_source();
    if (fn)
        fn(shader, count, str, len);
}

/* The GLES1 fixed-point entry points gl4es expects its *backend* to provide.
 *
 * gl4es treats these as driver-supplied -- true for a GLES1 backend, where they
 * exist, and false under GLES2, where they do not. Its wrap layer therefore
 * forwards to a null pointer and the call dies. Supplying the symbols is what
 * gl4es asks of its driver and costs nothing: a vendored fork of gl4es would
 * have to be maintained, this does not. They are the only lookup misses; every
 * other entry point this engine uses resolves by itself.
 *
 * The conversion is not simply divide-by-65536. GLfixed is 16.16, but the
 * GLES1 fixed-point calls take *enum-valued* parameters too -- GL_LINEAR for
 * GL_TEXTURE_MIN_FILTER, GL_MODULATE for GL_TEXTURE_ENV_MODE -- and the engine
 * passes those as the enum, not as the enum scaled into 16.16. Scaling one
 * yields 0x2601/65536 == 0, which the driver rejects with GL_INVALID_ENUM and
 * which silently leaves the texture filter unset. So enums pass through and
 * everything else is scaled. */
static void backend_glTexParameterx(GLenum target, GLenum pname, GLfixed param)
{
    gl4es_glTexParameterf(target, pname, XP(pname, param));
}

/* These call gl4es' own implementations, not the backend's.
 *
 * The float GLES1 entry points -- glTexEnvf, glColor4f, glAlphaFunc, glPointSize
 * -- have no GLES2 equivalent either; gl4es implements them itself (gl/texenv.c,
 * gl/gl4es.c, gl/pointsprite.c). So the shim converts the fixed value and goes
 * back in through gl4es' real implementation, which is where the GLES1
 * semantics live. Calling the backend here would simply fail to link, which is
 * how this was found. */
static void backend_glTexEnvx(GLenum target, GLenum pname, GLfixed param)
{
    gl4es_glTexEnvf(target, pname, XP(pname, param));
}


/* glClearColor and glLineWidth *are* GLES2 entry points, so those two go to the
 * backend directly. */
static void backend_glClearColorx(GLfixed r, GLfixed g, GLfixed b, GLfixed a)
{
    glClearColor((float)r / 65536.0f, (float)g / 65536.0f,
                 (float)b / 65536.0f, (float)a / 65536.0f);
}

static void backend_glAlphaFuncx(GLenum func, GLfixed ref)
{
    gl4es_glAlphaFunc(func, (float)ref / 65536.0f);
}

static void backend_glLineWidthx(GLfixed w) { glLineWidth((float)w / 65536.0f); }

static void backend_glPointSizex(GLfixed s)
{
    gl4es_glPointSize((float)s / 65536.0f);
}

/* Name -> shim. Only the request the wrap layer makes reaches here, so this is
 * exactly the set gl4es cannot find on a GLES2 backend. */
struct backend_shim { const char *name; void *fn; };

static const struct backend_shim k_backend_shims[] = {
    { "glTexParameterx", (void *)backend_glTexParameterx },
    { "glTexEnvx",       (void *)backend_glTexEnvx },
    { "glClearColorx",   (void *)backend_glClearColorx },
    { "glAlphaFuncx",    (void *)backend_glAlphaFuncx },
    { "glLineWidthx",    (void *)backend_glLineWidthx },
    { "glPointSizex",    (void *)backend_glPointSizex },
    { NULL, NULL }
};

static void *gl4es_get_proc(const char *name)
{
    for (int i = 0; k_backend_shims[i].name; i++)
        if (strcmp(name, k_backend_shims[i].name) == 0)
            return (void *)(uintptr_t)k_backend_shims[i].fn;
    if (strcmp(name, "glShaderSource") == 0)
        return (void *)(uintptr_t)gl_shader_source_wrap;
#ifdef __ANDROID__
    void *p = (void *)(uintptr_t)eglGetProcAddress(name);
    if (!p)
        p = dlsym(RTLD_DEFAULT, name);
#else
    void *p = (void *)(uintptr_t)emscripten_webgl_get_proc_address(name);
#endif
    /* gl4es asks for every backend entry point it might use. Logging the misses
     * is the difference between "the backend is not wired" and "this one symbol
     * has no GLES2 equivalent", which need different fixes. */
    static int logged;
    if (!p && logged < 300) {
        logged++;
        host_log("gl: backend lookup miss: %s\n", name);
    }
    return p;
}
#endif

void gl4es_bridge_init(int fb_w, int fb_h)
{
    gl4es_bridge_set_fb_size(fb_w, fb_h);
    /* With NOEGL there is no EGL surface to ask, so gl4es has to be told the
     * framebuffer size before it initialises -- otherwise it guesses, and every
     * glOrthox and viewport the engine sets is relative to the wrong size. */
    set_getmainfbsize(fb_size);
#if defined(__EMSCRIPTEN__) || defined(__ANDROID__)
    set_getprocaddress(gl4es_get_proc);
#endif
    initialize_gl4es();

#if defined(__EMSCRIPTEN__) || defined(__ANDROID__)
    /* gl4es allocates its per-context state in NewGLState(), which the EGL
     * layer calls when a context is created. With NOEGL there is no context
     * creation, so nothing calls it and `glstate` stays pointing at a
     * zero-filled default -- every matrix call then dereferences a null
     * sub-struct inside gl4es and traps at wasm level, with no guest PC and
     * nothing in the log to say which call it was.
     *
     * DEFAULT_STATE is gl4es' marker for "the one default context", so the
     * state lands in its static rather than a fresh allocation. */
    {
        extern void *NewGLState(void *, int);
        NewGLState((void *)(uintptr_t)~(uintptr_t)0, 0);
        extern void *glstate;
        host_log("gl: NewGLState -> glstate = %p\n", glstate);
    }
#endif
    g_gl4es_ready = 1;
    gl4es_bridge_attach();
    host_log("gl: gl4es initialised, framebuffer %dx%d\n", fb_w, fb_h);
}

/* ---- the dispatch --------------------------------------------------------
 *
 * One `if` per entry point, in the order the report lists them by usage, so the
 * common ones are found first. Returns 1 when handled.
 */
int gl4es_dispatch(const char *name, const uint32_t *args, int nargs,
                   uint32_t *ret)
{
    if (!g_gl4es_ready)
        return 0;

    /* Log the next N entry points by name. A wasm trap carries no guest PC and
     * no stack, so the last name printed is the only record of which call was
     * running when it happened -- the interpreter's own trace ring cannot help,
     * because the trap is not an interpreter-level fault. */
    extern int g_gl_trace;
    if (g_gl_trace > 0) {
        g_gl_trace--;
        host_log("  [glcall] %s\n", name);
    }

    uint32_t a0 = nargs > 0 ? args[0] : 0;
    uint32_t a1 = nargs > 1 ? args[1] : 0;
    uint32_t a2 = nargs > 2 ? args[2] : 0;
    uint32_t a3 = nargs > 3 ? args[3] : 0;
    uint32_t a4 = nargs > 4 ? args[4] : 0;
    uint32_t a5 = nargs > 5 ? args[5] : 0;
    uint32_t a6 = nargs > 6 ? args[6] : 0;
    uint32_t a7 = nargs > 7 ? args[7] : 0;
    uint32_t a8 = nargs > 8 ? args[8] : 0;

    /* ---- state ---- */
    if (strcmp(name, "glEnable") == 0 || strcmp(name, "glDisable") == 0) {
        extern int g_gl_trace;
        if (g_gl_trace > 0)
            host_log("  [state] %s(%04x)\n", name, a0);
        /* strcmp, not a character test: name[2] is 'E' in "glEnable" and 'D'
         * in "glDisable", so a one-character shortcut here silently turned
         * glEnable into a no-op -- no GL_TEXTURE_2D, no GL_BLEND, and a white
         * screen with no error to point at it. */
        if (strcmp(name, "glEnable") == 0)
            gl4es_glEnable(a0);
        else
            gl4es_glDisable(a0);
        return 1;
    }
    if (strcmp(name, "glIsEnabled") == 0) {
        *ret = (uint32_t)gl4es_glIsEnabled(a0); return 1;
    }
    if (strcmp(name, "glEnableClientState") == 0 ||
        strcmp(name, "glDisableClientState") == 0) {
        extern int g_gl_trace;
        if (g_gl_trace > 0)
            host_log("  [state] %s(%04x)\n", name, a0);
        if (strcmp(name, "glEnableClientState") == 0)
            gl4es_glEnableClientState(a0);
        else
            gl4es_glDisableClientState(a0);
        return 1;
    }
    if (strcmp(name, "glBlendFunc") == 0) {
        extern int g_gl_trace;
        if (g_gl_trace > 0) host_log("  [state] glBlendFunc(%04x, %04x)\n", a0, a1);
        gl4es_glBlendFunc(a0, a1); return 1;
    }
    if (strcmp(name, "glDepthMask") == 0)  { gl4es_glDepthMask((uint8_t)a0); return 1; }
    if (strcmp(name, "glDepthFunc") == 0)  { gl4es_glDepthFunc(a0); return 1; }
    if (strcmp(name, "glStencilMask") == 0) { gl4es_glStencilMask(a0); return 1; }
    if (strcmp(name, "glShadeModel") == 0) {
        extern int g_gl_trace;
        if (g_gl_trace > 0) host_log("  [state] glShadeModel(%04x)\n", a0);
        gl4es_glShadeModel(a0); return 1;
    }
    if (strcmp(name, "glCullFace") == 0)   { gl4es_glCullFace(a0); return 1; }
    if (strcmp(name, "glFrontFace") == 0)  { gl4es_glFrontFace(a0); return 1; }
    if (strcmp(name, "glColorMask") == 0) {
        gl4es_glColorMask((uint8_t)a0, (uint8_t)a1, (uint8_t)a2, (uint8_t)a3);
        return 1;
    }
    if (strcmp(name, "glAlphaFunc") == 0)  { gl4es_glAlphaFunc(a0, F(a1)); return 1; }
    if (strcmp(name, "glAlphaFuncx") == 0) { gl4es_glAlphaFunc(a0, X(a1)); return 1; }
    if (strcmp(name, "glLineWidthx") == 0) { gl4es_glLineWidth(X(a0)); return 1; }
    if (strcmp(name, "glPointSizex") == 0) { gl4es_glPointSize(X(a0)); return 1; }
    if (strcmp(name, "glDepthRangef") == 0) { gl4es_glDepthRangef(F(a0), F(a1)); return 1; }
    if (strcmp(name, "glDepthRangex") == 0) { gl4es_glDepthRangef(X(a0), X(a1)); return 1; }

    /* ---- viewport and clearing ---- */
    if (strcmp(name, "glViewport") == 0) {
        int32_t vx = (int32_t)a0, vy = (int32_t)a1;
        int32_t vw = (int32_t)a2, vh = (int32_t)a3;
#ifdef __ANDROID__
        /* The engine's screen is 960x544 and the window is the device's: the
         * host scales the rect into a centred letterbox and installs the GL
         * viewport itself, so gl4es never sees a viewport it would have to
         * translate later. */
        if (host_adjust_viewport(&vx, &vy, &vw, &vh))
            return 1;
#endif
        gl4es_glViewport(vx, vy, vw, vh);
        return 1;
    }
    if (strcmp(name, "glScissor") == 0) {
        gl4es_glScissor((int32_t)a0, (int32_t)a1, (int32_t)a2, (int32_t)a3);
        return 1;
    }
    if (strcmp(name, "glClearColor") == 0) {
        gl4es_glClearColor(F(a0), F(a1), F(a2), F(a3));
        return 1;
    }
    if (strcmp(name, "glClearColorx") == 0) {
        gl4es_glClearColor(X(a0), X(a1), X(a2), X(a3));
        return 1;
    }
    if (strcmp(name, "glClearDepthf") == 0) { gl4es_glClearDepthf(F(a0)); return 1; }
    if (strcmp(name, "glClearDepthx") == 0) { gl4es_glClearDepthx((int32_t)a0); return 1; }
    if (strcmp(name, "glClearStencil") == 0) { gl4es_glClearStencil((int32_t)a0); return 1; }
    if (strcmp(name, "glClear") == 0) { gl4es_glClear(a0); return 1; }

    /* ---- transforms ----
     * These are the calls the engine makes every frame; the matrix stack itself
     * lives in gl4es (gl/matrix.c). */
    /* gl4es keeps its state in a global that its own init is supposed to fill.
     * Every matrix call goes through it, and a null one faults inside gl4es
     * rather than here -- an "out of bounds" with no guest PC. */
    {
        extern void *glstate;
        static int logged;
        if (!logged) {
            logged = 1;
            host_log("gl: gl4es glstate = %p\n", glstate);
        }
    }

    if (strcmp(name, "glMatrixMode") == 0) {
        gl4es_glMatrixMode(a0); return 1;
    }
    if (strcmp(name, "glLoadIdentity") == 0) { gl4es_glLoadIdentity(); return 1; }
    if (strcmp(name, "glPushMatrix") == 0)   { gl4es_glPushMatrix(); return 1; }
    if (strcmp(name, "glPopMatrix") == 0)    { gl4es_glPopMatrix(); return 1; }
    if (strcmp(name, "glScalex") == 0) {
        gl4es_glScalef(X(a0), X(a1), X(a2));
        return 1;
    }
    if (strcmp(name, "glTranslatex") == 0) {
        gl4es_glTranslatef(X(a0), X(a1), X(a2));
        return 1;
    }
    if (strcmp(name, "glOrthox") == 0) {
        gl4es_glOrthof(X(a0), X(a1), X(a2), X(a3), X(a4), X(a5));
        return 1;
    }
    if (strcmp(name, "glOrthof") == 0) {
        gl4es_glOrthof(F(a0), F(a1), F(a2), F(a3), F(a4), F(a5));
        return 1;
    }
    if (strcmp(name, "glFrustumx") == 0) {
        gl4es_glFrustumf(X(a0), X(a1), X(a2), X(a3), X(a4), X(a5));
        return 1;
    }
    if (strcmp(name, "glRotatex") == 0) {
        gl4es_glRotatef(X(a0), X(a1), X(a2), X(a3));
        return 1;
    }
    if (strcmp(name, "glLoadMatrixf") == 0) { gl4es_glLoadMatrixf((const float *)G(a0)); return 1; }
    if (strcmp(name, "glMultMatrixf") == 0) { gl4es_glMultMatrixf((const float *)G(a0)); return 1; }
    /* gl4es has no internal glLoadMatrixx/glMultMatrixx -- the GLES1 API
     * layer forwards them to a GLES1 backend, which a GLES2 backend is not.
     * The float forms are implemented (gl/matrix.c), so convert and call
     * those. Same for every other x-suffixed entry point below. */
    if (strcmp(name, "glLoadMatrixx") == 0) {
        gl4es_glLoadMatrixf(a0 ? fixed_matrix(G(a0)) : 0);
        return 1;
    }
    if (strcmp(name, "glMultMatrixx") == 0) {
        gl4es_glMultMatrixf(a0 ? fixed_matrix(G(a0)) : 0);
        return 1;
    }
    if (strcmp(name, "glNormal3x") == 0) {
        gl4es_glNormal3f(X(a0), X(a1), X(a2));
        return 1;
    }

    /* ---- textures ---- */
    if (strcmp(name, "glGenTextures") == 0) {
        /* GLsizei n, GLuint *textures: the array is guest memory and gl4es
         * writes the ids straight into it, which is correct -- the engine reads
         * them back itself. */
        gl4es_glGenTextures((int32_t)a0, (uint32_t *)GW(a1));
        return 1;
    }
    if (strcmp(name, "glDeleteTextures") == 0) {
        gl4es_glDeleteTextures((int32_t)a0, (const uint32_t *)G(a1));
        return 1;
    }
    if (strcmp(name, "glBindTexture") == 0) {
        g_cur_tex = a1;
        gl4es_glBindTexture(a0, a1); return 1;
    }
    if (strcmp(name, "__glBindTexture") == 0) { gl4es_glBindTexture(a0, a1); return 1; }
    if (strcmp(name, "glActiveTexture") == 0) {
        int u = tex_unit_index(a0);
        if (u >= 0) g_active_tex_unit = u;
        gl4es_glActiveTexture(a0); return 1;
    }
    if (strcmp(name, "glClientActiveTexture") == 0) {
        int u = tex_unit_index(a0);
        if (u >= 0) g_client_tex_unit = u;
        gl4es_glClientActiveTexture(a0); return 1;
    }
    if (strcmp(name, "glTexParameteri") == 0) {
        gl4es_glTexParameteri(a0, a1, (int32_t)a2); return 1;
    }
    if (strcmp(name, "glTexParameterx") == 0) {
        gl4es_glTexParameterf(a0, a1, XP(a1, a2)); return 1;
    }
    if (strcmp(name, "glTexParameterf") == 0) {
        gl4es_glTexParameterf(a0, a1, F(a2)); return 1;
    }
    if (strcmp(name, "glTexImage2D") == 0) {
        {
            extern int g_gl_trace;
            if (g_gl_trace > 0) {
                uint32_t nz = 0, n = 0;
                if (a8 && a8 < GUEST_MEM_SIZE) {
                    n = (uint32_t)a3 * (uint32_t)a4 * 4u;
                    if (a8 + n > GUEST_MEM_SIZE) n = GUEST_MEM_SIZE - a8;
                    for (uint32_t i = 0; i < n; i++)
                        if (g_mem[a8 + i]) nz++;
                }
                host_log("  [tex] glTexImage2D target=%04x level=%d "
                         "internal=%04x w=%d h=%d border=%d format=%04x "
                         "type=%04x px=%08x (%u bytes, %u non-zero)\n",
                         a0, (int)a1, a2, (int)a3, (int)a4, (int)a5,
                         a6, a7, a8, n, nz);
            }
        }
        {
            int i;
            for (i = 0; i < g_ntex; i++)
                if (g_tex[i].id == g_cur_tex) break;
            if (i == g_ntex) {
                if (g_ntex < GTEX_MAX) {
                    i = g_ntex++;
                } else {
                    static int said;
                    if (!said) {
                        said = 1;
                        host_log("gl: more than %d textures this session; "
                                 "the draw-trace table now recycles entries\n",
                                 GTEX_MAX);
                    }
                    i = g_tex_next;
                    g_tex_next = (g_tex_next + 1) % GTEX_MAX;
                }
            }
            g_tex[i].id = g_cur_tex; g_tex[i].w = a3; g_tex[i].h = a4;
            g_tex[i].fmt = a2;
        }
        gl4es_glTexImage2D(a0, (int32_t)a1, (int32_t)a2, (int32_t)a3,
                           (int32_t)a4, (int32_t)a5, a6, a7, G(a8));
        return 1;
    }
    if (strcmp(name, "glTexSubImage2D") == 0) {
        extern int g_gl_trace;
        if (g_gl_trace > 0) {
            host_log("  [gl] glTexSubImage2D target=%04x level=%d "
                     "x=%d y=%d w=%d h=%d format=%04x type=%04x px=%08x "
                     "caller=%08x\n",
                     a0, (int)a1, (int)a2, (int)a3, (int)a4,
                     (int)a5, a6, a7, a8, g_cpu.r[14]);
        }
        /* What the engine is actually handing over. A black screen has two
         * very different causes -- the engine drew nothing, or it drew
         * something that never reached the canvas -- and this is the line that
         * tells them apart without a debugger. */
        if (a6 == 0x1907 && a7 == 0x8363 && a4 == 384 && a5 == 256 && a8)
            g_csfb_ptr = a8;
        if (a6 == 0x1908 && a7 == 0x1401 && a8) {
            if (a4 == 480 && a5 == 320) g_u480_ptr = a8;
            if (a4 == 600 && a5 == 320) g_u600_ptr = a8;
        }
        {
            extern int g_gl_trace;
            /* For an RGBA upload, the alpha byte is the whole question: a
             * texture that arrives with alpha 0.2 draws as a 20% ghost of
             * itself over whatever is behind it, and nothing else in the log
             * says so. */
            if (g_gl_trace > 0 && a6 == 0x1908 && a7 == 0x1401 &&
                a8 && a8 < GUEST_MEM_SIZE) {
                uint32_t n = (uint32_t)a4 * (uint32_t)a5;
                if (a8 + n * 4u <= GUEST_MEM_SIZE) {
                    uint32_t hist[8] = {0};
                    uint64_t sum = 0;
                    for (uint32_t i = 0; i < n; i++) {
                        uint32_t al = g_mem[a8 + i * 4u + 3u];
                        hist[al >> 5]++;
                        sum += al;
                    }
                    host_log("  [tex]   %ux%u RGBA alpha: mean=%.1f "
                             "hist[0-31,32-63,...]=%u,%u,%u,%u,%u,%u,%u,%u\n",
                             a4, a5, (double)sum / n, hist[0], hist[1],
                             hist[2], hist[3], hist[4], hist[5], hist[6],
                             hist[7]);
                }
            }
        }
        g_last_screen_ptr = a8; g_last_screen_w = (uint32_t)a4;
        g_last_screen_h = (uint32_t)a5; g_last_screen_fmt = a6;
        g_last_screen_type = a7;
        {
            extern int g_gl_trace;
            if (g_gl_trace > 0 && a8 && a8 < GUEST_MEM_SIZE) {
                uint32_t nz = 0, n = (uint32_t)a4 * (uint32_t)a5 * 2u;
                const uint8_t *p = g_mem + a8;
                if (a8 + n <= GUEST_MEM_SIZE)
                    for (uint32_t i = 0; i < n; i++)
                        if (p[i]) nz++;
                host_log("  [gl]   pixels: %u bytes, %u non-zero (%.1f%%), "
                         "first 8 = %02x %02x %02x %02x %02x %02x %02x %02x\n",
                         n, nz, n ? 100.0 * nz / n : 0.0,
                         p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7]);
            }
        }
        /* SGL_FrameBufferToScreen calls this twice: once with the real format
         * and type (GL_RGB / GL_UNSIGNED_SHORT_5_6_5, matching the 384x256
         * surface it just uploaded) and once with both set to -1, from the same
         * code path and with the same pixel pointer. gl4es rejects -1 with
         * "unsupported pixel format" and aborts the conversion, which kills the
         * run. Nothing in GLES1 defines -1 as meaningful here, so the likeliest
         * reading is "no format, nothing to upload" -- a path Android never
         * takes. Treating it as a no-op is a probe, not a conclusion: if the
         * screen comes up, it was right; if it stays black, the upload mattered
         * and the format has to be recovered from the preceding call. */
        if (a6 == 0xFFFFFFFFu || a7 == 0xFFFFFFFFu) {
            static int warned;
            if (!warned) {
                warned = 1;
                host_log("  [gl] glTexSubImage2D with format/type = -1 from "
                         "%08x -- treating as a no-op\n", g_cpu.r[14]);
            }
            return 1;
        }
        gl4es_glTexSubImage2D(a0, (int32_t)a1, (int32_t)a2, (int32_t)a3,
                              (int32_t)a4, (int32_t)a5, a6, a7, G(a8));
        return 1;
    }
    if (strcmp(name, "glCompressedTexImage2D") == 0) {
        gl4es_glCompressedTexImage2D(a0, (int32_t)a1, a2, (int32_t)a3,
                                     (int32_t)a4, (int32_t)a5, (int32_t)a6, G(a7));
        return 1;
    }
    if (strcmp(name, "glCopyTexImage2D") == 0) {
        gl4es_glCopyTexImage2D(a0, (int32_t)a1, (int32_t)a2, (int32_t)a3,
                               (int32_t)a4, (int32_t)a5, (int32_t)a6, (int32_t)a7);
        return 1;
    }
    if (strcmp(name, "glCopyTexSubImage2D") == 0) {
        gl4es_glCopyTexSubImage2D(a0, (int32_t)a1, (int32_t)a2, (int32_t)a3,
                                  (int32_t)a4, (int32_t)a5, (int32_t)a6, (int32_t)a7);
        return 1;
    }
    if (strcmp(name, "glTexEnvx") == 0) {
        extern int g_gl_trace;
        if (g_gl_trace > 0)
            host_log("  [state] glTexEnvx(target=%04x pname=%04x param=%08x)\n",
                     a0, a1, a2);
        gl4es_glTexEnvf(a0, a1, XP(a1, a2)); return 1;
    }
    if (strcmp(name, "glTexEnvf") == 0) { gl4es_glTexEnvf(a0, a1, F(a2)); return 1; }
    if (strcmp(name, "glTexEnvxv") == 0) {
        gl4es_glTexEnvfv(a0, a1, (const float *)G(a2)); return 1;
    }

    /* ---- vertex arrays ----
     * The engine sets client-side arrays and draws from them. Emscripten's GL
     * needs -sFULL_ES2 for these; gl4es also has its own path for them. */
    if (strcmp(name, "glVertexPointer") == 0) {
        if (a1 == GL_FIXED_) {
            /* Do not forward: WebGL rejects the type and the array is left
             * unbound. Recorded for conversion at draw time instead. */
            g_fx_vertex.active = 1; g_fx_vertex.size = (int)a0; g_fx_vertex.stride = a2; g_fx_vertex.ptr = a3;
            return 1;
        }
        g_fx_vertex.active = 0;
        g_fx_vertex.size = (int)a0; g_fx_vertex.stride = a2; g_fx_vertex.ptr = a3;
        gl4es_glVertexPointer((int32_t)a0, a1, (int32_t)a2, G(a3)); return 1;
    }
    if (strcmp(name, "glColorPointer") == 0) {
        if (a1 == GL_FIXED_) {
            /* Do not forward: WebGL rejects the type and the array is left
             * unbound. Recorded for conversion at draw time instead. */
            g_fx_color.active = 1; g_fx_color.size = (int)a0; g_fx_color.stride = a2; g_fx_color.ptr = a3;
            return 1;
        }
        g_fx_color.active = 0;
        gl4es_glColorPointer((int32_t)a0, a1, (int32_t)a2, G(a3)); return 1;
    }
    if (strcmp(name, "glTexCoordPointer") == 0) {
        /* The texture-coordinate array is per unit: whichever unit
         * glClientActiveTexture last selected owns this call, and each unit
         * keeps its own record (the HUD portrait samples unit 0 for the face
         * and unit 1 for the circular mask). See g_fx_texcoord's declaration. */
        fixed_array *fx = fx_texcoord();
        if (a1 == GL_FIXED_) {
            /* Do not forward: WebGL rejects the type and the array is left
             * unbound. Recorded for conversion at draw time instead. */
            fx->active = 1; fx->size = (int)a0; fx->stride = a2; fx->ptr = a3;
            return 1;
        }
        fx->active = 0;
        gl4es_glTexCoordPointer((int32_t)a0, a1, (int32_t)a2, G(a3)); return 1;
    }
    if (strcmp(name, "glNormalPointer") == 0) {
        gl4es_glNormalPointer(a0, (int32_t)a1, G(a2)); return 1;
    }

    /* ---- colour ---- */
    if (strcmp(name, "glColor4x") == 0) {
        extern int g_gl_trace;
        if (g_gl_trace > 0)
            host_log("  [state] glColor4x(%d,%d,%d,%d)\n",
                     (int32_t)a0 >> 16, (int32_t)a1 >> 16,
                     (int32_t)a2 >> 16, (int32_t)a3 >> 16);
        gl4es_glColor4f(X(a0), X(a1), X(a2), X(a3));
        return 1;
    }
    if (strcmp(name, "glColor4f") == 0) {
        gl4es_glColor4f(F(a0), F(a1), F(a2), F(a3)); return 1;
    }
    if (strcmp(name, "glColor4ub") == 0) {
        gl4es_glColor4ub((uint8_t)a0, (uint8_t)a1, (uint8_t)a2, (uint8_t)a3);
        return 1;
    }

    /* ---- lists and draws ----
     * glDrawArrays is the engine's only draw call on this path. These are also
     * the calls that will cost the most once the renderer is real, which is why
     * gl_report counts them. */
    if (strcmp(name, "glDrawArrays") == 0) {
        /* Snapshot before the conversion: fixed_install() clears `active` as
         * its last act, so anything read afterwards reports "not fixed" for a
         * buffer that was. */
        extern int g_gl_trace;
        if (g_gl_trace > 0) {
            {
                char vb[200] = {0};
                int o = 0;
                if (g_fx_vertex.active) {
                    /* GLfixed is 16.16, so the raw words have to be shifted
                     * back down. Reading them as floats gives ~1e-39, which
                     * prints as 0.0 and looks exactly like "the engine drew
                     * nothing" -- it cost an hour. */
                    const int32_t *v = (const int32_t *)(g_mem + g_fx_vertex.ptr);
                    int vn = g_fx_vertex.size ? g_fx_vertex.size : 2;
                    for (int i = 0; i < 4; i++) {
                        o += snprintf(vb + o, sizeof(vb) - (size_t)o, "(");
                        for (int k = 0; k < vn; k++)
                            o += snprintf(vb + o, sizeof(vb) - (size_t)o,
                                          "%s%.1f", k ? "," : "",
                                          (double)v[i * vn + k] / 65536.0);
                        o += snprintf(vb + o, sizeof(vb) - (size_t)o, ") ");
                    }
                } else {
                    const float *v = (const float *)(g_mem + g_fx_vertex.ptr);
                    int vn = g_fx_vertex.size ? g_fx_vertex.size : 2;
                    o = 0;
                    for (int i = 0; i < 4; i++) {
                        o += snprintf(vb + o, sizeof(vb) - (size_t)o, "(");
                        for (int k = 0; k < vn; k++)
                            o += snprintf(vb + o, sizeof(vb) - (size_t)o,
                                          "%s%.1f", k ? "," : "",
                                          (double)v[i * vn + k]);
                        o += snprintf(vb + o, sizeof(vb) - (size_t)o, ") ");
                    }
                }
                {
                    char tb[160] = {0};
                    int o3 = 0;
                    if (!g_fx_texcoord[g_client_tex_unit].active &&
                        !g_fx_texcoord[1].active) {
                        snprintf(tb, sizeof(tb), "(texcoord array off)");
                    } else {
                        for (int u = 0; u < 2; u++) {
                            const fixed_array *fx = &g_fx_texcoord[u];
                            if (!fx->active)
                                continue;
                            const int32_t *t =
                                (const int32_t *)(g_mem + fx->ptr);
                            int tn = fx->size ? fx->size : 2;
                            o3 += snprintf(tb + o3, sizeof(tb) - (size_t)o3,
                                           "u%d", u);
                            for (int i = 0; i < 4 && o3 < 140; i++) {
                                o3 += snprintf(tb + o3,
                                               sizeof(tb) - (size_t)o3, "(");
                                for (int k = 0; k < tn; k++)
                                    o3 += snprintf(tb + o3,
                                                   sizeof(tb) - (size_t)o3,
                                                   "%s%.3f", k ? "," : "",
                                                   (double)t[i * tn + k] /
                                                       65536.0);
                                o3 += snprintf(tb + o3,
                                               sizeof(tb) - (size_t)o3, ")");
                            }
                            o3 += snprintf(tb + o3, sizeof(tb) - (size_t)o3,
                                           " ");
                        }
                    }
                    host_log("  [draw]   texcoords %s\n", tb);
                }
                {
                    char cb[80] = {0};
                    if (g_fx_color.active) {
                        const int32_t *c = (const int32_t *)(g_mem + g_fx_color.ptr);
                        int cn = g_fx_color.size ? g_fx_color.size : 4;
                        int o2 = 0;
                        for (int i = 0; i < 4 && o2 < 60; i++) {
                            o2 += snprintf(cb + o2, sizeof(cb) - (size_t)o2, "(");
                            for (int k = 0; k < cn; k++)
                                o2 += snprintf(cb + o2, sizeof(cb) - (size_t)o2,
                                               "%s%.2f", k ? "," : "",
                                               (double)c[i * cn + k] / 65536.0);
                            o2 += snprintf(cb + o2, sizeof(cb) - (size_t)o2, ")");
                        }
                    } else {
                        snprintf(cb, sizeof(cb), "(color array off)");
                    }
                    host_log("  [draw]   colorarray %s\n", cb);
                }
                {
                    uint32_t tw = 0, th = 0, tf = 0;
                    for (int i = 0; i < g_ntex; i++)
                        if (g_tex[i].id == g_cur_tex) {
                            tw = g_tex[i].w; th = g_tex[i].h; tf = g_tex[i].fmt;
                        }
                    host_log("  [draw] mode=%d n=%d fixed=%d unit=%d tex=%u(%ux%u "
                             "internal=%04x) verts %s\n",
                             (int)a0, (int)a2, g_fx_vertex.active,
                             g_active_tex_unit, g_cur_tex, tw, th, tf, vb);
                }
            }
        }
        /* The texture-coordinate arrays are per unit and gl4es stores them per
         * unit, so each recorded array has to be reinstalled on its own unit,
         * with glClientActiveTexture selecting it: glTexCoordPointer always
         * writes the array of the unit that is client-active at the moment of
         * the call, and the engine leaves that on unit 0. Installing them all
         * on unit 0 -- what a single shared record did -- leaves unit 1 with
         * the raw GL_FIXED pointer it was given, which WebGL reads as garbage,
         * so the mask samples alpha 0 and masks the portrait out entirely. */
        fixed_install("glVertexPointer", &g_fx_vertex, (int)a1, (int)a2);
        fixed_install("glColorPointer", &g_fx_color, (int)a1, (int)a2);
        {
            int moved = 0;
            for (int u = 0; u < FIXED_TEX_UNITS; u++) {
                if (!g_fx_texcoord[u].active)
                    continue;
                if (u != g_client_tex_unit) {
                    gl4es_glClientActiveTexture(0x84c0 + u);
                    moved = 1;
                }
                fixed_install("glTexCoordPointer", &g_fx_texcoord[u],
                              (int)a1, (int)a2);
            }
            if (moved)
                gl4es_glClientActiveTexture(0x84c0 + g_client_tex_unit);
        }
        gl4es_glDrawArrays(a0, (int32_t)a1, (int32_t)a2);
        g_gl_draw_calls++;
        return 1;
    }
    if (strcmp(name, "glDrawElements") == 0) {
        gl4es_glDrawElements(a0, (int32_t)a1, a2, G(a3));
        g_gl_draw_calls++;
        return 1;
    }

    /* ---- queries ---- */
    if (strcmp(name, "glGetError") == 0)                 { *ret = gl4es_glGetError(); return 1; }
    if (strcmp(name, "glGetIntegerv") == 0)              { gl4es_glGetIntegerv(a0, (int32_t *)GW(a1)); return 1; }
    if (strcmp(name, "glGetFixedv") == 0)                { gl4es_glGetFixedv(a0, (int32_t *)GW(a1)); return 1; }
    if (strcmp(name, "glGetFloatv") == 0)                { gl4es_glGetFloatv(a0, (float *)GW(a1)); return 1; }
    if (strcmp(name, "glGetString") == 0)                { *ret = (uint32_t)(uintptr_t)gl4es_glGetString(a0); return 1; }
    if (strcmp(name, "glFinish") == 0)                   { gl4es_glFinish(); return 1; }
    if (strcmp(name, "glFlush") == 0)                    { gl4es_glFlush(); return 1; }

    return 0;
}
