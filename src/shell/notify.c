/*
 * Desktop notifications (org.freedesktop.Notifications on the session
 * bus): programs send them with notify-send or libnotify, and they appear
 * as cards in the top right corner of the screen. A card closes after its
 * timeout (six seconds by default, never for critical ones), on its close
 * button, or on a click, which also invokes its default action.
 */

#include <gtk4-layer-shell.h>

#include "shell.h"

#define BUS_NAME     "org.freedesktop.Notifications"
#define OBJECT_PATH  "/org/freedesktop/Notifications"
#define DEFAULT_TIMEOUT_MS  6000

/* NotificationClosed reasons. */
enum { CLOSED_EXPIRED = 1, CLOSED_DISMISSED = 2, CLOSED_CALL = 3 };

static const char introspection[] =
	"<node>"
	"  <interface name='org.freedesktop.Notifications'>"
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
	"    <method name='GetCapabilities'>"
	"      <arg type='as' name='capabilities' direction='out'/>"
	"    </method>"
	"    <method name='GetServerInformation'>"
	"      <arg type='s' name='name' direction='out'/>"
	"      <arg type='s' name='vendor' direction='out'/>"
	"      <arg type='s' name='version' direction='out'/>"
	"      <arg type='s' name='spec_version' direction='out'/>"
	"    </method>"
	"    <signal name='NotificationClosed'>"
	"      <arg type='u' name='id'/>"
	"      <arg type='u' name='reason'/>"
	"    </signal>"
	"    <signal name='ActionInvoked'>"
	"      <arg type='u' name='id'/>"
	"      <arg type='s' name='action_key'/>"
	"    </signal>"
	"  </interface>"
	"</node>";

struct note {
	guint id;
	GtkWidget *card;
	guint timer;
	gboolean has_default;
};

static GtkApplication *app;
static GDBusConnection *bus;
static GtkWidget *window, *stack;
static GHashTable *notes;               /* id -> struct note */
static guint next_id = 1;

static void emit(const char *signal, GVariant *args)
{
	if (bus)
		g_dbus_connection_emit_signal(bus, NULL, OBJECT_PATH, BUS_NAME, signal, args, NULL);
}

static void close_note(guint id, guint reason)
{
	struct note *n = g_hash_table_lookup(notes, GUINT_TO_POINTER(id));

	if (!n)
		return;
	if (n->timer)
		g_source_remove(n->timer);
	gtk_box_remove(GTK_BOX(stack), n->card);
	g_hash_table_remove(notes, GUINT_TO_POINTER(id));
	emit("NotificationClosed", g_variant_new("(uu)", id, reason));
	if (g_hash_table_size(notes) == 0)
		gtk_widget_set_visible(window, FALSE);
}

static gboolean expired(gpointer data)
{
	struct note *n = g_hash_table_lookup(notes, data);

	if (n)
		n->timer = 0;
	close_note(GPOINTER_TO_UINT(data), CLOSED_EXPIRED);
	return G_SOURCE_REMOVE;
}

static void close_clicked(GtkButton *button, gpointer data)
{
	(void)button;
	close_note(GPOINTER_TO_UINT(data), CLOSED_DISMISSED);
}

static void card_clicked(GtkGestureClick *gesture, int n_press, double x, double y,
			 gpointer data)
{
	guint id = GPOINTER_TO_UINT(data);
	struct note *n = g_hash_table_lookup(notes, data);

	(void)gesture;
	(void)n_press;
	(void)x;
	(void)y;
	if (n && n->has_default)
		emit("ActionInvoked", g_variant_new("(us)", id, "default"));
	close_note(id, CLOSED_DISMISSED);
}

static void action_clicked(GtkButton *button, gpointer data)
{
	guint id = GPOINTER_TO_UINT(data);

	emit("ActionInvoked", g_variant_new("(us)", id,
					    (const char *)g_object_get_data(G_OBJECT(button), "key")));
	close_note(id, CLOSED_DISMISSED);
}

/* Body text may use simple markup; show it as plain text if it is invalid. */
static GtkWidget *body_label(const char *body)
{
	GtkWidget *label = gtk_label_new(NULL);

	if (pango_parse_markup(body, -1, 0, NULL, NULL, NULL, NULL))
		gtk_label_set_markup(GTK_LABEL(label), body);
	else
		gtk_label_set_text(GTK_LABEL(label), body);
	gtk_label_set_wrap(GTK_LABEL(label), TRUE);
	gtk_label_set_wrap_mode(GTK_LABEL(label), PANGO_WRAP_WORD_CHAR);
	gtk_label_set_max_width_chars(GTK_LABEL(label), 40);
	gtk_label_set_lines(GTK_LABEL(label), 6);
	gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
	gtk_label_set_xalign(GTK_LABEL(label), 0);
	gtk_widget_add_css_class(label, "body");
	return label;
}

static GtkWidget *build_card(guint id, const char *app_name, const char *icon_name,
			     const char *summary, const char *body, const char **actions,
			     gboolean critical, gboolean *has_default)
{
	GtkWidget *card = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12), *text, *close, *title;
	GtkWidget *row = NULL;
	GtkGesture *click;

	gtk_widget_add_css_class(card, "notification");
	if (critical)
		gtk_widget_add_css_class(card, "critical");
	if (icon_name && *icon_name) {
		GtkWidget *image;

		if (g_path_is_absolute(icon_name))
			image = gtk_image_new_from_file(icon_name);
		else if (g_str_has_prefix(icon_name, "file://")) {
			char *path = g_filename_from_uri(icon_name, NULL, NULL);

			image = gtk_image_new_from_file(path);
			g_free(path);
		} else {
			image = gtk_image_new_from_icon_name(icon_name);
		}
		gtk_image_set_pixel_size(GTK_IMAGE(image), 40);
		gtk_widget_set_valign(image, GTK_ALIGN_START);
		gtk_box_append(GTK_BOX(card), image);
	}

	text = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
	gtk_widget_set_hexpand(text, TRUE);
	if (app_name && *app_name) {
		GtkWidget *source = gtk_label_new(app_name);

		gtk_label_set_xalign(GTK_LABEL(source), 0);
		gtk_widget_add_css_class(source, "source");
		gtk_box_append(GTK_BOX(text), source);
	}
	title = gtk_label_new(summary);
	gtk_label_set_xalign(GTK_LABEL(title), 0);
	gtk_label_set_wrap(GTK_LABEL(title), TRUE);
	gtk_label_set_max_width_chars(GTK_LABEL(title), 40);
	gtk_widget_add_css_class(title, "summary");
	gtk_box_append(GTK_BOX(text), title);
	if (body && *body)
		gtk_box_append(GTK_BOX(text), body_label(body));

	/* Actions come in pairs: key, label. "default" is a click on the card. */
	*has_default = FALSE;
	for (int i = 0; actions[i] && actions[i + 1]; i += 2) {
		GtkWidget *button;

		if (!strcmp(actions[i], "default")) {
			*has_default = TRUE;
			continue;
		}
		if (!row) {
			row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
			gtk_widget_add_css_class(row, "actions");
			gtk_box_append(GTK_BOX(text), row);
		}
		button = gtk_button_new_with_label(actions[i + 1]);
		g_object_set_data_full(G_OBJECT(button), "key", g_strdup(actions[i]), g_free);
		g_signal_connect(button, "clicked", G_CALLBACK(action_clicked), GUINT_TO_POINTER(id));
		gtk_box_append(GTK_BOX(row), button);
	}
	gtk_box_append(GTK_BOX(card), text);

	close = gtk_button_new_from_icon_name("window-close-symbolic");
	gtk_widget_set_valign(close, GTK_ALIGN_START);
	gtk_widget_add_css_class(close, "close");
	g_signal_connect(close, "clicked", G_CALLBACK(close_clicked), GUINT_TO_POINTER(id));
	gtk_box_append(GTK_BOX(card), close);

	click = gtk_gesture_click_new();
	g_signal_connect(click, "released", G_CALLBACK(card_clicked), GUINT_TO_POINTER(id));
	gtk_widget_add_controller(text, GTK_EVENT_CONTROLLER(click));
	return card;
}

static guint notify(const char *app_name, guint replaces, const char *icon,
		    const char *summary, const char *body, const char **actions,
		    GVariant *hints, int timeout)
{
	guchar urgency = 1;
	struct note *n;
	guint id;

	g_variant_lookup(hints, "urgency", "y", &urgency);
	if ((!icon || !*icon) && !g_variant_lookup(hints, "image-path", "&s", &icon))
		icon = NULL;
	id = replaces && g_hash_table_contains(notes, GUINT_TO_POINTER(replaces))
		? replaces : next_id++;
	/* Do not disturb: only critical notifications show. */
	if (notify_quiet() && urgency != 2 && id != replaces)
		return id;
	if (id == replaces) {
		struct note *old = g_hash_table_lookup(notes, GUINT_TO_POINTER(id));

		if (old->timer)
			g_source_remove(old->timer);
		gtk_box_remove(GTK_BOX(stack), old->card);
		g_hash_table_remove(notes, GUINT_TO_POINTER(id));
	}

	n = g_new0(struct note, 1);
	n->id = id;
	n->card = build_card(id, app_name, icon, summary, body, actions, urgency == 2,
			     &n->has_default);
	/* Newest on top. */
	gtk_box_prepend(GTK_BOX(stack), n->card);
	if (timeout < 0)
		timeout = DEFAULT_TIMEOUT_MS;
	if (urgency == 2)
		timeout = 0;
	if (timeout > 0)
		n->timer = g_timeout_add(timeout, expired, GUINT_TO_POINTER(id));
	g_hash_table_insert(notes, GUINT_TO_POINTER(id), n);
	gtk_window_present(GTK_WINDOW(window));
	return id;
}

gboolean notify_quiet(void)
{
	return config_bool("notifications", "do-not-disturb", FALSE);
}

static void method_call(GDBusConnection *conn, const char *sender, const char *path,
			const char *iface, const char *method, GVariant *params,
			GDBusMethodInvocation *call, gpointer data)
{
	(void)conn;
	(void)sender;
	(void)path;
	(void)iface;
	(void)data;
	if (!strcmp(method, "Notify")) {
		const char *app_name, *icon, *summary, *body, **actions;
		GVariant *hints;
		guint replaces;
		int timeout;

		g_variant_get(params, "(&su&s&s&s^a&s@a{sv}i)", &app_name, &replaces, &icon,
			      &summary, &body, &actions, &hints, &timeout);
		g_dbus_method_invocation_return_value(call, g_variant_new("(u)",
			notify(app_name, replaces, icon, summary, body, actions, hints, timeout)));
		g_free(actions);
		g_variant_unref(hints);
	} else if (!strcmp(method, "CloseNotification")) {
		guint id;

		g_variant_get(params, "(u)", &id);
		close_note(id, CLOSED_CALL);
		g_dbus_method_invocation_return_value(call, NULL);
	} else if (!strcmp(method, "GetCapabilities")) {
		const char *caps[] = { "body", "body-markup", "actions", "icon-static", NULL };

		g_dbus_method_invocation_return_value(call, g_variant_new("(^as)", caps));
	} else if (!strcmp(method, "GetServerInformation")) {
		g_dbus_method_invocation_return_value(call, g_variant_new("(ssss)",
			"worm-shell", "WormOS", "1.0", "1.2"));
	} else {
		g_dbus_method_invocation_return_error(call, G_DBUS_ERROR,
			G_DBUS_ERROR_UNKNOWN_METHOD, "no method %s", method);
	}
}

static const GDBusInterfaceVTable vtable = { .method_call = method_call };

static void bus_acquired(GDBusConnection *conn, const char *name, gpointer data)
{
	GDBusNodeInfo *info = g_dbus_node_info_new_for_xml(introspection, NULL);

	(void)name;
	(void)data;
	bus = conn;
	g_dbus_connection_register_object(conn, OBJECT_PATH, info->interfaces[0], &vtable,
					  NULL, NULL, NULL);
	g_dbus_node_info_unref(info);
}

static void name_lost(GDBusConnection *conn, const char *name, gpointer data)
{
	(void)conn;
	(void)data;
	g_warning("another program shows notifications (%s is taken)", name);
}

void notify_init(GtkApplication *application)
{
	app = application;
	notes = g_hash_table_new_full(NULL, NULL, NULL, g_free);

	window = gtk_application_window_new(app);
	gtk_layer_init_for_window(GTK_WINDOW(window));
	gtk_layer_set_namespace(GTK_WINDOW(window), "wormos-notifications");
	gtk_layer_set_layer(GTK_WINDOW(window), GTK_LAYER_SHELL_LAYER_OVERLAY);
	gtk_layer_set_anchor(GTK_WINDOW(window), GTK_LAYER_SHELL_EDGE_TOP, TRUE);
	gtk_layer_set_anchor(GTK_WINDOW(window), GTK_LAYER_SHELL_EDGE_RIGHT, TRUE);
	gtk_layer_set_margin(GTK_WINDOW(window), GTK_LAYER_SHELL_EDGE_TOP, 8);
	gtk_layer_set_margin(GTK_WINDOW(window), GTK_LAYER_SHELL_EDGE_RIGHT, 8);
	gtk_widget_add_css_class(window, "notifications");
	stack = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
	gtk_window_set_child(GTK_WINDOW(window), stack);

	g_bus_own_name(G_BUS_TYPE_SESSION, BUS_NAME, G_BUS_NAME_OWNER_FLAGS_NONE,
		       bus_acquired, NULL, name_lost, NULL, NULL);
}
