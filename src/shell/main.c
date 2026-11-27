/*
 * worm-shell - the WormOS desktop shell: the panel, the application
 * launcher and desktop notifications, in one program started with the
 * desktop. It runs once per session; "worm-shell --launcher" (bound to the
 * Super key) asks the running shell to open or close the launcher, and
 * "worm-shell --volume up|down|mute" (the volume keys) changes the volume,
 * "worm-shell --brightness up|down" (the brightness keys) the screen's,
 * "worm-shell --do-not-disturb on|off|toggle" quiets notifications.
 */

#include <stdio.h>
#include <string.h>

#include <glib-unix.h>
#include <glib/gstdio.h>
#include <unistd.h>
#include <signal.h>

#include "shell.h"

static void load_css(void)
{
	GtkCssProvider *css = gtk_css_provider_new();

	gtk_css_provider_load_from_path(css, SHELL_CSS);
	gtk_style_context_add_provider_for_display(gdk_display_get_default(),
						   GTK_STYLE_PROVIDER(css),
						   GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
	g_object_unref(css);
	/* An edition's own look (WormOS Amnesia's greys), on top. */
	if (g_file_test(SHELL_VARIANT_CSS, G_FILE_TEST_EXISTS)) {
		css = gtk_css_provider_new();
		gtk_css_provider_load_from_path(css, SHELL_VARIANT_CSS);
		gtk_style_context_add_provider_for_display(gdk_display_get_default(),
							   GTK_STYLE_PROVIDER(css),
							   GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 1);
		g_object_unref(css);
	}
}

/* Is a lock screen of ours running? */
static gboolean lock_running(void)
{
	GDir *dir = g_dir_open("/proc", 0, NULL);
	const char *name;
	gboolean found = FALSE;

	while (dir && !found && (name = g_dir_read_name(dir))) {
		char path[64], *comm = NULL;
		GStatBuf st;

		if (!g_ascii_isdigit(name[0]))
			continue;
		snprintf(path, sizeof(path), "/proc/%s", name);
		if (g_stat(path, &st) < 0 || st.st_uid != getuid())
			continue;
		snprintf(path, sizeof(path), "/proc/%s/comm", name);
		if (g_file_get_contents(path, &comm, NULL, NULL))
			found = !strcmp(g_strstrip(comm), "wormos-lock");
		g_free(comm);
	}
	if (dir)
		g_dir_close(dir);
	return found;
}

/* The session is marked locked but no lock screen runs (it crashed): labwc
 * still shows nothing; start a new one, which takes the lock over. */
static gboolean lock_watch(gpointer data)
{
	char *mark = data;

	if (g_file_test(mark, G_FILE_TEST_EXISTS) && !lock_running())
		panels_lock();
	return G_SOURCE_CONTINUE;
}

/* powerkey asks for the lock screen (the lid stayed closed) with SIGUSR1. */
static gboolean lock_signal(gpointer data)
{
	(void)data;
	panels_lock();
	return G_SOURCE_CONTINUE;
}

static void startup(GApplication *gapp, gpointer data)
{
	(void)data;
	load_css();
	wayland_init();
	panels_init(GTK_APPLICATION(gapp));
	g_unix_signal_add(SIGUSR1, lock_signal, NULL);
	{
		/* A new session is not locked: a mark left from an earlier one goes. */
		char *mark = g_build_filename(g_get_user_runtime_dir(), "wormos-locked", NULL);

		g_unlink(mark);
		g_timeout_add_seconds(2, lock_watch, mark);
	}
	notify_init(GTK_APPLICATION(gapp));
	/* The shell lives as long as the session, with or without windows. */
	g_application_hold(gapp);
}

static int command_line(GApplication *gapp, GApplicationCommandLine *cmd, gpointer data)
{
	GVariantDict *options = g_application_command_line_get_options_dict(cmd);
	const char *volume, *brightness, *quiet;

	(void)data;
	if (g_variant_dict_contains(options, "launcher"))
		launcher_toggle(GTK_APPLICATION(gapp));
	if (g_variant_dict_lookup(options, "volume", "&s", &volume))
		panels_volume(volume);
	if (g_variant_dict_lookup(options, "brightness", "&s", &brightness))
		panels_brightness(brightness);
	if (g_variant_dict_lookup(options, "do-not-disturb", "&s", &quiet)) {
		gboolean on = !strcmp(quiet, "on") ? TRUE : !strcmp(quiet, "off") ? FALSE
			      : !notify_quiet();

		config_set_bool("notifications", "do-not-disturb", on);
	}
	if (g_variant_dict_contains(options, "quit"))
		g_application_quit(gapp);
	return 0;
}

int main(int argc, char **argv)
{
	static const GOptionEntry entries[] = {
		{ "launcher", 0, 0, G_OPTION_ARG_NONE, NULL,
		  "Open or close the application launcher", NULL },
		{ "volume", 0, 0, G_OPTION_ARG_STRING, NULL,
		  "Change the volume of the sound output", "up|down|mute" },
		{ "brightness", 0, 0, G_OPTION_ARG_STRING, NULL,
		  "Change the screen's brightness", "up|down" },
		{ "do-not-disturb", 0, 0, G_OPTION_ARG_STRING, NULL,
		  "Show only urgent notifications", "on|off|toggle" },
		{ "quit", 0, 0, G_OPTION_ARG_NONE, NULL, "Stop the running shell", NULL },
		{ "battery", 0, 0, G_OPTION_ARG_NONE, NULL, "Show the battery's charge and exit", NULL },
		{ NULL, 0, 0, 0, NULL, NULL, NULL },
	};
	GtkApplication *app = gtk_application_new(SHELL_APP_ID,
						  G_APPLICATION_HANDLES_COMMAND_LINE);
	int status;

	/* --battery: what the panel would show, without a running shell. */
	if (argc == 2 && !strcmp(argv[1], "--battery")) {
		struct status s;

		status_battery(&s);
		if (s.battery < 0)
			puts("No battery");
		else
			printf("Battery: %d%%%s\n", s.battery, s.full ? ", full" :
			       s.charging ? ", charging" : "");
		g_object_unref(app);
		return 0;
	}
	g_application_add_main_option_entries(G_APPLICATION(app), entries);
	g_signal_connect(app, "startup", G_CALLBACK(startup), NULL);
	g_signal_connect(app, "command-line", G_CALLBACK(command_line), NULL);
	status = g_application_run(G_APPLICATION(app), argc, argv);
	g_object_unref(app);
	return status;
}
