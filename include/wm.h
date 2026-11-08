/*
 * purpwm+ — window-management model: dwm tiling engine, per-tag state, monitors, focus, stacking,
 * floating/scratchpad/fullscreen rules and the Mod+Shift+Arrow keyboard glide.
 *
 * Implemented by src/wm.c (everything except the glide) and src/glide.c (purp_key_glide_*).
 *
 * Pure header and pure modules: no libwayland, no GL. wm.c and glide.c never call into the server,
 * renderer or backends, so tests/test_purpwm_plus.c links them directly. The compositor owns one
 * purp_wm_t (server->wm) and turns the model's intent into protocol traffic:
 *   - every window carries the configure it *should* have (`want`, `configure_dirty`); xdg_shell.c
 *     sends it (purp_xdg_flush_configures) when it differs from the last configure sent;
 *   - monitor changes set `monitors_dirty`; server.c republishes wl_outputs and asks the backend
 *     to apply modes;
 *   - monitor removal reports an old->new index map so server-side objects can follow.
 *
 * Windows are stored newest-first (index 0 is the most recently mapped), matching dwm's attach()
 * and the Rust `Vec<ManagedWindow>`; `focused_idx` indexes that array (-1 = none). Window ids are
 * stable across reordering and are what long-lived references (grabs, key resizes) hold.
 */
#ifndef PURPWM_WM_H
#define PURPWM_WM_H

#include "config.h"
#include "ipc.h"
#include "types.h"

/* =========================================================================================== */
/* Layouts                                                                                    */
/* =========================================================================================== */

/* "[]=", "|M|", "###", "[M]", "><>". */
const char *purp_layout_symbol(purp_layout_t layout);
/* Tile -> CenteredMaster -> Grid -> Monocle -> Floating -> Tile. */
purp_layout_t purp_layout_next(purp_layout_t layout);

/* =========================================================================================== */
/* Windows                                                                                    */
/* =========================================================================================== */

/* What xdg_shell.c should tell the client in the next xdg_toplevel.configure. */
typedef struct purp_window_configure {
    int32_t width;        /* surface size; 0x0 lets the client pick (self-sized floats) */
    int32_t height;
    bool activated;       /* xdg_toplevel.state.activated */
    bool fullscreen;      /* .fullscreen */
    bool tiled;           /* all four .tiled_left/right/top/bottom states */
} purp_window_configure_t;

/* Per-window state managed by purpwm+ (Rust ManagedWindow). */
typedef struct purp_window {
    uint64_t id;                     /* stable, unique, never 0 */
    purp_toplevel_t *toplevel;       /* server xdg_toplevel; NULL in unit tests */

    int monitor;                     /* index of the owning monitor */
    uint16_t tags;                   /* 9-bit tag mask */
    bool is_floating;
    bool is_fullscreen;
    bool is_urgent;                  /* set by untrusted xdg_activation requests */
    bool is_scratchpad;              /* the Mod+s drop-down terminal */
    bool is_hidden;                  /* scratchpad currently hidden */
    bool has_opacity_override;       /* Mod+o or a window rule */
    float opacity_override;          /* 1.0 = fully opaque */
    bool rules_applied;              /* WINDOW_RULES settled (matched or both strings known) */
    char *rule_seen_app_id;          /* last (app_id, title) seen by the rule matcher (owned) */
    char *rule_seen_title;

    purp_rect_t geo;                 /* outer frame incl. borders, global logical coords */
    purp_rect_t float_geo;           /* saved floating frame */
    bool size_from_client;           /* self-sized float: configured 0x0, frame follows client */
    bool client_sized_once;          /* client size adopted at least once (first time centres) */
    float cfact;                     /* height weight inside its tiled column (dwm cfacts) */

    /* Client facts mirrored by xdg_shell.c on commit (owned strings, "" when unset). */
    char *app_id;
    char *title;
    bool mapped;                     /* has a committed buffer */
    bool has_client_geometry;        /* xdg_surface.set_window_geometry was used */
    purp_rect_t client_geometry;     /* window geometry relative to the surface origin */
    purp_size_t buffer_size;         /* logical size of the committed buffer (0x0 unmapped) */
    purp_size_t min_size;            /* xdg_toplevel min/max size hints (0 = unset) */
    purp_size_t max_size;

    /* Configure intent written by the WM and consumed by xdg_shell.c. */
    purp_window_configure_t want;
    bool configure_dirty;
} purp_window_t;

/* Initialise `win` (zeroed by the caller is fine) for a new window on `monitor` whose output
 * rectangle is `output_rect`: float_geo is 3/5 of the output (at least 360x240) centred below
 * the bar, geo = float_geo, cfact 1.0, tags as given, strings empty. `id` is left for the WM. */
void purp_window_init(purp_window_t *win, purp_toplevel_t *toplevel, uint16_t tags, int monitor,
                      purp_rect_t output_rect);
/* Free owned strings. */
void purp_window_finish(purp_window_t *win);
/* Replace app_id / title (NULL treated as ""). Return true when the value changed. */
bool purp_window_set_app_id(purp_window_t *win, const char *app_id);
bool purp_window_set_title(purp_window_t *win, const char *title);
/* Title for the bar: title, else app_id, else "purpwm+ client". */
const char *purp_window_display_title(const purp_window_t *win);
/* Visible on `selected_tags`: never when hidden; scratchpads always; else tags overlap. */
bool purp_window_is_visible_on(const purp_window_t *win, uint16_t selected_tags);
/* Where the client's visible content starts (inside the border; geo.loc when fullscreen). */
purp_point_t purp_window_content_loc(const purp_window_t *win);
/* Buffer origin: content_loc minus the xdg window-geometry offset (GTK client-side shadows). */
purp_point_t purp_window_surface_loc(const purp_window_t *win);
/* Inner size: geo minus 2*BORDER_WIDTH, each axis at least 1 (geo size when fullscreen). */
purp_size_t purp_window_surface_size(const purp_window_t *win);
/* Visible client size: window geometry size, else buffer size; false while unmapped/empty. */
bool purp_window_client_geometry_size(const purp_window_t *win, purp_size_t *out);
/* Clamp an outer frame so the inner client area respects min/max hints (0 = unset). */
purp_size_t purp_window_clamp_frame_to_client_limits(const purp_window_t *win, purp_size_t frame);
/* Compute `want` from geometry and focus and set configure_dirty when it changed. Tiled states
 * are set only for non-floating, non-fullscreen, non-scratchpad windows in a tiling layout;
 * self-sized floats (size_from_client, not fullscreen) get 0x0. Maximized is never advertised. */
void purp_window_configure(purp_window_t *win, bool focused, bool tiled_layout);

/* =========================================================================================== */
/* Window rules (config.h PURP_WINDOW_RULES)                                                  */
/* =========================================================================================== */

/* Conjunctive match: NULL matchers are wildcards, a rule with both NULL never matches, a set
 * matcher needs its field non-empty and containing the needle. */
bool purp_window_rule_matches(const purp_window_rule_t *rule, const char *app_id,
                              const char *title);
/* Matching settles once a rule matched or both app_id and title are known. */
bool purp_window_rules_settled(bool matched, const char *app_id, const char *title);

/* =========================================================================================== */
/* Per-tag workspace state (dwm Monitor + pertag)                                             */
/* =========================================================================================== */

typedef struct purp_tag_state {
    purp_layout_t layout;            /* default Tile */
    purp_layout_t prev_layout;       /* default Floating */
    float mfact;                     /* default PURP_DEFAULT_MFACT, clamped 0.10..0.90 */
    int nmaster;                     /* default PURP_DEFAULT_NMASTER, >= 0 */
} purp_tag_state_t;

typedef struct purp_workspace {
    uint16_t selected_tags;          /* viewed tags (default 1 << 0) */
    uint16_t prev_tags;              /* Mod+Tab target */
    int cur_tag;                     /* lowest selected tag index, keys `pertag` */
    purp_tag_state_t pertag[PURP_TAG_COUNT];
    int32_t gap_inner;               /* runtime gaps (Mod+= / Mod+-) */
    int32_t gap_outer;
    bool gaps_enabled;               /* Mod+Shift+g */
    bool show_bar;                   /* Mod+b */
} purp_workspace_t;

void purp_workspace_init(purp_workspace_t *ws);
purp_layout_t purp_workspace_layout(const purp_workspace_t *ws);
purp_layout_t purp_workspace_prev_layout(const purp_workspace_t *ws);
float purp_workspace_mfact(const purp_workspace_t *ws);
int purp_workspace_nmaster(const purp_workspace_t *ws);
/* Switch the current tag's layout, remembering the previous one when it changes. */
void purp_workspace_set_layout(purp_workspace_t *ws, purp_layout_t layout);
void purp_workspace_set_mfact(purp_workspace_t *ws, float mfact);
void purp_workspace_adjust_mfact(purp_workspace_t *ws, float delta);
void purp_workspace_adjust_nmaster(purp_workspace_t *ws, int delta);
/* Enables gaps, then inner += delta (0..48) and outer += delta (0..64). */
void purp_workspace_adjust_gaps(purp_workspace_t *ws, int32_t delta);
void purp_workspace_reset_gaps(purp_workspace_t *ws);
void purp_workspace_toggle_gaps(purp_workspace_t *ws);
/* cur_tag = lowest set bit of selected_tags (unchanged when none). */
void purp_workspace_sync_cur_tag(purp_workspace_t *ws);
/* Gap between tiled windows, 0 while gaps are off. */
int32_t purp_workspace_inner_gap(const purp_workspace_t *ws);
/* Tiling area: below the bar (when shown) and inside the outer gaps, each axis >= 1. */
purp_rect_t purp_workspace_usable_area_in(const purp_workspace_t *ws, purp_rect_t output_rect);
/* Whether centred-master currently has stack columns on both sides of the master. */
bool purp_workspace_centered_sides(const purp_workspace_t *ws, size_t tiled_count);
/* Indices (into `windows`) of windows on `monitor` (< 0: any monitor) that the layout tiles:
 * visible on selected tags, not floating/fullscreen/scratchpad. Returns the count written
 * (at most `cap`). */
size_t purp_workspace_tiled_indices(const purp_workspace_t *ws, purp_window_t *const *windows,
                                    size_t count, int monitor, size_t *out, size_t cap);
/*
 * Arrange the windows on `output_rect` (only those on `monitor`, or all when monitor < 0), just
 * like dwm's arrange(): tiled windows per layout, floating/scratchpad windows at float_geo,
 * fullscreen windows at output_rect; Floating layout keeps manual geometry. Then every visible
 * window gets purp_window_configure(focused == index, layout != Floating), and windows on hidden
 * tags get `want.activated = false`.
 */
void purp_workspace_arrange_in(const purp_workspace_t *ws, purp_window_t *const *windows,
                               size_t count, int focused_idx, purp_rect_t output_rect,
                               int monitor);

/* =========================================================================================== */
/* Pure layout math                                                                           */
/* =========================================================================================== */

/* Split `total` into `count` parts separated by `gap`; parts + gaps sum to total. Gaps shrink
 * (gap = max(total - count, 0) / (count - 1)) when they would not fit; remainder pixels go to the
 * first parts. Writes `count` lengths and returns the gap actually used (0 for count <= 1). */
int32_t purp_split_lengths(int32_t total, size_t count, int32_t gap, int32_t *lengths);
/* Like purp_split_lengths but each share follows its weight (weights < 0.05 count as 0.05;
 * equal weights give exactly purp_split_lengths). Cumulative rounding keeps the span exact. */
int32_t purp_split_weighted(int32_t total, const float *weights, size_t count, int32_t gap,
                            int32_t *lengths);
/* sum(lengths) + gap * (count - 1). */
int32_t purp_split_span(const int32_t *lengths, size_t count, int32_t gap);
/* (master, stack, gap) widths adding up to usable_w; mfact clamped 0.10..0.90. */
void purp_master_stack_widths(int32_t usable_w, int32_t gap, float mfact, int32_t *master,
                              int32_t *stack, int32_t *gap_out);
/* (left, master, right, gap) for centred master with stacks on both sides:
 * left + gap + master + gap + right == usable_w. */
void purp_centered_side_widths(int32_t usable_w, int32_t gap, float mfact, int32_t *left,
                               int32_t *master, int32_t *right, int32_t *gap_out);
/* Centred scratchpad frame: 70% of the output (at least 480x320) inside output_rect, below the
 * bar when shown, with an 8px margin, never larger than the output. */
purp_rect_t purp_scratchpad_geo_in(purp_rect_t output_rect, bool show_bar);
/* Floating layout keeps float_geo only for actual floats and scratchpads. */
purp_rect_t purp_floating_layout_geo(purp_rect_t geo, purp_rect_t float_geo, bool is_floating,
                                     bool is_scratchpad);
/* Resize `start` by (dx, dy) at `edges` (purp_edges_t), keeping the opposite edge anchored for
 * left/top drags and honouring min_w/min_h. */
purp_rect_t purp_resize_rect(purp_rect_t start, int32_t dx, int32_t dy, uint32_t edges,
                             int32_t min_w, int32_t min_h);
/* Focus index after Mod+Shift+Return swapped `target` with `master`. */
size_t purp_focus_after_zoom(size_t master, size_t focused, size_t target);
/* Keep a floating frame recoverable inside one output: at least PURP_VISIBLE_STRIP px visible
 * horizontally, top edge below the bar (when shown) and above the bottom edge. */
purp_point_t purp_clamp_floating_loc_in(purp_point_t loc, purp_size_t size, purp_rect_t output_rect,
                                        bool show_bar);
/* Fit a floating frame (size at least 1x1, at most the area below the bar) inside the output. */
purp_rect_t purp_clamp_floating_geo_in(purp_rect_t geo, purp_rect_t output_rect, bool show_bar);
/* Physical -> logical output size: round(physical / max(scale, 0.25)), at least 320x240. */
purp_size_t purp_logical_output_size_for(purp_size_t physical, double scale);
/* Raw physical window coordinates -> logical, clamped into [0, size - 1e-4]
 * (scale floored at 0.1). */
purp_pointf_t purp_touch_physical_to_logical(double phys_x, double phys_y, double scale,
                                             purp_size_t output_size);

/* =========================================================================================== */
/* Tiled resize planning (Mod+Right-drag and Mod+Shift+Arrow on tiled windows)                */
/* =========================================================================================== */

/* The master/stack divider a tiled resize moves. */
typedef struct purp_mfact_split {
    float start;      /* mfact when the resize started */
    float per_px;     /* mfact change per logical px the divider moves right (signed) */
} purp_mfact_split_t;

/* The divider between two neighbours in one tiled column. `upper`/`lower` are the caller's keys:
 * entry indices for purp_workspace_plan_tiled_resize_in(), window ids for
 * purp_wm_tiled_resize_plan(). */
typedef struct purp_row_split {
    uint64_t upper;
    uint64_t lower;
    int32_t upper_h;
    int32_t lower_h;
    float px_per_cfact;   /* pixels of height per unit of cfact in this column */
} purp_row_split_t;

typedef struct purp_tiled_resize {
    int monitor;                 /* monitor whose workspace owns the splits */
    bool has_mfact;
    purp_mfact_split_t mfact;
    bool has_rows;
    purp_row_split_t rows;
} purp_tiled_resize_t;

/* One tiled window for planning: caller key, frame and cfact. */
typedef struct purp_tiled_entry {
    uint64_t key;
    purp_rect_t geo;
    float cfact;
} purp_tiled_entry_t;

bool purp_tiled_resize_is_empty(const purp_tiled_resize_t *plan);
/* (upper, lower) cfacts once the divider moved `dy` px down; each row keeps >= MIN_TILED_ROW. */
void purp_row_split_cfacts(const purp_row_split_t *rows, int32_t dy, float *upper, float *lower);
/*
 * Plan a resize of tiled entry `target_key` grabbed at `edges` within `output_rect`:
 * horizontal edges move the master/stack divider (Tile/CenteredMaster only; the grabbed edge is
 * preferred, the other side is used at the screen edge; centred sides move symmetrically),
 * vertical edges move the divider to the neighbour in the same column (bottom prefers the row
 * below). Monocle/Floating and edge-less requests produce an empty plan. `monitor` is copied
 * from the argument.
 */
purp_tiled_resize_t purp_workspace_plan_tiled_resize_in(const purp_workspace_t *ws,
                                                        purp_rect_t output_rect,
                                                        const purp_tiled_entry_t *tiled,
                                                        size_t count, uint64_t target_key,
                                                        uint32_t edges, int monitor);

/* =========================================================================================== */
/* Monitors                                                                                   */
/* =========================================================================================== */

/* Description of a connector / virtual output as discovered by a backend. */
typedef struct purp_monitor_desc {
    char name[PURP_MONITOR_NAME_MAX];          /* "eDP-1", "HDMI-A-1", "purpwm+-eDP-1" ... */
    char description[PURP_MONITOR_DESC_MAX];   /* "<name> (Internal Display)" etc. */
    char make[32];                             /* "DRM" / "purpwm+" */
    char model[PURP_MONITOR_NAME_MAX];
    purp_size_t physical_size;                 /* current mode, px */
    int32_t refresh_mhz;
    purp_size_t physical_size_mm;              /* 0x0 when unknown */
    uint32_t subpixel;                         /* wl_output.subpixel value (0 = unknown) */
    double scale;                              /* initial scale */
    bool has_position;                         /* place at `position`, else right of the rest */
    purp_point_t position;
    bool is_internal;                          /* eDP/LVDS/DSI: sorted first, default scale */
    bool is_virtual;
    purp_display_mode_t modes[PURP_MAX_MODES];
    size_t mode_count;
} purp_monitor_desc_t;

/* Rust MonitorState: one output and its independent workspace. */
typedef struct purp_monitor {
    char name[PURP_MONITOR_NAME_MAX];
    char description[PURP_MONITOR_DESC_MAX];
    char make[32];
    char model[PURP_MONITOR_NAME_MAX];
    bool enabled;
    bool primary;
    purp_size_t physical_size;       /* current mode in px */
    int32_t refresh_mhz;
    purp_size_t physical_size_mm;
    uint32_t subpixel;
    double scale;
    purp_point_t loc;                /* logical origin in the global desktop */
    purp_size_t logical_size;        /* purp_logical_output_size_for(physical_size, scale) */
    purp_display_mode_t modes[PURP_MAX_MODES];
    size_t mode_count;
    bool is_internal;
    bool is_virtual;
    purp_workspace_t workspace;
    purp_output_t *output;           /* wl_output global (server-owned; NULL while disabled) */
    void *backend_data;              /* backend per-connector state (moves with the monitor) */
} purp_monitor_t;

/* Monitor rectangle in global logical coordinates. */
static inline purp_rect_t purp_monitor_rect(const purp_monitor_t *mon) {
    return purp_rect_make(mon->loc.x, mon->loc.y, mon->logical_size.w, mon->logical_size.h);
}

/* Keep a floating frame recoverable across the whole desktop: a PURP_VISIBLE_STRIP px strip and
 * its titlebar anchor (loc.y + 16) stay on some enabled monitor; frames that straddle a seam are
 * left alone. */
purp_point_t purp_clamp_floating_loc_across_monitors(purp_point_t loc, purp_size_t size,
                                                     const purp_monitor_t *monitors, size_t count);
/*
 * Move the pointer by `delta` from `from` across enabled monitors without snagging on corners:
 * monitors are half-open [x0, x0 + w), so a pointer at x = x0 + w - 1e-4 crosses into the
 * neighbour starting at x0 + w; positions outside every monitor are clamped (inset 1e-4) to the
 * nearest one, axis by axis, so sub-pixel motion along a seam keeps its fractional part.
 */
purp_pointf_t purp_clamp_pointer_to_monitors(const purp_monitor_t *monitors, size_t count,
                                             purp_pointf_t from, purp_pointf_t delta);
/* Preferred native resolution: `names[i]` are /sys/class/drm connector names, `modes[i]` their
 * first mode line ("WxH"); modes below 640x480 are ignored; internal panels (edp/lvds/dsi in the
 * lowercased name) win over the first external. */
bool purp_pick_preferred_mode(const char *const *names, const char *const *modes, size_t count,
                              uint32_t *width, uint32_t *height);
/* Scan /sys/class/drm/<connector>/{status,modes} (sorted, "connected" only) and apply
 * purp_pick_preferred_mode; falls back to 2560x1600. */
void purp_detect_preferred_resolution(uint32_t *width, uint32_t *height);
/* Mode list for a virtual output: `primary` first (preferred, at least 640x480), then the stock
 * table 3840x2160@60, 2560x1600@120, 2560x1440@144, 1920x1200@60, 1920x1080@120, 1920x1080@60,
 * 1600x900@60, 1280x720@60, skipping duplicates (same size, |dHz| < 0.5). Returns the count. */
size_t purp_default_virtual_modes(purp_size_t primary, int32_t refresh_mhz,
                                  purp_display_mode_t *out, size_t cap);
/* Parse PURPWM_VIRTUAL_MONITORS ("name:WxH[@HZ][:SCALE],..."): items trimmed, empty ones
 * skipped, at least name and WxH required, WxH >= 320x240, "@HZ" missing => 120 Hz (unparsable
 * => 60), scale default 1.0 clamped 0.5..4.0. Monitors are laid out left to right; the first is
 * primary; description "<name> (Virtual Display)", make "purpwm+", modes from
 * purp_default_virtual_modes. Returns the count written (<= cap). */
size_t purp_parse_virtual_monitors(const char *spec, purp_monitor_desc_t *out, size_t cap);
/* Name comparison tolerant of a nested "purpwm+-" / "purpwm-" prefix on either side. */
bool purp_monitor_name_matches(const char *monitor_name, const char *wanted);

/* =========================================================================================== */
/* The WM model                                                                               */
/* =========================================================================================== */

typedef enum purp_scratchpad_result {
    PURP_SCRATCHPAD_SHOWN = 0,   /* existing scratchpad shown on the active monitor */
    PURP_SCRATCHPAD_HIDDEN,      /* existing scratchpad hidden */
    PURP_SCRATCHPAD_NEEDS_SPAWN, /* none exists: caller spawns a terminal; the next mapped window
                                    becomes the scratchpad (pending_scratchpad_spawn) */
} purp_scratchpad_result_t;

typedef struct purp_wm {
    purp_monitor_t monitors[PURP_MAX_MONITORS];
    size_t monitor_count;
    int focused_monitor;                /* index into monitors (always enabled when any is) */
    bool monitors_dirty;                /* backend must re-apply modes / outputs republished */

    purp_window_t **windows;            /* newest first */
    size_t window_count;
    size_t window_cap;
    int focused_idx;                    /* index into windows, -1 = none */
    uint64_t next_window_id;

    purp_pointf_t pointer_location;     /* global logical pointer position (single source) */
    bool pending_scratchpad_spawn;      /* next new toplevel becomes the scratchpad */
    double default_scale;               /* scale for new internal/HiDPI monitors */
} purp_wm_t;

void purp_wm_init(purp_wm_t *wm, double default_scale);
/* Destroy all windows (purp_window_finish + free) and reset. */
void purp_wm_finish(purp_wm_t *wm);

/* ---- monitors ---------------------------------------------------------------------------- */

/* Append a monitor from `desc` (enabled; primary when it is the first; scale = desc->scale;
 * placed at desc->position or right of the rightmost enabled monitor). Returns its index, or -1
 * when PURP_MAX_MONITORS are tracked. A known name updates modes/description and returns the
 * existing index instead. Sets monitors_dirty. */
int purp_wm_add_monitor(purp_wm_t *wm, const purp_monitor_desc_t *desc);
/* Index of the monitor called `name` (prefix-tolerant), -1 if none. */
int purp_wm_find_monitor(const purp_wm_t *wm, const char *name);
/* Focused enabled monitor (falls back to primary, then 0). */
size_t purp_wm_active_monitor_index(const purp_wm_t *wm);
/* First enabled primary monitor (falls back to the first enabled, then 0). */
size_t purp_wm_primary_monitor_index(const purp_wm_t *wm);
/* Enabled monitors ordered left-to-right, then top-to-bottom. Returns the count. */
size_t purp_wm_enabled_monitor_order(const purp_wm_t *wm, size_t *out, size_t cap);
purp_rect_t purp_wm_monitor_rect(const purp_wm_t *wm, size_t idx);
double purp_wm_monitor_scale(const purp_wm_t *wm, size_t idx);
/* Enabled monitor containing `pt` (half-open), -1 if none. */
int purp_wm_monitor_at(const purp_wm_t *wm, purp_pointf_t pt);
/* Enabled monitor nearest to `pt` (distance to the rectangle). */
size_t purp_wm_nearest_enabled_monitor(const purp_wm_t *wm, purp_pointf_t pt);
/* Bounding box of all enabled monitors (at least 1x1). */
purp_rect_t purp_wm_desktop_bounds(const purp_wm_t *wm);
/* Workspace of the active monitor. */
purp_workspace_t *purp_wm_active_workspace(purp_wm_t *wm);
/* Neighbour of `src` in direction (dx, dy) (one of them non-zero): the enabled monitor whose edge
 * touches src's opposite edge with the largest overlap; -1 if none. */
int purp_wm_adjacent_monitor(const purp_wm_t *wm, size_t src, int dx, int dy);
/* Global touch/absolute position for a nested window's raw physical coordinates mapped onto the
 * whole virtual desktop (PURPWM_VIRTUAL_MONITORS) and clamped inside it. */
purp_pointf_t purp_wm_touch_virtual_to_logical(const purp_wm_t *wm, double phys_x, double phys_y,
                                               purp_size_t host_physical_size);
/* Touch device -> monitor: the monitor named `output_name` (prefix-tolerant; NULL allowed),
 * else the primary monitor. */
size_t purp_wm_resolve_touch_monitor(const purp_wm_t *wm, const char *output_name);

/* Snapshot of every monitor for `get_monitors`. Returns the count written. */
size_t purp_wm_monitor_infos(const purp_wm_t *wm, purp_monitor_info_t *out, size_t cap);
/* Current monitors as monitors.json entries. Returns the count written. */
size_t purp_wm_monitor_configs(const purp_wm_t *wm, purp_monitor_config_t *out, size_t cap);
/*
 * Rust normalize_and_sync_monitors (model part): enable the first monitor if none is, shift
 * enabled monitors (and every window frame and the pointer) so the bounding box starts at (0,0),
 * keep exactly one enabled primary, recompute logical sizes, evacuate windows on disabled
 * monitors to the primary (taking its selected tags unless scratchpad), keep focused_monitor
 * enabled, clamp the pointer and floating windows. Sets monitors_dirty. The server then
 * republishes wl_outputs and moves layer surfaces (purp_server_monitors_changed).
 */
void purp_wm_normalize_monitors(purp_wm_t *wm);
/*
 * Apply `apply_monitors` / monitors.json entries. Errors: PURP_IPC_ERR_EMPTY_CONFIG,
 * PURP_IPC_ERR_ALL_DISABLED (previewed before anything changes). Per matching monitor: enabled,
 * primary (only when enabled), scale rounded to 0.05 and clamped to [SCALE_MIN, SCALE_MAX], loc,
 * and for WxH >= 320x240 the listed mode with the nearest refresh — or, with
 * `allow_custom_modes` (nested/headless), a custom mode with refresh clamped 24000..500000 that is
 * appended to the mode list. Windows follow moved monitors, then normalize + relayout.
 * Unknown names are ignored.
 */
bool purp_wm_apply_monitor_configs(purp_wm_t *wm, const purp_monitor_config_t *entries,
                                   size_t count, bool allow_custom_modes, char *err,
                                   size_t err_len);
/*
 * Drop monitors whose names are not in `live_names` (hot-unplug), compacting the array without
 * disturbing survivors' workspaces. Windows of removed monitors move to the primary monitor at the
 * same offset (then clamped); focused_monitor follows its monitor or falls back to the primary.
 * `old_to_new` (capacity PURP_MAX_MONITORS) receives the new index of every old monitor or -1.
 * Returns the number removed.
 */
size_t purp_wm_retain_connected_monitors(purp_wm_t *wm, const char *const *live_names,
                                         size_t live_count, int *old_to_new);
/* Nested backend: the host window resized or a virtual output changed mode. */
void purp_wm_set_monitor_physical(purp_wm_t *wm, size_t idx, purp_size_t physical,
                                  int32_t refresh_mhz);
/* Set one monitor's scale (clamped), recompute its logical size and shift right/bottom
 * neighbours that were touching it (within 2 px) so the desktop stays contiguous. */
void purp_wm_set_monitor_scale(purp_wm_t *wm, size_t idx, double scale);
/* Mod+Ctrl+= / Mod+Ctrl+-: active monitor scale rounded to SCALE_STEP after adding `delta`,
 * clamped; returns the new scale. Caller persists it (scale file + monitors.json). */
double purp_wm_adjust_scale(purp_wm_t *wm, double delta);

/* ---- windows ----------------------------------------------------------------------------- */

/* Create and insert a window at index 0 (newest) on the active monitor with its selected tags,
 * focus it, and — when pending_scratchpad_spawn — turn it into the shown scratchpad at
 * purp_scratchpad_geo_in(). Returns NULL on allocation failure. */
purp_window_t *purp_wm_window_create(purp_wm_t *wm, purp_toplevel_t *toplevel);
/* Remove and free a window; focus moves to a visible window on the same monitor. */
void purp_wm_window_destroy(purp_wm_t *wm, purp_window_t *win);
int purp_wm_window_index(const purp_wm_t *wm, const purp_window_t *win);
int purp_wm_window_index_by_id(const purp_wm_t *wm, uint64_t id);
purp_window_t *purp_wm_window_by_id(const purp_wm_t *wm, uint64_t id);
/* Focused window or NULL. */
purp_window_t *purp_wm_focused_window(const purp_wm_t *wm);
/* Focus window `idx` (-1 clears) and make its monitor the focused monitor. */
void purp_wm_focus_window(purp_wm_t *wm, int idx);
/* Visible on its monitor's selected tags (and the monitor is enabled). */
bool purp_wm_is_window_visible(const purp_wm_t *wm, const purp_window_t *win);
/* Keep focus valid: when the focused window is gone or hidden, focus the first visible window on
 * the active monitor (never jumps monitors), else none. */
void purp_wm_ensure_valid_focus(purp_wm_t *wm);
/* Evaluate WINDOW_RULES for window `idx` once its strings settle; floated windows become
 * self-sized (size_from_client) and are cascaded. Returns true when something changed. */
bool purp_wm_apply_window_rules(purp_wm_t *wm, size_t idx);
/*
 * Full relayout: rules for unsettled windows, arrange every enabled monitor with its workspace
 * (purp_workspace_arrange_in), keep a dragged float at float_geo, ensure valid focus. The server
 * wraps this with focus sync, configure flushing and damage (purp_server_relayout).
 */
void purp_wm_relayout(purp_wm_t *wm);
/*
 * Visible windows top-most first (single source of truth for rendering and hit-testing): the
 * focused window first if it is fullscreen, floating or a scratchpad; then fullscreen windows;
 * then floating/scratchpad windows (focused first); then tiled windows (focused first).
 */
size_t purp_wm_stacking_order(const purp_wm_t *wm, size_t *out, size_t cap);
/* Top-most visible window whose frame contains `pt`, -1 if none. */
int purp_wm_window_at(const purp_wm_t *wm, purp_pointf_t pt);
/* Mod+j / Mod+k: next/previous visible window on the active monitor. */
void purp_wm_cycle_focus(purp_wm_t *wm, int delta);
/* Make the active monitor the one containing `pt`; true when it changed. */
bool purp_wm_focus_monitor_at_point(purp_wm_t *wm, purp_pointf_t pt);
/* Mod+, / Mod+.: focus the previous/next enabled monitor (left-to-right order). */
void purp_wm_cycle_monitor(purp_wm_t *wm, int delta);
/* Mod+Shift+, / Mod+Shift+.: send the focused window to the previous/next monitor and follow. */
void purp_wm_move_focused_window_to_monitor(purp_wm_t *wm, int delta);
/* Move window `win_idx` to monitor `target` (tags = target's selected tags unless scratchpad;
 * floats keep their offset inside the monitor, clamped); `follow` focuses it there. */
void purp_wm_send_window_to_monitor(purp_wm_t *wm, size_t win_idx, size_t target, bool follow);

/* Prepare `idx` for an interactive move: a tiled window outside the Floating layout becomes
 * floating at its current frame. Returns false for fullscreen/missing windows. */
bool purp_wm_floating_grab_geometry(purp_wm_t *wm, size_t idx, purp_rect_t *out);
/* Mod+Arrow: floats move PURP_MOVE_STEP px (across monitors, clamped); tiled windows swap with
 * their neighbour in that direction, or hop to the adjacent monitor at the stack edge. */
void purp_wm_move_focused_window(purp_wm_t *wm, int dx, int dy);
/* Grow/shrink window `idx` by (dx, dy) px in the arrow's direction: tiled windows move the
 * matching splits (purp_wm_tiled_resize_plan); floats move the edge with a 120x80 minimum;
 * fullscreen windows are ignored. */
void purp_wm_resize_window_by(purp_wm_t *wm, size_t idx, int32_t dx, int32_t dy);
/* Plan a tiled resize of window `idx` at `edges`; row keys are window ids. False when empty. */
bool purp_wm_tiled_resize_plan(const purp_wm_t *wm, size_t idx, uint32_t edges,
                               purp_tiled_resize_t *out);
/* Move the planned splits by (dx, dy) px from their start (absolute, not incremental) and
 * re-arrange the plan's monitor. */
void purp_wm_apply_tiled_resize(purp_wm_t *wm, const purp_tiled_resize_t *plan, int32_t dx,
                                int32_t dy);
/* Mod+Shift+Return: swap the focused tiled window with the master (or the master with the next
 * one when it already is the master). */
void purp_wm_zoom_focused(purp_wm_t *wm);

/* ---- tags and layouts (the *_on variants address one monitor) ------------------------------ */

void purp_wm_view_tags_on(purp_wm_t *wm, size_t mon, uint16_t mask);
void purp_wm_view_tags(purp_wm_t *wm, uint16_t mask);
void purp_wm_toggle_view_tags_on(purp_wm_t *wm, size_t mon, uint16_t mask);
void purp_wm_toggle_view_tags(purp_wm_t *wm, uint16_t mask);
/* Mod+Tab: swap selected_tags and prev_tags on the active monitor. */
void purp_wm_swap_prev_tags(purp_wm_t *wm);
void purp_wm_tag_focused_window(purp_wm_t *wm, uint16_t mask);
void purp_wm_toggle_tag_focused_window(purp_wm_t *wm, uint16_t mask);
void purp_wm_set_layout_on(purp_wm_t *wm, size_t mon, purp_layout_t layout);
void purp_wm_set_layout(purp_wm_t *wm, purp_layout_t layout);
void purp_wm_cycle_layout(purp_wm_t *wm);         /* Mod+Ctrl+Space: purp_layout_next */
void purp_wm_toggle_last_layout(purp_wm_t *wm);   /* Mod+Space: previous layout */
void purp_wm_adjust_mfact(purp_wm_t *wm, float delta);
void purp_wm_adjust_nmaster(purp_wm_t *wm, int delta);
void purp_wm_adjust_gaps(purp_wm_t *wm, int32_t delta);
void purp_wm_reset_gaps(purp_wm_t *wm);
void purp_wm_toggle_gaps(purp_wm_t *wm);
void purp_wm_toggle_bar(purp_wm_t *wm);

/* ---- floating, fullscreen, opacity, scratchpad --------------------------------------------- */

/* Top-left for a new floating frame of `size` on monitor `mon`: centred, then cascaded by
 * PURP_CASCADE_STEP past other floats (skipping window `skip_idx`, -1 for none), clamped below
 * the bar. Oversized frames end at the monitor's left edge / bar bottom. */
purp_point_t purp_wm_cascade_loc_on(const purp_wm_t *wm, size_t mon, purp_size_t size,
                                    int skip_idx);
/* Re-clamp every float into the desktop after monitors changed. */
void purp_wm_clamp_floating_windows(purp_wm_t *wm);
/* After a commit of a self-sized float: adopt the client's drawn size (limited to the monitor
 * minus 32 px margins below the bar, at least 64), centring it the first time. True on change. */
bool purp_wm_adopt_client_size(purp_wm_t *wm, size_t idx);
void purp_wm_toggle_floating_focused(purp_wm_t *wm);
void purp_wm_toggle_fullscreen_focused(purp_wm_t *wm);
/* Client-driven fullscreen (xdg_toplevel.set_fullscreen/unset_fullscreen); `monitor` >= 0 moves
 * the window to that monitor first. */
void purp_wm_set_window_fullscreen(purp_wm_t *wm, size_t idx, bool fullscreen, int monitor);
/* Mod+o: toggle a 1.0 opacity override. */
void purp_wm_toggle_opacity_focused(purp_wm_t *wm);
/* Mod+s: show (on the active monitor, re-centred), hide, or ask for a spawn. */
purp_scratchpad_result_t purp_wm_toggle_scratchpad(purp_wm_t *wm);

/* ---- status bar ---------------------------------------------------------------------------- */

/* The bar shows when the workspace has show_bar and no visible fullscreen window covers it. */
bool purp_wm_bar_visible_on(const purp_wm_t *wm, size_t mon);
/* Monitor whose visible bar contains `pt`, with the logical x inside that monitor. */
bool purp_wm_bar_hit_monitor(const purp_wm_t *wm, purp_pointf_t pt, size_t *mon,
                             int32_t *local_x);
/*
 * Fill the bar inputs for monitor `mon`: selected tags and layout of its workspace; occupied =
 * OR of non-scratchpad window tags; urgent = OR of urgent window tags; focused = tags of the
 * focused window when it lives there. The title (written to `title_buf`) is the focused visible
 * window's (else the first visible one's), prefixed "[scratch] " for the scratchpad or
 * "[opaque] " for an opacity-1.0 override; with no window it is the hint
 * "<monitor> — Mod+h: Help | Tap Mod: Spotlight | Mod+Enter: Term | Mod+,/.: Monitor"
 * (several monitors) or
 * "purpwm+ — Mod+h: Help | Tap Mod: Spotlight | Mod+Enter: Term | Mod+w: Wi-Fi | Mod+Shift+b: BT".
 */
void purp_wm_bar_inputs(const purp_wm_t *wm, size_t mon, purp_bar_inputs_t *out,
                        char *title_buf, size_t title_len);

/* =========================================================================================== */
/* Keyboard glide (src/glide.c)                                                               */
/* =========================================================================================== */

/* Held arrow keys gliding a window edge. Times are CLOCK_MONOTONIC microseconds (purp_now_us). */
#define PURP_GLIDE_MAX_KEYS 8

typedef struct purp_glide_key {
    bool has_keycode;            /* false: a synthetic tap (released at once) */
    uint32_t keycode;            /* evdev keycode of the held arrow */
    int32_t dir_x;               /* unit direction */
    int32_t dir_y;
    uint64_t pressed_us;
    bool held;
    double travelled;            /* px covered so far */
} purp_glide_key_t;

typedef struct purp_key_glide {
    purp_glide_key_t keys[PURP_GLIDE_MAX_KEYS];
    size_t key_count;
    uint64_t last_tick_us;
    double carry_x;              /* sub-pixel motion carried to the next frame */
    double carry_y;
} purp_key_glide_t;

/* Edge speed (px/s) after holding for `held_secs`: MIN + (MAX - MIN) * t^2, t = held/RAMP in
 * [0, 1]. */
double purp_glide_speed(double held_secs);
void purp_key_glide_init(purp_key_glide_t *glide, uint64_t now_us);
/* Start gliding in (dir_x, dir_y). A held key with the same keycode is not added twice. Taps
 * (has_keycode false) glide exactly PURP_RESIZE_STEP px. */
void purp_key_glide_press(purp_key_glide_t *glide, bool has_keycode, uint32_t keycode,
                          int32_t dir_x, int32_t dir_y, uint64_t now_us);
/* The key came up: it finishes its tap distance (RESIZE_STEP), then stops. */
void purp_key_glide_release(purp_key_glide_t *glide, uint32_t keycode);
void purp_key_glide_release_all(purp_key_glide_t *glide);
bool purp_key_glide_is_done(const purp_key_glide_t *glide);
/* Whole px (dx, dy) to move since the previous call. dt is capped at
 * PURP_GLIDE_MAX_FRAME_SECS; the sub-pixel rest carries over (rounded once the last key ends). */
purp_point_t purp_key_glide_advance(purp_key_glide_t *glide, uint64_t now_us);

#endif /* PURPWM_WM_H */
