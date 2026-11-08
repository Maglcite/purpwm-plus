/*
 * purpwm+ — surfaces and buffers (src/surface.c).
 *
 * wl_compositor v6 (wl_surface double-buffered state, regions, frame callbacks, enter/leave and
 * preferred buffer scale), wl_subcompositor v1 (synchronized/desynchronized caches, stacking,
 * positions), wl_shm (+ ABGR8888/XBGR8888), wp_single_pixel_buffer_manager_v1, wp_viewporter,
 * wp_fractional_scale_manager_v1 and zwp_linux_dmabuf_v1 v4 with default/surface feedback.
 *
 * Buffer locking: wl_surface.attach locks the buffer into the pending state, commits transfer
 * that lock into the current state (marking the buffer committed), and the last unlock of a
 * committed buffer sends wl_buffer.release. render.c may drop current.buffer right after an
 * upload (shm); mapping and sizes therefore never depend on current.buffer after the commit.
 */
#include "purpwm.h"
#include "render.h"

#include <errno.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <drm_fourcc.h>

struct surface_storage {
    purp_surface_t base;
    bool buffer_attached;
};

bool purp_surface_has_buffer_history(const purp_surface_t *surface);

bool purp_surface_has_buffer_history(const purp_surface_t *surface) {
    const struct surface_storage *storage = (const struct surface_storage *)surface;
    return storage && storage->buffer_attached;
}

static int32_t surface_clamp_i32(int64_t value) {
    if (value > INT32_MAX) return INT32_MAX;
    if (value < INT32_MIN) return INT32_MIN;
    return (int32_t)value;
}

/* =========================================================================================== */
/* Regions                                                                                    */
/* =========================================================================================== */

/* Coordinates are clamped to +-2^29 so every x + w / y + h stays inside int32. */
#define REGION_COORD_LIMIT (1 << 29)

typedef struct rect_vec {
    purp_rect_t *data;
    size_t len;
    size_t cap;
} rect_vec_t;

static bool rect_vec_push(rect_vec_t *vec, purp_rect_t rect) {
    if (vec->len == vec->cap) {
        size_t cap = vec->cap ? vec->cap * 2 : 8;
        purp_rect_t *data = realloc(vec->data, cap * sizeof(*data));
        if (!data) return false;
        vec->data = data;
        vec->cap = cap;
    }
    vec->data[vec->len++] = rect;
    return true;
}

static int32_t clamp_coord(int64_t v) {
    if (v < -REGION_COORD_LIMIT) return -REGION_COORD_LIMIT;
    if (v > REGION_COORD_LIMIT) return REGION_COORD_LIMIT;
    return (int32_t)v;
}

static purp_rect_t region_sanitize(purp_rect_t r) {
    int32_t x0 = clamp_coord(r.x), y0 = clamp_coord(r.y);
    int32_t x1 = clamp_coord((int64_t)r.x + r.w), y1 = clamp_coord((int64_t)r.y + r.h);
    return purp_rect_make(x0, y0, x1 - x0, y1 - y0);
}

static bool region_reserve(purp_region_t *region, size_t want) {
    if (want <= region->cap) return true;
    size_t cap = region->cap ? region->cap : 4;
    while (cap < want) cap *= 2;
    purp_rect_t *rects = realloc(region->rects, cap * sizeof(*rects));
    if (!rects) return false;
    region->rects = rects;
    region->cap = cap;
    return true;
}

/* `a` minus the overlapping `b` as up to four disjoint rectangles. */
static size_t rect_subtract(purp_rect_t a, purp_rect_t b, purp_rect_t out[4]) {
    size_t n = 0;
    int32_t ax1 = a.x + a.w, ay1 = a.y + a.h, bx1 = b.x + b.w, by1 = b.y + b.h;
    if (b.y > a.y) out[n++] = purp_rect_make(a.x, a.y, a.w, b.y - a.y);
    if (by1 < ay1) out[n++] = purp_rect_make(a.x, by1, a.w, ay1 - by1);
    int32_t my0 = purp_max_i32(a.y, b.y), my1 = purp_min_i32(ay1, by1);
    if (my1 > my0) {
        if (b.x > a.x) out[n++] = purp_rect_make(a.x, my0, b.x - a.x, my1 - my0);
        if (bx1 < ax1) out[n++] = purp_rect_make(bx1, my0, ax1 - bx1, my1 - my0);
    }
    return n;
}

void purp_region_init(purp_region_t *region) {
    if (region) memset(region, 0, sizeof(*region));
}

void purp_region_finish(purp_region_t *region) {
    if (!region) return;
    free(region->rects);
    memset(region, 0, sizeof(*region));
}

void purp_region_clear(purp_region_t *region) {
    if (region) region->count = 0;
}

bool purp_region_copy(purp_region_t *dst, const purp_region_t *src) {
    if (!dst) return false;
    if (dst == src) return true;
    if (!src || src->count == 0) {
        dst->count = 0;
        return true;
    }
    if (!region_reserve(dst, src->count)) return false;
    memcpy(dst->rects, src->rects, src->count * sizeof(*src->rects));
    dst->count = src->count;
    return true;
}

bool purp_region_add(purp_region_t *region, purp_rect_t rect) {
    if (!region) return false;
    rect = region_sanitize(rect);
    if (purp_rect_is_empty(rect)) return true;
    /* Keep the list disjoint: only the parts of `rect` not covered yet are appended. */
    rect_vec_t pieces = { 0 }, next = { 0 };
    bool ok = rect_vec_push(&pieces, rect);
    for (size_t i = 0; ok && i < region->count && pieces.len > 0; ++i) {
        purp_rect_t existing = region->rects[i];
        next.len = 0;
        for (size_t j = 0; ok && j < pieces.len; ++j) {
            purp_rect_t piece = pieces.data[j];
            if (!purp_rect_overlaps(piece, existing)) {
                ok = rect_vec_push(&next, piece);
                continue;
            }
            purp_rect_t parts[4];
            size_t n = rect_subtract(piece, existing, parts);
            for (size_t k = 0; ok && k < n; ++k) ok = rect_vec_push(&next, parts[k]);
        }
        rect_vec_t swap = pieces;
        pieces = next;
        next = swap;
    }
    if (ok && pieces.len > 0) {
        ok = region_reserve(region, region->count + pieces.len);
        if (ok) {
            memcpy(region->rects + region->count, pieces.data, pieces.len * sizeof(*pieces.data));
            region->count += pieces.len;
        }
    }
    free(pieces.data);
    free(next.data);
    return ok;
}

void purp_region_subtract(purp_region_t *region, purp_rect_t rect) {
    if (!region || region->count == 0) return;
    rect = region_sanitize(rect);
    if (purp_rect_is_empty(rect)) return;
    rect_vec_t out = { 0 };
    bool ok = true;
    for (size_t i = 0; ok && i < region->count; ++i) {
        purp_rect_t r = region->rects[i];
        if (!purp_rect_overlaps(r, rect)) {
            ok = rect_vec_push(&out, r);
            continue;
        }
        purp_rect_t parts[4];
        size_t n = rect_subtract(r, rect, parts);
        for (size_t k = 0; ok && k < n; ++k) ok = rect_vec_push(&out, parts[k]);
    }
    if (!ok) {
        /* Out of memory: leave the region unchanged rather than half-subtracted. */
        free(out.data);
        return;
    }
    free(region->rects);
    region->rects = out.data;
    region->count = out.len;
    region->cap = out.cap;
}

void purp_region_intersect_rect(purp_region_t *region, purp_rect_t rect) {
    if (!region) return;
    size_t kept = 0;
    for (size_t i = 0; i < region->count; ++i) {
        purp_rect_t out;
        if (purp_rect_intersect(region->rects[i], rect, &out)) region->rects[kept++] = out;
    }
    region->count = kept;
}

void purp_region_translate(purp_region_t *region, int32_t dx, int32_t dy) {
    if (!region) return;
    for (size_t i = 0; i < region->count; ++i)
        region->rects[i] = region_sanitize(purp_rect_translate(region->rects[i], dx, dy));
}

bool purp_region_contains(const purp_region_t *region, int32_t x, int32_t y) {
    if (!region) return false;
    for (size_t i = 0; i < region->count; ++i)
        if (purp_rect_contains(region->rects[i], x, y)) return true;
    return false;
}

bool purp_region_is_empty(const purp_region_t *region) {
    return !region || region->count == 0;
}

purp_rect_t purp_region_extents(const purp_region_t *region) {
    purp_rect_t extents = purp_rect_make(0, 0, 0, 0);
    if (!region) return extents;
    for (size_t i = 0; i < region->count; ++i) extents = purp_rect_union(extents, region->rects[i]);
    return extents;
}

void purp_region_collapse(purp_region_t *region) {
    if (!region || region->count <= 1) return;
    region->rects[0] = purp_region_extents(region);
    region->count = 1;
}

/* Damage accumulation: exact up to PURP_REGION_MAX_RECTS, extents beyond (or on OOM). */
static void damage_add(purp_region_t *damage, purp_rect_t rect) {
    if (!purp_region_add(damage, rect)) {
        purp_rect_t extents = purp_rect_union(purp_region_extents(damage), region_sanitize(rect));
        if (damage->cap == 0 && !region_reserve(damage, 1)) return;
        damage->rects[0] = extents;
        damage->count = purp_rect_is_empty(extents) ? 0 : 1;
        return;
    }
    if (damage->count > PURP_REGION_MAX_RECTS) purp_region_collapse(damage);
}

static void damage_merge(purp_region_t *dst, const purp_region_t *src) {
    for (size_t i = 0; i < src->count; ++i) damage_add(dst, src->rects[i]);
}

/* dst takes src's contents; src ends up empty (keeping dst's old storage for reuse). */
static void region_take(purp_region_t *dst, purp_region_t *src) {
    purp_region_t tmp = *dst;
    *dst = *src;
    *src = tmp;
    src->count = 0;
}

/* =========================================================================================== */
/* Buffers                                                                                    */
/* =========================================================================================== */

static bool drm_format_has_alpha(uint32_t format) {
    switch (format) {
    case DRM_FORMAT_ARGB8888:
    case DRM_FORMAT_ABGR8888:
    case DRM_FORMAT_RGBA8888:
    case DRM_FORMAT_BGRA8888:
    case DRM_FORMAT_ARGB2101010:
    case DRM_FORMAT_ABGR2101010:
    case DRM_FORMAT_RGBA1010102:
    case DRM_FORMAT_BGRA1010102:
    case DRM_FORMAT_ARGB4444:
    case DRM_FORMAT_ABGR4444:
    case DRM_FORMAT_RGBA4444:
    case DRM_FORMAT_BGRA4444:
    case DRM_FORMAT_ARGB1555:
    case DRM_FORMAT_ABGR1555:
    case DRM_FORMAT_RGBA5551:
    case DRM_FORMAT_BGRA5551:
    case DRM_FORMAT_ARGB16161616F:
    case DRM_FORMAT_ABGR16161616F:
    case DRM_FORMAT_ARGB16161616:
    case DRM_FORMAT_ABGR16161616:
        return true;
    default:
        return false;
    }
}

static bool shm_format_has_alpha(uint32_t format) {
    if (format == WL_SHM_FORMAT_ARGB8888) return true;
    if (format == WL_SHM_FORMAT_XRGB8888) return false;
    return drm_format_has_alpha(format); /* other wl_shm codes are DRM fourccs */
}

static void buffer_free(purp_buffer_t *buffer) {
    if (buffer->server && buffer->server->renderer)
        purp_render_buffer_destroyed(buffer->server->renderer, buffer);
    for (int i = 0; i < PURP_DMABUF_MAX_PLANES; ++i)
        if (buffer->dmabuf.fd[i] >= 0) close(buffer->dmabuf.fd[i]);
    free(buffer);
}

static void buffer_handle_resource_destroy(struct wl_listener *listener, void *data) {
    purp_buffer_t *buffer = PURP_CONTAINER_OF(listener, purp_buffer_t, resource_destroy);
    wl_list_remove(&buffer->resource_destroy.link);
    wl_list_init(&buffer->resource_destroy.link);
    buffer->resource = NULL;
    buffer->release_pending = false;
    if (buffer->lock_count == 0) buffer_free(buffer);
}

static purp_buffer_t *buffer_alloc(purp_server_t *server, purp_buffer_kind_t kind) {
    purp_buffer_t *buffer = calloc(1, sizeof(*buffer));
    if (!buffer) return NULL;
    buffer->server = server;
    buffer->kind = kind;
    for (int i = 0; i < PURP_DMABUF_MAX_PLANES; ++i) buffer->dmabuf.fd[i] = -1;
    wl_list_init(&buffer->resource_destroy.link);
    return buffer;
}

static void buffer_bind_resource(purp_buffer_t *buffer, struct wl_resource *resource) {
    buffer->resource = resource;
    buffer->resource_destroy.notify = buffer_handle_resource_destroy;
    wl_resource_add_destroy_listener(resource, &buffer->resource_destroy);
}

/* wl_buffer implementation for buffers created here (dmabuf, single-pixel, inert). */
static void buffer_handle_destroy(struct wl_client *client, struct wl_resource *resource) {
    wl_resource_destroy(resource);
}

static const struct wl_buffer_interface buffer_impl = {
    .destroy = buffer_handle_destroy,
};

purp_buffer_t *purp_buffer_from_resource(purp_server_t *server, struct wl_resource *resource) {
    if (!resource) return NULL;
    struct wl_listener *listener =
        wl_resource_get_destroy_listener(resource, buffer_handle_resource_destroy);
    if (listener) return PURP_CONTAINER_OF(listener, purp_buffer_t, resource_destroy);
    struct wl_shm_buffer *shm = wl_shm_buffer_get(resource);
    if (!shm) return NULL;
    purp_buffer_t *buffer = buffer_alloc(server, PURP_BUFFER_SHM);
    if (!buffer) return NULL;
    buffer->width = wl_shm_buffer_get_width(shm);
    buffer->height = wl_shm_buffer_get_height(shm);
    buffer->format = wl_shm_buffer_get_format(shm);
    buffer->has_alpha = shm_format_has_alpha(buffer->format);
    buffer_bind_resource(buffer, resource);
    return buffer;
}

void purp_buffer_lock(purp_buffer_t *buffer) {
    if (buffer) buffer->lock_count++;
}

void purp_buffer_unlock(purp_buffer_t *buffer) {
    if (!buffer) return;
    if (buffer->lock_count > 0) buffer->lock_count--;
    if (buffer->lock_count > 0) return;
    if (buffer->resource) {
        if (buffer->release_pending) wl_buffer_send_release(buffer->resource);
        buffer->release_pending = false;
        return;
    }
    buffer_free(buffer);
}

/* =========================================================================================== */
/* Surface state                                                                              */
/* =========================================================================================== */

static void state_init(purp_surface_state_t *state) {
    memset(state, 0, sizeof(*state));
    state->buffer_scale = 1;
    state->buffer_transform = WL_OUTPUT_TRANSFORM_NORMAL;
    state->input_infinite = true;
    wl_list_init(&state->frame_callbacks);
}

static void state_finish(purp_surface_state_t *state) {
    if (state->buffer) purp_buffer_unlock(state->buffer);
    state->buffer = NULL;
    purp_region_finish(&state->surface_damage);
    purp_region_finish(&state->buffer_damage);
    purp_region_finish(&state->opaque);
    purp_region_finish(&state->input);
    struct wl_resource *callback, *tmp;
    wl_resource_for_each_safe(callback, tmp, &state->frame_callbacks) wl_resource_destroy(callback);
    wl_list_init(&state->frame_callbacks);
}

/* Fold `src` (pending) into `dst` (a synchronized subsurface cache); src is left empty. */
static void state_merge(purp_surface_state_t *dst, purp_surface_state_t *src) {
    uint32_t bits = src->committed;
    if (bits & PURP_SURFACE_STATE_BUFFER) {
        if (dst->buffer) purp_buffer_unlock(dst->buffer);
        dst->buffer = src->buffer; /* the pending lock moves along */
        if (dst->buffer) dst->buffer->release_pending = true;
        src->buffer = NULL;
    }
    if (bits & PURP_SURFACE_STATE_OFFSET) {
        if (dst->committed & PURP_SURFACE_STATE_OFFSET) {
            dst->dx = surface_clamp_i32((int64_t)dst->dx + src->dx);
            dst->dy = surface_clamp_i32((int64_t)dst->dy + src->dy);
        } else {
            dst->dx = src->dx;
            dst->dy = src->dy;
        }
    }
    src->dx = src->dy = 0;
    if (bits & PURP_SURFACE_STATE_SCALE) dst->buffer_scale = src->buffer_scale;
    if (bits & PURP_SURFACE_STATE_TRANSFORM) dst->buffer_transform = src->buffer_transform;
    if (bits & PURP_SURFACE_STATE_SURFACE_DAMAGE) damage_merge(&dst->surface_damage, &src->surface_damage);
    if (bits & PURP_SURFACE_STATE_BUFFER_DAMAGE) damage_merge(&dst->buffer_damage, &src->buffer_damage);
    purp_region_clear(&src->surface_damage);
    purp_region_clear(&src->buffer_damage);
    if (bits & PURP_SURFACE_STATE_OPAQUE_REGION) region_take(&dst->opaque, &src->opaque);
    if (bits & PURP_SURFACE_STATE_INPUT_REGION) {
        region_take(&dst->input, &src->input);
        dst->input_infinite = src->input_infinite;
    }
    if (bits & PURP_SURFACE_STATE_VIEWPORT) {
        dst->viewport_has_src = src->viewport_has_src;
        dst->viewport_src = src->viewport_src;
        dst->viewport_has_dst = src->viewport_has_dst;
        dst->viewport_dst = src->viewport_dst;
    }
    wl_list_insert_list(dst->frame_callbacks.prev, &src->frame_callbacks);
    wl_list_init(&src->frame_callbacks);
    dst->committed |= bits;
    src->committed = 0;
}

/* =========================================================================================== */
/* Surface helpers                                                                            */
/* =========================================================================================== */

static const struct wl_surface_interface surface_impl;
static const purp_surface_role_t subsurface_role;

purp_surface_t *purp_surface_from_resource(struct wl_resource *resource) {
    if (!resource || !wl_resource_instance_of(resource, &wl_surface_interface, &surface_impl))
        return NULL;
    return wl_resource_get_user_data(resource);
}

purp_surface_t *purp_surface_root(purp_surface_t *surface) {
    for (int depth = 0; surface && surface->subsurface && surface->subsurface->parent &&
                        depth < 1024;
         ++depth)
        surface = surface->subsurface->parent;
    return surface;
}

bool purp_surface_is_mapped(const purp_surface_t *surface) {
    if (!surface) return false;
    if (surface->mapped) return true;
    const purp_subsurface_t *sub;
    wl_list_for_each(sub, &surface->subsurfaces_below, link)
        if (purp_surface_is_mapped(sub->surface)) return true;
    wl_list_for_each(sub, &surface->subsurfaces_above, link)
        if (purp_surface_is_mapped(sub->surface)) return true;
    return false;
}

bool purp_surface_set_role(purp_surface_t *surface, const purp_surface_role_t *role,
                           void *role_data, struct wl_resource *error_resource,
                           uint32_t error_code) {
    if (!surface || !role) return false;
    if (surface->role && surface->role != role && surface->role->kind != role->kind) {
        if (error_resource)
            wl_resource_post_error(error_resource, error_code,
                                   "wl_surface@%u already has another role (%s)",
                                   wl_resource_get_id(surface->resource), surface->role->name);
        return false;
    }
    if (surface->role && surface->role_data) {
        if (error_resource)
            wl_resource_post_error(error_resource, error_code,
                                   "wl_surface@%u already has an active %s role object",
                                   wl_resource_get_id(surface->resource), surface->role->name);
        return false;
    }
    surface->role = role;
    surface->role_data = role_data;
    return true;
}

void purp_surface_clear_role_data(purp_surface_t *surface) {
    if (surface) surface->role_data = NULL;
}

static purp_size_t surface_compute_size(const purp_surface_t *surface) {
    purp_size_t size = { 0, 0 };
    if (!surface->mapped) return size;
    const purp_surface_state_t *cur = &surface->current;
    if (cur->viewport_has_dst) return cur->viewport_dst;
    if (cur->viewport_has_src) {
        size.w = (int32_t)cur->viewport_src.w;
        size.h = (int32_t)cur->viewport_src.h;
        return size;
    }
    int32_t w = surface->buffer_size.w, h = surface->buffer_size.h;
    if (cur->buffer_transform & 1) {
        int32_t t = w;
        w = h;
        h = t;
    }
    int32_t scale = cur->buffer_scale > 0 ? cur->buffer_scale : 1;
    size.w = w / scale;
    size.h = h / scale;
    return size;
}

/* Monitor owning a surface tree root (window, layer or popup), else the active monitor; -1 when
 * no monitor is usable. */
static int surface_owner_monitor(purp_server_t *server, purp_surface_t *root) {
    const purp_wm_t *wm = &server->wm;
    if (wm->monitor_count == 0) return -1;
    int mon = -1;
    if (root && root->role) {
        switch (root->role->kind) {
        case PURP_ROLE_XDG_TOPLEVEL: {
            purp_window_t *win = purp_xdg_window_for_surface(server, root);
            if (win) mon = win->monitor;
            break;
        }
        case PURP_ROLE_XDG_POPUP: {
            purp_xdg_surface_t *xdg = purp_xdg_surface_from_surface(root);
            if (xdg && xdg->role == PURP_XDG_ROLE_POPUP && xdg->popup)
                mon = purp_xdg_popup_monitor(xdg->popup);
            break;
        }
        case PURP_ROLE_LAYER: {
            purp_layer_surface_t *layer = root->role_data;
            if (layer) mon = layer->monitor;
            break;
        }
        default:
            break;
        }
    }
    if (mon < 0 || (size_t)mon >= wm->monitor_count || !wm->monitors[mon].enabled)
        mon = (int)purp_wm_active_monitor_index(wm);
    if ((size_t)mon >= wm->monitor_count || mon >= 32 || !wm->monitors[mon].enabled) return -1;
    return mon;
}

static void damage_pointer_area(purp_server_t *server, purp_size_t size) {
    purp_pointf_t p = server->wm.pointer_location;
    int64_t margin = PURP_RENDER_CURSOR_MARGIN;
    purp_rect_t area = purp_rect_make(
        surface_clamp_i32((int64_t)purp_round_i32(p.x) - margin),
        surface_clamp_i32((int64_t)purp_round_i32(p.y) - margin),
        surface_clamp_i32(2 * margin + purp_max_i32(size.w, 0)),
        surface_clamp_i32(2 * margin + purp_max_i32(size.h, 0)));
    purp_server_damage_rect(server, area);
}

/* Damage the monitors showing the tree rooted at `root`. */
static void surface_damage_root(purp_server_t *server, purp_surface_t *root) {
    if (!root || !root->role) return;
    switch (root->role->kind) {
    case PURP_ROLE_XDG_TOPLEVEL: {
        purp_window_t *win = purp_xdg_window_for_surface(server, root);
        if (win && purp_wm_is_window_visible(&server->wm, win))
            purp_server_damage_rect(server, purp_rect_inflate(win->geo, PURP_BORDER_WIDTH));
        break;
    }
    case PURP_ROLE_XDG_POPUP: {
        purp_xdg_surface_t *xdg = purp_xdg_surface_from_surface(root);
        if (xdg && xdg->role == PURP_XDG_ROLE_POPUP && xdg->popup) {
            int mon = purp_xdg_popup_monitor(xdg->popup);
            if (mon >= 0) purp_host_damage_monitor(server, (size_t)mon);
        }
        break;
    }
    case PURP_ROLE_LAYER: {
        purp_layer_surface_t *layer = root->role_data;
        if (layer && layer->monitor >= 0) purp_host_damage_monitor(server, (size_t)layer->monitor);
        break;
    }
    case PURP_ROLE_CURSOR:
        if (server->seat.cursor_surface == root) damage_pointer_area(server, root->size);
        break;
    case PURP_ROLE_DND_ICON:
        if (server->seat.drag.icon == root) damage_pointer_area(server, root->size);
        break;
    default:
        break;
    }
}

/* =========================================================================================== */
/* Surface trees: iteration, hit-testing, frame callbacks, outputs, scale                    */
/* =========================================================================================== */

static bool surface_for_each_rec(purp_surface_t *surface, int64_t x, int64_t y,
                                 purp_surface_iter_func_t func, void *data, int depth) {
    if (!surface->mapped || depth > 64) return true;
    purp_subsurface_t *sub;
    wl_list_for_each(sub, &surface->subsurfaces_below, link) {
        if (sub->surface && !surface_for_each_rec(sub->surface, x + sub->position.x,
                                                  y + sub->position.y, func, data, depth + 1))
            return false;
    }
    if (!func(surface, surface_clamp_i32(x), surface_clamp_i32(y), data)) return false;
    wl_list_for_each(sub, &surface->subsurfaces_above, link) {
        if (sub->surface && !surface_for_each_rec(sub->surface, x + sub->position.x,
                                                  y + sub->position.y, func, data, depth + 1))
            return false;
    }
    return true;
}

void purp_surface_for_each(purp_surface_t *surface, purp_surface_iter_func_t func, void *data) {
    if (surface && func) surface_for_each_rec(surface, 0, 0, func, data, 0);
}

bool purp_surface_accepts_input(const purp_surface_t *surface, double x, double y) {
    if (!surface || !surface->mapped) return false;
    if (x < 0 || y < 0 || x >= surface->size.w || y >= surface->size.h) return false;
    if (surface->current.input_infinite) return true;
    return purp_region_contains(&surface->current.input, (int32_t)floor(x), (int32_t)floor(y));
}

static purp_surface_t *surface_at_rec(purp_surface_t *surface, double x, double y, double ox,
                                      double oy, double *sx, double *sy, int depth) {
    if (!surface->mapped || depth > 64) return NULL;
    purp_subsurface_t *sub;
    wl_list_for_each_reverse(sub, &surface->subsurfaces_above, link) {
        if (!sub->surface) continue;
        purp_surface_t *hit = surface_at_rec(sub->surface, x, y, ox + sub->position.x,
                                             oy + sub->position.y, sx, sy, depth + 1);
        if (hit) return hit;
    }
    if (purp_surface_accepts_input(surface, x - ox, y - oy)) {
        if (sx) *sx = x - ox;
        if (sy) *sy = y - oy;
        return surface;
    }
    wl_list_for_each_reverse(sub, &surface->subsurfaces_below, link) {
        if (!sub->surface) continue;
        purp_surface_t *hit = surface_at_rec(sub->surface, x, y, ox + sub->position.x,
                                             oy + sub->position.y, sx, sy, depth + 1);
        if (hit) return hit;
    }
    return NULL;
}

purp_surface_t *purp_surface_at(purp_surface_t *root, double x, double y, double *sx, double *sy) {
    if (!root) return NULL;
    return surface_at_rec(root, x, y, 0, 0, sx, sy, 0);
}

static void surface_send_frame_done_rec(purp_surface_t *surface, uint32_t time_ms, int depth) {
    if (depth > 64) return;
    struct wl_resource *callback, *tmp;
    wl_resource_for_each_safe(callback, tmp, &surface->current.frame_callbacks) {
        wl_callback_send_done(callback, time_ms);
        wl_resource_destroy(callback);
    }
    purp_subsurface_t *sub;
    wl_list_for_each(sub, &surface->subsurfaces_below, link)
        if (sub->surface) surface_send_frame_done_rec(sub->surface, time_ms, depth + 1);
    wl_list_for_each(sub, &surface->subsurfaces_above, link)
        if (sub->surface) surface_send_frame_done_rec(sub->surface, time_ms, depth + 1);
}

void purp_surface_send_frame_done(purp_surface_t *surface, uint32_t time_ms) {
    if (surface) surface_send_frame_done_rec(surface, time_ms, 0);
}

/* enter/leave diff for one surface; a bit is only recorded once enter was actually sent (the
 * client may not have bound that wl_output yet, the next update retries). */
static void surface_set_outputs_one(purp_surface_t *surface, uint32_t mask) {
    uint32_t old = surface->output_mask;
    if (old == mask) return;
    purp_wm_t *wm = &surface->server->wm;
    struct wl_client *client = wl_resource_get_client(surface->resource);
    uint32_t result = old & mask;
    for (size_t i = 0; i < wm->monitor_count && i < 32; ++i) {
        uint32_t bit = 1u << i;
        bool was = (old & bit) != 0, want = (mask & bit) != 0;
        if (was == want) continue;
        purp_output_t *output = wm->monitors[i].output;
        struct wl_resource *res = output ? purp_output_resource_for_client(output, client) : NULL;
        if (was) {
            if (res) wl_surface_send_leave(surface->resource, res);
        } else if (res) {
            wl_surface_send_enter(surface->resource, res);
            result |= bit;
        }
    }
    surface->output_mask = result;
}

static void surface_tree_set_outputs(purp_surface_t *surface, uint32_t mask, int depth) {
    if (depth > 64) return;
    surface_set_outputs_one(surface, mask);
    purp_subsurface_t *sub;
    wl_list_for_each(sub, &surface->subsurfaces_below, link)
        if (sub->surface) surface_tree_set_outputs(sub->surface, mask, depth + 1);
    wl_list_for_each(sub, &surface->subsurfaces_above, link)
        if (sub->surface) surface_tree_set_outputs(sub->surface, mask, depth + 1);
}

void purp_surface_set_outputs(purp_surface_t *surface, uint32_t monitor_mask) {
    if (surface) surface_tree_set_outputs(surface, monitor_mask, 0);
}

static void surface_set_preferred_scale_one(purp_surface_t *surface, double scale) {
    if (!(scale > 0) || !isfinite(scale)) return;
    if (fabs(surface->preferred_scale - scale) < 1e-9) return;
    surface->preferred_scale = scale;
    if (surface->fractional_scale)
        wp_fractional_scale_v1_send_preferred_scale(surface->fractional_scale,
                                                    (uint32_t)lround(scale * 120.0));
    if (wl_resource_get_version(surface->resource) >=
        WL_SURFACE_PREFERRED_BUFFER_SCALE_SINCE_VERSION)
        wl_surface_send_preferred_buffer_scale(surface->resource, (int32_t)ceil(scale));
}

static void surface_tree_set_preferred_scale(purp_surface_t *surface, double scale, int depth) {
    if (depth > 64) return;
    surface_set_preferred_scale_one(surface, scale);
    purp_subsurface_t *sub;
    wl_list_for_each(sub, &surface->subsurfaces_below, link)
        if (sub->surface) surface_tree_set_preferred_scale(sub->surface, scale, depth + 1);
    wl_list_for_each(sub, &surface->subsurfaces_above, link)
        if (sub->surface) surface_tree_set_preferred_scale(sub->surface, scale, depth + 1);
}

void purp_surface_set_preferred_scale(purp_surface_t *surface, double scale) {
    if (surface) surface_tree_set_preferred_scale(surface, scale, 0);
}

/* Scale the tree of `surface` should render at (its root's monitor). */
static double surface_owner_scale(purp_surface_t *surface) {
    purp_server_t *server = surface->server;
    int mon = surface_owner_monitor(server, purp_surface_root(surface));
    return mon >= 0 ? server->wm.monitors[mon].scale : server->wm.default_scale;
}

/* =========================================================================================== */
/* Subsurfaces                                                                                */
/* =========================================================================================== */

static bool subsurface_is_synchronized(const purp_subsurface_t *sub) {
    for (int depth = 0; sub && depth < 1024; ++depth) {
        if (sub->synchronized) return true;
        if (!sub->parent) return false;
        sub = sub->parent->subsurface;
    }
    return false;
}

static void subsurface_unlink(purp_subsurface_t *sub) {
    wl_list_remove(&sub->link);
    wl_list_init(&sub->link);
    wl_list_remove(&sub->pending_link);
    wl_list_init(&sub->pending_link);
    wl_list_remove(&sub->parent_destroy.link);
    wl_list_init(&sub->parent_destroy.link);
    sub->parent = NULL;
}

static void subsurface_handle_parent_destroy(struct wl_listener *listener, void *data) {
    purp_subsurface_t *sub = PURP_CONTAINER_OF(listener, purp_subsurface_t, parent_destroy);
    subsurface_unlink(sub);
}

/* The wl_surface died while its wl_subsurface lives on (inert from now on). */
static void subsurface_role_destroy(purp_surface_t *surface) {
    purp_subsurface_t *sub = surface->subsurface;
    if (!sub) return;
    subsurface_unlink(sub);
    state_finish(&sub->cached);
    state_init(&sub->cached);
    sub->has_cache = false;
    sub->surface = NULL;
    surface->subsurface = NULL;
    surface->role_data = NULL;
}

static const purp_surface_role_t subsurface_role = {
    .name = "wl_subsurface",
    .kind = PURP_ROLE_SUBSURFACE,
    .commit = NULL,
    .destroy = subsurface_role_destroy,
};

static void subsurface_handle_resource_destroy(struct wl_resource *resource) {
    purp_subsurface_t *sub = wl_resource_get_user_data(resource);
    if (!sub) return;
    purp_surface_t *surface = sub->surface;
    if (surface) {
        purp_server_t *server = surface->server;
        purp_surface_t *root = sub->parent && surface->mapped ? purp_surface_root(surface) : NULL;
        subsurface_unlink(sub);
        if (root) surface_damage_root(server, root);
        surface->subsurface = NULL;
        purp_surface_clear_role_data(surface);
    }
    state_finish(&sub->cached);
    free(sub);
}

static void subsurface_handle_destroy(struct wl_client *client, struct wl_resource *resource) {
    wl_resource_destroy(resource);
}

static void subsurface_handle_set_position(struct wl_client *client, struct wl_resource *resource,
                                           int32_t x, int32_t y) {
    purp_subsurface_t *sub = wl_resource_get_user_data(resource);
    if (!sub || !sub->surface) return;
    sub->pending_position.x = x;
    sub->pending_position.y = y;
    sub->position_pending = true;
}

/* NULL when `sibling_res` is neither the parent nor a sibling (protocol error posted). */
static struct wl_list *subsurface_sibling_link(purp_subsurface_t *sub,
                                               struct wl_resource *resource,
                                               struct wl_resource *sibling_res, bool *is_parent) {
    purp_surface_t *sibling = purp_surface_from_resource(sibling_res);
    *is_parent = false;
    if (sibling && sibling == sub->parent) {
        *is_parent = true;
        return &sibling->subsurfaces_pending_above;
    }
    if (!sibling || sibling == sub->surface || !sibling->subsurface ||
        sibling->subsurface->parent != sub->parent) {
        wl_resource_post_error(resource, WL_SUBSURFACE_ERROR_BAD_SURFACE,
                               "wl_surface is not a sibling or the parent");
        return NULL;
    }
    return &sibling->subsurface->pending_link;
}

static void subsurface_handle_place_above(struct wl_client *client, struct wl_resource *resource,
                                          struct wl_resource *sibling_res) {
    purp_subsurface_t *sub = wl_resource_get_user_data(resource);
    if (!sub || !sub->surface || !sub->parent) return;
    bool is_parent;
    struct wl_list *anchor = subsurface_sibling_link(sub, resource, sibling_res, &is_parent);
    if (!anchor) return;
    wl_list_remove(&sub->pending_link);
    /* Directly above the parent = bottom of the "above" list; else right after the sibling. */
    wl_list_insert(anchor, &sub->pending_link);
}

static void subsurface_handle_place_below(struct wl_client *client, struct wl_resource *resource,
                                          struct wl_resource *sibling_res) {
    purp_subsurface_t *sub = wl_resource_get_user_data(resource);
    if (!sub || !sub->surface || !sub->parent) return;
    bool is_parent;
    struct wl_list *anchor = subsurface_sibling_link(sub, resource, sibling_res, &is_parent);
    if (!anchor) return;
    wl_list_remove(&sub->pending_link);
    if (is_parent) {
        /* Directly below the parent = top of the "below" list. */
        wl_list_insert(sub->parent->subsurfaces_pending_below.prev, &sub->pending_link);
    } else {
        wl_list_insert(anchor->prev, &sub->pending_link);
    }
}

static void subsurface_handle_set_sync(struct wl_client *client, struct wl_resource *resource) {
    purp_subsurface_t *sub = wl_resource_get_user_data(resource);
    if (sub) sub->synchronized = true;
}

static void subsurface_flush_desynchronized(purp_subsurface_t *sub);

static void subsurface_handle_set_desync(struct wl_client *client, struct wl_resource *resource) {
    purp_subsurface_t *sub = wl_resource_get_user_data(resource);
    if (!sub) return;
    sub->synchronized = false;
    if (sub->surface && !subsurface_is_synchronized(sub))
        subsurface_flush_desynchronized(sub);
}

static const struct wl_subsurface_interface subsurface_impl = {
    .destroy = subsurface_handle_destroy,
    .set_position = subsurface_handle_set_position,
    .place_above = subsurface_handle_place_above,
    .place_below = subsurface_handle_place_below,
    .set_sync = subsurface_handle_set_sync,
    .set_desync = subsurface_handle_set_desync,
};

static void subcompositor_handle_destroy(struct wl_client *client, struct wl_resource *resource) {
    wl_resource_destroy(resource);
}

static void subcompositor_handle_get_subsurface(struct wl_client *client,
                                                struct wl_resource *resource, uint32_t id,
                                                struct wl_resource *surface_res,
                                                struct wl_resource *parent_res) {
    purp_surface_t *surface = purp_surface_from_resource(surface_res);
    purp_surface_t *parent = purp_surface_from_resource(parent_res);
    if (!surface || !parent) {
        wl_resource_post_error(resource, WL_SUBCOMPOSITOR_ERROR_BAD_SURFACE, "unknown wl_surface");
        return;
    }
    if (surface == parent) {
        wl_resource_post_error(resource, WL_SUBCOMPOSITOR_ERROR_BAD_SURFACE,
                               "wl_surface@%u cannot be its own parent",
                               wl_resource_get_id(surface_res));
        return;
    }
    if (surface->subsurface) {
        wl_resource_post_error(resource, WL_SUBCOMPOSITOR_ERROR_BAD_SURFACE,
                               "wl_surface@%u is already a sub-surface",
                               wl_resource_get_id(surface_res));
        return;
    }
    for (purp_surface_t *p = parent; p; p = p->subsurface ? p->subsurface->parent : NULL) {
        if (p == surface) {
            wl_resource_post_error(resource, WL_SUBCOMPOSITOR_ERROR_BAD_PARENT,
                                   "wl_surface@%u is an ancestor of the parent",
                                   wl_resource_get_id(surface_res));
            return;
        }
    }
    purp_subsurface_t *sub = calloc(1, sizeof(*sub));
    if (!sub) {
        wl_client_post_no_memory(client);
        return;
    }
    state_init(&sub->cached);
    wl_list_init(&sub->link);
    wl_list_init(&sub->pending_link);
    wl_list_init(&sub->parent_destroy.link);
    if (!purp_surface_set_role(surface, &subsurface_role, sub, resource,
                               WL_SUBCOMPOSITOR_ERROR_BAD_SURFACE)) {
        free(sub);
        return;
    }
    sub->resource = wl_resource_create(client, &wl_subsurface_interface,
                                       wl_resource_get_version(resource), id);
    if (!sub->resource) {
        surface->role_data = NULL;
        free(sub);
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(sub->resource, &subsurface_impl, sub,
                                   subsurface_handle_resource_destroy);
    sub->surface = surface;
    sub->parent = parent;
    sub->synchronized = true;
    surface->subsurface = sub;
    /* New sub-surfaces start on top of their siblings. */
    wl_list_insert(parent->subsurfaces_above.prev, &sub->link);
    wl_list_insert(parent->subsurfaces_pending_above.prev, &sub->pending_link);
    sub->parent_destroy.notify = subsurface_handle_parent_destroy;
    wl_signal_add(&parent->events_destroy, &sub->parent_destroy);
}

static const struct wl_subcompositor_interface subcompositor_impl = {
    .destroy = subcompositor_handle_destroy,
    .get_subsurface = subcompositor_handle_get_subsurface,
};

/* =========================================================================================== */
/* Commit                                                                                     */
/* =========================================================================================== */

/* Current stacking of `parent`'s children follows the pending stacking on parent commit. */
static void surface_apply_subsurface_order(purp_surface_t *parent) {
    purp_subsurface_t *sub;
    wl_list_init(&parent->subsurfaces_below);
    wl_list_init(&parent->subsurfaces_above);
    wl_list_for_each(sub, &parent->subsurfaces_pending_below, pending_link)
        wl_list_insert(parent->subsurfaces_below.prev, &sub->link);
    wl_list_for_each(sub, &parent->subsurfaces_pending_above, pending_link)
        wl_list_insert(parent->subsurfaces_above.prev, &sub->link);
}

static bool surface_apply(purp_surface_t *surface, purp_surface_state_t *state);

/* A synchronized ancestor also latches descendant caches when an intermediate surface has no
 * cached commit. Its uncommitted position/stacking requests must stay pending. */
static void subsurface_apply_synchronized_cache(purp_subsurface_t *sub) {
    if (!sub->surface || !subsurface_is_synchronized(sub)) return;
    if (sub->has_cache) {
        sub->has_cache = false;
        surface_apply(sub->surface, &sub->cached);
        return;
    }
    purp_subsurface_t *child, *tmp;
    wl_list_for_each_safe(child, tmp, &sub->surface->subsurfaces_below, link)
        subsurface_apply_synchronized_cache(child);
    wl_list_for_each_safe(child, tmp, &sub->surface->subsurfaces_above, link)
        subsurface_apply_synchronized_cache(child);
}

static void subsurface_parent_committed(purp_subsurface_t *sub) {
    if (sub->position_pending) {
        sub->position = sub->pending_position;
        sub->position_pending = false;
    }
    subsurface_apply_synchronized_cache(sub);
}

/* wp_viewport rules checked when the state is applied; false after posting an error. */
static bool surface_validate_viewport(purp_surface_t *surface, const purp_surface_state_t *state,
                                      bool mapped, purp_size_t buffer_size) {
    uint32_t bits = state->committed;
    const purp_surface_state_t *cur = &surface->current;
    bool vp = (bits & PURP_SURFACE_STATE_VIEWPORT) != 0;
    bool has_src = vp ? state->viewport_has_src : cur->viewport_has_src;
    bool has_dst = vp ? state->viewport_has_dst : cur->viewport_has_dst;
    purp_rectf_t src = vp ? state->viewport_src : cur->viewport_src;
    if (!surface->viewport || !mapped || !has_src) return true;
    if (!has_dst && (src.w != floor(src.w) || src.h != floor(src.h))) {
        wl_resource_post_error(surface->viewport, WP_VIEWPORT_ERROR_BAD_SIZE,
                               "source size %gx%g is not integral and no destination is set",
                               src.w, src.h);
        return false;
    }
    int32_t scale = (bits & PURP_SURFACE_STATE_SCALE) ? state->buffer_scale : cur->buffer_scale;
    int32_t transform =
        (bits & PURP_SURFACE_STATE_TRANSFORM) ? state->buffer_transform : cur->buffer_transform;
    double bw = buffer_size.w, bh = buffer_size.h;
    if (transform & 1) {
        double t = bw;
        bw = bh;
        bh = t;
    }
    if (scale > 0) {
        bw /= scale;
        bh /= scale;
    }
    if (src.x + src.w > bw + 1e-6 || src.y + src.h > bh + 1e-6) {
        wl_resource_post_error(surface->viewport, WP_VIEWPORT_ERROR_OUT_OF_BUFFER,
                               "source rectangle %gx%g+%g+%g extends outside the %gx%g buffer",
                               src.w, src.h, src.x, src.y, bw, bh);
        return false;
    }
    return true;
}

/* state (pending or a subsurface cache) -> current; renderer, children and role hooks. */
static bool surface_apply(purp_surface_t *surface, purp_surface_state_t *state) {
    uint32_t bits = state->committed;
    if ((bits & PURP_SURFACE_STATE_BUFFER) && state->buffer &&
        state->buffer->kind == PURP_BUFFER_SHM && !state->buffer->resource) {
        /* The wl_shm buffer died before the commit: keep the old contents. */
        purp_buffer_unlock(state->buffer);
        state->buffer = NULL;
        bits &= ~(uint32_t)PURP_SURFACE_STATE_BUFFER;
        state->committed = bits;
    }

    bool mapped = surface->mapped;
    purp_size_t buffer_size = surface->buffer_size;
    if (bits & PURP_SURFACE_STATE_BUFFER) {
        mapped = state->buffer != NULL;
        buffer_size.w = state->buffer ? state->buffer->width : 0;
        buffer_size.h = state->buffer ? state->buffer->height : 0;
    }
    if (!surface_validate_viewport(surface, state, mapped, buffer_size)) return false;

    purp_surface_state_t *cur = &surface->current;
    purp_buffer_t *old_buffer = NULL;
    if (bits & PURP_SURFACE_STATE_BUFFER) {
        old_buffer = cur->buffer;
        cur->buffer = state->buffer; /* the pending lock becomes the current lock */
        state->buffer = NULL;
        if (cur->buffer) cur->buffer->release_pending = true;
        surface->mapped = mapped;
        surface->buffer_size = buffer_size;
    }
    cur->dx = (bits & PURP_SURFACE_STATE_OFFSET) ? state->dx : 0;
    cur->dy = (bits & PURP_SURFACE_STATE_OFFSET) ? state->dy : 0;
    state->dx = state->dy = 0;
    if (bits & PURP_SURFACE_STATE_SCALE) cur->buffer_scale = state->buffer_scale;
    if (bits & PURP_SURFACE_STATE_TRANSFORM) cur->buffer_transform = state->buffer_transform;
    region_take(&cur->surface_damage, &state->surface_damage);
    region_take(&cur->buffer_damage, &state->buffer_damage);
    if (bits & PURP_SURFACE_STATE_OPAQUE_REGION) region_take(&cur->opaque, &state->opaque);
    if (bits & PURP_SURFACE_STATE_INPUT_REGION) {
        region_take(&cur->input, &state->input);
        cur->input_infinite = state->input_infinite;
    }
    if (bits & PURP_SURFACE_STATE_VIEWPORT) {
        cur->viewport_has_src = state->viewport_has_src;
        cur->viewport_src = state->viewport_src;
        cur->viewport_has_dst = state->viewport_has_dst;
        cur->viewport_dst = state->viewport_dst;
    }
    wl_list_insert_list(cur->frame_callbacks.prev, &state->frame_callbacks);
    wl_list_init(&state->frame_callbacks);
    cur->committed = bits;
    state->committed = 0;

    surface->size = surface_compute_size(surface);
    surface->commit_seq++;

    purp_server_t *server = surface->server;
    if (server->renderer) purp_render_surface_commit(server->renderer, surface);
    /* Only now: render.c may alias the previous buffer's texture until the call above. */
    if (old_buffer) purp_buffer_unlock(old_buffer);

    surface_apply_subsurface_order(surface);
    purp_subsurface_t *sub, *tmp;
    wl_list_for_each_safe(sub, tmp, &surface->subsurfaces_below, link) subsurface_parent_committed(sub);
    wl_list_for_each_safe(sub, tmp, &surface->subsurfaces_above, link) subsurface_parent_committed(sub);

    if (surface->role && surface->role->commit) surface->role->commit(surface);
    wl_signal_emit_mutable(&surface->events_commit, surface);
    return true;
}

/* Outputs, preferred scale and damage for the tree of a top-level applied commit. */
static void surface_after_commit(purp_surface_t *surface, bool was_mapped, purp_size_t old_size,
                                 bool input_changed) {
    purp_server_t *server = surface->server;
    purp_surface_t *root = purp_surface_root(surface);
    int mon = surface_owner_monitor(server, root);
    double scale = mon >= 0 ? server->wm.monitors[mon].scale : server->wm.default_scale;
    purp_surface_set_outputs(surface, mon >= 0 ? 1u << mon : 0);
    purp_surface_set_preferred_scale(surface, scale);
    surface_damage_root(server, root);
    /* Role surfaces resync pointer focus in their commit hooks; subsurfaces do it here. */
    bool changed = was_mapped != surface->mapped || old_size.w != surface->size.w ||
                   old_size.h != surface->size.h || input_changed;
    if (changed && surface->subsurface && surface->subsurface->parent)
        purp_input_sync_pointer_focus(server, 0);
}

/* Turning an ancestor desynchronized can make an entire branch effectively desynchronized.
 * Apply only committed caches; wl_surface pending state is still awaiting its own commit. */
static void subsurface_flush_desynchronized(purp_subsurface_t *sub) {
    if (!sub->surface || subsurface_is_synchronized(sub)) return;
    purp_surface_t *surface = sub->surface;
    if (sub->has_cache) {
        bool was_mapped = surface->mapped;
        purp_size_t old_size = surface->size;
        bool input_changed = (sub->cached.committed & PURP_SURFACE_STATE_INPUT_REGION) != 0;
        sub->has_cache = false;
        if (!surface_apply(surface, &sub->cached)) return;
        surface_after_commit(surface, was_mapped, old_size, input_changed);
    }
    purp_subsurface_t *child, *tmp;
    wl_list_for_each_safe(child, tmp, &surface->subsurfaces_below, link)
        subsurface_flush_desynchronized(child);
    wl_list_for_each_safe(child, tmp, &surface->subsurfaces_above, link)
        subsurface_flush_desynchronized(child);
}

static void surface_handle_commit(struct wl_client *client, struct wl_resource *resource) {
    purp_surface_t *surface = wl_resource_get_user_data(resource);
    if (!surface) return;
    purp_subsurface_t *sub = surface->subsurface;
    if (sub && sub->parent && subsurface_is_synchronized(sub)) {
        state_merge(&sub->cached, &surface->pending);
        sub->has_cache = true;
        return;
    }
    bool was_mapped = surface->mapped;
    purp_size_t old_size = surface->size;
    purp_surface_state_t *state = &surface->pending;
    if (sub && sub->has_cache) {
        state_merge(&sub->cached, &surface->pending);
        sub->has_cache = false;
        state = &sub->cached;
    }
    bool input_changed = (state->committed & PURP_SURFACE_STATE_INPUT_REGION) != 0;
    if (!surface_apply(surface, state)) return;
    surface_after_commit(surface, was_mapped, old_size, input_changed);
}

/* =========================================================================================== */
/* wl_surface                                                                                 */
/* =========================================================================================== */

static void surface_handle_destroy(struct wl_client *client, struct wl_resource *resource) {
    wl_resource_destroy(resource);
}

static void surface_handle_attach(struct wl_client *client, struct wl_resource *resource,
                                  struct wl_resource *buffer_res, int32_t x, int32_t y) {
    purp_surface_t *surface = wl_resource_get_user_data(resource);
    if (!surface) return;
    bool legacy_offset = wl_resource_get_version(resource) < WL_SURFACE_OFFSET_SINCE_VERSION;
    if (!legacy_offset && (x != 0 || y != 0)) {
        wl_resource_post_error(resource, WL_SURFACE_ERROR_INVALID_OFFSET,
                               "attach with a non-zero offset (use wl_surface.offset)");
        return;
    }
    purp_buffer_t *buffer = NULL;
    if (buffer_res) {
        buffer = purp_buffer_from_resource(surface->server, buffer_res);
        if (!buffer) {
            wl_client_post_implementation_error(client, "unsupported or invalid wl_buffer@%u",
                                                wl_resource_get_id(buffer_res));
            return;
        }
        ((struct surface_storage *)surface)->buffer_attached = true;
    }
    purp_buffer_lock(buffer);
    if (surface->pending.buffer) purp_buffer_unlock(surface->pending.buffer);
    surface->pending.buffer = buffer;
    surface->pending.committed |= PURP_SURFACE_STATE_BUFFER;
    if (legacy_offset) {
        surface->pending.dx = x;
        surface->pending.dy = y;
        surface->pending.committed |= PURP_SURFACE_STATE_OFFSET;
    }
}

static void surface_handle_damage(struct wl_client *client, struct wl_resource *resource,
                                  int32_t x, int32_t y, int32_t width, int32_t height) {
    purp_surface_t *surface = wl_resource_get_user_data(resource);
    if (!surface || width <= 0 || height <= 0) return;
    damage_add(&surface->pending.surface_damage, purp_rect_make(x, y, width, height));
    surface->pending.committed |= PURP_SURFACE_STATE_SURFACE_DAMAGE;
}

static void callback_handle_resource_destroy(struct wl_resource *resource) {
    wl_list_remove(wl_resource_get_link(resource));
}

static void surface_handle_frame(struct wl_client *client, struct wl_resource *resource,
                                 uint32_t callback_id) {
    purp_surface_t *surface = wl_resource_get_user_data(resource);
    struct wl_resource *callback = wl_resource_create(client, &wl_callback_interface, 1,
                                                      callback_id);
    if (!callback) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(callback, NULL, NULL, callback_handle_resource_destroy);
    if (!surface) {
        wl_list_init(wl_resource_get_link(callback));
        return;
    }
    wl_list_insert(surface->pending.frame_callbacks.prev, wl_resource_get_link(callback));
    surface->pending.committed |= PURP_SURFACE_STATE_FRAME_CALLBACKS;
}

static void surface_handle_set_opaque_region(struct wl_client *client,
                                             struct wl_resource *resource,
                                             struct wl_resource *region_res) {
    purp_surface_t *surface = wl_resource_get_user_data(resource);
    if (!surface) return;
    if (region_res) {
        const purp_region_t *region = wl_resource_get_user_data(region_res);
        if (!purp_region_copy(&surface->pending.opaque, region)) {
            wl_client_post_no_memory(client);
            return;
        }
    } else {
        purp_region_clear(&surface->pending.opaque);
    }
    surface->pending.committed |= PURP_SURFACE_STATE_OPAQUE_REGION;
}

static void surface_handle_set_input_region(struct wl_client *client,
                                            struct wl_resource *resource,
                                            struct wl_resource *region_res) {
    purp_surface_t *surface = wl_resource_get_user_data(resource);
    if (!surface) return;
    if (region_res) {
        const purp_region_t *region = wl_resource_get_user_data(region_res);
        if (!purp_region_copy(&surface->pending.input, region)) {
            wl_client_post_no_memory(client);
            return;
        }
        surface->pending.input_infinite = false;
    } else {
        purp_region_clear(&surface->pending.input);
        surface->pending.input_infinite = true;
    }
    surface->pending.committed |= PURP_SURFACE_STATE_INPUT_REGION;
}

static void surface_handle_set_buffer_transform(struct wl_client *client,
                                                struct wl_resource *resource, int32_t transform) {
    purp_surface_t *surface = wl_resource_get_user_data(resource);
    if (!surface) return;
    if (transform < WL_OUTPUT_TRANSFORM_NORMAL || transform > WL_OUTPUT_TRANSFORM_FLIPPED_270) {
        wl_resource_post_error(resource, WL_SURFACE_ERROR_INVALID_TRANSFORM,
                               "invalid buffer transform %d", transform);
        return;
    }
    surface->pending.buffer_transform = transform;
    surface->pending.committed |= PURP_SURFACE_STATE_TRANSFORM;
}

static void surface_handle_set_buffer_scale(struct wl_client *client, struct wl_resource *resource,
                                            int32_t scale) {
    purp_surface_t *surface = wl_resource_get_user_data(resource);
    if (!surface) return;
    if (scale <= 0) {
        wl_resource_post_error(resource, WL_SURFACE_ERROR_INVALID_SCALE,
                               "invalid buffer scale %d", scale);
        return;
    }
    surface->pending.buffer_scale = scale;
    surface->pending.committed |= PURP_SURFACE_STATE_SCALE;
}

static void surface_handle_damage_buffer(struct wl_client *client, struct wl_resource *resource,
                                         int32_t x, int32_t y, int32_t width, int32_t height) {
    purp_surface_t *surface = wl_resource_get_user_data(resource);
    if (!surface || width <= 0 || height <= 0) return;
    damage_add(&surface->pending.buffer_damage, purp_rect_make(x, y, width, height));
    surface->pending.committed |= PURP_SURFACE_STATE_BUFFER_DAMAGE;
}

static void surface_handle_offset(struct wl_client *client, struct wl_resource *resource,
                                  int32_t x, int32_t y) {
    purp_surface_t *surface = wl_resource_get_user_data(resource);
    if (!surface) return;
    surface->pending.dx = x;
    surface->pending.dy = y;
    surface->pending.committed |= PURP_SURFACE_STATE_OFFSET;
}

static const struct wl_surface_interface surface_impl = {
    .destroy = surface_handle_destroy,
    .attach = surface_handle_attach,
    .damage = surface_handle_damage,
    .frame = surface_handle_frame,
    .set_opaque_region = surface_handle_set_opaque_region,
    .set_input_region = surface_handle_set_input_region,
    .commit = surface_handle_commit,
    .set_buffer_transform = surface_handle_set_buffer_transform,
    .set_buffer_scale = surface_handle_set_buffer_scale,
    .damage_buffer = surface_handle_damage_buffer,
    .offset = surface_handle_offset,
};

static void surface_handle_resource_destroy(struct wl_resource *resource) {
    purp_surface_t *surface = wl_resource_get_user_data(resource);
    if (!surface) return;
    purp_server_t *server = surface->server;
    if (surface->mapped) surface_damage_root(server, purp_surface_root(surface));
    if (surface->role && surface->role->destroy) surface->role->destroy(surface);
    /* Children (parent_destroy), the seat and role modules detach here. */
    wl_signal_emit_mutable(&surface->events_destroy, surface);
    if (surface->subsurface) subsurface_role_destroy(surface);
    if (surface->viewport) wl_resource_set_user_data(surface->viewport, NULL);
    if (surface->fractional_scale) wl_resource_set_user_data(surface->fractional_scale, NULL);
    surface->viewport = surface->fractional_scale = NULL;
    if (server->renderer) purp_render_surface_destroyed(server->renderer, surface);
    surface->texture = NULL;
    state_finish(&surface->pending);
    state_finish(&surface->current);
    wl_list_remove(&surface->link);
    free(surface);
}

/* =========================================================================================== */
/* wl_region and wl_compositor                                                                */
/* =========================================================================================== */

static void region_handle_destroy(struct wl_client *client, struct wl_resource *resource) {
    wl_resource_destroy(resource);
}

static void region_handle_add(struct wl_client *client, struct wl_resource *resource, int32_t x,
                              int32_t y, int32_t width, int32_t height) {
    purp_region_t *region = wl_resource_get_user_data(resource);
    if (region && !purp_region_add(region, purp_rect_make(x, y, width, height)))
        wl_client_post_no_memory(client);
}

static void region_handle_subtract(struct wl_client *client, struct wl_resource *resource,
                                   int32_t x, int32_t y, int32_t width, int32_t height) {
    purp_region_t *region = wl_resource_get_user_data(resource);
    if (region) purp_region_subtract(region, purp_rect_make(x, y, width, height));
}

static const struct wl_region_interface region_impl = {
    .destroy = region_handle_destroy,
    .add = region_handle_add,
    .subtract = region_handle_subtract,
};

static void region_handle_resource_destroy(struct wl_resource *resource) {
    purp_region_t *region = wl_resource_get_user_data(resource);
    if (!region) return;
    purp_region_finish(region);
    free(region);
}

static void compositor_handle_create_surface(struct wl_client *client,
                                             struct wl_resource *resource, uint32_t id) {
    purp_server_t *server = wl_resource_get_user_data(resource);
    struct surface_storage *storage = calloc(1, sizeof(*storage));
    if (!storage) {
        wl_client_post_no_memory(client);
        return;
    }
    purp_surface_t *surface = &storage->base;
    surface->resource = wl_resource_create(client, &wl_surface_interface,
                                           wl_resource_get_version(resource), id);
    if (!surface->resource) {
        free(surface);
        wl_client_post_no_memory(client);
        return;
    }
    surface->server = server;
    state_init(&surface->pending);
    state_init(&surface->current);
    surface->preferred_scale = server->wm.default_scale > 0 ? server->wm.default_scale : 1.0;
    wl_list_init(&surface->subsurfaces_below);
    wl_list_init(&surface->subsurfaces_above);
    wl_list_init(&surface->subsurfaces_pending_below);
    wl_list_init(&surface->subsurfaces_pending_above);
    wl_signal_init(&surface->events_commit);
    wl_signal_init(&surface->events_destroy);
    wl_list_insert(&server->surfaces, &surface->link);
    wl_resource_set_implementation(surface->resource, &surface_impl, surface,
                                   surface_handle_resource_destroy);
}

static void compositor_handle_create_region(struct wl_client *client,
                                            struct wl_resource *resource, uint32_t id) {
    purp_region_t *region = calloc(1, sizeof(*region));
    if (!region) {
        wl_client_post_no_memory(client);
        return;
    }
    struct wl_resource *region_res = wl_resource_create(client, &wl_region_interface,
                                                        wl_resource_get_version(resource), id);
    if (!region_res) {
        free(region);
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(region_res, &region_impl, region,
                                   region_handle_resource_destroy);
}

static const struct wl_compositor_interface compositor_impl = {
    .create_surface = compositor_handle_create_surface,
    .create_region = compositor_handle_create_region,
};

static void compositor_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
    struct wl_resource *resource =
        wl_resource_create(client, &wl_compositor_interface, (int)version, id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &compositor_impl, data, NULL);
}

static void subcompositor_bind(struct wl_client *client, void *data, uint32_t version,
                               uint32_t id) {
    struct wl_resource *resource =
        wl_resource_create(client, &wl_subcompositor_interface, (int)version, id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &subcompositor_impl, data, NULL);
}

/* =========================================================================================== */
/* wp_single_pixel_buffer_manager_v1                                                          */
/* =========================================================================================== */

static void single_pixel_handle_destroy(struct wl_client *client, struct wl_resource *resource) {
    wl_resource_destroy(resource);
}

static void single_pixel_handle_create(struct wl_client *client, struct wl_resource *resource,
                                       uint32_t id, uint32_t r, uint32_t g, uint32_t b,
                                       uint32_t a) {
    purp_server_t *server = wl_resource_get_user_data(resource);
    struct wl_resource *buffer_res = wl_resource_create(client, &wl_buffer_interface, 1, id);
    if (!buffer_res) {
        wl_client_post_no_memory(client);
        return;
    }
    purp_buffer_t *buffer = buffer_alloc(server, PURP_BUFFER_SINGLE_PIXEL);
    if (!buffer) {
        wl_resource_destroy(buffer_res);
        wl_client_post_no_memory(client);
        return;
    }
    buffer->width = 1;
    buffer->height = 1;
    buffer->format = DRM_FORMAT_ARGB8888;
    buffer->has_alpha = a != UINT32_MAX;
    /* Premultiplied u32 channels -> straight 0..1. */
    double alpha = (double)a / (double)UINT32_MAX;
    double channels[3] = { (double)r / (double)UINT32_MAX, (double)g / (double)UINT32_MAX,
                           (double)b / (double)UINT32_MAX };
    for (int i = 0; i < 3; ++i)
        buffer->single_pixel[i] =
            alpha > 0 ? (float)purp_clamp_f64(channels[i] / alpha, 0.0, 1.0) : 0.0f;
    buffer->single_pixel[3] = (float)alpha;
    wl_resource_set_implementation(buffer_res, &buffer_impl, buffer, NULL);
    buffer_bind_resource(buffer, buffer_res);
}

static const struct wp_single_pixel_buffer_manager_v1_interface single_pixel_impl = {
    .destroy = single_pixel_handle_destroy,
    .create_u32_rgba_buffer = single_pixel_handle_create,
};

static void single_pixel_bind(struct wl_client *client, void *data, uint32_t version,
                              uint32_t id) {
    struct wl_resource *resource =
        wl_resource_create(client, &wp_single_pixel_buffer_manager_v1_interface, (int)version, id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &single_pixel_impl, data, NULL);
}

/* =========================================================================================== */
/* wp_viewporter                                                                              */
/* =========================================================================================== */

static void viewport_handle_destroy(struct wl_client *client, struct wl_resource *resource) {
    wl_resource_destroy(resource);
}

static void viewport_handle_resource_destroy(struct wl_resource *resource) {
    purp_surface_t *surface = wl_resource_get_user_data(resource);
    if (!surface) return;
    surface->viewport = NULL;
    surface->pending.viewport_has_src = false;
    surface->pending.viewport_has_dst = false;
    surface->pending.committed |= PURP_SURFACE_STATE_VIEWPORT;
}

static void viewport_handle_set_source(struct wl_client *client, struct wl_resource *resource,
                                       wl_fixed_t x, wl_fixed_t y, wl_fixed_t width,
                                       wl_fixed_t height) {
    purp_surface_t *surface = wl_resource_get_user_data(resource);
    if (!surface) {
        wl_resource_post_error(resource, WP_VIEWPORT_ERROR_NO_SURFACE,
                               "the wl_surface was destroyed");
        return;
    }
    double fx = wl_fixed_to_double(x), fy = wl_fixed_to_double(y);
    double fw = wl_fixed_to_double(width), fh = wl_fixed_to_double(height);
    if (fx == -1.0 && fy == -1.0 && fw == -1.0 && fh == -1.0) {
        surface->pending.viewport_has_src = false;
    } else if (fx < 0 || fy < 0 || fw <= 0 || fh <= 0) {
        wl_resource_post_error(resource, WP_VIEWPORT_ERROR_BAD_VALUE,
                               "invalid source rectangle %gx%g+%g+%g", fw, fh, fx, fy);
        return;
    } else {
        surface->pending.viewport_has_src = true;
        surface->pending.viewport_src.x = fx;
        surface->pending.viewport_src.y = fy;
        surface->pending.viewport_src.w = fw;
        surface->pending.viewport_src.h = fh;
    }
    surface->pending.committed |= PURP_SURFACE_STATE_VIEWPORT;
}

static void viewport_handle_set_destination(struct wl_client *client,
                                            struct wl_resource *resource, int32_t width,
                                            int32_t height) {
    purp_surface_t *surface = wl_resource_get_user_data(resource);
    if (!surface) {
        wl_resource_post_error(resource, WP_VIEWPORT_ERROR_NO_SURFACE,
                               "the wl_surface was destroyed");
        return;
    }
    if (width == -1 && height == -1) {
        surface->pending.viewport_has_dst = false;
    } else if (width <= 0 || height <= 0) {
        wl_resource_post_error(resource, WP_VIEWPORT_ERROR_BAD_VALUE,
                               "invalid destination size %dx%d", width, height);
        return;
    } else {
        surface->pending.viewport_has_dst = true;
        surface->pending.viewport_dst.w = width;
        surface->pending.viewport_dst.h = height;
    }
    surface->pending.committed |= PURP_SURFACE_STATE_VIEWPORT;
}

static const struct wp_viewport_interface viewport_impl = {
    .destroy = viewport_handle_destroy,
    .set_source = viewport_handle_set_source,
    .set_destination = viewport_handle_set_destination,
};

static void viewporter_handle_destroy(struct wl_client *client, struct wl_resource *resource) {
    wl_resource_destroy(resource);
}

static void viewporter_handle_get_viewport(struct wl_client *client, struct wl_resource *resource,
                                           uint32_t id, struct wl_resource *surface_res) {
    purp_surface_t *surface = purp_surface_from_resource(surface_res);
    if (!surface) {
        wl_client_post_implementation_error(client, "unknown wl_surface");
        return;
    }
    if (surface->viewport) {
        wl_resource_post_error(resource, WP_VIEWPORTER_ERROR_VIEWPORT_EXISTS,
                               "wl_surface@%u already has a wp_viewport",
                               wl_resource_get_id(surface_res));
        return;
    }
    struct wl_resource *viewport =
        wl_resource_create(client, &wp_viewport_interface, wl_resource_get_version(resource), id);
    if (!viewport) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(viewport, &viewport_impl, surface,
                                   viewport_handle_resource_destroy);
    surface->viewport = viewport;
}

static const struct wp_viewporter_interface viewporter_impl = {
    .destroy = viewporter_handle_destroy,
    .get_viewport = viewporter_handle_get_viewport,
};

static void viewporter_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
    struct wl_resource *resource =
        wl_resource_create(client, &wp_viewporter_interface, (int)version, id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &viewporter_impl, data, NULL);
}

/* =========================================================================================== */
/* wp_fractional_scale_manager_v1                                                             */
/* =========================================================================================== */

static void fractional_scale_handle_destroy(struct wl_client *client,
                                            struct wl_resource *resource) {
    wl_resource_destroy(resource);
}

static void fractional_scale_handle_resource_destroy(struct wl_resource *resource) {
    purp_surface_t *surface = wl_resource_get_user_data(resource);
    if (surface) surface->fractional_scale = NULL;
}

static const struct wp_fractional_scale_v1_interface fractional_scale_impl = {
    .destroy = fractional_scale_handle_destroy,
};

static void fractional_manager_handle_destroy(struct wl_client *client,
                                              struct wl_resource *resource) {
    wl_resource_destroy(resource);
}

static void fractional_manager_handle_get(struct wl_client *client, struct wl_resource *resource,
                                          uint32_t id, struct wl_resource *surface_res) {
    purp_surface_t *surface = purp_surface_from_resource(surface_res);
    if (!surface) {
        wl_client_post_implementation_error(client, "unknown wl_surface");
        return;
    }
    if (surface->fractional_scale) {
        wl_resource_post_error(resource,
                               WP_FRACTIONAL_SCALE_MANAGER_V1_ERROR_FRACTIONAL_SCALE_EXISTS,
                               "wl_surface@%u already has a wp_fractional_scale_v1",
                               wl_resource_get_id(surface_res));
        return;
    }
    struct wl_resource *fractional = wl_resource_create(
        client, &wp_fractional_scale_v1_interface, wl_resource_get_version(resource), id);
    if (!fractional) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(fractional, &fractional_scale_impl, surface,
                                   fractional_scale_handle_resource_destroy);
    surface->fractional_scale = fractional;
    if (surface->preferred_scale > 0)
        wp_fractional_scale_v1_send_preferred_scale(
            fractional, (uint32_t)lround(surface->preferred_scale * 120.0));
    else
        surface_set_preferred_scale_one(surface, surface_owner_scale(surface));
}

static const struct wp_fractional_scale_manager_v1_interface fractional_manager_impl = {
    .destroy = fractional_manager_handle_destroy,
    .get_fractional_scale = fractional_manager_handle_get,
};

static void fractional_manager_bind(struct wl_client *client, void *data, uint32_t version,
                                    uint32_t id) {
    struct wl_resource *resource =
        wl_resource_create(client, &wp_fractional_scale_manager_v1_interface, (int)version, id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &fractional_manager_impl, data, NULL);
}

/* =========================================================================================== */
/* Globals                                                                                    */
/* =========================================================================================== */

bool purp_surface_globals_init(purp_server_t *server) {
    if (!server || !server->display) return false;
    struct wl_display *display = server->display;
    server->compositor_global = wl_global_create(display, &wl_compositor_interface,
                                                 PURP_WL_COMPOSITOR_VERSION, server,
                                                 compositor_bind);
    server->subcompositor_global = wl_global_create(display, &wl_subcompositor_interface,
                                                    PURP_WL_SUBCOMPOSITOR_VERSION, server,
                                                    subcompositor_bind);
    if (!server->compositor_global || !server->subcompositor_global) return false;
    if (wl_display_init_shm(display) != 0) return false;
    if (!wl_display_add_shm_format(display, WL_SHM_FORMAT_ABGR8888) ||
        !wl_display_add_shm_format(display, WL_SHM_FORMAT_XBGR8888))
        return false;
    server->single_pixel_global = wl_global_create(display,
                                                   &wp_single_pixel_buffer_manager_v1_interface,
                                                   PURP_SINGLE_PIXEL_BUFFER_VERSION, server,
                                                   single_pixel_bind);
    server->viewporter_global = wl_global_create(display, &wp_viewporter_interface,
                                                 PURP_VIEWPORTER_VERSION, server,
                                                 viewporter_bind);
    server->fractional_scale_global = wl_global_create(display,
                                                       &wp_fractional_scale_manager_v1_interface,
                                                       PURP_FRACTIONAL_SCALE_VERSION, server,
                                                       fractional_manager_bind);
    return server->single_pixel_global && server->viewporter_global &&
           server->fractional_scale_global;
}

/* =========================================================================================== */
/* zwp_linux_dmabuf_v1                                                                        */
/* =========================================================================================== */

/* One format-table entry (linux-dmabuf v4). */
typedef struct dmabuf_table_entry {
    uint32_t format;
    uint32_t pad;
    uint64_t modifier;
} dmabuf_table_entry_t;

typedef struct dmabuf_params {
    purp_server_t *server;
    purp_dmabuf_attributes_t attrs;
    bool used;
    bool has_modifier;
} dmabuf_params_t;

static size_t dmabuf_table_count(const purp_server_t *server) {
    /* Tranche indices are uint16. */
    return server->dmabuf_format_count > 65536 ? 65536 : server->dmabuf_format_count;
}

static bool dmabuf_build_table(purp_server_t *server) {
    size_t count = dmabuf_table_count(server);
    if (count == 0) return false;
    size_t size = count * sizeof(dmabuf_table_entry_t);
    int fd = memfd_create("purpwm+-dmabuf-format-table", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (fd < 0) return false;
    if (ftruncate(fd, (off_t)size) != 0) {
        close(fd);
        return false;
    }
    void *map = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) {
        close(fd);
        return false;
    }
    dmabuf_table_entry_t *entries = map;
    for (size_t i = 0; i < count; ++i) {
        entries[i].format = server->dmabuf_formats[i].format;
        entries[i].pad = 0;
        entries[i].modifier = server->dmabuf_formats[i].modifier;
    }
    munmap(map, size);
    /* Clients map the same fd read-only; sealing keeps it immutable. */
    if (fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE | F_SEAL_SEAL) != 0) {
        close(fd);
        return false;
    }
    server->dmabuf_table_fd = fd;
    server->dmabuf_table_size = size;
    return true;
}

static void dmabuf_send_feedback(purp_server_t *server, struct wl_resource *feedback) {
    if (server->dmabuf_table_fd >= 0) {
        struct wl_array device;
        wl_array_init(&device);
        dev_t *dev = wl_array_add(&device, sizeof(*dev));
        if (dev) *dev = server->dmabuf_main_device;
        struct wl_array indices;
        wl_array_init(&indices);
        size_t count = dmabuf_table_count(server);
        uint16_t *idx = wl_array_add(&indices, count * sizeof(uint16_t));
        if (dev && idx) {
            for (size_t i = 0; i < count; ++i) idx[i] = (uint16_t)i;
            zwp_linux_dmabuf_feedback_v1_send_format_table(feedback, server->dmabuf_table_fd,
                                                           (uint32_t)server->dmabuf_table_size);
            zwp_linux_dmabuf_feedback_v1_send_main_device(feedback, &device);
            zwp_linux_dmabuf_feedback_v1_send_tranche_target_device(feedback, &device);
            zwp_linux_dmabuf_feedback_v1_send_tranche_flags(feedback, 0);
            zwp_linux_dmabuf_feedback_v1_send_tranche_formats(feedback, &indices);
            zwp_linux_dmabuf_feedback_v1_send_tranche_done(feedback);
        } else {
            wl_resource_post_no_memory(feedback);
        }
        wl_array_release(&indices);
        wl_array_release(&device);
    }
    zwp_linux_dmabuf_feedback_v1_send_done(feedback);
}

static void dmabuf_feedback_handle_destroy(struct wl_client *client,
                                           struct wl_resource *resource) {
    wl_resource_destroy(resource);
}

static const struct zwp_linux_dmabuf_feedback_v1_interface dmabuf_feedback_impl = {
    .destroy = dmabuf_feedback_handle_destroy,
};

static void dmabuf_create_feedback(struct wl_client *client, struct wl_resource *resource,
                                   uint32_t id) {
    purp_server_t *server = wl_resource_get_user_data(resource);
    struct wl_resource *feedback = wl_resource_create(
        client, &zwp_linux_dmabuf_feedback_v1_interface, wl_resource_get_version(resource), id);
    if (!feedback) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(feedback, &dmabuf_feedback_impl, NULL, NULL);
    dmabuf_send_feedback(server, feedback);
}

static void params_close_fds(dmabuf_params_t *params) {
    for (int i = 0; i < PURP_DMABUF_MAX_PLANES; ++i) {
        if (params->attrs.fd[i] >= 0) close(params->attrs.fd[i]);
        params->attrs.fd[i] = -1;
    }
}

static void params_handle_resource_destroy(struct wl_resource *resource) {
    dmabuf_params_t *params = wl_resource_get_user_data(resource);
    if (!params) return;
    params_close_fds(params);
    free(params);
}

static void params_handle_destroy(struct wl_client *client, struct wl_resource *resource) {
    wl_resource_destroy(resource);
}

static void params_handle_add(struct wl_client *client, struct wl_resource *resource, int32_t fd,
                              uint32_t plane_idx, uint32_t offset, uint32_t stride,
                              uint32_t modifier_hi, uint32_t modifier_lo) {
    dmabuf_params_t *params = wl_resource_get_user_data(resource);
    if (!params) {
        close(fd);
        return;
    }
    if (params->used) {
        close(fd);
        wl_resource_post_error(resource, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_ALREADY_USED,
                               "params were already used to create a wl_buffer");
        return;
    }
    if (plane_idx >= PURP_DMABUF_MAX_PLANES) {
        close(fd);
        wl_resource_post_error(resource, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_PLANE_IDX,
                               "plane index %u is out of bounds", plane_idx);
        return;
    }
    if (params->attrs.fd[plane_idx] >= 0) {
        close(fd);
        wl_resource_post_error(resource, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_PLANE_SET,
                               "plane %u was already set", plane_idx);
        return;
    }
    uint64_t modifier = ((uint64_t)modifier_hi << 32) | modifier_lo;
    if (params->has_modifier && modifier != params->attrs.modifier) {
        close(fd);
        wl_resource_post_error(resource, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INVALID_FORMAT,
                               "every plane must use the same modifier");
        return;
    }
    params->has_modifier = true;
    params->attrs.modifier = modifier;
    params->attrs.fd[plane_idx] = fd;
    params->attrs.offset[plane_idx] = offset;
    params->attrs.stride[plane_idx] = stride;
    if ((int)plane_idx + 1 > params->attrs.n_planes) params->attrs.n_planes = (int)plane_idx + 1;
}

/* Protocol validation shared by create and create_immed; false after posting an error. */
static bool params_validate(dmabuf_params_t *params, struct wl_resource *resource, int32_t width,
                            int32_t height) {
    purp_dmabuf_attributes_t *attrs = &params->attrs;
    if (attrs->n_planes <= 0) {
        wl_resource_post_error(resource, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INCOMPLETE,
                               "no dmabuf planes were added");
        return false;
    }
    for (int i = 0; i < attrs->n_planes; ++i) {
        if (attrs->fd[i] < 0) {
            wl_resource_post_error(resource, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INCOMPLETE,
                                   "plane %d is missing", i);
            return false;
        }
    }
    if (width <= 0 || height <= 0) {
        wl_resource_post_error(resource, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INVALID_DIMENSIONS,
                               "invalid buffer size %dx%d", width, height);
        return false;
    }
    for (int i = 0; i < attrs->n_planes; ++i) {
        uint64_t offset = attrs->offset[i], stride = attrs->stride[i];
        if (offset + stride > UINT32_MAX ||
            (i == 0 && offset + stride * (uint64_t)height > UINT32_MAX)) {
            wl_resource_post_error(resource, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_OUT_OF_BOUNDS,
                                   "plane %d size overflows", i);
            return false;
        }
        off_t size = lseek(attrs->fd[i], 0, SEEK_END);
        if (size < 0) continue; /* not every exporter supports lseek */
        lseek(attrs->fd[i], 0, SEEK_SET);
        if (size == 0) continue;
        if (offset >= (uint64_t)size || offset + stride > (uint64_t)size ||
            (i == 0 && offset + stride * (uint64_t)height > (uint64_t)size)) {
            wl_resource_post_error(resource, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_OUT_OF_BOUNDS,
                                   "plane %d is out of the dmabuf bounds", i);
            return false;
        }
    }
    return true;
}

static void params_create_common(struct wl_client *client, struct wl_resource *resource,
                                 uint32_t buffer_id, int32_t width, int32_t height,
                                 uint32_t format, uint32_t flags, bool immediate) {
    dmabuf_params_t *params = wl_resource_get_user_data(resource);
    if (!params) return;
    if (params->used) {
        wl_resource_post_error(resource, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_ALREADY_USED,
                               "params were already used to create a wl_buffer");
        return;
    }
    params->used = true;
    if (!params_validate(params, resource, width, height)) return;

    purp_dmabuf_attributes_t *attrs = &params->attrs;
    attrs->width = width;
    attrs->height = height;
    attrs->format = format;
    attrs->flags = flags;
    if (!params->has_modifier) attrs->modifier = DRM_FORMAT_MOD_INVALID;

    purp_server_t *server = params->server;
    /* Y-invert / interlaced buffers are not supported by the renderer. */
    bool importable = flags == 0 && server->renderer &&
                      purp_renderer_test_dmabuf(server->renderer, attrs);
    if (!importable) {
        if (immediate) {
            /* An inert wl_buffer: attaching it is an error (unknown buffer). */
            struct wl_resource *inert = wl_resource_create(client, &wl_buffer_interface, 1,
                                                           buffer_id);
            if (inert) wl_resource_set_implementation(inert, &buffer_impl, NULL, NULL);
        }
        zwp_linux_buffer_params_v1_send_failed(resource);
        return;
    }

    struct wl_resource *buffer_res = wl_resource_create(client, &wl_buffer_interface, 1,
                                                        buffer_id);
    if (!buffer_res) {
        wl_client_post_no_memory(client);
        return;
    }
    purp_buffer_t *buffer = buffer_alloc(server, PURP_BUFFER_DMABUF);
    if (!buffer) {
        wl_resource_destroy(buffer_res);
        wl_client_post_no_memory(client);
        return;
    }
    buffer->dmabuf = *attrs; /* the buffer owns the fds from now on */
    for (int i = 0; i < PURP_DMABUF_MAX_PLANES; ++i) attrs->fd[i] = -1;
    buffer->width = width;
    buffer->height = height;
    buffer->format = format;
    buffer->has_alpha = drm_format_has_alpha(format);
    wl_resource_set_implementation(buffer_res, &buffer_impl, buffer, NULL);
    buffer_bind_resource(buffer, buffer_res);
    if (!immediate) zwp_linux_buffer_params_v1_send_created(resource, buffer_res);
}

static void params_handle_create(struct wl_client *client, struct wl_resource *resource,
                                 int32_t width, int32_t height, uint32_t format, uint32_t flags) {
    params_create_common(client, resource, 0, width, height, format, flags, false);
}

static void params_handle_create_immed(struct wl_client *client, struct wl_resource *resource,
                                       uint32_t buffer_id, int32_t width, int32_t height,
                                       uint32_t format, uint32_t flags) {
    params_create_common(client, resource, buffer_id, width, height, format, flags, true);
}

static const struct zwp_linux_buffer_params_v1_interface params_impl = {
    .destroy = params_handle_destroy,
    .add = params_handle_add,
    .create = params_handle_create,
    .create_immed = params_handle_create_immed,
};

static void dmabuf_handle_destroy(struct wl_client *client, struct wl_resource *resource) {
    wl_resource_destroy(resource);
}

static void dmabuf_handle_create_params(struct wl_client *client, struct wl_resource *resource,
                                        uint32_t params_id) {
    purp_server_t *server = wl_resource_get_user_data(resource);
    dmabuf_params_t *params = calloc(1, sizeof(*params));
    if (!params) {
        wl_client_post_no_memory(client);
        return;
    }
    params->server = server;
    for (int i = 0; i < PURP_DMABUF_MAX_PLANES; ++i) params->attrs.fd[i] = -1;
    struct wl_resource *params_res =
        wl_resource_create(client, &zwp_linux_buffer_params_v1_interface,
                           wl_resource_get_version(resource), params_id);
    if (!params_res) {
        free(params);
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(params_res, &params_impl, params,
                                   params_handle_resource_destroy);
}

static void dmabuf_handle_get_default_feedback(struct wl_client *client,
                                               struct wl_resource *resource, uint32_t id) {
    dmabuf_create_feedback(client, resource, id);
}

static void dmabuf_handle_get_surface_feedback(struct wl_client *client,
                                               struct wl_resource *resource, uint32_t id,
                                               struct wl_resource *surface) {
    /* One GPU: the surface feedback equals the default feedback. */
    dmabuf_create_feedback(client, resource, id);
}

static const struct zwp_linux_dmabuf_v1_interface dmabuf_impl = {
    .destroy = dmabuf_handle_destroy,
    .create_params = dmabuf_handle_create_params,
    .get_default_feedback = dmabuf_handle_get_default_feedback,
    .get_surface_feedback = dmabuf_handle_get_surface_feedback,
};

static void dmabuf_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
    purp_server_t *server = data;
    struct wl_resource *resource =
        wl_resource_create(client, &zwp_linux_dmabuf_v1_interface, (int)version, id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &dmabuf_impl, server, NULL);
    if (version >= ZWP_LINUX_DMABUF_V1_GET_DEFAULT_FEEDBACK_SINCE_VERSION) return;
    for (size_t i = 0; i < server->dmabuf_format_count; ++i) {
        const purp_dmabuf_format_t *f = &server->dmabuf_formats[i];
        if (version >= ZWP_LINUX_DMABUF_V1_MODIFIER_SINCE_VERSION) {
            zwp_linux_dmabuf_v1_send_modifier(resource, f->format, (uint32_t)(f->modifier >> 32),
                                              (uint32_t)(f->modifier & 0xffffffffu));
            continue;
        }
        bool seen = false;
        for (size_t j = 0; j < i && !seen; ++j) seen = server->dmabuf_formats[j].format == f->format;
        if (!seen) zwp_linux_dmabuf_v1_send_format(resource, f->format);
    }
}

bool purp_dmabuf_init(purp_server_t *server, const purp_dmabuf_format_t *formats, size_t count,
                      dev_t main_device) {
    if (!server || !server->display) return false;
    purp_dmabuf_finish(server);
    if (count > 0) {
        server->dmabuf_formats = malloc(count * sizeof(*formats));
        if (!server->dmabuf_formats) return false;
        memcpy(server->dmabuf_formats, formats, count * sizeof(*formats));
    }
    server->dmabuf_format_count = count;
    server->dmabuf_main_device = main_device;
    /* Feedback (v4) needs a main device and a format table; otherwise advertise v3. */
    int version = ZWP_LINUX_DMABUF_V1_MODIFIER_SINCE_VERSION;
    if (main_device != 0 && dmabuf_build_table(server)) version = PURP_LINUX_DMABUF_VERSION;
    server->dmabuf_global = wl_global_create(server->display, &zwp_linux_dmabuf_v1_interface,
                                             version, server, dmabuf_bind);
    if (!server->dmabuf_global) {
        purp_dmabuf_finish(server);
        return false;
    }
    return true;
}

void purp_dmabuf_finish(purp_server_t *server) {
    if (!server) return;
    if (server->dmabuf_global) wl_global_destroy(server->dmabuf_global);
    server->dmabuf_global = NULL;
    if (server->dmabuf_table_fd >= 0) close(server->dmabuf_table_fd);
    server->dmabuf_table_fd = -1;
    server->dmabuf_table_size = 0;
    free(server->dmabuf_formats);
    server->dmabuf_formats = NULL;
    server->dmabuf_format_count = 0;
    server->dmabuf_main_device = 0;
}
