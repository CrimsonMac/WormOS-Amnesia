/*
 * wormos-viewer - Viewer: pictures and PDF documents.
 *
 * Pictures (PNG, JPEG, GIF, WebP, TIFF, BMP, SVG... what GTK's loaders read)
 * and PDF documents (through Poppler's GLib interface, drawn with cairo).
 * The picture or page fits the window; + and - zoom, 0 fits again. Left and
 * right (or Page Up/Down) go through the pages of a document, or through
 * the other pictures in the same folder. R rotates a picture, F11 is full
 * screen, Ctrl+O opens another file.
 *
 *     wormos-viewer [FILE]
 *
 * PDF links, forms and JavaScript are not acted on: the document is only
 * drawn, so a hostile PDF has little to work with (open it in a blackhole
 * from Files for more).
 */

#define _GNU_SOURCE
#include <gtk/gtk.h>
#include <poppler.h>
#include <string.h>

#define APP_ID "org.wormos.Viewer"

struct win {
	GtkWidget *window, *area, *scroll, *status, *title_label, *prev, *next, *empty;
	GFile *file;
	GdkTexture *picture;            /* a picture, or */
	cairo_surface_t *surface;       /* the picture, ready to draw */
	PopplerDocument *pdf;           /* a document */
	int page, pages;
	double zoom;                    /* 0: fit the window */
	int rotation;                   /* pictures: 0, 90, 180, 270 */
	GPtrArray *siblings;            /* the folder's pictures, for left/right */
	guint sibling;
};

static const char *const picture_types[] = { "image/png", "image/jpeg", "image/gif",
	"image/webp", "image/tiff", "image/bmp", "image/svg+xml", "image/x-icon",
	"image/vnd.microsoft.icon", "image/x-portable-pixmap", "image/avif", NULL };

static void open_file(struct win *w, GFile *file);

static gboolean is_picture(const char *type)
{
	for (int i = 0; picture_types[i]; i++)
		if (type && g_content_type_is_a(type, picture_types[i]))
			return TRUE;
	return type && g_str_has_prefix(type, "image/");
}

/* ---- drawing ---- */

/* The size of what is shown, unscaled (pixels, or PDF points). */
static void natural_size(struct win *w, double *width, double *height)
{
	*width = *height = 0;
	if (w->picture) {
		*width = gdk_texture_get_width(w->picture);
		*height = gdk_texture_get_height(w->picture);
		if (w->rotation % 180) {
			double t = *width;

			*width = *height;
			*height = t;
		}
	} else if (w->pdf) {
		PopplerPage *page = poppler_document_get_page(w->pdf, w->page);

		if (page) {
			poppler_page_get_size(page, width, height);
			g_object_unref(page);
		}
	}
}

static double scale_for(struct win *w)
{
	double nw, nh, aw, ah;

	if (w->zoom > 0)
		return w->zoom;
	natural_size(w, &nw, &nh);
	aw = gtk_widget_get_width(w->scroll) - 24;
	ah = gtk_widget_get_height(w->scroll) - 24;
	if (nw <= 0 || nh <= 0 || aw <= 0 || ah <= 0)
		return 1;
	/* Pictures are never enlarged to fit; documents are, to the window. */
	if (w->picture)
		return MIN(1.0, MIN(aw / nw, ah / nh));
	return MIN(aw / nw, ah / nh);
}

static void update_size(struct win *w)
{
	double nw, nh, s = scale_for(w);

	natural_size(w, &nw, &nh);
	gtk_drawing_area_set_content_width(GTK_DRAWING_AREA(w->area), (int)(nw * s));
	gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(w->area), (int)(nh * s));
	gtk_widget_queue_draw(w->area);
}

static void draw(GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer data)
{
	struct win *w = data;
	double nw, nh, s = scale_for(w);

	(void)area;
	natural_size(w, &nw, &nh);
	if (nw <= 0)
		return;
	cairo_translate(cr, (width - nw * s) / 2, (height - nh * s) / 2);
	cairo_scale(cr, s, s);
	if (w->picture) {
		double tw = gdk_texture_get_width(w->picture), th = gdk_texture_get_height(w->picture);

		cairo_translate(cr, nw / 2, nh / 2);
		cairo_rotate(cr, w->rotation * G_PI / 180);
		cairo_translate(cr, -tw / 2, -th / 2);
		cairo_set_source_surface(cr, w->surface, 0, 0);
		cairo_pattern_set_filter(cairo_get_source(cr), s < 1 ? CAIRO_FILTER_GOOD :
					 CAIRO_FILTER_NEAREST);
		cairo_paint(cr);
	} else if (w->pdf) {
		PopplerPage *page = poppler_document_get_page(w->pdf, w->page);

		/* Paper is white whatever the theme. */
		cairo_set_source_rgb(cr, 1, 1, 1);
		cairo_rectangle(cr, 0, 0, nw, nh);
		cairo_fill(cr);
		if (page) {
			poppler_page_render(page, cr);
			g_object_unref(page);
		}
	}
}

static void update_status(struct win *w)
{
	char *text = NULL, *name = w->file ? g_file_get_basename(w->file) : g_strdup("Viewer");
	int percent = (int)(scale_for(w) * 100 + 0.5);

	gtk_window_set_title(GTK_WINDOW(w->window), name);
	gtk_label_set_text(GTK_LABEL(w->title_label), name);
	if (w->pdf)
		text = g_strdup_printf("Page %d of %d  ·  %d%%", w->page + 1, w->pages, percent);
	else if (w->picture) {
		char of[48] = "";

		if (w->siblings && w->siblings->len > 1)
			snprintf(of, sizeof(of), "  ·  %u of %u", w->sibling + 1, w->siblings->len);
		text = g_strdup_printf("%d × %d  ·  %d%%%s", gdk_texture_get_width(w->picture),
				       gdk_texture_get_height(w->picture), percent, of);
	}
	gtk_label_set_text(GTK_LABEL(w->status), text ? text : "");
	gtk_widget_set_sensitive(w->prev, w->pdf ? w->page > 0 :
				 w->siblings && w->siblings->len > 1);
	gtk_widget_set_sensitive(w->next, w->pdf ? w->page + 1 < w->pages :
				 w->siblings && w->siblings->len > 1);
	gtk_widget_set_visible(w->empty, !w->pdf && !w->picture);
	g_free(text);
	g_free(name);
}

static void resized(GtkWidget *widget, GParamSpec *p, gpointer data)
{
	struct win *w = data;

	(void)widget; (void)p;
	if (w->zoom == 0) {
		update_size(w);
		update_status(w);
	}
}

/* ---- going through pages and pictures ---- */

static void step(struct win *w, int by)
{
	if (w->pdf) {
		int page = CLAMP(w->page + by, 0, w->pages - 1);

		if (page == w->page)
			return;
		w->page = page;
	} else if (w->siblings && w->siblings->len > 1) {
		w->sibling = (w->sibling + w->siblings->len + by) % w->siblings->len;
		open_file(w, g_ptr_array_index(w->siblings, w->sibling));
		return;
	} else {
		return;
	}
	update_size(w);
	update_status(w);
}

static void prev_clicked(GtkButton *b, gpointer d) { (void)b; step(d, -1); }
static void next_clicked(GtkButton *b, gpointer d) { (void)b; step(d, 1); }

static void set_zoom(struct win *w, double zoom)
{
	w->zoom = zoom > 0 ? CLAMP(zoom, 0.05, 16) : 0;
	update_size(w);
	update_status(w);
}

/* The pictures next to this one, sorted as Files sorts them. */
static int by_name(gconstpointer a, gconstpointer b)
{
	char *x = g_file_get_basename(*(GFile **)a), *y = g_file_get_basename(*(GFile **)b);
	char *kx = g_utf8_collate_key_for_filename(x, -1), *ky = g_utf8_collate_key_for_filename(y, -1);
	int r = strcmp(kx, ky);

	g_free(x); g_free(y); g_free(kx); g_free(ky);
	return r;
}

static void find_siblings(struct win *w)
{
	GFile *dir = g_file_get_parent(w->file);
	GFileEnumerator *e = dir ? g_file_enumerate_children(dir, "standard::name,standard::content-type,"
							     "standard::is-hidden", 0, NULL, NULL) : NULL;
	GFileInfo *info;

	if (w->siblings)
		g_ptr_array_unref(w->siblings);
	w->siblings = g_ptr_array_new_with_free_func(g_object_unref);
	while (e && (info = g_file_enumerator_next_file(e, NULL, NULL))) {
		if (!g_file_info_get_is_hidden(info) && is_picture(g_file_info_get_content_type(info)))
			g_ptr_array_add(w->siblings, g_file_get_child(dir, g_file_info_get_name(info)));
		g_object_unref(info);
	}
	g_ptr_array_sort(w->siblings, by_name);
	w->sibling = 0;
	for (guint i = 0; i < w->siblings->len; i++)
		if (g_file_equal(g_ptr_array_index(w->siblings, i), w->file))
			w->sibling = i;
	g_clear_object(&e);
	g_clear_object(&dir);
}

/* ---- opening ---- */

static void show_error(struct win *w, const char *text)
{
	gtk_label_set_text(GTK_LABEL(w->empty), text);
	gtk_widget_set_visible(w->empty, TRUE);
}

static void open_file(struct win *w, GFile *file)
{
	GFileInfo *info = g_file_query_info(file, G_FILE_ATTRIBUTE_STANDARD_CONTENT_TYPE, 0, NULL, NULL);
	const char *type = info ? g_file_info_get_content_type(info) : NULL;
	gboolean same_folder = w->file && w->siblings && is_picture(type);
	GError *error = NULL;

	g_clear_object(&w->picture);
	g_clear_pointer(&w->surface, cairo_surface_destroy);
	g_clear_object(&w->pdf);
	g_set_object(&w->file, file);
	w->zoom = 0;
	w->rotation = 0;
	w->page = w->pages = 0;
	if (type && g_content_type_is_a(type, "application/pdf")) {
		w->pdf = poppler_document_new_from_gfile(file, NULL, NULL, &error);
		if (w->pdf)
			w->pages = poppler_document_get_n_pages(w->pdf);
		g_clear_pointer(&w->siblings, g_ptr_array_unref);
	} else if (is_picture(type)) {
		w->picture = gdk_texture_new_from_file(file, &error);
		if (w->picture) {
			/* GTK's default memory format is cairo's ARGB32. */
			w->surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
								gdk_texture_get_width(w->picture),
								gdk_texture_get_height(w->picture));
			gdk_texture_download(w->picture, cairo_image_surface_get_data(w->surface),
					     cairo_image_surface_get_stride(w->surface));
			cairo_surface_mark_dirty(w->surface);
		}
		/* Keep the list when stepping through it. */
		if (!same_folder || !w->siblings ||
		    !g_file_equal(g_ptr_array_index(w->siblings, w->sibling), file))
			find_siblings(w);
	} else {
		show_error(w, "Viewer shows pictures and PDF documents.");
	}
	if (error) {
		char *text = g_strdup_printf("Cannot open it: %s", error->message);

		show_error(w, text);
		g_free(text);
		g_error_free(error);
	}
	g_clear_object(&info);
	update_size(w);
	update_status(w);
	if (error == NULL && (w->pdf || w->picture))
		gtk_widget_set_visible(w->empty, FALSE);
}

static void opened(GObject *source, GAsyncResult *res, gpointer data)
{
	GFile *file = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(source), res, NULL);

	if (file) {
		open_file(data, file);
		g_object_unref(file);
	}
}

static void ask_open(struct win *w)
{
	GtkFileDialog *dialog = gtk_file_dialog_new();
	GtkFileFilter *filter = gtk_file_filter_new();
	GListStore *filters = g_list_store_new(GTK_TYPE_FILE_FILTER);

	gtk_file_filter_set_name(filter, "Pictures and PDF documents");
	gtk_file_filter_add_mime_type(filter, "application/pdf");
	gtk_file_filter_add_mime_type(filter, "image/*");
	g_list_store_append(filters, filter);
	gtk_file_dialog_set_filters(dialog, G_LIST_MODEL(filters));
	gtk_file_dialog_open(dialog, GTK_WINDOW(w->window), NULL, opened, w);
	g_object_unref(filters);
	g_object_unref(filter);
	g_object_unref(dialog);
}

static void open_clicked(GtkButton *b, gpointer d) { (void)b; ask_open(d); }

/* Files dropped on the window open. */
static gboolean dropped(GtkDropTarget *t, const GValue *value, double x, double y, gpointer data)
{
	(void)t; (void)x; (void)y;
	if (G_VALUE_HOLDS(value, GDK_TYPE_FILE_LIST)) {
		GSList *files = gdk_file_list_get_files(g_value_get_boxed(value));

		if (files) {
			open_file(data, files->data);
			g_slist_free(files);
			return TRUE;
		}
	}
	return FALSE;
}

/* ---- keys ---- */

static gboolean key_pressed(GtkEventControllerKey *c, guint key, guint code,
			    GdkModifierType state, gpointer data)
{
	struct win *w = data;
	gboolean ctrl = state & GDK_CONTROL_MASK;

	(void)c; (void)code;
	switch (key) {
	case GDK_KEY_Right: case GDK_KEY_Page_Down: case GDK_KEY_space:
		step(w, 1);
		return TRUE;
	case GDK_KEY_Left: case GDK_KEY_Page_Up: case GDK_KEY_BackSpace:
		step(w, -1);
		return TRUE;
	case GDK_KEY_Home:
		if (w->pdf) { w->page = 0; update_size(w); update_status(w); }
		return TRUE;
	case GDK_KEY_End:
		if (w->pdf) { w->page = w->pages - 1; update_size(w); update_status(w); }
		return TRUE;
	case GDK_KEY_plus: case GDK_KEY_equal: case GDK_KEY_KP_Add:
		set_zoom(w, scale_for(w) * 1.25);
		return TRUE;
	case GDK_KEY_minus: case GDK_KEY_KP_Subtract:
		set_zoom(w, scale_for(w) / 1.25);
		return TRUE;
	case GDK_KEY_0:
		set_zoom(w, 0);
		return TRUE;
	case GDK_KEY_1:
		set_zoom(w, 1);
		return TRUE;
	case GDK_KEY_r: case GDK_KEY_R:
		if (w->picture && !ctrl) {
			w->rotation = (w->rotation + (key == GDK_KEY_R ? 270 : 90)) % 360;
			update_size(w);
			update_status(w);
		}
		return TRUE;
	case GDK_KEY_F11:
		if (gtk_window_is_fullscreen(GTK_WINDOW(w->window)))
			gtk_window_unfullscreen(GTK_WINDOW(w->window));
		else
			gtk_window_fullscreen(GTK_WINDOW(w->window));
		return TRUE;
	case GDK_KEY_Escape:
		if (gtk_window_is_fullscreen(GTK_WINDOW(w->window)))
			gtk_window_unfullscreen(GTK_WINDOW(w->window));
		return TRUE;
	case GDK_KEY_o:
		if (ctrl) { ask_open(w); return TRUE; }
		break;
	case GDK_KEY_w: case GDK_KEY_q:
		if (ctrl) { gtk_window_close(GTK_WINDOW(w->window)); return TRUE; }
		break;
	}
	return FALSE;
}

/* Ctrl + scroll zooms. */
static gboolean scrolled(GtkEventControllerScroll *c, double dx, double dy, gpointer data)
{
	GdkModifierType state = gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(c));

	(void)dx;
	if (!(state & GDK_CONTROL_MASK) || dy == 0)
		return FALSE;
	set_zoom(data, scale_for(data) * (dy < 0 ? 1.15 : 1 / 1.15));
	return TRUE;
}

/* ---- the window ---- */

static void win_free(gpointer data)
{
	struct win *w = data;

	g_clear_object(&w->picture);
	g_clear_pointer(&w->surface, cairo_surface_destroy);
	g_clear_object(&w->pdf);
	g_clear_object(&w->file);
	g_clear_pointer(&w->siblings, g_ptr_array_unref);
	g_free(w);
}

static struct win *new_window(GtkApplication *app)
{
	struct win *w = g_new0(struct win, 1);
	GtkWidget *header, *open, *box, *overlay, *bar, *zoom_fit;
	GtkEventController *keys, *scroll;
	GtkDropTarget *drop;

	w->window = gtk_application_window_new(app);
	g_object_set_data_full(G_OBJECT(w->window), "win", w, win_free);
	gtk_window_set_default_size(GTK_WINDOW(w->window), 900, 680);
	header = gtk_header_bar_new();
	open = gtk_button_new_with_label("Open");
	gtk_widget_set_tooltip_text(open, "Open a picture or document (Ctrl+O)");
	g_signal_connect(open, "clicked", G_CALLBACK(open_clicked), w);
	gtk_header_bar_pack_start(GTK_HEADER_BAR(header), open);
	w->prev = gtk_button_new_from_icon_name("go-previous-symbolic");
	gtk_widget_set_tooltip_text(w->prev, "Previous (Left)");
	g_signal_connect(w->prev, "clicked", G_CALLBACK(prev_clicked), w);
	w->next = gtk_button_new_from_icon_name("go-next-symbolic");
	gtk_widget_set_tooltip_text(w->next, "Next (Right)");
	g_signal_connect(w->next, "clicked", G_CALLBACK(next_clicked), w);
	gtk_header_bar_pack_start(GTK_HEADER_BAR(header), w->prev);
	gtk_header_bar_pack_start(GTK_HEADER_BAR(header), w->next);
	w->title_label = gtk_label_new("Viewer");
	gtk_label_set_ellipsize(GTK_LABEL(w->title_label), PANGO_ELLIPSIZE_MIDDLE);
	gtk_widget_add_css_class(w->title_label, "title");
	gtk_header_bar_set_title_widget(GTK_HEADER_BAR(header), w->title_label);
	gtk_window_set_titlebar(GTK_WINDOW(w->window), header);

	w->area = gtk_drawing_area_new();
	gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(w->area), draw, w, NULL);
	gtk_widget_set_halign(w->area, GTK_ALIGN_CENTER);
	gtk_widget_set_valign(w->area, GTK_ALIGN_CENTER);
	w->scroll = gtk_scrolled_window_new();
	gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(w->scroll), w->area);
	gtk_widget_set_vexpand(w->scroll, TRUE);
	g_signal_connect(w->scroll, "notify::width", G_CALLBACK(resized), w);
	g_signal_connect(w->scroll, "notify::height", G_CALLBACK(resized), w);
	overlay = gtk_overlay_new();
	gtk_overlay_set_child(GTK_OVERLAY(overlay), w->scroll);
	w->empty = gtk_label_new("Open a picture or a PDF document, or drop one here.");
	gtk_widget_set_opacity(w->empty, 0.6);
	gtk_widget_set_can_target(w->empty, FALSE);
	gtk_overlay_add_overlay(GTK_OVERLAY(overlay), w->empty);

	bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	gtk_widget_set_margin_start(bar, 12);
	gtk_widget_set_margin_end(bar, 6);
	w->status = gtk_label_new("");
	gtk_widget_set_hexpand(w->status, TRUE);
	gtk_label_set_xalign(GTK_LABEL(w->status), 0);
	gtk_widget_set_opacity(w->status, 0.75);
	zoom_fit = gtk_label_new("+ and − zoom, 0 fits, R rotates, F11 full screen");
	gtk_widget_set_opacity(zoom_fit, 0.5);
	gtk_box_append(GTK_BOX(bar), w->status);
	gtk_box_append(GTK_BOX(bar), zoom_fit);
	box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
	gtk_box_append(GTK_BOX(box), overlay);
	gtk_box_append(GTK_BOX(box), bar);
	gtk_widget_set_margin_bottom(box, 4);
	gtk_window_set_child(GTK_WINDOW(w->window), box);

	keys = gtk_event_controller_key_new();
	g_signal_connect(keys, "key-pressed", G_CALLBACK(key_pressed), w);
	gtk_widget_add_controller(w->window, keys);
	scroll = gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_VERTICAL);
	gtk_event_controller_set_propagation_phase(scroll, GTK_PHASE_CAPTURE);
	g_signal_connect(scroll, "scroll", G_CALLBACK(scrolled), w);
	gtk_widget_add_controller(w->scroll, scroll);
	drop = gtk_drop_target_new(GDK_TYPE_FILE_LIST, GDK_ACTION_COPY);
	g_signal_connect(drop, "drop", G_CALLBACK(dropped), w);
	gtk_widget_add_controller(w->window, GTK_EVENT_CONTROLLER(drop));
	update_status(w);
	gtk_window_present(GTK_WINDOW(w->window));
	return w;
}

static void activate(GtkApplication *app, gpointer data)
{
	(void)data;
	new_window(app);
}

static void open_files(GApplication *app, GFile **files, int n, const char *hint, gpointer data)
{
	(void)hint; (void)data;
	for (int i = 0; i < n; i++)
		open_file(new_window(GTK_APPLICATION(app)), files[i]);
}

int main(int argc, char **argv)
{
	GtkApplication *app = gtk_application_new(APP_ID, G_APPLICATION_HANDLES_OPEN |
						  G_APPLICATION_NON_UNIQUE);
	int status;

	g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);
	g_signal_connect(app, "open", G_CALLBACK(open_files), NULL);
	status = g_application_run(G_APPLICATION(app), argc, argv);
	g_object_unref(app);
	return status;
}
