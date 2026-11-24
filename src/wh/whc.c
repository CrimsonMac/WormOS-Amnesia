/*
 * whc - run a command inside a wormhole: a container holding another
 * distribution (Debian, Kali), installed by wh under /var/lib/wormholes.
 *
 *     whc NAME COMMAND [ARGS...]           as the calling user
 *     whc --root NAME COMMAND [ARGS...]    as root (wh uses this for apt)
 *
 * The container shares the host's network, /dev, /proc, /sys, /tmp, /home
 * and /run (sockets of the desktop session: Wayland programs show up on
 * the desktop), and the host's fonts read-only. Programs see the
 * container's files everywhere else. A normal user runs inside a user namespace that
 * maps only their own IDs: no setuid helper is involved, and the
 * container's root-owned files stay read-only to them.
 *
 * Wormholes are a convenience, not a sandbox: they share the user's home
 * and network. Untrusted programs belong in a blackhole.
 *
 * Internal options for wh: --dir DIR (use DIR as the root instead of NAME)
 * and --bind SRC:DST (extra bind mount, repeatable).
 */

#define _GNU_SOURCE
#include <errno.h>
#include <limits.h>
#include <sched.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>

#include "ns.h"

#define WORMHOLES  "/var/lib/wormholes"
#define MAX_BINDS  16

static char root_dir[PATH_MAX];

static void die(const char *fmt, ...)
{
	va_list ap;

	fputs("whc: ", stderr);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
	exit(127);
}

/* Bind src onto dst inside the container root. */
static void bind(const char *src, const char *dst, unsigned long flags, int optional)
{
	if (ns_bind(root_dir, src, dst, flags) < 0 && !optional)
		die("cannot mount %s in the wormhole: %s", dst, strerror(errno));
}

/* Folders that are the same inside a wormhole as outside. */
static int shared(const char *path)
{
	static const char *const dirs[] = { "/home", "/tmp", "/run", "/mnt", "/media", "/dev",
					    "/proc", "/sys", NULL };

	if (strcmp(path, "/") == 0)
		return 1;
	if (geteuid() == 0 && (!strcmp(path, "/root") || !strncmp(path, "/root/", 6)))
		return 1;
	for (int i = 0; dirs[i]; i++) {
		size_t n = strlen(dirs[i]);

		if (!strncmp(path, dirs[i], n) && (path[n] == '\0' || path[n] == '/'))
			return 1;
	}
	return 0;
}

/* var=only becomes var=list: a toolkit told to use only Wayland fails. */
static void fallback(const char *var, const char *only, const char *list)
{
	const char *v = getenv(var);

	if (v && strcmp(v, only) == 0)
		setenv(var, list, 1);
}

/*
 * The same environment for every program in a wormhole, whatever its
 * toolkit, so that none needs a fix of its own:
 *   - Qt, GTK and SDL try Wayland, then X11 (Xwayland). The session asks
 *     for Wayland, and a distribution's program without its Wayland
 *     plugin (Qt programs from Debian without qt6-wayland) would quit;
 *   - Java's windows stay blank under a compositor that does not reparent
 *     them (labwc's Xwayland);
 *   - there is no accessibility bus: GTK 3 would wait for one and warn.
 */
static void app_environment(void)
{
	fallback("QT_QPA_PLATFORM", "wayland", "wayland;xcb");
	fallback("GDK_BACKEND", "wayland", "wayland,x11");
	fallback("SDL_VIDEODRIVER", "wayland", "wayland,x11");
	setenv("_JAVA_AWT_WM_NONREPARENTING", "1", 0);
	setenv("NO_AT_BRIDGE", "1", 0);
}

int main(int argc, char **argv)
{
	const char *binds[MAX_BINDS], *dir = NULL, *name;
	char cwd[PATH_MAX], target[PATH_MAX + 16];
	int as_root = 0, nbinds = 0, i = 1;
	struct stat st;

	for (; i < argc && argv[i][0] == '-'; i++) {
		if (strcmp(argv[i], "--root") == 0) {
			as_root = 1;
		} else if (strcmp(argv[i], "--dir") == 0 && i + 1 < argc) {
			dir = argv[++i];
		} else if (strcmp(argv[i], "--bind") == 0 && i + 1 < argc && nbinds < MAX_BINDS) {
			binds[nbinds++] = argv[++i];
		} else if (strcmp(argv[i], "--") == 0) {
			i++;
			break;
		} else {
			die("unknown option %s", argv[i]);
		}
	}
	if ((dir && argc - i < 1) || (!dir && argc - i < 2)) {
		fprintf(stderr, "usage: whc [--root] NAME COMMAND [ARGS...]\n");
		return 2;
	}
	if ((as_root || dir || nbinds) && geteuid() != 0)
		die("--root, --dir and --bind are for root (wh)");

	if (dir) {
		snprintf(root_dir, sizeof(root_dir), "%s", dir);
		name = "wormhole";
	} else {
		name = argv[i++];
		if (!name[0] || strchr(name, '/') || name[0] == '.')
			die("invalid wormhole name '%s'", name);
		snprintf(root_dir, sizeof(root_dir), "%s/%s", WORMHOLES, name);
	}
	if (stat(root_dir, &st) < 0 || !S_ISDIR(st.st_mode))
		die("there is no '%s' wormhole (wh sets it up when needed)", name);
	if (!getcwd(cwd, sizeof(cwd)))
		snprintf(cwd, sizeof(cwd), "/");

	if (geteuid() == 0) {
		if (unshare(CLONE_NEWNS) < 0)
			die("cannot create a mount namespace: %s", strerror(errno));
	} else {
		uid_t uid = getuid();
		gid_t gid = getgid();

		/* Only the caller's own IDs, read before they become unmapped. */
		if (unshare(CLONE_NEWUSER | CLONE_NEWNS) < 0)
			die("cannot create a user namespace: %s", strerror(errno));
		if (ns_map_ids(0, uid, uid, gid, gid) < 0)
			die("cannot map user IDs: %s", strerror(errno));
	}
	if (mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) < 0)
		die("cannot make mounts private: %s", strerror(errno));

	/* The root must be a mount point for pivot_root. */
	if (mount(root_dir, root_dir, NULL, MS_BIND | MS_REC, NULL) < 0)
		die("cannot mount the wormhole: %s", strerror(errno));

	bind("/dev", "/dev", 0, 0);
	bind("/proc", "/proc", 0, 0);
	bind("/sys", "/sys", 0, 0);
	bind("/etc/resolv.conf", "/etc/resolv.conf", MS_RDONLY, 1);
	bind("/etc/hosts", "/etc/hosts", MS_RDONLY, 1);
	if (!as_root) {
		/* The user's own session: home, temporary files, sockets. */
		bind("/tmp", "/tmp", 0, 0);
		bind("/home", "/home", 0, 1);
		bind("/run", "/run", 0, 1);
		/* Disks the user mounted, and root's own home for programs
		 * started from the root terminal (they would otherwise see the
		 * container's /root and miss the files next to them). */
		bind("/mnt", "/mnt", 0, 1);
		bind("/media", "/media", 0, 1);
		if (geteuid() == 0)
			bind("/root", "/root", 0, 1);
		bind("/etc/passwd", "/etc/passwd", MS_RDONLY, 1);
		bind("/etc/group", "/etc/group", MS_RDONLY, 1);
		/* The system's fonts, so programs look like the desktop (the
		 * mount point is created when the wormhole is set up). */
		bind("/usr/share/fonts", "/usr/local/share/fonts/wormos", MS_RDONLY, 1);
	}
	for (int b = 0; b < nbinds; b++) {
		char src[PATH_MAX], *colon;

		snprintf(src, sizeof(src), "%s", binds[b]);
		if (!(colon = strchr(src, ':')) || colon[1] != '/')
			die("--bind needs SRC:/DST");
		*colon = '\0';
		bind(src, colon + 1, 0, 0);
	}

	if (ns_pivot(root_dir) < 0)
		die("cannot enter the wormhole: %s", strerror(errno));

	/* Stay in the same folder: shared folders (home, /tmp, /root, disks)
	 * are the same inside. Elsewhere the program would see the
	 * container's folder of that name, or none: say so. */
	snprintf(target, sizeof(target), "%s", cwd);
	if (!as_root && !dir && !shared(cwd))
		fprintf(stderr, "whc: %s is not shared with the %s wormhole; "
			"programs there see its own files (home, /tmp, /mnt and /media are shared)\n",
			cwd, name);
	if (chdir(target) < 0 && chdir("/") < 0)
		die("cannot change directory: %s", strerror(errno));

	setenv("PATH", "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin", 1);
	setenv("WORMHOLE", name, 1);
	/*
	 * The user's shell (zsh) is WormOS's and usually missing inside;
	 * programs that run commands through $SHELL (script, make, editors)
	 * get the container's sh instead.
	 */
	{
		const char *shell = getenv("SHELL");

		if (shell && (shell[0] != '/' || access(shell, X_OK) < 0))
			setenv("SHELL", "/bin/sh", 1);
	}
	if (!as_root)
		app_environment();
	if (as_root) {
		setenv("DEBIAN_FRONTEND", "noninteractive", 1);
		setenv("HOME", "/root", 1);
	}
	execvp(argv[i], argv + i);
	die("cannot run %s in the wormhole: %s", argv[i], strerror(errno));
}
