/* wl_seat (keyboard/pointer/touch), wl_output + xdg-output, cursor-shape, tablet stub,
 * clipboard/DnD/primary-selection/data-control data transfer, idle-inhibit. */
#include "purpwm.h"
#include "host_backend.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/mman.h>
#include <unistd.h>

#define PURP_DND_ACTIONS (WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY | \
                          WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE | \
                          WL_DATA_DEVICE_MANAGER_DND_ACTION_ASK)

/* =========================================================================================== */
/* Shared helpers                                                                             */
/* =========================================================================================== */

static purp_seat_t *server_seat(purp_server_t *s) { return &s->seat; }

static void remove_u32(uint32_t *codes, size_t *count, uint32_t code) {
    for (size_t i = 0; i < *count; ++i) {
        if (codes[i] == code) {
            codes[i] = codes[--*count];
            return;
        }
    }
}

static void add_u32(uint32_t *codes, size_t *count, size_t cap, uint32_t code) {
    for (size_t i = 0; i < *count; ++i)
        if (codes[i] == code) return;
    if (*count < cap) codes[(*count)++] = code;
}

/* Destructor for resources kept on a seat/output list via wl_resource_get_link(). */
static void resource_unlink(struct wl_resource *res) {
    wl_list_remove(wl_resource_get_link(res));
}

static bool surface_alive(purp_server_t *s, const purp_surface_t *surface) {
    if (!surface) return false;
    purp_surface_t *it;
    wl_list_for_each(it, &s->surfaces, link)
        if (it == surface) return true;
    return false;
}

static uint32_t next_serial(purp_server_t *s) {
    return wl_display_next_serial(s->display);
}


/* =========================================================================================== */
/* Role implementations (cursor surface + DnD icon live in the seat)                          */
/* =========================================================================================== */

static void cursor_role_destroy(purp_surface_t *surface) {
    purp_seat_t *seat = server_seat(surface->server);
    if (seat->cursor_surface == surface) {
        wl_list_remove(&seat->cursor_surface_destroy.link);
        seat->cursor_surface = NULL;
        seat->cursor_mode = PURP_CURSOR_MODE_NAMED;
    }
}

static const purp_surface_role_t cursor_role = {
    .name = "cursor", .kind = PURP_ROLE_CURSOR, .destroy = cursor_role_destroy,
};

static void dnd_icon_role_destroy(purp_surface_t *surface) {
    purp_seat_t *seat = server_seat(surface->server);
    if (seat->drag.icon == surface) seat->drag.icon = NULL;
}

static const purp_surface_role_t dnd_icon_role = {
    .name = "dnd-icon", .kind = PURP_ROLE_DND_ICON, .destroy = dnd_icon_role_destroy,
};

static void cursor_surface_died(struct wl_listener *listener, void *data) {
    purp_seat_t *seat = wl_container_of(listener, seat, cursor_surface_destroy);
    (void)data;
    seat->cursor_surface = NULL;
    seat->cursor_mode = PURP_CURSOR_MODE_NAMED;
    wl_list_remove(&seat->cursor_surface_destroy.link);
}

/* =========================================================================================== */
/* Keyboard focus / pointer focus listeners                                                   */
/* =========================================================================================== */

static void keyboard_focus_died(struct wl_listener *listener, void *data) {
    purp_seat_t *seat = wl_container_of(listener, seat, keyboard_focus_destroy);
    (void)data;
    seat->keyboard_focus = NULL;
    wl_list_remove(&seat->keyboard_focus_destroy.link);
}

static void pointer_focus_died(struct wl_listener *listener, void *data) {
    purp_seat_t *seat = wl_container_of(listener, seat, pointer_focus_destroy);
    (void)data;
    seat->pointer_focus = NULL;
    wl_list_remove(&seat->pointer_focus_destroy.link);
}

/* =========================================================================================== */
/* Data sources                                                                               */
/* =========================================================================================== */

/* First-member list entry: lets a source invalidate every live offer. */
typedef struct purp_offer_link {
    purp_data_source_t *source;
    struct wl_list source_link;             /* purp_source.offers */
} purp_offer_link_t;

/* All resource-backed purp_data_source_t objects are allocated through this wrapper. */
typedef struct purp_source {
    purp_data_source_t base;
    bool used;                              /* was a selection / drag source already */
    struct wl_list offers;                  /* purp_offer_link_t.source_link */
} purp_source_t;

typedef struct purp_offer {
    purp_offer_link_t link;
    struct wl_resource *resource;           /* wl_data_offer */
    purp_server_t *server;
    purp_blob_t *blob;                      /* compositor payload copy (source may be NULL) */
    bool finished;
} purp_offer_t;

/* zwp_primary_selection_offer_v1 / zwlr_data_control_offer_v1 share this shape. */
typedef struct purp_simple_offer {
    purp_offer_link_t link;
    struct wl_resource *resource;
    purp_server_t *server;
    bool primary;                           /* primary offer vs data-control offer */
} purp_simple_offer_t;


static purp_data_source_t *source_from_resource(struct wl_resource *res) {
    return res ? wl_resource_get_user_data(res) : NULL;
}

static purp_data_source_t *source_new(purp_server_t *s, purp_data_source_kind_t kind,
                                 struct wl_resource *res) {
    purp_source_t *src = calloc(1, sizeof(*src));
    if (!src) return NULL;
    src->base.server = s;
    src->base.kind = kind;
    src->base.resource = res;
    wl_list_init(&src->offers);
    if (res) wl_resource_set_user_data(res, &src->base);
    return &src->base;
}

static void source_add_mime(purp_data_source_t *base, const char *mime) {
    if (!mime) return;
    for (size_t i = 0; i < base->mime_count; ++i)
        if (!strcmp(base->mime_types[i], mime)) return;
    char *copy = strdup(mime);
    if (!copy) return;
    char **types = realloc(base->mime_types, (base->mime_count + 1) * sizeof(*types));
    if (!types) {
        free(copy);
        return;
    }
    base->mime_types = types;
    base->mime_types[base->mime_count++] = copy;
}

/* Every live offer created from `src` loses its source (receive fails silently). */
static void source_invalidate_offers(purp_source_t *src) {
    purp_offer_link_t *ol, *tmp;
    wl_list_for_each_safe(ol, tmp, &src->offers, source_link) {
        ol->source = NULL;
        wl_list_remove(&ol->source_link);
        wl_list_init(&ol->source_link);
    }
}

static void source_send_cancelled(purp_data_source_t *base) {
    if (!base || !base->resource) return;
    switch (base->kind) {
    case PURP_DATA_SOURCE_CLIENT:
        wl_data_source_send_cancelled(base->resource);
        break;
    case PURP_DATA_SOURCE_PRIMARY:
        zwp_primary_selection_source_v1_send_cancelled(base->resource);
        break;
    case PURP_DATA_SOURCE_CONTROL:
        zwlr_data_control_source_v1_send_cancelled(base->resource);
        break;
    case PURP_DATA_SOURCE_COMPOSITOR:
        break;
    }
}

static void source_free(purp_data_source_t *base) {
    if (!base) return;
    purp_source_t *src = wl_container_of(base, src, base);
    source_invalidate_offers(src);
    for (size_t i = 0; i < base->mime_count; ++i) free(base->mime_types[i]);
    free(base->mime_types);
    purp_blob_unref(base->blob);
    free(src);
}

static void source_detach_seat_refs(purp_data_source_t *base) {
    purp_seat_t *seat = server_seat(base->server);
    if (seat->selection == base) seat->selection = NULL;
    if (seat->primary_selection == base) seat->primary_selection = NULL;
    if (seat->drag.source == base) seat->drag.source = NULL;
}

/* wl_resource destroy of a client/primary/control source. */
static void source_resource_destroyed(struct wl_resource *res) {
    purp_data_source_t *base = source_from_resource(res);
    if (!base) return;
    base->resource = NULL;
    purp_server_t *s = base->server;
    purp_seat_t *seat = server_seat(s);
    bool was_selection = seat->selection == base;
    bool was_primary = seat->primary_selection == base;
    bool was_drag = seat->drag.active && seat->drag.source == base;
    source_detach_seat_refs(base);
    if (was_drag) purp_seat_drag_cancel(s);
    source_free(base);
    if (was_selection) purp_seat_set_selection(s, NULL);
    if (was_primary) purp_seat_set_primary_selection(s, NULL);
}

void purp_data_source_destroy(purp_data_source_t *base) {
    if (!base) return;
    source_detach_seat_refs(base);
    if (base->kind == PURP_DATA_SOURCE_COMPOSITOR) source_free(base);
    else source_send_cancelled(base);
}

/* SIGPIPE-safe write of a compositor blob to a paste fd, on a detached thread. */
typedef struct purp_blob_write {
    purp_blob_t *blob;
    int fd;
} purp_blob_write_t;

static void *compositor_blob_writer(void *arg) {
    purp_blob_write_t *job = arg;
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGPIPE);
    pthread_sigmask(SIG_BLOCK, &set, NULL);
    size_t off = 0;
    while (off < job->blob->len) {
        ssize_t n = write(job->fd, job->blob->data + off, job->blob->len - off);
        if (n > 0) {
            off += (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        break;                              /* EPIPE / error: give up quietly */
    }
    close(job->fd);
    purp_blob_unref(job->blob);
    free(job);
    return NULL;
}

static void compositor_send_to_fd(purp_data_source_t *base, int fd) {
    purp_blob_write_t *job = malloc(sizeof(*job));
    if (!job || !base->blob) {
        free(job);
        close(fd);
        return;
    }
    job->blob = purp_blob_ref(base->blob);
    job->fd = fd;
    pthread_t thread;
    if (pthread_create(&thread, NULL, compositor_blob_writer, job) != 0) {
        purp_blob_unref(job->blob);
        free(job);
        close(fd);
        return;
    }
    pthread_detach(thread);
}

void purp_data_source_send(purp_data_source_t *base, const char *mime, int fd) {
    if (fd < 0) return;
    if (!base) {
        close(fd);
        return;
    }
    switch (base->kind) {
    case PURP_DATA_SOURCE_CLIENT:
        if (base->resource) wl_data_source_send_send(base->resource, mime, fd);
        close(fd);
        break;
    case PURP_DATA_SOURCE_PRIMARY:
        if (base->resource) zwp_primary_selection_source_v1_send_send(base->resource, mime, fd);
        close(fd);
        break;
    case PURP_DATA_SOURCE_CONTROL:
        if (base->resource) zwlr_data_control_source_v1_send_send(base->resource, mime, fd);
        close(fd);
        break;
    case PURP_DATA_SOURCE_COMPOSITOR:
        compositor_send_to_fd(base, fd);
        break;
    }
}

/* =========================================================================================== */
/* wl_data_offer                                                                              */
/* =========================================================================================== */

static void offer_destroy_request(struct wl_client *client, struct wl_resource *res) {
    (void)client;
    wl_resource_destroy(res);
}

static void offer_resource_destroyed(struct wl_resource *res) {
    purp_offer_t *offer = wl_resource_get_user_data(res);
    if (!offer) return;
    purp_seat_t *seat = server_seat(offer->server);
    if (seat->drag.offer == res) seat->drag.offer = NULL;
    wl_list_remove(&offer->link.source_link);
    purp_blob_unref(offer->blob);
    free(offer);
}

static uint32_t offer_choose_action(purp_offer_t *offer, uint32_t dnd_actions,
                                    uint32_t preferred) {
    purp_data_source_t *base = offer->link.source;
    uint32_t both = dnd_actions & (base ? base->dnd_actions : 0) & PURP_DND_ACTIONS;
    if (preferred != WL_DATA_DEVICE_MANAGER_DND_ACTION_NONE && (both & preferred))
        return preferred;
    if (both & WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY)
        return WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY;
    if (both & WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE)
        return WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE;
    if (both & WL_DATA_DEVICE_MANAGER_DND_ACTION_ASK)
        return WL_DATA_DEVICE_MANAGER_DND_ACTION_ASK;
    return WL_DATA_DEVICE_MANAGER_DND_ACTION_NONE;
}

static void offer_accept(struct wl_client *client, struct wl_resource *res, uint32_t serial,
                         const char *mime_type) {
    (void)client;
    (void)serial;
    purp_offer_t *offer = wl_resource_get_user_data(res);
    if (!offer || !mime_type) return;
    purp_data_source_t *base = offer->link.source;
    if (!base || !base->resource) return;
    if (base->kind == PURP_DATA_SOURCE_CLIENT) {
        wl_data_source_send_target(base->resource, mime_type);
        base->accepted = true;
    }
}

static void offer_receive(struct wl_client *client, struct wl_resource *res,
                          const char *mime_type, int32_t fd) {
    (void)client;
    purp_offer_t *offer = wl_resource_get_user_data(res);
    if (!offer) {
        close(fd);
        return;
    }
    if (offer->link.source) {
        purp_data_source_send(offer->link.source, mime_type, fd);
        return;
    }
    if (offer->blob) {
        purp_blob_t *blob = purp_blob_ref(offer->blob);
        purp_blob_write_t *job = malloc(sizeof(*job));
        if (!job) {
            purp_blob_unref(blob);
            close(fd);
            return;
        }
        job->blob = blob;
        job->fd = fd;
        pthread_t thread;
        if (pthread_create(&thread, NULL, compositor_blob_writer, job) != 0) {
            purp_blob_unref(blob);
            free(job);
            close(fd);
            return;
        }
        pthread_detach(thread);
        return;
    }
    close(fd);
}

static void offer_finish(struct wl_client *client, struct wl_resource *res) {
    (void)client;
    purp_offer_t *offer = wl_resource_get_user_data(res);
    if (!offer) return;
    purp_data_source_t *base = offer->link.source;
    if (!base || base->kind != PURP_DATA_SOURCE_CLIENT || !base->dnd || !base->accepted) {
        wl_resource_post_error(res, WL_DATA_OFFER_ERROR_INVALID_FINISH,
                               "wl_data_offer.finish is only valid for an accepted drag offer");
        return;
    }
    if (offer->finished) {
        wl_resource_post_error(res, WL_DATA_OFFER_ERROR_INVALID_FINISH,
                               "wl_data_offer.finish called twice");
        return;
    }
    offer->finished = true;
    if (base->resource && wl_resource_get_version(base->resource) >=
        WL_DATA_SOURCE_DND_FINISHED_SINCE_VERSION)
        wl_data_source_send_dnd_finished(base->resource);
}

static void offer_set_actions(struct wl_client *client, struct wl_resource *res,
                              uint32_t dnd_actions, uint32_t preferred_action) {
    (void)client;
    purp_offer_t *offer = wl_resource_get_user_data(res);
    if (!offer) return;
    purp_data_source_t *base = offer->link.source;
    if (!base || base->kind != PURP_DATA_SOURCE_CLIENT || !base->dnd) return;
    if (dnd_actions & ~PURP_DND_ACTIONS) {
        wl_resource_post_error(res, WL_DATA_OFFER_ERROR_INVALID_ACTION_MASK,
                               "wl_data_offer.set_actions has bits outside copy/move/ask");
        return;
    }
    if (preferred_action != WL_DATA_DEVICE_MANAGER_DND_ACTION_NONE &&
        !(preferred_action & PURP_DND_ACTIONS)) {
        wl_resource_post_error(res, WL_DATA_OFFER_ERROR_INVALID_ACTION,
                               "wl_data_offer.set_actions preferred action is not a valid action");
        return;
    }
    uint32_t action = offer_choose_action(offer, dnd_actions, preferred_action);
    if (action == base->current_dnd_action) return;
    base->current_dnd_action = action;
    if (base->resource &&
        wl_resource_get_version(base->resource) >= WL_DATA_SOURCE_ACTION_SINCE_VERSION)
        wl_data_source_send_action(base->resource, action);
    if (wl_resource_get_version(res) >= WL_DATA_OFFER_ACTION_SINCE_VERSION)
        wl_data_offer_send_action(res, action);
}

static const struct wl_data_offer_interface offer_impl = {
    .accept = offer_accept,
    .receive = offer_receive,
    .destroy = offer_destroy_request,
    .finish = offer_finish,
    .set_actions = offer_set_actions,
};

static struct wl_resource *offer_new(purp_server_t *s, purp_data_source_t *base,
                                     struct wl_client *client, struct wl_resource *device) {
    purp_offer_t *offer = calloc(1, sizeof(*offer));
    if (!offer) return NULL;
    offer->server = s;
    offer->link.source = base;
    if (base && base->kind == PURP_DATA_SOURCE_COMPOSITOR) {
        offer->link.source = NULL;          /* never touch the compositor source again */
        offer->blob = purp_blob_ref(base->blob);
    }
    uint32_t version = wl_resource_get_version(device);
    struct wl_resource *res =
        wl_resource_create(client, &wl_data_offer_interface, (int)version, 0);
    if (!res) {
        purp_blob_unref(offer->blob);
        free(offer);
        return NULL;
    }
    offer->resource = res;
    wl_resource_set_implementation(res, &offer_impl, offer, offer_resource_destroyed);
    if (base && base->kind != PURP_DATA_SOURCE_COMPOSITOR) {
        purp_source_t *src = wl_container_of(base, src, base);
        wl_list_insert(&src->offers, &offer->link.source_link);
    } else {
        wl_list_init(&offer->link.source_link);
    }
    wl_data_device_send_data_offer(device, res);
    for (size_t i = 0; base && i < base->mime_count; ++i)
        wl_data_offer_send_offer(res, base->mime_types[i]);
    if (base && base->kind == PURP_DATA_SOURCE_CLIENT && base->dnd &&
        version >= WL_DATA_OFFER_SOURCE_ACTIONS_SINCE_VERSION)
        wl_data_offer_send_source_actions(res, base->dnd_actions);
    return res;
}

/* =========================================================================================== */
/* Primary / data-control offers                                                              */
/* =========================================================================================== */

static void simple_offer_resource_destroyed(struct wl_resource *res) {
    purp_simple_offer_t *offer = wl_resource_get_user_data(res);
    if (!offer) return;
    wl_list_remove(&offer->link.source_link);
    free(offer);
}

static void simple_offer_receive(struct wl_client *client, struct wl_resource *res,
                                 const char *mime_type, int32_t fd) {
    (void)client;
    purp_simple_offer_t *offer = wl_resource_get_user_data(res);
    purp_data_source_t *base = offer ? offer->link.source : NULL;
    if (!base) {
        close(fd);
        return;
    }
    purp_data_source_send(base, mime_type, fd);
}

static const struct zwp_primary_selection_offer_v1_interface primary_offer_impl = {
    .receive = simple_offer_receive,
    .destroy = offer_destroy_request,
};

static const struct zwlr_data_control_offer_v1_interface control_offer_impl = {
    .receive = simple_offer_receive,
    .destroy = offer_destroy_request,
};

static struct wl_resource *simple_offer_new(purp_server_t *s, purp_data_source_t *base,
                                            struct wl_resource *device, bool primary) {
    struct wl_client *client = wl_resource_get_client(device);
    uint32_t version = (uint32_t)wl_resource_get_version(device);
    purp_simple_offer_t *offer = calloc(1, sizeof(*offer));
    if (!offer) return NULL;
    offer->server = s;
    offer->link.source = base;
    offer->primary = primary;
    const struct wl_interface *iface =
        primary ? &zwp_primary_selection_offer_v1_interface
                : &zwlr_data_control_offer_v1_interface;
    struct wl_resource *res = wl_resource_create(client, iface, (int)version, 0);
    if (!res) {
        free(offer);
        return NULL;
    }
    offer->resource = res;
    wl_resource_set_implementation(res, primary ? (const void *)&primary_offer_impl
                                              : (const void *)&control_offer_impl,
                                   offer, simple_offer_resource_destroyed);
    if (base) {
        purp_source_t *src = wl_container_of(base, src, base);
        wl_list_insert(&src->offers, &offer->link.source_link);
    } else {
        wl_list_init(&offer->link.source_link);
    }
    if (primary) zwp_primary_selection_device_v1_send_data_offer(device, res);
    else zwlr_data_control_device_v1_send_data_offer(device, res);
    for (size_t i = 0; base && i < base->mime_count; ++i) {
        if (primary) zwp_primary_selection_offer_v1_send_offer(res, base->mime_types[i]);
        else zwlr_data_control_offer_v1_send_offer(res, base->mime_types[i]);
    }
    return res;
}

/* =========================================================================================== */
/* Selection broadcasting                                                                     */
/* =========================================================================================== */

static void seat_broadcast_selection(purp_server_t *s) {
    purp_seat_t *seat = server_seat(s);
    struct wl_client *focus = seat->keyboard_focus && seat->keyboard_focus->resource
        ? wl_resource_get_client(seat->keyboard_focus->resource) : NULL;
    if (focus) {
        struct wl_resource *dev;
        wl_list_for_each(dev, &seat->data_devices, link) {
            if (wl_resource_get_client(dev) != focus) continue;
            struct wl_resource *offer = NULL;
            if (seat->selection) {
                offer = offer_new(s, seat->selection, focus, dev);
            }
            wl_data_device_send_selection(dev, offer);
        }
    }
    struct wl_resource *dev;
    wl_list_for_each(dev, &seat->data_control_devices, link) {
        struct wl_resource *offer = NULL;
        if (seat->selection)
            offer = simple_offer_new(s, seat->selection, dev, false);
        zwlr_data_control_device_v1_send_selection(dev, offer);
    }
}

static void seat_broadcast_primary_selection(purp_server_t *s) {
    purp_seat_t *seat = server_seat(s);
    struct wl_client *focus = seat->keyboard_focus && seat->keyboard_focus->resource
        ? wl_resource_get_client(seat->keyboard_focus->resource) : NULL;
    if (focus) {
        struct wl_resource *dev;
        wl_list_for_each(dev, &seat->primary_devices, link) {
            if (wl_resource_get_client(dev) != focus) continue;
            struct wl_resource *offer = NULL;
            if (seat->primary_selection)
                offer = simple_offer_new(s, seat->primary_selection, dev, true);
            zwp_primary_selection_device_v1_send_selection(dev, offer);
        }
    }
    struct wl_resource *dev;
    wl_list_for_each(dev, &seat->data_control_devices, link) {
        if (wl_resource_get_version(dev) <
            ZWLR_DATA_CONTROL_DEVICE_V1_PRIMARY_SELECTION_SINCE_VERSION)
            continue;
        struct wl_resource *offer = NULL;
        if (seat->primary_selection)
            offer = simple_offer_new(s, seat->primary_selection, dev, false);
        zwlr_data_control_device_v1_send_primary_selection(dev, offer);
    }
}

void purp_seat_set_selection(purp_server_t *s, purp_data_source_t *source) {
    if (!s) return;
    purp_seat_t *seat = server_seat(s);
    if (seat->selection == source) {
        seat_broadcast_selection(s);
        return;
    }
    purp_data_source_t *old = seat->selection;
    seat->selection = source;
    if (old) purp_data_source_destroy(old);
    seat_broadcast_selection(s);
}

void purp_seat_set_primary_selection(purp_server_t *s, purp_data_source_t *source) {
    if (!s) return;
    purp_seat_t *seat = server_seat(s);
    if (seat->primary_selection == source) {
        seat_broadcast_primary_selection(s);
        return;
    }
    purp_data_source_t *old = seat->primary_selection;
    seat->primary_selection = source;
    if (old) purp_data_source_destroy(old);
    seat_broadcast_primary_selection(s);
}

void purp_seat_set_compositor_selection(purp_server_t *s, const char *mime, purp_blob_t *blob) {
    if (!s || !blob) return;
    purp_data_source_t *base = source_new(s, PURP_DATA_SOURCE_COMPOSITOR, NULL);
    if (!base) return;
    source_add_mime(base, mime ? mime : "application/octet-stream");
    base->blob = purp_blob_ref(blob);
    purp_seat_set_selection(s, base);
}

/* =========================================================================================== */
/* wl_data_source                                                                             */
/* =========================================================================================== */

static void wl_source_offer(struct wl_client *client, struct wl_resource *res,
                            const char *mime_type) {
    (void)client;
    purp_data_source_t *base = source_from_resource(res);
    if (!base) return;
    source_add_mime(base, mime_type);
}

static void wl_source_destroy_request(struct wl_client *client, struct wl_resource *res) {
    (void)client;
    wl_resource_destroy(res);
}

static void wl_source_set_actions(struct wl_client *client, struct wl_resource *res,
                                  uint32_t dnd_actions) {
    (void)client;
    purp_data_source_t *base = source_from_resource(res);
    if (!base) return;
    if (dnd_actions & ~PURP_DND_ACTIONS) {
        wl_resource_post_error(res, WL_DATA_SOURCE_ERROR_INVALID_ACTION_MASK,
                               "wl_data_source.set_actions has bits outside copy/move/ask");
        return;
    }
    base->dnd_actions = dnd_actions;
}

static const struct wl_data_source_interface wl_source_impl = {
    .offer = wl_source_offer,
    .destroy = wl_source_destroy_request,
    .set_actions = wl_source_set_actions,
};

static purp_data_source_t *client_source_new(purp_server_t *s, struct wl_client *client,
                                             uint32_t version, uint32_t id) {
    struct wl_resource *res =
        wl_resource_create(client, &wl_data_source_interface, (int)version, (uint32_t)id);
    if (!res) return NULL;
    purp_data_source_t *base = source_new(s, PURP_DATA_SOURCE_CLIENT, res);
    if (!base) {
        wl_resource_destroy(res);
        return NULL;
    }
    wl_resource_set_implementation(res, &wl_source_impl, base, source_resource_destroyed);
    return base;
}

/* =========================================================================================== */
/* Drag-and-drop                                                                              */
/* =========================================================================================== */

/* The drag's focus surface may die mid-drag; keep drag.focus honest. */
typedef struct purp_drag_focus_listener {
    struct wl_listener listener;
    purp_server_t *server;
} purp_drag_focus_listener_t;

static void drag_focus_died(struct wl_listener *listener, void *data) {
    purp_drag_focus_listener_t *box = wl_container_of(listener, box, listener);
    (void)data;
    purp_seat_t *seat = server_seat(box->server);
    if (seat->drag.focus_device) {
        wl_data_device_send_leave(seat->drag.focus_device);
        seat->drag.focus_device = NULL;
    }
    seat->drag.offer = NULL;
    seat->drag.focus = NULL;
    wl_list_remove(&listener->link);
    free(box);
}

static void drag_focus_unlisten(purp_seat_t *seat, purp_surface_t *surface) {
    (void)seat;
    /* Only drag-focus listener boxes carry this notify; never free foreign listeners. */
    struct wl_listener *listener, *tmp;
    wl_list_for_each_safe(listener, tmp, &surface->events_destroy.listener_list, link) {
        if (listener->notify != drag_focus_died) continue;
        purp_drag_focus_listener_t *box = wl_container_of(listener, box, listener);
        wl_list_remove(&listener->link);
        free(box);
    }
}

static bool drag_focus_listen(purp_server_t *s, purp_surface_t *surface) {
    if (!surface) return true;
    purp_drag_focus_listener_t *box = calloc(1, sizeof(*box));
    if (!box) return false;
    box->server = s;
    box->listener.notify = drag_focus_died;
    wl_signal_add(&surface->events_destroy, &box->listener);
    return true;
}

static void drag_origin_died(struct wl_listener *listener, void *data) {
    purp_drag_focus_listener_t *box = wl_container_of(listener, box, listener);
    purp_server_t *s = box->server;
    (void)data;
    wl_list_remove(&listener->link);
    free(box);
    s->seat.drag.origin = NULL;
    purp_seat_drag_cancel(s);
}

static void drag_origin_unlisten(purp_seat_t *seat) {
    if (!seat->drag.origin) return;
    struct wl_listener *listener, *tmp;
    wl_list_for_each_safe(listener, tmp,
                         &seat->drag.origin->events_destroy.listener_list, link) {
        if (listener->notify != drag_origin_died) continue;
        purp_drag_focus_listener_t *box = wl_container_of(listener, box, listener);
        wl_list_remove(&listener->link);
        free(box);
    }
}

static bool drag_origin_listen(purp_server_t *s, purp_surface_t *surface) {
    purp_drag_focus_listener_t *box = calloc(1, sizeof(*box));
    if (!box) return false;
    box->server = s;
    box->listener.notify = drag_origin_died;
    wl_signal_add(&surface->events_destroy, &box->listener);
    return true;
}

bool purp_seat_drag_active(const purp_server_t *s) {
    return s && s->seat.drag.active;
}

static struct wl_resource *drag_device_for(purp_seat_t *seat, struct wl_client *client) {
    struct wl_resource *dev;
    wl_list_for_each(dev, &seat->data_devices, link)
        if (wl_resource_get_client(dev) == client) return dev;
    return NULL;
}

static void drag_set_focus(purp_server_t *s, purp_surface_t *surface, purp_pointf_t origin,
                           uint32_t time_ms) {
    purp_seat_t *seat = server_seat(s);
    purp_drag_t *drag = &seat->drag;
    if (surface && !drag->source &&
        (!drag->origin || !drag->origin->resource || !surface->resource ||
         wl_resource_get_client(surface->resource) !=
             wl_resource_get_client(drag->origin->resource)))
        surface = NULL;
    if (drag->focus == surface) {
        drag->focus_origin = origin;
        if (drag->focus_device && surface)
            wl_data_device_send_motion(drag->focus_device, time_ms,
                                       wl_fixed_from_double(s->wm.pointer_location.x - origin.x),
                                       wl_fixed_from_double(s->wm.pointer_location.y - origin.y));
        return;
    }
    if (drag->focus) drag_focus_unlisten(seat, drag->focus);
    if (drag->focus_device) {
        wl_data_device_send_leave(drag->focus_device);
        drag->focus_device = NULL;
    }
    drag->offer = NULL;
    if (!drag_focus_listen(s, surface)) surface = NULL;
    drag->focus = surface;
    drag->focus_origin = origin;
    if (!surface || !surface->resource) return;
    struct wl_client *client = wl_resource_get_client(surface->resource);
    struct wl_resource *dev = drag_device_for(seat, client);
    if (!dev) return;
    drag->focus_device = dev;
    if (drag->source) {
        drag->offer = offer_new(s, drag->source, client, dev);
    }
    wl_data_device_send_enter(dev, next_serial(s), surface->resource,
                              wl_fixed_from_double(s->wm.pointer_location.x - origin.x),
                              wl_fixed_from_double(s->wm.pointer_location.y - origin.y),
                              drag->offer);
    if (drag->source && drag->source->resource &&
        wl_resource_get_version(drag->source->resource) >=
            WL_DATA_SOURCE_DND_DROP_PERFORMED_SINCE_VERSION &&
        !drag->source->accepted)
        wl_data_source_send_target(drag->source->resource, NULL);
}

void purp_seat_drag_motion(purp_server_t *s, uint32_t time_ms) {
    if (!s || !s->seat.drag.active) return;
    purp_surface_t *target = NULL;
    purp_pointf_t origin = {0};
    if (!purp_server_is_locked(s))
        target = purp_input_surface_under(s, s->wm.pointer_location, &origin);
    drag_set_focus(s, target, origin, time_ms);
}

static void drag_reset(purp_seat_t *seat, bool send_leave) {
    drag_origin_unlisten(seat);
    if (seat->drag.focus) drag_focus_unlisten(seat, seat->drag.focus);
    if (seat->drag.focus_device) {
        if (send_leave) wl_data_device_send_leave(seat->drag.focus_device);
        seat->drag.focus_device = NULL;
    }
    seat->drag.offer = NULL;
    seat->drag.source = NULL;
    seat->drag.active = false;
    seat->drag.origin = NULL;
    seat->drag.icon = NULL;
    seat->drag.focus = NULL;
    seat->drag.is_touch = false;
    seat->drag.touch_slot = -1;
    seat->drag.serial = 0;
}

void purp_seat_drag_drop(purp_server_t *s, uint32_t time_ms) {
    (void)time_ms;
    if (!s || !s->seat.drag.active) return;
    purp_seat_t *seat = server_seat(s);
    if (seat->drag.focus_device) {
        wl_data_device_send_drop(seat->drag.focus_device);
        if (seat->drag.source && seat->drag.source->resource &&
            wl_resource_get_version(seat->drag.source->resource) >=
                WL_DATA_SOURCE_DND_DROP_PERFORMED_SINCE_VERSION)
            wl_data_source_send_dnd_drop_performed(seat->drag.source->resource);
    }
    drag_reset(seat, false);                /* no leave after wl_data_device.drop */
}

void purp_seat_drag_cancel(purp_server_t *s) {
    if (!s || !s->seat.drag.active) return;
    purp_seat_t *seat = server_seat(s);
    purp_data_source_t *base = seat->drag.source;
    seat->drag.source = NULL;
    drag_reset(seat, true);
    purp_data_source_destroy(base);
}

/* =========================================================================================== */
/* wl_data_device                                                                             */
/* =========================================================================================== */

static purp_server_t *device_server(struct wl_resource *device) {
    return wl_resource_get_user_data(device);
}

static void data_device_destroyed(struct wl_resource *res) {
    purp_server_t *s = device_server(res);
    if (s && s->seat.drag.focus_device == res) {
        s->seat.drag.focus_device = NULL;
        s->seat.drag.offer = NULL;
    }
    resource_unlink(res);
}

static void data_device_start_drag(struct wl_client *client, struct wl_resource *res,
                                   struct wl_resource *source, struct wl_resource *origin,
                                   struct wl_resource *icon, uint32_t serial) {
    (void)client;
    purp_server_t *s = device_server(res);
    if (!s) return;
    purp_seat_t *seat = server_seat(s);
    purp_surface_t *origin_surface = purp_surface_from_resource(origin);
    if (!origin_surface) return;
    purp_data_source_t *base = source_from_resource(source);
    if (base) {
        purp_source_t *src = wl_container_of(base, src, base);
        if (src->used) {
            wl_resource_post_error(res, WL_DATA_DEVICE_ERROR_USED_SOURCE,
                                   "wl_data_source already used as selection or drag source");
            return;
        }
    }
    if (seat->drag.active) purp_seat_drag_cancel(s);
    purp_surface_t *icon_surface = NULL;
    if (icon) {
        icon_surface = purp_surface_from_resource(icon);
        if (!icon_surface) return;
        if (!purp_surface_set_role(icon_surface, &dnd_icon_role, NULL, res,
                                   WL_DATA_DEVICE_ERROR_ROLE))
            return;
    }
    bool pointer_serial = serial == seat->last_button_serial;
    bool touch_serial = serial == seat->last_touch_serial;
    if (!pointer_serial && !touch_serial) {
        if (icon_surface) purp_surface_clear_role_data(icon_surface);
        return;
    }
    if (base) {
        purp_source_t *src = wl_container_of(base, src, base);
        src->used = true;
        base->dnd = true;
        base->accepted = false;
        base->current_dnd_action = WL_DATA_DEVICE_MANAGER_DND_ACTION_NONE;
        if (base->resource &&
            wl_resource_get_version(base->resource) >= WL_DATA_SOURCE_ACTION_SINCE_VERSION &&
            base->dnd_actions)
            wl_data_source_send_action(base->resource,
                                       base->dnd_actions & WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY
                                           ? WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY
                                           : base->dnd_actions & PURP_DND_ACTIONS);
    }
    purp_drag_t *drag = &seat->drag;
    *drag = (purp_drag_t){0};
    drag->active = true;
    drag->source = base;
    drag->origin = origin_surface;
    drag->icon = icon_surface;
    if (!drag_origin_listen(s, origin_surface)) {
        purp_seat_drag_cancel(s);
        wl_resource_post_no_memory(res);
        return;
    }
    drag->is_touch = !pointer_serial && touch_serial;
    drag->touch_slot = -1;
    if (drag->is_touch) {
        for (size_t i = 0; i < PURP_MAX_TOUCH_POINTS; ++i)
            if (seat->touch[i].active) drag->touch_slot = seat->touch[i].slot;
    }
    drag->serial = serial;
    purp_surface_t *initial_focus = seat->pointer_focus;
    purp_pointf_t initial_origin = seat->pointer_focus_origin;
    if (drag->is_touch && drag->touch_slot >= 0) {
        purp_touch_point_t *point = purp_seat_touch_point(s, drag->touch_slot);
        if (point) {
            initial_focus = point->surface;
            initial_origin = point->surface_origin;
        }
    }
    drag_set_focus(s, initial_focus, initial_origin, purp_server_time_ms(s));
}

static void data_device_set_selection(struct wl_client *client, struct wl_resource *res,
                                      struct wl_resource *source, uint32_t serial) {
    (void)client;
    (void)serial;
    purp_server_t *s = device_server(res);
    if (!s) return;
    purp_data_source_t *base = source_from_resource(source);
    if (base) {
        purp_source_t *src = wl_container_of(base, src, base);
        if (src->used) {
            wl_resource_post_error(res, WL_DATA_DEVICE_ERROR_USED_SOURCE,
                                   "wl_data_source already used as selection or drag source");
            return;
        }
        src->used = true;
    }
    purp_seat_set_selection(s, base);
}

static const struct wl_data_device_interface data_device_impl = {
    .start_drag = data_device_start_drag,
    .set_selection = data_device_set_selection,
    .release = offer_destroy_request,
};

/* =========================================================================================== */
/* wl_data_device_manager                                                                     */
/* =========================================================================================== */

static void ddm_create_data_source(struct wl_client *client, struct wl_resource *res,
                                   uint32_t id) {
    purp_server_t *s = wl_resource_get_user_data(res);
    purp_data_source_t *base =
        client_source_new(s, client, (uint32_t)wl_resource_get_version(res), id);
    if (!base) wl_resource_post_no_memory(res);
}

static void ddm_get_data_device(struct wl_client *client, struct wl_resource *res, uint32_t id,
                                struct wl_resource *seat_res) {
    (void)seat_res;
    purp_server_t *s = wl_resource_get_user_data(res);
    purp_seat_t *seat = server_seat(s);
    struct wl_resource *dev = wl_resource_create(client, &wl_data_device_interface,
                                               wl_resource_get_version(res), (uint32_t)id);
    if (!dev) {
        wl_resource_post_no_memory(res);
        return;
    }
    wl_resource_set_implementation(dev, &data_device_impl, s, data_device_destroyed);
    wl_list_insert(&seat->data_devices, wl_resource_get_link(dev));
    if (seat->keyboard_focus && seat->keyboard_focus->resource &&
        wl_resource_get_client(seat->keyboard_focus->resource) == client &&
        seat->selection) {
        struct wl_resource *offer = offer_new(s, seat->selection, client, dev);
        wl_data_device_send_selection(dev, offer);
    }
}

static const struct wl_data_device_manager_interface ddm_impl = {
    .create_data_source = ddm_create_data_source,
    .get_data_device = ddm_get_data_device,
    .release = offer_destroy_request,
};

static void ddm_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
    purp_server_t *s = data;
    struct wl_resource *res = wl_resource_create(client, &wl_data_device_manager_interface,
                                               (int)version, id);
    if (!res) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(res, &ddm_impl, s, NULL);
}

/* =========================================================================================== */
/* zwp_primary_selection                                                                      */
/* =========================================================================================== */

static void primary_source_offer(struct wl_client *client, struct wl_resource *res,
                                 const char *mime_type) {
    (void)client;
    purp_data_source_t *base = source_from_resource(res);
    if (!base) return;
    source_add_mime(base, mime_type);
}

static const struct zwp_primary_selection_source_v1_interface primary_source_impl = {
    .offer = primary_source_offer,
    .destroy = offer_destroy_request,
};

static void primary_device_set_selection(struct wl_client *client, struct wl_resource *res,
                                         struct wl_resource *source, uint32_t serial) {
    (void)client;
    (void)serial;
    purp_server_t *s = device_server(res);
    if (!s) return;
    purp_data_source_t *base = source_from_resource(source);
    if (base && base->kind != PURP_DATA_SOURCE_PRIMARY) return;
    if (base) {
        /* The protocol has no used-source error; a reused source is just ignored. */
        purp_source_t *src = wl_container_of(base, src, base);
        if (src->used) return;
        src->used = true;
    }
    purp_seat_set_primary_selection(s, base);
}

static const struct zwp_primary_selection_device_v1_interface primary_device_impl = {
    .set_selection = primary_device_set_selection,
    .destroy = offer_destroy_request,
};

static void pdm_create_source(struct wl_client *client, struct wl_resource *res, uint32_t id) {
    purp_server_t *s = wl_resource_get_user_data(res);
    struct wl_resource *src_res =
        wl_resource_create(client, &zwp_primary_selection_source_v1_interface,
                           wl_resource_get_version(res), (uint32_t)id);
    if (!src_res) {
        wl_resource_post_no_memory(res);
        return;
    }
    purp_data_source_t *base = source_new(s, PURP_DATA_SOURCE_PRIMARY, src_res);
    if (!base) {
        wl_resource_destroy(src_res);
        wl_resource_post_no_memory(res);
        return;
    }
    wl_resource_set_implementation(src_res, &primary_source_impl, base,
                                   source_resource_destroyed);
}

static void pdm_get_device(struct wl_client *client, struct wl_resource *res, uint32_t id,
                           struct wl_resource *seat_res) {
    (void)seat_res;
    purp_server_t *s = wl_resource_get_user_data(res);
    purp_seat_t *seat = server_seat(s);
    struct wl_resource *dev =
        wl_resource_create(client, &zwp_primary_selection_device_v1_interface,
                           wl_resource_get_version(res), (uint32_t)id);
    if (!dev) {
        wl_resource_post_no_memory(res);
        return;
    }
    wl_resource_set_implementation(dev, &primary_device_impl, s, resource_unlink);
    wl_list_insert(&seat->primary_devices, wl_resource_get_link(dev));
    purp_seat_t *st = server_seat(s);
    if (st->keyboard_focus && st->keyboard_focus->resource &&
        wl_resource_get_client(st->keyboard_focus->resource) == client &&
        st->primary_selection) {
        struct wl_resource *offer =
            simple_offer_new(s, st->primary_selection, dev, true);
        zwp_primary_selection_device_v1_send_selection(dev, offer);
    }
}

static const struct zwp_primary_selection_device_manager_v1_interface pdm_impl = {
    .create_source = pdm_create_source,
    .get_device = pdm_get_device,
    .destroy = offer_destroy_request,
};

static void pdm_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
    purp_server_t *s = data;
    struct wl_resource *res =
        wl_resource_create(client, &zwp_primary_selection_device_manager_v1_interface,
                           (int)version, id);
    if (!res) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(res, &pdm_impl, s, NULL);
}

/* =========================================================================================== */
/* zwlr_data_control_v1                                                                       */
/* =========================================================================================== */

static void control_source_offer(struct wl_client *client, struct wl_resource *res,
                                 const char *mime_type) {
    (void)client;
    purp_data_source_t *base = source_from_resource(res);
    if (!base) return;
    purp_source_t *src = wl_container_of(base, src, base);
    if (src->used) {
        wl_resource_post_error(res, ZWLR_DATA_CONTROL_SOURCE_V1_ERROR_INVALID_OFFER,
                               "zwlr_data_control_source_v1.offer after set_selection");
        return;
    }
    source_add_mime(base, mime_type);
}

static const struct zwlr_data_control_source_v1_interface control_source_impl = {
    .offer = control_source_offer,
    .destroy = offer_destroy_request,
};

static purp_data_source_t *control_source_from(struct wl_resource *res) {
    purp_data_source_t *base = source_from_resource(res);
    return (base && base->kind == PURP_DATA_SOURCE_CONTROL) ? base : NULL;
}

static void control_device_set_selection(struct wl_client *client, struct wl_resource *res,
                                         struct wl_resource *source) {
    (void)client;
    purp_server_t *s = device_server(res);
    if (!s) return;
    purp_data_source_t *base = control_source_from(source);
    if (source && !base) {
        wl_resource_post_error(res, ZWLR_DATA_CONTROL_DEVICE_V1_ERROR_USED_SOURCE,
                               "set_selection source is not a zwlr_data_control_source_v1");
        return;
    }
    if (base) {
        purp_source_t *src = wl_container_of(base, src, base);
        if (src->used) {
            wl_resource_post_error(res, ZWLR_DATA_CONTROL_DEVICE_V1_ERROR_USED_SOURCE,
                                   "zwlr_data_control_source_v1 already used");
            return;
        }
        src->used = true;
    }
    purp_seat_set_selection(s, base);
}

static void control_device_set_primary(struct wl_client *client, struct wl_resource *res,
                                       struct wl_resource *source) {
    (void)client;
    purp_server_t *s = device_server(res);
    if (!s) return;
    purp_data_source_t *base = control_source_from(source);
    if (source && !base) {
        wl_resource_post_error(res, ZWLR_DATA_CONTROL_DEVICE_V1_ERROR_USED_SOURCE,
                               "set_primary_selection source is not a zwlr_data_control_source_v1");
        return;
    }
    if (base) {
        purp_source_t *src = wl_container_of(base, src, base);
        if (src->used) {
            wl_resource_post_error(res, ZWLR_DATA_CONTROL_DEVICE_V1_ERROR_USED_SOURCE,
                                   "zwlr_data_control_source_v1 already used");
            return;
        }
        src->used = true;
    }
    purp_seat_set_primary_selection(s, base);
}

static const struct zwlr_data_control_device_v1_interface control_device_impl = {
    .set_selection = control_device_set_selection,
    .destroy = offer_destroy_request,
    .set_primary_selection = control_device_set_primary,
};

static void dcm_create_source(struct wl_client *client, struct wl_resource *res, uint32_t id) {
    purp_server_t *s = wl_resource_get_user_data(res);
    struct wl_resource *src_res =
        wl_resource_create(client, &zwlr_data_control_source_v1_interface,
                           wl_resource_get_version(res), (uint32_t)id);
    if (!src_res) {
        wl_resource_post_no_memory(res);
        return;
    }
    purp_data_source_t *base = source_new(s, PURP_DATA_SOURCE_CONTROL, src_res);
    if (!base) {
        wl_resource_destroy(src_res);
        wl_resource_post_no_memory(res);
        return;
    }
    wl_resource_set_implementation(src_res, &control_source_impl, base,
                                   source_resource_destroyed);
}

static void dcm_get_device(struct wl_client *client, struct wl_resource *res, uint32_t id,
                           struct wl_resource *seat_res) {
    (void)seat_res;
    purp_server_t *s = wl_resource_get_user_data(res);
    purp_seat_t *seat = server_seat(s);
    struct wl_resource *dev =
        wl_resource_create(client, &zwlr_data_control_device_v1_interface,
                           wl_resource_get_version(res), (uint32_t)id);
    if (!dev) {
        wl_resource_post_no_memory(res);
        return;
    }
    wl_resource_set_implementation(dev, &control_device_impl, s, resource_unlink);
    wl_list_insert(&seat->data_control_devices, wl_resource_get_link(dev));
    if (seat->selection) {
        struct wl_resource *offer =
            simple_offer_new(s, seat->selection, dev, false);
        zwlr_data_control_device_v1_send_selection(dev, offer);
    }
    if (seat->primary_selection &&
        wl_resource_get_version(dev) >=
            ZWLR_DATA_CONTROL_DEVICE_V1_PRIMARY_SELECTION_SINCE_VERSION) {
        struct wl_resource *offer =
            simple_offer_new(s, seat->primary_selection, dev, false);
        zwlr_data_control_device_v1_send_primary_selection(dev, offer);
    }
}

static const struct zwlr_data_control_manager_v1_interface dcm_impl = {
    .create_data_source = dcm_create_source,
    .get_data_device = dcm_get_device,
    .destroy = offer_destroy_request,
};

static void dcm_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
    purp_server_t *s = data;
    struct wl_resource *res = wl_resource_create(client, &zwlr_data_control_manager_v1_interface,
                                               (int)version, id);
    if (!res) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(res, &dcm_impl, s, NULL);
}

/* =========================================================================================== */
/* wp_cursor_shape_manager_v1                                                                 */
/* =========================================================================================== */

typedef struct purp_shape_device {
    purp_server_t *server;
    struct wl_resource *pointer;
    struct wl_listener pointer_destroy;
    bool is_tablet_tool;
    bool inert;
} purp_shape_device_t;

static void shape_pointer_died(struct wl_listener *listener, void *data) {
    purp_shape_device_t *dev = wl_container_of(listener, dev, pointer_destroy);
    (void)data;
    wl_list_remove(&listener->link);
    wl_list_init(&listener->link);
    dev->pointer = NULL;
    dev->inert = true;
}

static void shape_device_set_shape(struct wl_client *client, struct wl_resource *res,
                                   uint32_t serial, uint32_t shape) {
    (void)client;
    purp_shape_device_t *dev = wl_resource_get_user_data(res);
    if (!dev) return;
    if (!wp_cursor_shape_device_v1_shape_is_valid(shape, 1)) {
        wl_resource_post_error(res, WP_CURSOR_SHAPE_DEVICE_V1_ERROR_INVALID_SHAPE,
                               "invalid cursor shape %u", shape);
        return;
    }
    if (dev->inert || dev->is_tablet_tool) return;
    purp_seat_t *seat = server_seat(dev->server);
    if (serial != seat->last_enter_serial) return;
    purp_surface_t *focus = seat->pointer_focus;
    if (!focus || !focus->resource) return;
    if (!dev->pointer || wl_resource_get_client(dev->pointer) !=
        wl_resource_get_client(focus->resource)) return;
    if (seat->cursor_surface) {
        wl_list_remove(&seat->cursor_surface_destroy.link);
        seat->cursor_surface = NULL;
    }
    seat->cursor_mode = PURP_CURSOR_MODE_NAMED;
    seat->cursor_shape = (purp_cursor_shape_t)shape;
    purp_host_damage_all(dev->server);
}

static void shape_device_destroyed(struct wl_resource *res) {
    purp_shape_device_t *dev = wl_resource_get_user_data(res);
    if (!dev) return;
    wl_list_remove(&dev->pointer_destroy.link);
    free(dev);
}

static const struct wp_cursor_shape_device_v1_interface shape_device_impl = {
    .destroy = offer_destroy_request,
    .set_shape = shape_device_set_shape,
};

static void shape_device_create(struct wl_client *client, struct wl_resource *manager_res,
                                uint32_t id, struct wl_resource *pointer,
                                bool is_tablet_tool) {
    purp_server_t *s = wl_resource_get_user_data(manager_res);
    purp_shape_device_t *dev = calloc(1, sizeof(*dev));
    if (!dev) {
        wl_resource_post_no_memory(manager_res);
        return;
    }
    dev->server = s;
    dev->pointer = pointer;
    dev->is_tablet_tool = is_tablet_tool;
    dev->inert = is_tablet_tool || !pointer;
    wl_list_init(&dev->pointer_destroy.link);
    struct wl_resource *res =
        wl_resource_create(client, &wp_cursor_shape_device_v1_interface,
                           wl_resource_get_version(manager_res), (uint32_t)id);
    if (!res) {
        free(dev);
        wl_resource_post_no_memory(manager_res);
        return;
    }
    wl_resource_set_implementation(res, &shape_device_impl, dev, shape_device_destroyed);
    if (pointer) {
        dev->pointer_destroy.notify = shape_pointer_died;
        wl_resource_add_destroy_listener(pointer, &dev->pointer_destroy);
    }
}

static void cursor_shape_get_pointer(struct wl_client *client, struct wl_resource *res,
                                     uint32_t id, struct wl_resource *pointer) {
    shape_device_create(client, res, id, pointer, false);
}

static void cursor_shape_get_tablet_tool(struct wl_client *client, struct wl_resource *res,
                                         uint32_t id, struct wl_resource *tablet_tool) {
    (void)tablet_tool;
    shape_device_create(client, res, id, NULL, true);
}

static const struct wp_cursor_shape_manager_v1_interface cursor_shape_impl = {
    .destroy = offer_destroy_request,
    .get_pointer = cursor_shape_get_pointer,
    .get_tablet_tool_v2 = cursor_shape_get_tablet_tool,
};

static void cursor_shape_bind(struct wl_client *client, void *data, uint32_t version,
                              uint32_t id) {
    purp_server_t *s = data;
    struct wl_resource *res = wl_resource_create(client, &wp_cursor_shape_manager_v1_interface,
                                               (int)version, id);
    if (!res) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(res, &cursor_shape_impl, s, NULL);
}

/* =========================================================================================== */
/* zwp_tablet_manager_v2 (stub: seats exist, never any tablets/tools/pads)                    */
/* =========================================================================================== */

static void tablet_get_seat(struct wl_client *client, struct wl_resource *res, uint32_t id,
                            struct wl_resource *seat_res) {
    (void)seat_res;
    struct wl_resource *seat_v2 =
        wl_resource_create(client, &zwp_tablet_seat_v2_interface,
                           wl_resource_get_version(res), (uint32_t)id);
    if (!seat_v2) {
        wl_resource_post_no_memory(res);
        return;
    }
    static const struct zwp_tablet_seat_v2_interface tablet_seat_impl = {
        .destroy = offer_destroy_request,
    };
    wl_resource_set_implementation(seat_v2, &tablet_seat_impl,
                                   wl_resource_get_user_data(res), NULL);
}

static const struct zwp_tablet_manager_v2_interface tablet_manager_impl = {
    .get_tablet_seat = tablet_get_seat,
    .destroy = offer_destroy_request,
};

static void tablet_manager_bind(struct wl_client *client, void *data, uint32_t version,
                                uint32_t id) {
    purp_server_t *s = data;
    struct wl_resource *res = wl_resource_create(client, &zwp_tablet_manager_v2_interface,
                                               (int)version, id);
    if (!res) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(res, &tablet_manager_impl, s, NULL);
}

/* =========================================================================================== */
/* zwp_idle_inhibit_manager_v1                                                                */
/* =========================================================================================== */

static void inhibitor_destroyed(struct wl_resource *res) {
    purp_idle_inhibitor_t *inhibitor = wl_resource_get_user_data(res);
    if (!inhibitor) return;
    wl_list_remove(&inhibitor->surface_destroy.link);
    wl_list_remove(&inhibitor->link);
    free(inhibitor);
}

static void inhibitor_surface_died(struct wl_listener *listener, void *data) {
    purp_idle_inhibitor_t *inhibitor = wl_container_of(listener, inhibitor, surface_destroy);
    (void)data;
    wl_list_remove(&listener->link);
    wl_list_init(&listener->link);
    inhibitor->surface = NULL;
}

static const struct zwp_idle_inhibitor_v1_interface inhibitor_impl = {
    .destroy = offer_destroy_request,
};

static void idle_inhibit_create(struct wl_client *client, struct wl_resource *res, uint32_t id,
                                struct wl_resource *surface_res) {
    purp_server_t *s = wl_resource_get_user_data(res);
    purp_surface_t *surface = purp_surface_from_resource(surface_res);
    if (!surface) {
        wl_resource_post_error(res, WL_DISPLAY_ERROR_INVALID_OBJECT,
                               "zwp_idle_inhibit_manager_v1.create_inhibitor: not a wl_surface");
        return;
    }
    purp_idle_inhibitor_t *inhibitor = calloc(1, sizeof(*inhibitor));
    if (!inhibitor) {
        wl_resource_post_no_memory(res);
        return;
    }
    struct wl_resource *inhib =
        wl_resource_create(client, &zwp_idle_inhibitor_v1_interface,
                           wl_resource_get_version(res), (uint32_t)id);
    if (!inhib) {
        free(inhibitor);
        wl_resource_post_no_memory(res);
        return;
    }
    inhibitor->resource = inhib;
    inhibitor->surface = surface;
    inhibitor->surface_destroy.notify = inhibitor_surface_died;
    wl_signal_add(&surface->events_destroy, &inhibitor->surface_destroy);
    wl_list_insert(&server_seat(s)->idle_inhibitors, &inhibitor->link);
    wl_resource_set_implementation(inhib, &inhibitor_impl, inhibitor, inhibitor_destroyed);
}

static const struct zwp_idle_inhibit_manager_v1_interface idle_inhibit_impl = {
    .destroy = offer_destroy_request,
    .create_inhibitor = idle_inhibit_create,
};

static void idle_inhibit_bind(struct wl_client *client, void *data, uint32_t version,
                              uint32_t id) {
    purp_server_t *s = data;
    struct wl_resource *res = wl_resource_create(client, &zwp_idle_inhibit_manager_v1_interface,
                                               (int)version, id);
    if (!res) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(res, &idle_inhibit_impl, s, NULL);
}

static purp_layer_surface_t *inhibitor_layer(purp_server_t *s, purp_surface_t *root) {
    purp_layer_surface_t *layer;
    wl_list_for_each(layer, &s->layers, link)
        if (layer->surface == root) return layer;
    return NULL;
}

bool purp_seat_idle_inhibited(purp_server_t *s) {
    if (!s) return false;
    purp_seat_t *seat = server_seat(s);
    purp_idle_inhibitor_t *inhibitor;
    wl_list_for_each(inhibitor, &seat->idle_inhibitors, link) {
        purp_surface_t *surface = inhibitor->surface;
        if (!surface_alive(s, surface)) continue;
        purp_surface_t *root = purp_surface_root(surface);
        if (!surface_alive(s, root) || !purp_surface_is_mapped(root)) continue;
        purp_window_t *win = purp_xdg_window_for_surface(s, root);
        if (win) {
            if (purp_wm_is_window_visible(&s->wm, win)) return true;
            continue;
        }
        purp_layer_surface_t *layer = inhibitor_layer(s, root);
        if (layer) {
            if (purp_layer_is_mapped(layer) && !purp_layer_is_keepalive(layer)) return true;
            continue;
        }
        return true;                        /* mapped role-less surface */
    }
    return false;
}

/* =========================================================================================== */
/* wl_pointer / wl_keyboard / wl_touch                                                        */
/* =========================================================================================== */

static void pointer_set_cursor(struct wl_client *client, struct wl_resource *res,
                               uint32_t serial, struct wl_resource *surface_res,
                               int32_t hotspot_x, int32_t hotspot_y) {
    purp_server_t *s = wl_resource_get_user_data(res);
    purp_seat_t *seat = server_seat(s);
    purp_surface_t *focus = seat->pointer_focus;
    if (!focus || !focus->resource || wl_resource_get_client(focus->resource) != client)
        return;
    if (serial != seat->last_enter_serial) return;
    if (!surface_res) {
        seat->cursor_mode = PURP_CURSOR_MODE_HIDDEN;
        if (seat->cursor_surface) {
            wl_list_remove(&seat->cursor_surface_destroy.link);
            seat->cursor_surface = NULL;
        }
        purp_host_damage_all(s);
        return;
    }
    purp_surface_t *surface = purp_surface_from_resource(surface_res);
    if (!surface) return;
    if (wl_resource_get_client(surface_res) != client) return;
    if (!purp_surface_set_role(surface, &cursor_role, NULL, res, WL_POINTER_ERROR_ROLE))
        return;
    if (seat->cursor_surface != surface) {
        if (seat->cursor_surface)
            wl_list_remove(&seat->cursor_surface_destroy.link);
        seat->cursor_surface = surface;
        seat->cursor_surface_destroy.notify = cursor_surface_died;
        wl_signal_add(&surface->events_destroy, &seat->cursor_surface_destroy);
    }
    seat->cursor_mode = PURP_CURSOR_MODE_SURFACE;
    seat->cursor_hotspot = (purp_point_t){hotspot_x, hotspot_y};
    purp_host_damage_all(s);
}

static const struct wl_pointer_interface pointer_impl = {
    .set_cursor = pointer_set_cursor,
    .release = offer_destroy_request,
};

static const struct wl_keyboard_interface keyboard_impl = {
    .release = offer_destroy_request,
};

static const struct wl_touch_interface touch_impl = {
    .release = offer_destroy_request,
};

static void seat_get_pointer(struct wl_client *client, struct wl_resource *res, uint32_t id) {
    purp_server_t *s = wl_resource_get_user_data(res);
    purp_seat_t *seat = server_seat(s);
    struct wl_resource *pointer = wl_resource_create(client, &wl_pointer_interface,
                                                   wl_resource_get_version(res), (uint32_t)id);
    if (!pointer) {
        wl_resource_post_no_memory(res);
        return;
    }
    wl_resource_set_implementation(pointer, &pointer_impl, s, resource_unlink);
    wl_list_insert(&seat->pointers, wl_resource_get_link(pointer));
    purp_surface_t *focus = seat->pointer_focus;
    if (!focus || !focus->resource || wl_resource_get_client(focus->resource) != client) return;
    uint32_t serial = next_serial(s);
    seat->last_enter_serial = serial;
    wl_pointer_send_enter(pointer, serial, focus->resource,
                          wl_fixed_from_double(s->wm.pointer_location.x -
                                               seat->pointer_focus_origin.x),
                          wl_fixed_from_double(s->wm.pointer_location.y -
                                               seat->pointer_focus_origin.y));
    wl_pointer_send_motion(pointer, purp_server_time_ms(s),
                           wl_fixed_from_double(s->wm.pointer_location.x -
                                                seat->pointer_focus_origin.x),
                           wl_fixed_from_double(s->wm.pointer_location.y -
                                                seat->pointer_focus_origin.y));
    if (wl_resource_get_version(pointer) >= WL_POINTER_FRAME_SINCE_VERSION)
        wl_pointer_send_frame(pointer);
}

static void seat_get_keyboard(struct wl_client *client, struct wl_resource *res, uint32_t id) {
    purp_server_t *s = wl_resource_get_user_data(res);
    purp_seat_t *seat = server_seat(s);
    struct wl_resource *keyboard = wl_resource_create(client, &wl_keyboard_interface,
                                                    wl_resource_get_version(res), (uint32_t)id);
    if (!keyboard) {
        wl_resource_post_no_memory(res);
        return;
    }
    wl_resource_set_implementation(keyboard, &keyboard_impl, s, resource_unlink);
    wl_list_insert(&seat->keyboards, wl_resource_get_link(keyboard));
    if (seat->keymap_fd >= 0)
        wl_keyboard_send_keymap(keyboard, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, seat->keymap_fd,
                                seat->keymap_size);
    if (wl_resource_get_version(keyboard) >= WL_KEYBOARD_REPEAT_INFO_SINCE_VERSION)
        wl_keyboard_send_repeat_info(keyboard, PURP_KEY_REPEAT_RATE, PURP_KEY_REPEAT_DELAY_MS);
    purp_surface_t *focus = seat->keyboard_focus;
    if (!focus || !focus->resource || wl_resource_get_client(focus->resource) != client) return;
    struct wl_array keys;
    wl_array_init(&keys);
    for (size_t i = 0; i < seat->pressed_key_count; ++i) {
        uint32_t *code = wl_array_add(&keys, sizeof(uint32_t));
        if (code) *code = seat->pressed_keys[i];
    }
    wl_keyboard_send_enter(keyboard, next_serial(s), focus->resource, &keys);
    wl_array_release(&keys);
    wl_keyboard_send_modifiers(keyboard, next_serial(s), seat->mods_depressed,
                               seat->mods_latched, seat->mods_locked, seat->group);
}

static void seat_get_touch(struct wl_client *client, struct wl_resource *res, uint32_t id) {
    purp_server_t *s = wl_resource_get_user_data(res);
    purp_seat_t *seat = server_seat(s);
    struct wl_resource *touch = wl_resource_create(client, &wl_touch_interface,
                                                 wl_resource_get_version(res), (uint32_t)id);
    if (!touch) {
        wl_resource_post_no_memory(res);
        return;
    }
    wl_resource_set_implementation(touch, &touch_impl, s, resource_unlink);
    wl_list_insert(&seat->touches, wl_resource_get_link(touch));
}

static const struct wl_seat_interface seat_impl = {
    .get_pointer = seat_get_pointer,
    .get_keyboard = seat_get_keyboard,
    .get_touch = seat_get_touch,
    .release = offer_destroy_request,
};

static void seat_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
    purp_server_t *s = data;
    purp_seat_t *seat = server_seat(s);
    struct wl_resource *res =
        wl_resource_create(client, &wl_seat_interface, (int)version, id);
    if (!res) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(res, &seat_impl, s, resource_unlink);
    wl_list_insert(&seat->resources, wl_resource_get_link(res));
    wl_seat_send_capabilities(res, WL_SEAT_CAPABILITY_POINTER | WL_SEAT_CAPABILITY_KEYBOARD |
                                       WL_SEAT_CAPABILITY_TOUCH);
    if (version >= WL_SEAT_NAME_SINCE_VERSION) wl_seat_send_name(res, "seat-0");
}

/* =========================================================================================== */
/* Keyboard                                                                                   */
/* =========================================================================================== */

static void seat_update_modifiers(purp_seat_t *seat) {
    uint32_t mods = 0;
    if (xkb_state_mod_name_is_active(seat->xkb_state, XKB_MOD_NAME_SHIFT,
                                     XKB_STATE_MODS_EFFECTIVE) > 0)
        mods |= PURP_MOD_SHIFT;
    if (xkb_state_mod_name_is_active(seat->xkb_state, XKB_MOD_NAME_CAPS,
                                     XKB_STATE_MODS_LOCKED) > 0)
        mods |= PURP_MOD_CAPS;
    if (xkb_state_mod_name_is_active(seat->xkb_state, XKB_MOD_NAME_CTRL,
                                     XKB_STATE_MODS_EFFECTIVE) > 0)
        mods |= PURP_MOD_CTRL;
    if (xkb_state_mod_name_is_active(seat->xkb_state, XKB_MOD_NAME_ALT,
                                     XKB_STATE_MODS_EFFECTIVE) > 0)
        mods |= PURP_MOD_ALT;
    if (xkb_state_mod_name_is_active(seat->xkb_state, XKB_MOD_NAME_NUM,
                                     XKB_STATE_MODS_LOCKED) > 0)
        mods |= PURP_MOD_NUM;
    if (xkb_state_mod_name_is_active(seat->xkb_state, XKB_MOD_NAME_LOGO,
                                     XKB_STATE_MODS_EFFECTIVE) > 0)
        mods |= PURP_MOD_LOGO;
    seat->modifiers = mods;
}

bool purp_seat_update_xkb(purp_server_t *s, uint32_t keycode, bool pressed) {
    if (!s || !s->seat.xkb_state) return false;
    purp_seat_t *seat = server_seat(s);
    xkb_state_update_key(seat->xkb_state, (xkb_keycode_t)keycode + 8,
                         pressed ? XKB_KEY_DOWN : XKB_KEY_UP);
    uint32_t depressed =
        xkb_state_serialize_mods(seat->xkb_state, XKB_STATE_MODS_DEPRESSED);
    uint32_t latched = xkb_state_serialize_mods(seat->xkb_state, XKB_STATE_MODS_LATCHED);
    uint32_t locked = xkb_state_serialize_mods(seat->xkb_state, XKB_STATE_MODS_LOCKED);
    uint32_t layout = xkb_state_serialize_layout(seat->xkb_state, XKB_STATE_LAYOUT_EFFECTIVE);
    bool changed = depressed != seat->mods_depressed || latched != seat->mods_latched ||
                   locked != seat->mods_locked || layout != seat->group;
    seat->mods_depressed = depressed;
    seat->mods_latched = latched;
    seat->mods_locked = locked;
    seat->group = layout;
    seat_update_modifiers(seat);
    return changed;
}

xkb_keysym_t purp_seat_keysym(purp_server_t *s, uint32_t keycode) {
    if (!s || !s->seat.xkb_state) return XKB_KEY_NoSymbol;
    return xkb_state_key_get_one_sym(s->seat.xkb_state, (xkb_keycode_t)keycode + 8);
}

xkb_keysym_t purp_seat_raw_keysym(purp_server_t *s, uint32_t keycode) {
    if (!s || !s->seat.keymap) return XKB_KEY_NoSymbol;
    xkb_keycode_t xkb_code = (xkb_keycode_t)keycode + 8;
    xkb_layout_index_t layouts = xkb_keymap_num_layouts_for_key(s->seat.keymap, xkb_code);
    if (!layouts) return XKB_KEY_NoSymbol;
    const xkb_keysym_t *syms = NULL;
    int count = xkb_keymap_key_get_syms_by_level(s->seat.keymap, xkb_code, 0, 0, &syms);
    return count > 0 ? syms[0] : XKB_KEY_NoSymbol;
}

uint32_t purp_seat_utf32(purp_server_t *s, uint32_t keycode) {
    if (!s || !s->seat.xkb_state) return 0;
    return xkb_state_key_get_utf32(s->seat.xkb_state, (xkb_keycode_t)keycode + 8);
}

void purp_seat_set_keyboard_focus(purp_server_t *s, purp_surface_t *surface) {
    if (!s) return;
    purp_seat_t *seat = server_seat(s);
    purp_surface_t *old = seat->keyboard_focus;
    if (old == surface) return;
    struct wl_client *old_client =
        old && old->resource ? wl_resource_get_client(old->resource) : NULL;
    struct wl_client *new_client =
        surface && surface->resource ? wl_resource_get_client(surface->resource) : NULL;
    uint32_t serial = next_serial(s);
    if (old_client) {
        struct wl_resource *keyboard;
        wl_list_for_each(keyboard, &seat->keyboards, link) {
            if (wl_resource_get_client(keyboard) != old_client) continue;
            wl_keyboard_send_leave(keyboard, serial, old->resource);
        }
    }
    if (old) wl_list_remove(&seat->keyboard_focus_destroy.link);
    seat->keyboard_focus = surface;
    if (surface) {
        seat->keyboard_focus_destroy.notify = keyboard_focus_died;
        wl_signal_add(&surface->events_destroy, &seat->keyboard_focus_destroy);
        if (new_client) {
            struct wl_array keys;
            wl_array_init(&keys);
            for (size_t i = 0; i < seat->pressed_key_count; ++i) {
                uint32_t *code = wl_array_add(&keys, sizeof(uint32_t));
                if (code) *code = seat->pressed_keys[i];
            }
            struct wl_resource *keyboard;
            wl_list_for_each(keyboard, &seat->keyboards, link) {
                if (wl_resource_get_client(keyboard) != new_client) continue;
                wl_keyboard_send_enter(keyboard, serial, surface->resource, &keys);
            }
            wl_array_release(&keys);
            purp_seat_keyboard_send_modifiers(s);
        }
    }
    seat_broadcast_selection(s);
    seat_broadcast_primary_selection(s);
}

void purp_seat_keyboard_send_key(purp_server_t *s, uint32_t time_ms, uint32_t keycode,
                                 bool pressed) {
    if (!s) return;
    purp_seat_t *seat = server_seat(s);
    if (pressed) {
        add_u32(seat->pressed_keys, &seat->pressed_key_count, PURP_MAX_PRESSED_KEYS, keycode);
    } else {
        remove_u32(seat->pressed_keys, &seat->pressed_key_count, keycode);
    }
    purp_surface_t *focus = seat->keyboard_focus;
    if (!focus || !focus->resource) return;
    struct wl_client *client = wl_resource_get_client(focus->resource);
    uint32_t serial = next_serial(s);
    struct wl_resource *keyboard;
    wl_list_for_each(keyboard, &seat->keyboards, link) {
        if (wl_resource_get_client(keyboard) != client) continue;
        wl_keyboard_send_key(keyboard, serial, time_ms, keycode,
                             pressed ? WL_KEYBOARD_KEY_STATE_PRESSED
                                     : WL_KEYBOARD_KEY_STATE_RELEASED);
    }
}

void purp_seat_keyboard_send_modifiers(purp_server_t *s) {
    if (!s) return;
    purp_seat_t *seat = server_seat(s);
    purp_surface_t *focus = seat->keyboard_focus;
    if (!focus || !focus->resource) return;
    struct wl_client *client = wl_resource_get_client(focus->resource);
    uint32_t serial = next_serial(s);
    struct wl_resource *keyboard;
    wl_list_for_each(keyboard, &seat->keyboards, link) {
        if (wl_resource_get_client(keyboard) != client) continue;
        wl_keyboard_send_modifiers(keyboard, serial, seat->mods_depressed, seat->mods_latched,
                                   seat->mods_locked, seat->group);
    }
}

/* =========================================================================================== */
/* Pointer                                                                                    */
/* =========================================================================================== */

void purp_seat_pointer_focus(purp_server_t *s, purp_surface_t *surface, purp_pointf_t origin,
                             uint32_t time_ms) {
    if (!s) return;
    purp_seat_t *seat = server_seat(s);
    purp_surface_t *old = seat->pointer_focus;
    if (old != surface) {
        struct wl_client *old_client =
            old && old->resource ? wl_resource_get_client(old->resource) : NULL;
        uint32_t serial = next_serial(s);
        if (old_client) {
            struct wl_resource *pointer;
            wl_list_for_each(pointer, &seat->pointers, link) {
                if (wl_resource_get_client(pointer) != old_client) continue;
                wl_pointer_send_leave(pointer, serial, old->resource);
                if (wl_resource_get_version(pointer) >= WL_POINTER_FRAME_SINCE_VERSION)
                    wl_pointer_send_frame(pointer);
            }
        }
        if (old) wl_list_remove(&seat->pointer_focus_destroy.link);
        seat->pointer_focus = surface;
        seat->pointer_focus_origin = origin;
        struct wl_client *new_client =
            surface && surface->resource ? wl_resource_get_client(surface->resource) : NULL;
        if (new_client != old_client) {
            /* The previous focus client's cursor choice does not carry over. */
            seat->cursor_mode = PURP_CURSOR_MODE_NAMED;
            seat->cursor_shape = PURP_CURSOR_DEFAULT;
            if (seat->cursor_surface) {
                wl_list_remove(&seat->cursor_surface_destroy.link);
                seat->cursor_surface = NULL;
            }
        }
        if (surface) {
            seat->pointer_focus_destroy.notify = pointer_focus_died;
            wl_signal_add(&surface->events_destroy, &seat->pointer_focus_destroy);
            struct wl_client *client =
                surface->resource ? wl_resource_get_client(surface->resource) : NULL;
            if (client) {
                seat->last_enter_serial = serial;
                struct wl_resource *pointer;
                wl_list_for_each(pointer, &seat->pointers, link) {
                    if (wl_resource_get_client(pointer) != client) continue;
                    wl_pointer_send_enter(pointer, serial, surface->resource,
                                          wl_fixed_from_double(s->wm.pointer_location.x -
                                                               origin.x),
                                          wl_fixed_from_double(s->wm.pointer_location.y -
                                                               origin.y));
                }
            }
        }
    } else {
        seat->pointer_focus_origin = origin;
    }
    purp_seat_pointer_send_motion(s, time_ms);
}

void purp_seat_pointer_send_motion(purp_server_t *s, uint32_t time_ms) {
    if (!s) return;
    purp_seat_t *seat = server_seat(s);
    purp_surface_t *focus = seat->pointer_focus;
    if (!focus || !focus->resource) return;
    struct wl_client *client = wl_resource_get_client(focus->resource);
    wl_fixed_t sx = wl_fixed_from_double(s->wm.pointer_location.x - seat->pointer_focus_origin.x);
    wl_fixed_t sy = wl_fixed_from_double(s->wm.pointer_location.y - seat->pointer_focus_origin.y);
    struct wl_resource *pointer;
    wl_list_for_each(pointer, &seat->pointers, link) {
        if (wl_resource_get_client(pointer) != client) continue;
        wl_pointer_send_motion(pointer, time_ms, sx, sy);
    }
}

uint32_t purp_seat_pointer_send_button(purp_server_t *s, uint32_t time_ms, uint32_t button,
                                       bool pressed) {
    if (!s) return 0;
    purp_seat_t *seat = server_seat(s);
    if (pressed) {
        add_u32(seat->pressed_buttons, &seat->pressed_button_count, PURP_MAX_PRESSED_BUTTONS,
                button);
    } else {
        remove_u32(seat->pressed_buttons, &seat->pressed_button_count, button);
    }
    purp_surface_t *focus = seat->pointer_focus;
    if (!focus || !focus->resource) return 0;
    struct wl_client *client = wl_resource_get_client(focus->resource);
    uint32_t serial = next_serial(s);
    seat->last_button_serial = serial;
    struct wl_resource *pointer;
    wl_list_for_each(pointer, &seat->pointers, link) {
        if (wl_resource_get_client(pointer) != client) continue;
        wl_pointer_send_button(pointer, serial, time_ms, button,
                               pressed ? WL_POINTER_BUTTON_STATE_PRESSED
                                       : WL_POINTER_BUTTON_STATE_RELEASED);
    }
    return serial;
}

void purp_seat_pointer_send_axis(purp_server_t *s, const purp_axis_event_t *event) {
    if (!s || !event) return;
    purp_seat_t *seat = server_seat(s);
    purp_surface_t *focus = seat->pointer_focus;
    if (!focus || !focus->resource) return;
    struct wl_client *client = wl_resource_get_client(focus->resource);
    uint32_t source = WL_POINTER_AXIS_SOURCE_CONTINUOUS;
    switch (event->source) {
    case PURP_AXIS_SOURCE_WHEEL: source = WL_POINTER_AXIS_SOURCE_WHEEL; break;
    case PURP_AXIS_SOURCE_FINGER: source = WL_POINTER_AXIS_SOURCE_FINGER; break;
    case PURP_AXIS_SOURCE_CONTINUOUS: source = WL_POINTER_AXIS_SOURCE_CONTINUOUS; break;
    case PURP_AXIS_SOURCE_WHEEL_TILT: source = WL_POINTER_AXIS_SOURCE_WHEEL_TILT; break;
    }
    struct wl_resource *pointer;
    wl_list_for_each(pointer, &seat->pointers, link) {
        if (wl_resource_get_client(pointer) != client) continue;
        uint32_t version = (uint32_t)wl_resource_get_version(pointer);
        if (version >= WL_POINTER_AXIS_SOURCE_SINCE_VERSION &&
            (source != WL_POINTER_AXIS_SOURCE_CONTINUOUS || event->has_vertical ||
             event->has_horizontal || event->vertical_v120 || event->horizontal_v120))
            wl_pointer_send_axis_source(pointer, source);
        if (event->has_vertical || event->vertical_v120) {
            if (event->has_vertical)
                wl_pointer_send_axis(pointer, event->time_ms,
                                     WL_POINTER_AXIS_VERTICAL_SCROLL,
                                     wl_fixed_from_double(event->vertical));
            if (event->vertical_v120) {
                if (version >= WL_POINTER_AXIS_VALUE120_SINCE_VERSION)
                    wl_pointer_send_axis_value120(pointer, WL_POINTER_AXIS_VERTICAL_SCROLL,
                                                  event->vertical_v120);
                else if (version >= WL_POINTER_AXIS_DISCRETE_SINCE_VERSION)
                    wl_pointer_send_axis_discrete(pointer, WL_POINTER_AXIS_VERTICAL_SCROLL,
                                                  event->vertical_v120 / 120);
                if (version >= WL_POINTER_AXIS_RELATIVE_DIRECTION_SINCE_VERSION)
                    wl_pointer_send_axis_relative_direction(
                        pointer, WL_POINTER_AXIS_VERTICAL_SCROLL,
                        event->inverted ? WL_POINTER_AXIS_RELATIVE_DIRECTION_INVERTED
                                        : WL_POINTER_AXIS_RELATIVE_DIRECTION_IDENTICAL);
            }
        }
        if (event->has_horizontal || event->horizontal_v120) {
            if (event->has_horizontal)
                wl_pointer_send_axis(pointer, event->time_ms,
                                     WL_POINTER_AXIS_HORIZONTAL_SCROLL,
                                     wl_fixed_from_double(event->horizontal));
            if (event->horizontal_v120) {
                if (version >= WL_POINTER_AXIS_VALUE120_SINCE_VERSION)
                    wl_pointer_send_axis_value120(pointer, WL_POINTER_AXIS_HORIZONTAL_SCROLL,
                                                  event->horizontal_v120);
                else if (version >= WL_POINTER_AXIS_DISCRETE_SINCE_VERSION)
                    wl_pointer_send_axis_discrete(pointer, WL_POINTER_AXIS_HORIZONTAL_SCROLL,
                                                  event->horizontal_v120 / 120);
                if (version >= WL_POINTER_AXIS_RELATIVE_DIRECTION_SINCE_VERSION)
                    wl_pointer_send_axis_relative_direction(
                        pointer, WL_POINTER_AXIS_HORIZONTAL_SCROLL,
                        event->inverted ? WL_POINTER_AXIS_RELATIVE_DIRECTION_INVERTED
                                        : WL_POINTER_AXIS_RELATIVE_DIRECTION_IDENTICAL);
            }
        }
    }
}

void purp_seat_pointer_send_frame(purp_server_t *s) {
    if (!s) return;
    purp_seat_t *seat = server_seat(s);
    purp_surface_t *focus = seat->pointer_focus;
    if (!focus || !focus->resource) return;
    struct wl_client *client = wl_resource_get_client(focus->resource);
    struct wl_resource *pointer;
    wl_list_for_each(pointer, &seat->pointers, link) {
        if (wl_resource_get_client(pointer) != client) continue;
        if (wl_resource_get_version(pointer) >= WL_POINTER_FRAME_SINCE_VERSION)
            wl_pointer_send_frame(pointer);
    }
}

/* =========================================================================================== */
/* Touch                                                                                      */
/* =========================================================================================== */

typedef struct purp_touch_surface_listener {
    struct wl_listener listener;
    purp_server_t *server;
} purp_touch_surface_listener_t;

static void touch_surface_died(struct wl_listener *listener, void *data) {
    purp_touch_surface_listener_t *box = wl_container_of(listener, box, listener);
    purp_server_t *s = box->server;
    (void)data;
    purp_seat_touch_send_cancel(s, -1);
}

static bool touch_surface_listen(purp_server_t *s, purp_surface_t *surface) {
    if (!surface) return true;
    struct wl_listener *listener;
    wl_list_for_each(listener, &surface->events_destroy.listener_list, link) {
        if (listener->notify == touch_surface_died) return true;
    }
    purp_touch_surface_listener_t *box = calloc(1, sizeof(*box));
    if (!box) return false;
    box->server = s;
    box->listener.notify = touch_surface_died;
    wl_signal_add(&surface->events_destroy, &box->listener);
    return true;
}

static void touch_surface_unlisten(purp_seat_t *seat, purp_surface_t *surface) {
    if (!surface) return;
    for (size_t i = 0; i < PURP_MAX_TOUCH_POINTS; ++i)
        if (seat->touch[i].active && seat->touch[i].surface == surface) return;
    struct wl_listener *listener, *tmp;
    wl_list_for_each_safe(listener, tmp, &surface->events_destroy.listener_list, link) {
        if (listener->notify != touch_surface_died) continue;
        purp_touch_surface_listener_t *box = wl_container_of(listener, box, listener);
        wl_list_remove(&listener->link);
        free(box);
    }
}

purp_touch_point_t *purp_seat_touch_point(purp_server_t *s, int32_t slot) {
    if (!s) return NULL;
    purp_seat_t *seat = server_seat(s);
    for (size_t i = 0; i < PURP_MAX_TOUCH_POINTS; ++i)
        if (seat->touch[i].active && seat->touch[i].slot == slot) return &seat->touch[i];
    return NULL;
}


void purp_seat_touch_send_down(purp_server_t *s, uint32_t time_ms, int32_t slot,
                               purp_surface_t *surface, purp_pointf_t origin, purp_pointf_t pos) {
    if (!s) return;
    purp_seat_t *seat = server_seat(s);
    purp_touch_point_t *point = purp_seat_touch_point(s, slot);
    if (!point) {
        for (size_t i = 0; i < PURP_MAX_TOUCH_POINTS; ++i) {
            if (seat->touch[i].active) continue;
            point = &seat->touch[i];
            break;
        }
    }
    if (!point) return;
    if (!touch_surface_listen(s, surface)) {
        if (surface && surface->resource) wl_resource_post_no_memory(surface->resource);
        return;
    }
    purp_surface_t *old_surface = point->surface;
    point->active = false;
    point->surface = NULL;
    if (old_surface != surface) touch_surface_unlisten(seat, old_surface);
    point->active = true;
    point->slot = slot;
    point->surface = surface;
    point->pos = pos;
    point->surface_origin = origin;
    if (!surface || !surface->resource) return;
    struct wl_client *client = wl_resource_get_client(surface->resource);
    uint32_t serial = next_serial(s);
    seat->last_touch_serial = serial;
    struct wl_resource *touch;
    wl_list_for_each(touch, &seat->touches, link) {
        if (wl_resource_get_client(touch) != client) continue;
        wl_touch_send_down(touch, serial, time_ms, surface->resource, slot,
                           wl_fixed_from_double(pos.x - origin.x),
                           wl_fixed_from_double(pos.y - origin.y));
    }
}

void purp_seat_touch_send_motion(purp_server_t *s, uint32_t time_ms, int32_t slot,
                                 purp_pointf_t pos) {
    if (!s) return;
    purp_touch_point_t *point = purp_seat_touch_point(s, slot);
    if (!point) return;
    point->pos = pos;
    if (!point->surface || !point->surface->resource) return;
    purp_seat_t *seat = server_seat(s);
    struct wl_client *client = wl_resource_get_client(point->surface->resource);
    struct wl_resource *touch;
    wl_list_for_each(touch, &seat->touches, link) {
        if (wl_resource_get_client(touch) != client) continue;
        wl_touch_send_motion(touch, time_ms, slot,
                             wl_fixed_from_double(pos.x - point->surface_origin.x),
                             wl_fixed_from_double(pos.y - point->surface_origin.y));
    }
}

void purp_seat_touch_send_up(purp_server_t *s, uint32_t time_ms, int32_t slot) {
    if (!s) return;
    purp_seat_t *seat = server_seat(s);
    purp_touch_point_t *point = purp_seat_touch_point(s, slot);
    if (!point) return;
    purp_surface_t *surface = point->surface;
    point->active = false;
    point->surface = NULL;
    touch_surface_unlisten(seat, surface);
    if (!surface || !surface->resource) return;
    struct wl_client *client = wl_resource_get_client(surface->resource);
    uint32_t serial = next_serial(s);
    struct wl_resource *touch;
    wl_list_for_each(touch, &seat->touches, link) {
        if (wl_resource_get_client(touch) != client) continue;
        wl_touch_send_up(touch, serial, time_ms, slot);
    }
}

void purp_seat_touch_send_cancel(purp_server_t *s, int32_t slot) {
    (void)slot;
    if (!s) return;
    purp_seat_t *seat = server_seat(s);
    if (seat->drag.active && seat->drag.is_touch) purp_seat_drag_cancel(s);
    struct wl_client *clients[PURP_MAX_TOUCH_POINTS];
    size_t count = 0;
    for (size_t i = 0; i < PURP_MAX_TOUCH_POINTS; ++i) {
        purp_touch_point_t *point = &seat->touch[i];
        if (!point->active) continue;
        purp_surface_t *surface = point->surface;
        point->active = false;
        struct wl_client *client =
            point->surface && point->surface->resource
                ? wl_resource_get_client(point->surface->resource) : NULL;
        point->surface = NULL;
        touch_surface_unlisten(seat, surface);
        if (!client) continue;
        bool seen = false;
        for (size_t n = 0; n < count; ++n)
            if (clients[n] == client) seen = true;
        if (!seen && count < PURP_MAX_TOUCH_POINTS) clients[count++] = client;
    }
    for (size_t n = 0; n < count; ++n) {
        struct wl_resource *touch;
        wl_list_for_each(touch, &seat->touches, link) {
            if (wl_resource_get_client(touch) != clients[n]) continue;
            wl_touch_send_cancel(touch);
            wl_touch_send_frame(touch);
        }
    }
}

void purp_seat_touch_send_frame(purp_server_t *s) {
    if (!s) return;
    purp_seat_t *seat = server_seat(s);
    struct wl_client *clients[PURP_MAX_TOUCH_POINTS];
    size_t count = 0;
    for (size_t i = 0; i < PURP_MAX_TOUCH_POINTS; ++i) {
        purp_touch_point_t *point = &seat->touch[i];
        if (!point->active || !point->surface || !point->surface->resource) continue;
        struct wl_client *client = wl_resource_get_client(point->surface->resource);
        bool seen = false;
        for (size_t n = 0; n < count; ++n)
            if (clients[n] == client) seen = true;
        if (!seen && count < PURP_MAX_TOUCH_POINTS) clients[count++] = client;
    }
    for (size_t n = 0; n < count; ++n) {
        struct wl_resource *touch;
        wl_list_for_each(touch, &seat->touches, link) {
            if (wl_resource_get_client(touch) != clients[n]) continue;
            wl_touch_send_frame(touch);
        }
    }
}

bool purp_seat_validate_grab_serial(purp_server_t *s, struct wl_client *client,
                                    uint32_t serial) {
    if (!s || !client || !serial) return false;
    purp_seat_t *seat = server_seat(s);
    if (serial == seat->last_button_serial && seat->pointer_focus &&
        seat->pointer_focus->resource &&
        wl_resource_get_client(seat->pointer_focus->resource) == client)
        return true;
    if (serial == seat->last_touch_serial) {
        for (size_t i = 0; i < PURP_MAX_TOUCH_POINTS; ++i) {
            purp_touch_point_t *point = &seat->touch[i];
            if (!point->active || !point->surface || !point->surface->resource) continue;
            if (wl_resource_get_client(point->surface->resource) == client) return true;
        }
    }
    return false;
}

/* =========================================================================================== */
/* wl_output + zxdg_output_v1                                                                 */
/* =========================================================================================== */

static purp_monitor_t *output_monitor(purp_output_t *output) {
    if (!output || !output->server || output->monitor >= output->server->wm.monitor_count)
        return NULL;
    purp_monitor_t *mon = &output->server->wm.monitors[output->monitor];
    return mon->enabled ? mon : NULL;
}

static void output_send_state_to(struct wl_resource *res, purp_monitor_t *mon) {
    uint32_t version = (uint32_t)wl_resource_get_version(res);
    int32_t scale = mon->scale >= 1.0 ? (int32_t)ceil(mon->scale) : 1;
    if (scale < 1) scale = 1;
    wl_output_send_geometry(res, mon->loc.x, mon->loc.y, mon->physical_size_mm.w,
                            mon->physical_size_mm.h, (int32_t)mon->subpixel,
                            mon->make[0] ? mon->make : "purpwm+",
                            mon->model[0] ? mon->model : mon->name,
                            WL_OUTPUT_TRANSFORM_NORMAL);
    uint32_t flags = WL_OUTPUT_MODE_CURRENT;
    for (size_t i = 0; i < mon->mode_count; ++i) {
        if (mon->modes[i].preferred && mon->modes[i].width == (uint32_t)mon->physical_size.w &&
            mon->modes[i].height == (uint32_t)mon->physical_size.h &&
            mon->modes[i].refresh_mhz == mon->refresh_mhz)
            flags |= WL_OUTPUT_MODE_PREFERRED;
    }
    wl_output_send_mode(res, flags, mon->physical_size.w, mon->physical_size.h,
                        mon->refresh_mhz);
    if (version >= WL_OUTPUT_SCALE_SINCE_VERSION) wl_output_send_scale(res, scale);
    if (version >= WL_OUTPUT_NAME_SINCE_VERSION) wl_output_send_name(res, mon->name);
    if (version >= WL_OUTPUT_DESCRIPTION_SINCE_VERSION)
        wl_output_send_description(res, mon->description[0] ? mon->description : mon->name);
    if (version >= WL_OUTPUT_DONE_SINCE_VERSION) wl_output_send_done(res);
}

static void xdg_output_send_state_to(struct wl_resource *res, purp_monitor_t *mon) {
    uint32_t version = (uint32_t)wl_resource_get_version(res);
    zxdg_output_v1_send_logical_position(res, mon->loc.x, mon->loc.y);
    zxdg_output_v1_send_logical_size(res, mon->logical_size.w, mon->logical_size.h);
    if (version >= ZXDG_OUTPUT_V1_NAME_SINCE_VERSION) zxdg_output_v1_send_name(res, mon->name);
    if (version >= ZXDG_OUTPUT_V1_DESCRIPTION_SINCE_VERSION)
        zxdg_output_v1_send_description(res, mon->description[0] ? mon->description : mon->name);
    if (version < ZXDG_OUTPUT_V1_DONE_SINCE_VERSION) zxdg_output_v1_send_done(res);
}

void purp_output_send_state(purp_output_t *output) {
    purp_monitor_t *mon = output_monitor(output);
    if (!mon) return;
    struct wl_resource *res;
    wl_list_for_each(res, &output->resources, link) output_send_state_to(res, mon);
    wl_list_for_each(res, &output->xdg_output_resources, link)
        xdg_output_send_state_to(res, mon);
}

struct wl_resource *purp_output_resource_for_client(purp_output_t *output,
                                                    struct wl_client *client) {
    if (!output || !client) return NULL;
    struct wl_resource *res;
    wl_list_for_each(res, &output->resources, link)
        if (wl_resource_get_client(res) == client) return res;
    return NULL;
}

int purp_output_monitor_from_resource(struct wl_resource *resource) {
    if (!resource) return -1;
    purp_output_t *output = wl_resource_get_user_data(resource);
    if (!output) return -1;
    return (int)output->monitor;
}

static const struct wl_output_interface output_impl = {
    .release = offer_destroy_request,
};

static void output_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
    purp_output_t *output = data;
    struct wl_resource *res =
        wl_resource_create(client, &wl_output_interface, (int)version, id);
    if (!res) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(res, &output_impl, output, resource_unlink);
    wl_list_insert(&output->resources, wl_resource_get_link(res));
    purp_monitor_t *mon = output_monitor(output);
    if (mon) output_send_state_to(res, mon);
}

purp_output_t *purp_output_create(purp_server_t *s, size_t monitor) {
    if (!s) return NULL;
    purp_output_t *output = calloc(1, sizeof(*output));
    if (!output) return NULL;
    output->server = s;
    output->monitor = monitor;
    wl_list_init(&output->resources);
    wl_list_init(&output->xdg_output_resources);
    wl_list_init(&output->link);
    output->global = wl_global_create(s->display, &wl_output_interface,
                                      PURP_WL_OUTPUT_VERSION, output, output_bind);
    if (!output->global) {
        free(output);
        return NULL;
    }
    wl_list_insert(&s->outputs, &output->link);
    return output;
}

void purp_output_destroy(purp_output_t *output) {
    if (!output) return;
    if (output->global) {
        wl_global_remove(output->global);   /* later: wl_global_destroy by the display */
        output->global = NULL;
    }
    struct wl_resource *res, *tmp;
    wl_list_for_each_safe(res, tmp, &output->resources, link) {
        wl_resource_set_user_data(res, NULL);
        wl_list_remove(wl_resource_get_link(res));
        wl_list_init(wl_resource_get_link(res));
    }
    wl_list_for_each_safe(res, tmp, &output->xdg_output_resources, link) {
        wl_resource_set_user_data(res, NULL);
        wl_list_remove(wl_resource_get_link(res));
        wl_list_init(wl_resource_get_link(res));
    }
    if (output->server) {
        wl_list_remove(&output->link);
        wl_list_init(&output->link);
        if (output->monitor < output->server->wm.monitor_count &&
            output->server->wm.monitors[output->monitor].output == output)
            output->server->wm.monitors[output->monitor].output = NULL;
    }
    free(output);
}

/* zxdg_output_manager_v1 */

static void xdg_output_destroyed(struct wl_resource *res) {
    wl_list_remove(wl_resource_get_link(res));
}

static const struct zxdg_output_v1_interface xdg_output_impl = {
    .destroy = offer_destroy_request,
};

static void xdg_output_manager_get(struct wl_client *client, struct wl_resource *res,
                                 uint32_t id, struct wl_resource *output_res) {
    purp_output_t *output = wl_resource_get_user_data(output_res);
    struct wl_resource *xdg =
        wl_resource_create(client, &zxdg_output_v1_interface,
                           wl_resource_get_version(res), (uint32_t)id);
    if (!xdg) {
        wl_resource_post_no_memory(res);
        return;
    }
    wl_resource_set_implementation(xdg, &xdg_output_impl, output, xdg_output_destroyed);
    if (!output) {
        /* The wl_output global is gone: park the resource on its own link (inert). */
        wl_list_init(wl_resource_get_link(xdg));
        return;
    }
    wl_list_insert(&output->xdg_output_resources, wl_resource_get_link(xdg));
    purp_monitor_t *mon = output_monitor(output);
    if (mon) xdg_output_send_state_to(xdg, mon);
}

static const struct zxdg_output_manager_v1_interface xdg_output_manager_impl = {
    .destroy = offer_destroy_request,
    .get_xdg_output = xdg_output_manager_get,
};

static void xdg_output_manager_bind(struct wl_client *client, void *data, uint32_t version,
                                  uint32_t id) {
    purp_server_t *s = data;
    struct wl_resource *res = wl_resource_create(client, &zxdg_output_manager_v1_interface,
                                               (int)version, id);
    if (!res) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(res, &xdg_output_manager_impl, s, NULL);
}

/* =========================================================================================== */
/* Seat init / finish                                                                         */
/* =========================================================================================== */

static int keymap_to_memfd(struct xkb_keymap *keymap, uint32_t *size_out) {
    char *text = xkb_keymap_get_as_string(keymap, XKB_KEYMAP_FORMAT_TEXT_V1);
    if (!text) return -1;
    size_t size = strlen(text) + 1;
    int fd = memfd_create("purpwm-keymap", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (fd < 0) {
        free(text);
        return -1;
    }
    if (ftruncate(fd, (off_t)size) < 0) {
        close(fd);
        free(text);
        return -1;
    }
    size_t off = 0;
    while (off < size) {
        ssize_t n = write(fd, text + off, size - off);
        if (n > 0) {
            off += (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        close(fd);
        free(text);
        return -1;
    }
    free(text);
    lseek(fd, 0, SEEK_SET);                 /* dups share the offset; clients mmap anyway */
    fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_SEAL);
    *size_out = (uint32_t)size;
    return fd;
}

bool purp_seat_init(purp_server_t *s) {
    if (!s) return false;
    purp_seat_t *seat = server_seat(s);
    memset(seat, 0, sizeof(*seat));
    seat->keymap_fd = -1;
    wl_list_init(&seat->resources);
    wl_list_init(&seat->keyboards);
    wl_list_init(&seat->pointers);
    wl_list_init(&seat->touches);
    wl_list_init(&seat->data_devices);
    wl_list_init(&seat->primary_devices);
    wl_list_init(&seat->data_control_devices);
    wl_list_init(&seat->idle_inhibitors);
    wl_list_init(&seat->keyboard_focus_destroy.link);
    wl_list_init(&seat->pointer_focus_destroy.link);
    wl_list_init(&seat->cursor_surface_destroy.link);
    seat->cursor_mode = PURP_CURSOR_MODE_NAMED;
    seat->cursor_shape = PURP_CURSOR_DEFAULT;
    seat->drag.touch_slot = -1;

    seat->xkb_context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    if (!seat->xkb_context) return false;
    struct xkb_rule_names names = {0};
    seat->keymap = xkb_keymap_new_from_names(seat->xkb_context, &names,
                                             XKB_KEYMAP_COMPILE_NO_FLAGS);
    if (!seat->keymap) {
        PURP_ERROR("Failed to compile the default keymap");
        return false;
    }
    seat->xkb_state = xkb_state_new(seat->keymap);
    if (!seat->xkb_state) {
        PURP_ERROR("Failed to create xkb state");
        return false;
    }
    seat->keymap_fd = keymap_to_memfd(seat->keymap, &seat->keymap_size);
    if (seat->keymap_fd < 0)
        PURP_WARN("Failed to export the keymap memfd; clients get no keymap");

    struct wl_display *display = s->display;
    seat->global = wl_global_create(display, &wl_seat_interface, PURP_WL_SEAT_VERSION, s,
                                    seat_bind);
    seat->data_device_manager =
        wl_global_create(display, &wl_data_device_manager_interface,
                         PURP_WL_DATA_DEVICE_MANAGER_VERSION, s, ddm_bind);
    seat->primary_selection_manager =
        wl_global_create(display, &zwp_primary_selection_device_manager_v1_interface,
                         PURP_PRIMARY_SELECTION_VERSION, s, pdm_bind);
    seat->data_control_manager =
        wl_global_create(display, &zwlr_data_control_manager_v1_interface,
                         PURP_DATA_CONTROL_VERSION, s, dcm_bind);
    seat->cursor_shape_manager =
        wl_global_create(display, &wp_cursor_shape_manager_v1_interface,
                         PURP_CURSOR_SHAPE_VERSION, s, cursor_shape_bind);
    seat->tablet_manager =
        wl_global_create(display, &zwp_tablet_manager_v2_interface,
                         PURP_TABLET_MANAGER_VERSION, s, tablet_manager_bind);
    seat->idle_inhibit_manager =
        wl_global_create(display, &zwp_idle_inhibit_manager_v1_interface,
                         PURP_IDLE_INHIBIT_VERSION, s, idle_inhibit_bind);
    seat->xdg_output_manager =
        wl_global_create(display, &zxdg_output_manager_v1_interface,
                         PURP_XDG_OUTPUT_MANAGER_VERSION, s, xdg_output_manager_bind);
    if (!seat->global || !seat->data_device_manager || !seat->primary_selection_manager ||
        !seat->data_control_manager || !seat->cursor_shape_manager || !seat->tablet_manager ||
        !seat->idle_inhibit_manager || !seat->xdg_output_manager) {
        PURP_ERROR("Failed to create seat/data globals");
        purp_seat_finish(s);
        return false;
    }
    return true;
}

void purp_seat_finish(purp_server_t *s) {
    if (!s) return;
    purp_seat_t *seat = server_seat(s);
    if (seat->global) {
        wl_global_destroy(seat->global);
        seat->global = NULL;
    }
    if (seat->data_device_manager) {
        wl_global_destroy(seat->data_device_manager);
        seat->data_device_manager = NULL;
    }
    if (seat->primary_selection_manager) {
        wl_global_destroy(seat->primary_selection_manager);
        seat->primary_selection_manager = NULL;
    }
    if (seat->data_control_manager) {
        wl_global_destroy(seat->data_control_manager);
        seat->data_control_manager = NULL;
    }
    if (seat->cursor_shape_manager) {
        wl_global_destroy(seat->cursor_shape_manager);
        seat->cursor_shape_manager = NULL;
    }
    if (seat->tablet_manager) {
        wl_global_destroy(seat->tablet_manager);
        seat->tablet_manager = NULL;
    }
    if (seat->idle_inhibit_manager) {
        wl_global_destroy(seat->idle_inhibit_manager);
        seat->idle_inhibit_manager = NULL;
    }
    if (seat->xdg_output_manager) {
        wl_global_destroy(seat->xdg_output_manager);
        seat->xdg_output_manager = NULL;
    }
    if (seat->selection && seat->selection->kind == PURP_DATA_SOURCE_COMPOSITOR) {
        purp_data_source_t *compositor = seat->selection;
        seat->selection = NULL;
        purp_data_source_destroy(compositor);
    }
    if (seat->primary_selection &&
        seat->primary_selection->kind == PURP_DATA_SOURCE_COMPOSITOR) {
        purp_data_source_t *compositor = seat->primary_selection;
        seat->primary_selection = NULL;
        purp_data_source_destroy(compositor);
    }
    seat->selection = NULL;
    seat->primary_selection = NULL;
    purp_seat_touch_send_cancel(s, -1);
    drag_reset(seat, false);
    if (seat->keymap_fd >= 0) {
        close(seat->keymap_fd);
        seat->keymap_fd = -1;
    }
    if (seat->xkb_state) {
        xkb_state_unref(seat->xkb_state);
        seat->xkb_state = NULL;
    }
    if (seat->keymap) {
        xkb_keymap_unref(seat->keymap);
        seat->keymap = NULL;
    }
    if (seat->xkb_context) {
        xkb_context_unref(seat->xkb_context);
        seat->xkb_context = NULL;
    }
}
