/*
 * purpwm+ — deterministic C11 unit & regression tests.
 *
 * Links only the pure modules (wm, glide, ipc, capture-side helpers, cursor, wallpaper, font,
 * bar, lock, notify, help): no Wayland, no EGL, no DRM. Every test prints PASS/FAIL and the
 * process exits 0 only when all of them pass. Port of purpwm's Rust tests.rs to the C API.
 */
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#include <xkbcommon/xkbcommon-keysyms.h>

#include <gio/gio.h>

#include "config.h"
#include "ipc.h"
#include "types.h"
#include "ui.h"
#include "wm.h"

/* =========================================================================================== */
/* Harness                                                                                     */
/* =========================================================================================== */

static int g_failures;
static const char *g_test_name;

#define CHECK(cond)                                                                            \
    do {                                                                                       \
        if (!(cond)) {                                                                         \
            ++g_failures;                                                                      \
            fprintf(stderr, "  FAIL %s:%d: %s\n", g_test_name, __LINE__, #cond);               \
        }                                                                                      \
    } while (0)

#define CHECK_STREQ(got, want)                                                                 \
    do {                                                                                       \
        const char *g_ = (got);                                                                \
        if (!g_ || strcmp(g_, (want)) != 0) {                                                  \
            ++g_failures;                                                                      \
            fprintf(stderr, "  FAIL %s:%d: \"%s\" != \"%s\"\n", g_test_name, __LINE__,         \
                    g_ ? g_ : "(null)", (want));                                               \
        }                                                                                      \
    } while (0)

typedef void (*purp_test_fn)(void);

typedef struct purp_test_case {
    const char *name;
    purp_test_fn fn;
} purp_test_case_t;

static char g_tmp_dir[256];

static bool test_make_tmp_dir(void) {
    snprintf(g_tmp_dir, sizeof g_tmp_dir, "/tmp/purpwm-test-%ld", (long)getpid());
    return mkdir(g_tmp_dir, 0700) == 0 || errno == EEXIST;
}

static bool test_write_file(const char *path, const char *text) {
    FILE *f = fopen(path, "w");
    if (!f) return false;
    bool ok = fputs(text, f) >= 0;
    if (fclose(f) != 0) ok = false;
    return ok;
}

static int test_unix_connect(const char *path) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_un addr = { 0 };
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof addr.sun_path, "%s", path);
    if (connect(fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static bool test_write_all(int fd, const char *data, size_t len) {
    while (len > 0) {
        ssize_t n = write(fd, data, len);
        if (n <= 0) return false;
        data += n;
        len -= (size_t)n;
    }
    return true;
}

/* Accumulate bytes until `needle` appears in the buffer or `timeout_ms` passes. */
static ssize_t test_read_until(int fd, char *buf, size_t cap, int timeout_ms,
                               const char *needle) {
    size_t total = 0;
    uint64_t deadline = purp_now_ms() + (uint64_t)timeout_ms;
    for (;;) {
        if (needle && total > 0) {
            buf[total] = '\0';
            if (strstr(buf, needle)) break;
        }
        uint64_t now = purp_now_ms();
        if (now >= deadline || total >= cap - 1) break;
        struct pollfd pfd = { .fd = fd, .events = POLLIN };
        int r = poll(&pfd, 1, (int)(deadline - now));
        if (r <= 0) break;
        ssize_t n = read(fd, buf + total, cap - 1 - total);
        if (n <= 0) break;
        total += (size_t)n;
    }
    buf[total] = '\0';
    return (ssize_t)total;
}

static purp_monitor_desc_t test_monitor_desc(const char *name, int32_t w, int32_t h,
                                             int32_t refresh_mhz, double scale, int32_t x,
                                             int32_t y) {
    purp_monitor_desc_t d = { 0 };
    snprintf(d.name, sizeof d.name, "%s", name);
    snprintf(d.description, sizeof d.description, "%s (Test Display)", name);
    snprintf(d.make, sizeof d.make, "purpwm+");
    snprintf(d.model, sizeof d.model, "%s", name);
    d.physical_size = (purp_size_t){ w, h };
    d.refresh_mhz = refresh_mhz;
    d.scale = scale;
    d.has_position = true;
    d.position = (purp_point_t){ x, y };
    d.modes[0] = (purp_display_mode_t){ (uint32_t)w, (uint32_t)h, refresh_mhz, true };
    d.mode_count = 1;
    return d;
}

static purp_monitor_config_t test_monitor_cfg(const char *name, bool enabled, bool primary,
                                              uint32_t w, uint32_t h, int32_t refresh_mhz,
                                              double scale, int32_t x, int32_t y) {
    purp_monitor_config_t c = { 0 };
    snprintf(c.name, sizeof c.name, "%s", name);
    c.enabled = enabled;
    c.primary = primary;
    c.width = w;
    c.height = h;
    c.refresh_mhz = refresh_mhz;
    c.scale = scale;
    c.x = x;
    c.y = y;
    return c;
}

static purp_monitor_t test_monitor_state(const char *name, purp_size_t physical, double scale,
                                         purp_point_t loc, bool primary) {
    purp_monitor_t m = { 0 };
    snprintf(m.name, sizeof m.name, "%s", name);
    m.enabled = true;
    m.primary = primary;
    m.physical_size = physical;
    m.refresh_mhz = 60000;
    m.scale = scale;
    m.loc = loc;
    m.logical_size = purp_logical_output_size_for(physical, scale);
    return m;
}

/* =========================================================================================== */
/* Layouts, pertag workspaces and pure layout math                                             */
/* =========================================================================================== */

static void test_layout_cycle(void) {
    purp_layout_t mode = PURP_LAYOUT_TILE;
    CHECK_STREQ(purp_layout_symbol(mode), "[]=");
    mode = purp_layout_next(mode);
    CHECK(mode == PURP_LAYOUT_CENTERED_MASTER);
    CHECK_STREQ(purp_layout_symbol(mode), "|M|");
    mode = purp_layout_next(mode);
    CHECK(mode == PURP_LAYOUT_GRID);
    CHECK_STREQ(purp_layout_symbol(mode), "###");
    mode = purp_layout_next(mode);
    CHECK(mode == PURP_LAYOUT_MONOCLE);
    CHECK_STREQ(purp_layout_symbol(mode), "[M]");
    mode = purp_layout_next(mode);
    CHECK(mode == PURP_LAYOUT_FLOATING);
    CHECK_STREQ(purp_layout_symbol(mode), "><>");
    CHECK(purp_layout_next(mode) == PURP_LAYOUT_TILE);
}

static void test_workspace_pertag_and_gaps(void) {
    purp_workspace_t ws;
    purp_workspace_init(&ws);
    CHECK(ws.selected_tags == 1u << 0);
    CHECK(purp_workspace_layout(&ws) == PURP_LAYOUT_TILE);
    CHECK(ws.show_bar);
    CHECK(ws.gap_inner == PURP_GAP_INNER);
    CHECK(ws.gap_outer == PURP_GAP_OUTER);

    /* Change layout on tag 1; tag 2 keeps its own independent layout. */
    purp_workspace_set_layout(&ws, PURP_LAYOUT_GRID);
    CHECK(purp_workspace_layout(&ws) == PURP_LAYOUT_GRID);
    ws.selected_tags = 1u << 1;
    purp_workspace_sync_cur_tag(&ws);
    CHECK(ws.cur_tag == 1);
    CHECK(purp_workspace_layout(&ws) == PURP_LAYOUT_TILE);
    ws.selected_tags = 1u << 0;
    purp_workspace_sync_cur_tag(&ws);
    CHECK(ws.cur_tag == 0);
    CHECK(purp_workspace_layout(&ws) == PURP_LAYOUT_GRID);

    purp_workspace_adjust_gaps(&ws, 4);
    CHECK(ws.gap_inner == PURP_GAP_INNER + 4);
    purp_workspace_reset_gaps(&ws);
    CHECK(ws.gap_inner == PURP_GAP_INNER);
}

static void test_layout_splits_fit_the_usable_rect(void) {
    int32_t lengths[9];
    int32_t gap = purp_split_lengths(100, 3, 6, lengths);
    CHECK(purp_split_span(lengths, 3, gap) == 100);
    gap = purp_split_lengths(20, 5, 8, lengths);
    CHECK(purp_split_span(lengths, 5, gap) == 20);
    for (size_t i = 0; i < 5; ++i) CHECK(lengths[i] >= 0);

    int32_t master = 0, stack = 0, g = 0;
    purp_master_stack_widths(100, 8, 0.9f, &master, &stack, &g);
    CHECK(master + g + stack == 100);
    int32_t left = 0, right = 0;
    purp_centered_side_widths(100, 8, 0.9f, &left, &master, &right, &g);
    CHECK(left + g + master + g + right == 100);

    purp_rect_t pad = purp_scratchpad_geo_in(purp_rect_make(0, 0, 200, 180), true);
    CHECK(pad.x >= 0 && pad.y >= PURP_BAR_HEIGHT);
    CHECK(pad.x + pad.w <= 200 && pad.y + pad.h <= 180);
}

static void test_weighted_column_split_keeps_the_span(void) {
    int32_t even[3], plain[3];
    float w_even[3] = { 1.0f, 1.0f, 1.0f };
    int32_t g_even = purp_split_weighted(100, w_even, 3, 6, even);
    int32_t g_plain = purp_split_lengths(100, 3, 6, plain);
    CHECK(g_even == g_plain);
    for (size_t i = 0; i < 3; ++i) CHECK(even[i] == plain[i]);

    int32_t uneven[2];
    float w_uneven[2] = { 3.0f, 1.0f };
    int32_t g_uneven = purp_split_weighted(100, w_uneven, 2, 4, uneven);
    CHECK(purp_split_span(uneven, 2, g_uneven) == 100);
    CHECK(uneven[0] == 72 && uneven[1] == 24);
}

static void test_resizing_tiled_windows_moves_the_splits(void) {
    /* Usable area (10, 10) 980x950 with 10 px gaps. */
    purp_rect_t output = purp_rect_make(0, 0, 1000, 970);
    purp_workspace_t ws;
    purp_workspace_init(&ws);
    ws.show_bar = false;
    ws.gap_outer = 10;
    ws.gap_inner = 10;
    ws.gaps_enabled = true;

    /* `[]=` with a master and two stacked windows on the right. */
    purp_tiled_entry_t tiled[3] = {
        { 0, { 10, 10, 534, 950 }, 1.0f },
        { 1, { 554, 10, 436, 470 }, 1.0f },
        { 2, { 554, 490, 436, 470 }, 1.0f },
    };
    float start = purp_workspace_mfact(&ws);

    /* Dragging the master's right edge (or its screen edge) moves the master/stack divider. */
    for (int i = 0; i < 2; ++i) {
        uint32_t edges = i == 0 ? PURP_EDGE_RIGHT : PURP_EDGE_LEFT;
        purp_tiled_resize_t plan =
            purp_workspace_plan_tiled_resize_in(&ws, output, tiled, 3, 0, edges, 0);
        CHECK(plan.has_mfact);
        CHECK(fabs((double)plan.mfact.start + (double)plan.mfact.per_px * 97.0 -
                   ((double)start + 0.1)) < 1e-4);
    }
    /* The stack's left edge is the same divider, and it does not touch the rows. */
    purp_tiled_resize_t stack_left =
        purp_workspace_plan_tiled_resize_in(&ws, output, tiled, 3, 1, PURP_EDGE_LEFT, 0);
    CHECK(stack_left.has_mfact && !stack_left.has_rows);

    /* Keyboard resize (bottom-right edges) moves the divider the way the arrow points for
     * every window: `Right` (+x) pushes it right, i.e. raises mfact, even from the stack
     * window whose own right edge is the screen edge. */
    for (uint64_t key = 0; key < 3; ++key) {
        purp_tiled_resize_t plan = purp_workspace_plan_tiled_resize_in(
            &ws, output, tiled, 3, key, PURP_EDGE_BOTTOM_RIGHT, 0);
        CHECK(plan.has_mfact);
        CHECK(plan.mfact.per_px > 0.0f);
    }

    /* Bottom edge of the upper stack window: the row divider follows the pointer and the
     * windows keep their minimum height. */
    purp_tiled_resize_t rows =
        purp_workspace_plan_tiled_resize_in(&ws, output, tiled, 3, 1, PURP_EDGE_BOTTOM, 0);
    CHECK(rows.has_rows);
    CHECK(rows.rows.upper == 1 && rows.rows.lower == 2);
    float upper = 0, lower = 0;
    purp_row_split_cfacts(&rows.rows, 100, &upper, &lower);
    CHECK(fabs((double)upper * rows.rows.px_per_cfact - 570.0) < 0.01);
    CHECK(fabs((double)upper + lower - 2.0) < 1e-4);
    purp_row_split_cfacts(&rows.rows, 10000, &upper, &lower);
    CHECK(fabs((double)lower * rows.rows.px_per_cfact - PURP_MIN_TILED_ROW) < 0.01);
    /* The last window in a column has no lower neighbour: use the divider above it.
     * `Down` (+y) still moves that divider down, shrinking the last window. */
    purp_tiled_resize_t last =
        purp_workspace_plan_tiled_resize_in(&ws, output, tiled, 3, 2, PURP_EDGE_BOTTOM, 0);
    CHECK(last.has_rows);
    CHECK(last.rows.upper == 1 && last.rows.lower == 2);
    purp_row_split_cfacts(&last.rows, 40, &upper, &lower);
    CHECK(upper * last.rows.px_per_cfact > 470.0f && lower * last.rows.px_per_cfact < 470.0f);
    /* A lone column window has no row divider. */
    purp_tiled_resize_t lone =
        purp_workspace_plan_tiled_resize_in(&ws, output, tiled, 3, 0, PURP_EDGE_TOP, 0);
    CHECK(!lone.has_rows);

    /* `|M|` with stacks on both sides: the left divider grows the master when dragged left. */
    purp_workspace_set_layout(&ws, PURP_LAYOUT_CENTERED_MASTER);
    purp_tiled_entry_t centered[3] = {
        { 0, { 10, 10, 200, 950 }, 1.0f },
        { 1, { 220, 10, 560, 950 }, 1.0f },
        { 2, { 790, 10, 200, 950 }, 1.0f },
    };
    purp_tiled_resize_t lsplit = purp_workspace_plan_tiled_resize_in(
        &ws, output, centered, 3, 0, PURP_EDGE_RIGHT, 0);
    CHECK(lsplit.has_mfact && lsplit.mfact.per_px < 0.0f);
    purp_tiled_resize_t rsplit = purp_workspace_plan_tiled_resize_in(
        &ws, output, centered, 3, 1, PURP_EDGE_RIGHT, 0);
    CHECK(rsplit.has_mfact && rsplit.mfact.per_px > 0.0f);

    purp_workspace_set_layout(&ws, PURP_LAYOUT_MONOCLE);
    purp_tiled_resize_t monocle = purp_workspace_plan_tiled_resize_in(
        &ws, output, tiled, 3, 0, PURP_EDGE_BOTTOM_RIGHT, 0);
    CHECK(purp_tiled_resize_is_empty(&monocle));

    purp_workspace_set_layout(&ws, PURP_LAYOUT_TILE);
    purp_tiled_resize_t no_edges =
        purp_workspace_plan_tiled_resize_in(&ws, output, tiled, 3, 0, PURP_EDGE_NONE, 0);
    CHECK(purp_tiled_resize_is_empty(&no_edges));
}

static void test_floating_and_touch_helpers(void) {
    /* floating_layout_geo keeps float_geo only for actual floats and scratchpads. */
    purp_rect_t tiled = purp_rect_make(12, 40, 700, 800);
    purp_rect_t saved = purp_rect_make(120, 90, 500, 400);
    CHECK(purp_rect_eq(purp_floating_layout_geo(tiled, saved, false, false), tiled));
    CHECK(purp_rect_eq(purp_floating_layout_geo(tiled, saved, true, false), saved));
    CHECK(purp_rect_eq(purp_floating_layout_geo(tiled, saved, false, true), saved));

    /* Floating frames stay recoverable and fit resized outputs. */
    purp_rect_t output = purp_rect_make(0, 0, 800, 600);
    purp_size_t size = { 200, 100 };
    purp_point_t top_left = purp_clamp_floating_loc_in((purp_point_t){ -1000, -1000 }, size,
                                                       output, true);
    CHECK(top_left.x == 32 - size.w && top_left.y == PURP_BAR_HEIGHT);
    purp_point_t bottom_right =
        purp_clamp_floating_loc_in((purp_point_t){ 1000, 1000 }, size, output, true);
    CHECK(bottom_right.x == 800 - 32 && bottom_right.y == 600 - 32);
    purp_rect_t resized = purp_clamp_floating_geo_in(purp_rect_make(500, 500, 1200, 900), output,
                                                     true);
    CHECK(resized.w == 800 && resized.h == 600 - PURP_BAR_HEIGHT);
    CHECK(resized.x == 0 && resized.y == PURP_BAR_HEIGHT);

    /* Anchored edges while resizing + focus_after_zoom. */
    purp_rect_t r = purp_resize_rect(purp_rect_make(100, 100, 200, 200), 40, 0, PURP_EDGE_LEFT,
                                     80, 60);
    CHECK(r.x == 140 && r.w == 160 && r.x + r.w == 300);
    CHECK(purp_focus_after_zoom(0, 0, 2) == 0);
    CHECK(purp_focus_after_zoom(0, 2, 0) == 0);

    /* Touch physical -> logical: (900, 600) at 1.5x is (600, 400); outside clamps. */
    purp_size_t out_size = { 1600, 1000 };
    purp_pointf_t p = purp_touch_physical_to_logical(900.0, 600.0, 1.5, out_size);
    CHECK(fabs(p.x - 600.0) < 1e-6 && fabs(p.y - 400.0) < 1e-6);
    p = purp_touch_physical_to_logical(-25.0, 5000.0, 1.5, out_size);
    CHECK(p.x == 0.0 && fabs(p.y - (1000.0 - 1e-4)) < 1e-3);

    /* Preferred mode: internal panel wins; virtual-monitor spec parsing. */
    const char *names[] = { "card0-HDMI-A-1", "card0-eDP-1" };
    const char *modes[] = { "1920x1080", "2560x1600" };
    uint32_t w = 0, h = 0;
    CHECK(purp_pick_preferred_mode(names, modes, 2, &w, &h));
    CHECK(w == 2560 && h == 1600);
    CHECK(purp_pick_preferred_mode(names, modes, 1, &w, &h));
    CHECK(w == 1920 && h == 1080);
    purp_monitor_desc_t descs[4];
    size_t n = purp_parse_virtual_monitors("V-1:1920x1080@60:1.25, V-2:1280x720", descs, 4);
    CHECK(n == 2);
    CHECK_STREQ(descs[0].name, "V-1");
    CHECK(descs[0].physical_size.w == 1920 && descs[0].physical_size.h == 1080);
    CHECK(fabs(descs[0].scale - 1.25) < 1e-6 && descs[1].refresh_mhz == 120000);
    CHECK(descs[0].is_virtual && descs[0].mode_count >= 1);
}

/* =========================================================================================== */
/* Keyboard glide                                                                              */
/* =========================================================================================== */

static purp_point_t test_run_glide(purp_key_glide_t *glide, uint64_t *t, int frames,
                                   int32_t max_frame_px) {
    purp_point_t total = { 0, 0 };
    for (int i = 0; i < frames; ++i) {
        *t += 8000;
        purp_point_t d = purp_key_glide_advance(glide, *t);
        CHECK(labs(d.x) <= max_frame_px && labs(d.y) <= max_frame_px);
        total.x += d.x;
        total.y += d.y;
        if (purp_key_glide_is_done(glide)) break;
    }
    return total;
}

static void test_keyboard_resize_glides_smoothly(void) {
    CHECK(purp_glide_speed(0.0) == PURP_RESIZE_SPEED_MIN);
    CHECK(purp_glide_speed(PURP_RESIZE_RAMP_SECS) == PURP_RESIZE_SPEED_MAX);
    CHECK(purp_glide_speed(10.0) == PURP_RESIZE_SPEED_MAX);
    CHECK(purp_glide_speed(0.2) < purp_glide_speed(0.4));

    int32_t step = purp_round_i32(PURP_RESIZE_STEP);
    const uint64_t start = 1000000;

    /* A tap glides exactly one step in a few small frames, then stops. */
    uint64_t t = start;
    purp_key_glide_t tap;
    purp_key_glide_init(&tap, t);
    purp_key_glide_press(&tap, false, 0, -1, 0, t);
    purp_point_t moved = test_run_glide(&tap, &t, 100, 3);
    CHECK(moved.x == -step && moved.y == 0);
    CHECK(purp_key_glide_is_done(&tap));

    /* Releasing a key right after pressing it still finishes the step. */
    const uint32_t keycode = 116;
    t = start;
    purp_key_glide_t quick;
    purp_key_glide_init(&quick, t);
    purp_key_glide_press(&quick, true, keycode, 0, 1, t);
    t += 8000;
    int32_t first = purp_key_glide_advance(&quick, t).y;
    purp_key_glide_release(&quick, keycode);
    int32_t rest = test_run_glide(&quick, &t, 100, 3).y;
    CHECK(first + rest == step);
    CHECK(purp_key_glide_is_done(&quick));

    /* Holding keeps it moving and speeding up, never in big jumps; a repeated press of the
     * held key does not double the speed. Releasing stops it straight away. */
    t = start;
    purp_key_glide_t held;
    purp_key_glide_init(&held, t);
    purp_key_glide_press(&held, true, keycode, 1, 0, t);
    purp_key_glide_press(&held, true, keycode, 1, 0, t);
    int32_t early = test_run_glide(&held, &t, 25, 3).x;
    int32_t late = test_run_glide(&held, &t, 100, 13).x;
    CHECK(!purp_key_glide_is_done(&held));
    CHECK(early > step && late > early * 3);
    purp_key_glide_release(&held, keycode);
    t += 8000;
    CHECK(labs(purp_key_glide_advance(&held, t).x) <= 1);
    CHECK(purp_key_glide_is_done(&held));

    /* A stalled frame only credits a short slice of motion. */
    t = start;
    purp_key_glide_t stalled;
    purp_key_glide_init(&stalled, t);
    purp_key_glide_press(&stalled, true, keycode, 1, 0, t);
    t += 2000000;
    CHECK(purp_key_glide_advance(&stalled, t).x <= (int32_t)ceil(PURP_RESIZE_SPEED_MAX * 0.05));
}

/* =========================================================================================== */
/* Multi-monitor WM model                                                                      */
/* =========================================================================================== */

static void test_multi_monitor_workspaces_and_reconfiguration(void) {
    purp_wm_t wm;
    purp_wm_init(&wm, 1.0);
    purp_monitor_desc_t d0 = test_monitor_desc("eDP-1", 2560, 1600, 120000, 1.5, 0, 0);
    purp_monitor_desc_t d1 = test_monitor_desc("HDMI-A-1", 1920, 1080, 60000, 1.0, 1707, 0);
    CHECK(purp_wm_add_monitor(&wm, &d0) == 0);
    CHECK(purp_wm_add_monitor(&wm, &d1) == 1);
    purp_wm_normalize_monitors(&wm);
    CHECK(wm.monitor_count == 2);
    CHECK(wm.monitors[0].logical_size.w == 1707 && wm.monitors[0].logical_size.h == 1067);

    /* Per-monitor workspaces stay independent. */
    purp_wm_view_tags_on(&wm, 0, 1u << 1);
    CHECK(wm.monitors[0].workspace.selected_tags == 1u << 1);
    CHECK(wm.monitors[1].workspace.selected_tags == 1u << 0);
    purp_wm_cycle_monitor(&wm, 1);
    CHECK(wm.focused_monitor == 1);
    /* The empty monitor keeps focus; nothing forces focus back to monitor 0. */
    purp_wm_ensure_valid_focus(&wm);
    CHECK(wm.focused_monitor == 1);
    purp_wm_view_tags(&wm, 1u << 3); /* acts on the focused monitor (1) */
    CHECK(wm.monitors[1].workspace.selected_tags == 1u << 3);
    CHECK(wm.monitors[0].workspace.selected_tags == 1u << 1);

    /* Reject disabling every display. */
    char err[PURP_IPC_ERROR_MAX];
    purp_monitor_config_t off[2] = {
        test_monitor_cfg("eDP-1", false, false, 2560, 1600, 120000, 1.5, 0, 0),
        test_monitor_cfg("HDMI-A-1", false, false, 1920, 1080, 60000, 1.0, 1707, 0),
    };
    CHECK(!purp_wm_apply_monitor_configs(&wm, off, 2, false, err, sizeof err));

    /* Live-update eDP-1 to 1.25x (2560/1.25 = 2048x1280) and park HDMI-A-1 at (2048, 100). */
    purp_monitor_config_t on[2] = {
        test_monitor_cfg("eDP-1", true, true, 2560, 1600, 120000, 1.25, 0, 0),
        test_monitor_cfg("HDMI-A-1", true, false, 1920, 1080, 120000, 1.0, 2048, 100),
    };
    CHECK(purp_wm_apply_monitor_configs(&wm, on, 2, false, err, sizeof err));
    CHECK(wm.monitors[0].logical_size.w == 2048 && wm.monitors[0].logical_size.h == 1280);
    CHECK(wm.monitors[1].loc.x == 2048 && wm.monitors[1].loc.y == 100);
    purp_wm_finish(&wm);
}

static void test_moved_monitor_translates_floating_windows(void) {
    purp_wm_t wm;
    purp_wm_init(&wm, 1.0);
    purp_monitor_desc_t d0 = test_monitor_desc("eDP-1", 1920, 1080, 60000, 1.0, 0, 0);
    purp_monitor_desc_t d1 = test_monitor_desc("HDMI-A-1", 1920, 1080, 60000, 1.0, 1920, 0);
    purp_wm_add_monitor(&wm, &d0);
    purp_wm_add_monitor(&wm, &d1);
    purp_wm_normalize_monitors(&wm);

    purp_wm_view_tags_on(&wm, 1, 1u << 0);
    CHECK(wm.focused_monitor == 1); /* window_create lands on the focused monitor */
    purp_window_t *win = purp_wm_window_create(&wm, NULL);
    CHECK(win != NULL);
    win->is_floating = true;
    win->geo = purp_rect_make(2000, 100, 300, 200);
    win->float_geo = win->geo;

    /* Move HDMI-A-1 down-right: the floating window follows its monitor. */
    char err[PURP_IPC_ERROR_MAX];
    purp_monitor_config_t cfgs[2] = {
        test_monitor_cfg("eDP-1", true, true, 1920, 1080, 60000, 1.0, 0, 0),
        test_monitor_cfg("HDMI-A-1", true, false, 1920, 1080, 60000, 1.0, 2100, 200),
    };
    CHECK(purp_wm_apply_monitor_configs(&wm, cfgs, 2, false, err, sizeof err));
    CHECK(win->monitor == 1);
    CHECK(win->geo.x == 2000 + 180 && win->geo.y == 100 + 200);
    purp_wm_finish(&wm);
}

static void test_retain_connected_monitors_remaps_middle_unplug(void) {
    purp_wm_t wm;
    purp_wm_init(&wm, 1.0);
    purp_monitor_desc_t d0 = test_monitor_desc("eDP-1", 1920, 1080, 60000, 1.0, 0, 0);
    purp_monitor_desc_t d1 = test_monitor_desc("DP-1", 1920, 1080, 60000, 1.0, 1920, 0);
    purp_monitor_desc_t d2 = test_monitor_desc("HDMI-A-1", 1920, 1080, 60000, 1.0, 3840, 0);
    purp_wm_add_monitor(&wm, &d0);
    purp_wm_add_monitor(&wm, &d1);
    purp_wm_add_monitor(&wm, &d2);
    purp_wm_normalize_monitors(&wm);

    /* A window on the middle monitor (about to be unplugged). */
    purp_wm_view_tags_on(&wm, 1, 1u << 0);
    CHECK(wm.focused_monitor == 1);
    purp_window_t *mid = purp_wm_window_create(&wm, NULL);
    CHECK(mid != NULL);
    mid->is_floating = true;
    mid->geo = purp_rect_make(2000, 100, 300, 200);
    mid->float_geo = mid->geo;

    /* Focus HDMI-A-1 on tag 5. */
    purp_wm_view_tags_on(&wm, 2, 1u << 4);
    CHECK(wm.focused_monitor == 2);
    CHECK(wm.monitors[2].workspace.selected_tags == 1u << 4);

    /* Unplug the middle monitor. */
    const char *live[] = { "eDP-1", "HDMI-A-1" };
    int old_to_new[PURP_MAX_MONITORS];
    size_t removed = purp_wm_retain_connected_monitors(&wm, live, 2, old_to_new);
    purp_wm_normalize_monitors(&wm);
    CHECK(removed == 1);
    CHECK(old_to_new[0] == 0 && old_to_new[1] == -1 && old_to_new[2] == 1);
    CHECK(wm.monitor_count == 2);
    CHECK_STREQ(wm.monitors[0].name, "eDP-1");
    CHECK_STREQ(wm.monitors[1].name, "HDMI-A-1");
    CHECK(wm.focused_monitor == 1);
    CHECK(wm.monitors[1].workspace.selected_tags == 1u << 4);
    CHECK(purp_wm_active_workspace(&wm)->selected_tags == 1u << 4);
    CHECK(wm.monitors[0].workspace.selected_tags == 1u << 0);

    /* The orphaned window landed on the primary monitor, at the same offset, remapped. */
    CHECK(mid->monitor == 0);
    CHECK(mid->geo.x < 1920 && purp_wm_is_window_visible(&wm, mid));
    purp_wm_finish(&wm);
}

static void test_adjacent_monitors_and_straddling_floats(void) {
    purp_monitor_t monitors[2];
    monitors[0] = test_monitor_state("eDP-1", (purp_size_t){ 2560, 1600 }, 1.5,
                                   (purp_point_t){ 0, 0 }, true);
    monitors[1] = test_monitor_state("HDMI-A-1", (purp_size_t){ 1920, 1080 }, 1.0,
                                   (purp_point_t){ 1707, 0 }, false);

    purp_wm_t wm;
    purp_wm_init(&wm, 1.0);
    wm.monitors[0] = monitors[0];
    wm.monitors[1] = monitors[1];
    wm.monitor_count = 2;
    /* Directional adjacency between side-by-side monitors. */
    CHECK(purp_wm_adjacent_monitor(&wm, 0, 1, 0) == 1);
    CHECK(purp_wm_adjacent_monitor(&wm, 1, -1, 0) == 0);
    CHECK(purp_wm_adjacent_monitor(&wm, 0, -1, 0) == -1);

    /* A floating window halfway across the x=1707 seam stays untouched; a completely
     * off-screen window is pulled back. */
    purp_point_t straddling = purp_clamp_floating_loc_across_monitors(
        (purp_point_t){ 1500, 200 }, (purp_size_t){ 600, 400 }, wm.monitors, wm.monitor_count);
    CHECK(straddling.x == 1500 && straddling.y == 200);
    purp_point_t offscreen = purp_clamp_floating_loc_across_monitors(
        (purp_point_t){ 9000, 9000 }, (purp_size_t){ 600, 400 }, wm.monitors, wm.monitor_count);
    CHECK(offscreen.x < 3627 && offscreen.y < 1080);
    purp_wm_finish(&wm);
}

static void test_pointer_clamping_crosses_seams_without_snag(void) {
    /* eDP-1: 2560x1600 @ 1.5x -> logical 1707x1067; HDMI-A-1 directly to its right and
     * 13 logical px taller. */
    purp_monitor_t monitors[2];
    monitors[0] = test_monitor_state("eDP-1", (purp_size_t){ 2560, 1600 }, 1.5,
                                   (purp_point_t){ 0, 0 }, true);
    monitors[1] = test_monitor_state("HDMI-A-1", (purp_size_t){ 1920, 1080 }, 1.0,
                                   (purp_point_t){ 1707, 0 }, false);

    /* Crossing right-to-left from HDMI-A-1's bottom strip (y = 1075) into shorter eDP-1 must
     * slide smoothly into eDP-1's bottom edge instead of snagging on the corner. */
    purp_pointf_t crossed = purp_clamp_pointer_to_monitors(monitors, 2,
                                                           (purp_pointf_t){ 1710.0, 1075.0 },
                                                           (purp_pointf_t){ -12.0, 0.0 });
    CHECK(crossed.x < 1707.0);
    CHECK(crossed.y == 1066.0);

    /* Left->right across the shared y=500 seam preserves exact motion. */
    purp_pointf_t direct = purp_clamp_pointer_to_monitors(monitors, 2,
                                                          (purp_pointf_t){ 1700.0, 500.0 },
                                                          (purp_pointf_t){ 25.0, 4.0 });
    CHECK(direct.x == 1725.0 && direct.y == 504.0);

    /* Fractional ±0.3 touchpad deltas glide across the seam without a 1 px dead zone. */
    purp_pointf_t pos = { 1706.0, 500.0 };
    for (int i = 1; i <= 4; ++i) {
        pos = purp_clamp_pointer_to_monitors(monitors, 2, pos, (purp_pointf_t){ 0.3, 0.0 });
        CHECK(fabs(pos.x - (1706.0 + 0.3 * i)) < 1e-9);
        CHECK(pos.y == 500.0);
    }
    CHECK(pos.x > 1707.0 && fabs(pos.x - 1707.2) < 1e-9);
    pos = purp_clamp_pointer_to_monitors(monitors, 2, pos, (purp_pointf_t){ -0.3, 0.0 });
    CHECK(pos.x < 1707.0 && fabs(pos.x - 1706.9) < 1e-9);
    pos = purp_clamp_pointer_to_monitors(monitors, 2, pos, (purp_pointf_t){ -0.3, 0.0 });
    CHECK(fabs(pos.x - 1706.6) < 1e-9);

    /* The far outer edge clamps inside the monitor (half-open bounds). */
    purp_pointf_t edge =
        purp_clamp_pointer_to_monitors(monitors, 2, pos, (purp_pointf_t){ 10000.0, 0.0 });
    CHECK(edge.x < 1707.0 + 1920.0 && edge.x > 1707.0 + 1919.0);
}

/* Server toplevel sentinels are never dereferenced by the pure WM model. */
static purp_toplevel_t *test_server_toplevel(void) {
    return (purp_toplevel_t *)(uintptr_t)1;
}

static void test_window_wm_init(purp_wm_t *wm) {
    purp_wm_init(wm, 1.0);
    purp_monitor_desc_t desc = test_monitor_desc("WM-1", 1000, 800, 60000, 1, 0, 0);
    CHECK(purp_wm_add_monitor(wm, &desc) == 0);
    wm->monitors[0].workspace.gaps_enabled = false;
}

static void test_unmapped_toplevels_enter_and_leave_the_wm(void) {
    purp_wm_t wm;
    test_window_wm_init(&wm);
    purp_window_t *pending = purp_wm_window_create(&wm, test_server_toplevel());
    CHECK(pending != NULL);
    if (!pending) { purp_wm_finish(&wm); return; }
    CHECK(!pending->mapped && wm.focused_idx == -1);
    CHECK(!purp_wm_is_window_visible(&wm, pending));
    size_t indices[2] = {0};
    CHECK(purp_workspace_tiled_indices(&wm.monitors[0].workspace, wm.windows,
        wm.window_count, 0, indices, PURP_ARRAY_LEN(indices)) == 0);
    CHECK(purp_wm_stacking_order(&wm, indices, PURP_ARRAY_LEN(indices)) == 0);
    purp_wm_window_destroy(&wm, pending);

    purp_window_t *existing = purp_wm_window_create(&wm, NULL);
    CHECK(existing != NULL);
    if (!existing) { purp_wm_finish(&wm); return; }
    purp_wm_relayout(&wm);
    purp_rect_t existing_frame = existing->geo;
    pending = purp_wm_window_create(&wm, test_server_toplevel());
    CHECK(pending != NULL);
    if (!pending) { purp_wm_finish(&wm); return; }
    purp_wm_relayout(&wm);
    CHECK(purp_wm_focused_window(&wm) == existing);
    CHECK(wm.focused_idx == purp_wm_window_index(&wm, existing));
    CHECK(purp_rect_eq(existing->geo, existing_frame));
    CHECK(purp_workspace_tiled_indices(&wm.monitors[0].workspace, wm.windows,
        wm.window_count, 0, indices, PURP_ARRAY_LEN(indices)) == 1);
    CHECK(wm.windows[indices[0]] == existing);
    CHECK(purp_wm_stacking_order(&wm, indices, PURP_ARRAY_LEN(indices)) == 1);
    CHECK(wm.windows[indices[0]] == existing);

    purp_wm_on_window_mapped(&wm, (size_t)purp_wm_window_index(&wm, pending));
    purp_wm_relayout(&wm);
    CHECK(pending->mapped && purp_wm_focused_window(&wm) == pending);
    CHECK(purp_wm_is_window_visible(&wm, pending));
    CHECK(purp_workspace_tiled_indices(&wm.monitors[0].workspace, wm.windows,
        wm.window_count, 0, indices, PURP_ARRAY_LEN(indices)) == 2);
    CHECK(purp_wm_stacking_order(&wm, indices, PURP_ARRAY_LEN(indices)) == 2);
    CHECK(existing->geo.w < existing_frame.w && pending->geo.w > 0);

    purp_wm_on_window_unmapped(&wm, (size_t)purp_wm_window_index(&wm, pending));
    purp_wm_relayout(&wm);
    CHECK(!pending->mapped && purp_wm_focused_window(&wm) == existing);
    CHECK(purp_rect_eq(existing->geo, existing_frame));
    CHECK(purp_workspace_tiled_indices(&wm.monitors[0].workspace, wm.windows,
        wm.window_count, 0, indices, PURP_ARRAY_LEN(indices)) == 1);
    CHECK(wm.windows[indices[0]] == existing);
    CHECK(purp_wm_stacking_order(&wm, indices, PURP_ARRAY_LEN(indices)) == 1);
    CHECK(wm.windows[indices[0]] == existing);
    purp_wm_on_window_mapped(&wm, (size_t)purp_wm_window_index(&wm, pending));
    purp_wm_relayout(&wm);
    CHECK(pending->mapped && purp_wm_focused_window(&wm) == pending);
    CHECK(purp_workspace_tiled_indices(&wm.monitors[0].workspace, wm.windows,
        wm.window_count, 0, indices, PURP_ARRAY_LEN(indices)) == 2);
    purp_wm_finish(&wm);
}

static void test_maximize_geometry_configure_and_restore(void) {
    purp_wm_t wm;
    test_window_wm_init(&wm);
    purp_window_t *window = purp_wm_window_create(&wm, NULL);
    CHECK(window != NULL);
    if (!window) { purp_wm_finish(&wm); return; }
    purp_wm_relayout(&wm);
    purp_rect_t tiled_frame = window->geo;
    purp_rect_t usable = purp_workspace_usable_area_in(&wm.monitors[0].workspace,
        purp_wm_monitor_rect(&wm, 0));
    CHECK(usable.y > wm.monitors[0].loc.y);
    purp_wm_set_window_maximized(&wm, 0, true);
    CHECK(window->is_maximized && window->want.maximized && !window->want.tiled);
    CHECK(!window->is_fullscreen && purp_rect_eq(window->geo, usable));
    purp_size_t inner = purp_window_surface_size(window);
    CHECK(window->want.width == inner.w && window->want.height == inner.h);
    size_t indices[1];
    CHECK(purp_workspace_tiled_indices(&wm.monitors[0].workspace, wm.windows,
        wm.window_count, 0, indices, PURP_ARRAY_LEN(indices)) == 0);
    CHECK(purp_wm_bar_visible_on(&wm, 0));
    purp_wm_set_window_maximized(&wm, 0, false);
    CHECK(!window->is_maximized && !window->want.maximized && window->want.tiled);
    CHECK(purp_rect_eq(window->geo, tiled_frame));

    purp_wm_toggle_floating_focused(&wm);
    purp_rect_t floating_frame = purp_rect_make(120, 150, 420, 280);
    window->float_geo = window->geo = floating_frame;
    purp_wm_relayout(&wm);
    purp_wm_set_window_maximized(&wm, 0, true);
    CHECK(purp_rect_eq(window->geo, usable) && window->want.maximized);
    purp_wm_set_window_fullscreen(&wm, 0, true, -1);
    CHECK(window->is_fullscreen && window->is_maximized && window->want.fullscreen);
    CHECK(purp_rect_eq(window->geo, purp_wm_monitor_rect(&wm, 0)));
    purp_wm_set_window_fullscreen(&wm, 0, false, -1);
    CHECK(!window->is_fullscreen && window->is_maximized && window->want.maximized);
    CHECK(purp_rect_eq(window->geo, usable));
    purp_wm_set_window_maximized(&wm, 0, false);
    CHECK(window->is_floating && !window->is_maximized && !window->want.maximized);
    CHECK(purp_rect_eq(window->geo, floating_frame));
    CHECK(purp_rect_eq(window->float_geo, floating_frame));
    purp_wm_set_window_fullscreen(&wm, 0, true, -1);
    purp_wm_set_window_maximized(&wm, 0, true);
    CHECK(!window->is_fullscreen && !window->want.fullscreen && window->want.maximized);
    CHECK(purp_rect_eq(window->geo, usable));
    purp_wm_set_window_maximized(&wm, 0, false);
    CHECK(purp_rect_eq(window->geo, floating_frame));
    purp_wm_finish(&wm);
}

static void test_focusing_or_mapping_tiles_reconciles_covering_windows(void) {
    for (int maximize = 0; maximize < 2; ++maximize) {
        for (int map = 0; map < 2; ++map) {
            purp_wm_t wm;
            test_window_wm_init(&wm);
            purp_window_t *background = purp_wm_window_create(&wm, NULL);
            purp_window_t *foreground = map ? NULL : purp_wm_window_create(&wm, NULL);
            CHECK(background != NULL && (map || foreground != NULL));
            if (!background || (!map && !foreground)) { purp_wm_finish(&wm); continue; }
            purp_wm_focus_window(&wm, purp_wm_window_index(&wm, background));
            if (maximize)
                purp_wm_set_window_maximized(&wm,
                    (size_t)purp_wm_window_index(&wm, background), true);
            else
                purp_wm_set_window_fullscreen(&wm,
                    (size_t)purp_wm_window_index(&wm, background), true, -1);
            CHECK(maximize ? background->is_maximized : background->is_fullscreen);
            if (map) {
                foreground = purp_wm_window_create(&wm, test_server_toplevel());
                CHECK(foreground != NULL);
                if (!foreground) { purp_wm_finish(&wm); continue; }
                CHECK(purp_wm_focused_window(&wm) == background);
                CHECK(maximize ? background->is_maximized : background->is_fullscreen);
                purp_wm_on_window_mapped(&wm,
                    (size_t)purp_wm_window_index(&wm, foreground));
            } else {
                purp_wm_focus_window(&wm, purp_wm_window_index(&wm, foreground));
            }
            purp_wm_relayout(&wm);
            CHECK(purp_wm_focused_window(&wm) == foreground);
            CHECK(!background->is_fullscreen && !background->is_maximized);
            CHECK(!background->want.fullscreen && !background->want.maximized);
            CHECK(background->want.tiled && foreground->want.tiled);
            CHECK(!purp_rect_overlaps(background->geo, foreground->geo));
            size_t indices[2];
            CHECK(purp_workspace_tiled_indices(&wm.monitors[0].workspace, wm.windows,
                wm.window_count, 0, indices, PURP_ARRAY_LEN(indices)) == 2);
            CHECK(purp_wm_bar_visible_on(&wm, 0));
            purp_wm_finish(&wm);
        }
    }
}

static void test_transients_follow_and_stack_above_fullscreen_parent(void) {
    purp_wm_t wm;
    test_window_wm_init(&wm);
    purp_monitor_desc_t desc = test_monitor_desc("WM-2", 1200, 900, 60000, 1, 1000, 0);
    CHECK(purp_wm_add_monitor(&wm, &desc) == 1);
    wm.monitors[1].workspace.gaps_enabled = false;
    purp_wm_view_tags_on(&wm, 1, 1u << 3);
    purp_window_t *parent = purp_wm_window_create(&wm, NULL);
    CHECK(parent != NULL);
    if (!parent) { purp_wm_finish(&wm); return; }
    purp_wm_set_window_fullscreen(&wm, (size_t)purp_wm_window_index(&wm, parent), true, -1);
    purp_wm_view_tags_on(&wm, 0, 1);
    purp_window_t *child = purp_wm_window_create(&wm, test_server_toplevel());
    CHECK(child != NULL);
    if (!child) { purp_wm_finish(&wm); return; }
    CHECK(child->monitor == 0 && child->tags == 1);
    purp_wm_set_window_parent(&wm, (size_t)purp_wm_window_index(&wm, child), parent->id);
    purp_wm_on_window_mapped(&wm, (size_t)purp_wm_window_index(&wm, child));
    purp_wm_relayout(&wm);
    CHECK(child->parent_id == parent->id && child->is_floating);
    CHECK(child->monitor == parent->monitor && child->tags == parent->tags);
    CHECK(purp_wm_focused_window(&wm) == child && parent->is_fullscreen);
    CHECK(!child->want.tiled && parent->want.fullscreen);
    purp_rect_t output = purp_wm_monitor_rect(&wm, (size_t)parent->monitor);
    CHECK(child->geo.x >= output.x && child->geo.x + child->geo.w <= output.x + output.w);
    CHECK(child->geo.y >= output.y && child->geo.y + child->geo.h <= output.y + output.h);
    CHECK(abs((2 * child->geo.x + child->geo.w) - (2 * output.x + output.w)) <= 1);
    CHECK(abs((2 * child->geo.y + child->geo.h) - (2 * output.y + output.h)) <= 1);
    size_t order[2] = {0};
    CHECK(purp_wm_stacking_order(&wm, order, PURP_ARRAY_LEN(order)) == 2);
    CHECK(wm.windows[order[0]] == child && wm.windows[order[1]] == parent);
    purp_wm_focus_window(&wm, purp_wm_window_index(&wm, parent));
    CHECK(parent->is_fullscreen);
    CHECK(purp_wm_stacking_order(&wm, order, PURP_ARRAY_LEN(order)) == 2);
    CHECK(wm.windows[order[0]] == child && wm.windows[order[1]] == parent);
    purp_wm_on_window_unmapped(&wm, (size_t)purp_wm_window_index(&wm, child));
    purp_wm_relayout(&wm);
    CHECK(purp_wm_focused_window(&wm) == parent && parent->is_fullscreen);
    CHECK(purp_wm_stacking_order(&wm, order, PURP_ARRAY_LEN(order)) == 1);
    CHECK(wm.windows[order[0]] == parent);
    purp_wm_finish(&wm);
}

static void test_rule_sized_float_retiles_with_nonzero_configure(void) {
    purp_wm_t wm;
    test_window_wm_init(&wm);
    purp_window_t *window = purp_wm_window_create(&wm, NULL);
    CHECK(window != NULL);
    if (!window) { purp_wm_finish(&wm); return; }
    purp_window_set_app_id(window, "spotlight");
    purp_window_set_title(window, "Rule-sized dialog");
    CHECK(purp_wm_apply_window_rules(&wm, (size_t)purp_wm_window_index(&wm, window)));
    purp_wm_relayout(&wm);
    CHECK(window->is_floating && window->size_from_client);
    CHECK(window->want.width == 0 && window->want.height == 0 && !window->want.tiled);
    purp_wm_toggle_floating_focused(&wm);
    CHECK(!window->is_floating && window->want.tiled);
    purp_size_t inner = purp_window_surface_size(window);
    CHECK(window->want.width > 0 && window->want.height > 0);
    CHECK(window->want.width == inner.w && window->want.height == inner.h);
    purp_wm_finish(&wm);
}

static void test_floating_layout_focus_raises_and_fullscreen_restores(void) {
    purp_wm_t wm;
    test_window_wm_init(&wm);
    purp_window_t *older_float = purp_wm_window_create(&wm, NULL);
    CHECK(older_float != NULL);
    if (!older_float) { purp_wm_finish(&wm); return; }
    purp_wm_toggle_floating_focused(&wm);
    purp_window_t *ordinary = purp_wm_window_create(&wm, NULL);
    CHECK(ordinary != NULL);
    if (!ordinary) { purp_wm_finish(&wm); return; }
    purp_wm_set_layout(&wm, PURP_LAYOUT_FLOATING);
    CHECK_STREQ(purp_layout_symbol(purp_workspace_layout(&wm.monitors[0].workspace)), "><>");
    purp_rect_t saved = purp_rect_make(130, 170, 460, 310);
    ordinary->geo = saved;
    ordinary->float_geo = purp_rect_make(40, 60, 300, 200);
    purp_wm_relayout(&wm);
    CHECK(!ordinary->is_floating && purp_rect_eq(ordinary->geo, saved));
    purp_wm_focus_window(&wm, purp_wm_window_index(&wm, older_float));
    size_t order[2];
    CHECK(purp_wm_stacking_order(&wm, order, PURP_ARRAY_LEN(order)) == 2);
    CHECK(wm.windows[order[0]] == older_float);
    purp_wm_focus_window(&wm, purp_wm_window_index(&wm, ordinary));
    CHECK(purp_wm_stacking_order(&wm, order, PURP_ARRAY_LEN(order)) == 2);
    CHECK(wm.windows[order[0]] == ordinary && wm.windows[order[1]] == older_float);
    purp_wm_toggle_fullscreen_focused(&wm);
    CHECK(ordinary->is_fullscreen && purp_rect_eq(ordinary->geo, purp_wm_monitor_rect(&wm, 0)));
    purp_wm_toggle_fullscreen_focused(&wm);
    CHECK(!ordinary->is_fullscreen && !ordinary->is_floating);
    CHECK(purp_rect_eq(ordinary->geo, saved));
    CHECK(!ordinary->want.fullscreen && !ordinary->want.tiled);
    purp_wm_finish(&wm);
}

static void test_spatial_arrows_swap_neighbours_without_wrapping(void) {
    purp_wm_t wm;
    test_window_wm_init(&wm);
    for (int i = 0; i < 3; ++i) CHECK(purp_wm_window_create(&wm, NULL) != NULL);
    if (wm.window_count != 3) { purp_wm_finish(&wm); return; }
    purp_wm_relayout(&wm);
    purp_window_t *master = wm.windows[0], *upper = wm.windows[1], *lower = wm.windows[2];
    purp_rect_t master_frame = master->geo, upper_frame = upper->geo, lower_frame = lower->geo;
    CHECK(master_frame.x < lower_frame.x && upper_frame.y < lower_frame.y);
    purp_wm_focus_window(&wm, purp_wm_window_index(&wm, lower));
    purp_wm_move_focused_window(&wm, -1, 0);
    CHECK(purp_wm_focused_window(&wm) == lower);
    CHECK(purp_rect_eq(lower->geo, master_frame));
    CHECK(purp_rect_eq(master->geo, lower_frame));
    CHECK(purp_rect_eq(upper->geo, upper_frame));
    purp_wm_focus_window(&wm, purp_wm_window_index(&wm, upper));
    purp_wm_move_focused_window(&wm, 0, 1);
    CHECK(purp_wm_focused_window(&wm) == upper);
    CHECK(purp_rect_eq(upper->geo, lower_frame));
    CHECK(purp_rect_eq(master->geo, upper_frame));
    purp_wm_move_focused_window(&wm, 0, 1);
    CHECK(purp_wm_focused_window(&wm) == upper);
    CHECK(purp_rect_eq(upper->geo, lower_frame));
    CHECK(purp_rect_eq(master->geo, upper_frame));
    CHECK(purp_rect_eq(lower->geo, master_frame));
    purp_wm_finish(&wm);
}

/* =========================================================================================== */
/* JSON, IPC and monitors.json                                                                  */
/* =========================================================================================== */

static void test_json_parse_and_serialize(void) {
    purp_json_value_t v;
    char err[128];

    /* Basic values and accessors. */
    CHECK(purp_json_parse(" 42 ", 4, &v, err, sizeof err));
    CHECK(v.type == PURP_JSON_NUMBER);
    int32_t i32 = 0;
    CHECK(purp_json_as_i32(&v, &i32) && i32 == 42);
    purp_json_free(&v);

    /* UTF-8 and surrogate pairs. */
    static const char emoji[] = "\"A\\uD834\\uDD1E\"";
    CHECK(purp_json_parse(emoji, sizeof emoji - 1, &v, err, sizeof err));
    CHECK(v.type == PURP_JSON_STRING);
    CHECK(v.u.string.len == 5); /* 'A' + 4-byte U+1D11E */
    CHECK((uint8_t)v.u.string.data[0] == 'A' && (uint8_t)v.u.string.data[1] == 0xF0 &&
          (uint8_t)v.u.string.data[4] == 0x9E);
    purp_strbuf_t sb = { 0 };
    purp_json_serialize(&v, &sb);
    CHECK(!sb.oom && sb.data != NULL);
    CHECK(strstr(sb.data, "\xF0\x9D\x84\x9E") != NULL); /* emitted as raw UTF-8 */
    purp_strbuf_free(&sb);
    purp_json_free(&v);

    /* Raw UTF-8 round-trips; escapes are produced on serialize. */
    static const char raw_utf8[] = "\"caf\xC3\xA9\"";
    CHECK(purp_json_parse(raw_utf8, sizeof raw_utf8 - 1, &v, err, sizeof err));
    purp_json_free(&v);
    CHECK(purp_json_parse("\"\\u00e9\"", strlen("\"\\u00e9\""), &v, err, sizeof err));
    CHECK(v.u.string.len == 2 && (uint8_t)v.u.string.data[0] == 0xC3);
    purp_json_free(&v);

    /* Errors: lone surrogate, bad escape, control byte, trailing data, depth. */
    CHECK(!purp_json_parse("\"\\uD834x\"", 9, &v, err, sizeof err));
    CHECK(!purp_json_parse("[1,]", 4, &v, err, sizeof err));
    CHECK(!purp_json_parse("1 2", 3, &v, err, sizeof err));
    {
        char deep[80];
        memset(deep, '[', 40);
        memset(deep + 40, ']', 40);
        CHECK(!purp_json_parse(deep, 80, &v, err, sizeof err));
    }

    /* Integer bounds: INT32_MIN/INT32_MAX convert, anything beyond does not. */
    CHECK(purp_json_parse("2147483647", 10, &v, err, sizeof err));
    CHECK(purp_json_as_i32(&v, &i32) && i32 == INT32_MAX);
    purp_json_free(&v);
    CHECK(purp_json_parse("-2147483648", 11, &v, err, sizeof err));
    CHECK(purp_json_as_i32(&v, &i32) && i32 == INT32_MIN);
    purp_json_free(&v);
    CHECK(purp_json_parse("2147483648", 10, &v, err, sizeof err));
    CHECK(!purp_json_as_i32(&v, &i32));
    purp_json_free(&v);
    CHECK(purp_json_parse("-2147483649", 11, &v, err, sizeof err));
    CHECK(!purp_json_as_i32(&v, &i32));
    purp_json_free(&v);
    CHECK(purp_json_parse("1.5", 3, &v, err, sizeof err));
    CHECK(!purp_json_as_i32(&v, &i32));
    double f = 0;
    CHECK(purp_json_as_f64(&v, &f) && f == 1.5);
    purp_json_free(&v);

    /* Objects: duplicate keys resolve to the last occurrence. */
    CHECK(purp_json_parse("{\"a\":1,\"a\":2,\"b\":[true,null]}", 29, &v, err, sizeof err));
    const purp_json_value_t *a = purp_json_get(&v, "a");
    CHECK(a && purp_json_as_i32(a, &i32) && i32 == 2);
    CHECK(purp_json_get(&v, "missing") == NULL);
    purp_json_free(&v);

    /* String escaping on serialize: control bytes become \u00xx, quotes are escaped. */
    purp_strbuf_t esc = { 0 };
    purp_strbuf_append_json_string(&esc, "a\"b\nc\x01");
    CHECK_STREQ(esc.data, "\"a\\\"b\\nc\\u0001\"");
    purp_strbuf_free(&esc);

    /* Scale formatting used by monitors.json. */
    char scale[32];
    purp_ipc_format_scale(1.0, scale, sizeof scale);
    CHECK_STREQ(scale, "1.0");
    purp_ipc_format_scale(1.25, scale, sizeof scale);
    CHECK_STREQ(scale, "1.25");
    purp_ipc_format_scale(1.5, scale, sizeof scale);
    CHECK_STREQ(scale, "1.5");
}

static void test_ipc_command_parsing(void) {
    purp_ipc_command_t cmd;
    char err[PURP_IPC_ERROR_MAX];

    static const char get[] = "{\"cmd\":\"get_monitors\"}";
    CHECK(purp_ipc_parse_command(get, sizeof get - 1, &cmd, err, sizeof err));
    CHECK(cmd.kind == PURP_IPC_GET_MONITORS);
    static const char focus[] = "{\"cmd\":\"focus_monitor\",\"delta\":-1}";
    CHECK(purp_ipc_parse_command(focus, sizeof focus - 1, &cmd, err, sizeof err));
    CHECK(cmd.kind == PURP_IPC_FOCUS_MONITOR && cmd.delta == -1);
    static const char move[] = "{\"cmd\":\"move_window_to_monitor\",\"delta\":1}";
    CHECK(purp_ipc_parse_command(move, sizeof move - 1, &cmd, err, sizeof err));
    CHECK(cmd.kind == PURP_IPC_MOVE_WINDOW_TO_MONITOR && cmd.delta == 1);
    static const char no_delta[] = "{\"cmd\":\"focus_monitor\"}";
    CHECK(purp_ipc_parse_command(no_delta, sizeof no_delta - 1, &cmd, err, sizeof err));
    CHECK(cmd.delta == 1);
    /* A non-integer delta silently becomes 1. */
    static const char str_delta[] = "{\"cmd\":\"focus_monitor\",\"delta\":\"x\"}";
    CHECK(purp_ipc_parse_command(str_delta, sizeof str_delta - 1, &cmd, err, sizeof err));
    CHECK(cmd.delta == 1);

    static const char apply[] =
        "{\"cmd\":\"apply_monitors\",\"monitors\":["
        "{\"name\":\"eDP-1\",\"enabled\":true,\"primary\":true,\"width\":2560,\"height\":1600,"
        "\"refresh_mhz\":120000,\"scale\":1.5,\"x\":0,\"y\":0},"
        "{\"name\":\"HDMI-A-1\",\"enabled\":true,\"primary\":false,\"width\":1920,"
        "\"height\":1080,\"refresh_mhz\":60000,\"scale\":1.0,\"x\":1707,\"y\":0}]}";
    CHECK(purp_ipc_parse_command(apply, sizeof apply - 1, &cmd, err, sizeof err));
    CHECK(cmd.kind == PURP_IPC_APPLY_MONITORS && cmd.monitor_count == 2);
    CHECK_STREQ(cmd.monitors[0].name, "eDP-1");
    CHECK(fabs(cmd.monitors[0].scale - 1.5) < 1e-6);
    CHECK(cmd.monitors[1].x == 1707);

    /* Exact Rust error strings. */
    static const char empty[] = "   ";
    CHECK(!purp_ipc_parse_command(empty, sizeof empty - 1, &cmd, err, sizeof err));
    CHECK_STREQ(err, PURP_IPC_ERR_EMPTY);
    static const char arr[] = "[]";
    CHECK(!purp_ipc_parse_command(arr, sizeof arr - 1, &cmd, err, sizeof err));
    CHECK_STREQ(err, PURP_IPC_ERR_NOT_OBJECT);
    static const char no_cmd[] = "{\"x\":1}";
    CHECK(!purp_ipc_parse_command(no_cmd, sizeof no_cmd - 1, &cmd, err, sizeof err));
    CHECK_STREQ(err, PURP_IPC_ERR_NO_CMD);
    static const char no_mon[] = "{\"cmd\":\"apply_monitors\"}";
    CHECK(!purp_ipc_parse_command(no_mon, sizeof no_mon - 1, &cmd, err, sizeof err));
    CHECK_STREQ(err, PURP_IPC_ERR_NO_MONITORS);
    static const char bogus[] = "{\"cmd\":\"bogus\"}";
    CHECK(!purp_ipc_parse_command(bogus, sizeof bogus - 1, &cmd, err, sizeof err));
    CHECK(strstr(err, "bogus") != NULL);

    /* Coordinate range -131072..=131072: INT32_MIN/INT32_MAX are parsed as integers but
     * rejected as coordinates. */
    static const char x_max[] =
        "{\"cmd\":\"apply_monitors\",\"monitors\":[{\"name\":\"m\",\"width\":1920,"
        "\"height\":1080,\"scale\":1.0,\"x\":2147483647}]}";
    CHECK(!purp_ipc_parse_command(x_max, sizeof x_max - 1, &cmd, err, sizeof err));
    CHECK(strstr(err, "-131072..=131072") != NULL);
    static const char x_min[] =
        "{\"cmd\":\"apply_monitors\",\"monitors\":[{\"name\":\"m\",\"width\":1920,"
        "\"height\":1080,\"scale\":1.0,\"x\":-2147483648}]}";
    CHECK(!purp_ipc_parse_command(x_min, sizeof x_min - 1, &cmd, err, sizeof err));
    CHECK(strstr(err, "-131072..=131072") != NULL);
    static const char x_over[] =
        "{\"cmd\":\"apply_monitors\",\"monitors\":[{\"name\":\"m\",\"width\":1920,"
        "\"height\":1080,\"scale\":1.0,\"x\":131073}]}";
    CHECK(!purp_ipc_parse_command(x_over, sizeof x_over - 1, &cmd, err, sizeof err));
    static const char x_ok[] =
        "{\"cmd\":\"apply_monitors\",\"monitors\":[{\"name\":\"m\",\"width\":1920,"
        "\"height\":1080,\"scale\":1.0,\"x\":-131072,\"y\":131072}]}";
    CHECK(purp_ipc_parse_command(x_ok, sizeof x_ok - 1, &cmd, err, sizeof err));
    CHECK(cmd.monitors[0].x == -131072 && cmd.monitors[0].y == 131072);

    static const char bad_w[] =
        "{\"cmd\":\"apply_monitors\",\"monitors\":[{\"name\":\"m\",\"width\":50,"
        "\"height\":1080,\"scale\":1.0}]}";
    CHECK(!purp_ipc_parse_command(bad_w, sizeof bad_w - 1, &cmd, err, sizeof err));
    CHECK(strstr(err, "100..=32768") != NULL);
}

static void test_monitors_json_round_trip(void) {
    purp_monitor_config_t entries[2] = {
        test_monitor_cfg("eDP-1", true, true, 2560, 1600, 120000, 1.5, 0, 0),
        test_monitor_cfg("HDMI-A-1", true, false, 1920, 1080, 60000, 1.0, 1707, 0),
    };

    /* Serialize -> parse round trip. */
    purp_strbuf_t sb = { 0 };
    purp_serialize_monitors_config_json(entries, 2, &sb);
    CHECK(!sb.oom && sb.data != NULL);
    CHECK(strstr(sb.data, "\"monitors\"") != NULL);
    purp_monitor_config_t reparsed[PURP_IPC_MAX_MONITORS];
    size_t count = 0;
    char err[PURP_IPC_ERROR_MAX];
    CHECK(purp_parse_monitors_config_json(sb.data, sb.len, reparsed, &count, err, sizeof err));
    CHECK(count == 2);
    CHECK_STREQ(reparsed[0].name, "eDP-1");
    CHECK(reparsed[0].primary && !reparsed[1].primary);
    CHECK(fabs(reparsed[0].scale - 1.5) < 1e-6 && reparsed[1].x == 1707);
    CHECK(reparsed[0].refresh_mhz == 120000);
    purp_strbuf_free(&sb);

    /* A bare array is also accepted. */
    static const char bare[] =
        "[{\"name\":\"X\",\"width\":1280,\"height\":720,\"scale\":1.0}]";
    CHECK(purp_parse_monitors_config_json(bare, sizeof bare - 1, reparsed, &count, err,
                                          sizeof err));
    CHECK(count == 1 && reparsed[0].enabled && reparsed[0].refresh_mhz == 60000);

    /* Atomic save + load through $PURPWM_MONITORS_CONFIG. */
    CHECK(test_make_tmp_dir());
    char path[320];
    snprintf(path, sizeof path, "%s/monitors.json", g_tmp_dir);
    CHECK(purp_save_monitors_config_to(path, entries, 2));
    /* The rename target exists and the ".<file>.tmp.<pid>" sibling is gone. */
    char tmp[340];
    snprintf(tmp, sizeof tmp, "%s/.monitors.json.tmp.%ld", g_tmp_dir, (long)getpid());
    CHECK(access(tmp, F_OK) != 0);
    size_t loaded = purp_load_saved_monitors_from(path, reparsed);
    CHECK(loaded == 2);
    CHECK_STREQ(reparsed[1].name, "HDMI-A-1");

    setenv(PURP_ENV_MONITORS_CONFIG, path, 1);
    char resolved[512];
    CHECK(purp_monitors_config_path(resolved, sizeof resolved));
    CHECK_STREQ(resolved, path);
    loaded = purp_load_saved_monitors(reparsed);
    CHECK(loaded == 2);
    unsetenv(PURP_ENV_MONITORS_CONFIG);
}

static void test_ipc_server_round_trip_and_locked_rejection(void) {
    CHECK(test_make_tmp_dir());
    char sock[320];
    snprintf(sock, sizeof sock, "%s/test-ipc.sock", g_tmp_dir);
    char err[PURP_IPC_ERROR_MAX];
    purp_ipc_server_t srv = { 0 };
    CHECK(purp_ipc_server_bind(&srv, sock, g_tmp_dir, true, err, sizeof err));
    CHECK(purp_ipc_server_fd(&srv) >= 0);
    CHECK(getenv(PURP_ENV_IPC_SOCKET) != NULL);

    /* get_monitors: request -> parsed request -> JSON reply. */
    int client = test_unix_connect(sock);
    CHECK(client >= 0);
    CHECK(test_write_all(client, "{\"cmd\":\"get_monitors\"}\n", 23));
    purp_ipc_request_t reqs[PURP_IPC_MAX_CLIENTS];
    size_t n = 0;
    for (int i = 0; i < 200 && n == 0; ++i) {
        n = purp_ipc_server_poll(&srv, false, reqs, PURP_IPC_MAX_CLIENTS);
        if (n == 0) usleep(5000);
    }
    CHECK(n == 1);
    CHECK(reqs[0].command.kind == PURP_IPC_GET_MONITORS);

    purp_monitor_info_t info = { 0 };
    snprintf(info.name, sizeof info.name, "eDP-1");
    snprintf(info.description, sizeof info.description, "Built-in");
    info.enabled = true;
    info.primary = true;
    info.width = 2560;
    info.height = 1600;
    info.refresh_mhz = 120000;
    info.scale = 1.5;
    info.logical_width = 1707;
    info.logical_height = 1067;
    purp_strbuf_t reply = { 0 };
    purp_ipc_serialize_monitors_response(&info, 1, &reply);
    purp_ipc_server_respond(&srv, &reqs[0], reply.data);
    purp_strbuf_free(&reply);
    purp_ipc_server_flush(&srv);

    char buf[4096];
    CHECK(test_read_until(client, buf, sizeof buf, 3000, "\n") > 0);
    CHECK(strstr(buf, "\"ok\":true") != NULL);
    CHECK(strstr(buf, "eDP-1") != NULL);
    close(client);

    /* Locked session: everything except get_monitors is rejected with "session is locked". */
    client = test_unix_connect(sock);
    CHECK(client >= 0);
    static const char apply_req[] =
        "{\"cmd\":\"apply_monitors\",\"monitors\":[{\"name\":\"m\",\"width\":1920,"
        "\"height\":1080,\"scale\":1.0}]}\n";
    CHECK(test_write_all(client, apply_req, sizeof apply_req - 1));
    /* The locked reply is answered directly by poll/flush (no parsed request). */
    for (int i = 0; i < 200; ++i) {
        (void)purp_ipc_server_poll(&srv, true, reqs, PURP_IPC_MAX_CLIENTS);
        purp_ipc_server_flush(&srv);
        ssize_t got = test_read_until(client, buf, sizeof buf, 5, "\n");
        if (got > 0) break;
        usleep(5000);
    }
    CHECK(strstr(buf, "\"ok\":false") != NULL);
    CHECK(strstr(buf, PURP_IPC_ERR_LOCKED) != NULL);
    close(client);

    /* get_monitors still works while locked. */
    client = test_unix_connect(sock);
    CHECK(client >= 0);
    CHECK(test_write_all(client, "{\"cmd\":\"get_monitors\"}\n", 23));
    n = 0;
    for (int i = 0; i < 200 && n == 0; ++i) {
        n = purp_ipc_server_poll(&srv, true, reqs, PURP_IPC_MAX_CLIENTS);
        if (n == 0) usleep(5000);
    }
    CHECK(n == 1 && reqs[0].command.kind == PURP_IPC_GET_MONITORS);
    purp_strbuf_t ok = { 0 };
    purp_ipc_serialize_ok_response(&ok);
    purp_ipc_server_respond(&srv, &reqs[0], ok.data);
    purp_strbuf_free(&ok);
    purp_ipc_server_flush(&srv);
    CHECK(test_read_until(client, buf, sizeof buf, 3000, "\n") > 0);
    CHECK(strstr(buf, "\"ok\":true") != NULL);
    close(client);

    purp_ipc_server_close(&srv);
    CHECK(purp_ipc_server_fd(&srv) < 0);
}

static purp_blob_t *test_capture_blob(void *data) {
    size_t *calls = data;
    ++*calls;
    return purp_blob_new("PNGDATA", 7);
}

static void test_capture_server_round_trip(void) {
    CHECK(test_make_tmp_dir());
    char sock[320];
    snprintf(sock, sizeof sock, "%s/test-capture.sock", g_tmp_dir);
    char err[PURP_IPC_ERROR_MAX];
    purp_capture_server_t srv = { 0 };
    CHECK(purp_capture_server_bind(&srv, sock, err, sizeof err));
    CHECK(purp_capture_server_fd(&srv) >= 0);

    size_t calls = 0;
    int client = test_unix_connect(sock);
    CHECK(client >= 0);
    CHECK(test_write_all(client, PURP_CAPTURE_REQUEST, strlen(PURP_CAPTURE_REQUEST)));
    for (int i = 0; i < 200; ++i) {
        if (purp_capture_server_poll(&srv, false, test_capture_blob, &calls)) break;
        usleep(5000);
    }
    CHECK(calls == 1);
    char buf[512];
    CHECK(test_read_until(client, buf, sizeof buf, 3000, "PNGDATA") > 0);
    CHECK(strncmp(buf, "OK 7\n", 5) == 0);
    CHECK(memcmp(buf + 5, "PNGDATA", 7) == 0);
    close(client);

    /* Invalid request -> ERR. */
    client = test_unix_connect(sock);
    CHECK(client >= 0);
    CHECK(test_write_all(client, "garbage\n", 8));
    for (int i = 0; i < 200; ++i) {
        purp_capture_server_poll(&srv, false, test_capture_blob, &calls);
        ssize_t n = test_read_until(client, buf, sizeof buf, 5, "\n");
        if (n > 0) break;
        usleep(5000);
    }
    CHECK_STREQ(buf, PURP_CAPTURE_ERR_REPLY);
    close(client);

    /* Locked session -> ERR and the capture callback is not invoked. */
    client = test_unix_connect(sock);
    CHECK(client >= 0);
    CHECK(test_write_all(client, PURP_CAPTURE_REQUEST, strlen(PURP_CAPTURE_REQUEST)));
    size_t before = calls;
    for (int i = 0; i < 200; ++i) {
        purp_capture_server_poll(&srv, true, test_capture_blob, &calls);
        ssize_t n = test_read_until(client, buf, sizeof buf, 5, "\n");
        if (n > 0) break;
        usleep(5000);
    }
    CHECK_STREQ(buf, PURP_CAPTURE_ERR_REPLY);
    CHECK(calls == before);
    close(client);

    purp_capture_server_close(&srv);
}

/* =========================================================================================== */
/* UTF-8 and fonts                                                                             */
/* =========================================================================================== */

static void test_utf8_helpers(void) {
    const char *p = "h\xC3\xA9llo\xE2\x82\xAC"; /* "héllo€" */
    CHECK(purp_utf8_count(p) == 6);
    const char *walk = p;
    CHECK(purp_utf8_decode(&walk) == 'h');
    CHECK(purp_utf8_decode(&walk) == 0xE9);
    walk += 3;
    CHECK(purp_utf8_decode(&walk) == 0x20AC);
    CHECK(purp_utf8_decode(&walk) == 0); /* NUL */

    const char *bad = "\xFFz";
    CHECK(purp_utf8_decode(&bad) == 0xFFFD);
    CHECK(purp_utf8_decode(&bad) == 'z');

    char enc[8];
    CHECK(purp_utf8_encode(0x20AC, enc) == 3);
    CHECK((uint8_t)enc[0] == 0xE2 && (uint8_t)enc[1] == 0x82 && (uint8_t)enc[2] == 0xAC);

    /* Truncate never splits a sequence. */
    char dst[16];
    CHECK(purp_utf8_truncate(dst, sizeof dst, "h\xC3\xA9llo", 2) == 3);
    CHECK_STREQ(dst, "h\xC3\xA9");
    CHECK(purp_utf8_truncate(dst, 2, "\xC3\xA9x", 5) == 0);
    CHECK(dst[0] == '\0');
    CHECK(purp_utf8_sanitize_line(dst, sizeof dst, "  hi\nthere\t ", 64) == 8);
    CHECK_STREQ(dst, "hi there");

    /* pop_wipe removes whole code points and zeroes the bytes. */
    char pw[16];
    snprintf(pw, sizeof pw, "ab\xE2\x82\xAC"); /* "ab€" */
    CHECK(purp_utf8_pop_wipe(pw) == 3);
    CHECK_STREQ(pw, "ab");
    CHECK(pw[3] == '\0');
    CHECK(purp_utf8_pop_wipe(pw) == 1 && strcmp(pw, "a") == 0);
    CHECK(purp_utf8_pop_wipe(pw) == 1 && pw[0] == '\0');
    CHECK(purp_utf8_pop_wipe(pw) == 0);
}

static void test_bundled_font_rasterizes(void) {
    purp_font_t *font = purp_font_load_memory(purp_bundled_font, purp_bundled_font_len);
    CHECK(font != NULL);
    if (!font) return;
    CHECK_STREQ(purp_font_source(font), "<bundled>");

    const purp_glyph_t *glyph = purp_font_glyph(font, 'A', 16.0f);
    CHECK(glyph != NULL && glyph->width > 0 && glyph->advance > 0);
    bool inked = false;
    if (glyph->coverage)
        for (int32_t i = 0; i < glyph->width * glyph->height; ++i)
            inked |= glyph->coverage[i] > 0;
    CHECK(inked);

    int32_t ascent = 0, descent = 0;
    purp_font_metrics(font, 16.0f, &ascent, &descent);
    CHECK(ascent > 0 && descent > 0);
    CHECK(purp_font_measure(font, "A", 16.0f) > 0);
    /* NULL font measures chars * max((int)(px*0.6), 8). */
    CHECK(purp_font_measure(NULL, "abc", 16.0f) == 27);

    /* Drawing into a pixel buffer inks pixels and advances the pen. */
    purp_pixel_buf_t buf = { 0 };
    CHECK(purp_pixel_buf_ensure(&buf, 200, 60));
    int32_t pen = purp_font_draw(font, &buf, "Hi", 5, 40, 24.0f,
                                 (purp_rgba_t){ 0xFF, 0xFF, 0xFF, 0xFF });
    CHECK(pen > 5);
    bool any = false;
    for (int32_t i = 0; i < buf.width * buf.height * 4; i += 4)
        any |= buf.data[i + 3] > 0;
    CHECK(any);
    purp_pixel_buf_free(&buf);
    purp_font_destroy(font);
}

/* =========================================================================================== */
/* Status bar                                                                                  */
/* =========================================================================================== */

static void test_status_bar_hit_test_and_dirty_edge(void) {
    purp_font_t *font = purp_font_load_memory(purp_bundled_font, purp_bundled_font_len);
    purp_bar_t bar;
    CHECK(purp_bar_init(&bar, font, false));
    purp_bar_set_monitor_scale(&bar, 0, 1.0);

    /* Tag 1 is at x=0..34, tag 9 at 34*8..34*9. */
    purp_bar_action_t a = purp_bar_hit_test(&bar, 0, 5);
    CHECK(a.kind == PURP_BAR_ACTION_TAG && a.tag == 0);
    a = purp_bar_hit_test(&bar, 0, 34 * 8 + 4);
    CHECK(a.kind == PURP_BAR_ACTION_TAG && a.tag == 8);
    /* Layout symbol sits right after the 9 tag cells. */
    a = purp_bar_hit_test(&bar, 0, 34 * 9 + 10);
    CHECK(a.kind == PURP_BAR_ACTION_CYCLE_LAYOUT);
    /* Clicking in the middle title area cycles focus. */
    a = purp_bar_hit_test(&bar, 0, 800);
    CHECK(a.kind == PURP_BAR_ACTION_FOCUS_NEXT);
    /* Unknown monitor falls back to slot 0. */
    a = purp_bar_hit_test(&bar, 3, 5);
    CHECK(a.kind == PURP_BAR_ACTION_TAG && a.tag == 0);

    /* Changing scale must not collapse the layout-symbol hit target before the next raster. */
    purp_bar_set_monitor_scale(&bar, 0, 1.5);
    a = purp_bar_hit_test(&bar, 0, 34 * 9 + 4);
    CHECK(a.kind == PURP_BAR_ACTION_CYCLE_LAYOUT);

    /* Edge-triggered dirty flag: one true per mark_dirty, never spurious. */
    while (purp_bar_is_dirty(&bar)) {
    }
    purp_bar_mark_dirty(&bar);
    CHECK(purp_bar_is_dirty(&bar));
    CHECK(!purp_bar_is_dirty(&bar));
    CHECK(!purp_bar_is_dirty(&bar));

    purp_bar_finish(&bar);
    purp_font_destroy(font);
}

static void test_status_parsers(void) {
    char label[PURP_BAR_LABEL_MAX];
    bool connected = false;

    /* nmcli -t -f TYPE,STATE,CONNECTION device status. */
    purp_bar_wifi_label_from_nmcli("wifi:disconnected:\nwifi:connected:Home Network\n", label,
                                   sizeof label, &connected);
    CHECK(connected);
    CHECK_STREQ(label, "WIFI: Home Network");
    purp_bar_wifi_label_from_nmcli("wifi:disconnected:\n", label, sizeof label, &connected);
    CHECK(!connected);
    CHECK_STREQ(label, "WIFI: Disconn");
    purp_bar_wifi_label_from_nmcli("ethernet:connected:Wired\n", label, sizeof label,
                                   &connected);
    CHECK_STREQ(label, "WIFI: N/A");

    CHECK(!purp_wifi_needs_autoconnect("wifi:connected\nwifi:disconnected"));
    CHECK(purp_wifi_needs_autoconnect("wifi:disconnected"));
    CHECK(!purp_wifi_needs_autoconnect("ethernet:disconnected"));
    CHECK(!purp_wifi_needs_autoconnect("wifi:connecting"));

    /* wpctl get-volume @DEFAULT_AUDIO_SINK@ */
    bool unmuted = false;
    purp_bar_volume_label_from_wpctl("Volume: 0.45\n", label, sizeof label, &unmuted);
    CHECK(unmuted);
    CHECK_STREQ(label, "VOL 45%");
    purp_bar_volume_label_from_wpctl("Volume: 0.45 [MUTED]\n", label, sizeof label, &unmuted);
    CHECK(!unmuted);
    CHECK_STREQ(label, "VOL Mute 45%");
    purp_bar_volume_label_from_wpctl("nonsense\n", label, sizeof label, &unmuted);
    CHECK_STREQ(label, "VOL N/A");

    char clock_out[32];
    purp_format_local_time(clock_out, sizeof clock_out);
    CHECK(strlen(clock_out) >= 11); /* "Jan 01 00:00" */
}

/* =========================================================================================== */
/* PAM lock screen                                                                             */
/* =========================================================================================== */

static void test_lock_screen_keys(void) {
    purp_lock_key_t key;
    CHECK(purp_lock_key_from_keysym(XKB_KEY_a, 'a', 0, &key));
    CHECK(key.kind == PURP_LOCK_KEY_CHAR && key.codepoint == 'a');
    CHECK(purp_lock_key_from_keysym(XKB_KEY_BackSpace, 0, 0, &key));
    CHECK(key.kind == PURP_LOCK_KEY_BACKSPACE);
    CHECK(purp_lock_key_from_keysym(XKB_KEY_Escape, 0, 0, &key));
    CHECK(key.kind == PURP_LOCK_KEY_CLEAR);
    CHECK(purp_lock_key_from_keysym(XKB_KEY_Return, '\r', 0, &key));
    CHECK(key.kind == PURP_LOCK_KEY_SUBMIT);
    CHECK(purp_lock_key_from_keysym(XKB_KEY_KP_Enter, '\r', 0, &key));
    CHECK(key.kind == PURP_LOCK_KEY_SUBMIT);
    /* Ctrl+u clears like Escape. */
    CHECK(purp_lock_key_from_keysym(XKB_KEY_u, 0x15, PURP_MOD_CTRL, &key));
    CHECK(key.kind == PURP_LOCK_KEY_CLEAR);
    /* Non-printable keys are not lock keys. */
    CHECK(!purp_lock_key_from_keysym(XKB_KEY_F5, 0, 0, &key));
    CHECK(!purp_lock_key_from_keysym(XKB_KEY_Control_L, 0, PURP_MOD_CTRL, &key));

    purp_lock_screen_t lock;
    purp_lock_init(&lock);
    key.kind = PURP_LOCK_KEY_CHAR;
    key.codepoint = 'p';
    purp_lock_handle_key(&lock, key);
    key.codepoint = 'u';
    purp_lock_handle_key(&lock, key);
    key.codepoint = 'r';
    purp_lock_handle_key(&lock, key);
    CHECK(purp_lock_password_len(&lock) == 3);
    key.kind = PURP_LOCK_KEY_BACKSPACE;
    purp_lock_handle_key(&lock, key);
    CHECK(purp_lock_password_len(&lock) == 2);
    key.kind = PURP_LOCK_KEY_CLEAR;
    purp_lock_handle_key(&lock, key);
    CHECK(purp_lock_password_len(&lock) == 0);
    CHECK(purp_lock_poll(&lock) == PURP_LOCK_POLL_PENDING);
    CHECK(!purp_lock_is_checking(&lock));
    purp_lock_finish(&lock);
}

static void test_pam_confdir_allow_deny_and_service_discovery(void) {
    char dir[256];
    snprintf(dir, sizeof dir, "/tmp/purpam-test-XXXXXX");
    CHECK(mkdtemp(dir) != NULL);
    char allow[300], deny[300], primary[300], legacy[300];
    snprintf(allow, sizeof allow, "%s/test-allow", dir);
    snprintf(deny, sizeof deny, "%s/test-deny", dir);
    snprintf(primary, sizeof primary, "%s/%s", dir, PURP_PAM_SERVICE_PRIMARY);
    snprintf(legacy, sizeof legacy, "%s/%s", dir, PURP_PAM_SERVICE_LEGACY);

    CHECK(test_write_file(allow, "auth optional pam_echo.so prompt\n"
                                "auth required pam_permit.so\n"));
    CHECK(test_write_file(deny, "auth required pam_deny.so\n"));

    char err[PURP_LOCK_MESSAGE_MAX];
    size_t convos_before = atomic_load(&purp_pam_conversations);
    CHECK(purp_pam_check("test-allow", "nobody", "secret", dir, err, sizeof err));
    CHECK(atomic_load(&purp_pam_conversations) > convos_before);
    CHECK(!purp_pam_check("test-deny", "nobody", "secret", dir, err, sizeof err));

    /* $PURPWM_PAM_CONFDIR service discovery: a legacy "purpwm" service is found when
     * "purpwm+" is absent, then a "purpwm+" file wins and can deny. */
    CHECK(test_write_file(legacy, "auth optional pam_echo.so prompt\n"
                                  "auth required pam_permit.so\n"));
    setenv(PURP_ENV_PAM_CONFDIR, dir, 1);
    unsetenv(PURP_ENV_PAM_SERVICE);
    CHECK(purp_pam_authenticate_current_user("secret", err, sizeof err));
    CHECK(test_write_file(primary, "auth required pam_deny.so\n"));
    CHECK(!purp_pam_authenticate_current_user("secret", err, sizeof err));

    /* The lock screen worker reaches the same denying stack: FAILED, message set, password
     * cleared, then input accepted again. */
    purp_lock_screen_t lock;
    purp_lock_init(&lock);
    purp_lock_key_t key = { PURP_LOCK_KEY_CHAR, 'x' };
    purp_lock_handle_key(&lock, key);
    key.kind = PURP_LOCK_KEY_SUBMIT;
    purp_lock_handle_key(&lock, key);
    CHECK(purp_lock_is_checking(&lock));
    uint64_t deadline = purp_now_ms() + 10000;
    purp_lock_poll_t outcome = PURP_LOCK_POLL_PENDING;
    while (purp_now_ms() < deadline) {
        outcome = purp_lock_poll(&lock);
        if (outcome != PURP_LOCK_POLL_PENDING) break;
        usleep(5000);
    }
    unsetenv(PURP_ENV_PAM_CONFDIR);
    CHECK(outcome == PURP_LOCK_POLL_FAILED);
    CHECK(!purp_lock_is_checking(&lock));
    const char *msg = purp_lock_message(&lock);
    CHECK(msg != NULL && msg[0] != '\0');
    CHECK(purp_lock_password_len(&lock) == 0);
    key.kind = PURP_LOCK_KEY_CHAR;
    key.codepoint = 'y';
    purp_lock_handle_key(&lock, key);
    CHECK(purp_lock_password_len(&lock) == 1);
    purp_lock_finish(&lock);

    unlink(allow);
    unlink(deny);
    unlink(primary);
    unlink(legacy);
    rmdir(dir);
}

/* =========================================================================================== */
/* Notifications and help                                                                      */
/* =========================================================================================== */

static void test_notification_center(void) {
    purp_notify_center_t nc;
    CHECK(purp_notify_init(&nc));
    CHECK(purp_notify_is_empty(&nc));
    CHECK(!purp_notify_stack_geometry(&nc, 1600, true, &(purp_rect_t){ 0 }));

    /* D-Bus timeout mapping. */
    CHECK(purp_notify_timeout_ms(0, 1) == 0);
    CHECK(purp_notify_timeout_ms(-1, 0) == PURP_NOTIFY_DEFAULT_TIMEOUT_MS);
    CHECK(purp_notify_timeout_ms(-1, 2) == PURP_NOTIFY_CRITICAL_TIMEOUT_MS);
    CHECK(purp_notify_timeout_ms(30000, 1) == 30000);
    CHECK(purp_notify_timeout_ms(1, 1) == 1);

    uint64_t gen0 = nc.content_generation;
    uint32_t id1 = purp_notify_push(&nc, 0, "purpwm", "Hello", "First toast", 1, 10000);
    uint32_t id2 = purp_notify_push(&nc, 0, "purpwm", "Second", "Second toast", 2, 10000);
    CHECK(purp_notify_count(&nc) == 2);
    CHECK(id1 != id2 && nc.content_generation > gen0);
    /* Newest first. */
    CHECK(nc.toasts[0].id == id2 && nc.toasts[1].id == id1);

    /* Replacing a live id updates in place without growing the stack. */
    uint32_t back = purp_notify_push(&nc, id1, "purpwm", "Updated", "Replaced body", 1, 10000);
    CHECK(back == id1 && purp_notify_count(&nc) == 2);
    CHECK_STREQ(nc.toasts[1].summary, "Updated");

    /* Stack geometry and in-bounds dismiss: x = w - 340 - 8, y = 28 + 8. */
    purp_rect_t geo;
    CHECK(purp_notify_stack_geometry(&nc, 1600, true, &geo));
    CHECK(geo.x == 1600 - 340 - 8 && geo.w == 340 && geo.y == 36 && geo.h == 144);
    CHECK(purp_notify_dismiss_at(&nc, (purp_pointf_t){ geo.x + 20.0, geo.y + 20.0 },
                                 purp_rect_make(0, 0, 1600, 1200), true));
    CHECK(purp_notify_count(&nc) == 1);

    /* A click on the inner edge of the card counts (subpixel bound rounding). */
    CHECK(purp_notify_stack_geometry(&nc, 1600, true, &geo));
    purp_pointf_t edge = { geo.x + geo.w - 0.2, geo.y + geo.h - 0.2 };
    CHECK(purp_notify_dismiss_at(&nc, edge, purp_rect_make(0, 0, 1600, 1200), true));
    CHECK(purp_notify_is_empty(&nc));

    /* Queued CLOSE events apply on tick (the D-Bus path). */
    uint32_t id3 = purp_notify_push(&nc, 0, "app", "Hi", "Body", 1, 10000);
    purp_notify_event_t ev = { 0 };
    ev.kind = PURP_NOTIFY_EVENT_CLOSE;
    ev.close_id = id3;
    purp_notify_enqueue(&nc, &ev);
    CHECK(purp_notify_tick(&nc, purp_now_ms()));
    CHECK(purp_notify_is_empty(&nc));

    /* Expiry on tick. */
    purp_notify_push(&nc, 0, "app", "Tmp", "Expires", 1, 1000);
    uint64_t tick_now = purp_now_ms();
    (void)purp_notify_tick(&nc, tick_now);
    CHECK(purp_notify_tick(&nc, tick_now + 60000));
    CHECK(purp_notify_is_empty(&nc));

    /* Render caching: content changes invalidate the cached raster. */
    purp_font_t *font = purp_font_load_memory(purp_bundled_font, purp_bundled_font_len);
    purp_notify_push(&nc, 0, "app", "One", "First", 1, 30000);
    purp_point_t origin = { 0, 0 };
    const purp_pixel_buf_t *r1 = purp_notify_render(&nc, font, 1600, true, 1.0, &origin);
    CHECK(r1 != NULL && r1->data != NULL);
    uint64_t gen1 = r1->generation;
    purp_notify_push(&nc, 0, "app", "Two", "Second", 1, 30000);
    const purp_pixel_buf_t *r2 = purp_notify_render(&nc, font, 1600, true, 1.0, &origin);
    CHECK(r2 != NULL && r2->generation != gen1);
    purp_font_destroy(font);

    purp_notify_finish(&nc);
}

/* Regression test for the D-Bus GVariant format strings used by
 * notification_method_call(): GetCapabilities must use `(^as)` (strv), and the
 * Notify parser uses `&s` borrows + `@as`/`@a{sv}` child variants. */
static void test_notification_dbus_variants(void) {
    /* GetCapabilities reply: `(^as)` round-trips through asv iteration. */
    const char *capabilities[] = { "body", NULL };
    GVariant *caps_reply = g_variant_ref_sink(g_variant_new("(^as)", capabilities));
    CHECK(caps_reply != NULL);
    CHECK(g_variant_is_of_type(caps_reply, G_VARIANT_TYPE("(as)")));
    {
        GVariant *inner = g_variant_get_child_value(caps_reply, 0);
        GVariantIter it;
        const gchar *entry;
        int count = 0;
        g_variant_iter_init(&it, inner);
        while (g_variant_iter_loop(&it, "&s", &entry)) {
            CHECK_STREQ(entry, "body");
            count++;
        }
        CHECK(count == 1);
        g_variant_unref(inner);
        g_variant_unref(caps_reply);
    }

    /* GetServerInformation reply: `(ssss)` parses cleanly. */
    GVariant *info = g_variant_ref_sink(
        g_variant_new("(ssss)", PURP_NAME, PURP_NAME, PURPWM_VERSION, "1.2"));
    CHECK(g_variant_is_of_type(info, G_VARIANT_TYPE("(ssss)")));
    g_variant_unref(info);

    /* Notify parameters with a non-empty hints dict: the handler's
     * `(&su&s&s&s@as@a{sv}i)` format must parse a byte-typed `urgency` hint. */
    {
        GVariantBuilder hints;
        g_variant_builder_init(&hints, G_VARIANT_TYPE("a{sv}"));
        g_variant_builder_add(&hints, "{sv}", "urgency",
                              g_variant_new_byte(2));
        const gchar *actions[] = { "default", "open", NULL };
        GVariant *params = g_variant_ref_sink(
            g_variant_new("(susss^asa{sv}i)",
                          "brave", (uint32_t)0, "icon",
                          "Title", "Body text",
                          actions, &hints, (int32_t)-1));
        const char *app = "", *icon = "", *summary = "", *body = "";
        uint32_t replaces_id = 0;
        int32_t expire_timeout = -1;
        GVariant *actions_v = NULL;
        GVariant *hints_v = NULL;
        g_variant_get(params, "(&su&s&s&s@as@a{sv}i)",
                      &app, &replaces_id, &icon, &summary, &body,
                      &actions_v, &hints_v, &expire_timeout);
        CHECK_STREQ(app, "brave");
        CHECK_STREQ(summary, "Title");
        CHECK_STREQ(body, "Body text");
        CHECK(expire_timeout == -1);
        CHECK(actions_v != NULL && g_variant_n_children(actions_v) == 2);
        CHECK(hints_v != NULL);
        GVariant *urg = g_variant_lookup_value(hints_v, "urgency", NULL);
        CHECK(urg != NULL && g_variant_is_of_type(urg, G_VARIANT_TYPE_BYTE));
        CHECK(g_variant_get_byte(urg) == 2);
        g_variant_unref(urg);
        if (actions_v) g_variant_unref(actions_v);
        if (hints_v) g_variant_unref(hints_v);
        g_variant_unref(params);
    }

    /* An empty actions array + empty hints dict must also parse (no crash). */
    {
        GVariantBuilder hints;
        g_variant_builder_init(&hints, G_VARIANT_TYPE("a{sv}"));
        const gchar *actions[] = { NULL };
        GVariant *params = g_variant_ref_sink(
            g_variant_new("(susss^asa{sv}i)",
                          "app", (uint32_t)7, "i", "S", "B",
                          actions, &hints, (int32_t)0));
        const char *app = "", *icon = "", *summary = "", *body = "";
        uint32_t replaces_id = 0;
        int32_t expire_timeout = -1;
        GVariant *actions_v = NULL;
        GVariant *hints_v = NULL;
        g_variant_get(params, "(&su&s&s&s@as@a{sv}i)",
                      &app, &replaces_id, &icon, &summary, &body,
                      &actions_v, &hints_v, &expire_timeout);
        CHECK(replaces_id == 7 && expire_timeout == 0);
        CHECK(actions_v != NULL && g_variant_n_children(actions_v) == 0);
        if (actions_v) g_variant_unref(actions_v);
        if (hints_v) g_variant_unref(hints_v);
        g_variant_unref(params);
    }
}

static void test_help_overlay(void) {
    purp_help_overlay_t help;
    purp_help_init(&help);
    CHECK(!help.visible);
    CHECK(purp_help_render(&help, NULL, 2560, 1600, 1.5, &(purp_point_t){ 0 }) == NULL);
    purp_help_toggle(&help);
    CHECK(help.visible);
    purp_help_toggle(&help);
    CHECK(!help.visible);

    CHECK(purp_help_section_count >= 5);
    CHECK(purp_help_sections[0].row_count > 0);
    CHECK(purp_help_sections[0].rows[0].keys != NULL);

    purp_font_t *font = purp_font_load_memory(purp_bundled_font, purp_bundled_font_len);
    CHECK(font != NULL);
    purp_pixel_buf_t buf = { 0 };
    purp_point_t origin = { 0, 0 };
    CHECK(purp_help_rasterize(font, 2560, 1600, 1.5, &buf, &origin));
    CHECK(buf.data != NULL);
    CHECK(origin.x > 0 && origin.x < 1280);
    CHECK(origin.y > 0 && origin.y < 800);
    purp_pixel_buf_free(&buf);
    purp_font_destroy(font);
    purp_help_finish(&help);
}

/* =========================================================================================== */
/* Cursor theme                                                                                */
/* =========================================================================================== */

static void test_cursor_theme(void) {
    CHECK(purp_cursor_target_px(24, 1.0) == 24);
    CHECK(purp_cursor_target_px(24, 1.5) == 36);
    CHECK(purp_cursor_target_px(24, 0.0) == PURP_CURSOR_MIN_PX);
    CHECK(purp_cursor_target_px(24, 0.2) == PURP_CURSOR_MIN_PX); /* round(4.8) < 8 */

    /* All 36 shape values have a CSS name and an alias list. */
    for (int shape = 1; shape <= PURP_CURSOR_SHAPE_MAX; ++shape) {
        const char *name = purp_cursor_shape_name((purp_cursor_shape_t)shape);
        CHECK(name != NULL && name[0] != '\0');
        CHECK(purp_cursor_shape_alt_names((purp_cursor_shape_t)shape) != NULL);
    }
    CHECK_STREQ(purp_cursor_shape_name(PURP_CURSOR_DEFAULT), "default");
    CHECK_STREQ(purp_cursor_shape_name(PURP_CURSOR_POINTER), "pointer");
    CHECK_STREQ(purp_cursor_shape_name(PURP_CURSOR_TEXT), "text");
    CHECK_STREQ(purp_cursor_shape_name(PURP_CURSOR_ALL_RESIZE), "all-resize");
    CHECK(purp_cursor_shape_name((purp_cursor_shape_t)0) == NULL);
    CHECK(purp_cursor_shape_name((purp_cursor_shape_t)37) == NULL);
    CHECK_STREQ(purp_cursor_shape_alt_names(PURP_CURSOR_DEFAULT)[0], "left_ptr");

    /* Built-in purple arrow at scale 1: 20x26, hotspot (0,0), outline pixel opaque at (0,0)
     * and transparent corner at (19,0). */
    purp_cursor_image_t arrow = { 0 };
    CHECK(purp_cursor_builtin_arrow(&arrow, 1.0));
    CHECK(arrow.buffer.width == 20 && arrow.buffer.height == 26);
    CHECK(arrow.hotspot_x == 0 && arrow.hotspot_y == 0);
    const uint8_t *px = arrow.buffer.data;
    CHECK(px[3] == 255 && px[0] == 0xFF && px[1] == 0xF5 && px[2] == 0xFA); /* W outline */
    CHECK(arrow.buffer.data[19 * 4 + 3] == 0);                             /* transparent */
    purp_pixel_buf_free(&arrow.buffer);
    memset(&arrow, 0, sizeof arrow);
    CHECK(purp_cursor_builtin_arrow(&arrow, 2.0));
    CHECK(arrow.buffer.width == 40 && arrow.buffer.height == 52);
    purp_pixel_buf_free(&arrow.buffer);

    /* Theme lookups never fail: named shape, missing shape and nonsense shape all produce a
     * raster (theme -> default -> built-in arrow). */
    purp_cursor_theme_t theme;
    purp_cursor_theme_init(&theme);
    CHECK(theme.base_size == PURP_CURSOR_DEFAULT_SIZE);
    const purp_cursor_image_t *cur = purp_cursor_theme_get(&theme, PURP_CURSOR_DEFAULT, 1.0);
    CHECK(cur != NULL && cur->buffer.data != NULL);
    CHECK(cur->buffer.width >= (int32_t)PURP_CURSOR_MIN_PX);
    const purp_cursor_image_t *again = purp_cursor_theme_get(&theme, PURP_CURSOR_DEFAULT, 1.0);
    CHECK(again == cur); /* cached */
    const purp_cursor_image_t *text = purp_cursor_theme_get(&theme, PURP_CURSOR_TEXT, 1.5);
    CHECK(text != NULL && text->buffer.data != NULL);
    const purp_cursor_image_t *bogus =
        purp_cursor_theme_get(&theme, (purp_cursor_shape_t)99, 1.0);
    CHECK(bogus != NULL && bogus->buffer.data != NULL);
    purp_cursor_theme_finish(&theme);
}

/* =========================================================================================== */
/* Wallpaper                                                                                   */
/* =========================================================================================== */

static void test_wallpaper_cover_crop(void) {
    /* Source wider than the destination: crop horizontally, full height, centred. */
    purp_rect_t c = purp_wallpaper_cover_crop(4000, 1000, 1000, 1000);
    CHECK(c.w == 1000 && c.h == 1000 && c.x == 1500 && c.y == 0);
    /* Source taller: crop vertically. */
    c = purp_wallpaper_cover_crop(1000, 4000, 1000, 1000);
    CHECK(c.w == 1000 && c.h == 1000 && c.x == 0 && c.y == 1500);
    /* Equal aspect: whole image. */
    c = purp_wallpaper_cover_crop(2000, 1000, 1000, 500);
    CHECK(c.x == 0 && c.y == 0 && c.w == 2000 && c.h == 1000);
    /* Degenerate inputs clamp to 1. */
    c = purp_wallpaper_cover_crop(0, 0, 0, 0);
    CHECK(c.w >= 1 && c.h >= 1);
}

/* A 4x2 RGBA PNG: red, green, blue, half-transparent #204080 (repeated on both rows). */
static const unsigned char TEST_PNG[] = {
    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44,
    0x52, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x02, 0x08, 0x06, 0x00, 0x00, 0x00, 0x7f,
    0xa8, 0x7d, 0x63, 0x00, 0x00, 0x00, 0x19, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9c, 0x63, 0xf8,
    0xcf, 0xc0, 0xf0, 0x1f, 0x0c, 0x19, 0xfe, 0xff, 0x57, 0x70, 0x68, 0x68, 0x00, 0xb3, 0x90,
    0x05, 0x00, 0xfa, 0x24, 0x0e, 0xb5, 0x7f, 0xf7, 0xcc, 0x28, 0x00, 0x00, 0x00, 0x00, 0x49,
    0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82
};

static void test_wallpaper_load_and_render(void) {
    purp_wallpaper_t wp;
    purp_wallpaper_init_from_env(&wp); /* PURPWM_WALLPAPER unset: nothing loaded. */
    CHECK(purp_wallpaper_get(&wp, 64, 64) == NULL);
    CHECK(!purp_wallpaper_load(&wp, "/nonexistent/wallpaper.png"));

    CHECK(test_make_tmp_dir());
    char path[320];
    snprintf(path, sizeof path, "%s/wall.png", g_tmp_dir);
    FILE *f = fopen(path, "wb");
    CHECK(f != NULL);
    CHECK(fwrite(TEST_PNG, 1, sizeof TEST_PNG, f) == sizeof TEST_PNG);
    CHECK(fclose(f) == 0);

    CHECK(purp_wallpaper_load(&wp, path));
    const purp_pixel_buf_t *buf = purp_wallpaper_get(&wp, 64, 32);
    CHECK(buf != NULL);
    CHECK(buf->width == 64 && buf->height == 32 && buf->stride >= 64 * 4);

    /* Opaque premultiplied B,G,R,A: the red source column is bright red at the left edge. */
    const uint8_t *px = buf->data;
    CHECK(px[3] == 255);
    CHECK(px[2] > 200 && px[1] < 60 && px[0] < 60);
    /* Every pixel is opaque after compositing over #11081F. */
    bool opaque = true;
    for (int32_t i = 3; i < buf->width * buf->height * 4; i += 4) opaque &= buf->data[i] == 255;
    CHECK(opaque);
    /* The half-transparent #204080 source column (dst x in [48,64) maps src x = 3) is
     * composited over the background: R = (0x20*0x80 + 0x11*0x7F)/255 = 24 etc. */
    const uint8_t *half = buf->data + (16 * buf->stride) + 56 * 4;
    CHECK(half[3] == 255);
    CHECK(half[2] >= 20 && half[2] <= 32); /* R ~24.5 */
    CHECK(half[1] >= 30 && half[1] <= 44); /* G ~36.1 */
    CHECK(half[0] >= 70 && half[0] <= 92); /* B ~79.7 */

    /* Cached per (w, h); a new aspect ratio re-renders. */
    CHECK(purp_wallpaper_get(&wp, 64, 32) == buf);
    const purp_pixel_buf_t *tall = purp_wallpaper_get(&wp, 32, 64);
    CHECK(tall != NULL && tall != buf);
    CHECK(tall->width == 32 && tall->height == 64);

    purp_wallpaper_finish(&wp);
    CHECK(purp_wallpaper_get(&wp, 64, 32) == NULL);

    /* $PURPWM_WALLPAPER drives init_from_env. */
    setenv(PURP_ENV_WALLPAPER, path, 1);
    purp_wallpaper_init_from_env(&wp);
    CHECK(wp.loaded);
    CHECK(purp_wallpaper_get(&wp, 16, 16) != NULL);
    purp_wallpaper_finish(&wp);
    unsetenv(PURP_ENV_WALLPAPER);
}

/* =========================================================================================== */
/* main                                                                                        */
/* =========================================================================================== */

static const purp_test_case_t TESTS[] = {
    { "layout_cycle", test_layout_cycle },
    { "workspace_pertag_and_gaps", test_workspace_pertag_and_gaps },
    { "layout_splits_fit_the_usable_rect", test_layout_splits_fit_the_usable_rect },
    { "weighted_column_split_keeps_the_span", test_weighted_column_split_keeps_the_span },
    { "resizing_tiled_windows_moves_the_splits", test_resizing_tiled_windows_moves_the_splits },
    { "floating_and_touch_helpers", test_floating_and_touch_helpers },
    { "keyboard_resize_glides_smoothly", test_keyboard_resize_glides_smoothly },
    { "multi_monitor_workspaces_and_reconfiguration",
      test_multi_monitor_workspaces_and_reconfiguration },
    { "moved_monitor_translates_floating_windows", test_moved_monitor_translates_floating_windows },
    { "retain_connected_monitors_remaps_middle_unplug",
      test_retain_connected_monitors_remaps_middle_unplug },
    { "adjacent_monitors_and_straddling_floats", test_adjacent_monitors_and_straddling_floats },
    { "pointer_clamping_crosses_seams_without_snag",
      test_pointer_clamping_crosses_seams_without_snag },
    { "unmapped_toplevels_enter_and_leave_the_wm", test_unmapped_toplevels_enter_and_leave_the_wm },
    { "maximize_geometry_configure_and_restore", test_maximize_geometry_configure_and_restore },
    { "focusing_or_mapping_tiles_reconciles_covering_windows",
      test_focusing_or_mapping_tiles_reconciles_covering_windows },
    { "transients_follow_and_stack_above_fullscreen_parent",
      test_transients_follow_and_stack_above_fullscreen_parent },
    { "rule_sized_float_retiles_with_nonzero_configure",
      test_rule_sized_float_retiles_with_nonzero_configure },
    { "floating_layout_focus_raises_and_fullscreen_restores",
      test_floating_layout_focus_raises_and_fullscreen_restores },
    { "spatial_arrows_swap_neighbours_without_wrapping",
      test_spatial_arrows_swap_neighbours_without_wrapping },
    { "json_parse_and_serialize", test_json_parse_and_serialize },
    { "ipc_command_parsing", test_ipc_command_parsing },
    { "monitors_json_round_trip", test_monitors_json_round_trip },
    { "ipc_server_round_trip_and_locked_rejection",
      test_ipc_server_round_trip_and_locked_rejection },
    { "capture_server_round_trip", test_capture_server_round_trip },
    { "utf8_helpers", test_utf8_helpers },
    { "bundled_font_rasterizes", test_bundled_font_rasterizes },
    { "status_bar_hit_test_and_dirty_edge", test_status_bar_hit_test_and_dirty_edge },
    { "status_parsers", test_status_parsers },
    { "lock_screen_keys", test_lock_screen_keys },
    { "pam_confdir_allow_deny_and_service_discovery",
      test_pam_confdir_allow_deny_and_service_discovery },
    { "notification_center", test_notification_center },
    { "notification_dbus_variants", test_notification_dbus_variants },
    { "help_overlay", test_help_overlay },
    { "cursor_theme", test_cursor_theme },
    { "wallpaper_cover_crop", test_wallpaper_cover_crop },
    { "wallpaper_load_and_render", test_wallpaper_load_and_render },
};

int main(void) {
    int failed = 0;
    for (size_t i = 0; i < PURP_ARRAY_LEN(TESTS); ++i) {
        g_test_name = TESTS[i].name;
        int before = g_failures;
        TESTS[i].fn();
        if (g_failures == before) {
            printf("PASS %s\n", TESTS[i].name);
        } else {
            printf("FAIL %s (%d failed checks)\n", TESTS[i].name, g_failures - before);
            ++failed;
        }
    }
    printf("%zu tests, %d failed, %d failed checks\n", PURP_ARRAY_LEN(TESTS), failed,
           g_failures);
    return failed == 0 ? 0 : 1;
}
