/* Native DRM/KMS outputs, seat ownership and libinput input. */
#include "host_backend.h"
#include "config.h"

#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <drm_fourcc.h>
#include <errno.h>
#include <fcntl.h>
#include <gbm.h>
#include <libinput.h>
#include <libseat.h>
#include <libudev.h>
#include <limits.h>
#include <poll.h>
#include <sys/stat.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

typedef struct drm_backend drm_backend_t;

typedef struct drm_output {
    drm_backend_t *backend;
    bool occupied;
    bool connected;
    bool modeset;
    bool crtc_off;
    bool vblank_pending;
    char name[PURP_MONITOR_NAME_MAX];
    uint32_t connector_id;
    uint32_t crtc_id;
    drmModeModeInfo modes[PURP_MAX_MODES];
    size_t mode_count;
    drmModeModeInfo mode;
    drmModeCrtc *saved_crtc;
    struct gbm_surface *surface;
    EGLSurface egl_surface;
    struct gbm_bo *cur_bo;
    struct gbm_bo *next_bo;
} drm_output_t;

typedef struct seat_device {
    int fd;
    int id;
    struct seat_device *next;
} seat_device_t;

typedef struct drm_framebuffer {
    int fd;
    uint32_t id;
} drm_framebuffer_t;

struct drm_backend {
    purp_server_t *server;
    struct libseat *seat;
    struct udev *udev;
    struct udev_monitor *hotplug;
    struct libinput *input;
    struct gbm_device *gbm;
    EGLDisplay display;
    EGLContext context;
    EGLConfig config;
    int fd;
    int device_id;
    bool active;
    bool rescan_pending;
    bool resume_pending;
    bool renderer_ready;
    seat_device_t *devices;
    drm_output_t outputs[PURP_MAX_MONITORS];
    char touch_monitor[PURP_MAX_TOUCH_POINTS][PURP_MONITOR_NAME_MAX];
    purp_event_source_t *seat_source;
    purp_event_source_t *drm_source;
    purp_event_source_t *input_source;
    purp_event_source_t *hotplug_source;
    purp_event_source_t *timer;
};

static bool drm_make_current(void *data) {
    drm_backend_t *backend = data;
    return eglMakeCurrent(backend->display, EGL_NO_SURFACE, EGL_NO_SURFACE,
                          backend->context) == EGL_TRUE;
}

static bool internal_connector(uint32_t type) {
    return type == DRM_MODE_CONNECTOR_eDP || type == DRM_MODE_CONNECTOR_LVDS ||
           type == DRM_MODE_CONNECTOR_DSI;
}

static int32_t mode_refresh(const drmModeModeInfo *mode) {
    if (!mode->htotal || !mode->vtotal) return (int32_t)mode->vrefresh * 1000;
    double refresh = (double)mode->clock * 1000000.0 / mode->htotal / mode->vtotal;
    if (mode->flags & DRM_MODE_FLAG_INTERLACE) refresh *= 2.0;
    if (mode->flags & DRM_MODE_FLAG_DBLSCAN) refresh /= 2.0;
    if (mode->vscan > 1) refresh /= mode->vscan;
    return purp_round_i32(refresh);
}

static drmModeModeInfo initial_mode(const drmModeConnector *connector) {
    int chosen = 0;
    for (int i = 0; i < connector->count_modes; ++i) {
        if (connector->modes[i].type & DRM_MODE_TYPE_PREFERRED) { chosen = i; break; }
    }
    const char *resolution = getenv(PURP_ENV_RESOLUTION);
    unsigned width, height;
    if (resolution && sscanf(resolution, "%ux%u", &width, &height) == 2) {
        for (int i = 0; i < connector->count_modes; ++i) {
            if (connector->modes[i].hdisplay == width && connector->modes[i].vdisplay == height) {
                chosen = i;
                break;
            }
        }
    }
    const char *refresh = getenv(PURP_ENV_REFRESH_RATE);
    double hz = refresh ? strtod(refresh, NULL) : 0.0;
    if (isfinite(hz) && hz > 0.0) {
        int32_t target = purp_round_i32(hz * 1000.0);
        drmModeModeInfo base = connector->modes[chosen];
        int64_t distance = INT64_MAX;
        for (int i = 0; i < connector->count_modes; ++i) {
            const drmModeModeInfo *mode = &connector->modes[i];
            int64_t delta = llabs((int64_t)mode_refresh(mode) - target);
            if (mode->hdisplay == base.hdisplay && mode->vdisplay == base.vdisplay &&
                delta < distance) { chosen = i; distance = delta; }
        }
    }
    return connector->modes[chosen];
}

static int monitor_index(const drm_output_t *output) {
    for (size_t i = 0; i < purp_host_monitor_count(output->backend->server); ++i) {
        const purp_monitor_t *monitor = purp_host_monitor(output->backend->server, i);
        if (monitor && strcmp(monitor->name, output->name) == 0) return (int)i;
    }
    return -1;
}

static void framebuffer_destroy(struct gbm_bo *bo, void *data) {
    drm_framebuffer_t *framebuffer = data;
    (void)bo;
    drmModeRmFB(framebuffer->fd, framebuffer->id);
    free(framebuffer);
}

static uint32_t framebuffer_for_bo(drm_backend_t *backend, struct gbm_bo *bo) {
    drm_framebuffer_t *framebuffer = gbm_bo_get_user_data(bo);
    if (framebuffer) return framebuffer->id;
    framebuffer = calloc(1, sizeof(*framebuffer));
    if (!framebuffer) return 0;
    framebuffer->fd = backend->fd;
    uint32_t handles[4] = {0}, pitches[4] = {0}, offsets[4] = {0};
    uint64_t modifiers[4] = {0};
    int planes = gbm_bo_get_plane_count(bo);
    if (planes < 1 || planes > 4) { free(framebuffer); return 0; }
    uint64_t modifier = gbm_bo_get_modifier(bo);
    for (int i = 0; i < planes; ++i) {
        handles[i] = gbm_bo_get_handle_for_plane(bo, i).u32;
        pitches[i] = gbm_bo_get_stride_for_plane(bo, i);
        offsets[i] = gbm_bo_get_offset(bo, i);
        modifiers[i] = modifier;
    }
    int result = -1;
    if (modifier != DRM_FORMAT_MOD_INVALID && modifier != DRM_FORMAT_MOD_LINEAR)
        result = drmModeAddFB2WithModifiers(backend->fd, gbm_bo_get_width(bo),
                    gbm_bo_get_height(bo), gbm_bo_get_format(bo), handles, pitches, offsets,
                    modifiers, &framebuffer->id, DRM_MODE_FB_MODIFIERS);
    if (result < 0)
        result = drmModeAddFB2(backend->fd, gbm_bo_get_width(bo), gbm_bo_get_height(bo),
                              gbm_bo_get_format(bo), handles, pitches, offsets,
                              &framebuffer->id, 0);
    if (result < 0 && planes == 1)
        result = drmModeAddFB(backend->fd, gbm_bo_get_width(bo), gbm_bo_get_height(bo),
                             24, 32, pitches[0], handles[0], &framebuffer->id);
    if (result < 0) { free(framebuffer); return 0; }
    gbm_bo_set_user_data(bo, framebuffer, framebuffer_destroy);
    return framebuffer->id;
}

static void output_release_buffers(drm_output_t *output) {
    if (output->next_bo) gbm_surface_release_buffer(output->surface, output->next_bo);
    if (output->cur_bo) gbm_surface_release_buffer(output->surface, output->cur_bo);
    output->next_bo = output->cur_bo = NULL;
    output->vblank_pending = false;
    output->modeset = false;
}

static void output_destroy_surface(drm_output_t *output) {
    drm_backend_t *backend = output->backend;
    drm_make_current(backend);
    output_release_buffers(output);
    if (output->egl_surface != EGL_NO_SURFACE)
        eglDestroySurface(backend->display, output->egl_surface);
    if (output->surface) gbm_surface_destroy(output->surface);
    output->egl_surface = EGL_NO_SURFACE;
    output->surface = NULL;
}

static bool output_disable(drm_output_t *output) {
    if (!output->surface && output->crtc_off) return true;
    if (output->vblank_pending) return false;
    if (!output->crtc_off &&
        drmModeSetCrtc(output->backend->fd, output->crtc_id, 0, 0, 0, NULL, 0, NULL) < 0)
        return false;
    output->crtc_off = true;
    if (output->surface) output_destroy_surface(output);
    return true;
}

static bool output_create_surface(drm_output_t *output, drmModeModeInfo mode) {
    drm_backend_t *backend = output->backend;
    output->surface = gbm_surface_create(backend->gbm, mode.hdisplay, mode.vdisplay,
                         GBM_FORMAT_XRGB8888, GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
    if (!output->surface) return false;
    output->egl_surface = eglCreateWindowSurface(backend->display, backend->config,
                              (EGLNativeWindowType)output->surface, NULL);
    if (output->egl_surface == EGL_NO_SURFACE) {
        gbm_surface_destroy(output->surface);
        output->surface = NULL;
        return false;
    }
    if (!eglMakeCurrent(backend->display, output->egl_surface, output->egl_surface,
                        backend->context)) {
        output_destroy_surface(output);
        return false;
    }
    eglSwapInterval(backend->display, 0);
    output->mode = mode;
    output->modeset = false;
    return true;
}

static bool drm_apply_monitors(void *data, purp_server_t *server) {
    drm_backend_t *backend = data;
    if (!backend->active) return false;
    bool applied = true;
    for (size_t i = 0; i < PURP_MAX_MONITORS; ++i) {
        drm_output_t *output = &backend->outputs[i];
        if (!output->occupied) continue;
        int idx = monitor_index(output);
        purp_monitor_t *monitor = idx >= 0 ? purp_host_monitor(server, (size_t)idx) : NULL;
        if (!output->connected || !monitor || !monitor->enabled) {
            if (!output_disable(output)) applied = false;
            continue;
        }
        const drmModeModeInfo *chosen = NULL;
        int64_t distance = INT64_MAX;
        for (size_t j = 0; j < output->mode_count; ++j) {
            const drmModeModeInfo *mode = &output->modes[j];
            int64_t delta = llabs((int64_t)mode_refresh(mode) - monitor->refresh_mhz);
            if (mode->hdisplay == monitor->physical_size.w &&
                mode->vdisplay == monitor->physical_size.h && delta < distance) {
                chosen = mode;
                distance = delta;
            }
        }
        if (!chosen) { applied = false; continue; }
        if (output->surface && output->mode.hdisplay == chosen->hdisplay &&
            output->mode.vdisplay == chosen->vdisplay &&
            mode_refresh(&output->mode) == mode_refresh(chosen)) continue;
        if (!output_disable(output) || !output_create_surface(output, *chosen)) {
            applied = false;
            continue;
        }
        purp_host_damage_monitor(server, (size_t)idx);
    }
    return applied;
}

static bool drm_switch_vt(void *data, int vt) {
    drm_backend_t *backend = data;
    return libseat_switch_session(backend->seat, vt) == 0;
}

static const purp_backend_ops_t drm_ops = {
    .name = "drm",
    .allow_custom_modes = false,
    .apply_monitors = drm_apply_monitors,
    .make_current = drm_make_current,
    .switch_vt = drm_switch_vt,
};

static void page_flip(int fd, unsigned sequence, unsigned sec, unsigned usec, void *data) {
    drm_output_t *output = data;
    (void)fd; (void)sequence; (void)sec; (void)usec;
    if (!output->vblank_pending) return;
    if (output->cur_bo) gbm_surface_release_buffer(output->surface, output->cur_bo);
    output->cur_bo = output->next_bo;
    output->next_bo = NULL;
    output->vblank_pending = false;
    int idx = monitor_index(output);
    if (idx >= 0 && output->connected && output->backend->active)
        purp_host_monitor_frame_presented(output->backend->server, (size_t)idx);
}

static int drm_fd_ready(int fd, uint32_t mask, void *data) {
    drm_backend_t *backend = data;
    if (mask & (PURP_FD_ERROR | PURP_FD_HANGUP)) {
        purp_host_request_quit(backend->server);
        return 0;
    }
    drmEventContext events = { .version = 2, .page_flip_handler = page_flip };
    if ((mask & PURP_FD_READABLE) && drmHandleEvent(fd, &events) < 0 && errno != EINTR &&
        errno != EAGAIN && backend->active) {
        PURP_ERROR("DRM event dispatch failed: %s", strerror(errno));
        purp_host_request_quit(backend->server);
    }
    return 0;
}

static void render_outputs(drm_backend_t *backend) {
    if (!backend->active) return;
    for (size_t i = 0; i < PURP_MAX_MONITORS; ++i) {
        drm_output_t *output = &backend->outputs[i];
        if (!output->occupied || !output->connected || !output->surface ||
            output->vblank_pending) continue;
        int idx = monitor_index(output);
        if (idx < 0) continue;
        purp_monitor_t *monitor = purp_host_monitor(backend->server, (size_t)idx);
        if (!monitor->enabled || !purp_host_monitor_needs_frame(backend->server, (size_t)idx))
            continue;
        if (!eglMakeCurrent(backend->display, output->egl_surface, output->egl_surface,
                            backend->context)) continue;
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        if (!purp_host_render_monitor(backend->server, (size_t)idx, output->mode.hdisplay,
                                     output->mode.vdisplay, true) ||
            !eglSwapBuffers(backend->display, output->egl_surface)) {
            purp_host_damage_monitor(backend->server, (size_t)idx);
            continue;
        }
        struct gbm_bo *bo = gbm_surface_lock_front_buffer(output->surface);
        if (!bo) {
            purp_host_damage_monitor(backend->server, (size_t)idx);
            continue;
        }
        uint32_t framebuffer = framebuffer_for_bo(backend, bo);
        if (!framebuffer) {
            gbm_surface_release_buffer(output->surface, bo);
            purp_host_damage_monitor(backend->server, (size_t)idx);
            continue;
        }
        if (!output->modeset) {
            if (drmModeSetCrtc(backend->fd, output->crtc_id, framebuffer, 0, 0,
                              &output->connector_id, 1, &output->mode) == 0) {
                if (output->cur_bo) gbm_surface_release_buffer(output->surface, output->cur_bo);
                output->cur_bo = bo;
                output->modeset = true;
                output->crtc_off = false;
                purp_host_monitor_frame_presented(backend->server, (size_t)idx);
            } else {
                PURP_WARN("Cannot modeset %s: %s", output->name, strerror(errno));
                gbm_surface_release_buffer(output->surface, bo);
                purp_host_damage_monitor(backend->server, (size_t)idx);
            }
        } else if (drmModePageFlip(backend->fd, output->crtc_id, framebuffer,
                                  DRM_MODE_PAGE_FLIP_EVENT, output) == 0) {
            output->next_bo = bo;
            output->vblank_pending = true;
        } else {
            int error = errno;
            gbm_surface_release_buffer(output->surface, bo);
            purp_host_damage_monitor(backend->server, (size_t)idx);
            if (error != EBUSY) output->modeset = false;
        }
    }
}

static bool crtc_used(const drm_backend_t *backend, uint32_t crtc) {
    for (size_t i = 0; i < PURP_MAX_MONITORS; ++i)
        if (backend->outputs[i].occupied && backend->outputs[i].crtc_id == crtc) return true;
    return false;
}

static uint32_t pick_crtc(drm_backend_t *backend, const drmModeRes *resources,
                          const drmModeConnector *connector) {
    drmModeEncoder *encoder = drmModeGetEncoder(backend->fd, connector->encoder_id);
    if (encoder) {
        uint32_t current = encoder->crtc_id;
        drmModeFreeEncoder(encoder);
        if (current && !crtc_used(backend, current)) return current;
    }
    for (int i = 0; i < connector->count_encoders; ++i) {
        encoder = drmModeGetEncoder(backend->fd, connector->encoders[i]);
        if (!encoder) continue;
        for (int j = 0; j < resources->count_crtcs && j < 32; ++j) {
            if ((encoder->possible_crtcs & (1u << (unsigned)j)) &&
                !crtc_used(backend, resources->crtcs[j])) {
                uint32_t chosen = resources->crtcs[j];
                drmModeFreeEncoder(encoder);
                return chosen;
            }
        }
        drmModeFreeEncoder(encoder);
    }
    return 0;
}

static int connector_compare(const void *left, const void *right) {
    const drmModeConnector *a = *(drmModeConnector *const *)left;
    const drmModeConnector *b = *(drmModeConnector *const *)right;
    bool a_internal = internal_connector(a->connector_type);
    bool b_internal = internal_connector(b->connector_type);
    if (a_internal != b_internal) return a_internal ? -1 : 1;
    if (a->connector_type != b->connector_type)
        return a->connector_type < b->connector_type ? -1 : 1;
    return a->connector_type_id < b->connector_type_id ? -1 :
           a->connector_type_id > b->connector_type_id;
}

static void rescan_connectors(drm_backend_t *backend) {
    if (!backend->active) return;
    drmModeRes *resources = drmModeGetResources(backend->fd);
    if (!resources) return;
    drmModeConnector **connectors = calloc((size_t)resources->count_connectors + 1,
                                          sizeof(*connectors));
    if (!connectors) { drmModeFreeResources(resources); return; }
    size_t count = 0;
    for (int i = 0; i < resources->count_connectors; ++i) {
        drmModeConnector *connector = drmModeGetConnector(backend->fd, resources->connectors[i]);
        if (!connector) continue;
        if (connector->connection == DRM_MODE_CONNECTED && connector->count_modes > 0 &&
            connector->connector_type != DRM_MODE_CONNECTOR_WRITEBACK)
            connectors[count++] = connector;
        else drmModeFreeConnector(connector);
    }
    qsort(connectors, count, sizeof(*connectors), connector_compare);
    for (size_t i = 0; i < PURP_MAX_MONITORS; ++i) {
        drm_output_t *output = &backend->outputs[i];
        if (!output->occupied) continue;
        output->connected = false;
        for (size_t j = 0; j < count; ++j)
            if (connectors[j]->connector_id == output->connector_id) output->connected = true;
        if (!output->connected && output_disable(output)) {
            if (output->saved_crtc) drmModeFreeCrtc(output->saved_crtc);
            memset(output, 0, sizeof(*output));
        }
    }
    const char *names[PURP_MAX_MONITORS];
    size_t live_count = 0;
    bool retry = false;
    for (size_t i = 0; i < count; ++i) {
        drmModeConnector *connector = connectors[i];
        drm_output_t *output = NULL;
        for (size_t j = 0; j < PURP_MAX_MONITORS; ++j) {
            if (backend->outputs[j].occupied &&
                backend->outputs[j].connector_id == connector->connector_id) {
                output = &backend->outputs[j];
                break;
            }
        }
        if (!output) {
            uint32_t crtc = pick_crtc(backend, resources, connector);
            if (!crtc) continue;
            for (size_t j = 0; j < PURP_MAX_MONITORS; ++j)
                if (!backend->outputs[j].occupied) { output = &backend->outputs[j]; break; }
            if (!output) continue;
            *output = (drm_output_t){ .backend = backend, .occupied = true, .connected = true,
                .connector_id = connector->connector_id, .crtc_id = crtc,
                .egl_surface = EGL_NO_SURFACE, .mode = initial_mode(connector) };
            const char *type = drmModeGetConnectorTypeName(connector->connector_type);
            snprintf(output->name, sizeof(output->name), "%s-%u", type ? type : "Unknown",
                     connector->connector_type_id);
            output->saved_crtc = drmModeGetCrtc(backend->fd, crtc);
        }
        output->mode_count = (size_t)connector->count_modes < PURP_MAX_MODES
                          ? (size_t)connector->count_modes : PURP_MAX_MODES;
        memcpy(output->modes, connector->modes, output->mode_count * sizeof(output->modes[0]));
        /* Keep an explicitly selected mode even when the public mode table is capped. */
        bool has_current = false;
        for (size_t j = 0; j < output->mode_count; ++j)
            if (memcmp(&output->modes[j], &output->mode, sizeof(output->mode)) == 0)
                has_current = true;
        if (!has_current && output->mode_count)
            output->modes[output->mode_count - 1] = output->mode;
        bool internal = internal_connector(connector->connector_type);
        purp_monitor_desc_t desc = { .physical_size = { output->mode.hdisplay,
                                                       output->mode.vdisplay },
            .refresh_mhz = mode_refresh(&output->mode),
            .physical_size_mm = { (int32_t)connector->mmWidth, (int32_t)connector->mmHeight },
            .is_internal = internal, .mode_count = output->mode_count,
            .scale = getenv(PURP_ENV_SCALE) || internal || output->mode.hdisplay >= 2200
                   ? purp_host_wm(backend->server)->default_scale : 1.0 };
        snprintf(desc.name, sizeof(desc.name), "%s", output->name);
        snprintf(desc.description, sizeof(desc.description), "%s (%s Display)", output->name,
                 internal ? "Internal" : "External");
        snprintf(desc.make, sizeof(desc.make), "DRM");
        snprintf(desc.model, sizeof(desc.model), "%s", output->name);
        /* RGB/BGR entries agree; only Unknown and None have different numeric values. */
        if (connector->subpixel >= DRM_MODE_SUBPIXEL_UNKNOWN &&
            connector->subpixel <= DRM_MODE_SUBPIXEL_NONE)
            desc.subpixel = connector->subpixel == DRM_MODE_SUBPIXEL_NONE ? 1u :
                            connector->subpixel == DRM_MODE_SUBPIXEL_UNKNOWN ? 0u :
                            (uint32_t)connector->subpixel;
        for (size_t j = 0; j < desc.mode_count; ++j) {
            desc.modes[j] = (purp_display_mode_t){ .width = output->modes[j].hdisplay,
                .height = output->modes[j].vdisplay, .refresh_mhz = mode_refresh(&output->modes[j]),
                .preferred = (output->modes[j].type & DRM_MODE_TYPE_PREFERRED) != 0 };
        }
        if (purp_host_monitor_add(backend->server, &desc) >= 0)
            names[live_count++] = output->name;
    }
    purp_host_monitors_retain(backend->server, names, live_count);
    purp_host_load_saved_monitors(backend->server);
    drm_apply_monitors(backend, backend->server);
    purp_host_damage_all(backend->server);
    for (size_t i = 0; i < count; ++i) drmModeFreeConnector(connectors[i]);
    free(connectors);
    drmModeFreeResources(resources);
    for (size_t i = 0; i < PURP_MAX_MONITORS; ++i)
        if (backend->outputs[i].occupied && !backend->outputs[i].connected) retry = true;
    backend->rescan_pending = retry;
}

static void enable_seat(struct libseat *seat, void *data) {
    drm_backend_t *backend = data;
    (void)seat;
    backend->active = true;
    backend->resume_pending = true;
    backend->rescan_pending = true;
    if (backend->input && libinput_resume(backend->input) < 0)
        PURP_WARN("Cannot resume libinput");
    if (backend->server) purp_host_session_active(backend->server, true);
}

static void pause_seat(struct libseat *seat, void *data) {
    drm_backend_t *backend = data;
    backend->active = false;
    if (backend->server) purp_host_session_active(backend->server, false);
    memset(backend->touch_monitor, 0, sizeof(backend->touch_monitor));
    if (backend->input) libinput_suspend(backend->input);
    libseat_disable_seat(seat);
}

static int seat_fd_ready(int fd, uint32_t mask, void *data) {
    drm_backend_t *backend = data;
    (void)fd;
    if ((mask & (PURP_FD_ERROR | PURP_FD_HANGUP)) || libseat_dispatch(backend->seat, 0) < 0)
        purp_host_request_quit(backend->server);
    return 0;
}

static int input_open(const char *path, int flags, void *data) {
    drm_backend_t *backend = data;
    seat_device_t *device = calloc(1, sizeof(*device));
    if (!device) return -ENOMEM;
    device->id = libseat_open_device(backend->seat, path, &device->fd);
    if (device->id < 0) { int error = errno; free(device); return -error; }
    int status = fcntl(device->fd, F_GETFL);
    if (status >= 0) fcntl(device->fd, F_SETFL, status | (flags & O_NONBLOCK));
    fcntl(device->fd, F_SETFD, FD_CLOEXEC);
    device->next = backend->devices;
    backend->devices = device;
    return device->fd;
}

static void input_close(int fd, void *data) {
    drm_backend_t *backend = data;
    seat_device_t **link = &backend->devices;
    while (*link && (*link)->fd != fd) link = &(*link)->next;
    if (*link) {
        seat_device_t *device = *link;
        *link = device->next;
        libseat_close_device(backend->seat, device->id);
        close(device->fd);
        free(device);
    }
}

static void input_axis(drm_backend_t *backend, struct libinput_event_pointer *pointer,
                       enum libinput_event_type type) {
    bool wheel = type == LIBINPUT_EVENT_POINTER_SCROLL_WHEEL;
    purp_axis_event_t axis = { .time_ms = libinput_event_pointer_get_time(pointer),
        .source = wheel ? PURP_AXIS_SOURCE_WHEEL :
                  type == LIBINPUT_EVENT_POINTER_SCROLL_FINGER ? PURP_AXIS_SOURCE_FINGER :
                  PURP_AXIS_SOURCE_CONTINUOUS,
        .inverted = libinput_device_config_scroll_get_natural_scroll_enabled(
                        libinput_event_get_device(libinput_event_pointer_get_base_event(pointer))) != 0,
        .has_vertical = libinput_event_pointer_has_axis(pointer, LIBINPUT_POINTER_AXIS_SCROLL_VERTICAL),
        .has_horizontal = libinput_event_pointer_has_axis(pointer, LIBINPUT_POINTER_AXIS_SCROLL_HORIZONTAL) };
    if (axis.has_vertical) {
        axis.vertical = libinput_event_pointer_get_scroll_value(pointer, LIBINPUT_POINTER_AXIS_SCROLL_VERTICAL);
        if (wheel) axis.vertical_v120 = purp_round_i32(libinput_event_pointer_get_scroll_value_v120(
                                            pointer, LIBINPUT_POINTER_AXIS_SCROLL_VERTICAL));
    }
    if (axis.has_horizontal) {
        axis.horizontal = libinput_event_pointer_get_scroll_value(pointer, LIBINPUT_POINTER_AXIS_SCROLL_HORIZONTAL);
        if (wheel) axis.horizontal_v120 = purp_round_i32(libinput_event_pointer_get_scroll_value_v120(
                                            pointer, LIBINPUT_POINTER_AXIS_SCROLL_HORIZONTAL));
    }
    purp_host_pointer_axis(backend->server, &axis);
    purp_host_pointer_frame(backend->server);
}

static void input_touch(drm_backend_t *backend, struct libinput_event *event,
                        enum libinput_event_type type) {
    if (type == LIBINPUT_EVENT_TOUCH_FRAME) { purp_host_touch_frame(backend->server); return; }
    struct libinput_event_touch *touch = libinput_event_get_touch_event(event);
    int32_t slot = libinput_event_touch_get_seat_slot(touch);
    if (slot < 0 || slot >= PURP_MAX_TOUCH_POINTS) return;
    uint32_t time = libinput_event_touch_get_time(touch);
    if (type == LIBINPUT_EVENT_TOUCH_UP || type == LIBINPUT_EVENT_TOUCH_CANCEL) {
        if (type == LIBINPUT_EVENT_TOUCH_UP) purp_host_touch_up(backend->server, time, slot);
        else purp_host_touch_cancel(backend->server, slot);
        backend->touch_monitor[slot][0] = '\0';
        return;
    }
    const char *name = backend->touch_monitor[slot];
    if (type == LIBINPUT_EVENT_TOUCH_DOWN || !*name)
        name = libinput_device_get_output_name(libinput_event_get_device(event));
    size_t idx = purp_wm_resolve_touch_monitor(purp_host_wm(backend->server), name);
    purp_monitor_t *monitor = purp_host_monitor(backend->server, idx);
    if (!monitor || !monitor->enabled) return;
    if (type == LIBINPUT_EVENT_TOUCH_DOWN)
        snprintf(backend->touch_monitor[slot], sizeof(backend->touch_monitor[slot]), "%s", monitor->name);
    double x = monitor->loc.x + libinput_event_touch_get_x_transformed(touch,
                                                         (uint32_t)monitor->logical_size.w);
    double y = monitor->loc.y + libinput_event_touch_get_y_transformed(touch,
                                                         (uint32_t)monitor->logical_size.h);
    x = purp_clamp_f64(x, monitor->loc.x, monitor->loc.x + monitor->logical_size.w - 0.0001);
    y = purp_clamp_f64(y, monitor->loc.y, monitor->loc.y + monitor->logical_size.h - 0.0001);
    if (type == LIBINPUT_EVENT_TOUCH_DOWN) purp_host_touch_down(backend->server, time, slot, x, y);
    else purp_host_touch_motion(backend->server, time, slot, x, y);
}

static int input_fd_ready(int fd, uint32_t mask, void *data) {
    drm_backend_t *backend = data;
    (void)fd;
    if (mask & (PURP_FD_ERROR | PURP_FD_HANGUP)) return 0;
    if (libinput_dispatch(backend->input) < 0) return 0;
    struct libinput_event *event;
    while ((event = libinput_get_event(backend->input))) {
        enum libinput_event_type type = libinput_event_get_type(event);
        if (!backend->active) { libinput_event_destroy(event); continue; }
        switch (type) {
        case LIBINPUT_EVENT_KEYBOARD_KEY: {
            struct libinput_event_keyboard *key = libinput_event_get_keyboard_event(event);
            purp_host_keyboard_key(backend->server, libinput_event_keyboard_get_time(key),
                libinput_event_keyboard_get_key(key),
                libinput_event_keyboard_get_key_state(key) == LIBINPUT_KEY_STATE_PRESSED);
            break;
        }
        case LIBINPUT_EVENT_POINTER_MOTION: {
            struct libinput_event_pointer *pointer = libinput_event_get_pointer_event(event);
            purp_host_pointer_motion(backend->server, libinput_event_pointer_get_time(pointer),
                libinput_event_pointer_get_dx(pointer), libinput_event_pointer_get_dy(pointer),
                libinput_event_pointer_get_dx_unaccelerated(pointer),
                libinput_event_pointer_get_dy_unaccelerated(pointer));
            purp_host_pointer_frame(backend->server);
            break;
        }
        case LIBINPUT_EVENT_POINTER_MOTION_ABSOLUTE: {
            struct libinput_event_pointer *pointer = libinput_event_get_pointer_event(event);
            purp_rect_t bounds = purp_wm_desktop_bounds(purp_host_wm(backend->server));
            purp_host_pointer_motion_absolute(backend->server, libinput_event_pointer_get_time(pointer),
                bounds.x + libinput_event_pointer_get_absolute_x_transformed(pointer, (uint32_t)bounds.w),
                bounds.y + libinput_event_pointer_get_absolute_y_transformed(pointer, (uint32_t)bounds.h));
            purp_host_pointer_frame(backend->server);
            break;
        }
        case LIBINPUT_EVENT_POINTER_BUTTON: {
            struct libinput_event_pointer *pointer = libinput_event_get_pointer_event(event);
            purp_host_pointer_button(backend->server, libinput_event_pointer_get_time(pointer),
                libinput_event_pointer_get_button(pointer),
                libinput_event_pointer_get_button_state(pointer) == LIBINPUT_BUTTON_STATE_PRESSED);
            purp_host_pointer_frame(backend->server);
            break;
        }
        case LIBINPUT_EVENT_POINTER_SCROLL_WHEEL:
        case LIBINPUT_EVENT_POINTER_SCROLL_FINGER:
        case LIBINPUT_EVENT_POINTER_SCROLL_CONTINUOUS:
            input_axis(backend, libinput_event_get_pointer_event(event), type);
            break;
        case LIBINPUT_EVENT_TOUCH_DOWN:
        case LIBINPUT_EVENT_TOUCH_MOTION:
        case LIBINPUT_EVENT_TOUCH_UP:
        case LIBINPUT_EVENT_TOUCH_CANCEL:
        case LIBINPUT_EVENT_TOUCH_FRAME:
            input_touch(backend, event, type);
            break;
        default: break;
        }
        libinput_event_destroy(event);
    }
    return 0;
}

static int hotplug_fd_ready(int fd, uint32_t mask, void *data) {
    drm_backend_t *backend = data;
    (void)fd;
    if (!(mask & PURP_FD_READABLE)) return 0;
    struct udev_device *device;
    while ((device = udev_monitor_receive_device(backend->hotplug))) {
        backend->rescan_pending = true;
        udev_device_unref(device);
    }
    return 0;
}

static int tick_timer(void *data) {
    drm_backend_t *backend = data;
    purp_host_timer_update(backend->timer, PURP_TICK_INTERVAL_MS);
    return 0;
}

static char *discover_gpu(drm_backend_t *backend) {
    const char *explicit_path = getenv(PURP_ENV_DRM_DEVICE);
    if (explicit_path && *explicit_path) return strdup(explicit_path);
    struct udev_enumerate *enumerate = udev_enumerate_new(backend->udev);
    if (!enumerate) return NULL;
    udev_enumerate_add_match_subsystem(enumerate, "drm");
    udev_enumerate_add_match_sysname(enumerate, "card[0-9]*");
    udev_enumerate_scan_devices(enumerate);
    char *selected = NULL;
    int selected_rank = -1;
    struct udev_list_entry *entry;
    udev_list_entry_foreach(entry, udev_enumerate_get_list_entry(enumerate)) {
        struct udev_device *device = udev_device_new_from_syspath(backend->udev,
                                                         udev_list_entry_get_name(entry));
        if (!device) continue;
        const char *path = udev_device_get_devnode(device);
        const char *name = udev_device_get_sysname(device);
        const char *seat_name = udev_device_get_property_value(device, "ID_SEAT");
        if (!seat_name) seat_name = "seat0";
        if (!path || !name || strspn(name + 4, "0123456789") != strlen(name + 4) ||
            strcmp(seat_name, libseat_seat_name(backend->seat)) != 0) {
            udev_device_unref(device);
            continue;
        }
        struct udev_device *pci = udev_device_get_parent_with_subsystem_devtype(device, "pci", NULL);
        const char *boot = pci ? udev_device_get_sysattr_value(pci, "boot_vga") : NULL;
        int rank = boot && strcmp(boot, "1") == 0 ? 1 : 0;
        if (rank > selected_rank) {
            int fd = -1;
            int id = libseat_open_device(backend->seat, path, &fd);
            if (id >= 0) {
                drmModeRes *resources = drmModeGetResources(fd);
                if (resources && resources->count_crtcs > 0 && resources->count_connectors > 0) {
                    char *copy = strdup(path);
                    if (copy) { free(selected); selected = copy; selected_rank = rank; }
                }
                if (resources) drmModeFreeResources(resources);
                libseat_close_device(backend->seat, id);
                close(fd);
            }
        }
        udev_device_unref(device);
    }
    udev_enumerate_unref(enumerate);
    return selected;
}

static bool init_egl(drm_backend_t *backend) {
    backend->gbm = gbm_create_device(backend->fd);
    if (!backend->gbm) return false;
    backend->display = eglGetPlatformDisplay(EGL_PLATFORM_GBM_KHR, backend->gbm, NULL);
    if (backend->display == EGL_NO_DISPLAY) {
        PFNEGLGETPLATFORMDISPLAYEXTPROC get_platform =
            (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
        if (get_platform) backend->display = get_platform(EGL_PLATFORM_GBM_KHR, backend->gbm, NULL);
    }
    if (backend->display == EGL_NO_DISPLAY || !eglInitialize(backend->display, NULL, NULL))
        return false;
    const char *extensions = eglQueryString(backend->display, EGL_EXTENSIONS);
    if (!extensions || !strstr(extensions, "EGL_KHR_surfaceless_context")) {
        PURP_ERROR("DRM EGL requires EGL_KHR_surfaceless_context");
        return false;
    }
    if (!eglBindAPI(EGL_OPENGL_ES_API)) return false;
    const EGLint attributes[] = { EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 0, EGL_NONE };
    EGLint count = 0;
    if (!eglChooseConfig(backend->display, attributes, NULL, 0, &count) || count <= 0)
        return false;
    EGLConfig *configs = calloc((size_t)count, sizeof(*configs));
    if (!configs) return false;
    if (!eglChooseConfig(backend->display, attributes, configs, count, &count)) {
        free(configs);
        return false;
    }
    bool found = false;
    for (EGLint i = 0; i < count; ++i) {
        EGLint visual = 0;
        eglGetConfigAttrib(backend->display, configs[i], EGL_NATIVE_VISUAL_ID, &visual);
        if ((uint32_t)visual == GBM_FORMAT_XRGB8888) {
            backend->config = configs[i]; found = true; break;
        }
    }
    free(configs);
    if (!found) return false;
    const EGLint context_attributes[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    backend->context = eglCreateContext(backend->display, backend->config, EGL_NO_CONTEXT,
                                        context_attributes);
    return backend->context != EGL_NO_CONTEXT && drm_make_current(backend);
}

static void resume_outputs(drm_backend_t *backend) {
    /* Consume old flips before invalidating their BO bookkeeping on VT re-entry. */
    struct pollfd pending = { .fd = backend->fd, .events = POLLIN };
    while (poll(&pending, 1, 0) > 0 && (pending.revents & POLLIN))
        drm_fd_ready(backend->fd, PURP_FD_READABLE, backend);
    for (size_t i = 0; i < PURP_MAX_MONITORS; ++i) {
        drm_output_t *output = &backend->outputs[i];
        if (!output->occupied) continue;
        output->crtc_off = false;
        if (!output->surface) continue;
        if (drmModeSetCrtc(backend->fd, output->crtc_id, 0, 0, 0, NULL, 0, NULL) == 0) {
            output->crtc_off = true;
            output_release_buffers(output);
        }
        output->modeset = false;
    }
    backend->resume_pending = false;
}

static void destroy_backend(drm_backend_t *backend) {
    if (backend->timer) purp_host_remove_source(backend->timer);
    if (backend->hotplug_source) purp_host_remove_source(backend->hotplug_source);
    if (backend->input_source) purp_host_remove_source(backend->input_source);
    if (backend->drm_source) purp_host_remove_source(backend->drm_source);
    if (backend->seat_source) purp_host_remove_source(backend->seat_source);
    if (backend->display != EGL_NO_DISPLAY && backend->context != EGL_NO_CONTEXT)
        drm_make_current(backend);
    for (size_t i = 0; i < PURP_MAX_MONITORS; ++i) {
        drm_output_t *output = &backend->outputs[i];
        if (!output->occupied) continue;
        if (backend->active) {
            drmModeCrtc *saved = output->saved_crtc;
            if (saved && saved->mode_valid)
                drmModeSetCrtc(backend->fd, saved->crtc_id, saved->buffer_id, saved->x, saved->y,
                              &output->connector_id, 1, &saved->mode);
            else drmModeSetCrtc(backend->fd, output->crtc_id, 0, 0, 0, NULL, 0, NULL);
        }
        output_destroy_surface(output);
        if (output->saved_crtc) drmModeFreeCrtc(output->saved_crtc);
    }
    if (backend->renderer_ready) purp_host_renderer_finish(backend->server);
    if (backend->server) purp_host_server_destroy(backend->server);
    backend->server = NULL;
    if (backend->display != EGL_NO_DISPLAY) {
        eglMakeCurrent(backend->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (backend->context != EGL_NO_CONTEXT) eglDestroyContext(backend->display, backend->context);
        eglTerminate(backend->display);
    }
    if (backend->gbm) gbm_device_destroy(backend->gbm);
    if (backend->input) libinput_unref(backend->input);
    while (backend->devices) input_close(backend->devices->fd, backend);
    if (backend->hotplug) udev_monitor_unref(backend->hotplug);
    if (backend->device_id >= 0) libseat_close_device(backend->seat, backend->device_id);
    if (backend->fd >= 0) close(backend->fd);
    if (backend->seat) libseat_close_seat(backend->seat);
    if (backend->udev) udev_unref(backend->udev);
}

int purp_backend_drm_run(const purp_options_t *opts) {
    drm_backend_t backend = { .fd = -1, .device_id = -1, .display = EGL_NO_DISPLAY,
                              .context = EGL_NO_CONTEXT };
    int result = EXIT_FAILURE;
    const struct libseat_seat_listener seat_listener = {
        .enable_seat = enable_seat, .disable_seat = pause_seat };
    backend.seat = libseat_open_seat(&seat_listener, &backend);
    if (!backend.seat) { PURP_ERROR("Cannot open seat: %s", strerror(errno)); goto done; }
    /* The initial enable notification may be queued until the first dispatch. */
    while (!backend.active && !purp_signal_quit) {
        if (libseat_dispatch(backend.seat, 100) < 0) goto done;
    }
    if (purp_signal_quit) { result = EXIT_SUCCESS; goto done; }
    backend.udev = udev_new();
    if (!backend.udev) goto done;
    char *path = discover_gpu(&backend);
    if (!path) { PURP_ERROR("No KMS GPU found on seat %s", libseat_seat_name(backend.seat)); goto done; }
    backend.device_id = libseat_open_device(backend.seat, path, &backend.fd);
    if (backend.device_id < 0) PURP_ERROR("Cannot open DRM device %s: %s", path, strerror(errno));
    free(path);
    if (backend.device_id < 0) goto done;
    fcntl(backend.fd, F_SETFD, FD_CLOEXEC);
    if (!init_egl(&backend)) { PURP_ERROR("Cannot initialise GBM/EGL (0x%x)", eglGetError()); goto done; }
    backend.server = purp_host_server_create(opts);
    if (!backend.server) goto done;
    purp_host_set_backend(backend.server, &drm_ops, &backend);
    dev_t render_node = 0;
    char *render_path = drmGetRenderDeviceNameFromFd(backend.fd);
    if (render_path) {
        struct stat info;
        if (stat(render_path, &info) == 0) render_node = info.st_rdev;
        free(render_path);
    }
    if (!purp_host_renderer_init(backend.server, backend.display, backend.context, render_node))
        goto done;
    backend.renderer_ready = true;
    static const struct libinput_interface input_interface = {
        .open_restricted = input_open, .close_restricted = input_close };
    backend.input = libinput_udev_create_context(&input_interface, &backend, backend.udev);
    if (!backend.input || libinput_udev_assign_seat(backend.input,
                                                  libseat_seat_name(backend.seat)) < 0) {
        PURP_ERROR("Cannot initialise libinput for seat %s", libseat_seat_name(backend.seat));
        goto done;
    }
    backend.hotplug = udev_monitor_new_from_netlink(backend.udev, "udev");
    if (!backend.hotplug ||
        udev_monitor_filter_add_match_subsystem_devtype(backend.hotplug, "drm", NULL) < 0 ||
        udev_monitor_enable_receiving(backend.hotplug) < 0) goto done;
    backend.seat_source = purp_host_add_fd(backend.server, libseat_get_fd(backend.seat),
                                         PURP_FD_READABLE, seat_fd_ready, &backend);
    backend.drm_source = purp_host_add_fd(backend.server, backend.fd,
                                        PURP_FD_READABLE, drm_fd_ready, &backend);
    backend.input_source = purp_host_add_fd(backend.server, libinput_get_fd(backend.input),
                                          PURP_FD_READABLE, input_fd_ready, &backend);
    backend.hotplug_source = purp_host_add_fd(backend.server, udev_monitor_get_fd(backend.hotplug),
                                            PURP_FD_READABLE, hotplug_fd_ready, &backend);
    backend.timer = purp_host_add_timer(backend.server, tick_timer, &backend);
    if (!backend.seat_source || !backend.drm_source || !backend.input_source ||
        !backend.hotplug_source || !backend.timer) goto done;
    purp_host_timer_update(backend.timer, PURP_TICK_INTERVAL_MS);
    backend.resume_pending = false;
    rescan_connectors(&backend);
    if (!purp_host_start(backend.server)) goto done;
    while (!purp_host_should_quit(backend.server)) {
        if (purp_host_dispatch(backend.server, PURP_TICK_INTERVAL_MS) < 0 && errno != EINTR)
            goto done;
        if (backend.active && backend.resume_pending) resume_outputs(&backend);
        if (backend.active && backend.rescan_pending) rescan_connectors(&backend);
        purp_host_tick(backend.server);
        render_outputs(&backend);
        purp_host_flush_clients(backend.server);
    }
    result = EXIT_SUCCESS;
done:
    destroy_backend(&backend);
    return result;
}
