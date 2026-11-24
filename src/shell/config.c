/*
 * The user's desktop settings, as the Settings program writes them:
 *
 *     ~/.config/wormos/shell     [panel] position = top|bottom,
 *                                clock-seconds, clock-12h, clock-weekday,
 *                                clock-date (true/false);
 *                                [notifications] do-not-disturb
 *     ~/.config/wormos/display   the display scale (outputs.c)
 *
 * The folder is watched, so changes apply at once.
 */

#include "shell.h"

static GKeyFile *keys;
static GFileMonitor *monitor;
static GPtrArray *watchers;     /* of config_changed_fn */

static char *config_path(const char *name)
{
	return g_build_filename(g_get_user_config_dir(), "wormos", name, NULL);
}

static void load(void)
{
	char *path = config_path("shell");

	if (keys)
		g_key_file_unref(keys);
	keys = g_key_file_new();
	g_key_file_load_from_file(keys, path, G_KEY_FILE_KEEP_COMMENTS, NULL);
	g_free(path);
}

gboolean config_bool(const char *group, const char *key, gboolean fallback)
{
	GError *error = NULL;
	gboolean v;

	if (!keys)
		load();
	v = g_key_file_get_boolean(keys, group, key, &error);
	if (error) {
		g_error_free(error);
		return fallback;
	}
	return v;
}

char *config_string(const char *group, const char *key)
{
	if (!keys)
		load();
	return g_key_file_get_string(keys, group, key, NULL);
}

void config_set_bool(const char *group, const char *key, gboolean value)
{
	char *path = config_path("shell"), *dir = g_path_get_dirname(path);

	if (!keys)
		load();
	g_key_file_set_boolean(keys, group, key, value);
	g_mkdir_with_parents(dir, 0700);
	if (!g_key_file_save_to_file(keys, path, NULL))
		g_warning("cannot save %s", path);
	g_free(dir);
	g_free(path);
}

static void changed(GFileMonitor *m, GFile *file, GFile *other, GFileMonitorEvent event,
		    gpointer data)
{
	char *name = g_file_get_basename(file);

	(void)m;
	(void)other;
	(void)data;
	if (event == G_FILE_MONITOR_EVENT_CHANGES_DONE_HINT || event == G_FILE_MONITOR_EVENT_CREATED ||
	    event == G_FILE_MONITOR_EVENT_DELETED || event == G_FILE_MONITOR_EVENT_RENAMED ||
	    event == G_FILE_MONITOR_EVENT_MOVED_IN) {
		if (!strcmp(name, "display")) {
			outputs_reload();
		} else if (!strcmp(name, "shell")) {
			load();
			for (guint i = 0; i < watchers->len; i++)
				((config_changed_fn)g_ptr_array_index(watchers, i))();
		}
	}
	g_free(name);
}

void config_watch(config_changed_fn fn)
{
	if (!watchers)
		watchers = g_ptr_array_new();
	g_ptr_array_add(watchers, fn);
	if (!monitor) {
		char *dir = g_build_filename(g_get_user_config_dir(), "wormos", NULL);
		GFile *f;

		g_mkdir_with_parents(dir, 0700);
		f = g_file_new_for_path(dir);
		monitor = g_file_monitor_directory(f, G_FILE_MONITOR_WATCH_MOVES, NULL, NULL);
		if (monitor)
			g_signal_connect(monitor, "changed", G_CALLBACK(changed), NULL);
		g_object_unref(f);
		g_free(dir);
	}
}
