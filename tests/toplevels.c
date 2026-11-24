/* Print the title and app id of every toplevel window, then exit. */
#include <stdio.h>
#include <string.h>
#include <wayland-client.h>
#include "wlr-foreign-toplevel-management-unstable-v1-client-protocol.h"
static struct zwlr_foreign_toplevel_manager_v1 *mgr;
struct tl { char title[256], app[256]; };
static void t_title(void *d, struct zwlr_foreign_toplevel_handle_v1 *h, const char *t) { snprintf(((struct tl *)d)->title, 256, "%s", t); }
static void t_app(void *d, struct zwlr_foreign_toplevel_handle_v1 *h, const char *a) { snprintf(((struct tl *)d)->app, 256, "%s", a); }
static void t_out(void *d, struct zwlr_foreign_toplevel_handle_v1 *h, struct wl_output *o) {}
static void t_state(void *d, struct zwlr_foreign_toplevel_handle_v1 *h, struct wl_array *s) {}
static void t_done(void *d, struct zwlr_foreign_toplevel_handle_v1 *h) {}
static void t_closed(void *d, struct zwlr_foreign_toplevel_handle_v1 *h) {}
static void t_parent(void *d, struct zwlr_foreign_toplevel_handle_v1 *h, struct zwlr_foreign_toplevel_handle_v1 *p) {}
static const struct zwlr_foreign_toplevel_handle_v1_listener tl_l = { t_title, t_app, t_out, t_out, t_state, t_done, t_closed, t_parent };
static struct tl tls[256]; static int n;
static void m_top(void *d, struct zwlr_foreign_toplevel_manager_v1 *m, struct zwlr_foreign_toplevel_handle_v1 *h) {
	if (n < 256) zwlr_foreign_toplevel_handle_v1_add_listener(h, &tl_l, &tls[n++]);
}
static void m_fin(void *d, struct zwlr_foreign_toplevel_manager_v1 *m) {}
static const struct zwlr_foreign_toplevel_manager_v1_listener m_l = { m_top, m_fin };
static void g_add(void *d, struct wl_registry *r, uint32_t name, const char *iface, uint32_t v) {
	if (!strcmp(iface, zwlr_foreign_toplevel_manager_v1_interface.name)) {
		mgr = wl_registry_bind(r, name, &zwlr_foreign_toplevel_manager_v1_interface, v < 3 ? v : 3);
		zwlr_foreign_toplevel_manager_v1_add_listener(mgr, &m_l, NULL);
	}
}
static void g_rm(void *d, struct wl_registry *r, uint32_t name) {}
static const struct wl_registry_listener r_l = { g_add, g_rm };
int main(void) {
	struct wl_display *dpy = wl_display_connect(NULL);
	if (!dpy) { fprintf(stderr, "no display\n"); return 1; }
	wl_registry_add_listener(wl_display_get_registry(dpy), &r_l, NULL);
	wl_display_roundtrip(dpy); wl_display_roundtrip(dpy); wl_display_roundtrip(dpy);
	for (int i = 0; i < n; i++) printf("%s\t%s\n", tls[i].app, tls[i].title);
	return 0;
}
