/*
 * Display scale (wlr-output-management).
 *
 * The user's choice is kept in ~/.config/wormos/display ("scale = auto" or
 * a factor such as 1.5). Automatic means 2 for screens at least 2000
 * pixels high (4K) and 1 below. The scale is applied when the setting
 * changes and when an output appears or changes its mode (the window of a
 * virtual machine was resized), so a scale set by hand with wlr-randr
 * stays until then.
 */

#include <math.h>
#include <stdio.h>
#include <wayland-client.h>

#include "shell.h"
#include "wlr-output-management-unstable-v1-client-protocol.h"

struct mode {
	struct zwlr_output_mode_v1 *mode;
	int width, height;
};

struct head {
	struct zwlr_output_head_v1 *head;
	char *name;
	gboolean enabled;
	struct mode *current;
	double scale;
	int applied_width, applied_height;   /* mode the scale was set for */
	double applied_setting;
	GPtrArray *modes;                    /* of struct mode */
};

static struct zwlr_output_manager_v1 *manager;
static GPtrArray *heads;
static double setting = -1;                  /* -1: not read yet */

static char *setting_path(void)
{
	return g_build_filename(g_get_user_config_dir(), "wormos", "display", NULL);
}

double outputs_scale_setting(void)
{
	char *path, *text = NULL;

	if (setting >= 0)
		return setting;
	setting = 0;
	path = setting_path();
	if (g_file_get_contents(path, &text, NULL, NULL)) {
		double v;

		if (sscanf(text, "scale = %lf", &v) == 1 && v >= 0.5 && v <= 4)
			setting = v;
	} else if (g_getenv("WORM_DEFAULT_SCALE")) {
		/* No choice of the user's: the machine's (worm vm set scale). */
		double v = g_ascii_strtod(g_getenv("WORM_DEFAULT_SCALE"), NULL);

		if (v >= 0.5 && v <= 4)
			setting = v;
	}
	g_free(text);
	g_free(path);
	return setting;
}

static double wanted_scale(struct head *h)
{
	if (outputs_scale_setting() > 0)
		return setting;
	return h->current && h->current->height >= 2000 ? 2 : 1;
}

static void config_done(void *data, struct zwlr_output_configuration_v1 *config)
{
	(void)data;
	zwlr_output_configuration_v1_destroy(config);
}

static void config_failed(void *data, struct zwlr_output_configuration_v1 *config)
{
	(void)data;
	g_warning("the display scale could not be applied");
	zwlr_output_configuration_v1_destroy(config);
}

static const struct zwlr_output_configuration_v1_listener config_listener = {
	.succeeded = config_done,
	.failed = config_failed,
	.cancelled = config_done,
};

static void apply(uint32_t serial)
{
	struct zwlr_output_configuration_v1 *config;
	gboolean needed = FALSE;

	for (guint i = 0; i < heads->len; i++) {
		struct head *h = g_ptr_array_index(heads, i);

		if (!h->enabled || !h->current)
			continue;
		if (h->applied_width == h->current->width &&
		    h->applied_height == h->current->height &&
		    h->applied_setting == outputs_scale_setting())
			continue;       /* nothing changed since we last applied */
		h->applied_width = h->current->width;
		h->applied_height = h->current->height;
		h->applied_setting = outputs_scale_setting();
		if (fabs(h->scale - wanted_scale(h)) > 0.001)
			needed = TRUE;
	}
	if (!needed)
		return;

	config = zwlr_output_manager_v1_create_configuration(manager, serial);
	zwlr_output_configuration_v1_add_listener(config, &config_listener, NULL);
	for (guint i = 0; i < heads->len; i++) {
		struct head *h = g_ptr_array_index(heads, i);

		if (h->enabled) {
			struct zwlr_output_configuration_head_v1 *ch =
				zwlr_output_configuration_v1_enable_head(config, h->head);

			zwlr_output_configuration_head_v1_set_scale(
				ch, wl_fixed_from_double(wanted_scale(h)));
		} else {
			zwlr_output_configuration_v1_disable_head(config, h->head);
		}
	}
	zwlr_output_configuration_v1_apply(config);
}

static uint32_t last_serial;

/* The file changed (the Settings program, or another shell's menu). */
void outputs_reload(void)
{
	double old = setting;

	setting = -1;
	if (outputs_scale_setting() != old && manager && heads)
		apply(last_serial);
}

void outputs_set_scale(double scale)
{
	char *path = setting_path(), *dir = g_path_get_dirname(path), *text;

	setting = scale;
	g_mkdir_with_parents(dir, 0700);
	text = scale > 0 ? g_strdup_printf("scale = %.2f\n", scale) : g_strdup("scale = auto\n");
	if (!g_file_set_contents(path, text, -1, NULL))
		g_warning("cannot save %s", path);
	g_free(text);
	g_free(dir);
	g_free(path);
	if (manager && heads)
		apply(last_serial);
}

/* ---- modes ---- */

static void mode_size(void *data, struct zwlr_output_mode_v1 *m, int32_t w, int32_t h)
{
	struct mode *mode = data;

	(void)m;
	mode->width = w;
	mode->height = h;
}

static void mode_refresh(void *data, struct zwlr_output_mode_v1 *m, int32_t refresh)
{
	(void)data;
	(void)m;
	(void)refresh;
}

static void mode_preferred(void *data, struct zwlr_output_mode_v1 *m)
{
	(void)data;
	(void)m;
}

static void mode_finished(void *data, struct zwlr_output_mode_v1 *m)
{
	struct mode *mode = data;

	for (guint i = 0; heads && i < heads->len; i++) {
		struct head *h = g_ptr_array_index(heads, i);

		if (h->current == mode)
			h->current = NULL;
		g_ptr_array_remove(h->modes, mode);
	}
	zwlr_output_mode_v1_release(m);
	g_free(mode);
}

static const struct zwlr_output_mode_v1_listener mode_listener = {
	.size = mode_size,
	.refresh = mode_refresh,
	.preferred = mode_preferred,
	.finished = mode_finished,
};

/* ---- heads ---- */

static void head_name(void *data, struct zwlr_output_head_v1 *hd, const char *name)
{
	struct head *h = data;

	(void)hd;
	g_free(h->name);
	h->name = g_strdup(name);
}

static void head_ignore_string(void *data, struct zwlr_output_head_v1 *hd, const char *s)
{
	(void)data;
	(void)hd;
	(void)s;
}

static void head_physical_size(void *data, struct zwlr_output_head_v1 *hd, int32_t w, int32_t h)
{
	(void)data;
	(void)hd;
	(void)w;
	(void)h;
}

static void head_mode(void *data, struct zwlr_output_head_v1 *hd, struct zwlr_output_mode_v1 *m)
{
	struct head *h = data;
	struct mode *mode = g_new0(struct mode, 1);

	(void)hd;
	mode->mode = m;
	g_ptr_array_add(h->modes, mode);
	zwlr_output_mode_v1_add_listener(m, &mode_listener, mode);
}

static void head_enabled(void *data, struct zwlr_output_head_v1 *hd, int32_t enabled)
{
	struct head *h = data;

	(void)hd;
	h->enabled = enabled;
}

static void head_current_mode(void *data, struct zwlr_output_head_v1 *hd,
			      struct zwlr_output_mode_v1 *m)
{
	struct head *h = data;

	(void)hd;
	for (guint i = 0; i < h->modes->len; i++) {
		struct mode *mode = g_ptr_array_index(h->modes, i);

		if (mode->mode == m)
			h->current = mode;
	}
}

static void head_position(void *data, struct zwlr_output_head_v1 *hd, int32_t x, int32_t y)
{
	(void)data;
	(void)hd;
	(void)x;
	(void)y;
}

static void head_transform(void *data, struct zwlr_output_head_v1 *hd, int32_t t)
{
	(void)data;
	(void)hd;
	(void)t;
}

static void head_scale(void *data, struct zwlr_output_head_v1 *hd, wl_fixed_t scale)
{
	struct head *h = data;

	(void)hd;
	h->scale = wl_fixed_to_double(scale);
}

static void head_finished(void *data, struct zwlr_output_head_v1 *hd)
{
	struct head *h = data;

	g_ptr_array_remove(heads, h);
	g_ptr_array_free(h->modes, TRUE);
	g_free(h->name);
	g_free(h);
	zwlr_output_head_v1_release(hd);
}

static void head_adaptive_sync(void *data, struct zwlr_output_head_v1 *hd, uint32_t state)
{
	(void)data;
	(void)hd;
	(void)state;
}

static const struct zwlr_output_head_v1_listener head_listener = {
	.name = head_name,
	.description = head_ignore_string,
	.physical_size = head_physical_size,
	.mode = head_mode,
	.enabled = head_enabled,
	.current_mode = head_current_mode,
	.position = head_position,
	.transform = head_transform,
	.scale = head_scale,
	.finished = head_finished,
	.make = head_ignore_string,
	.model = head_ignore_string,
	.serial_number = head_ignore_string,
	.adaptive_sync = head_adaptive_sync,
};

/* ---- manager ---- */

static void manager_head(void *data, struct zwlr_output_manager_v1 *m,
			 struct zwlr_output_head_v1 *hd)
{
	struct head *h = g_new0(struct head, 1);

	(void)data;
	(void)m;
	h->head = hd;
	h->scale = 1;
	h->applied_setting = -1;
	h->modes = g_ptr_array_new();
	g_ptr_array_add(heads, h);
	zwlr_output_head_v1_add_listener(hd, &head_listener, h);
}

static void manager_done(void *data, struct zwlr_output_manager_v1 *m, uint32_t serial)
{
	(void)data;
	(void)m;
	last_serial = serial;
	apply(serial);
}

static void manager_finished(void *data, struct zwlr_output_manager_v1 *m)
{
	(void)data;
	zwlr_output_manager_v1_destroy(m);
	manager = NULL;
}

static const struct zwlr_output_manager_v1_listener manager_listener = {
	.head = manager_head,
	.done = manager_done,
	.finished = manager_finished,
};

void outputs_bind(void *registry, uint32_t name, uint32_t version)
{
	heads = g_ptr_array_new();
	manager = wl_registry_bind(registry, name, &zwlr_output_manager_v1_interface,
				   MIN(version, 4));
	zwlr_output_manager_v1_add_listener(manager, &manager_listener, NULL);
}
