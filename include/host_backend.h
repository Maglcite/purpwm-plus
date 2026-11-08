/*
 * purpwm+ — the contract between the compositor core and its backends.
 *
 * Backends (src/backend_drm.c, src/backend_wayland.c for --nested and --headless) own the GPU,
 * the outputs and the input devices; the core (server.c, input.c, render.c, ...) owns Wayland.
 * Backends talk to the core ONLY through this header, which therefore never includes
 * <wayland-server*.h>: src/backend_wayland.c includes it next to <wayland-client.h>,
 * <wayland-egl.h> and "xdg-shell-client-protocol.h" with zero symbol collisions. Backends reach
 * monitors through the pure purp_monitor_t model (wm.h).
 *
 * Lifecycle (every backend's run function):
 *
 *     purp_server_t *s = purp_host_server_create(opts);          // wl_display, socket, globals
 *     ... create EGLDisplay/EGLContext, make it current ...
 *     purp_host_set_backend(s, &ops, backend);
 *     purp_host_renderer_init(s, egl_display, egl_context, render_node);  // + linux-dmabuf
 *     purp_host_monitor_add(s, &desc) for each output;
 *     purp_host_load_saved_monitors(s);                          // monitors.json
 *     purp_host_start(s);                                        // IPC/capture, autostart, log
 *     while (!purp_host_should_quit(s)) {
 *         purp_host_dispatch(s, timeout_ms);                     // clients + backend fds
 *         purp_host_tick(s);                                     // signals, IPC, UI, glide...
 *         for each monitor i ready to draw:
 *             if (purp_host_monitor_needs_frame(s, i)) {
 *                 bind target; purp_host_render_monitor(s, i, w, h, y_flip); present;
 *                 purp_host_monitor_frame_presented(s, i);
 *             }
 *         purp_host_flush_clients(s);
 *     }
 *     purp_host_server_destroy(s);                               // shutdown_session + teardown
 *
 * Everything runs on one thread; all functions here must be called from it (except the
 * signal flags, which handlers only store into).
 */
#ifndef PURPWM_HOST_BACKEND_H
#define PURPWM_HOST_BACKEND_H

#include <signal.h>
#include <sys/types.h>

/* Keep <EGL/eglplatform.h> from pulling in Xlib (the Makefile defines these too). */
#ifndef EGL_NO_X11
#define EGL_NO_X11
#endif
#ifndef MESA_EGL_NO_X11_HEADERS
#define MESA_EGL_NO_X11_HEADERS
#endif
#include <EGL/egl.h>

#include "types.h"
#include "wm.h"

/* =========================================================================================== */
/* Command line                                                                               */
/* =========================================================================================== */

typedef enum purp_backend_kind {
    PURP_BACKEND_AUTO = 0,   /* DRM when standalone without a host display, else nested */
    PURP_BACKEND_DRM,        /* --drm */
    PURP_BACKEND_NESTED,     /* --winit / --nested: a window inside a host Wayland session */
    PURP_BACKEND_HEADLESS,   /* --headless: surfaceless/GBM offscreen outputs, no input */
} purp_backend_kind_t;

/* Parsed CLI (src/main.c):
 *   --drm | --winit | --nested | --headless   backend selection
 *   --standalone | --fullscreen               native-resolution fullscreen session
 *                                             (also when $PURPWM_STANDALONE is set)
 *   --spawn-terminal                          start a terminal once ready
 *   --no-autostart                            skip Spotlight daemon, portal, polkit, keyring,
 *                                             radios, notifications and AUTOSTART_COMMANDS
 *                                             (also when $PURPWM_AUTOSTART == "0")
 *   -h | --help                               usage + keybinding summary
 * Unknown arguments are ignored. */
typedef struct purp_options {
    purp_backend_kind_t backend;
    bool standalone;
    bool spawn_terminal;
    bool autostart;
} purp_options_t;

/* Signal flags set by src/main.c's handlers (async-signal-safe stores only) and consumed by
 * purp_host_tick(): SIGUSR1 = screenshot, SIGUSR2 = lock, SIGINT/SIGTERM = quit. */
extern volatile sig_atomic_t purp_signal_screenshot;
extern volatile sig_atomic_t purp_signal_lock;
extern volatile sig_atomic_t purp_signal_quit;

/* Backend entry points; return the process exit status. */
int purp_backend_drm_run(const purp_options_t *opts);       /* src/backend_drm.c */
int purp_backend_nested_run(const purp_options_t *opts);    /* src/backend_wayland.c */
int purp_backend_headless_run(const purp_options_t *opts);  /* src/backend_wayland.c */

/* =========================================================================================== */
/* Backend callbacks (implemented by each backend, called by the core)                        */
/* =========================================================================================== */

typedef struct purp_backend_ops {
    const char *name;                 /* "drm", "nested", "headless" */
    /* Whether apply_monitors may invent modes (nested/headless) or must use listed ones (DRM). */
    bool allow_custom_modes;
    /* Bring hardware in line with the wm monitors (enabled, physical_size/refresh mode, scale,
     * position): called after monitors.json, IPC apply_monitors, scale changes and hotplug
     * normalisation. Return false to keep wm->monitors_dirty set and retry next tick. */
    bool (*apply_monitors)(void *backend, purp_server_t *server);
    /* Make the backend's EGL context current (before offscreen captures and texture uploads
     * from outside the frame path). */
    bool (*make_current)(void *backend);
    /* Ctrl+Alt+F<n> / XF86Switch_VT_<n>; NULL when unsupported (nested/headless). */
    bool (*switch_vt)(void *backend, int vt);
    /* A monitor got damaged: wake the backend so it renders soon (DRM: render now when no flip
     * is pending). May be NULL when the backend polls purp_host_monitor_needs_frame(). */
    void (*schedule_frame)(void *backend, size_t monitor);
} purp_backend_ops_t;

/* =========================================================================================== */
/* Server lifecycle and event loop (src/server.c)                                             */
/* =========================================================================================== */

/* Create the display, bind the first free wayland-1..32 socket, export WAYLAND_DISPLAY, create
 * the renderer-independent globals and the UI state. NULL on fatal errors (logged). */
purp_server_t *purp_host_server_create(const purp_options_t *opts);
/* shutdown_session (SIGTERM session-helper process groups), close sockets, free everything. */
void purp_host_server_destroy(purp_server_t *server);
/* Register the backend callbacks (ops must outlive the server). */
void purp_host_set_backend(purp_server_t *server, const purp_backend_ops_t *ops, void *backend);
/* WAYLAND_DISPLAY socket name ("wayland-1"). */
const char *purp_host_socket_name(const purp_server_t *server);
/* Apply monitors.json (purp_load_saved_monitors) without persisting; logs and ignores errors
 * ("Ignoring saved monitors.json: ..."). Called at startup, on VT resume and after hotplug. */
void purp_host_load_saved_monitors(purp_server_t *server);
/* Bind IPC + capture sockets, log "purpwm+ listening on WAYLAND_DISPLAY=...", run autostart
 * (keyring, portal, network/bluetooth, Spotlight daemon, notifications, AUTOSTART_COMMANDS,
 * polkit) unless disabled (PURPWM_NOTIFY_DBUS still starts notifications), then
 * --spawn-terminal. */
bool purp_host_start(purp_server_t *server);
bool purp_host_should_quit(const purp_server_t *server);
void purp_host_request_quit(purp_server_t *server);

/* Event sources multiplexed into the core's wl_event_loop (backend fds, timers). */
typedef struct purp_event_source purp_event_source_t;
#define PURP_FD_READABLE (1u << 0)
#define PURP_FD_WRITABLE (1u << 1)
#define PURP_FD_HANGUP   (1u << 2)
#define PURP_FD_ERROR    (1u << 3)
/* Callback: `mask` is PURP_FD_* bits; return value is ignored (0 by convention). */
typedef int (*purp_fd_func_t)(int fd, uint32_t mask, void *data);
typedef int (*purp_timer_func_t)(void *data);
purp_event_source_t *purp_host_add_fd(purp_server_t *server, int fd, uint32_t mask,
                                      purp_fd_func_t func, void *data);
purp_event_source_t *purp_host_add_timer(purp_server_t *server, purp_timer_func_t func,
                                         void *data);
/* Arm a timer `ms` from now (0 disarms). */
void purp_host_timer_update(purp_event_source_t *source, int ms);
void purp_host_remove_source(purp_event_source_t *source);
/* Poll fd of the core loop (for backends that wait on several loops). */
int purp_host_event_fd(const purp_server_t *server);
/* Dispatch clients and backend sources, waiting at most `timeout_ms` (-1 = forever). */
int purp_host_dispatch(purp_server_t *server, int timeout_ms);
/* wl_display_flush_clients. */
void purp_host_flush_clients(purp_server_t *server);
/*
 * Periodic work (call every iteration / every PURP_TICK_INTERVAL_MS): consume signal flags
 * (screenshot, lock, quit), hand finished screenshots to the clipboard, poll IPC and capture
 * sockets, tick the bar status, lock screen, notifications and the keyboard glide, apply
 * pending VT switches and dirty monitors (ops->apply_monitors), run maintenance (activation
 * token TTL, child reaping, 30 s malloc_trim). Returns true when something visible changed (the
 * affected monitors are already marked as needing a frame).
 */
bool purp_host_tick(purp_server_t *server);

/* =========================================================================================== */
/* Monitors (src/server.c on top of the wm model)                                             */
/* =========================================================================================== */

/* The WM model (read-only use by backends except through the functions below). */
purp_wm_t *purp_host_wm(purp_server_t *server);
size_t purp_host_monitor_count(const purp_server_t *server);
purp_monitor_t *purp_host_monitor(purp_server_t *server, size_t idx);
/* Add (or refresh, by name) a monitor, publish its wl_output and relayout. Returns its index or
 * -1. Initial scale rules belong to the backend (desc->scale). */
int purp_host_monitor_add(purp_server_t *server, const purp_monitor_desc_t *desc);
/* Hotplug: keep only monitors named in `names`; server-side outputs, surfaces (wl_surface.leave),
 * layer surfaces (explicit-output ones on removed monitors are closed) and touch pins follow. */
void purp_host_monitors_retain(purp_server_t *server, const char *const *names, size_t count);
/* Nested window resized / virtual mode changed: update physical size and relayout. */
void purp_host_monitor_set_physical(purp_server_t *server, size_t idx, int32_t width,
                                    int32_t height, int32_t refresh_mhz);
/* Damage tracking: whether monitor `idx` needs a new frame; mark one / all monitors dirty. */
bool purp_host_monitor_needs_frame(const purp_server_t *server, size_t idx);
void purp_host_damage_monitor(purp_server_t *server, size_t idx);
void purp_host_damage_all(purp_server_t *server);
/* A frame for `idx` reached the screen (DRM: page flip / VBlank; nested: after swap): clears the
 * damage flag and sends wl_surface.frame done to surfaces visible on that monitor (windows on
 * its tags, popups, layers, the cursor surface and DnD icon when the pointer is on it) — none
 * while locked. */
void purp_host_monitor_frame_presented(purp_server_t *server, size_t idx);

/* =========================================================================================== */
/* Rendering (src/render.c)                                                                   */
/* =========================================================================================== */

/* GL framebuffer object + colour texture (RGBA8) for offscreen targets. */
typedef struct purp_offscreen {
    uint32_t fbo;
    uint32_t texture;
    int32_t width;
    int32_t height;
} purp_offscreen_t;

/* Initialise the GLES2 renderer on the current context and create the zwp_linux_dmabuf_v1 global
 * (v4 with feedback; formats queried from EGL). `render_node` is the main device for dmabuf
 * feedback (0 when unknown: dmabuf is then advertised without feedback). */
bool purp_host_renderer_init(purp_server_t *server, EGLDisplay display, EGLContext context,
                             dev_t render_node);
/* Release all GL objects (context must be current). */
void purp_host_renderer_finish(purp_server_t *server);
/*
 * Compose monitor `idx` into the currently bound framebuffer of fb_width x fb_height physical px.
 * `y_flip` = true for window-system surfaces (EGL window / GBM scanout: GL row 0 is the bottom)
 * and false for offscreen FBOs (row 0 = logical top, readable top-down without flipping).
 * While locked only the lock screen is drawn. Returns false on GL errors.
 */
bool purp_host_render_monitor(purp_server_t *server, size_t idx, int32_t fb_width,
                              int32_t fb_height, bool y_flip);
/* (Re)create `target` at width x height (no-op when unchanged). */
bool purp_host_offscreen_ensure(purp_server_t *server, purp_offscreen_t *target, int32_t width,
                                int32_t height);
void purp_host_offscreen_bind(const purp_offscreen_t *target);
void purp_host_offscreen_release(purp_server_t *server, purp_offscreen_t *target);
/* Host-window composition of offscreen tiles (nested virtual monitors): clear the bound
 * framebuffer, draw textures, finish. Rectangles are in physical px of the bound framebuffer. */
void purp_host_render_begin(purp_server_t *server, int32_t fb_width, int32_t fb_height,
                            bool y_flip, purp_color_t clear);
void purp_host_render_texture(purp_server_t *server, uint32_t texture, int32_t tex_width,
                              int32_t tex_height, purp_rect_t src, purp_rect_t dst, float alpha);
void purp_host_render_end(purp_server_t *server);

/* =========================================================================================== */
/* Input (src/input.c)                                                                        */
/* =========================================================================================== */

/* Raw evdev keycode (no +8 offset); the core runs xkb, bindings, the lock screen and forwarding. */
void purp_host_keyboard_key(purp_server_t *server, uint32_t time_ms, uint32_t keycode,
                            bool pressed);
/* Relative pointer motion in logical px (accelerated and raw). */
void purp_host_pointer_motion(purp_server_t *server, uint32_t time_ms, double dx, double dy,
                              double dx_unaccel, double dy_unaccel);
/* Absolute pointer position in global logical coordinates (backend maps tablets / host windows
 * onto purp_wm_desktop_bounds or a monitor rectangle). */
void purp_host_pointer_motion_absolute(purp_server_t *server, uint32_t time_ms, double x,
                                       double y);
/* Linux button code (BTN_LEFT = 0x110 ...). */
void purp_host_pointer_button(purp_server_t *server, uint32_t time_ms, uint32_t button,
                              bool pressed);

typedef enum purp_axis_source {
    PURP_AXIS_SOURCE_WHEEL = 0,
    PURP_AXIS_SOURCE_FINGER,
    PURP_AXIS_SOURCE_CONTINUOUS,
    PURP_AXIS_SOURCE_WHEEL_TILT,
} purp_axis_source_t;

typedef struct purp_axis_event {
    uint32_t time_ms;
    purp_axis_source_t source;
    bool has_vertical;
    double vertical;             /* logical scroll distance (wl_pointer.axis value) */
    int32_t vertical_v120;       /* high-resolution wheel steps (0 if none) */
    bool has_horizontal;
    double horizontal;
    int32_t horizontal_v120;
    bool inverted;               /* natural scrolling (axis_relative_direction) */
} purp_axis_event_t;

void purp_host_pointer_axis(purp_server_t *server, const purp_axis_event_t *event);
/* End of a group of pointer events (wl_pointer.frame). */
void purp_host_pointer_frame(purp_server_t *server);
/* Touch in global logical coordinates; `slot` is the device slot id (pinned to the monitor where
 * it went down by the backend). */
void purp_host_touch_down(purp_server_t *server, uint32_t time_ms, int32_t slot, double x,
                          double y);
void purp_host_touch_motion(purp_server_t *server, uint32_t time_ms, int32_t slot, double x,
                            double y);
void purp_host_touch_up(purp_server_t *server, uint32_t time_ms, int32_t slot);
void purp_host_touch_cancel(purp_server_t *server, int32_t slot);
void purp_host_touch_frame(purp_server_t *server);
/* VT switched away (false) or back (true): releases held keys/buttons/touches and glides on
 * pause; damages everything on resume. */
void purp_host_session_active(purp_server_t *server, bool active);

#endif /* PURPWM_HOST_BACKEND_H */
