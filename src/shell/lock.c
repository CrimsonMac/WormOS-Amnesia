/*
 * wormos-lock - the lock screen.
 *
 * Locks the session through the Wayland session-lock protocol
 * (ext-session-lock-v1, via gtk4-session-lock): the compositor shows only
 * this program's windows, one per screen, and gives input to nothing else;
 * if the program dies, the session stays locked (labwc then shows a plain
 * colour). Each window shows the time, the user and a password field; the
 * password is checked by /usr/lib/worm/checkpass (setuid, the caller's own
 * password only, slow on failure).
 *
 * While locked, $XDG_RUNTIME_DIR/wormos-locked exists: powerkey waits for
 * it (and this program running) before putting a laptop to sleep, and
 * worm-shell starts the lock screen again if it finds the mark without it
 * (labwc keeps the screens locked when a lock client dies, and lets a new
 * one take over). The program is not dumpable: other programs of the same
 * user cannot attach to it. A user without a password is never
 * locked (the screen could not be opened again): the program exits.
 * Started by Super+L, the panel's Lock button and worm-shell on SIGUSR1
 * (from powerkey when the lid stays closed). One instance at a time.
 */

#include <gtk/gtk.h>
#if __has_include(<gtk4-session-lock.h>)
#include <gtk4-session-lock.h>
#else
/* Some distributions' gtk4-layer-shell (Debian's, used by CI) leave the
 * session lock out: build, but never lock. */
#define NO_SESSION_LOCK 1
#endif
#include <pwd.h>
#include <sys/prctl.h>
#include <string.h>
#include <unistd.h>

#define CHECKPASS "/usr/lib/worm/checkpass"

#ifdef NO_SESSION_LOCK
int main(void)
{
	g_printerr("wormos-lock: built without gtk4-session-lock\n");
	return 1;
}
#else

static GtkApplication *app;
static GtkSessionLockInstance *lock;
static GPtrArray *clocks;               /* the time labels, one per screen */
static char *mark;                      /* $XDG_RUNTIME_DIR/wormos-locked */
static gboolean checking;

static const char css[] =
	"window.lock { background: #0b0a12; color: #e9e6f5; }"
	".lock-time { font-size: 64pt; font-weight: 300; }"
	".lock-date { font-size: 15pt; color: #b7b0d0; }"
	".lock-user { font-size: 13pt; font-weight: 600; margin-top: 24px; }"
	".lock-status { color: #f0a0b0; min-height: 1.4em; }"
	"window.lock entry { min-width: 280px; }";

static gboolean tick(gpointer data)
{
	GDateTime *t = g_date_time_new_now_local();
	char *time_text = g_date_time_format(t, "%H:%M");
	char *date_text = g_date_time_format(t, "%A, %e %B");

	(void)data;
	for (guint i = 0; i < clocks->len; i++) {
		GtkWidget *time_label = g_ptr_array_index(clocks, i);

		gtk_label_set_text(GTK_LABEL(time_label), time_text);
		gtk_label_set_text(GTK_LABEL(g_object_get_data(G_OBJECT(time_label), "date")),
				   g_strstrip(date_text));
	}
	g_free(time_text);
	g_free(date_text);
	g_date_time_unref(t);
	return G_SOURCE_CONTINUE;
}

struct attempt {
	GtkWidget *entry, *status;
};

static void retry(struct attempt *a, const char *message)
{
	gtk_label_set_text(GTK_LABEL(a->status), message);
	gtk_editable_set_text(GTK_EDITABLE(a->entry), "");
	gtk_widget_set_sensitive(a->entry, TRUE);
	gtk_widget_grab_focus(a->entry);
}

static void checked(GObject *source, GAsyncResult *res, gpointer data)
{
	GSubprocess *proc = G_SUBPROCESS(source);
	struct attempt *a = data;
	char *out = NULL;
	int status = -1;

	checking = FALSE;
	if (g_subprocess_communicate_utf8_finish(proc, res, &out, NULL, NULL) &&
	    g_subprocess_get_if_exited(proc))
		status = g_subprocess_get_exit_status(proc);
	if (status == 0) {
		gtk_session_lock_instance_unlock(lock);
	} else if (status == 3) {
		/* Too many wrong passwords: checkpass says how long to wait. */
		char *text = g_strdup_printf("Too many wrong passwords. Wait %s seconds, then try "
					     "again.", out && *g_strstrip(out) ? out : "a few");

		retry(a, text);
		g_free(text);
	} else {
		retry(a, "Wrong password. Try again.");
	}
	g_free(out);
	g_object_unref(proc);
	g_free(a);
}

static void entered(GtkEntry *entry, gpointer data)
{
	const char *password = gtk_editable_get_text(GTK_EDITABLE(entry));
	GError *error = NULL;
	GSubprocess *proc;
	struct attempt *a;
	char *line;

	if (checking || !*password)
		return;
	proc = g_subprocess_new(G_SUBPROCESS_FLAGS_STDIN_PIPE | G_SUBPROCESS_FLAGS_STDOUT_PIPE,
				&error, CHECKPASS, NULL);
	if (!proc) {
		gtk_label_set_text(GTK_LABEL(data), "Cannot check the password.");
		g_clear_error(&error);
		return;
	}
	checking = TRUE;
	a = g_new0(struct attempt, 1);
	a->entry = GTK_WIDGET(entry);
	a->status = data;
	gtk_widget_set_sensitive(a->entry, FALSE);
	gtk_label_set_text(GTK_LABEL(a->status), "Checking...");
	/* The password goes in, the answer comes out, without blocking. */
	line = g_strconcat(password, "\n", NULL);
	g_subprocess_communicate_utf8_async(proc, line, NULL, checked, a);
	explicit_bzero(line, strlen(line));
	g_free(line);
}

static GtkWidget *label_new(const char *text, const char *class)
{
	GtkWidget *l = gtk_label_new(text);

	gtk_widget_add_css_class(l, class);
	return l;
}

static void monitor_added(GtkSessionLockInstance *instance, GdkMonitor *monitor, gpointer data)
{
	GtkWidget *window = gtk_application_window_new(app);
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
	GtkWidget *time_label = label_new("", "lock-time"), *date_label = label_new("", "lock-date");
	GtkWidget *entry = gtk_password_entry_new(), *status = label_new("", "lock-status");
	const char *real = g_get_real_name();

	(void)data;
	gtk_widget_add_css_class(window, "lock");
	gtk_widget_set_halign(box, GTK_ALIGN_CENTER);
	gtk_widget_set_valign(box, GTK_ALIGN_CENTER);
	g_object_set_data(G_OBJECT(time_label), "date", date_label);
	g_ptr_array_add(clocks, time_label);
	gtk_box_append(GTK_BOX(box), time_label);
	gtk_box_append(GTK_BOX(box), date_label);
	gtk_box_append(GTK_BOX(box), label_new(real && strcmp(real, "Unknown") ? real
					       : g_get_user_name(), "lock-user"));
	gtk_password_entry_set_show_peek_icon(GTK_PASSWORD_ENTRY(entry), TRUE);
	g_object_set(entry, "placeholder-text", "Password", "activates-default", FALSE, NULL);
	g_signal_connect(entry, "activate", G_CALLBACK(entered), status);
	gtk_box_append(GTK_BOX(box), entry);
	gtk_box_append(GTK_BOX(box), status);
	gtk_window_set_child(GTK_WINDOW(window), box);
	gtk_session_lock_instance_assign_window_to_monitor(instance, GTK_WINDOW(window), monitor);
	gtk_window_present(GTK_WINDOW(window));
	gtk_widget_grab_focus(entry);
	tick(NULL);
}

static void locked(GtkSessionLockInstance *instance, gpointer data)
{
	(void)instance;
	(void)data;
	if (mark)
		g_file_set_contents(mark, "", 0, NULL);
}

static void ended(GtkSessionLockInstance *instance, gpointer data)
{
	(void)instance;
	(void)data;
	if (mark)
		unlink(mark);
	g_application_release(G_APPLICATION(app));
	g_application_quit(G_APPLICATION(app));
}

/* Has the user a password the lock screen could take? */
static gboolean has_password(void)
{
	int status = -1;

	return g_spawn_sync(NULL, (char *[]){ CHECKPASS, "--status", NULL }, NULL,
			    G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL, NULL, NULL,
			    NULL, NULL, &status, NULL) && g_spawn_check_wait_status(status, NULL);
}

static void activate(GApplication *gapp, gpointer data)
{
	GtkCssProvider *provider;

	(void)data;
	if (lock)
		return;         /* already locked: a second start does nothing */
	if (!has_password() || !gtk_session_lock_is_supported())
		return;
	provider = gtk_css_provider_new();
	gtk_css_provider_load_from_string(provider, css);
	gtk_style_context_add_provider_for_display(gdk_display_get_default(),
						   GTK_STYLE_PROVIDER(provider),
						   GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
	g_application_hold(gapp);
	clocks = g_ptr_array_new();
	if (g_get_user_runtime_dir())
		mark = g_build_filename(g_get_user_runtime_dir(), "wormos-locked", NULL);
	lock = gtk_session_lock_instance_new();
	g_signal_connect(lock, "monitor", G_CALLBACK(monitor_added), NULL);
	g_signal_connect(lock, "locked", G_CALLBACK(locked), NULL);
	g_signal_connect(lock, "failed", G_CALLBACK(ended), NULL);
	g_signal_connect(lock, "unlocked", G_CALLBACK(ended), NULL);
	g_timeout_add_seconds(1, tick, NULL);
	gtk_session_lock_instance_lock(lock);
}

int main(int argc, char **argv)
{
	int status;

	/* No other program of the user may attach to this one or read its
	 * memory (the password being typed). */
	prctl(PR_SET_DUMPABLE, 0, 0, 0, 0);
	app = gtk_application_new("org.wormos.Lock", G_APPLICATION_DEFAULT_FLAGS);
	g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);
	status = g_application_run(G_APPLICATION(app), argc, argv);
	g_object_unref(app);
	return status;
}
#endif
