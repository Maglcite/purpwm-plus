/*
 * purpwm+ — shared foundation types.
 *
 * Geometry (logical and physical pixels), colours, CPU pixel buffers, monotonic time, logging,
 * reference-counted byte blobs and the small enums every module agrees on.
 *
 * Pure header: never includes <wayland-server*.h> or <wayland-client*.h>, so the nested host
 * backend (a Wayland *client*) and the unit tests can include it alongside anything.
 *
 * Coordinate conventions
 *   - "logical" pixels: layout, hit-testing and everything clients see. The global desktop is the
 *     union of enabled monitor rectangles; each monitor has a logical origin `loc`.
 *   - "physical" pixels: framebuffer pixels, `logical * monitor scale` (rounded).
 *   - Rectangles are half-open: a point p is inside r when r.x <= p.x < r.x + r.w (same for y).
 */
#ifndef PURPWM_TYPES_H
#define PURPWM_TYPES_H

#include <math.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <time.h>

/* =========================================================================================== */
/* Compiler helpers                                                                           */
/* =========================================================================================== */

#define PURP_UNUSED           __attribute__((unused))
#define PURP_PRINTF(fmt, va)  __attribute__((format(printf, fmt, va)))
#define PURP_WARN_UNUSED      __attribute__((warn_unused_result))
#define PURP_ARRAY_LEN(a)     (sizeof(a) / sizeof((a)[0]))
/* `container_of` for intrusive structures (wl_listener members and friends). */
#define PURP_CONTAINER_OF(ptr, type, member)                                                 \
    ((type *)(void *)((char *)(ptr) - offsetof(type, member)))

/* =========================================================================================== */
/* Global limits                                                                              */
/* =========================================================================================== */

/* dwm-style workspace tags: bit i of a tag mask is tag i + 1. */
#define PURP_TAG_COUNT        9
#define PURP_TAGMASK          ((uint16_t)((1u << PURP_TAG_COUNT) - 1u))

/* Monitors: at most this many connectors/virtual outputs are tracked at once. The IPC limit on
 * `apply_monitors` entries (32) is larger on purpose: unknown names are simply ignored. */
#define PURP_MAX_MONITORS     16
#define PURP_MAX_MODES        64
#define PURP_MONITOR_NAME_MAX 64
#define PURP_MONITOR_DESC_MAX 128

/* Simultaneous touch points tracked by the seat (slots beyond are ignored). */
#define PURP_MAX_TOUCH_POINTS 16

/* =========================================================================================== */
/* Opaque cross-module handles                                                                */
/* =========================================================================================== */

/* The compositor instance (struct defined in purpwm.h; backends only ever hold a pointer). */
typedef struct purp_server purp_server_t;
/* The wl_output global behind one monitor (purpwm.h). */
typedef struct purp_output purp_output_t;
/* The xdg_toplevel behind a managed window (purpwm.h). NULL for windows built by unit tests. */
typedef struct purp_toplevel purp_toplevel_t;
/* The GLES2 renderer (render.h). */
typedef struct purp_renderer purp_renderer_t;
/* A loaded FreeType face plus its glyph cache (ui.h / font.c). */
typedef struct purp_font purp_font_t;

/* =========================================================================================== */
/* Small numeric helpers                                                                      */
/* =========================================================================================== */

static inline int32_t purp_min_i32(int32_t a, int32_t b) { return a < b ? a : b; }
static inline int32_t purp_max_i32(int32_t a, int32_t b) { return a > b ? a : b; }
/* Clamp to [lo, hi]; when lo > hi the result is lo (Rust's `max(lo).min(hi)` order differs, so
 * callers that need that exact order spell it out). */
static inline int32_t purp_clamp_i32(int32_t v, int32_t lo, int32_t hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}
static inline double purp_clamp_f64(double v, double lo, double hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}
static inline float purp_clamp_f32(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}
/* Round half away from zero to int32 (Rust `f64::round() as i32`, saturating). */
static inline int32_t purp_round_i32(double v) {
    double r = round(v);
    if (r >= 2147483647.0) return INT32_MAX;
    if (r <= -2147483648.0) return INT32_MIN;
    return (int32_t)r;
}

/* =========================================================================================== */
/* Geometry                                                                                   */
/* =========================================================================================== */

typedef struct purp_point {
    int32_t x, y;
} purp_point_t;

typedef struct purp_pointf {
    double x, y;
} purp_pointf_t;

typedef struct purp_size {
    int32_t w, h;
} purp_size_t;

typedef struct purp_rect {
    int32_t x, y, w, h;
} purp_rect_t;

typedef struct purp_rectf {
    double x, y, w, h;
} purp_rectf_t;

static inline purp_rect_t purp_rect_make(int32_t x, int32_t y, int32_t w, int32_t h) {
    purp_rect_t r = { x, y, w, h };
    return r;
}
static inline purp_rect_t purp_rect_from_size(purp_size_t s) {
    return purp_rect_make(0, 0, s.w, s.h);
}
static inline purp_point_t purp_rect_loc(purp_rect_t r) {
    purp_point_t p = { r.x, r.y };
    return p;
}
static inline purp_size_t purp_rect_size(purp_rect_t r) {
    purp_size_t s = { r.w, r.h };
    return s;
}
static inline bool purp_rect_is_empty(purp_rect_t r) { return r.w <= 0 || r.h <= 0; }
static inline bool purp_rect_eq(purp_rect_t a, purp_rect_t b) {
    return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h;
}
/* Half-open containment: [x, x + w) x [y, y + h). */
static inline bool purp_rect_contains(purp_rect_t r, int32_t x, int32_t y) {
    return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h;
}
/* Half-open containment for sub-pixel points (pointer and touch positions). */
static inline bool purp_rect_contains_f(purp_rect_t r, purp_pointf_t p) {
    return p.x >= (double)r.x && p.y >= (double)r.y && p.x < (double)r.x + (double)r.w &&
           p.y < (double)r.y + (double)r.h;
}
static inline bool purp_rect_overlaps(purp_rect_t a, purp_rect_t b) {
    return a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h;
}
/* Intersection; returns false (and an empty *out) when the rectangles do not overlap. */
static inline bool purp_rect_intersect(purp_rect_t a, purp_rect_t b, purp_rect_t *out) {
    int32_t x0 = purp_max_i32(a.x, b.x), y0 = purp_max_i32(a.y, b.y);
    int32_t x1 = purp_min_i32(a.x + a.w, b.x + b.w), y1 = purp_min_i32(a.y + a.h, b.y + b.h);
    if (x1 <= x0 || y1 <= y0) {
        *out = purp_rect_make(x0, y0, 0, 0);
        return false;
    }
    *out = purp_rect_make(x0, y0, x1 - x0, y1 - y0);
    return true;
}
/* Smallest rectangle covering both (an empty rectangle is ignored). */
static inline purp_rect_t purp_rect_union(purp_rect_t a, purp_rect_t b) {
    if (purp_rect_is_empty(a)) return b;
    if (purp_rect_is_empty(b)) return a;
    int32_t x0 = purp_min_i32(a.x, b.x), y0 = purp_min_i32(a.y, b.y);
    int32_t x1 = purp_max_i32(a.x + a.w, b.x + b.w), y1 = purp_max_i32(a.y + a.h, b.y + b.h);
    return purp_rect_make(x0, y0, x1 - x0, y1 - y0);
}
static inline purp_rect_t purp_rect_translate(purp_rect_t r, int32_t dx, int32_t dy) {
    return purp_rect_make(r.x + dx, r.y + dy, r.w, r.h);
}
/* Grow (or shrink, for negative n) by n on every side. */
static inline purp_rect_t purp_rect_inflate(purp_rect_t r, int32_t n) {
    return purp_rect_make(r.x - n, r.y - n, r.w + 2 * n, r.h + 2 * n);
}
static inline purp_pointf_t purp_rect_center(purp_rect_t r) {
    purp_pointf_t p = { (double)r.x + (double)r.w / 2.0, (double)r.y + (double)r.h / 2.0 };
    return p;
}
/* Logical -> physical, Smithay's `to_physical_precise_round`: both corners are scaled and
 * rounded, so adjacent rectangles stay seamless at fractional scales. */
static inline purp_rect_t purp_rect_to_physical(purp_rect_t r, double scale) {
    int32_t x0 = purp_round_i32((double)r.x * scale);
    int32_t y0 = purp_round_i32((double)r.y * scale);
    int32_t x1 = purp_round_i32((double)(r.x + r.w) * scale);
    int32_t y1 = purp_round_i32((double)(r.y + r.h) * scale);
    return purp_rect_make(x0, y0, x1 - x0, y1 - y0);
}

/* =========================================================================================== */
/* Colours and CPU pixel buffers                                                              */
/* =========================================================================================== */

/* Straight-alpha float colour for the GL renderer (premultiplied at draw time). */
typedef struct purp_color {
    float r, g, b, a;
} purp_color_t;

/* Straight-alpha 8-bit colour used by the software rasterisers (bar, lock, toasts, help). */
typedef struct purp_rgba {
    uint8_t r, g, b, a;
} purp_rgba_t;

/*
 * A CPU raster uploaded to the GPU by render.c (`purp_render_pixel_buf`).
 *
 * Format: premultiplied ARGB8888 as stored by a little-endian CPU, i.e. bytes B, G, R, A per
 * pixel (DRM_FORMAT_ARGB8888 / GL_BGRA_EXT), rows top-down, `stride` bytes per row.
 *
 * `id` is process-unique and assigned on first allocation; render.c keys its texture cache on it
 * and re-uploads whenever `generation` differs from the uploaded one. Every function that changes
 * pixels must bump `generation` (the drawing helpers in ui.h only write pixels; callers bump it
 * once per redraw via purp_pixel_buf_touch()). Allocation helpers live in ui.h (font.c).
 */
typedef struct purp_pixel_buf {
    uint8_t *data;
    int32_t width;
    int32_t height;
    int32_t stride;
    uint64_t id;
    uint64_t generation;
} purp_pixel_buf_t;

/* =========================================================================================== */
/* Shared enums                                                                               */
/* =========================================================================================== */

/* dwm layouts, in Mod+Ctrl+Space cycle order: Tile -> CenteredMaster -> Grid -> Monocle ->
 * Floating -> Tile (purp_layout_next). Symbols: "[]=", "|M|", "###", "[M]", "><>". */
typedef enum purp_layout {
    PURP_LAYOUT_TILE = 0,
    PURP_LAYOUT_CENTERED_MASTER,
    PURP_LAYOUT_GRID,
    PURP_LAYOUT_MONOCLE,
    PURP_LAYOUT_FLOATING,
    PURP_LAYOUT_COUNT
} purp_layout_t;

/* Resize edges, bit-compatible with xdg_toplevel.resize_edge (top=1, bottom=2, left=4,
 * right=8; e.g. XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM_RIGHT == PURP_EDGE_BOTTOM | PURP_EDGE_RIGHT). */
typedef enum purp_edges {
    PURP_EDGE_NONE = 0,
    PURP_EDGE_TOP = 1u << 0,
    PURP_EDGE_BOTTOM = 1u << 1,
    PURP_EDGE_LEFT = 1u << 2,
    PURP_EDGE_RIGHT = 1u << 3,
    PURP_EDGE_BOTTOM_RIGHT = (1u << 1) | (1u << 3),
} purp_edges_t;

/* Keyboard modifier mask derived from the xkb state ("Mod" = LOGO or ALT). */
typedef enum purp_modifiers {
    PURP_MOD_SHIFT = 1u << 0,
    PURP_MOD_CAPS = 1u << 1,
    PURP_MOD_CTRL = 1u << 2,
    PURP_MOD_ALT = 1u << 3,
    PURP_MOD_NUM = 1u << 4,
    PURP_MOD_LOGO = 1u << 6,
} purp_modifiers_t;

static inline bool purp_mods_has_mod(uint32_t mods) {
    return (mods & (PURP_MOD_LOGO | PURP_MOD_ALT)) != 0;
}

/* Linux input button codes used by bindings (linux/input-event-codes.h values). */
#define PURP_BTN_LEFT   0x110u
#define PURP_BTN_RIGHT  0x111u
#define PURP_BTN_MIDDLE 0x112u

/* One output mode (ipc `available_modes` entry and DRM/virtual mode lists). */
typedef struct purp_display_mode {
    uint32_t width;
    uint32_t height;
    int32_t refresh_mhz;
    bool preferred;
} purp_display_mode_t;

/* What the status bar shows for one monitor; produced by purp_wm_bar_inputs() (wm.h) and
 * consumed by purp_bar_render() (ui.h). `focused_title` points into caller storage. */
typedef struct purp_bar_inputs {
    uint16_t selected_tags;
    uint16_t occupied_tags;
    uint16_t focused_tags;
    uint16_t urgent_tags;
    purp_layout_t layout;
    const char *focused_title;
} purp_bar_inputs_t;

/* =========================================================================================== */
/* Monotonic time                                                                             */
/* =========================================================================================== */

/* CLOCK_MONOTONIC in microseconds / milliseconds. */
static inline uint64_t purp_now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}
static inline uint64_t purp_now_ms(void) { return purp_now_us() / 1000u; }

/* =========================================================================================== */
/* Logging                                                                                    */
/* =========================================================================================== */

typedef enum purp_log_level {
    PURP_LOG_ERROR = 0,
    PURP_LOG_WARN,
    PURP_LOG_INFO,
    PURP_LOG_DEBUG,
} purp_log_level_t;

/* Writes "purpwm+ <LEVEL>: message\n" to stderr. DEBUG lines appear only when $PURPWM_DEBUG is
 * set. Safe from any thread (one fprintf per line); not async-signal-safe. */
static inline PURP_PRINTF(2, 0) void purp_logv(purp_log_level_t level, const char *fmt,
                                               va_list ap) {
    static const char *const names[] = { "ERROR", "WARN", "INFO", "DEBUG" };
    if (level == PURP_LOG_DEBUG && getenv("PURPWM_DEBUG") == NULL) return;
    char line[1024];
    vsnprintf(line, sizeof line, fmt, ap);
    fprintf(stderr, "purpwm+ %s: %s\n", names[level], line);
}
static inline PURP_PRINTF(2, 3) void purp_log(purp_log_level_t level, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    purp_logv(level, fmt, ap);
    va_end(ap);
}
#define PURP_ERROR(...) purp_log(PURP_LOG_ERROR, __VA_ARGS__)
#define PURP_WARN(...)  purp_log(PURP_LOG_WARN, __VA_ARGS__)
#define PURP_INFO(...)  purp_log(PURP_LOG_INFO, __VA_ARGS__)
#define PURP_DEBUG(...) purp_log(PURP_LOG_DEBUG, __VA_ARGS__)

/* =========================================================================================== */
/* Reference-counted immutable byte blobs                                                     */
/* =========================================================================================== */

/* Shared, immutable bytes handed between threads: encoded screenshot PNGs travel from the encoder
 * thread to the clipboard source and to every fd a paste writes to. */
typedef struct purp_blob {
    atomic_size_t refs;
    size_t len;
    uint8_t data[];
} purp_blob_t;

/* New blob with one reference holding a copy of `len` bytes (data may be NULL to zero-fill). */
static inline purp_blob_t *purp_blob_new(const void *data, size_t len) {
    purp_blob_t *b = (purp_blob_t *)malloc(sizeof(*b) + (len ? len : 1));
    if (!b) return NULL;
    atomic_init(&b->refs, 1);
    b->len = len;
    if (len) {
        if (data) memcpy(b->data, data, len);
        else memset(b->data, 0, len);
    }
    return b;
}
static inline purp_blob_t *purp_blob_ref(purp_blob_t *b) {
    if (b) atomic_fetch_add_explicit(&b->refs, 1, memory_order_relaxed);
    return b;
}
static inline void purp_blob_unref(purp_blob_t *b) {
    if (b && atomic_fetch_sub_explicit(&b->refs, 1, memory_order_acq_rel) == 1) free(b);
}

#endif /* PURPWM_TYPES_H */
