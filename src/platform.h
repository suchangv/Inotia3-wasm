/*
 * platform.h -- services the core needs but cannot provide itself.
 *
 * guest.h declares the four that every port must supply (host_log, host_alloc,
 * host_now, host_fatal). This header adds the ones that only *some* hosts can
 * answer, so that a host which cannot answer one yet fails visibly at the
 * import boundary rather than at link time -- the point being that the boot
 * path should not have to wait for the asset path to be written.
 */
#ifndef PLATFORM_H
#define PLATFORM_H

#include <stdint.h>

/* Guest mmap arena. Returns a guest address, or 0 on exhaustion. */
uint32_t host_guest_mmap(uint32_t size);

/* The directory the engine treats as its writable app storage -- what
 * WrapperJinterface.GetUserDir() returns -- after creating it and the
 * per-package directory under it.
 *
 * Both halves matter. The result has to end with a slash, because the engine
 * concatenates the package name onto it directly: a missing separator yields
 * "/datacom.com2us...". (The PS Vita port returns "ux0:data/" for exactly this
 * reason.) And the directories have to exist, because the engine never creates
 * them -- on Android the OS does, and here nothing did, so every
 * GAMELOADER_Install failed, INITSTATE_nState went to 3 (a terminal "File
 * installation error" state) and the game sat on the loading screen forever.
 */
const char *host_app_data_dir(const char *package);

/* Optional: translate a host-level crash back into a guest PC. A no-op where
 * the host has no such notion (the browser). */
void host_install_crash_handler(void);

/* The host's own locale, which is what WrapperJinterface.GetLanguage() answers
 * on Android (Locale.getDefault()) and what the engine turns into a language:
 * the first two letters pick the text file (memorytext.dat / _en / _jp / _fr /
 * _zhhans / _zhhant -- all UTF-8), so "ko-KR", "zh-CN" and "fr-FR" all work.
 * The browser answers with navigator.language; the native runner has no locale
 * of its own and takes SPIKE_LANG=... or "en". */
const char *host_language(void);

/* Browser build only: create the WebGL context on the generated canvas.
 * Returns 0 if the context could not be created. */
int host_browser_gl_setup(void);

/* Tell the browser build what size the engine believes the screen is, before
 * the canvas is created. */
void host_browser_set_size(int w, int h);

/* Browser build only: let the page choose the frame count. */
void host_browser_read_frames(void);
void host_browser_read_dump(void);
void host_browser_read_gl_trace(void);
void host_browser_read_fixed(void);
void host_browser_read_frame_ms(void);
void host_browser_read_trace(void);
void spike_set_trace(int n);
extern int g_browser_dump_screen;
extern int g_browser_gl_trace;
void host_browser_read_pixels(unsigned char *out, int w, int h, int step);

/* Browser build only: hand the engine's tick function to requestAnimationFrame.
 *
 * Does not return: main() must return straight after this so the rAF chain owns
 * the page. `tick` returns 0 when the run is over. */
void host_browser_main_loop(int (*tick)(void));

/* Browser build only: start delivering pointer events. Call after the canvas
 * exists and before the first tick, so the first frame can already see input. */
void host_browser_input_init(void);

/* Browser build only: ?fixed=1, a fixed 16 ms step instead of real elapsed
 * time. Read at startup. */
extern int g_browser_fixed;

/* Browser build only: ?trace=1, the interpreter's instruction ring. Read at
 * startup. */
extern int g_browser_trace;

/* ---- Android (NativeActivity) -------------------------------------------
 *
 * The app's window, input queue and storage are the platform's; android_main()
 * in main.c drives the engine. These are the pieces that loop cannot do
 * itself. Implemented in platform_android.c, which also owns the EGL surface
 * and the letterbox mapping between the engine's 960x544 and the device's
 * window.
 */
#ifdef __ANDROID__
struct android_app;
void host_android_init(struct android_app *app);
/* Copy libgame.so and assets.apk out of the APK, then chdir to the app's
 * private files directory. Returns 0 on success. */
int  host_android_prepare(void);
/* Process the looper: 0 drains pending events and returns, >0 waits up to that
 * many milliseconds (the loop's sleep, and how input still arrives while the
 * engine has nothing to draw), and it blocks outright while there is no window.
 * Returns 0 when the activity is going away. */
int  host_android_pump(int timeout_ms);
int  host_android_have_window(void);
/* Clear the whole window surface (black), scissor off. Called before each tick
 * so the letterbox bars are black rather than stale buffer contents. */
void host_android_begin_frame(void);
void host_android_present(void);
void host_android_window_size(int *w, int *h);

/* Called for every glViewport the engine issues. The engine's screen is
 * 960x544, the window is the device's: the host scales the rect into a centred
 * letterbox and installs the GL viewport itself. Returns 1 if it did. */
int host_adjust_viewport(int32_t *x, int32_t *y, int32_t *w, int32_t *h);
#endif

/* Resolve one GL entry point from the *driver* (not from gl4es). Every host
 * that links gl4es statically needs it: the static archive also exports the
 * unprefixed gl* names, so a plain call in port code binds to gl4es' own
 * implementation, which is not what a driver-level lookup is for. */
#if defined(__ANDROID__) || defined(SPIKE_SDL)
void *host_gl_proc(const char *name);
#endif

/* ---- SDL2 (the handheld, and a desktop window) --------------------------
 *
 * SDL2 owns the window, the GL context, the gamepad and the clock; the port
 * keeps the same host_* contract as everywhere else. main.c drives it exactly
 * like the Android loop: pump, tick, present only when the engine drew.
 */
#ifdef SPIKE_SDL
int  host_sdl_init(void);
int  host_sdl_vdprobe(void);
/* 0/90/180/270: render offscreen and rotate on present (portrait panels). */
void host_sdl_set_rotation(int deg);   /* --vdprobe: ask every video driver why */
void host_sdl_finish(void);
/* Process events, waiting up to `timeout_ms` for one. Returns 0 to quit. */
int  host_sdl_pump(int timeout_ms);
/* Clear the whole window black and put the viewport inside the letterbox. */
void host_sdl_begin_frame(void);
void host_sdl_present(void);

extern int  g_sdl_windowed;        /* --windowed: 960x544 window */
extern int  g_sdl_frames;          /* --frames=N: stop after N engine frames */
extern const char *g_sdl_shot;     /* --shot=FILE: write a PPM */
extern int  g_sdl_shot_frame;      /* presented frames to wait before the shot */
extern int  g_sdl_cursor_x, g_sdl_cursor_y;  /* --cursor=x,y: show the pointer */

int host_adjust_viewport(int32_t *x, int32_t *y, int32_t *w, int32_t *h);
#endif

/* ---- MiniLoong (RK3566, aarch64 Linux) ----------------------------------
 *
 * A PortMaster game: no Android, no browser, no X server. The window is an EGL
 * surface on whatever display the handheld's compositor hands us, the input is
 * evdev gamepad events, and the audio is ALSA. loong_main() in main.c drives
 * the engine; these are the pieces that loop cannot do itself.
 *
 * Deliberately not SDL. SDL2 would be a second windowing dependency that the
 * device is not guaranteed to ship headers for, while EGL is what every other
 * PortMaster game on this hardware already uses. evdev is a handful of ioctls.
 */
#if defined(__aarch64__) && defined(GUEST_PORT_LOONG)
int  host_loong_init(void);
/* Open the display and create the GLES context. Separate from init() because
 * the engine must not touch GL until there is a surface. Returns 0 on success. */
int  host_loong_create_window(void);
/* Drain input events (non-blocking) and return 1 if the app should keep going,
 * 0 if the user asked to quit. */
int  host_loong_pump(void);
void host_loong_begin_frame(void);
void host_loong_present(void);
/* The engine's 960x544 viewport mapped into a centred letterbox of the panel. */
int  host_loong_adjust_viewport(int32_t *x, int32_t *y, int32_t *w, int32_t *h);
void host_loong_window_size(int *w, int *h);
#endif

/* Per-instruction trace sink, written from inside the interpreter loop.
 * Format is a contract with tools/oracle.py. */
void host_trace_open(const char *path);
void host_trace_close(void);

/* ---- durable storage -----------------------------------------------------
 *
 * The loader reports every write it makes under the app's data directory and
 * the platform decides when that becomes durable. On a real filesystem there is
 * nothing to do and these are no-ops. In the browser /data is IDBFS, so
 * "durable" means FS.syncfs -- without it a reload silently loses every save,
 * which is what the browser build used to do.
 *
 *   host_storage_dirty(path)  something under /data changed (cheap, prefix test)
 *   host_storage_flush()      make it durable now (the engine called fsync)
 *   host_storage_tick()       once per frame: rate-limited flush of anything
 *                             still pending, for writes that never get an
 *                             fsync
 */
void host_storage_dirty(const char *path);
void host_storage_flush(void);
void host_storage_tick(void);

#endif /* PLATFORM_H */
