/*
 * wormos-welcome - Welcome: what this system does, in a few pages.
 *
 * Opened the first time a user starts the desktop (WormOS Amnesia: at
 * every start, since every start is a first one), and from the launcher.
 * Each page says one thing and has buttons that open the program it is
 * about. The pages follow the edition (/etc/worm/variant). The last page
 * lists the keyboard shortcuts; --shortcuts opens on it (Super+/).
 */

#define _GNU_SOURCE
#include <gtk/gtk.h>
#include <stdlib.h>
#include <string.h>

#define APP_ID  "org.wormos.Welcome"
#define CSS     "/usr/share/wormos/welcome/welcome.css"

struct action {
	const char *label, *command;
};

struct page {
	const char *icon, *title, *text;
	struct action actions[3];
};

static const struct page wormos_pages[] = {
	{ "org.wormos.Settings.About", "Welcome to %s",
	  "A safe place to try programs, write code and open files you do not trust. "
	  "Nothing here has to be learned first: this short tour shows what makes it "
	  "different, and everything has a window as well as a command.", { { NULL, NULL } } },
	{ "document-open-recent-symbolic", "You can always go back",
	  "WormOS takes a snapshot of the system before every change. If something breaks, "
	  "open Snapshots and rewind: the system is as it was. Take one yourself before "
	  "anything risky (Super+S), and bring back a single file the same way.",
	  { { "Open Snapshots", "wormos-snapshots" } } },
	{ "org.wormos.Software", "Get the programs you need",
	  "Software finds programs among WormOS's own and tens of thousands from Debian and "
	  "Kali. Those from Debian and Kali are ready in moments, each source in its own "
	  "wormhole; WormOS's own are added at the next restart, so nothing closes under you.",
	  { { "Open Software", "wormos-software" } } },
	{ "security-high-symbolic", "Private and protected",
	  "The firewall blocks everything that comes in. Tor Browser is in the launcher, and "
	  "anonymous mode sends all traffic through Tor (Settings, Network & privacy). Programs "
	  "you do not trust belong in a blackhole, a sandbox that cannot reach your files: "
	  "bh open lab in a terminal.",
	  { { "Open Tor Browser", "tor-browser" },
	    { "Privacy settings", "wormos-settings --page privacy" } } },
	{ NULL, "Find your way", NULL,
	  { { "Open Settings", "wormos-settings" },
	    { "Read the guide", "foot --title \"WormOS guide\" sh -c \"welcome --full | less -R\"" } } },
};

/* Amnesia's pages keep to its greys: symbolic icons only. */
static const struct page amnesia_pages[] = {
	{ "edit-clear-all-symbolic", "Welcome to %s",
	  "This system forgets. Everything runs from memory: when the computer stops, "
	  "everything done here is gone, files, downloads, settings and programs alike. "
	  "Nothing is written to a disk. Start it again and it is exactly as new.", { { NULL, NULL } } },
	{ "network-vpn-symbolic", "Everything goes through Tor",
	  "Every connection goes through the Tor network from the first moment; anything that "
	  "cannot is blocked. The shield in the panel shows how far Tor has connected, and "
	  "gives new connections new circuits when you want a fresh start.",
	  { { "Open Tor Browser", "tor-browser" },
	    { "New Tor circuits", "/usr/lib/worm/quick-action new-circuits" } } },
	{ "system-software-install-symbolic", "Programs, for this session",
	  "Software adds programs from WormOS, Debian and Kali, through Tor, without any "
	  "password. They stay until the computer stops.",
	  { { "Open Software", "wormos-software" } } },
	{ "system-shutdown-symbolic", "In a hurry",
	  "Super+Shift+Escape shuts down at once, and so does pulling out the USB stick. "
	  "The session is wiped from memory first.", { { NULL, NULL } } },
	{ "dialog-warning-symbolic", "What it cannot protect you from",
	  "A computer that is already compromised (its firmware, a hardware key logger, the host "
	  "of a virtual machine), what you reveal yourself (names, accounts, writing style, the "
	  "files you share), and attacks on the Tor network itself. Signing in to your own "
	  "accounts links them to you.", { { NULL, NULL } } },
	{ NULL, "Find your way", NULL, { { "Open Settings", "wormos-settings" } } },
};

static const char *const shortcuts[][2] = {
	{ "Super", "Open the launcher" },
	{ "Super + Enter", "Open a terminal" },
	{ "Super + E", "Files" },
	{ "Super + I", "Settings" },
	{ "Super + S", "Take a snapshot" },
	{ "Super + Q", "Close the window" },
	{ "Alt + Tab", "Switch windows" },
	{ "Super + \xe2\x86\x90 / \xe2\x86\x92", "Fill the left or right half" },
	{ "Super + F", "Maximize" },
	{ "Super + N", "Do not disturb" },
	{ "Print", "Screenshot (Shift + Print: in 5 seconds)" },
	{ "Super + /", "These shortcuts" },
};

static GtkWidget *window, *stack, *back, *next, *dots;
static const struct page *pages;
static size_t n_pages;
static gboolean amnesia;

static char *read_value(const char *path, const char *key)
{
	char *text = NULL, *value = NULL;
	size_t klen = strlen(key);

	if (!g_file_get_contents(path, &text, NULL, NULL))
		return NULL;
	for (char *line = strtok(text, "\n"); line && !value; line = strtok(NULL, "\n"))
		if (!strncmp(line, key, klen) && line[klen] == '=') {
			char *v = line + klen + 1;
			size_t n = strlen(v);

			if (n >= 2 && v[0] == '"' && v[n - 1] == '"')
				value = g_strndup(v + 1, n - 2);
			else
				value = g_strdup(v);
		}
	g_free(text);
	return value;
}

static void run(GtkButton *button, gpointer data)
{
	GError *error = NULL;

	(void)button;
	if (!g_spawn_command_line_async(data, &error)) {
		g_warning("cannot start %s: %s", (const char *)data, error->message);
		g_error_free(error);
	}
}

static GtkWidget *shortcut_grid(void)
{
	GtkWidget *grid = gtk_grid_new();

	gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
	gtk_grid_set_column_spacing(GTK_GRID(grid), 18);
	gtk_widget_set_halign(grid, GTK_ALIGN_CENTER);
	for (size_t i = 0; i < G_N_ELEMENTS(shortcuts); i++) {
		GtkWidget *key = gtk_label_new(shortcuts[i][0]), *what = gtk_label_new(shortcuts[i][1]);

		gtk_widget_add_css_class(key, "key");
		gtk_label_set_xalign(GTK_LABEL(key), 1);
		gtk_label_set_xalign(GTK_LABEL(what), 0);
		gtk_grid_attach(GTK_GRID(grid), key, 0, (int)i, 1, 1);
		gtk_grid_attach(GTK_GRID(grid), what, 1, (int)i, 1, 1);
	}
	if (amnesia) {
		/* Amnesia has one more, and it matters most. */
		GtkWidget *key = gtk_label_new("Super + Shift + Esc"),
			  *what = gtk_label_new("Shut down at once");

		gtk_widget_add_css_class(key, "key");
		gtk_label_set_xalign(GTK_LABEL(key), 1);
		gtk_label_set_xalign(GTK_LABEL(what), 0);
		gtk_grid_attach(GTK_GRID(grid), key, 0, G_N_ELEMENTS(shortcuts), 1, 1);
		gtk_grid_attach(GTK_GRID(grid), what, 1, G_N_ELEMENTS(shortcuts), 1, 1);
	}
	return grid;
}

static GtkWidget *page_widget(const struct page *p, const char *name)
{
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 16), *label, *row;
	char *title = g_strdup_printf(p->title, name);

	gtk_widget_add_css_class(box, "page");
	gtk_widget_set_hexpand(box, TRUE);
	gtk_widget_set_valign(box, GTK_ALIGN_CENTER);
	if (p->icon) {
		GtkWidget *icon = gtk_image_new_from_icon_name(p->icon);

		gtk_image_set_pixel_size(GTK_IMAGE(icon), 96);
		gtk_widget_add_css_class(icon, "hero");
		gtk_box_append(GTK_BOX(box), icon);
	}
	label = gtk_label_new(title);
	gtk_widget_add_css_class(label, "title-1");
	gtk_label_set_wrap(GTK_LABEL(label), TRUE);
	gtk_label_set_justify(GTK_LABEL(label), GTK_JUSTIFY_CENTER);
	gtk_box_append(GTK_BOX(box), label);
	g_free(title);
	if (p->text) {
		label = gtk_label_new(p->text);
		gtk_widget_add_css_class(label, "body");
		gtk_label_set_wrap(GTK_LABEL(label), TRUE);
		gtk_label_set_max_width_chars(GTK_LABEL(label), 56);
		gtk_label_set_justify(GTK_LABEL(label), GTK_JUSTIFY_CENTER);
		gtk_box_append(GTK_BOX(box), label);
	} else {
		gtk_box_append(GTK_BOX(box), shortcut_grid());
	}
	row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
	gtk_widget_set_halign(row, GTK_ALIGN_CENTER);
	for (int i = 0; i < 3 && p->actions[i].label; i++) {
		GtkWidget *button = gtk_button_new_with_label(p->actions[i].label);

		gtk_widget_add_css_class(button, "pill");
		g_signal_connect(button, "clicked", G_CALLBACK(run), (gpointer)p->actions[i].command);
		gtk_box_append(GTK_BOX(row), button);
	}
	if (gtk_widget_get_first_child(row))
		gtk_box_append(GTK_BOX(box), row);
	return box;
}

static void update_buttons(void)
{
	const char *name = gtk_stack_get_visible_child_name(GTK_STACK(stack));
	guint i = name ? (guint)atoi(name) : 0, k = 0;

	gtk_widget_set_sensitive(back, i > 0);
	gtk_button_set_label(GTK_BUTTON(next), i + 1 < n_pages ? "Next" :
			     amnesia ? "Start" : "Get started");
	for (GtkWidget *d = gtk_widget_get_first_child(dots); d; d = gtk_widget_get_next_sibling(d), k++)
		if (k == i)
			gtk_widget_add_css_class(d, "current");
		else
			gtk_widget_remove_css_class(d, "current");
}

static void go(int step)
{
	const char *name = gtk_stack_get_visible_child_name(GTK_STACK(stack));
	int i = name ? atoi(name) : 0;
	char target[24];

	if (i + step >= (int)n_pages) {
		gtk_window_close(GTK_WINDOW(window));
		return;
	}
	if (i + step < 0)
		return;
	gtk_stack_set_transition_type(GTK_STACK(stack), step > 0 ?
				      GTK_STACK_TRANSITION_TYPE_SLIDE_LEFT :
				      GTK_STACK_TRANSITION_TYPE_SLIDE_RIGHT);
	snprintf(target, sizeof(target), "%d", i + step);
	gtk_stack_set_visible_child_name(GTK_STACK(stack), target);
	update_buttons();
}

static void back_clicked(GtkButton *b, gpointer d) { (void)b; (void)d; go(-1); }
static void next_clicked(GtkButton *b, gpointer d) { (void)b; (void)d; go(1); }

static gboolean key_pressed(GtkEventControllerKey *c, guint keyval, guint code,
			    GdkModifierType state, gpointer data)
{
	(void)c; (void)code; (void)state; (void)data;
	if (keyval == GDK_KEY_Right || keyval == GDK_KEY_Page_Down) {
		go(1);
		return TRUE;
	}
	if (keyval == GDK_KEY_Left || keyval == GDK_KEY_Page_Up) {
		go(-1);
		return TRUE;
	}
	if (keyval == GDK_KEY_Escape) {
		gtk_window_close(GTK_WINDOW(window));
		return TRUE;
	}
	return FALSE;
}

static void activate(GtkApplication *app, gpointer data)
{
	GtkWidget *box, *bar;
	GtkEventController *key;
	GtkCssProvider *css;
	char *name, *variant;
	gboolean only_shortcuts = GPOINTER_TO_INT(data);

	if (window) {
		gtk_window_present(GTK_WINDOW(window));
		return;
	}
	variant = read_value("/etc/worm/variant", "VARIANT_ID");
	amnesia = variant && !strcmp(variant, "amnesia");
	g_free(variant);
	pages = amnesia ? amnesia_pages : wormos_pages;
	n_pages = amnesia ? G_N_ELEMENTS(amnesia_pages) : G_N_ELEMENTS(wormos_pages);
	name = read_value("/etc/os-release", "PRETTY_NAME");
	if (!name)
		name = g_strdup("WormOS");
	if (amnesia) {
		char *v = read_value("/etc/worm/variant", "VARIANT");

		if (v && !strstr(name, v)) {
			char *both = g_strdup_printf("%s %s", name, v);

			g_free(name);
			name = both;
		}
		g_free(v);
	}

	css = gtk_css_provider_new();
	gtk_css_provider_load_from_path(css, CSS);
	gtk_style_context_add_provider_for_display(gdk_display_get_default(),
						   GTK_STYLE_PROVIDER(css),
						   GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
	g_object_unref(css);

	window = gtk_application_window_new(app);
	gtk_window_set_title(GTK_WINDOW(window), only_shortcuts ? "Keyboard shortcuts" : "Welcome");
	gtk_window_set_default_size(GTK_WINDOW(window), 720, 560);
	box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	stack = gtk_stack_new();
	gtk_stack_set_transition_duration(GTK_STACK(stack), 220);
	gtk_widget_set_vexpand(stack, TRUE);
	for (size_t i = 0; i < n_pages; i++) {
		char id[24];

		snprintf(id, sizeof(id), "%zu", i);
		gtk_stack_add_named(GTK_STACK(stack), page_widget(&pages[i], name), id);
	}
	g_free(name);
	gtk_box_append(GTK_BOX(box), stack);

	bar = gtk_center_box_new();
	gtk_widget_add_css_class(bar, "bar");
	back = gtk_button_new_with_label("Back");
	g_signal_connect(back, "clicked", G_CALLBACK(back_clicked), NULL);
	next = gtk_button_new_with_label("Next");
	gtk_widget_add_css_class(next, "suggested-action");
	g_signal_connect(next, "clicked", G_CALLBACK(next_clicked), NULL);
	dots = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	gtk_widget_set_valign(dots, GTK_ALIGN_CENTER);
	for (size_t i = 0; i < n_pages; i++) {
		GtkWidget *dot = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);

		gtk_widget_add_css_class(dot, "dot");
		gtk_box_append(GTK_BOX(dots), dot);
	}
	gtk_center_box_set_start_widget(GTK_CENTER_BOX(bar), back);
	gtk_center_box_set_center_widget(GTK_CENTER_BOX(bar), dots);
	gtk_center_box_set_end_widget(GTK_CENTER_BOX(bar), next);
	gtk_box_append(GTK_BOX(box), bar);
	gtk_window_set_child(GTK_WINDOW(window), box);

	key = gtk_event_controller_key_new();
	g_signal_connect(key, "key-pressed", G_CALLBACK(key_pressed), NULL);
	gtk_widget_add_controller(window, key);

	if (only_shortcuts) {
		char id[24];

		snprintf(id, sizeof(id), "%zu", n_pages - 1);
		gtk_stack_set_transition_type(GTK_STACK(stack), GTK_STACK_TRANSITION_TYPE_NONE);
		gtk_stack_set_visible_child_name(GTK_STACK(stack), id);
	}
	update_buttons();
	gtk_window_present(GTK_WINDOW(window));
	gtk_widget_grab_focus(next);
}

int main(int argc, char **argv)
{
	GtkApplication *app = gtk_application_new(APP_ID, G_APPLICATION_DEFAULT_FLAGS);
	gboolean only_shortcuts = argc > 1 && !strcmp(argv[1], "--shortcuts");
	int status;

	g_signal_connect(app, "activate", G_CALLBACK(activate), GINT_TO_POINTER(only_shortcuts));
	status = g_application_run(G_APPLICATION(app), 1, argv);
	g_object_unref(app);
	return status;
}
