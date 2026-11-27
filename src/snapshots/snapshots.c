/*
 * Snapshots - the WormOS snapshot timeline.
 *
 * Shows the snapshots by day, with what each one holds, and does what
 * snap does from the command line: take a snapshot, comment, pin, show
 * what changed, bring back a file, rewind and delete. Everything goes
 * through /usr/lib/worm/snapshots (setuid root, for the group wheel),
 * which asks for your password (an administrator's) before anything that removes protection
 * or changes the system; this program only passes the password on.
 */

#include <gtk/gtk.h>
#include <stdlib.h>
#include <string.h>

#define APP_ID  "org.wormos.Snapshots"
#define HELPER  "/usr/lib/worm/snapshots"
#define CSS     "/usr/share/wormos/snapshots/snapshots.css"

struct snapshot {
	int id;
	gint64 date;
	char scope[8];          /* system, home, all */
	char type[16];          /* manual, auto */
	gboolean pinned, suspect;
	char *comment;
};

static GtkWidget *window, *list, *details, *placeholder, *stack, *message, *revealer, *toast;
static GtkWidget *d_title, *d_date, *d_holds, *d_kind, *d_comment, *d_pin, *d_restore_path;
static GPtrArray *snapshots;            /* of struct snapshot */
static struct snapshot *selected;
static guint message_timer;

static void reload(void);

/* ---- messages ---- */

static gboolean hide_message(gpointer data)
{
	(void)data;
	gtk_revealer_set_reveal_child(GTK_REVEALER(revealer), FALSE);
	message_timer = 0;
	return G_SOURCE_REMOVE;
}

static void show_message(const char *text, gboolean error)
{
	gtk_label_set_text(GTK_LABEL(message), text);
	if (error)
		gtk_widget_add_css_class(toast, "error");
	else
		gtk_widget_remove_css_class(toast, "error");
	gtk_revealer_set_reveal_child(GTK_REVEALER(revealer), TRUE);
	if (message_timer)
		g_source_remove(message_timer);
	message_timer = g_timeout_add_seconds(error ? 10 : 5, hide_message, NULL);
}

/* ---- running the helper ---- */

typedef void (*done_fn)(int status, const char *out, const char *err, gpointer data);

struct job {
	done_fn done;
	gpointer data;
};

static void job_finished(GObject *source, GAsyncResult *res, gpointer data)
{
	struct job *job = data;
	char *out = NULL, *err = NULL;
	GError *error = NULL;
	int status = -1;

	if (g_subprocess_communicate_utf8_finish(G_SUBPROCESS(source), res, &out, &err, &error))
		status = g_subprocess_get_if_exited(G_SUBPROCESS(source))
			? g_subprocess_get_exit_status(G_SUBPROCESS(source)) : -1;
	else
		err = g_strdup(error->message);
	job->done(status, out ? out : "", err ? err : "", job->data);
	g_clear_error(&error);
	g_free(out);
	g_free(err);
	g_free(job);
}

/* Run the helper with args; password (or NULL) goes to its input. */
static void run_helper(const char *const *args, const char *password, done_fn done,
		       gpointer data)
{
	GPtrArray *argv = g_ptr_array_new();
	GSubprocess *proc;
	GError *error = NULL;
	struct job *job;
	char *input = password ? g_strconcat(password, "\n", NULL) : NULL;

	g_ptr_array_add(argv, HELPER);
	for (int i = 0; args[i]; i++)
		g_ptr_array_add(argv, (gpointer)args[i]);
	g_ptr_array_add(argv, NULL);
	proc = g_subprocess_newv((const char *const *)argv->pdata,
				 G_SUBPROCESS_FLAGS_STDIN_PIPE | G_SUBPROCESS_FLAGS_STDOUT_PIPE |
				 G_SUBPROCESS_FLAGS_STDERR_PIPE, &error);
	g_ptr_array_free(argv, TRUE);
	if (!proc) {
		done(-1, "", error->message, data);
		g_error_free(error);
		g_free(input);
		return;
	}
	job = g_new0(struct job, 1);
	job->done = done;
	job->data = data;
	g_subprocess_communicate_utf8_async(proc, input ? input : "", NULL, job_finished, job);
	g_object_unref(proc);
	if (input) {
		explicit_bzero(input, strlen(input));
		g_free(input);
	}
}

/* The last line the helper or snap wrote, for messages. */
static char *last_line(const char *text)
{
	char *copy = g_strstrip(g_strdup(text)), *nl = strrchr(copy, '\n'), *line;

	line = g_strdup(nl ? nl + 1 : copy);
	g_free(copy);
	if (g_str_has_prefix(line, "snapshots: ") || g_str_has_prefix(line, "snap: ")) {
		char *rest = g_strdup(strchr(line, ' ') + 1);

		g_free(line);
		line = rest;
	}
	if (*line)
		*line = g_ascii_toupper(*line);
	return line;
}

/* ---- the password dialog ---- */

struct ask {
	GtkWidget *dialog, *entry, *error, *scope;
	char **args;            /* the helper command, without the password */
	int scope_arg;          /* index in args of the scope, or -1 */
	char *done_text;
};

static void ask_free(struct ask *a)
{
	g_strfreev(a->args);
	g_free(a->done_text);
	g_free(a);
}

static void asked_done(int status, const char *out, const char *err, gpointer data)
{
	struct ask *a = data;

	(void)out;
	gtk_widget_set_sensitive(a->dialog, TRUE);
	if (status == 3) {
		gtk_label_set_text(GTK_LABEL(a->error), "Wrong password. Try again.");
		gtk_editable_set_text(GTK_EDITABLE(a->entry), "");
		gtk_widget_grab_focus(a->entry);
		return;
	}
	gtk_window_destroy(GTK_WINDOW(a->dialog));
	if (status == 0) {
		show_message(a->done_text, FALSE);
	} else {
		char *line = last_line(err);

		show_message(*line ? line : "That did not work.", TRUE);
		g_free(line);
	}
	ask_free(a);
	reload();
}

static void ask_confirmed(GtkWidget *widget, gpointer data)
{
	struct ask *a = data;
	const char *password = gtk_editable_get_text(GTK_EDITABLE(a->entry));

	(void)widget;
	if (a->scope) {
		static const char *const scopes[] = { "system", "home", "all" };
		guint i = gtk_drop_down_get_selected(GTK_DROP_DOWN(a->scope));

		g_free(a->args[a->scope_arg]);
		a->args[a->scope_arg] = g_strdup(scopes[MIN(i, 2)]);
	}
	gtk_label_set_text(GTK_LABEL(a->error), "");
	gtk_widget_set_sensitive(a->dialog, FALSE);
	run_helper((const char *const *)a->args, password, asked_done, a);
}

static void ask_cancelled(GtkButton *button, gpointer data)
{
	struct ask *a = data;

	(void)button;
	gtk_window_destroy(GTK_WINDOW(a->dialog));
	ask_free(a);
}

/*
 * Ask for the administrator's password, then run args. With scope_arg >= 0 the dialog
 * also offers the scope (for rewinding a snapshot that holds both).
 */
static void ask_password(const char *title, const char *text, const char *button_text,
			 gboolean destructive, char **args, int scope_arg,
			 const char *done_text)
{
	struct ask *a = g_new0(struct ask, 1);
	GtkWidget *box, *label, *row, *cancel, *ok;

	a->args = args;
	a->scope_arg = scope_arg;
	a->done_text = g_strdup(done_text);
	a->dialog = gtk_window_new();
	gtk_window_set_title(GTK_WINDOW(a->dialog), title);
	gtk_window_set_transient_for(GTK_WINDOW(a->dialog), GTK_WINDOW(window));
	gtk_window_set_modal(GTK_WINDOW(a->dialog), TRUE);
	gtk_window_set_resizable(GTK_WINDOW(a->dialog), FALSE);
	gtk_window_set_default_size(GTK_WINDOW(a->dialog), 420, -1);

	box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
	gtk_widget_add_css_class(box, "dialog");
	label = gtk_label_new(title);
	gtk_widget_add_css_class(label, "title-2");
	gtk_label_set_xalign(GTK_LABEL(label), 0);
	gtk_box_append(GTK_BOX(box), label);
	label = gtk_label_new(text);
	gtk_label_set_wrap(GTK_LABEL(label), TRUE);
	gtk_label_set_max_width_chars(GTK_LABEL(label), 48);
	gtk_label_set_xalign(GTK_LABEL(label), 0);
	gtk_box_append(GTK_BOX(box), label);

	if (scope_arg >= 0) {
		static const char *const names[] = { "The system only", "Home folders only",
						     "System and home folders", NULL };

		a->scope = gtk_drop_down_new_from_strings(names);
		gtk_box_append(GTK_BOX(box), a->scope);
	}

	a->entry = gtk_password_entry_new();
	gtk_password_entry_set_show_peek_icon(GTK_PASSWORD_ENTRY(a->entry), TRUE);
	g_object_set(a->entry, "placeholder-text", "Your password", "activates-default", TRUE,
		     NULL);
	g_signal_connect(a->entry, "activate", G_CALLBACK(ask_confirmed), a);
	gtk_box_append(GTK_BOX(box), a->entry);
	a->error = gtk_label_new("");
	gtk_widget_add_css_class(a->error, "error");
	gtk_label_set_xalign(GTK_LABEL(a->error), 0);
	gtk_box_append(GTK_BOX(box), a->error);

	row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	gtk_widget_set_halign(row, GTK_ALIGN_END);
	cancel = gtk_button_new_with_label("Cancel");
	g_signal_connect(cancel, "clicked", G_CALLBACK(ask_cancelled), a);
	ok = gtk_button_new_with_label(button_text);
	gtk_widget_add_css_class(ok, destructive ? "destructive-action" : "suggested-action");
	g_signal_connect(ok, "clicked", G_CALLBACK(ask_confirmed), a);
	gtk_box_append(GTK_BOX(row), cancel);
	gtk_box_append(GTK_BOX(row), ok);
	gtk_box_append(GTK_BOX(box), row);

	gtk_window_set_child(GTK_WINDOW(a->dialog), box);
	gtk_window_present(GTK_WINDOW(a->dialog));
	gtk_widget_grab_focus(a->entry);
}

/* ---- describing snapshots ---- */

static const char *holds(const struct snapshot *s)
{
	if (!strcmp(s->scope, "all"))
		return "The system and home folders";
	if (!strcmp(s->scope, "home"))
		return "Home folders";
	return "The system";
}

static char *day_label(gint64 date)
{
	GDateTime *t = g_date_time_new_from_unix_local(date), *now = g_date_time_new_now_local();
	int days = g_date_time_get_day_of_year(now) - g_date_time_get_day_of_year(t);
	char *label;

	if (g_date_time_get_year(now) != g_date_time_get_year(t))
		label = g_date_time_format(t, "%A %e %B %Y");
	else if (days == 0)
		label = g_strdup("Today");
	else if (days == 1)
		label = g_strdup("Yesterday");
	else
		label = g_date_time_format(t, "%A %e %B");
	g_date_time_unref(t);
	g_date_time_unref(now);
	return label;
}

static GtkWidget *badge(GtkWidget *row, const char *text, const char *style)
{
	GtkWidget *b = gtk_label_new(text);

	gtk_widget_add_css_class(b, "badge");
	if (style)
		gtk_widget_add_css_class(b, style);
	gtk_box_append(GTK_BOX(row), b);
	return b;
}

static GtkWidget *snapshot_row(struct snapshot *s)
{
	GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12), *text, *title, *comment;
	GtkWidget *badges, *dot;
	GDateTime *t = g_date_time_new_from_unix_local(s->date);
	char *time = g_date_time_format(t, "%H:%M"), *heading;

	dot = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_widget_add_css_class(dot, "dot");
	if (s->pinned)
		gtk_widget_add_css_class(dot, "pinned");
	if (!strcmp(s->type, "auto"))
		gtk_widget_add_css_class(dot, "auto");
	gtk_widget_set_valign(dot, GTK_ALIGN_CENTER);
	gtk_box_append(GTK_BOX(row), dot);

	text = gtk_box_new(GTK_ORIENTATION_VERTICAL, 3);
	heading = g_strdup_printf("Snapshot %d", s->id);
	title = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	gtk_box_append(GTK_BOX(title), gtk_label_new(heading));
	gtk_widget_add_css_class(gtk_widget_get_first_child(title), "heading");
	{
		GtkWidget *when = gtk_label_new(time);

		gtk_widget_add_css_class(when, "dim-label");
		gtk_box_append(GTK_BOX(title), when);
	}
	gtk_box_append(GTK_BOX(text), title);
	comment = gtk_label_new(*s->comment ? s->comment
				: !strcmp(s->type, "auto") ? "Taken automatically" : "No comment");
	gtk_label_set_xalign(GTK_LABEL(comment), 0);
	gtk_label_set_ellipsize(GTK_LABEL(comment), PANGO_ELLIPSIZE_END);
	gtk_widget_add_css_class(comment, "comment");
	gtk_box_append(GTK_BOX(text), comment);

	badges = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
	badge(badges, !strcmp(s->scope, "all") ? "System + home"
		      : !strcmp(s->scope, "home") ? "Home" : "System", NULL);
	if (!strcmp(s->type, "auto"))
		badge(badges, "Automatic", NULL);
	if (s->pinned)
		badge(badges, "Pinned", "accent");
	if (s->suspect)
		badge(badges, "Not for safe mode", "warning");
	gtk_box_append(GTK_BOX(text), badges);
	gtk_widget_set_hexpand(text, TRUE);
	gtk_box_append(GTK_BOX(row), text);
	gtk_widget_add_css_class(row, "snapshot");

	g_free(heading);
	g_free(time);
	g_date_time_unref(t);
	return row;
}

static void day_header(GtkListBoxRow *row, GtkListBoxRow *before, gpointer data)
{
	struct snapshot *s = g_object_get_data(G_OBJECT(row), "snapshot");
	char *day, *prev = NULL;

	(void)data;
	if (!s)
		return;
	day = day_label(s->date);
	if (before && g_object_get_data(G_OBJECT(before), "snapshot"))
		prev = day_label(((struct snapshot *)
				  g_object_get_data(G_OBJECT(before), "snapshot"))->date);
	if (!before || strcmp(day, prev)) {
		GtkWidget *label = gtk_label_new(day);

		gtk_label_set_xalign(GTK_LABEL(label), 0);
		gtk_widget_add_css_class(label, "day");
		gtk_list_box_row_set_header(row, label);
	} else {
		gtk_list_box_row_set_header(row, NULL);
	}
	g_free(day);
	g_free(prev);
}

/* ---- the details pane ---- */

static void show_details(struct snapshot *s)
{
	GDateTime *t;
	char *text, *when;

	selected = s;
	if (!s) {
		gtk_stack_set_visible_child(GTK_STACK(stack), placeholder);
		return;
	}
	t = g_date_time_new_from_unix_local(s->date);
	text = g_strdup_printf("Snapshot %d", s->id);
	gtk_label_set_text(GTK_LABEL(d_title), text);
	g_free(text);
	when = g_date_time_format(t, "%A %e %B %Y, %H:%M");
	gtk_label_set_text(GTK_LABEL(d_date), when);
	g_free(when);
	g_date_time_unref(t);
	gtk_label_set_text(GTK_LABEL(d_holds), holds(s));
	gtk_label_set_text(GTK_LABEL(d_kind),
			   !strcmp(s->type, "auto")
			   ? (s->suspect ? "Automatically, before a rewind or by worm panic"
					 : "Automatically, before a change")
			   : "By hand");
	gtk_editable_set_text(GTK_EDITABLE(d_comment), s->comment);
	gtk_button_set_label(GTK_BUTTON(d_pin), s->pinned ? "Unpin" : "Pin");
	gtk_editable_set_text(GTK_EDITABLE(d_restore_path), "");
	gtk_stack_set_visible_child(GTK_STACK(stack), details);
}

static void row_selected(GtkListBox *box, GtkListBoxRow *row, gpointer data)
{
	(void)box;
	(void)data;
	show_details(row ? g_object_get_data(G_OBJECT(row), "snapshot") : NULL);
}

static void simple_done(int status, const char *out, const char *err, gpointer data)
{
	char *line = last_line(status == 0 ? out : err);

	if (status == 0)
		show_message(data ? data : line, FALSE);
	else
		show_message(*line ? line : "That did not work.", TRUE);
	g_free(line);
	g_free(data);
	reload();
}

static char *id_text(void)
{
	return g_strdup_printf("%d", selected->id);
}

static void save_comment(GtkWidget *widget, gpointer data)
{
	char *id = id_text();
	const char *args[] = { "comment", id, gtk_editable_get_text(GTK_EDITABLE(d_comment)),
			       NULL };

	(void)widget;
	(void)data;
	run_helper(args, NULL, simple_done, g_strdup("Comment saved."));
	g_free(id);
}

static void pin_clicked(GtkButton *button, gpointer data)
{
	char *id = id_text();

	(void)button;
	(void)data;
	if (!selected->pinned) {
		const char *args[] = { "pin", id, NULL };

		run_helper(args, NULL, simple_done,
			   g_strdup_printf("Snapshot %d is pinned: pruning keeps it.", selected->id));
	} else {
		char *msg = g_strdup_printf("Unpinned snapshot %d.", selected->id);

		ask_password("Unpin this snapshot?",
			     "Unpinned snapshots can be pruned. Removing protection needs your "
			     "password.", "Unpin", FALSE,
			     g_strdupv((char *[]){ "unpin", id, NULL }), -1, msg);
		g_free(msg);
	}
	g_free(id);
}

static void changes_done(int status, const char *out, const char *err, gpointer data)
{
	GtkWidget *win, *scroll, *view, *header;
	char *title;

	if (status != 0) {
		char *line = last_line(err);

		show_message(line, TRUE);
		g_free(line);
		return;
	}
	win = gtk_window_new();
	title = g_strdup_printf("Changes since snapshot %d", GPOINTER_TO_INT(data));
	gtk_window_set_title(GTK_WINDOW(win), title);
	g_free(title);
	gtk_window_set_transient_for(GTK_WINDOW(win), GTK_WINDOW(window));
	gtk_window_set_default_size(GTK_WINDOW(win), 640, 480);
	header = gtk_header_bar_new();
	gtk_window_set_titlebar(GTK_WINDOW(win), header);
	view = gtk_text_view_new();
	gtk_text_view_set_editable(GTK_TEXT_VIEW(view), FALSE);
	gtk_text_view_set_monospace(GTK_TEXT_VIEW(view), TRUE);
	gtk_text_view_set_left_margin(GTK_TEXT_VIEW(view), 12);
	gtk_text_view_set_top_margin(GTK_TEXT_VIEW(view), 12);
	gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(view)),
				 *out ? out : "No changes.", -1);
	scroll = gtk_scrolled_window_new();
	gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), view);
	gtk_window_set_child(GTK_WINDOW(win), scroll);
	gtk_window_present(GTK_WINDOW(win));
}

static void changes_clicked(GtkButton *button, gpointer data)
{
	char *id = id_text();
	const char *args[] = { "diff", id, NULL };

	(void)button;
	(void)data;
	if (!strcmp(selected->scope, "home")) {
		show_message("Changes are shown for the system; this snapshot holds home folders.",
			     TRUE);
	} else {
		show_message("Comparing with the current system...", FALSE);
		run_helper(args, NULL, changes_done, GINT_TO_POINTER(selected->id));
	}
	g_free(id);
}

static void restore_clicked(GtkWidget *widget, gpointer data)
{
	const char *path = gtk_editable_get_text(GTK_EDITABLE(d_restore_path));
	char *id, *text, *msg;

	(void)widget;
	(void)data;
	if (path[0] != '/') {
		show_message("Type the full path of a file or folder, such as /etc/hosts.", TRUE);
		return;
	}
	id = id_text();
	text = g_strdup_printf("%s comes back as it was in snapshot %d. What is there now is "
			       "saved in a new snapshot first.", path, selected->id);
	msg = g_strdup_printf("Restored %s from snapshot %d.", path, selected->id);
	ask_password("Restore from this snapshot?", text, "Restore", FALSE,
		     g_strdupv((char *[]){ "restore", id, (char *)path, NULL }), -1, msg);
	g_free(text);
	g_free(msg);
	g_free(id);
}

static void rewind_clicked(GtkButton *button, gpointer data)
{
	char *id = id_text(), *text;
	gboolean both = !strcmp(selected->scope, "all");

	(void)button;
	(void)data;
	text = g_strdup_printf("%s go%s back to snapshot %d. The system restarts right away "
			       "(programs are closed; save your work). The current state is "
			       "saved as a new snapshot first, so this can be undone.",
			       both ? "Choose what to rewind; it will" : holds(selected),
			       both ? "" : "es", selected->id);
	ask_password("Rewind to this snapshot?", text, "Rewind and restart", TRUE,
		     g_strdupv((char *[]){ "rewind", id, selected->scope, NULL }),
		     both ? 2 : -1, "Restarting...");
	g_free(text);
	g_free(id);
}

static void delete_clicked(GtkButton *button, gpointer data)
{
	char *id = id_text(), *text, *msg;

	(void)button;
	(void)data;
	text = g_strdup_printf("Snapshot %d is removed for good.", selected->id);
	msg = g_strdup_printf("Deleted snapshot %d.", selected->id);
	ask_password("Delete this snapshot?", text, "Delete", TRUE,
		     g_strdupv((char *[]){ "delete", id, NULL }), -1, msg);
	g_free(text);
	g_free(msg);
	g_free(id);
}

static GtkWidget *field(GtkWidget *grid, int row, const char *name)
{
	GtkWidget *label = gtk_label_new(name), *value = gtk_label_new("");

	gtk_label_set_xalign(GTK_LABEL(label), 0);
	gtk_widget_add_css_class(label, "dim-label");
	gtk_label_set_xalign(GTK_LABEL(value), 0);
	gtk_label_set_wrap(GTK_LABEL(value), TRUE);
	gtk_grid_attach(GTK_GRID(grid), label, 0, row, 1, 1);
	gtk_grid_attach(GTK_GRID(grid), value, 1, row, 1, 1);
	return value;
}

static GtkWidget *section(GtkWidget *box, const char *title)
{
	GtkWidget *label = gtk_label_new(title);

	gtk_label_set_xalign(GTK_LABEL(label), 0);
	gtk_widget_add_css_class(label, "section");
	gtk_box_append(GTK_BOX(box), label);
	return label;
}

static GtkWidget *build_details(void)
{
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10), *grid, *row, *button;

	gtk_widget_add_css_class(box, "details");
	d_title = gtk_label_new("");
	gtk_widget_add_css_class(d_title, "title-1");
	gtk_label_set_xalign(GTK_LABEL(d_title), 0);
	gtk_box_append(GTK_BOX(box), d_title);
	d_date = gtk_label_new("");
	gtk_widget_add_css_class(d_date, "dim-label");
	gtk_label_set_xalign(GTK_LABEL(d_date), 0);
	gtk_box_append(GTK_BOX(box), d_date);

	grid = gtk_grid_new();
	gtk_grid_set_column_spacing(GTK_GRID(grid), 16);
	gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
	gtk_widget_set_margin_top(grid, 8);
	d_holds = field(grid, 0, "Holds");
	d_kind = field(grid, 1, "Taken");
	gtk_box_append(GTK_BOX(box), grid);

	section(box, "Comment");
	row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	d_comment = gtk_entry_new();
	gtk_entry_set_max_length(GTK_ENTRY(d_comment), 200);
	gtk_widget_set_hexpand(d_comment, TRUE);
	g_signal_connect(d_comment, "activate", G_CALLBACK(save_comment), NULL);
	gtk_box_append(GTK_BOX(row), d_comment);
	button = gtk_button_new_with_label("Save");
	g_signal_connect(button, "clicked", G_CALLBACK(save_comment), NULL);
	gtk_box_append(GTK_BOX(row), button);
	gtk_box_append(GTK_BOX(box), row);

	row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	gtk_widget_set_margin_top(row, 6);
	d_pin = gtk_button_new_with_label("Pin");
	g_signal_connect(d_pin, "clicked", G_CALLBACK(pin_clicked), NULL);
	gtk_box_append(GTK_BOX(row), d_pin);
	button = gtk_button_new_with_label("Show changes since");
	g_signal_connect(button, "clicked", G_CALLBACK(changes_clicked), NULL);
	gtk_box_append(GTK_BOX(row), button);
	gtk_box_append(GTK_BOX(box), row);

	section(box, "Bring back a file or folder");
	row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	d_restore_path = gtk_entry_new();
	gtk_entry_set_placeholder_text(GTK_ENTRY(d_restore_path), "/home/you/Documents/report.txt");
	gtk_widget_set_hexpand(d_restore_path, TRUE);
	g_signal_connect(d_restore_path, "activate", G_CALLBACK(restore_clicked), NULL);
	gtk_box_append(GTK_BOX(row), d_restore_path);
	button = gtk_button_new_with_label("Restore");
	g_signal_connect(button, "clicked", G_CALLBACK(restore_clicked), NULL);
	gtk_box_append(GTK_BOX(row), button);
	gtk_box_append(GTK_BOX(box), row);

	section(box, "Go back");
	row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	button = gtk_button_new_with_label("Rewind to this snapshot");
	gtk_widget_add_css_class(button, "destructive-action");
	g_signal_connect(button, "clicked", G_CALLBACK(rewind_clicked), NULL);
	gtk_box_append(GTK_BOX(row), button);
	button = gtk_button_new_with_label("Delete");
	g_signal_connect(button, "clicked", G_CALLBACK(delete_clicked), NULL);
	gtk_box_append(GTK_BOX(row), button);
	gtk_box_append(GTK_BOX(box), row);
	return box;
}

/* ---- the list ---- */

static void snapshot_free(gpointer p)
{
	struct snapshot *s = p;

	g_free(s->comment);
	g_free(s);
}

static void list_done(int status, const char *out, const char *err, gpointer data)
{
	int keep = selected ? selected->id : 0;
	char **lines;
	GtkWidget *child;

	(void)data;
	if (status != 0) {
		char *line = last_line(err);

		show_message(*line ? line : "Cannot read the snapshots.", TRUE);
		g_free(line);
		return;
	}
	selected = NULL;
	while ((child = gtk_widget_get_first_child(list)))
		gtk_list_box_remove(GTK_LIST_BOX(list), child);
	g_ptr_array_set_size(snapshots, 0);

	lines = g_strsplit(out, "\n", -1);
	/* Newest first. */
	for (int i = g_strv_length(lines) - 1; i >= 0; i--) {
		char **f = g_strsplit(lines[i], "\t", 7);
		struct snapshot *s;
		GtkWidget *row;

		if (g_strv_length(f) == 7) {
			s = g_new0(struct snapshot, 1);
			s->id = atoi(f[0]);
			s->date = g_ascii_strtoll(f[1], NULL, 10);
			g_strlcpy(s->scope, f[2], sizeof(s->scope));
			g_strlcpy(s->type, f[3], sizeof(s->type));
			s->pinned = f[4][0] == '1';
			s->suspect = f[5][0] == '1';
			s->comment = g_strdup(f[6]);
			g_ptr_array_add(snapshots, s);
			/* The row carries its snapshot before the list sees it (the
			 * day headers need it). */
			row = gtk_list_box_row_new();
			gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), snapshot_row(s));
			g_object_set_data(G_OBJECT(row), "snapshot", s);
			gtk_list_box_append(GTK_LIST_BOX(list), row);
			if (s->id == keep)
				gtk_list_box_select_row(GTK_LIST_BOX(list), GTK_LIST_BOX_ROW(row));
		}
		g_strfreev(f);
	}
	g_strfreev(lines);
	gtk_list_box_invalidate_headers(GTK_LIST_BOX(list));
	if (!selected)
		show_details(NULL);
	gtk_label_set_text(GTK_LABEL(placeholder),
			   snapshots->len ? "Select a snapshot to see what you can do with it."
					  : "No snapshots yet. Take one with the button above.");
}

static void reload(void)
{
	const char *args[] = { "list", NULL };

	run_helper(args, NULL, list_done, NULL);
}

/* ---- taking a snapshot ---- */

static GtkWidget *take_comment, *take_scope, *take_popover, *take_menu;

static void take_done(int status, const char *out, const char *err, gpointer data)
{
	(void)data;
	gtk_widget_set_sensitive(take_menu, TRUE);
	simple_done(status, out, err, NULL);
}

static void take_clicked(GtkWidget *widget, gpointer data)
{
	static const char *const scopes[] = { "system", "home", "all" };
	const char *comment = gtk_editable_get_text(GTK_EDITABLE(take_comment));
	const char *args[] = { "now",
			       scopes[MIN(gtk_drop_down_get_selected(GTK_DROP_DOWN(take_scope)), 2)],
			       *comment ? comment : "From the Snapshots program", NULL };

	(void)widget;
	(void)data;
	/* Close the menu first: its entry must lose the focus normally. */
	gtk_popover_popdown(GTK_POPOVER(take_popover));
	gtk_widget_set_sensitive(take_menu, FALSE);
	show_message("Taking a snapshot...", FALSE);
	run_helper(args, NULL, take_done, NULL);
	gtk_editable_set_text(GTK_EDITABLE(take_comment), "");
}

static GtkWidget *take_button(void)
{
	static const char *const names[] = { "The system", "Home folders",
					     "System and home folders", NULL };
	GtkWidget *button = gtk_menu_button_new(), *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
	GtkWidget *label, *take;

	gtk_menu_button_set_label(GTK_MENU_BUTTON(button), "Take a snapshot");
	gtk_widget_add_css_class(button, "suggested-action");
	take_popover = gtk_popover_new();
	label = gtk_label_new("What is it for?");
	gtk_label_set_xalign(GTK_LABEL(label), 0);
	gtk_box_append(GTK_BOX(box), label);
	take_comment = gtk_entry_new();
	gtk_entry_set_max_length(GTK_ENTRY(take_comment), 200);
	gtk_entry_set_placeholder_text(GTK_ENTRY(take_comment), "Before installing a tool");
	gtk_widget_set_size_request(take_comment, 280, -1);
	g_signal_connect(take_comment, "activate", G_CALLBACK(take_clicked), NULL);
	gtk_box_append(GTK_BOX(box), take_comment);
	take_scope = gtk_drop_down_new_from_strings(names);
	gtk_box_append(GTK_BOX(box), take_scope);
	take = gtk_button_new_with_label("Take snapshot");
	gtk_widget_add_css_class(take, "suggested-action");
	g_signal_connect(take, "clicked", G_CALLBACK(take_clicked), NULL);
	gtk_box_append(GTK_BOX(box), take);
	gtk_popover_set_child(GTK_POPOVER(take_popover), box);
	gtk_menu_button_set_popover(GTK_MENU_BUTTON(button), take_popover);
	take_menu = button;
	return button;
}

/* ---- the window ---- */

static void activate(GtkApplication *app, gpointer data)
{
	GtkWidget *header, *paned, *scroll, *overlay, *right;
	GtkCssProvider *css;

	(void)data;
	if (window) {
		gtk_window_present(GTK_WINDOW(window));
		return;
	}
	css = gtk_css_provider_new();
	gtk_css_provider_load_from_path(css, CSS);
	gtk_style_context_add_provider_for_display(gdk_display_get_default(),
						   GTK_STYLE_PROVIDER(css),
						   GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
	g_object_unref(css);
	snapshots = g_ptr_array_new_with_free_func(snapshot_free);

	window = gtk_application_window_new(app);
	gtk_window_set_title(GTK_WINDOW(window), "Snapshots");
	gtk_window_set_default_size(GTK_WINDOW(window), 940, 620);
	header = gtk_header_bar_new();
	gtk_header_bar_pack_start(GTK_HEADER_BAR(header), take_button());
	gtk_window_set_titlebar(GTK_WINDOW(window), header);

	list = gtk_list_box_new();
	gtk_widget_add_css_class(list, "timeline");
	gtk_list_box_set_header_func(GTK_LIST_BOX(list), day_header, NULL, NULL);
	g_signal_connect(list, "row-selected", G_CALLBACK(row_selected), NULL);
	scroll = gtk_scrolled_window_new();
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER,
				       GTK_POLICY_AUTOMATIC);
	gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), list);
	gtk_widget_set_size_request(scroll, 360, -1);

	stack = gtk_stack_new();
	placeholder = gtk_label_new("Loading...");
	gtk_widget_add_css_class(placeholder, "placeholder");
	gtk_label_set_wrap(GTK_LABEL(placeholder), TRUE);
	gtk_stack_add_child(GTK_STACK(stack), placeholder);
	details = build_details();
	gtk_stack_add_child(GTK_STACK(stack), details);
	right = gtk_scrolled_window_new();
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(right), GTK_POLICY_NEVER,
				       GTK_POLICY_AUTOMATIC);
	gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(right), stack);

	paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
	gtk_paned_set_start_child(GTK_PANED(paned), scroll);
	gtk_paned_set_end_child(GTK_PANED(paned), right);
	gtk_paned_set_resize_start_child(GTK_PANED(paned), FALSE);
	gtk_paned_set_shrink_start_child(GTK_PANED(paned), FALSE);
	gtk_paned_set_position(GTK_PANED(paned), 380);

	overlay = gtk_overlay_new();
	gtk_overlay_set_child(GTK_OVERLAY(overlay), paned);
	revealer = gtk_revealer_new();
	gtk_revealer_set_transition_type(GTK_REVEALER(revealer),
					 GTK_REVEALER_TRANSITION_TYPE_SLIDE_UP);
	gtk_widget_set_valign(revealer, GTK_ALIGN_END);
	gtk_widget_set_halign(revealer, GTK_ALIGN_CENTER);
	toast = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	gtk_widget_add_css_class(toast, "toast");
	message = gtk_label_new("");
	gtk_label_set_wrap(GTK_LABEL(message), TRUE);
	gtk_box_append(GTK_BOX(toast), message);
	gtk_revealer_set_child(GTK_REVEALER(revealer), toast);
	gtk_overlay_add_overlay(GTK_OVERLAY(overlay), revealer);
	gtk_window_set_child(GTK_WINDOW(window), overlay);

	gtk_window_present(GTK_WINDOW(window));
	reload();
}

int main(int argc, char **argv)
{
	GtkApplication *app = gtk_application_new(APP_ID, G_APPLICATION_DEFAULT_FLAGS);
	int status;

	g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);
	status = g_application_run(G_APPLICATION(app), argc, argv);
	g_object_unref(app);
	return status;
}
