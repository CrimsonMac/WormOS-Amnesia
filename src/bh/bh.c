/*
 * bh - blackholes: disposable sandboxes for untrusted programs.
 *
 *     bh open [OPTIONS] [NAME] [-- COMMAND...]
 *                                      enter a blackhole (a shell by default);
 *                                      if NAME is running, another session in it
 *     bh put NAME FILE...              copy files into its /inbox
 *     bh list                          your blackholes
 *     bh close NAME                    stop it and delete everything in it
 *
 * Options for open (when the blackhole starts):
 *     --net            a network through pasta (not the local network)
 *     --lan            with --net, the local network too
 *     --vm             a virtual machine with its own kernel (KVM)
 *     --gui            programs may open windows on the desktop, marked as
 *                      sandboxed: no screenshots, no clipboard managers, no
 *                      control of other windows (BH_LIBDIR/wayland)
 *     --from SOURCE    the system of a wormhole (debian, kali) instead of
 *                      the host's
 *     --memory SIZE    memory limit (K, M, G); with --vm the guest's memory
 *     --pids N         most processes and threads
 *     --cpus N         with --vm, virtual CPUs
 *
 * Without a NAME, open makes a temporary blackhole that is deleted when the
 * program exits. A named one keeps its files until it is closed.
 *
 * Things go in, nothing escapes: inside, programs see the system read-only
 * and nothing of /home, /root, /var or the snapshots; they have no network
 * but loopback unless opened with --net, and run as root of the blackhole
 * only (see box.c and sandbox.c).
 *
 * State lives in BH_DIR/UID/NAME, owned by the caller:
 *     lock     held by the bh running it     info    its ID and creation time
 *     pid      that bh's PID                 rootfs  its root directory
 *     inbox    shared with /inbox inside     mode    how it runs (net, vm, from)
 *     control  where other sessions connect
 */

#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "bh.h"

void die(const char *fmt, ...)
{
	va_list ap;

	fputs("bh: ", stderr);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
	exit(1);
}

void warn(const char *fmt, ...)
{
	va_list ap;

	fputs("bh: ", stderr);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
}

int valid_name(const char *s)
{
	size_t len = strlen(s);

	if (len == 0 || len > NAME_MAX_BH || s[0] == '-' || s[0] == '.')
		return 0;
	for (; *s; s++)
		if (!(*s >= 'a' && *s <= 'z') && !(*s >= 'A' && *s <= 'Z') &&
		    !(*s >= '0' && *s <= '9') && *s != '-' && *s != '_' && *s != '.')
			return 0;
	return 1;
}

/*
 * BH_DIR/UID. BH_DIR is shared and sticky like /tmp, so anyone could have
 * made a directory with our number: only use it if it is really ours.
 * Root's is traversable so the blackhole's own ID can reach its layers.
 */
static void user_dir(char *out, size_t len)
{
	uid_t uid = getuid();
	struct stat st;
	int fd;

	if (uid == 0 && mkdir(BH_DIR, 0755) == 0)
		chmod(BH_DIR, 01777);
	snprintf(out, len, "%s/%u", BH_DIR, uid);
	if (mkdir(out, 0700) < 0 && errno != EEXIST)
		die("cannot create %s: %s", out, strerror(errno));
	/* Checked and changed through one descriptor: never a link. */
	if ((fd = open(out, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)) < 0 ||
	    fstat(fd, &st) < 0 || st.st_uid != uid)
		die("%s does not belong to you; remove it", out);
	if (fchmod(fd, uid == 0 ? 0711 : 0700) < 0)
		die("cannot set the mode of %s: %s", out, strerror(errno));
	close(fd);
}

static void box_path(struct box *b, const char *name)
{
	char dir[64];

	if (!valid_name(name))
		die("invalid blackhole name '%s' (letters, digits, '-', '_' and '.')", name);
	user_dir(dir, sizeof(dir));
	snprintf(b->name, sizeof(b->name), "%s", name);
	if (snprintf(b->dir, sizeof(b->dir), "%s/%s", dir, name) >= (int)sizeof(b->dir))
		die("path too long");
}

static int read_info(const char *dir, uid_t *id, time_t *created)
{
	char path[PATH_MAX], line[128];
	unsigned long long v;
	int found = 0;
	FILE *f;

	snprintf(path, sizeof(path), "%s/info", dir);
	if (!(f = fopen(path, "re")))
		return -1;
	while (fgets(line, sizeof(line), f)) {
		if (sscanf(line, "id %llu", &v) == 1) {
			*id = v;
			found = 1;
		} else if (created && sscanf(line, "created %llu", &v) == 1) {
			*created = v;
		}
	}
	fclose(f);
	return found ? 0 : -1;
}

/* The outside ID for a new blackhole of root: the lowest one not in use. */
static uid_t free_root_id(const char *parent)
{
	static unsigned char used[BH_ID_COUNT / 8];
	char path[PATH_MAX];
	struct dirent *e;
	DIR *d = opendir(parent);
	uid_t id;

	memset(used, 0, sizeof(used));
	while (d && (e = readdir(d))) {
		if (e->d_name[0] == '.')
			continue;
		snprintf(path, sizeof(path), "%s/%.32s", parent, e->d_name);
		if (read_info(path, &id, NULL) == 0 && id >= BH_ID_BASE &&
		    id - BH_ID_BASE < BH_ID_COUNT)
			used[(id - BH_ID_BASE) / 8] |= 1 << ((id - BH_ID_BASE) % 8);
	}
	if (d)
		closedir(d);
	for (unsigned n = 1; n < BH_ID_COUNT; n++)
		if (!(used[n / 8] & (1 << (n % 8))))
			return BH_ID_BASE + n;
	die("no free IDs for another blackhole");
}

static void make_dir(const char *dir, const char *sub, mode_t mode, uid_t owner)
{
	char path[PATH_MAX];

	snprintf(path, sizeof(path), "%s/%s", dir, sub);
	if (mkdir(path, mode) < 0 && errno != EEXIST)
		die("cannot create %s: %s", path, strerror(errno));
	if (chmod(path, mode) < 0 || lchown(path, owner, owner) < 0)
		die("cannot set up %s: %s", path, strerror(errno));
}

/*
 * Root's blackholes run as their own ID, which must reach the root
 * directory inside: their directory is open to that ID's group only, not
 * to other users (who could otherwise use what a blackhole left there).
 */
static void box_protect(const struct box *b)
{
	if (getuid() != 0)
		return;
	if (chown(b->dir, 0, b->id) < 0 || chmod(b->dir, 0710) < 0)
		die("cannot protect %s: %s", b->dir, strerror(errno));
}

/*
 * Create the blackhole's directories. Its root holds mount points for the
 * host's /usr and /etc and its own writable directories, all owned by the
 * blackhole: what the host has there is owned by IDs it does not have.
 */
static void box_create(struct box *b)
{
	static const struct { const char *path; mode_t mode; } layout[] = {
		{ "rootfs", 0755 }, { "rootfs/usr", 0755 }, { "rootfs/etc", 0755 },
		{ "rootfs/proc", 0555 }, { "rootfs/dev", 0755 }, { "rootfs/inbox", 0755 },
		{ "rootfs/root", 0700 }, { "rootfs/home", 0755 }, { "rootfs/tmp", 01777 },
		{ "rootfs/var", 0755 }, { "rootfs/var/tmp", 01777 }, { "rootfs/run", 0755 },
		{ "rootfs/mnt", 0755 }, { "inbox", 0755 },
	};
	static const char *const links[][2] = {
		{ "usr/bin", "bin" }, { "usr/sbin", "sbin" }, { "usr/lib", "lib" },
		{ "usr/lib", "lib64" },
	};
	char parent[256], path[PATH_MAX];
	uid_t uid = getuid();
	int pfd;
	FILE *f;

	snprintf(parent, sizeof(parent), "%.*s", (int)(strrchr(b->dir, '/') - b->dir), b->dir);
	/* One creation at a time, so two never take the same ID. */
	if ((pfd = open(parent, O_RDONLY | O_DIRECTORY | O_CLOEXEC)) < 0 || flock(pfd, LOCK_EX) < 0)
		die("cannot lock %s: %s", parent, strerror(errno));
	if (mkdir(b->dir, 0700) < 0) {
		if (errno != EEXIST)
			die("cannot create %s: %s", b->dir, strerror(errno));
		close(pfd);
		if (read_info(b->dir, &b->id, NULL) < 0)
			die("%s is damaged; 'bh close %s' removes it", b->dir, b->name);
		box_protect(b);
		return;
	}
	b->id = uid == 0 ? free_root_id(parent) : uid;
	box_protect(b);
	for (size_t i = 0; i < sizeof(layout) / sizeof(layout[0]); i++)
		make_dir(b->dir, layout[i].path, layout[i].mode, b->id);
	for (size_t i = 0; i < sizeof(links) / sizeof(links[0]); i++) {
		snprintf(path, sizeof(path), "%s/rootfs/%s", b->dir, links[i][1]);
		if (symlink(links[i][0], path) < 0 || lchown(path, b->id, b->id) < 0)
			die("cannot create %s: %s", path, strerror(errno));
	}

	snprintf(path, sizeof(path), "%s/info", b->dir);
	if (!(f = fopen(path, "wxe")))
		die("cannot write %s: %s", path, strerror(errno));
	fprintf(f, "id %u\ncreated %lld\n", (unsigned)b->id, (long long)time(NULL));
	if (fclose(f) != 0)
		die("cannot write %s: %s", path, strerror(errno));
	close(pfd);
}

static int box_exists(const struct box *b)
{
	struct stat st;

	return lstat(b->dir, &st) == 0 && S_ISDIR(st.st_mode);
}

/* Open the lock file; returns the fd, locked if lock_it and it was free. */
static int box_lock(const struct box *b, int lock_it, int *running)
{
	char path[PATH_MAX];
	int fd;

	snprintf(path, sizeof(path), "%s/lock", b->dir);
	fd = open(path, O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
	if (fd < 0)
		die("cannot open %s: %s", path, strerror(errno));
	*running = 0;
	if (flock(fd, LOCK_EX | LOCK_NB) < 0) {
		if (errno != EWOULDBLOCK)
			die("cannot lock %s: %s", path, strerror(errno));
		*running = 1;
	} else if (!lock_it) {
		flock(fd, LOCK_UN);
	}
	return fd;
}

/* Delete a tree without following symlinks, even through unreadable dirs. */
static int remove_tree(int at, const char *name)
{
	struct dirent *e;
	struct stat st;
	DIR *d;
	int fd;

	if (fstatat(at, name, &st, AT_SYMLINK_NOFOLLOW) < 0)
		return errno == ENOENT ? 0 : -1;
	if (!S_ISDIR(st.st_mode))
		return unlinkat(at, name, 0);
	/* Best effort: if the directory still cannot be opened, that is reported below. */
	if (fchmodat(at, name, 0700, 0) < 0) {
	}
	fd = openat(at, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	if (fd < 0 || !(d = fdopendir(fd))) {
		if (fd >= 0)
			close(fd);
		return -1;
	}
	while ((e = readdir(d)))
		if (strcmp(e->d_name, ".") && strcmp(e->d_name, ".."))
			remove_tree(dirfd(d), e->d_name);
	closedir(d);
	return unlinkat(at, name, AT_REMOVEDIR);
}

static void box_delete(const struct box *b)
{
	char parent[256];
	int fd;

	snprintf(parent, sizeof(parent), "%.*s", (int)(strrchr(b->dir, '/') - b->dir), b->dir);
	fd = open(parent, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (fd < 0 || remove_tree(fd, b->name) < 0)
		die("cannot delete %s: %s", b->dir, strerror(errno));
	close(fd);
}

/* What a running blackhole was opened with, for bh list. */
static void write_mode(const struct box *b, const struct opts *o)
{
	char path[PATH_MAX];
	FILE *f;

	snprintf(path, sizeof(path), "%s/mode", b->dir);
	unlink(path);
	if (!(f = fopen(path, "wxe")))
		return;
	fprintf(f, "%s%s%s%s%s\n", o->vm ? "vm " : "", o->net ? (o->lan ? "net+lan " : "net ") : "",
		o->gui ? "gui " : "", o->from[0] ? "from=" : "", o->from);
	fclose(f);
}

static void read_mode(const struct box *b, char *out, size_t len)
{
	char path[PATH_MAX];
	FILE *f;

	snprintf(out, len, "-");
	snprintf(path, sizeof(path), "%s/mode", b->dir);
	if (!(f = fopen(path, "re")))
		return;
	if (fgets(out, len, f)) {
		out[strcspn(out, "\n")] = '\0';
		while (*out && out[strlen(out) - 1] == ' ')
			out[strlen(out) - 1] = '\0';
		if (!*out)
			snprintf(out, len, "-");
	}
	fclose(f);
}

/*
 * --gui: the blackhole's own Wayland display (BH_LIBDIR/wayland, which asks
 * the compositor for a sandboxed listening socket at DIR/wayland-0). Returns
 * the helper's input: closing it ends the display.
 */
static int start_display(const struct box *b, pid_t *pid)
{
	int in[2], out[2];
	char socket_path[PATH_MAX], line[32] = "";
	ssize_t n;

	snprintf(socket_path, sizeof(socket_path), "%s/wayland-0", b->dir);
	if (pipe2(in, O_CLOEXEC) < 0 || pipe2(out, O_CLOEXEC) < 0)
		die("pipe: %s", strerror(errno));
	if ((*pid = fork()) < 0)
		die("fork: %s", strerror(errno));
	if (*pid == 0) {
		dup2(in[0], 0);
		dup2(out[1], 1);
		execl(BH_LIBDIR "/wayland", "bh-wayland", socket_path, b->name, (char *)NULL);
		fprintf(stderr, "bh: --gui needs %s (the bh-wayland package)\n", BH_LIBDIR "/wayland");
		_exit(127);
	}
	close(in[0]);
	close(out[1]);
	n = read(out[0], line, sizeof(line) - 1);
	close(out[0]);
	if (n <= 0 || strncmp(line, "ready", 5)) {
		waitpid(*pid, NULL, 0);
		die("cannot give the blackhole a display%s", "");
	}
	return in[1];
}

static int cmd_open(const char *name, const struct opts *o, int given, char *const argv[])
{
	pid_t display_pid = 0;
	int display = -1;
	char path[PATH_MAX], tmp[NAME_MAX_BH + 1];
	struct box b;
	int fd, running, code;
	FILE *f;

	if (!name) {
		snprintf(tmp, sizeof(tmp), "tmp%d", (int)getpid());
		name = tmp;
	}
	box_path(&b, name);
	box_create(&b);
	fd = box_lock(&b, 1, &running);
	if (running) {
		close(fd);
		if (given)
			die("'%s' is already running; options only apply when a blackhole starts",
			    b.name);
		if (!argv)
			fprintf(stderr, "Joining blackhole %s (a new session; it closes when its "
				"first session ends).\n", b.name);
		return box_join(&b, argv);
	}
	snprintf(path, sizeof(path), "%s/pid", b.dir);
	if ((f = fopen(path, "we"))) {
		fprintf(f, "%d\n", (int)getpid());
		fclose(f);
	}
	write_mode(&b, o);
	if (o->gui)
		display = start_display(&b, &display_pid);
	if (!argv)
		fprintf(stderr, "Entering blackhole %s%s: %s, the system is read-only, "
			"files from 'bh put' are in /inbox.\n%s\n", b.name,
			o->vm ? " (virtual machine)" : "",
			!o->net ? (o->gui ? "no network, windows on the desktop" : "no network") :
			o->lan ? "network including the local one" : "network (not the local one)",
			o->vm ? "Exit the shell to leave; Ctrl+A X stops the machine." :
			"Exit the shell to leave. 'bh open NAME' in another terminal joins it.");

	code = box_run(&b, o, argv);
	if (display >= 0) {
		close(display);
		waitpid(display_pid, NULL, 0);
	}

	unlink(path);
	snprintf(path, sizeof(path), "%s/mode", b.dir);
	unlink(path);
	if (name == tmp)
		box_delete(&b);
	else if (!argv)
		fprintf(stderr, "Left blackhole %s. Its files stay until 'bh close %s'.\n",
			b.name, b.name);
	close(fd);
	return code;
}

/*
 * Copy files into the inbox. The blackhole may be running and controls
 * what is in there, so files are only ever created new (O_EXCL, no
 * symlinks), and for root the writing runs as the blackhole's own ID.
 */
static int cmd_put(const char *name, char **files, int n)
{
	char path[PATH_MAX];
	struct box b;
	int inbox, status, *src = calloc(n, sizeof(int));
	pid_t pid;

	box_path(&b, name);
	box_create(&b);
	if (!src)
		die("out of memory");
	for (int i = 0; i < n; i++) {
		struct stat st;

		if ((src[i] = open(files[i], O_RDONLY | O_CLOEXEC)) < 0)
			die("cannot open %s: %s", files[i], strerror(errno));
		if (fstat(src[i], &st) < 0 || !S_ISREG(st.st_mode))
			die("%s is not a regular file", files[i]);
	}
	snprintf(path, sizeof(path), "%s/inbox", b.dir);
	if ((inbox = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)) < 0)
		die("cannot open %s: %s", path, strerror(errno));

	if ((pid = fork()) < 0)
		die("cannot fork: %s", strerror(errno));
	if (pid == 0) {
		if (getuid() == 0 && (setgroups(0, NULL) < 0 || setresgid(b.id, b.id, b.id) < 0 ||
				      setresuid(b.id, b.id, b.id) < 0))
			die("cannot drop privileges: %s", strerror(errno));
		for (int i = 0; i < n; i++) {
			const char *base = strrchr(files[i], '/') ? strrchr(files[i], '/') + 1 : files[i];
			char buf[65536];
			ssize_t r;
			int out;

			out = openat(inbox, base, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,
				     0644);
			if (out < 0)
				die("cannot create %s in the inbox: %s", base, strerror(errno));
			while ((r = read(src[i], buf, sizeof(buf))) > 0)
				if (write(out, buf, r) != r)
					die("cannot write %s: %s", base, strerror(errno));
			if (r < 0 || close(out) < 0)
				die("cannot copy %s: %s", files[i], strerror(errno));
			printf("%s -> %s:/inbox/%s\n", files[i], b.name, base);
		}
		exit(0);
	}
	while (waitpid(pid, &status, 0) < 0)
		if (errno != EINTR)
			die("cannot wait: %s", strerror(errno));
	return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}

static int cmd_list(void)
{
	char dir[64];
	struct dirent *e;
	int shown = 0;
	DIR *d;

	user_dir(dir, sizeof(dir));
	if (!(d = opendir(dir)))
		die("cannot read %s: %s", dir, strerror(errno));
	while ((e = readdir(d))) {
		char created[32] = "?", mode[64];
		struct box b;
		time_t t = 0;
		int fd, running;

		if (!valid_name(e->d_name))
			continue;
		snprintf(b.name, sizeof(b.name), "%.32s", e->d_name);
		snprintf(b.dir, sizeof(b.dir), "%s/%.32s", dir, e->d_name);
		if (read_info(b.dir, &b.id, &t) < 0)
			continue;
		fd = box_lock(&b, 0, &running);
		close(fd);
		strftime(created, sizeof(created), "%Y-%m-%d %H:%M", localtime_r(&t, &(struct tm){ 0 }));
		if (running)
			read_mode(&b, mode, sizeof(mode));
		else
			snprintf(mode, sizeof(mode), "-");
		if (!shown++)
			printf("%-20s %-8s %-17s %s\n", "NAME", "STATE", "CREATED", "MODE");
		printf("%-20s %-8s %-17s %s\n", b.name, running ? "running" : "idle", created, mode);
	}
	closedir(d);
	if (!shown)
		printf("No blackholes. 'bh open' makes a temporary one.\n");
	return 0;
}

static int cmd_close(const char *name)
{
	char path[PATH_MAX];
	struct box b;
	int fd, running, pid = 0;
	FILE *f;

	box_path(&b, name);
	if (!box_exists(&b))
		die("there is no blackhole '%s'", name);
	fd = box_lock(&b, 1, &running);
	if (running) {
		/* The bh running it kills everything inside and lets go of the lock. */
		snprintf(path, sizeof(path), "%s/pid", b.dir);
		if ((f = fopen(path, "re"))) {
			if (fscanf(f, "%d", &pid) != 1)
				pid = 0;
			fclose(f);
		}
		if (pid <= 1 || kill(pid, SIGTERM) < 0)
			die("cannot stop '%s'; exit its shell first", name);
		/* A stopped bh (a suspended job) handles the signal once it runs. */
		kill(pid, SIGCONT);
		if (flock(fd, LOCK_EX) < 0)
			die("cannot lock %s: %s", b.dir, strerror(errno));
	}
	box_delete(&b);
	close(fd);
	printf("Closed blackhole %s.\n", name);
	return 0;
}

static void usage(FILE *out)
{
	fputs("usage: bh open [--net [--lan]] [--vm] [--gui] [--from SOURCE] [--memory SIZE]\n"
	      "               [--pids N] [--cpus N] [NAME] [-- COMMAND [ARGS...]]\n"
	      "       bh put NAME FILE...\n"
	      "       bh list\n"
	      "       bh close NAME\n", out);
}

static unsigned long long parse_size(const char *s)
{
	char *end;
	unsigned long long v;

	errno = 0;
	v = strtoull(s, &end, 10);
	if (errno || end == s || s[0] == '-')
		die("bad size '%s' (e.g. 512M or 2G)", s);
	switch (*end) {
	case 'k': case 'K': v <<= 10; end++; break;
	case 'm': case 'M': v <<= 20; end++; break;
	case 'g': case 'G': v <<= 30; end++; break;
	case '\0': break;
	default: die("bad size '%s' (e.g. 512M or 2G)", s);
	}
	if (*end && strcmp(end, "B") && strcmp(end, "iB"))
		die("bad size '%s' (e.g. 512M or 2G)", s);
	return v;
}

static unsigned parse_count(const char *s, unsigned max)
{
	char *end;
	unsigned long v;

	errno = 0;
	v = strtoul(s, &end, 10);
	if (errno || end == s || *end || s[0] == '-' || v == 0 || v > max)
		die("bad number '%s' (1 to %u)", s, max);
	return v;
}

/* Fill in the defaults that depend on the machine. */
static void finish_opts(struct opts *o)
{
	unsigned long long total = memory_total(), guest;
	long cpus = sysconf(_SC_NPROCESSORS_ONLN);
	struct stat st;

	if (o->lan && !o->net)
		die("--lan goes with --net");
	if (o->gui && o->vm)
		die("--gui does not go with --vm");
	if (o->from[0]) {
		char path[PATH_MAX];

		if (!valid_name(o->from))
			die("invalid source '%s'", o->from);
		snprintf(path, sizeof(path), "%s/%s/usr/bin", WORMHOLES, o->from);
		if (stat(path, &st) < 0)
			die("there is no %s wormhole yet: 'wh install --from %s PACKAGE' sets it up",
			    o->from, o->from);
	}
	if (!o->pids)
		o->pids = BH_PIDS_DEFAULT;
	if (o->vm) {
		/* The guest's memory, and the cgroup's room for it and the monitor. */
		guest = o->memory ? o->memory : total / 4;
		if (guest > 4ull << 30 && !o->memory)
			guest = 4ull << 30;
		if (guest < 256ull << 20)
			guest = 256ull << 20;
		o->vm_mb = guest >> 20;
		o->memory = guest + (256ull << 20);
		if (!o->cpus)
			o->cpus = cpus > 2 ? 2 : (cpus > 0 ? cpus : 1);
	} else {
		if (o->cpus)
			die("--cpus goes with --vm");
		if (!o->memory)
			o->memory = total / 2;
	}
	if (o->memory < 64ull << 20)
		die("at least 64M of memory");
	if (total && o->memory > total)
		o->memory = total;
}

int main(int argc, char **argv)
{
	umask(022);
	/* Output to a reader that went away (| head) must not kill bh
	 * before it cleans up: writes fail instead (see relay.c). */
	signal(SIGPIPE, SIG_IGN);
	if (argc < 2) {
		usage(stderr);
		return 2;
	}
	if (!strcmp(argv[1], "help") || !strcmp(argv[1], "--help")) {
		usage(stdout);
		return 0;
	}
	if (!strcmp(argv[1], "open")) {
		struct opts o = { 0 };
		const char *name = NULL;
		int i = 2, given = 0;

		for (; i < argc && !strncmp(argv[i], "--", 2) && argv[i][2]; i++, given = 1) {
			const char *opt = argv[i] + 2;

			if (!strcmp(opt, "net"))
				o.net = 1;
			else if (!strcmp(opt, "lan"))
				o.lan = 1;
			else if (!strcmp(opt, "vm"))
				o.vm = 1;
			else if (!strcmp(opt, "gui"))
				o.gui = 1;
			else if (i + 1 < argc && !strcmp(opt, "from"))
				snprintf(o.from, sizeof(o.from), "%s", argv[++i]);
			else if (i + 1 < argc && !strcmp(opt, "memory"))
				o.memory = parse_size(argv[++i]);
			else if (i + 1 < argc && !strcmp(opt, "pids"))
				o.pids = parse_count(argv[++i], BH_PIDS_MAX);
			else if (i + 1 < argc && !strcmp(opt, "cpus"))
				o.cpus = parse_count(argv[++i], 64);
			else {
				usage(stderr);
				return 2;
			}
		}
		if (i < argc && strcmp(argv[i], "--"))
			name = argv[i++];
		if (i < argc && strcmp(argv[i], "--")) {
			usage(stderr);
			return 2;
		}
		if (i < argc && ++i == argc) {
			usage(stderr);
			return 2;
		}
		finish_opts(&o);
		return cmd_open(name, &o, given, i < argc ? argv + i : NULL);
	}
	if (!strcmp(argv[1], "put") && argc >= 4)
		return cmd_put(argv[2], argv + 3, argc - 3);
	if (!strcmp(argv[1], "list") && argc == 2)
		return cmd_list();
	if (!strcmp(argv[1], "close") && argc == 3)
		return cmd_close(argv[2]);
	usage(stderr);
	return 2;
}
