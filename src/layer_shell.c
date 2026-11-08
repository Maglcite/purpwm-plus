#include "purpwm.h"

#include <stdlib.h>
#include <string.h>

/* Private surface metadata: empty commits do not construct buffer contents. */
bool purp_surface_has_buffer_history(const purp_surface_t *surface);

static int32_t layer_clamp_i32(int64_t value) {
    if (value > INT32_MAX) return INT32_MAX;
    if (value < INT32_MIN) return INT32_MIN;
    return (int32_t)value;
}

purp_rect_t purp_layer_compute_geometry(const purp_layer_state_t *state, purp_rect_t monitor_rect) {
    if (!state) return purp_rect_make(0, 0, 0, 0);

    const uint32_t horizontal =
        ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT;
    const uint32_t vertical =
        ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM;
    bool left = (state->anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT) != 0;
    bool right = (state->anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT) != 0;
    bool top = (state->anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP) != 0;
    bool bottom = (state->anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM) != 0;
    int64_t monitor_w = purp_max_i32(monitor_rect.w, 0);
    int64_t monitor_h = purp_max_i32(monitor_rect.h, 0);
    int64_t width;
    int64_t height;

    if (state->desired_width != 0) {
        width = state->desired_width;
    } else if ((state->anchor & horizontal) == horizontal) {
        width = monitor_w - state->margin_left - state->margin_right;
        if (width < 0) width = 0;
    } else {
        width = monitor_w < PURP_LAYER_DEFAULT_WIDTH ? monitor_w : PURP_LAYER_DEFAULT_WIDTH;
    }

    if (state->desired_height != 0) {
        height = state->desired_height;
    } else if ((state->anchor & vertical) == vertical) {
        height = monitor_h - state->margin_top - state->margin_bottom;
        if (height < 0) height = 0;
    } else {
        height = monitor_h < PURP_LAYER_DEFAULT_HEIGHT ? monitor_h : PURP_LAYER_DEFAULT_HEIGHT;
    }

    int64_t x;
    int64_t y;
    if (left) {
        x = (int64_t)monitor_rect.x + state->margin_left;
    } else if (right) {
        x = (int64_t)monitor_rect.x + monitor_w - width - state->margin_right;
    } else {
        x = (int64_t)monitor_rect.x + (monitor_w - width) / 2;
    }

    if (top) {
        y = (int64_t)monitor_rect.y + state->margin_top;
    } else if (bottom) {
        y = (int64_t)monitor_rect.y + monitor_h - height - state->margin_bottom;
    } else {
        y = (int64_t)monitor_rect.y + (monitor_h - height) / 2;
    }

    return purp_rect_make(layer_clamp_i32(x), layer_clamp_i32(y),
                          layer_clamp_i32(width), layer_clamp_i32(height));
}

static purp_rect_t layer_monitor_rect(purp_server_t *server, const purp_layer_surface_t *layer) {
    if (!server || server->wm.monitor_count == 0) return purp_rect_make(0, 0, 0, 0);

    size_t monitor;
    if (layer && layer->monitor >= 0 && (size_t)layer->monitor < server->wm.monitor_count &&
        server->wm.monitors[layer->monitor].enabled) {
        monitor = (size_t)layer->monitor;
    } else {
        monitor = purp_wm_primary_monitor_index(&server->wm);
    }
    if (monitor >= server->wm.monitor_count) return purp_rect_make(0, 0, 0, 0);
    return purp_monitor_rect(&server->wm.monitors[monitor]);
}

static void layer_send_configure(purp_layer_surface_t *layer) {
    if (!layer || !layer->resource || layer->closed || !layer->server ||
        !layer->server->display) {
        return;
    }

    layer->configure_serial = wl_display_next_serial(layer->server->display);
    layer->configured = true;
    layer->configured_size = (purp_size_t){layer->geo.w, layer->geo.h};
    zwlr_layer_surface_v1_send_configure(layer->resource, layer->configure_serial,
                                         (uint32_t)layer->geo.w, (uint32_t)layer->geo.h);
}

static void layer_unconstrain_popup(purp_layer_surface_t *layer, purp_popup_t *popup) {
    purp_rect_t bounds = layer_monitor_rect(layer->server, layer);
    bounds.x = layer_clamp_i32((int64_t)bounds.x - layer->geo.x);
    bounds.y = layer_clamp_i32((int64_t)bounds.y - layer->geo.y);
    popup->geometry = purp_positioner_unconstrain(&popup->positioner, bounds);
}

static void layer_detach_popups(purp_layer_surface_t *layer) {
    while (!wl_list_empty(&layer->popups)) {
        purp_popup_t *popup = wl_container_of(layer->popups.next, popup, parent_link);
        wl_list_remove(&popup->parent_link);
        wl_list_init(&popup->parent_link);
        popup->parent = NULL;
        popup->parent_layer = NULL;
        popup->parent_xdg = NULL;
    }
}

static void layer_free(purp_layer_surface_t *layer) {
    if (!layer) return;
    purp_server_t *server = layer->server;
    bool was_mapped = purp_layer_is_mapped(layer);

    layer->mapped = false;
    layer_detach_popups(layer);
    if (server) {
        if (server->layer_focus == layer) server->layer_focus = NULL;
        if (was_mapped) purp_host_damage_monitor(server, (size_t)layer->monitor);
    }
    if (layer->surface && layer->surface->role_data == layer)
        purp_surface_clear_role_data(layer->surface);
    wl_list_remove(&layer->link);
    wl_list_init(&layer->link);

    if (server) {
        purp_input_sync_keyboard_focus(server);
        purp_input_sync_pointer_focus(server, 0);
    }
    free(layer->namespace_name);
    free(layer);
}

static void layer_resource_destroy(struct wl_resource *resource) {
    purp_layer_surface_t *layer = wl_resource_get_user_data(resource);
    if (!layer) return;
    layer->resource = NULL;
    layer_free(layer);
}

static void layer_role_destroy(purp_surface_t *surface) {
    purp_layer_surface_t *layer = surface ? surface->role_data : NULL;
    if (!layer) return;
    if (layer->resource) {
        wl_resource_destroy(layer->resource);
    } else {
        layer_free(layer);
    }
}

static void layer_role_commit(purp_surface_t *surface) {
    purp_layer_surface_t *layer = surface ? surface->role_data : NULL;
    if (!layer || layer->closed) return;
    purp_server_t *server = layer->server;
    bool initial = !layer->initial_commit_done;

    layer->initial_commit_done = true;
    layer->current = layer->pending;
    layer->geo = purp_layer_compute_geometry(&layer->current, layer_monitor_rect(server, layer));

    if (initial) {
        layer->mapped = false;
        layer_send_configure(layer);
        purp_server_update_surface_outputs(server);
        return;
    }

    layer->mapped = layer->configured && surface->mapped;
    if (!layer->configured || layer->configured_size.w != layer->geo.w ||
        layer->configured_size.h != layer->geo.h) {
        layer_send_configure(layer);
    }

    if (server->layer_focus == layer &&
        (!layer->mapped ||
         layer->current.keyboard_interactivity !=
             ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_ON_DEMAND)) {
        server->layer_focus = NULL;
    }
    purp_input_sync_keyboard_focus(server);
    purp_input_sync_pointer_focus(server, 0);
    purp_host_damage_monitor(server, (size_t)layer->monitor);
}

static const purp_surface_role_t layer_surface_role = {
    .name = "zwlr_layer_surface_v1",
    .kind = PURP_ROLE_LAYER,
    .commit = layer_role_commit,
    .destroy = layer_role_destroy,
};

static purp_layer_surface_t *layer_from_resource(struct wl_resource *resource) {
    return resource ? wl_resource_get_user_data(resource) : NULL;
}

static void layer_surface_set_size(struct wl_client *client, struct wl_resource *resource,
                                   uint32_t width, uint32_t height) {
    purp_layer_surface_t *layer = layer_from_resource(resource);
    if (!layer || layer->closed) return;
    if (width > INT32_MAX || height > INT32_MAX) {
        wl_resource_post_error(resource, ZWLR_LAYER_SURFACE_V1_ERROR_INVALID_SIZE,
                               "layer surface dimensions must fit in signed 32-bit coordinates");
        return;
    }
    layer->pending.desired_width = width;
    layer->pending.desired_height = height;
}

static void layer_surface_set_anchor(struct wl_client *client, struct wl_resource *resource,
                                     uint32_t anchor) {
    purp_layer_surface_t *layer = layer_from_resource(resource);
    if (!layer || layer->closed) return;
    const uint32_t valid = ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
                           ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
                           ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
                           ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT;
    if ((anchor & ~valid) != 0) {
        wl_resource_post_error(resource, ZWLR_LAYER_SURFACE_V1_ERROR_INVALID_ANCHOR,
                               "invalid layer surface anchor bits");
        return;
    }
    layer->pending.anchor = anchor;
}

static void layer_surface_set_exclusive_zone(struct wl_client *client,
                                             struct wl_resource *resource, int32_t zone) {
    purp_layer_surface_t *layer = layer_from_resource(resource);
    if (!layer || layer->closed) return;
    layer->pending.exclusive_zone = zone;
}

static void layer_surface_set_margin(struct wl_client *client, struct wl_resource *resource,
                                     int32_t top, int32_t right, int32_t bottom, int32_t left) {
    purp_layer_surface_t *layer = layer_from_resource(resource);
    if (!layer || layer->closed) return;
    layer->pending.margin_top = top;
    layer->pending.margin_right = right;
    layer->pending.margin_bottom = bottom;
    layer->pending.margin_left = left;
}

static void layer_surface_set_keyboard_interactivity(struct wl_client *client,
                                                     struct wl_resource *resource,
                                                     uint32_t keyboard_interactivity) {
    purp_layer_surface_t *layer = layer_from_resource(resource);
    if (!layer || layer->closed) return;
    if (!zwlr_layer_surface_v1_keyboard_interactivity_is_valid(
            keyboard_interactivity, (uint32_t)wl_resource_get_version(resource))) {
        wl_resource_post_error(resource,
                               ZWLR_LAYER_SURFACE_V1_ERROR_INVALID_KEYBOARD_INTERACTIVITY,
                               "invalid layer surface keyboard interactivity");
        return;
    }
    layer->pending.keyboard_interactivity = keyboard_interactivity;
}

static void layer_surface_get_popup(struct wl_client *client, struct wl_resource *resource,
                                    struct wl_resource *popup_resource) {
    purp_layer_surface_t *layer = layer_from_resource(resource);
    purp_popup_t *popup = popup_resource ? wl_resource_get_user_data(popup_resource) : NULL;
    if (!layer || !popup || layer->closed) return;

    bool already_linked = popup->parent_layer == layer && popup->parent_xdg == NULL;
    if (!already_linked) {
        if (popup->parent_layer || popup->parent_xdg) wl_list_remove(&popup->parent_link);
        wl_list_insert(layer->popups.prev, &popup->parent_link);
    }
    popup->parent = layer->surface;
    popup->parent_layer = layer;
    popup->parent_xdg = NULL;
    layer_unconstrain_popup(layer, popup);
}

static void layer_surface_ack_configure(struct wl_client *client, struct wl_resource *resource,
                                        uint32_t serial) {
    (void)serial;
}

static void layer_surface_destroy(struct wl_client *client, struct wl_resource *resource) {
    wl_resource_destroy(resource);
}

static void layer_surface_set_layer(struct wl_client *client, struct wl_resource *resource,
                                    uint32_t level) {
    purp_layer_surface_t *layer = layer_from_resource(resource);
    if (!layer || layer->closed) return;
    if (level > ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY) {
        wl_resource_post_error(resource, ZWLR_LAYER_SHELL_V1_ERROR_INVALID_LAYER,
                               "invalid layer surface layer");
        return;
    }
    layer->pending.layer = level;
}

static void layer_surface_set_exclusive_edge(struct wl_client *client,
                                             struct wl_resource *resource, uint32_t edge) {
    purp_layer_surface_t *layer = layer_from_resource(resource);
    if (!layer || layer->closed) return;
    const uint32_t valid = ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
                           ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
                           ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
                           ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT;
    if (edge == 0 || (edge & ~valid) != 0 || (edge & (edge - 1)) != 0 ||
        (layer->pending.anchor & edge) == 0) {
        wl_resource_post_error(resource, ZWLR_LAYER_SURFACE_V1_ERROR_INVALID_EXCLUSIVE_EDGE,
                               "exclusive edge must be one anchored edge");
        return;
    }
    layer->pending.exclusive_edge = edge;
}

static const struct zwlr_layer_surface_v1_interface layer_surface_implementation = {
    .set_size = layer_surface_set_size,
    .set_anchor = layer_surface_set_anchor,
    .set_exclusive_zone = layer_surface_set_exclusive_zone,
    .set_margin = layer_surface_set_margin,
    .set_keyboard_interactivity = layer_surface_set_keyboard_interactivity,
    .get_popup = layer_surface_get_popup,
    .ack_configure = layer_surface_ack_configure,
    .destroy = layer_surface_destroy,
    .set_layer = layer_surface_set_layer,
    .set_exclusive_edge = layer_surface_set_exclusive_edge,
};

static int layer_initial_monitor(purp_server_t *server, struct wl_resource *output) {
    if (!server || server->wm.monitor_count == 0) return -1;
    int monitor = output ? purp_output_monitor_from_resource(output)
                         : (int)purp_wm_active_monitor_index(&server->wm);
    if (monitor < 0 || (size_t)monitor >= server->wm.monitor_count ||
        !server->wm.monitors[monitor].enabled) {
        monitor = (int)purp_wm_primary_monitor_index(&server->wm);
    }
    return monitor;
}

static void layer_shell_get_layer_surface(struct wl_client *client, struct wl_resource *resource,
                                          uint32_t id, struct wl_resource *surface_resource,
                                          struct wl_resource *output, uint32_t level,
                                          const char *namespace_name) {
    purp_server_t *server = wl_resource_get_user_data(resource);
    purp_surface_t *surface = purp_surface_from_resource(surface_resource);
    if (!surface) {
        wl_resource_post_error(resource, ZWLR_LAYER_SHELL_V1_ERROR_ROLE,
                               "wl_surface is not a compositor surface");
        return;
    }
    if (level > ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY) {
        wl_resource_post_error(resource, ZWLR_LAYER_SHELL_V1_ERROR_INVALID_LAYER,
                               "invalid layer surface layer");
        return;
    }
    if (purp_surface_has_buffer_history(surface) || surface->mapped ||
        surface->pending.buffer || surface->current.buffer) {
        wl_resource_post_error(resource, ZWLR_LAYER_SHELL_V1_ERROR_ALREADY_CONSTRUCTED,
                               "wl_surface already has attached buffer contents");
        return;
    }

    purp_layer_surface_t *layer = calloc(1, sizeof(*layer));
    struct wl_resource *layer_resource =
        wl_resource_create(client, &zwlr_layer_surface_v1_interface,
                           wl_resource_get_version(resource), id);
    if (!layer || !layer_resource) {
        free(layer);
        if (layer_resource) wl_resource_destroy(layer_resource);
        wl_client_post_no_memory(client);
        return;
    }
    layer->namespace_name = strdup(namespace_name ? namespace_name : "");
    if (!layer->namespace_name) {
        wl_resource_destroy(layer_resource);
        free(layer);
        wl_client_post_no_memory(client);
        return;
    }

    layer->server = server;
    layer->resource = layer_resource;
    layer->surface = surface;
    layer->monitor = layer_initial_monitor(server, output);
    layer->explicit_output = output != NULL;
    layer->pending.layer = level;
    layer->current.layer = level;
    wl_list_init(&layer->popups);
    wl_list_init(&layer->link);

    if (!purp_surface_set_role(surface, &layer_surface_role, layer, resource,
                               ZWLR_LAYER_SHELL_V1_ERROR_ROLE)) {
        wl_resource_destroy(layer_resource);
        free(layer->namespace_name);
        free(layer);
        return;
    }

    layer->geo = purp_layer_compute_geometry(&layer->current, layer_monitor_rect(server, layer));
    wl_resource_set_implementation(layer_resource, &layer_surface_implementation, layer,
                                   layer_resource_destroy);
    wl_list_insert(server->layers.prev, &layer->link);
}

static void layer_shell_destroy(struct wl_client *client, struct wl_resource *resource) {
    wl_resource_destroy(resource);
}

static const struct zwlr_layer_shell_v1_interface layer_shell_implementation = {
    .get_layer_surface = layer_shell_get_layer_surface,
    .destroy = layer_shell_destroy,
};

static void layer_shell_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
    purp_server_t *server = data;
    struct wl_resource *resource =
        wl_resource_create(client, &zwlr_layer_shell_v1_interface, version, id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &layer_shell_implementation, server, NULL);
}

bool purp_layer_init(purp_server_t *server) {
    if (!server || !server->display) return false;
    server->layer_shell_global =
        wl_global_create(server->display, &zwlr_layer_shell_v1_interface,
                         PURP_LAYER_SHELL_VERSION, server, layer_shell_bind);
    return server->layer_shell_global != NULL;
}

void purp_layer_arrange_all(purp_server_t *server) {
    if (!server) return;
    purp_layer_surface_t *layer;
    wl_list_for_each(layer, &server->layers, link) {
        if (layer->closed) continue;
        layer->geo = purp_layer_compute_geometry(&layer->current, layer_monitor_rect(server, layer));
        if (layer->initial_commit_done &&
            (!layer->configured || layer->configured_size.w != layer->geo.w ||
             layer->configured_size.h != layer->geo.h)) {
            layer_send_configure(layer);
        }
    }
}

bool purp_layer_is_mapped(const purp_layer_surface_t *layer) {
    return layer && layer->mapped && !layer->closed;
}

bool purp_layer_is_above_windows(const purp_layer_surface_t *layer) {
    return layer && (layer->current.layer == ZWLR_LAYER_SHELL_V1_LAYER_TOP ||
                     layer->current.layer == ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY);
}

bool purp_layer_wants_keyboard(const purp_layer_surface_t *layer) {
    return layer && layer->current.keyboard_interactivity ==
                        ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE;
}

bool purp_layer_is_keepalive(const purp_layer_surface_t *layer) {
    return layer && layer->namespace_name &&
           strcmp(layer->namespace_name, PURP_SPOTLIGHT_KEEPALIVE_NS) == 0;
}

purp_layer_surface_t *purp_layer_at(purp_server_t *server, purp_pointf_t pt,
                                    purp_pointf_t *origin) {
    if (!server) return NULL;
    static const uint32_t levels[] = {
        ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY,
        ZWLR_LAYER_SHELL_V1_LAYER_TOP,
    };

    for (size_t i = 0; i < PURP_ARRAY_LEN(levels); ++i) {
        purp_layer_surface_t *layer;
        wl_list_for_each_reverse(layer, &server->layers, link) {
            if (!purp_layer_is_mapped(layer) || purp_layer_is_keepalive(layer) ||
                layer->current.layer != levels[i] || !layer->surface) {
                continue;
            }
            double sx;
            double sy;
            if (!purp_surface_at(layer->surface, pt.x - layer->geo.x, pt.y - layer->geo.y,
                                 &sx, &sy)) {
                continue;
            }
            if (origin) *origin = (purp_pointf_t){layer->geo.x, layer->geo.y};
            return layer;
        }
    }
    return NULL;
}

purp_layer_surface_t *purp_layer_keyboard_target(purp_server_t *server) {
    if (!server) return NULL;
    purp_layer_surface_t *layer;
    wl_list_for_each_reverse(layer, &server->layers, link) {
        if (purp_layer_is_mapped(layer) && purp_layer_wants_keyboard(layer)) return layer;
    }
    if (server->layer_focus && purp_layer_is_mapped(server->layer_focus))
        return server->layer_focus;
    return NULL;
}

void purp_layer_monitors_changed(purp_server_t *server, const int *old_to_new) {
    if (!server) return;
    size_t primary = server->wm.monitor_count > 0 ? purp_wm_primary_monitor_index(&server->wm) : 0;
    purp_layer_surface_t *layer;

    wl_list_for_each(layer, &server->layers, link) {
        if (layer->closed) continue;
        if (old_to_new) {
            int old = layer->monitor;
            int target = old >= 0 && old < PURP_MAX_MONITORS ? old_to_new[old] : -1;
            if (target < 0) {
                if (layer->explicit_output) {
                    layer->closed = true;
                    layer->mapped = false;
                    if (server->layer_focus == layer) server->layer_focus = NULL;
                    if (layer->resource) zwlr_layer_surface_v1_send_closed(layer->resource);
                    continue;
                }
                layer->monitor = (int)primary;
            } else {
                layer->monitor = target;
            }
        }

        if (layer->monitor < 0 || (size_t)layer->monitor >= server->wm.monitor_count ||
            !server->wm.monitors[layer->monitor].enabled) {
            layer->monitor = (int)primary;
        }
    }
}
