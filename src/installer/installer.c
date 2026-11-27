/*
 * wormos-installer - install WormOS from the live system onto a disk.
 *
 * A front end to /usr/lib/worm/install-system (src/worm/install-system.c),
 * which lists the disks and does the work as root. Three pages: choose a
 * disk (with a confirmation, since the disk is erased), progress, done.
 * Outside the live system it only says where to get the ISO.
 */

#define _GNU_SOURCE
#include <gtk/gtk.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define APP_ID   "org.wormos.Installer"
#define HELPER   "/usr/lib/worm/install-system"
#define POWER    "/usr/lib/worm/power"
#define CSS      "/usr/share/wormos/installer/installer.css"

static GtkWidget *window, *stack, *disk_list, *install_button, *list_note;
static GtkWidget *encrypt_check, *pass1, *pass2, *pass_box, *pass_note;
static GtkWidget *progress_bar, *step_label, *error_label, *done_button;
static char chosen[96], chosen_desc[256];
static GString *errors;

static int live_system(void)
{
	char *cmdline = NULL;
	int live = 0;

	if (g_file_get_contents("/proc/cmdline", &cmdline, NULL, NULL)) {
		char **words = g_strsplit_set(cmdline, " \n", -1);

		for (char **w = words; *w; w++)
			live |= !strcmp(*w, "wormos.live");
		g_strfreev(words);
		g_free(cmdline);
	}
	return live;
}

static GtkWidget *label(const char *text, const char *css)
{
	GtkWidget *l = gtk_label_new(text);

	gtk_label_set_wrap(GTK_LABEL(l), TRUE);
	gtk_label_set_xalign(GTK_LABEL(l), 0);
	if (css)
		gtk_widget_add_css_class(l, css);
	return l;
}

static GtkWidget *page(const char *title, const char *text)
{
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);

	gtk_widget_add_css_class(box, "page");
	gtk_box_append(GTK_BOX(box), label(title, "title-1"));
	if (text)
		gtk_box_append(GTK_BOX(box), label(text, NULL));
	return box;
}

static char *human_size(unsigned long long bytes)
{
	return g_format_size_full(bytes, G_FORMAT_SIZE_DEFAULT);
}

/* ---- choosing the disk ---- */

static void row_selected(GtkListBox *box, GtkListBoxRow *row, gpointer data)
{
	(void)box;
	(void)data;
	gtk_widget_set_sensitive(install_button, row != NULL);
	if (row) {
		snprintf(chosen, sizeof(chosen), "%s",
			 (const char *)g_object_get_data(G_OBJECT(row), "disk"));
		snprintf(chosen_desc, sizeof(chosen_desc), "%s",
			 (const char *)g_object_get_data(G_OBJECT(row), "desc"));
	}
}

static void fill_disks(void)
{
	char *out = NULL, *err = NULL;
	char *argv[] = { HELPER, "list", NULL };
	int status = 0, n = 0;
	GtkWidget *child;

	while ((child = gtk_widget_get_first_child(disk_list)))
		gtk_list_box_remove(GTK_LIST_BOX(disk_list), child);
	gtk_widget_set_sensitive(install_button, FALSE);
	if (!g_spawn_sync(NULL, argv, NULL, G_SPAWN_DEFAULT, NULL, NULL, &out, &err, &status,
			  NULL) || !g_spawn_check_wait_status(status, NULL)) {
		gtk_label_set_text(GTK_LABEL(list_note), err && *err ? err : "Cannot list the disks.");
		g_free(out);
		g_free(err);
		return;
	}
	char **lines = g_strsplit(out, "\n", -1);

	for (char **l = lines; *l; l++) {
		char name[96], removable[8], model[160] = "";
		unsigned long long bytes;
		GtkWidget *row, *box, *title, *sub;
		char *size, *desc, *detail;

		if (sscanf(*l, "%95s %llu %7s %159[^\n]", name, &bytes, removable, model) < 3)
			continue;
		size = human_size(bytes);
		desc = g_strdup_printf("%s, %s", model[0] ? model : "Disk", size);
		detail = g_strdup_printf("/dev/%s%s", name,
					 !strcmp(removable, "1") ? " · removable" : "");
		row = gtk_list_box_row_new();
		box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
		gtk_widget_add_css_class(box, "disk");
		title = label(desc, "heading");
		sub = label(detail, "dim-label");
		gtk_box_append(GTK_BOX(box), title);
		gtk_box_append(GTK_BOX(box), sub);
		gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), box);
		g_object_set_data_full(G_OBJECT(row), "disk", g_strdup(name), g_free);
		g_object_set_data_full(G_OBJECT(row), "desc", g_strdup_printf("%s (%s)", desc,
						     detail), g_free);
		gtk_list_box_append(GTK_LIST_BOX(disk_list), row);
		g_free(size);
		g_free(desc);
		g_free(detail);
		n++;
	}
	g_strfreev(lines);
	gtk_label_set_text(GTK_LABEL(list_note), n ? "" :
			   "No disk of at least 8 GB was found. Add one and press Refresh.");
	g_free(out);
	g_free(err);
}

/* ---- installing ---- */

static void show_error(const char *msg)
{
	gtk_label_set_text(GTK_LABEL(step_label), "The installation failed.");
	gtk_label_set_text(GTK_LABEL(error_label), msg);
	gtk_widget_set_visible(error_label, TRUE);
	gtk_button_set_label(GTK_BUTTON(done_button), "Back");
	gtk_widget_set_visible(done_button, TRUE);
	g_object_set_data(G_OBJECT(done_button), "failed", GINT_TO_POINTER(1));
}

static void read_line(GObject *src, GAsyncResult *res, gpointer data);

static void next_line(GDataInputStream *in)
{
	g_data_input_stream_read_line_async(in, G_PRIORITY_DEFAULT, NULL, read_line, NULL);
}

static void read_errors(GObject *src, GAsyncResult *res, gpointer data)
{
	char *line = g_data_input_stream_read_line_finish(G_DATA_INPUT_STREAM(src), res, NULL,
							  NULL);

	(void)data;
	if (!line)
		return;
	g_string_append_printf(errors, "%s%s", errors->len ? "\n" : "", line);
	g_free(line);
	g_data_input_stream_read_line_async(G_DATA_INPUT_STREAM(src), G_PRIORITY_DEFAULT, NULL,
					    read_errors, NULL);
}

static void read_line(GObject *src, GAsyncResult *res, gpointer data)
{
	char *line = g_data_input_stream_read_line_finish(G_DATA_INPUT_STREAM(src), res, NULL,
							  NULL);

	(void)data;
	if (!line)
		return;
	if (!strncmp(line, "step ", 5))
		gtk_label_set_text(GTK_LABEL(step_label), line + 5);
	else if (!strncmp(line, "progress ", 9))
		gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(progress_bar), atoi(line + 9) / 100.0);
	g_free(line);
	next_line(G_DATA_INPUT_STREAM(src));
}

static void finished(GObject *src, GAsyncResult *res, gpointer data)
{
	GSubprocess *proc = G_SUBPROCESS(src);

	(void)data;
	if (!g_subprocess_wait_finish(proc, res, NULL) || !g_subprocess_get_if_exited(proc) ||
	    g_subprocess_get_exit_status(proc) != 0) {
		show_error(errors->len ? errors->str : "The installer stopped unexpectedly.");
		return;
	}
	gtk_stack_set_visible_child_name(GTK_STACK(stack), "done");
}

static void start_install(void)
{
	GError *err = NULL;
	GSubprocess *proc;

	gtk_stack_set_visible_child_name(GTK_STACK(stack), "progress");
	gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(progress_bar), 0);
	gtk_label_set_text(GTK_LABEL(step_label), "Starting");
	gtk_widget_set_visible(error_label, FALSE);
	gtk_widget_set_visible(done_button, FALSE);
	g_string_truncate(errors, 0);
	if (gtk_check_button_get_active(GTK_CHECK_BUTTON(encrypt_check))) {
		/* The passphrase goes to the helper's input, never its arguments. */
		char *line = g_strconcat(gtk_editable_get_text(GTK_EDITABLE(pass1)), "\n", NULL);
		GOutputStream *in;

		proc = g_subprocess_new(G_SUBPROCESS_FLAGS_STDIN_PIPE | G_SUBPROCESS_FLAGS_STDOUT_PIPE |
					G_SUBPROCESS_FLAGS_STDERR_PIPE, &err, HELPER, "install", chosen,
					"--encrypt", NULL);
		if (proc) {
			in = g_subprocess_get_stdin_pipe(proc);
			g_output_stream_write_all(in, line, strlen(line), NULL, NULL, NULL);
			g_output_stream_close(in, NULL, NULL);
		}
		explicit_bzero(line, strlen(line));
		g_free(line);
		gtk_editable_set_text(GTK_EDITABLE(pass1), "");
		gtk_editable_set_text(GTK_EDITABLE(pass2), "");
	} else {
		proc = g_subprocess_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE,
					&err, HELPER, "install", chosen, NULL);
	}
	if (!proc) {
		show_error(err->message);
		g_error_free(err);
		return;
	}
	next_line(g_data_input_stream_new(g_subprocess_get_stdout_pipe(proc)));
	g_data_input_stream_read_line_async(g_data_input_stream_new(
		g_subprocess_get_stderr_pipe(proc)), G_PRIORITY_DEFAULT, NULL, read_errors, NULL);
	g_subprocess_wait_async(proc, NULL, finished, NULL);
}

static void confirmed(GObject *src, GAsyncResult *res, gpointer data)
{
	(void)data;
	if (gtk_alert_dialog_choose_finish(GTK_ALERT_DIALOG(src), res, NULL) == 1)
		start_install();
}

static void install_clicked(GtkButton *b, gpointer data)
{
	const char *buttons[] = { "Cancel", "Erase and install", NULL };
	g_autofree char *detail = g_strdup_printf(
		"Everything on %s will be deleted and replaced by WormOS. "
		"This cannot be undone.", chosen_desc);
	GtkAlertDialog *dialog;

	(void)b;
	(void)data;
	if (gtk_check_button_get_active(GTK_CHECK_BUTTON(encrypt_check))) {
		const char *a = gtk_editable_get_text(GTK_EDITABLE(pass1));
		const char *c = gtk_editable_get_text(GTK_EDITABLE(pass2));

		if (strlen(a) < 8) {
			gtk_label_set_text(GTK_LABEL(pass_note), "The passphrase needs at least 8 characters.");
			return;
		}
		if (strcmp(a, c)) {
			gtk_label_set_text(GTK_LABEL(pass_note), "The two passphrases are not the same.");
			return;
		}
		gtk_label_set_text(GTK_LABEL(pass_note), "");
	}
	dialog = gtk_alert_dialog_new("Erase this disk?");
	gtk_alert_dialog_set_detail(dialog, detail);
	gtk_alert_dialog_set_buttons(dialog, buttons);
	gtk_alert_dialog_set_cancel_button(dialog, 0);
	gtk_alert_dialog_set_default_button(dialog, 0);
	gtk_alert_dialog_choose(dialog, GTK_WINDOW(window), NULL, confirmed, NULL);
}

static void back_clicked(GtkButton *b, gpointer data)
{
	(void)b;
	(void)data;
	fill_disks();
	gtk_stack_set_visible_child_name(GTK_STACK(stack), "choose");
}

static void restart_clicked(GtkButton *b, gpointer data)
{
	char *argv[] = { POWER, "reboot", NULL };

	(void)b;
	(void)data;
	g_spawn_async(NULL, argv, NULL, G_SPAWN_DEFAULT, NULL, NULL, NULL, NULL);
}

static void refresh_clicked(GtkButton *b, gpointer data)
{
	(void)b;
	(void)data;
	fill_disks();
}

static void encrypt_toggled(GtkCheckButton *b, gpointer data)
{
	gboolean on = gtk_check_button_get_active(b);

	(void)data;
	gtk_widget_set_visible(pass_box, on);
	gtk_label_set_text(GTK_LABEL(pass_note), on ?
			   "Everything on the disk is encrypted. Without the passphrase nobody can "
			   "read it, and nobody can recover it for you: keep it safe." : "");
}

/* ---- the window ---- */

static GtkWidget *choose_page(void)
{
	GtkWidget *box = page("Install WormOS",
		"WormOS goes onto a disk of its own: the disk you choose is erased. It "
		"needs at least 8 GB. After a restart from that disk, WormOS asks for your "
		"name and passwords, as on any new installation.");
	GtkWidget *scroll = gtk_scrolled_window_new(), *buttons, *refresh;

	disk_list = gtk_list_box_new();
	gtk_widget_add_css_class(disk_list, "boxed-list");
	g_signal_connect(disk_list, "row-selected", G_CALLBACK(row_selected), NULL);
	gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), disk_list);
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER,
				       GTK_POLICY_AUTOMATIC);
	gtk_widget_set_vexpand(scroll, TRUE);
	gtk_box_append(GTK_BOX(box), scroll);
	list_note = label("", "dim-label");
	gtk_box_append(GTK_BOX(box), list_note);

	/* Encryption: the whole system and home folders, opened at boot. */
	encrypt_check = gtk_check_button_new_with_label("Encrypt the disk (asks for a passphrase at every start)");
	g_signal_connect(encrypt_check, "toggled", G_CALLBACK(encrypt_toggled), NULL);
	gtk_box_append(GTK_BOX(box), encrypt_check);
	pass_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	pass1 = gtk_password_entry_new();
	pass2 = gtk_password_entry_new();
	gtk_password_entry_set_show_peek_icon(GTK_PASSWORD_ENTRY(pass1), TRUE);
	g_object_set(pass1, "placeholder-text", "Passphrase", NULL);
	g_object_set(pass2, "placeholder-text", "The same again", NULL);
	gtk_widget_set_hexpand(pass1, TRUE);
	gtk_widget_set_hexpand(pass2, TRUE);
	gtk_box_append(GTK_BOX(pass_box), pass1);
	gtk_box_append(GTK_BOX(pass_box), pass2);
	gtk_widget_set_visible(pass_box, FALSE);
	gtk_box_append(GTK_BOX(box), pass_box);
	pass_note = label("", "dim-label");
	gtk_box_append(GTK_BOX(box), pass_note);

	buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	gtk_widget_set_halign(buttons, GTK_ALIGN_END);
	refresh = gtk_button_new_with_label("Refresh");
	g_signal_connect(refresh, "clicked", G_CALLBACK(refresh_clicked), NULL);
	install_button = gtk_button_new_with_label("Install…");
	gtk_widget_add_css_class(install_button, "destructive-action");
	g_signal_connect(install_button, "clicked", G_CALLBACK(install_clicked), NULL);
	gtk_box_append(GTK_BOX(buttons), refresh);
	gtk_box_append(GTK_BOX(buttons), install_button);
	gtk_box_append(GTK_BOX(box), buttons);
	return box;
}

static GtkWidget *progress_page(void)
{
	GtkWidget *box = page("Installing WormOS", NULL);

	step_label = label("", "heading");
	progress_bar = gtk_progress_bar_new();
	error_label = label("", "error");
	gtk_label_set_selectable(GTK_LABEL(error_label), TRUE);
	done_button = gtk_button_new_with_label("Back");
	gtk_widget_set_halign(done_button, GTK_ALIGN_END);
	g_signal_connect(done_button, "clicked", G_CALLBACK(back_clicked), NULL);
	gtk_box_append(GTK_BOX(box), step_label);
	gtk_box_append(GTK_BOX(box), progress_bar);
	gtk_box_append(GTK_BOX(box), error_label);
	gtk_box_append(GTK_BOX(box), done_button);
	return box;
}

static GtkWidget *done_page(void)
{
	GtkWidget *box = page("WormOS is installed",
		"Remove the installation medium (eject the ISO from the virtual machine, or "
		"unplug the USB stick), then restart. WormOS starts from the disk and asks "
		"for your name and passwords."), *restart;

	restart = gtk_button_new_with_label("Restart now");
	gtk_widget_add_css_class(restart, "suggested-action");
	gtk_widget_set_halign(restart, GTK_ALIGN_END);
	g_signal_connect(restart, "clicked", G_CALLBACK(restart_clicked), NULL);
	gtk_box_append(GTK_BOX(box), restart);
	return box;
}

static void activate(GtkApplication *app, gpointer data)
{
	GtkCssProvider *css = gtk_css_provider_new();

	(void)data;
	if (window) {
		gtk_window_present(GTK_WINDOW(window));
		return;
	}
	gtk_css_provider_load_from_path(css, CSS);
	gtk_style_context_add_provider_for_display(gdk_display_get_default(),
		GTK_STYLE_PROVIDER(css), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

	window = gtk_application_window_new(app);
	gtk_window_set_title(GTK_WINDOW(window), "Install WormOS");
	gtk_window_set_default_size(GTK_WINDOW(window), 620, 520);
	stack = gtk_stack_new();
	gtk_stack_set_transition_type(GTK_STACK(stack), GTK_STACK_TRANSITION_TYPE_CROSSFADE);
	if (live_system()) {
		gtk_stack_add_named(GTK_STACK(stack), choose_page(), "choose");
		gtk_stack_add_named(GTK_STACK(stack), progress_page(), "progress");
		gtk_stack_add_named(GTK_STACK(stack), done_page(), "done");
		fill_disks();
	} else {
		gtk_stack_add_named(GTK_STACK(stack), page("Install WormOS",
			"WormOS installs from its live system: start the computer from the "
			"WormOS ISO (as a CD/DVD, or written to a USB stick) and open the "
			"installer there."), "info");
	}
	gtk_window_set_child(GTK_WINDOW(window), stack);
	gtk_window_present(GTK_WINDOW(window));
}

int main(int argc, char **argv)
{
	GtkApplication *app = gtk_application_new(APP_ID, G_APPLICATION_DEFAULT_FLAGS);
	int status;

	errors = g_string_new("");
	g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);
	status = g_application_run(G_APPLICATION(app), argc, argv);
	g_object_unref(app);
	return status;
}
