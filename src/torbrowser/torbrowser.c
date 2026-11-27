/*
 * tor-browser - start Tor Browser, setting it up on the first run.
 *
 * The image ships Tor Browser in /opt/tor-browser (the package
 * tor-browser-bundle). The first start copies it into
 * ~/.local/share/tor-browser, where the browser keeps its profile and
 * updates itself; on btrfs the copy shares the data (copy_file_range).
 * Without the bundle, the first run installs the system libraries it needs
 * (the core package tor-browser-runtime, through the setuid helper
 * /usr/lib/worm/install with the administrator's own password; the system restarts), then
 * downloads the current release from torproject.org, checks its signature
 * against the Tor Browser Developers' key shipped with this package, and
 * unpacks it. After that it only starts the browser.
 *
 * The browser never runs as root. Started by root (su - in the desktop's
 * terminal), it starts again as the user who owns the terminal, with the
 * environment of that user's desktop session.
 *
 * Steps of the download:
 *   1. The release version from Tor Browser's download list; the archive
 *      and signature URLs are built here from it, never taken from the list.
 *      A version older than MIN_VERSION or than the installed one is refused.
 *   2. Archive and signature (curl, HTTPS only).
 *   3. gpgv must report a valid signature by a key whose primary
 *      fingerprint is TOR_BROWSER_KEY.
 *   4. tar unpacks into a fresh directory, which then replaces the old one.
 *
 * The browser runs as in Tor Browser's own start script: its home folder is
 * the Browser directory, ~/.local/share there points to /dev/null, and
 * GSettings keeps nothing. Firefox uses Wayland only with WAYLAND_DISPLAY
 * set. Our GTK 3 has only the Wayland backend, so the
 * X11 functions Tor Browser links against come from a library of inert
 * stand-ins (LD_PRELOAD). In anonymous mode (worm anon on) the system's
 * Tor carries everything, so the browser uses it instead of starting its
 * own (which the firewall would block).
 *
 *     tor-browser [URL...]        start it (setting it up first if needed)
 *     tor-browser --download      download and check it, without a window
 *     tor-browser --check FILE SIG   only check a signature (for tests)
 */

#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <gtk/gtk.h>

#define APP_ID           "org.wormos.TorBrowser"
#define CSS              "/usr/share/wormos/tor-browser/tor-browser.css"
#define KEYRING          "/usr/share/wormos/tor-browser/tor-browser-developers.gpg"
#define TOR_BROWSER_KEY  "EF6E286DDA85EA2A4BA7DE684E2C6E8793298290"
#define MIN_VERSION      "16.0.1"
#define DOWNLOADS_JSON   "https://aus1.torproject.org/torbrowser/update_3/release/download-linux-x86_64.json"
#define DIST             "https://dist.torproject.org/torbrowser"
#define COMPAT           "/usr/lib/tor-browser/libx11-compat.so"
#define RUNTIME          "tor-browser-runtime"
#define INSTALL_HELPER   "/usr/lib/worm/install"
#define ANON_SWITCH      "/etc/worm/anon"
#define SYSTEM_TOR_SOCKS "9050"
#define BUNDLE           "/opt/tor-browser"

static char *top;          /* ~/.local/share/tor-browser */
static GtkWidget *window, *stack, *status, *progress, *detail, *retry;
static GtkWidget *password, *install_error, *install_button;

/* ---- small helpers ---- */

static char *path(const char *name)
{
	return g_build_filename(top, name, NULL);
}

static gboolean browser_installed(void)
{
	char *p = g_build_filename(top, "tor-browser", "Browser", "firefox", NULL);
	gboolean ok = access(p, X_OK) == 0;

	g_free(p);
	return ok;
}

/* -1, 0 or 1 for dotted versions such as 15.0.26. */
static int version_cmp(const char *a, const char *b)
{
	while (*a || *b) {
		unsigned long x = strtoul(a, (char **)&a, 10), y = strtoul(b, (char **)&b, 10);

		if (x != y)
			return x < y ? -1 : 1;
		if (*a == '.')
			a++;
		if (*b == '.')
			b++;
	}
	return 0;
}

static gboolean valid_version(const char *v)
{
	size_t n = strlen(v);

	return n > 0 && n < 32 && strspn(v, "0123456789.") == n && g_ascii_isdigit(v[0]) &&
	       g_ascii_isdigit(v[n - 1]) && !strstr(v, "..");
}

/* The "version" field of the download list. */
static char *json_version(const char *json)
{
	const char *p = strstr(json, "\"version\"");
	char *v;

	if (!p || !(p = strchr(p + 9, '"')))
		return NULL;
	v = g_strndup(p + 1, strcspn(p + 1, "\""));
	if (!valid_version(v)) {
		g_free(v);
		return NULL;
	}
	return v;
}

/* Does gpgv's status output report a good signature by Tor Browser's key? */
static gboolean signature_ok(const char *file, const char *sig, char **why)
{
	const char *argv[] = { "gpgv", "--status-fd", "1", "--keyring", KEYRING, sig, file, NULL };
	char *out = NULL, *err = NULL;
	gboolean ok = FALSE;
	int wait = 0;

	if (!g_spawn_sync(NULL, (char **)argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL,
			  &out, &err, &wait, NULL)) {
		*why = g_strdup("gpgv could not be run.");
		return FALSE;
	}
	if (g_spawn_check_wait_status(wait, NULL)) {
		char **lines = g_strsplit(out, "\n", -1);

		/* VALIDSIG <fingerprint> ... <primary key fingerprint> */
		for (char **l = lines; *l; l++) {
			if (g_str_has_prefix(*l, "[GNUPG:] VALIDSIG ")) {
				char **f = g_strsplit(*l, " ", -1);
				guint n = g_strv_length(f);

				ok = n >= 3 && !strcmp(f[n - 1], TOR_BROWSER_KEY);
				g_strfreev(f);
			}
		}
		g_strfreev(lines);
	}
	if (!ok)
		*why = g_strdup("The signature does not match Tor Browser's signing key. "
				"The download was deleted.");
	g_free(out);
	g_free(err);
	return ok;
}

/* ---- starting the browser ---- */

/* The browser gets a session of its own: closing the terminal it was
 * started from does not close it. */
static void detach(gpointer data)
{
	(void)data;
	setsid();
}

static void start_browser(char **urls)
{
	char *browser = g_build_filename(top, "tor-browser", "Browser", NULL);
	char *local = g_build_filename(browser, ".local", NULL);
	char *share = g_build_filename(local, "share", NULL);
	GPtrArray *argv = g_ptr_array_new();
	char **env = g_get_environ();
	GError *error = NULL;
	struct stat st;

	/* Nothing is left in ~/.local/share of the browser's home folder. */
	if (lstat(share, &st) < 0 || !S_ISLNK(st.st_mode)) {
		char *rm[] = { "rm", "-rf", share, NULL };

		g_mkdir_with_parents(local, 0700);
		g_spawn_sync(NULL, rm, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL, NULL, NULL);
		if (symlink("/dev/null", share) < 0)
			g_warning("cannot link %s: %s", share, g_strerror(errno));
	}

	env = g_environ_setenv(env, "BB_ORIGINAL_HOME", g_get_home_dir(), TRUE);
	env = g_environ_setenv(env, "HOME", browser, TRUE);
	env = g_environ_setenv(env, "GSETTINGS_BACKEND", "memory", TRUE);
	env = g_environ_setenv(env, "__GL_SHADER_DISK_CACHE", "0", TRUE);
	env = g_environ_setenv(env, "MOZ_ENABLE_WAYLAND", "1", TRUE);
	env = g_environ_setenv(env, "GDK_BACKEND", "wayland", TRUE);
	env = g_environ_setenv(env, "LD_PRELOAD", COMPAT, TRUE);
	env = g_environ_unsetenv(env, "DISPLAY");
	/* Firefox picks Wayland only when this is set; wayland-0 is the
	 * default that GTK 4 and libwayland use too. */
	if (!g_environ_getenv(env, "WAYLAND_DISPLAY"))
		env = g_environ_setenv(env, "WAYLAND_DISPLAY", "wayland-0", TRUE);
	if (access(ANON_SWITCH, F_OK) == 0) {
		env = g_environ_setenv(env, "TOR_PROVIDER", "none", TRUE);
		env = g_environ_setenv(env, "TOR_SKIP_LAUNCH", "1", TRUE);
		env = g_environ_setenv(env, "TOR_SOCKS_HOST", "127.0.0.1", TRUE);
		env = g_environ_setenv(env, "TOR_SOCKS_PORT", SYSTEM_TOR_SOCKS, TRUE);
	}

	g_ptr_array_add(argv, "./firefox");
	for (char **u = urls; u && *u; u++)
		g_ptr_array_add(argv, *u);
	g_ptr_array_add(argv, NULL);
	/* Like the start script: its output goes nowhere. */
	if (!g_spawn_async(browser, (char **)argv->pdata, env,
			   G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL |
			   G_SPAWN_STDIN_FROM_DEV_NULL, detach, NULL, NULL, &error)) {
		g_printerr("tor-browser: cannot start Tor Browser: %s\n", error->message);
		g_error_free(error);
	}
	g_ptr_array_free(argv, TRUE);
	g_strfreev(env);
	g_free(share);
	g_free(local);
	g_free(browser);
}

/* ---- the download ---- */

struct download {
	char *version;
	char *archive, *signature;      /* local paths */
	char *message;                  /* why it failed, or NULL */
	goffset total;                  /* archive size, 0 if unknown */
	guint timer;                    /* progress updates while downloading */
};

static void download_free(struct download *d)
{
	g_free(d->version);
	g_free(d->archive);
	g_free(d->signature);
	g_free(d->message);
	g_free(d);
}

static gboolean run(const char *const *argv, char **out, char **why)
{
	char *err = NULL;
	int wait = 0;
	gboolean ok;

	ok = g_spawn_sync(NULL, (char **)argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL,
			  out, &err, &wait, NULL) && g_spawn_check_wait_status(wait, NULL);
	if (!ok && why)
		*why = g_strdup_printf("%s failed%s%s", argv[0], err && *err ? ": " : ".",
				       err ? g_strstrip(err) : "");
	g_free(err);
	return ok;
}

/* Steps 1 and the signature download: quick, run on a thread. */
static gboolean find_release(struct download *d)
{
	const char *list[] = { "curl", "-fsS", "--proto", "=https", "--tlsv1.2",
			       "--max-time", "60", DOWNLOADS_JSON, NULL };
	char *json = NULL, *installed = NULL, *vfile = path("version"), *name, *url, *head = NULL;
	gboolean ok = FALSE;

	if (!run(list, &json, &d->message))
		goto out;
	if (!(d->version = json_version(json))) {
		d->message = g_strdup("The download list has no valid version.");
		goto out;
	}
	if (version_cmp(d->version, MIN_VERSION) < 0 ||
	    (g_file_get_contents(vfile, &installed, NULL, NULL) &&
	     version_cmp(d->version, g_strstrip(installed)) < 0)) {
		d->message = g_strdup_printf("The download list offers version %s, older than "
					     "the one expected. Nothing was changed.", d->version);
		goto out;
	}
	name = g_strdup_printf("tor-browser-linux-x86_64-%s.tar.xz", d->version);
	d->archive = g_build_filename(top, "download", name, NULL);
	d->signature = g_strconcat(d->archive, ".asc", NULL);
	url = g_strdup_printf("%s/%s/%s.asc", DIST, d->version, name);
	{
		const char *sig[] = { "curl", "-fsS", "--proto", "=https", "--tlsv1.2",
				      "--max-time", "60", "-o", d->signature, url, NULL };

		ok = run(sig, NULL, &d->message);
	}
	g_free(url);
	/* The archive's size, for the progress bar. */
	url = g_strdup_printf("%s/%s/%s", DIST, d->version, name);
	{
		const char *size[] = { "curl", "-fsSI", "--proto", "=https", "--max-time", "30",
				       url, NULL };

		if (ok && run(size, &head, NULL)) {
			char *cl = strcasestr(head, "\ncontent-length:");

			if (cl)
				d->total = g_ascii_strtoll(cl + 16, NULL, 10);
		}
	}
	g_free(head);
	g_free(url);
	g_free(name);
out:
	g_free(json);
	g_free(installed);
	g_free(vfile);
	return ok;
}

/* Steps 3 and 4. */
static gboolean check_and_unpack(struct download *d)
{
	char *unpack = path("unpack"), *dest = path("tor-browser"), *old = path("tor-browser.old");
	char *inner = g_build_filename(unpack, "tor-browser", NULL), *vfile = path("version");
	const char *rm_unpack[] = { "rm", "-rf", unpack, NULL };
	const char *rm_old[] = { "rm", "-rf", old, NULL };
	const char *tar[] = { "tar", "-xJf", d->archive, "-C", unpack, NULL };
	gboolean ok = FALSE;

	if (!signature_ok(d->archive, d->signature, &d->message)) {
		unlink(d->archive);
		unlink(d->signature);
		goto out;
	}
	run(rm_unpack, NULL, NULL);
	g_mkdir_with_parents(unpack, 0700);
	if (!run(tar, NULL, &d->message))
		goto out;
	if (access(inner, F_OK) != 0) {
		d->message = g_strdup("The archive does not hold a tor-browser folder.");
		goto out;
	}
	run(rm_old, NULL, NULL);
	if ((access(dest, F_OK) == 0 && rename(dest, old) < 0) || rename(inner, dest) < 0) {
		d->message = g_strdup_printf("Cannot move Tor Browser into place: %s",
					     g_strerror(errno));
		goto out;
	}
	ok = g_file_set_contents(vfile, d->version, -1, NULL);
	run(rm_old, NULL, NULL);
	unlink(d->archive);
	unlink(d->signature);
out:
	run(rm_unpack, NULL, NULL);
	g_free(unpack);
	g_free(dest);
	g_free(old);
	g_free(inner);
	g_free(vfile);
	return ok;
}

static void prepare_dirs(void)
{
	char *dl = path("download");

	g_mkdir_with_parents(dl, 0700);
	g_free(dl);
}

/* --download: the same steps, reported on standard output. */
static int download_now(void)
{
	struct download *d = g_new0(struct download, 1);
	int status = 1;

	prepare_dirs();
	printf("Looking for the current release...\n");
	if (find_release(d)) {
		const char *curl[] = { "curl", "-fsS", "--proto", "=https", "--tlsv1.2",
				       "-o", d->archive, NULL, NULL };
		char *url = g_strdup_printf("%s/%s/%s", DIST, d->version,
					    strrchr(d->archive, '/') + 1);

		curl[7] = url;
		printf("Downloading Tor Browser %s...\n", d->version);
		fflush(stdout);
		if (run(curl, NULL, &d->message)) {
			printf("Checking the signature and unpacking...\n");
			fflush(stdout);
			if (check_and_unpack(d)) {
				printf("Tor Browser %s is ready.\n", d->version);
				status = 0;
			}
		}
		g_free(url);
	}
	if (status)
		fprintf(stderr, "tor-browser: %s\n", d->message ? d->message : "failed");
	download_free(d);
	return status;
}

/* ---- the window ---- */

static void show_error(const char *message)
{
	gtk_label_set_text(GTK_LABEL(status), "Tor Browser could not be set up");
	gtk_label_set_text(GTK_LABEL(detail), message);
	gtk_widget_set_visible(progress, FALSE);
	gtk_widget_set_visible(retry, TRUE);
}

static void unpacked(GObject *source, GAsyncResult *res, gpointer data)
{
	struct download *d = data;

	(void)source;
	if (g_task_propagate_boolean(G_TASK(res), NULL)) {
		start_browser(NULL);
		gtk_window_destroy(GTK_WINDOW(window));
	} else {
		show_error(d->message ? d->message : "Unpacking failed.");
	}
	download_free(d);
}

static void unpack_thread(GTask *task, gpointer source, gpointer data, GCancellable *c)
{
	(void)source;
	(void)c;
	g_task_return_boolean(task, check_and_unpack(data));
}

static gboolean show_progress(gpointer data)
{
	struct download *d = data;
	struct stat st;
	goffset have = stat(d->archive, &st) == 0 ? st.st_size : 0;
	char *text;

	if (d->total > 0) {
		gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(progress),
					      MIN(1.0, (double)have / (double)d->total));
		text = g_strdup_printf("%.0f of %.0f MB", have / 1e6, d->total / 1e6);
	} else {
		gtk_progress_bar_pulse(GTK_PROGRESS_BAR(progress));
		text = g_strdup_printf("%.0f MB", have / 1e6);
	}
	gtk_label_set_text(GTK_LABEL(detail), text);
	g_free(text);
	return G_SOURCE_CONTINUE;
}

static void downloaded(GObject *source, GAsyncResult *res, gpointer data)
{
	struct download *d = data;
	GTask *task;

	g_source_remove(d->timer);
	if (!g_subprocess_wait_check_finish(G_SUBPROCESS(source), res, NULL)) {
		unlink(d->archive);
		show_error("The download failed. Check the network connection and try again.");
		download_free(d);
		return;
	}
	gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(progress), 1);
	gtk_label_set_text(GTK_LABEL(status), "Checking the signature and unpacking...");
	gtk_label_set_text(GTK_LABEL(detail), "");
	task = g_task_new(NULL, NULL, unpacked, d);
	g_task_set_task_data(task, d, NULL);
	g_task_run_in_thread(task, unpack_thread);
	g_object_unref(task);
}

static void found(GObject *source, GAsyncResult *res, gpointer data)
{
	struct download *d = data;
	GError *error = NULL;
	GSubprocess *curl;
	char *url, *text;

	(void)source;
	if (!g_task_propagate_boolean(G_TASK(res), NULL)) {
		show_error(d->message ? d->message : "No release found.");
		download_free(d);
		return;
	}
	url = g_strdup_printf("%s/%s/%s", DIST, d->version, strrchr(d->archive, '/') + 1);
	curl = g_subprocess_new(G_SUBPROCESS_FLAGS_STDERR_SILENCE, &error, "curl", "-fsS",
				"--proto", "=https", "--tlsv1.2", "-o", d->archive, url, NULL);
	g_free(url);
	if (!curl) {
		show_error(error->message);
		g_error_free(error);
		download_free(d);
		return;
	}
	text = g_strdup_printf("Downloading Tor Browser %s...", d->version);
	gtk_label_set_text(GTK_LABEL(status), text);
	g_free(text);
	d->timer = g_timeout_add(300, show_progress, d);
	g_subprocess_wait_check_async(curl, NULL, downloaded, d);
	g_object_unref(curl);
}

static void find_thread(GTask *task, gpointer source, gpointer data, GCancellable *c)
{
	(void)source;
	(void)c;
	g_task_return_boolean(task, find_release(data));
}

static void start_download(void)
{
	struct download *d = g_new0(struct download, 1);
	GTask *task = g_task_new(NULL, NULL, found, d);

	prepare_dirs();
	gtk_stack_set_visible_child_name(GTK_STACK(stack), "progress");
	gtk_label_set_text(GTK_LABEL(status), "Looking for the current release...");
	gtk_label_set_text(GTK_LABEL(detail), "");
	gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(progress), 0);
	gtk_widget_set_visible(progress, TRUE);
	gtk_widget_set_visible(retry, FALSE);
	g_task_set_task_data(task, d, NULL);
	g_task_run_in_thread(task, find_thread);
	g_object_unref(task);
}

static void download_clicked(GtkButton *button, gpointer data)
{
	(void)button;
	(void)data;
	start_download();
}

/* The runtime: installing it restarts the system. */
static void installed(GObject *source, GAsyncResult *res, gpointer data)
{
	GSubprocess *proc = G_SUBPROCESS(source);

	(void)data;
	if (!g_subprocess_wait_finish(proc, res, NULL) || !g_subprocess_get_if_exited(proc)) {
		gtk_label_set_text(GTK_LABEL(install_error), "The installation could not run.");
	} else if (g_subprocess_get_exit_status(proc) == 0) {
		gtk_label_set_text(GTK_LABEL(install_error),
				   "Installed. The desktop restarts now; open Tor Browser again "
				   "afterwards.");
		return;
	} else if (g_subprocess_get_exit_status(proc) == 3) {
		gtk_label_set_text(GTK_LABEL(install_error), "Wrong password.");
		gtk_editable_set_text(GTK_EDITABLE(password), "");
	} else {
		gtk_label_set_text(GTK_LABEL(install_error),
				   "The installation failed. Check the network connection.");
	}
	gtk_widget_set_sensitive(install_button, TRUE);
	gtk_widget_set_sensitive(password, TRUE);
}

static void install_clicked(GtkWidget *widget, gpointer data)
{
	GError *error = NULL;
	GSubprocess *proc;
	char *input;

	(void)widget;
	(void)data;
	proc = g_subprocess_new(G_SUBPROCESS_FLAGS_STDIN_PIPE | G_SUBPROCESS_FLAGS_STDOUT_SILENCE |
				G_SUBPROCESS_FLAGS_STDERR_SILENCE, &error, INSTALL_HELPER, RUNTIME,
				NULL);
	if (!proc) {
		gtk_label_set_text(GTK_LABEL(install_error), error->message);
		g_error_free(error);
		return;
	}
	gtk_widget_set_sensitive(install_button, FALSE);
	gtk_widget_set_sensitive(password, FALSE);
	gtk_label_set_text(GTK_LABEL(install_error), "Installing (this takes a few minutes)...");
	input = g_strconcat(gtk_editable_get_text(GTK_EDITABLE(password)), "\n", NULL);
	g_subprocess_communicate_utf8_async(proc, input, NULL, NULL, NULL);
	explicit_bzero(input, strlen(input));
	g_free(input);
	g_subprocess_wait_async(proc, NULL, installed, NULL);
	g_object_unref(proc);
}

static GtkWidget *page(const char *title, const char *text)
{
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12), *label;

	gtk_widget_add_css_class(box, "page");
	gtk_box_append(GTK_BOX(box), gtk_image_new_from_icon_name(APP_ID));
	gtk_image_set_pixel_size(GTK_IMAGE(gtk_widget_get_first_child(box)), 96);
	label = gtk_label_new(title);
	gtk_widget_add_css_class(label, "title-1");
	gtk_box_append(GTK_BOX(box), label);
	label = gtk_label_new(text);
	gtk_label_set_wrap(GTK_LABEL(label), TRUE);
	gtk_label_set_max_width_chars(GTK_LABEL(label), 52);
	gtk_label_set_justify(GTK_LABEL(label), GTK_JUSTIFY_CENTER);
	gtk_box_append(GTK_BOX(box), label);
	return box;
}

static gboolean in_wheel(void)
{
	char *out = NULL;
	const char *id[] = { "id", "-Gn", NULL };
	gboolean yes = FALSE;

	if (getuid() == 0)
		return TRUE;
	if (run(id, &out, NULL)) {
		char **groups = g_strsplit(g_strstrip(out), " ", -1);

		yes = g_strv_contains((const char *const *)groups, "wheel");
		g_strfreev(groups);
	}
	g_free(out);
	return yes;
}

static void activate(GtkApplication *app, gpointer data)
{
	GtkWidget *box, *button, *runtime, *download, *working;
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

	window = gtk_application_window_new(app);
	gtk_window_set_title(GTK_WINDOW(window), "Tor Browser");
	gtk_window_set_default_size(GTK_WINDOW(window), 560, 460);
	stack = gtk_stack_new();
	gtk_stack_set_transition_type(GTK_STACK(stack), GTK_STACK_TRANSITION_TYPE_CROSSFADE);

	/* The system libraries. */
	if (in_wheel()) {
		runtime = page("Set up Tor Browser",
			       "Tor Browser needs a few system libraries first (GTK 3 and "
			       "the X11 client libraries, about 10 MB). Installing them "
			       "restarts the desktop; open Tor Browser again afterwards.");
		password = gtk_password_entry_new();
		gtk_password_entry_set_show_peek_icon(GTK_PASSWORD_ENTRY(password), TRUE);
		gtk_editable_set_width_chars(GTK_EDITABLE(password), 28);
		gtk_widget_set_halign(password, GTK_ALIGN_CENTER);
		g_object_set(password, "placeholder-text", "Your password", NULL);
		g_signal_connect(password, "activate", G_CALLBACK(install_clicked), NULL);
		gtk_box_append(GTK_BOX(runtime), password);
		install_error = gtk_label_new("");
		gtk_widget_add_css_class(install_error, "dim-label");
		gtk_box_append(GTK_BOX(runtime), install_error);
		install_button = gtk_button_new_with_label("Install");
		gtk_widget_add_css_class(install_button, "suggested-action");
		gtk_widget_set_halign(install_button, GTK_ALIGN_CENTER);
		g_signal_connect(install_button, "clicked", G_CALLBACK(install_clicked), NULL);
		gtk_box_append(GTK_BOX(runtime), install_button);
	} else {
		runtime = page("Set up Tor Browser",
			       "Tor Browser needs a few system libraries first. Ask an "
			       "administrator to install them:\n\nwh install " RUNTIME);
	}
	gtk_stack_add_named(GTK_STACK(stack), runtime, "runtime");

	/* The browser itself. */
	download = page("Download Tor Browser",
			"Tor Browser comes from torproject.org (about 140 MB). Its signature "
			"is checked against the Tor Browser Developers' key before it is "
			"unpacked into your home folder. It updates itself afterwards.");
	button = gtk_button_new_with_label("Download");
	gtk_widget_add_css_class(button, "suggested-action");
	gtk_widget_set_halign(button, GTK_ALIGN_CENTER);
	g_signal_connect(button, "clicked", G_CALLBACK(download_clicked), NULL);
	gtk_box_append(GTK_BOX(download), button);
	gtk_stack_add_named(GTK_STACK(stack), download, "download");

	/* Progress and errors. */
	working = page("Tor Browser", "");
	box = gtk_widget_get_last_child(working);
	status = box;
	gtk_widget_add_css_class(status, "heading");
	progress = gtk_progress_bar_new();
	gtk_widget_set_size_request(progress, 320, -1);
	gtk_widget_set_halign(progress, GTK_ALIGN_CENTER);
	gtk_box_append(GTK_BOX(working), progress);
	detail = gtk_label_new("");
	gtk_label_set_wrap(GTK_LABEL(detail), TRUE);
	gtk_label_set_max_width_chars(GTK_LABEL(detail), 52);
	gtk_widget_add_css_class(detail, "dim-label");
	gtk_box_append(GTK_BOX(working), detail);
	retry = gtk_button_new_with_label("Try again");
	gtk_widget_set_halign(retry, GTK_ALIGN_CENTER);
	gtk_widget_set_visible(retry, FALSE);
	g_signal_connect(retry, "clicked", G_CALLBACK(download_clicked), NULL);
	gtk_box_append(GTK_BOX(working), retry);
	gtk_stack_add_named(GTK_STACK(stack), working, "progress");

	gtk_stack_set_visible_child_name(GTK_STACK(stack),
					 access(COMPAT, F_OK) == 0 ? "download" : "runtime");
	gtk_window_set_child(GTK_WINDOW(window), stack);
	gtk_window_present(GTK_WINDOW(window));
}

/* ---- the copy shipped with the system ---- */

static gboolean copy_file(const char *from, const char *to, mode_t mode)
{
	int in = open(from, O_RDONLY | O_CLOEXEC), out;
	gboolean ok = FALSE;
	struct stat st;

	if (in < 0)
		return FALSE;
	out = open(to, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, mode & 07777);
	if (out >= 0 && fstat(in, &st) == 0) {
		off_t left = st.st_size;
		ssize_t n = 0;

		/* Shares the extents on btrfs; a plain copy elsewhere. */
		while (left > 0 && (n = copy_file_range(in, NULL, out, NULL, left, 0)) > 0)
			left -= n;
		if (left > 0 && n < 0) {
			char buf[65536];

			while ((n = read(in, buf, sizeof buf)) > 0)
				if (write(out, buf, n) != n)
					break;
			ok = n == 0;
		} else {
			ok = left == 0;
		}
		ok = ok && fchmod(out, mode & 07777) == 0;
	}
	if (out >= 0 && close(out) < 0)
		ok = FALSE;
	close(in);
	return ok;
}

static gboolean copy_tree(const char *from, const char *to)
{
	DIR *dir = opendir(from);
	struct dirent *e;
	gboolean ok = TRUE;
	struct stat st;

	if (!dir || stat(from, &st) < 0 || mkdir(to, 0700) < 0) {
		if (dir)
			closedir(dir);
		return FALSE;
	}
	while (ok && (e = readdir(dir))) {
		char *a, *b;

		if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
			continue;
		a = g_build_filename(from, e->d_name, NULL);
		b = g_build_filename(to, e->d_name, NULL);
		if (lstat(a, &st) < 0) {
			ok = FALSE;
		} else if (S_ISDIR(st.st_mode)) {
			ok = copy_tree(a, b);
		} else if (S_ISLNK(st.st_mode)) {
			char *target = g_file_read_link(a, NULL);

			ok = target && symlink(target, b) == 0;
			g_free(target);
		} else if (S_ISREG(st.st_mode)) {
			ok = copy_file(a, b, st.st_mode | S_IWUSR);
		}
		g_free(a);
		g_free(b);
	}
	closedir(dir);
	/* The user's copy must be writable: the browser updates itself. */
	return ok && stat(from, &st) == 0 && chmod(to, (st.st_mode & 07777) | S_IWUSR) == 0;
}

/* Copy /opt/tor-browser into the user's folder. */
static gboolean install_bundle(void)
{
	char *dest = path("tor-browser"), *tmp = path("tor-browser.new");
	char *vfile = path("version"), *version = NULL;
	const char *rm_tmp[] = { "rm", "-rf", tmp, NULL };
	gboolean ok;

	g_mkdir_with_parents(top, 0700);
	run(rm_tmp, NULL, NULL);
	fprintf(stderr, "tor-browser: setting up Tor Browser in %s\n", top);
	ok = copy_tree(BUNDLE, tmp) && rename(tmp, dest) == 0;
	if (!ok) {
		fprintf(stderr, "tor-browser: cannot copy %s: %s\n", BUNDLE, g_strerror(errno));
		run(rm_tmp, NULL, NULL);
	} else if (g_file_get_contents(BUNDLE "/version", &version, NULL, NULL)) {
		g_file_set_contents(vfile, g_strstrip(version), -1, NULL);
	}
	g_free(version);
	g_free(vfile);
	g_free(tmp);
	g_free(dest);
	return ok;
}

/* ---- root and the desktop session ---- */

/* The environment of a process of UID inside the desktop session (one that
 * has WAYLAND_DISPLAY), or NULL. */
static char **session_environ(uid_t uid)
{
	DIR *proc = opendir("/proc");
	struct dirent *e;
	char **env = NULL;

	while (proc && !env && (e = readdir(proc))) {
		char *file, *data = NULL;
		struct stat st;
		gsize len;

		if (!g_ascii_isdigit(e->d_name[0]))
			continue;
		file = g_strdup_printf("/proc/%s/environ", e->d_name);
		if (stat(file, &st) == 0 && st.st_uid == uid &&
		    g_file_get_contents(file, &data, &len, NULL) && len > 0) {
			GPtrArray *vars = g_ptr_array_new();
			gboolean wayland = FALSE;

			for (gsize i = 0; i < len; i += strlen(data + i) + 1) {
				wayland |= g_str_has_prefix(data + i, "WAYLAND_DISPLAY=");
				g_ptr_array_add(vars, g_strdup(data + i));
			}
			g_ptr_array_add(vars, NULL);
			env = (char **)g_ptr_array_free(vars, FALSE);
			if (!wayland) {
				g_strfreev(env);
				env = NULL;
			}
		}
		g_free(data);
		g_free(file);
	}
	if (proc)
		closedir(proc);
	return env;
}

/* Started as root: start again as the user of this desktop session. */
static void leave_root(char **argv)
{
	const char *tty = ttyname(STDIN_FILENO);
	struct passwd *pw = NULL;
	char **env = NULL;
	struct stat st;

	/* su - keeps the terminal, which belongs to the user who opened it. */
	if (tty && stat(tty, &st) == 0 && st.st_uid != 0)
		pw = getpwuid(st.st_uid);
	if (pw)
		env = session_environ(pw->pw_uid);
	if (!env) {
		fprintf(stderr, "tor-browser: Tor Browser does not run as root. Start it as "
				"your own user from the desktop (the launcher or a terminal "
				"without su).\n");
		exit(1);
	}
	if (initgroups(pw->pw_name, pw->pw_gid) < 0 || setgid(pw->pw_gid) < 0 ||
	    setuid(pw->pw_uid) < 0 || setuid(0) == 0) {
		fprintf(stderr, "tor-browser: cannot change to user %s\n", pw->pw_name);
		exit(1);
	}
	if (chdir(pw->pw_dir) < 0 && chdir("/") < 0)
		fprintf(stderr, "tor-browser: cannot change directory\n");
	fprintf(stderr, "tor-browser: Tor Browser does not run as root; starting it as %s.\n",
		pw->pw_name);
	execve("/usr/bin/tor-browser", argv, env);
	fprintf(stderr, "tor-browser: cannot start again: %s\n", g_strerror(errno));
	exit(1);
}

/* Is there a Wayland display to show a window on? */
static gboolean have_display(void)
{
	const char *name = g_getenv("WAYLAND_DISPLAY"), *dir = g_getenv("XDG_RUNTIME_DIR");
	char *sock;
	gboolean ok;

	if (name && *name)
		return TRUE;
	if (!dir)
		return FALSE;
	sock = g_build_filename(dir, "wayland-0", NULL);
	ok = access(sock, F_OK) == 0;
	g_free(sock);
	return ok;
}

int main(int argc, char **argv)
{
	GtkApplication *app;
	int status_code;

	top = g_build_filename(g_get_user_data_dir(), "tor-browser", NULL);

	if (argc == 4 && !strcmp(argv[1], "--check")) {
		char *why = NULL;

		if (signature_ok(argv[2], argv[3], &why)) {
			printf("good signature\n");
			return 0;
		}
		fprintf(stderr, "tor-browser: %s\n", why);
		return 1;
	}
	if (argc == 2 && !strcmp(argv[1], "--download")) {
		if (access(COMPAT, F_OK) != 0) {
			fprintf(stderr, "tor-browser: install %s first\n", RUNTIME);
			return 1;
		}
		return download_now();
	}
	if (argc > 1 && argv[1][0] == '-' && strcmp(argv[1], "--")) {
		fprintf(stderr, "usage: tor-browser [URL...] | --download | --check FILE SIG\n");
		return 2;
	}

	if (getuid() == 0 || geteuid() == 0)
		leave_root(argv);
	if (!have_display()) {
		fprintf(stderr, "tor-browser: no desktop session here (WAYLAND_DISPLAY is not "
				"set). Start Tor Browser from the desktop.\n");
		return 1;
	}

	/* The copy that comes with the system. */
	if (access(COMPAT, F_OK) == 0 && !browser_installed() &&
	    access(BUNDLE "/Browser/firefox", X_OK) == 0)
		install_bundle();

	/* Set up already: just start it. */
	if (access(COMPAT, F_OK) == 0 && browser_installed()) {
		start_browser(argv + 1 + (argc > 1 && !strcmp(argv[1], "--")));
		return 0;
	}

	app = gtk_application_new(APP_ID, G_APPLICATION_NON_UNIQUE);
	g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);
	status_code = g_application_run(G_APPLICATION(app), 1, argv);
	g_object_unref(app);
	return status_code;
}
