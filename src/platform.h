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

/* Per-instruction trace sink, written from inside the interpreter loop.
 * Format is a contract with tools/oracle.py. */
void host_trace_open(const char *path);
void host_trace_close(void);

#endif /* PLATFORM_H */
