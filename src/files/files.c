/*
 * wormos-files - Files: folders and files in a window.
 *
 * A small file manager: places on the left (home, the user folders, the
 * trash, the whole computer and mounted drives), the folder as a grid of
 * icons, a path bar to go back up. Double-click or Enter opens a folder,
 * or a file with its default program. Right click (or the keys) to open,
 * rename, copy, cut, paste, move to the trash, make a folder or open a
 * terminal there. Typing filters the folder by name. GTK's own directory
 * model watches the folder, so changes made elsewhere show at once.
 *
 *     wormos-files [FOLDER]
 */

#define _GNU_SOURCE
#include <gtk/gtk.h>
#include <string.h>

#define APP_ID "org.wormos.Files"
#define CSS    "/usr/share/wormos/files/files.css"
#define ATTRS  G_FILE_ATTRIBUTE_STANDARD_NAME "," G_FILE_ATTRIBUTE_STANDARD_DISPLAY_NAME "," \
	       G_FILE_ATTRIBUTE_STANDARD_TYPE "," G_FILE_ATTRIBUTE_STANDARD_ICON "," \
	       G_FILE_ATTRIBUTE_STANDARD_IS_HIDDEN "," G_FILE_ATTRIBUTE_STANDARD_IS_BACKUP "," \
	       G_FILE_ATTRIBUTE_STANDARD_SIZE "," G_FILE_ATTRIBUTE_TIME_MODIFIED "," \
	       G_FILE_ATTRIBUTE_STANDARD_CONTENT_TYPE

struct win {
	GtkWidget *window, *grid, *pathbar, *places, *search, *empty, *status, *back, *forward;
	GtkWidget *menu;
	GtkDirectoryList *dir;
	GtkFilterListModel *filtered;
	GtkCustomFilter *filter;
	GtkSortListModel *sorted;
	GtkMultiSelection *selection;
	GFile *folder;
	GList *history, *future;        /* of GFile */
	gboolean show_hidden;
};

/* The clipboard of this program: files to copy or move on paste. */
static GList *clip_files;
static gboolean clip_cut;

static void navigate(struct win *w, GFile *folder, gboolean record);

/* ---- helpers ---- */

static GFile *trash_folder(void)
{
	return g_file_new_build_filename(g_get_user_data_dir(), "Trash", "files", NULL);
}

static GList *selected_files(struct win *w)
{
	GtkBitset *set = gtk_selection_model_get_selection(GTK_SELECTION_MODEL(w->selection));
	GtkBitsetIter iter;
	GList *files = NULL;
	guint i;

	if (gtk_bitset_iter_init_first(&iter, set, &i)) {
		do {
			GFileInfo *info = g_list_model_get_item(G_LIST_MODEL(w->selection), i);
			GFile *file = G_FILE(g_file_info_get_attribute_object(info, "standard::file"));

			files = g_list_prepend(files, g_object_ref(file));
			g_object_unref(info);
		} while (gtk_bitset_iter_next(&iter, &i));
	}
	gtk_bitset_unref(set);
	return g_list_reverse(files);
}

static void message(struct win *w, const char *text)
{
	gtk_label_set_text(GTK_LABEL(w->status), text);
}

static void error_message(struct win *w, const char *what, GError *error)
{
	char *text = g_strdup_printf("%s: %s", what, error->message);

	message(w, text);
	g_free(text);
}

/* A name that does not exist yet in folder: "name", "name (2)", ... */
static GFile *free_name(GFile *folder, const char *name)
{
	GFile *file = g_file_get_child(folder, name);
	const char *dot = strrchr(name, '.');
	size_t stem = dot && dot != name ? (size_t)(dot - name) : strlen(name);

	for (int n = 2; g_file_query_exists(file, NULL) && n < 1000; n++) {
		char *alt = g_strdup_printf("%.*s (%d)%s", (int)stem, name, n, name + stem);

		g_object_unref(file);
		file = g_file_get_child(folder, alt);
		g_free(alt);
	}
	return file;
}

/* ---- the folder's contents ---- */

static gboolean keep(gpointer item, gpointer data)
{
	struct win *w = data;
	GFileInfo *info = item;
	const char *text = gtk_editable_get_text(GTK_EDITABLE(w->search));
	char *name, *folded, *needle;
	gboolean ok;

	if (!w->show_hidden && (g_file_info_get_is_hidden(info) || g_file_info_get_is_backup(info)))
		return FALSE;
	if (!*text)
		return TRUE;
	name = g_utf8_casefold(g_file_info_get_display_name(info), -1);
	folded = g_utf8_casefold(text, -1);
	needle = g_utf8_normalize(folded, -1, G_NORMALIZE_ALL);
	ok = strstr(name, needle) != NULL;
	g_free(name);
	g_free(folded);
	g_free(needle);
	return ok;
}

/* Folders first, then by name as people sort (numbers in order). */
static int compare(gconstpointer a, gconstpointer b, gpointer data)
{
	GFileInfo *x = (GFileInfo *)a, *y = (GFileInfo *)b;
	gboolean dx = g_file_info_get_file_type(x) == G_FILE_TYPE_DIRECTORY;
	gboolean dy = g_file_info_get_file_type(y) == G_FILE_TYPE_DIRECTORY;
	char *kx, *ky;
	int r;

	(void)data;
	if (dx != dy)
		return dx ? -1 : 1;
	kx = g_utf8_collate_key_for_filename(g_file_info_get_display_name(x), -1);
	ky = g_utf8_collate_key_for_filename(g_file_info_get_display_name(y), -1);
	r = strcmp(kx, ky);
	g_free(kx);
	g_free(ky);
	return r;
}

static void setup_item(GtkSignalListItemFactory *f, GtkListItem *item, gpointer data)
{
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4), *image, *label;

	(void)f;
	(void)data;
	gtk_widget_add_css_class(box, "item");
	image = gtk_image_new();
	gtk_image_set_pixel_size(GTK_IMAGE(image), 56);
	label = gtk_label_new("");
	gtk_label_set_wrap(GTK_LABEL(label), TRUE);
	gtk_label_set_wrap_mode(GTK_LABEL(label), PANGO_WRAP_WORD_CHAR);
	gtk_label_set_lines(GTK_LABEL(label), 2);
	gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
	gtk_label_set_justify(GTK_LABEL(label), GTK_JUSTIFY_CENTER);
	gtk_label_set_max_width_chars(GTK_LABEL(label), 14);
	gtk_label_set_width_chars(GTK_LABEL(label), 14);
	gtk_box_append(GTK_BOX(box), image);
	gtk_box_append(GTK_BOX(box), label);
	g_object_set_data(G_OBJECT(box), "list-item", item);
	gtk_list_item_set_child(item, box);
}

static void bind_item(GtkSignalListItemFactory *f, GtkListItem *item, gpointer data)
{
	GtkWidget *box = gtk_list_item_get_child(item);
	GtkWidget *image = gtk_widget_get_first_child(box), *label = gtk_widget_get_last_child(box);
	GFileInfo *info = gtk_list_item_get_item(item);
	GIcon *icon = g_file_info_get_icon(info);
	char *tip, *size;

	(void)f;
	(void)data;
	if (icon)
		gtk_image_set_from_gicon(GTK_IMAGE(image), icon);
	else
		gtk_image_set_from_icon_name(GTK_IMAGE(image), "text-x-generic");
	gtk_label_set_text(GTK_LABEL(label), g_file_info_get_display_name(info));
	size = g_file_info_get_file_type(info) == G_FILE_TYPE_DIRECTORY ? g_strdup("Folder") :
	       g_format_size(g_file_info_get_size(info));
	tip = g_strdup_printf("%s\n%s", g_file_info_get_display_name(info), size);
	gtk_widget_set_tooltip_text(box, tip);
	g_free(tip);
	g_free(size);
}

static void update_empty(struct win *w)
{
	guint n = g_list_model_get_n_items(G_LIST_MODEL(w->filtered));
	gboolean loading = gtk_directory_list_is_loading(w->dir);
	const GError *error = gtk_directory_list_get_error(w->dir);

	gtk_widget_set_visible(w->empty, n == 0 && !loading);
	if (error)
		gtk_label_set_text(GTK_LABEL(w->empty), error->message);
	else if (*gtk_editable_get_text(GTK_EDITABLE(w->search)))
		gtk_label_set_text(GTK_LABEL(w->empty), "Nothing here matches.");
	else
		gtk_label_set_text(GTK_LABEL(w->empty), "This folder is empty.");
	if (!error) {
		char *text = g_strdup_printf("%u item%s", n, n == 1 ? "" : "s");

		message(w, text);
		g_free(text);
	}
}

static void items_changed(GListModel *m, guint p, guint r, guint a, gpointer data)
{
	(void)m; (void)p; (void)r; (void)a;
	update_empty(data);
}

static void loading_changed(GObject *o, GParamSpec *p, gpointer data)
{
	(void)o; (void)p;
	update_empty(data);
}

/* ---- the path bar ---- */

static void crumb_clicked(GtkButton *button, gpointer data)
{
	navigate(data, g_object_get_data(G_OBJECT(button), "file"), TRUE);
}

static void update_pathbar(struct win *w)
{
	GtkWidget *child;
	GFile *home = g_file_new_for_path(g_get_home_dir()), *f = g_object_ref(w->folder);
	GList *parts = NULL;

	while ((child = gtk_widget_get_first_child(w->pathbar)))
		gtk_box_remove(GTK_BOX(w->pathbar), child);
	/* From the folder up to the home folder (or /). */
	while (f) {
		GFile *parent;

		parts = g_list_prepend(parts, f);
		if (g_file_equal(f, home))
			break;
		parent = g_file_get_parent(f);
		f = parent;
	}
	for (GList *l = parts; l; l = l->next) {
		GFile *file = l->data;
		char *name = g_file_equal(file, home) ? g_strdup("Home") :
			     g_file_has_parent(file, NULL) ? g_file_get_basename(file) :
			     g_strdup("Computer");
		GtkWidget *button = gtk_button_new_with_label(name);

		gtk_widget_add_css_class(button, "flat");
		if (!l->next)
			gtk_widget_add_css_class(button, "current");
		g_object_set_data_full(G_OBJECT(button), "file", file, g_object_unref);
		g_signal_connect(button, "clicked", G_CALLBACK(crumb_clicked), w);
		if (l != parts)
			gtk_box_append(GTK_BOX(w->pathbar), gtk_label_new("\xe2\x80\xba"));
		gtk_box_append(GTK_BOX(w->pathbar), button);
		g_free(name);
	}
	g_list_free(parts);
	g_object_unref(home);
}

/* ---- navigation ---- */

static void navigate(struct win *w, GFile *folder, gboolean record)
{
	char *title;

	if (w->folder && g_file_equal(w->folder, folder))
		return;
	if (record && w->folder) {
		w->history = g_list_prepend(w->history, g_object_ref(w->folder));
		g_list_free_full(w->future, g_object_unref);
		w->future = NULL;
	}
	g_set_object(&w->folder, folder);
	gtk_editable_set_text(GTK_EDITABLE(w->search), "");
	gtk_directory_list_set_file(w->dir, folder);
	update_pathbar(w);
	{
		GFile *home = g_file_new_for_path(g_get_home_dir());

		title = g_file_get_basename(folder);
		gtk_window_set_title(GTK_WINDOW(w->window), g_file_equal(folder, home) ? "Home" :
				     !g_file_has_parent(folder, NULL) ? "Computer" : title);
		g_free(title);
		g_object_unref(home);
	}
	gtk_widget_set_sensitive(w->back, w->history != NULL);
	gtk_widget_set_sensitive(w->forward, w->future != NULL);
	message(w, "");
	/* The sidebar marks the place shown, if it is one. */
	gtk_list_box_unselect_all(GTK_LIST_BOX(w->places));
	for (GtkWidget *row = gtk_widget_get_first_child(w->places); row;
	     row = gtk_widget_get_next_sibling(row)) {
		GFile *place = g_object_get_data(G_OBJECT(row), "file");

		if (place && g_file_equal(place, folder)) {
			gtk_list_box_select_row(GTK_LIST_BOX(w->places), GTK_LIST_BOX_ROW(row));
			break;
		}
	}
}

static void go_back(struct win *w)
{
	GFile *f;

	if (!w->history)
		return;
	f = w->history->data;
	w->history = g_list_delete_link(w->history, w->history);
	w->future = g_list_prepend(w->future, g_object_ref(w->folder));
	navigate(w, f, FALSE);
	g_object_unref(f);
	gtk_widget_set_sensitive(w->back, w->history != NULL);
	gtk_widget_set_sensitive(w->forward, TRUE);
}

static void go_forward(struct win *w)
{
	GFile *f;

	if (!w->future)
		return;
	f = w->future->data;
	w->future = g_list_delete_link(w->future, w->future);
	w->history = g_list_prepend(w->history, g_object_ref(w->folder));
	navigate(w, f, FALSE);
	g_object_unref(f);
	gtk_widget_set_sensitive(w->forward, w->future != NULL);
	gtk_widget_set_sensitive(w->back, TRUE);
}

static void go_up(struct win *w)
{
	GFile *parent = g_file_get_parent(w->folder);

	if (parent) {
		navigate(w, parent, TRUE);
		g_object_unref(parent);
	}
}

static void back_clicked(GtkButton *b, gpointer d) { (void)b; go_back(d); }
static void forward_clicked(GtkButton *b, gpointer d) { (void)b; go_forward(d); }

/* ---- opening ---- */

static void launched(GObject *source, GAsyncResult *res, gpointer data)
{
	GError *error = NULL;

	if (!gtk_file_launcher_launch_finish(GTK_FILE_LAUNCHER(source), res, &error)) {
		if (!g_error_matches(error, GTK_DIALOG_ERROR, GTK_DIALOG_ERROR_DISMISSED))
			error_message(data, "Cannot open it", error);
		g_error_free(error);
	}
}

static void open_file(struct win *w, GFile *file)
{
	GFileType type = g_file_query_file_type(file, G_FILE_QUERY_INFO_NONE, NULL);

	if (type == G_FILE_TYPE_DIRECTORY) {
		navigate(w, file, TRUE);
	} else {
		GtkFileLauncher *launcher = gtk_file_launcher_new(file);

		gtk_file_launcher_set_always_ask(launcher, FALSE);
		gtk_file_launcher_launch(launcher, GTK_WINDOW(w->window), NULL, launched, w);
		g_object_unref(launcher);
	}
}

static void activated(GtkGridView *view, guint position, gpointer data)
{
	GFileInfo *info = g_list_model_get_item(G_LIST_MODEL(gtk_grid_view_get_model(view)), position);
	GFile *file = G_FILE(g_file_info_get_attribute_object(info, "standard::file"));

	open_file(data, file);
	g_object_unref(info);
}

/* ---- actions ---- */

static void act_open(GSimpleAction *a, GVariant *p, gpointer data)
{
	struct win *w = data;
	GList *files = selected_files(w);

	(void)a; (void)p;
	for (GList *l = files; l; l = l->next)
		open_file(w, l->data);
	g_list_free_full(files, g_object_unref);
}

static void act_trash(GSimpleAction *a, GVariant *p, gpointer data)
{
	struct win *w = data;
	GList *files = selected_files(w);
	GFile *trash = trash_folder();
	int moved = 0;

	(void)a; (void)p;
	for (GList *l = files; l; l = l->next) {
		GError *error = NULL;
		gboolean in_trash = g_file_has_prefix(l->data, trash);

		/* In the trash itself, delete for good. */
		if (in_trash ? !g_file_delete(l->data, NULL, &error) : !g_file_trash(l->data, NULL, &error)) {
			error_message(w, in_trash ? "Cannot delete it" : "Cannot move it to the trash",
				      error);
			g_error_free(error);
		} else {
			moved++;
		}
	}
	if (moved) {
		char *text = g_strdup_printf(g_file_has_prefix(w->folder, trash) ||
					     g_file_equal(w->folder, trash) ?
					     "%d deleted for good." : "%d moved to the trash.", moved);

		message(w, text);
		g_free(text);
	}
	g_object_unref(trash);
	g_list_free_full(files, g_object_unref);
}

static void set_clipboard(struct win *w, gboolean cut)
{
	GList *files = selected_files(w);
	GdkClipboard *clipboard = gtk_widget_get_clipboard(w->window);
	GString *text = g_string_new("");

	if (!files)
		return;
	g_list_free_full(clip_files, g_object_unref);
	clip_files = files;
	clip_cut = cut;
	/* Paths for other programs too. */
	for (GList *l = files; l; l = l->next) {
		char *path = g_file_get_path(l->data);

		if (path)
			g_string_append_printf(text, "%s%s", text->len ? "\n" : "", path);
		g_free(path);
	}
	gdk_clipboard_set_text(clipboard, text->str);
	g_string_free(text, TRUE);
	message(w, cut ? "Cut: paste it into another folder to move it." :
		"Copied: paste it into a folder.");
}

static void act_copy(GSimpleAction *a, GVariant *p, gpointer d) { (void)a; (void)p; set_clipboard(d, FALSE); }
static void act_cut(GSimpleAction *a, GVariant *p, gpointer d) { (void)a; (void)p; set_clipboard(d, TRUE); }

/* Copying and moving run in cp and mv, one file after another. */
struct paste {
	struct win *w;
	GList *files, *next;
	gboolean cut;
	int done;
};

static void paste_next(struct paste *p);

static void paste_step_done(GObject *source, GAsyncResult *res, gpointer data)
{
	struct paste *p = data;

	if (g_subprocess_wait_check_finish(G_SUBPROCESS(source), res, NULL))
		p->done++;
	paste_next(p);
}

static void paste_next(struct paste *p)
{
	while (p->next) {
		GFile *src = p->next->data, *dest;
		char *name = g_file_get_basename(src), *from = g_file_get_path(src), *to;
		GSubprocess *proc;

		p->next = p->next->next;
		dest = free_name(p->w->folder, name);
		to = g_file_get_path(dest);
		g_free(name);
		if (!from || !to || g_file_has_prefix(p->w->folder, src)) {
			g_free(from);
			g_free(to);
			g_object_unref(dest);
			continue;
		}
		proc = p->cut ? g_subprocess_new(G_SUBPROCESS_FLAGS_NONE, NULL, "mv", "--", from, to, NULL)
			      : g_subprocess_new(G_SUBPROCESS_FLAGS_NONE, NULL, "cp", "-a", "--", from, to,
						 NULL);
		g_free(from);
		g_free(to);
		g_object_unref(dest);
		if (proc) {
			g_subprocess_wait_check_async(proc, NULL, paste_step_done, p);
			g_object_unref(proc);
			return;
		}
	}
	{
		char *text = g_strdup_printf("%d %s.", p->done, p->cut ? "moved" : "copied");

		message(p->w, text);
		g_free(text);
	}
	if (p->cut) {
		g_list_free_full(clip_files, g_object_unref);
		clip_files = NULL;
	}
	g_list_free_full(p->files, g_object_unref);
	g_free(p);
}

static void act_paste(GSimpleAction *a, GVariant *v, gpointer data)
{
	struct win *w = data;
	struct paste *p;

	(void)a; (void)v;
	if (!clip_files) {
		message(w, "Nothing to paste: copy or cut something first.");
		return;
	}
	p = g_new0(struct paste, 1);
	p->w = w;
	p->files = g_list_copy_deep(clip_files, (GCopyFunc)(void (*)(void))g_object_ref, NULL);
	p->next = p->files;
	p->cut = clip_cut;
	message(w, p->cut ? "Moving..." : "Copying...");
	paste_next(p);
}

/* A small dialog with one entry: rename and new folder. */
struct ask {
	struct win *w;
	GtkWidget *dialog, *entry, *error;
	GFile *file;            /* to rename, or NULL for a new folder */
};

static void ask_done(GtkWidget *widget, gpointer data)
{
	struct ask *a = data;
	const char *name = gtk_editable_get_text(GTK_EDITABLE(a->entry));
	GError *error = NULL;
	gboolean ok;

	(void)widget;
	if (!*name || strchr(name, '/') || !strcmp(name, ".") || !strcmp(name, "..")) {
		gtk_label_set_text(GTK_LABEL(a->error), "Not a name for a file (no slashes).");
		return;
	}
	if (a->file) {
		GFile *renamed = g_file_set_display_name(a->file, name, NULL, &error);

		ok = renamed != NULL;
		g_clear_object(&renamed);
	} else {
		GFile *folder = g_file_get_child(a->w->folder, name);

		ok = g_file_make_directory(folder, NULL, &error);
		g_object_unref(folder);
	}
	if (!ok) {
		gtk_label_set_text(GTK_LABEL(a->error), error->message);
		g_error_free(error);
		return;
	}
	gtk_window_destroy(GTK_WINDOW(a->dialog));
	g_clear_object(&a->file);
	g_free(a);
}

static void ask_cancel(GtkButton *button, gpointer data)
{
	struct ask *a = data;

	(void)button;
	gtk_window_destroy(GTK_WINDOW(a->dialog));
	g_clear_object(&a->file);
	g_free(a);
}

static void ask_name(struct win *w, const char *title, const char *button_text, GFile *file,
		     const char *initial)
{
	struct ask *a = g_new0(struct ask, 1);
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12), *label, *row, *cancel, *ok;

	a->w = w;
	a->file = file;
	a->dialog = gtk_window_new();
	gtk_window_set_title(GTK_WINDOW(a->dialog), title);
	gtk_window_set_transient_for(GTK_WINDOW(a->dialog), GTK_WINDOW(w->window));
	gtk_window_set_modal(GTK_WINDOW(a->dialog), TRUE);
	gtk_window_set_default_size(GTK_WINDOW(a->dialog), 380, -1);
	gtk_widget_add_css_class(box, "dialog");
	label = gtk_label_new(title);
	gtk_widget_add_css_class(label, "title-2");
	gtk_label_set_xalign(GTK_LABEL(label), 0);
	gtk_box_append(GTK_BOX(box), label);
	a->entry = gtk_entry_new();
	gtk_editable_set_text(GTK_EDITABLE(a->entry), initial);
	g_signal_connect(a->entry, "activate", G_CALLBACK(ask_done), a);
	gtk_box_append(GTK_BOX(box), a->entry);
	a->error = gtk_label_new("");
	gtk_widget_add_css_class(a->error, "error");
	gtk_label_set_xalign(GTK_LABEL(a->error), 0);
	gtk_box_append(GTK_BOX(box), a->error);
	row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	gtk_widget_set_halign(row, GTK_ALIGN_END);
	cancel = gtk_button_new_with_label("Cancel");
	g_signal_connect(cancel, "clicked", G_CALLBACK(ask_cancel), a);
	ok = gtk_button_new_with_label(button_text);
	gtk_widget_add_css_class(ok, "suggested-action");
	g_signal_connect(ok, "clicked", G_CALLBACK(ask_done), a);
	gtk_box_append(GTK_BOX(row), cancel);
	gtk_box_append(GTK_BOX(row), ok);
	gtk_box_append(GTK_BOX(box), row);
	gtk_window_set_child(GTK_WINDOW(a->dialog), box);
	gtk_window_present(GTK_WINDOW(a->dialog));
	gtk_widget_grab_focus(a->entry);
	/* Select the name without its extension, as people rename. */
	if (file) {
		const char *dot = strrchr(initial, '.');

		gtk_editable_select_region(GTK_EDITABLE(a->entry), 0,
					   dot && dot != initial ? g_utf8_pointer_to_offset(initial, dot)
								 : -1);
	}
}

static void act_rename(GSimpleAction *a, GVariant *p, gpointer data)
{
	struct win *w = data;
	GList *files = selected_files(w);

	(void)a; (void)p;
	if (files && !files->next) {
		char *name = g_file_get_basename(files->data);

		ask_name(w, "Rename", "Rename", g_object_ref(files->data), name);
		g_free(name);
	} else if (files) {
		message(w, "Select one file to rename it.");
	}
	g_list_free_full(files, g_object_unref);
}

static void act_new_folder(GSimpleAction *a, GVariant *p, gpointer data)
{
	struct win *w = data;
	GFile *f = free_name(w->folder, "New folder");
	char *name = g_file_get_basename(f);

	(void)a; (void)p;
	ask_name(w, "New folder", "Make it", NULL, name);
	g_free(name);
	g_object_unref(f);
}

static void act_terminal(GSimpleAction *a, GVariant *p, gpointer data)
{
	struct win *w = data;
	char *path = g_file_get_path(w->folder);

	(void)a; (void)p;
	if (path)
		g_spawn_async(path, (char *[]){ "foot", NULL }, NULL, G_SPAWN_SEARCH_PATH,
			      NULL, NULL, NULL, NULL);
	g_free(path);
}

static void act_hidden(GSimpleAction *a, GVariant *p, gpointer data)
{
	struct win *w = data;

	(void)p;
	w->show_hidden = !w->show_hidden;
	g_simple_action_set_state(a, g_variant_new_boolean(w->show_hidden));
	gtk_filter_changed(GTK_FILTER(w->filter), GTK_FILTER_CHANGE_DIFFERENT);
}

static void act_empty_trash(GSimpleAction *a, GVariant *p, gpointer data)
{
	struct win *w = data;
	char *files = g_build_filename(g_get_user_data_dir(), "Trash", "files", NULL);
	char *info = g_build_filename(g_get_user_data_dir(), "Trash", "info", NULL);
	GError *error = NULL;

	(void)a; (void)p;
	/* rm -rf on the trash's own two folders, then make them again. */
	if (!g_spawn_sync(NULL, (char *[]){ "rm", "-rf", "--", files, info, NULL }, NULL,
			  G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL, NULL, &error)) {
		error_message(w, "Cannot empty the trash", error);
		g_error_free(error);
	} else {
		g_mkdir_with_parents(files, 0700);
		g_mkdir_with_parents(info, 0700);
		message(w, "The trash is empty.");
	}
	g_free(files);
	g_free(info);
}

static void act_back(GSimpleAction *a, GVariant *p, gpointer d) { (void)a; (void)p; go_back(d); }
static void act_up(GSimpleAction *a, GVariant *p, gpointer d) { (void)a; (void)p; go_up(d); }

static void act_search(GSimpleAction *a, GVariant *p, gpointer data)
{
	struct win *w = data;

	(void)a; (void)p;
	gtk_widget_grab_focus(w->search);
}

static const GActionEntry actions[] = {
	{ .name = "open", .activate = act_open },
	{ .name = "trash", .activate = act_trash },
	{ .name = "copy", .activate = act_copy },
	{ .name = "cut", .activate = act_cut },
	{ .name = "paste", .activate = act_paste },
	{ .name = "rename", .activate = act_rename },
	{ .name = "new-folder", .activate = act_new_folder },
	{ .name = "terminal", .activate = act_terminal },
	{ .name = "hidden", .activate = act_hidden, .state = "false" },
	{ .name = "empty-trash", .activate = act_empty_trash },
	{ .name = "back", .activate = act_back },
	{ .name = "up", .activate = act_up },
	{ .name = "search", .activate = act_search },
};

/* ---- the context menu ---- */

static void right_click(GtkGestureClick *g, int n, double x, double y, gpointer data)
{
	struct win *w = data;
	GtkWidget *picked = gtk_widget_pick(w->grid, x, y, GTK_PICK_DEFAULT);
	GMenu *menu = g_menu_new(), *section;
	GtkBitset *set;
	gboolean on_item = FALSE;
	GFile *trash = trash_folder();
	GdkRectangle where = { (int)x, (int)y, 1, 1 };

	(void)g; (void)n;
	/* A right click on an item selects it, alone unless it was selected. */
	for (GtkWidget *p = picked; p && p != w->grid; p = gtk_widget_get_parent(p)) {
		GtkListItem *item = g_object_get_data(G_OBJECT(p), "list-item");

		if (item) {
			guint pos = gtk_list_item_get_position(item);

			if (pos != GTK_INVALID_LIST_POSITION) {
				if (!gtk_selection_model_is_selected(GTK_SELECTION_MODEL(w->selection), pos))
					gtk_selection_model_select_item(GTK_SELECTION_MODEL(w->selection),
									pos, TRUE);
				on_item = TRUE;
			}
			break;
		}
	}
	if (!on_item)
		gtk_selection_model_unselect_all(GTK_SELECTION_MODEL(w->selection));
	set = gtk_selection_model_get_selection(GTK_SELECTION_MODEL(w->selection));
	on_item = on_item && !gtk_bitset_is_empty(set);
	gtk_bitset_unref(set);

	section = g_menu_new();
	if (on_item) {
		g_menu_append(section, "Open", "win.open");
		g_menu_append(section, "Rename…", "win.rename");
		g_menu_append_section(menu, NULL, G_MENU_MODEL(section));
		g_object_unref(section);
		section = g_menu_new();
		g_menu_append(section, "Copy", "win.copy");
		g_menu_append(section, "Cut", "win.cut");
		g_menu_append_section(menu, NULL, G_MENU_MODEL(section));
		g_object_unref(section);
		section = g_menu_new();
		g_menu_append(section, g_file_has_prefix(w->folder, trash) ||
			      g_file_equal(w->folder, trash) ? "Delete for good" : "Move to the trash",
			      "win.trash");
	} else {
		g_menu_append(section, "New folder…", "win.new-folder");
		g_menu_append(section, "Paste", "win.paste");
		g_menu_append_section(menu, NULL, G_MENU_MODEL(section));
		g_object_unref(section);
		section = g_menu_new();
		g_menu_append(section, "Open a terminal here", "win.terminal");
		g_menu_append(section, "Show hidden files", "win.hidden");
		if (g_file_equal(w->folder, trash))
			g_menu_append(section, "Empty the trash", "win.empty-trash");
	}
	g_menu_append_section(menu, NULL, G_MENU_MODEL(section));
	g_object_unref(section);
	g_object_unref(trash);

	if (w->menu)
		gtk_widget_unparent(w->menu);
	w->menu = gtk_popover_menu_new_from_model(G_MENU_MODEL(menu));
	g_object_unref(menu);
	gtk_widget_set_parent(w->menu, w->grid);
	gtk_popover_set_has_arrow(GTK_POPOVER(w->menu), FALSE);
	gtk_popover_set_pointing_to(GTK_POPOVER(w->menu), &where);
	gtk_popover_popup(GTK_POPOVER(w->menu));
}

/* ---- places ---- */

static void place_add(struct win *w, const char *icon, const char *name, GFile *file)
{
	GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);

	gtk_widget_add_css_class(row, "place");
	gtk_box_append(GTK_BOX(row), gtk_image_new_from_icon_name(icon));
	gtk_box_append(GTK_BOX(row), gtk_label_new(name));
	gtk_list_box_append(GTK_LIST_BOX(w->places), row);
	g_object_set_data_full(G_OBJECT(gtk_widget_get_parent(row)), "file", file, g_object_unref);
}

static void place_user_dir(struct win *w, GUserDirectory which, const char *icon,
			   const char *fallback)
{
	const char *path = g_get_user_special_dir(which);
	char *name;

	if (!path || !g_file_test(path, G_FILE_TEST_IS_DIR) || !strcmp(path, g_get_home_dir()))
		return;
	name = g_path_get_basename(path);
	place_add(w, icon, *name ? name : fallback, g_file_new_for_path(path));
	g_free(name);
}

static void place_selected(GtkListBox *box, GtkListBoxRow *row, gpointer data)
{
	(void)box;
	if (row)
		navigate(data, g_object_get_data(G_OBJECT(row), "file"), TRUE);
}

static void fill_places(struct win *w)
{
	GFile *trash = trash_folder();
	GDir *media;

	place_add(w, "user-home-symbolic", "Home", g_file_new_for_path(g_get_home_dir()));
	place_user_dir(w, G_USER_DIRECTORY_DESKTOP, "user-desktop-symbolic", "Desktop");
	place_user_dir(w, G_USER_DIRECTORY_DOCUMENTS, "folder-documents-symbolic", "Documents");
	place_user_dir(w, G_USER_DIRECTORY_DOWNLOAD, "folder-download-symbolic", "Downloads");
	place_user_dir(w, G_USER_DIRECTORY_PICTURES, "folder-pictures-symbolic", "Pictures");
	place_user_dir(w, G_USER_DIRECTORY_MUSIC, "folder-music-symbolic", "Music");
	place_user_dir(w, G_USER_DIRECTORY_VIDEOS, "folder-videos-symbolic", "Videos");
	g_file_make_directory_with_parents(trash, NULL, NULL);
	place_add(w, "user-trash-symbolic", "Trash", trash);
	place_add(w, "drive-harddisk-symbolic", "Computer", g_file_new_for_path("/"));
	/* Drives and shared folders mounted under /media. */
	if ((media = g_dir_open("/media", 0, NULL))) {
		const char *name;

		while ((name = g_dir_read_name(media))) {
			char *path = g_build_filename("/media", name, NULL);

			if (g_file_test(path, G_FILE_TEST_IS_DIR))
				place_add(w, "drive-removable-media-symbolic", name,
					  g_file_new_for_path(path));
			g_free(path);
		}
		g_dir_close(media);
	}
}

/* ---- keys ---- */

static gboolean key_pressed(GtkEventControllerKey *c, guint keyval, guint code,
			    GdkModifierType state, gpointer data)
{
	struct win *w = data;
	gboolean ctrl = state & GDK_CONTROL_MASK, alt = state & GDK_ALT_MASK;

	(void)c; (void)code;
	if (keyval == GDK_KEY_Delete && !gtk_widget_has_focus(w->search)) {
		act_trash(NULL, NULL, w);
		return TRUE;
	}
	if (keyval == GDK_KEY_F2) {
		act_rename(NULL, NULL, w);
		return TRUE;
	}
	if (ctrl && keyval == GDK_KEY_h) {
		act_hidden(G_SIMPLE_ACTION(g_action_map_lookup_action(G_ACTION_MAP(w->window), "hidden")),
			   NULL, w);
		return TRUE;
	}
	if (ctrl && (state & GDK_SHIFT_MASK) && (keyval == GDK_KEY_N || keyval == GDK_KEY_n)) {
		act_new_folder(NULL, NULL, w);
		return TRUE;
	}
	if (!gtk_widget_has_focus(w->search) || !*gtk_editable_get_text(GTK_EDITABLE(w->search))) {
		if (ctrl && keyval == GDK_KEY_c) { set_clipboard(w, FALSE); return TRUE; }
		if (ctrl && keyval == GDK_KEY_x) { set_clipboard(w, TRUE); return TRUE; }
		if (ctrl && keyval == GDK_KEY_v) { act_paste(NULL, NULL, w); return TRUE; }
	}
	if ((alt && keyval == GDK_KEY_Left) ||
	    (keyval == GDK_KEY_BackSpace && !gtk_widget_has_focus(w->search))) {
		go_back(w);
		return TRUE;
	}
	if (alt && keyval == GDK_KEY_Right) {
		go_forward(w);
		return TRUE;
	}
	if (alt && keyval == GDK_KEY_Up) {
		go_up(w);
		return TRUE;
	}
	if ((ctrl && keyval == GDK_KEY_f) || (ctrl && keyval == GDK_KEY_l)) {
		gtk_widget_grab_focus(w->search);
		return TRUE;
	}
	if (keyval == GDK_KEY_Escape && *gtk_editable_get_text(GTK_EDITABLE(w->search))) {
		gtk_editable_set_text(GTK_EDITABLE(w->search), "");
		gtk_widget_grab_focus(w->grid);
		return TRUE;
	}
	return FALSE;
}

static void search_changed(GtkSearchEntry *e, gpointer data)
{
	struct win *w = data;

	(void)e;
	gtk_filter_changed(GTK_FILTER(w->filter), GTK_FILTER_CHANGE_DIFFERENT);
}

/* ---- the window ---- */

static void win_free(gpointer data)
{
	struct win *w = data;

	g_list_free_full(w->history, g_object_unref);
	g_list_free_full(w->future, g_object_unref);
	g_clear_object(&w->folder);
	g_free(w);
}

static void new_window(GtkApplication *app, GFile *folder)
{
	struct win *w = g_new0(struct win, 1);
	GtkWidget *header, *paned, *side, *scroll, *right, *overlay, *bar, *menu_button;
	GtkSignalListItemFactory *factory;
	GtkCustomSorter *sorter;
	GtkGesture *click;
	GtkEventController *keys;
	GMenu *menu;

	w->window = gtk_application_window_new(app);
	g_object_set_data_full(G_OBJECT(w->window), "win", w, win_free);
	gtk_window_set_default_size(GTK_WINDOW(w->window), 980, 640);
	g_action_map_add_action_entries(G_ACTION_MAP(w->window), actions, G_N_ELEMENTS(actions), w);

	header = gtk_header_bar_new();
	w->back = gtk_button_new_from_icon_name("go-previous-symbolic");
	gtk_widget_set_tooltip_text(w->back, "Back (Alt+Left)");
	g_signal_connect(w->back, "clicked", G_CALLBACK(back_clicked), w);
	w->forward = gtk_button_new_from_icon_name("go-next-symbolic");
	gtk_widget_set_tooltip_text(w->forward, "Forward (Alt+Right)");
	g_signal_connect(w->forward, "clicked", G_CALLBACK(forward_clicked), w);
	gtk_header_bar_pack_start(GTK_HEADER_BAR(header), w->back);
	gtk_header_bar_pack_start(GTK_HEADER_BAR(header), w->forward);
	w->pathbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
	gtk_widget_add_css_class(w->pathbar, "pathbar");
	gtk_header_bar_set_title_widget(GTK_HEADER_BAR(header), w->pathbar);
	menu = g_menu_new();
	g_menu_append(menu, "New folder", "win.new-folder");
	g_menu_append(menu, "Open a terminal here", "win.terminal");
	g_menu_append(menu, "Show hidden files", "win.hidden");
	g_menu_append(menu, "Empty the trash", "win.empty-trash");
	menu_button = gtk_menu_button_new();
	gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(menu_button), "open-menu-symbolic");
	gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(menu_button), G_MENU_MODEL(menu));
	g_object_unref(menu);
	gtk_header_bar_pack_end(GTK_HEADER_BAR(header), menu_button);
	w->search = gtk_search_entry_new();
	g_object_set(w->search, "placeholder-text", "Find in this folder", NULL);
	g_signal_connect(w->search, "search-changed", G_CALLBACK(search_changed), w);
	gtk_header_bar_pack_end(GTK_HEADER_BAR(header), w->search);
	gtk_window_set_titlebar(GTK_WINDOW(w->window), header);

	/* The folder's contents: filtered, sorted, selectable. */
	w->dir = gtk_directory_list_new(ATTRS, NULL);
	gtk_directory_list_set_monitored(w->dir, TRUE);
	w->filter = gtk_custom_filter_new(keep, w, NULL);
	w->filtered = gtk_filter_list_model_new(G_LIST_MODEL(w->dir), GTK_FILTER(w->filter));
	sorter = gtk_custom_sorter_new(compare, NULL, NULL);
	w->sorted = gtk_sort_list_model_new(G_LIST_MODEL(w->filtered), GTK_SORTER(sorter));
	w->selection = gtk_multi_selection_new(G_LIST_MODEL(w->sorted));
	factory = GTK_SIGNAL_LIST_ITEM_FACTORY(gtk_signal_list_item_factory_new());
	g_signal_connect(factory, "setup", G_CALLBACK(setup_item), NULL);
	g_signal_connect(factory, "bind", G_CALLBACK(bind_item), NULL);
	w->grid = gtk_grid_view_new(GTK_SELECTION_MODEL(w->selection), GTK_LIST_ITEM_FACTORY(factory));
	gtk_grid_view_set_max_columns(GTK_GRID_VIEW(w->grid), 24);
	gtk_grid_view_set_enable_rubberband(GTK_GRID_VIEW(w->grid), TRUE);
	gtk_widget_add_css_class(w->grid, "files");
	g_signal_connect(w->grid, "activate", G_CALLBACK(activated), w);
	g_signal_connect(w->filtered, "items-changed", G_CALLBACK(items_changed), w);
	g_signal_connect(w->dir, "notify::loading", G_CALLBACK(loading_changed), w);
	click = gtk_gesture_click_new();
	gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), GDK_BUTTON_SECONDARY);
	g_signal_connect(click, "pressed", G_CALLBACK(right_click), w);
	gtk_widget_add_controller(w->grid, GTK_EVENT_CONTROLLER(click));
	keys = gtk_event_controller_key_new();
	gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
	g_signal_connect(keys, "key-pressed", G_CALLBACK(key_pressed), w);
	gtk_widget_add_controller(w->window, keys);

	scroll = gtk_scrolled_window_new();
	gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), w->grid);
	gtk_widget_set_vexpand(scroll, TRUE);
	overlay = gtk_overlay_new();
	gtk_overlay_set_child(GTK_OVERLAY(overlay), scroll);
	w->empty = gtk_label_new("This folder is empty.");
	gtk_widget_add_css_class(w->empty, "placeholder");
	gtk_widget_set_can_target(w->empty, FALSE);
	gtk_overlay_add_overlay(GTK_OVERLAY(overlay), w->empty);
	bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	gtk_widget_add_css_class(bar, "statusbar");
	w->status = gtk_label_new("");
	gtk_label_set_xalign(GTK_LABEL(w->status), 0);
	gtk_label_set_ellipsize(GTK_LABEL(w->status), PANGO_ELLIPSIZE_END);
	gtk_box_append(GTK_BOX(bar), w->status);
	right = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_box_append(GTK_BOX(right), overlay);
	gtk_box_append(GTK_BOX(right), bar);

	w->places = gtk_list_box_new();
	gtk_widget_add_css_class(w->places, "places");
	fill_places(w);
	g_signal_connect(w->places, "row-activated", G_CALLBACK(place_selected), w);
	side = gtk_scrolled_window_new();
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(side), GTK_POLICY_NEVER,
				       GTK_POLICY_AUTOMATIC);
	gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(side), w->places);
	gtk_widget_set_size_request(side, 190, -1);

	paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
	gtk_paned_set_start_child(GTK_PANED(paned), side);
	gtk_paned_set_end_child(GTK_PANED(paned), right);
	gtk_paned_set_resize_start_child(GTK_PANED(paned), FALSE);
	gtk_paned_set_shrink_start_child(GTK_PANED(paned), FALSE);
	gtk_window_set_child(GTK_WINDOW(w->window), paned);

	navigate(w, folder, FALSE);
	gtk_window_present(GTK_WINDOW(w->window));
	gtk_widget_grab_focus(w->grid);
}

static void load_css(void)
{
	static gboolean done;
	GtkCssProvider *css;

	if (done)
		return;
	done = TRUE;
	css = gtk_css_provider_new();
	gtk_css_provider_load_from_path(css, CSS);
	gtk_style_context_add_provider_for_display(gdk_display_get_default(),
						   GTK_STYLE_PROVIDER(css),
						   GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
	g_object_unref(css);
}

static void activate(GtkApplication *app, gpointer data)
{
	GFile *home = g_file_new_for_path(g_get_home_dir());

	(void)data;
	load_css();
	new_window(app, home);
	g_object_unref(home);
}

/* wormos-files FOLDER (or a file: its folder), also from other programs. */
static void open_files(GApplication *app, GFile **files, int n, const char *hint, gpointer data)
{
	(void)hint; (void)data;
	load_css();
	for (int i = 0; i < n; i++) {
		GFile *folder = g_file_query_file_type(files[i], G_FILE_QUERY_INFO_NONE, NULL) ==
				G_FILE_TYPE_DIRECTORY ? g_object_ref(files[i]) :
				g_file_get_parent(files[i]);

		if (folder) {
			new_window(GTK_APPLICATION(app), folder);
			g_object_unref(folder);
		}
	}
}

int main(int argc, char **argv)
{
	GtkApplication *app = gtk_application_new(APP_ID, G_APPLICATION_HANDLES_OPEN);
	int status;

	g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);
	g_signal_connect(app, "open", G_CALLBACK(open_files), NULL);
	status = g_application_run(G_APPLICATION(app), argc, argv);
	g_object_unref(app);
	return status;
}
