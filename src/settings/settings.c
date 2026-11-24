/*
 * wormos-settings - the WormOS Settings program.
 *
 * Pages: Display, Appearance, Keyboard, Mouse & touchpad, Sound,
 * Notifications, Date & time, Network & privacy, Virtual machine, About.
 * "wormos-settings --page NAME" opens a page (also in the running
 * program).
 *
 * The user's own settings are plain files the desktop reads and watches:
 *
 *     ~/.config/wormos/display          display scale (worm-shell applies it)
 *     ~/.config/wormos/shell            panel position, clock, do not disturb
 *     ~/.config/wormos/wallpaper        the wallpaper (/usr/lib/worm/wallpaper)
 *     ~/.config/wormos/xdg/labwc/rc.xml keyboard repeat, pointer and touchpad,
 *                                       merged by labwc over the system's
 *                                       rc.xml and under the user's own
 *     ~/.config/labwc/environment       the keyboard layout (XKB_DEFAULT_LAYOUT)
 *
 * System settings (time zone, host name, anonymous mode, automatic
 * snapshots, the virtual machine's settings) go through the setuid helpers
 * /usr/lib/worm/settings and /usr/lib/worm/snapshots, which need root's
 * password, asked for in a dialog.
 */

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/statvfs.h>
#include <sys/utsname.h>
#include <unistd.h>

#include <gtk/gtk.h>

#define APP_ID          "org.wormos.Settings"
#define CSS_PATH        "/usr/share/wormos/settings/settings.css"
#define SETTINGS_HELPER "/usr/lib/worm/settings"
#define SNAP_HELPER     "/usr/lib/worm/snapshots"
#define PASSWD_HELPER   "/usr/lib/worm/passwd"
#define VM_RUN          "/run/worm/vm"
#define NO_ROOT         "/etc/worm/no-root"

static GtkWidget *window, *stack, *sidebar;

/* ---- small helpers ---- */

static char *config_path(const char *name)
{
	return g_build_filename(g_get_user_config_dir(), "wormos", name, NULL);
}

static void save_file(const char *path, const char *text)
{
	char *dir = g_path_get_dirname(path);
	GError *error = NULL;

	g_mkdir_with_parents(dir, 0700);
	if (!g_file_set_contents(path, text, -1, &error)) {
		g_warning("cannot save %s: %s", path, error->message);
		g_error_free(error);
	}
	g_free(dir);
}

/* Standard output of a short command (NULL if it could not run). */
static char *capture(const char *const *argv)
{
	char *out = NULL;

	if (!g_spawn_sync(NULL, (char **)argv, NULL, G_SPAWN_SEARCH_PATH |
			  G_SPAWN_STDERR_TO_DEV_NULL, NULL, NULL, &out, NULL, NULL, NULL))
		return NULL;
	return out;
}

static void spawn(const char *const *argv)
{
	GError *error = NULL;

	if (!g_spawn_async(NULL, (char **)argv, NULL, G_SPAWN_SEARCH_PATH |
			   G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL,
			   NULL, NULL, NULL, &error)) {
		g_warning("cannot run %s: %s", argv[0], error->message);
		g_error_free(error);
	}
}

/* KEY=VALUE from /run/worm/vm; g_free. */
static char *vm_value(const char *key)
{
	char *text = NULL, *value = NULL;

	if (!g_file_get_contents(VM_RUN, &text, NULL, NULL))
		return NULL;
	for (char **lines = g_strsplit(text, "\n", -1), **l = lines; *l || (g_strfreev(lines), 0); l++) {
		size_t n = strlen(key);

		if (!value && !strncmp(*l, key, n) && (*l)[n] == '=')
			value = g_strdup(*l + n + 1);
	}
	g_free(text);
	return value;
}

static void labwc_reconfigure(void)
{
	spawn((const char *const[]){ "labwc", "--reconfigure", NULL });
}

/* ---- the user's shell settings (~/.config/wormos/shell) ---- */

static GKeyFile *shell_keys(void)
{
	GKeyFile *k = g_key_file_new();
	char *path = config_path("shell");

	g_key_file_load_from_file(k, path, G_KEY_FILE_KEEP_COMMENTS, NULL);
	g_free(path);
	return k;
}

static gboolean shell_bool(const char *group, const char *key, gboolean fallback)
{
	GKeyFile *k = shell_keys();
	GError *error = NULL;
	gboolean v = g_key_file_get_boolean(k, group, key, &error);

	if (error) {
		v = fallback;
		g_error_free(error);
	}
	g_key_file_unref(k);
	return v;
}

static char *shell_string(const char *group, const char *key)
{
	GKeyFile *k = shell_keys();
	char *v = g_key_file_get_string(k, group, key, NULL);

	g_key_file_unref(k);
	return v;
}

static void shell_set(const char *group, const char *key, const char *value)
{
	GKeyFile *k = shell_keys();
	char *path = config_path("shell"), *dir = g_path_get_dirname(path);

	g_key_file_set_string(k, group, key, value);
	g_mkdir_with_parents(dir, 0700);
	g_key_file_save_to_file(k, path, NULL);
	g_key_file_unref(k);
	g_free(dir);
	g_free(path);
}

/* ---- building pages ---- */

static GtkWidget *page_new(const char *name, const char *title, const char *icon,
			   const char *subtitle)
{
	GtkWidget *scroll = gtk_scrolled_window_new(), *clamp, *box, *label, *row, *rbox;

	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER,
				       GTK_POLICY_AUTOMATIC);
	clamp = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_widget_set_halign(clamp, GTK_ALIGN_CENTER);
	gtk_widget_set_size_request(clamp, 560, -1);
	box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 18);
	gtk_widget_add_css_class(box, "page");
	gtk_box_append(GTK_BOX(clamp), box);
	gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), clamp);

	label = gtk_label_new(title);
	gtk_widget_add_css_class(label, "page-title");
	gtk_label_set_xalign(GTK_LABEL(label), 0);
	gtk_box_append(GTK_BOX(box), label);
	if (subtitle) {
		label = gtk_label_new(subtitle);
		gtk_widget_add_css_class(label, "dim-label");
		gtk_label_set_xalign(GTK_LABEL(label), 0);
		gtk_label_set_wrap(GTK_LABEL(label), TRUE);
		gtk_box_append(GTK_BOX(box), label);
	}
	gtk_stack_add_named(GTK_STACK(stack), scroll, name);

	row = gtk_list_box_row_new();
	rbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
	gtk_box_append(GTK_BOX(rbox), gtk_image_new_from_icon_name(icon));
	gtk_box_append(GTK_BOX(rbox), gtk_label_new(title));
	gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), rbox);
	g_object_set_data_full(G_OBJECT(row), "page", g_strdup(name), g_free);
	gtk_list_box_append(GTK_LIST_BOX(sidebar), row);
	g_object_set_data(G_OBJECT(box), "scroll", scroll);
	return box;
}

static GtkWidget *group_new(GtkWidget *page, const char *title, const char *description)
{
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6), *list, *label;

	if (title) {
		label = gtk_label_new(title);
		gtk_widget_add_css_class(label, "heading");
		gtk_label_set_xalign(GTK_LABEL(label), 0);
		gtk_box_append(GTK_BOX(box), label);
	}
	if (description) {
		label = gtk_label_new(description);
		gtk_widget_add_css_class(label, "dim-label");
		gtk_label_set_xalign(GTK_LABEL(label), 0);
		gtk_label_set_wrap(GTK_LABEL(label), TRUE);
		gtk_box_append(GTK_BOX(box), label);
	}
	list = gtk_list_box_new();
	gtk_list_box_set_selection_mode(GTK_LIST_BOX(list), GTK_SELECTION_NONE);
	gtk_widget_add_css_class(list, "settings-group");
	gtk_box_append(GTK_BOX(box), list);
	gtk_box_append(GTK_BOX(page), box);
	return list;
}

/* A row: title, optional subtitle (returned through sub), widget at the end. */
static GtkWidget *row_new(GtkWidget *group, const char *title, const char *subtitle,
			  GtkWidget *suffix, GtkWidget **sub)
{
	GtkWidget *row = gtk_list_box_row_new(), *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
	GtkWidget *text = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2), *label;

	gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), FALSE);
	gtk_widget_add_css_class(box, "settings-row");
	label = gtk_label_new(title);
	gtk_label_set_xalign(GTK_LABEL(label), 0);
	gtk_label_set_wrap(GTK_LABEL(label), TRUE);
	gtk_box_append(GTK_BOX(text), label);
	label = gtk_label_new(subtitle ? subtitle : "");
	gtk_label_set_xalign(GTK_LABEL(label), 0);
	gtk_label_set_wrap(GTK_LABEL(label), TRUE);
	gtk_label_set_selectable(GTK_LABEL(label), TRUE);
	gtk_widget_add_css_class(label, "dim-label");
	gtk_widget_add_css_class(label, "caption");
	gtk_widget_set_visible(label, subtitle != NULL || sub != NULL);
	gtk_box_append(GTK_BOX(text), label);
	if (sub)
		*sub = label;
	gtk_widget_set_hexpand(text, TRUE);
	gtk_widget_set_valign(text, GTK_ALIGN_CENTER);
	gtk_box_append(GTK_BOX(box), text);
	if (suffix) {
		gtk_widget_set_valign(suffix, GTK_ALIGN_CENTER);
		gtk_box_append(GTK_BOX(box), suffix);
	}
	gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), box);
	gtk_list_box_append(GTK_LIST_BOX(group), row);
	return row;
}

static GtkWidget *switch_new(gboolean active, GCallback changed, gpointer data)
{
	GtkWidget *sw = gtk_switch_new();

	gtk_switch_set_active(GTK_SWITCH(sw), active);
	g_signal_connect(sw, "notify::active", changed, data);
	return sw;
}

static GtkWidget *button_new(const char *label, GCallback clicked, gpointer data)
{
	GtkWidget *b = gtk_button_new_with_label(label);

	g_signal_connect(b, "clicked", clicked, data);
	return b;
}

/* ---- root's password, for system changes ---- */

typedef void (*admin_done_fn)(gboolean ok, gpointer data);

struct admin {
	GtkWidget *dialog, *entry, *message, *ok;
	char **argv;
	gboolean password;
	admin_done_fn done;
	gpointer data;
};

static void admin_free(struct admin *a)
{
	g_strfreev(a->argv);
	g_free(a);
}

static gboolean root_locked(void)
{
	return g_file_test(NO_ROOT, G_FILE_TEST_EXISTS);
}

static void admin_finished(GObject *source, GAsyncResult *res, gpointer data)
{
	struct admin *a = data;
	GSubprocess *proc = G_SUBPROCESS(source);
	char *err = NULL;
	GError *error = NULL;
	int status;

	g_subprocess_communicate_utf8_finish(proc, res, NULL, &err, &error);
	status = g_subprocess_get_if_exited(proc) ? g_subprocess_get_exit_status(proc) : -1;
	g_object_unref(proc);
	if (status == 0) {
		if (a->dialog)
			gtk_window_destroy(GTK_WINDOW(a->dialog));
		if (a->done)
			a->done(TRUE, a->data);
		admin_free(a);
	} else if (a->dialog) {
		gtk_widget_set_sensitive(a->dialog, TRUE);
		gtk_label_set_text(GTK_LABEL(a->message), status == 3 ? "Wrong password, try again." :
				   err && *err ? g_strstrip(err) : "It did not work.");
		gtk_widget_add_css_class(a->message, "error");
		if (a->entry) {
			gtk_editable_set_text(GTK_EDITABLE(a->entry), "");
			gtk_widget_grab_focus(a->entry);
		}
	} else {
		if (a->done)
			a->done(FALSE, a->data);
		admin_free(a);
	}
	g_free(err);
	if (error)
		g_error_free(error);
}

static void admin_run(struct admin *a, const char *password)
{
	GError *error = NULL;
	GSubprocess *proc = g_subprocess_newv((const char *const *)a->argv,
					      G_SUBPROCESS_FLAGS_STDIN_PIPE |
					      G_SUBPROCESS_FLAGS_STDOUT_SILENCE |
					      G_SUBPROCESS_FLAGS_STDERR_PIPE, &error);
	char *input;

	if (!proc) {
		if (a->message)
			gtk_label_set_text(GTK_LABEL(a->message), error->message);
		g_error_free(error);
		return;
	}
	if (a->dialog)
		gtk_widget_set_sensitive(a->dialog, FALSE);
	input = g_strdup_printf("%s\n", password ? password : "");
	g_subprocess_communicate_utf8_async(proc, input, NULL, admin_finished, a);
	explicit_bzero(input, strlen(input));
	g_free(input);
}

static void admin_ok(GtkWidget *w, gpointer data)
{
	struct admin *a = data;

	(void)w;
	admin_run(a, gtk_editable_get_text(GTK_EDITABLE(a->entry)));
}

static void admin_cancel(GtkWidget *w, gpointer data)
{
	struct admin *a = data;

	(void)w;
	gtk_window_destroy(GTK_WINDOW(a->dialog));
	if (a->done)
		a->done(FALSE, a->data);
	admin_free(a);
}

/* Run a helper; with password, ask for root's password first. */
static void admin(const char *what, gboolean password, admin_done_fn done, gpointer data,
		  const char *first, ...)
{
	struct admin *a = g_new0(struct admin, 1);
	GPtrArray *argv = g_ptr_array_new();
	GtkWidget *box, *label, *buttons, *cancel;
	va_list ap;

	va_start(ap, first);
	for (const char *s = first; s; s = va_arg(ap, const char *))
		g_ptr_array_add(argv, g_strdup(s));
	va_end(ap);
	g_ptr_array_add(argv, NULL);
	a->argv = (char **)g_ptr_array_free(argv, FALSE);
	a->password = password;
	a->done = done;
	a->data = data;
	if (!password) {
		admin_run(a, NULL);
		return;
	}
	if (root_locked()) {
		GtkAlertDialog *alert = gtk_alert_dialog_new("Root is locked on this system");

		gtk_alert_dialog_set_detail(alert, "System settings cannot be changed here.");
		gtk_alert_dialog_show(alert, GTK_WINDOW(window));
		g_object_unref(alert);
		if (done)
			done(FALSE, data);
		admin_free(a);
		return;
	}

	a->dialog = gtk_window_new();
	gtk_window_set_transient_for(GTK_WINDOW(a->dialog), GTK_WINDOW(window));
	gtk_window_set_modal(GTK_WINDOW(a->dialog), TRUE);
	gtk_window_set_title(GTK_WINDOW(a->dialog), "Administrator password");
	gtk_window_set_resizable(GTK_WINDOW(a->dialog), FALSE);
	box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
	gtk_widget_add_css_class(box, "dialog-box");
	label = gtk_label_new(what);
	gtk_widget_add_css_class(label, "heading");
	gtk_label_set_wrap(GTK_LABEL(label), TRUE);
	gtk_label_set_max_width_chars(GTK_LABEL(label), 40);
	gtk_box_append(GTK_BOX(box), label);
	label = gtk_label_new("Enter the root password to change the system.");
	gtk_widget_add_css_class(label, "dim-label");
	gtk_box_append(GTK_BOX(box), label);
	a->entry = gtk_password_entry_new();
	gtk_password_entry_set_show_peek_icon(GTK_PASSWORD_ENTRY(a->entry), TRUE);
	g_signal_connect(a->entry, "activate", G_CALLBACK(admin_ok), a);
	gtk_box_append(GTK_BOX(box), a->entry);
	a->message = gtk_label_new("");
	gtk_label_set_wrap(GTK_LABEL(a->message), TRUE);
	gtk_box_append(GTK_BOX(box), a->message);
	buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	gtk_widget_set_halign(buttons, GTK_ALIGN_END);
	cancel = button_new("Cancel", G_CALLBACK(admin_cancel), a);
	a->ok = button_new("Change", G_CALLBACK(admin_ok), a);
	gtk_widget_add_css_class(a->ok, "suggested-action");
	gtk_box_append(GTK_BOX(buttons), cancel);
	gtk_box_append(GTK_BOX(buttons), a->ok);
	gtk_box_append(GTK_BOX(box), buttons);
	gtk_window_set_child(GTK_WINDOW(a->dialog), box);
	gtk_window_set_default_widget(GTK_WINDOW(a->dialog), a->ok);
	gtk_window_present(GTK_WINDOW(a->dialog));
}

/* ==== Display ==== */

static const double scales[] = { 0, 0.75, 0.8, 0.9, 1, 1.25, 1.5, 1.75, 2 };
static const char *const scale_names[] = { "Automatic", "75%", "80%", "90%", "100%", "125%",
					   "150%", "175%", "200%", NULL };
static GtkWidget *outputs_group;

static double current_scale(void)
{
	char *path = config_path("display"), *text = NULL;
	double v = 0;

	if (g_file_get_contents(path, &text, NULL, NULL) && sscanf(text, "scale = %lf", &v) != 1)
		v = 0;
	g_free(text);
	g_free(path);
	return v;
}

static void scale_selected(GtkDropDown *d, GParamSpec *p, gpointer data)
{
	guint i = gtk_drop_down_get_selected(d);
	char *path = config_path("display"), *text;

	(void)p;
	(void)data;
	if (i >= G_N_ELEMENTS(scales))
		return;
	text = scales[i] > 0 ? g_strdup_printf("scale = %.2f\n", scales[i]) : g_strdup("scale = auto\n");
	save_file(path, text);
	g_free(text);
	g_free(path);
}

static void mode_selected(GtkDropDown *d, GParamSpec *p, gpointer data)
{
	const char *output = data;
	GtkStringObject *item = gtk_drop_down_get_selected_item(d);
	char **modes = g_object_get_data(G_OBJECT(d), "modes");
	guint i = gtk_drop_down_get_selected(d);

	(void)p;
	if (!item || !modes || i >= g_strv_length(modes) || g_object_get_data(G_OBJECT(d), "filling"))
		return;
	spawn((const char *const[]){ "wlr-randr", "--output", output, "--mode", modes[i], NULL });
}

static void free_data(gpointer data, GClosure *closure)
{
	(void)closure;
	g_free(data);
}

/* The screens, from wlr-randr: name, description, modes. */
static void outputs_fill(void)
{
	char *out = capture((const char *const[]){ "wlr-randr", NULL });
	char **lines;
	GtkWidget *child, *dropdown = NULL;
	GtkStringList *names = NULL;
	GPtrArray *modes = NULL;
	char *name = NULL, *desc = NULL;
	int current = -1;

	while ((child = gtk_widget_get_first_child(outputs_group)))
		gtk_list_box_remove(GTK_LIST_BOX(outputs_group), child);
	if (!out) {
		row_new(outputs_group, "Screens", "wlr-randr is not available.", NULL, NULL);
		return;
	}
	lines = g_strsplit(out, "\n", -1);
	for (int i = 0; ; i++) {
		char *l = lines[i];

		if (!l || (l[0] && l[0] != ' ')) {
			if (name && dropdown) {
				g_ptr_array_add(modes, NULL);
				g_object_set_data_full(G_OBJECT(dropdown), "modes",
						       g_ptr_array_free(modes, FALSE),
						       (GDestroyNotify)g_strfreev);
				gtk_drop_down_set_model(GTK_DROP_DOWN(dropdown), G_LIST_MODEL(names));
				if (current >= 0)
					gtk_drop_down_set_selected(GTK_DROP_DOWN(dropdown), current);
				g_object_set_data(G_OBJECT(dropdown), "filling", NULL);
				g_signal_connect_data(dropdown, "notify::selected", G_CALLBACK(mode_selected),
						      g_strdup(name), free_data, 0);
				row_new(outputs_group, name, desc, dropdown, NULL);
				g_object_unref(names);
			}
			g_free(name);
			g_free(desc);
			name = desc = NULL;
			if (!l)
				break;
			name = g_strndup(l, strcspn(l, " "));
			desc = strchr(l, '"') ? g_strdup(strchr(l, '"') + 1) : g_strdup("");
			if (strchr(desc, '"'))
				*strchr(desc, '"') = '\0';
			dropdown = gtk_drop_down_new(NULL, NULL);
			g_object_set_data(G_OBJECT(dropdown), "filling", GINT_TO_POINTER(1));
			names = gtk_string_list_new(NULL);
			modes = g_ptr_array_new();
			current = -1;
			continue;
		}
		if (name && strstr(l, " px, ")) {
			int w, h;
			double hz;

			if (sscanf(l, " %dx%d px, %lf Hz", &w, &h, &hz) == 3) {
				char *label = g_strdup_printf("%d × %d, %.0f Hz%s", w, h, hz,
							      strstr(l, "preferred") ? " (preferred)" : "");

				if (strstr(l, "current"))
					current = (int)modes->len;
				g_ptr_array_add(modes, g_strdup_printf("%dx%d@%fHz", w, h, hz));
				gtk_string_list_append(names, label);
				g_free(label);
			}
		}
	}
	g_strfreev(lines);
	g_free(out);
}

static void display_mapped(GtkWidget *w, gpointer data)
{
	(void)w;
	(void)data;
	outputs_fill();
}

static void build_display(void)
{
	GtkWidget *page = page_new("display", "Display", "video-display-symbolic",
				   "The size of everything on the screen and the screen's mode.");
	GtkWidget *group, *dropdown;
	double scale = current_scale();

	group = group_new(page, NULL, NULL);
	dropdown = gtk_drop_down_new_from_strings(scale_names);
	for (guint i = 0; i < G_N_ELEMENTS(scales); i++)
		if (scales[i] == scale)
			gtk_drop_down_set_selected(GTK_DROP_DOWN(dropdown), i);
	g_signal_connect(dropdown, "notify::selected", G_CALLBACK(scale_selected), NULL);
	row_new(group, "Scale", "Automatic is 200% on screens 2000 pixels high or more. Below "
		"100% gives more room on large screens.", dropdown, NULL);

	outputs_group = group_new(page, "Screens", "In a virtual machine the screen follows the "
				  "window's size; a mode chosen here lasts until the window changes "
				  "or the next login.");
	g_signal_connect(g_object_get_data(G_OBJECT(page), "scroll"), "map",
			 G_CALLBACK(display_mapped), NULL);
}

/* ==== Appearance ==== */

static GtkWidget *wallpaper_sub;

static void wallpaper_apply(const char *line)
{
	char *path = config_path("wallpaper");

	save_file(path, line);
	g_free(path);
	spawn((const char *const[]){ "/usr/lib/worm/wallpaper", NULL });
}

static void wallpaper_describe(void)
{
	char *path = config_path("wallpaper"), *text = NULL, *label;

	if (!g_file_get_contents(path, &text, NULL, NULL))
		text = g_strdup("");
	g_strstrip(text);
	if (g_str_has_prefix(text, "image = "))
		label = g_path_get_basename(text + 8);
	else if (g_str_has_prefix(text, "color = "))
		label = g_strdup_printf("Plain colour %s", text + 8);
	else
		label = g_strdup("The WormOS wallpaper");
	gtk_label_set_text(GTK_LABEL(wallpaper_sub), label);
	g_free(label);
	g_free(text);
	g_free(path);
}

static void wallpaper_default(GtkButton *b, gpointer data)
{
	(void)b;
	(void)data;
	wallpaper_apply("");
	wallpaper_describe();
}

static void wallpaper_color(GtkColorDialogButton *b, GParamSpec *p, gpointer data)
{
	const GdkRGBA *c = gtk_color_dialog_button_get_rgba(b);
	char *line;

	(void)p;
	(void)data;
	if (g_object_get_data(G_OBJECT(b), "filling"))
		return;
	line = g_strdup_printf("color = #%02x%02x%02x\n", (int)(c->red * 255 + 0.5),
			       (int)(c->green * 255 + 0.5), (int)(c->blue * 255 + 0.5));
	wallpaper_apply(line);
	g_free(line);
	wallpaper_describe();
}

static void wallpaper_chosen(GObject *source, GAsyncResult *res, gpointer data)
{
	GFile *file = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(source), res, NULL);
	char *path, *line;

	(void)data;
	if (!file)
		return;
	path = g_file_get_path(file);
	if (path && !strchr(path, '\n')) {
		line = g_strdup_printf("image = %s\n", path);
		wallpaper_apply(line);
		g_free(line);
		wallpaper_describe();
	}
	g_free(path);
	g_object_unref(file);
}

static void wallpaper_pick(GtkButton *b, gpointer data)
{
	GtkFileDialog *dialog = gtk_file_dialog_new();
	GtkFileFilter *filter = gtk_file_filter_new();
	GListStore *filters = g_list_store_new(GTK_TYPE_FILE_FILTER);
	char *pictures = g_build_filename(g_get_home_dir(), "Pictures", NULL);
	GFile *folder = g_file_new_for_path(pictures);

	(void)b;
	(void)data;
	gtk_file_filter_set_name(filter, "Pictures");
	gtk_file_filter_add_mime_type(filter, "image/png");
	gtk_file_filter_add_mime_type(filter, "image/jpeg");
	g_list_store_append(filters, filter);
	gtk_file_dialog_set_filters(dialog, G_LIST_MODEL(filters));
	gtk_file_dialog_set_title(dialog, "Choose a wallpaper");
	if (g_file_test(pictures, G_FILE_TEST_IS_DIR))
		gtk_file_dialog_set_initial_folder(dialog, folder);
	gtk_file_dialog_open(dialog, GTK_WINDOW(window), NULL, wallpaper_chosen, NULL);
	g_object_unref(folder);
	g_free(pictures);
	g_object_unref(filters);
	g_object_unref(filter);
	g_object_unref(dialog);
}

static void panel_edge(GtkDropDown *d, GParamSpec *p, gpointer data)
{
	(void)p;
	(void)data;
	shell_set("panel", "position", gtk_drop_down_get_selected(d) == 1 ? "bottom" : "top");
}

static void build_appearance(void)
{
	GtkWidget *page = page_new("appearance", "Appearance", "preferences-desktop-wallpaper-symbolic",
				   NULL);
	GtkWidget *group, *box, *b, *dropdown;
	GdkRGBA color = { 0.10, 0.08, 0.16, 1 };
	char *edge;

	group = group_new(page, "Wallpaper", NULL);
	box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	gtk_box_append(GTK_BOX(box), button_new("WormOS", G_CALLBACK(wallpaper_default), NULL));
	b = gtk_color_dialog_button_new(gtk_color_dialog_new());
	g_object_set_data(G_OBJECT(b), "filling", GINT_TO_POINTER(1));
	gtk_color_dialog_button_set_rgba(GTK_COLOR_DIALOG_BUTTON(b), &color);
	g_object_set_data(G_OBJECT(b), "filling", NULL);
	gtk_widget_set_tooltip_text(b, "A plain colour");
	g_signal_connect(b, "notify::rgba", G_CALLBACK(wallpaper_color), NULL);
	gtk_box_append(GTK_BOX(box), b);
	gtk_box_append(GTK_BOX(box), button_new("Picture…", G_CALLBACK(wallpaper_pick), NULL));
	row_new(group, "Background", NULL, box, &wallpaper_sub);
	wallpaper_describe();

	group = group_new(page, "Panel", NULL);
	dropdown = gtk_drop_down_new_from_strings((const char *const[]){ "Top", "Bottom", NULL });
	edge = shell_string("panel", "position");
	gtk_drop_down_set_selected(GTK_DROP_DOWN(dropdown), edge && !strcmp(edge, "bottom"));
	g_free(edge);
	g_signal_connect(dropdown, "notify::selected", G_CALLBACK(panel_edge), NULL);
	row_new(group, "Position", "The edge of the screen the panel sits on.", dropdown, NULL);
}

/* ==== Keyboard and mouse: ~/.config/wormos/xdg/labwc/rc.xml ==== */

struct input {
	int repeat_rate, repeat_delay;
	double mouse_speed, touch_speed;
	gboolean mouse_natural, left_handed, tap, touch_natural, dwt;
};

static struct input input = { 40, 250, 0, 0, FALSE, FALSE, TRUE, TRUE, TRUE };

static char *input_path(void)
{
	return g_build_filename(g_get_user_config_dir(), "wormos", "xdg", "labwc", "rc.xml", NULL);
}

/* The values back from the file this program wrote. */
static void input_load(void)
{
	char *path = input_path(), *text = NULL, *p;

	if (g_file_get_contents(path, &text, NULL, NULL)) {
		if ((p = strstr(text, "<repeatRate>")))
			input.repeat_rate = atoi(p + 12);
		if ((p = strstr(text, "<repeatDelay>")))
			input.repeat_delay = atoi(p + 13);
		if ((p = strstr(text, "<!-- mouse -->"))) {
			char *q;

			if ((q = strstr(p, "<pointerSpeed>")))
				input.mouse_speed = g_ascii_strtod(q + 14, NULL);
			if ((q = strstr(p, "<naturalScroll>")))
				input.mouse_natural = !strncmp(q + 15, "yes", 3);
			if ((q = strstr(p, "<leftHanded>")))
				input.left_handed = !strncmp(q + 12, "yes", 3);
		}
		if ((p = strstr(text, "<!-- touchpad -->"))) {
			char *q;

			if ((q = strstr(p, "<pointerSpeed>")))
				input.touch_speed = g_ascii_strtod(q + 14, NULL);
			if ((q = strstr(p, "<naturalScroll>")))
				input.touch_natural = !strncmp(q + 15, "yes", 3);
			if ((q = strstr(p, "<tap>")))
				input.tap = !strncmp(q + 5, "yes", 3);
			if ((q = strstr(p, "<disableWhileTyping>")))
				input.dwt = !strncmp(q + 20, "yes", 3);
		}
	}
	g_free(text);
	g_free(path);
}

static void input_save(void)
{
	char *path = input_path(), ms[G_ASCII_DTOSTR_BUF_SIZE], ts[G_ASCII_DTOSTR_BUF_SIZE];
	char *text;

	g_ascii_formatd(ms, sizeof(ms), "%.2f", input.mouse_speed);
	g_ascii_formatd(ts, sizeof(ts), "%.2f", input.touch_speed);
	text = g_strdup_printf(
		"<?xml version=\"1.0\"?>\n"
		"<!-- Written by Settings (wormos-settings); labwc merges it over\n"
		"     /etc/xdg/labwc/rc.xml. Your own ~/.config/labwc/rc.xml wins. -->\n"
		"<labwc_config>\n"
		"  <keyboard>\n"
		"    <repeatRate>%d</repeatRate>\n"
		"    <repeatDelay>%d</repeatDelay>\n"
		"  </keyboard>\n"
		"  <libinput>\n"
		"    <!-- mouse -->\n"
		"    <device category=\"default\">\n"
		"      <pointerSpeed>%s</pointerSpeed>\n"
		"      <naturalScroll>%s</naturalScroll>\n"
		"      <leftHanded>%s</leftHanded>\n"
		"    </device>\n"
		"    <!-- touchpad -->\n"
		"    <device category=\"touchpad\">\n"
		"      <pointerSpeed>%s</pointerSpeed>\n"
		"      <naturalScroll>%s</naturalScroll>\n"
		"      <leftHanded>%s</leftHanded>\n"
		"      <tap>%s</tap>\n"
		"      <disableWhileTyping>%s</disableWhileTyping>\n"
		"    </device>\n"
		"  </libinput>\n"
		"</labwc_config>\n",
		input.repeat_rate, input.repeat_delay, ms, input.mouse_natural ? "yes" : "no",
		input.left_handed ? "yes" : "no", ts, input.touch_natural ? "yes" : "no",
		input.left_handed ? "yes" : "no", input.tap ? "yes" : "no", input.dwt ? "yes" : "no");
	save_file(path, text);
	g_free(text);
	g_free(path);
	labwc_reconfigure();
}

static guint input_timer;

static gboolean input_save_later(gpointer data)
{
	(void)data;
	input_timer = 0;
	input_save();
	return G_SOURCE_REMOVE;
}

/* Sliders change often while dragged: write once they rest. */
static void input_changed(void)
{
	if (input_timer)
		g_source_remove(input_timer);
	input_timer = g_timeout_add(400, input_save_later, NULL);
}

static void int_slider(GtkRange *r, gpointer data)
{
	*(int *)data = (int)gtk_range_get_value(r);
	input_changed();
}

static void double_slider(GtkRange *r, gpointer data)
{
	*(double *)data = gtk_range_get_value(r);
	input_changed();
}

static void bool_switch(GtkSwitch *s, GParamSpec *p, gpointer data)
{
	(void)p;
	*(gboolean *)data = gtk_switch_get_active(s);
	input_changed();
}

static GtkWidget *slider(double min, double max, double step, double value, GCallback cb,
			 gpointer data)
{
	GtkWidget *s = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, min, max, step);

	gtk_range_set_value(GTK_RANGE(s), value);
	gtk_widget_set_size_request(s, 220, -1);
	g_signal_connect(s, "value-changed", cb, data);
	return s;
}

/* The keyboard layout, XKB_DEFAULT_LAYOUT in ~/.config/labwc/environment. */
static const char *const layouts[][2] = {
	{ "us", "English (US)" }, { "gb", "English (UK)" }, { "de", "German" },
	{ "fr", "French" }, { "es", "Spanish" }, { "it", "Italian" }, { "pt", "Portuguese" },
	{ "br", "Portuguese (Brazil)" }, { "nl", "Dutch" }, { "be", "Belgian" },
	{ "ch", "Swiss" }, { "at", "Austrian" }, { "se", "Swedish" }, { "no", "Norwegian" },
	{ "dk", "Danish" }, { "fi", "Finnish" }, { "pl", "Polish" }, { "cz", "Czech" },
	{ "sk", "Slovak" }, { "hu", "Hungarian" }, { "ro", "Romanian" }, { "gr", "Greek" },
	{ "tr", "Turkish" }, { "ru", "Russian" }, { "ua", "Ukrainian" }, { "il", "Hebrew" },
	{ "ara", "Arabic" }, { "jp", "Japanese" }, { "kr", "Korean" }, { "cn", "Chinese" },
	{ "in", "Indian" }, { "latam", "Spanish (Latin America)" }, { "ca", "Canadian" },
};

static char *env_path(void)
{
	return g_build_filename(g_get_user_config_dir(), "labwc", "environment", NULL);
}

static char *current_layout(void)
{
	char *path = env_path(), *text = NULL, *layout = NULL;

	if (g_file_get_contents(path, &text, NULL, NULL)) {
		char **lines = g_strsplit(text, "\n", -1);

		for (int i = 0; lines[i]; i++)
			if (g_str_has_prefix(lines[i], "XKB_DEFAULT_LAYOUT="))
				layout = g_strdup(g_strstrip(lines[i] + 19));
		g_strfreev(lines);
	}
	g_free(text);
	g_free(path);
	return layout ? layout : g_strdup("us");
}

static void layout_selected(GtkDropDown *d, GParamSpec *p, gpointer data)
{
	guint i = gtk_drop_down_get_selected(d);
	char *path = env_path(), *text = NULL;
	GString *out = g_string_new(NULL);
	gboolean done = FALSE;

	(void)p;
	(void)data;
	if (i >= G_N_ELEMENTS(layouts))
		return;
	if (g_file_get_contents(path, &text, NULL, NULL)) {
		char **lines = g_strsplit(text, "\n", -1);

		for (int l = 0; lines[l]; l++) {
			if (!lines[l + 1] && !*lines[l])
				break;          /* the final newline */
			if (g_str_has_prefix(lines[l], "XKB_DEFAULT_LAYOUT=")) {
				if (done)
					continue;
				g_string_append_printf(out, "XKB_DEFAULT_LAYOUT=%s\n", layouts[i][0]);
				done = TRUE;
			} else {
				g_string_append_printf(out, "%s\n", lines[l]);
			}
		}
		g_strfreev(lines);
	}
	if (!done)
		g_string_append_printf(out, "XKB_DEFAULT_LAYOUT=%s\n", layouts[i][0]);
	save_file(path, out->str);
	g_string_free(out, TRUE);
	g_free(text);
	g_free(path);
	labwc_reconfigure();
}

static void build_keyboard(void)
{
	GtkWidget *page = page_new("keyboard", "Keyboard", "input-keyboard-symbolic", NULL);
	GtkWidget *group, *dropdown;
	GtkStringList *names = gtk_string_list_new(NULL);
	char *layout = current_layout();
	guint selected = GTK_INVALID_LIST_POSITION;

	for (guint i = 0; i < G_N_ELEMENTS(layouts); i++) {
		gtk_string_list_append(names, layouts[i][1]);
		if (!strcmp(layout, layouts[i][0]))
			selected = i;
	}
	group = group_new(page, NULL, NULL);
	dropdown = gtk_drop_down_new(G_LIST_MODEL(names), NULL);
	gtk_drop_down_set_enable_search(GTK_DROP_DOWN(dropdown), TRUE);
	if (selected != GTK_INVALID_LIST_POSITION)
		gtk_drop_down_set_selected(GTK_DROP_DOWN(dropdown), selected);
	g_signal_connect(dropdown, "notify::selected", G_CALLBACK(layout_selected), NULL);
	row_new(group, "Layout", selected == GTK_INVALID_LIST_POSITION ?
		"Set by hand in ~/.config/labwc/environment" : NULL, dropdown, NULL);
	g_free(layout);

	group = group_new(page, "Key repeat", "How soon a held key repeats, and how fast.");
	row_new(group, "Delay (ms)", NULL, slider(150, 1000, 10, input.repeat_delay,
						  G_CALLBACK(int_slider), &input.repeat_delay), NULL);
	row_new(group, "Speed (per second)", NULL, slider(10, 80, 1, input.repeat_rate,
							  G_CALLBACK(int_slider), &input.repeat_rate), NULL);
	group = group_new(page, "Try it", NULL);
	{
		GtkWidget *entry = gtk_entry_new();

		gtk_entry_set_placeholder_text(GTK_ENTRY(entry), "Type here to test the settings");
		gtk_widget_set_hexpand(entry, TRUE);
		row_new(group, "Test", NULL, entry, NULL);
	}
}

static void build_mouse(void)
{
	GtkWidget *page = page_new("mouse", "Mouse & touchpad", "input-mouse-symbolic", NULL);
	GtkWidget *group;

	group = group_new(page, "Mouse", NULL);
	row_new(group, "Pointer speed", NULL, slider(-1, 1, 0.05, input.mouse_speed,
						     G_CALLBACK(double_slider), &input.mouse_speed), NULL);
	row_new(group, "Natural scrolling", "Content moves with the wheel, as on a phone.",
		switch_new(input.mouse_natural, G_CALLBACK(bool_switch), &input.mouse_natural), NULL);
	row_new(group, "Left-handed", "Swap the left and right buttons.",
		switch_new(input.left_handed, G_CALLBACK(bool_switch), &input.left_handed), NULL);

	group = group_new(page, "Touchpad", NULL);
	row_new(group, "Pointer speed", NULL, slider(-1, 1, 0.05, input.touch_speed,
						     G_CALLBACK(double_slider), &input.touch_speed), NULL);
	row_new(group, "Tap to click", NULL,
		switch_new(input.tap, G_CALLBACK(bool_switch), &input.tap), NULL);
	row_new(group, "Natural scrolling", NULL,
		switch_new(input.touch_natural, G_CALLBACK(bool_switch), &input.touch_natural), NULL);
	row_new(group, "Off while typing", NULL,
		switch_new(input.dwt, G_CALLBACK(bool_switch), &input.dwt), NULL);
}

/* ==== Sound (wpctl) ==== */

static GtkWidget *sound_outputs, *sound_inputs;
static gboolean sound_filling;

struct node {
	int id;
	gboolean is_default;
	double volume;
	char *name;
};

static void node_free(gpointer p)
{
	struct node *n = p;

	g_free(n->name);
	g_free(n);
}

/* Sinks and sources of "wpctl status" (Audio part only). */
static void sound_read(GPtrArray *sinks, GPtrArray *sources)
{
	char *out = capture((const char *const[]){ "wpctl", "status", NULL });
	GPtrArray *into = NULL;
	gboolean audio = FALSE;

	if (!out)
		return;
	for (char **lines = g_strsplit(out, "\n", -1), **l = lines; *l || (g_strfreev(lines), 0); l++) {
		char *s = *l;
		struct node *n;
		char *dot, *vol;

		if (!strncmp(s, "Audio", 5))
			audio = TRUE;
		else if (!strncmp(s, "Video", 5) || !strncmp(s, "Settings", 8))
			audio = FALSE;
		if (!audio)
			continue;
		if (strstr(s, "Sinks:")) {
			into = sinks;
			continue;
		}
		if (strstr(s, "Sources:")) {
			into = sources;
			continue;
		}
		if (strstr(s, ":") && !strstr(s, "[vol:")) {
			into = NULL;    /* Devices:, Filters:, Streams: */
			continue;
		}
		if (!into)
			continue;
		/* "│  *   46. Built-in Audio Analog Stereo     [vol: 0.40]" */
		while (*s && !g_ascii_isdigit(*s) && *s != '*')
			s++;
		n = g_new0(struct node, 1);
		if (*s == '*') {
			n->is_default = TRUE;
			s++;
		}
		n->id = atoi(s);
		dot = strchr(s, '.');
		vol = strstr(s, "[vol:");
		if (!n->id || !dot || !vol) {
			node_free(n);
			continue;
		}
		n->volume = g_ascii_strtod(vol + 5, NULL);
		n->name = g_strstrip(g_strndup(dot + 1, vol - dot - 1));
		g_ptr_array_add(into, n);
	}
	g_free(out);
}

static void node_default(GtkCheckButton *b, gpointer data)
{
	char id[16];

	if (sound_filling || !gtk_check_button_get_active(b))
		return;
	snprintf(id, sizeof(id), "%d", GPOINTER_TO_INT(data));
	spawn((const char *const[]){ "wpctl", "set-default", id, NULL });
}

static void node_volume(GtkRange *r, gpointer data)
{
	char id[16], vol[G_ASCII_DTOSTR_BUF_SIZE];

	if (sound_filling)
		return;
	snprintf(id, sizeof(id), "%d", GPOINTER_TO_INT(data));
	g_ascii_formatd(vol, sizeof(vol), "%.2f", gtk_range_get_value(r) / 100);
	spawn((const char *const[]){ "wpctl", "set-volume", id, vol, NULL });
}

static void sound_list(GtkWidget *group, GPtrArray *nodes, const char *none)
{
	GtkWidget *child, *first = NULL;

	while ((child = gtk_widget_get_first_child(group)))
		gtk_list_box_remove(GTK_LIST_BOX(group), child);
	if (!nodes->len) {
		row_new(group, none, NULL, NULL, NULL);
		return;
	}
	for (guint i = 0; i < nodes->len; i++) {
		struct node *n = g_ptr_array_index(nodes, i);
		GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
		GtkWidget *check = gtk_check_button_new();
		GtkWidget *scale = slider(0, 100, 1, n->volume * 100, G_CALLBACK(node_volume),
					  GINT_TO_POINTER(n->id));

		if (first)
			gtk_check_button_set_group(GTK_CHECK_BUTTON(check), GTK_CHECK_BUTTON(first));
		else
			first = check;
		gtk_check_button_set_active(GTK_CHECK_BUTTON(check), n->is_default);
		gtk_widget_set_tooltip_text(check, "Use this one");
		g_signal_connect(check, "toggled", G_CALLBACK(node_default), GINT_TO_POINTER(n->id));
		gtk_box_append(GTK_BOX(box), scale);
		gtk_box_append(GTK_BOX(box), check);
		row_new(group, n->name, NULL, box, NULL);
	}
}

static void sound_fill(void)
{
	GPtrArray *sinks = g_ptr_array_new_with_free_func(node_free);
	GPtrArray *sources = g_ptr_array_new_with_free_func(node_free);

	sound_filling = TRUE;
	sound_read(sinks, sources);
	sound_list(sound_outputs, sinks, "No sound output (no sound device or sound server)");
	sound_list(sound_inputs, sources, "No microphone");
	sound_filling = FALSE;
	g_ptr_array_free(sinks, TRUE);
	g_ptr_array_free(sources, TRUE);
}

static void sound_mapped(GtkWidget *w, gpointer data)
{
	(void)w;
	(void)data;
	sound_fill();
}

static void build_sound(void)
{
	GtkWidget *page = page_new("sound", "Sound", "audio-speakers-symbolic", NULL);

	sound_outputs = group_new(page, "Output", "The ticked one plays the sound; each has its "
				  "own volume.");
	sound_inputs = group_new(page, "Input", NULL);
	g_signal_connect(g_object_get_data(G_OBJECT(page), "scroll"), "map",
			 G_CALLBACK(sound_mapped), NULL);
}

/* ==== Notifications ==== */

static void shell_switch(GtkSwitch *s, GParamSpec *p, gpointer data)
{
	const char *const *where = data;

	(void)p;
	shell_set(where[0], where[1], gtk_switch_get_active(s) ? "true" : "false");
}

static const char *const dnd_key[] = { "notifications", "do-not-disturb" };
static const char *const clock_keys[][2] = {
	{ "panel", "clock-12h" }, { "panel", "clock-seconds" }, { "panel", "clock-weekday" },
	{ "panel", "clock-date" },
};

static void build_notifications(void)
{
	GtkWidget *page = page_new("notifications", "Notifications",
				   "preferences-system-notifications-symbolic", NULL);
	GtkWidget *group = group_new(page, NULL, NULL);

	row_new(group, "Do not disturb", "Only urgent notifications show. Also in the panel's "
		"quick settings.", switch_new(shell_bool("notifications", "do-not-disturb", FALSE),
					      G_CALLBACK(shell_switch), (gpointer)dnd_key), NULL);
}

/* ==== Date and time ==== */

static GtkWidget *zone_sub, *zone_list, *zone_search;

static char *current_zone(void)
{
	char *target = g_file_read_link("/etc/localtime", NULL), *z;

	if (!target)
		return g_strdup("UTC");
	z = strstr(target, "zoneinfo/") ? g_strdup(strstr(target, "zoneinfo/") + 9) : g_strdup("UTC");
	g_free(target);
	return z;
}

static void zone_describe(void)
{
	char *zone = current_zone();
	GDateTime *now = g_date_time_new_now_local();
	char *time = g_date_time_format(now, "%H:%M, %A %e %B %Y");
	char *text = g_strdup_printf("%s · %s", zone, time);

	gtk_label_set_text(GTK_LABEL(zone_sub), text);
	g_free(text);
	g_free(time);
	g_date_time_unref(now);
	g_free(zone);
}

static void zone_done(gboolean ok, gpointer data)
{
	(void)data;
	if (ok) {
		/* This program keeps the zone it started with; the label says. */
		char *zone = current_zone(), *text = g_strdup_printf("%s (new programs use it; "
								     "log out for the clock)", zone);

		gtk_label_set_text(GTK_LABEL(zone_sub), text);
		g_free(text);
		g_free(zone);
	}
}

static void zone_activated(GtkListBox *box, GtkListBoxRow *row, gpointer data)
{
	const char *zone = g_object_get_data(G_OBJECT(row), "zone");
	char *what = g_strdup_printf("Set the time zone to %s", zone);

	(void)box;
	(void)data;
	admin(what, TRUE, zone_done, NULL, SETTINGS_HELPER, "zone", zone, NULL);
	g_free(what);
}

static gboolean zone_filter(GtkListBoxRow *row, gpointer data)
{
	const char *text = gtk_editable_get_text(GTK_EDITABLE(zone_search));
	const char *zone = g_object_get_data(G_OBJECT(row), "zone");
	char *a, *b;
	gboolean match;

	(void)data;
	if (!*text)
		return TRUE;
	a = g_utf8_casefold(zone, -1);
	b = g_utf8_casefold(text, -1);
	g_strdelimit(b, " ", '_');
	match = strstr(a, b) != NULL;
	g_free(a);
	g_free(b);
	return match;
}

static void zone_search_changed(GtkSearchEntry *e, gpointer data)
{
	(void)e;
	(void)data;
	gtk_list_box_invalidate_filter(GTK_LIST_BOX(zone_list));
}

static int by_string(gconstpointer a, gconstpointer b)
{
	return strcmp(*(char *const *)a, *(char *const *)b);
}

static void build_time(void)
{
	GtkWidget *page = page_new("time", "Date & time", "preferences-system-time-symbolic", NULL);
	GtkWidget *group, *scroll;
	char *text = NULL;
	GPtrArray *zones = g_ptr_array_new_with_free_func(g_free);

	group = group_new(page, "Time zone", "The clock is checked over the internet every "
			  "six hours (worm time). Pick a zone to change it.");
	row_new(group, "Current", NULL, NULL, &zone_sub);
	zone_describe();
	zone_search = gtk_search_entry_new();
	gtk_search_entry_set_placeholder_text(GTK_SEARCH_ENTRY(zone_search),
					      "Search a city or region, e.g. Berlin");
	g_signal_connect(zone_search, "search-changed", G_CALLBACK(zone_search_changed), NULL);
	gtk_box_append(GTK_BOX(gtk_widget_get_parent(group)), zone_search);
	zone_list = gtk_list_box_new();
	gtk_widget_add_css_class(zone_list, "settings-group");
	gtk_list_box_set_filter_func(GTK_LIST_BOX(zone_list), zone_filter, NULL, NULL);
	g_signal_connect(zone_list, "row-activated", G_CALLBACK(zone_activated), NULL);
	if (g_file_get_contents("/usr/share/zoneinfo/zone1970.tab", &text, NULL, NULL)) {
		char **lines = g_strsplit(text, "\n", -1);

		for (int i = 0; lines[i]; i++) {
			char **f = g_strsplit(lines[i], "\t", 4);

			if (lines[i][0] != '#' && g_strv_length(f) >= 3)
				g_ptr_array_add(zones, g_strdup(f[2]));
			g_strfreev(f);
		}
		g_strfreev(lines);
		g_free(text);
	}
	g_ptr_array_add(zones, g_strdup("UTC"));
	g_ptr_array_sort(zones, by_string);
	for (guint i = 0; i < zones->len; i++) {
		GtkWidget *row = gtk_list_box_row_new(), *label;
		char *shown = g_strdup(g_ptr_array_index(zones, i));

		g_strdelimit(shown, "_", ' ');
		label = gtk_label_new(shown);
		gtk_label_set_xalign(GTK_LABEL(label), 0);
		gtk_widget_add_css_class(label, "zone");
		gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), label);
		g_object_set_data_full(G_OBJECT(row), "zone", g_strdup(g_ptr_array_index(zones, i)),
				       g_free);
		gtk_list_box_append(GTK_LIST_BOX(zone_list), row);
		g_free(shown);
	}
	g_ptr_array_free(zones, TRUE);
	scroll = gtk_scrolled_window_new();
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER,
				       GTK_POLICY_AUTOMATIC);
	gtk_widget_set_size_request(scroll, -1, 200);
	gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), zone_list);
	gtk_box_append(GTK_BOX(gtk_widget_get_parent(group)), scroll);

	group = group_new(page, "Clock in the panel", NULL);
	row_new(group, "12-hour clock", "3:45 PM instead of 15:45",
		switch_new(shell_bool("panel", "clock-12h", FALSE), G_CALLBACK(shell_switch),
			   (gpointer)clock_keys[0]), NULL);
	row_new(group, "Seconds", NULL, switch_new(shell_bool("panel", "clock-seconds", FALSE),
						   G_CALLBACK(shell_switch), (gpointer)clock_keys[1]), NULL);
	row_new(group, "Weekday", NULL, switch_new(shell_bool("panel", "clock-weekday", TRUE),
						   G_CALLBACK(shell_switch), (gpointer)clock_keys[2]), NULL);
	row_new(group, "Date", NULL, switch_new(shell_bool("panel", "clock-date", TRUE),
						G_CALLBACK(shell_switch), (gpointer)clock_keys[3]), NULL);
}

/* ==== Network and privacy ==== */

static GtkWidget *net_group, *fw_label, *anon_switch, *auto_switch;
static gboolean privacy_filling;

static void net_fill(void)
{
	struct ifaddrs *list = NULL;
	GtkWidget *child;
	GHashTable *seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	struct if_nameindex *names = if_nameindex();

	while ((child = gtk_widget_get_first_child(net_group)))
		gtk_list_box_remove(GTK_LIST_BOX(net_group), child);
	getifaddrs(&list);
	for (struct if_nameindex *n = names; n && n->if_index; n++) {
		GString *addr = g_string_new(NULL);
		char path[128], mac[32] = "", *carrier = NULL;

		if (!strcmp(n->if_name, "lo"))
			continue;
		for (struct ifaddrs *a = list; a; a = a->ifa_next) {
			char ip[INET6_ADDRSTRLEN];

			if (!a->ifa_addr || strcmp(a->ifa_name, n->if_name))
				continue;
			if (a->ifa_addr->sa_family == AF_INET)
				inet_ntop(AF_INET, &((struct sockaddr_in *)a->ifa_addr)->sin_addr, ip,
					  sizeof(ip));
			else if (a->ifa_addr->sa_family == AF_INET6)
				inet_ntop(AF_INET6, &((struct sockaddr_in6 *)a->ifa_addr)->sin6_addr, ip,
					  sizeof(ip));
			else
				continue;
			g_string_append_printf(addr, "%s%s", addr->len ? "\n" : "", ip);
		}
		snprintf(path, sizeof(path), "/sys/class/net/%s/carrier", n->if_name);
		g_file_get_contents(path, &carrier, NULL, NULL);
		snprintf(path, sizeof(path), "/sys/class/net/%s/address", n->if_name);
		{
			char *m = NULL;

			if (g_file_get_contents(path, &m, NULL, NULL))
				g_strlcpy(mac, g_strstrip(m), sizeof(mac));
			g_free(m);
		}
		{
			char *title = g_strdup_printf("%s · %s", n->if_name, carrier && carrier[0] == '1' ?
						      "connected" : "not connected");
			char *sub = g_strdup_printf("%s%sHardware address %s", addr->str,
						    addr->len ? "\n" : "", mac);

			row_new(net_group, title, sub, NULL, NULL);
			g_free(title);
			g_free(sub);
		}
		g_free(carrier);
		g_string_free(addr, TRUE);
	}
	if (!gtk_widget_get_first_child(net_group))
		row_new(net_group, "No network device", NULL, NULL, NULL);
	if (list)
		freeifaddrs(list);
	if (names)
		if_freenameindex(names);
	g_hash_table_unref(seen);
}

static gboolean snap_auto_on(void)
{
	char *text = NULL;
	gboolean on = TRUE;

	if (g_file_get_contents("/run/snap-status", &text, NULL, NULL))
		on = strstr(text, "auto = off") == NULL;
	g_free(text);
	return on;
}

static void fw_finished(GObject *source, GAsyncResult *res, gpointer data)
{
	char *out = NULL;

	(void)data;
	if (g_subprocess_communicate_utf8_finish(G_SUBPROCESS(source), res, &out, NULL, NULL) &&
	    out && *out && g_subprocess_get_exit_status(G_SUBPROCESS(source)) == 0)
		gtk_label_set_text(GTK_LABEL(fw_label), g_strstrip(out));
	else
		gtk_label_set_text(GTK_LABEL(fw_label), "Only administrators can see the rules. "
				   "Incoming connections are blocked unless opened with "
				   "worm fw allow PORT.");
	g_free(out);
	g_object_unref(source);
}

static void privacy_fill(void)
{
	GSubprocess *proc;

	privacy_filling = TRUE;
	gtk_switch_set_active(GTK_SWITCH(anon_switch), g_file_test("/etc/worm/anon",
								  G_FILE_TEST_EXISTS));
	gtk_switch_set_active(GTK_SWITCH(auto_switch), snap_auto_on());
	privacy_filling = FALSE;
	net_fill();
	proc = g_subprocess_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE,
				NULL, SETTINGS_HELPER, "firewall", NULL);
	if (proc)
		g_subprocess_communicate_utf8_async(proc, NULL, NULL, fw_finished, NULL);
}

static void privacy_done(gboolean ok, gpointer data)
{
	(void)ok;
	(void)data;
	privacy_fill();
}

static void anon_changed(GtkSwitch *s, GParamSpec *p, gpointer data)
{
	gboolean on = gtk_switch_get_active(s);

	(void)p;
	(void)data;
	if (privacy_filling)
		return;
	admin(on ? "Send all traffic through Tor" : "Turn anonymous mode off", TRUE, privacy_done,
	      NULL, SETTINGS_HELPER, "anon", on ? "on" : "off", NULL);
}

static void auto_changed(GtkSwitch *s, GParamSpec *p, gpointer data)
{
	gboolean on = gtk_switch_get_active(s);

	(void)p;
	(void)data;
	if (privacy_filling)
		return;
	admin(on ? "Turn automatic snapshots on" : "Turn automatic snapshots off", TRUE,
	      privacy_done, NULL, SNAP_HELPER, "auto", on ? "on" : "off", NULL);
}

static void privacy_mapped(GtkWidget *w, gpointer data)
{
	(void)w;
	(void)data;
	privacy_fill();
}

static void build_privacy(void)
{
	GtkWidget *page = page_new("privacy", "Network & privacy", "network-wired-symbolic", NULL);
	GtkWidget *group;

	net_group = group_new(page, "Network", NULL);
	group = group_new(page, "Protection", NULL);
	anon_switch = switch_new(FALSE, G_CALLBACK(anon_changed), NULL);
	row_new(group, "Anonymous mode", "All traffic goes through Tor; what cannot is blocked.",
		anon_switch, NULL);
	auto_switch = switch_new(TRUE, G_CALLBACK(auto_changed), NULL);
	row_new(group, "Automatic snapshots", "A snapshot before every package change, so "
		"wh undo can go back.", auto_switch, NULL);
	group = group_new(page, "Firewall", NULL);
	fw_label = gtk_label_new("…");
	gtk_label_set_xalign(GTK_LABEL(fw_label), 0);
	gtk_label_set_wrap(GTK_LABEL(fw_label), TRUE);
	gtk_label_set_selectable(GTK_LABEL(fw_label), TRUE);
	gtk_widget_add_css_class(fw_label, "monospace");
	gtk_widget_add_css_class(fw_label, "settings-text");
	gtk_list_box_append(GTK_LIST_BOX(group), fw_label);
	g_signal_connect(g_object_get_data(G_OBJECT(page), "scroll"), "map",
			 G_CALLBACK(privacy_mapped), NULL);
}

/* ==== Virtual machine ==== */

static GtkWidget *vm_info, *vm_settings, *vm_tips;
static gboolean vm_filling;

static const struct {
	const char *key, *title, *subtitle;
	const char *const values[10];
	const char *const labels[10];
} vm_keys[] = {
	{ "renderer", "Drawing", "OpenGL ES needs a GPU with 3D; software works everywhere.",
	  { "auto", "gles", "software" }, { "Automatic", "OpenGL ES", "Software" } },
	{ "cursor", "Pointer", "Software draws it into the picture, for viewers that lose it.",
	  { "auto", "software", "hardware" }, { "Automatic", "Software", "Hardware" } },
	{ "scale", "Default display scale", "For users who have not chosen one.",
	  { "auto", "0.75", "0.8", "0.9", "1", "1.25", "1.5", "1.75", "2" },
	  { "Automatic", "75%", "80%", "90%", "100%", "125%", "150%", "175%", "200%" } },
	{ "clipboard", "SPICE agent", "Shared clipboard, pointer and window size.",
	  { "on", "off" }, { "On", "Off" } },
	{ "agent", "Guest agent", "The host sees the IP address, shuts down cleanly, sets the clock.",
	  { "on", "off" }, { "On", "Off" } },
	{ "shares", "Shared folders", "The host's shared folders under /media.",
	  { "on", "off" }, { "On", "Off" } },
	{ "tuning", "Tuning", "Disk and memory settings for virtual disks.",
	  { "on", "off" }, { "On", "Off" } },
};

static void vm_fill(void);

static void vm_done(gboolean ok, gpointer data)
{
	(void)ok;
	(void)data;
	vm_fill();
}

static void vm_selected(GtkDropDown *d, GParamSpec *p, gpointer data)
{
	guint k = GPOINTER_TO_UINT(data), i = gtk_drop_down_get_selected(d);
	char *what;

	(void)p;
	if (vm_filling || i >= G_N_ELEMENTS(vm_keys[k].values) || !vm_keys[k].values[i])
		return;
	what = g_strdup_printf("Virtual machine: %s → %s", vm_keys[k].title, vm_keys[k].labels[i]);
	admin(what, TRUE, vm_done, NULL, SETTINGS_HELPER, "vm", vm_keys[k].key,
	      vm_keys[k].values[i], NULL);
	g_free(what);
}

static void vm_fill(void)
{
	static const char *const info[][2] = {
		{ "hypervisor", "Hypervisor" }, { "accel", "Acceleration" }, { "product", "Machine" },
		{ "gpu", "Graphics" }, { "gl", "3D acceleration" }, { "spice", "SPICE channel" },
		{ "agent-port", "Guest agent channel" }, { "balloon", "Memory balloon" },
		{ "share-tags", "Shared folders" },
	};
	char *hv = vm_value("hypervisor"), *out, *tips;
	GtkWidget *child;

	vm_filling = TRUE;
	while ((child = gtk_widget_get_first_child(vm_info)))
		gtk_list_box_remove(GTK_LIST_BOX(vm_info), child);
	while ((child = gtk_widget_get_first_child(vm_settings)))
		gtk_list_box_remove(GTK_LIST_BOX(vm_settings), child);
	if (!hv || !strcmp(hv, "none")) {
		row_new(vm_info, hv ? "Not a virtual machine" : "Unknown",
			hv ? "WormOS runs on real hardware here." :
			"/run/worm/vm is missing (the 13-vm service did not run).", NULL, NULL);
		gtk_widget_set_visible(gtk_widget_get_parent(vm_settings), FALSE);
		gtk_widget_set_visible(vm_tips, FALSE);
		g_free(hv);
		vm_filling = FALSE;
		return;
	}
	gtk_widget_set_visible(gtk_widget_get_parent(vm_settings), TRUE);
	for (size_t i = 0; i < G_N_ELEMENTS(info); i++) {
		char *v = vm_value(info[i][0]);

		if (v && *v)
			row_new(vm_info, info[i][1], NULL, gtk_label_new(v), NULL);
		g_free(v);
	}
	for (size_t k = 0; k < G_N_ELEMENTS(vm_keys); k++) {
		GtkStringList *labels = gtk_string_list_new(NULL);
		GtkWidget *dropdown;
		char *v = vm_value(vm_keys[k].key), *conf = NULL;
		guint selected = 0;

		/* The value set (auto stays auto), from worm vm's own view. */
		for (size_t i = 0; vm_keys[k].values[i]; i++) {
			gtk_string_list_append(labels, vm_keys[k].labels[i]);
			if (v && !strcmp(v, vm_keys[k].values[i]))
				selected = (guint)i;
		}
		if (g_file_get_contents("/etc/worm/vm.conf", &conf, NULL, NULL)) {
			char *needle = g_strdup_printf("\n%s = ", vm_keys[k].key), *full;

			full = g_strconcat("\n", conf, NULL);
			if (!strstr(full, needle) && !strcmp(vm_keys[k].values[0], "auto"))
				selected = 0;
			g_free(full);
			g_free(needle);
		} else if (!strcmp(vm_keys[k].values[0], "auto")) {
			selected = 0;
		}
		g_free(conf);
		dropdown = gtk_drop_down_new(G_LIST_MODEL(labels), NULL);
		gtk_drop_down_set_selected(GTK_DROP_DOWN(dropdown), selected);
		g_signal_connect(dropdown, "notify::selected", G_CALLBACK(vm_selected),
				 GUINT_TO_POINTER(k));
		row_new(vm_settings, vm_keys[k].title, vm_keys[k].subtitle, dropdown, NULL);
		g_free(v);
	}
	/* The tips of worm vm, for the host's side. */
	out = capture((const char *const[]){ "worm", "vm", NULL });
	tips = out ? strstr(out, "Tips for the host:\n") : NULL;
	if (tips) {
		GString *t = g_string_new(NULL);

		for (char **lines = g_strsplit(tips + 19, "\n", -1), **l = lines;
		     *l || (g_strfreev(lines), 0); l++) {
			char *s = g_strstrip(*l);

			if (g_str_has_prefix(s, "- "))
				g_string_append_printf(t, "%s• %s", t->len ? "\n" : "", s + 2);
			else if (*s && t->len)
				g_string_append_printf(t, " %s", s);
		}
		gtk_label_set_text(GTK_LABEL(vm_tips), t->str);
		g_string_free(t, TRUE);
	}
	gtk_widget_set_visible(vm_tips, tips != NULL);
	g_free(out);
	g_free(hv);
	vm_filling = FALSE;
}

static void vm_mapped(GtkWidget *w, gpointer data)
{
	(void)w;
	(void)data;
	vm_fill();
}

static void build_vm(void)
{
	GtkWidget *page = page_new("vm", "Virtual machine", "computer-symbolic",
				   "WormOS sets itself up for the hypervisor it finds at every "
				   "start (worm vm). Change the settings here or with "
				   "worm vm set KEY VALUE.");

	vm_info = group_new(page, "Found", NULL);
	vm_settings = group_new(page, "Settings", "Drawing, pointer, scale and SPICE agent apply "
				"from the next login.");
	vm_tips = gtk_label_new("");
	gtk_label_set_xalign(GTK_LABEL(vm_tips), 0);
	gtk_label_set_wrap(GTK_LABEL(vm_tips), TRUE);
	gtk_label_set_selectable(GTK_LABEL(vm_tips), TRUE);
	gtk_widget_add_css_class(vm_tips, "tips");
	gtk_box_append(GTK_BOX(page), vm_tips);
	g_signal_connect(g_object_get_data(G_OBJECT(page), "scroll"), "map",
			 G_CALLBACK(vm_mapped), NULL);
}

/* ==== About ==== */

static GtkWidget *host_sub;

static char *os_release(const char *key)
{
	char *text = NULL, *value = NULL;

	if (!g_file_get_contents("/etc/os-release", &text, NULL, NULL))
		return g_strdup("");
	for (char **lines = g_strsplit(text, "\n", -1), **l = lines; *l || (g_strfreev(lines), 0); l++) {
		size_t n = strlen(key);

		if (!value && !strncmp(*l, key, n) && (*l)[n] == '=') {
			char *v = *l + n + 1;

			if (*v == '"')
				v++;
			value = g_strdup(v);
			if (*value && value[strlen(value) - 1] == '"')
				value[strlen(value) - 1] = '\0';
		}
	}
	g_free(text);
	return value ? value : g_strdup("");
}

static char *cpu_model(void)
{
	char *text = NULL, *model = NULL;
	int count = 0;

	if (g_file_get_contents("/proc/cpuinfo", &text, NULL, NULL)) {
		for (char **lines = g_strsplit(text, "\n", -1), **l = lines;
		     *l || (g_strfreev(lines), 0); l++) {
			if (g_str_has_prefix(*l, "model name")) {
				count++;
				if (!model && strchr(*l, ':'))
					model = g_strdup(g_strstrip(strchr(*l, ':') + 1));
			}
		}
	}
	g_free(text);
	if (!model)
		model = g_strdup("Unknown");
	{
		char *full = g_strdup_printf("%s × %d", model, count ? count : 1);

		g_free(model);
		return full;
	}
}

static char *memory(void)
{
	char *text = NULL, *result;
	long long total = 0, avail = 0;

	if (g_file_get_contents("/proc/meminfo", &text, NULL, NULL)) {
		char *p;

		if ((p = strstr(text, "MemTotal:")))
			total = atoll(p + 9);
		if ((p = strstr(text, "MemAvailable:")))
			avail = atoll(p + 13);
	}
	g_free(text);
	result = g_strdup_printf("%.1f GB, %.1f GB in use", total / 1048576.0,
				 (total - avail) / 1048576.0);
	return result;
}

static char *disk(const char *path)
{
	struct statvfs st;

	if (statvfs(path, &st) < 0)
		return g_strdup("Unknown");
	return g_strdup_printf("%.1f GB free of %.1f GB",
			       (double)st.f_bavail * st.f_frsize / 1e9,
			       (double)st.f_blocks * st.f_frsize / 1e9);
}

static void host_done(gboolean ok, gpointer data)
{
	char name[256] = "";

	(void)ok;
	(void)data;
	gethostname(name, sizeof(name) - 1);
	gtk_label_set_text(GTK_LABEL(host_sub), name);
}

static void host_rename(GtkEntry *entry, gpointer data)
{
	const char *name = gtk_editable_get_text(GTK_EDITABLE(entry));
	char *what;

	(void)data;
	if (!*name)
		return;
	what = g_strdup_printf("Name this computer %s", name);
	admin(what, TRUE, host_done, NULL, SETTINGS_HELPER, "hostname", name, NULL);
	g_free(what);
}

/* Changing one's own password: /usr/lib/worm/passwd reads the current one
 * and the new one. */
struct pw {
	GtkWidget *dialog, *current, *new1, *new2, *message;
};

static void pw_finished(GObject *source, GAsyncResult *res, gpointer data)
{
	struct pw *pw = data;
	char *err = NULL;

	g_subprocess_communicate_utf8_finish(G_SUBPROCESS(source), res, NULL, &err, NULL);
	if (g_subprocess_get_if_exited(G_SUBPROCESS(source)) &&
	    g_subprocess_get_exit_status(G_SUBPROCESS(source)) == 0) {
		gtk_window_destroy(GTK_WINDOW(pw->dialog));
		g_free(pw);
	} else {
		gtk_widget_set_sensitive(pw->dialog, TRUE);
		gtk_label_set_text(GTK_LABEL(pw->message), err && *err ? g_strstrip(err) :
				   "The password was not changed.");
	}
	g_free(err);
	g_object_unref(source);
}

static void pw_ok(GtkWidget *w, gpointer data)
{
	struct pw *pw = data;
	const char *a = gtk_editable_get_text(GTK_EDITABLE(pw->new1));
	const char *b = gtk_editable_get_text(GTK_EDITABLE(pw->new2));
	GSubprocess *proc;
	char *input;

	(void)w;
	if (strcmp(a, b)) {
		gtk_label_set_text(GTK_LABEL(pw->message), "The new passwords differ.");
		return;
	}
	if (!*a) {
		gtk_label_set_text(GTK_LABEL(pw->message), "The new password is empty.");
		return;
	}
	proc = g_subprocess_new(G_SUBPROCESS_FLAGS_STDIN_PIPE | G_SUBPROCESS_FLAGS_STDOUT_SILENCE |
				G_SUBPROCESS_FLAGS_STDERR_PIPE, NULL, PASSWD_HELPER, NULL);
	if (!proc) {
		gtk_label_set_text(GTK_LABEL(pw->message), "Cannot run " PASSWD_HELPER);
		return;
	}
	gtk_widget_set_sensitive(pw->dialog, FALSE);
	input = g_strdup_printf("%s\n%s\n", gtk_editable_get_text(GTK_EDITABLE(pw->current)), a);
	g_subprocess_communicate_utf8_async(proc, input, NULL, pw_finished, pw);
	explicit_bzero(input, strlen(input));
	g_free(input);
}

static void pw_cancel(GtkWidget *w, gpointer data)
{
	struct pw *pw = data;

	(void)w;
	gtk_window_destroy(GTK_WINDOW(pw->dialog));
	g_free(pw);
}

static GtkWidget *pw_field(GtkWidget *box, const char *label)
{
	GtkWidget *l = gtk_label_new(label), *e = gtk_password_entry_new();

	gtk_label_set_xalign(GTK_LABEL(l), 0);
	gtk_box_append(GTK_BOX(box), l);
	gtk_password_entry_set_show_peek_icon(GTK_PASSWORD_ENTRY(e), TRUE);
	gtk_box_append(GTK_BOX(box), e);
	return e;
}

static void password_dialog(GtkButton *b, gpointer data)
{
	struct pw *pw = g_new0(struct pw, 1);
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8), *buttons, *ok;

	(void)b;
	(void)data;
	pw->dialog = gtk_window_new();
	gtk_window_set_transient_for(GTK_WINDOW(pw->dialog), GTK_WINDOW(window));
	gtk_window_set_modal(GTK_WINDOW(pw->dialog), TRUE);
	gtk_window_set_title(GTK_WINDOW(pw->dialog), "Change your password");
	gtk_window_set_resizable(GTK_WINDOW(pw->dialog), FALSE);
	gtk_widget_add_css_class(box, "dialog-box");
	pw->current = pw_field(box, "Current password");
	pw->new1 = pw_field(box, "New password");
	pw->new2 = pw_field(box, "New password again");
	g_signal_connect(pw->new2, "activate", G_CALLBACK(pw_ok), pw);
	pw->message = gtk_label_new("");
	gtk_widget_add_css_class(pw->message, "error");
	gtk_label_set_wrap(GTK_LABEL(pw->message), TRUE);
	gtk_box_append(GTK_BOX(box), pw->message);
	buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	gtk_widget_set_halign(buttons, GTK_ALIGN_END);
	gtk_box_append(GTK_BOX(buttons), button_new("Cancel", G_CALLBACK(pw_cancel), pw));
	ok = button_new("Change", G_CALLBACK(pw_ok), pw);
	gtk_widget_add_css_class(ok, "suggested-action");
	gtk_box_append(GTK_BOX(buttons), ok);
	gtk_box_append(GTK_BOX(box), buttons);
	gtk_window_set_child(GTK_WINDOW(pw->dialog), box);
	gtk_window_present(GTK_WINDOW(pw->dialog));
}

static void build_about(void)
{
	GtkWidget *page = page_new("about", "About", "help-about-symbolic", NULL);
	GtkWidget *group, *logo, *name, *entry;
	struct utsname u;
	char *pretty = os_release("PRETTY_NAME"), *variant = os_release("VARIANT");
	char *cpu = cpu_model(), *mem = memory(), *sys = disk("/"), *home = disk(g_get_home_dir());
	char host[256] = "";
	const char *real = g_get_real_name();

	uname(&u);
	gethostname(host, sizeof(host) - 1);
	logo = gtk_image_new_from_icon_name("org.wormos.Settings.About");
	gtk_image_set_pixel_size(GTK_IMAGE(logo), 96);
	gtk_box_append(GTK_BOX(page), logo);
	name = gtk_label_new(pretty);
	gtk_widget_add_css_class(name, "title-2");
	gtk_box_append(GTK_BOX(page), name);

	group = group_new(page, "System", NULL);
	if (*variant)
		row_new(group, "Edition", variant, NULL, NULL);
	row_new(group, "Kernel", u.release, NULL, NULL);
	row_new(group, "Processor", cpu, NULL, NULL);
	row_new(group, "Memory", mem, NULL, NULL);
	row_new(group, "System disk", sys, NULL, NULL);
	row_new(group, "Home folder", home, NULL, NULL);
	entry = gtk_entry_new();
	gtk_editable_set_text(GTK_EDITABLE(entry), host);
	gtk_entry_set_max_length(GTK_ENTRY(entry), 63);
	gtk_widget_set_tooltip_text(entry, "Type a new name and press Enter");
	g_signal_connect(entry, "activate", G_CALLBACK(host_rename), NULL);
	row_new(group, "Computer name", host, entry, &host_sub);

	group = group_new(page, "You", NULL);
	row_new(group, real && strcmp(real, "Unknown") ? real : g_get_user_name(), g_get_user_name(),
		button_new("Change password…", G_CALLBACK(password_dialog), NULL), NULL);

	g_free(pretty);
	g_free(variant);
	g_free(cpu);
	g_free(mem);
	g_free(sys);
	g_free(home);
}

/* ==== the window ==== */

static void sidebar_selected(GtkListBox *box, GtkListBoxRow *row, gpointer data)
{
	(void)box;
	(void)data;
	if (row)
		gtk_stack_set_visible_child_name(GTK_STACK(stack),
						 g_object_get_data(G_OBJECT(row), "page"));
}

static void show_page(const char *name)
{
	for (GtkWidget *r = gtk_widget_get_first_child(sidebar); r; r = gtk_widget_get_next_sibling(r)) {
		const char *p = g_object_get_data(G_OBJECT(r), "page");

		if (p && !strcmp(p, name))
			gtk_list_box_select_row(GTK_LIST_BOX(sidebar), GTK_LIST_BOX_ROW(r));
	}
}

static void build(GtkApplication *app)
{
	GtkWidget *paned = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0), *side;
	GtkCssProvider *css = gtk_css_provider_new();
	char *vm = vm_value("hypervisor");

	gtk_css_provider_load_from_path(css, CSS_PATH);
	gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(css),
						   GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
	g_object_unref(css);

	window = gtk_application_window_new(app);
	gtk_window_set_title(GTK_WINDOW(window), "Settings");
	gtk_window_set_icon_name(GTK_WINDOW(window), APP_ID);
	gtk_window_set_default_size(GTK_WINDOW(window), 920, 640);

	side = gtk_scrolled_window_new();
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(side), GTK_POLICY_NEVER,
				       GTK_POLICY_AUTOMATIC);
	gtk_widget_add_css_class(side, "sidebar-pane");
	sidebar = gtk_list_box_new();
	gtk_widget_add_css_class(sidebar, "navigation-sidebar");
	g_signal_connect(sidebar, "row-selected", G_CALLBACK(sidebar_selected), NULL);
	gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(side), sidebar);
	gtk_widget_set_size_request(side, 220, -1);
	gtk_box_append(GTK_BOX(paned), side);

	stack = gtk_stack_new();
	gtk_stack_set_transition_type(GTK_STACK(stack), GTK_STACK_TRANSITION_TYPE_CROSSFADE);
	gtk_widget_set_hexpand(stack, TRUE);
	gtk_box_append(GTK_BOX(paned), stack);

	input_load();
	build_display();
	build_appearance();
	build_keyboard();
	build_mouse();
	build_sound();
	build_notifications();
	build_time();
	build_privacy();
	/* Real hardware has nothing to show there. */
	if (vm && strcmp(vm, "none"))
		build_vm();
	build_about();
	g_free(vm);

	gtk_window_set_child(GTK_WINDOW(window), paned);
	show_page("display");
}

static int command_line(GApplication *gapp, GApplicationCommandLine *cmd, gpointer data)
{
	GVariantDict *options = g_application_command_line_get_options_dict(cmd);
	const char *page = NULL;

	(void)data;
	if (!window)
		build(GTK_APPLICATION(gapp));
	if (g_variant_dict_lookup(options, "page", "&s", &page))
		show_page(page);
	gtk_window_present(GTK_WINDOW(window));
	return 0;
}

int main(int argc, char **argv)
{
	static const GOptionEntry entries[] = {
		{ "page", 0, 0, G_OPTION_ARG_STRING, NULL,
		  "Open a page: display, appearance, keyboard, mouse, sound, notifications, "
		  "time, privacy, vm, about", "NAME" },
		{ NULL, 0, 0, 0, NULL, NULL, NULL },
	};
	GtkApplication *app = gtk_application_new(APP_ID, G_APPLICATION_HANDLES_COMMAND_LINE);
	int status;

	g_application_add_main_option_entries(G_APPLICATION(app), entries);
	g_signal_connect(app, "command-line", G_CALLBACK(command_line), NULL);
	status = g_application_run(G_APPLICATION(app), argc, argv);
	g_object_unref(app);
	return status;
}
