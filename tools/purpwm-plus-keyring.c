/*
 * purpwm+ session keyring setup and Secret Service controls.
 *
 * Thin wrapper around gnome-keyring-daemon and libsecret. Prompts are handled
 * by GNOME's system prompter; this tool never touches raw secret contents.
 *
 * Subcommands:
 *   start    Spawn the keyring daemon and ensure a "default" collection alias
 *   status   List collections and their lock state (default)
 *   unlock   Create and/or unlock the default collection
 *   manage   Hand off to seahorse ("Passwords and Keys")
 */

#include <libsecret/secret.h>
#include <gio/gio.h>
#include <glib/gstdio.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void die(const char *message)
{
    g_printerr("purpwm+-keyring: %s\n", message);
    exit(1);
}

static void die_error(const char *prefix, const GError *error)
{
    g_printerr("purpwm+-keyring: %s: %s\n", prefix, error->message);
    exit(1);
}

/* Export the session environment into systemd --user / D-Bus activation so
 * D-Bus-activated prompters connect to purpwm's own display. */
static void import_environment(void)
{
    static const char *const candidates[] = {
        "WAYLAND_DISPLAY", "DISPLAY", "XDG_RUNTIME_DIR", "XDG_SESSION_TYPE",
        "XDG_CURRENT_DESKTOP", "GDK_BACKEND", "GNOME_KEYRING_CONTROL",
        "SSH_AUTH_SOCK",
    };
    g_autofree gchar *updater =
        g_find_program_in_path("dbus-update-activation-environment");
    g_autofree gchar *systemctl = NULL;
    GPtrArray *argv;
    gsize i;

    argv = g_ptr_array_new();
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

static SecretService *secret_service(void)
{
    g_autoptr(GError) error = NULL;
    SecretService *service;

    service = secret_service_get_sync(SECRET_SERVICE_LOAD_COLLECTIONS,
                                      NULL, &error);
    if (!service)
        die_error("cannot reach the Secret Service (is a keyring daemon "
                  "running?)", error);
    return service;
}

static void cmd_start(void)
{
    g_autoptr(GError) error = NULL;
    g_autofree gchar *output = NULL;
    g_autofree gchar *daemon = NULL;
    SecretService *service;
    SecretCollection *collection;
    const gchar *argv[4];

    argv[1] = "--start";
    argv[2] = "--components=pkcs11,secrets";
    argv[3] = NULL;
    gchar **lines;
    gsize i;

    daemon = g_find_program_in_path("gnome-keyring-daemon");
    if (!daemon)
        die("install gnome-keyring to enable the OS keyring.");

    /* Prompts are D-Bus activated: they must connect to purpwm's display. */
    import_environment();

    argv[0] = daemon;
    if (!g_spawn_sync(NULL, (gchar **)argv, NULL, G_SPAWN_SEARCH_PATH,
                      NULL, NULL, &output, NULL, NULL, &error))
        die_error("gnome-keyring-daemon failed", error);

    lines = g_strsplit(output ? output : "", "\n", -1);
    for (i = 0; lines[i]; i++) {
        gchar *line = lines[i];
        gchar *equals = strchr(line, '=');
        gchar *value;

        if (!equals)
            continue;
        *equals = '\0';
        value = equals + 1;
        /* gnome-keyring-daemon emits "NAME=value;" lines. */
        g_strchomp(value);
        if (g_str_has_suffix(value, ";"))
            value[strlen(value) - 1] = '\0';
        if (g_strcmp0(line, "GNOME_KEYRING_CONTROL") == 0 ||
            g_strcmp0(line, "SSH_AUTH_SOCK") == 0)
            g_setenv(line, value, TRUE);
    }
    g_strfreev(lines);
    import_environment();

    service = secret_service();
    collection = secret_collection_for_alias_sync(
        service, "default", SECRET_COLLECTION_NONE, NULL, &error);
    if (!collection && !error) {
        g_autoptr(SecretCollection) login =
            secret_collection_for_alias_sync(
                service, "login", SECRET_COLLECTION_NONE, NULL, NULL);
        if (login) {
            if (!secret_service_set_alias_sync(service, "default",
                                               SECRET_COLLECTION(login),
                                               NULL, &error))
                die_error("cannot point the default alias at the login "
                          "keyring", error);
        }
    } else if (collection) {
        g_object_unref(collection);
    }
}

static void cmd_status(void)
{
    g_autoptr(SecretService) service = secret_service();
    GList *collections = secret_service_get_collections(service);
    GList *l;
    gsize count = 0;

    g_print("Secret Service is available.\n");
    for (l = collections; l; l = l->next) {
        SecretCollection *collection = l->data;
        g_autofree gchar *label = NULL;
        const gchar *state;

        count++;
        label = secret_collection_get_label(collection);
        state = secret_collection_get_locked(collection)
            ? "locked" : "unlocked";
        if (label && *label) {
            g_print("%s: %s\n", label, state);
        } else {
            const gchar *path = g_dbus_proxy_get_object_path(
                G_DBUS_PROXY(collection));
            const gchar *base = strrchr(path, '/');
            g_print("%s: %s\n", base ? base + 1 : path, state);
        }
    }
    if (!count)
        g_print("No keyrings yet. Run purpwm+-keyring unlock to create one.\n");
}

static gboolean collection_is_locked(SecretCollection *collection)
{
    /* Reload the property over D-Bus rather than relying on delivery of a
     * change signal. */
    g_autoptr(GVariant) reply = NULL;
    g_autoptr(GVariant) inner = NULL;
    g_autoptr(GError) error = NULL;
    gboolean locked = TRUE;

    reply = g_dbus_proxy_call_sync(
        G_DBUS_PROXY(collection),
        "org.freedesktop.DBus.Properties.Get",
        g_variant_new("(ss)", "org.freedesktop.Secret.Collection", "Locked"),
        G_DBUS_CALL_FLAGS_NONE, 5000, NULL, &error);
    if (!reply)
        return TRUE;
    g_variant_get(reply, "(v)", &inner);
    if (inner && g_variant_is_of_type(inner, G_VARIANT_TYPE_BOOLEAN))
        locked = g_variant_get_boolean(inner);
    return locked;
}

static void cmd_unlock(void)
{
    g_autoptr(SecretService) service = secret_service();
    g_autoptr(GError) error = NULL;
    SecretCollection *collection = NULL;
    GList *items;

    collection = secret_collection_for_alias_sync(
        service, "default", SECRET_COLLECTION_NONE, NULL, NULL);
    if (!collection)
        collection = secret_collection_for_alias_sync(
            service, "login", SECRET_COLLECTION_NONE, NULL, NULL);
    if (!collection) {
        collection = secret_collection_create_sync(
            service, "Login", "default", SECRET_COLLECTION_CREATE_NONE,
            NULL, &error);
        if (!collection)
            die_error("keyring creation was cancelled", error);
    }

    if (secret_collection_get_locked(collection)) {
        /* libsecret delegates the password dialog to GNOME's system
         * prompter. */
        items = g_list_append(NULL, collection);
        if (!secret_service_unlock_sync(service, items, NULL, NULL, &error)) {
            g_list_free(items);
            die_error("unlock failed", error);
        }
        g_list_free(items);
    }

    if (collection_is_locked(collection))
        die("keyring remains locked (unlock was cancelled or failed).");
    g_object_unref(collection);
    g_print("Keyring unlocked.\n");
}

int main(int argc, char **argv)
{
    const char *action = (argc > 1) ? argv[1] : "status";
    g_autofree gchar *runtime = NULL;
    g_autofree gchar *bus = NULL;

    if (g_strcmp0(action, "--help") == 0 || g_strcmp0(action, "-h") == 0) {
        g_print("Usage: %s [start|status|unlock|manage]\n"
                "  start    Start gnome-keyring-daemon and import its environment\n"
                "  status   List keyring collections (default)\n"
                "  unlock   Create and/or unlock the default keyring\n"
                "  manage   Open the Passwords and Keys manager (seahorse)\n",
                argv[0]);
        return 0;
    }

    /* Fall back to the systemd user bus when no address is exported. */
    if (!g_getenv("DBUS_SESSION_BUS_ADDRESS")) {
        const gchar *runtime_dir = g_getenv("XDG_RUNTIME_DIR");
        g_autofree gchar *bus_path = NULL;

        if (!runtime_dir) {
            runtime = g_strdup_printf("/run/user/%u", getuid());
            runtime_dir = runtime;
        }
        bus_path = g_build_filename(runtime_dir, "bus", NULL);
        if (g_file_test(bus_path, G_FILE_TEST_EXISTS)) {
            bus = g_strdup_printf("unix:path=%s", bus_path);
            g_setenv("DBUS_SESSION_BUS_ADDRESS", bus, TRUE);
        }
    }

    if (g_strcmp0(action, "manage") == 0) {
        gchar *seahorse = g_find_program_in_path("seahorse");
        if (!seahorse)
            die("install seahorse for the Passwords and Keys manager.");
        {
            const gchar *const seahorse_argv[] = { "seahorse", NULL };
            execvp("seahorse", (gchar **)seahorse_argv);
        }
        g_free(seahorse);
        die("cannot execute seahorse.");
    } else if (g_strcmp0(action, "start") == 0) {
        cmd_start();
    } else if (g_strcmp0(action, "status") == 0) {
        cmd_status();
    } else if (g_strcmp0(action, "unlock") == 0) {
        cmd_unlock();
    } else {
        g_printerr("purpwm+-keyring: unknown action '%s' "
                   "(expected start, status, unlock or manage)\n", action);
        return 2;
    }
    return 0;
}
