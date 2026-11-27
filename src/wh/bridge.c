/*
 * bridge - lets programs in wormholes reach the desktop.
 *
 * Runs in each desktop session, as its user (started by the session
 * script). Wormhole programs that open a link, a file or a terminal, or
 * that ask for root (sudo, pkexec), run stand-ins that wh puts in every
 * wormhole (src/wh/runtime.c). Those write a request into the FIFO
 * $XDG_RUNTIME_DIR/wormhole-bridge (mode 0600: only the user, and root,
 * can write): NUL-separated fields, ended by an empty field:
 *
 *     open WORMHOLE FOLDER TARGET     open a link or file (gio open)
 *     terminal WORMHOLE FOLDER [CMD]  a terminal running CMD in WORMHOLE
 *     root WORMHOLE FOLDER [CMD]      the root terminal running CMD as root
 *
 * Nothing runs with more rights than the user has: the root terminal
 * asks for the root password, as when it is opened from the menu.
 * Wormholes are not a security boundary (they share the user's home);
 * untrusted programs belong in a blackhole, which has no bridge.
 *
 * Usage: bridge (from the compositor's autostart, so that it has the
 * display for what it starts; it exits when the compositor goes)
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define MAX_REQUEST 65536
#define MAX_FIELDS  64

static char fifo[PATH_MAX];

static void quit(int sig)
{
	(void)sig;
	unlink(fifo);
	_exit(0);
}

/* Run argv detached from the bridge (a new session), in folder. */
static void spawn(char *const argv[], const char *folder)
{
	pid_t pid = fork();

	if (pid != 0)
		return;
	setsid();
	if (fork() != 0)
		_exit(0);
	if (!folder || chdir(folder) < 0) {
		const char *home = getenv("HOME");

		if ((!home || chdir(home) < 0) && chdir("/") < 0)
			_exit(127);
	}
	execvp(argv[0], argv);
	_exit(127);
}

static int valid_name(const char *s)
{
	return *s && strlen(s) < 32 &&
	       strspn(s, "abcdefghijklmnopqrstuvwxyz0123456789-") == strlen(s);
}

/* Append s to out, quoted for sh. */
static void sh_quote(char *out, size_t size, const char *s)
{
	size_t n = strlen(out);

	if (n + 3 >= size)
		return;
	out[n++] = '\'';
	for (; *s && n + 6 < size; s++) {
		if (*s == '\'') {
			memcpy(out + n, "'\\''", 4);
			n += 4;
		} else {
			out[n++] = *s;
		}
	}
	out[n++] = '\'';
	out[n] = '\0';
}

static void handle(char **f, int n)
{
	const char *verb = f[0], *wormhole = n > 1 ? f[1] : "", *folder = n > 2 ? f[2] : "/";

	if (n < 3 || !valid_name(wormhole) || folder[0] != '/')
		return;
	/* A line per request in the session's bridge.log. */
	fprintf(stderr, "bridge: %s %s %s", verb, wormhole, folder);
	for (int i = 3; i < n; i++)
		fprintf(stderr, " %s", f[i]);
	fputc('\n', stderr);
	if (!strcmp(verb, "open") && n == 4) {
		char path[PATH_MAX * 2];
		const char *target = f[3];

		/* A relative file name is relative to the program's folder. */
		if (!strstr(target, ":") && target[0] != '/') {
			snprintf(path, sizeof(path), "%s/%s", folder, target);
			target = path;
		}
		spawn((char *[]){ "gio", "open", (char *)target, NULL }, folder);
	} else if (!strcmp(verb, "terminal")) {
		char *argv[MAX_FIELDS + 8];
		int a = 0;

		argv[a++] = "foot";
		argv[a++] = "--";
		argv[a++] = "whc";
		argv[a++] = (char *)wormhole;
		if (n == 3) {
			argv[a++] = "sh";
			argv[a++] = "-c";
			argv[a++] = "command -v bash >/dev/null && exec bash -l; exec sh -l";
		}
		for (int i = 3; i < n; i++)
			argv[a++] = f[i];
		argv[a] = NULL;
		spawn(argv, folder);
	} else if (!strcmp(verb, "root")) {
		char cmd[8192];
		size_t len;

		len = (size_t)snprintf(cmd, sizeof(cmd), "whc %s%s", wormhole, n == 3 ?
			" sh -c 'command -v bash >/dev/null && exec bash -l; exec sh -l'" : "");
		if (len >= sizeof(cmd))
			return;
		for (int i = 3; i < n; i++) {
			/* Quoting at most quadruples it: no cut-off arguments. */
			if (len + 4 * strlen(f[i]) + 4 >= sizeof(cmd))
				return;
			cmd[len++] = ' ';
			cmd[len] = '\0';
			sh_quote(cmd, sizeof(cmd), f[i]);
			len = strlen(cmd);
		}
		spawn((char *[]){ "foot", "--title", "Root terminal", "/usr/lib/worm/root-terminal",
				  "--cd", (char *)folder, cmd, NULL }, folder);
	}
}

/*
 * A connection to the compositor's Wayland socket, never used: it hangs
 * up when the compositor exits, which ends the bridge. -1 without one.
 */
static int compositor(const char *run)
{
	const char *display = getenv("WAYLAND_DISPLAY");
	struct sockaddr_un addr = { .sun_family = AF_UNIX };
	int fd;

	if (!display || !*display)
		return -1;
	if (display[0] == '/')
		snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", display);
	else
		snprintf(addr.sun_path, sizeof(addr.sun_path), "%s/%s", run, display);
	if ((fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0)) < 0)
		return -1;
	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		close(fd);
		return -1;
	}
	return fd;
}

int main(void)
{
	static char buf[MAX_REQUEST];
	const char *run = getenv("XDG_RUNTIME_DIR");
	size_t have = 0;
	int fd, wayland;

	if (!run || run[0] != '/') {
		fprintf(stderr, "bridge: XDG_RUNTIME_DIR is not set\n");
		return 1;
	}
	snprintf(fifo, sizeof(fifo), "%s/wormhole-bridge", run);
	unlink(fifo);
	if (mkfifo(fifo, 0600) < 0) {
		fprintf(stderr, "bridge: cannot create %s: %s\n", fifo, strerror(errno));
		return 1;
	}
	signal(SIGTERM, quit);
	signal(SIGINT, quit);
	signal(SIGHUP, quit);
	signal(SIGCHLD, SIG_IGN);       /* children are detached anyway */
	/* Open for reading and writing: the FIFO never reaches end of file
	 * between requests. */
	if ((fd = open(fifo, O_RDWR | O_CLOEXEC)) < 0) {
		fprintf(stderr, "bridge: cannot open %s: %s\n", fifo, strerror(errno));
		unlink(fifo);
		return 1;
	}
	wayland = compositor(run);
	for (;;) {
		struct pollfd fds[2] = { { .fd = fd, .events = POLLIN },
					 { .fd = wayland, .events = 0 } };
		ssize_t r;
		size_t start = 0;

		if (poll(fds, wayland >= 0 ? 2 : 1, -1) < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		if (wayland >= 0 && (fds[1].revents & (POLLHUP | POLLERR)))
			break;          /* the compositor is gone */
		if (!(fds[0].revents & POLLIN))
			continue;
		r = read(fd, buf + have, sizeof(buf) - have);
		if (r < 0 && errno == EINTR)
			continue;
		if (r <= 0)
			break;
		have += r;
		/* Requests end with an empty field: "\0\0". */
		for (size_t i = 0; i + 1 < have; i++) {
			if (buf[i] != '\0' || buf[i + 1] != '\0')
				continue;
			{
				char *fields[MAX_FIELDS];
				int n = 0;

				for (size_t p = start; p <= i && n < MAX_FIELDS; p += strlen(buf + p) + 1)
					fields[n++] = buf + p;
				if (n > 0 && n < MAX_FIELDS)
					handle(fields, n);
			}
			start = i + 2;
			i++;
		}
		if (start > 0) {
			memmove(buf, buf + start, have - start);
			have -= start;
		} else if (have == sizeof(buf)) {
			have = 0;       /* a request too large: dropped */
		}
	}
	unlink(fifo);
	return 0;
}
