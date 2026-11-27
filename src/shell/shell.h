/* Shared declarations for worm-shell, the WormOS desktop shell. */

#ifndef SHELL_H
#define SHELL_H

#include <gtk/gtk.h>

#define SHELL_APP_ID   "org.wormos.Shell"
#define SHELL_CSS      "/usr/share/wormos/shell/shell.css"
#define SHELL_VARIANT_CSS "/usr/share/wormos/shell/variant.css"

/* A window of any program, as the compositor reports it (toplevels.c). */
struct toplevel {
	void *handle;           /* struct zwlr_foreign_toplevel_handle_v1 */
	char *title;
	char *app_id;
	gboolean activated, minimized, maximized;
	gboolean ready;         /* the first "done" event arrived */
};

/* What the panel shows about the system (status.c). */
struct status {
	int snapshots;          /* -1: unknown */
	int last_id;
	gint64 last_date;
	char last_comment[256];
	gboolean auto_snapshots;
	char pending[16];       /* rewind, change, trial or empty */
	gboolean anon;          /* worm anon on */
	gboolean network_cut;   /* worm panic or safe mode */
	int tor_percent;        /* how far Tor has connected, -1: unknown or unreadable */
	int battery;            /* percent charged, -1: no battery */
	gboolean charging;      /* plugged in and charging (or full) */
	gboolean full;
	gboolean online;        /* a network device has a carrier */
	gboolean wireless;      /* ... and it is a Wi-Fi adapter */
	char address[64];       /* first IPv4 address, if any */
};

/* The default sound output (volume.c). */
struct volume {
	gboolean available;     /* a sound server with an output */
	double level;           /* 0 to 1 */
	gboolean muted;
	char *output;           /* its description, once known */
};

/* wayland.c */
void wayland_init(void);
void *wayland_seat(void);   /* struct wl_seat */

/* toplevels.c */
typedef void (*toplevels_changed_fn)(void);
void toplevels_bind(void *registry, uint32_t name, uint32_t version);
void toplevels_watch(toplevels_changed_fn fn);
GPtrArray *toplevels_list(void);   /* of struct toplevel, not owned */
void toplevel_activate(struct toplevel *t);
void toplevel_minimize(struct toplevel *t);
void toplevel_close(struct toplevel *t);

/* outputs.c */
void outputs_bind(void *registry, uint32_t name, uint32_t version);
double outputs_scale_setting(void);    /* 0: automatic */
void outputs_set_scale(double scale);  /* 0: automatic; saved for the user */

/* status.c */
void status_read(struct status *s);
void status_battery(struct status *s);   /* just the battery */

/* panel.c */
void panels_init(GtkApplication *app);
void panels_volume(const char *what);        /* up, down or mute */
void panels_brightness(const char *what);    /* up or down */
void panels_lock(void);                      /* start the lock screen */
gboolean lock_possible(void);                /* the user has a password */

/* launcher.c */
void launcher_toggle(GtkApplication *app);

/* notify.c */
void notify_init(GtkApplication *app);

/* wifi.c: Wi-Fi through iwd's D-Bus interface. */
struct wifi_network {
	char *path, *name, *type;   /* type: open, psk, 8021x, wep */
	int bars;                   /* signal, 1 to 4 */
	gboolean connected, known;
};
typedef void (*wifi_list_fn)(GPtrArray *networks, gpointer data);  /* takes ownership */
typedef void (*wifi_done_fn)(const char *error, gpointer data);    /* NULL on success */
gboolean wifi_init(void);             /* TRUE if there is a Wi-Fi adapter */
gboolean wifi_powered(void);
void wifi_set_powered(gboolean on);
void wifi_scan(wifi_list_fn done, gpointer data);
void wifi_connect(const char *network, const char *passphrase, wifi_done_fn done, gpointer data);
void wifi_disconnect(void);
char *wifi_connected_name(void);
void wifi_network_free(gpointer p);

/* volume.c */
/* brightness.c: the screen's backlight. */
gboolean brightness_available(void);
double brightness_get(void);                 /* 0 to 1, or -1 */
void brightness_set(double level);
void brightness_step(int direction);         /* up (1) or down (-1) by a tenth */

typedef void (*volume_changed_fn)(void);
void volume_watch(volume_changed_fn fn);
void volume_refresh(gboolean details);       /* details: also the output's name */
const struct volume *volume_get(void);
void volume_set(double level);
void volume_step(int direction);             /* up (1) or down (-1) by 5% */
void volume_set_muted(gboolean muted);
void volume_toggle_mute(void);

/* power.c */
void power_logout(void);
gboolean power_request(const char *what);   /* "off" or "reboot" */

/* config.c: ~/.config/wormos/shell, watched */
typedef void (*config_changed_fn)(void);
gboolean config_bool(const char *group, const char *key, gboolean fallback);
char *config_string(const char *group, const char *key);   /* g_free; NULL if unset */
void config_set_bool(const char *group, const char *key, gboolean value);
void config_watch(config_changed_fn fn);
void outputs_reload(void);                   /* the display file changed */
gboolean notify_quiet(void);                 /* do not disturb */

/* util */
char *relative_time(gint64 unix_time);       /* "5 minutes ago"; g_free */
GIcon *app_icon(const char *app_id);         /* for a window's app_id */

#endif
