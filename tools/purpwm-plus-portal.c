/*
 * purpwm+ Screenshot portal backend
 *
 * Native backend for purpwm's framebuffer capture transport. Implements
 * org.freedesktop.impl.portal.Screenshot (version 2) on the session bus so
 * sandboxed applications can request a screenshot through xdg-desktop-portal.
 *
 * The compositor serves PNG frames over a dedicated UNIX socket; this backend
 * asks the user for consent (unless --no-ask / PURPWM_PORTAL_AUTO_APPROVE=1),
 * captures one frame, stores it under $XDG_RUNTIME_DIR/purpwm-screenshots and
 * returns a file:// URI.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE /* mkstemps */
#endif

#include <gtk/gtk.h>
#include <gio/gio.h>
#include <glib/gstdio.h>

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#define BUS_NAME "org.freedesktop.impl.portal.desktop.purpwm"
#define FRONTEND "org.freedesktop.portal.Desktop"
#define INTERFACE_NAME "org.freedesktop.impl.portal.Screenshot"
#define OBJECT_PATH "/org/freedesktop/portal/desktop"
#define PORTAL_DESCRIPTOR "purpwm+.portal"
#define PORTALS_CONF "purpwm+-portals.conf"
#define SCREENSHOT_VERSION 2
#define MAX_PENDING 8
#define MAX_PNG_BYTES (256LL * 1024 * 1024)
#define SCREENSHOT_MAX_AGE 3600   /* seconds */
#define CLEANUP_INTERVAL 900      /* 15-minute cleanup timer */

static const char PORTAL_XML[] =
    "<node><interface name=\"org.freedesktop.impl.portal.Screenshot\">"
    "<method name=\"Screenshot\">"
    "<arg type=\"o\" direction=\"in\"/><arg type=\"s\" direction=\"in\"/>"
    "<arg type=\"s\" direction=\"in\"/><arg type=\"a{sv}\" direction=\"in\"/>"
    "<arg type=\"u\" direction=\"out\"/><arg type=\"a{sv}\" direction=\"out\"/>"
    "</method>"
    "<method name=\"PickColor\">"
    "<arg type=\"o\" direction=\"in\"/><arg type=\"s\" direction=\"in\"/>"
    "<arg type=\"s\" direction=\"in\"/><arg type=\"a{sv}\" direction=\"in\"/>"
    "<arg type=\"u\" direction=\"out\"/><arg type=\"a{sv}\" direction=\"out\"/>"
    "</method>"
    "<property name=\"version\" type=\"u\" access=\"read\"/>"
    "</interface></node>";

static const char REQUEST_XML[] =
    "<node><interface name=\"org.freedesktop.impl.portal.Request\">"
    "<method name=\"Close\"/></interface></node>";

static const char DIALOG_CSS[] =
    "window {"
    "  background-color: rgba(23, 11, 41, 0.92);"
    "  color: #F3E8FF;"
    "  border: 2px solid #A855F7;"
    "  border-radius: 12px;"
    "}"
    "button {"
    "  background-color: #2A1447;"
    "  color: #E9D5FF;"
    "  border: 1px solid #4C1D95;"
    "  border-radius: 6px;"
    "  padding: 8px 16px;"
    "}"
    "button.suggested-action {"
    "  background-color: #7C3AED;"
    "  color: #FFFFFF;"
    "  font-weight: bold;"
    "}"
    "button.suggested-action:hover {"
    "  background-color: #9333EA;"
    "}"
    "label { color: #D8B4FE; }";

typedef struct {
    GDBusMethodInvocation *invocation;
    guint registration;
    GtkWidget *dialog;
    char *sender;
    gchar *handle; /* owned copy; outlives the GVariant it was parsed from */
} PendingRequest;

/* Owned job payload handed to the detached capture thread. */
typedef struct {
    gchar *handle;
    gchar **endpoints;
} CaptureJob;

typedef struct {
    GDBusConnection *connection;
    GDBusNodeInfo *info;
    GDBusNodeInfo *request_info;
    guint registration;
    GHashTable *pending; /* handle (owned) -> PendingRequest* (owned) */
    gboolean auto_approve;
    gboolean css_installed;
} Backend;

typedef struct {
    char *handle;
    guint response;
    char *uri; /* NULL on failure */
} CaptureResult;

static Backend backend;
static GMainLoop *main_loop;
static GStrv endpoints; /* capture socket candidates, in order */
static pid_t initial_ppid;
static gboolean session_mode_arg;

static void on_request_method_call(GDBusConnection *connection,
                                   const gchar *sender,
                                   const gchar *object_path,
                                   const gchar *interface_name,
                                   const gchar *method_name,
                                   GVariant *parameters,
                                   GDBusMethodInvocation *invocation,
                                   gpointer user_data);

static const GDBusInterfaceVTable request_vtable;

/* Only the portal frontend may drive this backend. */
static gboolean sender_is_frontend(const char *sender);

static void die(const char *message)
{
    g_printerr("purpwm+-portal: %s\n", message);
    exit(1);
}

/* ------------------------------------------------------------------ *
 * Filesystem / configuration setup                                    *
 * ------------------------------------------------------------------ */

/* Keep portal selection specific to purpwm; retain installed file chooser
 * and other distro backends by symlinking their descriptors in. */
static void link_matching_files(const char *directory,
                                const char **bases,
                                gsize n_bases,
                                const char *subdirectory,
                                const char *suffix,
                                const char *skip)
{
    g_autoptr(GHashTable) seen = NULL;
    gsize i;

    seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    g_hash_table_add(seen, g_strdup(skip));

    for (i = 0; i < n_bases; i++) {
        g_autofree gchar *dir_path = NULL;
        GDir *dir;
        const gchar *name;

        dir_path = g_build_filename(bases[i], subdirectory, NULL);
        dir = g_dir_open(dir_path, 0, NULL);
        if (!dir)
            continue;
        while ((name = g_dir_read_name(dir)) != NULL) {
            g_autofree gchar *source = NULL, *target = NULL;

            if (!g_str_has_suffix(name, suffix))
                continue;
            if (g_hash_table_contains(seen, name))
                continue;
            g_hash_table_add(seen, g_strdup(name));
            source = g_build_filename(dir_path, name, NULL);
            target = g_build_filename(directory, name, NULL);
            if (g_file_test(target, G_FILE_TEST_IS_SYMLINK))
                unlink(target);
            if (!g_file_test(target, G_FILE_TEST_EXISTS))
                symlink(source, target);
        }
        g_dir_close(dir);
    }
}

/*
 * Merge our backend into an existing portals.conf, preserving unrelated
 * sections and keys. Returns newly allocated file content and sets *changed.
 */
static gchar *merge_portals_conf(const gchar *existing, gboolean *changed)
{
    GString *out = g_string_new(NULL);
    gchar **lines;
    gboolean in_preferred = FALSE;
    gboolean seen_preferred = FALSE;
    gboolean have_key = FALSE;
    gboolean have_default = FALSE;
    gsize i;

    *changed = FALSE;
    if (!existing) {
        *changed = TRUE;
        g_string_append(out, "[preferred]\n"
                             "default=gtk;*\n"
                             INTERFACE_NAME "=purpwm\n");
        return g_string_free(out, FALSE);
    }

    lines = g_strsplit(existing, "\n", -1);
    for (i = 0; lines[i]; i++) {
        gchar *line = lines[i];
        gchar *trimmed = g_strstrip(line);
        gboolean last = lines[i + 1] == NULL;

        if (*trimmed == '[') {
            if (in_preferred) {
                if (!have_default) {
                    g_string_append(out, "default=gtk;*\n");
                    *changed = TRUE;
                }
                if (!have_key) {
                    g_string_append(out, INTERFACE_NAME "=purpwm\n");
                    *changed = TRUE;
                }
                in_preferred = FALSE;
            }
            if (g_strcmp0(trimmed, "[preferred]") == 0) {
                in_preferred = TRUE;
                seen_preferred = TRUE;
            }
            g_string_append(out, lines[i]);
            if (!last || *line)
                g_string_append_c(out, '\n');
            continue;
        }

        if (in_preferred && *trimmed && *trimmed != '#') {
            const gchar *equals = strchr(trimmed, '=');
            if (equals) {
                gsize key_len = (gsize)(equals - trimmed);
                while (key_len > 0 &&
                       (trimmed[key_len - 1] == ' ' ||
                        trimmed[key_len - 1] == '\t'))
                    key_len--;
                if (key_len == strlen("default") &&
                    strncmp(trimmed, "default", key_len) == 0) {
                    have_default = TRUE;
                } else if (key_len == strlen(INTERFACE_NAME) &&
                           strncmp(trimmed, INTERFACE_NAME, key_len) == 0) {
                    g_autofree gchar *value = g_strdup(equals + 1);
                    g_strstrip(value);
                    if (g_strcmp0(value, "purpwm") != 0)
                        *changed = TRUE;
                    have_key = TRUE;
                    g_string_append(out, INTERFACE_NAME "=purpwm");
                    if (!last || *line)
                        g_string_append_c(out, '\n');
                    continue;
                }
            }
        }
        g_string_append(out, lines[i]);
        if (!last || *line)
            g_string_append_c(out, '\n');
    }

    if (in_preferred) {
        if (!have_default) {
            g_string_append(out, "default=gtk;*\n");
            *changed = TRUE;
        }
        if (!have_key) {
            g_string_append(out, INTERFACE_NAME "=purpwm\n");
            *changed = TRUE;
        }
    }
    if (!seen_preferred) {
        g_string_append(out, "[preferred]\n"
                             "default=gtk;*\n"
                             INTERFACE_NAME "=purpwm\n");
        *changed = TRUE;
    }

    g_strfreev(lines);
    return g_string_free(out, FALSE);
}

static void write_file_or_die(const char *path, const char *content)
{
    g_autoptr(GError) error = NULL;

    if (!g_file_set_contents(path, content, -1, &error)) {
        g_autofree gchar *message =
            g_strdup_printf("cannot write %s: %s", path, error->message);
        die(message);
    }
}

static void import_environment(void)
{
    static const char *const candidates[] = {
        "XDG_DESKTOP_PORTAL_DIR", "XDG_CURRENT_DESKTOP", "XDG_SESSION_TYPE",
        "WAYLAND_DISPLAY", "DISPLAY", "GDK_BACKEND", "PURPWM_CAPTURE_SOCKET",
    };
    g_autofree gchar *updater = NULL;
    g_autofree gchar *systemctl = NULL;
    GPtrArray *argv = g_ptr_array_new();
    gsize i;

    updater = g_find_program_in_path("dbus-update-activation-environment");
    if (updater) {
        g_ptr_array_add(argv, updater);
        g_ptr_array_add(argv, (gpointer) "--systemd");
    } else if ((systemctl = g_find_program_in_path("systemctl")) != NULL) {
        g_ptr_array_add(argv, systemctl);
        g_ptr_array_add(argv, (gpointer) "--user");
        g_ptr_array_add(argv, (gpointer) "import-environment");
    } else {
        g_ptr_array_free(argv, TRUE);
        return;
    }
    for (i = 0; i < G_N_ELEMENTS(candidates); i++)
        if (g_getenv(candidates[i]))
            g_ptr_array_add(argv, (gpointer) candidates[i]);
    g_ptr_array_add(argv, NULL);
    g_spawn_sync(NULL, (gchar **)argv->pdata, NULL,
                 G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL, NULL, NULL);
    g_ptr_array_free(argv, TRUE);
}

static void portal_setup(void)
{
    const char *data_home = g_get_user_data_dir();
    const char *config_home = g_get_user_config_dir();
    g_autofree gchar *directory = NULL;
    g_autofree gchar *descriptor = NULL, *descriptor_path = NULL;
    g_autofree gchar *config_dir = NULL, *config_path = NULL;
    g_autofree gchar *existing = NULL, *merged = NULL;
    g_autofree gchar *dir_conf = NULL;
    g_autofree gchar *services = NULL, *service_path = NULL;
    g_autofree gchar *service = NULL;
    gboolean changed = FALSE;
    const gchar *const *data_dirs;
    const gchar *const *config_dirs;
    g_autofree gchar **portal_bases = NULL; /* elements not owned */
    g_autofree gchar **conf_bases = NULL;
    GPtrArray *bases;
    gchar exe[4096];
    ssize_t len;

    directory = g_build_filename(data_home, "purpwm+", "portals", NULL);
    if (g_mkdir_with_parents(directory, 0700) != 0)
        die("cannot create the portal backend directory");
    g_chmod(directory, 0700);

    /* Symlink every distro *.portal descriptor into the merged directory. */
    bases = g_ptr_array_new();
    g_ptr_array_add(bases, (gpointer) data_home);
    for (data_dirs = g_get_system_data_dirs(); *data_dirs; data_dirs++)
        g_ptr_array_add(bases, (gpointer) *data_dirs);
    g_ptr_array_add(bases, NULL);
    portal_bases = (gchar **) g_ptr_array_free(bases, FALSE);
    link_matching_files(directory, (const char **)portal_bases,
                        g_strv_length(portal_bases),
                        "xdg-desktop-portal/portals", ".portal",
                        PORTAL_DESCRIPTOR);

    descriptor =
        g_strdup_printf("[portal]\nDBusName=%s\nInterfaces=%s;\nUseIn=purpwm\n",
                        BUS_NAME, INTERFACE_NAME);
    descriptor_path = g_build_filename(directory, PORTAL_DESCRIPTOR, NULL);
    write_file_or_die(descriptor_path, descriptor);

    /* User configuration takes precedence over distro defaults. */
    config_dir = g_build_filename(config_home, "xdg-desktop-portal", NULL);
    if (g_mkdir_with_parents(config_dir, 0755) != 0)
        die("cannot create the xdg-desktop-portal config directory");
    config_path = g_build_filename(config_dir, PORTALS_CONF, NULL);

    g_file_get_contents(config_path, &existing, NULL, NULL);
    merged = merge_portals_conf(existing, &changed);
    if (changed && existing) {
        g_autofree gchar *backup = g_strconcat(config_path, ".bak", NULL);
        if (!g_file_test(backup, G_FILE_TEST_EXISTS)) {
            g_autoptr(GFile) from = g_file_new_for_path(config_path);
            g_autoptr(GFile) to = g_file_new_for_path(backup);
            g_file_copy(from, to, G_FILE_COPY_NONE, NULL, NULL, NULL, NULL);
        }
    }
    write_file_or_die(config_path, merged);
    /* XDG_DESKTOP_PORTAL_DIR also overrides the frontend's config search path. */
    dir_conf = g_build_filename(directory, PORTALS_CONF, NULL);
    write_file_or_die(dir_conf, merged);

    /* Keep other desktops' portal configs reachable from the merged dir. */
    bases = g_ptr_array_new();
    g_ptr_array_add(bases, (gpointer) config_home);
    for (config_dirs = g_get_system_config_dirs(); *config_dirs; config_dirs++)
        g_ptr_array_add(bases, (gpointer) *config_dirs);
    g_ptr_array_add(bases, (gpointer) "/usr/local/share");
    g_ptr_array_add(bases, (gpointer) "/usr/share");
    g_ptr_array_add(bases, NULL);
    conf_bases = (gchar **) g_ptr_array_free(bases, FALSE);
    link_matching_files(directory, (const char **)conf_bases,
                        g_strv_length(conf_bases),
                        "xdg-desktop-portal", "portals.conf",
                        PORTALS_CONF);

    /* D-Bus activation service for this backend. */
    services = g_build_filename(data_home, "dbus-1", "services", NULL);
    if (g_mkdir_with_parents(services, 0755) != 0)
        die("cannot create the D-Bus services directory");
    len = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (len < 0)
        die("cannot resolve /proc/self/exe");
    exe[len] = '\0';
    {
        /* Escape for the Exec= line of a .service file. */
        GString *escaped = g_string_new(NULL);
        const gchar *p;
        for (p = exe; *p; p++) {
            if (*p == '\\' || *p == '"')
                g_string_append_c(escaped, '\\');
            g_string_append_c(escaped, *p);
        }
        service = g_strdup_printf("[D-BUS Service]\nName=%s\nExec=\"%s\"\n",
                                  BUS_NAME, escaped->str);
        g_string_free(escaped, TRUE);
    }
    service_path = g_build_filename(services, BUS_NAME ".service", NULL);
    write_file_or_die(service_path, service);

    g_setenv("XDG_DESKTOP_PORTAL_DIR", directory, TRUE);
    import_environment();
}

/* ------------------------------------------------------------------ *
 * Compositor capture transport                                        *
 * ------------------------------------------------------------------ */

static int connect_unix(const char *path)
{
    int fd;
    struct sockaddr_un addr;
    struct timeval timeout = { .tv_sec = 10, .tv_usec = 0 };

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    if (strlen(path) >= sizeof(addr.sun_path)) {
        close(fd);
        errno = ENAMETOOLONG;
        return -1;
    }
    strcpy(addr.sun_path, path);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static ssize_t read_full(int fd, void *buffer, size_t count)
{
    size_t done = 0;

    while (done < count) {
        ssize_t n = read(fd, (char *)buffer + done, count - done);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (n == 0)
            break;
        done += (size_t)n;
    }
    return (ssize_t)done;
}

/* Read one "\n"-terminated status line (bounded). */
static gboolean read_header(int fd, char *buffer, size_t size)
{
    size_t i = 0;

    while (i + 1 < size) {
        ssize_t n = read(fd, buffer + i, 1);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return FALSE;
        }
        if (n == 0)
            return FALSE;
        if (buffer[i] == '\n') {
            buffer[i + 1] = '\0';
            return TRUE;
        }
        i++;
    }
    return FALSE;
}

static GBytes *capture_png(const char *endpoint, GError **error)
{
    static const guchar PNG_MAGIC[8] = { 0x89, 'P', 'N', 'G',
                                         '\r', '\n', 0x1a, '\n' };
    int fd;
    char header[64];
    long long size;
    guchar *data;

    fd = connect_unix(endpoint);
    if (fd < 0) {
        g_set_error(error, G_IO_ERROR, g_io_error_from_errno(errno),
                    "cannot reach capture socket %s: %s",
                    endpoint, g_strerror(errno));
        return NULL;
    }
    if (write(fd, "capture\n", 8) != 8 ||
        !read_header(fd, header, sizeof(header))) {
        close(fd);
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "capture request failed for %s", endpoint);
        return NULL;
    }
    if (!g_str_has_prefix(header, "OK ")) {
        close(fd);
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "capture denied or compositor unavailable");
        return NULL;
    }
    size = strtoll(header + 3, NULL, 10);
    if (size < 8 || size > MAX_PNG_BYTES) {
        close(fd);
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "invalid screenshot size");
        return NULL;
    }
    data = g_malloc((gsize)size);
    if (read_full(fd, data, (gsize)size) != (ssize_t)size ||
        memcmp(data, PNG_MAGIC, 8) != 0) {
        close(fd);
        g_free(data);
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "incomplete screenshot");
        return NULL;
    }
    close(fd);
    return g_bytes_new_take(data, (gsize)size);
}

/* Try each candidate socket in order; returns the first good capture. */
static GBytes *capture_from_endpoints(GStrv candidates, GError **error)
{
    gsize i, count = g_strv_length(candidates);

    for (i = 0; i < count; i++) {
        g_autoptr(GError) local = NULL;
        GBytes *png = capture_png(candidates[i], &local);
        if (png)
            return png;
        if (i == count - 1)
            g_propagate_error(error, g_steal_pointer(&local));
    }
    return NULL;
}

/* ------------------------------------------------------------------ *
 * Screenshot file bookkeeping                                         *
 * ------------------------------------------------------------------ */

static gchar *screenshot_dir(void)
{
    const char *runtime = g_getenv("XDG_RUNTIME_DIR");
    gchar *dir = g_build_filename(runtime, "purpwm-screenshots", NULL);

    g_mkdir_with_parents(dir, 0700);
    g_chmod(dir, 0700);
    return dir;
}

/* 0600 temp file named portal-XXXXXX.png; returns path or NULL. */
static gchar *store_screenshot(GBytes *png)
{
    g_autofree gchar *dir = screenshot_dir();
    gchar *path = g_build_filename(dir, "portal-XXXXXX.png", NULL);
    gsize size;
    gconstpointer data = g_bytes_get_data(png, &size);
    int fd;
    size_t done = 0;

    fd = mkstemps(path, 4);
    if (fd < 0) {
        g_free(path);
        return NULL;
    }
    while (done < size) {
        ssize_t n = write(fd, (const char *)data + done, size - done);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            close(fd);
            unlink(path);
            g_free(path);
            return NULL;
        }
        done += (size_t)n;
    }
    if (close(fd) != 0) {
        unlink(path);
        g_free(path);
        return NULL;
    }
    return path;
}

static gboolean cleanup_screenshots(gpointer user_data)
{
    g_autofree gchar *dir = screenshot_dir();
    GDir *listing;
    const gchar *name;
    time_t now = time(NULL);

    (void)user_data;
    listing = g_dir_open(dir, 0, NULL);
    if (!listing)
        return G_SOURCE_CONTINUE;
    while ((name = g_dir_read_name(listing)) != NULL) {
        g_autofree gchar *path = NULL;
        struct stat st;

        if (!g_str_has_prefix(name, "portal-") ||
            !g_str_has_suffix(name, ".png"))
            continue;
        path = g_build_filename(dir, name, NULL);
        if (stat(path, &st) == 0 && now - st.st_mtime > SCREENSHOT_MAX_AGE)
            unlink(path);
    }
    g_dir_close(listing);
    return G_SOURCE_CONTINUE;
}

/* ------------------------------------------------------------------ *
 * D-Bus backend                                                       *
 * ------------------------------------------------------------------ */

/* Only the portal frontend may drive this backend. */
static gboolean sender_is_frontend(const char *sender)
{
    g_autoptr(GVariant) reply = NULL;
    const char *owner = NULL;

    if (!sender)
        return FALSE;
    reply = g_dbus_connection_call_sync(
        backend.connection, "org.freedesktop.DBus", "/org/freedesktop/DBus",
        "org.freedesktop.DBus", "GetNameOwner",
        g_variant_new("(s)", FRONTEND),
        G_VARIANT_TYPE("(s)"), G_DBUS_CALL_FLAGS_NONE, 1000, NULL, NULL);
    if (!reply)
        return FALSE;
    g_variant_get(reply, "(&s)", &owner);
    return g_strcmp0(sender, owner) == 0;
}

static void pending_free(PendingRequest *pending)
{
    g_clear_object(&pending->invocation);
    if (pending->dialog)
        gtk_window_destroy(GTK_WINDOW(pending->dialog));
    g_free(pending->sender);
    g_free(pending->handle);
    g_free(pending);
}

/* Schedule the completion of one request; returns FALSE for g_idle_add. */
static gboolean finish_request(const char *handle, guint response,
                               const char *uri)
{
    gpointer orig_key = NULL, val = NULL;
    PendingRequest *pending;
    GVariantBuilder results;

    /* Steal so the value destroy notify cannot free `pending` while this
     * function still uses it; the stolen key is freed at the end. */
    if (!g_hash_table_steal_extended(backend.pending, handle, &orig_key, &val))
        return FALSE;
    pending = val;
    if (pending->dialog) {
        gtk_window_destroy(GTK_WINDOW(pending->dialog));
        pending->dialog = NULL;
    }
    g_dbus_connection_unregister_object(backend.connection,
                                        pending->registration);
    g_variant_builder_init(&results, G_VARIANT_TYPE("a{sv}"));
    if (uri)
        g_variant_builder_add(&results, "{sv}", "uri",
                              g_variant_new_string(uri));
    g_dbus_method_invocation_return_value(
        pending->invocation,
        g_variant_new("(ua{sv})", response, &results));
    pending_free(pending);
    g_free(orig_key);
    return FALSE;
}

static gboolean finish_request_idempotent(gpointer data)
{
    CaptureResult *result = data;
    gchar *handle = g_steal_pointer(&result->handle);
    gchar *uri = g_steal_pointer(&result->uri);

    /* If the request already vanished, drop a stray screenshot file. */
    if (!g_hash_table_contains(backend.pending, handle) && uri) {
        g_autofree gchar *path =
            g_filename_from_uri(uri, NULL, NULL);
        if (path)
            unlink(path);
    }
    finish_request(handle, result->response, uri);
    g_free(handle);
    g_free(uri);
    g_free(result);
    return G_SOURCE_REMOVE;
}

/* Worker thread: capture the frame and park it under XDG_RUNTIME_DIR. */
static gpointer capture_worker(gpointer data)
{
    CaptureJob *job = data;
    g_autoptr(GError) error = NULL;
    GBytes *png = capture_from_endpoints(job->endpoints, &error);
    CaptureResult *result = g_new0(CaptureResult, 1);

    result->handle = g_steal_pointer(&job->handle);
    if (png) {
        gchar *path = store_screenshot(png);
        g_bytes_unref(png);
        if (path) {
            result->uri = g_filename_to_uri(path, NULL, NULL);
            g_free(path);
            result->response = 0;
        } else {
            result->response = 2;
            g_printerr("purpwm+-portal: cannot store screenshot\n");
        }
    } else {
        result->response = 2;
        g_printerr("purpwm+-portal: %s\n",
                   error ? error->message : "capture failed");
    }
    g_idle_add(finish_request_idempotent, result);
    g_strfreev(job->endpoints);
    g_free(job);
    return NULL;
}

static gboolean begin_capture(gpointer data)
{
    gchar *handle = data; /* owned by this timeout source */
    CaptureJob *job;

    if (!g_hash_table_contains(backend.pending, handle)) {
        g_free(handle);
        return G_SOURCE_REMOVE;
    }
    /* Copy endpoints so the detached worker never reads globals after exit. */
    job = g_new0(CaptureJob, 1);
    job->handle = g_strdup(handle);
    job->endpoints = g_strdupv(endpoints);
    /* The GThread handle is not needed once the worker is detached. */
    g_thread_unref(g_thread_new("purpwm-capture", (GThreadFunc)capture_worker,
                                job));
    g_free(handle);
    return G_SOURCE_REMOVE;
}

static void approve_request(const char *handle)
{
    PendingRequest *pending = g_hash_table_lookup(backend.pending, handle);

    if (!pending)
        return;
    if (pending->dialog) {
        gtk_window_destroy(GTK_WINDOW(pending->dialog));
        pending->dialog = NULL;
    }
    /* Let the compositor remove the consent window before capturing. The key can
     * be freed by finish_request before this fires, so the timeout owns a copy. */
    g_timeout_add(150, begin_capture, g_strdup(handle));
}

static void install_dialog_css(void)
{
    GtkCssProvider *provider;

    if (backend.css_installed)
        return;
    provider = gtk_css_provider_new();
    gtk_css_provider_load_from_string(provider, DIALOG_CSS);
    gtk_style_context_add_provider_for_display(
        gdk_display_get_default(), GTK_STYLE_PROVIDER(provider),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(provider);
    backend.css_installed = TRUE;
}

static void on_cancel_clicked(GtkWidget *widget, gpointer data)
{
    (void)widget;
    finish_request(data, 1, NULL);
}

static void on_share_clicked(GtkWidget *widget, gpointer data)
{
    (void)widget;
    approve_request(data);
}

static gboolean on_dialog_close(GtkWindow *window, gpointer data)
{
    (void)window;
    finish_request(data, 1, NULL);
    return TRUE;
}

static void ask_permission(const char *handle, const char *app_id)
{
    PendingRequest *pending;
    GtkWidget *window, *box, *label, *buttons, *cancel, *capture;
    g_autofree gchar *text = NULL;

    if (backend.auto_approve) {
        approve_request(handle);
        return;
    }

    if (!gtk_init_check()) {
        g_printerr("purpwm+-portal: cannot open a display for the "
                   "permission dialog\n");
        finish_request(handle, 2, NULL);
        return;
    }
    install_dialog_css();

    pending = g_hash_table_lookup(backend.pending, handle);
    if (!pending)
        return;

    window = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(window), "Screenshot — purpwm");
    gtk_window_set_default_size(GTK_WINDOW(window), 440, 180);
    gtk_window_set_modal(GTK_WINDOW(window), TRUE);

    box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 18);
    gtk_widget_set_margin_top(box, 24);
    gtk_widget_set_margin_bottom(box, 24);
    gtk_widget_set_margin_start(box, 24);
    gtk_widget_set_margin_end(box, 24);

    text = g_strdup_printf("Allow %s to capture your screen?",
                           (app_id && *app_id) ? app_id : "this application");
    label = gtk_label_new(text);
    gtk_label_set_wrap(GTK_LABEL(label), TRUE);
    gtk_box_append(GTK_BOX(box), label);

    buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_set_halign(buttons, GTK_ALIGN_END);
    cancel = gtk_button_new_with_label("Cancel");
    capture = gtk_button_new_with_label("Share screen");
    gtk_widget_add_css_class(capture, "suggested-action");
    g_signal_connect(cancel, "clicked",
                     G_CALLBACK(on_cancel_clicked), (gpointer)pending->handle);
    g_signal_connect(capture, "clicked",
                     G_CALLBACK(on_share_clicked), (gpointer)pending->handle);
    gtk_box_append(GTK_BOX(buttons), cancel);
    gtk_box_append(GTK_BOX(buttons), capture);
    gtk_box_append(GTK_BOX(box), buttons);

    gtk_window_set_child(GTK_WINDOW(window), box);
    g_signal_connect(window, "close-request",
                     G_CALLBACK(on_dialog_close), (gpointer)pending->handle);
    pending->dialog = window;
    gtk_window_present(GTK_WINDOW(window));
}

static void on_portal_method_call(GDBusConnection *connection,
                                  const gchar *sender,
                                  const gchar *object_path,
                                  const gchar *interface_name,
                                  const gchar *method_name,
                                  GVariant *parameters,
                                  GDBusMethodInvocation *invocation,
                                  gpointer user_data)
{
    const char *handle = NULL, *app_id = NULL, *parent_window = NULL;
    GVariant *options = NULL;
    PendingRequest *pending;
    guint registration;
    GError *error = NULL;

    (void)object_path;
    (void)interface_name;
    (void)user_data;

    if (!sender_is_frontend(sender)) {
        g_dbus_method_invocation_return_dbus_error(
            invocation, "org.freedesktop.DBus.Error.AccessDenied",
            "Only the desktop portal may call this backend");
        return;
    }

    /* `handle` is D-Bus type `o` (object path) in PORTAL_XML for both
     * Screenshot and PickColor; a plain `s` here would misparse the tuple. */
    g_variant_get(parameters, "(&o&s&s@a{sv})",
                  &handle, &app_id, &parent_window, &options);
    (void)parent_window;
    if (options)
        g_variant_unref(options);

    if (g_strcmp0(method_name, "Screenshot") != 0 ||
        g_hash_table_size(backend.pending) >= MAX_PENDING ||
        g_hash_table_contains(backend.pending, handle)) {
        GVariantBuilder empty;

        g_variant_builder_init(&empty, G_VARIANT_TYPE("a{sv}"));
        g_dbus_method_invocation_return_value(
            invocation, g_variant_new("(ua{sv})", 2, &empty));
        return;
    }

    registration = g_dbus_connection_register_object(
        connection, handle, backend.request_info->interfaces[0],
        &request_vtable, NULL, NULL, &error);
    if (registration == 0) {
        /* A colliding or invalid handle must not hang the D-Bus caller. */
        GVariantBuilder empty;

        g_clear_error(&error);
        g_variant_builder_init(&empty, G_VARIANT_TYPE("a{sv}"));
        g_dbus_method_invocation_return_value(
            invocation, g_variant_new("(ua{sv})", 2, &empty));
        return;
    }

    pending = g_new0(PendingRequest, 1);
    pending->invocation = g_object_ref(invocation);
    pending->registration = registration;
    pending->sender = g_strdup(sender);
    pending->handle = g_strdup(handle);
    g_hash_table_insert(backend.pending, g_strdup(handle), pending);

    ask_permission(handle, app_id);
}

static void on_request_method_call(GDBusConnection *connection,
                                   const gchar *sender,
                                   const gchar *object_path,
                                   const gchar *interface_name,
                                   const gchar *method_name,
                                   GVariant *parameters,
                                   GDBusMethodInvocation *invocation,
                                   gpointer user_data)
{
    PendingRequest *pending;

    (void)connection;
    (void)interface_name;
    (void)method_name;
    (void)parameters;
    (void)user_data;

    pending = g_hash_table_lookup(backend.pending, object_path);
    if (pending && g_strcmp0(sender, pending->sender) == 0) {
        finish_request(object_path, 1, NULL);
        g_dbus_method_invocation_return_value(invocation, NULL);
    } else {
        g_dbus_method_invocation_return_dbus_error(
            invocation, "org.freedesktop.DBus.Error.AccessDenied",
            "Not your request");
    }
}
static const GDBusInterfaceVTable request_vtable = {
    on_request_method_call,
    NULL,
    NULL,
    { 0 }
};


static GVariant *on_get_property(GDBusConnection *connection,
                                 const gchar *sender,
                                 const gchar *object_path,
                                 const gchar *interface_name,
                                 const gchar *property_name,
                                 GError **error,
                                 gpointer user_data)
{
    (void)connection;
    (void)sender;
    (void)object_path;
    (void)interface_name;
    (void)property_name;
    (void)error;
    (void)user_data;
    return g_variant_new_uint32(SCREENSHOT_VERSION);
}

static const GDBusInterfaceVTable portal_vtable = {
    on_portal_method_call,
    on_get_property,
    NULL,
    { 0 }
};

static void on_name_acquired(GDBusConnection *connection,
                             const gchar *name,
                             gpointer user_data)
{
    (void)connection;
    (void)name;
    (void)user_data;
    g_print("purpwm+ Screenshot portal ready\n");
    fflush(stdout);
    /* The frontend caches its backend choice, so refresh it after
     * registering when running as the session instance. */
    if (session_mode_arg) {
        const char *const argv[] = {
            "systemctl", "--user", "try-restart",
            "xdg-desktop-portal.service", NULL
        };
        g_spawn_async(NULL, (gchar **)argv, NULL,
                      G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL |
                      G_SPAWN_STDERR_TO_DEV_NULL,
                      NULL, NULL, NULL, NULL);
    }
}

static void on_name_lost(GDBusConnection *connection,
                         const gchar *name,
                         gpointer user_data)
{
    (void)connection;
    (void)name;
    (void)user_data;
    if (main_loop)
        g_main_loop_quit(main_loop);
}

static gboolean parent_alive(gpointer user_data)
{
    (void)user_data;
    if (getppid() != initial_ppid) {
        g_main_loop_quit(main_loop);
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

static void usage(const char *prog)
{
    g_printerr(
        "Usage: %s [--install] [--session] [--no-ask]\n"
        "  --install   Install per-user portal selection and exit\n"
        "  --session   Refresh the portal frontend for this desktop session\n"
        "  --no-ask    Approve screenshots without a permission dialog\n"
        "              (also PURPWM_PORTAL_AUTO_APPROVE=1)\n",
        prog);
    exit(2);
}

int main(int argc, char **argv)
{
    gboolean install_mode = FALSE;
    const char *runtime;
    const char *capture_env;
    const char *wayland_display;
    g_autofree gchar *plus_sock = NULL, *legacy_sock = NULL;
    g_autoptr(GError) error = NULL;
    guint owner_id;
    int i;

    /* Vulkan WSI can stall on nested/sandboxed displays; keep GTK on the
     * GL renderer. */
    g_setenv("GSK_RENDERER", "gl", FALSE);

    for (i = 1; i < argc; i++) {
        if (g_strcmp0(argv[i], "--install") == 0)
            install_mode = TRUE;
        else if (g_strcmp0(argv[i], "--session") == 0)
            session_mode_arg = TRUE;
        else if (g_strcmp0(argv[i], "--no-ask") == 0)
            backend.auto_approve = TRUE;
        else
            usage(argv[0]);
    }

    runtime = g_getenv("XDG_RUNTIME_DIR");
    if (!runtime || !*runtime)
        die("XDG_RUNTIME_DIR is required");

    if (g_strcmp0(g_getenv("PURPWM_PORTAL_AUTO_APPROVE"), "1") == 0)
        backend.auto_approve = TRUE;

    portal_setup();
    if (install_mode) {
        g_print("Installed purpwm's Screenshot portal configuration.\n");
        return 0;
    }

    /* Capture socket candidates, in fallback order. */
    wayland_display = g_getenv("WAYLAND_DISPLAY");
    if (!wayland_display || !*wayland_display)
        wayland_display = "wayland-1";
    plus_sock = g_strdup_printf("%s/purpwm+-capture-%s.sock",
                                runtime, wayland_display);
    legacy_sock = g_strdup_printf("%s/purpwm-capture-%s.sock",
                                  runtime, wayland_display);
    capture_env = g_getenv("PURPWM_CAPTURE_SOCKET");
    if (capture_env && *capture_env) {
        endpoints = g_new0(gchar *, 4);
        endpoints[0] = g_strdup(capture_env);
        endpoints[1] = g_steal_pointer(&plus_sock);
        endpoints[2] = g_steal_pointer(&legacy_sock);
    } else {
        endpoints = g_new0(gchar *, 3);
        endpoints[0] = g_steal_pointer(&plus_sock);
        endpoints[1] = g_steal_pointer(&legacy_sock);
    }

    backend.connection = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &error);
    if (!backend.connection) {
        g_autofree gchar *message = g_strdup_printf(
            "cannot connect to the session bus: %s", error->message);
        die(message);
    }
    backend.pending = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                            (GDestroyNotify)pending_free);
    backend.info = g_dbus_node_info_new_for_xml(PORTAL_XML, &error);
    backend.request_info = g_dbus_node_info_new_for_xml(REQUEST_XML, &error);
    if (!backend.info || !backend.request_info)
        die("bad portal introspection XML");

    cleanup_screenshots(NULL);
    g_timeout_add_seconds(CLEANUP_INTERVAL, cleanup_screenshots, NULL);

    backend.registration = g_dbus_connection_register_object(
        backend.connection, OBJECT_PATH, backend.info->interfaces[0],
        &portal_vtable, NULL, NULL, &error);
    if (backend.registration == 0) {
        g_autofree gchar *message = g_strdup_printf(
            "cannot register %s: %s", OBJECT_PATH, error->message);
        die(message);
    }

    main_loop = g_main_loop_new(NULL, FALSE);

    /* --session: quit when the launcher reparents us. */
    if (session_mode_arg) {
        initial_ppid = getppid();
        g_timeout_add_seconds(2, parent_alive, NULL);
    }

    owner_id = g_bus_own_name_on_connection(
        backend.connection, BUS_NAME,
        G_BUS_NAME_OWNER_FLAGS_ALLOW_REPLACEMENT |
        G_BUS_NAME_OWNER_FLAGS_REPLACE,
        on_name_acquired, on_name_lost, NULL, NULL);

    g_main_loop_run(main_loop);

    /* Answer any in-flight calls before dropping the name. */
    {
        GList *handles = g_hash_table_get_keys(backend.pending);
        GList *l;

        for (l = handles; l; l = l->next)
            finish_request(l->data, 2, NULL);
        g_list_free(handles);
    }
    g_bus_unown_name(owner_id);
    g_main_loop_unref(main_loop);
    g_strfreev(endpoints);
    return 0;
}
