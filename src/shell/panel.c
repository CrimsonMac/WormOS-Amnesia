/*
 * The panel along the top of every screen: the launcher button and the
 * open windows on the left, the clock in the middle, and on the right the
 * snapshots, the protections, the network, the volume and the power menu.
 */

#include <gtk4-layer-shell.h>

#include "shell.h"

struct panel {
	GtkWidget *window;
	GdkMonitor *monitor;
	GtkWidget *tasks;
	GtkWidget *clock;
	GtkWidget *calendar;
	GtkWidget *snap_label, *snap_info;
	GtkWidget *shield, *shield_info;
	GtkWidget *network, *network_info;
	GtkWidget *volume, *volume_output, *volume_mute, *volume_scale, *volume_percent;
	GtkWidget *confirm;             /* the power action waiting for a second click */
};

/*
 * The panel sits at the top, or at the bottom with WORM_SHELL_PANEL=bottom
 * (the session sets it for editions that want it); its menus then open
 * upwards.
 */
static int at_bottom(void)
{
	const char *edge = getenv("WORM_SHELL_PANEL");

	return edge && !strcmp(edge, "bottom");
}

static void menu_direction(GtkWidget *button)
{
	if (at_bottom())
		gtk_menu_button_set_direction(GTK_MENU_BUTTON(button), GTK_ARROW_UP);
}

static GtkApplication *app;
static GPtrArray *panels;               /* of struct panel */
static gboolean volume_updating;        /* showing a new reading, not a change */

static void keyboard_while_open(GtkWidget *popover);

/* ---- the task list ---- */

static void task_clicked(GtkButton *button, gpointer data)
{
	struct toplevel *t = data;

	(void)button;
	if (t->activated && !t->minimized)
		toplevel_minimize(t);
	else
		toplevel_activate(t);
}

static void task_middle_click(GtkGestureClick *gesture, int n, double x, double y,
			      gpointer data)
{
	(void)gesture;
	(void)n;
	(void)x;
	(void)y;
	toplevel_close(data);
}

static GtkWidget *task_button(struct toplevel *t)
{
	GtkWidget *button = gtk_button_new(), *box, *label;
	GIcon *icon = app_icon(t->app_id);
	GtkGesture *middle;

	box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	gtk_box_append(GTK_BOX(box), gtk_image_new_from_gicon(icon));
	label = gtk_label_new(t->title && *t->title ? t->title : t->app_id);
	gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
	gtk_label_set_max_width_chars(GTK_LABEL(label), 24);
	gtk_box_append(GTK_BOX(box), label);
	gtk_button_set_child(GTK_BUTTON(button), box);
	gtk_widget_add_css_class(button, "task");
	if (t->activated)
		gtk_widget_add_css_class(button, "active");
	if (t->minimized)
		gtk_widget_add_css_class(button, "minimized");
	gtk_widget_set_tooltip_text(button, t->title);
	g_signal_connect(button, "clicked", G_CALLBACK(task_clicked), t);
	middle = gtk_gesture_click_new();
	gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(middle), 2);
	g_signal_connect(middle, "released", G_CALLBACK(task_middle_click), t);
	gtk_widget_add_controller(button, GTK_EVENT_CONTROLLER(middle));
	g_object_unref(icon);
	return button;
}

static void tasks_changed(void)
{
	GPtrArray *list = toplevels_list();

	for (guint p = 0; p < panels->len; p++) {
		struct panel *panel = g_ptr_array_index(panels, p);
		GtkWidget *child;

		while ((child = gtk_widget_get_first_child(panel->tasks)))
			gtk_box_remove(GTK_BOX(panel->tasks), child);
		for (guint i = 0; i < list->len; i++) {
			struct toplevel *t = g_ptr_array_index(list, i);

			if (t->ready)
				gtk_box_append(GTK_BOX(panel->tasks), task_button(t));
		}
	}
}

/* ---- clock and status ---- */

static gboolean tick(gpointer data)
{
	GDateTime *now = g_date_time_new_now_local();
	char *text = g_date_time_format(now, "%a %e %b   %H:%M");

	(void)data;
	for (guint p = 0; p < panels->len; p++) {
		struct panel *panel = g_ptr_array_index(panels, p);

		if (g_strcmp0(gtk_menu_button_get_label(GTK_MENU_BUTTON(panel->clock)), text))
			gtk_menu_button_set_label(GTK_MENU_BUTTON(panel->clock), text);
	}
	g_free(text);
	g_date_time_unref(now);
	return G_SOURCE_CONTINUE;
}

static void set_class(GtkWidget *w, const char *name, gboolean on)
{
	if (on)
		gtk_widget_add_css_class(w, name);
	else
		gtk_widget_remove_css_class(w, name);
}

static gboolean refresh_status(gpointer data)
{
	struct status s;
	char *snap_text, *snap_info, *shield_info, *network_info;
	const char *shield_icon, *network_icon;

	(void)data;
	status_read(&s);

	if (s.snapshots < 0) {
		snap_text = g_strdup("?");
		snap_info = g_strdup("The snapshot summary is not available.");
	} else if (s.snapshots == 0) {
		snap_text = g_strdup("0");
		snap_info = g_strdup("No snapshots yet.");
	} else {
		char *when = relative_time(s.last_date);

		snap_text = g_strdup_printf("%d", s.snapshots);
		snap_info = g_strdup_printf(
			"%d snapshot%s\nLast: number %d, %s%s%s%s\nAutomatic snapshots: %s%s",
			s.snapshots, s.snapshots == 1 ? "" : "s", s.last_id, when,
			*s.last_comment ? "\n\xe2\x80\x9c" : "", s.last_comment,
			*s.last_comment ? "\xe2\x80\x9d" : "", s.auto_snapshots ? "on" : "off",
			!strcmp(s.pending, "rewind") ? "\n\nA rewind happens at the next restart." :
			!strcmp(s.pending, "change") ? "\n\nA package change waits for a restart." :
			!strcmp(s.pending, "trial") ? "\n\nA trial is running (wh keep keeps it)." : "");
		g_free(when);
	}

	if (s.network_cut) {
		shield_icon = "security-low-symbolic";
		shield_info = g_strdup("The network is cut (worm panic or safe mode).");
	} else if (s.anon) {
		shield_icon = "network-vpn-symbolic";
		shield_info = g_strdup("Firewall: incoming connections blocked\n"
				       "Anonymous mode: all traffic goes through Tor");
	} else {
		shield_icon = "security-high-symbolic";
		shield_info = g_strdup("Firewall: incoming connections blocked\n"
				       "Anonymous mode: off (worm anon on)");
	}
	network_icon = s.online ? "network-wired-symbolic" : "network-wired-disconnected-symbolic";
	network_info = s.online ? g_strdup_printf("Connected%s%s", *s.address ? "\n" : "",
						  s.address)
				: g_strdup("Not connected");

	for (guint p = 0; p < panels->len; p++) {
		struct panel *panel = g_ptr_array_index(panels, p);

		gtk_label_set_text(GTK_LABEL(panel->snap_label), snap_text);
		gtk_label_set_text(GTK_LABEL(panel->snap_info), snap_info);
		gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(panel->shield), shield_icon);
		set_class(panel->shield, "warning", s.network_cut);
		set_class(panel->shield, "tor", s.anon && !s.network_cut);
		gtk_label_set_text(GTK_LABEL(panel->shield_info), shield_info);
		gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(panel->network), network_icon);
		gtk_widget_set_tooltip_text(panel->network, network_info);
		gtk_label_set_text(GTK_LABEL(panel->network_info), network_info);
	}
	g_free(snap_text);
	g_free(snap_info);
	g_free(shield_info);
	g_free(network_info);
	volume_refresh(FALSE);
	return G_SOURCE_CONTINUE;
}

/* ---- the volume ---- */

static const char *volume_icon(const struct volume *v)
{
	if (v->muted || v->level <= 0)
		return "audio-volume-muted-symbolic";
	if (v->level < 0.34)
		return "audio-volume-low-symbolic";
	if (v->level < 0.67)
		return "audio-volume-medium-symbolic";
	return "audio-volume-high-symbolic";
}

static void volume_changed(void)
{
	const struct volume *v = volume_get();
	int percent = (int)(v->level * 100 + 0.5);
	char *tooltip = v->muted ? g_strdup_printf("Volume %d%% (muted)", percent)
				 : g_strdup_printf("Volume %d%%", percent);
	char *text = g_strdup_printf("%d%%", percent);

	volume_updating = TRUE;
	for (guint p = 0; p < panels->len; p++) {
		struct panel *panel = g_ptr_array_index(panels, p);

		/* No sound device or no sound server: nothing to show. */
		gtk_widget_set_visible(panel->volume, v->available);
		gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(panel->volume), volume_icon(v));
		gtk_widget_set_tooltip_text(panel->volume, tooltip);
		gtk_label_set_text(GTK_LABEL(panel->volume_output), v->output ? v->output : "Sound");
		gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(panel->volume_mute), v->muted);
		gtk_button_set_icon_name(GTK_BUTTON(panel->volume_mute), volume_icon(v));
		gtk_range_set_value(GTK_RANGE(panel->volume_scale), percent);
		gtk_label_set_text(GTK_LABEL(panel->volume_percent), text);
	}
	volume_updating = FALSE;
	g_free(tooltip);
	g_free(text);
}

static void volume_moved(GtkRange *range, gpointer data)
{
	(void)data;
	if (volume_updating)
		return;
	volume_set(gtk_range_get_value(range) / 100);
	volume_changed();
}

static void volume_mute_toggled(GtkToggleButton *button, gpointer data)
{
	(void)data;
	if (volume_updating)
		return;
	volume_set_muted(gtk_toggle_button_get_active(button));
	volume_changed();
}

static gboolean volume_scrolled(GtkEventControllerScroll *scroll, double dx, double dy,
				gpointer data)
{
	(void)scroll;
	(void)dx;
	(void)data;
	if (dy == 0)
		return FALSE;
	volume_step(dy < 0 ? 1 : -1);
	volume_changed();
	return TRUE;
}

static void volume_opened(GtkPopover *popover, gpointer data)
{
	(void)popover;
	(void)data;
	volume_refresh(TRUE);
}

void panels_volume(const char *what)
{
	if (!strcmp(what, "up"))
		volume_step(1);
	else if (!strcmp(what, "down"))
		volume_step(-1);
	else if (!strcmp(what, "mute"))
		volume_toggle_mute();
	else
		return;
	volume_changed();
}

/* The volume menu: the output's name, a mute button and a slider. */
static GtkWidget *volume_button(struct panel *panel)
{
	GtkWidget *button = gtk_menu_button_new(), *popover = gtk_popover_new();
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8), *row;
	GtkEventController *scroll;

	gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(button), "audio-volume-high-symbolic");
	gtk_widget_set_visible(button, FALSE);
	panel->volume = button;

	panel->volume_output = gtk_label_new("Sound");
	gtk_widget_add_css_class(panel->volume_output, "heading");
	gtk_widget_set_halign(panel->volume_output, GTK_ALIGN_START);
	gtk_label_set_ellipsize(GTK_LABEL(panel->volume_output), PANGO_ELLIPSIZE_END);
	gtk_label_set_max_width_chars(GTK_LABEL(panel->volume_output), 32);
	gtk_box_append(GTK_BOX(box), panel->volume_output);

	row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	panel->volume_mute = gtk_toggle_button_new();
	gtk_button_set_icon_name(GTK_BUTTON(panel->volume_mute), "audio-volume-high-symbolic");
	gtk_widget_set_tooltip_text(panel->volume_mute, "Mute");
	gtk_widget_add_css_class(panel->volume_mute, "flat");
	g_signal_connect(panel->volume_mute, "toggled", G_CALLBACK(volume_mute_toggled), NULL);
	gtk_box_append(GTK_BOX(row), panel->volume_mute);
	panel->volume_scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 100, 1);
	gtk_scale_set_draw_value(GTK_SCALE(panel->volume_scale), FALSE);
	gtk_widget_set_hexpand(panel->volume_scale, TRUE);
	gtk_widget_set_size_request(panel->volume_scale, 220, -1);
	g_signal_connect(panel->volume_scale, "value-changed", G_CALLBACK(volume_moved), NULL);
	gtk_box_append(GTK_BOX(row), panel->volume_scale);
	panel->volume_percent = gtk_label_new("");
	gtk_label_set_width_chars(GTK_LABEL(panel->volume_percent), 4);
	gtk_label_set_xalign(GTK_LABEL(panel->volume_percent), 1);
	gtk_widget_add_css_class(panel->volume_percent, "numeric");
	gtk_box_append(GTK_BOX(row), panel->volume_percent);
	gtk_box_append(GTK_BOX(box), row);

	gtk_popover_set_child(GTK_POPOVER(popover), box);
	g_signal_connect(popover, "show", G_CALLBACK(volume_opened), NULL);
	keyboard_while_open(popover);
	gtk_menu_button_set_popover(GTK_MENU_BUTTON(button), popover);
	menu_direction(button);

	scroll = gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_VERTICAL |
						 GTK_EVENT_CONTROLLER_SCROLL_DISCRETE);
	g_signal_connect(scroll, "scroll", G_CALLBACK(volume_scrolled), NULL);
	gtk_widget_add_controller(button, scroll);
	return button;
}

/* ---- the power menu ---- */

/* Below 100%: more room on large screens (a 2560x1440 window at 80% has
 * the space of a 3200x1800 one). */
static const double scales[] = { 0, 0.75, 0.8, 0.9, 1, 1.25, 1.5, 1.75, 2 };

static void scale_selected(GtkDropDown *dropdown, GParamSpec *pspec, gpointer data)
{
	guint i = gtk_drop_down_get_selected(dropdown);

	(void)pspec;
	(void)data;
	if (i < G_N_ELEMENTS(scales) && scales[i] != outputs_scale_setting())
		outputs_set_scale(scales[i]);
}

static void power_label(GtkWidget *button, const char *text)
{
	GtkWidget *label = g_object_get_data(G_OBJECT(button), "text");

	gtk_label_set_text(GTK_LABEL(label),
			   text ? text : g_object_get_data(G_OBJECT(button), "label"));
}

static void power_clicked(GtkButton *button, gpointer data)
{
	struct panel *panel = data;
	const char *action = g_object_get_data(G_OBJECT(button), "action");

	/* Restart, shut down and panic ask for a second click. */
	if (strcmp(action, "logout") && panel->confirm != GTK_WIDGET(button)) {
		if (panel->confirm)
			power_label(panel->confirm, NULL);
		panel->confirm = GTK_WIDGET(button);
		power_label(GTK_WIDGET(button), !strcmp(action, "off") ? "Click again to shut down" :
			    !strcmp(action, "panic") ? "Click again to cut the network" :
			    "Click again to restart");
		return;
	}
	if (!strcmp(action, "logout"))
		power_logout();
	else if (!power_request(action))
		power_label(GTK_WIDGET(button), "Only administrators can do this");
	else if (!strcmp(action, "panic")) {
		power_label(GTK_WIDGET(button), "Network cut; 'worm panic off' (root) restores it");
		panel->confirm = NULL;
	}
}

static GtkWidget *power_button(struct panel *panel, const char *icon, const char *label,
			       const char *action)
{
	GtkWidget *button = gtk_button_new(), *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
	GtkWidget *text = gtk_label_new(label);

	gtk_box_append(GTK_BOX(box), gtk_image_new_from_icon_name(icon));
	gtk_box_append(GTK_BOX(box), text);
	gtk_button_set_child(GTK_BUTTON(button), box);
	g_object_set_data(G_OBJECT(button), "action", (gpointer)action);
	g_object_set_data(G_OBJECT(button), "label", (gpointer)label);
	g_object_set_data(G_OBJECT(button), "text", text);
	gtk_widget_add_css_class(button, "power-action");
	if (!strcmp(action, "off") || !strcmp(action, "panic"))
		gtk_widget_add_css_class(button, "destructive-action");
	g_signal_connect(button, "clicked", G_CALLBACK(power_clicked), panel);
	return button;
}

static void power_menu_closed(GtkPopover *popover, gpointer data)
{
	struct panel *panel = data;

	(void)popover;
	if (panel->confirm)
		power_label(panel->confirm, NULL);
	panel->confirm = NULL;
}

static GtkWidget *power_menu(struct panel *panel)
{
	static const char *const names[] = { "Automatic", "75%", "80%", "90%", "100%", "125%",
					     "150%", "175%", "200%", NULL };
	GtkWidget *button = gtk_menu_button_new(), *popover = gtk_popover_new();
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6), *row, *dropdown, *user;
	double current = outputs_scale_setting();

	gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(button), "system-shutdown-symbolic");
	gtk_widget_set_tooltip_text(button, "Display and power");

	user = gtk_label_new(g_get_real_name() && strcmp(g_get_real_name(), "Unknown")
			     ? g_get_real_name() : g_get_user_name());
	gtk_widget_add_css_class(user, "heading");
	gtk_widget_set_halign(user, GTK_ALIGN_START);
	gtk_box_append(GTK_BOX(box), user);

	row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
	gtk_box_append(GTK_BOX(row), gtk_image_new_from_icon_name("video-display-symbolic"));
	gtk_box_append(GTK_BOX(row), gtk_label_new("Display scale"));
	dropdown = gtk_drop_down_new_from_strings(names);
	gtk_widget_set_hexpand(dropdown, TRUE);
	gtk_widget_set_halign(dropdown, GTK_ALIGN_END);
	for (guint i = 0; i < G_N_ELEMENTS(scales); i++)
		if (scales[i] == current)
			gtk_drop_down_set_selected(GTK_DROP_DOWN(dropdown), i);
	g_signal_connect(dropdown, "notify::selected", G_CALLBACK(scale_selected), NULL);
	gtk_box_append(GTK_BOX(row), dropdown);
	gtk_box_append(GTK_BOX(box), row);

	gtk_box_append(GTK_BOX(box), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));
	/* worm panic: cut the network, stop blackholes, keep a snapshot. */
	gtk_box_append(GTK_BOX(box), power_button(panel, "network-offline-symbolic",
						  "Panic: cut the network", "panic"));
	gtk_box_append(GTK_BOX(box), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));
	gtk_box_append(GTK_BOX(box), power_button(panel, "system-log-out-symbolic", "Log out",
						  "logout"));
	gtk_box_append(GTK_BOX(box), power_button(panel, "system-reboot-symbolic", "Restart",
						  "reboot"));
	gtk_box_append(GTK_BOX(box), power_button(panel, "system-shutdown-symbolic",
						  "Shut down", "off"));
	gtk_popover_set_child(GTK_POPOVER(popover), box);
	g_signal_connect(popover, "closed", G_CALLBACK(power_menu_closed), panel);
	keyboard_while_open(popover);
	gtk_menu_button_set_popover(GTK_MENU_BUTTON(button), popover);
	menu_direction(button);
	return button;
}

/* ---- building a panel ---- */

static void menu_opened(GtkPopover *popover, gpointer data)
{
	(void)data;
	gtk_layer_set_keyboard_mode(GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(popover))),
				    GTK_LAYER_SHELL_KEYBOARD_MODE_ON_DEMAND);
}

static void menu_closed(GtkPopover *popover, gpointer data)
{
	(void)data;
	gtk_layer_set_keyboard_mode(GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(popover))),
				    GTK_LAYER_SHELL_KEYBOARD_MODE_NONE);
}

/* Menus can take the keyboard (Escape, typing in fields) while open. */
static void keyboard_while_open(GtkWidget *popover)
{
	g_signal_connect(popover, "show", G_CALLBACK(menu_opened), NULL);
	g_signal_connect(popover, "closed", G_CALLBACK(menu_closed), NULL);
}

static GtkWidget *info_button(const char *icon, const char *tooltip, GtkWidget **info,
			      GtkWidget **label)
{
	GtkWidget *button = gtk_menu_button_new(), *popover = gtk_popover_new();

	if (label) {
		GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);

		gtk_box_append(GTK_BOX(box), gtk_image_new_from_icon_name(icon));
		*label = gtk_label_new("");
		gtk_box_append(GTK_BOX(box), *label);
		gtk_menu_button_set_child(GTK_MENU_BUTTON(button), box);
	} else {
		gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(button), icon);
	}
	gtk_widget_set_tooltip_text(button, tooltip);
	*info = gtk_label_new("");
	gtk_label_set_xalign(GTK_LABEL(*info), 0);
	gtk_widget_add_css_class(*info, "info");
	gtk_popover_set_child(GTK_POPOVER(popover), *info);
	gtk_menu_button_set_popover(GTK_MENU_BUTTON(button), popover);
	menu_direction(button);
	keyboard_while_open(popover);
	return button;
}

static void snapshot_taken(GObject *source, GAsyncResult *res, gpointer data)
{
	GtkWidget *label = data;
	GError *error = NULL;

	if (g_subprocess_wait_check_finish(G_SUBPROCESS(source), res, &error)) {
		gtk_label_set_text(GTK_LABEL(label), "Snapshot taken.");
	} else {
		gtk_label_set_text(GTK_LABEL(label), "No snapshot: only administrators can "
				   "take one.");
		g_error_free(error);
	}
	refresh_status(NULL);
	g_object_unref(label);
}

static void take_snapshot(GtkButton *button, gpointer data)
{
	struct panel *panel = data;
	GError *error = NULL;
	GSubprocess *proc = g_subprocess_new(G_SUBPROCESS_FLAGS_STDOUT_SILENCE |
					     G_SUBPROCESS_FLAGS_STDERR_SILENCE, &error,
					     "/usr/lib/worm/snapshots", "now", "system",
					     "From the panel", NULL);

	(void)button;
	if (!proc) {
		g_warning("cannot take a snapshot: %s", error->message);
		g_error_free(error);
		return;
	}
	gtk_label_set_text(GTK_LABEL(panel->snap_info), "Taking a snapshot...");
	g_subprocess_wait_check_async(proc, NULL, snapshot_taken, g_object_ref(panel->snap_info));
	g_object_unref(proc);
}

static void open_snapshots(GtkButton *button, gpointer data)
{
	GError *error = NULL;

	(void)button;
	gtk_popover_popdown(GTK_POPOVER(data));
	if (!g_spawn_command_line_async("wormos-snapshots", &error)) {
		g_warning("cannot open Snapshots: %s", error->message);
		g_error_free(error);
	}
}

/* The snapshot menu: the summary and the two things people do most. */
static GtkWidget *snapshot_button(struct panel *panel)
{
	GtkWidget *button = info_button("camera-photo-symbolic", "Snapshots", &panel->snap_info,
					&panel->snap_label);
	GtkWidget *popover = GTK_WIDGET(gtk_menu_button_get_popover(GTK_MENU_BUTTON(button)));
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8), *row, *take, *open;

	g_object_ref(panel->snap_info);
	gtk_popover_set_child(GTK_POPOVER(popover), box);
	gtk_box_append(GTK_BOX(box), panel->snap_info);
	g_object_unref(panel->snap_info);
	row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	take = gtk_button_new_with_label("Take a snapshot");
	gtk_widget_add_css_class(take, "suggested-action");
	g_signal_connect(take, "clicked", G_CALLBACK(take_snapshot), panel);
	open = gtk_button_new_with_label("Open Snapshots");
	g_signal_connect(open, "clicked", G_CALLBACK(open_snapshots), popover);
	gtk_box_append(GTK_BOX(row), take);
	gtk_box_append(GTK_BOX(row), open);
	gtk_box_append(GTK_BOX(box), row);
	return button;
}

static void launcher_clicked(GtkButton *button, gpointer data)
{
	(void)button;
	(void)data;
	launcher_toggle(app);
}

static void calendar_opened(GtkPopover *popover, gpointer data)
{
	struct panel *panel = data;
	GDateTime *now = g_date_time_new_now_local();

	(void)popover;
	gtk_calendar_set_date(GTK_CALENDAR(panel->calendar), now);
	g_date_time_unref(now);
}

static void panel_new(GdkMonitor *monitor)
{
	struct panel *panel = g_new0(struct panel, 1);
	GtkWidget *window = gtk_application_window_new(app), *bar, *start, *end, *button;
	GtkWidget *popover;

	panel->window = window;
	panel->monitor = monitor;
	gtk_layer_init_for_window(GTK_WINDOW(window));
	gtk_layer_set_namespace(GTK_WINDOW(window), "wormos-panel");
	gtk_layer_set_layer(GTK_WINDOW(window), GTK_LAYER_SHELL_LAYER_TOP);
	gtk_layer_set_monitor(GTK_WINDOW(window), monitor);
	gtk_layer_set_anchor(GTK_WINDOW(window),
			     at_bottom() ? GTK_LAYER_SHELL_EDGE_BOTTOM : GTK_LAYER_SHELL_EDGE_TOP, TRUE);
	gtk_layer_set_anchor(GTK_WINDOW(window), GTK_LAYER_SHELL_EDGE_LEFT, TRUE);
	gtk_layer_set_anchor(GTK_WINDOW(window), GTK_LAYER_SHELL_EDGE_RIGHT, TRUE);
	gtk_layer_auto_exclusive_zone_enable(GTK_WINDOW(window));
	/* No keyboard focus, except while a menu of the panel is open. */
	gtk_layer_set_keyboard_mode(GTK_WINDOW(window), GTK_LAYER_SHELL_KEYBOARD_MODE_NONE);
	gtk_widget_add_css_class(window, "panel");

	bar = gtk_center_box_new();
	gtk_widget_add_css_class(bar, "bar");

	start = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
	button = gtk_button_new_from_icon_name("view-app-grid-symbolic");
	gtk_widget_add_css_class(button, "launcher-button");
	gtk_widget_set_tooltip_text(button, "Applications (Super)");
	g_signal_connect(button, "clicked", G_CALLBACK(launcher_clicked), NULL);
	gtk_box_append(GTK_BOX(start), button);
	panel->tasks = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
	gtk_widget_add_css_class(panel->tasks, "tasks");
	gtk_box_append(GTK_BOX(start), panel->tasks);
	gtk_center_box_set_start_widget(GTK_CENTER_BOX(bar), start);

	panel->clock = gtk_menu_button_new();
	gtk_widget_add_css_class(panel->clock, "clock");
	popover = gtk_popover_new();
	panel->calendar = gtk_calendar_new();
	gtk_popover_set_child(GTK_POPOVER(popover), panel->calendar);
	g_signal_connect(popover, "show", G_CALLBACK(calendar_opened), panel);
	keyboard_while_open(popover);
	gtk_menu_button_set_popover(GTK_MENU_BUTTON(panel->clock), popover);
	menu_direction(panel->clock);
	gtk_center_box_set_center_widget(GTK_CENTER_BOX(bar), panel->clock);

	end = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
	gtk_widget_add_css_class(end, "status");
	gtk_box_append(GTK_BOX(end), snapshot_button(panel));
	panel->shield = info_button("security-high-symbolic", "Protection",
				    &panel->shield_info, NULL);
	gtk_box_append(GTK_BOX(end), panel->shield);
	panel->network = info_button("network-wired-symbolic", "Network",
				     &panel->network_info, NULL);
	gtk_box_append(GTK_BOX(end), panel->network);
	gtk_box_append(GTK_BOX(end), volume_button(panel));
	gtk_box_append(GTK_BOX(end), power_menu(panel));
	gtk_center_box_set_end_widget(GTK_CENTER_BOX(bar), end);

	gtk_window_set_child(GTK_WINDOW(window), bar);
	g_ptr_array_add(panels, panel);
	gtk_window_present(GTK_WINDOW(window));
}

static void panel_free(struct panel *panel)
{
	gtk_window_destroy(GTK_WINDOW(panel->window));
	g_free(panel);
}

static void monitors_changed(GListModel *monitors, guint pos, guint removed, guint added,
			     gpointer data)
{
	(void)pos;
	(void)removed;
	(void)added;
	(void)data;
	/* Few monitors, rare changes: rebuild all panels. */
	while (panels->len)
		panel_free(g_ptr_array_steal_index(panels, panels->len - 1));
	for (guint i = 0; i < g_list_model_get_n_items(monitors); i++) {
		GdkMonitor *m = g_list_model_get_item(monitors, i);

		panel_new(m);
		g_object_unref(m);
	}
	tasks_changed();
	refresh_status(NULL);
	volume_changed();
	tick(NULL);
}

void panels_init(GtkApplication *application)
{
	GListModel *monitors = gdk_display_get_monitors(gdk_display_get_default());

	app = application;
	panels = g_ptr_array_new();
	toplevels_watch(tasks_changed);
	volume_watch(volume_changed);
	g_signal_connect(monitors, "items-changed", G_CALLBACK(monitors_changed), NULL);
	monitors_changed(monitors, 0, 0, 0, NULL);
	g_timeout_add_seconds(1, tick, NULL);
	g_timeout_add_seconds(5, refresh_status, NULL);
}
