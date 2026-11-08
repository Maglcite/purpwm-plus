/*
 * purpwm+ — compositor core (server side).
 *
 * The Wayland server: protocol objects, the seat, outputs, input routing and session services.
 * This header is server-side only: it includes <wayland-server-core.h> and the generated
 * *-server-protocol.h headers, so it must NEVER be included next to <wayland-client.h>
 * (src/backend_wayland.c uses host_backend.h instead).
 *
 * Modules and the functions they own (prefix -> file):
 *   purp_server_*, purp_child_*                src/server.c   lifecycle, globals, monitors,
 *                                                             spawning, screenshots, lock,
 *                                                             scale, IPC dispatch, maintenance
 *   purp_region_*, purp_buffer_*, purp_surface_*,
 *   purp_subsurface_*, purp_dmabuf_*           src/surface.c  wl_compositor v6, wl_subcompositor,
 *                                                             wl_shm, single-pixel buffers,
 *                                                             viewporter, fractional scale,
 *                                                             linux-dmabuf v4
 *   purp_xdg_*, purp_positioner_*              src/xdg_shell.c xdg_wm_base, toplevels, popups,
 *                                                             decorations, xdg_activation_v1
 *   purp_layer_*                               src/layer_shell.c zwlr_layer_shell_v1
 *   purp_seat_*, purp_output_*, purp_data_*    src/seat_data.c wl_seat, cursor-shape, tablet stub,
 *                                                             wl_output v4 + xdg-output, clipboard,
 *                                                             DnD, primary selection,
 *                                                             data-control, idle-inhibit
 *   purp_input_*  (+ purp_host_* input entry points)  src/input.c  bindings, focus, grabs, touch
 * Backend-facing purp_host_* functions are declared in host_backend.h; the renderer in render.h.
 *
 * Threading: everything here runs on the compositor thread. Helper threads (screenshot encoder,
 * clipboard writers, malloc_trim) only touch purp_blob_t data and the mutex-guarded fields named
 * below.
 */
#ifndef PURPWM_PURPWM_H
#define PURPWM_PURPWM_H

#include <limits.h>
#include <pthread.h>
#include <sys/types.h>

#include <wayland-server-core.h>
#include <wayland-server-protocol.h>
#include <xkbcommon/xkbcommon.h>

#include "cursor-shape-v1-server-protocol.h"
#include "fractional-scale-v1-server-protocol.h"
#include "idle-inhibit-unstable-v1-server-protocol.h"
#include "linux-dmabuf-v1-server-protocol.h"
#include "primary-selection-unstable-v1-server-protocol.h"
#include "single-pixel-buffer-v1-server-protocol.h"
#include "tablet-v2-server-protocol.h"
#include "viewporter-server-protocol.h"
#include "wlr-data-control-unstable-v1-server-protocol.h"
#include "wlr-layer-shell-unstable-v1-server-protocol.h"
#include "xdg-activation-v1-server-protocol.h"
#include "xdg-decoration-unstable-v1-server-protocol.h"
#include "xdg-output-unstable-v1-server-protocol.h"
#include "xdg-shell-server-protocol.h"

#include "config.h"
#include "host_backend.h"
#include "ipc.h"
#include "types.h"
#include "ui.h"
#include "wm.h"

/* =========================================================================================== */
/* Protocol versions advertised                                                               */
/* =========================================================================================== */

#define PURP_WL_COMPOSITOR_VERSION         6
#define PURP_WL_SUBCOMPOSITOR_VERSION      1
#define PURP_WL_SHM_VERSION                1   /* via wl_display_init_shm */
#define PURP_WL_SEAT_VERSION               9
#define PURP_WL_OUTPUT_VERSION             4
#define PURP_WL_DATA_DEVICE_MANAGER_VERSION 3
#define PURP_XDG_WM_BASE_VERSION           6
#define PURP_XDG_DECORATION_VERSION        1
#define PURP_XDG_ACTIVATION_VERSION        1
#define PURP_XDG_OUTPUT_MANAGER_VERSION    3
#define PURP_LAYER_SHELL_VERSION           4
#define PURP_LINUX_DMABUF_VERSION          4
#define PURP_VIEWPORTER_VERSION            1
#define PURP_FRACTIONAL_SCALE_VERSION      1
#define PURP_SINGLE_PIXEL_BUFFER_VERSION   1
#define PURP_CURSOR_SHAPE_VERSION          1
#define PURP_TABLET_MANAGER_VERSION        1   /* minimal: tablet seats exist, never any tools */
#define PURP_PRIMARY_SELECTION_VERSION     1
#define PURP_DATA_CONTROL_VERSION          2
#define PURP_IDLE_INHIBIT_VERSION          1

/* =========================================================================================== */
/* Regions (src/surface.c)                                                                    */
/* =========================================================================================== */

/* Disjoint rectangle list. Damage regions collapse to their extents beyond
 * PURP_REGION_MAX_RECTS; input/opaque regions stay exact. Zero-initialise before use. */
#define PURP_REGION_MAX_RECTS 64

typedef struct purp_region {
    purp_rect_t *rects;
    size_t count;
    size_t cap;
} purp_region_t;

void purp_region_init(purp_region_t *region);
void purp_region_finish(purp_region_t *region);
void purp_region_clear(purp_region_t *region);
bool purp_region_copy(purp_region_t *dst, const purp_region_t *src);
bool purp_region_add(purp_region_t *region, purp_rect_t rect);
void purp_region_subtract(purp_region_t *region, purp_rect_t rect);
void purp_region_intersect_rect(purp_region_t *region, purp_rect_t rect);
void purp_region_translate(purp_region_t *region, int32_t dx, int32_t dy);
bool purp_region_contains(const purp_region_t *region, int32_t x, int32_t y);
bool purp_region_is_empty(const purp_region_t *region);
purp_rect_t purp_region_extents(const purp_region_t *region);
/* Collapse to the bounding box (damage accumulation). */
void purp_region_collapse(purp_region_t *region);

/* =========================================================================================== */
/* Buffers (src/surface.c)                                                                    */
/* =========================================================================================== */

typedef enum purp_buffer_kind {
    PURP_BUFFER_SHM = 0,         /* wl_shm: copied into the surface texture at commit */
    PURP_BUFFER_DMABUF,          /* zwp_linux_buffer_params_v1: imported as EGLImage */
    PURP_BUFFER_SINGLE_PIXEL,    /* wp_single_pixel_buffer_v1: drawn as a solid quad */
} purp_buffer_kind_t;

#define PURP_DMABUF_MAX_PLANES 4

typedef struct purp_dmabuf_attributes {
    int32_t width;
    int32_t height;
    uint32_t format;             /* DRM fourcc */
    uint32_t flags;              /* zwp_linux_buffer_params_v1 flags */
    uint64_t modifier;
    int n_planes;
    int fd[PURP_DMABUF_MAX_PLANES];         /* owned; closed when the buffer dies */
    uint32_t offset[PURP_DMABUF_MAX_PLANES];
    uint32_t stride[PURP_DMABUF_MAX_PLANES];
} purp_dmabuf_attributes_t;

/* Format/modifier pair advertised through linux-dmabuf (from the renderer). */
typedef struct purp_dmabuf_format {
    uint32_t format;
    uint64_t modifier;
    bool external_only;          /* importable only as GL_TEXTURE_EXTERNAL_OES */
} purp_dmabuf_format_t;

typedef struct purp_buffer {
    purp_server_t *server;
    struct wl_resource *resource;           /* the wl_buffer; NULL once the client destroyed it */
    purp_buffer_kind_t kind;
    int32_t width;                          /* buffer px */
    int32_t height;
    uint32_t format;                        /* wl_shm format (SHM) or DRM fourcc (DMABUF) */
    bool has_alpha;
    purp_dmabuf_attributes_t dmabuf;        /* DMABUF only */
    float single_pixel[4];                  /* SINGLE_PIXEL: straight RGBA 0..1 */
    size_t lock_count;                      /* surface states referencing it */
    bool release_pending;                   /* send wl_buffer.release when lock_count hits 0 */
    struct purp_texture *texture;           /* render.c import cache (DMABUF, SINGLE_PIXEL) */
    struct wl_listener resource_destroy;
} purp_buffer_t;

/* purp_buffer_t behind any wl_buffer (wl_shm buffers are wrapped on first use). NULL if the
 * resource is not a buffer this compositor understands. */
purp_buffer_t *purp_buffer_from_resource(purp_server_t *server, struct wl_resource *resource);
/* Reference counting by surface states; the last unlock of a committed buffer sends
 * wl_buffer.release, and frees the wrapper once the resource is gone too. */
void purp_buffer_lock(purp_buffer_t *buffer);
void purp_buffer_unlock(purp_buffer_t *buffer);

/* =========================================================================================== */
/* Surfaces (src/surface.c)                                                                   */
/* =========================================================================================== */

typedef struct purp_surface purp_surface_t;
typedef struct purp_subsurface purp_subsurface_t;
typedef struct purp_xdg_surface purp_xdg_surface_t;
typedef struct purp_popup purp_popup_t;
typedef struct purp_layer_surface purp_layer_surface_t;

typedef enum purp_surface_role_kind {
    PURP_ROLE_NONE = 0,
    PURP_ROLE_XDG_TOPLEVEL,
    PURP_ROLE_XDG_POPUP,
    PURP_ROLE_LAYER,
    PURP_ROLE_SUBSURFACE,
    PURP_ROLE_CURSOR,
    PURP_ROLE_DND_ICON,
} purp_surface_role_kind_t;

/* Role implementation; a surface's role never changes once set (protocol rule), but role
 * objects may be destroyed and recreated with the same role. */
typedef struct purp_surface_role {
    const char *name;
    purp_surface_role_kind_t kind;
    /* Called after the pending state became current. */
    void (*commit)(purp_surface_t *surface);
    /* Called before the wl_surface is destroyed (role object must detach). */
    void (*destroy)(purp_surface_t *surface);
} purp_surface_role_t;

/* Which members of a purp_surface_state_t were set since the last commit. */
enum purp_surface_state_field {
    PURP_SURFACE_STATE_BUFFER = 1u << 0,
    PURP_SURFACE_STATE_SURFACE_DAMAGE = 1u << 1,
    PURP_SURFACE_STATE_BUFFER_DAMAGE = 1u << 2,
    PURP_SURFACE_STATE_OPAQUE_REGION = 1u << 3,
    PURP_SURFACE_STATE_INPUT_REGION = 1u << 4,
    PURP_SURFACE_STATE_SCALE = 1u << 5,
    PURP_SURFACE_STATE_TRANSFORM = 1u << 6,
    PURP_SURFACE_STATE_OFFSET = 1u << 7,
    PURP_SURFACE_STATE_VIEWPORT = 1u << 8,
    PURP_SURFACE_STATE_FRAME_CALLBACKS = 1u << 9,
};

/* Double-buffered wl_surface state (pending -> current on commit; cached for synchronized
 * subsurfaces). */
typedef struct purp_surface_state {
    uint32_t committed;                     /* PURP_SURFACE_STATE_* bits */
    purp_buffer_t *buffer;                  /* locked reference, NULL = no buffer */
    int32_t dx;                             /* attach / wl_surface.offset */
    int32_t dy;
    int32_t buffer_scale;                   /* default 1 */
    int32_t buffer_transform;               /* enum wl_output_transform */
    purp_region_t surface_damage;
    purp_region_t buffer_damage;
    purp_region_t opaque;
    purp_region_t input;
    bool input_infinite;                    /* default true (no input region set) */
    struct wl_list frame_callbacks;         /* wl_resource links (wl_callback) */
    bool viewport_has_src;
    purp_rectf_t viewport_src;              /* buffer coords after scale/transform */
    bool viewport_has_dst;
    purp_size_t viewport_dst;
} purp_surface_state_t;

struct purp_surface {
    purp_server_t *server;
    struct wl_resource *resource;
    purp_surface_state_t pending;
    purp_surface_state_t current;
    const purp_surface_role_t *role;        /* NULL until assigned */
    void *role_data;                        /* purp_xdg_surface_t *, purp_layer_surface_t *, ... */
    purp_subsurface_t *subsurface;          /* when role is SUBSURFACE */
    purp_size_t size;                       /* logical size of the current state */
    purp_size_t buffer_size;                /* px of the current buffer (0x0 when none) */
    bool mapped;                            /* current state has a buffer */
    struct purp_texture *texture;           /* render.c: current contents (shm copy or import) */
    uint64_t commit_seq;                    /* increments on every applied commit */
    /* Subsurface stacking, bottom-to-top (purp_subsurface_t.link / .pending_link). */
    struct wl_list subsurfaces_below;
    struct wl_list subsurfaces_above;
    struct wl_list subsurfaces_pending_below;
    struct wl_list subsurfaces_pending_above;
    uint32_t output_mask;                   /* bit i: wl_surface.enter sent for monitor i */
    double preferred_scale;                 /* last wp_fractional_scale_v1 / buffer scale sent */
    struct wl_resource *viewport;           /* wp_viewport, NULL if none */
    struct wl_resource *fractional_scale;   /* wp_fractional_scale_v1, NULL if none */
    struct wl_signal events_commit;         /* data: purp_surface_t * */
    struct wl_signal events_destroy;        /* data: purp_surface_t * */
    struct wl_list link;                    /* purp_server.surfaces */
};

struct purp_subsurface {
    struct wl_resource *resource;
    purp_surface_t *surface;
    purp_surface_t *parent;                 /* NULL once the parent died */
    purp_point_t position;                  /* relative to the parent, applied on parent commit */
    purp_point_t pending_position;
    bool position_pending;
    bool synchronized;                      /* default true */
    bool has_cache;                         /* a synchronized commit is cached */
    purp_surface_state_t cached;
    struct wl_list link;                    /* parent's current below/above list */
    struct wl_list pending_link;            /* parent's pending below/above list */
    struct wl_listener parent_destroy;
};

/* Create the wl_compositor (v6), wl_subcompositor, wl_shm (ARGB8888/XRGB8888 + extras),
 * wp_single_pixel_buffer_manager_v1, wp_viewporter and wp_fractional_scale_manager_v1 globals. */
bool purp_surface_globals_init(purp_server_t *server);
/* Create zwp_linux_dmabuf_v1 v4 with the renderer's formats; with `main_device` != 0 the
 * default/surface feedback (format table memfd + tranche) is served, otherwise only
 * format/modifier events. */
bool purp_dmabuf_init(purp_server_t *server, const purp_dmabuf_format_t *formats, size_t count,
                      dev_t main_device);
void purp_dmabuf_finish(purp_server_t *server);

purp_surface_t *purp_surface_from_resource(struct wl_resource *resource);
/* Assign a role; posts `error_code` on `error_resource` and returns false when the surface
 * already has a different role (or the same role with a live role object). */
bool purp_surface_set_role(purp_surface_t *surface, const purp_surface_role_t *role,
                           void *role_data, struct wl_resource *error_resource,
                           uint32_t error_code);
/* Detach the role object (keeps the role kind for re-assignment of the same role). */
void purp_surface_clear_role_data(purp_surface_t *surface);
/* Topmost ancestor through subsurfaces (the toplevel / popup / layer surface). */
purp_surface_t *purp_surface_root(purp_surface_t *surface);
/* Whether `surface` or one of its subsurfaces has content. */
bool purp_surface_is_mapped(const purp_surface_t *surface);

/* Surface-tree iteration callback: `x`, `y` are the surface's offset from the tree root
 * (logical); return false to stop. */
typedef bool (*purp_surface_iter_func_t)(purp_surface_t *surface, int32_t x, int32_t y,
                                         void *data);
/* Visit mapped surfaces back-to-front (below subsurfaces, the surface, above subsurfaces). */
void purp_surface_for_each(purp_surface_t *surface, purp_surface_iter_func_t func, void *data);
/* Topmost mapped surface of the tree accepting input at root-local (x, y); `*sx`, `*sy` receive
 * surface-local coordinates. */
purp_surface_t *purp_surface_at(purp_surface_t *root, double x, double y, double *sx, double *sy);
/* Whether surface-local (x, y) is inside the surface and its input region. */
bool purp_surface_accepts_input(const purp_surface_t *surface, double x, double y);
/* Send wl_callback.done(time) for the frame callbacks of every surface in the tree. */
void purp_surface_send_frame_done(purp_surface_t *surface, uint32_t time_ms);
/* Set the entered-outputs mask for the whole tree (wl_surface.enter/leave diffs per monitor). */
void purp_surface_set_outputs(purp_surface_t *surface, uint32_t monitor_mask);
/* Preferred scale for the tree: wp_fractional_scale_v1.preferred_scale(round(scale * 120)) and
 * wl_surface.preferred_buffer_scale(ceil(scale)) (v6) when changed. */
void purp_surface_set_preferred_scale(purp_surface_t *surface, double scale);

/* =========================================================================================== */
/* XDG shell (src/xdg_shell.c)                                                                */
/* =========================================================================================== */

typedef enum purp_xdg_role {
    PURP_XDG_ROLE_NONE = 0,
    PURP_XDG_ROLE_TOPLEVEL,
    PURP_XDG_ROLE_POPUP,
} purp_xdg_role_t;

struct purp_xdg_surface {
    purp_server_t *server;
    struct wl_resource *resource;
    struct wl_resource *wm_base;            /* owning xdg_wm_base */
    purp_surface_t *surface;
    purp_xdg_role_t role;
    union {
        purp_toplevel_t *toplevel;
        purp_popup_t *popup;
    };
    bool has_pending_geometry;
    purp_rect_t pending_geometry;
    bool has_geometry;                      /* window geometry (current) */
    purp_rect_t geometry;
    bool initial_commit_done;               /* first commit seen (initial configure owed) */
    bool configured;                        /* a configure was sent */
    uint32_t configure_serial;              /* last configure sent */
    uint32_t acked_serial;                  /* last ack_configure */
    struct wl_event_source *configure_idle; /* coalesced configure sender */
    struct wl_list popups;                  /* purp_popup_t.parent_link */
    struct wl_listener surface_destroy;
    struct wl_list link;                    /* purp_server.xdg_surfaces */
};

struct purp_toplevel {
    purp_xdg_surface_t *xdg;
    struct wl_resource *resource;
    purp_window_t *window;                  /* created at get_toplevel (Rust new_toplevel) */
    purp_toplevel_t *parent;                /* xdg_toplevel.set_parent (cleared on destroy) */
    purp_size_t pending_min_size;           /* double-buffered size hints */
    purp_size_t pending_max_size;
    struct wl_resource *decoration;         /* zxdg_toplevel_decoration_v1 (always server-side) */
    purp_window_configure_t sent;           /* last configure sent */
    bool has_sent;
    bool mapped;
};

/* xdg_positioner state (also copied into popups). */
typedef struct purp_positioner {
    int32_t width;
    int32_t height;
    purp_rect_t anchor_rect;
    uint32_t anchor;                        /* enum xdg_positioner_anchor */
    uint32_t gravity;                       /* enum xdg_positioner_gravity */
    uint32_t constraint_adjustment;         /* enum xdg_positioner_constraint_adjustment bits */
    int32_t offset_x;
    int32_t offset_y;
    bool reactive;
    bool has_parent_size;
    int32_t parent_width;
    int32_t parent_height;
    uint32_t parent_configure;
} purp_positioner_t;

struct purp_popup {
    purp_xdg_surface_t *xdg;
    struct wl_resource *resource;
    purp_surface_t *parent;                 /* parent wl_surface (xdg surface or layer surface) */
    purp_xdg_surface_t *parent_xdg;         /* NULL for layer-shell parents */
    purp_layer_surface_t *parent_layer;     /* NULL for xdg parents */
    purp_positioner_t positioner;
    purp_rect_t geometry;                   /* relative to the parent's window-geometry origin */
    bool grabbed;                           /* xdg_popup.grab */
    bool committed;
    uint32_t reposition_token;
    bool reposition_pending;
    struct wl_list parent_link;             /* parent's popups list */
    struct wl_list grab_link;               /* purp_server.popup_grabs (topmost first) */
};

/* An xdg_activation_v1 token (valid PURP_ACTIVATION_TOKEN_TTL_MS, consumed on use). */
typedef struct purp_activation_token {
    struct wl_resource *resource;           /* NULL after commit */
    char token[40];
    uint64_t created_ms;
    bool committed;
    bool has_serial;                        /* set_serial with a recent input serial */
    uint32_t serial;
    purp_surface_t *surface;                /* requesting surface (may be NULL) */
    char *app_id;
    struct wl_list link;                    /* purp_server.activation_tokens */
} purp_activation_token_t;

/* xdg_wm_base v6, zxdg_decoration_manager_v1 (always SERVER_SIDE), xdg_activation_v1. */
bool purp_xdg_init(purp_server_t *server);
purp_xdg_surface_t *purp_xdg_surface_from_surface(purp_surface_t *surface);
/* Window owning `surface` (after resolving subsurfaces and popups to their toplevel). */
purp_window_t *purp_xdg_window_for_surface(purp_server_t *server, purp_surface_t *surface);
/* wl_surface of a managed window (NULL for test windows). */
purp_surface_t *purp_xdg_window_surface(const purp_window_t *window);
/* Send every window's `want` configure that differs from the last one sent. */
void purp_xdg_flush_configures(purp_server_t *server);
void purp_xdg_toplevel_send_close(purp_toplevel_t *toplevel);
/* Positioner math: placement relative to the parent geometry origin, and the constrained
 * placement inside `bounds` (same space) honouring flip/slide/resize adjustments. */
purp_rect_t purp_positioner_geometry(const purp_positioner_t *positioner);
purp_rect_t purp_positioner_unconstrain(const purp_positioner_t *positioner, purp_rect_t bounds);
/* Global logical origin of a popup's window geometry. */
purp_point_t purp_xdg_popup_global_loc(const purp_popup_t *popup);
/* Monitor a popup belongs to (its root toplevel's or layer's monitor). */
int purp_xdg_popup_monitor(const purp_popup_t *popup);
/* Mapped popups whose root window is visible (or whose layer is mapped), bottom-to-top, on
 * `monitor` (< 0: all). Returns the count written. */
size_t purp_xdg_visible_popups(purp_server_t *server, int monitor, purp_popup_t **out,
                               size_t cap);
/* Topmost visible popup accepting input at global `pt`. */
purp_popup_t *purp_xdg_popup_at(purp_server_t *server, purp_pointf_t pt);
/* Dismiss grabbed popups (popup_done, topmost first) — clicking outside or focus changes. */
void purp_xdg_dismiss_popup_grabs(purp_server_t *server);
/* Drop activation tokens older than the TTL. */
void purp_xdg_activation_prune(purp_server_t *server, uint64_t now_ms);

/* =========================================================================================== */
/* wlr-layer-shell (src/layer_shell.c)                                                        */
/* =========================================================================================== */

#define PURP_LAYER_DEFAULT_WIDTH  740      /* unanchored axis with zero size (Spotlight) */
#define PURP_LAYER_DEFAULT_HEIGHT 620

typedef struct purp_layer_state {
    uint32_t anchor;                        /* ZWLR_LAYER_SURFACE_V1_ANCHOR_* bits */
    int32_t exclusive_zone;
    uint32_t exclusive_edge;
    int32_t margin_top;
    int32_t margin_right;
    int32_t margin_bottom;
    int32_t margin_left;
    uint32_t keyboard_interactivity;        /* NONE / EXCLUSIVE / ON_DEMAND */
    uint32_t layer;                         /* BACKGROUND / BOTTOM / TOP / OVERLAY */
    uint32_t desired_width;
    uint32_t desired_height;
} purp_layer_state_t;

struct purp_layer_surface {
    purp_server_t *server;
    struct wl_resource *resource;
    purp_surface_t *surface;
    int monitor;                            /* index into wm.monitors */
    bool explicit_output;                   /* client named a wl_output */
    char *namespace_name;                   /* "Spotlight-keepalive" is never shown or hit */
    purp_layer_state_t pending;
    purp_layer_state_t current;
    purp_rect_t geo;                        /* global logical */
    bool initial_commit_done;
    bool configured;
    bool mapped;
    bool closed;
    uint32_t configure_serial;
    purp_size_t configured_size;
    struct wl_list popups;                  /* purp_popup_t.parent_link */
    struct wl_list link;                    /* purp_server.layers (creation order) */
};

/* zwlr_layer_shell_v1 v4. */
bool purp_layer_init(purp_server_t *server);
/* Geometry inside `monitor_rect`: anchored edges + margins; zero size on an axis anchored on both
 * sides stretches; zero size otherwise defaults to 740x620 (clamped to the monitor); unanchored
 * axes centre. */
purp_rect_t purp_layer_compute_geometry(const purp_layer_state_t *state, purp_rect_t monitor_rect);
/* Recompute every layer's geometry and send configures where the size changed. */
void purp_layer_arrange_all(purp_server_t *server);
bool purp_layer_is_mapped(const purp_layer_surface_t *layer);
bool purp_layer_is_above_windows(const purp_layer_surface_t *layer);   /* TOP or OVERLAY */
bool purp_layer_wants_keyboard(const purp_layer_surface_t *layer);     /* EXCLUSIVE */
bool purp_layer_is_keepalive(const purp_layer_surface_t *layer);
/* Topmost mapped above-window layer (not keepalive) accepting input at `pt`; `*origin` receives
 * its global origin. */
purp_layer_surface_t *purp_layer_at(purp_server_t *server, purp_pointf_t pt,
                                    purp_pointf_t *origin);
/* Keyboard target: the last mapped EXCLUSIVE layer, else server->layer_focus while mapped. */
purp_layer_surface_t *purp_layer_keyboard_target(purp_server_t *server);
/* Monitors were remapped (old -> new index, -1 removed): explicit-output layers on removed
 * monitors are closed, others move to the primary; then layers on disabled monitors move to the
 * primary. */
void purp_layer_monitors_changed(purp_server_t *server, const int *old_to_new);

/* =========================================================================================== */
/* Seat, outputs, data transfer (src/seat_data.c)                                             */
/* =========================================================================================== */

typedef enum purp_cursor_mode {
    PURP_CURSOR_MODE_NAMED = 0,             /* compositor theme cursor (`cursor_shape`) */
    PURP_CURSOR_MODE_SURFACE,               /* client cursor surface */
    PURP_CURSOR_MODE_HIDDEN,                /* wl_pointer.set_cursor(NULL) */
} purp_cursor_mode_t;

typedef struct purp_touch_point {
    bool active;
    int32_t slot;                           /* backend slot == wl_touch id */
    purp_surface_t *surface;                /* focus (NULL when swallowed / no client) */
    purp_pointf_t pos;                      /* global logical */
    purp_pointf_t surface_origin;           /* global origin of `surface` */
} purp_touch_point_t;

typedef enum purp_data_source_kind {
    PURP_DATA_SOURCE_CLIENT = 0,            /* wl_data_source */
    PURP_DATA_SOURCE_PRIMARY,               /* zwp_primary_selection_source_v1 */
    PURP_DATA_SOURCE_CONTROL,               /* zwlr_data_control_source_v1 */
    PURP_DATA_SOURCE_COMPOSITOR,            /* in-process blob (screenshot PNG) */
} purp_data_source_kind_t;

typedef struct purp_data_source {
    purp_server_t *server;
    purp_data_source_kind_t kind;
    struct wl_resource *resource;           /* NULL for compositor sources */
    char **mime_types;
    size_t mime_count;
    uint32_t dnd_actions;                   /* wl_data_source.set_actions */
    uint32_t current_dnd_action;
    bool accepted;                          /* a target accepted a mime type */
    bool dnd;                               /* used by a drag */
    purp_blob_t *blob;                      /* COMPOSITOR payload, served for every mime type */
} purp_data_source_t;

typedef struct purp_drag {
    bool active;
    purp_data_source_t *source;             /* NULL: same-client drag without data */
    purp_surface_t *origin;
    purp_surface_t *icon;                   /* role DND_ICON */
    purp_surface_t *focus;
    struct wl_resource *focus_device;       /* wl_data_device of the focus client */
    struct wl_resource *offer;
    purp_pointf_t focus_origin;
    bool is_touch;
    int32_t touch_slot;
    uint32_t serial;
} purp_drag_t;

typedef struct purp_idle_inhibitor {
    struct wl_resource *resource;
    purp_surface_t *surface;
    struct wl_listener surface_destroy;
    struct wl_list link;                    /* purp_seat.idle_inhibitors */
} purp_idle_inhibitor_t;

#define PURP_MAX_PRESSED_KEYS    32
#define PURP_MAX_PRESSED_BUTTONS 16

typedef struct purp_seat {
    struct wl_global *global;
    struct wl_list resources;               /* wl_seat */
    struct wl_list keyboards;               /* wl_keyboard */
    struct wl_list pointers;                /* wl_pointer */
    struct wl_list touches;                 /* wl_touch */

    /* Keyboard (xkbcommon; keymap shared with clients via a sealed memfd). */
    struct xkb_context *xkb_context;
    struct xkb_keymap *keymap;
    struct xkb_state *xkb_state;
    int keymap_fd;
    uint32_t keymap_size;
    uint32_t mods_depressed;
    uint32_t mods_latched;
    uint32_t mods_locked;
    uint32_t group;
    uint32_t modifiers;                     /* PURP_MOD_* summary */
    uint32_t pressed_keys[PURP_MAX_PRESSED_KEYS];   /* forwarded keys still down */
    size_t pressed_key_count;
    purp_surface_t *keyboard_focus;
    struct wl_listener keyboard_focus_destroy;

    /* Pointer. The global position is wm.pointer_location. */
    purp_surface_t *pointer_focus;
    purp_pointf_t pointer_focus_origin;     /* global origin of pointer_focus */
    struct wl_listener pointer_focus_destroy;
    uint32_t pressed_buttons[PURP_MAX_PRESSED_BUTTONS];
    size_t pressed_button_count;
    uint32_t last_button_serial;
    uint32_t last_enter_serial;

    /* Touch. */
    purp_touch_point_t touch[PURP_MAX_TOUCH_POINTS];
    uint32_t last_touch_serial;

    /* Cursor image requested by the pointer-focus client. */
    purp_cursor_mode_t cursor_mode;
    purp_cursor_shape_t cursor_shape;
    purp_surface_t *cursor_surface;
    purp_point_t cursor_hotspot;
    struct wl_listener cursor_surface_destroy;

    /* Selections and drag-and-drop. */
    purp_data_source_t *selection;
    purp_data_source_t *primary_selection;
    struct wl_list data_devices;            /* wl_data_device resources */
    struct wl_list primary_devices;         /* zwp_primary_selection_device_v1 resources */
    struct wl_list data_control_devices;    /* zwlr_data_control_device_v1 resources */
    purp_drag_t drag;

    struct wl_list idle_inhibitors;         /* purp_idle_inhibitor_t.link */

    struct wl_global *data_device_manager;
    struct wl_global *primary_selection_manager;
    struct wl_global *data_control_manager;
    struct wl_global *cursor_shape_manager;
    struct wl_global *tablet_manager;
    struct wl_global *idle_inhibit_manager;
    struct wl_global *xdg_output_manager;
} purp_seat_t;

/* wl_seat "seat-0" (keyboard|pointer|touch, keymap from the default RMLVO), cursor-shape v1,
 * tablet manager v1 (tablet seats only, so cursor-shape tablet devices validate), xdg-output v3,
 * data device v3, primary selection, data-control v2, idle-inhibit. */
bool purp_seat_init(purp_server_t *server);
void purp_seat_finish(purp_server_t *server);

/* Keyboard: update xkb with an evdev keycode (returns true when the modifier state changed),
 * query symbols, and forward to clients. */
bool purp_seat_update_xkb(purp_server_t *server, uint32_t keycode, bool pressed);
xkb_keysym_t purp_seat_keysym(purp_server_t *server, uint32_t keycode);       /* shifted */
xkb_keysym_t purp_seat_raw_keysym(purp_server_t *server, uint32_t keycode);   /* level 0 */
uint32_t purp_seat_utf32(purp_server_t *server, uint32_t keycode);
void purp_seat_set_keyboard_focus(purp_server_t *server, purp_surface_t *surface);
void purp_seat_keyboard_send_key(purp_server_t *server, uint32_t time_ms, uint32_t keycode,
                                 bool pressed);
void purp_seat_keyboard_send_modifiers(purp_server_t *server);
/* Pointer: focus `surface` whose global origin is `origin` (enter/leave as needed), then motion
 * at the current wm.pointer_location. NULL clears focus. */
void purp_seat_pointer_focus(purp_server_t *server, purp_surface_t *surface, purp_pointf_t origin,
                             uint32_t time_ms);
void purp_seat_pointer_send_motion(purp_server_t *server, uint32_t time_ms);
/* Returns the button serial (0 when nobody has focus). */
uint32_t purp_seat_pointer_send_button(purp_server_t *server, uint32_t time_ms, uint32_t button,
                                       bool pressed);
void purp_seat_pointer_send_axis(purp_server_t *server, const purp_axis_event_t *event);
void purp_seat_pointer_send_frame(purp_server_t *server);
/* Touch forwarding (the input module decides focus and swallowing). */
void purp_seat_touch_send_down(purp_server_t *server, uint32_t time_ms, int32_t slot,
                               purp_surface_t *surface, purp_pointf_t origin, purp_pointf_t pos);
void purp_seat_touch_send_motion(purp_server_t *server, uint32_t time_ms, int32_t slot,
                                 purp_pointf_t pos);
void purp_seat_touch_send_up(purp_server_t *server, uint32_t time_ms, int32_t slot);
void purp_seat_touch_send_cancel(purp_server_t *server, int32_t slot);
void purp_seat_touch_send_frame(purp_server_t *server);
purp_touch_point_t *purp_seat_touch_point(purp_server_t *server, int32_t slot);
/* Whether `serial` is a recent pointer button / touch-down serial of the focus client
 * (validates move/resize/popup-grab/activation requests). */
bool purp_seat_validate_grab_serial(purp_server_t *server, struct wl_client *client,
                                    uint32_t serial);

/* Selections. `source` NULL clears. Clients bound to data devices / primary / data-control get
 * offers for the keyboard-focus client (and every data-control client). */
void purp_seat_set_selection(purp_server_t *server, purp_data_source_t *source);
void purp_seat_set_primary_selection(purp_server_t *server, purp_data_source_t *source);
/* Offer `blob` as the clipboard selection with a single mime type (screenshots: image/png). */
void purp_seat_set_compositor_selection(purp_server_t *server, const char *mime,
                                        purp_blob_t *blob);
/* Write the source's data for `mime` to `fd` (client sources: send event; compositor sources:
 * detached writer thread). Takes ownership of fd. */
void purp_data_source_send(purp_data_source_t *source, const char *mime, int fd);
void purp_data_source_destroy(purp_data_source_t *source);

/* Drag-and-drop driven by input.c. */
bool purp_seat_drag_active(const purp_server_t *server);
void purp_seat_drag_motion(purp_server_t *server, uint32_t time_ms);
void purp_seat_drag_drop(purp_server_t *server, uint32_t time_ms);
void purp_seat_drag_cancel(purp_server_t *server);

/* True while any idle inhibitor's surface is visible. */
bool purp_seat_idle_inhibited(purp_server_t *server);

/* wl_output v4 (name, description, geometry, mode, scale = ceil(scale), done) + xdg_output
 * (logical position/size). One output per enabled monitor. */
struct purp_output {
    purp_server_t *server;
    size_t monitor;                         /* index into wm.monitors (updated on remap) */
    struct wl_global *global;
    struct wl_list resources;               /* wl_output */
    struct wl_list xdg_output_resources;    /* zxdg_output_v1 */
    struct wl_list link;                    /* purp_server.outputs */
};

purp_output_t *purp_output_create(purp_server_t *server, size_t monitor);
/* Remove the global (wl_global_remove, destroyed later) and detach from the monitor. */
void purp_output_destroy(purp_output_t *output);
/* Resend geometry/mode/scale/name/description + xdg-output and done to every bound client. */
void purp_output_send_state(purp_output_t *output);
/* The wl_output resource of `client` for monitor `monitor` (wl_surface.enter), or NULL. */
struct wl_resource *purp_output_resource_for_client(purp_output_t *output,
                                                    struct wl_client *client);
/* Monitor index behind a client's wl_output resource, -1 if unknown. */
int purp_output_monitor_from_resource(struct wl_resource *resource);

/* =========================================================================================== */
/* Input routing (src/input.c)                                                                */
/* =========================================================================================== */

/* Every key action (Rust KeyAction). Parameters live in purp_action_t. */
typedef enum purp_action_kind {
    PURP_ACTION_NONE = 0,
    PURP_ACTION_TOGGLE_HELP,
    PURP_ACTION_SPAWN_LAUNCHER,
    PURP_ACTION_SPAWN_TERMINAL,
    PURP_ACTION_SPAWN_SYSTEM_MONITOR,
    PURP_ACTION_SPAWN_WIFI,
    PURP_ACTION_TOGGLE_WIFI,
    PURP_ACTION_SPAWN_BLUETOOTH,
    PURP_ACTION_TOGGLE_BLUETOOTH,
    PURP_ACTION_TOGGLE_SCRATCHPAD,
    PURP_ACTION_LOCK_SCREEN,
    PURP_ACTION_CLOSE_WINDOW,
    PURP_ACTION_QUIT,
    PURP_ACTION_FOCUS_DELTA,            /* delta */
    PURP_ACTION_MOVE_WINDOW,            /* dx, dy (unit steps) */
    PURP_ACTION_RESIZE_WINDOW,          /* dx, dy (unit directions; glides) */
    PURP_ACTION_ADJUST_MFACT,           /* mfact_delta */
    PURP_ACTION_ADJUST_NMASTER,         /* delta */
    PURP_ACTION_ADJUST_GAPS,            /* delta */
    PURP_ACTION_ADJUST_SCALE,           /* scale_delta */
    PURP_ACTION_RESET_GAPS,
    PURP_ACTION_TOGGLE_GAPS,
    PURP_ACTION_ZOOM,
    PURP_ACTION_SET_LAYOUT,             /* layout */
    PURP_ACTION_CYCLE_LAYOUT,
    PURP_ACTION_TOGGLE_LAST_LAYOUT,
    PURP_ACTION_TOGGLE_FLOATING,
    PURP_ACTION_TOGGLE_FULLSCREEN,
    PURP_ACTION_TOGGLE_OPACITY,
    PURP_ACTION_TOGGLE_BAR,
    PURP_ACTION_VIEW_TAGS,              /* tags */
    PURP_ACTION_TOGGLE_VIEW_TAGS,       /* tags */
    PURP_ACTION_TAG_WINDOW,             /* tags */
    PURP_ACTION_TOGGLE_TAG_WINDOW,      /* tags */
    PURP_ACTION_SWAP_PREV_TAGS,
    PURP_ACTION_VOLUME_UP,
    PURP_ACTION_VOLUME_DOWN,
    PURP_ACTION_VOLUME_MUTE,
    PURP_ACTION_MIC_MUTE,
    PURP_ACTION_BRIGHTNESS_UP,
    PURP_ACTION_BRIGHTNESS_DOWN,
    PURP_ACTION_MEDIA_PLAY,
    PURP_ACTION_MEDIA_NEXT,
    PURP_ACTION_MEDIA_PREV,
    PURP_ACTION_SCREENSHOT,
    PURP_ACTION_SCREENSHOT_WINDOW,
    PURP_ACTION_FOCUS_MONITOR,          /* delta */
    PURP_ACTION_MOVE_WINDOW_TO_MONITOR, /* delta */
    PURP_ACTION_SWITCH_VT,              /* vt (1..12) */
} purp_action_kind_t;

typedef struct purp_action {
    purp_action_kind_t kind;
    int32_t delta;
    int32_t dx;
    int32_t dy;
    float mfact_delta;
    double scale_delta;
    purp_layout_t layout;
    uint16_t tags;
    int vt;
} purp_action_t;

/*
 * Key table (Rust process_keyboard_event steps 4-9). `keysym` is the shifted keysym, `raw_keysym`
 * the level-0 one (Ctrl+Alt+F<n>), `mods` PURP_MOD_* bits. Global keys without Mod: XF86 audio,
 * mic, brightness, media, ScreenSaver (lock), Print (Shift/Mod: window), XF86Switch_VT_n,
 * Ctrl+Alt+F1..F12. Mod keys: see the help sheet (purp_help_sections). With help visible, a bare
 * Escape toggles help. Returns false when the key is not bound (forward it).
 */
bool purp_input_lookup_binding(uint32_t keysym, uint32_t raw_keysym, uint32_t mods,
                               bool help_visible, purp_action_t *out);
/* Run an action (Rust execute_key_action); RESIZE_WINDOW with `keycode` != 0 starts a held
 * glide for that key, 0 glides one tap. */
void purp_input_execute_action(purp_server_t *server, const purp_action_t *action,
                               uint32_t keycode);
/* Keyboard focus: nothing while locked; else the keyboard layer target, else the focused visible
 * window's surface. Dismisses popup grabs when focus leaves their client. */
void purp_input_sync_keyboard_focus(purp_server_t *server);
/* Re-run pointer focus at wm.pointer_location (skipped while a grab holds the same target);
 * time 0 = now. */
void purp_input_sync_pointer_focus(purp_server_t *server, uint32_t time_ms);
/* Surface under a global point: popups, above-window layers, windows (stacking order,
 * subsurfaces included), below-window layers. `*origin` receives the hit surface's global
 * origin. */
purp_surface_t *purp_input_surface_under(purp_server_t *server, purp_pointf_t pt,
                                         purp_pointf_t *origin);
/* xdg_toplevel.move / resize: start an interactive grab when `serial` matches the active pointer
 * or touch press; tiled windows resized at their edges get a tiled-split grab. */
void purp_input_begin_client_grab(purp_server_t *server, purp_toplevel_t *toplevel,
                                  uint32_t serial, bool resize, uint32_t edges);
/* Abort any interactive grab or key resize (window destroyed, lock, VT switch). */
void purp_input_cancel_grabs(purp_server_t *server);
/* Advance a held Mod+Shift+Arrow glide one frame; true while it is still moving. */
bool purp_input_tick_key_resize(purp_server_t *server);
/* Status-bar click on monitor `mon` at logical x (tag/layout/pill actions per button). */
void purp_input_handle_bar_click(purp_server_t *server, size_t mon, int32_t local_x,
                                 uint32_t button);
/* Spotlight: write "\"Toggle\"\n" to $XDG_RUNTIME_DIR/spotlight.sock, else spawn
 * PURPWM_SPOTLIGHT / LAUNCHER_COMMANDS ("none" disables). */
void purp_input_spawn_launcher(purp_server_t *server);
/* Pill toggles (background threads, then an immediate bar refresh). */
void purp_input_toggle_wifi_radio(purp_server_t *server);
void purp_input_toggle_bluetooth_power(purp_server_t *server);
/* Brightness fallback through logind's Session.SetBrightness (busctl), on a helper thread. */
void purp_input_adjust_backlight_logind(int percent_delta);

/* =========================================================================================== */
/* Server state (src/server.c)                                                                */
/* =========================================================================================== */

typedef enum purp_grab_kind {
    PURP_GRAB_NONE = 0,
    PURP_GRAB_MOVE,                         /* floating move (Mod+Left-drag, xdg move, touch) */
    PURP_GRAB_RESIZE,                       /* floating resize at `edges` */
    PURP_GRAB_TILED_RESIZE,                 /* moves tiled splits (`plan`) */
} purp_grab_kind_t;

typedef struct purp_interactive_grab {
    purp_grab_kind_t kind;
    uint64_t window_id;
    purp_pointf_t start_pointer;
    purp_rect_t start_geo;
    uint32_t edges;                         /* purp_edges_t */
    purp_tiled_resize_t plan;               /* TILED_RESIZE: row keys are window ids */
    bool forward_release;                   /* client-started grabs forward the button release */
    int32_t touch_slot;                     /* -1 for pointer grabs */
} purp_interactive_grab_t;

typedef struct purp_key_resize {
    bool active;
    uint64_t window_id;
    purp_key_glide_t glide;
} purp_key_resize_t;

typedef enum purp_screenshot_kind {
    PURP_SCREENSHOT_NONE = 0,
    PURP_SCREENSHOT_SCREEN,                 /* the active monitor */
    PURP_SCREENSHOT_REGION,                 /* the focused window's frame */
} purp_screenshot_kind_t;

typedef struct purp_screenshot_request {
    purp_screenshot_kind_t kind;
    purp_rect_t region;                     /* global logical (REGION) */
} purp_screenshot_request_t;

/* A spawned child; session helpers (portal, polkit agent) are SIGTERMed as a process group. */
typedef struct purp_child {
    pid_t pid;
    bool session_helper;
    char label[48];
} purp_child_t;

#define PURP_MAX_INTERCEPTED_KEYS 32
#define PURP_MAX_SCREENSHOT_QUEUE 4

struct purp_server {
    purp_options_t options;
    struct wl_display *display;
    struct wl_event_loop *loop;
    char socket_name[32];                   /* "wayland-N" */
    char runtime_dir[256];                  /* $XDG_RUNTIME_DIR or /run/user/<uid> */
    bool is_nested;                         /* not the session compositor (IPC link policy) */
    bool should_quit;
    bool started;                           /* purp_host_start ran */

    const purp_backend_ops_t *backend_ops;
    void *backend;
    purp_renderer_t *renderer;              /* NULL until purp_host_renderer_init */

    /* Globals. */
    struct wl_global *compositor_global;
    struct wl_global *subcompositor_global;
    struct wl_global *single_pixel_global;
    struct wl_global *viewporter_global;
    struct wl_global *fractional_scale_global;
    struct wl_global *dmabuf_global;
    struct wl_global *xdg_wm_base_global;
    struct wl_global *decoration_global;
    struct wl_global *activation_global;
    struct wl_global *layer_shell_global;

    /* linux-dmabuf. */
    purp_dmabuf_format_t *dmabuf_formats;
    size_t dmabuf_format_count;
    dev_t dmabuf_main_device;
    int dmabuf_table_fd;                    /* sealed memfd with the format table, -1 if none */
    size_t dmabuf_table_size;

    /* Protocol object lists. */
    struct wl_list surfaces;                /* purp_surface_t.link */
    struct wl_list xdg_surfaces;            /* purp_xdg_surface_t.link */
    struct wl_list layers;                  /* purp_layer_surface_t.link */
    struct wl_list outputs;                 /* purp_output_t.link */
    struct wl_list activation_tokens;       /* purp_activation_token_t.link */
    struct wl_list popup_grabs;             /* purp_popup_t.grab_link, topmost first */
    uint32_t activation_counter;

    /* Model and UI. */
    purp_wm_t wm;
    purp_seat_t seat;
    purp_font_t *font;
    purp_bar_t bar;
    purp_lock_screen_t *lock_screen;        /* non-NULL while locked */
    purp_notify_center_t notify;
    purp_help_overlay_t help;
    purp_cursor_theme_t cursor_theme;
    purp_wallpaper_t wallpaper;
    purp_ipc_server_t ipc;
    purp_capture_server_t capture;

    /* Input routing state (input.c). */
    bool mod_tap_candidate;                 /* a lone Mod press may still launch Spotlight */
    uint32_t intercepted_keys[PURP_MAX_INTERCEPTED_KEYS];   /* bound keys: swallow release */
    size_t intercepted_key_count;
    uint32_t swallowed_buttons[PURP_MAX_PRESSED_BUTTONS];   /* consumed presses: drop release */
    size_t swallowed_button_count;
    int32_t swallowed_touch_slots[PURP_MAX_TOUCH_POINTS];
    size_t swallowed_touch_count;
    int32_t touch_grab_slot;                /* slot driving `grab`, -1 none */
    double bar_scroll_accum;
    purp_interactive_grab_t grab;
    purp_key_resize_t key_resize;
    purp_layer_surface_t *layer_focus;      /* ON_DEMAND layer focused by click */
    int pending_vt_switch;                  /* 0 = none */

    /* Rendering bookkeeping. */
    bool monitor_damaged[PURP_MAX_MONITORS];

    /* Screenshots: request consumed by the next tick; encoded PNGs come back from the encoder
     * thread through `screenshot_ready` (guarded by screenshot_lock). */
    purp_screenshot_request_t screenshot_request;
    pthread_mutex_t screenshot_lock;
    purp_blob_t *screenshot_ready[PURP_MAX_SCREENSHOT_QUEUE];
    size_t screenshot_ready_count;

    /* Children (guarded by children_lock: reaped from tick, appended by spawns). */
    pthread_mutex_t children_lock;
    purp_child_t *children;
    size_t child_count;
    size_t child_cap;

    uint64_t start_ms;
    uint64_t last_trim_ms;
};

/* ---- relayout, focus, damage --------------------------------------------------------------- */

/* purp_wm_relayout + layer arrangement + configure flush + keyboard/pointer focus sync + bar
 * dirty + damage (Rust PurpState::relayout). */
void purp_server_relayout(purp_server_t *server);
/* Damage the monitors overlapping a global logical rectangle. */
void purp_server_damage_rect(purp_server_t *server, purp_rect_t rect);
/* Monitors whose rectangle overlaps `rect` as a bitmask. */
uint32_t purp_server_monitor_mask_for_rect(const purp_server_t *server, purp_rect_t rect);
/* Recompute wl_surface.enter/leave and preferred scales for every window, layer and popup. */
void purp_server_update_surface_outputs(purp_server_t *server);
/* Send frame callbacks for everything visible on `monitor` (none while locked). */
void purp_server_send_frame_callbacks(purp_server_t *server, size_t monitor, uint32_t time_ms);
/* Milliseconds since the server started (wl_callback / event timestamps). */
uint32_t purp_server_time_ms(const purp_server_t *server);

/* ---- monitors ------------------------------------------------------------------------------ */

/* After any monitor model change: normalise, create/destroy wl_output globals for enabled /
 * disabled monitors and resend their state, prune bar slots and set their scales, move layers,
 * update surface outputs, relayout, damage everything. `old_to_new` may be NULL (no removal). */
void purp_server_monitors_changed(purp_server_t *server, const int *old_to_new);
/* Apply entries (IPC or monitors.json); with `persist` write monitors.json on success. The
 * backend's allow_custom_modes decides custom modes. */
bool purp_server_apply_monitor_configs(purp_server_t *server, const purp_monitor_config_t *entries,
                                       size_t count, bool persist, char *err, size_t err_len);
/* Answer one IPC request (get/apply monitors replies with monitor infos; focus/move with ok). */
void purp_server_handle_ipc_request(purp_server_t *server, const purp_ipc_request_t *request);

/* ---- scale --------------------------------------------------------------------------------- */

/* $PURPWM_SCALE, else the saved scale file (purpwm+ then legacy purpwm), else
 * $PURPWM_DEFAULT_SCALE, else PURP_DEFAULT_SCALE; clamped to [SCALE_MIN, SCALE_MAX]. */
double purp_initial_scale(void);
/* ~/.config/purpwm+/scale (XDG_CONFIG_HOME aware). */
bool purp_scale_file_path(char *out, size_t out_len);
/* Mod+Ctrl+= / -: adjust the active monitor, persist scale + monitors.json, republish. */
void purp_server_adjust_scale(purp_server_t *server, double delta);

/* ---- spawning ------------------------------------------------------------------------------ */

/* argv[0] exists: contains '/' and is a regular executable, or is found in $PATH. */
bool purp_command_exists(const char *program);
/* fork + setpgid(0, 0) + stdin </dev/null + child environment (WAYLAND_DISPLAY,
 * PURPWM_CAPTURE_SOCKET, PURPWM_IPC_SOCKET, XDG_RUNTIME_DIR, XDG_SESSION_TYPE=wayland,
 * XDG_CURRENT_DESKTOP=purpwm, WGPU_BACKEND=gl, QT_QPA_PLATFORM=wayland, GDK_BACKEND=wayland,
 * MOZ_ENABLE_WAYLAND=1, ELECTRON_OZONE_PLATFORM_HINT=auto, DBUS_SESSION_BUS_ADDRESS from
 * <runtime>/bus when unset), signal mask/dispositions reset, then execvp. Returns the pid or -1.
 * Logs "Spawned <label> on WAYLAND_DISPLAY=<socket>: <argv>". */
pid_t purp_server_spawn(purp_server_t *server, const char *const *argv, const char *label,
                        bool session_helper);
/* First candidate whose argv[0] exists ("No available <label> binary found in config
 * candidates" otherwise). */
pid_t purp_server_spawn_first(purp_server_t *server, purp_command_list_t candidates,
                              const char *label, bool session_helper);
/* $<env_var> == "none": nothing; non-empty and not "auto": that program + `extra_args`
 * (NULL-terminated, may be NULL); else purp_server_spawn_first(candidates). */
bool purp_server_spawn_configured(purp_server_t *server, const char *env_var,
                                  const char *const *extra_args, purp_command_list_t candidates,
                                  const char *label);
/* Run a command synchronously with a timeout, capturing stdout (helper threads only). */
bool purp_run_capture(const char *const *argv, int timeout_ms, char *out, size_t out_len);
/* waitpid(WNOHANG) every child; exited session helpers leave the pgid list. */
void purp_server_reap_children(purp_server_t *server);
/* SIGTERM every session-helper process group (pgid > 1), then reap. */
void purp_server_shutdown_session(purp_server_t *server);

/* Session services started by autostart. */
void purp_server_spawn_terminal(purp_server_t *server);
void purp_server_spawn_spotlight_daemon(purp_server_t *server);   /* pkill, rm sock, --daemon */
void purp_server_spawn_keyring(purp_server_t *server);
void purp_server_spawn_screenshot_portal(purp_server_t *server);
void purp_server_spawn_polkit_agent(purp_server_t *server);
void purp_server_start_notifications(purp_server_t *server);
void purp_server_ensure_network_and_bluetooth(purp_server_t *server);
void purp_server_run_autostart(purp_server_t *server);

/* ---- screenshots, clipboard, capture ------------------------------------------------------- */

/* Print / SIGUSR1 / Shift+Print: ignored while locked; REGION when `focused_window_only` and a
 * visible window is focused, else SCREEN. Taken on the next tick. */
void purp_server_request_screenshot(purp_server_t *server, bool focused_window_only);
/* Render the pending request offscreen (backend make_current + purp_render_capture_rgba) and
 * hand the pixels to purp_server_save_screenshot. */
void purp_server_process_screenshot(purp_server_t *server);
/* Takes ownership of `rgba` (malloc'd, w*h*4, opaque). A detached thread encodes the PNG,
 * writes $HOME/Pictures/Screenshots/purpwm+-<stamp>[-N].png atomically (0600; /tmp without HOME)
 * and queues the PNG for the clipboard. */
void purp_server_save_screenshot(purp_server_t *server, uint8_t *rgba, uint32_t width,
                                 uint32_t height);
/* Move finished PNGs to the clipboard (image/png). */
void purp_server_offer_screenshots(purp_server_t *server);
/* purp_capture_fn for the portal socket: PNG of the active monitor without the cursor. */
purp_blob_t *purp_server_capture_png(void *server);

/* ---- lock ---------------------------------------------------------------------------------- */

/* Mod+Shift+l / Mod+Ctrl+l / XF86ScreenSaver / SIGUSR2: create the lock screen (if not locked),
 * cancel grabs, clear keyboard/pointer/touch focus, dismiss popups and help, damage all. */
void purp_server_lock_session(purp_server_t *server);
bool purp_server_is_locked(const purp_server_t *server);
/* Poll the PAM worker; on unlock destroy the lock screen and restore focus. True on change. */
bool purp_server_poll_lock(purp_server_t *server);

/* ---- maintenance --------------------------------------------------------------------------- */

/* Activation-token TTL, child reaping, malloc_trim(0) on a helper thread every 30 s. */
void purp_server_maintenance_tick(purp_server_t *server);
/* "$XDG_RUNTIME_DIR/<prefix><socket>.sock" paths for IPC / capture. */
void purp_server_socket_path(const purp_server_t *server, const char *prefix, char *out,
                             size_t out_len);

#endif /* PURPWM_PURPWM_H */
