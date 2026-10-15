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
/* For the backend prototypes the wrapper below calls. gl4es never sees this
 * header -- it resolves its own pointers -- so there is no aliasing. */
#include <GLES2/gl2.h>
#endif

/* ---- argument helpers ---------------------------------------------------- */

/* A guest address as a host pointer. Address 0 stays NULL: several GL calls
 * treat a null pointer as meaningful (glTexImage2D with NULL allocates storage
 * without uploading), and handing gl4es g_mem+0 instead would make it read the
 * guard region. */
static inline const void *G(uint32_t guest_addr)
{
    return guest_addr ? (const void *)(g_mem + guest_addr) : (const void *)0;
}
static inline void *GW(uint32_t guest_addr)
{
    return guest_addr ? (void *)(g_mem + guest_addr) : (void *)0;
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

#ifdef __EMSCRIPTEN__
/* gl4es resolves its backend GLES2 entry points through this. With NOEGL there
 * is no EGL to ask, so without it every gles_gl* pointer stays NULL and the
 * first call prints "gles_glXxx is NULL" and does nothing -- the engine runs
 * but the screen stays empty. Emscripten's own lookup is the right source. */
/* The shader source gl4es hands the backend. Wrapping it here is the only place
 * to see it: gl4es generates the fixed-pipeline shader internally and calls the
 * backend's glShaderSource directly, so nothing in this port sees it otherwise.
 * A GLSL error with no source to look at is unguessable. */
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
    glShaderSource(shader, count, str, len);
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

static void backend_glColor4x(GLfixed r, GLfixed g, GLfixed b, GLfixed a)
{
    gl4es_glColor4f((float)r / 65536.0f, (float)g / 65536.0f,
                    (float)b / 65536.0f, (float)a / 65536.0f);
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
    { "glColor4x",       (void *)backend_glColor4x },
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
    void *p = (void *)(uintptr_t)emscripten_webgl_get_proc_address(name);
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
#ifdef __EMSCRIPTEN__
    set_getprocaddress(gl4es_get_proc);
#endif
    initialize_gl4es();
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
    if (strcmp(name, "glEnable") == 0)  { gl4es_glEnable(a0); return 1; }
    if (strcmp(name, "glDisable") == 0) { gl4es_glDisable(a0); return 1; }
    if (strcmp(name, "glIsEnabled") == 0) {
        *ret = (uint32_t)gl4es_glIsEnabled(a0); return 1;
    }
    if (strcmp(name, "glEnableClientState") == 0)  { gl4es_glEnableClientState(a0); return 1; }
    if (strcmp(name, "glDisableClientState") == 0) { gl4es_glDisableClientState(a0); return 1; }
    if (strcmp(name, "glBlendFunc") == 0) { gl4es_glBlendFunc(a0, a1); return 1; }
    if (strcmp(name, "glDepthMask") == 0)  { gl4es_glDepthMask((uint8_t)a0); return 1; }
    if (strcmp(name, "glDepthFunc") == 0)  { gl4es_glDepthFunc(a0); return 1; }
    if (strcmp(name, "glStencilMask") == 0) { gl4es_glStencilMask(a0); return 1; }
    if (strcmp(name, "glShadeModel") == 0) { gl4es_glShadeModel(a0); return 1; }
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
        gl4es_glViewport((int32_t)a0, (int32_t)a1, (int32_t)a2, (int32_t)a3);
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
    if (strcmp(name, "glMatrixMode") == 0)   { gl4es_glMatrixMode(a0); return 1; }
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
    if (strcmp(name, "glBindTexture") == 0) { gl4es_glBindTexture(a0, a1); return 1; }
    if (strcmp(name, "glActiveTexture") == 0)       { gl4es_glActiveTexture(a0); return 1; }
    if (strcmp(name, "glClientActiveTexture") == 0) { gl4es_glClientActiveTexture(a0); return 1; }
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
        gl4es_glTexImage2D(a0, (int32_t)a1, (int32_t)a2, (int32_t)a3,
                           (int32_t)a4, (int32_t)a5, a6, a7, G(a8));
        return 1;
    }
    if (strcmp(name, "glTexSubImage2D") == 0) {
        static int logged;
        if (logged < 4) {
            logged++;
            host_log("  [gl] glTexSubImage2D nargs=%d target=%04x level=%d "
                     "x=%d y=%d w=%d h=%d format=%04x type=%04x px=%08x\n",
                     nargs, a0, (int)a1, (int)a2, (int)a3, (int)a4,
                     (int)a5, a6, a7, a8);
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
    if (strcmp(name, "glTexEnvx") == 0) { gl4es_glTexEnvf(a0, a1, XP(a1, a2)); return 1; }
    if (strcmp(name, "glTexEnvf") == 0) { gl4es_glTexEnvf(a0, a1, F(a2)); return 1; }
    if (strcmp(name, "glTexEnvxv") == 0) {
        gl4es_glTexEnvfv(a0, a1, (const float *)G(a2)); return 1;
    }

    /* ---- vertex arrays ----
     * The engine sets client-side arrays and draws from them. Emscripten's GL
     * needs -sFULL_ES2 for these; gl4es also has its own path for them. */
    if (strcmp(name, "glVertexPointer") == 0) {
        gl4es_glVertexPointer((int32_t)a0, a1, (int32_t)a2, G(a3)); return 1;
    }
    if (strcmp(name, "glColorPointer") == 0) {
        gl4es_glColorPointer((int32_t)a0, a1, (int32_t)a2, G(a3)); return 1;
    }
    if (strcmp(name, "glTexCoordPointer") == 0) {
        gl4es_glTexCoordPointer((int32_t)a0, a1, (int32_t)a2, G(a3)); return 1;
    }
    if (strcmp(name, "glNormalPointer") == 0) {
        gl4es_glNormalPointer(a0, (int32_t)a1, G(a2)); return 1;
    }

    /* ---- colour ---- */
    if (strcmp(name, "glColor4x") == 0) {
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
