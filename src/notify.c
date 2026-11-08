#include "ui.h"

#include <gio/gio.h>
#include <glib.h>
#include <sys/stat.h>

struct purp_notify_dbus {
    purp_notify_center_t *center;
    pthread_t thread;
    bool thread_started;
    GMainContext *context;
    GMainLoop *loop;
    GDBusConnection *connection;
    GDBusNodeInfo *node_info;
    guint registration_id;
    guint owner_id;
    GHashTable *live_ids;
    uint32_t next_id;

    pthread_mutex_t lock;
    pthread_cond_t cond;
    bool ready;
    bool start_ok;
};

static const char notification_xml[] =
    "<node>"
    "  <interface name='" PURP_NOTIFY_DBUS_NAME "'>"
    "    <method name='GetCapabilities'>"
    "      <arg type='as' name='caps' direction='out'/>"
    "    </method>"
    "    <method name='Notify'>"
    "      <arg type='s' name='app_name' direction='in'/>"
    "      <arg type='u' name='replaces_id' direction='in'/>"
    "      <arg type='s' name='app_icon' direction='in'/>"
    "      <arg type='s' name='summary' direction='in'/>"
    "      <arg type='s' name='body' direction='in'/>"
    "      <arg type='as' name='actions' direction='in'/>"
    "      <arg type='a{sv}' name='hints' direction='in'/>"
    "      <arg type='i' name='expire_timeout' direction='in'/>"
    "      <arg type='u' name='id' direction='out'/>"
    "    </method>"
    "    <method name='CloseNotification'>"
    "      <arg type='u' name='id' direction='in'/>"
    "    </method>"
    "    <signal name='NotificationClosed'>"
    "      <arg type='u' name='id'/>"
    "      <arg type='u' name='reason'/>"
    "    </signal>"
    "    <method name='GetServerInformation'>"
    "      <arg type='s' name='name' direction='out'/>"
    "      <arg type='s' name='vendor' direction='out'/>"
    "      <arg type='s' name='version' direction='out'/>"
    "      <arg type='s' name='spec_version' direction='out'/>"
    "    </method>"
    "  </interface>"
    "</node>";

static void notify_copy(char *dst, size_t dst_len, const char *src) {
    if (!dst || dst_len == 0) return;
    if (!src) src = "";
    size_t len = strlen(src);
    if (len >= dst_len) len = dst_len - 1;
    memcpy(dst, src, len);
    dst[len] = '\0';
}

static uint64_t notify_deadline(uint64_t now, uint64_t duration) {
    if (duration == 0) return 0;
    if (UINT64_MAX - now < duration) return UINT64_MAX;
    return now + duration;
}

static void emit_closed(purp_notify_dbus_t *dbus, uint32_t id, purp_notify_close_reason_t reason) {
    if (!dbus) return;
    GDBusConnection *connection = NULL;
    pthread_mutex_lock(&dbus->lock);
    if (dbus->live_ids) g_hash_table_remove(dbus->live_ids, GUINT_TO_POINTER(id));
    if (dbus->connection) connection = g_object_ref(dbus->connection);
    pthread_mutex_unlock(&dbus->lock);
    if (!connection) return;
    GError *error = NULL;
    g_dbus_connection_emit_signal(connection, NULL, PURP_NOTIFY_DBUS_PATH, PURP_NOTIFY_DBUS_NAME,
                                  "NotificationClosed", g_variant_new("(uu)", id, (uint32_t)reason),
                                  &error);
    if (error) g_error_free(error);
    g_object_unref(connection);
}

static uint32_t allocate_dbus_id(purp_notify_dbus_t *dbus, uint32_t replaces_id) {
    uint32_t id = replaces_id;
    if (id == 0) {
        id = dbus->next_id;
        if (id == 0 || id > PURP_NOTIFY_MAX_ID) id = 1;
        while (g_hash_table_contains(dbus->live_ids, GUINT_TO_POINTER(id)))
            id = id % PURP_NOTIFY_MAX_ID + 1;
        dbus->next_id = id % PURP_NOTIFY_MAX_ID + 1;
    } else {
        uint32_t following = id % PURP_NOTIFY_MAX_ID + 1;
        if (following > dbus->next_id) dbus->next_id = following;
    }
    g_hash_table_add(dbus->live_ids, GUINT_TO_POINTER(id));
    return id;
}

static uint8_t notification_urgency(GVariant *hints) {
    if (!hints) return 1;
    GVariant *value = g_variant_lookup_value(hints, "urgency", NULL);
    if (!value) return 1;
    uint64_t raw = 1;
    const GVariantType *type = g_variant_get_type(value);
    if (g_variant_type_equal(type, G_VARIANT_TYPE_BYTE)) raw = g_variant_get_byte(value);
    else if (g_variant_type_equal(type, G_VARIANT_TYPE_INT16)) raw = (uint64_t)g_variant_get_int16(value);
    else if (g_variant_type_equal(type, G_VARIANT_TYPE_UINT16)) raw = g_variant_get_uint16(value);
    else if (g_variant_type_equal(type, G_VARIANT_TYPE_INT32)) raw = (uint64_t)g_variant_get_int32(value);
    else if (g_variant_type_equal(type, G_VARIANT_TYPE_UINT32)) raw = g_variant_get_uint32(value);
    else if (g_variant_type_equal(type, G_VARIANT_TYPE_INT64)) raw = (uint64_t)g_variant_get_int64(value);
    else if (g_variant_type_equal(type, G_VARIANT_TYPE_UINT64)) raw = g_variant_get_uint64(value);
    else if (g_variant_type_equal(type, G_VARIANT_TYPE_VARIANT)) {
        GVariant *inner = g_variant_get_variant(value);
        g_variant_unref(value);
        value = inner;
        if (g_variant_is_of_type(value, G_VARIANT_TYPE_BYTE)) raw = g_variant_get_byte(value);
    }
    g_variant_unref(value);
    return raw == 0 ? 0 : (raw >= 2 ? 2 : 1);
}

static void notification_method_call(GDBusConnection *connection, const gchar *sender,
                                     const gchar *object_path, const gchar *interface_name,
                                     const gchar *method_name, GVariant *parameters,
                                     GDBusMethodInvocation *invocation, gpointer user_data) {
    (void)connection;
    (void)sender;
    (void)object_path;
    (void)interface_name;
    purp_notify_dbus_t *dbus = user_data;
    if (strcmp(method_name, "GetCapabilities") == 0) {
        const char *capabilities[] = { "body", NULL };
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(^as)", capabilities));
        return;
    }
    if (strcmp(method_name, "GetServerInformation") == 0) {
        g_dbus_method_invocation_return_value(
            invocation, g_variant_new("(ssss)", PURP_NAME, PURP_NAME, PURPWM_VERSION, "1.2"));
        return;
    }
    if (strcmp(method_name, "CloseNotification") == 0) {
        uint32_t id;
        g_variant_get(parameters, "(u)", &id);
        if (id == 0) {
            g_dbus_method_invocation_return_dbus_error(
                invocation, PURP_NOTIFY_DBUS_NAME ".Error.InvalidId", "Notification ID 0 is invalid");
            return;
        }
        emit_closed(dbus, id, PURP_NOTIFY_CLOSED_BY_CALL);
        purp_notify_event_t event;
        memset(&event, 0, sizeof(event));
        event.kind = PURP_NOTIFY_EVENT_CLOSE;
        event.close_id = id;
        purp_notify_enqueue(dbus->center, &event);
        g_dbus_method_invocation_return_value(invocation, NULL);
        return;
    }
    if (strcmp(method_name, "Notify") != 0) {
        g_dbus_method_invocation_return_dbus_error(invocation, "org.freedesktop.DBus.Error.UnknownMethod",
                                                   "Unknown notification method");
        return;
    }

    const char *app = "", *icon = "", *summary = "", *body = "";
    uint32_t replaces_id = 0;
    int32_t expire_timeout = -1;
    GVariant *actions = NULL;
    GVariant *hints = NULL;
    g_variant_get(parameters, "(&su&s&s&s@as@a{sv}i)", &app, &replaces_id, &icon, &summary, &body,
                  &actions, &hints, &expire_timeout);
    (void)icon;
    if (actions) g_variant_unref(actions);

    purp_notify_event_t event;
    memset(&event, 0, sizeof(event));
    event.kind = PURP_NOTIFY_EVENT_SHOW;
    event.expire_timeout = expire_timeout;
    event.notification.urgency = notification_urgency(hints);
    if (hints) g_variant_unref(hints);
    purp_utf8_sanitize_line(event.notification.app_name, sizeof(event.notification.app_name), app,
                            PURP_NOTIFY_APP_MAX_CHARS);
    purp_utf8_sanitize_line(event.notification.summary, sizeof(event.notification.summary), summary,
                            PURP_NOTIFY_SUMMARY_MAX_CHARS);
    purp_utf8_sanitize_line(event.notification.body, sizeof(event.notification.body), body,
                            PURP_NOTIFY_BODY_MAX_CHARS);
    pthread_mutex_lock(&dbus->lock);
    event.notification.id = allocate_dbus_id(dbus, replaces_id);
    pthread_mutex_unlock(&dbus->lock);
    purp_notify_enqueue(dbus->center, &event);
    g_dbus_method_invocation_return_value(invocation, g_variant_new("(u)", event.notification.id));
}

static const GDBusInterfaceVTable notification_vtable = {
    .method_call = notification_method_call,
};

static void name_lost(GDBusConnection *connection, const gchar *name, gpointer user_data) {
    (void)connection;
    (void)name;
    purp_notify_dbus_t *dbus = user_data;
    if (dbus->loop) g_main_loop_quit(dbus->loop);
}

static gboolean quit_main_loop(gpointer data) {
    GMainLoop *loop = data;
    if (loop) g_main_loop_quit(loop);
    return G_SOURCE_REMOVE;
}

static void dbus_set_ready(purp_notify_dbus_t *dbus, bool ok) {
    pthread_mutex_lock(&dbus->lock);
    dbus->start_ok = ok;
    dbus->ready = true;
    pthread_cond_signal(&dbus->cond);
    pthread_mutex_unlock(&dbus->lock);
}

static void *notify_dbus_thread(void *userdata) {
    purp_notify_dbus_t *dbus = userdata;
    GError *error = NULL;
    bool ok = false;
    dbus->context = g_main_context_new();
    g_main_context_push_thread_default(dbus->context);
    dbus->loop = g_main_loop_new(dbus->context, FALSE);
    dbus->connection = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &error);
    if (!dbus->connection) goto ready;
    dbus->node_info = g_dbus_node_info_new_for_xml(notification_xml, &error);
    if (!dbus->node_info) goto ready;
    dbus->registration_id = g_dbus_connection_register_object(
        dbus->connection, PURP_NOTIFY_DBUS_PATH, dbus->node_info->interfaces[0],
        &notification_vtable, dbus, NULL, &error);
    if (dbus->registration_id == 0) goto ready;
    dbus->owner_id = g_bus_own_name_on_connection(
        dbus->connection, PURP_NOTIFY_DBUS_NAME,
        G_BUS_NAME_OWNER_FLAGS_REPLACE | G_BUS_NAME_OWNER_FLAGS_ALLOW_REPLACEMENT,
        NULL, name_lost, dbus, NULL);
    ok = true;

ready:
    if (!ok && error) {
        PURP_WARN("Unable to start notification D-Bus: %s", error->message);
        g_clear_error(&error);
    }
    dbus_set_ready(dbus, ok);
    if (ok) g_main_loop_run(dbus->loop);
    if (dbus->owner_id) {
        g_bus_unown_name(dbus->owner_id);
        dbus->owner_id = 0;
    }
    if (dbus->registration_id) {
        g_dbus_connection_unregister_object(dbus->connection, dbus->registration_id);
        dbus->registration_id = 0;
    }
    pthread_mutex_lock(&dbus->lock);
    GDBusConnection *connection = dbus->connection;
    dbus->connection = NULL;
    pthread_mutex_unlock(&dbus->lock);
    if (connection) g_object_unref(connection);
    if (dbus->node_info) {
        g_dbus_node_info_unref(dbus->node_info);
        dbus->node_info = NULL;
    }
    g_main_context_pop_thread_default(dbus->context);
    return NULL;
}

static void dbus_destroy(purp_notify_dbus_t *dbus) {
    if (!dbus) return;
    if (dbus->loop) g_main_loop_unref(dbus->loop);
    if (dbus->context) g_main_context_unref(dbus->context);
    if (dbus->live_ids) g_hash_table_destroy(dbus->live_ids);
    pthread_cond_destroy(&dbus->cond);
    pthread_mutex_destroy(&dbus->lock);
    free(dbus);
}

bool purp_notify_init(purp_notify_center_t *center) {
    if (!center) return false;
    memset(center, 0, sizeof(*center));
    center->next_local_id = 1;
    if (pthread_mutex_init(&center->queue_lock, NULL) != 0) return false;
    center->queue = calloc(PURP_NOTIFY_QUEUE_CAPACITY, sizeof(*center->queue));
    if (!center->queue) {
        pthread_mutex_destroy(&center->queue_lock);
        return false;
    }
    return true;
}

bool purp_notify_start_dbus(purp_notify_center_t *center, const char *runtime_dir) {
    if (!center || center->dbus) return false;
    if (!getenv("DBUS_SESSION_BUS_ADDRESS") && runtime_dir && *runtime_dir) {
        char *socket = g_build_filename(runtime_dir, "bus", NULL);
        struct stat st;
        if (stat(socket, &st) == 0) {
            char *address = g_strdup_printf("unix:path=%s", socket);
            g_setenv("DBUS_SESSION_BUS_ADDRESS", address, TRUE);
            g_free(address);
        }
        g_free(socket);
    }
    purp_notify_dbus_t *dbus = calloc(1, sizeof(*dbus));
    if (!dbus) return false;
    dbus->center = center;
    dbus->next_id = 1;
    if (pthread_mutex_init(&dbus->lock, NULL) != 0) {
        free(dbus);
        return false;
    }
    if (pthread_cond_init(&dbus->cond, NULL) != 0) {
        pthread_mutex_destroy(&dbus->lock);
        free(dbus);
        return false;
    }
    dbus->live_ids = g_hash_table_new(g_direct_hash, g_direct_equal);
    if (!dbus->live_ids) {
        pthread_cond_destroy(&dbus->cond);
        pthread_mutex_destroy(&dbus->lock);
        free(dbus);
        return false;
    }
    center->dbus = dbus;
    if (pthread_create(&dbus->thread, NULL, notify_dbus_thread, dbus) != 0) {
        center->dbus = NULL;
        dbus_destroy(dbus);
        return false;
    }
    dbus->thread_started = true;
    pthread_mutex_lock(&dbus->lock);
    while (!dbus->ready) pthread_cond_wait(&dbus->cond, &dbus->lock);
    bool ok = dbus->start_ok;
    pthread_mutex_unlock(&dbus->lock);
    if (!ok) {
        pthread_join(dbus->thread, NULL);
        center->dbus = NULL;
        dbus_destroy(dbus);
    }
    return ok;
}

void purp_notify_finish(purp_notify_center_t *center) {
    if (!center) return;
    purp_notify_dbus_t *dbus = center->dbus;
    center->dbus = NULL;
    if (dbus) {
        if (dbus->context) g_main_context_invoke(dbus->context, quit_main_loop, dbus->loop);
        if (dbus->thread_started) pthread_join(dbus->thread, NULL);
        dbus_destroy(dbus);
    }
    for (size_t i = 0; i < PURP_NOTIFY_CACHE_ENTRIES; ++i)
        purp_pixel_buf_free(&center->cache[i].buffer);
    free(center->queue);
    pthread_mutex_destroy(&center->queue_lock);
    memset(center, 0, sizeof(*center));
}

uint64_t purp_notify_timeout_ms(int32_t expire_timeout, uint8_t urgency) {
    if (expire_timeout == 0) return 0;
    if (expire_timeout < 0)
        return urgency >= 2 ? PURP_NOTIFY_CRITICAL_TIMEOUT_MS : PURP_NOTIFY_DEFAULT_TIMEOUT_MS;
    uint64_t timeout = (uint64_t)expire_timeout;
    if (timeout < 1) timeout = 1;
    if (timeout > PURP_NOTIFY_MAX_TIMEOUT_MS) timeout = PURP_NOTIFY_MAX_TIMEOUT_MS;
    return timeout;
}

void purp_notify_enqueue(purp_notify_center_t *center, const purp_notify_event_t *event) {
    if (!center || !event || !center->queue) return;
    pthread_mutex_lock(&center->queue_lock);
    if (center->queue_len >= PURP_NOTIFY_QUEUE_CAPACITY) {
        center->queue_head = (center->queue_head + 1) % PURP_NOTIFY_QUEUE_CAPACITY;
        center->queue_len--;
    }
    size_t tail = (center->queue_head + center->queue_len) % PURP_NOTIFY_QUEUE_CAPACITY;
    center->queue[tail] = *event;
    center->queue_len++;
    pthread_mutex_unlock(&center->queue_lock);
}

static uint32_t allocate_local_id(purp_notify_center_t *center) {
    uint32_t id = center->next_local_id;
    if (id == 0 || id > PURP_NOTIFY_MAX_ID) id = 1;
    for (;;) {
        bool live = false;
        for (size_t i = 0; i < center->toast_count; ++i)
            if (center->toasts[i].id == id) live = true;
        if (!live) break;
        id = id % PURP_NOTIFY_MAX_ID + 1;
    }
    center->next_local_id = id % PURP_NOTIFY_MAX_ID + 1;
    return id;
}

static void insert_toast(purp_notify_center_t *center, const purp_notification_t *toast) {
    for (size_t i = 0; i < center->toast_count; ++i) {
        if (center->toasts[i].id == toast->id) {
            center->toasts[i] = *toast;
            center->content_generation++;
            return;
        }
    }
    size_t move = center->toast_count < PURP_NOTIFY_MAX_VISIBLE ? center->toast_count
                                                                : PURP_NOTIFY_MAX_VISIBLE - 1;
    if (move) memmove(center->toasts + 1, center->toasts, move * sizeof(center->toasts[0]));
    center->toasts[0] = *toast;
    if (center->toast_count < PURP_NOTIFY_MAX_VISIBLE) center->toast_count++;
    center->content_generation++;
}

static bool remove_toast(purp_notify_center_t *center, size_t index) {
    if (index >= center->toast_count) return false;
    if (index + 1 < center->toast_count)
        memmove(center->toasts + index, center->toasts + index + 1,
                (center->toast_count - index - 1) * sizeof(center->toasts[0]));
    center->toast_count--;
    memset(&center->toasts[center->toast_count], 0, sizeof(center->toasts[0]));
    center->content_generation++;
    return true;
}

uint32_t purp_notify_push(purp_notify_center_t *center, uint32_t replaces_id, const char *app,
                          const char *summary, const char *body, uint8_t urgency,
                          uint64_t timeout_ms) {
    if (!center) return 0;
    uint32_t id = replaces_id ? replaces_id : allocate_local_id(center);
    if (replaces_id) center->next_local_id = id % PURP_NOTIFY_MAX_ID + 1;
    if (timeout_ms == 0) timeout_ms = PURP_NOTIFY_DEFAULT_TIMEOUT_MS;
    else {
        if (timeout_ms < 1000) timeout_ms = 1000;
        if (timeout_ms > 30000) timeout_ms = 30000;
    }
    purp_notification_t toast;
    memset(&toast, 0, sizeof(toast));
    toast.id = id;
    toast.urgency = urgency;
    toast.expires_at_ms = notify_deadline(purp_now_ms(), timeout_ms);
    purp_utf8_sanitize_line(toast.app_name, sizeof(toast.app_name), app, PURP_NOTIFY_APP_MAX_CHARS);
    purp_utf8_sanitize_line(toast.summary, sizeof(toast.summary), summary, PURP_NOTIFY_SUMMARY_MAX_CHARS);
    purp_utf8_sanitize_line(toast.body, sizeof(toast.body), body, PURP_NOTIFY_BODY_MAX_CHARS);
    insert_toast(center, &toast);
    return id;
}

bool purp_notify_tick(purp_notify_center_t *center, uint64_t now_ms) {
    if (!center) return false;
    bool changed = false;
    for (;;) {
        purp_notify_event_t event;
        pthread_mutex_lock(&center->queue_lock);
        if (center->queue_len == 0) {
            pthread_mutex_unlock(&center->queue_lock);
            break;
        }
        event = center->queue[center->queue_head];
        memset(&center->queue[center->queue_head], 0, sizeof(center->queue[0]));
        center->queue_head = (center->queue_head + 1) % PURP_NOTIFY_QUEUE_CAPACITY;
        center->queue_len--;
        pthread_mutex_unlock(&center->queue_lock);

        if (event.kind == PURP_NOTIFY_EVENT_CLOSE) {
            for (size_t i = 0; i < center->toast_count; ++i)
                if (center->toasts[i].id == event.close_id) {
                    remove_toast(center, i);
                    changed = true;
                    break;
                }
        } else if (event.kind == PURP_NOTIFY_EVENT_SHOW) {
            purp_notification_t toast;
            memset(&toast, 0, sizeof(toast));
            toast.id = event.notification.id;
            toast.urgency = event.notification.urgency;
            uint64_t timeout = purp_notify_timeout_ms(event.expire_timeout, toast.urgency);
            toast.expires_at_ms = notify_deadline(now_ms, timeout);
            purp_utf8_sanitize_line(toast.app_name, sizeof(toast.app_name), event.notification.app_name,
                                    PURP_NOTIFY_APP_MAX_CHARS);
            purp_utf8_sanitize_line(toast.summary, sizeof(toast.summary), event.notification.summary,
                                    PURP_NOTIFY_SUMMARY_MAX_CHARS);
            purp_utf8_sanitize_line(toast.body, sizeof(toast.body), event.notification.body,
                                    PURP_NOTIFY_BODY_MAX_CHARS);
            insert_toast(center, &toast);
            changed = true;
        }
    }

    size_t i = 0;
    while (i < center->toast_count) {
        if (center->toasts[i].expires_at_ms != 0 && center->toasts[i].expires_at_ms <= now_ms) {
            uint32_t id = center->toasts[i].id;
            remove_toast(center, i);
            emit_closed(center->dbus, id, PURP_NOTIFY_CLOSED_EXPIRED);
            changed = true;
        } else {
            i++;
        }
    }
    return changed;
}

bool purp_notify_is_empty(const purp_notify_center_t *center) {
    return !center || center->toast_count == 0;
}

size_t purp_notify_count(const purp_notify_center_t *center) {
    return center ? center->toast_count : 0;
}

bool purp_notify_stack_geometry(const purp_notify_center_t *center, int32_t output_width,
                                bool bar_visible, purp_rect_t *out) {
    if (!center || !out || center->toast_count == 0) return false;
    output_width = purp_max_i32(output_width, 0);
    int32_t width = purp_min_i32(PURP_NOTIFY_TOAST_WIDTH, output_width);
    int32_t count = (int32_t)center->toast_count;
    *out = purp_rect_make(purp_max_i32(output_width - width - PURP_GAP_OUTER, 0),
                          (bar_visible ? PURP_BAR_HEIGHT : 0) + PURP_GAP_OUTER, width,
                          count * PURP_NOTIFY_TOAST_HEIGHT + (count - 1) * PURP_NOTIFY_TOAST_GAP);
    return true;
}

bool purp_notify_dismiss_at(purp_notify_center_t *center, purp_pointf_t pt,
                            purp_rect_t output_rect, bool bar_visible) {
    if (!center || !purp_rect_contains_f(output_rect, pt)) return false;
    purp_pointf_t local = { pt.x - output_rect.x, pt.y - output_rect.y };
    purp_rect_t stack;
    if (!purp_notify_stack_geometry(center, output_rect.w, bar_visible, &stack) ||
        !purp_rect_contains_f(stack, local))
        return false;
    double relative_y = local.y - stack.y;
    double stride = PURP_NOTIFY_TOAST_HEIGHT + PURP_NOTIFY_TOAST_GAP;
    size_t index = (size_t)floor(relative_y / stride);
    if (index >= center->toast_count || fmod(relative_y, stride) >= PURP_NOTIFY_TOAST_HEIGHT)
        return false;
    uint32_t id = center->toasts[index].id;
    remove_toast(center, index);
    emit_closed(center->dbus, id, PURP_NOTIFY_CLOSED_DISMISSED);
    return true;
}

static int32_t notify_px(int32_t value, double scale) {
    return purp_round_i32((double)value * scale);
}

static void draw_notify_top(purp_font_t *font, purp_pixel_buf_t *buffer, const char *text, int32_t x,
                            int32_t top, float px, int32_t max_w, purp_rgba_t color) {
    purp_font_draw_clipped(font, buffer, text, x, top + (int32_t)(px * 0.8f), px, max_w, color);
}

static void notify_redraw(purp_notify_center_t *center, purp_font_t *font,
                          purp_notify_cache_entry_t *entry, double scale) {
    purp_pixel_buf_t *buffer = &entry->buffer;
    int32_t w = entry->width;
    int32_t card_h = notify_px(PURP_NOTIFY_TOAST_HEIGHT, scale);
    int32_t gap = notify_px(PURP_NOTIFY_TOAST_GAP, scale);
    int32_t border = purp_max_i32(notify_px(2, scale), 1);
    float title_px = (PURP_BAR_FONT_SIZE + 0.5f) * (float)scale;
    float body_px = (PURP_BAR_FONT_SIZE - 1.0f) * (float)scale;
    purp_pixel_buf_clear(buffer, (purp_rgba_t){0});
    for (size_t i = 0; i < center->toast_count; ++i) {
        const purp_notification_t *toast = &center->toasts[i];
        int32_t y = (int32_t)i * (card_h + gap);
        purp_rgba_t border_color = toast->urgency >= 2 ? PURP_TOAST_CRITICAL_RGBA : PURP_BAR_TAG_ACTIVE_RGBA;
        purp_fill_rect(buffer, 0, y, w, card_h, border_color);
        purp_fill_rect(buffer, border, y + border, w - 2 * border, card_h - 2 * border, PURP_BAR_BG_RGBA);
        purp_fill_rect(buffer, border, y + border, purp_max_i32(notify_px(4, scale), 2),
                       card_h - 2 * border, PURP_BAR_ACCENT_RGBA);
        int32_t text_x = notify_px(14, scale);
        int32_t max_text_w = purp_max_i32(w - text_x - notify_px(12, scale), notify_px(40, scale));
        char header[PURP_NOTIFY_APP_MAX_CHARS * 4 + PURP_NOTIFY_SUMMARY_MAX_CHARS * 4 + 8];
        if (!toast->app_name[0]) notify_copy(header, sizeof(header), toast->summary);
        else if (!toast->summary[0]) notify_copy(header, sizeof(header), toast->app_name);
        else snprintf(header, sizeof(header), "%s \xC2\xB7 %s", toast->app_name, toast->summary);
        draw_notify_top(font, buffer, header, text_x, y + notify_px(10, scale), title_px,
                        max_text_w, PURP_BAR_TEXT_FG_RGBA);
        const char *sub = toast->body[0] ? toast->body : "Click or tap to dismiss";
        draw_notify_top(font, buffer, sub, text_x, y + notify_px(36, scale), body_px,
                        max_text_w, PURP_BAR_TEXT_DIM_RGBA);
    }
    purp_pixel_buf_touch(buffer);
}

static void notify_cache_clear(purp_notify_center_t *center) {
    for (size_t i = 0; i < PURP_NOTIFY_CACHE_ENTRIES; ++i) {
        purp_pixel_buf_free(&center->cache[i].buffer);
        memset(&center->cache[i], 0, sizeof(center->cache[i]));
    }
}

const purp_pixel_buf_t *purp_notify_render(purp_notify_center_t *center, purp_font_t *font,
                                           int32_t output_width_logical, bool bar_visible,
                                           double scale, purp_point_t *origin) {
    if (!center || !origin || !isfinite(scale) || scale <= 0) return NULL;
    purp_rect_t stack;
    if (!purp_notify_stack_geometry(center, output_width_logical, bar_visible, &stack) || stack.w <= 0)
        return NULL;
    origin->x = purp_round_i32((double)stack.x * scale);
    origin->y = purp_round_i32((double)stack.y * scale);
    int32_t width = purp_max_i32(purp_round_i32((double)stack.w * scale), 1);
    int32_t card_h = notify_px(PURP_NOTIFY_TOAST_HEIGHT, scale);
    int32_t gap = notify_px(PURP_NOTIFY_TOAST_GAP, scale);
    int32_t height = (int32_t)center->toast_count * card_h +
                     (int32_t)(center->toast_count - 1) * gap;
    height = purp_max_i32(height, 1);

    purp_notify_cache_entry_t *entry = NULL, *free_entry = NULL;
    for (size_t i = 0; i < PURP_NOTIFY_CACHE_ENTRIES; ++i) {
        purp_notify_cache_entry_t *candidate = &center->cache[i];
        if (!candidate->in_use) {
            if (!free_entry) free_entry = candidate;
            continue;
        }
        if (candidate->scale == scale) {
            entry = candidate;
            break;
        }
    }
    if (!entry) {
        if (!free_entry) {
            notify_cache_clear(center);
            free_entry = &center->cache[0];
        }
        entry = free_entry;
    }
    if (entry->in_use && entry->width == width && entry->height == height &&
        entry->generation == center->content_generation)
        return &entry->buffer;
    if (!purp_pixel_buf_ensure(&entry->buffer, width, height)) return NULL;
    entry->in_use = true;
    entry->scale = scale;
    entry->width = width;
    entry->height = height;
    entry->generation = center->content_generation;
    notify_redraw(center, font, entry, scale);
    return &entry->buffer;
}
