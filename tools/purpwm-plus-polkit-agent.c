/*
 * purpwm+ Polkit Authentication Agent
 *
 * Ultra-lightweight, translucent-purple styled graphical authentication dialog
 * built with GTK4 and libpolkit-agent-1. Registers with polkitd on the system
 * bus to handle authorization requests (e.g. gparted, wireshark, system
 * updates).
 *
 * The agent D-Bus interface is implemented directly with GDBus (rather than
 * subclassing PolkitAgentListener) so request completion stays fully under our
 * control: BeginAuthentication is answered only once the dialog resolves.
 */

#ifndef POLKIT_AGENT_I_KNOW_API_IS_SUBJECT_TO_CHANGE
#define POLKIT_AGENT_I_KNOW_API_IS_SUBJECT_TO_CHANGE
#endif

#include <gtk/gtk.h>
#include <polkitagent/polkitagent.h>
#include <polkit/polkit.h>

#include <string.h>
#include <unistd.h>

#define AGENT_PATH "/org/purpwm/PolicyKit1/AuthenticationAgent"
#define POLKIT_BUS_NAME "org.freedesktop.PolicyKit1"

/* The authentication agent D-Bus interface polkitd calls into. */
static const char AGENT_INTROSPECTION[] =
    "<node>"
    "  <interface name=\"org.freedesktop.PolicyKit1.AuthenticationAgent\">"
    "    <method name=\"BeginAuthentication\">"
    "      <arg type=\"s\" name=\"action_id\" direction=\"in\"/>"
    "      <arg type=\"s\" name=\"message\" direction=\"in\"/>"
    "      <arg type=\"s\" name=\"icon_name\" direction=\"in\"/>"
    "      <arg type=\"a{ss}\" name=\"details\" direction=\"in\"/>"
    "      <arg type=\"s\" name=\"cookie\" direction=\"in\"/>"
    "      <arg type=\"a(sa{sv})\" name=\"identities\" direction=\"in\"/>"
    "    </method>"
    "    <method name=\"CancelAuthentication\">"
    "      <arg type=\"s\" name=\"cookie\" direction=\"in\"/>"
    "    </method>"
    "  </interface>"
    "</node>";

static const char DIALOG_CSS[] =
    "window {"
    "  background-color: rgba(23, 11, 41, 0.92);"
    "  color: #F3E8FF;"
    "  border: 2px solid #A855F7;"
    "  border-radius: 12px;"
    "}"
    "headerbar {"
    "  background-color: #24123E;"
    "  color: #F3E8FF;"
    "  border-bottom: 1px solid #7C3AED;"
    "}"
    "entry {"
    "  background-color: #2A1447;"
    "  color: #FAF5FF;"
    "  border: 1px solid #7C3AED;"
    "  border-radius: 6px;"
    "  padding: 8px 12px;"
    "  font-size: 14px;"
    "}"
    "entry:focus {"
    "  border: 1px solid #C084FC;"
    "  box-shadow: 0 0 6px #A855F7;"
    "}"
    "button.suggested-action {"
    "  background-color: #7C3AED;"
    "  color: #FFFFFF;"
    "  font-weight: bold;"
    "  border-radius: 6px;"
    "  padding: 8px 16px;"
    "}"
    "button.suggested-action:hover {"
    "  background-color: #9333EA;"
    "}"
    "button {"
    "  background-color: #2A1447;"
    "  color: #E9D5FF;"
    "  border: 1px solid #4C1D95;"
    "  border-radius: 6px;"
    "  padding: 8px 16px;"
    "}"
    "label.title {"
    "  font-weight: bold;"
    "  font-size: 16px;"
    "  color: #F3E8FF;"
    "}"
    "label.msg {"
    "  color: #D8B4FE;"
    "  font-size: 13px;"
    "}"
    "label.error {"
    "  color: #F43F5E;"
    "  font-size: 12px;"
    "  font-weight: bold;"
    "}";

typedef struct {
    GtkWidget *window;
    GtkWidget *prompt_label;
    GtkWidget *entry;
    GtkWidget *error_label;
    PolkitIdentity *identity;
    PolkitAgentSession *session;
    GDBusMethodInvocation *invocation; /* the BeginAuthentication call */
    char *cookie;
    gboolean completed;
    gboolean cancelled;
} PurpAuthDialog;

static GApplication *app;
static GDBusConnection *system_bus;
static GHashTable *dialogs; /* cookie (owned) -> PurpAuthDialog* (owned) */
static pid_t initial_ppid;

static void auth_cancel(PurpAuthDialog *auth);
static void auth_start_session(PurpAuthDialog *auth);

static void auth_free(PurpAuthDialog *auth)
{
    if (auth->session) {
        g_signal_handlers_disconnect_by_data(auth->session, auth);
        g_clear_object(&auth->session);
    }
    if (auth->window) {
        gtk_window_destroy(GTK_WINDOW(auth->window));
        g_clear_object(&auth->window);
    }
    g_clear_object(&auth->invocation);
    g_clear_object(&auth->identity);
    g_free(auth->cookie);
    g_free(auth);
}

static void auth_show_error(PurpAuthDialog *auth, const char *text)
{
    gtk_label_set_text(GTK_LABEL(auth->error_label), text ? text : "");
    gtk_widget_set_visible(auth->error_label, TRUE);
}

static void auth_finish(PurpAuthDialog *auth, gboolean gained_authorization)
{
    GDBusMethodInvocation *invocation;

    (void)gained_authorization;
    if (auth->completed)
        return;
    auth->completed = TRUE;
    /* Detach before answering so a late CancelAuthentication cannot find us. */
    g_hash_table_steal(dialogs, auth->cookie);
    invocation = auth->invocation;
    auth->invocation = NULL;
    if (auth->cancelled)
        g_dbus_method_invocation_return_dbus_error(
            invocation, "org.freedesktop.PolicyKit1.Error.Cancelled",
            "Authentication dialog was dismissed by the user");
    else
        g_dbus_method_invocation_return_value(invocation, NULL);
    g_object_unref(invocation);
    auth_free(auth);
}

static void auth_cancel(PurpAuthDialog *auth)
{
    if (auth->completed)
        return;
    auth->cancelled = TRUE;
    if (auth->session)
        polkit_agent_session_cancel(auth->session);
    auth_finish(auth, FALSE);
}

static void on_session_request(PolkitAgentSession *session,
                               const gchar *request,
                               gboolean echo_on,
                               gpointer user_data)
{
    PurpAuthDialog *auth = user_data;
    g_autofree gchar *text = NULL;

    (void)session;
    (void)echo_on;
    if (request) {
        text = g_strdup(request);
        g_strstrip(text);
        if (*text) {
            gtk_label_set_text(GTK_LABEL(auth->prompt_label), text);
            gtk_widget_set_visible(auth->prompt_label, TRUE);
        }
    }
    gtk_widget_grab_focus(auth->entry);
}

static void on_session_show_error(PolkitAgentSession *session,
                                  const gchar *text,
                                  gpointer user_data)
{
    (void)session;
    auth_show_error(user_data, text);
}

static void on_session_show_info(PolkitAgentSession *session,
                                 const gchar *text,
                                 gpointer user_data)
{
    PurpAuthDialog *auth = user_data;
    g_autofree gchar *info = NULL;

    (void)session;
    if (!text)
        return;
    info = g_strdup(text);
    g_strstrip(info);
    if (*info) {
        gtk_label_set_text(GTK_LABEL(auth->prompt_label), info);
        gtk_widget_set_visible(auth->prompt_label, TRUE);
    }
}

static void on_session_completed(PolkitAgentSession *session,
                                 gboolean gained_authorization,
                                 gpointer user_data)
{
    PurpAuthDialog *auth = user_data;

    (void)session;
    if (auth->completed)
        return;
    if (!gained_authorization && !auth->cancelled) {
        /* Wrong password (or PAM retry): start a fresh session for another go. */
        auth_show_error(auth, "Authentication failed — please try again.");
        auth_start_session(auth);
        return;
    }
    auth_finish(auth, gained_authorization);
}

static void auth_start_session(PurpAuthDialog *auth)
{
    g_clear_object(&auth->session);
    auth->session = polkit_agent_session_new(auth->identity, auth->cookie);
    g_signal_connect(auth->session, "request",
                     G_CALLBACK(on_session_request), auth);
    g_signal_connect(auth->session, "show-error",
                     G_CALLBACK(on_session_show_error), auth);
    g_signal_connect(auth->session, "show-info",
                     G_CALLBACK(on_session_show_info), auth);
    g_signal_connect(auth->session, "completed",
                     G_CALLBACK(on_session_completed), auth);
    polkit_agent_session_initiate(auth->session);
}

static void on_entry_activate(GtkWidget *widget, gpointer user_data)
{
    PurpAuthDialog *auth = user_data;

    (void)widget;
    gtk_widget_set_visible(auth->error_label, FALSE);
    if (auth->session)
        polkit_agent_session_response(
            auth->session,
            gtk_editable_get_text(GTK_EDITABLE(auth->entry)));
    gtk_editable_set_text(GTK_EDITABLE(auth->entry), "");
}

static void on_cancel_clicked(GtkWidget *widget, gpointer user_data)
{
    (void)widget;
    auth_cancel(user_data);
}

static gboolean on_close_request(GtkWindow *window, gpointer user_data)
{
    (void)window;
    auth_cancel(user_data);
    return FALSE;
}

static PurpAuthDialog *auth_dialog_new(const char *action_id,
                                       const char *message,
                                       PolkitIdentity *identity,
                                       const char *cookie,
                                       GDBusMethodInvocation *invocation)
{
    static gboolean css_installed = FALSE;
    PurpAuthDialog *auth = g_new0(PurpAuthDialog, 1);
    GtkWidget *box, *title_label, *msg_label, *btn_box, *cancel_btn, *auth_btn;
    const char *msg_text = (message && *message) ? message : action_id;
    g_autofree gchar *fallback = NULL;

    if (!css_installed) {
        GtkCssProvider *provider = gtk_css_provider_new();
        gtk_css_provider_load_from_string(provider, DIALOG_CSS);
        gtk_style_context_add_provider_for_display(
            gdk_display_get_default(), GTK_STYLE_PROVIDER(provider),
            GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
        g_object_unref(provider);
        css_installed = TRUE;
    }

    if (!msg_text || !*msg_text) {
        fallback = g_strdup_printf("Action: %s", action_id);
        msg_text = fallback;
    }

    auth->window = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(auth->window),
                         "Authentication Required — purpwm+");
    gtk_window_set_default_size(GTK_WINDOW(auth->window), 440, 260);
    gtk_window_set_modal(GTK_WINDOW(auth->window), TRUE);
    if (GTK_IS_APPLICATION(app))
        gtk_window_set_application(GTK_WINDOW(auth->window),
                                   GTK_APPLICATION(app));

    box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 14);
    gtk_widget_set_margin_top(box, 20);
    gtk_widget_set_margin_bottom(box, 20);
    gtk_widget_set_margin_start(box, 24);
    gtk_widget_set_margin_end(box, 24);
    gtk_window_set_child(GTK_WINDOW(auth->window), box);

    title_label = gtk_label_new("Authentication Required");
    gtk_widget_add_css_class(title_label, "title");
    gtk_label_set_xalign(GTK_LABEL(title_label), 0.0f);
    gtk_box_append(GTK_BOX(box), title_label);

    msg_label = gtk_label_new(msg_text);
    gtk_widget_add_css_class(msg_label, "msg");
    gtk_label_set_wrap(GTK_LABEL(msg_label), TRUE);
    gtk_label_set_xalign(GTK_LABEL(msg_label), 0.0f);
    gtk_box_append(GTK_BOX(box), msg_label);

    /* PAM prompt/info text ("Password:", smartcard PIN, etc.) */
    auth->prompt_label = gtk_label_new("");
    gtk_widget_add_css_class(auth->prompt_label, "msg");
    gtk_label_set_wrap(GTK_LABEL(auth->prompt_label), TRUE);
    gtk_label_set_xalign(GTK_LABEL(auth->prompt_label), 0.0f);
    gtk_widget_set_visible(auth->prompt_label, FALSE);
    gtk_box_append(GTK_BOX(box), auth->prompt_label);

    auth->entry = gtk_password_entry_new();
    gtk_password_entry_set_show_peek_icon(GTK_PASSWORD_ENTRY(auth->entry), TRUE);
    g_signal_connect(auth->entry, "activate",
                     G_CALLBACK(on_entry_activate), auth);
    gtk_box_append(GTK_BOX(box), auth->entry);

    auth->error_label = gtk_label_new("");
    gtk_widget_add_css_class(auth->error_label, "error");
    gtk_label_set_xalign(GTK_LABEL(auth->error_label), 0.0f);
    gtk_widget_set_visible(auth->error_label, FALSE);
    gtk_box_append(GTK_BOX(box), auth->error_label);

    btn_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_widget_set_halign(btn_box, GTK_ALIGN_END);
    gtk_box_append(GTK_BOX(box), btn_box);

    cancel_btn = gtk_button_new_with_label("Cancel");
    g_signal_connect(cancel_btn, "clicked",
                     G_CALLBACK(on_cancel_clicked), auth);
    gtk_box_append(GTK_BOX(btn_box), cancel_btn);

    auth_btn = gtk_button_new_with_label("Authenticate");
    gtk_widget_add_css_class(auth_btn, "suggested-action");
    g_signal_connect(auth_btn, "clicked",
                     G_CALLBACK(on_entry_activate), auth);
    gtk_box_append(GTK_BOX(btn_box), auth_btn);

    g_signal_connect(auth->window, "close-request",
                     G_CALLBACK(on_close_request), auth);

    /* Keep our own reference: destroying the window must not free the dialog
     * state while a PolkitAgentSession or invocation still references it. */
    g_object_ref_sink(auth->window);

    auth->identity = g_object_ref(identity);
    auth->cookie = g_strdup(cookie);
    auth->invocation = g_object_ref(invocation);
    auth->completed = FALSE;
    auth->cancelled = FALSE;
    auth_start_session(auth);
    return auth;
}

static void auth_dialog_destroy(PurpAuthDialog *auth)
{
    auth_free(auth);
}

/* Only polkitd itself may drive this agent; anyone else could spoof prompts. */
static gboolean sender_is_polkitd(const char *sender)
{
    g_autoptr(GVariant) reply = NULL;
    const char *owner = NULL;
    gboolean trusted;

    if (!sender || !system_bus)
        return FALSE;
    reply = g_dbus_connection_call_sync(
        system_bus, "org.freedesktop.DBus", "/org/freedesktop/DBus",
        "org.freedesktop.DBus", "GetNameOwner",
        g_variant_new("(s)", POLKIT_BUS_NAME),
        G_VARIANT_TYPE("(s)"), G_DBUS_CALL_FLAGS_NONE, 1000, NULL, NULL);
    if (!reply)
        return FALSE;
    g_variant_get(reply, "(&s)", &owner);
    trusted = g_strcmp0(sender, owner) == 0;
    return trusted;
}

static gint64 variant_int64(GVariant *value)
{
    if (!value)
        return -1;
    if (g_variant_is_of_type(value, G_VARIANT_TYPE_UINT32))
        return (gint64)g_variant_get_uint32(value);
    if (g_variant_is_of_type(value, G_VARIANT_TYPE_INT32))
        return (gint64)g_variant_get_int32(value);
    if (g_variant_is_of_type(value, G_VARIANT_TYPE_UINT64))
        return (gint64)g_variant_get_uint64(value);
    if (g_variant_is_of_type(value, G_VARIANT_TYPE_INT64))
        return g_variant_get_int64(value);
    if (g_variant_is_of_type(value, G_VARIANT_TYPE_UINT16))
        return (gint64)g_variant_get_uint16(value);
    if (g_variant_is_of_type(value, G_VARIANT_TYPE_INT16))
        return (gint64)g_variant_get_int16(value);
    if (g_variant_is_of_type(value, G_VARIANT_TYPE_BYTE))
        return (gint64)g_variant_get_byte(value);
    return -1;
}

static PolkitIdentity *identity_from_variant(const char *kind, GVariant *details)
{
    g_autoptr(GVariant) value = NULL;
    gint64 id;

    if (!kind || !details)
        return NULL;
    if (g_strcmp0(kind, "unix-user") == 0) {
        value = g_variant_lookup_value(details, "uid", NULL);
        id = variant_int64(value);
        if (id >= 0)
            return polkit_unix_user_new((uid_t)id);
    } else if (g_strcmp0(kind, "unix-group") == 0) {
        value = g_variant_lookup_value(details, "gid", NULL);
        id = variant_int64(value);
        if (id >= 0)
            return polkit_unix_group_new((gid_t)id);
    }
    return NULL;
}

static void on_agent_method_call(GDBusConnection *connection,
                                 const gchar *sender,
                                 const gchar *object_path,
                                 const gchar *interface_name,
                                 const gchar *method_name,
                                 GVariant *parameters,
                                 GDBusMethodInvocation *invocation,
                                 gpointer user_data)
{
    (void)connection;
    (void)object_path;
    (void)interface_name;
    (void)user_data;

    if (!sender_is_polkitd(sender)) {
        g_dbus_method_invocation_return_dbus_error(
            invocation, "org.freedesktop.DBus.Error.AccessDenied",
            "Caller is not org.freedesktop.PolicyKit1");
        return;
    }

    if (g_strcmp0(method_name, "BeginAuthentication") == 0) {
        const char *action_id, *message, *icon_name, *cookie;
        GVariant *details, *identities_v;
        GVariantIter iter;
        GVariant *entry;
        GList *identities = NULL, *l;
        PolkitIdentity *chosen = NULL;
        PurpAuthDialog *auth, *stale;
        uid_t uid = getuid();

        g_variant_get(parameters, "(&s&s&s@a{ss}&s@a(sa{sv}))",
                      &action_id, &message, &icon_name, &details,
                      &cookie, &identities_v);
        (void)icon_name;

        g_variant_iter_init(&iter, identities_v);
        while ((entry = g_variant_iter_next_value(&iter)) != NULL) {
            const char *kind;
            g_autoptr(GVariant) idetails = NULL;
            PolkitIdentity *identity;

            g_variant_get(entry, "(&s@a{sv})", &kind, &idetails);
            identity = identity_from_variant(kind, idetails);
            if (identity)
                identities = g_list_prepend(identities, identity);
            g_variant_unref(entry);
        }
        identities = g_list_reverse(identities);

        if (!identities) {
            g_dbus_method_invocation_return_dbus_error(
                invocation, "org.freedesktop.PolicyKit1.Error.Failed",
                "No usable identity to authenticate as");
            g_variant_unref(details);
            g_variant_unref(identities_v);
            return;
        }

        /* Prefer authenticating as ourselves when we are an allowed identity. */
        chosen = identities->data;
        for (l = identities; l; l = l->next) {
            if (POLKIT_IS_UNIX_USER(l->data) &&
                (uid_t)polkit_unix_user_get_uid(POLKIT_UNIX_USER(l->data)) == uid) {
                chosen = l->data;
                break;
            }
        }

        stale = g_hash_table_lookup(dialogs, cookie);
        if (stale) {
            /* polkitd re-sent this cookie; tear down the orphaned dialog. */
            auth_cancel(stale);
        }

        auth = auth_dialog_new(action_id, message, chosen, cookie, invocation);
        g_hash_table_insert(dialogs, g_strdup(cookie), auth);
        gtk_window_present(GTK_WINDOW(auth->window));

        g_list_free_full(identities, g_object_unref);
        g_variant_unref(details);
        g_variant_unref(identities_v);
    } else if (g_strcmp0(method_name, "CancelAuthentication") == 0) {
        const char *cookie;
        PurpAuthDialog *auth;

        g_variant_get(parameters, "(&s)", &cookie);
        auth = g_hash_table_lookup(dialogs, cookie);
        if (!auth) {
            g_dbus_method_invocation_return_dbus_error(
                invocation, "org.freedesktop.PolicyKit1.Error.Failed",
                "No pending authentication for that cookie");
            return;
        }
        /* polkit cancels when the requesting program goes away. */
        auth_cancel(auth);
        g_dbus_method_invocation_return_value(invocation, NULL);
    } else {
        g_dbus_method_invocation_return_dbus_error(
            invocation, "org.freedesktop.DBus.Error.UnknownMethod",
            "Unknown authentication agent method");
    }
}

static const GDBusInterfaceVTable agent_vtable = {
    on_agent_method_call,
    NULL,
    NULL,
    { 0 }
};

static char *current_session_id(void)
{
    g_autoptr(PolkitUnixSession) session = NULL;
    const char *env;

    session = POLKIT_UNIX_SESSION(
        polkit_unix_session_new_for_process_sync(getpid(), NULL, NULL));
    if (session)
        return g_strdup(polkit_unix_session_get_session_id(session));
    env = g_getenv("XDG_SESSION_ID");
    return g_strdup(env ? env : "");
}

static void register_with_authority(void)
{
    g_autoptr(GError) error = NULL;
    g_autofree gchar *session_id = current_session_id();
    const char *locale;
    GVariant *subject;
    GVariantBuilder details;

    g_variant_builder_init(&details, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&details, "{sv}", "session-id",
                          g_variant_new_string(session_id));
    subject = g_variant_new("(sa{sv})", "unix-session", &details);

    locale = g_getenv("LANG");
    if (!locale || !*locale)
        locale = "en_US.UTF-8";

    g_dbus_connection_call_sync(
        system_bus, POLKIT_BUS_NAME,
        "/org/freedesktop/PolicyKit1/Authority",
        "org.freedesktop.PolicyKit1.Authority",
        "RegisterAuthenticationAgent",
        g_variant_new("(@(sa{sv})ss)", subject, locale, AGENT_PATH),
        NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, &error);
    if (error)
        g_print("Polkit agent registration notice: %s\n", error->message);
    else
        g_print("purpwm+ Polkit Authentication Agent successfully registered.\n");
    fflush(stdout);
}

static void on_activate(GApplication *application, gpointer user_data)
{
    g_autoptr(GError) error = NULL;
    g_autoptr(GDBusNodeInfo) node = NULL;
    guint registration;

    (void)user_data;

    /* No window exists until a request arrives; without a hold the
     * application would exit at once. */
    g_application_hold(application);

    dialogs = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                    (GDestroyNotify)auth_dialog_destroy);

    /* GIO only caches the bus weakly: keep a reference or the connection (and
     * with it our registration) closes as soon as this callback returns. */
    system_bus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, &error);
    if (!system_bus) {
        g_warning("purpwm+-polkit-agent: cannot connect to the system bus: %s",
                  error->message);
        return;
    }

    node = g_dbus_node_info_new_for_xml(AGENT_INTROSPECTION, &error);
    if (!node) {
        g_warning("purpwm+-polkit-agent: bad introspection XML: %s",
                  error->message);
        return;
    }

    registration = g_dbus_connection_register_object(
        system_bus, AGENT_PATH, node->interfaces[0], &agent_vtable,
        NULL, NULL, &error);
    if (registration == 0) {
        g_warning("purpwm+-polkit-agent: object registration failed: %s",
                  error->message);
        return;
    }

    register_with_authority();
}

static gboolean parent_alive(gpointer user_data)
{
    (void)user_data;
    if (getppid() != initial_ppid) {
        g_application_quit(app);
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

int main(int argc, char **argv)
{
    int status;

    /* Started by the session launcher: quit when reparented so no GTK
     * process outlives the login session. */
    initial_ppid = getppid();

    app = G_APPLICATION(gtk_application_new(
        "org.purpwm.PolkitAuthenticationAgent",
        G_APPLICATION_DEFAULT_FLAGS));
    g_signal_connect(app, "activate", G_CALLBACK(on_activate), NULL);
    g_timeout_add_seconds(2, parent_alive, NULL);

    status = g_application_run(app, argc, argv);
    g_object_unref(app);
    return status;
}
