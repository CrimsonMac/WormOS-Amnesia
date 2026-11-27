/*
 * Wi-Fi through iwd's D-Bus interface (net.connman.iwd on the system bus;
 * administrators, the group wheel, may use it).
 *
 * The first Station is the Wi-Fi adapter shown. wifi_scan() asks it to
 * scan and then lists its networks strongest first; wifi_connect() joins
 * one. Networks that need a passphrase get it through an agent this
 * process registers: the panel stores the passphrase typed for that
 * network just before connecting, and iwd asks the agent for it. iwd keeps
 * known networks (/var/lib/iwd) and reconnects to them by itself.
 */

#include <string.h>

#include "shell.h"

#define IWD          "net.connman.iwd"
#define AGENT_PATH   "/org/wormos/shell/wifi_agent"

static GDBusConnection *bus;
static char *station;           /* object path of the Station, or NULL */
static char *pending_network;   /* the network the passphrase below is for */
static char *pending_passphrase;
static guint agent_id;

static const char agent_xml[] =
	"<node><interface name='net.connman.iwd.Agent'>"
	"<method name='Release'/>"
	"<method name='RequestPassphrase'><arg type='o' direction='in'/>"
	"<arg type='s' direction='out'/></method>"
	"<method name='RequestPrivateKeyPassphrase'><arg type='o' direction='in'/>"
	"<arg type='s' direction='out'/></method>"
	"<method name='RequestUserNameAndPassword'><arg type='o' direction='in'/>"
	"<arg type='s' direction='out'/><arg type='s' direction='out'/></method>"
	"<method name='RequestUserPassword'><arg type='o' direction='in'/>"
	"<arg type='s' direction='in'/><arg type='s' direction='out'/></method>"
	"<method name='Cancel'><arg type='s' direction='in'/></method>"
	"</interface></node>";

static void forget_passphrase(void)
{
	if (pending_passphrase) {
		explicit_bzero(pending_passphrase, strlen(pending_passphrase));
		g_clear_pointer(&pending_passphrase, g_free);
	}
	g_clear_pointer(&pending_network, g_free);
}

static void agent_call(GDBusConnection *c, const char *sender, const char *path,
		       const char *iface, const char *method, GVariant *params,
		       GDBusMethodInvocation *inv, gpointer data)
{
	const char *network = NULL;

	(void)c; (void)sender; (void)path; (void)iface; (void)data;
	if (!strcmp(method, "RequestPassphrase")) {
		g_variant_get(params, "(&o)", &network);
		if (pending_passphrase && pending_network && !strcmp(network, pending_network)) {
			g_dbus_method_invocation_return_value(inv, g_variant_new("(s)", pending_passphrase));
			forget_passphrase();
			return;
		}
		g_dbus_method_invocation_return_dbus_error(inv, "net.connman.iwd.Agent.Error.Canceled",
							   "No passphrase was given");
		return;
	}
	if (!strcmp(method, "Release") || !strcmp(method, "Cancel")) {
		forget_passphrase();
		g_dbus_method_invocation_return_value(inv, NULL);
		return;
	}
	/* Enterprise networks (802.1X) need settings iwctl or a file provides. */
	g_dbus_method_invocation_return_dbus_error(inv, "net.connman.iwd.Agent.Error.Canceled",
						   "Use iwctl for enterprise networks");
}

static const GDBusInterfaceVTable agent_vtable = { agent_call, NULL, NULL, { 0 } };

/* The first object with a Station interface, from iwd's object manager. */
static void find_station(void)
{
	GVariant *reply, *objects, *ifaces;
	GVariantIter iter;
	const char *path;

	g_clear_pointer(&station, g_free);
	reply = g_dbus_connection_call_sync(bus, IWD, "/", "org.freedesktop.DBus.ObjectManager",
					    "GetManagedObjects", NULL, NULL, G_DBUS_CALL_FLAGS_NONE,
					    2000, NULL, NULL);
	if (!reply)
		return;
	objects = g_variant_get_child_value(reply, 0);
	g_variant_iter_init(&iter, objects);
	while (!station && g_variant_iter_next(&iter, "{&o@a{sa{sv}}}", &path, &ifaces)) {
		GVariant *s = g_variant_lookup_value(ifaces, "net.connman.iwd.Station", NULL);

		if (s) {
			station = g_strdup(path);
			g_variant_unref(s);
		}
		g_variant_unref(ifaces);
	}
	g_variant_unref(objects);
	g_variant_unref(reply);
}

gboolean wifi_init(void)
{
	GDBusNodeInfo *info;

	if (!bus && !(bus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, NULL)))
		return FALSE;
	find_station();
	if (!station)
		return FALSE;
	if (!agent_id) {
		info = g_dbus_node_info_new_for_xml(agent_xml, NULL);
		agent_id = g_dbus_connection_register_object(bus, AGENT_PATH, info->interfaces[0],
							     &agent_vtable, NULL, NULL, NULL);
		g_dbus_node_info_unref(info);
		g_dbus_connection_call(bus, IWD, "/net/connman/iwd", "net.connman.iwd.AgentManager",
				       "RegisterAgent", g_variant_new("(o)", AGENT_PATH), NULL,
				       G_DBUS_CALL_FLAGS_NONE, 2000, NULL, NULL, NULL);
	}
	return TRUE;
}

static GVariant *get_property(const char *path, const char *iface, const char *name)
{
	GVariant *reply = g_dbus_connection_call_sync(bus, IWD, path, "org.freedesktop.DBus.Properties",
						      "Get", g_variant_new("(ss)", iface, name),
						      G_VARIANT_TYPE("(v)"), G_DBUS_CALL_FLAGS_NONE,
						      2000, NULL, NULL);
	GVariant *value = NULL;

	if (reply) {
		g_variant_get(reply, "(v)", &value);
		g_variant_unref(reply);
	}
	return value;
}

/* The adapter (the Device under the Station) switched on? */
gboolean wifi_powered(void)
{
	GVariant *v = station ? get_property(station, "net.connman.iwd.Device", "Powered") : NULL;
	gboolean on = v && g_variant_get_boolean(v);

	if (v)
		g_variant_unref(v);
	return on;
}

void wifi_set_powered(gboolean on)
{
	if (!station)
		return;
	g_dbus_connection_call(bus, IWD, station, "org.freedesktop.DBus.Properties", "Set",
			       g_variant_new("(ssv)", "net.connman.iwd.Device", "Powered",
					     g_variant_new_boolean(on)),
			       NULL, G_DBUS_CALL_FLAGS_NONE, 5000, NULL, NULL, NULL);
}

void wifi_network_free(gpointer p)
{
	struct wifi_network *n = p;

	g_free(n->path);
	g_free(n->name);
	g_free(n->type);
	g_free(n);
}

/* The networks in range, strongest first (a GPtrArray of wifi_network). */
static GPtrArray *list_networks(void)
{
	GPtrArray *list = g_ptr_array_new_with_free_func(wifi_network_free);
	GVariant *reply;
	GVariantIter *iter;
	const char *path;
	gint16 signal;

	if (!station)
		return list;
	reply = g_dbus_connection_call_sync(bus, IWD, station, "net.connman.iwd.Station",
					    "GetOrderedNetworks", NULL, G_VARIANT_TYPE("(a(on))"),
					    G_DBUS_CALL_FLAGS_NONE, 5000, NULL, NULL);
	if (!reply)
		return list;
	g_variant_get(reply, "(a(on))", &iter);
	while (g_variant_iter_next(iter, "(&on)", &path, &signal)) {
		struct wifi_network *n = g_new0(struct wifi_network, 1);
		GVariant *v;

		n->path = g_strdup(path);
		/* iwd gives 100 * dBm: -40 dBm is excellent, -85 barely usable. */
		n->bars = signal >= -5500 ? 4 : signal >= -6700 ? 3 : signal >= -7500 ? 2 : 1;
		if ((v = get_property(path, "net.connman.iwd.Network", "Name"))) {
			n->name = g_variant_dup_string(v, NULL);
			g_variant_unref(v);
		}
		if ((v = get_property(path, "net.connman.iwd.Network", "Type"))) {
			n->type = g_variant_dup_string(v, NULL);
			g_variant_unref(v);
		}
		if ((v = get_property(path, "net.connman.iwd.Network", "Connected"))) {
			n->connected = g_variant_get_boolean(v);
			g_variant_unref(v);
		}
		if ((v = get_property(path, "net.connman.iwd.Network", "KnownNetwork"))) {
			n->known = TRUE;
			g_variant_unref(v);
		}
		if (n->name)
			g_ptr_array_add(list, n);
		else
			wifi_network_free(n);
	}
	g_variant_iter_free(iter);
	g_variant_unref(reply);
	return list;
}

struct scan {
	wifi_list_fn done;
	gpointer data;
	int polls;
};

/* iwd scans in the background; Scanning goes false when it is done. */
static gboolean scan_poll(gpointer p)
{
	struct scan *s = p;
	GVariant *v = get_property(station, "net.connman.iwd.Station", "Scanning");
	gboolean scanning = v && g_variant_get_boolean(v);

	if (v)
		g_variant_unref(v);
	if (scanning && ++s->polls < 40)
		return G_SOURCE_CONTINUE;
	s->done(list_networks(), s->data);
	g_free(s);
	return G_SOURCE_REMOVE;
}

void wifi_scan(wifi_list_fn done, gpointer data)
{
	struct scan *s = g_new0(struct scan, 1);

	s->done = done;
	s->data = data;
	if (!station) {
		done(g_ptr_array_new_with_free_func(wifi_network_free), data);
		g_free(s);
		return;
	}
	/* Show what is known now, then again after a fresh scan. */
	done(list_networks(), data);
	g_dbus_connection_call(bus, IWD, station, "net.connman.iwd.Station", "Scan", NULL, NULL,
			       G_DBUS_CALL_FLAGS_NONE, 5000, NULL, NULL, NULL);
	g_timeout_add(250, scan_poll, s);
}

struct connect {
	wifi_done_fn done;
	gpointer data;
};

static void connected(GObject *source, GAsyncResult *res, gpointer p)
{
	struct connect *c = p;
	GError *error = NULL;
	GVariant *reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), res, &error);
	char *message = NULL;

	if (reply) {
		g_variant_unref(reply);
	} else {
		/* "net.connman.iwd.Failed: Operation failed" -> a sentence. */
		char *remote = g_dbus_error_get_remote_error(error);

		message = g_strdup(remote && strstr(remote, "Canceled") ? "No passphrase was given." :
				   remote && (strstr(remote, "Failed") || strstr(remote, "Aborted")) ?
				   "Could not connect: wrong passphrase, or the network is out of reach." :
				   error->message);
		g_free(remote);
		g_error_free(error);
	}
	forget_passphrase();
	c->done(message, c->data);
	g_free(message);
	g_free(c);
}

void wifi_connect(const char *network, const char *passphrase, wifi_done_fn done, gpointer data)
{
	struct connect *c = g_new0(struct connect, 1);

	c->done = done;
	c->data = data;
	forget_passphrase();
	if (passphrase && *passphrase) {
		pending_network = g_strdup(network);
		pending_passphrase = g_strdup(passphrase);
	}
	g_dbus_connection_call(bus, IWD, network, "net.connman.iwd.Network", "Connect", NULL, NULL,
			       G_DBUS_CALL_FLAGS_NONE, 60000, NULL, connected, c);
}

void wifi_disconnect(void)
{
	if (station)
		g_dbus_connection_call(bus, IWD, station, "net.connman.iwd.Station", "Disconnect",
				       NULL, NULL, G_DBUS_CALL_FLAGS_NONE, 5000, NULL, NULL, NULL);
}

/* The name of the network connected to, or NULL (free it). */
char *wifi_connected_name(void)
{
	GVariant *v;

	if (!station)
		wifi_init();
	v = station ? get_property(station, "net.connman.iwd.Station", "ConnectedNetwork") : NULL;
	char *name = NULL;

	if (v) {
		GVariant *n = get_property(g_variant_get_string(v, NULL), "net.connman.iwd.Network", "Name");

		if (n) {
			name = g_variant_dup_string(n, NULL);
			g_variant_unref(n);
		}
		g_variant_unref(v);
	}
	return name;
}
