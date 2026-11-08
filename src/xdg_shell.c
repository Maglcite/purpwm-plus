/* xdg-shell v6, server-side decorations and one-shot activation tokens. */
#include "purpwm.h"

#include <errno.h>
#include <sys/random.h>

struct configure_record {
    uint32_t serial;
    struct wl_list link;
};

struct xdg_state {
    purp_xdg_surface_t base;
    struct wl_list configures;
};

struct popup_state {
    purp_popup_t base;
    bool dismissed;
};

struct activation_state {
    purp_activation_token_t base;
    purp_server_t *server;
    struct wl_listener surface_destroy;
};

struct xdg_lifetime {
    purp_server_t *server;
    struct wl_listener destroy;
};

static void destroy_request(struct wl_client *client, struct wl_resource *resource) {
    wl_resource_destroy(resource);
}

static int32_t coord(int64_t value) {
    return value < INT32_MIN ? INT32_MIN : value > INT32_MAX ? INT32_MAX : (int32_t)value;
}

static int direction(uint32_t edge, bool horizontal) {
    if (horizontal) {
        if (edge == 3 || edge == 5 || edge == 6) return -1;
        if (edge == 4 || edge == 7 || edge == 8) return 1;
    } else {
        if (edge == 1 || edge == 5 || edge == 7) return -1;
        if (edge == 2 || edge == 6 || edge == 8) return 1;
    }
    return 0;
}

static int32_t position_axis(int32_t start, int32_t span, int32_t size,
                             int anchor, int gravity, int32_t offset) {
    int64_t point = (int64_t)start + (anchor < 0 ? 0 : anchor > 0 ? span : span / 2);
    return coord(point + offset - (gravity < 0 ? size : gravity > 0 ? 0 : size / 2));
}

purp_rect_t purp_positioner_geometry(const purp_positioner_t *p) {
    return (purp_rect_t){
        position_axis(p->anchor_rect.x, p->anchor_rect.w, p->width,
                      direction(p->anchor, true), direction(p->gravity, true), p->offset_x),
        position_axis(p->anchor_rect.y, p->anchor_rect.h, p->height,
                      direction(p->anchor, false), direction(p->gravity, false), p->offset_y),
        p->width, p->height
    };
}

static bool axis_fits(int64_t start, int64_t size, int64_t bound, int64_t span) {
    return start >= bound && start + size <= bound + span;
}

static void unconstrain_axis(const purp_positioner_t *p, bool horizontal, int32_t bound,
                             int32_t span, int32_t *start, int32_t *size) {
    uint32_t flip = horizontal ? XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_X :
                                XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_Y;
    uint32_t slide = horizontal ? XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_X :
                                 XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_Y;
    uint32_t resize = horizontal ? XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_RESIZE_X :
                                  XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_RESIZE_Y;
    if (axis_fits(*start, *size, bound, span)) return;
    if (p->constraint_adjustment & flip) {
        int32_t flipped = position_axis(horizontal ? p->anchor_rect.x : p->anchor_rect.y,
            horizontal ? p->anchor_rect.w : p->anchor_rect.h, *size,
            -direction(p->anchor, horizontal), -direction(p->gravity, horizontal),
            horizontal ? p->offset_x : p->offset_y);
        if (axis_fits(flipped, *size, bound, span)) *start = flipped;
    }
    if (p->constraint_adjustment & slide) {
        int64_t lo = bound, hi = (int64_t)bound + span - *size;
        if (hi < lo) {
            /* For oversized popups preserve the edge opposite the gravity. */
            if (direction(p->gravity, horizontal) < 0) { int64_t swap = lo; lo = hi; hi = swap; }
            else hi = lo;
        }
        *start = coord(*start < lo ? lo : *start > hi ? hi : *start);
    }
    if ((p->constraint_adjustment & resize) && !axis_fits(*start, *size, bound, span)) {
        int64_t end = (int64_t)*start + *size;
        int64_t first = *start > bound ? *start : bound;
        int64_t last = end < (int64_t)bound + span ? end : (int64_t)bound + span;
        if (last > first) { *start = coord(first); *size = coord(last - first); }
    }
}

purp_rect_t purp_positioner_unconstrain(const purp_positioner_t *p, purp_rect_t bounds) {
    purp_rect_t result = purp_positioner_geometry(p);
    unconstrain_axis(p, true, bounds.x, bounds.w, &result.x, &result.w);
    unconstrain_axis(p, false, bounds.y, bounds.h, &result.y, &result.h);
    return result;
}

static void positioner_free(struct wl_resource *resource) {
    free(wl_resource_get_user_data(resource));
}

static void positioner_size(struct wl_client *client, struct wl_resource *resource,
                            int32_t width, int32_t height) {
    purp_positioner_t *p = wl_resource_get_user_data(resource);
    if (width <= 0 || height <= 0) {
        wl_resource_post_error(resource, XDG_POSITIONER_ERROR_INVALID_INPUT, "invalid popup size");
        return;
    }
    p->width = width; p->height = height;
}

static void positioner_anchor_rect(struct wl_client *client, struct wl_resource *resource,
                                   int32_t x, int32_t y, int32_t width, int32_t height) {
    purp_positioner_t *p = wl_resource_get_user_data(resource);
    if (width <= 0 || height <= 0) {
        wl_resource_post_error(resource, XDG_POSITIONER_ERROR_INVALID_INPUT, "invalid anchor rect");
        return;
    }
    p->anchor_rect = (purp_rect_t){x, y, width, height};
}

static void positioner_anchor(struct wl_client *client, struct wl_resource *resource,
                              uint32_t anchor) {
    purp_positioner_t *p = wl_resource_get_user_data(resource);
    if (anchor > XDG_POSITIONER_ANCHOR_BOTTOM_RIGHT) {
        wl_resource_post_error(resource, XDG_POSITIONER_ERROR_INVALID_INPUT, "invalid anchor");
        return;
    }
    p->anchor = anchor;
}

static void positioner_gravity(struct wl_client *client, struct wl_resource *resource,
                               uint32_t gravity) {
    purp_positioner_t *p = wl_resource_get_user_data(resource);
    if (gravity > XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT) {
        wl_resource_post_error(resource, XDG_POSITIONER_ERROR_INVALID_INPUT, "invalid gravity");
        return;
    }
    p->gravity = gravity;
}

static void positioner_constraints(struct wl_client *client, struct wl_resource *resource,
                                   uint32_t adjustment) {
    purp_positioner_t *p = wl_resource_get_user_data(resource);
    if (adjustment & ~63u) {
        wl_resource_post_error(resource, XDG_POSITIONER_ERROR_INVALID_INPUT, "invalid constraints");
        return;
    }
    p->constraint_adjustment = adjustment;
}

static void positioner_offset(struct wl_client *client, struct wl_resource *resource,
                              int32_t x, int32_t y) {
    purp_positioner_t *p = wl_resource_get_user_data(resource);
    p->offset_x = x; p->offset_y = y;
}

static void positioner_reactive(struct wl_client *client, struct wl_resource *resource) {
    purp_positioner_t *p = wl_resource_get_user_data(resource);
    p->reactive = true;
}

static void positioner_parent_size(struct wl_client *client, struct wl_resource *resource,
                                   int32_t width, int32_t height) {
    purp_positioner_t *p = wl_resource_get_user_data(resource);
    if (width <= 0 || height <= 0) {
        wl_resource_post_error(resource, XDG_POSITIONER_ERROR_INVALID_INPUT, "invalid parent size");
        return;
    }
    p->has_parent_size = true; p->parent_width = width; p->parent_height = height;
}

static void positioner_parent_configure(struct wl_client *client, struct wl_resource *resource,
                                        uint32_t serial) {
    purp_positioner_t *p = wl_resource_get_user_data(resource);
    p->parent_configure = serial;
}

static const struct xdg_positioner_interface positioner_impl = {
    .destroy = destroy_request, .set_size = positioner_size,
    .set_anchor_rect = positioner_anchor_rect, .set_anchor = positioner_anchor,
    .set_gravity = positioner_gravity, .set_constraint_adjustment = positioner_constraints,
    .set_offset = positioner_offset, .set_reactive = positioner_reactive,
    .set_parent_size = positioner_parent_size, .set_parent_configure = positioner_parent_configure,
};

purp_xdg_surface_t *purp_xdg_surface_from_surface(purp_surface_t *surface) {
    if (!surface) return NULL;
    purp_xdg_surface_t *xdg;
    wl_list_for_each(xdg, &surface->server->xdg_surfaces, link)
        if (xdg->surface == surface) return xdg;
    return NULL;
}

purp_window_t *purp_xdg_window_for_surface(purp_server_t *server, purp_surface_t *surface) {
    (void)server;
    while (surface) {
        purp_xdg_surface_t *xdg = purp_xdg_surface_from_surface(purp_surface_root(surface));
        if (!xdg) return NULL;
        if (xdg->role == PURP_XDG_ROLE_TOPLEVEL)
            return xdg->toplevel ? xdg->toplevel->window : NULL;
        if (xdg->role != PURP_XDG_ROLE_POPUP || !xdg->popup) return NULL;
        surface = xdg->popup->parent;
    }
    return NULL;
}

purp_surface_t *purp_xdg_window_surface(const purp_window_t *window) {
    return window && window->toplevel ? window->toplevel->xdg->surface : NULL;
}

static purp_point_t popup_parent_origin(const purp_popup_t *popup) {
    if (popup->parent_layer) return purp_rect_loc(popup->parent_layer->geo);
    if (popup->parent_xdg) {
        purp_xdg_surface_t *parent = popup->parent_xdg;
        if (parent->role == PURP_XDG_ROLE_POPUP && parent->popup)
            return purp_xdg_popup_global_loc(parent->popup);
        if (parent->role == PURP_XDG_ROLE_TOPLEVEL && parent->toplevel && parent->toplevel->window)
            return purp_window_content_loc(parent->toplevel->window);
    }
    return (purp_point_t){0, 0};
}

purp_point_t purp_xdg_popup_global_loc(const purp_popup_t *popup) {
    purp_point_t origin = popup_parent_origin(popup);
    return (purp_point_t){coord((int64_t)origin.x + popup->geometry.x),
                          coord((int64_t)origin.y + popup->geometry.y)};
}

int purp_xdg_popup_monitor(const purp_popup_t *popup) {
    if (!popup) return -1;
    if (popup->parent_layer) return popup->parent_layer->monitor;
    purp_window_t *window = purp_xdg_window_for_surface(popup->xdg->server, popup->parent);
    return window ? window->monitor : -1;
}

static purp_rect_t popup_geometry(purp_popup_t *popup) {
    int monitor = purp_xdg_popup_monitor(popup);
    if (monitor < 0) return purp_positioner_geometry(&popup->positioner);
    purp_server_t *server = popup->xdg->server;
    purp_rect_t bounds = purp_wm_monitor_rect(&server->wm, (size_t)monitor);
    if (purp_wm_bar_visible_on(&server->wm, (size_t)monitor)) {
        bounds.y += PURP_BAR_HEIGHT;
        bounds.h = purp_max_i32(bounds.h - PURP_BAR_HEIGHT, 1);
    }
    purp_point_t origin = popup_parent_origin(popup);
    bounds.x = coord((int64_t)bounds.x - origin.x);
    bounds.y = coord((int64_t)bounds.y - origin.y);
    return purp_positioner_unconstrain(&popup->positioner, bounds);
}

static void configure_clear(purp_xdg_surface_t *xdg) {
    struct xdg_state *state = PURP_CONTAINER_OF(xdg, struct xdg_state, base);
    struct configure_record *record, *tmp;
    wl_list_for_each_safe(record, tmp, &state->configures, link) {
        wl_list_remove(&record->link); free(record);
    }
    xdg->configured = false; xdg->configure_serial = 0; xdg->acked_serial = 0;
}

static struct configure_record *configure_new(purp_xdg_surface_t *xdg) {
    struct configure_record *record = calloc(1, sizeof(*record));
    if (!record) { wl_resource_post_no_memory(xdg->resource); return NULL; }
    struct xdg_state *state = PURP_CONTAINER_OF(xdg, struct xdg_state, base);
    record->serial = wl_display_next_serial(xdg->server->display);
    wl_list_insert(state->configures.prev, &record->link);
    return record;
}

static void configure_finish(purp_xdg_surface_t *xdg, struct configure_record *record) {
    xdg->configure_serial = record->serial;
    xdg->configured = true;
    xdg_surface_send_configure(xdg->resource, record->serial);
}

static void toplevel_configure(purp_toplevel_t *top) {
    struct configure_record *record = configure_new(top->xdg);
    if (!record) return;
    purp_window_configure_t want = top->window->want;
    uint32_t values[6];
    size_t count = 0;
    if (want.activated) values[count++] = XDG_TOPLEVEL_STATE_ACTIVATED;
    if (want.fullscreen) values[count++] = XDG_TOPLEVEL_STATE_FULLSCREEN;
    if (want.tiled && wl_resource_get_version(top->resource) >= 2) {
        values[count++] = XDG_TOPLEVEL_STATE_TILED_LEFT;
        values[count++] = XDG_TOPLEVEL_STATE_TILED_RIGHT;
        values[count++] = XDG_TOPLEVEL_STATE_TILED_TOP;
        values[count++] = XDG_TOPLEVEL_STATE_TILED_BOTTOM;
    }
    if (!top->has_sent && wl_resource_get_version(top->resource) >= 5) {
        uint32_t capabilities[] = {XDG_TOPLEVEL_WM_CAPABILITIES_FULLSCREEN};
        struct wl_array array = {sizeof(capabilities), sizeof(capabilities), capabilities};
        xdg_toplevel_send_wm_capabilities(top->resource, &array);
    }
    if (wl_resource_get_version(top->resource) >= 4 && !top->has_sent) {
        purp_rect_t bounds = purp_wm_monitor_rect(&top->xdg->server->wm,
            top->window->monitor < 0 ? 0 : (size_t)top->window->monitor);
        xdg_toplevel_send_configure_bounds(top->resource, bounds.w, bounds.h);
    }
    struct wl_array states = {count * sizeof(values[0]), sizeof(values), values};
    xdg_toplevel_send_configure(top->resource, want.width, want.height, &states);
    top->sent = want; top->has_sent = true; top->window->configure_dirty = false;
    configure_finish(top->xdg, record);
}

static void popup_configure(purp_popup_t *popup) {
    if (PURP_CONTAINER_OF(popup, struct popup_state, base)->dismissed) return;
    struct configure_record *record = configure_new(popup->xdg);
    if (!record) return;
    if (popup->reposition_pending && wl_resource_get_version(popup->resource) >= 3) {
        xdg_popup_send_repositioned(popup->resource, popup->reposition_token);
        popup->reposition_pending = false;
    }
    popup->geometry = popup_geometry(popup);
    xdg_popup_send_configure(popup->resource, popup->geometry.x, popup->geometry.y,
                            popup->geometry.w, popup->geometry.h);
    configure_finish(popup->xdg, record);
}

static void configure_idle(void *data) {
    purp_xdg_surface_t *xdg = data;
    xdg->configure_idle = NULL;
    if (!xdg->initial_commit_done) return;
    if (xdg->role == PURP_XDG_ROLE_TOPLEVEL && xdg->toplevel) toplevel_configure(xdg->toplevel);
    else if (xdg->role == PURP_XDG_ROLE_POPUP && xdg->popup) popup_configure(xdg->popup);
}

static void configure_schedule(purp_xdg_surface_t *xdg) {
    if (!xdg->initial_commit_done || xdg->configure_idle) return;
    xdg->configure_idle = wl_event_loop_add_idle(xdg->server->loop, configure_idle, xdg);
    if (!xdg->configure_idle) wl_resource_post_no_memory(xdg->resource);
}

static bool configure_equal(purp_window_configure_t a, purp_window_configure_t b) {
    return a.width == b.width && a.height == b.height && a.activated == b.activated &&
           a.fullscreen == b.fullscreen && a.tiled == b.tiled;
}

void purp_xdg_flush_configures(purp_server_t *server) {
    purp_xdg_surface_t *xdg;
    wl_list_for_each(xdg, &server->xdg_surfaces, link) {
        if (!xdg->initial_commit_done) continue;
        if (xdg->role == PURP_XDG_ROLE_TOPLEVEL && xdg->toplevel) {
            purp_toplevel_t *top = xdg->toplevel;
            if (xdg->configure_idle) continue;
            if (!top->has_sent || !configure_equal(top->sent, top->window->want))
                toplevel_configure(top);
            else top->window->configure_dirty = false;
        } else if (xdg->role == PURP_XDG_ROLE_POPUP && xdg->popup &&
                   xdg->popup->positioner.reactive && xdg->configured &&
                   !purp_rect_eq(xdg->popup->geometry, popup_geometry(xdg->popup)))
            configure_schedule(xdg);
    }
}

void purp_xdg_toplevel_send_close(purp_toplevel_t *toplevel) {
    if (toplevel && toplevel->resource) xdg_toplevel_send_close(toplevel->resource);
}

static void popup_ungrab(purp_popup_t *popup) {
    wl_list_remove(&popup->grab_link); wl_list_init(&popup->grab_link); popup->grabbed = false;
}

static void popup_dismiss(purp_popup_t *popup) {
    struct popup_state *state = PURP_CONTAINER_OF(popup, struct popup_state, base);
    if (state->dismissed) return;
    state->dismissed = true;
    purp_popup_t *child;
    wl_list_for_each_reverse(child, &popup->xdg->popups, parent_link) popup_dismiss(child);
    xdg_popup_send_popup_done(popup->resource);
    popup_ungrab(popup);
    popup->committed = false;
}

void purp_xdg_dismiss_popup_grabs(purp_server_t *server) {
    while (!wl_list_empty(&server->popup_grabs)) {
        purp_popup_t *popup = wl_container_of(server->popup_grabs.next, popup, grab_link);
        popup_dismiss(popup);
    }
    purp_host_damage_all(server);
}

static bool popup_visible(purp_popup_t *popup) {
    if (!popup->committed || !popup->parent || !popup->xdg->surface ||
        !purp_surface_is_mapped(popup->xdg->surface)) return false;
    if (popup->parent_layer) return purp_layer_is_mapped(popup->parent_layer);
    if (!popup->parent_xdg) return false;
    if (popup->parent_xdg->role == PURP_XDG_ROLE_POPUP)
        return popup->parent_xdg->popup && popup_visible(popup->parent_xdg->popup);
    purp_window_t *win = purp_xdg_window_for_surface(popup->xdg->server, popup->parent);
    return win && win->mapped && purp_wm_is_window_visible(&popup->xdg->server->wm, win);
}

static void visible_children(struct wl_list *list, purp_popup_t **out, size_t cap, size_t *count) {
    purp_popup_t *popup;
    wl_list_for_each(popup, list, parent_link) {
        if (!popup_visible(popup)) continue;
        if (*count < cap) out[(*count)++] = popup;
        visible_children(&popup->xdg->popups, out, cap, count);
    }
}

size_t purp_xdg_visible_popups(purp_server_t *server, int monitor, purp_popup_t **out, size_t cap) {
    if (!out || !cap || purp_server_is_locked(server)) return 0;
    size_t count = 0;
    purp_layer_surface_t *layer;
    wl_list_for_each(layer, &server->layers, link)
        if (!purp_layer_is_above_windows(layer) && (monitor < 0 || layer->monitor == monitor))
            visible_children(&layer->popups, out, cap, &count);
    size_t order[server->wm.window_count ? server->wm.window_count : 1];
    size_t windows = purp_wm_stacking_order(&server->wm, order, server->wm.window_count);
    while (windows) {
        purp_window_t *win = server->wm.windows[order[--windows]];
        if (win->toplevel && (monitor < 0 || win->monitor == monitor))
            visible_children(&win->toplevel->xdg->popups, out, cap, &count);
    }
    wl_list_for_each(layer, &server->layers, link)
        if (purp_layer_is_above_windows(layer) && (monitor < 0 || layer->monitor == monitor))
            visible_children(&layer->popups, out, cap, &count);
    return count;
}

purp_popup_t *purp_xdg_popup_at(purp_server_t *server, purp_pointf_t pt) {
    size_t capacity = (size_t)wl_list_length(&server->xdg_surfaces);
    purp_popup_t *popups[capacity ? capacity : 1];
    size_t count = purp_xdg_visible_popups(server, -1, popups, capacity);
    while (count) {
        purp_popup_t *popup = popups[--count];
        purp_point_t loc = purp_xdg_popup_global_loc(popup);
        purp_xdg_surface_t *xdg = popup->xdg;
        double sx, sy;
        if (purp_surface_at(xdg->surface, pt.x - loc.x + (xdg->has_geometry ? xdg->geometry.x : 0),
                            pt.y - loc.y + (xdg->has_geometry ? xdg->geometry.y : 0), &sx, &sy))
            return popup;
    }
    return NULL;
}

static void detach_children(purp_xdg_surface_t *xdg) {
    purp_popup_t *popup, *tmp;
    wl_list_for_each_safe(popup, tmp, &xdg->popups, parent_link) {
        popup_dismiss(popup);
        wl_list_remove(&popup->parent_link); wl_list_init(&popup->parent_link);
        popup->parent = NULL; popup->parent_xdg = NULL; popup->parent_layer = NULL;
    }
}

static void role_reset(purp_xdg_surface_t *xdg) {
    if (xdg->configure_idle) { wl_event_source_remove(xdg->configure_idle); xdg->configure_idle = NULL; }
    configure_clear(xdg);
    xdg->initial_commit_done = false;
    xdg->has_geometry = false; xdg->has_pending_geometry = false;
    if (xdg->surface) purp_surface_clear_role_data(xdg->surface);
    xdg->role = PURP_XDG_ROLE_NONE; xdg->toplevel = NULL;
}

static void toplevel_free(struct wl_resource *resource) {
    purp_toplevel_t *top = wl_resource_get_user_data(resource);
    if (!top) return;
    purp_xdg_surface_t *xdg = top->xdg;
    purp_server_t *server = xdg->server;
    detach_children(xdg);
    purp_xdg_surface_t *other;
    wl_list_for_each(other, &server->xdg_surfaces, link)
        if (other->role == PURP_XDG_ROLE_TOPLEVEL && other->toplevel && other->toplevel->parent == top)
            other->toplevel->parent = NULL;
    if (top->decoration) wl_resource_set_user_data(top->decoration, NULL);
    if (top->window) {
        if (server->grab.window_id == top->window->id || server->key_resize.window_id == top->window->id)
            purp_input_cancel_grabs(server);
        purp_wm_window_destroy(&server->wm, top->window);
    }
    role_reset(xdg); free(top);
    purp_server_relayout(server);
}

static void popup_free(struct wl_resource *resource) {
    purp_popup_t *popup = wl_resource_get_user_data(resource);
    if (!popup) return;
    purp_xdg_surface_t *xdg = popup->xdg;
    purp_server_t *server = xdg->server;
    detach_children(xdg);
    popup_ungrab(popup);
    wl_list_remove(&popup->parent_link);
    role_reset(xdg); free(popup);
    purp_server_relayout(server);
}

static void xdg_free(struct wl_resource *resource) {
    purp_xdg_surface_t *xdg = wl_resource_get_user_data(resource);
    if (!xdg) return;
    if (xdg->role == PURP_XDG_ROLE_TOPLEVEL && xdg->toplevel)
        wl_resource_destroy(xdg->toplevel->resource);
    else if (xdg->role == PURP_XDG_ROLE_POPUP && xdg->popup)
        wl_resource_destroy(xdg->popup->resource);
    detach_children(xdg);
    if (xdg->configure_idle) wl_event_source_remove(xdg->configure_idle);
    configure_clear(xdg);
    wl_list_remove(&xdg->surface_destroy.link);
    wl_list_remove(&xdg->link);
    free(PURP_CONTAINER_OF(xdg, struct xdg_state, base));
}

static void xdg_surface_gone(struct wl_listener *listener, void *data) {
    purp_xdg_surface_t *xdg = wl_container_of(listener, xdg, surface_destroy);
    if (xdg->surface) purp_surface_clear_role_data(xdg->surface);
    xdg->surface = NULL;
    wl_list_remove(&xdg->surface_destroy.link); wl_list_init(&xdg->surface_destroy.link);
    if (xdg->role == PURP_XDG_ROLE_TOPLEVEL && xdg->toplevel)
        wl_resource_destroy(xdg->toplevel->resource);
    else if (xdg->role == PURP_XDG_ROLE_POPUP && xdg->popup)
        wl_resource_destroy(xdg->popup->resource);
}

static void xdg_role_commit(purp_surface_t *surface) {
    purp_xdg_surface_t *xdg = surface->role_data;
    if (!xdg) return;
    if (surface->mapped && (!xdg->configured || !xdg->acked_serial)) {
        wl_resource_post_error(xdg->resource, XDG_SURFACE_ERROR_UNCONFIGURED_BUFFER,
                               "buffer committed before acknowledging configure");
        return;
    }
    if (xdg->has_pending_geometry) {
        xdg->geometry = xdg->pending_geometry; xdg->has_geometry = true;
        xdg->has_pending_geometry = false;
    }
    bool first = !xdg->initial_commit_done;
    if (xdg->role == PURP_XDG_ROLE_TOPLEVEL && xdg->toplevel) {
        purp_toplevel_t *top = xdg->toplevel;
        purp_window_t *win = top->window;
        if ((top->pending_max_size.w && top->pending_min_size.w > top->pending_max_size.w) ||
            (top->pending_max_size.h && top->pending_min_size.h > top->pending_max_size.h)) {
            wl_resource_post_error(top->resource, XDG_TOPLEVEL_ERROR_INVALID_SIZE, "minimum exceeds maximum");
            return;
        }
        bool unmap = top->mapped && !surface->mapped;
        top->mapped = win->mapped = surface->mapped;
        win->has_client_geometry = xdg->has_geometry; win->client_geometry = xdg->geometry;
        win->buffer_size = surface->size;
        win->min_size = top->pending_min_size; win->max_size = top->pending_max_size;
        if (unmap) {
            detach_children(xdg); configure_clear(xdg);
            xdg->initial_commit_done = false; top->has_sent = false;
            top->parent = NULL;
            top->pending_min_size = top->pending_max_size = (purp_size_t){0, 0};
            win->min_size = win->max_size = (purp_size_t){0, 0};
            win->is_fullscreen = false;
        } else {
            xdg->initial_commit_done = true;
            int index = purp_wm_window_index(&xdg->server->wm, win);
            if (index >= 0 && surface->mapped) purp_wm_adopt_client_size(&xdg->server->wm, (size_t)index);
        }
        purp_server_relayout(xdg->server);
    } else if (xdg->role == PURP_XDG_ROLE_POPUP && xdg->popup) {
        purp_popup_t *popup = xdg->popup;
        if (PURP_CONTAINER_OF(popup, struct popup_state, base)->dismissed) return;
        if (!popup->parent) {
            wl_resource_post_error(xdg->wm_base, XDG_WM_BASE_ERROR_INVALID_POPUP_PARENT,
                                   "popup must have a parent before initial commit");
            return;
        }
        if (popup->committed && !surface->mapped) {
            popup_dismiss(popup); configure_clear(xdg); xdg->initial_commit_done = false;
        } else {
            xdg->initial_commit_done = true;
            popup->committed = surface->mapped;
            if (first) popup_configure(popup);
        }
        purp_server_update_surface_outputs(xdg->server);
        purp_host_damage_all(xdg->server);
    }
}

static void xdg_role_destroy(purp_surface_t *surface) {
    /* The destroy signal listener handles either role/destroy-signal ordering. */
    purp_xdg_surface_t *xdg = surface->role_data;
    if (!xdg) return;
    xdg_surface_gone(&xdg->surface_destroy, surface);
}

static const purp_surface_role_t toplevel_role = {
    .name = "xdg_toplevel", .kind = PURP_ROLE_XDG_TOPLEVEL,
    .commit = xdg_role_commit, .destroy = xdg_role_destroy,
};
static const purp_surface_role_t popup_role = {
    .name = "xdg_popup", .kind = PURP_ROLE_XDG_POPUP,
    .commit = xdg_role_commit, .destroy = xdg_role_destroy,
};

static void toplevel_parent(struct wl_client *client, struct wl_resource *resource,
                            struct wl_resource *parent_resource) {
    purp_toplevel_t *top = wl_resource_get_user_data(resource);
    purp_toplevel_t *parent = parent_resource ? wl_resource_get_user_data(parent_resource) : NULL;
    for (purp_toplevel_t *ancestor = parent; ancestor; ancestor = ancestor->parent) {
        if (ancestor == top) {
            wl_resource_post_error(resource, XDG_TOPLEVEL_ERROR_INVALID_PARENT, "cyclic toplevel parent");
            return;
        }
    }
    top->parent = parent;
}

static void toplevel_title(struct wl_client *client, struct wl_resource *resource, const char *title) {
    purp_toplevel_t *top = wl_resource_get_user_data(resource);
    if (purp_window_set_title(top->window, title)) purp_server_relayout(top->xdg->server);
}

static void toplevel_app_id(struct wl_client *client, struct wl_resource *resource, const char *app_id) {
    purp_toplevel_t *top = wl_resource_get_user_data(resource);
    if (purp_window_set_app_id(top->window, app_id)) purp_server_relayout(top->xdg->server);
}

static bool valid_seat(purp_server_t *server, struct wl_resource *seat) {
    return seat && wl_resource_get_user_data(seat) == server;
}

static void toplevel_menu(struct wl_client *client, struct wl_resource *resource,
                          struct wl_resource *seat, uint32_t serial, int32_t x, int32_t y) {
    /* No compositor window menu is exposed. The request is explicitly optional. */
}

static void toplevel_move(struct wl_client *client, struct wl_resource *resource,
                          struct wl_resource *seat, uint32_t serial) {
    purp_toplevel_t *top = wl_resource_get_user_data(resource);
    if (valid_seat(top->xdg->server, seat))
        purp_input_begin_client_grab(top->xdg->server, top, serial, false, 0);
}

static void toplevel_resize(struct wl_client *client, struct wl_resource *resource,
                            struct wl_resource *seat, uint32_t serial, uint32_t edges) {
    purp_toplevel_t *top = wl_resource_get_user_data(resource);
    if (!(edges == 0 || edges == 1 || edges == 2 || edges == 4 || edges == 5 ||
          edges == 6 || edges == 8 || edges == 9 || edges == 10)) {
        wl_resource_post_error(resource, XDG_TOPLEVEL_ERROR_INVALID_RESIZE_EDGE, "invalid resize edge");
        return;
    }
    if (edges && valid_seat(top->xdg->server, seat))
        purp_input_begin_client_grab(top->xdg->server, top, serial, true, edges);
}

static void toplevel_min_size(struct wl_client *client, struct wl_resource *resource,
                              int32_t width, int32_t height) {
    purp_toplevel_t *top = wl_resource_get_user_data(resource);
    if (width < 0 || height < 0) {
        wl_resource_post_error(resource, XDG_TOPLEVEL_ERROR_INVALID_SIZE, "negative minimum size");
        return;
    }
    top->pending_min_size = (purp_size_t){width, height};
}

static void toplevel_max_size(struct wl_client *client, struct wl_resource *resource,
                              int32_t width, int32_t height) {
    purp_toplevel_t *top = wl_resource_get_user_data(resource);
    if (width < 0 || height < 0) {
        wl_resource_post_error(resource, XDG_TOPLEVEL_ERROR_INVALID_SIZE, "negative maximum size");
        return;
    }
    top->pending_max_size = (purp_size_t){width, height};
}

static void toplevel_maximize(struct wl_client *client, struct wl_resource *resource) {
    purp_toplevel_t *top = wl_resource_get_user_data(resource);
    /* Tiling determines size; acknowledge the request without advertising maximized. */
    configure_schedule(top->xdg);
}

static void toplevel_fullscreen(struct wl_client *client, struct wl_resource *resource,
                                struct wl_resource *output) {
    purp_toplevel_t *top = wl_resource_get_user_data(resource);
    purp_server_t *server = top->xdg->server;
    configure_schedule(top->xdg);
    if (purp_server_is_locked(server)) return;
    int index = purp_wm_window_index(&server->wm, top->window);
    if (index >= 0) purp_wm_set_window_fullscreen(&server->wm, (size_t)index, true,
        output ? purp_output_monitor_from_resource(output) : -1);
    purp_server_relayout(server);
}

static void toplevel_unfullscreen(struct wl_client *client, struct wl_resource *resource) {
    purp_toplevel_t *top = wl_resource_get_user_data(resource);
    purp_server_t *server = top->xdg->server;
    configure_schedule(top->xdg);
    if (purp_server_is_locked(server)) return;
    int index = purp_wm_window_index(&server->wm, top->window);
    if (index >= 0) purp_wm_set_window_fullscreen(&server->wm, (size_t)index, false, -1);
    purp_server_relayout(server);
}

static void toplevel_minimize(struct wl_client *client, struct wl_resource *resource) {
    /* There is no minimized state in this tag-based window manager. */
}

static const struct xdg_toplevel_interface toplevel_impl = {
    .destroy = destroy_request, .set_parent = toplevel_parent, .set_title = toplevel_title,
    .set_app_id = toplevel_app_id, .show_window_menu = toplevel_menu,
    .move = toplevel_move, .resize = toplevel_resize,
    .set_max_size = toplevel_max_size, .set_min_size = toplevel_min_size,
    .set_maximized = toplevel_maximize, .unset_maximized = toplevel_maximize,
    .set_fullscreen = toplevel_fullscreen, .unset_fullscreen = toplevel_unfullscreen,
    .set_minimized = toplevel_minimize,
};

static void popup_destroy_request(struct wl_client *client, struct wl_resource *resource) {
    purp_popup_t *popup = wl_resource_get_user_data(resource);
    if (!wl_list_empty(&popup->xdg->popups)) {
        wl_resource_post_error(popup->xdg->wm_base, XDG_WM_BASE_ERROR_NOT_THE_TOPMOST_POPUP,
                               "destroy popup children first");
        return;
    }
    wl_resource_destroy(resource);
}

static void popup_grab(struct wl_client *client, struct wl_resource *resource,
                        struct wl_resource *seat, uint32_t serial) {
    purp_popup_t *popup = wl_resource_get_user_data(resource);
    if (PURP_CONTAINER_OF(popup, struct popup_state, base)->dismissed) return;
    purp_server_t *server = popup->xdg->server;
    if (purp_server_is_locked(server) || !valid_seat(server, seat) ||
        !purp_seat_validate_grab_serial(server, client, serial)) {
        popup_dismiss(popup); return;
    }
    if (popup->committed || popup->grabbed) {
        wl_resource_post_error(resource, XDG_POPUP_ERROR_INVALID_GRAB, "popup already mapped or grabbed");
        return;
    }
    if (!wl_list_empty(&server->popup_grabs)) {
        purp_popup_t *top = wl_container_of(server->popup_grabs.next, top, grab_link);
        if (popup->parent_xdg != top->xdg) {
            wl_resource_post_error(popup->xdg->wm_base, XDG_WM_BASE_ERROR_NOT_THE_TOPMOST_POPUP,
                                   "grab parent must be the topmost popup");
            return;
        }
    } else if (popup->parent_xdg && popup->parent_xdg->role == PURP_XDG_ROLE_POPUP) {
        wl_resource_post_error(resource, XDG_POPUP_ERROR_INVALID_GRAB, "parent popup has no grab");
        return;
    }
    popup->grabbed = true;
    wl_list_insert(&server->popup_grabs, &popup->grab_link);
}

static bool positioner_complete(purp_positioner_t *p) {
    return p && p->width > 0 && p->height > 0 && p->anchor_rect.w > 0 && p->anchor_rect.h > 0;
}

static void popup_reposition(struct wl_client *client, struct wl_resource *resource,
                             struct wl_resource *positioner, uint32_t token) {
    purp_popup_t *popup = wl_resource_get_user_data(resource);
    if (PURP_CONTAINER_OF(popup, struct popup_state, base)->dismissed) return;
    purp_positioner_t *p = wl_resource_get_user_data(positioner);
    if (!positioner_complete(p)) {
        wl_resource_post_error(popup->xdg->wm_base, XDG_WM_BASE_ERROR_INVALID_POSITIONER,
                               "incomplete positioner");
        return;
    }
    popup->positioner = *p; popup->reposition_token = token; popup->reposition_pending = true;
    if (popup->xdg->initial_commit_done) popup_configure(popup);
}

static const struct xdg_popup_interface popup_impl = {
    .destroy = popup_destroy_request, .grab = popup_grab, .reposition = popup_reposition,
};

static void xdg_destroy_request(struct wl_client *client, struct wl_resource *resource) {
    purp_xdg_surface_t *xdg = wl_resource_get_user_data(resource);
    if (xdg->role != PURP_XDG_ROLE_NONE) {
        wl_resource_post_error(resource, XDG_SURFACE_ERROR_DEFUNCT_ROLE_OBJECT,
                               "destroy the role object before xdg_surface");
        return;
    }
    wl_resource_destroy(resource);
}

static bool xdg_can_construct(purp_xdg_surface_t *xdg) {
    if (!xdg->surface || xdg->role != PURP_XDG_ROLE_NONE) {
        wl_resource_post_error(xdg->resource, XDG_SURFACE_ERROR_ALREADY_CONSTRUCTED,
                               "xdg surface already has a role or no wl_surface");
        return false;
    }
    return true;
}

static void xdg_get_toplevel(struct wl_client *client, struct wl_resource *resource, uint32_t id) {
    purp_xdg_surface_t *xdg = wl_resource_get_user_data(resource);
    if (!xdg_can_construct(xdg)) return;
    purp_toplevel_t *top = calloc(1, sizeof(*top));
    if (!top) { wl_resource_post_no_memory(resource); return; }
    top->xdg = xdg;
    top->resource = wl_resource_create(client, &xdg_toplevel_interface, wl_resource_get_version(resource), id);
    if (!top->resource) { free(top); wl_resource_post_no_memory(resource); return; }
    if (!purp_surface_set_role(xdg->surface, &toplevel_role, xdg, xdg->wm_base, XDG_WM_BASE_ERROR_ROLE)) {
        wl_resource_destroy(top->resource); free(top); return;
    }
    top->window = purp_wm_window_create(&xdg->server->wm, top);
    if (!top->window) {
        purp_surface_clear_role_data(xdg->surface);
        wl_resource_destroy(top->resource); free(top); wl_resource_post_no_memory(resource); return;
    }
    xdg->role = PURP_XDG_ROLE_TOPLEVEL; xdg->toplevel = top;
    wl_resource_set_implementation(top->resource, &toplevel_impl, top, toplevel_free);
}

static void xdg_get_popup(struct wl_client *client, struct wl_resource *resource, uint32_t id,
                          struct wl_resource *parent_resource, struct wl_resource *positioner) {
    purp_xdg_surface_t *xdg = wl_resource_get_user_data(resource);
    if (!xdg_can_construct(xdg)) return;
    purp_positioner_t *p = wl_resource_get_user_data(positioner);
    purp_xdg_surface_t *parent = parent_resource ? wl_resource_get_user_data(parent_resource) : NULL;
    if (!positioner_complete(p)) {
        wl_resource_post_error(xdg->wm_base, XDG_WM_BASE_ERROR_INVALID_POSITIONER, "incomplete positioner");
        return;
    }
    if (parent_resource && (!parent || parent == xdg || !parent->surface || parent->role == PURP_XDG_ROLE_NONE)) {
        wl_resource_post_error(xdg->wm_base, XDG_WM_BASE_ERROR_INVALID_POPUP_PARENT, "invalid popup parent");
        return;
    }
    purp_popup_t *popup = calloc(1, sizeof(struct popup_state));
    if (!popup) { wl_resource_post_no_memory(resource); return; }
    popup->xdg = xdg; popup->positioner = *p;
    wl_list_init(&popup->parent_link); wl_list_init(&popup->grab_link);
    popup->resource = wl_resource_create(client, &xdg_popup_interface, wl_resource_get_version(resource), id);
    if (!popup->resource) { free(popup); wl_resource_post_no_memory(resource); return; }
    if (!purp_surface_set_role(xdg->surface, &popup_role, xdg, xdg->wm_base, XDG_WM_BASE_ERROR_ROLE)) {
        wl_resource_destroy(popup->resource); free(popup); return;
    }
    popup->parent_xdg = parent; popup->parent = parent ? parent->surface : NULL;
    if (parent) wl_list_insert(parent->popups.prev, &popup->parent_link);
    popup->geometry = popup_geometry(popup);
    xdg->role = PURP_XDG_ROLE_POPUP; xdg->popup = popup;
    wl_resource_set_implementation(popup->resource, &popup_impl, popup, popup_free);
}

static void xdg_geometry(struct wl_client *client, struct wl_resource *resource,
                          int32_t x, int32_t y, int32_t width, int32_t height) {
    purp_xdg_surface_t *xdg = wl_resource_get_user_data(resource);
    if (width <= 0 || height <= 0) {
        wl_resource_post_error(resource, XDG_SURFACE_ERROR_INVALID_SIZE, "invalid window geometry");
        return;
    }
    xdg->pending_geometry = (purp_rect_t){x, y, width, height}; xdg->has_pending_geometry = true;
}

static void xdg_ack(struct wl_client *client, struct wl_resource *resource, uint32_t serial) {
    purp_xdg_surface_t *xdg = wl_resource_get_user_data(resource);
    struct xdg_state *state = PURP_CONTAINER_OF(xdg, struct xdg_state, base);
    struct configure_record *record;
    bool found = false;
    wl_list_for_each(record, &state->configures, link)
        if (record->serial == serial) { found = true; break; }
    if (!found) {
        wl_resource_post_error(resource, XDG_SURFACE_ERROR_INVALID_SERIAL, "unknown configure serial");
        return;
    }
    while (!wl_list_empty(&state->configures)) {
        record = wl_container_of(state->configures.next, record, link);
        bool last = record->serial == serial;
        wl_list_remove(&record->link); free(record);
        if (last) break;
    }
    xdg->acked_serial = serial;
}

static const struct xdg_surface_interface xdg_surface_impl = {
    .destroy = xdg_destroy_request, .get_toplevel = xdg_get_toplevel, .get_popup = xdg_get_popup,
    .set_window_geometry = xdg_geometry, .ack_configure = xdg_ack,
};

static void wm_base_destroy(struct wl_client *client, struct wl_resource *resource) {
    purp_server_t *server = wl_resource_get_user_data(resource);
    purp_xdg_surface_t *xdg;
    wl_list_for_each(xdg, &server->xdg_surfaces, link) {
        if (xdg->wm_base == resource) {
            wl_resource_post_error(resource, XDG_WM_BASE_ERROR_DEFUNCT_SURFACES, "xdg surfaces remain");
            return;
        }
    }
    wl_resource_destroy(resource);
}

static void wm_base_free(struct wl_resource *resource) {
    purp_server_t *server = wl_resource_get_user_data(resource);
    purp_xdg_surface_t *xdg;
    wl_list_for_each(xdg, &server->xdg_surfaces, link)
        if (xdg->wm_base == resource) xdg->wm_base = NULL;
}

static void wm_base_positioner(struct wl_client *client, struct wl_resource *resource, uint32_t id) {
    purp_positioner_t *p = calloc(1, sizeof(*p));
    if (!p) { wl_resource_post_no_memory(resource); return; }
    struct wl_resource *object = wl_resource_create(client, &xdg_positioner_interface,
                                                    wl_resource_get_version(resource), id);
    if (!object) { free(p); wl_resource_post_no_memory(resource); return; }
    wl_resource_set_implementation(object, &positioner_impl, p, positioner_free);
}

static void wm_base_surface(struct wl_client *client, struct wl_resource *resource, uint32_t id,
                             struct wl_resource *surface_resource) {
    purp_server_t *server = wl_resource_get_user_data(resource);
    purp_surface_t *surface = purp_surface_from_resource(surface_resource);
    if (!surface || purp_xdg_surface_from_surface(surface) || surface->role_data || surface->mapped ||
        surface->current.buffer || surface->pending.buffer) {
        wl_resource_post_error(resource, XDG_WM_BASE_ERROR_INVALID_SURFACE_STATE,
                               "surface already constructed or has a buffer");
        return;
    }
    struct xdg_state *state = calloc(1, sizeof(*state));
    if (!state) { wl_resource_post_no_memory(resource); return; }
    purp_xdg_surface_t *xdg = &state->base;
    xdg->resource = wl_resource_create(client, &xdg_surface_interface, wl_resource_get_version(resource), id);
    if (!xdg->resource) { free(state); wl_resource_post_no_memory(resource); return; }
    xdg->server = server; xdg->wm_base = resource; xdg->surface = surface;
    wl_list_init(&state->configures); wl_list_init(&xdg->popups);
    xdg->surface_destroy.notify = xdg_surface_gone;
    wl_signal_add(&surface->events_destroy, &xdg->surface_destroy);
    wl_list_insert(server->xdg_surfaces.prev, &xdg->link);
    wl_resource_set_implementation(xdg->resource, &xdg_surface_impl, xdg, xdg_free);
}

static void wm_base_pong(struct wl_client *client, struct wl_resource *resource, uint32_t serial) {
    /* No outstanding ping is maintained: clients are not timed out by this compositor. */
}

static const struct xdg_wm_base_interface wm_base_impl = {
    .destroy = wm_base_destroy, .create_positioner = wm_base_positioner,
    .get_xdg_surface = wm_base_surface, .pong = wm_base_pong,
};

static void bind_wm_base(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
    struct wl_resource *resource = wl_resource_create(client, &xdg_wm_base_interface, (int)version, id);
    if (!resource) { wl_client_post_no_memory(client); return; }
    wl_resource_set_implementation(resource, &wm_base_impl, data, wm_base_free);
}

static void decoration_free(struct wl_resource *resource) {
    purp_toplevel_t *top = wl_resource_get_user_data(resource);
    if (top) { top->decoration = NULL; configure_schedule(top->xdg); }
}

static void decoration_configure(struct wl_resource *resource) {
    purp_toplevel_t *top = wl_resource_get_user_data(resource);
    if (!top) return;
    zxdg_toplevel_decoration_v1_send_configure(resource, ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
    configure_schedule(top->xdg);
}

static void decoration_mode(struct wl_client *client, struct wl_resource *resource, uint32_t mode) {
    if (mode != ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE &&
        mode != ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE) {
        wl_resource_post_error(resource, ZXDG_TOPLEVEL_DECORATION_V1_ERROR_INVALID_MODE, "invalid mode");
        return;
    }
    decoration_configure(resource);
}

static void decoration_unset(struct wl_client *client, struct wl_resource *resource) {
    decoration_configure(resource);
}

static const struct zxdg_toplevel_decoration_v1_interface decoration_impl = {
    .destroy = destroy_request, .set_mode = decoration_mode, .unset_mode = decoration_unset,
};

static void decoration_get(struct wl_client *client, struct wl_resource *resource, uint32_t id,
                            struct wl_resource *toplevel) {
    purp_toplevel_t *top = wl_resource_get_user_data(toplevel);
    if (!top) return;
    if (top->decoration) {
        wl_resource_post_error(resource, ZXDG_TOPLEVEL_DECORATION_V1_ERROR_ALREADY_CONSTRUCTED,
                               "decoration already exists");
        return;
    }
    if (top->xdg->surface && top->xdg->surface->mapped) {
        wl_resource_post_error(resource, ZXDG_TOPLEVEL_DECORATION_V1_ERROR_UNCONFIGURED_BUFFER,
                               "decoration created after attaching a buffer");
        return;
    }
    top->decoration = wl_resource_create(client, &zxdg_toplevel_decoration_v1_interface, 1, id);
    if (!top->decoration) { wl_resource_post_no_memory(resource); return; }
    wl_resource_set_implementation(top->decoration, &decoration_impl, top, decoration_free);
    decoration_configure(top->decoration);
}

static const struct zxdg_decoration_manager_v1_interface decoration_manager_impl = {
    .destroy = destroy_request, .get_toplevel_decoration = decoration_get,
};

static void bind_decoration(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
    struct wl_resource *resource = wl_resource_create(client, &zxdg_decoration_manager_v1_interface,
                                                     (int)version, id);
    if (!resource) { wl_client_post_no_memory(client); return; }
    wl_resource_set_implementation(resource, &decoration_manager_impl, data, NULL);
}

static void activation_surface_gone(struct wl_listener *listener, void *data) {
    struct activation_state *state = wl_container_of(listener, state, surface_destroy);
    state->base.surface = NULL;
    wl_list_remove(&state->surface_destroy.link); wl_list_init(&state->surface_destroy.link);
}

static void activation_free(purp_activation_token_t *token) {
    struct activation_state *state = PURP_CONTAINER_OF(token, struct activation_state, base);
    if (token->resource) wl_resource_set_user_data(token->resource, NULL);
    wl_list_remove(&state->surface_destroy.link);
    wl_list_remove(&token->link);
    free(token->app_id); free(state);
}

void purp_xdg_activation_prune(purp_server_t *server, uint64_t now_ms) {
    purp_activation_token_t *token, *tmp;
    wl_list_for_each_safe(token, tmp, &server->activation_tokens, link)
        if (token->committed && token->resource == NULL && now_ms >= token->created_ms &&
            now_ms - token->created_ms >= PURP_ACTIVATION_TOKEN_TTL_MS)
            activation_free(token);
}

static purp_activation_token_t *activation_mutable(struct wl_resource *resource) {
    purp_activation_token_t *token = wl_resource_get_user_data(resource);
    if (!token || token->committed) {
        wl_resource_post_error(resource, XDG_ACTIVATION_TOKEN_V1_ERROR_ALREADY_USED, "token already committed");
        return NULL;
    }
    return token;
}

static void activation_token_free(struct wl_resource *resource) {
    purp_activation_token_t *token = wl_resource_get_user_data(resource);
    if (!token) return;
    token->resource = NULL;
    if (!token->committed) activation_free(token);
}

static void activation_serial(struct wl_client *client, struct wl_resource *resource,
                               uint32_t serial, struct wl_resource *seat) {
    purp_activation_token_t *token = activation_mutable(resource);
    if (!token) return;
    struct activation_state *state = PURP_CONTAINER_OF(token, struct activation_state, base);
    token->serial = serial;
    token->has_serial = valid_seat(state->server, seat) &&
                       purp_seat_validate_grab_serial(state->server, client, serial);
}

static void activation_app_id(struct wl_client *client, struct wl_resource *resource, const char *app_id) {
    purp_activation_token_t *token = activation_mutable(resource);
    if (!token) return;
    char *copy = strdup(app_id);
    if (!copy) { wl_resource_post_no_memory(resource); return; }
    free(token->app_id); token->app_id = copy;
}

static void activation_surface(struct wl_client *client, struct wl_resource *resource,
                                struct wl_resource *surface) {
    purp_activation_token_t *token = activation_mutable(resource);
    if (!token) return;
    struct activation_state *state = PURP_CONTAINER_OF(token, struct activation_state, base);
    wl_list_remove(&state->surface_destroy.link); wl_list_init(&state->surface_destroy.link);
    token->surface = purp_surface_from_resource(surface);
    if (token->surface) wl_signal_add(&token->surface->events_destroy, &state->surface_destroy);
}

static void activation_commit(struct wl_client *client, struct wl_resource *resource) {
    purp_activation_token_t *token = activation_mutable(resource);
    if (!token) return;
    unsigned char random[16];
    size_t done = 0;
    while (done < sizeof(random)) {
        ssize_t bytes = getrandom(random + done, sizeof(random) - done, 0);
        if (bytes < 0 && errno == EINTR) continue;
        if (bytes <= 0) { wl_client_post_implementation_error(client, "cannot generate activation token"); return; }
        done += (size_t)bytes;
    }
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < sizeof(random); ++i) {
        token->token[2 * i] = hex[random[i] >> 4];
        token->token[2 * i + 1] = hex[random[i] & 15];
    }
    token->token[32] = '\0'; token->committed = true; token->created_ms = purp_now_ms();
    xdg_activation_token_v1_send_done(resource, token->token);
    token->resource = NULL;
    wl_resource_set_user_data(resource, NULL);
}

static const struct xdg_activation_token_v1_interface activation_token_impl = {
    .set_serial = activation_serial, .set_app_id = activation_app_id,
    .set_surface = activation_surface, .commit = activation_commit, .destroy = destroy_request,
};

static void activation_get(struct wl_client *client, struct wl_resource *resource, uint32_t id) {
    struct activation_state *state = calloc(1, sizeof(*state));
    if (!state) { wl_resource_post_no_memory(resource); return; }
    purp_activation_token_t *token = &state->base;
    state->server = wl_resource_get_user_data(resource);
    token->created_ms = purp_now_ms();
    token->resource = wl_resource_create(client, &xdg_activation_token_v1_interface, 1, id);
    if (!token->resource) { free(state); wl_resource_post_no_memory(resource); return; }
    wl_list_init(&state->surface_destroy.link); state->surface_destroy.notify = activation_surface_gone;
    wl_list_insert(state->server->activation_tokens.prev, &token->link);
    wl_resource_set_implementation(token->resource, &activation_token_impl, token, activation_token_free);
}

static void activation_activate(struct wl_client *client, struct wl_resource *resource,
                                 const char *value, struct wl_resource *surface) {
    purp_server_t *server = wl_resource_get_user_data(resource);
    purp_xdg_activation_prune(server, purp_now_ms());
    bool trusted = false;
    purp_activation_token_t *token;
    wl_list_for_each(token, &server->activation_tokens, link) {
        if (token->committed && strcmp(token->token, value) == 0) {
            trusted = token->has_serial;
            activation_free(token);
            break;
        }
    }
    purp_window_t *win = purp_xdg_window_for_surface(server, purp_surface_from_resource(surface));
    if (!win) return;
    if (trusted && !purp_server_is_locked(server) && win->mapped && purp_wm_is_window_visible(&server->wm, win)) {
        server->layer_focus = NULL;
        purp_wm_focus_window(&server->wm, purp_wm_window_index(&server->wm, win));
    } else win->is_urgent = true;
    purp_server_relayout(server);
}

static const struct xdg_activation_v1_interface activation_impl = {
    .destroy = destroy_request, .get_activation_token = activation_get, .activate = activation_activate,
};

static void bind_activation(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
    struct wl_resource *resource = wl_resource_create(client, &xdg_activation_v1_interface, (int)version, id);
    if (!resource) { wl_client_post_no_memory(client); return; }
    wl_resource_set_implementation(resource, &activation_impl, data, NULL);
}

static void xdg_display_destroy(struct wl_listener *listener, void *data) {
    struct xdg_lifetime *lifetime = wl_container_of(listener, lifetime, destroy);
    purp_activation_token_t *token, *tmp;
    wl_list_for_each_safe(token, tmp, &lifetime->server->activation_tokens, link) activation_free(token);
    wl_list_remove(&lifetime->destroy.link); free(lifetime);
}

bool purp_xdg_init(purp_server_t *server) {
    struct xdg_lifetime *lifetime = calloc(1, sizeof(*lifetime));
    if (!lifetime) return false;
    server->xdg_wm_base_global = wl_global_create(server->display, &xdg_wm_base_interface,
        PURP_XDG_WM_BASE_VERSION, server, bind_wm_base);
    server->decoration_global = wl_global_create(server->display, &zxdg_decoration_manager_v1_interface,
        PURP_XDG_DECORATION_VERSION, server, bind_decoration);
    server->activation_global = wl_global_create(server->display, &xdg_activation_v1_interface,
        PURP_XDG_ACTIVATION_VERSION, server, bind_activation);
    if (!server->xdg_wm_base_global || !server->decoration_global || !server->activation_global) {
        if (server->xdg_wm_base_global) wl_global_destroy(server->xdg_wm_base_global);
        if (server->decoration_global) wl_global_destroy(server->decoration_global);
        if (server->activation_global) wl_global_destroy(server->activation_global);
        server->xdg_wm_base_global = NULL; server->decoration_global = NULL; server->activation_global = NULL;
        free(lifetime); return false;
    }
    lifetime->server = server; lifetime->destroy.notify = xdg_display_destroy;
    wl_display_add_destroy_listener(server->display, &lifetime->destroy);
    return true;
}
