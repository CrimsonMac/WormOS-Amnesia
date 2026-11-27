/*
 * wormos-editor - Text Editor: plain text files.
 *
 * New, open, save and save as (Ctrl+N, Ctrl+O, Ctrl+S, Ctrl+Shift+S),
 * undo and redo (Ctrl+Z, Ctrl+Shift+Z), find (Ctrl+F; Enter and
 * Shift+Enter for the next and previous match), go to a line (Ctrl+L),
 * word wrap and a monospace font from the menu. The title shows the file
 * and a dot while it has unsaved changes; closing asks first. Files are
 * written whole to a temporary file next to them and moved over the old
 * one, so a crash never leaves half a file.
 *
 *     wormos-editor [FILE...]
 */

#define _GNU_SOURCE
#include <gtk/gtk.h>
#include <string.h>

#define APP_ID "org.wormos.Editor"
#define MAX_FILE (64 << 20)

struct win {
	GtkWidget *window, *view, *title, *subtitle, *position, *findbar, *find, *find_status;
	GtkTextBuffer *buffer;
	GFile *file;
	gboolean closing;       /* close once saved */
};

static struct win *new_window(GtkApplication *app);

/* ---- the title ---- */

static void update_title(struct win *w)
{
	gboolean modified = gtk_text_buffer_get_modified(w->buffer);
	char *name = w->file ? g_file_get_basename(w->file) : g_strdup("New document");
	char *title = g_strdup_printf("%s%s", modified ? "• " : "", name);
	char *dir = NULL;

	if (w->file) {
		GFile *parent = g_file_get_parent(w->file);
		char *path = parent ? g_file_get_path(parent) : NULL;
		const char *home = g_get_home_dir();

		dir = path && g_str_has_prefix(path, home) ? g_strconcat("~", path + strlen(home), NULL)
							   : g_strdup(path ? path : "");
		g_free(path);
		g_clear_object(&parent);
	}
	gtk_window_set_title(GTK_WINDOW(w->window), title);
	gtk_label_set_text(GTK_LABEL(w->title), title);
	gtk_label_set_text(GTK_LABEL(w->subtitle), dir ? dir : "Not saved yet");
	g_free(name);
	g_free(title);
	g_free(dir);
}

static void modified_changed(GtkTextBuffer *b, gpointer data) { (void)b; update_title(data); }

static void cursor_moved(GObject *o, GParamSpec *p, gpointer data)
{
	struct win *w = data;
	GtkTextIter it;
	char text[64];

	(void)o; (void)p;
	gtk_text_buffer_get_iter_at_mark(w->buffer, &it, gtk_text_buffer_get_insert(w->buffer));
	snprintf(text, sizeof(text), "Line %d, column %d", gtk_text_iter_get_line(&it) + 1,
		 gtk_text_iter_get_line_offset(&it) + 1);
	gtk_label_set_text(GTK_LABEL(w->position), text);
}

static void error_dialog(struct win *w, const char *what, const char *detail)
{
	GtkAlertDialog *d = gtk_alert_dialog_new("%s", what);

	gtk_alert_dialog_set_detail(d, detail);
	gtk_alert_dialog_show(d, GTK_WINDOW(w->window));
	g_object_unref(d);
}

/* ---- loading and saving ---- */

static gboolean load(struct win *w, GFile *file)
{
	char *data = NULL;
	gsize len = 0;
	GError *error = NULL;
	GFileInfo *info = g_file_query_info(file, G_FILE_ATTRIBUTE_STANDARD_SIZE, 0, NULL, NULL);
	GtkTextIter start;

	if (info && g_file_info_get_size(info) > MAX_FILE) {
		error_dialog(w, "This file is too big for Text Editor", "It opens files up to 64 MB.");
		g_object_unref(info);
		return FALSE;
	}
	g_clear_object(&info);
	if (!g_file_load_contents(file, NULL, &data, &len, NULL, &error)) {
		/* A file that does not exist yet: start it empty. */
		if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND)) {
			error_dialog(w, "Cannot open the file", error->message);
			g_error_free(error);
			return FALSE;
		}
		g_clear_error(&error);
		data = g_strdup("");
	}
	if (!g_utf8_validate(data, len, NULL)) {
		error_dialog(w, "This is not a text file",
			     "Text Editor opens UTF-8 text; this file holds other data.");
		g_free(data);
		return FALSE;
	}
	gtk_text_buffer_begin_irreversible_action(w->buffer);
	gtk_text_buffer_set_text(w->buffer, data, (int)len);
	gtk_text_buffer_end_irreversible_action(w->buffer);
	gtk_text_buffer_get_start_iter(w->buffer, &start);
	gtk_text_buffer_place_cursor(w->buffer, &start);
	gtk_text_buffer_set_modified(w->buffer, FALSE);
	g_set_object(&w->file, file);
	g_free(data);
	update_title(w);
	return TRUE;
}

static gboolean save_to(struct win *w, GFile *file)
{
	GtkTextIter start, end;
	char *text;
	GError *error = NULL;
	gboolean ok;

	gtk_text_buffer_get_bounds(w->buffer, &start, &end);
	text = gtk_text_buffer_get_text(w->buffer, &start, &end, FALSE);
	/* GIO writes a temporary file and renames it over the old one. */
	ok = g_file_replace_contents(file, text, strlen(text), NULL, FALSE, G_FILE_CREATE_NONE,
				     NULL, NULL, &error);
	g_free(text);
	if (!ok) {
		error_dialog(w, "Cannot save the file", error->message);
		g_error_free(error);
		return FALSE;
	}
	g_set_object(&w->file, file);
	gtk_text_buffer_set_modified(w->buffer, FALSE);
	update_title(w);
	if (w->closing)
		gtk_window_destroy(GTK_WINDOW(w->window));
	return TRUE;
}

static void saved_as(GObject *source, GAsyncResult *res, gpointer data)
{
	struct win *w = data;
	GFile *file = gtk_file_dialog_save_finish(GTK_FILE_DIALOG(source), res, NULL);

	if (file) {
		save_to(w, file);
		g_object_unref(file);
	} else {
		w->closing = FALSE;
	}
}

static void save_as(struct win *w)
{
	GtkFileDialog *dialog = gtk_file_dialog_new();

	if (w->file) {
		gtk_file_dialog_set_initial_file(dialog, w->file);
	} else {
		const char *docs = g_get_user_special_dir(G_USER_DIRECTORY_DOCUMENTS);
		GFile *dir = g_file_new_for_path(docs ? docs : g_get_home_dir());

		gtk_file_dialog_set_initial_folder(dialog, dir);
		gtk_file_dialog_set_initial_name(dialog, "Untitled.txt");
		g_object_unref(dir);
	}
	gtk_file_dialog_save(dialog, GTK_WINDOW(w->window), NULL, saved_as, w);
	g_object_unref(dialog);
}

static void save(struct win *w)
{
	if (w->file)
		save_to(w, w->file);
	else
		save_as(w);
}

static void opened(GObject *source, GAsyncResult *res, gpointer data)
{
	struct win *w = data;
	GListModel *files = gtk_file_dialog_open_multiple_finish(GTK_FILE_DIALOG(source), res, NULL);

	if (!files)
		return;
	for (guint i = 0; i < g_list_model_get_n_items(files); i++) {
		GFile *file = g_list_model_get_item(files, i);
		/* An empty, unchanged window takes the first file. */
		struct win *target = !w->file && !gtk_text_buffer_get_modified(w->buffer) &&
				     gtk_text_buffer_get_char_count(w->buffer) == 0 && i == 0 ? w :
				     new_window(gtk_window_get_application(GTK_WINDOW(w->window)));

		if (!load(target, file) && target != w)
			gtk_window_destroy(GTK_WINDOW(target->window));
		g_object_unref(file);
	}
	g_object_unref(files);
}

/* ---- finding ---- */

static void find_from(struct win *w, gboolean backwards)
{
	const char *needle = gtk_editable_get_text(GTK_EDITABLE(w->find));
	GtkTextIter cursor, start, end, ms, me;
	gboolean found;

	if (!*needle) {
		gtk_label_set_text(GTK_LABEL(w->find_status), "");
		return;
	}
	gtk_text_buffer_get_selection_bounds(w->buffer, &start, &end);
	cursor = backwards ? start : end;
	found = backwards ? gtk_text_iter_backward_search(&cursor, needle, GTK_TEXT_SEARCH_CASE_INSENSITIVE,
							  &ms, &me, NULL)
			  : gtk_text_iter_forward_search(&cursor, needle, GTK_TEXT_SEARCH_CASE_INSENSITIVE,
							 &ms, &me, NULL);
	if (!found) {
		/* Wrap around once. */
		if (backwards)
			gtk_text_buffer_get_end_iter(w->buffer, &cursor);
		else
			gtk_text_buffer_get_start_iter(w->buffer, &cursor);
		found = backwards ? gtk_text_iter_backward_search(&cursor, needle,
								  GTK_TEXT_SEARCH_CASE_INSENSITIVE, &ms, &me, NULL)
				  : gtk_text_iter_forward_search(&cursor, needle,
								 GTK_TEXT_SEARCH_CASE_INSENSITIVE, &ms, &me, NULL);
	}
	gtk_label_set_text(GTK_LABEL(w->find_status), found ? "" : "Not found");
	if (found) {
		gtk_text_buffer_select_range(w->buffer, &ms, &me);
		gtk_text_view_scroll_to_mark(GTK_TEXT_VIEW(w->view), gtk_text_buffer_get_insert(w->buffer),
					     0.2, FALSE, 0, 0);
	}
}

static void find_changed(GtkSearchEntry *e, gpointer data)
{
	struct win *w = data;
	GtkTextIter start;

	(void)e;
	/* Search again from where the selection starts. */
	if (gtk_text_buffer_get_selection_bounds(w->buffer, &start, NULL))
		gtk_text_buffer_place_cursor(w->buffer, &start);
	find_from(w, FALSE);
}

static void find_next(GtkSearchEntry *e, gpointer d) { (void)e; find_from(d, FALSE); }
static void find_prev(GtkSearchEntry *e, gpointer d) { (void)e; find_from(d, TRUE); }

static void find_stop(GtkSearchEntry *e, gpointer data)
{
	struct win *w = data;

	(void)e;
	gtk_search_bar_set_search_mode(GTK_SEARCH_BAR(w->findbar), FALSE);
	gtk_widget_grab_focus(w->view);
}

/* ---- go to a line ---- */

static void goto_line_done(GtkEntry *entry, gpointer data)
{
	struct win *w = data;
	int line = atoi(gtk_editable_get_text(GTK_EDITABLE(entry)));
	GtkTextIter it;

	if (line > 0) {
		gtk_text_buffer_get_iter_at_line(w->buffer, &it, line - 1);
		gtk_text_buffer_place_cursor(w->buffer, &it);
		gtk_text_view_scroll_to_mark(GTK_TEXT_VIEW(w->view), gtk_text_buffer_get_insert(w->buffer),
					     0.2, FALSE, 0, 0);
	}
	gtk_popover_popdown(GTK_POPOVER(gtk_widget_get_ancestor(GTK_WIDGET(entry), GTK_TYPE_POPOVER)));
	gtk_widget_grab_focus(w->view);
}

static void goto_line(struct win *w)
{
	GtkWidget *popover = gtk_popover_new(), *entry = gtk_entry_new();
	GdkRectangle where = { 0, 0, 1, 1 };

	gtk_entry_set_placeholder_text(GTK_ENTRY(entry), "Line number");
	gtk_entry_set_input_purpose(GTK_ENTRY(entry), GTK_INPUT_PURPOSE_DIGITS);
	g_signal_connect(entry, "activate", G_CALLBACK(goto_line_done), w);
	gtk_popover_set_child(GTK_POPOVER(popover), entry);
	gtk_widget_set_parent(popover, w->position);
	gtk_popover_set_pointing_to(GTK_POPOVER(popover), &where);
	g_signal_connect(popover, "closed", G_CALLBACK(gtk_widget_unparent), NULL);
	gtk_popover_popup(GTK_POPOVER(popover));
	gtk_widget_grab_focus(entry);
}

/* ---- closing with unsaved changes ---- */

static void close_answered(GObject *source, GAsyncResult *res, gpointer data)
{
	struct win *w = data;
	int choice = gtk_alert_dialog_choose_finish(GTK_ALERT_DIALOG(source), res, NULL);

	if (choice == 2) {              /* Save */
		w->closing = TRUE;
		save(w);
	} else if (choice == 1) {       /* Discard */
		gtk_text_buffer_set_modified(w->buffer, FALSE);
		gtk_window_destroy(GTK_WINDOW(w->window));
	}
}

static gboolean close_request(GtkWindow *window, gpointer data)
{
	struct win *w = data;
	GtkAlertDialog *d;
	char *name;

	(void)window;
	if (!gtk_text_buffer_get_modified(w->buffer))
		return FALSE;
	name = w->file ? g_file_get_basename(w->file) : g_strdup("this new document");
	d = gtk_alert_dialog_new("Save the changes to %s?", name);
	gtk_alert_dialog_set_detail(d, "If you close without saving, the changes are lost.");
	gtk_alert_dialog_set_buttons(d, (const char *[]){ "Cancel", "Close without saving",
							 "Save", NULL });
	gtk_alert_dialog_set_cancel_button(d, 0);
	gtk_alert_dialog_set_default_button(d, 2);
	gtk_alert_dialog_choose(d, GTK_WINDOW(w->window), NULL, close_answered, w);
	g_object_unref(d);
	g_free(name);
	return TRUE;
}

/* ---- actions ---- */

#define WIN(data) ((struct win *)(data))

static void act_new(GSimpleAction *a, GVariant *p, gpointer d) { (void)a; (void)p;
	new_window(gtk_window_get_application(GTK_WINDOW(WIN(d)->window))); }
static void act_save(GSimpleAction *a, GVariant *p, gpointer d) { (void)a; (void)p; save(d); }
static void act_save_as(GSimpleAction *a, GVariant *p, gpointer d) { (void)a; (void)p; save_as(d); }
static void act_goto(GSimpleAction *a, GVariant *p, gpointer d) { (void)a; (void)p; goto_line(d); }
static void act_close(GSimpleAction *a, GVariant *p, gpointer d) { (void)a; (void)p;
	gtk_window_close(GTK_WINDOW(WIN(d)->window)); }
static void act_undo(GSimpleAction *a, GVariant *p, gpointer d) { (void)a; (void)p;
	gtk_text_buffer_undo(WIN(d)->buffer); }
static void act_redo(GSimpleAction *a, GVariant *p, gpointer d) { (void)a; (void)p;
	gtk_text_buffer_redo(WIN(d)->buffer); }

static void act_open(GSimpleAction *a, GVariant *p, gpointer data)
{
	GtkFileDialog *dialog = gtk_file_dialog_new();

	(void)a; (void)p;
	gtk_file_dialog_open_multiple(dialog, GTK_WINDOW(WIN(data)->window), NULL, opened, data);
	g_object_unref(dialog);
}

static void act_find(GSimpleAction *a, GVariant *p, gpointer data)
{
	struct win *w = data;
	GtkTextIter start, end;

	(void)a; (void)p;
	/* The selection, if any, is what to look for. */
	if (gtk_text_buffer_get_selection_bounds(w->buffer, &start, &end)) {
		char *text = gtk_text_buffer_get_text(w->buffer, &start, &end, FALSE);

		if (!strchr(text, '\n'))
			gtk_editable_set_text(GTK_EDITABLE(w->find), text);
		g_free(text);
	}
	gtk_search_bar_set_search_mode(GTK_SEARCH_BAR(w->findbar), TRUE);
	gtk_widget_grab_focus(w->find);
}

static void act_wrap(GSimpleAction *a, GVariant *p, gpointer data)
{
	gboolean on = !g_variant_get_boolean(g_action_get_state(G_ACTION(a)));

	(void)p;
	g_simple_action_set_state(a, g_variant_new_boolean(on));
	gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(WIN(data)->view), on ? GTK_WRAP_WORD_CHAR : GTK_WRAP_NONE);
}

static void act_mono(GSimpleAction *a, GVariant *p, gpointer data)
{
	gboolean on = !g_variant_get_boolean(g_action_get_state(G_ACTION(a)));

	(void)p;
	g_simple_action_set_state(a, g_variant_new_boolean(on));
	gtk_text_view_set_monospace(GTK_TEXT_VIEW(WIN(data)->view), on);
}

static const GActionEntry actions[] = {
	{ .name = "new", .activate = act_new },
	{ .name = "open", .activate = act_open },
	{ .name = "save", .activate = act_save },
	{ .name = "save-as", .activate = act_save_as },
	{ .name = "find", .activate = act_find },
	{ .name = "goto", .activate = act_goto },
	{ .name = "close", .activate = act_close },
	{ .name = "undo", .activate = act_undo },
	{ .name = "redo", .activate = act_redo },
	{ .name = "wrap", .activate = act_wrap, .state = "true" },
	{ .name = "monospace", .activate = act_mono, .state = "true" },
};

/* ---- the window ---- */

static void win_free(gpointer data)
{
	struct win *w = data;

	g_clear_object(&w->file);
	g_free(w);
}

static gboolean dropped(GtkDropTarget *t, const GValue *value, double x, double y, gpointer data)
{
	struct win *w = data;
	GSList *files;

	(void)t; (void)x; (void)y;
	if (!G_VALUE_HOLDS(value, GDK_TYPE_FILE_LIST))
		return FALSE;
	files = gdk_file_list_get_files(g_value_get_boxed(value));
	for (GSList *l = files; l; l = l->next) {
		struct win *target = !w->file && !gtk_text_buffer_get_modified(w->buffer) &&
				     gtk_text_buffer_get_char_count(w->buffer) == 0 && l == files ? w :
				     new_window(gtk_window_get_application(GTK_WINDOW(w->window)));

		load(target, l->data);
	}
	g_slist_free(files);
	return TRUE;
}

static struct win *new_window(GtkApplication *app)
{
	struct win *w = g_new0(struct win, 1);
	GtkWidget *header, *titles, *open, *menu_button, *box, *scroll, *bar, *find_box;
	GtkDropTarget *drop;
	GMenu *menu = g_menu_new(), *section;

	w->window = gtk_application_window_new(app);
	g_object_set_data_full(G_OBJECT(w->window), "win", w, win_free);
	gtk_window_set_default_size(GTK_WINDOW(w->window), 820, 640);
	g_action_map_add_action_entries(G_ACTION_MAP(w->window), actions, G_N_ELEMENTS(actions), w);
	g_signal_connect(w->window, "close-request", G_CALLBACK(close_request), w);

	header = gtk_header_bar_new();
	open = gtk_button_new_with_label("Open");
	gtk_actionable_set_action_name(GTK_ACTIONABLE(open), "win.open");
	gtk_widget_set_tooltip_text(open, "Open a file (Ctrl+O)");
	gtk_header_bar_pack_start(GTK_HEADER_BAR(header), open);
	titles = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_widget_set_valign(titles, GTK_ALIGN_CENTER);
	w->title = gtk_label_new("");
	gtk_widget_add_css_class(w->title, "title");
	gtk_label_set_ellipsize(GTK_LABEL(w->title), PANGO_ELLIPSIZE_MIDDLE);
	w->subtitle = gtk_label_new("");
	gtk_widget_add_css_class(w->subtitle, "subtitle");
	gtk_widget_set_opacity(w->subtitle, 0.65);
	gtk_label_set_ellipsize(GTK_LABEL(w->subtitle), PANGO_ELLIPSIZE_START);
	gtk_box_append(GTK_BOX(titles), w->title);
	gtk_box_append(GTK_BOX(titles), w->subtitle);
	gtk_header_bar_set_title_widget(GTK_HEADER_BAR(header), titles);

	section = g_menu_new();
	g_menu_append(section, "New window", "win.new");
	g_menu_append(section, "Save", "win.save");
	g_menu_append(section, "Save as…", "win.save-as");
	g_menu_append_section(menu, NULL, G_MENU_MODEL(section));
	g_object_unref(section);
	section = g_menu_new();
	g_menu_append(section, "Find…", "win.find");
	g_menu_append(section, "Go to line…", "win.goto");
	g_menu_append_section(menu, NULL, G_MENU_MODEL(section));
	g_object_unref(section);
	section = g_menu_new();
	g_menu_append(section, "Wrap long lines", "win.wrap");
	g_menu_append(section, "Monospace font", "win.monospace");
	g_menu_append_section(menu, NULL, G_MENU_MODEL(section));
	g_object_unref(section);
	menu_button = gtk_menu_button_new();
	gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(menu_button), "open-menu-symbolic");
	gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(menu_button), G_MENU_MODEL(menu));
	g_object_unref(menu);
	gtk_header_bar_pack_end(GTK_HEADER_BAR(header), menu_button);
	{
		GtkWidget *save_button = gtk_button_new_with_label("Save");

		gtk_actionable_set_action_name(GTK_ACTIONABLE(save_button), "win.save");
		gtk_widget_set_tooltip_text(save_button, "Save (Ctrl+S)");
		gtk_header_bar_pack_end(GTK_HEADER_BAR(header), save_button);
	}
	gtk_window_set_titlebar(GTK_WINDOW(w->window), header);

	w->view = gtk_text_view_new();
	w->buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(w->view));
	gtk_text_buffer_set_enable_undo(w->buffer, TRUE);
	gtk_text_view_set_monospace(GTK_TEXT_VIEW(w->view), TRUE);
	gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(w->view), GTK_WRAP_WORD_CHAR);
	gtk_text_view_set_left_margin(GTK_TEXT_VIEW(w->view), 14);
	gtk_text_view_set_right_margin(GTK_TEXT_VIEW(w->view), 14);
	gtk_text_view_set_top_margin(GTK_TEXT_VIEW(w->view), 10);
	gtk_text_view_set_bottom_margin(GTK_TEXT_VIEW(w->view), 10);
	g_signal_connect(w->buffer, "modified-changed", G_CALLBACK(modified_changed), w);
	g_signal_connect(w->buffer, "notify::cursor-position", G_CALLBACK(cursor_moved), w);
	scroll = gtk_scrolled_window_new();
	gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), w->view);
	gtk_widget_set_vexpand(scroll, TRUE);

	w->find = gtk_search_entry_new();
	gtk_widget_set_size_request(w->find, 320, -1);
	g_signal_connect(w->find, "search-changed", G_CALLBACK(find_changed), w);
	g_signal_connect(w->find, "activate", G_CALLBACK(find_next), w);
	g_signal_connect(w->find, "next-match", G_CALLBACK(find_next), w);
	g_signal_connect(w->find, "previous-match", G_CALLBACK(find_prev), w);
	g_signal_connect(w->find, "stop-search", G_CALLBACK(find_stop), w);
	w->find_status = gtk_label_new("");
	gtk_widget_set_opacity(w->find_status, 0.7);
	find_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	gtk_box_append(GTK_BOX(find_box), w->find);
	gtk_box_append(GTK_BOX(find_box), w->find_status);
	w->findbar = gtk_search_bar_new();
	gtk_search_bar_set_child(GTK_SEARCH_BAR(w->findbar), find_box);
	gtk_search_bar_connect_entry(GTK_SEARCH_BAR(w->findbar), GTK_EDITABLE(w->find));
	gtk_search_bar_set_show_close_button(GTK_SEARCH_BAR(w->findbar), TRUE);

	bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	gtk_widget_set_margin_start(bar, 14);
	gtk_widget_set_margin_end(bar, 14);
	gtk_widget_set_margin_top(bar, 3);
	gtk_widget_set_margin_bottom(bar, 3);
	w->position = gtk_label_new("Line 1, column 1");
	gtk_widget_set_opacity(w->position, 0.65);
	gtk_box_append(GTK_BOX(bar), w->position);

	box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_box_append(GTK_BOX(box), w->findbar);
	gtk_box_append(GTK_BOX(box), scroll);
	gtk_box_append(GTK_BOX(box), bar);
	gtk_window_set_child(GTK_WINDOW(w->window), box);
	drop = gtk_drop_target_new(GDK_TYPE_FILE_LIST, GDK_ACTION_COPY);
	g_signal_connect(drop, "drop", G_CALLBACK(dropped), w);
	gtk_widget_add_controller(w->window, GTK_EVENT_CONTROLLER(drop));
	update_title(w);
	gtk_window_present(GTK_WINDOW(w->window));
	gtk_widget_grab_focus(w->view);
	return w;
}

static void startup(GApplication *app, gpointer data)
{
	static const struct { const char *action, *keys[3]; } accels[] = {
		{ "win.new", { "<Control>n", NULL } },
		{ "win.open", { "<Control>o", NULL } },
		{ "win.save", { "<Control>s", NULL } },
		{ "win.save-as", { "<Control><Shift>s", NULL } },
		{ "win.find", { "<Control>f", NULL } },
		{ "win.goto", { "<Control>l", "<Control>i", NULL } },
		{ "win.close", { "<Control>w", "<Control>q", NULL } },
		{ "win.undo", { "<Control>z", NULL } },
		{ "win.redo", { "<Control><Shift>z", "<Control>y", NULL } },
	};

	(void)data;
	for (size_t i = 0; i < G_N_ELEMENTS(accels); i++)
		gtk_application_set_accels_for_action(GTK_APPLICATION(app), accels[i].action,
						      accels[i].keys);
}

static void activate(GtkApplication *app, gpointer data)
{
	(void)data;
	new_window(app);
}

static void open_files(GApplication *app, GFile **files, int n, const char *hint, gpointer data)
{
	(void)hint; (void)data;
	for (int i = 0; i < n; i++) {
		struct win *w = new_window(GTK_APPLICATION(app));

		if (!load(w, files[i]))
			gtk_window_destroy(GTK_WINDOW(w->window));
	}
}

int main(int argc, char **argv)
{
	GtkApplication *app = gtk_application_new(APP_ID, G_APPLICATION_HANDLES_OPEN);
	int status;

	g_signal_connect(app, "startup", G_CALLBACK(startup), NULL);
	g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);
	g_signal_connect(app, "open", G_CALLBACK(open_files), NULL);
	status = g_application_run(G_APPLICATION(app), argc, argv);
	g_object_unref(app);
	return status;
}
