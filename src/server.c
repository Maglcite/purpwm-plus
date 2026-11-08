/*
 * purpwm+ — server lifecycle, event loop, monitors, spawning, screenshots, lock and maintenance
 * (src/server.c).
 *
 * Owns every purp_host_* lifecycle/monitor entry point of host_backend.h and the purp_server_*
 * helpers of purpwm.h. Everything runs on the compositor thread except the detached helper
 * threads started here (screenshot encoder, network bring-up, malloc_trim), which only touch
 * purp_blob_t data and the mutex-guarded screenshot queue.
 */
#include "purpwm.h"
#include "render.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <malloc.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

/* =========================================================================================== */
/* Small helpers                                                                              */
/* =========================================================================================== */

/* Screenshot encoder threads still running (purp_host_server_destroy waits for them). */
static atomic_int screenshot_jobs_inflight;

static bool spawn_detached_thread(void *(*func)(void *), void *arg) {
    pthread_attr_t attr;
    if (pthread_attr_init(&attr) != 0) return false;
    int rc = pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_t thread;
    if (rc == 0) rc = pthread_create(&thread, &attr, func, arg);
    pthread_attr_destroy(&attr);
    if (rc != 0) PURP_WARN("Failed to start helper thread: %s", strerror(rc));
    return rc == 0;
}

static void sleep_ms(long ms) {
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {
    }
}

/* mkdir -p (existing directories are fine). */
static bool mkdir_p(const char *path, mode_t mode) {
    char buf[PATH_MAX];
    size_t len = strlen(path);
    if (len == 0 || len >= sizeof(buf)) {
        errno = ENAMETOOLONG;
        return false;
    }
    memcpy(buf, path, len + 1);
    for (size_t i = 1; i <= len; ++i) {
        if (buf[i] != '/' && buf[i] != '\0') continue;
        char saved = buf[i];
        buf[i] = '\0';
        if (mkdir(buf, mode) != 0 && errno != EEXIST) return false;
        buf[i] = saved;
    }
    struct stat st;
    if (stat(path, &st) != 0) return false;
    if (!S_ISDIR(st.st_mode)) {
        errno = ENOTDIR;
        return false;
    }
    return true;
}

/* Directory part of `path` (mkdir -p before writing a file). */
static bool parent_dir(const char *path, char *out, size_t out_len) {
    const char *slash = strrchr(path, '/');
    if (!slash || slash == path) return false;
    size_t len = (size_t)(slash - path);
    if (len >= out_len) return false;
    memcpy(out, path, len);
    out[len] = '\0';
    return true;
}

/* Dispositions to SIG_DFL and an empty mask: async-signal-safe, used between fork and exec. */
static void child_reset_signals(const struct sigaction *dfl, const sigset_t *empty) {
    for (int sig = 1; sig < NSIG; ++sig) {
        if (sig == SIGKILL || sig == SIGSTOP) continue;
        sigaction(sig, dfl, NULL);
    }
    sigprocmask(SIG_SETMASK, empty, NULL);
}

/* dup2 that also works when `fd` already is `target` (clears close-on-exec instead). */
static void child_dup_to(int fd, int target) {
    if (fd < 0) return;
    if (fd == target) fcntl(fd, F_SETFD, 0);
    else dup2(fd, target);
}

static void server_damage_bars(purp_server_t *server) {
    for (size_t i = 0; i < server->wm.monitor_count; ++i)
        if (server->wm.monitors[i].enabled && purp_wm_bar_visible_on(&server->wm, i))
            purp_host_damage_monitor(server, i);
}

/* =========================================================================================== */
/* Event sources                                                                              */
/* =========================================================================================== */

struct purp_event_source {
    struct wl_event_source *source;
    purp_fd_func_t fd_func;
    purp_timer_func_t timer_func;
    void *data;
};

static int event_source_fd_dispatch(int fd, uint32_t mask, void *data) {
    purp_event_source_t *src = data;
    uint32_t purp_mask = 0;
    if (mask & WL_EVENT_READABLE) purp_mask |= PURP_FD_READABLE;
    if (mask & WL_EVENT_WRITABLE) purp_mask |= PURP_FD_WRITABLE;
    if (mask & WL_EVENT_HANGUP) purp_mask |= PURP_FD_HANGUP;
    if (mask & WL_EVENT_ERROR) purp_mask |= PURP_FD_ERROR;
    if (src->fd_func) src->fd_func(fd, purp_mask, src->data);
    return 0;
}

static int event_source_timer_dispatch(void *data) {
    purp_event_source_t *src = data;
    if (src->timer_func) src->timer_func(src->data);
    return 0;
}

purp_event_source_t *purp_host_add_fd(purp_server_t *server, int fd, uint32_t mask,
                                      purp_fd_func_t func, void *data) {
    if (!server || !server->loop || fd < 0 || !func) return NULL;
    purp_event_source_t *src = calloc(1, sizeof(*src));
    if (!src) return NULL;
    uint32_t wl_mask = 0;
    if (mask & PURP_FD_READABLE) wl_mask |= WL_EVENT_READABLE;
    if (mask & PURP_FD_WRITABLE) wl_mask |= WL_EVENT_WRITABLE;
    src->fd_func = func;
    src->data = data;
    src->source = wl_event_loop_add_fd(server->loop, fd, wl_mask, event_source_fd_dispatch, src);
    if (!src->source) {
        PURP_WARN("Failed to watch fd %d: %s", fd, strerror(errno));
        free(src);
        return NULL;
    }
    return src;
}

purp_event_source_t *purp_host_add_timer(purp_server_t *server, purp_timer_func_t func,
                                         void *data) {
    if (!server || !server->loop || !func) return NULL;
    purp_event_source_t *src = calloc(1, sizeof(*src));
    if (!src) return NULL;
    src->timer_func = func;
    src->data = data;
    src->source = wl_event_loop_add_timer(server->loop, event_source_timer_dispatch, src);
    if (!src->source) {
        free(src);
        return NULL;
    }
    return src;
}

void purp_host_timer_update(purp_event_source_t *source, int ms) {
    if (source && source->source) wl_event_source_timer_update(source->source, ms > 0 ? ms : 0);
}

void purp_host_remove_source(purp_event_source_t *source) {
    if (!source) return;
    if (source->source) wl_event_source_remove(source->source);
    free(source);
}

int purp_host_event_fd(const purp_server_t *server) {
    return server && server->loop ? wl_event_loop_get_fd(server->loop) : -1;
}

int purp_host_dispatch(purp_server_t *server, int timeout_ms) {
    if (!server || !server->loop) return -1;
    return wl_event_loop_dispatch(server->loop, timeout_ms);
}

void purp_host_flush_clients(purp_server_t *server) {
    if (server && server->display) wl_display_flush_clients(server->display);
}

/* =========================================================================================== */
/* Lifecycle                                                                                  */
/* =========================================================================================== */

static void server_init_runtime_dir(purp_server_t *server) {
    const char *dir = getenv("XDG_RUNTIME_DIR");
    int n = -1;
    if (dir && *dir) n = snprintf(server->runtime_dir, sizeof(server->runtime_dir), "%s", dir);
    if (n <= 0 || (size_t)n >= sizeof(server->runtime_dir)) {
        snprintf(server->runtime_dir, sizeof(server->runtime_dir), "/run/user/%u",
                 (unsigned)getuid());
        /* libwayland resolves socket names against $XDG_RUNTIME_DIR. */
        setenv("XDG_RUNTIME_DIR", server->runtime_dir, 1);
    }
}

static bool server_bind_socket(purp_server_t *server) {
    for (int i = PURP_SOCKET_FIRST; i <= PURP_SOCKET_LAST; ++i) {
        char name[sizeof(server->socket_name)];
        int n = snprintf(name, sizeof(name), "wayland-%d", i);
        if (n <= 0 || (size_t)n >= sizeof(name)) continue;
        if (wl_display_add_socket(server->display, name) == 0) {
            memcpy(server->socket_name, name, (size_t)n + 1);
            return true;
        }
    }
    return false;
}

purp_server_t *purp_host_server_create(const purp_options_t *opts) {
    purp_server_t *server = calloc(1, sizeof(*server));
    if (!server) {
        PURP_ERROR("Out of memory creating the compositor");
        return NULL;
    }
    if (opts) server->options = *opts;
    server->is_nested = server->options.backend != PURP_BACKEND_DRM && !server->options.standalone;
    server->dmabuf_table_fd = -1;
    server->ipc.listen_fd = -1;
    for (size_t i = 0; i < PURP_IPC_MAX_CLIENTS; ++i) server->ipc.clients[i].fd = -1;
    server->capture.listen_fd = -1;
    for (size_t i = 0; i < PURP_CAPTURE_MAX_CLIENTS; ++i) server->capture.clients[i].fd = -1;
    server->touch_grab_slot = -1;
    server->grab.touch_slot = -1;
    wl_list_init(&server->surfaces);
    wl_list_init(&server->xdg_surfaces);
    wl_list_init(&server->layers);
    wl_list_init(&server->outputs);
    wl_list_init(&server->activation_tokens);
    wl_list_init(&server->popup_grabs);
    pthread_mutex_init(&server->screenshot_lock, NULL);
    pthread_mutex_init(&server->children_lock, NULL);
    server->start_ms = purp_now_ms();
    server->last_trim_ms = server->start_ms;
    server_init_runtime_dir(server);

    /* Model and UI state. */
    purp_wm_init(&server->wm, purp_initial_scale());
    purp_help_init(&server->help);
    purp_cursor_theme_init(&server->cursor_theme);
    purp_wallpaper_init_from_env(&server->wallpaper);
    if (!purp_notify_init(&server->notify)) PURP_WARN("Notification center unavailable");
    server->font = purp_font_load();
    if (!server->font) PURP_WARN("No UI font available; text will not be drawn");
    if (!purp_bar_init(&server->bar, server->font, true)) PURP_WARN("Status bar unavailable");

    /* Wayland display and socket. */
    server->display = wl_display_create();
    if (!server->display) {
        PURP_ERROR("Failed to create the Wayland display");
        goto fail;
    }
    server->loop = wl_display_get_event_loop(server->display);
    if (!server_bind_socket(server)) {
        PURP_ERROR("No free Wayland socket in %s (wayland-%d .. wayland-%d)", server->runtime_dir,
                   PURP_SOCKET_FIRST, PURP_SOCKET_LAST);
        goto fail;
    }
    setenv("WAYLAND_DISPLAY", server->socket_name, 1);
    unsetenv("WAYLAND_SOCKET");

    /* Renderer-independent globals (linux-dmabuf arrives with purp_host_renderer_init). */
    if (!purp_surface_globals_init(server) || !purp_seat_init(server) || !purp_xdg_init(server) ||
        !purp_layer_init(server)) {
        PURP_ERROR("Failed to create the Wayland globals");
        goto fail;
    }
    return server;

fail:
    purp_host_server_destroy(server);
    return NULL;
}

void purp_host_server_destroy(purp_server_t *server) {
    if (!server) return;
    purp_server_shutdown_session(server);
    /* The backend is going away: never call back into it from teardown paths. */
    server->backend_ops = NULL;
    server->backend = NULL;
    if (server->ipc.listen_fd >= 0) purp_ipc_server_close(&server->ipc);
    if (server->capture.listen_fd >= 0) purp_capture_server_close(&server->capture);
    if (server->display) wl_display_destroy_clients(server->display);

    /* Workers retain server through their last queue access, regardless of encoding/I/O time. */
    while (atomic_load(&screenshot_jobs_inflight) > 0)
        sleep_ms(10);

    if (server->lock_screen) {
        purp_lock_finish(server->lock_screen);
        free(server->lock_screen);
        server->lock_screen = NULL;
    }
    if (server->seat.global) purp_seat_finish(server);
    purp_dmabuf_finish(server);
    for (size_t i = 0; i < server->wm.monitor_count; ++i) {
        if (!server->wm.monitors[i].output) continue;
        purp_output_destroy(server->wm.monitors[i].output);
        server->wm.monitors[i].output = NULL;
    }
    if (server->display) wl_display_destroy(server->display);
    server->display = NULL;
    server->loop = NULL;

    purp_wm_finish(&server->wm);
    purp_bar_finish(&server->bar);
    purp_notify_finish(&server->notify);
    purp_help_finish(&server->help);
    purp_cursor_theme_finish(&server->cursor_theme);
    purp_wallpaper_finish(&server->wallpaper);
    if (server->font) purp_font_destroy(server->font);
    server->font = NULL;

    pthread_mutex_lock(&server->screenshot_lock);
    for (size_t i = 0; i < server->screenshot_ready_count; ++i)
        purp_blob_unref(server->screenshot_ready[i]);
    server->screenshot_ready_count = 0;
    pthread_mutex_unlock(&server->screenshot_lock);

    pthread_mutex_lock(&server->children_lock);
    free(server->children);
    server->children = NULL;
    server->child_count = server->child_cap = 0;
    pthread_mutex_unlock(&server->children_lock);

    pthread_mutex_destroy(&server->screenshot_lock);
    pthread_mutex_destroy(&server->children_lock);
    free(server);
}

void purp_host_set_backend(purp_server_t *server, const purp_backend_ops_t *ops, void *backend) {
    if (!server) return;
    server->backend_ops = ops;
    server->backend = backend;
}

const char *purp_host_socket_name(const purp_server_t *server) {
    return server ? server->socket_name : "";
}

void purp_host_load_saved_monitors(purp_server_t *server) {
    if (!server) return;
    purp_monitor_config_t entries[PURP_IPC_MAX_MONITORS];
    size_t count = purp_load_saved_monitors(entries);
    if (count == 0) return;
    char err[PURP_IPC_ERROR_MAX] = "";
    if (!purp_server_apply_monitor_configs(server, entries, count, false, err, sizeof(err)))
        PURP_WARN("Ignoring saved monitors.json: %s", err[0] ? err : "invalid configuration");
}

bool purp_host_start(purp_server_t *server) {
    if (!server) return false;
    char path[PATH_MAX];
    char err[PURP_IPC_ERROR_MAX];

    purp_server_socket_path(server, PURP_IPC_SOCKET_PREFIX, path, sizeof(path));
    err[0] = '\0';
    if (!purp_ipc_server_bind(&server->ipc, path, server->runtime_dir, server->is_nested, err,
                              sizeof(err)))
        PURP_WARN("IPC socket %s unavailable: %s", path, err[0] ? err : strerror(errno));

    purp_server_socket_path(server, PURP_CAPTURE_SOCKET_PREFIX, path, sizeof(path));
    err[0] = '\0';
    if (purp_capture_server_bind(&server->capture, path, err, sizeof(err)))
        setenv(PURP_ENV_CAPTURE_SOCKET, path, 1);
    else
        PURP_WARN("Capture socket %s unavailable: %s", path, err[0] ? err : strerror(errno));

    const char *backend_name =
        server->backend_ops && server->backend_ops->name ? server->backend_ops->name : "unknown";
    if (server->wm.monitor_count > 0) {
        const purp_monitor_t *mon =
            &server->wm.monitors[purp_wm_active_monitor_index(&server->wm)];
        PURP_INFO("purpwm+ listening on WAYLAND_DISPLAY=%s (%s backend, %dx%d @ %gx scale, "
                  "%zu monitor%s, standalone=%s)",
                  server->socket_name, backend_name, mon->physical_size.w, mon->physical_size.h,
                  mon->scale, server->wm.monitor_count, server->wm.monitor_count == 1 ? "" : "s",
                  server->options.standalone ? "true" : "false");
    } else {
        PURP_INFO("purpwm+ listening on WAYLAND_DISPLAY=%s (%s backend, no monitors, "
                  "standalone=%s)",
                  server->socket_name, backend_name, server->options.standalone ? "true" : "false");
    }

    if (server->options.autostart) {
        purp_server_spawn_keyring(server);
        purp_server_spawn_screenshot_portal(server);
        purp_server_ensure_network_and_bluetooth(server);
        purp_server_spawn_spotlight_daemon(server);
        purp_server_start_notifications(server);
        purp_server_run_autostart(server);
        purp_server_spawn_polkit_agent(server);
    } else if (getenv(PURP_ENV_NOTIFY_DBUS)) {
        purp_server_start_notifications(server);
    }
    if (server->options.spawn_terminal) purp_server_spawn_terminal(server);

    server->started = true;
    purp_host_damage_all(server);
    return true;
}

bool purp_host_should_quit(const purp_server_t *server) {
    return !server || server->should_quit;
}

void purp_host_request_quit(purp_server_t *server) {
    if (server) server->should_quit = true;
}

bool purp_host_tick(purp_server_t *server) {
    if (!server) return false;
    bool changed = false;
    uint64_t now = purp_now_ms();

    if (purp_signal_quit) {
        purp_signal_quit = 0;
        server->should_quit = true;
    }
    if (purp_signal_lock) {
        purp_signal_lock = 0;
        if (!purp_server_is_locked(server)) {
            purp_server_lock_session(server);
            changed = true;
        }
    }
    if (purp_signal_screenshot) {
        purp_signal_screenshot = 0;
        purp_server_request_screenshot(server, false);
    }
    purp_server_offer_screenshots(server);

    if (server->ipc.listen_fd >= 0) {
        purp_ipc_request_t requests[PURP_IPC_MAX_CLIENTS];
        size_t count = purp_ipc_server_poll(&server->ipc, purp_server_is_locked(server), requests,
                                            PURP_ARRAY_LEN(requests));
        for (size_t i = 0; i < count; ++i) {
            purp_server_handle_ipc_request(server, &requests[i]);
            if (requests[i].command.kind != PURP_IPC_GET_MONITORS) changed = true;
        }
    }
    if (server->capture.listen_fd >= 0 &&
        purp_capture_server_poll(&server->capture, purp_server_is_locked(server),
                                 purp_server_capture_png, server))
        changed = true;

    bool status_changed = purp_bar_tick_status(&server->bar);
    bool bar_dirty = purp_bar_is_dirty(&server->bar);
    if (status_changed || bar_dirty) {
        server_damage_bars(server);
        changed = true;
    }
    if (purp_server_poll_lock(server)) changed = true;
    if (purp_notify_tick(&server->notify, now)) {
        purp_host_damage_all(server);
        changed = true;
    }
    if (purp_input_tick_key_resize(server)) changed = true;

    if (server->pending_vt_switch) {
        int vt = server->pending_vt_switch;
        server->pending_vt_switch = 0;
        if (server->backend_ops && server->backend_ops->switch_vt &&
            !server->backend_ops->switch_vt(server->backend, vt))
            PURP_WARN("Switching to VT %d failed", vt);
    }
    if (server->wm.monitors_dirty) {
        const purp_backend_ops_t *ops = server->backend_ops;
        if (!ops || !ops->apply_monitors || ops->apply_monitors(server->backend, server)) {
            server->wm.monitors_dirty = false;
            purp_host_damage_all(server);
            changed = true;
        }
    }
    if (server->screenshot_request.kind != PURP_SCREENSHOT_NONE) {
        purp_server_process_screenshot(server);
        changed = true;
    }
    purp_server_maintenance_tick(server);
    return changed;
}

/* =========================================================================================== */
/* Monitors (host side)                                                                       */
/* =========================================================================================== */

purp_wm_t *purp_host_wm(purp_server_t *server) {
    return server ? &server->wm : NULL;
}

size_t purp_host_monitor_count(const purp_server_t *server) {
    return server ? server->wm.monitor_count : 0;
}

purp_monitor_t *purp_host_monitor(purp_server_t *server, size_t idx) {
    if (!server || idx >= server->wm.monitor_count) return NULL;
    return &server->wm.monitors[idx];
}

int purp_host_monitor_add(purp_server_t *server, const purp_monitor_desc_t *desc) {
    if (!server || !desc) return -1;
    int idx = purp_wm_add_monitor(&server->wm, desc);
    if (idx < 0) {
        PURP_WARN("Cannot track monitor %s: at most %d monitors are supported", desc->name,
                  PURP_MAX_MONITORS);
        return -1;
    }
    purp_server_monitors_changed(server, NULL);
    return idx;
}

void purp_host_monitors_retain(purp_server_t *server, const char *const *names, size_t count) {
    if (!server) return;
    int old_to_new[PURP_MAX_MONITORS];
    for (size_t i = 0; i < PURP_MAX_MONITORS; ++i) old_to_new[i] = -1;
    size_t removed = purp_wm_retain_connected_monitors(&server->wm, names, count, old_to_new);
    if (removed == 0) return;
    purp_server_monitors_changed(server, old_to_new);
}

void purp_host_monitor_set_physical(purp_server_t *server, size_t idx, int32_t width,
                                    int32_t height, int32_t refresh_mhz) {
    if (!server || idx >= server->wm.monitor_count) return;
    purp_size_t physical = { width, height };
    purp_wm_set_monitor_physical(&server->wm, idx, physical, refresh_mhz);
    purp_server_monitors_changed(server, NULL);
}

bool purp_host_monitor_needs_frame(const purp_server_t *server, size_t idx) {
    return server && idx < server->wm.monitor_count && idx < PURP_MAX_MONITORS &&
           server->monitor_damaged[idx];
}

void purp_host_damage_monitor(purp_server_t *server, size_t idx) {
    if (!server || idx >= PURP_MAX_MONITORS || idx >= server->wm.monitor_count) return;
    server->monitor_damaged[idx] = true;
    if (server->backend_ops && server->backend_ops->schedule_frame)
        server->backend_ops->schedule_frame(server->backend, idx);
}

void purp_host_damage_all(purp_server_t *server) {
    if (!server) return;
    for (size_t i = 0; i < server->wm.monitor_count; ++i) purp_host_damage_monitor(server, i);
}

void purp_host_monitor_frame_presented(purp_server_t *server, size_t idx) {
    /* The damage flag is cleared when the renderer samples the monitor (purp_render_monitor_frame):
     * damage that arrived while this frame was in flight must schedule the next one. */
    if (!server || idx >= server->wm.monitor_count) return;
    purp_server_send_frame_callbacks(server, idx, purp_server_time_ms(server));
}

/* =========================================================================================== */
/* Relayout, damage, surface outputs, frame callbacks                                         */
/* =========================================================================================== */

void purp_server_relayout(purp_server_t *server) {
    if (!server) return;
    purp_wm_relayout(&server->wm);
    purp_layer_arrange_all(server);
    purp_xdg_flush_configures(server);
    purp_server_update_surface_outputs(server);
    purp_input_sync_keyboard_focus(server);
    purp_input_sync_pointer_focus(server, 0);
    purp_bar_mark_dirty(&server->bar);
    purp_host_damage_all(server);
}

uint32_t purp_server_monitor_mask_for_rect(const purp_server_t *server, purp_rect_t rect) {
    if (!server || purp_rect_is_empty(rect)) return 0;
    uint32_t mask = 0;
    for (size_t i = 0; i < server->wm.monitor_count && i < 32; ++i) {
        const purp_monitor_t *mon = &server->wm.monitors[i];
        if (mon->enabled && purp_rect_overlaps(purp_monitor_rect(mon), rect)) mask |= 1u << i;
    }
    return mask;
}

void purp_server_damage_rect(purp_server_t *server, purp_rect_t rect) {
    uint32_t mask = purp_server_monitor_mask_for_rect(server, rect);
    for (size_t i = 0; mask != 0 && i < 32; ++i)
        if (mask & (1u << i)) purp_host_damage_monitor(server, i);
}

/* Enter exactly the owning monitor (none when it is gone or disabled) at its scale. */
static void server_sync_tree_outputs(purp_server_t *server, purp_surface_t *surface, int monitor) {
    if (!surface) return;
    const purp_wm_t *wm = &server->wm;
    uint32_t mask = 0;
    double scale = wm->default_scale;
    if (monitor >= 0 && (size_t)monitor < wm->monitor_count && monitor < 32 &&
        wm->monitors[monitor].enabled) {
        mask = 1u << monitor;
        scale = wm->monitors[monitor].scale;
    } else if (wm->monitor_count > 0) {
        scale = purp_wm_monitor_scale(wm, purp_wm_active_monitor_index(wm));
    }
    purp_surface_set_outputs(surface, mask);
    if (scale > 0) purp_surface_set_preferred_scale(surface, scale);
}

void purp_server_update_surface_outputs(purp_server_t *server) {
    if (!server) return;
    for (size_t i = 0; i < server->wm.window_count; ++i) {
        purp_window_t *win = server->wm.windows[i];
        if (win) server_sync_tree_outputs(server, purp_xdg_window_surface(win), win->monitor);
    }
    purp_layer_surface_t *layer;
    wl_list_for_each(layer, &server->layers, link)
        server_sync_tree_outputs(server, layer->surface, layer->monitor);
    purp_xdg_surface_t *xdg;
    wl_list_for_each(xdg, &server->xdg_surfaces, link) {
        if (xdg->role != PURP_XDG_ROLE_POPUP || !xdg->popup || !xdg->surface) continue;
        server_sync_tree_outputs(server, xdg->surface, purp_xdg_popup_monitor(xdg->popup));
    }
}

void purp_server_send_frame_callbacks(purp_server_t *server, size_t monitor, uint32_t time_ms) {
    if (!server || purp_server_is_locked(server)) return;
    purp_wm_t *wm = &server->wm;
    if (monitor >= wm->monitor_count || !wm->monitors[monitor].enabled) return;

    for (size_t i = 0; i < wm->window_count; ++i) {
        purp_window_t *win = wm->windows[i];
        if (!win || win->monitor != (int)monitor || !purp_wm_is_window_visible(wm, win)) continue;
        purp_surface_t *surface = purp_xdg_window_surface(win);
        if (surface) purp_surface_send_frame_done(surface, time_ms);
    }

    purp_popup_t *popups[64];
    size_t popup_count = purp_xdg_visible_popups(server, (int)monitor, popups,
                                                 PURP_ARRAY_LEN(popups));
    for (size_t i = 0; i < popup_count; ++i)
        if (popups[i] && popups[i]->xdg && popups[i]->xdg->surface)
            purp_surface_send_frame_done(popups[i]->xdg->surface, time_ms);

    purp_layer_surface_t *layer;
    wl_list_for_each(layer, &server->layers, link) {
        if (layer->monitor != (int)monitor || !layer->surface || !purp_layer_is_mapped(layer) ||
            purp_layer_is_keepalive(layer))
            continue;
        purp_surface_send_frame_done(layer->surface, time_ms);
    }

    int pointer_mon = purp_wm_monitor_at(wm, wm->pointer_location);
    if (pointer_mon == (int)monitor) {
        if (server->seat.cursor_mode == PURP_CURSOR_MODE_SURFACE && server->seat.cursor_surface)
            purp_surface_send_frame_done(server->seat.cursor_surface, time_ms);
        if (server->seat.drag.active && server->seat.drag.icon)
            purp_surface_send_frame_done(server->seat.drag.icon, time_ms);
    }
}

uint32_t purp_server_time_ms(const purp_server_t *server) {
    if (!server) return 0;
    return (uint32_t)(purp_now_ms() - server->start_ms);
}

/* =========================================================================================== */
/* Monitors (model changes, IPC)                                                              */
/* =========================================================================================== */

/* wl_surface.leave for every surface that entered monitor slot `bit` through `output`. */
static void server_surfaces_leave_output(purp_server_t *server, purp_output_t *output,
                                         size_t bit) {
    if (bit >= 32) return;
    uint32_t flag = 1u << bit;
    purp_surface_t *surface;
    wl_list_for_each(surface, &server->surfaces, link) {
        if (!(surface->output_mask & flag)) continue;
        surface->output_mask &= ~flag;
        if (!output || !surface->resource) continue;
        struct wl_resource *res =
            purp_output_resource_for_client(output, wl_resource_get_client(surface->resource));
        if (res) wl_surface_send_leave(surface->resource, res);
    }
}

static uint32_t remap_monitor_mask(uint32_t mask, const int *old_to_new) {
    uint32_t out = 0;
    for (size_t i = 0; i < PURP_MAX_MONITORS && i < 32; ++i) {
        if (!(mask & (1u << i))) continue;
        int target = old_to_new[i];
        if (target >= 0 && target < 32) out |= 1u << target;
    }
    return out;
}

static void server_remap_removed_monitors(purp_server_t *server, const int *old_to_new) {
    /* server->outputs still knows the old index of every output. */
    purp_output_t *output, *tmp;
    wl_list_for_each_safe(output, tmp, &server->outputs, link) {
        size_t old = output->monitor;
        int target = old < PURP_MAX_MONITORS ? old_to_new[old] : -1;
        if (target >= 0) {
            output->monitor = (size_t)target;
            continue;
        }
        server_surfaces_leave_output(server, output, old);
        /* The array was compacted: park the stale index on an unused slot before detaching. */
        output->monitor = server->wm.monitor_count;
        purp_output_destroy(output);
    }

    purp_surface_t *surface;
    wl_list_for_each(surface, &server->surfaces, link)
        surface->output_mask = remap_monitor_mask(surface->output_mask, old_to_new);

    if (server->grab.kind == PURP_GRAB_TILED_RESIZE) {
        int mon = server->grab.plan.monitor;
        int target = mon >= 0 && mon < PURP_MAX_MONITORS ? old_to_new[mon] : -1;
        if (target < 0) purp_input_cancel_grabs(server);
        else server->grab.plan.monitor = target;
    }
}

void purp_server_monitors_changed(purp_server_t *server, const int *old_to_new) {
    if (!server) return;
    purp_wm_t *wm = &server->wm;
    if (old_to_new) server_remap_removed_monitors(server, old_to_new);

    purp_wm_normalize_monitors(wm);

    /* One wl_output per enabled monitor. */
    for (size_t i = 0; i < wm->monitor_count; ++i) {
        purp_monitor_t *mon = &wm->monitors[i];
        if (mon->enabled) {
            if (!mon->output) {
                mon->output = purp_output_create(server, i);
                if (!mon->output) PURP_WARN("Failed to publish wl_output for %s", mon->name);
            } else {
                mon->output->monitor = i;
            }
        } else if (mon->output) {
            server_surfaces_leave_output(server, mon->output, i);
            purp_output_destroy(mon->output);
            mon->output = NULL;
        }
    }
    for (size_t i = 0; i < wm->monitor_count; ++i)
        if (wm->monitors[i].output) purp_output_send_state(wm->monitors[i].output);

    purp_bar_prune_monitors(&server->bar, wm->monitor_count);
    for (size_t i = 0; i < wm->monitor_count; ++i)
        purp_bar_set_monitor_scale(&server->bar, i, wm->monitors[i].scale);

    purp_layer_monitors_changed(server, old_to_new);
    purp_server_relayout(server);
}

bool purp_server_apply_monitor_configs(purp_server_t *server, const purp_monitor_config_t *entries,
                                       size_t count, bool persist, char *err, size_t err_len) {
    if (!server) return false;
    bool allow_custom = server->backend_ops ? server->backend_ops->allow_custom_modes : true;
    if (!purp_wm_apply_monitor_configs(&server->wm, entries, count, allow_custom, err, err_len))
        return false;
    purp_server_monitors_changed(server, NULL);
    if (persist) {
        purp_monitor_config_t configs[PURP_MAX_MONITORS];
        size_t n = purp_wm_monitor_configs(&server->wm, configs, PURP_ARRAY_LEN(configs));
        if (!purp_save_monitors_config(configs, n))
            PURP_WARN("Failed to save monitors.json: %s", strerror(errno));
    }
    return true;
}

static void server_serialize_monitors(purp_server_t *server, purp_strbuf_t *out) {
    purp_monitor_info_t infos[PURP_MAX_MONITORS];
    size_t count = purp_wm_monitor_infos(&server->wm, infos, PURP_ARRAY_LEN(infos));
    purp_ipc_serialize_monitors_response(infos, count, out);
}

void purp_server_handle_ipc_request(purp_server_t *server, const purp_ipc_request_t *request) {
    if (!server || !request) return;
    const purp_ipc_command_t *cmd = &request->command;
    purp_strbuf_t reply = { 0 };
    switch (cmd->kind) {
    case PURP_IPC_GET_MONITORS:
        server_serialize_monitors(server, &reply);
        break;
    case PURP_IPC_APPLY_MONITORS: {
        char err[PURP_IPC_ERROR_MAX] = "";
        if (purp_server_apply_monitor_configs(server, cmd->monitors, cmd->monitor_count, true, err,
                                              sizeof(err)))
            server_serialize_monitors(server, &reply);
        else
            purp_ipc_serialize_error_response(err[0] ? err : "failed to apply monitors", &reply);
        break;
    }
    case PURP_IPC_FOCUS_MONITOR:
        purp_wm_cycle_monitor(&server->wm, cmd->delta);
        purp_server_relayout(server);
        purp_ipc_serialize_ok_response(&reply);
        break;
    case PURP_IPC_MOVE_WINDOW_TO_MONITOR:
        purp_wm_move_focused_window_to_monitor(&server->wm, cmd->delta);
        purp_server_relayout(server);
        purp_ipc_serialize_ok_response(&reply);
        break;
    default:
        purp_ipc_serialize_error_response("unknown IPC command", &reply);
        break;
    }
    if (reply.oom || !reply.data)
        purp_ipc_server_respond(&server->ipc, request, "{\"ok\":false,\"error\":\"out of memory\"}");
    else
        purp_ipc_server_respond(&server->ipc, request, reply.data);
    purp_strbuf_free(&reply);
}

/* =========================================================================================== */
/* Scale                                                                                      */
/* =========================================================================================== */

static bool config_base_dir(char *out, size_t out_len) {
    const char *xdg = getenv("XDG_CONFIG_HOME");
    int n;
    if (xdg && *xdg) {
        n = snprintf(out, out_len, "%s", xdg);
    } else {
        const char *home = getenv("HOME");
        if (!home || !*home) return false;
        n = snprintf(out, out_len, "%s/.config", home);
    }
    return n > 0 && (size_t)n < out_len;
}

static bool config_file_path(const char *dir_name, char *out, size_t out_len) {
    char base[PATH_MAX];
    if (!out || out_len == 0 || !config_base_dir(base, sizeof(base))) return false;
    int n = snprintf(out, out_len, "%s/%s/%s", base, dir_name, PURP_SCALE_FILE_NAME);
    return n > 0 && (size_t)n < out_len;
}

bool purp_scale_file_path(char *out, size_t out_len) {
    return config_file_path(PURP_CONFIG_DIR_NAME, out, out_len);
}

static bool parse_scale_text(const char *text, double *out) {
    if (!text) return false;
    while (*text == ' ' || *text == '\t' || *text == '\n' || *text == '\r') ++text;
    if (!*text) return false;
    char *end = NULL;
    errno = 0;
    double value = strtod(text, &end);
    if (end == text || errno == ERANGE) return false;
    while (*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r') ++end;
    if (*end != '\0' || !isfinite(value)) return false;
    *out = value;
    return true;
}

static bool read_scale_file(const char *path, double *out) {
    FILE *file = fopen(path, "re");
    if (!file) return false;
    char text[64];
    size_t len = fread(text, 1, sizeof(text) - 1, file);
    fclose(file);
    text[len] = '\0';
    return parse_scale_text(text, out);
}

double purp_initial_scale(void) {
    double scale = PURP_DEFAULT_SCALE;
    char path[PATH_MAX];
    if (parse_scale_text(getenv(PURP_ENV_SCALE), &scale)) goto done;
    if (purp_scale_file_path(path, sizeof(path)) && read_scale_file(path, &scale)) goto done;
    if (config_file_path(PURP_LEGACY_CONFIG_DIR_NAME, path, sizeof(path)) &&
        read_scale_file(path, &scale))
        goto done;
    if (parse_scale_text(getenv(PURP_ENV_DEFAULT_SCALE), &scale)) goto done;
    scale = PURP_DEFAULT_SCALE;
done:
    return purp_clamp_f64(scale, PURP_SCALE_MIN, PURP_SCALE_MAX);
}

static void write_scale_file(double scale) {
    char path[PATH_MAX], dir[PATH_MAX];
    if (!purp_scale_file_path(path, sizeof(path))) return;
    if (parent_dir(path, dir, sizeof(dir)) && !mkdir_p(dir, 0755)) {
        PURP_WARN("Failed to create %s: %s", dir, strerror(errno));
        return;
    }
    FILE *file = fopen(path, "we");
    if (!file) {
        PURP_WARN("Failed to save scale to %s: %s", path, strerror(errno));
        return;
    }
    fprintf(file, "%g\n", scale);
    if (fclose(file) != 0) PURP_WARN("Failed to save scale to %s: %s", path, strerror(errno));
}

void purp_server_adjust_scale(purp_server_t *server, double delta) {
    if (!server || server->wm.monitor_count == 0) return;
    purp_wm_t *wm = &server->wm;
    size_t active = purp_wm_active_monitor_index(wm);
    double before = purp_wm_monitor_scale(wm, active);
    double next = purp_wm_adjust_scale(wm, delta);
    if (fabs(next - before) < 1e-9) return;
    purp_server_monitors_changed(server, NULL);
    /* The single-value scale file describes the primary monitor only. */
    if (active == purp_wm_primary_monitor_index(wm)) write_scale_file(next);
    if (wm->monitor_count > 1) {
        purp_monitor_config_t configs[PURP_MAX_MONITORS];
        size_t n = purp_wm_monitor_configs(wm, configs, PURP_ARRAY_LEN(configs));
        if (!purp_save_monitors_config(configs, n))
            PURP_WARN("Failed to save monitors.json: %s", strerror(errno));
    }
}

/* =========================================================================================== */
/* Spawning                                                                                   */
/* =========================================================================================== */

static bool is_executable_file(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode) && access(path, X_OK) == 0;
}

bool purp_command_exists(const char *program) {
    if (!program || !*program) return false;
    if (strchr(program, '/')) return is_executable_file(program);
    const char *path = getenv("PATH");
    if (!path || !*path) path = "/usr/local/bin:/usr/bin:/bin";
    size_t prog_len = strlen(program);
    for (const char *p = path;;) {
        const char *end = strchr(p, ':');
        size_t dir_len = end ? (size_t)(end - p) : strlen(p);
        char full[PATH_MAX];
        if (dir_len == 0) {
            if (prog_len < sizeof(full)) {
                memcpy(full, program, prog_len + 1);
                if (is_executable_file(full)) return true;
            }
        } else if (dir_len + 1 + prog_len < sizeof(full)) {
            memcpy(full, p, dir_len);
            full[dir_len] = '/';
            memcpy(full + dir_len + 1, program, prog_len + 1);
            if (is_executable_file(full)) return true;
        }
        if (!end) break;
        p = end + 1;
    }
    return false;
}

static void free_string_array(char **items) {
    if (!items) return;
    for (char **p = items; *p; ++p) free(*p);
    free(items);
}

/* The child environment: everything inherited except the overridden keys, then the session
 * variables (see purp_server_spawn). */
static char **server_build_child_env(const purp_server_t *server) {
    char capture[PATH_MAX], ipc[PATH_MAX], bus_path[PATH_MAX], bus_addr[PATH_MAX + 16];
    purp_server_socket_path(server, PURP_CAPTURE_SOCKET_PREFIX, capture, sizeof(capture));
    purp_server_socket_path(server, PURP_IPC_SOCKET_PREFIX, ipc, sizeof(ipc));
    const char *const keys[] = {
        "WAYLAND_DISPLAY", PURP_ENV_CAPTURE_SOCKET, PURP_ENV_IPC_SOCKET, "XDG_RUNTIME_DIR",
        "XDG_SESSION_TYPE", "XDG_CURRENT_DESKTOP", "WGPU_BACKEND", "QT_QPA_PLATFORM",
        "GDK_BACKEND", "MOZ_ENABLE_WAYLAND", "ELECTRON_OZONE_PLATFORM_HINT", "WAYLAND_SOCKET",
    };
    const char *const values[] = {
        server->socket_name, capture, ipc, server->runtime_dir,
        "wayland", PURP_XDG_CURRENT_DESKTOP, "gl", "wayland",
        "wayland", "1", "auto", NULL,
    };
    size_t key_count = PURP_ARRAY_LEN(keys);

    bool add_bus = false;
    if (!getenv("DBUS_SESSION_BUS_ADDRESS")) {
        int n = snprintf(bus_path, sizeof(bus_path), "%s/bus", server->runtime_dir);
        struct stat st;
        if (n > 0 && (size_t)n < sizeof(bus_path) && stat(bus_path, &st) == 0) {
            n = snprintf(bus_addr, sizeof(bus_addr), "unix:path=%s", bus_path);
            add_bus = n > 0 && (size_t)n < sizeof(bus_addr);
        }
    }

    size_t inherited = 0;
    for (char **e = environ; e && *e; ++e) ++inherited;
    char **envp = calloc(inherited + key_count + 2, sizeof(*envp));
    if (!envp) return NULL;
    size_t n = 0;
    for (char **e = environ; e && *e; ++e) {
        bool overridden = false;
        for (size_t k = 0; k < key_count && !overridden; ++k) {
            size_t len = strlen(keys[k]);
            overridden = strncmp(*e, keys[k], len) == 0 && (*e)[len] == '=';
        }
        if (overridden) continue;
        if (!(envp[n] = strdup(*e))) goto oom;
        ++n;
    }
    for (size_t k = 0; k < key_count; ++k) {
        if (!values[k]) continue;
        size_t len = strlen(keys[k]) + strlen(values[k]) + 2;
        if (!(envp[n] = malloc(len))) goto oom;
        snprintf(envp[n], len, "%s=%s", keys[k], values[k]);
        ++n;
    }
    if (add_bus) {
        size_t len = strlen("DBUS_SESSION_BUS_ADDRESS=") + strlen(bus_addr) + 1;
        if (!(envp[n] = malloc(len))) goto oom;
        snprintf(envp[n], len, "DBUS_SESSION_BUS_ADDRESS=%s", bus_addr);
        ++n;
    }
    envp[n] = NULL;
    return envp;
oom:
    free_string_array(envp);
    return NULL;
}

/* `["prog", "arg", ...]` for logs. */
static void format_argv(const char *const *argv, char *out, size_t out_len) {
    size_t used = 0;
    out[0] = '\0';
    for (size_t i = 0; argv[i]; ++i) {
        int n = snprintf(out + used, out_len - used, "%s\"%s\"", i == 0 ? "[" : ", ", argv[i]);
        if (n < 0 || (size_t)n >= out_len - used) {
            used = out_len - 1;
            break;
        }
        used += (size_t)n;
    }
    if (used + 2 <= out_len) {
        out[used] = argv[0] ? ']' : '\0';
        out[used + 1] = '\0';
    }
}

static void server_track_child(purp_server_t *server, pid_t pid, const char *label,
                               bool session_helper) {
    pthread_mutex_lock(&server->children_lock);
    if (server->child_count == server->child_cap) {
        size_t cap = server->child_cap ? server->child_cap * 2 : 8;
        purp_child_t *children = realloc(server->children, cap * sizeof(*children));
        if (!children) {
            pthread_mutex_unlock(&server->children_lock);
            PURP_WARN("Out of memory tracking child %d", (int)pid);
            return;
        }
        server->children = children;
        server->child_cap = cap;
    }
    purp_child_t *child = &server->children[server->child_count++];
    memset(child, 0, sizeof(*child));
    child->pid = pid;
    child->session_helper = session_helper;
    snprintf(child->label, sizeof(child->label), "%s", label ? label : "");
    pthread_mutex_unlock(&server->children_lock);
}

pid_t purp_server_spawn(purp_server_t *server, const char *const *argv, const char *label,
                        bool session_helper) {
    if (!server || !argv || !argv[0]) return -1;
    if (!label) label = argv[0];
    char cmdline[1024];
    format_argv(argv, cmdline, sizeof(cmdline));

    char **envp = server_build_child_env(server);
    if (!envp) {
        PURP_WARN("Failed to spawn %s: out of memory", cmdline);
        return -1;
    }
    int errpipe[2];
    if (pipe2(errpipe, O_CLOEXEC) != 0) {
        PURP_WARN("Failed to spawn %s: %s", cmdline, strerror(errno));
        free_string_array(envp);
        return -1;
    }
    struct sigaction dfl;
    memset(&dfl, 0, sizeof(dfl));
    dfl.sa_handler = SIG_DFL;
    sigemptyset(&dfl.sa_mask);
    sigset_t empty;
    sigemptyset(&empty);

    pid_t pid = fork();
    if (pid == 0) {
        /* Child: async-signal-safe calls only. */
        close(errpipe[0]);
        setpgid(0, 0);
        child_reset_signals(&dfl, &empty);
        int devnull = open("/dev/null", O_RDONLY | O_CLOEXEC);
        child_dup_to(devnull, STDIN_FILENO);
        if (devnull > STDIN_FILENO) close(devnull);
        environ = envp;
        execvp(argv[0], (char *const *)argv);
        int err = errno;
        ssize_t written = write(errpipe[1], &err, sizeof(err));
        (void)written;
        _exit(127);
    }
    int fork_errno = errno;
    close(errpipe[1]);
    free_string_array(envp);
    if (pid < 0) {
        close(errpipe[0]);
        PURP_WARN("Failed to spawn %s: %s", cmdline, strerror(fork_errno));
        return -1;
    }
    setpgid(pid, pid); /* also in the child; whichever runs first wins, errors are harmless */

    int child_errno = 0;
    ssize_t n;
    do {
        n = read(errpipe[0], &child_errno, sizeof(child_errno));
    } while (n < 0 && errno == EINTR);
    close(errpipe[0]);
    if (n == (ssize_t)sizeof(child_errno)) {
        while (waitpid(pid, NULL, 0) < 0 && errno == EINTR) {
        }
        PURP_WARN("Failed to spawn %s: %s", cmdline, strerror(child_errno));
        return -1;
    }

    PURP_INFO("Spawned %s on WAYLAND_DISPLAY=%s: %s", label, server->socket_name, cmdline);
    /* Reap here too so zombies never pile up before the event loop runs maintenance. */
    purp_server_reap_children(server);
    server_track_child(server, pid, label, session_helper);
    return pid;
}

pid_t purp_server_spawn_first(purp_server_t *server, purp_command_list_t candidates,
                              const char *label, bool session_helper) {
    for (size_t i = 0; i < candidates.count; ++i) {
        const purp_command_t *cmd = &candidates.items[i];
        if (!cmd->argv[0] || !purp_command_exists(cmd->argv[0])) continue;
        pid_t pid = purp_server_spawn(server, cmd->argv, label, session_helper);
        if (pid > 0) return pid;
    }
    PURP_WARN("No available %s binary found in config candidates", label ? label : "command");
    return -1;
}

bool purp_server_spawn_configured(purp_server_t *server, const char *env_var,
                                  const char *const *extra_args, purp_command_list_t candidates,
                                  const char *label) {
    const char *program = env_var ? getenv(env_var) : NULL;
    if (program) {
        if (strcmp(program, "none") == 0) return false;
        if (*program && strcmp(program, "auto") != 0) {
            const char *argv[PURP_COMMAND_MAX_ARGV + 2];
            size_t n = 0;
            argv[n++] = program;
            for (size_t i = 0; extra_args && extra_args[i] && n + 1 < PURP_ARRAY_LEN(argv); ++i)
                argv[n++] = extra_args[i];
            argv[n] = NULL;
            return purp_server_spawn(server, argv, label, false) > 0;
        }
    }
    return purp_server_spawn_first(server, candidates, label, false) > 0;
}

bool purp_run_capture(const char *const *argv, int timeout_ms, char *out, size_t out_len) {
    if (out && out_len) out[0] = '\0';
    if (!argv || !argv[0]) return false;
    int pipefd[2];
    if (pipe2(pipefd, O_CLOEXEC) != 0) return false;
    int devnull = open("/dev/null", O_RDWR | O_CLOEXEC);
    if (devnull < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return false;
    }
    struct sigaction dfl;
    memset(&dfl, 0, sizeof(dfl));
    dfl.sa_handler = SIG_DFL;
    sigemptyset(&dfl.sa_mask);
    sigset_t empty;
    sigemptyset(&empty);

    pid_t pid = fork();
    if (pid == 0) {
        child_reset_signals(&dfl, &empty);
        child_dup_to(pipefd[1], STDOUT_FILENO);
        child_dup_to(devnull, STDIN_FILENO);
        child_dup_to(devnull, STDERR_FILENO);
        execvp(argv[0], (char *const *)argv);
        _exit(127);
    }
    close(pipefd[1]);
    close(devnull);
    if (pid < 0) {
        close(pipefd[0]);
        return false;
    }

    bool has_deadline = timeout_ms > 0;
    uint64_t deadline = purp_now_ms() + (has_deadline ? (uint64_t)timeout_ms : 0);
    bool timed_out = false;
    size_t len = 0;
    char sink[512];
    for (;;) {
        int wait_ms = -1;
        if (has_deadline) {
            uint64_t now = purp_now_ms();
            if (now >= deadline) {
                timed_out = true;
                break;
            }
            wait_ms = (int)(deadline - now);
        }
        struct pollfd pfd = { .fd = pipefd[0], .events = POLLIN };
        int r = poll(&pfd, 1, wait_ms);
        if (r < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (r == 0) {
            timed_out = true;
            break;
        }
        char *dst = sink;
        size_t cap = sizeof(sink);
        if (out && len + 1 < out_len) {
            dst = out + len;
            cap = out_len - 1 - len;
        }
        ssize_t got = read(pipefd[0], dst, cap);
        if (got < 0) {
            if (errno == EINTR || errno == EAGAIN) continue;
            break;
        }
        if (got == 0) break;
        if (dst != sink) len += (size_t)got;
    }
    if (out && out_len) out[len] = '\0';
    close(pipefd[0]);

    int status = 0;
    pid_t reaped = -1;
    if (timed_out) kill(pid, SIGKILL);
    for (;;) {
        reaped = waitpid(pid, &status, (has_deadline && !timed_out) ? WNOHANG : 0);
        if (reaped == pid) break;
        if (reaped < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (purp_now_ms() >= deadline) {
            kill(pid, SIGKILL);
            timed_out = true;
            continue;
        }
        sleep_ms(5);
    }
    return !timed_out && reaped == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

void purp_server_reap_children(purp_server_t *server) {
    if (!server) return;
    pthread_mutex_lock(&server->children_lock);
    size_t kept = 0;
    for (size_t i = 0; i < server->child_count; ++i) {
        purp_child_t child = server->children[i];
        int status = 0;
        pid_t r;
        do {
            r = waitpid(child.pid, &status, WNOHANG);
        } while (r < 0 && errno == EINTR);
        if (r == child.pid || (r < 0 && errno == ECHILD)) {
            PURP_DEBUG("%s (pid %d) exited", child.label, (int)child.pid);
            continue;
        }
        server->children[kept++] = child;
    }
    server->child_count = kept;
    pthread_mutex_unlock(&server->children_lock);
}

void purp_server_shutdown_session(purp_server_t *server) {
    if (!server) return;
    pthread_mutex_lock(&server->children_lock);
    for (size_t i = 0; i < server->child_count; ++i) {
        purp_child_t *child = &server->children[i];
        if (!child->session_helper || child->pid <= 1) continue;
        kill(-child->pid, SIGTERM);
        child->session_helper = false;
    }
    pthread_mutex_unlock(&server->children_lock);
    purp_server_reap_children(server);
}

/* ---- session services ----------------------------------------------------------------------- */

bool purp_server_spawn_terminal(purp_server_t *server) {
    return purp_server_spawn_configured(server, PURP_ENV_TERMINAL, NULL,
                                        PURP_COMMANDS(PURP_TERMINAL_COMMANDS), "terminal");
}

void purp_server_spawn_spotlight_daemon(purp_server_t *server) {
    if (!server) return;
    const char *program = getenv(PURP_ENV_SPOTLIGHT);
    if (!program || !*program || strcmp(program, "auto") == 0) program = "spotlight";
    if (strcmp(program, "none") == 0 || !purp_command_exists(program)) return;
    if (purp_command_exists("pkill")) {
        static const char *const pkill_argv[] = { "pkill", "-x", "spotlight", NULL };
        purp_run_capture(pkill_argv, 2000, NULL, 0);
    }
    char sock[PATH_MAX];
    int n = snprintf(sock, sizeof(sock), "%s/%s", server->runtime_dir, PURP_SPOTLIGHT_SOCKET_NAME);
    if (n > 0 && (size_t)n < sizeof(sock)) unlink(sock);
    const char *const argv[] = { program, "--daemon", NULL };
    purp_server_spawn(server, argv, "Spotlight daemon", false);
}

void purp_server_spawn_keyring(purp_server_t *server) {
    purp_server_spawn_first(server, PURP_COMMANDS(PURP_KEYRING_COMMANDS), "OS keyring", false);
}

void purp_server_spawn_screenshot_portal(purp_server_t *server) {
    purp_server_spawn_first(server, PURP_COMMANDS(PURP_SCREENSHOT_PORTAL_COMMANDS),
                            "screenshot portal", true);
}

void purp_server_spawn_polkit_agent(purp_server_t *server) {
    purp_server_spawn_first(server, PURP_COMMANDS(PURP_POLKIT_COMMANDS), "polkit agent", true);
}

void purp_server_start_notifications(purp_server_t *server) {
    if (!server) return;
    if (!purp_notify_start_dbus(&server->notify, server->runtime_dir))
        PURP_WARN("Failed to start the notification daemon (%s)", PURP_NOTIFY_DBUS_NAME);
}

static void *network_bluetooth_thread(void *unused) {
    (void)unused;
    static const char *const networking_on[] = { "nmcli", "networking", "on", NULL };
    static const char *const wifi_on[] = { "nmcli", "radio", "wifi", "on", NULL };
    static const char *const device_status[] = { "nmcli", "-t", "-f", "TYPE,STATE", "device",
                                                 "status", NULL };
    static const char *const wifi_connect[] = { "nmcli", "device", "connect",
                                                PURP_WIFI_AUTOCONNECT_DEVICE, NULL };
    static const char *const rfkill_unblock[] = { "rfkill", "unblock", "bluetooth", NULL };
    static const char *const bt_power_on[] = { "bluetoothctl", "power", "on", NULL };
    char status[4096];
    purp_run_capture(networking_on, 10000, NULL, 0);
    purp_run_capture(wifi_on, 10000, NULL, 0);
    purp_run_capture(device_status, 10000, status, sizeof(status));
    if (purp_wifi_needs_autoconnect(status)) purp_run_capture(wifi_connect, 30000, NULL, 0);
    purp_run_capture(rfkill_unblock, 10000, NULL, 0);
    purp_run_capture(bt_power_on, 10000, NULL, 0);
    return NULL;
}

void purp_server_ensure_network_and_bluetooth(purp_server_t *server) {
    if (!server) return;
    spawn_detached_thread(network_bluetooth_thread, NULL);
    purp_bar_request_immediate_refresh(&server->bar);
}

void purp_server_run_autostart(purp_server_t *server) {
    for (size_t i = 0; i < PURP_ARRAY_LEN(PURP_AUTOSTART_COMMANDS); ++i) {
        const purp_command_t *cmd = &PURP_AUTOSTART_COMMANDS[i];
        if (cmd->argv[0] && purp_command_exists(cmd->argv[0]))
            purp_server_spawn(server, cmd->argv, "autostart", false);
    }
}

/* =========================================================================================== */
/* Screenshots, clipboard, capture                                                            */
/* =========================================================================================== */

void purp_server_request_screenshot(purp_server_t *server, bool focused_window_only) {
    if (!server || purp_server_is_locked(server)) return;
    purp_window_t *win = purp_wm_focused_window(&server->wm);
    if (focused_window_only && win && purp_wm_is_window_visible(&server->wm, win)) {
        server->screenshot_request.kind = PURP_SCREENSHOT_REGION;
        server->screenshot_request.region = win->geo;
    } else {
        server->screenshot_request.kind = PURP_SCREENSHOT_SCREEN;
        server->screenshot_request.region = purp_rect_make(0, 0, 0, 0);
    }
}

static bool server_make_current(purp_server_t *server) {
    if (!server->backend_ops || !server->backend_ops->make_current) return true;
    return server->backend_ops->make_current(server->backend);
}

void purp_server_process_screenshot(purp_server_t *server) {
    if (!server) return;
    purp_screenshot_request_t request = server->screenshot_request;
    server->screenshot_request.kind = PURP_SCREENSHOT_NONE;
    if (request.kind == PURP_SCREENSHOT_NONE || purp_server_is_locked(server)) return;
    if (!server->renderer || server->wm.monitor_count == 0) {
        PURP_WARN("Screenshot capture failed: renderer not ready");
        return;
    }
    if (!server_make_current(server)) {
        PURP_WARN("Screenshot capture failed: cannot make the GL context current");
        return;
    }
    size_t mon = purp_wm_active_monitor_index(&server->wm);
    const purp_rect_t *region = NULL;
    if (request.kind == PURP_SCREENSHOT_REGION) {
        mon = purp_wm_nearest_enabled_monitor(&server->wm, purp_rect_center(request.region));
        region = &request.region;
    }
    uint8_t *rgba = NULL;
    uint32_t width = 0, height = 0;
    bool ok = purp_render_capture_rgba(server, mon, region, false, &rgba, &width, &height);
    /* The offscreen render touched GL state: repaint everything from scratch. */
    purp_host_damage_all(server);
    if (!ok || !rgba) {
        free(rgba);
        PURP_WARN("Screenshot capture failed");
        return;
    }
    purp_server_save_screenshot(server, rgba, width, height);
}

typedef struct screenshot_job {
    purp_server_t *server;
    uint8_t *rgba;
    uint32_t width;
    uint32_t height;
} screenshot_job_t;

static void screenshot_timestamp(char *out, size_t out_len) {
    time_t now = time(NULL);
    struct tm tm;
    if (!localtime_r(&now, &tm) || strftime(out, out_len, "%Y%m%d-%H%M%S", &tm) == 0)
        snprintf(out, out_len, "00000000-000000");
}

/* Private 0600 temp file renamed into place (`-2`, `-3`, ... for same-second shots), so readers
 * never see a partial PNG. */
static bool write_screenshot_atomically(const char *dir, const purp_blob_t *png, char *saved,
                                        size_t saved_len) {
    char stamp[32];
    screenshot_timestamp(stamp, sizeof(stamp));
    char tmp[PATH_MAX];
    int fd = -1;
    for (unsigned attempt = 0; attempt <= 64 && fd < 0; ++attempt) {
        int n = snprintf(tmp, sizeof(tmp), "%s/.%s%s-%ld-%u.png.tmp", dir, PURP_SCREENSHOT_PREFIX,
                         stamp, (long)getpid(), attempt);
        if (n < 0 || (size_t)n >= sizeof(tmp)) {
            errno = ENAMETOOLONG;
            return false;
        }
        fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        if (fd < 0 && errno != EEXIST) return false;
    }
    if (fd < 0) return false;

    size_t off = 0;
    bool ok = true;
    while (off < png->len) {
        ssize_t w = write(fd, png->data + off, png->len - off);
        if (w < 0) {
            if (errno == EINTR) continue;
            ok = false;
            break;
        }
        off += (size_t)w;
    }
    if (ok && fsync(fd) != 0) ok = false;
    int saved_errno = errno;
    if (close(fd) != 0 && ok) {
        ok = false;
        saved_errno = errno;
    }
    if (!ok) {
        unlink(tmp);
        errno = saved_errno;
        return false;
    }

    char target[PATH_MAX];
    for (unsigned n = 1; n < 10000; ++n) {
        int len = n == 1 ? snprintf(target, sizeof(target), "%s/%s%s.png", dir,
                                    PURP_SCREENSHOT_PREFIX, stamp)
                         : snprintf(target, sizeof(target), "%s/%s%s-%u.png", dir,
                                    PURP_SCREENSHOT_PREFIX, stamp, n);
        if (len < 0 || (size_t)len >= sizeof(target)) break;
        struct stat st;
        if (lstat(target, &st) == 0) continue;
        if (rename(tmp, target) != 0) break;
        snprintf(saved, saved_len, "%s", target);
        return true;
    }
    saved_errno = errno;
    unlink(tmp);
    errno = saved_errno ? saved_errno : EEXIST;
    return false;
}

static void *screenshot_thread(void *data) {
    screenshot_job_t *job = data;
    purp_server_t *server = job->server;
    purp_blob_t *png = NULL;
    bool encoded = purp_png_encode_rgba(job->rgba, job->width, job->height, &png);
    free(job->rgba);
    job->rgba = NULL;
    if (!encoded || !png) {
        PURP_WARN("Failed to encode screenshot");
        purp_blob_unref(png);
        goto done;
    }

    char dir[PATH_MAX];
    const char *home = getenv("HOME");
    int n = snprintf(dir, sizeof(dir), "%s/%s", home && *home ? home : "/tmp",
                     PURP_SCREENSHOT_DIR);
    char saved[PATH_MAX];
    if (n < 0 || (size_t)n >= sizeof(dir))
        PURP_WARN("Failed to save screenshot: path too long");
    else if (!mkdir_p(dir, 0755))
        PURP_WARN("Failed to save screenshot in %s: %s", dir, strerror(errno));
    else if (write_screenshot_atomically(dir, png, saved, sizeof(saved)))
        PURP_INFO("Saved screenshot to %s", saved);
    else
        PURP_WARN("Failed to save screenshot in %s: %s", dir, strerror(errno));

    /* The clipboard gets the PNG even when saving failed. */
    pthread_mutex_lock(&server->screenshot_lock);
    if (server->screenshot_ready_count == PURP_MAX_SCREENSHOT_QUEUE) {
        purp_blob_unref(server->screenshot_ready[0]);
        memmove(server->screenshot_ready, server->screenshot_ready + 1,
                (PURP_MAX_SCREENSHOT_QUEUE - 1) * sizeof(server->screenshot_ready[0]));
        server->screenshot_ready_count--;
    }
    server->screenshot_ready[server->screenshot_ready_count++] = png;
    pthread_mutex_unlock(&server->screenshot_lock);

done:
    free(job);
    atomic_fetch_sub(&screenshot_jobs_inflight, 1);
    return NULL;
}

void purp_server_save_screenshot(purp_server_t *server, uint8_t *rgba, uint32_t width,
                                 uint32_t height) {
    if (!rgba) return;
    if (!server || width == 0 || height == 0) {
        free(rgba);
        return;
    }
    screenshot_job_t *job = calloc(1, sizeof(*job));
    if (!job) {
        free(rgba);
        PURP_WARN("Failed to save screenshot: out of memory");
        return;
    }
    job->server = server;
    job->rgba = rgba;
    job->width = width;
    job->height = height;
    atomic_fetch_add(&screenshot_jobs_inflight, 1);
    if (!spawn_detached_thread(screenshot_thread, job)) {
        atomic_fetch_sub(&screenshot_jobs_inflight, 1);
        free(job->rgba);
        free(job);
    }
}

void purp_server_offer_screenshots(purp_server_t *server) {
    if (!server) return;
    purp_blob_t *ready[PURP_MAX_SCREENSHOT_QUEUE];
    pthread_mutex_lock(&server->screenshot_lock);
    size_t count = server->screenshot_ready_count;
    memcpy(ready, server->screenshot_ready, count * sizeof(ready[0]));
    server->screenshot_ready_count = 0;
    pthread_mutex_unlock(&server->screenshot_lock);
    for (size_t i = 0; i < count; ++i) {
        purp_seat_set_compositor_selection(server, PURP_SCREENSHOT_MIME, ready[i]);
        purp_blob_unref(ready[i]);
    }
}

purp_blob_t *purp_server_capture_png(void *data) {
    purp_server_t *server = data;
    if (!server || purp_server_is_locked(server) || !server->renderer ||
        server->wm.monitor_count == 0)
        return NULL;
    if (!server_make_current(server)) return NULL;
    uint8_t *rgba = NULL;
    uint32_t width = 0, height = 0;
    size_t mon = purp_wm_active_monitor_index(&server->wm);
    bool ok = purp_render_capture_rgba(server, mon, NULL, true, &rgba, &width, &height);
    purp_host_damage_all(server);
    if (!ok || !rgba) {
        free(rgba);
        return NULL;
    }
    purp_blob_t *png = NULL;
    bool encoded = purp_png_encode_rgba(rgba, width, height, &png);
    free(rgba);
    if (!encoded) {
        purp_blob_unref(png);
        return NULL;
    }
    return png;
}

/* =========================================================================================== */
/* Lock                                                                                       */
/* =========================================================================================== */

void purp_server_lock_session(purp_server_t *server) {
    if (!server || server->lock_screen) return;
    purp_lock_screen_t *lock = calloc(1, sizeof(*lock));
    if (!lock) {
        PURP_ERROR("Cannot lock the session: out of memory");
        return;
    }
    server->help.visible = false;
    purp_input_cancel_grabs(server);
    server->mod_tap_candidate = false;
    server->swallowed_button_count = 0;
    server->swallowed_touch_count = 0;
    server->touch_grab_slot = -1;
    server->hovered_window_id = 0;

    /* Nothing may keep routing input behind the lock. */
    for (size_t i = 0; i < PURP_MAX_TOUCH_POINTS; ++i)
        if (server->seat.touch[i].active)
            purp_seat_touch_send_cancel(server, server->seat.touch[i].slot);
    if (purp_seat_drag_active(server)) purp_seat_drag_cancel(server);
    purp_xdg_dismiss_popup_grabs(server);
    purp_seat_set_keyboard_focus(server, NULL);
    purp_pointf_t origin = { 0, 0 };
    purp_seat_pointer_focus(server, NULL, origin, purp_server_time_ms(server));
    purp_seat_pointer_send_frame(server);

    purp_lock_init(lock);
    server->lock_screen = lock;
    purp_host_damage_all(server);
}

bool purp_server_is_locked(const purp_server_t *server) {
    return server && server->lock_screen != NULL;
}

bool purp_server_poll_lock(purp_server_t *server) {
    if (!server || !server->lock_screen) return false;
    switch (purp_lock_poll(server->lock_screen)) {
    case PURP_LOCK_POLL_UNLOCKED:
        purp_lock_finish(server->lock_screen);
        free(server->lock_screen);
        server->lock_screen = NULL;
        purp_input_sync_keyboard_focus(server);
        purp_input_sync_pointer_focus(server, 0);
        purp_host_damage_all(server);
        return true;
    case PURP_LOCK_POLL_FAILED:
        purp_host_damage_all(server);
        return true;
    case PURP_LOCK_POLL_PENDING:
    default:
        if (purp_lock_needs_clock_redraw(server->lock_screen)) {
            purp_host_damage_all(server);
            return true;
        }
        return false;
    }
}

/* =========================================================================================== */
/* Maintenance                                                                                */
/* =========================================================================================== */

static void *malloc_trim_thread(void *unused) {
    (void)unused;
    malloc_trim(0);
    return NULL;
}

void purp_server_maintenance_tick(purp_server_t *server) {
    if (!server) return;
    uint64_t now = purp_now_ms();
    purp_xdg_activation_prune(server, now);
    purp_server_reap_children(server);
    if (now - server->last_trim_ms >= PURP_MALLOC_TRIM_INTERVAL_MS) {
        server->last_trim_ms = now;
        spawn_detached_thread(malloc_trim_thread, NULL);
    }
}

void purp_server_socket_path(const purp_server_t *server, const char *prefix, char *out,
                             size_t out_len) {
    if (!out || out_len == 0) return;
    out[0] = '\0';
    if (!server) return;
    int n = snprintf(out, out_len, "%s/%s%s.sock", server->runtime_dir, prefix ? prefix : "",
                     server->socket_name);
    if (n < 0 || (size_t)n >= out_len) out[0] = '\0';
}
