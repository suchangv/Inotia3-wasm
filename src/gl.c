/*
 * gl.c -- GLES1 state bookkeeping. See gl.h for what this is and is not.
 *
 * The rule followed throughout: answer with something *consistent*, never with
 * a plausible-looking constant. A texture ID must be unique and must survive
 * being bound and queried; a query must return what a previous call set. The
 * engine builds its own data structures on top of these answers, so an
 * inconsistent one corrupts guest state in ways that surface far from the call
 * -- which is exactly how the crash that prompted this file happened:
 * glGenTextures() returned without writing any IDs, the engine stored whatever
 * was in its buffer, and it faulted several thousand instructions later inside
 * textureStore::grabMagicTextureIds().
 */

#include "gl.h"
#include "guest.h"
#include "cpu.h"
#include "compat.h"

#ifndef WASM_FREESTANDING
#include <stdio.h>
#include <stdlib.h>   /* the framebuffer dump below is a debugging aid */
#endif

uint64_t g_gl_draw_calls;
uint64_t g_gl_textures_created;
uint64_t g_gl_tex_uploads;

/* ---- GLES1 enums (only those the engine actually asks about) -------------- */

#define GL_NO_ERROR                 0x0000
#define GL_MAX_LIGHTS               0x0D31
#define GL_MAX_TEXTURE_SIZE         0x0D33
#define GL_MAX_MODELVIEW_STACK_DEPTH 0x0D36
#define GL_MAX_PROJECTION_STACK_DEPTH 0x0D38
#define GL_MAX_TEXTURE_STACK_DEPTH  0x0D39
#define GL_SUBPIXEL_BITS            0x0D50
#define GL_RED_BITS                 0x0D52
#define GL_GREEN_BITS               0x0D53
#define GL_BLUE_BITS                0x0D54
#define GL_ALPHA_BITS               0x0D55
#define GL_DEPTH_BITS               0x0D56
#define GL_STENCIL_BITS             0x0D57
#define GL_MATRIX_MODE              0x0BA0
#define GL_VIEWPORT                 0x0BA2
#define GL_SMOOTH                   0x1D01
#define GL_TEXTURE_BINDING_2D       0x8069
#define GL_MAX_TEXTURE_UNITS        0x84E2
#define GL_ALIASED_POINT_SIZE_RANGE 0x846D
#define GL_ALIASED_LINE_WIDTH_RANGE 0x846E
#define GL_NUM_COMPRESSED_TEXTURE_FORMATS 0x86A2

/* What the "device" claims to be. Values chosen to match a phone GPU of the
 * game's era rather than a modern desktop: a larger MAX_TEXTURE_SIZE would
 * invite the engine to allocate atlases this layer cannot honour later, and
 * the numbers are cheap to raise once the renderer exists. */
#define REPORTED_MAX_TEXTURE_SIZE   2048
#define REPORTED_MAX_TEXTURE_UNITS  4
#define REPORTED_MAX_LIGHTS         8
#define REPORTED_MATRIX_STACK       32

/* ---- state ---------------------------------------------------------------- */

typedef struct {
    uint32_t cur_texture_2d;      /* bound texture object                  */
    uint32_t next_texture_id;     /* monotonic allocator for glGenTextures  */
    uint32_t matrix_mode;
    int32_t  viewport[4];
    int32_t  scissor[4];
    uint32_t enabled[8];          /* bitset, by GLenum >> 5                 */
    uint32_t client_enabled[8];
    uint32_t tex_params[8];       /* bound-texture parameters, by pname>>5  */
    uint32_t tex_env[8];
    int      inited;
} gl_state;

static gl_state g_gl;

/* Texture IDs are handed out from here. Starting at 1 matters: 0 is "no
 * texture" in GL, and an engine that tests `if (id)` must not be told it has a
 * texture when it does not. */
#define GL_FIRST_TEXTURE_ID 1

void gl_state_init(void)
{
    memset(&g_gl, 0, sizeof(g_gl));
    g_gl.next_texture_id = GL_FIRST_TEXTURE_ID;
    g_gl.viewport[2] = 960;
    g_gl.viewport[3] = 544;
    g_gl.inited = 1;
}

static void set_bit(uint32_t *set, uint32_t cap, int on)
{
    uint32_t i = (cap >> 5) & 7;
    uint32_t b = 1u << (cap & 31);
    if (on) set[i] |= b; else set[i] &= ~b;
}

static int get_bit(const uint32_t *set, uint32_t cap)
{
    return (set[(cap >> 5) & 7] >> (cap & 31)) & 1;
}

/* ---- entry points --------------------------------------------------------- */

/* Everything this layer answers, as one table. Keeping it in one place is what
 * makes "is this GL, and is it answered?" a single question -- otherwise a name
 * can be in the dispatch switch but not the recogniser, and the call falls
 * through to the generic unimplemented path while the report blames the
 * function. That exact bug happened once already with the jni/env/ prefix. */
static const char *const k_gl_entries[] = {
    /* texture objects */
    "glGenTextures", "glDeleteTextures", "glBindTexture",
    "glTexParameteri", "glTexParameterf", "glTexParameterx",
    "glTexImage2D", "glTexSubImage2D", "glCompressedTexImage2D",
    "glCompressedTexSubImage2D", "glCopyTexImage2D", "glCopyTexSubImage2D",
    "glTexEnvf", "glTexEnvx", "glTexEnvxv",
    "glActiveTexture", "glClientActiveTexture",
    /* state */
    "glEnable", "glDisable", "glIsEnabled",
    "glEnableClientState", "glDisableClientState",
    "glBlendFunc", "glBlendFuncSeparate", "glBlendEquation",
    "glDepthFunc", "glDepthMask", "glDepthRangef", "glDepthRangex",
    "glShadeModel", "glAlphaFunc", "glAlphaFuncx", "glLineWidthx",
    "glPointSizex", "glStencilMask", "glColorMask", "glCullFace",
    "glFrontFace", "glPolygonOffset", "glSampleCoverage",
    "glClipPlanef", "glClipPlanex", "glLightxv", "glLightModelfv",
    "glNormal3f", "glNormal3x",
    /* transforms: recorded, not evaluated */
    "glMatrixMode", "glLoadIdentity", "glPushMatrix", "glPopMatrix",
    "glLoadMatrixf", "glLoadMatrixx", "glMultMatrixf", "glMultMatrixx",
    "glFrustumf", "glFrustumx", "glOrthof", "glOrthox",
    "glRotatef", "glRotatex", "glScalef", "glScalex",
    "glTranslatef", "glTranslatex",
    /* viewport / clear */
    "glViewport", "glScissor", "glClearColor", "glClearColorx",
    "glClearDepthf", "glClearDepthx", "glClearStencil", "glClear",
    "glColorMask", "glFinish", "glFlush",
    /* immediate-mode and array colour */
    "glColor4f", "glColor4x", "glColor4ub",
    "glColorPointer", "glVertexPointer", "glTexCoordPointer",
    "glNormalPointer", "glPointSizePointerOES",
    /* queries */
    "glGetError", "glGetIntegerv", "glGetFixedv", "glGetFloatv",
    "glGetPointerv", "glGetString",
    /* framebuffers (render-to-texture; the engine uses these) */
    "glGenFramebuffersOES", "glBindFramebufferOES", "glDeleteFramebuffersOES",
    "glCheckFramebufferStatusOES", "glGenRenderbuffersOES",
    "glBindRenderbufferOES", "glDeleteRenderbuffersOES",
    "glFramebufferTexture2DOES", "glRenderbufferStorageOES",
    "glGenFramebuffers", "glBindFramebuffer", "glDeleteFramebuffers",
    "glCheckFramebufferStatus", "glGenRenderbuffers", "glBindRenderbuffer",
    "glDeleteRenderbuffers", "glFramebufferTexture2D",
    "glRenderbufferStorage",
    /* draws: counted, not performed (see gl.h) */
    "glDrawArrays", "glDrawElements", "glDrawTexfOES", "glDrawTexfvOES",
    "glDrawTexiOES", "glDrawTexivOES", "glDrawTexsOES", "glDrawTexsvOES",
    "glDrawTexxOES", "glDrawTexxvOES",
    /* shaders: the engine links libGLESv1_CM but was compiled for the fixed
     * pipeline, so these exist in the import table without being on the game's
     * path. Answered so a stray call does not look like a missing feature. */
    "glCreateShader", "glShaderSource", "glCompileShader", "glCreateProgram",
    "glAttachShader", "glLinkProgram", "glUseProgram", "glDeleteShader",
    "glDeleteProgram", "glGetShaderiv", "glGetProgramiv", "glGetUniformLocation",
    "glUniform1i", "glUniform4fv", "glBindAttribLocation",
    "glGetAttribLocation", "glVertexAttribPointer", "glEnableVertexAttribArray",
    "glDisableVertexAttribArray",
    /* buffers */
    "glGenBuffers", "glBindBuffer", "glBufferData", "glBufferSubData",
    "glDeleteBuffers",
    NULL
};

int gl_is_entry(const char *name)
{
    if (name[0] != 'g' || name[1] != 'l')
        return 0;
    for (int i = 0; k_gl_entries[i]; i++)
        if (strcmp(k_gl_entries[i], name) == 0)
            return 1;
    return 0;
}

/* Queries the engine makes about its environment. Answered from the reported
 * device description rather than from real state, because there is no real
 * device yet -- but consistently, and from one place, so raising a limit later
 * is a one-line change. */
static int query_constant(uint32_t pname, uint32_t *out, int *count)
{
    switch (pname) {
    case GL_MAX_TEXTURE_SIZE:            *out = REPORTED_MAX_TEXTURE_SIZE; *count = 1; return 1;
    case GL_MAX_TEXTURE_UNITS:           *out = REPORTED_MAX_TEXTURE_UNITS; *count = 1; return 1;
    case GL_MAX_LIGHTS:                  *out = REPORTED_MAX_LIGHTS; *count = 1; return 1;
    case GL_MAX_MODELVIEW_STACK_DEPTH:
    case GL_MAX_PROJECTION_STACK_DEPTH:
    case GL_MAX_TEXTURE_STACK_DEPTH:     *out = REPORTED_MATRIX_STACK; *count = 1; return 1;
    case GL_SUBPIXEL_BITS:               *out = 4; *count = 1; return 1;
    case GL_RED_BITS: case GL_GREEN_BITS:
    case GL_BLUE_BITS: case GL_ALPHA_BITS: *out = 8; *count = 1; return 1;
    case GL_DEPTH_BITS:                  *out = 16; *count = 1; return 1;
    case GL_STENCIL_BITS:                *out = 0; *count = 1; return 1;
    case GL_NUM_COMPRESSED_TEXTURE_FORMATS: *out = 0; *count = 1; return 1;
    default: return 0;
    }
}

int (*g_gl_backend)(const char *name, const uint32_t *args, int nargs,
                    uint32_t *ret);

void gl_host_call(const char *name, const uint32_t *args, int nargs,
                  uint32_t *ret_lo)
{
    /* A real renderer, if one is linked in, answers first. Everything below is
     * the fallback that lets the engine believe it has a GL context. */
    if (g_gl_backend && g_gl_backend(name, args, nargs, ret_lo))
        return;

    if (!g_gl.inited)
        gl_state_init();

    /* GLES1's entry points all take their arguments positionally, and the ones
     * past the fourth are rare enough that reading a0..a4 covers every call in
     * this guest's import table. */
    uint32_t a0 = nargs > 0 ? args[0] : 0;
    uint32_t a1 = nargs > 1 ? args[1] : 0;
    uint32_t a2 = nargs > 2 ? args[2] : 0;
    uint32_t a3 = nargs > 3 ? args[3] : 0;
    uint32_t a4 = nargs > 4 ? args[4] : 0;
    (void)a4;

    /* ---- texture objects ---- */
    if (strcmp(name, "glGenTextures") == 0) {
        /* GLsizei n, GLuint *textures. The array must be filled in: the engine
         * stores these IDs and binds them later, and a zero or a stale value
         * corrupts its texture table rather than failing here. */
        for (uint32_t i = 0; i < a0; i++) {
            ST32(a1 + i * 4, g_gl.next_texture_id++);
            g_gl_textures_created++;
        }
        return;
    }
    if (strcmp(name, "glDeleteTextures") == 0)
        return;   /* IDs are never reused, so deletion needs no bookkeeping */
    if (strcmp(name, "glBindTexture") == 0) {
        if (a0 == 0x0DE1)          /* GL_TEXTURE_2D */
            g_gl.cur_texture_2d = a1;
        return;
    }
    /* Log the dimensions of the first few uploads. Whether the engine
     * rasterises in software and presents one full-screen texture, or draws
     * geometry through the fixed pipeline, changes the size of the renderer by
     * a lot -- and the call sequence alone does not distinguish them. */
    {
        static int logged;
        if ((strcmp(name, "glTexImage2D") == 0 ||
             strcmp(name, "glTexSubImage2D") == 0) && logged < 8) {
            logged++;
            /* glTexImage2D(target, level, internalFormat, w, h, border, fmt,
             *              type, pixels) / glTexSubImage2D(target, level,
             *              xoff, yoff, w, h, fmt, type, pixels) */
            int is_sub = strcmp(name, "glTexSubImage2D") == 0;
            host_log("  [gl] %-18s w=%u h=%u %s fmt=%x pixels=%08x\n", name,
                     is_sub ? args[4] : args[3], is_sub ? args[5] : args[4],
                     is_sub ? "sub" : "full", is_sub ? args[6] : args[5],
                     is_sub ? args[8] : args[8]);
        }
    }
    /* Dump the first full-surface upload to a raw file.
     *
     * The engine composites its 2D scene itself and hands the result to GL as a
     * 384x256 texture. "It is drawing" and "it is drawing something" are
     * different claims, and this is what separates them: the pixels are in guest
     * memory right here, so they can be looked at without a working renderer, a
     * browser, or a GL stack at all. */
#ifndef WASM_FREESTANDING
    /* Only for the two upload entry points. This block used to run for every GL
     * call, reading args[8] as a pixel pointer and args[3]/args[4] as dimensions
     * -- which for, say, glTexCoordPointer(2, GL_FIXED, 0, ptr) means reading
     * guest memory at whatever happens to be in that stack slot. That is a host
     * fault several frames later, indistinguishable from an engine bug. */
    if (strcmp(name, "glTexImage2D") == 0 ||
        strcmp(name, "glTexSubImage2D") == 0) {
        static int dumped;
        int is_sub = strcmp(name, "glTexSubImage2D") == 0;
        int w = (int)(is_sub ? args[4] : args[3]);
        int h = (int)(is_sub ? args[5] : args[4]);
        uint32_t px = args[8];
        /* Every upload, with its size and how much of it is non-zero. The
         * question this answers: does the asset pipeline deliver real pixels to
         * GL at all? If the PNG-derived textures are non-black and the 384x256
         * surface is not, the renderer is fine and the compositing is not. */
        if (px && w > 0 && h > 0 && w <= 2048 && h <= 2048 && dumped < 24) {
            const uint16_t *q = (const uint16_t *)(g_mem + px);
            uint32_t nz = 0;
            long n = (long)w * h;
            for (long i = 0; i < n; i++)
                if (q[i]) nz++;
            host_log("  [tex] %-18s %4dx%-4d %s caller=%08x px=%08x  nonblack=%u/%ld\n",
                     name, w, h, is_sub ? "sub" : "full",
                     g_cpu.r[14], px, nz, n);
        }
        /* Opt-in: this writes up to 80 x 192 KB files into the working
         * directory, which is fine when you are staring at a black screen and
         * want to know what the engine handed over, and pure litter the rest of
         * the time. SPIKE_DUMP_FB=1. */
        if (getenv("SPIKE_DUMP_FB") && dumped < 80 && px && w >= 256 && h >= 192) {
            dumped++;
            char path[64];
            snprintf(path, sizeof(path), "fb%03d.raw", dumped);
            FILE *f = fopen(path, "wb");
            if (f) {
                fwrite(g_mem + px, 1, (size_t)w * h * 2, f);
                fclose(f);
                uint32_t nz = 0;
                const uint16_t *q = (const uint16_t *)(g_mem + px);
                for (int i = 0; i < w * h; i++)
                    if (q[i]) nz++;
                host_log("dumped %s %dx%d, %u/%d non-black\n",
                         path, w, h, nz, w * h);
            }
        }
    }
#endif

    if (strcmp(name, "glTexImage2D") == 0 ||
        strcmp(name, "glTexSubImage2D") == 0 ||
        strcmp(name, "glCompressedTexImage2D") == 0 ||
        strcmp(name, "glCompressedTexSubImage2D") == 0 ||
        strcmp(name, "glCopyTexImage2D") == 0 ||
        strcmp(name, "glCopyTexSubImage2D") == 0) {
        /* The pixel data itself is not touched. The renderer will need it, but
         * the engine's bookkeeping only needs the upload to have happened. */
        g_gl_tex_uploads++;
        return;
    }
    if (strcmp(name, "glTexParameteri") == 0 ||
        strcmp(name, "glTexParameterf") == 0 ||
        strcmp(name, "glTexParameterx") == 0) {
        g_gl.tex_params[(a1 >> 5) & 7] = a2;
        return;
    }
    if (strcmp(name, "glTexEnvf") == 0 || strcmp(name, "glTexEnvx") == 0) {
        g_gl.tex_env[(a1 >> 5) & 7] = a2;
        return;
    }
    if (strcmp(name, "glTexEnvxv") == 0)
        return;   /* GLenum target, GLenum pname, const GLfixed *params */

    /* ---- enable / disable ---- */
    if (strcmp(name, "glEnable") == 0)  { set_bit(g_gl.enabled, a0, 1); return; }
    if (strcmp(name, "glDisable") == 0) { set_bit(g_gl.enabled, a0, 0); return; }
    if (strcmp(name, "glIsEnabled") == 0) {
        *ret_lo = (uint32_t)get_bit(g_gl.enabled, a0);
        return;
    }
    if (strcmp(name, "glEnableClientState") == 0)  { set_bit(g_gl.client_enabled, a0, 1); return; }
    if (strcmp(name, "glDisableClientState") == 0) { set_bit(g_gl.client_enabled, a0, 0); return; }

    /* ---- transforms: recorded, not evaluated ----
     * The matrix stack is the renderer's job. Recording the mode is enough for
     * the engine's own queries to be consistent. */
    if (strcmp(name, "glMatrixMode") == 0) { g_gl.matrix_mode = a0; return; }
    if (strcmp(name, "glViewport") == 0) {
        g_gl.viewport[0] = (int32_t)a0;
        g_gl.viewport[1] = (int32_t)a1;
        g_gl.viewport[2] = (int32_t)a2;
        g_gl.viewport[3] = (int32_t)a3;
        return;
    }
    if (strcmp(name, "glScissor") == 0) {
        g_gl.scissor[0] = (int32_t)a0;
        g_gl.scissor[1] = (int32_t)a1;
        g_gl.scissor[2] = (int32_t)a2;
        g_gl.scissor[3] = (int32_t)a3;
        return;
    }

    /* ---- draws ----
     * Counted, not performed. The count is the number the browser port needs:
     * draw calls per frame, and how much vertex data each carries. */
    if (strcmp(name, "glDrawArrays") == 0) {
        /* GLenum mode, GLint first, GLsizei count */
        g_gl_draw_calls++;
        return;
    }
    if (strcmp(name, "glDrawElements") == 0) {
        g_gl_draw_calls++;
        return;
    }
    if (strncmp(name, "glDrawTex", 9) == 0) {
        g_gl_draw_calls++;
        return;
    }

    /* ---- queries ---- */
    if (strcmp(name, "glGetError") == 0) {
        /* Always GL_NO_ERROR. A real error would have to come from somewhere,
         * and nothing here generates one; inventing errors would make the
         * engine take recovery paths that do not exist. */
        *ret_lo = GL_NO_ERROR;
        return;
    }
    if (strcmp(name, "glGetIntegerv") == 0) {
        uint32_t v = 0;
        int n = 0;
        if (query_constant(a0, &v, &n)) {
            for (int i = 0; i < n; i++)
                ST32(a1 + i * 4, v);
            return;
        }
        switch (a0) {
        case GL_VIEWPORT:
            for (int i = 0; i < 4; i++)
                ST32(a1 + i * 4, (uint32_t)g_gl.viewport[i]);
            return;
        case GL_MATRIX_MODE:
            ST32(a1, g_gl.matrix_mode);
            return;
        case GL_TEXTURE_BINDING_2D:
            ST32(a1, g_gl.cur_texture_2d);
            return;
        case GL_ALIASED_POINT_SIZE_RANGE:
        case GL_ALIASED_LINE_WIDTH_RANGE:
            /* GLfloat[2]: min and max. */
            ST32(a1, 0x3F800000u);      /* 1.0f */
            ST32(a1 + 4, 0x42800000u);  /* 64.0f */
            return;
        default:
            ST32(a1, 0);
            return;
        }
    }
    if (strcmp(name, "glGetFixedv") == 0) {
        /* GLfixed output: 16.16. Mirrors glGetIntegerv's scalars. */
        uint32_t v = 0;
        int n = 0;
        if (query_constant(a0, &v, &n)) {
            ST32(a1, v << 16);
            return;
        }
        ST32(a1, 0);
        return;
    }
    if (strcmp(name, "glGetFloatv") == 0) {
        uint32_t v = 0;
        int n = 0;
        if (query_constant(a0, &v, &n)) {
            ST32(a1, 0x4B000000u | (v & 0x7FFFFFu));   /* (float)v, adequate here */
            return;
        }
        ST32(a1, 0);
        return;
    }
    if (strcmp(name, "glGetPointerv") == 0) {
        if (a1)
            ST32(a1, 0);
        return;
    }
    if (strcmp(name, "glGetString") == 0) {
        /* A NUL-terminated string in guest memory. The engine logs it and may
         * branch on the vendor; answering as a fixed-function GLES1 device is
         * the honest description of what this layer pretends to be. */
        static uint32_t cached;
        static uint32_t cached_for;
        if (!cached || cached_for != a0) {
            const char *s = "OpenGL ES-CM 1.1";
            if (a0 == 0x1F01) s = "Inotia3 wasm";        /* GL_RENDERER */
            else if (a0 == 0x1F00) s = "soloader";       /* GL_VENDOR   */
            else if (a0 == 0x1F03) s = "";               /* GL_EXTENSIONS */
            size_t n = strlen(s) + 1;
            cached = guest_heap_alloc((uint32_t)n);
            memcpy(g_mem + cached, s, n);
            cached_for = a0;
        }
        *ret_lo = cached;
        return;
    }

    /* ---- framebuffer objects ----
     * Render-to-texture. IDs are allocated from the same monotonic source as
     * textures, and glCheckFramebufferStatus reports complete: an incomplete
     * status sends the engine down a fallback path that does not exist here,
     * and the renderer will be the thing that decides completeness later. */
    if (strcmp(name, "glGenFramebuffersOES") == 0 ||
        strcmp(name, "glGenFramebuffers") == 0 ||
        strcmp(name, "glGenRenderbuffersOES") == 0 ||
        strcmp(name, "glGenRenderbuffers") == 0 ||
        strcmp(name, "glGenBuffers") == 0) {
        for (uint32_t i = 0; i < a0; i++)
            ST32(a1 + i * 4, g_gl.next_texture_id++);
        return;
    }
    if (strcmp(name, "glCheckFramebufferStatusOES") == 0 ||
        strcmp(name, "glCheckFramebufferStatus") == 0) {
        *ret_lo = 0x8CD5;   /* GL_FRAMEBUFFER_COMPLETE_OES */
        return;
    }

    /* Everything else in the table is state this layer does not need to keep:
     * the calls are recorded as crossed and answered, and the renderer will
     * decide what of it matters. Silence here is deliberate -- see gl.h. */
    (void)a2; (void)a3;
}

/* ---- report --------------------------------------------------------------- */

typedef struct { const char *name; uint64_t calls; } gl_stat;
static gl_stat g_stats[160];
static int     g_nstats;

void gl_note_call(const char *name)
{
    for (int i = 0; i < g_nstats; i++)
        if (g_stats[i].name == name) { g_stats[i].calls++; return; }
    if (g_nstats < (int)(sizeof(g_stats) / sizeof(g_stats[0]))) {
        g_stats[g_nstats].name = name;
        g_stats[g_nstats].calls = 1;
        g_nstats++;
    }
}

void gl_report(int top_n)
{
    if (!g_nstats) {
        host_log("\n--- GL: nothing called ---\n");
        return;
    }
    host_log("\n--- GL activity ---\n");
    host_log("  draw calls        : %llu\n", (unsigned long long)g_gl_draw_calls);
    host_log("  texture objects   : %llu\n", (unsigned long long)g_gl_textures_created);
    host_log("  texture uploads   : %llu\n", (unsigned long long)g_gl_tex_uploads);

    /* Insertion sort over a small table: the report runs once and the entry
     * count is bounded by the import table. */
    int order[160];
    for (int i = 0; i < g_nstats; i++) order[i] = i;
    for (int i = 0; i < g_nstats; i++)
        for (int j = i + 1; j < g_nstats; j++)
            if (g_stats[order[j]].calls > g_stats[order[i]].calls) {
                int t = order[i]; order[i] = order[j]; order[j] = t;
            }
    host_log("  most-called entry points:\n");
    for (int i = 0; i < g_nstats && i < top_n; i++)
        host_log("    %-28s %8llu\n", g_stats[order[i]].name,
                 (unsigned long long)g_stats[order[i]].calls);
}
