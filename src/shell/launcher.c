/*
 * The application launcher: a card over the desktop with a search field,
 * a sidebar of sections and the programs of the section chosen.
 *
 * Sections: Recent (the programs started last), All, the usual categories
 * of desktop entries (freedesktop Categories) and the security tool
 * sections that wh gives the entries of tools from wormholes
 * (X-WormOS-Section, in the order of tool-menus' sections file). Programs
 * with a window show as tiles; command line tools (Terminal=true: their
 * entry opens a terminal) as rows with their description.
 *
 * Typing anywhere searches every program by name, command, keywords and
 * description, best match first; Up and Down choose, Enter starts the
 * chosen one, Escape clears the search or closes the launcher, as does a
 * click outside the card.
 */

#include <gio/gdesktopappinfo.h>
#include <gtk4-layer-shell.h>

#include "shell.h"

#define SECTIONS_FILE "/usr/share/wormos/tool-menu/sections"
#define RECENT_MAX 12
#define RESULTS_MAX 60

struct entry {
	GAppInfo *info;
	char *id;               /* desktop file id, for Recent */
	char *name;
	char *comment;
	char *section;          /* sidebar section */
	gboolean terminal;
	gboolean tool;          /* a security section */
	gboolean action;        /* a quick action: found by searching only */
};

struct section {
	char *name;
	char *icon;
	int order;
	gboolean tool;
	GPtrArray *entries;     /* struct entry *, not owned */
};

static GtkWidget *window, *card, *search, *sidebar, *scroll, *hint;
static GPtrArray *entries;      /* struct entry *, owned */
static GPtrArray *sections;     /* struct section *, owned */
static char *current;           /* the section shown */
static GtkWidget *results;      /* the search results' list, while searching */
static gboolean filling;

/* ---- the programs ---- */

static void entry_free(gpointer p)
{
	struct entry *e = p;

	g_object_unref(e->info);
	g_free(e->id);
	g_free(e->name);
	g_free(e->comment);
	g_free(e->section);
	g_free(e);
}

static void section_free(gpointer p)
{
	struct section *s = p;

	g_free(s->name);
	g_free(s->icon);
	g_ptr_array_free(s->entries, TRUE);
	g_free(s);
}

/* The usual categories, by the first main category an entry has. */
static const struct {
	const char *category, *section;
} category_map[] = {
	{ "TerminalEmulator", "System" }, { "Settings", "System" }, { "System", "System" },
	{ "Development", "Development" }, { "Graphics", "Graphics" },
	{ "AudioVideo", "Multimedia" }, { "Audio", "Multimedia" }, { "Video", "Multimedia" },
	{ "Network", "Internet" }, { "Office", "Office" }, { "Education", "Education" },
	{ "Science", "Education" }, { "Game", "Games" }, { "Utility", "Accessories" },
};

static const struct {
	const char *name, *icon;
} app_sections[] = {
	{ "Accessories", "applications-utilities-symbolic" },
	{ "Development", "applications-engineering-symbolic" },
	{ "Education", "applications-science-symbolic" },
	{ "Games", "applications-games-symbolic" },
	{ "Graphics", "applications-graphics-symbolic" },
	{ "Internet", "applications-internet-symbolic" },
	{ "Multimedia", "applications-multimedia-symbolic" },
	{ "Office", "x-office-document-symbolic" },
	{ "System", "applications-system-symbolic" },
	{ "Other", "applications-other-symbolic" },
};

static char *app_section(GDesktopAppInfo *info)
{
	const char *cats = g_desktop_app_info_get_categories(info);
	char **list;
	char *found = NULL;

	if (!cats)
		return g_strdup("Other");
	list = g_strsplit(cats, ";", -1);
	for (size_t m = 0; m < G_N_ELEMENTS(category_map) && !found; m++)
		for (int i = 0; list[i] && !found; i++)
			if (!strcmp(list[i], category_map[m].category))
				found = g_strdup(category_map[m].section);
	g_strfreev(list);
	return found ? found : g_strdup("Other");
}

static struct section *section_get(const char *name, gboolean create)
{
	struct section *s;

	for (guint i = 0; i < sections->len; i++) {
		s = g_ptr_array_index(sections, i);
		if (!strcmp(s->name, name))
			return s;
	}
	if (!create)
		return NULL;
	s = g_new0(struct section, 1);
	s->name = g_strdup(name);
	s->order = 1000;
	s->entries = g_ptr_array_new();
	g_ptr_array_add(sections, s);
	return s;
}

/* Order and icons of the security sections. */
static void read_tool_sections(void)
{
	char *text = NULL, **lines;

	if (!g_file_get_contents(SECTIONS_FILE, &text, NULL, NULL))
		return;
	lines = g_strsplit(text, "\n", -1);
	for (int i = 0; lines[i]; i++) {
		char *eq = strstr(lines[i], " = "), **f;
		struct section *s;

		if (!eq)
			continue;
		f = g_strsplit(eq + 3, "|", 3);
		if (f[0] && f[1] && f[2] && (s = section_get(f[1], FALSE))) {
			s->order = atoi(f[0]);
			if (!s->icon && *f[2])
				s->icon = g_strdup(f[2]);
		}
		g_strfreev(f);
	}
	g_strfreev(lines);
	g_free(text);
}

static int by_name(gconstpointer a, gconstpointer b)
{
	const struct entry *x = *(struct entry **)a, *y = *(struct entry **)b;

	return g_utf8_collate(x->name, y->name);
}

static int section_order(gconstpointer a, gconstpointer b)
{
	const struct section *x = *(struct section **)a, *y = *(struct section **)b;
	int rank_x = x->tool ? 2 : 1, rank_y = y->tool ? 2 : 1;

	if (rank_x != rank_y)
		return rank_x - rank_y;
	if (x->order != y->order)
		return x->order - y->order;
	return g_utf8_collate(x->name, y->name);
}

static void load(void)
{
	GList *all = g_app_info_get_all();
	gboolean no_root = g_file_test("/etc/worm/no-root", G_FILE_TEST_EXISTS);

	if (entries)
		g_ptr_array_free(entries, TRUE);
	if (sections)
		g_ptr_array_free(sections, TRUE);
	entries = g_ptr_array_new_with_free_func(entry_free);
	sections = g_ptr_array_new_with_free_func(section_free);

	for (GList *l = all; l; l = l->next) {
		GAppInfo *info = l->data;
		GDesktopAppInfo *d = G_IS_DESKTOP_APP_INFO(info) ? G_DESKTOP_APP_INFO(info) : NULL;
		struct entry *e;
		char *tool_section;

		gboolean action;

		if (!d)
			continue;
		/* Quick actions (X-WormOS-Action, NoDisplay so other menus leave
		 * them out): take a snapshot, a settings page... */
		action = g_desktop_app_info_get_boolean(d, "X-WormOS-Action");
		if (!action && !g_app_info_should_show(info))
			continue;
		/* Programs that only work as root, where root is locked
		 * (/etc/worm/no-root: WormOS Amnesia). */
		if (no_root && g_desktop_app_info_get_boolean(d, "X-WormOS-Needs-Root"))
			continue;
		e = g_new0(struct entry, 1);
		e->action = action;
		e->info = g_object_ref(info);
		e->id = g_strdup(g_app_info_get_id(info));
		e->name = g_strdup(g_app_info_get_display_name(info));
		e->comment = g_strdup(g_app_info_get_description(info));
		e->terminal = g_desktop_app_info_get_boolean(d, "Terminal");
		tool_section = g_desktop_app_info_get_string(d, "X-WormOS-Section");
		if (action) {
			g_free(tool_section);
			e->section = g_strdup("Quick action");
		} else if (tool_section && *tool_section) {
			e->section = tool_section;
			e->tool = TRUE;
		} else {
			g_free(tool_section);
			e->section = app_section(d);
		}
		g_ptr_array_add(entries, e);
	}
	g_list_free_full(all, g_object_unref);
	g_ptr_array_sort(entries, by_name);

	for (guint i = 0; i < entries->len; i++) {
		struct entry *e = g_ptr_array_index(entries, i);
		struct section *s;

		if (e->action)
			continue;
		s = section_get(e->section, TRUE);
		s->tool = e->tool;
		g_ptr_array_add(s->entries, e);
	}
	for (guint i = 0; i < sections->len; i++) {
		struct section *s = g_ptr_array_index(sections, i);

		for (size_t a = 0; a < G_N_ELEMENTS(app_sections) && !s->tool; a++)
			if (!strcmp(s->name, app_sections[a].name)) {
				s->order = (int)a;
				s->icon = g_strdup(app_sections[a].icon);
			}
	}
	read_tool_sections();
	g_ptr_array_sort(sections, section_order);
}

/* ---- recently started ---- */

static char *recent_path(void)
{
	return g_build_filename(g_get_user_state_dir(), "wormos", "launcher-recent", NULL);
}

static char **recent_read(void)
{
	char *path = recent_path(), *text = NULL, **ids;

	if (!g_file_get_contents(path, &text, NULL, NULL))
		text = g_strdup("");
	ids = g_strsplit(g_strstrip(text), "\n", RECENT_MAX + 1);
	g_free(text);
	g_free(path);
	return ids;
}

static void recent_add(const char *id)
{
	char **old = recent_read(), *path = recent_path(), *dir;
	GString *out = g_string_new(id);
	int n = 1;

	for (int i = 0; old[i] && n < RECENT_MAX; i++)
		if (*old[i] && strcmp(old[i], id)) {
			g_string_append_printf(out, "\n%s", old[i]);
			n++;
		}
	g_string_append_c(out, '\n');
	dir = g_path_get_dirname(path);
	g_mkdir_with_parents(dir, 0700);
	g_file_set_contents(path, out->str, -1, NULL);
	g_free(dir);
	g_free(path);
	g_string_free(out, TRUE);
	g_strfreev(old);
}

static struct entry *entry_by_id(const char *id)
{
	for (guint i = 0; i < entries->len; i++) {
		struct entry *e = g_ptr_array_index(entries, i);

		if (e->id && !strcmp(e->id, id))
			return e;
	}
	return NULL;
}

static GPtrArray *recent_entries(void)
{
	char **ids = recent_read();
	GPtrArray *list = g_ptr_array_new();

	for (int i = 0; ids[i]; i++) {
		struct entry *e = *ids[i] ? entry_by_id(ids[i]) : NULL;

		if (e)
			g_ptr_array_add(list, e);
	}
	g_strfreev(ids);
	return list;
}

/* ---- starting a program ---- */

static void hide(void)
{
	gtk_widget_set_visible(window, FALSE);
}

static void launch(struct entry *e)
{
	GdkAppLaunchContext *ctx =
		gdk_display_get_app_launch_context(gtk_widget_get_display(window));
	GError *error = NULL;

	if (!g_app_info_launch(e->info, NULL, G_APP_LAUNCH_CONTEXT(ctx), &error)) {
		g_warning("cannot start %s: %s", e->name, error->message);
		g_error_free(error);
	} else if (e->id) {
		recent_add(e->id);
	}
	g_object_unref(ctx);
	hide();
}

static void tile_activated(GtkFlowBox *box, GtkFlowBoxChild *child, gpointer data)
{
	(void)box;
	(void)data;
	launch(g_object_get_data(G_OBJECT(child), "entry"));
}

static void row_activated(GtkListBox *box, GtkListBoxRow *row, gpointer data)
{
	(void)box;
	(void)data;
	launch(g_object_get_data(G_OBJECT(row), "entry"));
}

/* ---- the content ---- */

/* An icon for entries without one, or whose icon the theme lacks: by
 * what the program is. */
static const char *fallback_icon(struct entry *e)
{
	static const char *const by_section[][2] = {
		{ "Internet", "applications-internet" }, { "Multimedia", "applications-multimedia" },
		{ "Graphics", "applications-graphics" }, { "Office", "applications-office" },
		{ "Development", "applications-engineering" }, { "Games", "applications-games" },
		{ "Education", "applications-science" }, { "Science", "applications-science" },
		{ "System", "applications-system" }, { "Settings", "preferences-system" },
		{ "Accessories", "applications-utilities" }, { "Quick action", "system-run" },
	};

	if (e->terminal)
		return "utilities-terminal";
	for (size_t i = 0; i < G_N_ELEMENTS(by_section); i++)
		if (e->section && !strcmp(e->section, by_section[i][0]))
			return by_section[i][1];
	return "application-x-executable";
}

static GtkWidget *icon_for(struct entry *e, int size)
{
	GIcon *icon = g_app_info_get_icon(e->info);
	GtkIconTheme *theme = gtk_icon_theme_get_for_display(gdk_display_get_default());
	GtkWidget *image;

	/* A file icon that exists, or a themed one the theme has. */
	if (icon && G_IS_THEMED_ICON(icon) && !gtk_icon_theme_has_gicon(theme, icon))
		icon = NULL;
	if (icon && G_IS_FILE_ICON(icon)) {
		char *path = g_file_get_path(g_file_icon_get_file(G_FILE_ICON(icon)));

		if (!path || !g_file_test(path, G_FILE_TEST_EXISTS))
			icon = NULL;
		g_free(path);
	}
	image = icon ? gtk_image_new_from_gicon(icon) : gtk_image_new_from_icon_name(fallback_icon(e));

	gtk_image_set_pixel_size(GTK_IMAGE(image), size);
	return image;
}

static GtkWidget *heading(const char *text)
{
	GtkWidget *label = gtk_label_new(text);

	gtk_label_set_xalign(GTK_LABEL(label), 0);
	gtk_widget_add_css_class(label, "content-heading");
	return label;
}

/* A grid of tiles for programs with a window. */
static GtkWidget *tile_grid(GPtrArray *list, int max)
{
	GtkWidget *grid = gtk_flow_box_new();
	int shown = 0;

	gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(grid), GTK_SELECTION_NONE);
	gtk_flow_box_set_activate_on_single_click(GTK_FLOW_BOX(grid), TRUE);
	gtk_flow_box_set_homogeneous(GTK_FLOW_BOX(grid), TRUE);
	gtk_flow_box_set_min_children_per_line(GTK_FLOW_BOX(grid), 3);
	gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(grid), 8);
	gtk_flow_box_set_column_spacing(GTK_FLOW_BOX(grid), 4);
	gtk_flow_box_set_row_spacing(GTK_FLOW_BOX(grid), 4);
	gtk_widget_set_valign(grid, GTK_ALIGN_START);
	g_signal_connect(grid, "child-activated", G_CALLBACK(tile_activated), NULL);
	for (guint i = 0; i < list->len && (max <= 0 || shown < max); i++) {
		struct entry *e = g_ptr_array_index(list, i);
		GtkWidget *tile = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6), *label, *child;

		gtk_box_append(GTK_BOX(tile), icon_for(e, 48));
		label = gtk_label_new(e->name);
		gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
		gtk_label_set_max_width_chars(GTK_LABEL(label), 13);
		gtk_label_set_justify(GTK_LABEL(label), GTK_JUSTIFY_CENTER);
		gtk_box_append(GTK_BOX(tile), label);
		gtk_widget_add_css_class(tile, "app");
		if (e->comment)
			gtk_widget_set_tooltip_text(tile, e->comment);
		gtk_flow_box_append(GTK_FLOW_BOX(grid), tile);
		child = gtk_widget_get_parent(tile);
		g_object_set_data(G_OBJECT(child), "entry", e);
		shown++;
	}
	return grid;
}

/* A row: icon, name, description and, if given, a note at the end. */
static GtkWidget *entry_row(struct entry *e, const char *note)
{
	GtkWidget *row = gtk_list_box_row_new(), *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
	GtkWidget *text = gtk_box_new(GTK_ORIENTATION_VERTICAL, 1), *name, *comment;

	gtk_box_append(GTK_BOX(box), icon_for(e, 28));
	name = gtk_label_new(e->name);
	gtk_label_set_xalign(GTK_LABEL(name), 0);
	gtk_widget_add_css_class(name, "row-name");
	gtk_box_append(GTK_BOX(text), name);
	if (e->comment && *e->comment) {
		comment = gtk_label_new(e->comment);
		gtk_label_set_xalign(GTK_LABEL(comment), 0);
		gtk_label_set_ellipsize(GTK_LABEL(comment), PANGO_ELLIPSIZE_END);
		gtk_widget_add_css_class(comment, "row-comment");
		gtk_box_append(GTK_BOX(text), comment);
	}
	gtk_widget_set_hexpand(text, TRUE);
	gtk_widget_set_valign(text, GTK_ALIGN_CENTER);
	gtk_box_append(GTK_BOX(box), text);
	if (note) {
		GtkWidget *label = gtk_label_new(note);

		gtk_widget_add_css_class(label, "row-note");
		gtk_widget_set_valign(label, GTK_ALIGN_CENTER);
		gtk_box_append(GTK_BOX(box), label);
	}
	if (e->terminal) {
		GtkWidget *badge = gtk_image_new_from_icon_name("utilities-terminal-symbolic");

		gtk_widget_set_tooltip_text(badge, "Opens in a terminal");
		gtk_widget_add_css_class(badge, "row-badge");
		gtk_widget_set_valign(badge, GTK_ALIGN_CENTER);
		gtk_box_append(GTK_BOX(box), badge);
	}
	gtk_widget_add_css_class(box, "entry-row");
	gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), box);
	g_object_set_data(G_OBJECT(row), "entry", e);
	return row;
}

static GtkWidget *row_list(void)
{
	GtkWidget *list = gtk_list_box_new();

	gtk_list_box_set_selection_mode(GTK_LIST_BOX(list), GTK_SELECTION_NONE);
	gtk_list_box_set_activate_on_single_click(GTK_LIST_BOX(list), TRUE);
	gtk_widget_add_css_class(list, "entry-list");
	g_signal_connect(list, "row-activated", G_CALLBACK(row_activated), NULL);
	return list;
}

static void set_content(GtkWidget *child)
{
	gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), child);
	gtk_adjustment_set_value(gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(scroll)), 0);
}

/* Programs with a window as tiles, command line tools as rows. */
static void add_entries(GtkWidget *box, GPtrArray *list, gboolean headings)
{
	GPtrArray *windows = g_ptr_array_new(), *tools = g_ptr_array_new();

	for (guint i = 0; i < list->len; i++) {
		struct entry *e = g_ptr_array_index(list, i);

		g_ptr_array_add(e->terminal ? tools : windows, e);
	}
	if (windows->len) {
		if (headings && tools->len)
			gtk_box_append(GTK_BOX(box), heading("Programs"));
		gtk_box_append(GTK_BOX(box), tile_grid(windows, 0));
	}
	if (tools->len) {
		GtkWidget *rows = row_list();

		if (headings || windows->len)
			gtk_box_append(GTK_BOX(box), heading("Command line tools"));
		for (guint i = 0; i < tools->len; i++)
			gtk_list_box_append(GTK_LIST_BOX(rows),
					    entry_row(g_ptr_array_index(tools, i), NULL));
		gtk_box_append(GTK_BOX(box), rows);
	}
	g_ptr_array_free(windows, TRUE);
	g_ptr_array_free(tools, TRUE);
}

static GtkWidget *page(const char *title, const char *subtitle)
{
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10), *t, *s;

	gtk_widget_add_css_class(box, "content");
	t = gtk_label_new(title);
	gtk_label_set_xalign(GTK_LABEL(t), 0);
	gtk_widget_add_css_class(t, "content-title");
	gtk_box_append(GTK_BOX(box), t);
	if (subtitle) {
		s = gtk_label_new(subtitle);
		gtk_label_set_xalign(GTK_LABEL(s), 0);
		gtk_label_set_wrap(GTK_LABEL(s), TRUE);
		gtk_widget_add_css_class(s, "content-subtitle");
		gtk_box_append(GTK_BOX(box), s);
	}
	return box;
}

static void show_section(const char *name)
{
	GtkWidget *box;
	char *sub;

	if (name != current) {            /* name may be current itself */
		g_free(current);
		current = g_strdup(name);
	}
	name = current;
	results = NULL;
	if (!strcmp(name, "Recent")) {
		GPtrArray *recent = recent_entries();

		box = page("Recent", recent->len ? "The programs you started last."
					       : "Programs you start will show here.");
		if (recent->len) {
			GtkWidget *rows = row_list();

			for (guint i = 0; i < recent->len; i++) {
				struct entry *e = g_ptr_array_index(recent, i);

				gtk_list_box_append(GTK_LIST_BOX(rows), entry_row(e, e->section));
			}
			gtk_box_append(GTK_BOX(box), rows);
		}
		g_ptr_array_free(recent, TRUE);
	} else if (!strcmp(name, "All")) {
		GPtrArray *recent = recent_entries(), *windows = g_ptr_array_new();
		guint tools = 0;

		for (guint i = 0; i < entries->len; i++) {
			struct entry *e = g_ptr_array_index(entries, i);

			/* Security command line tools stay in their sections. */
			if (e->terminal && e->tool)
				tools++;
			else
				g_ptr_array_add(windows, e);
		}
		sub = tools ? g_strdup_printf("%u programs, and %u security tools in their "
					      "sections on the left.",
					      windows->len, tools)
			    : g_strdup_printf("%u programs.", windows->len);
		box = page("All programs", sub);
		g_free(sub);
		if (recent->len) {
			gtk_box_append(GTK_BOX(box), heading("Recent"));
			gtk_box_append(GTK_BOX(box), tile_grid(recent, 8));
			gtk_box_append(GTK_BOX(box), heading("Programs"));
		}
		gtk_box_append(GTK_BOX(box), tile_grid(windows, 0));
		g_ptr_array_free(windows, TRUE);
		g_ptr_array_free(recent, TRUE);
	} else {
		struct section *s = section_get(name, FALSE);

		sub = s ? g_strdup_printf(s->entries->len == 1 ? "%u program" : "%u programs",
					  s->entries->len) : NULL;
		box = page(name, sub);
		g_free(sub);
		if (s)
			add_entries(box, s->entries, FALSE);
	}
	set_content(box);
}

/* ---- search ---- */

static int score_text(const char *text, const char *needle)
{
	char *hay, *at;
	int score = 0;

	if (!text || !*text)
		return 0;
	hay = g_utf8_casefold(text, -1);
	if ((at = strstr(hay, needle))) {
		if (at == hay)
			score = 3;
		else if (!g_ascii_isalnum(at[-1]))
			score = 2;
		else
			score = 1;
	}
	g_free(hay);
	return score;
}

/* How well e matches every word of the query (0: not at all). */
static int score(struct entry *e, char **words)
{
	GDesktopAppInfo *d = G_DESKTOP_APP_INFO(e->info);
	const char *const *keywords = g_desktop_app_info_get_keywords(d);
	int total = 0;

	for (int w = 0; words[w]; w++) {
		int best = score_text(e->name, words[w]) * 30;

		best = MAX(best, score_text(g_app_info_get_executable(e->info), words[w]) * 12);
		best = MAX(best, score_text(g_desktop_app_info_get_generic_name(d), words[w]) * 8);
		for (int k = 0; keywords && keywords[k]; k++)
			best = MAX(best, score_text(keywords[k], words[w]) * 6);
		best = MAX(best, score_text(e->comment, words[w]) * 3);
		best = MAX(best, score_text(e->section, words[w]) * 2);
		if (!best)
			return 0;
		total += best;
	}
	return total;
}

struct hit {
	struct entry *e;
	int score;
};

static int by_score(gconstpointer a, gconstpointer b)
{
	const struct hit *x = a, *y = b;

	if (x->score != y->score)
		return y->score - x->score;
	return g_utf8_collate(x->e->name, y->e->name);
}

static void show_results(const char *query)
{
	char *folded = g_utf8_casefold(query, -1), **words = g_strsplit_set(g_strstrip(folded), " ", -1);
	GArray *hits = g_array_new(FALSE, FALSE, sizeof(struct hit));
	GtkWidget *box, *rows;
	char *title;

	{
		int j = 0;

		for (int i = 0; words[i]; i++) {    /* drop empty words */
			if (*words[i])
				words[j++] = words[i];
			else
				g_free(words[i]);
		}
		words[j] = NULL;
	}
	for (guint i = 0; i < entries->len; i++) {
		struct hit h = { g_ptr_array_index(entries, i), 0 };

		if ((h.score = score(h.e, words)) > 0)
			g_array_append_val(hits, h);
	}
	g_array_sort(hits, by_score);
	title = g_strdup_printf(hits->len ? "Results for “%s”"
					  : "Nothing found for “%s”", query);
	box = page(title, hits->len ? NULL : "Try another word, or install the program: "
					     "wh search NAME in a terminal.");
	g_free(title);
	rows = row_list();
	gtk_list_box_set_selection_mode(GTK_LIST_BOX(rows), GTK_SELECTION_SINGLE);
	for (guint i = 0; i < hits->len && i < RESULTS_MAX; i++) {
		struct entry *e = g_array_index(hits, struct hit, i).e;

		gtk_list_box_append(GTK_LIST_BOX(rows), entry_row(e, e->section));
	}
	if (hits->len) {
		gtk_box_append(GTK_BOX(box), rows);
		gtk_list_box_select_row(GTK_LIST_BOX(rows),
					gtk_list_box_get_row_at_index(GTK_LIST_BOX(rows), 0));
	}
	set_content(box);
	results = hits->len ? rows : NULL;
	g_array_free(hits, TRUE);
	g_strfreev(words);
	g_free(folded);
}

static void search_changed(GtkSearchEntry *entry, gpointer data)
{
	const char *text = gtk_editable_get_text(GTK_EDITABLE(entry));

	(void)data;
	if (filling)
		return;
	if (*text) {
		gtk_list_box_unselect_all(GTK_LIST_BOX(sidebar));
		show_results(text);
	} else {
		show_section(current ? current : "All");
		for (GtkWidget *r = gtk_widget_get_first_child(sidebar); r;
		     r = gtk_widget_get_next_sibling(r)) {
			const char *n = g_object_get_data(G_OBJECT(r), "section");

			if (n && current && !strcmp(n, current))
				gtk_list_box_select_row(GTK_LIST_BOX(sidebar), GTK_LIST_BOX_ROW(r));
		}
	}
}

/* Enter: the chosen result, or the first one. */
static void search_activated(GtkSearchEntry *entry, gpointer data)
{
	GtkListBoxRow *row;

	(void)entry;
	(void)data;
	if (!results)
		return;
	row = gtk_list_box_get_selected_row(GTK_LIST_BOX(results));
	if (!row)
		row = gtk_list_box_get_row_at_index(GTK_LIST_BOX(results), 0);
	if (row)
		launch(g_object_get_data(G_OBJECT(row), "entry"));
}

/* Up and Down in the search field choose among the results. */
static gboolean search_key(GtkEventControllerKey *ctl, guint key, guint code,
			   GdkModifierType state, gpointer data)
{
	GtkListBoxRow *row, *next;
	int index;

	(void)ctl;
	(void)code;
	(void)state;
	(void)data;
	if ((key != GDK_KEY_Down && key != GDK_KEY_Up) || !results)
		return FALSE;
	row = gtk_list_box_get_selected_row(GTK_LIST_BOX(results));
	index = row ? gtk_list_box_row_get_index(row) : -1;
	next = gtk_list_box_get_row_at_index(GTK_LIST_BOX(results),
					     key == GDK_KEY_Down ? index + 1 : MAX(index - 1, 0));
	if (next) {
		GtkAdjustment *adj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(scroll));
		graphene_point_t p;

		gtk_list_box_select_row(GTK_LIST_BOX(results), next);
		if (gtk_widget_compute_point(GTK_WIDGET(next), gtk_scrolled_window_get_child(
			GTK_SCROLLED_WINDOW(scroll)), &GRAPHENE_POINT_INIT(0, 0), &p)) {
			double top = gtk_adjustment_get_value(adj);
			double page_size = gtk_adjustment_get_page_size(adj);

			if (p.y < top)
				gtk_adjustment_set_value(adj, p.y);
			else if (p.y + gtk_widget_get_height(GTK_WIDGET(next)) > top + page_size)
				gtk_adjustment_set_value(adj, p.y + gtk_widget_get_height(GTK_WIDGET(next)) -
							 page_size);
		}
	}
	return TRUE;
}

/* ---- the sidebar ---- */

static void sidebar_selected(GtkListBox *box, GtkListBoxRow *row, gpointer data)
{
	const char *name;

	(void)box;
	(void)data;
	if (!row || filling || !(name = g_object_get_data(G_OBJECT(row), "section")))
		return;
	if (*gtk_editable_get_text(GTK_EDITABLE(search))) {
		filling = TRUE;
		gtk_editable_set_text(GTK_EDITABLE(search), "");
		filling = FALSE;
	}
	show_section(name);
}

static void sidebar_add(const char *name, const char *label, const char *icon, guint count)
{
	GtkWidget *row = gtk_list_box_row_new(), *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
	GtkWidget *image = gtk_image_new_from_icon_name(icon ? icon : "folder-symbolic"), *text;

	gtk_image_set_pixel_size(GTK_IMAGE(image), 16);
	gtk_box_append(GTK_BOX(box), image);
	text = gtk_label_new(label);
	gtk_label_set_xalign(GTK_LABEL(text), 0);
	gtk_label_set_ellipsize(GTK_LABEL(text), PANGO_ELLIPSIZE_END);
	gtk_widget_set_hexpand(text, TRUE);
	gtk_box_append(GTK_BOX(box), text);
	if (count) {
		char n[16];
		GtkWidget *c;

		g_snprintf(n, sizeof(n), "%u", count);
		c = gtk_label_new(n);
		gtk_widget_add_css_class(c, "count");
		gtk_box_append(GTK_BOX(box), c);
	}
	gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), box);
	g_object_set_data_full(G_OBJECT(row), "section", g_strdup(name), g_free);
	gtk_list_box_append(GTK_LIST_BOX(sidebar), row);
}

static void sidebar_heading(const char *text)
{
	GtkWidget *row = gtk_list_box_row_new(), *label = gtk_label_new(text);

	gtk_label_set_xalign(GTK_LABEL(label), 0);
	gtk_widget_add_css_class(label, "sidebar-heading");
	gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), label);
	gtk_list_box_row_set_selectable(GTK_LIST_BOX_ROW(row), FALSE);
	gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), FALSE);
	gtk_widget_add_css_class(row, "heading-row");
	gtk_widget_set_focusable(row, FALSE);
	gtk_list_box_append(GTK_LIST_BOX(sidebar), row);
}

static void fill_sidebar(void)
{
	GtkWidget *child;
	GPtrArray *recent = recent_entries();
	gboolean tools = FALSE;

	while ((child = gtk_widget_get_first_child(sidebar)))
		gtk_list_box_remove(GTK_LIST_BOX(sidebar), child);
	if (recent->len)
		sidebar_add("Recent", "Recent", "document-open-recent-symbolic", 0);
	sidebar_add("All", "All programs", "view-app-grid-symbolic", 0);
	sidebar_heading("Categories");
	for (guint i = 0; i < sections->len; i++) {
		struct section *s = g_ptr_array_index(sections, i);

		if (s->tool && !tools) {
			sidebar_heading("Security tools");
			tools = TRUE;
		}
		sidebar_add(s->name, s->name, s->icon, s->entries->len);
	}
	g_ptr_array_free(recent, TRUE);
}

/* ---- the window ---- */

static gboolean key_pressed(GtkEventControllerKey *ctl, guint key, guint code,
			    GdkModifierType state, gpointer data)
{
	(void)ctl;
	(void)code;
	(void)state;
	(void)data;
	if (key == GDK_KEY_Escape) {
		if (*gtk_editable_get_text(GTK_EDITABLE(search)))
			gtk_editable_set_text(GTK_EDITABLE(search), "");
		else
			hide();
		return TRUE;
	}
	return FALSE;
}

/* Escape in the search field: clear it first, then close. */
static void search_stopped(GtkSearchEntry *entry, gpointer data)
{
	(void)data;
	if (*gtk_editable_get_text(GTK_EDITABLE(entry)))
		gtk_editable_set_text(GTK_EDITABLE(entry), "");
	else
		hide();
}

static void background_clicked(GtkGestureClick *gesture, int n, double x, double y,
			       gpointer data)
{
	GtkWidget *target = gtk_widget_pick(window, x, y, GTK_PICK_DEFAULT);

	(void)gesture;
	(void)n;
	(void)data;
	if (!target || (target != card && !gtk_widget_is_ancestor(target, card)))
		hide();
}

static void build(GtkApplication *app)
{
	GtkWidget *top, *panes, *side_scroll, *keys_box;
	GtkEventController *keys, *search_keys;
	GtkGesture *click;

	window = gtk_application_window_new(app);
	gtk_layer_init_for_window(GTK_WINDOW(window));
	gtk_layer_set_namespace(GTK_WINDOW(window), "wormos-launcher");
	gtk_layer_set_layer(GTK_WINDOW(window), GTK_LAYER_SHELL_LAYER_OVERLAY);
	for (int e = 0; e < GTK_LAYER_SHELL_EDGE_ENTRY_NUMBER; e++)
		gtk_layer_set_anchor(GTK_WINDOW(window), e, TRUE);
	gtk_layer_set_exclusive_zone(GTK_WINDOW(window), -1);
	gtk_layer_set_keyboard_mode(GTK_WINDOW(window), GTK_LAYER_SHELL_KEYBOARD_MODE_EXCLUSIVE);
	gtk_widget_add_css_class(window, "launcher");

	card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_widget_add_css_class(card, "launcher-card");
	gtk_widget_set_halign(card, GTK_ALIGN_CENTER);
	gtk_widget_set_valign(card, GTK_ALIGN_CENTER);

	top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	gtk_widget_add_css_class(top, "launcher-top");
	search = gtk_search_entry_new();
	gtk_search_entry_set_placeholder_text(GTK_SEARCH_ENTRY(search),
					      "Search programs and tools");
	gtk_widget_set_hexpand(search, TRUE);
	gtk_widget_add_css_class(search, "launcher-search");
	g_signal_connect(search, "search-changed", G_CALLBACK(search_changed), NULL);
	g_signal_connect(search, "activate", G_CALLBACK(search_activated), NULL);
	g_signal_connect(search, "stop-search", G_CALLBACK(search_stopped), NULL);
	gtk_search_entry_set_search_delay(GTK_SEARCH_ENTRY(search), 60);
	search_keys = gtk_event_controller_key_new();
	gtk_event_controller_set_propagation_phase(search_keys, GTK_PHASE_CAPTURE);
	g_signal_connect(search_keys, "key-pressed", G_CALLBACK(search_key), NULL);
	gtk_widget_add_controller(search, search_keys);
	gtk_box_append(GTK_BOX(top), search);
	gtk_box_append(GTK_BOX(card), top);

	panes = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	gtk_widget_set_vexpand(panes, TRUE);
	sidebar = gtk_list_box_new();
	gtk_list_box_set_selection_mode(GTK_LIST_BOX(sidebar), GTK_SELECTION_BROWSE);
	gtk_widget_add_css_class(sidebar, "launcher-sidebar");
	g_signal_connect(sidebar, "row-selected", G_CALLBACK(sidebar_selected), NULL);
	side_scroll = gtk_scrolled_window_new();
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(side_scroll), GTK_POLICY_NEVER,
				       GTK_POLICY_AUTOMATIC);
	gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(side_scroll), sidebar);
	gtk_widget_add_css_class(side_scroll, "sidebar-scroll");
	gtk_box_append(GTK_BOX(panes), side_scroll);

	scroll = gtk_scrolled_window_new();
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER,
				       GTK_POLICY_AUTOMATIC);
	gtk_widget_set_hexpand(scroll, TRUE);
	gtk_box_append(GTK_BOX(panes), scroll);
	gtk_box_append(GTK_BOX(card), panes);

	keys_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	gtk_widget_add_css_class(keys_box, "launcher-hint");
	hint = gtk_label_new("Type to search  ·  ↑↓ choose  ·  "
			     "Enter starts  ·  Esc closes");
	gtk_widget_set_hexpand(hint, TRUE);
	gtk_label_set_xalign(GTK_LABEL(hint), 0.5);
	gtk_box_append(GTK_BOX(keys_box), hint);
	gtk_box_append(GTK_BOX(card), keys_box);

	gtk_window_set_child(GTK_WINDOW(window), card);
	gtk_search_entry_set_key_capture_widget(GTK_SEARCH_ENTRY(search), window);
	keys = gtk_event_controller_key_new();
	g_signal_connect(keys, "key-pressed", G_CALLBACK(key_pressed), NULL);
	gtk_widget_add_controller(window, keys);
	click = gtk_gesture_click_new();
	g_signal_connect(click, "pressed", G_CALLBACK(background_clicked), NULL);
	gtk_widget_add_controller(window, GTK_EVENT_CONTROLLER(click));
}

/* The card fills most of a small screen and stays a comfortable size on
 * a large one. */
static void size_card(void)
{
	GListModel *monitors = gdk_display_get_monitors(gtk_widget_get_display(window));
	GdkMonitor *m = g_list_model_get_n_items(monitors) ? g_list_model_get_item(monitors, 0) : NULL;
	int w = 1040, h = 680;

	if (m) {
		GdkRectangle r;

		gdk_monitor_get_geometry(m, &r);
		w = MIN(w, r.width - 48);
		h = MIN(h, r.height - 96);
		g_object_unref(m);
	}
	gtk_widget_set_size_request(card, MAX(w, 360), MAX(h, 320));
}

void launcher_toggle(GtkApplication *app)
{
	const char *start = "All";

	if (!window)
		build(app);
	if (gtk_widget_get_visible(window)) {
		hide();
		return;
	}
	load();
	filling = TRUE;
	gtk_editable_set_text(GTK_EDITABLE(search), "");
	fill_sidebar();
	/* Opens on All, with the recent programs at its top. */
	for (GtkWidget *r = gtk_widget_get_first_child(sidebar); r;
	     r = gtk_widget_get_next_sibling(r)) {
		const char *n = g_object_get_data(G_OBJECT(r), "section");

		if (n && !strcmp(n, start))
			gtk_list_box_select_row(GTK_LIST_BOX(sidebar), GTK_LIST_BOX_ROW(r));
	}
	filling = FALSE;
	show_section(start);
	size_card();
	gtk_window_present(GTK_WINDOW(window));
	gtk_widget_grab_focus(search);
}
