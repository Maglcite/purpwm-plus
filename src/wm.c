#include "wm.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>

static const char *nonempty(const char *s) { return s ? s : ""; }

static bool replace_string(char **dst, const char *value) {
    value = nonempty(value);
    if (strcmp(nonempty(*dst), value) == 0) return false;
    char *copy = strdup(value);
    if (!copy) return false;
    free(*dst);
    *dst = copy;
    return true;
}

const char *purp_layout_symbol(purp_layout_t layout) {
    static const char *const symbols[] = { "[]=", "|M|", "###", "[M]", "><>" };
    return layout >= 0 && layout < PURP_LAYOUT_COUNT ? symbols[layout] : symbols[0];
}

purp_layout_t purp_layout_next(purp_layout_t layout) {
    return layout >= 0 && layout < PURP_LAYOUT_FLOATING ? (purp_layout_t)(layout + 1) : PURP_LAYOUT_TILE;
}

void purp_window_init(purp_window_t *win, purp_toplevel_t *toplevel, uint16_t tags, int monitor,
                      purp_rect_t output_rect) {
    *win = (purp_window_t){ .toplevel = toplevel, .tags = tags, .monitor = monitor,
                           .cfact = 1.0f, .opacity_override = 1.0f };
    int32_t w = purp_max_i32(output_rect.w * 3 / 5, 360);
    int32_t h = purp_max_i32(output_rect.h * 3 / 5, 240);
    win->float_geo = purp_rect_make(output_rect.x + (output_rect.w - w) / 2,
        output_rect.y + PURP_BAR_HEIGHT + purp_max_i32(output_rect.h - PURP_BAR_HEIGHT - h, 0) / 2,
        w, h);
    win->geo = win->float_geo;
    win->app_id = strdup("");
    win->title = strdup("");
    win->rule_seen_app_id = strdup("");
    win->rule_seen_title = strdup("");
}

void purp_window_finish(purp_window_t *win) {
    free(win->app_id);
    free(win->title);
    free(win->rule_seen_app_id);
    free(win->rule_seen_title);
    win->app_id = win->title = win->rule_seen_app_id = win->rule_seen_title = NULL;
}

bool purp_window_set_app_id(purp_window_t *win, const char *app_id) {
    return replace_string(&win->app_id, app_id);
}
bool purp_window_set_title(purp_window_t *win, const char *title) {
    return replace_string(&win->title, title);
}
const char *purp_window_display_title(const purp_window_t *win) {
    if (*nonempty(win->title)) return win->title;
    return *nonempty(win->app_id) ? win->app_id : "purpwm+ client";
}
bool purp_window_is_visible_on(const purp_window_t *win, uint16_t selected_tags) {
    return (win->toplevel == NULL || win->mapped) && !win->is_hidden &&
           (win->is_scratchpad || (win->tags & selected_tags) != 0);
}
purp_point_t purp_window_content_loc(const purp_window_t *win) {
    int32_t border = win->is_fullscreen ? 0 : PURP_BORDER_WIDTH;
    return (purp_point_t){ win->geo.x + border, win->geo.y + border };
}
purp_point_t purp_window_surface_loc(const purp_window_t *win) {
    purp_point_t loc = purp_window_content_loc(win);
    if (win->has_client_geometry) {
        loc.x -= win->client_geometry.x;
        loc.y -= win->client_geometry.y;
    }
    return loc;
}
purp_size_t purp_window_surface_size(const purp_window_t *win) {
    int32_t border = win->is_fullscreen ? 0 : 2 * PURP_BORDER_WIDTH;
    return (purp_size_t){ purp_max_i32(win->geo.w - border, 1),
                          purp_max_i32(win->geo.h - border, 1) };
}
bool purp_window_client_geometry_size(const purp_window_t *win, purp_size_t *out) {
    if (!win->mapped) return false;
    purp_size_t size = win->has_client_geometry ? purp_rect_size(win->client_geometry) : win->buffer_size;
    if (size.w <= 0 || size.h <= 0) return false;
    if (out) *out = size;
    return true;
}
static int32_t clamp_client_axis(int32_t frame, int32_t min, int32_t max) {
    int32_t inner = frame - 2 * PURP_BORDER_WIDTH;
    if (max > 0) inner = purp_min_i32(inner, max);
    if (min > 0) inner = purp_max_i32(inner, min);
    return inner + 2 * PURP_BORDER_WIDTH;
}
purp_size_t purp_window_clamp_frame_to_client_limits(const purp_window_t *win, purp_size_t frame) {
    return (purp_size_t){ clamp_client_axis(frame.w, win->min_size.w, win->max_size.w),
                          clamp_client_axis(frame.h, win->min_size.h, win->max_size.h) };
}
void purp_window_configure(purp_window_t *win, bool focused, bool tiled_layout) {
    purp_size_t size = purp_window_surface_size(win);
    if (win->size_from_client && (win->is_floating || win->is_scratchpad) &&
        !win->is_fullscreen && !win->is_maximized) size = (purp_size_t){ 0, 0 };
    purp_window_configure_t next = { .width = size.w, .height = size.h,
        .activated = focused, .fullscreen = win->is_fullscreen,
        .maximized = win->is_maximized, .resizing = win->is_resizing,
        .tiled = !win->is_floating && !win->is_fullscreen && !win->is_maximized &&
                 !win->is_scratchpad && tiled_layout };
    if (next.width != win->want.width || next.height != win->want.height ||
        next.activated != win->want.activated || next.fullscreen != win->want.fullscreen ||
        next.maximized != win->want.maximized || next.resizing != win->want.resizing ||
        next.tiled != win->want.tiled) {
        win->want = next;
        win->configure_dirty = true;
    }
}
bool purp_window_rule_matches(const purp_window_rule_t *rule, const char *app_id, const char *title) {
    if (!rule->app_id_match && !rule->title_match) return false;
    app_id = nonempty(app_id);
    title = nonempty(title);
    return (!rule->app_id_match || (*app_id && strstr(app_id, rule->app_id_match))) &&
           (!rule->title_match || (*title && strstr(title, rule->title_match)));
}
bool purp_window_rules_settled(bool matched, const char *app_id, const char *title) {
    return matched || (*nonempty(app_id) && *nonempty(title));
}

void purp_workspace_init(purp_workspace_t *ws) {
    *ws = (purp_workspace_t){ .selected_tags = 1, .prev_tags = 1,
        .gap_inner = PURP_GAP_INNER, .gap_outer = PURP_GAP_OUTER,
        .gaps_enabled = true, .show_bar = true };
    for (size_t i = 0; i < PURP_TAG_COUNT; ++i)
        ws->pertag[i] = (purp_tag_state_t){ .layout = PURP_LAYOUT_TILE,
            .prev_layout = PURP_LAYOUT_FLOATING, .mfact = PURP_DEFAULT_MFACT,
            .nmaster = PURP_DEFAULT_NMASTER };
}
purp_layout_t purp_workspace_layout(const purp_workspace_t *ws) { return ws->pertag[ws->cur_tag].layout; }
purp_layout_t purp_workspace_prev_layout(const purp_workspace_t *ws) { return ws->pertag[ws->cur_tag].prev_layout; }
float purp_workspace_mfact(const purp_workspace_t *ws) { return ws->pertag[ws->cur_tag].mfact; }
int purp_workspace_nmaster(const purp_workspace_t *ws) { return ws->pertag[ws->cur_tag].nmaster; }
void purp_workspace_set_layout(purp_workspace_t *ws, purp_layout_t layout) {
    purp_tag_state_t *tag = &ws->pertag[ws->cur_tag];
    if (tag->layout != layout) { tag->prev_layout = tag->layout; tag->layout = layout; }
}
void purp_workspace_set_mfact(purp_workspace_t *ws, float mfact) {
    ws->pertag[ws->cur_tag].mfact = purp_clamp_f32(mfact, PURP_MFACT_MIN, PURP_MFACT_MAX);
}
void purp_workspace_adjust_mfact(purp_workspace_t *ws, float delta) {
    purp_workspace_set_mfact(ws, purp_workspace_mfact(ws) + delta);
}
void purp_workspace_adjust_nmaster(purp_workspace_t *ws, int delta) {
    int64_t next = (int64_t)purp_workspace_nmaster(ws) + delta;
    ws->pertag[ws->cur_tag].nmaster = (int)fmin(fmax((double)next, 0.0), INT_MAX);
}
void purp_workspace_adjust_gaps(purp_workspace_t *ws, int32_t delta) {
    ws->gaps_enabled = true;
    ws->gap_inner = (int32_t)purp_clamp_f64((double)ws->gap_inner + delta, 0, PURP_GAP_INNER_MAX);
    ws->gap_outer = (int32_t)purp_clamp_f64((double)ws->gap_outer + delta, 0, PURP_GAP_OUTER_MAX);
}
void purp_workspace_reset_gaps(purp_workspace_t *ws) {
    ws->gap_inner = PURP_GAP_INNER;
    ws->gap_outer = PURP_GAP_OUTER;
    ws->gaps_enabled = true;
}
void purp_workspace_toggle_gaps(purp_workspace_t *ws) { ws->gaps_enabled = !ws->gaps_enabled; }
void purp_workspace_sync_cur_tag(purp_workspace_t *ws) {
    for (int i = 0; i < PURP_TAG_COUNT; ++i)
        if ((ws->selected_tags & (1u << i)) != 0) { ws->cur_tag = i; return; }
}
int32_t purp_workspace_inner_gap(const purp_workspace_t *ws) { return ws->gaps_enabled ? ws->gap_inner : 0; }
purp_rect_t purp_workspace_usable_area_in(const purp_workspace_t *ws, purp_rect_t output_rect) {
    int32_t bar = ws->show_bar ? PURP_BAR_HEIGHT : 0;
    int32_t gap = ws->gaps_enabled ? ws->gap_outer : 0;
    return purp_rect_make(output_rect.x + gap, output_rect.y + bar + gap,
        purp_max_i32(output_rect.w - 2 * gap, 1), purp_max_i32(output_rect.h - bar - 2 * gap, 1));
}
static purp_rect_t maximized_area_in(const purp_workspace_t *ws, purp_rect_t output_rect) {
    int32_t bar = ws->show_bar ? PURP_BAR_HEIGHT : 0;
    return purp_rect_make(output_rect.x, output_rect.y + bar, output_rect.w,
                          purp_max_i32(output_rect.h - bar, 1));
}
bool purp_workspace_centered_sides(const purp_workspace_t *ws, size_t tiled_count) {
    size_t masters = (size_t)purp_max_i32(purp_workspace_nmaster(ws), 0);
    return purp_workspace_layout(ws) == PURP_LAYOUT_CENTERED_MASTER && masters > 0 &&
           tiled_count > masters + 1;
}
size_t purp_workspace_tiled_indices(const purp_workspace_t *ws, purp_window_t *const *windows,
                                    size_t count, int monitor, size_t *out, size_t cap) {
    size_t n = 0;
    for (size_t i = 0; i < count && n < cap; ++i) {
        const purp_window_t *win = windows[i];
        if ((monitor < 0 || win->monitor == monitor) && purp_window_is_visible_on(win, ws->selected_tags) &&
            !win->is_floating && !win->is_fullscreen && !win->is_maximized &&
            !win->is_scratchpad) out[n++] = i;
    }
    return n;
}

static int32_t split_gap(int32_t total, size_t count, int32_t gap) {
    if (count <= 1) return 0;
    int64_t available = (int64_t)total - (int64_t)count;
    int64_t max_gap = available > 0 ? available / (int64_t)(count - 1) : 0;
    return (int32_t)fmin((double)purp_max_i32(gap, 0), (double)max_gap);
}
int32_t purp_split_lengths(int32_t total, size_t count, int32_t gap, int32_t *lengths) {
    if (count == 0) return 0;
    total = purp_max_i32(total, 0);
    gap = split_gap(total, count, gap);
    int64_t span = total - (int64_t)gap * (int64_t)(count - 1);
    int32_t base = (int32_t)(span / (int64_t)count);
    size_t remainder = (size_t)(span % (int64_t)count);
    for (size_t i = 0; i < count; ++i) lengths[i] = base + (i < remainder ? 1 : 0);
    return gap;
}
static double weight_value(float value) { return isfinite(value) && value >= PURP_MIN_CFACT ? value : PURP_MIN_CFACT; }
int32_t purp_split_weighted(int32_t total, const float *weights, size_t count, int32_t gap,
                            int32_t *lengths) {
    if (count == 0) return 0;
    bool equal = true;
    double sum = 0.0;
    for (size_t i = 0; i < count; ++i) {
        double value = weight_value(weights[i]);
        if (weights[i] != weights[0]) equal = false;
        sum += value;
    }
    if (equal) return purp_split_lengths(total, count, gap, lengths);
    total = purp_max_i32(total, 0);
    gap = split_gap(total, count, gap);
    int32_t span = (int32_t)(total - (int64_t)gap * (int64_t)(count - 1));
    double accumulated = 0.0;
    int32_t previous = 0;
    for (size_t i = 0; i < count; ++i) {
        accumulated += weight_value(weights[i]);
        int32_t end = i + 1 == count ? span : purp_round_i32(span * accumulated / sum);
        lengths[i] = end - previous;
        previous = end;
    }
    return gap;
}
int32_t purp_split_span(const int32_t *lengths, size_t count, int32_t gap) {
    int64_t span = count ? (int64_t)gap * (int64_t)(count - 1) : 0;
    for (size_t i = 0; i < count; ++i) span += lengths[i];
    return (int32_t)span;
}
void purp_master_stack_widths(int32_t usable_w, int32_t gap, float mfact, int32_t *master,
                              int32_t *stack, int32_t *gap_out) {
    usable_w = purp_max_i32(usable_w, 0);
    if (usable_w < 2) { *master = usable_w; *stack = 0; *gap_out = 0; return; }
    int32_t actual_gap = split_gap(usable_w, 2, gap);
    int32_t span = usable_w - actual_gap;
    int32_t mw = purp_round_i32(span * purp_clamp_f32(mfact, PURP_MFACT_MIN, PURP_MFACT_MAX));
    if (span >= 2) mw = purp_clamp_i32(mw, 1, span - 1);
    *master = mw;
    *stack = span - mw;
    *gap_out = actual_gap;
}
void purp_centered_side_widths(int32_t usable_w, int32_t gap, float mfact, int32_t *left,
                               int32_t *master, int32_t *right, int32_t *gap_out) {
    usable_w = purp_max_i32(usable_w, 0);
    if (usable_w < 3) { *left = 0; *master = usable_w; *right = 0; *gap_out = 0; return; }
    int32_t actual_gap = split_gap(usable_w, 3, gap);
    int32_t span = usable_w - 2 * actual_gap;
    int32_t mw = purp_round_i32(span * purp_clamp_f32(mfact, PURP_MFACT_MIN, PURP_MFACT_MAX));
    if (span >= 3) mw = purp_clamp_i32(mw, 1, span - 2);
    *left = (span - mw) / 2;
    *master = mw;
    *right = span - mw - *left;
    *gap_out = actual_gap;
}
purp_rect_t purp_scratchpad_geo_in(purp_rect_t output_rect, bool show_bar) {
    int32_t bar = show_bar ? PURP_BAR_HEIGHT : 0;
    int32_t w = purp_min_i32(purp_max_i32(output_rect.w * 7 / 10, 480), purp_max_i32(output_rect.w - 16, 1));
    int32_t h = purp_min_i32(purp_max_i32(output_rect.h * 7 / 10, 320), purp_max_i32(output_rect.h - bar - 16, 1));
    int32_t x = purp_clamp_i32((output_rect.w - w) / 2, 0, purp_max_i32(output_rect.w - w, 0));
    int32_t y = purp_clamp_i32(bar + purp_max_i32((output_rect.h - bar - h) / 2, 0),
                             0, purp_max_i32(output_rect.h - h, 0));
    return purp_rect_make(output_rect.x + x, output_rect.y + y, w, h);
}
purp_rect_t purp_floating_layout_geo(purp_rect_t geo, purp_rect_t float_geo, bool is_floating,
                                     bool is_scratchpad) {
    return is_floating || is_scratchpad ? float_geo : geo;
}
purp_rect_t purp_resize_rect(purp_rect_t start, int32_t dx, int32_t dy, uint32_t edges,
                             int32_t min_w, int32_t min_h) {
    purp_rect_t result = start;
    min_w = purp_max_i32(min_w, 1);
    min_h = purp_max_i32(min_h, 1);
    if ((edges & PURP_EDGE_LEFT) != 0) {
        result.w = purp_max_i32(start.w - dx, min_w);
        result.x = start.x + start.w - result.w;
    } else if ((edges & PURP_EDGE_RIGHT) != 0) result.w = purp_max_i32(start.w + dx, min_w);
    if ((edges & PURP_EDGE_TOP) != 0) {
        result.h = purp_max_i32(start.h - dy, min_h);
        result.y = start.y + start.h - result.h;
    } else if ((edges & PURP_EDGE_BOTTOM) != 0) result.h = purp_max_i32(start.h + dy, min_h);
    return result;
}
size_t purp_focus_after_zoom(size_t master, size_t focused, size_t target) {
    return focused == master ? master : target;
}
purp_point_t purp_clamp_floating_loc_in(purp_point_t loc, purp_size_t size, purp_rect_t output_rect,
                                        bool show_bar) {
    int32_t top = output_rect.y + (show_bar ? PURP_BAR_HEIGHT : 0);
    int32_t min_x = output_rect.x - size.w + PURP_VISIBLE_STRIP;
    loc.x = purp_clamp_i32(loc.x, min_x, purp_max_i32(output_rect.x + output_rect.w - PURP_VISIBLE_STRIP, min_x));
    loc.y = purp_clamp_i32(loc.y, top, purp_max_i32(top, output_rect.y + output_rect.h - PURP_VISIBLE_STRIP));
    return loc;
}
purp_rect_t purp_clamp_floating_geo_in(purp_rect_t geo, purp_rect_t output_rect, bool show_bar) {
    int32_t top = output_rect.y + (show_bar ? PURP_BAR_HEIGHT : 0);
    geo.w = purp_clamp_i32(geo.w, 1, purp_max_i32(output_rect.w, 1));
    geo.h = purp_clamp_i32(geo.h, 1, purp_max_i32(output_rect.y + output_rect.h - top, 1));
    geo.x = purp_clamp_i32(geo.x, output_rect.x, purp_max_i32(output_rect.x + output_rect.w - geo.w, output_rect.x));
    geo.y = purp_clamp_i32(geo.y, top, purp_max_i32(top, output_rect.y + output_rect.h - geo.h));
    return geo;
}
purp_size_t purp_logical_output_size_for(purp_size_t physical, double scale) {
    scale = fmax(scale, 0.25);
    return (purp_size_t){ purp_max_i32(purp_round_i32(physical.w / scale), 320),
                          purp_max_i32(purp_round_i32(physical.h / scale), 240) };
}
purp_pointf_t purp_touch_physical_to_logical(double phys_x, double phys_y, double scale,
                                             purp_size_t output_size) {
    scale = fmax(scale, 0.1);
    return (purp_pointf_t){ purp_clamp_f64(phys_x / scale, 0.0, fmax(output_size.w - 1e-4, 0.0)),
                           purp_clamp_f64(phys_y / scale, 0.0, fmax(output_size.h - 1e-4, 0.0)) };
}

static void arrange_column(purp_window_t *const *windows, const size_t *indices, size_t count,
                           size_t stride, purp_rect_t area, int32_t gap) {
    if (count == 0) return;
    float weights[count];
    int32_t lengths[count];
    for (size_t i = 0; i < count; ++i) weights[i] = windows[indices[i * stride]]->cfact;
    int32_t actual_gap = purp_split_weighted(area.h, weights, count, gap, lengths);
    for (size_t i = 0; i < count; ++i) {
        windows[indices[i * stride]]->geo = purp_rect_make(area.x, area.y, area.w, lengths[i]);
        area.y += lengths[i] + actual_gap;
    }
}

void purp_workspace_arrange_in(const purp_workspace_t *ws, purp_window_t *const *windows,
                               size_t count, int focused_idx, purp_rect_t output_rect,
                               int monitor) {
    size_t indices[count ? count : 1];
    size_t n = purp_workspace_tiled_indices(ws, windows, count, monitor, indices, count);
    size_t masters = (size_t)purp_max_i32(purp_workspace_nmaster(ws), 0);
    if (masters > n) masters = n;
    purp_layout_t layout = purp_workspace_layout(ws);
    purp_rect_t usable = purp_workspace_usable_area_in(ws, output_rect);
    int32_t gap = purp_workspace_inner_gap(ws);
    if (layout == PURP_LAYOUT_FLOATING) {
        for (size_t i = 0; i < count; ++i) {
            purp_window_t *win = windows[i];
            if ((monitor < 0 || win->monitor == monitor) &&
                purp_window_is_visible_on(win, ws->selected_tags) &&
                !win->is_fullscreen && !win->is_maximized) {
                win->geo = purp_floating_layout_geo(win->geo, win->float_geo,
                                                   win->is_floating, win->is_scratchpad);
                win->float_geo = win->geo;
            }
        }
    } else if (layout == PURP_LAYOUT_MONOCLE) {
        for (size_t i = 0; i < n; ++i) windows[indices[i]]->geo = usable;
    } else if (n > 0 && (layout == PURP_LAYOUT_TILE || layout == PURP_LAYOUT_CENTERED_MASTER)) {
        if (masters == 0 || masters == n) {
            arrange_column(windows, indices, n, 1, usable, gap);
        } else if (layout == PURP_LAYOUT_TILE || n - masters == 1) {
            int32_t mw, sw, col_gap;
            purp_master_stack_widths(usable.w, gap, purp_workspace_mfact(ws), &mw, &sw, &col_gap);
            arrange_column(windows, indices, masters, 1,
                           purp_rect_make(usable.x, usable.y, mw, usable.h), gap);
            arrange_column(windows, indices + masters, n - masters, 1,
                           purp_rect_make(usable.x + mw + col_gap, usable.y, sw, usable.h), gap);
        } else {
            int32_t lw, mw, rw, col_gap;
            purp_centered_side_widths(usable.w, gap, purp_workspace_mfact(ws), &lw, &mw, &rw, &col_gap);
            int32_t mx = usable.x + lw + col_gap;
            arrange_column(windows, indices, masters, 1, purp_rect_make(mx, usable.y, mw, usable.h), gap);
            arrange_column(windows, indices + masters, (n - masters + 1) / 2, 2,
                           purp_rect_make(usable.x, usable.y, lw, usable.h), gap);
            arrange_column(windows, indices + masters + 1, (n - masters) / 2, 2,
                           purp_rect_make(mx + mw + col_gap, usable.y, rw, usable.h), gap);
        }
    } else if (n > 0 && layout == PURP_LAYOUT_GRID) {
        size_t cols = (size_t)ceil(sqrt((double)n));
        size_t rows = (n + cols - 1) / cols;
        int32_t widths[cols];
        int32_t col_gap = purp_split_lengths(usable.w, cols, gap, widths);
        int32_t x = usable.x;
        for (size_t c = 0; c < cols && c * rows < n; ++c) {
            size_t col_count = n - c * rows;
            if (col_count > rows) col_count = rows;
            arrange_column(windows, indices + c * rows, col_count, 1,
                           purp_rect_make(x, usable.y, widths[c], usable.h), gap);
            x += widths[c] + col_gap;
        }
    }
    for (size_t i = 0; i < count; ++i) {
        purp_window_t *win = windows[i];
        if (monitor >= 0 && win->monitor != monitor) continue;
        if (!purp_window_is_visible_on(win, ws->selected_tags)) {
            if (win->want.activated) { win->want.activated = false; win->configure_dirty = true; }
            continue;
        }
        if (win->is_fullscreen) win->geo = output_rect;
        else if (win->is_maximized) win->geo = maximized_area_in(ws, output_rect);
        else if (win->is_floating || win->is_scratchpad) win->geo = win->float_geo;
        purp_window_configure(win, focused_idx >= 0 && (size_t)focused_idx == i,
                              layout != PURP_LAYOUT_FLOATING);
    }
}

bool purp_tiled_resize_is_empty(const purp_tiled_resize_t *plan) {
    return !plan->has_mfact && !plan->has_rows;
}
void purp_row_split_cfacts(const purp_row_split_t *rows, int32_t dy, float *upper, float *lower) {
    int32_t lo = purp_min_i32(PURP_MIN_TILED_ROW - rows->upper_h, 0);
    int32_t hi = purp_max_i32(rows->lower_h - PURP_MIN_TILED_ROW, 0);
    int32_t d = purp_clamp_i32(dy, lo, hi);
    *upper = (float)(rows->upper_h + d) / rows->px_per_cfact;
    *lower = (float)(rows->lower_h - d) / rows->px_per_cfact;
}
purp_tiled_resize_t purp_workspace_plan_tiled_resize_in(const purp_workspace_t *ws,
                                                        purp_rect_t output_rect,
                                                        const purp_tiled_entry_t *tiled,
                                                        size_t count, uint64_t target_key,
                                                        uint32_t edges, int monitor) {
    purp_tiled_resize_t plan = { .monitor = monitor };
    purp_layout_t layout = purp_workspace_layout(ws);
    if (layout == PURP_LAYOUT_MONOCLE || layout == PURP_LAYOUT_FLOATING || edges == 0) return plan;
    size_t target = count;
    for (size_t i = 0; i < count; ++i) if (tiled[i].key == target_key) { target = i; break; }
    if (target == count) return plan;
    purp_rect_t geo = tiled[target].geo;
    purp_rect_t area = purp_workspace_usable_area_in(ws, output_rect);
    int32_t gap = purp_workspace_inner_gap(ws);
    if ((edges & (PURP_EDGE_LEFT | PURP_EDGE_RIGHT)) != 0 &&
        (layout == PURP_LAYOUT_TILE || layout == PURP_LAYOUT_CENTERED_MASTER)) {
        int32_t first = (edges & PURP_EDGE_RIGHT) != 0 ? geo.x + geo.w : geo.x;
        int32_t second = (edges & PURP_EDGE_RIGHT) != 0 ? geo.x : geo.x + geo.w;
        int32_t divider = first > area.x && first < area.x + area.w ? first : second;
        if (divider > area.x && divider < area.x + area.w) {
            float per_px;
            if (purp_workspace_centered_sides(ws, count)) {
                float sign = (int64_t)divider * 2 < (int64_t)area.x * 2 + area.w ? -1.0f : 1.0f;
                per_px = sign * 2.0f / (float)purp_max_i32(area.w - 2 * gap, 1);
            } else per_px = 1.0f / (float)purp_max_i32(area.w - gap, 1);
            plan.has_mfact = true;
            plan.mfact = (purp_mfact_split_t){ purp_workspace_mfact(ws), per_px };
        }
    }
    if ((edges & (PURP_EDGE_TOP | PURP_EDGE_BOTTOM)) != 0) {
        size_t above = count, below = count;
        int64_t heights = 0;
        float weights = 0.0f;
        for (size_t i = 0; i < count; ++i) {
            if (tiled[i].geo.x != geo.x || tiled[i].geo.w != geo.w) continue;
            heights += tiled[i].geo.h;
            weights += (float)weight_value(tiled[i].cfact);
            if (tiled[i].geo.y < geo.y && (above == count || tiled[i].geo.y > tiled[above].geo.y)) above = i;
            if (tiled[i].geo.y > geo.y && (below == count || tiled[i].geo.y < tiled[below].geo.y)) below = i;
        }
        bool use_below = (edges & PURP_EDGE_BOTTOM) != 0 ? below != count : above == count;
        size_t upper = use_below ? target : above, lower = use_below ? below : target;
        if (upper < count && lower < count && weights > 0.0f && heights > 0) {
            plan.has_rows = true;
            plan.rows = (purp_row_split_t){ .upper = tiled[upper].key, .lower = tiled[lower].key,
                .upper_h = tiled[upper].geo.h, .lower_h = tiled[lower].geo.h,
                .px_per_cfact = (float)heights / weights };
        }
    }
    return plan;
}

static double rect_distance_squared(purp_rect_t rect, purp_pointf_t point) {
    double x = purp_clamp_f64(point.x, rect.x, (double)rect.x + rect.w);
    double y = purp_clamp_f64(point.y, rect.y, (double)rect.y + rect.h);
    return (point.x - x) * (point.x - x) + (point.y - y) * (point.y - y);
}
purp_point_t purp_clamp_floating_loc_across_monitors(purp_point_t loc, purp_size_t size,
                                                     const purp_monitor_t *monitors, size_t count) {
    size_t enabled = 0, best = 0;
    int64_t best_overlap = -1;
    double best_distance = INFINITY;
    purp_pointf_t anchor = { loc.x + size.w / 2.0, loc.y + purp_min_i32(16, size.h / 2) };
    int32_t min_x = INT32_MAX, min_y = INT32_MAX, max_x = INT32_MIN, max_y = INT32_MIN;
    purp_rect_t frame = purp_rect_make(loc.x, loc.y, size.w, size.h);
    for (size_t i = 0; i < count; ++i) {
        if (!monitors[i].enabled) continue;
        ++enabled;
        purp_rect_t rect = purp_monitor_rect(&monitors[i]), intersection;
        int64_t overlap = purp_rect_intersect(frame, rect, &intersection) ?
                          (int64_t)intersection.w * intersection.h : 0;
        double distance = rect_distance_squared(rect, anchor);
        if (overlap > best_overlap || (overlap == best_overlap && distance <= best_distance)) {
            best = i; best_overlap = overlap; best_distance = distance;
        }
        min_x = purp_min_i32(min_x, rect.x + PURP_VISIBLE_STRIP - size.w);
        max_x = purp_max_i32(max_x, rect.x + rect.w - PURP_VISIBLE_STRIP);
        min_y = purp_min_i32(min_y, rect.y + (monitors[i].workspace.show_bar ? PURP_BAR_HEIGHT : 0));
        max_y = purp_max_i32(max_y, rect.y + rect.h - PURP_VISIBLE_STRIP);
    }
    if (enabled == 0) return loc;
    purp_rect_t best_rect = purp_monitor_rect(&monitors[best]);
    if (enabled == 1) return purp_clamp_floating_loc_in(loc, size, best_rect, monitors[best].workspace.show_bar);
    purp_point_t clamped = { purp_clamp_i32(loc.x, min_x, purp_max_i32(max_x, min_x)),
                             purp_clamp_i32(loc.y, min_y, purp_max_i32(max_y, min_y)) };
    bool visible = false, has_above = false;
    frame.x = clamped.x; frame.y = clamped.y;
    for (size_t i = 0; i < count; ++i) {
        if (!monitors[i].enabled) continue;
        purp_rect_t rect = purp_monitor_rect(&monitors[i]), intersection;
        if (purp_rect_intersect(frame, rect, &intersection) &&
            intersection.w >= purp_min_i32(PURP_VISIBLE_STRIP, size.w) &&
            intersection.h >= purp_min_i32(PURP_VISIBLE_STRIP, size.h)) visible = true;
        if (rect.y + rect.h <= best_rect.y + 4) has_above = true;
    }
    if (!visible) return purp_clamp_floating_loc_in(clamped, size, best_rect, monitors[best].workspace.show_bar);
    int32_t top = best_rect.y + (monitors[best].workspace.show_bar ? PURP_BAR_HEIGHT : 0);
    if (!has_above && clamped.y < top && anchor.x >= best_rect.x && anchor.x < best_rect.x + best_rect.w)
        clamped.y = top;
    return clamped;
}

purp_pointf_t purp_clamp_pointer_to_monitors(const purp_monitor_t *monitors, size_t count,
                                             purp_pointf_t from, purp_pointf_t delta) {
    purp_pointf_t candidate = { from.x + delta.x, from.y + delta.y };
    size_t source = count;
    bool any = false;
    for (size_t i = 0; i < count; ++i) {
        if (!monitors[i].enabled || monitors[i].logical_size.w <= 0 || monitors[i].logical_size.h <= 0) continue;
        any = true;
        purp_rect_t rect = purp_monitor_rect(&monitors[i]);
        if (purp_rect_contains_f(rect, candidate)) return candidate;
        if (source == count && purp_rect_contains_f(rect, from)) source = i;
    }
    if (!any) return (purp_pointf_t){ fmax(candidate.x, 0.0), fmax(candidate.y, 0.0) };
    if (source < count) {
        purp_rect_t src = purp_monitor_rect(&monitors[source]);
        for (int axis = 0; axis < 2; ++axis) {
            bool crossing = axis == 0 ? delta.x != 0.0 && (candidate.x < src.x || candidate.x >= src.x + src.w) :
                                       delta.y != 0.0 && (candidate.y < src.y || candidate.y >= src.y + src.h);
            if (!crossing) continue;
            for (size_t i = 0; i < count; ++i) {
                if (!monitors[i].enabled || monitors[i].logical_size.w <= 0 || monitors[i].logical_size.h <= 0) continue;
                purp_rect_t dst = purp_monitor_rect(&monitors[i]);
                if (axis == 0 && candidate.x >= dst.x && candidate.x < dst.x + dst.w &&
                    dst.y < src.y + src.h && dst.y + dst.h > src.y)
                    return (purp_pointf_t){ candidate.x, purp_clamp_f64(candidate.y, dst.y, fmax(dst.y + dst.h - 1.0, dst.y)) };
                if (axis == 1 && candidate.y >= dst.y && candidate.y < dst.y + dst.h &&
                    dst.x < src.x + src.w && dst.x + dst.w > src.x)
                    return (purp_pointf_t){ purp_clamp_f64(candidate.x, dst.x, fmax(dst.x + dst.w - 1.0, dst.x)), candidate.y };
            }
        }
    }
    double best_distance = INFINITY;
    purp_pointf_t best = candidate;
    for (size_t i = 0; i < count; ++i) {
        if (!monitors[i].enabled || monitors[i].logical_size.w <= 0 || monitors[i].logical_size.h <= 0) continue;
        purp_rect_t rect = purp_monitor_rect(&monitors[i]);
        purp_pointf_t clamped = {
            purp_clamp_f64(candidate.x, rect.x, fmax((double)rect.x + rect.w - 1e-4, rect.x)),
            purp_clamp_f64(candidate.y, rect.y, fmax((double)rect.y + rect.h - 1e-4, rect.y)) };
        double distance = (candidate.x - clamped.x) * (candidate.x - clamped.x) +
                          (candidate.y - clamped.y) * (candidate.y - clamped.y);
        if (distance < best_distance) { best_distance = distance; best = clamped; }
    }
    return best;
}

static bool internal_name(const char *name) {
    char lower[256];
    size_t i = 0;
    for (; name[i] && i + 1 < sizeof(lower); ++i) lower[i] = (char)tolower((unsigned char)name[i]);
    lower[i] = '\0';
    return strstr(lower, "edp") || strstr(lower, "lvds") || strstr(lower, "dsi");
}
static bool parse_dimensions(const char *text, uint32_t *width, uint32_t *height) {
    char *end;
    errno = 0;
    unsigned long w = strtoul(text, &end, 10);
    if (end == text || *end != 'x' || errno || w > INT32_MAX) return false;
    const char *start = end + 1;
    unsigned long h = strtoul(start, &end, 10);
    while (isspace((unsigned char)*end)) ++end;
    if (end == start || *end || errno || h > INT32_MAX) return false;
    *width = (uint32_t)w; *height = (uint32_t)h;
    return true;
}
bool purp_pick_preferred_mode(const char *const *names, const char *const *modes, size_t count,
                              uint32_t *width, uint32_t *height) {
    bool found = false;
    uint32_t chosen_w = 0, chosen_h = 0;
    for (size_t i = 0; i < count; ++i) {
        uint32_t w, h;
        if (!modes[i] || !parse_dimensions(modes[i], &w, &h) || w < 640 || h < 480) continue;
        if (!found || internal_name(nonempty(names[i]))) { chosen_w = w; chosen_h = h; found = true; }
        if (internal_name(nonempty(names[i]))) break;
    }
    if (found) { if (width) *width = chosen_w; if (height) *height = chosen_h; }
    return found;
}
void purp_detect_preferred_resolution(uint32_t *width, uint32_t *height) {
    uint32_t chosen_w = 2560, chosen_h = 1600;
    struct dirent **entries = NULL;
    int n = scandir("/sys/class/drm", &entries, NULL, alphasort);
    bool found = false, found_internal = false;
    for (int i = 0; i < n; ++i) {
        if (!found_internal && strchr(entries[i]->d_name, '-') != NULL) {
            char path[sizeof("/sys/class/drm/") + 256 + sizeof("/status")];
            (void)snprintf(path, sizeof(path), "/sys/class/drm/%s/status", entries[i]->d_name);
            FILE *file = fopen(path, "r");
            char line[128];
            bool connected = file && fgets(line, sizeof(line), file) &&
                             (strcmp(line, "connected\n") == 0 || strcmp(line, "connected") == 0);
            if (file) fclose(file);
            if (connected) {
                (void)snprintf(path, sizeof(path), "/sys/class/drm/%s/modes", entries[i]->d_name);
                file = fopen(path, "r");
                uint32_t w, h;
                if (file && fgets(line, sizeof(line), file) && parse_dimensions(line, &w, &h) && w >= 640 && h >= 480) {
                    bool internal = internal_name(entries[i]->d_name);
                    if (!found || internal) { chosen_w = w; chosen_h = h; found = true; }
                    if (internal) found_internal = true;
                }
                if (file) fclose(file);
            }
        }
        free(entries[i]);
    }
    free(entries);
    if (width) *width = chosen_w;
    if (height) *height = chosen_h;
}
size_t purp_default_virtual_modes(purp_size_t primary, int32_t refresh_mhz,
                                  purp_display_mode_t *out, size_t cap) {
    static const purp_display_mode_t stock[] = {
        { 3840, 2160, 60000, false }, { 2560, 1600, 120000, false },
        { 2560, 1440, 144000, false }, { 1920, 1200, 60000, false },
        { 1920, 1080, 120000, false }, { 1920, 1080, 60000, false },
        { 1600, 900, 60000, false }, { 1280, 720, 60000, false },
    };
    if (cap == 0) return 0;
    size_t n = 1;
    out[0] = (purp_display_mode_t){ (uint32_t)purp_max_i32(primary.w, 640),
        (uint32_t)purp_max_i32(primary.h, 480), refresh_mhz, true };
    for (size_t i = 0; i < PURP_ARRAY_LEN(stock) && n < cap; ++i) {
        bool duplicate = false;
        for (size_t j = 0; j < n; ++j)
            if (out[j].width == stock[i].width && out[j].height == stock[i].height &&
                llabs((long long)out[j].refresh_mhz - stock[i].refresh_mhz) < 500) duplicate = true;
        if (!duplicate) out[n++] = stock[i];
    }
    return n;
}
static char *trim(char *text) {
    while (isspace((unsigned char)*text)) ++text;
    char *end = text + strlen(text);
    while (end > text && isspace((unsigned char)end[-1])) --end;
    *end = '\0';
    return text;
}
static double parse_number_or(const char *text, double fallback) {
    if (!text) return fallback;
    char *end;
    double value = strtod(text, &end);
    while (isspace((unsigned char)*end)) ++end;
    return end != text && !*end && isfinite(value) ? value : fallback;
}
size_t purp_parse_virtual_monitors(const char *spec, purp_monitor_desc_t *out, size_t cap) {
    if (!spec || cap == 0) return 0;
    char *copy = strdup(spec);
    if (!copy) return 0;
    size_t n = 0;
    int32_t x = 0;
    char *context = NULL;
    for (char *item = strtok_r(copy, ",", &context); item && n < cap; item = strtok_r(NULL, ",", &context)) {
        item = trim(item);
        char *dimensions = strchr(item, ':');
        if (!dimensions) continue;
        *dimensions++ = '\0';
        char *name = trim(item);
        if (!*name) continue;
        char *scale_text = strchr(dimensions, ':');
        if (scale_text) *scale_text++ = '\0';
        char *refresh_text = strchr(dimensions, '@');
        if (refresh_text) *refresh_text++ = '\0';
        uint32_t w, h;
        if (!parse_dimensions(trim(dimensions), &w, &h) || w < 320 || h < 240) continue;
        double hz = refresh_text ? parse_number_or(trim(refresh_text), 60.0) : 120.0;
        double scale = purp_clamp_f64(parse_number_or(scale_text ? trim(scale_text) : NULL, 1.0), 0.5, 4.0);
        purp_monitor_desc_t *desc = &out[n++];
        *desc = (purp_monitor_desc_t){ .physical_size = { (int32_t)w, (int32_t)h },
            .refresh_mhz = purp_round_i32(hz * 1000.0), .scale = scale,
            .has_position = true, .position = { x, 0 }, .is_virtual = true };
        (void)snprintf(desc->name, sizeof(desc->name), "%s", name);
        (void)snprintf(desc->description, sizeof(desc->description), "%s (Virtual Display)", desc->name);
        (void)snprintf(desc->make, sizeof(desc->make), "%s", "purpwm+");
        (void)snprintf(desc->model, sizeof(desc->model), "%s", desc->name);
        desc->mode_count = purp_default_virtual_modes(desc->physical_size, desc->refresh_mhz, desc->modes, PURP_MAX_MODES);
        x += purp_logical_output_size_for(desc->physical_size, scale).w;
    }
    free(copy);
    return n;
}
static const char *bare_monitor_name(const char *name) {
    name = nonempty(name);
    if (strncmp(name, "purpwm+-", 8) == 0) return name + 8;
    if (strncmp(name, "purpwm-", 7) == 0) return name + 7;
    return name;
}
bool purp_monitor_name_matches(const char *monitor_name, const char *wanted) {
    return strcmp(bare_monitor_name(monitor_name), bare_monitor_name(wanted)) == 0;
}

void purp_wm_init(purp_wm_t *wm, double default_scale) {
    *wm = (purp_wm_t){ .focused_idx = -1, .focused_monitor = 0, .next_window_id = 1,
        .default_scale = purp_clamp_f64(default_scale, PURP_SCALE_MIN, PURP_SCALE_MAX) };
}
void purp_wm_finish(purp_wm_t *wm) {
    for (size_t i = 0; i < wm->window_count; ++i) {
        purp_window_finish(wm->windows[i]);
        free(wm->windows[i]);
    }
    free(wm->windows);
    *wm = (purp_wm_t){ .focused_idx = -1 };
}
int purp_wm_find_monitor(const purp_wm_t *wm, const char *name) {
    if (!name) return -1;
    for (size_t i = 0; i < wm->monitor_count; ++i)
        if (purp_monitor_name_matches(wm->monitors[i].name, name)) return (int)i;
    return -1;
}
static void copy_monitor_description(purp_monitor_t *mon, const purp_monitor_desc_t *desc) {
    (void)snprintf(mon->description, sizeof(mon->description), "%s", desc->description);
    (void)snprintf(mon->make, sizeof(mon->make), "%s", desc->make);
    (void)snprintf(mon->model, sizeof(mon->model), "%s", desc->model);
    mon->physical_size_mm = desc->physical_size_mm;
    mon->subpixel = desc->subpixel;
    mon->is_internal = desc->is_internal;
    mon->is_virtual = desc->is_virtual;
    mon->mode_count = desc->mode_count < PURP_MAX_MODES ? desc->mode_count : PURP_MAX_MODES;
    memcpy(mon->modes, desc->modes, mon->mode_count * sizeof(*mon->modes));
}
int purp_wm_add_monitor(purp_wm_t *wm, const purp_monitor_desc_t *desc) {
    int existing = purp_wm_find_monitor(wm, desc->name);
    if (existing >= 0) {
        copy_monitor_description(&wm->monitors[existing], desc);
        wm->monitors_dirty = true;
        return existing;
    }
    if (wm->monitor_count == PURP_MAX_MONITORS) return -1;
    int32_t right = 0;
    for (size_t i = 0; i < wm->monitor_count; ++i)
        if (wm->monitors[i].enabled) right = purp_max_i32(right, wm->monitors[i].loc.x + wm->monitors[i].logical_size.w);
    size_t idx = wm->monitor_count++;
    purp_monitor_t *mon = &wm->monitors[idx];
    *mon = (purp_monitor_t){ .enabled = true, .primary = idx == 0,
        .physical_size = desc->physical_size, .refresh_mhz = desc->refresh_mhz,
        .scale = desc->scale, .loc = desc->has_position ? desc->position : (purp_point_t){ right, 0 } };
    (void)snprintf(mon->name, sizeof(mon->name), "%s", desc->name);
    copy_monitor_description(mon, desc);
    mon->logical_size = purp_logical_output_size_for(mon->physical_size, mon->scale);
    purp_workspace_init(&mon->workspace);
    wm->monitors_dirty = true;
    return (int)idx;
}
size_t purp_wm_primary_monitor_index(const purp_wm_t *wm) {
    for (size_t i = 0; i < wm->monitor_count; ++i) if (wm->monitors[i].enabled && wm->monitors[i].primary) return i;
    for (size_t i = 0; i < wm->monitor_count; ++i) if (wm->monitors[i].enabled) return i;
    return 0;
}
size_t purp_wm_active_monitor_index(const purp_wm_t *wm) {
    return wm->focused_monitor >= 0 && (size_t)wm->focused_monitor < wm->monitor_count &&
           wm->monitors[wm->focused_monitor].enabled ? (size_t)wm->focused_monitor : purp_wm_primary_monitor_index(wm);
}
size_t purp_wm_enabled_monitor_order(const purp_wm_t *wm, size_t *out, size_t cap) {
    size_t sorted[PURP_MAX_MONITORS], n = 0;
    for (size_t i = 0; i < wm->monitor_count; ++i) {
        if (!wm->monitors[i].enabled) continue;
        size_t pos = n++;
        while (pos > 0) {
            const purp_monitor_t *before = &wm->monitors[sorted[pos - 1]], *mon = &wm->monitors[i];
            if (before->loc.x < mon->loc.x || (before->loc.x == mon->loc.x && before->loc.y <= mon->loc.y)) break;
            sorted[pos] = sorted[pos - 1]; --pos;
        }
        sorted[pos] = i;
    }
    if (n > cap) n = cap;
    if (n) memcpy(out, sorted, n * sizeof(*out));
    return n;
}
purp_rect_t purp_wm_monitor_rect(const purp_wm_t *wm, size_t idx) {
    return idx < wm->monitor_count ? purp_monitor_rect(&wm->monitors[idx]) : purp_rect_make(0, 0, 1, 1);
}
double purp_wm_monitor_scale(const purp_wm_t *wm, size_t idx) {
    return idx < wm->monitor_count ? wm->monitors[idx].scale : wm->default_scale;
}
int purp_wm_monitor_at(const purp_wm_t *wm, purp_pointf_t pt) {
    for (size_t i = 0; i < wm->monitor_count; ++i)
        if (wm->monitors[i].enabled && purp_rect_contains_f(purp_monitor_rect(&wm->monitors[i]), pt)) return (int)i;
    return -1;
}
size_t purp_wm_nearest_enabled_monitor(const purp_wm_t *wm, purp_pointf_t pt) {
    size_t best = purp_wm_primary_monitor_index(wm);
    double distance = INFINITY;
    for (size_t i = 0; i < wm->monitor_count; ++i) {
        if (!wm->monitors[i].enabled) continue;
        double d = rect_distance_squared(purp_monitor_rect(&wm->monitors[i]), pt);
        if (d < distance) { best = i; distance = d; }
    }
    return best;
}
purp_rect_t purp_wm_desktop_bounds(const purp_wm_t *wm) {
    purp_rect_t bounds = { 0, 0, 0, 0 };
    for (size_t i = 0; i < wm->monitor_count; ++i)
        if (wm->monitors[i].enabled) bounds = purp_rect_union(bounds, purp_monitor_rect(&wm->monitors[i]));
    if (bounds.w < 1) bounds.w = 1;
    if (bounds.h < 1) bounds.h = 1;
    return bounds;
}
purp_workspace_t *purp_wm_active_workspace(purp_wm_t *wm) {
    return &wm->monitors[purp_wm_active_monitor_index(wm)].workspace;
}
int purp_wm_adjacent_monitor(const purp_wm_t *wm, size_t src, int dx, int dy) {
    if (src >= wm->monitor_count || (!dx && !dy)) return -1;
    purp_rect_t rect = purp_wm_monitor_rect(wm, src);
    int best = -1;
    int32_t best_overlap = 0;
    for (size_t i = 0; i < wm->monitor_count; ++i) {
        if (i == src || !wm->monitors[i].enabled) continue;
        purp_rect_t other = purp_wm_monitor_rect(wm, i);
        int32_t overlap = 0;
        if ((dx > 0 && rect.x + rect.w == other.x) || (dx < 0 && other.x + other.w == rect.x))
            overlap = purp_min_i32(rect.y + rect.h, other.y + other.h) - purp_max_i32(rect.y, other.y);
        else if ((dy > 0 && rect.y + rect.h == other.y) || (dy < 0 && other.y + other.h == rect.y))
            overlap = purp_min_i32(rect.x + rect.w, other.x + other.w) - purp_max_i32(rect.x, other.x);
        if (overlap > best_overlap) { best_overlap = overlap; best = (int)i; }
    }
    return best;
}
purp_pointf_t purp_wm_touch_virtual_to_logical(const purp_wm_t *wm, double phys_x, double phys_y,
                                               purp_size_t host_physical_size) {
    purp_rect_t bounds = purp_wm_desktop_bounds(wm);
    purp_pointf_t point = { bounds.x + phys_x * bounds.w / purp_max_i32(host_physical_size.w, 1),
                           bounds.y + phys_y * bounds.h / purp_max_i32(host_physical_size.h, 1) };
    return purp_clamp_pointer_to_monitors(wm->monitors, wm->monitor_count, point, (purp_pointf_t){ 0, 0 });
}
size_t purp_wm_resolve_touch_monitor(const purp_wm_t *wm, const char *output_name) {
    int idx = purp_wm_find_monitor(wm, output_name);
    return idx >= 0 && wm->monitors[idx].enabled ? (size_t)idx : purp_wm_primary_monitor_index(wm);
}

size_t purp_wm_monitor_infos(const purp_wm_t *wm, purp_monitor_info_t *out, size_t cap) {
    size_t n = wm->monitor_count < cap ? wm->monitor_count : cap;
    for (size_t i = 0; i < n; ++i) {
        const purp_monitor_t *mon = &wm->monitors[i];
        out[i] = (purp_monitor_info_t){ .enabled = mon->enabled, .primary = mon->primary,
            .width = (uint32_t)mon->physical_size.w, .height = (uint32_t)mon->physical_size.h,
            .refresh_mhz = mon->refresh_mhz, .scale = mon->scale, .x = mon->loc.x, .y = mon->loc.y,
            .logical_width = mon->logical_size.w, .logical_height = mon->logical_size.h,
            .mode_count = mon->mode_count };
        (void)snprintf(out[i].name, sizeof(out[i].name), "%s", mon->name);
        (void)snprintf(out[i].description, sizeof(out[i].description), "%s", mon->description);
        memcpy(out[i].modes, mon->modes, mon->mode_count * sizeof(*mon->modes));
    }
    return n;
}
size_t purp_wm_monitor_configs(const purp_wm_t *wm, purp_monitor_config_t *out, size_t cap) {
    size_t n = wm->monitor_count < cap ? wm->monitor_count : cap;
    for (size_t i = 0; i < n; ++i) {
        const purp_monitor_t *mon = &wm->monitors[i];
        out[i] = (purp_monitor_config_t){ .enabled = mon->enabled, .primary = mon->primary,
            .width = (uint32_t)mon->physical_size.w, .height = (uint32_t)mon->physical_size.h,
            .refresh_mhz = mon->refresh_mhz, .scale = mon->scale, .x = mon->loc.x, .y = mon->loc.y };
        (void)snprintf(out[i].name, sizeof(out[i].name), "%s", mon->name);
    }
    return n;
}
static void translate_windows(purp_wm_t *wm, const purp_point_t *old_locs) {
    for (size_t i = 0; i < wm->window_count; ++i) {
        purp_window_t *win = wm->windows[i];
        if (win->monitor < 0 || (size_t)win->monitor >= wm->monitor_count) continue;
        purp_point_t loc = wm->monitors[win->monitor].loc, old = old_locs[win->monitor];
        win->geo = purp_rect_translate(win->geo, loc.x - old.x, loc.y - old.y);
        win->float_geo = purp_rect_translate(win->float_geo, loc.x - old.x, loc.y - old.y);
        if (win->has_saved_geo)
            win->saved_geo = purp_rect_translate(win->saved_geo, loc.x - old.x, loc.y - old.y);
    }
}
static void ensure_primary(purp_wm_t *wm) {
    bool enabled = false, primary = false;
    for (size_t i = 0; i < wm->monitor_count; ++i) if (wm->monitors[i].enabled) enabled = true;
    if (!enabled && wm->monitor_count) wm->monitors[0].enabled = true;
    for (size_t i = 0; i < wm->monitor_count; ++i) {
        purp_monitor_t *mon = &wm->monitors[i];
        if (mon->enabled && mon->primary && !primary) primary = true;
        else mon->primary = false;
    }
    if (!primary && wm->monitor_count) wm->monitors[purp_wm_primary_monitor_index(wm)].primary = true;
}
void purp_wm_normalize_monitors(purp_wm_t *wm) {
    ensure_primary(wm);
    int32_t min_x = INT32_MAX, min_y = INT32_MAX;
    for (size_t i = 0; i < wm->monitor_count; ++i) {
        purp_monitor_t *mon = &wm->monitors[i];
        mon->logical_size = purp_logical_output_size_for(mon->physical_size, mon->scale);
        if (mon->enabled) { min_x = purp_min_i32(min_x, mon->loc.x); min_y = purp_min_i32(min_y, mon->loc.y); }
    }
    if (min_x == INT32_MAX) min_x = min_y = 0;
    for (size_t i = 0; i < wm->monitor_count; ++i) {
        if (!wm->monitors[i].enabled) continue;
        wm->monitors[i].loc.x -= min_x;
        wm->monitors[i].loc.y -= min_y;
    }
    size_t primary = purp_wm_primary_monitor_index(wm);
    for (size_t i = 0; i < wm->window_count; ++i) {
        purp_window_t *win = wm->windows[i];
        win->geo = purp_rect_translate(win->geo, -min_x, -min_y);
        win->float_geo = purp_rect_translate(win->float_geo, -min_x, -min_y);
        if (win->has_saved_geo) win->saved_geo = purp_rect_translate(win->saved_geo, -min_x, -min_y);
        if (wm->monitor_count && (win->monitor < 0 || (size_t)win->monitor >= wm->monitor_count ||
                                 !wm->monitors[win->monitor].enabled)) {
            win->monitor = (int)primary;
            if (!win->is_scratchpad) win->tags = wm->monitors[primary].workspace.selected_tags;
        }
    }
    wm->pointer_location.x -= min_x; wm->pointer_location.y -= min_y;
    wm->focused_monitor = (int)purp_wm_active_monitor_index(wm);
    wm->pointer_location = purp_clamp_pointer_to_monitors(wm->monitors, wm->monitor_count,
        wm->pointer_location, (purp_pointf_t){ 0, 0 });
    purp_wm_clamp_floating_windows(wm);
    purp_wm_ensure_valid_focus(wm);
    wm->monitors_dirty = true;
}
bool purp_wm_apply_monitor_configs(purp_wm_t *wm, const purp_monitor_config_t *entries,
                                   size_t count, bool allow_custom_modes, char *err, size_t err_len) {
    if (count == 0) {
        if (err && err_len) (void)snprintf(err, err_len, "%s", PURP_IPC_ERR_EMPTY_CONFIG);
        return false;
    }
    bool enabled[PURP_MAX_MONITORS] = { false };
    for (size_t i = 0; i < wm->monitor_count; ++i) enabled[i] = wm->monitors[i].enabled;
    for (size_t e = 0; e < count; ++e) {
        int idx = purp_wm_find_monitor(wm, entries[e].name);
        if (idx >= 0) enabled[idx] = entries[e].enabled;
    }
    bool any = false;
    for (size_t i = 0; i < wm->monitor_count; ++i) if (enabled[i]) any = true;
    if (!any) {
        if (err && err_len) (void)snprintf(err, err_len, "%s", PURP_IPC_ERR_ALL_DISABLED);
        return false;
    }
    purp_point_t old_locs[PURP_MAX_MONITORS];
    for (size_t i = 0; i < wm->monitor_count; ++i) old_locs[i] = wm->monitors[i].loc;
    for (size_t e = 0; e < count; ++e) {
        const purp_monitor_config_t *entry = &entries[e];
        int idx = purp_wm_find_monitor(wm, entry->name);
        if (idx < 0) continue;
        purp_monitor_t *mon = &wm->monitors[idx];
        mon->enabled = entry->enabled; mon->primary = entry->primary && entry->enabled;
        mon->scale = purp_clamp_f64(round(entry->scale / PURP_MONITOR_SCALE_ROUNDING) *
                                  PURP_MONITOR_SCALE_ROUNDING, PURP_SCALE_MIN, PURP_SCALE_MAX);
        mon->loc = (purp_point_t){ entry->x, entry->y };
        if (entry->width >= 320 && entry->height >= 240) {
            size_t best = mon->mode_count;
            int64_t best_difference = INT64_MAX;
            for (size_t i = 0; i < mon->mode_count; ++i) {
                if (mon->modes[i].width != entry->width || mon->modes[i].height != entry->height) continue;
                int64_t difference = llabs((long long)mon->modes[i].refresh_mhz - entry->refresh_mhz);
                if (difference < best_difference) { best = i; best_difference = difference; }
            }
            if (best < mon->mode_count) {
                mon->physical_size = (purp_size_t){ (int32_t)mon->modes[best].width, (int32_t)mon->modes[best].height };
                mon->refresh_mhz = mon->modes[best].refresh_mhz;
            } else if (allow_custom_modes) {
                mon->physical_size = (purp_size_t){ (int32_t)entry->width, (int32_t)entry->height };
                mon->refresh_mhz = purp_clamp_i32(entry->refresh_mhz, 24000, 500000);
                if (mon->mode_count < PURP_MAX_MODES)
                    mon->modes[mon->mode_count++] = (purp_display_mode_t){ entry->width, entry->height, mon->refresh_mhz, false };
            }
        }
        mon->logical_size = purp_logical_output_size_for(mon->physical_size, mon->scale);
    }
    translate_windows(wm, old_locs);
    purp_wm_normalize_monitors(wm);
    purp_wm_relayout(wm);
    if (err && err_len) *err = '\0';
    return true;
}
size_t purp_wm_retain_connected_monitors(purp_wm_t *wm, const char *const *live_names,
                                         size_t live_count, int *old_to_new) {
    int map[PURP_MAX_MONITORS];
    purp_rect_t old_rects[PURP_MAX_MONITORS];
    size_t old_count = wm->monitor_count, kept = 0;
    for (size_t i = 0; i < PURP_MAX_MONITORS; ++i) map[i] = -1;
    for (size_t i = 0; i < old_count; ++i) {
        old_rects[i] = purp_monitor_rect(&wm->monitors[i]);
        bool live = false;
        for (size_t j = 0; j < live_count; ++j)
            if (purp_monitor_name_matches(wm->monitors[i].name, live_names[j])) { live = true; break; }
        if (live) {
            map[i] = (int)kept;
            if (kept != i) wm->monitors[kept] = wm->monitors[i];
            ++kept;
        }
    }
    if (old_to_new) memcpy(old_to_new, map, sizeof(map));
    if (kept == old_count) return 0;
    wm->monitor_count = kept;
    ensure_primary(wm);
    size_t primary = purp_wm_primary_monitor_index(wm);
    int old_focus = wm->focused_monitor;
    wm->focused_monitor = old_focus >= 0 && (size_t)old_focus < old_count && map[old_focus] >= 0 ?
                          map[old_focus] : (int)primary;
    for (size_t i = 0; i < wm->window_count; ++i) {
        purp_window_t *win = wm->windows[i];
        int old = win->monitor;
        if (old >= 0 && (size_t)old < old_count && map[old] >= 0) { win->monitor = map[old]; continue; }
        win->monitor = (int)primary;
        if (!kept) continue;
        purp_monitor_t *mon = &wm->monitors[primary];
        if (old >= 0 && (size_t)old < old_count) {
            int32_t dx = mon->loc.x - old_rects[old].x, dy = mon->loc.y - old_rects[old].y;
            win->float_geo = purp_rect_translate(win->float_geo, dx, dy);
            win->geo = purp_rect_translate(win->geo, dx, dy);
            if (win->has_saved_geo) win->saved_geo = purp_rect_translate(win->saved_geo, dx, dy);
        }
        win->float_geo = purp_clamp_floating_geo_in(win->float_geo, purp_monitor_rect(mon), mon->workspace.show_bar);
        if (win->is_floating || win->is_scratchpad) win->geo = win->float_geo;
        if (!win->is_scratchpad) win->tags = mon->workspace.selected_tags;
    }
    wm->monitors_dirty = true;
    wm->pointer_location = purp_clamp_pointer_to_monitors(wm->monitors, kept, wm->pointer_location, (purp_pointf_t){ 0, 0 });
    purp_wm_relayout(wm);
    return old_count - kept;
}
void purp_wm_set_monitor_physical(purp_wm_t *wm, size_t idx, purp_size_t physical, int32_t refresh_mhz) {
    if (idx >= wm->monitor_count) return;
    purp_monitor_t *mon = &wm->monitors[idx];
    mon->physical_size = physical;
    mon->refresh_mhz = refresh_mhz;
    mon->logical_size = purp_logical_output_size_for(physical, mon->scale);
    wm->monitors_dirty = true;
    purp_wm_clamp_floating_windows(wm);
    purp_wm_relayout(wm);
}
void purp_wm_set_monitor_scale(purp_wm_t *wm, size_t idx, double scale) {
    if (idx >= wm->monitor_count) return;
    purp_point_t old_locs[PURP_MAX_MONITORS];
    purp_rect_t old_rects[PURP_MAX_MONITORS];
    bool shifted_x[PURP_MAX_MONITORS] = { false }, shifted_y[PURP_MAX_MONITORS] = { false };
    for (size_t i = 0; i < wm->monitor_count; ++i) {
        old_locs[i] = wm->monitors[i].loc;
        old_rects[i] = purp_monitor_rect(&wm->monitors[i]);
    }
    purp_monitor_t *mon = &wm->monitors[idx];
    mon->scale = purp_clamp_f64(scale, PURP_SCALE_MIN, PURP_SCALE_MAX);
    mon->logical_size = purp_logical_output_size_for(mon->physical_size, mon->scale);
    int32_t dw = mon->logical_size.w - old_rects[idx].w;
    int32_t dh = mon->logical_size.h - old_rects[idx].h;
    shifted_x[idx] = shifted_y[idx] = true;
    bool changed;
    do {
        changed = false;
        for (size_t src = 0; src < wm->monitor_count; ++src) {
            if (!wm->monitors[src].enabled) continue;
            purp_rect_t from = old_rects[src];
            for (size_t dst = 0; dst < wm->monitor_count; ++dst) {
                if (dst == idx || !wm->monitors[dst].enabled) continue;
                purp_rect_t to = old_rects[dst];
                if (dw && shifted_x[src] && !shifted_x[dst] &&
                    llabs((long long)to.x - from.x - from.w) <= 2 &&
                    purp_min_i32(from.y + from.h, to.y + to.h) > purp_max_i32(from.y, to.y)) {
                    shifted_x[dst] = true;
                    wm->monitors[dst].loc.x += dw;
                    changed = true;
                }
                if (dh && shifted_y[src] && !shifted_y[dst] &&
                    llabs((long long)to.y - from.y - from.h) <= 2 &&
                    purp_min_i32(from.x + from.w, to.x + to.w) > purp_max_i32(from.x, to.x)) {
                    shifted_y[dst] = true;
                    wm->monitors[dst].loc.y += dh;
                    changed = true;
                }
            }
        }
    } while (changed);
    translate_windows(wm, old_locs);
    wm->monitors_dirty = true;
    purp_wm_clamp_floating_windows(wm);
    purp_wm_relayout(wm);
}
double purp_wm_adjust_scale(purp_wm_t *wm, double delta) {
    size_t idx = purp_wm_active_monitor_index(wm);
    double next = purp_clamp_f64(round((purp_wm_monitor_scale(wm, idx) + delta) / PURP_SCALE_STEP) *
                                PURP_SCALE_STEP, PURP_SCALE_MIN, PURP_SCALE_MAX);
    purp_wm_set_monitor_scale(wm, idx, next);
    return next;
}

static void admit_pending_scratchpad(purp_wm_t *wm, purp_window_t *win) {
    if (!wm->pending_scratchpad_spawn) return;
    size_t mon = win->monitor >= 0 ? (size_t)win->monitor : purp_wm_active_monitor_index(wm);
    win->is_scratchpad = win->is_floating = true;
    win->is_hidden = false;
    win->float_geo = purp_scratchpad_geo_in(purp_wm_monitor_rect(wm, mon),
                                           mon < wm->monitor_count && wm->monitors[mon].workspace.show_bar);
    win->geo = win->float_geo;
    wm->pending_scratchpad_spawn = false;
}

purp_window_t *purp_wm_window_create(purp_wm_t *wm, purp_toplevel_t *toplevel) {
    if (wm->window_count == wm->window_cap) {
        size_t cap = wm->window_cap ? wm->window_cap * 2 : 16;
        if (cap < wm->window_cap || cap > SIZE_MAX / sizeof(*wm->windows)) return NULL;
        purp_window_t **windows = realloc(wm->windows, cap * sizeof(*windows));
        if (!windows) return NULL;
        wm->windows = windows; wm->window_cap = cap;
    }
    purp_window_t *win = malloc(sizeof(*win));
    if (!win) return NULL;
    size_t mon = purp_wm_active_monitor_index(wm);
    uint16_t tags = wm->monitor_count ? wm->monitors[mon].workspace.selected_tags : 1;
    purp_window_init(win, toplevel, tags, (int)mon, purp_wm_monitor_rect(wm, mon));
    win->id = wm->next_window_id++;
    if (wm->next_window_id == 0) wm->next_window_id = 1;
    if (!toplevel) admit_pending_scratchpad(wm, win);
    else if (wm->monitor_count)
        win->geo = purp_workspace_usable_area_in(&wm->monitors[mon].workspace,
                                                purp_wm_monitor_rect(wm, mon));
    memmove(wm->windows + 1, wm->windows, wm->window_count * sizeof(*wm->windows));
    wm->windows[0] = win; ++wm->window_count;
    if (toplevel) {
        if (wm->focused_idx >= 0) ++wm->focused_idx;
        purp_window_configure(win, false, wm->monitor_count &&
            purp_workspace_layout(&wm->monitors[mon].workspace) != PURP_LAYOUT_FLOATING);
    } else wm->focused_idx = 0;
    purp_wm_relayout(wm);
    return win;
}
int purp_wm_window_index(const purp_wm_t *wm, const purp_window_t *win) {
    for (size_t i = 0; i < wm->window_count; ++i) if (wm->windows[i] == win) return (int)i;
    return -1;
}
int purp_wm_window_index_by_id(const purp_wm_t *wm, uint64_t id) {
    for (size_t i = 0; i < wm->window_count; ++i) if (wm->windows[i]->id == id) return (int)i;
    return -1;
}
purp_window_t *purp_wm_window_by_id(const purp_wm_t *wm, uint64_t id) {
    int idx = purp_wm_window_index_by_id(wm, id);
    return idx >= 0 ? wm->windows[idx] : NULL;
}
purp_window_t *purp_wm_focused_window(const purp_wm_t *wm) {
    return wm->focused_idx >= 0 && (size_t)wm->focused_idx < wm->window_count ? wm->windows[wm->focused_idx] : NULL;
}
static purp_layout_t window_layout(const purp_wm_t *wm, const purp_window_t *win) {
    return win->monitor >= 0 && (size_t)win->monitor < wm->monitor_count ?
        purp_workspace_layout(&wm->monitors[win->monitor].workspace) : PURP_LAYOUT_TILE;
}

static bool window_is_ancestor(const purp_wm_t *wm, const purp_window_t *ancestor,
                               const purp_window_t *win) {
    uint64_t parent_id = win->parent_id;
    for (size_t depth = 0; parent_id && depth < wm->window_count; ++depth) {
        if (parent_id == ancestor->id) return true;
        const purp_window_t *parent = purp_wm_window_by_id(wm, parent_id);
        if (!parent) break;
        parent_id = parent->parent_id;
    }
    return false;
}

static void restore_saved_geometry(purp_wm_t *wm, purp_window_t *win) {
    if (!win->has_saved_geo) return;
    win->float_geo = win->saved_geo;
    if (win->is_floating || win->is_scratchpad || window_layout(wm, win) == PURP_LAYOUT_FLOATING)
        win->geo = win->saved_geo;
    win->has_saved_geo = false;
}

static void set_window_states(purp_wm_t *wm, purp_window_t *win, bool fullscreen, bool maximized) {
    bool was_expanded = win->is_fullscreen || win->is_maximized;
    if (!was_expanded && (fullscreen || maximized)) {
        win->saved_geo = win->is_floating || win->is_scratchpad ? win->float_geo : win->geo;
        win->has_saved_geo = true;
    }
    win->is_fullscreen = fullscreen;
    win->is_maximized = maximized;
    if (was_expanded && !fullscreen && !maximized) restore_saved_geometry(wm, win);
    if (win->monitor >= 0 && (size_t)win->monitor < wm->monitor_count) {
        const purp_monitor_t *mon = &wm->monitors[win->monitor];
        if (fullscreen) win->geo = purp_monitor_rect(mon);
        else if (maximized) win->geo = maximized_area_in(&mon->workspace, purp_monitor_rect(mon));
    }
    purp_window_configure(win, purp_wm_focused_window(wm) == win && purp_wm_is_window_visible(wm, win),
                          window_layout(wm, win) != PURP_LAYOUT_FLOATING);
}

static void reconcile_window_focus(purp_wm_t *wm, purp_window_t *win) {
    if (!purp_wm_is_window_visible(wm, win)) return;
    bool tiled = !win->is_floating && !win->is_scratchpad && !win->is_fullscreen &&
                 !win->is_maximized && window_layout(wm, win) != PURP_LAYOUT_FLOATING;
    for (size_t i = 0; i < wm->window_count; ++i) {
        purp_window_t *other = wm->windows[i];
        if (other == win || other->monitor != win->monitor || !purp_wm_is_window_visible(wm, other) ||
            window_is_ancestor(wm, other, win)) continue;
        if (other->is_fullscreen || (tiled && other->is_maximized))
            set_window_states(wm, other, false, tiled ? false : other->is_maximized);
    }
}

static purp_rect_t center_over_parent(const purp_wm_t *wm, const purp_window_t *parent,
                                      purp_rect_t geo) {
    geo.x = parent->geo.x + (parent->geo.w - geo.w) / 2;
    geo.y = parent->geo.y + (parent->geo.h - geo.h) / 2;
    const purp_monitor_t *mon = &wm->monitors[parent->monitor];
    return purp_clamp_floating_geo_in(geo, purp_monitor_rect(mon), mon->workspace.show_bar);
}

static void reparent_window_children(purp_wm_t *wm, const purp_window_t *win) {
    for (size_t i = 0; i < wm->window_count; ++i)
        if (wm->windows[i]->parent_id == win->id) wm->windows[i]->parent_id = win->parent_id;
}

void purp_wm_set_window_parent(purp_wm_t *wm, size_t idx, uint64_t parent_id) {
    if (idx >= wm->window_count) return;
    purp_window_t *win = wm->windows[idx];
    purp_window_t *parent = parent_id ? purp_wm_window_by_id(wm, parent_id) : NULL;
    if (parent_id == win->id || (parent && window_is_ancestor(wm, win, parent))) return;
    win->parent_id = parent_id;
    if (parent && purp_wm_is_window_visible(wm, parent)) {
        win->monitor = parent->monitor;
        win->tags = parent->tags;
        win->is_floating = true;
        win->size_from_client = true;
        win->float_geo = center_over_parent(wm, parent, win->float_geo);
        if (!win->is_fullscreen && !win->is_maximized) win->geo = win->float_geo;
    }
    purp_window_configure(win, purp_wm_focused_window(wm) == win && purp_wm_is_window_visible(wm, win),
                          window_layout(wm, win) != PURP_LAYOUT_FLOATING);
    purp_wm_relayout(wm);
}

void purp_wm_on_window_mapped(purp_wm_t *wm, size_t idx) {
    if (idx >= wm->window_count) return;
    purp_window_t *win = wm->windows[idx];
    win->mapped = true;
    admit_pending_scratchpad(wm, win);
    (void)purp_wm_apply_window_rules(wm, idx);
    if (win->parent_id) purp_wm_set_window_parent(wm, idx, win->parent_id);
    if (purp_wm_is_window_visible(wm, win) && win->parent_id == 0 &&
        !win->is_floating && !win->is_scratchpad) {
        for (size_t i = 0; i < wm->window_count; ++i) {
            purp_window_t *other = wm->windows[i];
            if (other != win && other->monitor == win->monitor && purp_wm_is_window_visible(wm, other) &&
                (other->is_fullscreen || other->is_maximized))
                set_window_states(wm, other, false, false);
        }
    }
    purp_wm_focus_window(wm, (int)idx);
    purp_wm_relayout(wm);
}

void purp_wm_on_window_unmapped(purp_wm_t *wm, size_t idx) {
    if (idx >= wm->window_count) return;
    purp_window_t *win = wm->windows[idx];
    win->mapped = false;
    win->is_resizing = false;
    win->rules_applied = false;
    set_window_states(wm, win, false, false);
    reparent_window_children(wm, win);
    purp_wm_relayout(wm);
}

void purp_wm_window_destroy(purp_wm_t *wm, purp_window_t *win) {
    int idx = purp_wm_window_index(wm, win);
    if (idx < 0) return;
    reparent_window_children(wm, win);
    if (wm->focused_idx == idx) { wm->focused_monitor = win->monitor; wm->focused_idx = -1; }
    else if (wm->focused_idx > idx) --wm->focused_idx;
    memmove(wm->windows + idx, wm->windows + idx + 1, (wm->window_count - (size_t)idx - 1) * sizeof(*wm->windows));
    --wm->window_count;
    purp_window_finish(win);
    free(win);
    purp_wm_relayout(wm);
}
bool purp_wm_is_window_visible(const purp_wm_t *wm, const purp_window_t *win) {
    return win->monitor >= 0 && (size_t)win->monitor < wm->monitor_count && wm->monitors[win->monitor].enabled &&
           purp_window_is_visible_on(win, wm->monitors[win->monitor].workspace.selected_tags);
}
void purp_wm_ensure_valid_focus(purp_wm_t *wm) {
    size_t active = purp_wm_active_monitor_index(wm);
    wm->focused_monitor = (int)active;
    purp_window_t *focused = purp_wm_focused_window(wm);
    if (focused && focused->monitor == (int)active && purp_wm_is_window_visible(wm, focused)) return;
    wm->focused_idx = -1;
    for (size_t i = 0; i < wm->window_count; ++i)
        if (wm->windows[i]->monitor == (int)active && purp_wm_is_window_visible(wm, wm->windows[i])) {
            wm->focused_idx = (int)i; break;
        }
}
void purp_wm_focus_window(purp_wm_t *wm, int idx) {
    if (idx < 0 || (size_t)idx >= wm->window_count) {
        wm->focused_idx = -1;
        for (size_t i = 0; i < wm->window_count; ++i)
            purp_window_configure(wm->windows[i], false,
                wm->windows[i]->monitor >= 0 && (size_t)wm->windows[i]->monitor < wm->monitor_count &&
                purp_workspace_layout(&wm->monitors[wm->windows[i]->monitor].workspace) != PURP_LAYOUT_FLOATING);
        return;
    }
    if (!purp_wm_is_window_visible(wm, wm->windows[idx])) return;
    reconcile_window_focus(wm, wm->windows[idx]);
    wm->focused_idx = idx;
    wm->focused_monitor = wm->windows[idx]->monitor;
    wm->windows[idx]->is_urgent = false;
    purp_wm_relayout(wm);
}

bool purp_wm_apply_window_rules(purp_wm_t *wm, size_t idx) {
    if (idx >= wm->window_count) return false;
    purp_window_t *win = wm->windows[idx];
    if (win->rules_applied || (!*nonempty(win->app_id) && !*nonempty(win->title))) return false;
    bool changed = replace_string(&win->rule_seen_app_id, win->app_id);
    changed = replace_string(&win->rule_seen_title, win->title) || changed;
    bool matched = false;
    for (size_t i = 0; i < PURP_WINDOW_RULE_COUNT; ++i) {
        const purp_window_rule_t *rule = &PURP_WINDOW_RULES[i];
        if (!purp_window_rule_matches(rule, win->app_id, win->title)) continue;
        matched = true;
        if (rule->tags) win->tags = rule->tags & PURP_TAGMASK;
        if (rule->is_floating) {
            purp_rect_t rect = purp_wm_monitor_rect(wm, (size_t)win->monitor);
            win->is_floating = true;
            win->size_from_client = true;
            win->float_geo.w = purp_clamp_i32(purp_min_i32(920, rect.w - 48), 1, purp_max_i32(rect.w, 1));
            win->float_geo.h = purp_clamp_i32(purp_min_i32(680, rect.h - 64), 1, purp_max_i32(rect.h, 1));
            purp_point_t loc = purp_wm_cascade_loc_on(wm, (size_t)win->monitor, purp_rect_size(win->float_geo), (int)idx);
            win->float_geo.x = loc.x; win->float_geo.y = loc.y;
            win->geo = win->float_geo;
        }
        if (rule->has_opacity) { win->has_opacity_override = true; win->opacity_override = rule->opacity; }
        changed = true;
        break;
    }
    if (purp_window_rules_settled(matched, win->app_id, win->title)) {
        win->rules_applied = true;
        changed = true;
    }
    return changed;
}
void purp_wm_relayout(purp_wm_t *wm) {
    for (size_t i = 0; i < wm->window_count; ++i) {
        purp_window_t *win = wm->windows[i];
        if (!win->toplevel || win->mapped) (void)purp_wm_apply_window_rules(wm, i);
        if (win->toplevel && !win->mapped)
            purp_window_configure(win, false, window_layout(wm, win) != PURP_LAYOUT_FLOATING);
    }
    purp_wm_ensure_valid_focus(wm);
    purp_window_t *focused = purp_wm_focused_window(wm);
    if (focused) focused->is_urgent = false;
    for (size_t i = 0; i < wm->monitor_count; ++i) {
        if (!wm->monitors[i].enabled) continue;
        purp_workspace_arrange_in(&wm->monitors[i].workspace, wm->windows, wm->window_count,
                                  wm->focused_idx, purp_monitor_rect(&wm->monitors[i]), (int)i);
    }
    for (size_t i = 0; i < wm->window_count; ++i) {
        if (!purp_wm_is_window_visible(wm, wm->windows[i]) && wm->windows[i]->want.activated) {
            wm->windows[i]->want.activated = false; wm->windows[i]->configure_dirty = true;
        }
    }
}
static int stacking_rank(const purp_wm_t *wm, size_t i) {
    const purp_window_t *win = wm->windows[i];
    bool focused = wm->focused_idx >= 0 && (size_t)wm->focused_idx == i;
    int tier = win->is_fullscreen ? 0 : win->is_maximized ? 1 :
               win->is_floating || win->is_scratchpad ||
               window_layout(wm, win) == PURP_LAYOUT_FLOATING ? 2 : 3;
    return tier * 2 + (focused ? 0 : 1);
}
size_t purp_wm_stacking_order(const purp_wm_t *wm, size_t *out, size_t cap) {
    if (!cap || !wm->window_count) return 0;
    size_t order[wm->window_count], children[wm->window_count], parents[wm->window_count];
    int ranks[wm->window_count];
    size_t visible_count = 0;
    for (size_t i = 0; i < wm->window_count; ++i) {
        bool visible = purp_wm_is_window_visible(wm, wm->windows[i]);
        ranks[i] = visible ? stacking_rank(wm, i) : -1;
        children[i] = visible ? 0 : SIZE_MAX;
        parents[i] = SIZE_MAX;
    }
    for (int rank = 0; rank < 8; ++rank)
        for (size_t i = 0; i < wm->window_count; ++i)
            if (ranks[i] == rank) order[visible_count++] = i;
    for (size_t i = 0; i < visible_count; ++i) {
        size_t child = order[i];
        uint64_t parent_id = wm->windows[child]->parent_id;
        for (size_t depth = 0; parent_id && depth < wm->window_count; ++depth) {
            int parent = purp_wm_window_index_by_id(wm, parent_id);
            if (parent < 0 || (size_t)parent == child) break;
            if (children[parent] != SIZE_MAX) {
                parents[child] = (size_t)parent;
                ++children[parent];
                break;
            }
            parent_id = wm->windows[parent]->parent_id;
        }
    }
    size_t n = 0;
    while (n < visible_count && n < cap) {
        size_t next = SIZE_MAX;
        for (size_t i = 0; i < visible_count; ++i)
            if (children[order[i]] == 0) { next = order[i]; break; }
        if (next == SIZE_MAX) break; /* Parent cycles are rejected by set_window_parent(). */
        out[n++] = next;
        children[next] = SIZE_MAX;
        if (parents[next] != SIZE_MAX) --children[parents[next]];
    }
    return n;
}
int purp_wm_window_at(const purp_wm_t *wm, purp_pointf_t pt) {
    size_t order[wm->window_count ? wm->window_count : 1];
    size_t n = purp_wm_stacking_order(wm, order, wm->window_count);
    for (size_t i = 0; i < n; ++i)
        if (purp_rect_contains_f(wm->windows[order[i]]->geo, pt)) return (int)order[i];
    return -1;
}
static size_t wrap_position(size_t position, int delta, size_t count) {
    int64_t value = ((int64_t)position + delta) % (int64_t)count;
    return (size_t)(value < 0 ? value + (int64_t)count : value);
}
void purp_wm_cycle_focus(purp_wm_t *wm, int delta) {
    size_t indices[wm->window_count ? wm->window_count : 1], n = 0, position = SIZE_MAX;
    size_t mon = purp_wm_active_monitor_index(wm);
    for (size_t i = 0; i < wm->window_count; ++i) {
        if (wm->windows[i]->monitor != (int)mon || !purp_wm_is_window_visible(wm, wm->windows[i])) continue;
        if (wm->focused_idx >= 0 && i == (size_t)wm->focused_idx) position = n;
        indices[n++] = i;
    }
    int target = n ? (int)indices[position == SIZE_MAX ? (delta < 0 ? n - 1 : 0) :
                                  wrap_position(position, delta, n)] : -1;
    purp_wm_focus_window(wm, target);
}
bool purp_wm_focus_monitor_at_point(purp_wm_t *wm, purp_pointf_t pt) {
    int mon = purp_wm_monitor_at(wm, pt);
    if (mon < 0 || mon == wm->focused_monitor) return false;
    wm->focused_monitor = mon;
    purp_wm_relayout(wm);
    return true;
}
static size_t cycled_monitor(const purp_wm_t *wm, int delta) {
    size_t order[PURP_MAX_MONITORS], n = purp_wm_enabled_monitor_order(wm, order, PURP_MAX_MONITORS);
    size_t active = purp_wm_active_monitor_index(wm);
    if (!n) return active;
    size_t position = 0;
    for (size_t i = 0; i < n; ++i) if (order[i] == active) { position = i; break; }
    return order[wrap_position(position, delta, n)];
}
void purp_wm_cycle_monitor(purp_wm_t *wm, int delta) {
    size_t mon = cycled_monitor(wm, delta);
    wm->focused_monitor = (int)mon;
    wm->pointer_location = purp_rect_center(purp_wm_monitor_rect(wm, mon));
    purp_wm_relayout(wm);
}
void purp_wm_move_focused_window_to_monitor(purp_wm_t *wm, int delta) {
    if (wm->focused_idx < 0) return;
    purp_wm_send_window_to_monitor(wm, (size_t)wm->focused_idx, cycled_monitor(wm, delta), true);
}
void purp_wm_send_window_to_monitor(purp_wm_t *wm, size_t win_idx, size_t target, bool follow) {
    if (win_idx >= wm->window_count || target >= wm->monitor_count || !wm->monitors[target].enabled) return;
    purp_window_t *win = wm->windows[win_idx];
    purp_rect_t old = purp_wm_monitor_rect(wm, (size_t)win->monitor), rect = purp_wm_monitor_rect(wm, target);
    purp_rect_t mapped = win->float_geo;
    mapped.x = rect.x + purp_round_i32((double)(mapped.x - old.x) / purp_max_i32(old.w, 1) * rect.w);
    mapped.y = rect.y + purp_round_i32((double)(mapped.y - old.y) / purp_max_i32(old.h, 1) * rect.h);
    win->float_geo = purp_clamp_floating_geo_in(mapped, rect, wm->monitors[target].workspace.show_bar);
    if (win->has_saved_geo) {
        purp_rect_t saved = win->saved_geo;
        saved.x = rect.x + purp_round_i32((double)(saved.x - old.x) / purp_max_i32(old.w, 1) * rect.w);
        saved.y = rect.y + purp_round_i32((double)(saved.y - old.y) / purp_max_i32(old.h, 1) * rect.h);
        win->saved_geo = purp_clamp_floating_geo_in(saved, rect, wm->monitors[target].workspace.show_bar);
    }
    if (win->is_floating || win->is_scratchpad ||
        purp_workspace_layout(&wm->monitors[target].workspace) == PURP_LAYOUT_FLOATING)
        win->geo = win->float_geo;
    win->monitor = (int)target;
    if (!win->is_scratchpad) win->tags = wm->monitors[target].workspace.selected_tags;
    if (follow) {
        wm->focused_monitor = (int)target;
        wm->focused_idx = (int)win_idx;
        wm->pointer_location = purp_rect_center(rect);
        reconcile_window_focus(wm, win);
    }
    purp_wm_relayout(wm);
}
bool purp_wm_floating_grab_geometry(purp_wm_t *wm, size_t idx, purp_rect_t *out) {
    if (idx >= wm->window_count || wm->windows[idx]->is_fullscreen) return false;
    purp_window_t *win = wm->windows[idx];
    bool floating_layout = win->monitor >= 0 && (size_t)win->monitor < wm->monitor_count &&
                           purp_workspace_layout(&wm->monitors[win->monitor].workspace) == PURP_LAYOUT_FLOATING;
    if (win->is_maximized) {
        win->is_maximized = false;
        restore_saved_geometry(wm, win);
        win->is_floating = true;
    } else if (!floating_layout && !win->is_floating && !win->is_scratchpad) {
        win->is_floating = true; win->float_geo = win->geo;
    }
    if (out) *out = win->float_geo;
    return true;
}
void purp_wm_move_focused_window(purp_wm_t *wm, int dx, int dy) {
    purp_window_t *win = purp_wm_focused_window(wm);
    if (!win || win->is_fullscreen || (!dx && !dy) || win->monitor < 0 ||
        (size_t)win->monitor >= wm->monitor_count) return;
    size_t mon = (size_t)win->monitor;
    purp_workspace_t *ws = &wm->monitors[mon].workspace;
    if (win->is_floating || win->is_scratchpad || purp_workspace_layout(ws) == PURP_LAYOUT_FLOATING) {
        purp_point_t loc = { win->float_geo.x + dx * PURP_MOVE_STEP, win->float_geo.y + dy * PURP_MOVE_STEP };
        loc = purp_clamp_floating_loc_across_monitors(loc, purp_rect_size(win->float_geo), wm->monitors, wm->monitor_count);
        win->float_geo.x = loc.x; win->float_geo.y = loc.y; win->geo = win->float_geo;
        size_t target = purp_wm_nearest_enabled_monitor(wm, purp_rect_center(win->float_geo));
        if (target != mon) {
            win->monitor = (int)target; wm->focused_monitor = (int)target;
            if (!win->is_scratchpad) win->tags = wm->monitors[target].workspace.selected_tags;
        }
        purp_wm_relayout(wm);
        return;
    }
    size_t indices[wm->window_count ? wm->window_count : 1];
    size_t n = purp_workspace_tiled_indices(ws, wm->windows, wm->window_count, (int)mon, indices, wm->window_count);
    size_t position = n;
    for (size_t i = 0; i < n; ++i) if (indices[i] == (size_t)wm->focused_idx) { position = i; break; }
    if (position == n) return;
    size_t target = SIZE_MAX;
    double best_distance = INFINITY;
    purp_pointf_t center = purp_rect_center(win->geo);
    for (size_t i = 0; i < n; ++i) {
        if (i == position) continue;
        purp_rect_t other = wm->windows[indices[i]]->geo;
        purp_pointf_t other_center = purp_rect_center(other);
        int32_t overlap = dx ?
            purp_min_i32(win->geo.y + win->geo.h, other.y + other.h) - purp_max_i32(win->geo.y, other.y) :
            purp_min_i32(win->geo.x + win->geo.w, other.x + other.w) - purp_max_i32(win->geo.x, other.x);
        double along = dx ? (other_center.x - center.x) * dx : (other_center.y - center.y) * dy;
        if (overlap <= 0 || along <= 0) continue;
        double x = other_center.x - center.x, y = other_center.y - center.y;
        double distance = x * x + y * y;
        if (distance < best_distance) { target = indices[i]; best_distance = distance; }
    }
    if (target == SIZE_MAX) {
        int adjacent = purp_wm_adjacent_monitor(wm, mon, dx, dy);
        if (adjacent >= 0) {
            purp_wm_send_window_to_monitor(wm, (size_t)wm->focused_idx, (size_t)adjacent, true);
            return;
        }
        return;
    }
    wm->windows[wm->focused_idx] = wm->windows[target];
    wm->windows[target] = win; wm->focused_idx = (int)target;
    purp_wm_relayout(wm);
}
bool purp_wm_tiled_resize_plan(const purp_wm_t *wm, size_t idx, uint32_t edges, purp_tiled_resize_t *out) {
    *out = (purp_tiled_resize_t){ .monitor = -1 };
    if (idx >= wm->window_count) return false;
    int mon = wm->windows[idx]->monitor;
    if (mon < 0 || (size_t)mon >= wm->monitor_count) return false;
    out->monitor = mon;
    const purp_workspace_t *ws = &wm->monitors[mon].workspace;
    size_t indices[wm->window_count ? wm->window_count : 1];
    size_t n = purp_workspace_tiled_indices(ws, wm->windows, wm->window_count, mon, indices, wm->window_count);
    bool found = false;
    purp_tiled_entry_t tiled[n ? n : 1];
    for (size_t i = 0; i < n; ++i) {
        purp_window_t *win = wm->windows[indices[i]];
        tiled[i] = (purp_tiled_entry_t){ .key = win->id, .geo = win->geo, .cfact = win->cfact };
        if (indices[i] == idx) found = true;
    }
    if (!found) return false;
    *out = purp_workspace_plan_tiled_resize_in(ws, purp_wm_monitor_rect(wm, (size_t)mon),
                                               tiled, n, wm->windows[idx]->id, edges, mon);
    return !purp_tiled_resize_is_empty(out);
}
void purp_wm_apply_tiled_resize(purp_wm_t *wm, const purp_tiled_resize_t *plan, int32_t dx, int32_t dy) {
    if (plan->monitor < 0 || (size_t)plan->monitor >= wm->monitor_count) return;
    if (plan->has_mfact)
        purp_workspace_set_mfact(&wm->monitors[plan->monitor].workspace, plan->mfact.start + plan->mfact.per_px * (float)dx);
    if (plan->has_rows) {
        float upper, lower;
        purp_row_split_cfacts(&plan->rows, dy, &upper, &lower);
        purp_window_t *upper_win = purp_wm_window_by_id(wm, plan->rows.upper);
        purp_window_t *lower_win = purp_wm_window_by_id(wm, plan->rows.lower);
        if (upper_win && upper_win->monitor == plan->monitor) upper_win->cfact = upper;
        if (lower_win && lower_win->monitor == plan->monitor) lower_win->cfact = lower;
    }
    purp_wm_relayout(wm);
}
void purp_wm_resize_window_by(purp_wm_t *wm, size_t idx, int32_t dx, int32_t dy) {
    if (idx >= wm->window_count || wm->windows[idx]->is_fullscreen) return;
    purp_tiled_resize_t plan;
    if (purp_wm_tiled_resize_plan(wm, idx, PURP_EDGE_BOTTOM_RIGHT, &plan)) {
        purp_wm_apply_tiled_resize(wm, &plan, dx, dy);
        return;
    }
    purp_rect_t geo;
    if (!purp_wm_floating_grab_geometry(wm, idx, &geo)) return;
    purp_window_t *win = wm->windows[idx];
    win->size_from_client = false;
    purp_size_t size = purp_window_clamp_frame_to_client_limits(win,
        (purp_size_t){ purp_max_i32(geo.w + dx, PURP_MIN_DRAG_WIDTH), purp_max_i32(geo.h + dy, PURP_MIN_DRAG_HEIGHT) });
    win->float_geo.w = size.w; win->float_geo.h = size.h; win->geo = win->float_geo;
    purp_wm_relayout(wm);
}
void purp_wm_zoom_focused(purp_wm_t *wm) {
    purp_window_t *win = purp_wm_focused_window(wm);
    if (!win || win->monitor < 0 || (size_t)win->monitor >= wm->monitor_count) return;
    size_t indices[wm->window_count ? wm->window_count : 1];
    size_t n = purp_workspace_tiled_indices(&wm->monitors[win->monitor].workspace, wm->windows,
        wm->window_count, win->monitor, indices, wm->window_count);
    if (n < 2) return;
    bool found = false;
    for (size_t i = 0; i < n; ++i) if (indices[i] == (size_t)wm->focused_idx) found = true;
    if (!found) return;
    size_t master = indices[0], focused = (size_t)wm->focused_idx;
    size_t target = focused == master ? indices[1] : master;
    wm->windows[focused] = wm->windows[target]; wm->windows[target] = win;
    wm->focused_idx = (int)purp_focus_after_zoom(master, focused, target);
    purp_wm_relayout(wm);
}

void purp_wm_view_tags_on(purp_wm_t *wm, size_t mon, uint16_t mask) {
    mask &= PURP_TAGMASK;
    if (mon >= wm->monitor_count || !mask) return;
    purp_workspace_t *ws = &wm->monitors[mon].workspace;
    /* Bar actions select their monitor even when the workspace itself does not change. */
    wm->focused_monitor = (int)mon;
    if (mask == ws->selected_tags) {
        purp_wm_ensure_valid_focus(wm);
        purp_wm_relayout(wm);
        return;
    }
    ws->prev_tags = ws->selected_tags; ws->selected_tags = mask;
    purp_workspace_sync_cur_tag(ws);
    purp_wm_relayout(wm);
}
void purp_wm_view_tags(purp_wm_t *wm, uint16_t mask) {
    purp_wm_view_tags_on(wm, purp_wm_active_monitor_index(wm), mask);
}
void purp_wm_toggle_view_tags_on(purp_wm_t *wm, size_t mon, uint16_t mask) {
    if (mon >= wm->monitor_count) return;
    purp_workspace_t *ws = &wm->monitors[mon].workspace;
    uint16_t next = ws->selected_tags ^ (mask & PURP_TAGMASK);
    wm->focused_monitor = (int)mon;
    if (!next) {
        purp_wm_ensure_valid_focus(wm);
        purp_wm_relayout(wm);
        return;
    }
    ws->prev_tags = ws->selected_tags; ws->selected_tags = next;
    purp_workspace_sync_cur_tag(ws);
    purp_wm_relayout(wm);
}
void purp_wm_toggle_view_tags(purp_wm_t *wm, uint16_t mask) {
    purp_wm_toggle_view_tags_on(wm, purp_wm_active_monitor_index(wm), mask);
}
void purp_wm_swap_prev_tags(purp_wm_t *wm) {
    if (!wm->monitor_count) return;
    purp_workspace_t *ws = purp_wm_active_workspace(wm);
    uint16_t previous = ws->prev_tags;
    ws->prev_tags = ws->selected_tags; ws->selected_tags = previous;
    purp_workspace_sync_cur_tag(ws);
    purp_wm_relayout(wm);
}
void purp_wm_tag_focused_window(purp_wm_t *wm, uint16_t mask) {
    purp_window_t *win = purp_wm_focused_window(wm);
    mask &= PURP_TAGMASK;
    if (!win || !mask) return;
    win->tags = mask; purp_wm_relayout(wm);
}
void purp_wm_toggle_tag_focused_window(purp_wm_t *wm, uint16_t mask) {
    purp_window_t *win = purp_wm_focused_window(wm);
    if (!win) return;
    uint16_t next = win->tags ^ (mask & PURP_TAGMASK);
    if (next) { win->tags = next; purp_wm_relayout(wm); }
}
void purp_wm_set_layout_on(purp_wm_t *wm, size_t mon, purp_layout_t layout) {
    if (mon >= wm->monitor_count) return;
    wm->focused_monitor = (int)mon;
    purp_workspace_set_layout(&wm->monitors[mon].workspace, layout);
    purp_wm_relayout(wm);
}
void purp_wm_set_layout(purp_wm_t *wm, purp_layout_t layout) {
    purp_wm_set_layout_on(wm, purp_wm_active_monitor_index(wm), layout);
}
void purp_wm_cycle_layout(purp_wm_t *wm) {
    if (wm->monitor_count) purp_wm_set_layout(wm, purp_layout_next(purp_workspace_layout(purp_wm_active_workspace(wm))));
}
void purp_wm_toggle_last_layout(purp_wm_t *wm) {
    if (wm->monitor_count) purp_wm_set_layout(wm, purp_workspace_prev_layout(purp_wm_active_workspace(wm)));
}
void purp_wm_adjust_mfact(purp_wm_t *wm, float delta) {
    if (!wm->monitor_count) return;
    purp_workspace_adjust_mfact(purp_wm_active_workspace(wm), delta); purp_wm_relayout(wm);
}
void purp_wm_adjust_nmaster(purp_wm_t *wm, int delta) {
    if (!wm->monitor_count) return;
    purp_workspace_adjust_nmaster(purp_wm_active_workspace(wm), delta); purp_wm_relayout(wm);
}
void purp_wm_adjust_gaps(purp_wm_t *wm, int32_t delta) {
    if (!wm->monitor_count) return;
    purp_workspace_adjust_gaps(purp_wm_active_workspace(wm), delta); purp_wm_relayout(wm);
}
void purp_wm_reset_gaps(purp_wm_t *wm) {
    if (!wm->monitor_count) return;
    purp_workspace_reset_gaps(purp_wm_active_workspace(wm)); purp_wm_relayout(wm);
}
void purp_wm_toggle_gaps(purp_wm_t *wm) {
    if (!wm->monitor_count) return;
    purp_workspace_toggle_gaps(purp_wm_active_workspace(wm)); purp_wm_relayout(wm);
}
void purp_wm_toggle_bar(purp_wm_t *wm) {
    if (!wm->monitor_count) return;
    purp_workspace_t *ws = purp_wm_active_workspace(wm);
    ws->show_bar = !ws->show_bar;
    purp_wm_relayout(wm);
}

static bool cascade_taken(const purp_wm_t *wm, size_t mon, purp_point_t loc, int skip_idx) {
    for (size_t i = 0; i < wm->window_count; ++i) {
        const purp_window_t *win = wm->windows[i];
        if ((skip_idx >= 0 && i == (size_t)skip_idx) || win->monitor != (int)mon ||
            !(win->is_floating || win->is_scratchpad) || !purp_wm_is_window_visible(wm, win)) continue;
        if (llabs((long long)win->float_geo.x - loc.x) < PURP_CASCADE_STEP / 2 &&
            llabs((long long)win->float_geo.y - loc.y) < PURP_CASCADE_STEP / 2) return true;
    }
    return false;
}
static purp_point_t clamp_cascade(purp_point_t loc, purp_size_t size, purp_rect_t rect, int32_t top) {
    return (purp_point_t){ purp_clamp_i32(loc.x, rect.x, purp_max_i32(rect.x + rect.w - size.w, rect.x)),
        purp_clamp_i32(loc.y, top, purp_max_i32(rect.y + rect.h - size.h, top)) };
}
purp_point_t purp_wm_cascade_loc_on(const purp_wm_t *wm, size_t mon, purp_size_t size, int skip_idx) {
    purp_rect_t rect = purp_wm_monitor_rect(wm, mon);
    int32_t bar = mon < wm->monitor_count && wm->monitors[mon].workspace.show_bar ? PURP_BAR_HEIGHT : 0;
    int32_t top = rect.y + bar;
    purp_point_t start = { rect.x + purp_max_i32((rect.w - size.w) / 2, 0),
                          top + purp_max_i32((rect.h - bar - size.h) / 2, 0) };
    for (int32_t step = 0; step < 12; ++step) {
        purp_point_t candidate = { start.x + step * PURP_CASCADE_STEP, start.y + step * PURP_CASCADE_STEP };
        if (candidate.x + size.w > rect.x + rect.w || candidate.y + size.h > rect.y + rect.h)
            candidate = (purp_point_t){ rect.x + step * PURP_CASCADE_STEP, top + step * PURP_CASCADE_STEP };
        candidate = clamp_cascade(candidate, size, rect, top);
        if (!cascade_taken(wm, mon, candidate, skip_idx)) return candidate;
    }
    for (int32_t y = top; y < rect.y + rect.h; y += PURP_CASCADE_STEP)
        for (int32_t x = rect.x; x < rect.x + rect.w; x += PURP_CASCADE_STEP) {
            purp_point_t candidate = clamp_cascade((purp_point_t){ x, y }, size, rect, top);
            if (!cascade_taken(wm, mon, candidate, skip_idx)) return candidate;
        }
    return clamp_cascade(start, size, rect, top);
}
void purp_wm_clamp_floating_windows(purp_wm_t *wm) {
    for (size_t i = 0; i < wm->window_count; ++i) {
        purp_window_t *win = wm->windows[i];
        if (win->monitor < 0 || (size_t)win->monitor >= wm->monitor_count ||
            win->is_fullscreen || win->is_maximized) continue;
        const purp_monitor_t *mon = &wm->monitors[win->monitor];
        if (!win->is_floating && !win->is_scratchpad && purp_workspace_layout(&mon->workspace) != PURP_LAYOUT_FLOATING) continue;
        win->float_geo = purp_clamp_floating_geo_in(win->float_geo, purp_monitor_rect(mon), mon->workspace.show_bar);
        win->geo = win->float_geo;
    }
}
bool purp_wm_adopt_client_size(purp_wm_t *wm, size_t idx) {
    if (idx >= wm->window_count) return false;
    purp_window_t *win = wm->windows[idx];
    purp_size_t client;
    if (!win->size_from_client || !(win->is_floating || win->is_scratchpad) ||
        win->is_fullscreen || win->is_maximized || win->monitor < 0 ||
        (size_t)win->monitor >= wm->monitor_count || !purp_window_client_geometry_size(win, &client)) return false;
    const purp_monitor_t *mon = &wm->monitors[win->monitor];
    int32_t top = mon->workspace.show_bar ? PURP_BAR_HEIGHT : 0;
    purp_size_t frame = { client.w + 2 * PURP_BORDER_WIDTH, client.h + 2 * PURP_BORDER_WIDTH };
    purp_size_t clamped = {
        purp_clamp_i32(frame.w, PURP_MIN_CLIENT_SIZE, purp_max_i32(mon->logical_size.w - 32, PURP_MIN_CLIENT_SIZE)),
        purp_clamp_i32(frame.h, PURP_MIN_CLIENT_SIZE, purp_max_i32(mon->logical_size.h - top - 32, PURP_MIN_CLIENT_SIZE)) };
    bool first = !win->client_sized_once;
    if (!first && win->float_geo.w == clamped.w && win->float_geo.h == clamped.h) return false;
    purp_point_t loc = first ? purp_wm_cascade_loc_on(wm, (size_t)win->monitor, clamped, (int)idx) : purp_rect_loc(win->float_geo);
    win->client_sized_once = true;
    if (clamped.w != frame.w || clamped.h != frame.h) win->size_from_client = false;
    win->float_geo = purp_rect_make(loc.x, loc.y, clamped.w, clamped.h);
    purp_window_t *parent = first && win->parent_id ? purp_wm_window_by_id(wm, win->parent_id) : NULL;
    if (parent && purp_wm_is_window_visible(wm, parent) && parent->monitor == win->monitor)
        win->float_geo = center_over_parent(wm, parent, win->float_geo);
    win->geo = win->float_geo;
    purp_wm_relayout(wm);
    return true;
}
void purp_wm_toggle_floating_focused(purp_wm_t *wm) {
    purp_window_t *win = purp_wm_focused_window(wm);
    if (!win || win->is_fullscreen) return;
    win->is_floating = !win->is_floating;
    if (win->is_floating) win->float_geo = win->geo;
    purp_wm_relayout(wm);
}
void purp_wm_toggle_fullscreen_focused(purp_wm_t *wm) {
    purp_window_t *win = purp_wm_focused_window(wm);
    if (win) purp_wm_set_window_fullscreen(wm, (size_t)wm->focused_idx, !win->is_fullscreen, -1);
}
void purp_wm_set_window_fullscreen(purp_wm_t *wm, size_t idx, bool fullscreen, int monitor) {
    if (idx >= wm->window_count) return;
    if (monitor >= 0 && (size_t)monitor < wm->monitor_count && wm->monitors[monitor].enabled &&
        wm->windows[idx]->monitor != monitor)
        purp_wm_send_window_to_monitor(wm, idx, (size_t)monitor, wm->focused_idx >= 0 && idx == (size_t)wm->focused_idx);
    purp_window_t *win = wm->windows[idx];
    set_window_states(wm, win, fullscreen, win->is_maximized);
    if (fullscreen && purp_wm_is_window_visible(wm, win)) {
        for (size_t i = 0; i < wm->window_count; ++i) {
            purp_window_t *other = wm->windows[i];
            if (other != win && other->monitor == win->monitor && other->is_fullscreen &&
                purp_wm_is_window_visible(wm, other))
                set_window_states(wm, other, false, other->is_maximized);
        }
        wm->focused_idx = (int)idx;
        wm->focused_monitor = win->monitor;
    }
    purp_wm_relayout(wm);
}
void purp_wm_set_window_maximized(purp_wm_t *wm, size_t idx, bool maximized) {
    if (idx >= wm->window_count) return;
    purp_window_t *win = wm->windows[idx];
    set_window_states(wm, win, maximized ? false : win->is_fullscreen, maximized);
    if (maximized && purp_wm_is_window_visible(wm, win)) {
        for (size_t i = 0; i < wm->window_count; ++i) {
            purp_window_t *other = wm->windows[i];
            if (other != win && other->monitor == win->monitor && purp_wm_is_window_visible(wm, other) &&
                (other->is_fullscreen || other->is_maximized))
                set_window_states(wm, other, false, false);
        }
        wm->focused_idx = (int)idx;
        wm->focused_monitor = win->monitor;
    }
    purp_wm_relayout(wm);
}
void purp_wm_toggle_opacity_focused(purp_wm_t *wm) {
    purp_window_t *win = purp_wm_focused_window(wm);
    if (!win) return;
    win->has_opacity_override = !win->has_opacity_override;
    if (win->has_opacity_override) win->opacity_override = 1.0f;
}
purp_scratchpad_result_t purp_wm_toggle_scratchpad(purp_wm_t *wm) {
    size_t mon = purp_wm_active_monitor_index(wm);
    for (size_t i = 0; i < wm->window_count; ++i) {
        purp_window_t *win = wm->windows[i];
        if (!win->is_scratchpad) continue;
        bool show = win->is_hidden || win->monitor != (int)mon;
        win->is_hidden = !show;
        if (show) {
            win->monitor = (int)mon;
            win->float_geo = purp_scratchpad_geo_in(purp_wm_monitor_rect(wm, mon),
                mon < wm->monitor_count && wm->monitors[mon].workspace.show_bar);
            win->geo = win->float_geo;
            wm->focused_idx = (int)i;
        }
        purp_wm_relayout(wm);
        return show ? PURP_SCRATCHPAD_SHOWN : PURP_SCRATCHPAD_HIDDEN;
    }
    wm->pending_scratchpad_spawn = true;
    return PURP_SCRATCHPAD_NEEDS_SPAWN;
}
bool purp_wm_bar_visible_on(const purp_wm_t *wm, size_t mon) {
    if (mon >= wm->monitor_count || !wm->monitors[mon].enabled || !wm->monitors[mon].workspace.show_bar) return false;
    for (size_t i = 0; i < wm->window_count; ++i)
        if (wm->windows[i]->monitor == (int)mon && wm->windows[i]->is_fullscreen &&
            purp_wm_is_window_visible(wm, wm->windows[i])) return false;
    return true;
}
bool purp_wm_bar_hit_monitor(const purp_wm_t *wm, purp_pointf_t pt, size_t *mon, int32_t *local_x) {
    for (size_t i = 0; i < wm->monitor_count; ++i) {
        if (!purp_wm_bar_visible_on(wm, i)) continue;
        purp_rect_t rect = purp_wm_monitor_rect(wm, i);
        rect.h = PURP_BAR_HEIGHT;
        if (purp_rect_contains_f(rect, pt)) {
            if (mon) *mon = i;
            if (local_x) *local_x = (int32_t)(pt.x - rect.x);
            return true;
        }
    }
    return false;
}
void purp_wm_bar_inputs(const purp_wm_t *wm, size_t mon, purp_bar_inputs_t *out,
                        char *title_buf, size_t title_len) {
    *out = (purp_bar_inputs_t){ .focused_title = title_buf };
    if (mon < wm->monitor_count) {
        out->selected_tags = wm->monitors[mon].workspace.selected_tags;
        out->layout = purp_workspace_layout(&wm->monitors[mon].workspace);
    }
    const purp_window_t *title_win = NULL;
    purp_window_t *focused = purp_wm_focused_window(wm);
    for (size_t i = 0; i < wm->window_count; ++i) {
        const purp_window_t *win = wm->windows[i];
        if (win->monitor != (int)mon) continue;
        if (!win->is_scratchpad) out->occupied_tags |= win->tags;
        if (win->is_urgent) out->urgent_tags |= win->tags;
        if (!title_win && purp_wm_is_window_visible(wm, win)) title_win = win;
    }
    if (focused && focused->monitor == (int)mon) {
        out->focused_tags = focused->tags;
        if (purp_wm_is_window_visible(wm, focused)) title_win = focused;
    }
    if (!title_buf || !title_len) return;
    if (title_win) {
        const char *prefix = title_win->is_scratchpad ? "[scratch] " :
            title_win->has_opacity_override && title_win->opacity_override >= 1.0f ? "[opaque] " : "";
        (void)snprintf(title_buf, title_len, "%s%s", prefix, purp_window_display_title(title_win));
    } else if (wm->monitor_count > 1 && mon < wm->monitor_count) {
        (void)snprintf(title_buf, title_len,
            "%s — Mod+h: Help | Tap Mod: Spotlight | Mod+Enter: Term | Mod+,/.: Monitor", wm->monitors[mon].name);
    } else {
        (void)snprintf(title_buf, title_len,
            "purpwm+ — Mod+h: Help | Tap Mod: Spotlight | Mod+Enter: Term | Mod+w: Wi-Fi | Mod+Shift+b: BT");
    }
}
