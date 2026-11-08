/* Wayland-client and surfaceless backends; no compositor-private or server headers. */
#include "host_backend.h"
#include "wm.h"
#include "config.h"
#include "types.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/input-event-codes.h>
#include <sys/stat.h>
#include <unistd.h>

#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <gbm.h>
#include <wayland-client.h>
#include <wayland-egl.h>
#include "xdg-shell-client-protocol.h"

typedef struct host_touch {
    bool active;
    int32_t id;
    char monitor[PURP_MONITOR_NAME_MAX];
} host_touch_t;

typedef struct window_backend {
    purp_server_t *server;
    EGLDisplay egl_display;
    EGLContext egl_context;
    EGLSurface egl_surface;
    struct gbm_device *gbm;
    int render_fd;
    bool renderer_ready;
    bool nested;
    bool virtual_outputs;
    bool failed;
    bool closed;
    bool configured;
    bool host_dirty;
    bool read_prepared;
    int32_t width, height;
    int32_t pending_width, pending_height;
    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_compositor *compositor;
    struct xdg_wm_base *wm_base;
    struct wl_seat *seat;
    uint32_t compositor_name, wm_base_name, seat_name;
    struct wl_keyboard *keyboard;
    struct wl_pointer *pointer;
    struct wl_touch *touch;
    struct wl_surface *surface;
    struct xdg_surface *xdg_surface;
    struct xdg_toplevel *toplevel;
    struct wl_egl_window *egl_window;
    struct wl_callback *frame;
    purp_event_source_t *display_source;
    purp_event_source_t *write_source;
    purp_event_source_t *timer_source;
    purp_axis_event_t axis;
    bool axis_inverted[2];
    bool keys[KEY_CNT];
    bool buttons[KEY_CNT];
    host_touch_t touches[PURP_MAX_TOUCH_POINTS];
    purp_offscreen_t targets[PURP_MAX_MONITORS];
} window_backend_t;

static uint32_t host_time_ms(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint32_t)((uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u);
}

static void backend_fail(window_backend_t *backend, const char *operation) {
    if (!backend->failed)
        PURP_ERROR("%s: %s", operation, strerror(errno));
    backend->failed = true;
    if (backend->server) purp_host_request_quit(backend->server);
}

static bool has_extension(const char *extensions, const char *name) {
    if (!extensions) return false;
    size_t length = strlen(name);
    const char *found = extensions;
    while ((found = strstr(found, name)) != NULL) {
        if ((found == extensions || found[-1] == ' ') &&
            (found[length] == '\0' || found[length] == ' ')) return true;
        found += length;
    }
    return false;
}

static bool backend_make_current(void *data) {
    window_backend_t *backend = data;
    return eglMakeCurrent(backend->egl_display, backend->egl_surface,
                          backend->egl_surface, backend->egl_context) == EGL_TRUE;
}

static dev_t backend_render_node(window_backend_t *backend) {
    struct stat info;
    if (backend->render_fd >= 0 && fstat(backend->render_fd, &info) == 0)
        return info.st_rdev;
    PFNEGLQUERYDISPLAYATTRIBEXTPROC query_display =
        (PFNEGLQUERYDISPLAYATTRIBEXTPROC)eglGetProcAddress("eglQueryDisplayAttribEXT");
    PFNEGLQUERYDEVICESTRINGEXTPROC query_device =
        (PFNEGLQUERYDEVICESTRINGEXTPROC)eglGetProcAddress("eglQueryDeviceStringEXT");
    EGLAttrib device = 0;
    if (query_display && query_device &&
        has_extension(eglQueryString(backend->egl_display, EGL_EXTENSIONS),
                      "EGL_EXT_device_query") &&
        query_display(backend->egl_display, EGL_DEVICE_EXT, &device)) {
        const char *extensions = query_device((EGLDeviceEXT)device, EGL_EXTENSIONS);
        if (has_extension(extensions, "EGL_EXT_device_drm_render_node")) {
            const char *path = query_device((EGLDeviceEXT)device, EGL_DRM_RENDER_NODE_FILE_EXT);
            if (path && stat(path, &info) == 0) return info.st_rdev;
        }
    }
    return 0;
}

static EGLDisplay platform_display(EGLenum platform, void *native) {
    PFNEGLGETPLATFORMDISPLAYEXTPROC get_display =
        (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
    if (get_display) return get_display(platform, native, NULL);
    return eglGetPlatformDisplay(platform, native, NULL);
}

static bool initialize_display(window_backend_t *backend, EGLDisplay display) {
    if (display == EGL_NO_DISPLAY) return false;
    if (!eglInitialize(display, NULL, NULL)) return false;
    backend->egl_display = display;
    return true;
}

static bool initialize_egl(window_backend_t *backend) {
    if (backend->nested) {
        if (!initialize_display(backend, platform_display(EGL_PLATFORM_WAYLAND_KHR,
                                                         backend->display)) &&
            !initialize_display(backend, eglGetDisplay((EGLNativeDisplayType)backend->display))) {
            PURP_ERROR("Cannot initialize host Wayland EGL display (0x%x)", eglGetError());
            return false;
        }
    } else {
        bool surfaceless = initialize_display(backend,
            platform_display(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY));
        if (surfaceless && !has_extension(eglQueryString(backend->egl_display, EGL_EXTENSIONS),
                                         "EGL_KHR_surfaceless_context")) {
            eglTerminate(backend->egl_display);
            backend->egl_display = EGL_NO_DISPLAY;
            surfaceless = false;
        }
        if (!surfaceless) {
            backend->render_fd = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
            if (backend->render_fd < 0 ||
                !(backend->gbm = gbm_create_device(backend->render_fd)) ||
                !initialize_display(backend,
                    platform_display(EGL_PLATFORM_GBM_KHR, backend->gbm))) {
                PURP_ERROR("Cannot initialize surfaceless EGL or GBM /dev/dri/renderD128");
                return false;
            }
        }
        if (!has_extension(eglQueryString(backend->egl_display, EGL_EXTENSIONS),
                           "EGL_KHR_surfaceless_context")) {
            PURP_ERROR("Headless backend requires EGL_KHR_surfaceless_context");
            return false;
        }
    }
    const EGLint attributes[] = {
        EGL_SURFACE_TYPE, backend->nested ? EGL_WINDOW_BIT : 0,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 8, EGL_NONE
    };
    EGLConfig config = NULL;
    EGLint count = 0;
    if (!eglBindAPI(EGL_OPENGL_ES_API) ||
        !eglChooseConfig(backend->egl_display, attributes, &config, 1, &count) || count < 1) {
        PURP_ERROR("Cannot choose GLES2 EGL configuration (0x%x)", eglGetError());
        return false;
    }
    const EGLint context_attributes[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    backend->egl_context = eglCreateContext(backend->egl_display, config, EGL_NO_CONTEXT,
                                            context_attributes);
    if (backend->egl_context == EGL_NO_CONTEXT) {
        PURP_ERROR("Cannot create GLES2 context (0x%x)", eglGetError());
        return false;
    }
    if (backend->nested) {
        backend->egl_window = wl_egl_window_create(backend->surface, backend->width,
                                                   backend->height);
        if (!backend->egl_window) return false;
        backend->egl_surface = eglCreateWindowSurface(backend->egl_display, config,
            (EGLNativeWindowType)backend->egl_window, NULL);
        if (backend->egl_surface == EGL_NO_SURFACE) {
            PURP_ERROR("Cannot create host EGL window surface (0x%x)", eglGetError());
            return false;
        }
    }
    if (!backend_make_current(backend)) {
        PURP_ERROR("Cannot make GLES2 context current (0x%x)", eglGetError());
        return false;
    }
    if (backend->nested) eglSwapInterval(backend->egl_display, 0);
    return true;
}

static bool backend_apply_monitors(void *data, purp_server_t *server) {
    window_backend_t *backend = data;
    if (!backend_make_current(backend)) return false;
    size_t count = purp_host_monitor_count(server);
    for (size_t index = 0; index < PURP_MAX_MONITORS; ++index) {
        purp_monitor_t *monitor = index < count ? purp_host_monitor(server, index) : NULL;
        if (!monitor || !monitor->enabled) {
            purp_host_offscreen_release(server, &backend->targets[index]);
            continue;
        }
        if ((!backend->nested || backend->virtual_outputs) &&
            !purp_host_offscreen_ensure(server, &backend->targets[index],
                monitor->physical_size.w, monitor->physical_size.h)) return false;
        if (backend->nested && !backend->virtual_outputs && index == 0 &&
            (backend->width != monitor->physical_size.w ||
             backend->height != monitor->physical_size.h)) {
            backend->width = monitor->physical_size.w;
            backend->height = monitor->physical_size.h;
            wl_egl_window_resize(backend->egl_window, backend->width, backend->height, 0, 0);
            xdg_surface_set_window_geometry(backend->xdg_surface, 0, 0,
                                             backend->width, backend->height);
        }
    }
    backend->host_dirty = true;
    purp_host_damage_all(server);
    return true;
}

static const purp_backend_ops_t nested_ops = {
    .name = "nested", .allow_custom_modes = true,
    .apply_monitors = backend_apply_monitors, .make_current = backend_make_current,
};
static const purp_backend_ops_t headless_ops = {
    .name = "headless", .allow_custom_modes = true,
    .apply_monitors = backend_apply_monitors, .make_current = backend_make_current,
};

static bool add_monitors(window_backend_t *backend) {
    purp_monitor_desc_t descriptions[PURP_MAX_MONITORS];
    const char *specification = getenv(PURP_ENV_VIRTUAL_MONITORS);
    size_t count = specification ? purp_parse_virtual_monitors(specification,
        descriptions, PURP_MAX_MONITORS) : 0;
    backend->virtual_outputs = count != 0;
    if (!count) {
        purp_monitor_desc_t *description = &descriptions[0];
        memset(description, 0, sizeof(*description));
        snprintf(description->name, sizeof(description->name), "%s",
                 backend->nested ? "purpwm+-eDP-1" : "purpwm+-headless-1");
        snprintf(description->description, sizeof(description->description), "%s (%s Display)",
                 description->name, backend->nested ? "Windowed" : "Headless");
        snprintf(description->make, sizeof(description->make), "%s", PURP_NAME);
        snprintf(description->model, sizeof(description->model), "%s", "display");
        description->physical_size = (purp_size_t){ backend->width, backend->height };
        description->refresh_mhz = 120000;
        description->scale = purp_host_wm(backend->server)->default_scale;
        description->is_virtual = true;
        description->mode_count = purp_default_virtual_modes(description->physical_size,
            description->refresh_mhz, description->modes, PURP_MAX_MODES);
        count = 1;
    }
    for (size_t index = 0; index < count; ++index)
        if (purp_host_monitor_add(backend->server, &descriptions[index]) < 0) return false;
    return true;
}

static purp_pointf_t map_position(window_backend_t *backend, wl_fixed_t x, wl_fixed_t y) {
    purp_wm_t *wm = purp_host_wm(backend->server);
    if (backend->virtual_outputs)
        return purp_wm_touch_virtual_to_logical(wm, wl_fixed_to_double(x),
            wl_fixed_to_double(y), (purp_size_t){ backend->width, backend->height });
    size_t index = purp_wm_primary_monitor_index(wm);
    purp_rect_t rectangle = purp_wm_monitor_rect(wm, index);
    purp_pointf_t position = purp_touch_physical_to_logical(wl_fixed_to_double(x),
        wl_fixed_to_double(y), purp_wm_monitor_scale(wm, index), purp_rect_size(rectangle));
    position.x += rectangle.x;
    position.y += rectangle.y;
    return position;
}

static void release_keys(window_backend_t *backend) {
    uint32_t time_ms = host_time_ms();
    for (uint32_t key = 0; key < KEY_CNT; ++key) {
        if (backend->keys[key] && backend->server)
            purp_host_keyboard_key(backend->server, time_ms, key, false);
        backend->keys[key] = false;
    }
}

static void release_buttons(window_backend_t *backend) {
    uint32_t time_ms = host_time_ms();
    for (uint32_t button = 0; button < KEY_CNT; ++button) {
        if (backend->buttons[button] && backend->server)
            purp_host_pointer_button(backend->server, time_ms, button, false);
        backend->buttons[button] = false;
    }
    memset(&backend->axis, 0, sizeof(backend->axis));
    if (backend->server) purp_host_pointer_frame(backend->server);
}

static void keyboard_keymap(void *data, struct wl_keyboard *keyboard, uint32_t format,
                            int32_t fd, uint32_t size) {
    /* The core owns its xkb keymap and consumes raw evdev codes. */
    close(fd);
}
static void keyboard_enter(void *data, struct wl_keyboard *keyboard, uint32_t serial,
                           struct wl_surface *surface, struct wl_array *keys) {
    /* Enter's key list describes pre-existing state, not new key presses. */
}
static void keyboard_leave(void *data, struct wl_keyboard *keyboard, uint32_t serial,
                           struct wl_surface *surface) {
    release_keys(data);
}
static void keyboard_key(void *data, struct wl_keyboard *keyboard, uint32_t serial,
                         uint32_t time_ms, uint32_t key, uint32_t state) {
    window_backend_t *backend = data;
    if (!backend->server) return;
    bool pressed = state == WL_KEYBOARD_KEY_STATE_PRESSED;
    if (key < KEY_CNT) backend->keys[key] = pressed;
    purp_host_keyboard_key(backend->server, time_ms, key, pressed);
}
static void keyboard_modifiers(void *data, struct wl_keyboard *keyboard, uint32_t serial,
                               uint32_t depressed, uint32_t latched, uint32_t locked,
                               uint32_t group) {
    /* Modifier state is derived from raw keys by the core, never from the host keymap. */
}
static void keyboard_repeat(void *data, struct wl_keyboard *keyboard,
                           int32_t rate, int32_t delay) {
    /* The core advertises its configured repeat policy to its clients. */
}
static const struct wl_keyboard_listener keyboard_listener = {
    .keymap = keyboard_keymap, .enter = keyboard_enter, .leave = keyboard_leave,
    .key = keyboard_key, .modifiers = keyboard_modifiers, .repeat_info = keyboard_repeat,
};

static void pointer_motion(void *data, struct wl_pointer *pointer, uint32_t time_ms,
                           wl_fixed_t x, wl_fixed_t y) {
    window_backend_t *backend = data;
    if (!backend->server || !purp_host_monitor_count(backend->server)) return;
    purp_pointf_t position = map_position(backend, x, y);
    purp_host_pointer_motion_absolute(backend->server, time_ms, position.x, position.y);
    if (wl_pointer_get_version(pointer) < WL_POINTER_FRAME_SINCE_VERSION)
        purp_host_pointer_frame(backend->server);
}
static void pointer_enter(void *data, struct wl_pointer *pointer, uint32_t serial,
                          struct wl_surface *surface, wl_fixed_t x, wl_fixed_t y) {
    wl_pointer_set_cursor(pointer, serial, NULL, 0, 0);
    pointer_motion(data, pointer, host_time_ms(), x, y);
}
static void pointer_leave(void *data, struct wl_pointer *pointer, uint32_t serial,
                          struct wl_surface *surface) {
    release_buttons(data);
}
static void pointer_button(void *data, struct wl_pointer *pointer, uint32_t serial,
                           uint32_t time_ms, uint32_t button, uint32_t state) {
    window_backend_t *backend = data;
    if (!backend->server) return;
    bool pressed = state == WL_POINTER_BUTTON_STATE_PRESSED;
    if (button < KEY_CNT) backend->buttons[button] = pressed;
    purp_host_pointer_button(backend->server, time_ms, button, pressed);
    if (wl_pointer_get_version(pointer) < WL_POINTER_FRAME_SINCE_VERSION)
        purp_host_pointer_frame(backend->server);
}
static void pointer_frame(void *data, struct wl_pointer *pointer) {
    window_backend_t *backend = data;
    if (backend->server) {
        /* The public event has one inversion bit; emit each axis separately if needed. */
        if (backend->axis.has_vertical) {
            purp_axis_event_t event = backend->axis;
            event.has_horizontal = false;
            event.horizontal = 0;
            event.horizontal_v120 = 0;
            event.inverted = backend->axis_inverted[WL_POINTER_AXIS_VERTICAL_SCROLL];
            purp_host_pointer_axis(backend->server, &event);
        }
        if (backend->axis.has_horizontal) {
            purp_axis_event_t event = backend->axis;
            event.has_vertical = false;
            event.vertical = 0;
            event.vertical_v120 = 0;
            event.inverted = backend->axis_inverted[WL_POINTER_AXIS_HORIZONTAL_SCROLL];
            purp_host_pointer_axis(backend->server, &event);
        }
        purp_host_pointer_frame(backend->server);
    }
    memset(&backend->axis, 0, sizeof(backend->axis));
    memset(backend->axis_inverted, 0, sizeof(backend->axis_inverted));
}
static void pointer_axis(void *data, struct wl_pointer *pointer, uint32_t time_ms,
                         uint32_t axis, wl_fixed_t value) {
    window_backend_t *backend = data;
    backend->axis.time_ms = time_ms;
    if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL) {
        backend->axis.has_vertical = true;
        backend->axis.vertical += wl_fixed_to_double(value);
    } else if (axis == WL_POINTER_AXIS_HORIZONTAL_SCROLL) {
        backend->axis.has_horizontal = true;
        backend->axis.horizontal += wl_fixed_to_double(value);
    }
    if (wl_pointer_get_version(pointer) < WL_POINTER_FRAME_SINCE_VERSION)
        pointer_frame(data, pointer);
}
static void pointer_axis_source(void *data, struct wl_pointer *pointer, uint32_t source) {
    window_backend_t *backend = data;
    switch (source) {
    case WL_POINTER_AXIS_SOURCE_FINGER: backend->axis.source = PURP_AXIS_SOURCE_FINGER; break;
    case WL_POINTER_AXIS_SOURCE_CONTINUOUS:
        backend->axis.source = PURP_AXIS_SOURCE_CONTINUOUS; break;
    case WL_POINTER_AXIS_SOURCE_WHEEL_TILT:
        backend->axis.source = PURP_AXIS_SOURCE_WHEEL_TILT; break;
    default: backend->axis.source = PURP_AXIS_SOURCE_WHEEL; break;
    }
}
static void pointer_axis_stop(void *data, struct wl_pointer *pointer,
                              uint32_t time_ms, uint32_t axis) {
    window_backend_t *backend = data;
    backend->axis.time_ms = time_ms;
    if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL) backend->axis.has_vertical = true;
    if (axis == WL_POINTER_AXIS_HORIZONTAL_SCROLL) backend->axis.has_horizontal = true;
}
static void pointer_axis_value120(void *data, struct wl_pointer *pointer,
                                  uint32_t axis, int32_t value) {
    window_backend_t *backend = data;
    if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL) {
        backend->axis.has_vertical = true;
        backend->axis.vertical_v120 += value;
    } else if (axis == WL_POINTER_AXIS_HORIZONTAL_SCROLL) {
        backend->axis.has_horizontal = true;
        backend->axis.horizontal_v120 += value;
    }
}
static void pointer_axis_discrete(void *data, struct wl_pointer *pointer,
                                  uint32_t axis, int32_t discrete) {
    int64_t value = (int64_t)discrete * 120;
    pointer_axis_value120(data, pointer, axis,
        (int32_t)(value < INT32_MIN ? INT32_MIN : value > INT32_MAX ? INT32_MAX : value));
}
#ifdef WL_POINTER_AXIS_RELATIVE_DIRECTION_SINCE_VERSION
static void pointer_axis_direction(void *data, struct wl_pointer *pointer,
                                   uint32_t axis, uint32_t direction) {
    window_backend_t *backend = data;
    if (axis < PURP_ARRAY_LEN(backend->axis_inverted))
        backend->axis_inverted[axis] = direction == WL_POINTER_AXIS_RELATIVE_DIRECTION_INVERTED;
}
#endif
static const struct wl_pointer_listener pointer_listener = {
    .enter = pointer_enter, .leave = pointer_leave, .motion = pointer_motion,
    .button = pointer_button, .axis = pointer_axis, .frame = pointer_frame,
    .axis_source = pointer_axis_source, .axis_stop = pointer_axis_stop,
    .axis_discrete = pointer_axis_discrete,
#ifdef WL_POINTER_AXIS_VALUE120_SINCE_VERSION
    .axis_value120 = pointer_axis_value120,
#endif
#ifdef WL_POINTER_AXIS_RELATIVE_DIRECTION_SINCE_VERSION
    .axis_relative_direction = pointer_axis_direction,
#endif
};

static int touch_slot(window_backend_t *backend, int32_t id) {
    for (int slot = 0; slot < PURP_MAX_TOUCH_POINTS; ++slot)
        if (backend->touches[slot].active && backend->touches[slot].id == id) return slot;
    return -1;
}
static void touch_down(void *data, struct wl_touch *touch, uint32_t serial, uint32_t time_ms,
                       struct wl_surface *surface, int32_t id, wl_fixed_t x, wl_fixed_t y) {
    window_backend_t *backend = data;
    if (!backend->server || !purp_host_monitor_count(backend->server)) return;
    purp_wm_t *wm = purp_host_wm(backend->server);
    purp_pointf_t position = map_position(backend, x, y);
    size_t monitor = purp_wm_nearest_enabled_monitor(wm, position);
    if (!wm->monitors[monitor].enabled) return;
    for (int slot = 0; slot < PURP_MAX_TOUCH_POINTS; ++slot) {
        if (backend->touches[slot].active) continue;
        backend->touches[slot].active = true;
        backend->touches[slot].id = id;
        snprintf(backend->touches[slot].monitor, sizeof(backend->touches[slot].monitor),
                 "%s", wm->monitors[monitor].name);
        purp_host_touch_down(backend->server, time_ms, slot, position.x, position.y);
        return;
    }
}
static void touch_up(void *data, struct wl_touch *touch, uint32_t serial,
                     uint32_t time_ms, int32_t id) {
    window_backend_t *backend = data;
    int slot = touch_slot(backend, id);
    if (slot < 0) return;
    purp_host_touch_up(backend->server, time_ms, slot);
    backend->touches[slot].active = false;
}
static void touch_motion(void *data, struct wl_touch *touch, uint32_t time_ms,
                         int32_t id, wl_fixed_t x, wl_fixed_t y) {
    window_backend_t *backend = data;
    int slot = touch_slot(backend, id);
    if (slot < 0) return;
    purp_wm_t *wm = purp_host_wm(backend->server);
    int monitor = purp_wm_find_monitor(wm, backend->touches[slot].monitor);
    if (monitor < 0 || !wm->monitors[monitor].enabled) {
        purp_host_touch_cancel(backend->server, slot);
        backend->touches[slot].active = false;
        return;
    }
    purp_pointf_t position = map_position(backend, x, y);
    purp_rect_t rectangle = purp_wm_monitor_rect(wm, (size_t)monitor);
    position.x = purp_clamp_f64(position.x, rectangle.x, (double)rectangle.x + rectangle.w - 1e-4);
    position.y = purp_clamp_f64(position.y, rectangle.y, (double)rectangle.y + rectangle.h - 1e-4);
    purp_host_touch_motion(backend->server, time_ms, slot, position.x, position.y);
}
static void touch_frame(void *data, struct wl_touch *touch) {
    window_backend_t *backend = data;
    if (backend->server) purp_host_touch_frame(backend->server);
}
static void touch_cancel(void *data, struct wl_touch *touch) {
    window_backend_t *backend = data;
    for (int slot = 0; slot < PURP_MAX_TOUCH_POINTS; ++slot) {
        if (backend->touches[slot].active && backend->server)
            purp_host_touch_cancel(backend->server, slot);
        backend->touches[slot].active = false;
    }
}
static void touch_shape(void *data, struct wl_touch *touch, int32_t id,
                        wl_fixed_t major, wl_fixed_t minor) {
    /* The core's touch contract has no shape metadata. */
}
static void touch_orientation(void *data, struct wl_touch *touch, int32_t id,
                              wl_fixed_t orientation) {
}
static const struct wl_touch_listener touch_listener = {
    .down = touch_down, .up = touch_up, .motion = touch_motion,
    .frame = touch_frame, .cancel = touch_cancel,
    .shape = touch_shape, .orientation = touch_orientation,
};

static void destroy_keyboard(window_backend_t *backend) {
    if (!backend->keyboard) return;
    release_keys(backend);
    if (wl_keyboard_get_version(backend->keyboard) >= WL_KEYBOARD_RELEASE_SINCE_VERSION)
        wl_keyboard_release(backend->keyboard);
    else wl_keyboard_destroy(backend->keyboard);
    backend->keyboard = NULL;
}
static void destroy_pointer(window_backend_t *backend) {
    if (!backend->pointer) return;
    release_buttons(backend);
    if (wl_pointer_get_version(backend->pointer) >= WL_POINTER_RELEASE_SINCE_VERSION)
        wl_pointer_release(backend->pointer);
    else wl_pointer_destroy(backend->pointer);
    backend->pointer = NULL;
}
static void destroy_touch(window_backend_t *backend) {
    if (!backend->touch) return;
    touch_cancel(backend, backend->touch);
    if (wl_touch_get_version(backend->touch) >= WL_TOUCH_RELEASE_SINCE_VERSION)
        wl_touch_release(backend->touch);
    else wl_touch_destroy(backend->touch);
    backend->touch = NULL;
}
static void destroy_seat(window_backend_t *backend) {
    destroy_keyboard(backend);
    destroy_pointer(backend);
    destroy_touch(backend);
    if (!backend->seat) return;
    if (wl_seat_get_version(backend->seat) >= WL_SEAT_RELEASE_SINCE_VERSION)
        wl_seat_release(backend->seat);
    else wl_seat_destroy(backend->seat);
    backend->seat = NULL;
}
static void seat_capabilities(void *data, struct wl_seat *seat, uint32_t capabilities) {
    window_backend_t *backend = data;
    if ((capabilities & WL_SEAT_CAPABILITY_KEYBOARD) && !backend->keyboard) {
        backend->keyboard = wl_seat_get_keyboard(seat);
        if (backend->keyboard)
            wl_keyboard_add_listener(backend->keyboard, &keyboard_listener, backend);
    } else if (!(capabilities & WL_SEAT_CAPABILITY_KEYBOARD)) destroy_keyboard(backend);
    if ((capabilities & WL_SEAT_CAPABILITY_POINTER) && !backend->pointer) {
        backend->pointer = wl_seat_get_pointer(seat);
        if (backend->pointer)
            wl_pointer_add_listener(backend->pointer, &pointer_listener, backend);
    } else if (!(capabilities & WL_SEAT_CAPABILITY_POINTER)) destroy_pointer(backend);
    if ((capabilities & WL_SEAT_CAPABILITY_TOUCH) && !backend->touch) {
        backend->touch = wl_seat_get_touch(seat);
        if (backend->touch) wl_touch_add_listener(backend->touch, &touch_listener, backend);
    } else if (!(capabilities & WL_SEAT_CAPABILITY_TOUCH)) destroy_touch(backend);
}
static void seat_name(void *data, struct wl_seat *seat, const char *name) {
}
static const struct wl_seat_listener seat_listener = {
    .capabilities = seat_capabilities, .name = seat_name,
};
static void wm_ping(void *data, struct xdg_wm_base *wm_base, uint32_t serial) {
    xdg_wm_base_pong(wm_base, serial);
}
static const struct xdg_wm_base_listener wm_listener = { .ping = wm_ping };

static void registry_global(void *data, struct wl_registry *registry, uint32_t name,
                            const char *interface, uint32_t version) {
    window_backend_t *backend = data;
    if (strcmp(interface, wl_compositor_interface.name) == 0 && !backend->compositor) {
        backend->compositor = wl_registry_bind(registry, name, &wl_compositor_interface,
                                                version < 4 ? version : 4);
        backend->compositor_name = name;
    } else if (strcmp(interface, xdg_wm_base_interface.name) == 0 && !backend->wm_base) {
        backend->wm_base = wl_registry_bind(registry, name, &xdg_wm_base_interface,
                                             version < 3 ? version : 3);
        backend->wm_base_name = name;
        if (backend->wm_base) xdg_wm_base_add_listener(backend->wm_base, &wm_listener, backend);
    } else if (strcmp(interface, wl_seat_interface.name) == 0 && !backend->seat) {
        /* Version 9 includes high-resolution axes and their physical direction, but not
         * host-generated key repeats (v10) or pointer warps (v11). */
        uint32_t supported = 8;
#ifdef WL_POINTER_AXIS_RELATIVE_DIRECTION_SINCE_VERSION
        supported = 9;
#endif
        backend->seat = wl_registry_bind(registry, name, &wl_seat_interface,
                                          version < supported ? version : supported);
        backend->seat_name = name;
        if (backend->seat) wl_seat_add_listener(backend->seat, &seat_listener, backend);
    }
}
static void registry_remove(void *data, struct wl_registry *registry, uint32_t name) {
    window_backend_t *backend = data;
    if (name == backend->seat_name) destroy_seat(backend);
    if (name == backend->compositor_name || name == backend->wm_base_name) {
        backend->closed = true;
        if (backend->server) purp_host_request_quit(backend->server);
    }
}
static const struct wl_registry_listener registry_listener = {
    .global = registry_global, .global_remove = registry_remove,
};
static void surface_configure(void *data, struct xdg_surface *surface, uint32_t serial) {
    window_backend_t *backend = data;
    xdg_surface_ack_configure(surface, serial);
    if (backend->pending_width > 0) backend->width = backend->pending_width;
    if (backend->pending_height > 0) backend->height = backend->pending_height;
    backend->configured = true;
    backend->host_dirty = true;
    if (backend->egl_window)
        wl_egl_window_resize(backend->egl_window, backend->width, backend->height, 0, 0);
    xdg_surface_set_window_geometry(surface, 0, 0, backend->width, backend->height);
    if (backend->server && !backend->virtual_outputs && purp_host_monitor_count(backend->server))
        purp_host_monitor_set_physical(backend->server, 0, backend->width, backend->height, 120000);
}
static const struct xdg_surface_listener surface_listener = { .configure = surface_configure };
static void toplevel_configure(void *data, struct xdg_toplevel *toplevel,
                               int32_t width, int32_t height, struct wl_array *states) {
    window_backend_t *backend = data;
    backend->pending_width = width;
    backend->pending_height = height;
}
static void toplevel_close(void *data, struct xdg_toplevel *toplevel) {
    window_backend_t *backend = data;
    backend->closed = true;
    if (backend->server) purp_host_request_quit(backend->server);
}
static const struct xdg_toplevel_listener toplevel_listener = {
    .configure = toplevel_configure, .close = toplevel_close,
};
static void frame_done(void *data, struct wl_callback *callback, uint32_t time_ms) {
    window_backend_t *backend = data;
    wl_callback_destroy(callback);
    backend->frame = NULL;
}
static const struct wl_callback_listener frame_listener = { .done = frame_done };

static int host_writable(int fd, uint32_t mask, void *data);

static bool flush_host(window_backend_t *backend) {
    if (wl_display_flush(backend->display) >= 0) {
        if (backend->write_source) {
            purp_host_remove_source(backend->write_source);
            backend->write_source = NULL;
        }
        return true;
    }
    if (errno == EAGAIN) {
        if (!backend->write_source) {
            backend->write_source = purp_host_add_fd(backend->server,
                wl_display_get_fd(backend->display), PURP_FD_WRITABLE,
                host_writable, backend);
            if (!backend->write_source) {
                backend_fail(backend, "Registering host write readiness");
                return false;
            }
        }
        return true;
    }
    backend_fail(backend, "Flushing host Wayland connection");
    return false;
}
static int host_writable(int fd, uint32_t mask, void *data) {
    window_backend_t *backend = data;
    if (mask & (PURP_FD_HANGUP | PURP_FD_ERROR)) {
        errno = EPIPE;
        backend_fail(backend, "Host Wayland connection closed");
    } else flush_host(backend);
    return 0;
}
static int host_readable(int fd, uint32_t mask, void *data) {
    window_backend_t *backend = data;
    if (mask & (PURP_FD_HANGUP | PURP_FD_ERROR)) {
        errno = EPIPE;
        backend_fail(backend, "Host Wayland connection closed");
        return 0;
    }
    if ((mask & PURP_FD_READABLE) && backend->read_prepared) {
        backend->read_prepared = false;
        if (wl_display_read_events(backend->display) < 0) {
            backend_fail(backend, "Reading host Wayland events");
            return 0;
        }
        if (wl_display_dispatch_pending(backend->display) < 0)
            backend_fail(backend, "Dispatching host Wayland events");
    }
    return 0;
}
static int host_timer(void *data) {
    window_backend_t *backend = data;
    purp_host_timer_update(backend->timer_source, PURP_TICK_INTERVAL_MS);
    return 0;
}
static bool prepare_host_read(window_backend_t *backend) {
    while (wl_display_prepare_read(backend->display) < 0) {
        if (wl_display_dispatch_pending(backend->display) < 0) {
            backend_fail(backend, "Dispatching pending host events");
            return false;
        }
    }
    backend->read_prepared = true;
    return flush_host(backend);
}
static void cancel_host_read(window_backend_t *backend) {
    if (!backend->read_prepared) return;
    wl_display_cancel_read(backend->display);
    backend->read_prepared = false;
}

static bool render_nested(window_backend_t *backend) {
    if (backend->frame || !backend->configured) return true;
    bool dirty = backend->host_dirty;
    size_t count = purp_host_monitor_count(backend->server);
    for (size_t index = 0; index < count; ++index) {
        purp_monitor_t *monitor = purp_host_monitor(backend->server, index);
        if (monitor->enabled && purp_host_monitor_needs_frame(backend->server, index)) dirty = true;
    }
    if (!dirty) return true;
    if (!backend_make_current(backend)) return false;
    bool presented[PURP_MAX_MONITORS] = { false };
    if (backend->virtual_outputs) {
        for (size_t index = 0; index < count; ++index) {
            purp_monitor_t *monitor = purp_host_monitor(backend->server, index);
            if (!monitor->enabled) continue;
            purp_offscreen_t *target = &backend->targets[index];
            bool fresh = !target->texture || target->width != monitor->physical_size.w ||
                         target->height != monitor->physical_size.h;
            if (!purp_host_offscreen_ensure(backend->server, target,
                monitor->physical_size.w, monitor->physical_size.h)) return false;
            if (fresh || purp_host_monitor_needs_frame(backend->server, index)) {
                purp_host_offscreen_bind(target);
                if (!purp_host_render_monitor(backend->server, index, target->width,
                                               target->height, false)) return false;
            }
            presented[index] = true;
        }
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        purp_host_render_begin(backend->server, backend->width, backend->height, true,
                               PURP_COLOR_HOST_CLEAR);
        purp_rect_t bounds = purp_wm_desktop_bounds(purp_host_wm(backend->server));
        double scale_x = (double)backend->width / bounds.w;
        double scale_y = (double)backend->height / bounds.h;
        for (size_t index = 0; index < count; ++index) {
            purp_monitor_t *monitor = purp_host_monitor(backend->server, index);
            if (!monitor->enabled) continue;
            purp_offscreen_t *target = &backend->targets[index];
            purp_rect_t rectangle = purp_monitor_rect(monitor);
            int32_t left = purp_round_i32(((double)rectangle.x - bounds.x) * scale_x);
            int32_t top = purp_round_i32(((double)rectangle.y - bounds.y) * scale_y);
            int32_t right = purp_round_i32(((double)rectangle.x - bounds.x + rectangle.w) * scale_x);
            int32_t bottom = purp_round_i32(((double)rectangle.y - bounds.y + rectangle.h) * scale_y);
            purp_host_render_texture(backend->server, target->texture, target->width,
                target->height, purp_rect_make(0, 0, target->width, target->height),
                purp_rect_make(left, top, right - left, bottom - top), 1.0f);
        }
        purp_host_render_end(backend->server);
    } else {
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        purp_monitor_t *monitor = count ? purp_host_monitor(backend->server, 0) : NULL;
        if (monitor && monitor->enabled) {
            if (!purp_host_render_monitor(backend->server, 0, backend->width,
                                           backend->height, true)) return false;
            presented[0] = true;
        } else {
            purp_host_render_begin(backend->server, backend->width, backend->height, true,
                                   PURP_COLOR_HOST_CLEAR);
            purp_host_render_end(backend->server);
        }
    }
    backend->frame = wl_surface_frame(backend->surface);
    if (!backend->frame || wl_callback_add_listener(backend->frame, &frame_listener, backend) < 0)
        return false;
    if (!eglSwapBuffers(backend->egl_display, backend->egl_surface)) return false;
    backend->host_dirty = false;
    for (size_t index = 0; index < count; ++index)
        if (presented[index]) purp_host_monitor_frame_presented(backend->server, index);
    return flush_host(backend);
}

static bool render_headless(window_backend_t *backend) {
    bool presented[PURP_MAX_MONITORS] = { false };
    bool rendered = false;
    if (!backend_make_current(backend)) return false;
    size_t count = purp_host_monitor_count(backend->server);
    for (size_t index = 0; index < count; ++index) {
        purp_monitor_t *monitor = purp_host_monitor(backend->server, index);
        if (!monitor->enabled || !purp_host_monitor_needs_frame(backend->server, index)) continue;
        purp_offscreen_t *target = &backend->targets[index];
        if (!purp_host_offscreen_ensure(backend->server, target,
            monitor->physical_size.w, monitor->physical_size.h)) return false;
        purp_host_offscreen_bind(target);
        if (!purp_host_render_monitor(backend->server, index, target->width,
                                       target->height, false)) return false;
        presented[index] = true;
        rendered = true;
    }
    if (rendered) glFlush();
    for (size_t index = 0; index < count; ++index)
        if (presented[index]) purp_host_monitor_frame_presented(backend->server, index);
    return true;
}

static void cleanup_backend(window_backend_t *backend) {
    if (backend->display) cancel_host_read(backend);
    if (backend->display_source) purp_host_remove_source(backend->display_source);
    if (backend->write_source) purp_host_remove_source(backend->write_source);
    if (backend->timer_source) purp_host_remove_source(backend->timer_source);
    backend->display_source = NULL;
    backend->write_source = NULL;
    backend->timer_source = NULL;
    destroy_seat(backend);
    if (backend->server) {
        if (backend->renderer_ready) {
            backend_make_current(backend);
            for (size_t index = 0; index < PURP_MAX_MONITORS; ++index)
                purp_host_offscreen_release(backend->server, &backend->targets[index]);
            purp_host_renderer_finish(backend->server);
        }
        purp_host_server_destroy(backend->server);
        backend->server = NULL;
    }
    if (backend->egl_display != EGL_NO_DISPLAY) {
        eglMakeCurrent(backend->egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (backend->egl_surface != EGL_NO_SURFACE)
            eglDestroySurface(backend->egl_display, backend->egl_surface);
        if (backend->egl_context != EGL_NO_CONTEXT)
            eglDestroyContext(backend->egl_display, backend->egl_context);
        eglTerminate(backend->egl_display);
    }
    if (backend->egl_window) wl_egl_window_destroy(backend->egl_window);
    if (backend->frame) wl_callback_destroy(backend->frame);
    if (backend->toplevel) xdg_toplevel_destroy(backend->toplevel);
    if (backend->xdg_surface) xdg_surface_destroy(backend->xdg_surface);
    if (backend->surface) wl_surface_destroy(backend->surface);
    if (backend->wm_base) xdg_wm_base_destroy(backend->wm_base);
    if (backend->compositor) wl_compositor_destroy(backend->compositor);
    if (backend->registry) wl_registry_destroy(backend->registry);
    if (backend->display) wl_display_disconnect(backend->display);
    if (backend->gbm) gbm_device_destroy(backend->gbm);
    if (backend->render_fd >= 0) close(backend->render_fd);
    eglReleaseThread();
}

int purp_backend_nested_run(const purp_options_t *opts) {
    window_backend_t backend = {
        .egl_display = EGL_NO_DISPLAY, .egl_context = EGL_NO_CONTEXT,
        .egl_surface = EGL_NO_SURFACE, .render_fd = -1, .nested = true,
        .host_dirty = true,
    };
    int result = EXIT_FAILURE;
    /* Capture the host connection before server_create exports the child socket name. */
    backend.display = wl_display_connect(NULL);
    if (!backend.display) {
        PURP_ERROR("Cannot connect to the host Wayland display: %s. %s"
                   "Use --headless for offscreen outputs or --drm for a native session.",
                   strerror(errno), getenv("DISPLAY") ? "" : "DISPLAY is unset. ");
        goto out;
    }
    uint32_t native_width, native_height;
    purp_detect_preferred_resolution(&native_width, &native_height);
    backend.width = opts->standalone ? (int32_t)native_width :
        purp_max_i32((int32_t)(native_width * 3u / 4u), 1440);
    backend.height = opts->standalone ? (int32_t)native_height :
        purp_max_i32((int32_t)(native_height * 3u / 4u), 900);
    backend.registry = wl_display_get_registry(backend.display);
    if (!backend.registry ||
        wl_registry_add_listener(backend.registry, &registry_listener, &backend) < 0 ||
        wl_display_roundtrip(backend.display) < 0) goto out;
    if (!backend.compositor || !backend.wm_base) {
        PURP_ERROR("Host display lacks wl_compositor or xdg_wm_base");
        goto out;
    }
    backend.surface = wl_compositor_create_surface(backend.compositor);
    if (!backend.surface) goto out;
    backend.xdg_surface = xdg_wm_base_get_xdg_surface(backend.wm_base, backend.surface);
    if (!backend.xdg_surface || xdg_surface_add_listener(backend.xdg_surface,
        &surface_listener, &backend) < 0) goto out;
    backend.toplevel = xdg_surface_get_toplevel(backend.xdg_surface);
    if (!backend.toplevel || xdg_toplevel_add_listener(backend.toplevel,
        &toplevel_listener, &backend) < 0) goto out;
    xdg_toplevel_set_title(backend.toplevel, PURP_NAME);
    xdg_toplevel_set_app_id(backend.toplevel, PURP_NAME);
    if (opts->standalone) xdg_toplevel_set_fullscreen(backend.toplevel, NULL);
    wl_surface_commit(backend.surface);
    while (!backend.configured && !backend.closed)
        if (wl_display_dispatch(backend.display) < 0) goto out;
    if (backend.closed) goto out;
    backend.server = purp_host_server_create(opts);
    if (!backend.server || !initialize_egl(&backend)) goto out;
    purp_host_set_backend(backend.server, &nested_ops, &backend);
    backend.renderer_ready = purp_host_renderer_init(backend.server, backend.egl_display,
        backend.egl_context, backend_render_node(&backend));
    if (!backend.renderer_ready || !add_monitors(&backend)) goto out;
    purp_host_load_saved_monitors(backend.server);
    if (!backend_apply_monitors(&backend, backend.server)) goto out;
    backend.display_source = purp_host_add_fd(backend.server, wl_display_get_fd(backend.display),
        PURP_FD_READABLE, host_readable, &backend);
    backend.timer_source = purp_host_add_timer(backend.server, host_timer, &backend);
    if (!backend.display_source || !backend.timer_source) goto out;
    purp_host_timer_update(backend.timer_source, PURP_TICK_INTERVAL_MS);
    if (!purp_host_start(backend.server)) goto out;
    while (!purp_host_should_quit(backend.server)) {
        /* Drain without blocking; only the core loop waits. Every prepared read is either
         * consumed by its readable callback or cancelled before EGL/core rendering. */
        if (!prepare_host_read(&backend)) break;
        int dispatched = purp_host_dispatch(backend.server, PURP_TICK_INTERVAL_MS);
        cancel_host_read(&backend);
        if (dispatched < 0 && errno != EINTR) {
            backend_fail(&backend, "Dispatching compositor events");
            break;
        }
        if (backend.failed || backend.closed) break;
        purp_host_tick(backend.server);
        if (purp_host_should_quit(backend.server)) break;
        if (!render_nested(&backend)) {
            PURP_ERROR("Nested presentation failed (EGL 0x%x)", eglGetError());
            backend.failed = true;
            break;
        }
        purp_host_flush_clients(backend.server);
        if (!flush_host(&backend)) break;
    }
    result = backend.failed ? EXIT_FAILURE : EXIT_SUCCESS;
out:
    cleanup_backend(&backend);
    return result;
}

int purp_backend_headless_run(const purp_options_t *opts) {
    window_backend_t backend = {
        .egl_display = EGL_NO_DISPLAY, .egl_context = EGL_NO_CONTEXT,
        .egl_surface = EGL_NO_SURFACE, .render_fd = -1,
        .width = 1600, .height = 1000,
    };
    int result = EXIT_FAILURE;
    backend.server = purp_host_server_create(opts);
    if (!backend.server || !initialize_egl(&backend)) goto out;
    purp_host_set_backend(backend.server, &headless_ops, &backend);
    backend.renderer_ready = purp_host_renderer_init(backend.server, backend.egl_display,
        backend.egl_context, backend_render_node(&backend));
    if (!backend.renderer_ready || !add_monitors(&backend)) goto out;
    purp_host_load_saved_monitors(backend.server);
    if (!backend_apply_monitors(&backend, backend.server) || !purp_host_start(backend.server))
        goto out;
    while (!purp_host_should_quit(backend.server)) {
        if (purp_host_dispatch(backend.server, PURP_TICK_INTERVAL_MS) < 0 && errno != EINTR) {
            backend_fail(&backend, "Dispatching headless compositor events");
            break;
        }
        purp_host_tick(backend.server);
        if (purp_host_should_quit(backend.server)) break;
        if (!render_headless(&backend)) {
            PURP_ERROR("Headless rendering failed (EGL 0x%x)", eglGetError());
            backend.failed = true;
            break;
        }
        purp_host_flush_clients(backend.server);
    }
    result = backend.failed ? EXIT_FAILURE : EXIT_SUCCESS;
out:
    cleanup_backend(&backend);
    return result;
}
