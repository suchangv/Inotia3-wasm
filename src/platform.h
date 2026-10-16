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
extern int g_browser_dump_screen;
extern int g_browser_gl_trace;

/* Per-instruction trace sink, written from inside the interpreter loop.
 * Format is a contract with tools/oracle.py. */
void host_trace_open(const char *path);
void host_trace_close(void);

#endif /* PLATFORM_H */
