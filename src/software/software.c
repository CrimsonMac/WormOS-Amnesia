/*
 * wormos-software - Software: find, install, remove and update programs.
 *
 * A window over wh. Explore searches every source (WormOS's own packages,
 * then Debian and Kali, which wh keeps in wormholes) and shows a short
 * list of popular programs before anything is typed; Installed lists what
 * is there; Updates looks for newer versions and installs them.
 *
 * Everything that needs root goes through /usr/lib/worm/software (see
 * src/worm/software.c), which asks for root's password where there is a
 * root, and where users may install by themselves (WormOS Amnesia) needs
 * none. This program only passes the password on. Programs from Debian or
 * Kali are ready at once; WormOS packages are applied at the next restart,
 * so the session is never restarted under the user's windows: the window
 * says so and offers to restart.
 */

#define _GNU_SOURCE
#include <gtk/gtk.h>
#include <string.h>
#include <unistd.h>

#define APP_ID      "org.wormos.Software"
#define HELPER      "/usr/lib/worm/software"
#define POWER       "/usr/lib/worm/power"
#define USERS_FLAG  "/etc/wh/users-may-install"
#define NO_ROOT     "/etc/worm/no-root"
#define CSS         "/usr/share/wormos/software/software.css"
#define MAX_RESULTS 150

/* Programs people often look for first, all from Debian. */
static const struct popular {
	const char *package, *title, *what, *icon;
} popular[] = {
	{ "thunar", "Thunar", "Files and folders in a window", "system-file-manager" },
	{ "mousepad", "Mousepad", "A simple text editor", "accessories-text-editor" },
	{ "ristretto", "Ristretto", "Look at photos and images", "image-viewer" },
	{ "vlc", "VLC", "Play videos and music", "multimedia-player" },
	{ "evince", "Document Viewer", "Read PDF documents", "x-office-document" },
	{ "libreoffice", "LibreOffice", "Documents, spreadsheets and presentations", "x-office-spreadsheet" },
	{ "gimp", "GIMP", "Edit photos and draw", "applications-graphics" },
	{ "keepassxc", "KeePassXC", "Keep your passwords safe", "dialog-password" },
	{ "thunderbird", "Thunderbird", "Email", "internet-mail" },
	{ "file-roller", "Archive Manager", "Open and make zip and other archives", "package-x-generic" },
	{ "geany", "Geany", "A light editor for programmers", "applications-development" },
	{ "meld", "Meld", "Compare files and folders", "edit-copy" },
	{ "gnome-calculator", "Calculator", "Do sums", "accessories-calculator" },
	{ "audacity", "Audacity", "Record and edit sound", "audio-x-generic" },
};

struct result {
	char *name, *version, *source, *description;
	gboolean installed;
};

static GtkWidget *window, *stack, *search, *explore_list, *explore_status;
static GtkWidget *installed_list, *installed_status, *updates_list, *updates_status;
static GtkWidget *updates_button, *check_button, *banner, *banner_label;
static GtkWidget *toast_revealer, *toast_label;
static GHashTable *installed;           /* name -> source ("core", "debian", ...) */
static guint search_timer, toast_timer;
static GCancellable *search_cancel;
static int busy;                        /* a change is running */

static void load_installed(void);
static void run_search(void);

/* ---- small things ---- */

static gboolean needs_password(void)
{
	return access(USERS_FLAG, F_OK) != 0;
}

static gboolean hide_toast(gpointer data)
{
	(void)data;
	gtk_revealer_set_reveal_child(GTK_REVEALER(toast_revealer), FALSE);
	toast_timer = 0;
	return G_SOURCE_REMOVE;
}

static void toast(const char *text)
{
	gtk_label_set_text(GTK_LABEL(toast_label), text);
	gtk_revealer_set_reveal_child(GTK_REVEALER(toast_revealer), TRUE);
	if (toast_timer)
		g_source_remove(toast_timer);
	toast_timer = g_timeout_add_seconds(6, hide_toast, NULL);
}

static const char *source_label(const char *source)
{
	if (!strcmp(source, "core"))
		return "WormOS";
	if (!strcmp(source, "debian"))
		return "Debian";
	if (!strcmp(source, "kali"))
		return "Kali";
	return source;
}

static void clear_list(GtkWidget *list)
{
	GtkWidget *child;

	while ((child = gtk_widget_get_first_child(list)))
		gtk_list_box_remove(GTK_LIST_BOX(list), child);
}

/* The last meaningful line of wh's output, for error messages. */
static char *last_line(const char *text)
{
	char **lines = g_strsplit(text, "\n", -1), *found = NULL;

	for (int i = g_strv_length(lines) - 1; i >= 0 && !found; i--) {
		char *line = g_strstrip(lines[i]);

		if (*line)
			found = g_strdup(g_str_has_prefix(line, "wh: ") ? line + 4 :
					 g_str_has_prefix(line, "software: ") ? line + 10 : line);
	}
	g_strfreev(lines);
	return found ? found : g_strdup("Something went wrong.");
}

/* ---- running the helper ---- */

typedef void (*done_fn)(int status, const char *out, gpointer data);

struct job {
	done_fn done;
	gpointer data;
	GString *out;
	GDataInputStream *in;
	GSubprocess *proc;
	GtkTextBuffer *log;     /* lines go here as they come, if set */
	GCancellable *cancel;
};

static void job_exited(GObject *source, GAsyncResult *res, gpointer data)
{
	struct job *job = data;
	int status = -1;

	if (g_subprocess_wait_finish(G_SUBPROCESS(source), res, NULL) &&
	    g_subprocess_get_if_exited(G_SUBPROCESS(source)))
		status = g_subprocess_get_exit_status(G_SUBPROCESS(source));
	if (!job->cancel || !g_cancellable_is_cancelled(job->cancel))
		job->done(status, job->out->str, job->data);
	g_string_free(job->out, TRUE);
	g_object_unref(job->in);
	g_object_unref(job->proc);
	g_clear_object(&job->cancel);
	g_free(job);
}

static void job_line(GObject *source, GAsyncResult *res, gpointer data)
{
	struct job *job = data;
	gsize len;
	char *line = g_data_input_stream_read_line_finish_utf8(G_DATA_INPUT_STREAM(source), res,
								&len, NULL);

	if (!line) {
		g_subprocess_wait_async(job->proc, NULL, job_exited, job);
		return;
	}
	g_string_append(job->out, line);
	g_string_append_c(job->out, '\n');
	if (job->log) {
		GtkTextIter end;

		gtk_text_buffer_get_end_iter(job->log, &end);
		gtk_text_buffer_insert(job->log, &end, line, -1);
		gtk_text_buffer_insert(job->log, &end, "\n", 1);
	}
	g_free(line);
	g_data_input_stream_read_line_async(job->in, G_PRIORITY_DEFAULT, NULL, job_line, job);
}

/*
 * Run the helper with args (a NULL-terminated list); password, if any, is
 * its first line of input. Output and errors arrive together.
 */
static void run_helper(const char *const *args, const char *password, GtkTextBuffer *log,
		       GCancellable *cancel, done_fn done, gpointer data)
{
	GPtrArray *argv = g_ptr_array_new();
	GError *error = NULL;
	GSubprocess *proc;
	struct job *job;

	g_ptr_array_add(argv, HELPER);
	for (int i = 0; args[i]; i++)
		g_ptr_array_add(argv, (gpointer)args[i]);
	g_ptr_array_add(argv, NULL);
	proc = g_subprocess_newv((const char *const *)argv->pdata,
				 G_SUBPROCESS_FLAGS_STDIN_PIPE | G_SUBPROCESS_FLAGS_STDOUT_PIPE |
				 G_SUBPROCESS_FLAGS_STDERR_MERGE, &error);
	g_ptr_array_free(argv, TRUE);
	if (!proc) {
		done(-1, error->message, data);
		g_error_free(error);
		return;
	}
	{
		GOutputStream *in = g_subprocess_get_stdin_pipe(proc);

		if (password) {
			char *line = g_strconcat(password, "\n", NULL);

			g_output_stream_write_all(in, line, strlen(line), NULL, NULL, NULL);
			explicit_bzero(line, strlen(line));
			g_free(line);
		}
		g_output_stream_close(in, NULL, NULL);
	}
	job = g_new0(struct job, 1);
	job->done = done;
	job->data = data;
	job->out = g_string_new("");
	job->proc = proc;
	job->log = log;
	job->cancel = cancel ? g_object_ref(cancel) : NULL;
	job->in = g_data_input_stream_new(g_subprocess_get_stdout_pipe(proc));
	g_data_input_stream_read_line_async(job->in, G_PRIORITY_DEFAULT, NULL, job_line, job);
}

/* ---- changes: password, progress, result ---- */

struct change {
	char **args;            /* for the helper */
	char *what;             /* "Installing VLC" */
	char *done_text;        /* "VLC is installed." */
	GtkWidget *dialog, *heading, *text, *entry, *error, *spinner, *buttons, *ok, *log_view;
	gboolean restart;       /* the change waits for a restart */
};

static void change_free(struct change *c)
{
	g_strfreev(c->args);
	g_free(c->what);
	g_free(c->done_text);
	g_free(c);
}

static void show_restart_banner(void)
{
	gtk_label_set_text(GTK_LABEL(banner_label), "Restart to finish: WormOS packages are "
			   "installed at the next restart, so your windows stay open until then.");
	gtk_widget_set_visible(banner, TRUE);
}

static void restart_now(GtkButton *button, gpointer data)
{
	(void)button;
	(void)data;
	g_spawn_async(NULL, (char *[]){ POWER, "reboot", NULL }, NULL, 0, NULL, NULL, NULL, NULL);
}

static void dialog_close(GtkButton *button, gpointer data)
{
	struct change *c = data;

	(void)button;
	gtk_window_destroy(GTK_WINDOW(c->dialog));
	change_free(c);
}

static void change_done(int status, const char *out, gpointer data)
{
	struct change *c = data;
	GtkWidget *child;

	busy--;
	gtk_widget_set_visible(c->spinner, FALSE);
	gtk_spinner_stop(GTK_SPINNER(c->spinner));
	if (status == 3) {
		/* Wrong password: ask again. */
		gtk_widget_set_visible(c->entry, TRUE);
		gtk_widget_set_visible(c->buttons, TRUE);
		gtk_label_set_text(GTK_LABEL(c->error), "Wrong password. Try again.");
		gtk_label_set_text(GTK_LABEL(c->heading), c->what);
		gtk_editable_set_text(GTK_EDITABLE(c->entry), "");
		gtk_widget_grab_focus(c->entry);
		return;
	}
	while ((child = gtk_widget_get_first_child(c->buttons)))
		gtk_box_remove(GTK_BOX(c->buttons), child);
	if (status == 0) {
		c->restart = strstr(out, "applied at the next restart") != NULL;
		gtk_label_set_text(GTK_LABEL(c->heading), c->restart ? "Ready after a restart" :
				   "Done");
		gtk_label_set_text(GTK_LABEL(c->text), c->restart ?
				   "This is a WormOS package: it is installed into the system at "
				   "the next restart, so nothing closes now. Restart when it suits "
				   "you." : c->done_text);
		if (c->restart) {
			GtkWidget *later = gtk_button_new_with_label("Later"),
				  *now = gtk_button_new_with_label("Restart now");

			gtk_widget_add_css_class(now, "suggested-action");
			g_signal_connect(later, "clicked", G_CALLBACK(dialog_close), c);
			g_signal_connect(now, "clicked", G_CALLBACK(restart_now), NULL);
			gtk_box_append(GTK_BOX(c->buttons), later);
			gtk_box_append(GTK_BOX(c->buttons), now);
			show_restart_banner();
		} else {
			GtkWidget *close = gtk_button_new_with_label("Close");

			gtk_widget_add_css_class(close, "suggested-action");
			g_signal_connect(close, "clicked", G_CALLBACK(dialog_close), c);
			gtk_box_append(GTK_BOX(c->buttons), close);
		}
		toast(c->restart ? "Ready: restart to finish." : c->done_text);
	} else {
		char *line = last_line(out);
		GtkWidget *close = gtk_button_new_with_label("Close");

		gtk_label_set_text(GTK_LABEL(c->heading), "That did not work");
		gtk_label_set_text(GTK_LABEL(c->text), line);
		g_signal_connect(close, "clicked", G_CALLBACK(dialog_close), c);
		gtk_box_append(GTK_BOX(c->buttons), close);
		g_free(line);
	}
	gtk_widget_set_visible(c->buttons, TRUE);
	load_installed();
}

static void change_start(GtkWidget *widget, gpointer data)
{
	struct change *c = data;
	const char *password = needs_password() ?
			       gtk_editable_get_text(GTK_EDITABLE(c->entry)) : NULL;

	(void)widget;
	if (password && !*password) {
		gtk_label_set_text(GTK_LABEL(c->error), "Type the root password.");
		return;
	}
	gtk_label_set_text(GTK_LABEL(c->error), "");
	gtk_label_set_text(GTK_LABEL(c->heading), c->what);
	gtk_label_set_text(GTK_LABEL(c->text), "This can take a few minutes: the first program "
			   "from Debian or Kali sets up its source.");
	gtk_widget_set_visible(c->entry, FALSE);
	gtk_widget_set_visible(c->buttons, FALSE);
	gtk_widget_set_visible(c->spinner, TRUE);
	gtk_spinner_start(GTK_SPINNER(c->spinner));
	busy++;
	run_helper((const char *const *)c->args, password,
		   gtk_text_view_get_buffer(GTK_TEXT_VIEW(c->log_view)), NULL, change_done, c);
	if (password)
		gtk_editable_set_text(GTK_EDITABLE(c->entry), "");
}

/*
 * Ask (the root password where there is one), then run the change and
 * show how it goes. Takes ownership of args.
 */
static void change(const char *title, const char *text, const char *button, gboolean destructive,
		   char **args, const char *what, const char *done_text)
{
	struct change *c = g_new0(struct change, 1);
	GtkWidget *box, *cancel, *expander, *scroll;

	if (access(NO_ROOT, F_OK) == 0 && needs_password()) {
		toast("Root is locked on this system: programs cannot be installed.");
		g_strfreev(args);
		g_free(c);
		return;
	}
	c->args = args;
	c->what = g_strdup(what);
	c->done_text = g_strdup(done_text);
	c->dialog = gtk_window_new();
	gtk_window_set_title(GTK_WINDOW(c->dialog), title);
	gtk_window_set_transient_for(GTK_WINDOW(c->dialog), GTK_WINDOW(window));
	gtk_window_set_modal(GTK_WINDOW(c->dialog), TRUE);
	gtk_window_set_default_size(GTK_WINDOW(c->dialog), 460, -1);
	gtk_window_set_deletable(GTK_WINDOW(c->dialog), FALSE);

	box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
	gtk_widget_add_css_class(box, "dialog");
	c->heading = gtk_label_new(title);
	gtk_widget_add_css_class(c->heading, "title-2");
	gtk_label_set_xalign(GTK_LABEL(c->heading), 0);
	gtk_box_append(GTK_BOX(box), c->heading);
	c->text = gtk_label_new(text);
	gtk_label_set_wrap(GTK_LABEL(c->text), TRUE);
	gtk_label_set_max_width_chars(GTK_LABEL(c->text), 50);
	gtk_label_set_xalign(GTK_LABEL(c->text), 0);
	gtk_box_append(GTK_BOX(box), c->text);
	c->spinner = gtk_spinner_new();
	gtk_widget_set_size_request(c->spinner, 32, 32);
	gtk_widget_set_visible(c->spinner, FALSE);
	gtk_box_append(GTK_BOX(box), c->spinner);

	c->entry = gtk_password_entry_new();
	gtk_password_entry_set_show_peek_icon(GTK_PASSWORD_ENTRY(c->entry), TRUE);
	g_object_set(c->entry, "placeholder-text", "Root password", "activates-default", TRUE,
		     NULL);
	g_signal_connect(c->entry, "activate", G_CALLBACK(change_start), c);
	gtk_widget_set_visible(c->entry, needs_password());
	gtk_box_append(GTK_BOX(box), c->entry);
	c->error = gtk_label_new("");
	gtk_widget_add_css_class(c->error, "error");
	gtk_label_set_xalign(GTK_LABEL(c->error), 0);
	gtk_box_append(GTK_BOX(box), c->error);

	/* What wh says, for the curious. */
	c->log_view = gtk_text_view_new();
	gtk_text_view_set_editable(GTK_TEXT_VIEW(c->log_view), FALSE);
	gtk_text_view_set_monospace(GTK_TEXT_VIEW(c->log_view), TRUE);
	gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(c->log_view), GTK_WRAP_WORD_CHAR);
	scroll = gtk_scrolled_window_new();
	gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(scroll), 160);
	gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), c->log_view);
	expander = gtk_expander_new("Details");
	gtk_expander_set_child(GTK_EXPANDER(expander), scroll);
	gtk_box_append(GTK_BOX(box), expander);

	c->buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	gtk_widget_set_halign(c->buttons, GTK_ALIGN_END);
	cancel = gtk_button_new_with_label("Cancel");
	g_signal_connect(cancel, "clicked", G_CALLBACK(dialog_close), c);
	c->ok = gtk_button_new_with_label(button);
	gtk_widget_add_css_class(c->ok, destructive ? "destructive-action" : "suggested-action");
	g_signal_connect(c->ok, "clicked", G_CALLBACK(change_start), c);
	gtk_box_append(GTK_BOX(c->buttons), cancel);
	gtk_box_append(GTK_BOX(c->buttons), c->ok);
	gtk_box_append(GTK_BOX(box), c->buttons);
	gtk_window_set_child(GTK_WINDOW(c->dialog), box);
	gtk_window_set_default_widget(GTK_WINDOW(c->dialog), c->ok);
	gtk_window_present(GTK_WINDOW(c->dialog));
	if (needs_password())
		gtk_widget_grab_focus(c->entry);
}

static void install_clicked(GtkButton *button, gpointer data)
{
	const char *name = g_object_get_data(G_OBJECT(button), "package");
	const char *source = g_object_get_data(G_OBJECT(button), "source");
	const char *title = g_object_get_data(G_OBJECT(button), "title");
	char *text, *what, *done_text, **args;

	(void)data;
	if (source && strcmp(source, "core"))
		args = g_strdupv((char *[]){ "install", "--from", (char *)source, (char *)name, NULL });
	else
		args = g_strdupv((char *[]){ "install", (char *)name, NULL });
	text = g_strdup_printf("%s comes from %s%s. %s", title, source_label(source ? source : "core"),
			       source && strcmp(source, "core") ? ", in its own wormhole" : "",
			       needs_password() ? "Installing programs needs the administrator "
			       "(root) password." : "It stays until the computer stops.");
	what = g_strdup_printf("Installing %s...", title);
	done_text = g_strdup_printf("%s is installed: find it in the launcher.", title);
	change("Install a program", text, "Install", FALSE, args, what, done_text);
	g_free(text);
	g_free(what);
	g_free(done_text);
}

static void remove_clicked(GtkButton *button, gpointer data)
{
	const char *name = g_object_get_data(G_OBJECT(button), "package");
	const char *title = g_object_get_data(G_OBJECT(button), "title");
	char *text, *what, *done_text;

	(void)data;
	text = g_strdup_printf("%s and the files it installed are removed; your own files stay.%s",
			       title, needs_password() ? " This needs the administrator (root) "
			       "password." : "");
	what = g_strdup_printf("Removing %s...", title);
	done_text = g_strdup_printf("%s is removed.", title);
	change("Remove a program", text, "Remove", TRUE,
	       g_strdupv((char *[]){ "remove", (char *)name, NULL }), what, done_text);
	g_free(text);
	g_free(what);
	g_free(done_text);
}

/* ---- rows ---- */

static GtkWidget *program_row(const char *name, const char *title, const char *description,
			      const char *source, const char *icon, const char *version)
{
	GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 14), *text, *label, *line, *button;
	const char *have = g_hash_table_lookup(installed, name);
	char *markup;

	gtk_widget_add_css_class(row, "program");
	label = gtk_image_new_from_icon_name(icon ? icon : "application-x-executable");
	gtk_image_set_pixel_size(GTK_IMAGE(label), 40);
	gtk_box_append(GTK_BOX(row), label);

	text = gtk_box_new(GTK_ORIENTATION_VERTICAL, 3);
	gtk_widget_set_hexpand(text, TRUE);
	gtk_widget_set_valign(text, GTK_ALIGN_CENTER);
	line = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	label = gtk_label_new(title);
	gtk_widget_add_css_class(label, "heading");
	gtk_label_set_xalign(GTK_LABEL(label), 0);
	gtk_box_append(GTK_BOX(line), label);
	label = gtk_label_new(source_label(source));
	gtk_widget_add_css_class(label, "badge");
	if (!strcmp(source, "core"))
		gtk_widget_add_css_class(label, "accent");
	gtk_box_append(GTK_BOX(line), label);
	gtk_box_append(GTK_BOX(text), line);
	markup = version && *version ? g_markup_printf_escaped("%s  <span alpha='60%%'>%s %s</span>",
							       description, name, version)
				     : g_markup_printf_escaped("%s", description);
	label = gtk_label_new(NULL);
	gtk_label_set_markup(GTK_LABEL(label), markup);
	g_free(markup);
	gtk_widget_add_css_class(label, "dim");
	gtk_label_set_xalign(GTK_LABEL(label), 0);
	gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
	gtk_box_append(GTK_BOX(text), label);
	gtk_box_append(GTK_BOX(row), text);

	button = gtk_button_new_with_label(have ? "Remove" : "Install");
	gtk_widget_set_valign(button, GTK_ALIGN_CENTER);
	gtk_widget_add_css_class(button, have ? "flat" : "suggested-action");
	g_object_set_data_full(G_OBJECT(button), "package", g_strdup(name), g_free);
	g_object_set_data_full(G_OBJECT(button), "source", g_strdup(have ? have : source), g_free);
	g_object_set_data_full(G_OBJECT(button), "title", g_strdup(title), g_free);
	g_signal_connect(button, "clicked", G_CALLBACK(have ? remove_clicked : install_clicked),
			 NULL);
	gtk_box_append(GTK_BOX(row), button);
	return row;
}

static void status_text(GtkWidget *label, const char *text)
{
	gtk_label_set_text(GTK_LABEL(label), text);
	gtk_widget_set_visible(label, text && *text);
}

/* ---- explore ---- */

static void show_popular(void)
{
	clear_list(explore_list);
	status_text(explore_status, "Popular programs. Search above for anything else: WormOS's "
		    "own packages first, then Debian's and Kali's.");
	for (size_t i = 0; i < G_N_ELEMENTS(popular); i++)
		gtk_list_box_append(GTK_LIST_BOX(explore_list),
				    program_row(popular[i].package, popular[i].title, popular[i].what,
						"debian", popular[i].icon, NULL));
}

/*
 * wh search output: "name version [installed]" (WormOS packages) or
 * "name [source]" (Debian, Kali), each followed by an indented
 * description line.
 */
static void search_done(int status, const char *out, gpointer data)
{
	char **lines = g_strsplit(out, "\n", -1), *term = data;
	int shown = 0, total = 0;

	(void)status;
	clear_list(explore_list);
	for (int i = 0; lines[i]; i++) {
		char name[128], word[128] = "", *desc;
		const char *source = "core", *version = "";

		if (!lines[i][0] || lines[i][0] == ' ' || !lines[i + 1] || lines[i + 1][0] != ' ')
			continue;
		if (sscanf(lines[i], "%127s %127s", name, word) < 1)
			continue;
		if (word[0] == '[') {
			word[strcspn(word, "]")] = '\0';
			source = word + 1;
		} else {
			version = word;
		}
		desc = g_strstrip(g_strdup(lines[i + 1]));
		total++;
		if (shown < MAX_RESULTS) {
			gtk_list_box_append(GTK_LIST_BOX(explore_list),
					    program_row(name, name, desc, source,
							!strcmp(source, "core") ? "package-x-generic" :
							"application-x-executable", version));
			shown++;
		}
		g_free(desc);
	}
	g_strfreev(lines);
	if (total == 0) {
		char *text = g_strdup_printf("Nothing found for \xe2\x80\x9c%s\xe2\x80\x9d. Try "
					     "another word, or a shorter one.", term);

		status_text(explore_status, text);
		g_free(text);
	} else if (total > shown) {
		char *text = g_strdup_printf("The first %d of %d programs: type more to narrow it "
					     "down.", shown, total);

		status_text(explore_status, text);
		g_free(text);
	} else {
		status_text(explore_status, "");
	}
	g_free(term);
}

static gboolean search_now(gpointer data)
{
	(void)data;
	search_timer = 0;
	run_search();
	return G_SOURCE_REMOVE;
}

static void run_search(void)
{
	const char *term = gtk_editable_get_text(GTK_EDITABLE(search));
	char *clean = g_strstrip(g_ascii_strdown(term, -1));

	if (search_cancel) {
		g_cancellable_cancel(search_cancel);
		g_clear_object(&search_cancel);
	}
	if (strlen(clean) < 2) {
		g_free(clean);
		show_popular();
		return;
	}
	status_text(explore_status, "Searching...");
	search_cancel = g_cancellable_new();
	run_helper((const char *const[]){ "search", clean, NULL }, NULL, NULL, search_cancel,
		   search_done, clean);
}

static void search_changed(GtkSearchEntry *entry, gpointer data)
{
	(void)entry;
	(void)data;
	if (search_timer)
		g_source_remove(search_timer);
	search_timer = g_timeout_add(450, search_now, NULL);
}

/* ---- installed ---- */

/*
 * wh list: "name version description" (WormOS packages) and
 * "name version [source]" (Debian, Kali).
 */
static void installed_done(GObject *source_object, GAsyncResult *res, gpointer data)
{
	char *out = NULL, **lines;
	GtkWidget *core_box = NULL, *expander;
	int programs = 0, core = 0;

	(void)data;
	if (!g_subprocess_communicate_utf8_finish(G_SUBPROCESS(source_object), res, &out, NULL,
						  NULL))
		out = g_strdup("");
	g_hash_table_remove_all(installed);
	clear_list(installed_list);
	lines = g_strsplit(out, "\n", -1);
	for (int i = 0; lines[i]; i++) {
		char name[128], version[128], rest[512] = "";

		if (sscanf(lines[i], "%127s %127s %511[^\n]", name, version, rest) < 2)
			continue;
		if (rest[0] == '[') {
			rest[strcspn(rest, "]")] = '\0';
			g_hash_table_insert(installed, g_strdup(name), g_strdup(rest + 1));
			gtk_list_box_append(GTK_LIST_BOX(installed_list),
					    program_row(name, name, "From a wormhole", rest + 1,
							"application-x-executable", version));
			programs++;
		} else {
			g_hash_table_insert(installed, g_strdup(name), g_strdup("core"));
			if (!core_box)
				core_box = gtk_list_box_new();
			gtk_list_box_append(GTK_LIST_BOX(core_box),
					    program_row(name, name, rest, "core", "package-x-generic",
							version));
			core++;
		}
	}
	g_strfreev(lines);
	g_free(out);
	if (!programs) {
		GtkWidget *label = gtk_label_new("No programs from Debian or Kali yet: Explore has "
						 "thousands.");

		gtk_widget_add_css_class(label, "placeholder");
		gtk_list_box_append(GTK_LIST_BOX(installed_list), label);
	}
	if (core_box) {
		char *title = g_strdup_printf("WormOS packages (%d): the system itself", core);

		gtk_widget_add_css_class(core_box, "programs");
		gtk_list_box_set_selection_mode(GTK_LIST_BOX(core_box), GTK_SELECTION_NONE);
		expander = gtk_expander_new(title);
		gtk_widget_add_css_class(expander, "section");
		gtk_expander_set_child(GTK_EXPANDER(expander), core_box);
		gtk_list_box_append(GTK_LIST_BOX(installed_list), expander);
		g_free(title);
	}
	status_text(installed_status, "Programs from Debian and Kali run in wormholes and are "
		    "ready as soon as they are installed.");
	/* The buttons in search results follow what is installed. */
	if (gtk_editable_get_text(GTK_EDITABLE(search))[0])
		run_search();
	else
		show_popular();
}

static void load_installed(void)
{
	GSubprocess *proc = g_subprocess_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE |
					     G_SUBPROCESS_FLAGS_STDERR_SILENCE, NULL,
					     "wh", "list", NULL);

	if (!proc)
		return;
	g_subprocess_communicate_utf8_async(proc, NULL, NULL, installed_done, NULL);
	g_object_unref(proc);
}

/* ---- updates ---- */

static void updates_done(int status, const char *out, gpointer data)
{
	char **lines = g_strsplit(out, "\n", -1);
	int n = 0;

	(void)data;
	gtk_widget_set_sensitive(check_button, TRUE);
	clear_list(updates_list);
	if (status != 0) {
		char *line = last_line(out);

		status_text(updates_status, line);
		g_free(line);
		g_strfreev(lines);
		return;
	}
	for (int i = 0; lines[i]; i++) {
		char name[128], old[128], new[128], *text;

		if (sscanf(lines[i], "%127s %127s %127s", name, old, new) != 3)
			continue;
		text = g_strdup_printf("%s \xe2\x86\x92 %s", old, new);
		gtk_list_box_append(GTK_LIST_BOX(updates_list),
				    program_row(name, name, text, "core", "package-x-generic", NULL));
		g_free(text);
		n++;
	}
	g_strfreev(lines);
	{
		char *text = n ? g_strdup_printf("%d update%s for WormOS. Programs from Debian and "
						 "Kali are brought up to date with them.", n,
						 n == 1 ? "" : "s")
			       : g_strdup("Everything is up to date.");

		status_text(updates_status, text);
		g_free(text);
	}
	gtk_widget_set_visible(updates_button, n > 0);
	/* The rows' buttons are not wanted here: updating is all or nothing. */
	for (GtkWidget *row = gtk_widget_get_first_child(updates_list); row;
	     row = gtk_widget_get_next_sibling(row)) {
		GtkWidget *box = gtk_list_box_row_get_child(GTK_LIST_BOX_ROW(row));

		if (box)
			gtk_widget_set_visible(gtk_widget_get_last_child(box), FALSE);
	}
}

static void refreshed(int status, const char *out, gpointer data)
{
	(void)data;
	if (status != 0) {
		char *line = last_line(out);

		gtk_widget_set_sensitive(check_button, TRUE);
		status_text(updates_status, line);
		g_free(line);
		return;
	}
	run_helper((const char *const[]){ "updates", NULL }, NULL, NULL, NULL, updates_done, NULL);
}

static void check_updates(GtkButton *button, gpointer data)
{
	(void)button;
	(void)data;
	gtk_widget_set_sensitive(check_button, FALSE);
	gtk_widget_set_visible(updates_button, FALSE);
	status_text(updates_status, "Looking for updates...");
	run_helper((const char *const[]){ "refresh", NULL }, NULL, NULL, NULL, refreshed, NULL);
}

static void update_all(GtkButton *button, gpointer data)
{
	(void)button;
	(void)data;
	change("Install updates", needs_password() ?
	       "Every update is installed at once. This needs the administrator (root) password."
	       : "Every update is installed at once.", "Update",
	       FALSE, g_strdupv((char *[]){ "upgrade", NULL }), "Installing updates...",
	       "Everything is up to date.");
}

/* ---- the window ---- */

static GtkWidget *page(GtkWidget **list, GtkWidget **status, GtkWidget *top)
{
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10), *scroll, *clamp;

	gtk_widget_add_css_class(box, "page");
	if (top)
		gtk_box_append(GTK_BOX(box), top);
	*status = gtk_label_new("");
	gtk_widget_add_css_class(*status, "dim");
	gtk_label_set_wrap(GTK_LABEL(*status), TRUE);
	gtk_label_set_xalign(GTK_LABEL(*status), 0);
	gtk_box_append(GTK_BOX(box), *status);
	*list = gtk_list_box_new();
	gtk_widget_add_css_class(*list, "programs");
	gtk_list_box_set_selection_mode(GTK_LIST_BOX(*list), GTK_SELECTION_NONE);
	gtk_box_append(GTK_BOX(box), *list);
	clamp = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_widget_set_halign(box, GTK_ALIGN_FILL);
	gtk_box_append(GTK_BOX(clamp), box);
	scroll = gtk_scrolled_window_new();
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER,
				       GTK_POLICY_AUTOMATIC);
	gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), clamp);
	gtk_widget_set_vexpand(scroll, TRUE);
	return scroll;
}

static gboolean close_request(GtkWindow *win, gpointer data)
{
	(void)win;
	(void)data;
	if (busy) {
		toast("Wait until the change has finished.");
		return TRUE;
	}
	return FALSE;
}

static void activate(GtkApplication *app, gpointer data)
{
	GtkWidget *header, *switcher, *box, *overlay, *toast_box, *restart, *top;
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
	installed = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);

	window = gtk_application_window_new(app);
	gtk_window_set_title(GTK_WINDOW(window), "Software");
	gtk_window_set_default_size(GTK_WINDOW(window), 860, 640);
	g_signal_connect(window, "close-request", G_CALLBACK(close_request), NULL);
	stack = gtk_stack_new();
	gtk_stack_set_transition_type(GTK_STACK(stack), GTK_STACK_TRANSITION_TYPE_CROSSFADE);

	search = gtk_search_entry_new();
	g_object_set(search, "placeholder-text", "Search for programs, such as \xe2\x80\x9cvideo\xe2\x80\x9d"
		     " or \xe2\x80\x9cgimp\xe2\x80\x9d", NULL);
	gtk_widget_set_hexpand(search, TRUE);
	g_signal_connect(search, "search-changed", G_CALLBACK(search_changed), NULL);
	gtk_stack_add_titled(GTK_STACK(stack), page(&explore_list, &explore_status, search),
			     "explore", "Explore");
	gtk_stack_add_titled(GTK_STACK(stack), page(&installed_list, &installed_status, NULL),
			     "installed", "Installed");

	top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	check_button = gtk_button_new_with_label("Check for updates");
	g_signal_connect(check_button, "clicked", G_CALLBACK(check_updates), NULL);
	updates_button = gtk_button_new_with_label("Update all");
	gtk_widget_add_css_class(updates_button, "suggested-action");
	gtk_widget_set_visible(updates_button, FALSE);
	g_signal_connect(updates_button, "clicked", G_CALLBACK(update_all), NULL);
	gtk_box_append(GTK_BOX(top), check_button);
	gtk_box_append(GTK_BOX(top), updates_button);
	gtk_stack_add_titled(GTK_STACK(stack), page(&updates_list, &updates_status, top),
			     "updates", "Updates");

	header = gtk_header_bar_new();
	switcher = gtk_stack_switcher_new();
	gtk_stack_switcher_set_stack(GTK_STACK_SWITCHER(switcher), GTK_STACK(stack));
	gtk_header_bar_set_title_widget(GTK_HEADER_BAR(header), switcher);
	gtk_window_set_titlebar(GTK_WINDOW(window), header);

	/* Shown once a change waits for a restart. */
	banner = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
	gtk_widget_add_css_class(banner, "banner");
	banner_label = gtk_label_new("");
	gtk_label_set_wrap(GTK_LABEL(banner_label), TRUE);
	gtk_label_set_xalign(GTK_LABEL(banner_label), 0);
	gtk_widget_set_hexpand(banner_label, TRUE);
	restart = gtk_button_new_with_label("Restart now");
	gtk_widget_set_valign(restart, GTK_ALIGN_CENTER);
	g_signal_connect(restart, "clicked", G_CALLBACK(restart_now), NULL);
	gtk_box_append(GTK_BOX(banner), banner_label);
	gtk_box_append(GTK_BOX(banner), restart);
	gtk_widget_set_visible(banner, FALSE);

	box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_box_append(GTK_BOX(box), banner);
	gtk_box_append(GTK_BOX(box), stack);
	overlay = gtk_overlay_new();
	gtk_overlay_set_child(GTK_OVERLAY(overlay), box);
	toast_revealer = gtk_revealer_new();
	gtk_revealer_set_transition_type(GTK_REVEALER(toast_revealer),
					 GTK_REVEALER_TRANSITION_TYPE_SLIDE_UP);
	gtk_widget_set_valign(toast_revealer, GTK_ALIGN_END);
	gtk_widget_set_halign(toast_revealer, GTK_ALIGN_CENTER);
	toast_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	gtk_widget_add_css_class(toast_box, "toast");
	toast_label = gtk_label_new("");
	gtk_label_set_wrap(GTK_LABEL(toast_label), TRUE);
	gtk_box_append(GTK_BOX(toast_box), toast_label);
	gtk_revealer_set_child(GTK_REVEALER(toast_revealer), toast_box);
	gtk_overlay_add_overlay(GTK_OVERLAY(overlay), toast_revealer);
	gtk_window_set_child(GTK_WINDOW(window), overlay);

	/* wormos-software --updates opens on the Updates page (the notice). */
	if (g_object_get_data(G_OBJECT(app), "updates")) {
		gtk_stack_set_visible_child_name(GTK_STACK(stack), "updates");
		check_updates(NULL, NULL);
	}
	gtk_window_present(GTK_WINDOW(window));
	gtk_widget_grab_focus(search);
	load_installed();
}

int main(int argc, char **argv)
{
	GtkApplication *app = gtk_application_new(APP_ID, G_APPLICATION_DEFAULT_FLAGS);
	int status;

	if (argc > 1 && !strcmp(argv[1], "--updates")) {
		g_object_set_data(G_OBJECT(app), "updates", GINT_TO_POINTER(1));
		argc = 1;
	}
	g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);
	status = g_application_run(G_APPLICATION(app), argc, argv);
	g_object_unref(app);
	return status;
}
