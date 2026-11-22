/*
 * wh (wormhole) - the WormOS package manager.
 *
 * Changes are transactional: wh never modifies the running system. It asks
 * snap for a writable copy of the system (@.next), mounts that copy where
 * only wh can see it, installs or removes packages there, and has snap
 * record the change, taking a snapshot of the current system first so the
 * change can be undone. A soft reboot then switches to the new system.
 *
 * With --root DIR, wh works directly on DIR instead (no copy, snapshot or
 * restart). The image build uses this to assemble the root filesystem.
 *
 * Software that is not in the core repository comes from other
 * distributions (Debian, then Kali), each kept in its own container, a
 * wormhole (see foreign.c). Those installs need no restart.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "wh.h"

#define STAGE_DIR    "/run/wh"
#define STAGE_MNT    STAGE_DIR "/next"
#define LOCK_FILE    "/run/wh.lock"
#define HISTORY      WH_STATE_DIR "/history"
#define COMMENT_LEN  256

const char *root = "/";
int root_fd = -1;
int verbose;
const char *key_file;

static const char *repo_override;
static const char *opt_from;            /* --from SOURCE: core, debian, kali */
static int opt_try, opt_later, opt_yes, opt_force;
static int direct;              /* --root given: no staging */
static int live;                /* WH_LIVE_FLAG: changes go into the running system */
static int staging;             /* a staged copy is mounted */
static int kernel_changed;

/* Packages the system cannot run without. */
static const char *const essential[] = {
	"base-files", "glibc", "gcc-libs", "busybox", "worminit", "wormrd",
	"snap", "wh", "linux", "zstd", "zlib", "openssl", "curl",
	"ca-certificates", "worm", NULL,
};

static void usage(FILE *out)
{
	fputs(
"usage: wh COMMAND [ARGS]\n"
"\n"
"  update                   download the latest package index\n"
"  search [TERM]            find packages (--from: in one source)\n"
"  info PACKAGE             show a package\n"
"  list                     list installed packages\n"
"  install [--try] [--from SOURCE] PKG...\n"
"                           install packages (and what they need)\n"
"  remove PKG...            remove packages\n"
"  upgrade                  upgrade installed packages\n"
"  history                  list package changes\n"
"  undo                     undo the last package change\n"
"  keep                     keep a change made with --try\n"
"\n"
"Changes are applied with a quick restart of the system (the kernel keeps\n"
"running). --later applies them at the next restart instead. --try makes a\n"
"trial: the restart after the next one goes back, unless 'wh keep' is run.\n"
"\n"
"Packages not in the core repository come from Debian, then Kali, each in\n"
"its own container (a wormhole); --from picks the source. Those installs\n"
"need no restart.\n",
	      out);
}

static int is_essential(const char *name)
{
	for (int i = 0; essential[i]; i++)
		if (strcmp(essential[i], name) == 0)
			return 1;
	return 0;
}

static void set_root(const char *dir)
{
	if (root_fd >= 0)
		close(root_fd);
	root = dir;
	root_fd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (root_fd < 0)
		die("cannot open %s: %s", dir, strerror(errno));
}

static void lock_wh(void)
{
	int fd;

	if (direct)
		return;
	if (geteuid() != 0)
		die("packages are managed by root, try again with su");
	fd = open(LOCK_FILE, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
	if (fd < 0 || flock(fd, LOCK_EX | LOCK_NB) < 0)
		die("another wh command is running");
}

/* ---- the staged copy ---- */

/* Device, superblock options and per-mount options of the running root. */
static void root_mount(char *dev, size_t dlen, char *super, size_t slen, int *rw)
{
	char line[4096];
	FILE *f = fopen("/proc/self/mountinfo", "re");

	if (!f)
		die("cannot read /proc/self/mountinfo");
	while (fgets(line, sizeof(line), f)) {
		char mnt[PATH_MAX], opts[512], type[64], src[PATH_MAX], sup[1024], *sep;

		if (sscanf(line, "%*s %*s %*s %*s %4095s %511s", mnt, opts) != 2 ||
		    strcmp(mnt, "/") != 0 || !(sep = strstr(line, " - ")) ||
		    sscanf(sep + 3, "%63s %4095s %1023s", type, src, sup) != 3)
			continue;
		fclose(f);
		if (strcmp(type, "btrfs") != 0)
			die("the root filesystem is not btrfs");
		snprintf(dev, dlen, "%s", src);
		snprintf(super, slen, "%s", sup);
		*rw = strncmp(opts, "rw", 2) == 0 && (opts[2] == ',' || !opts[2]);
		return;
	}
	fclose(f);
	die("cannot find the root filesystem");
}

static void snap_cmd(char *const argv[], char *out, size_t len)
{
	if (run(argv, out, len) != 0)
		die("snap %s failed", argv[1]);
}

static void stage_abort(void)
{
	if (!staging)
		return;
	staging = 0;
	umount2(STAGE_MNT, MNT_DETACH);
	run((char *[]){ "snap", "stage", "abort", NULL }, NULL, 0);
	warn("nothing was changed");
}

/* Changes wait until manual edits are locked in (and snapshotted). */
static void require_locked(void)
{
	char dev[PATH_MAX], super[1024];
	int rw;

	if (direct)
		return;
	root_mount(dev, sizeof(dev), super, sizeof(super), &rw);
	if (rw)
		die("the system is unlocked; run 'worm lock' first so manual "
		    "changes are saved");
}

/* Live changes: the running root, writable for the change. */
static void live_relock(void)
{
	if (mount(NULL, "/", NULL, MS_REMOUNT | MS_BIND | MS_RDONLY, NULL) < 0)
		warn("cannot make / read-only again: %s", strerror(errno));
}

/*
 * Mount a fresh copy of the system where only this process sees it (or,
 * live, make the running system writable for the change).
 */
static void stage_begin(void)
{
	char dev[PATH_MAX], super[1024], opts[1200], *tok, *save;
	size_t n = 0;
	int rw;

	if (direct)
		return;
	if (live) {
		if (mount(NULL, "/", NULL, MS_REMOUNT | MS_BIND, NULL) < 0)
			die("cannot make / writable for the change: %s", strerror(errno));
		atexit(live_relock);
		return;
	}
	require_locked();
	root_mount(dev, sizeof(dev), super, sizeof(super), &rw);

	snap_cmd((char *[]){ "snap", "stage", "begin", NULL }, NULL, 0);
	staging = 1;
	atexit(stage_abort);

	/* Same filesystem options as the root, but the staged subvolume. */
	for (tok = strtok_r(super, ",", &save); tok; tok = strtok_r(NULL, ",", &save))
		if (strcmp(tok, "rw") && strcmp(tok, "ro") &&
		    strncmp(tok, "subvol=", 7) && strncmp(tok, "subvolid=", 9))
			n += snprintf(opts + n, sizeof(opts) - n, "%s,", tok);
	snprintf(opts + n, sizeof(opts) - n, "subvol=@.next");

	mkdir(STAGE_DIR, 0700);
	mkdir(STAGE_MNT, 0700);
	if (unshare(CLONE_NEWNS) < 0 ||
	    mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) < 0)
		die("cannot create a private mount namespace: %s", strerror(errno));
	if (mount(dev, STAGE_MNT, "btrfs", MS_NOSUID | MS_NODEV, opts) < 0)
		die("cannot mount the staged system: %s", strerror(errno));
	set_root(STAGE_MNT);
}

/* ---- history ---- */

struct record {
	int id;
	long long date;
	char action[16];
	char packages[1024];
	int snapshot;
};

static int history_read(struct record **out)
{
	char path[PATH_MAX], line[1200];
	struct record *list = NULL;
	int n = 0;
	FILE *f;

	root_path(path, sizeof(path), HISTORY);
	*out = NULL;
	if (!(f = fopen(path, "re")))
		return 0;
	while (fgets(line, sizeof(line), f)) {
		struct record r = { 0 };

		if (sscanf(line, "%d\t%lld\t%15[^\t]\t%1023[^\t]\t%d", &r.id, &r.date,
			   r.action, r.packages, &r.snapshot) != 5)
			continue;
		if (!(list = realloc(list, (n + 1) * sizeof(*list))))
			die("out of memory");
		list[n++] = r;
	}
	fclose(f);
	*out = list;
	return n;
}

static int history_add(const char *action, const char *packages, int snapshot)
{
	char path[PATH_MAX], dir[PATH_MAX];
	struct record *list;
	int n = history_read(&list), id = n ? list[n - 1].id + 1 : 1;
	FILE *f;

	free(list);
	root_path(dir, sizeof(dir), WH_STATE_DIR);
	mkdirs_at(AT_FDCWD, dir, 0755);
	root_path(path, sizeof(path), HISTORY);
	if (!(f = fopen(path, "ae"))) {
		warn("cannot write the history: %s", strerror(errno));
		return id;
	}
	fprintf(f, "%d\t%lld\t%s\t%s\t%d\n", id, (long long)time(NULL), action,
		packages, snapshot);
	fclose(f);
	return id;
}

/* Finish a change: record it with snap and restart into it. */
static void stage_commit(const char *action, const char *packages)
{
	char comment[COMMENT_LEN], out[64];
	int before;

	/* Rebuild the caches the change affects, in the changed root. */
	hooks_run();
	if (direct)
		return;
	if (live) {
		syncfs(root_fd);
		history_add(action, packages, 0);
		info("Done: changed in the running system, with no restart.");
		return;
	}
	/* Let go of the staged copy (no open directory may keep it busy). */
	if (syncfs(root_fd) < 0)
		die("cannot finish the staged system: %s", strerror(errno));
	set_root("/");
	if (umount(STAGE_MNT) < 0)
		die("cannot finish the staged system: %s", strerror(errno));

	snprintf(comment, sizeof(comment), "wh %s %s", action, packages);
	snap_cmd(opt_try ? (char *[]){ "snap", "stage", "commit", "--try", "-c", comment, NULL }
			 : (char *[]){ "snap", "stage", "commit", "-c", comment, NULL },
		 out, sizeof(out));
	staging = 0;
	before = atoi(out);
	history_add(opt_try ? "try" : action, packages, before);

	if (before)
		info("The previous system is saved as snapshot %d ('wh undo' goes back).", before);
	if (opt_later) {
		info("The change will be applied at the next restart.");
		return;
	}
	if (kernel_changed) {
		info("The kernel changed: restarting the machine...");
		kill(1, SIGTERM);
	} else {
		info("Applying the change: restarting the system...");
		kill(1, SIGHUP);
	}
}

/* Ask before a change that is applied right away. */
static void confirm_now(const char *what)
{
	char answer[16];

	if (opt_yes)
		return;
	if (!isatty(STDIN_FILENO))
		die("not a terminal, pass -y to confirm");
	printf("%s Continue? [Y/n] ", what);
	fflush(stdout);
	if (!fgets(answer, sizeof(answer), stdin) ||
	    (answer[0] != '\n' && answer[0] != 'y' && answer[0] != 'Y'))
		die("cancelled");
}

/* Ask before a change that restarts the system. */
static void confirm(const char *what)
{
	char answer[16];

	if (direct || opt_yes)
		return;
	if (!isatty(STDIN_FILENO))
		die("not a terminal, pass -y to confirm");
	printf("%s\n%s Continue? [Y/n] ", what, opt_later ? "It is applied at the next restart."
	       : "The system restarts to apply it.");
	fflush(stdout);
	if (!fgets(answer, sizeof(answer), stdin) ||
	    (answer[0] != '\n' && answer[0] != 'y' && answer[0] != 'Y'))
		die("cancelled");
}

/* ---- resolving ---- */

struct plan {
	struct package **pkgs;
	int count;
};

static int in_plan(struct plan *pl, const char *name)
{
	for (int i = 0; i < pl->count; i++)
		if (strcmp(pl->pkgs[i]->name, name) == 0)
			return 1;
	return 0;
}

/* Add name and, first, whatever it needs that is not installed. */
static void resolve(struct index *idx, struct plan *pl, const char *name,
		    int depth, int reinstall)
{
	struct package *p = index_find(idx, name), cur;
	char deps[DEPS_MAX_LEN], *tok, *save;

	if (!p)
		die("no package named '%s' (try 'wh search')", name);
	if (depth > 64)
		die("dependency loop involving %s", name);
	if (in_plan(pl, name))
		return;
	if (!reinstall && db_installed(name, &cur) && strcmp(cur.version, p->version) == 0)
		return;

	snprintf(deps, sizeof(deps), "%s", p->depends);
	for (tok = strtok_r(deps, " ", &save); tok; tok = strtok_r(NULL, " ", &save))
		if (!db_installed(tok, NULL))
			resolve(idx, pl, tok, depth + 1, 0);
	if (in_plan(pl, name))
		return;
	if (!(pl->pkgs = realloc(pl->pkgs, (pl->count + 1) * sizeof(*pl->pkgs))))
		die("out of memory");
	pl->pkgs[pl->count++] = p;
	/*
	 * Recommended packages come along, unless the user removed them once
	 * (declined) or the repository does not have them. Unlike depends,
	 * they can be removed on their own later.
	 */
	snprintf(deps, sizeof(deps), "%s", p->recommends);
	for (tok = strtok_r(deps, " ", &save); tok; tok = strtok_r(NULL, " ", &save))
		if (!db_installed(tok, NULL) && !db_declined(tok) && index_find(idx, tok))
			resolve(idx, pl, tok, depth + 1, 0);
}

/*
 * The edition's own package (wormos-standard on the standard edition,
 * wormos-VARIANT_ID on others that have one) recommends what the edition
 * comes with; a system that lacks it, such as one upgraded from before it
 * existed, gets it on the next upgrade, so new releases' programs arrive.
 */
static void edition_package(char *out, size_t len)
{
	char path[PATH_MAX], line[128], id[64] = "standard";
	FILE *f;

	root_path(path, sizeof(path), "etc/worm/variant");
	if ((f = fopen(path, "re"))) {
		while (fgets(line, sizeof(line), f))
			if (sscanf(line, "VARIANT_ID=%63[a-z0-9-]", id) == 1)
				break;
		fclose(f);
	}
	snprintf(out, len, "wormos-%s", id);
}

/* Download and check every package of the plan (all at once). */
static char **fetch_plan(struct plan *pl, const char *url)
{
	return repo_fetch_all(url, pl->pkgs, pl->count);
}

static void install_plan(struct plan *pl, char **files)
{
	for (int i = 0; i < pl->count; i++) {
		struct package cur;
		int upgrade = db_installed(pl->pkgs[i]->name, &cur);

		info("%s %s %s", upgrade ? "Upgrading" : "Installing",
		     pl->pkgs[i]->name, pl->pkgs[i]->version);
		if (pkg_install(files[i], pl->pkgs[i]->name) < 0)
			die("installing %s failed", pl->pkgs[i]->name);
		if (strcmp(pl->pkgs[i]->name, "linux") == 0)
			kernel_changed = 1;
	}
	/* Everything is in place: the downloads are not needed any more
	 * (they are in the running system's /var, not the staged one). */
	repo_remove_downloads(files);
}

static void plan_names(struct plan *pl, char *out, size_t len)
{
	size_t n = 0;

	out[0] = '\0';
	for (int i = 0; i < pl->count && n < len; i++)
		n += snprintf(out + n, len - n, "%s%s", i ? " " : "", pl->pkgs[i]->name);
}

/* ---- commands ---- */

static int cmd_update(int argc, char **argv)
{
	(void)argv;
	if (argc != 1)
		die("usage: wh update");
	lock_wh();
	repo_update(repo_url(repo_override));
	if (!direct)
		foreign_refresh(0);
	return 0;
}

static int cmd_install(int argc, char **argv, int reinstall)
{
	struct index idx;
	struct plan pl = { 0 };
	char names[1024], **files;

	if (argc < 2)
		die("usage: wh install [--try] [--later] [--from SOURCE] PACKAGE...");
	lock_wh();
	if (opt_from && strcmp(opt_from, "core") && !foreign_source_known(opt_from))
		die("unknown source '%s' (see /etc/wh/sources)", opt_from);
	repo_load(&idx);

	/* Packages not in core come through a wormhole, applied right away. */
	for (int i = 1; i < argc; i++) {
		const char *source = NULL;
		char what[512], label[256];

		if (opt_from && strcmp(opt_from, "core"))
			source = opt_from;
		else if (!index_find(&idx, argv[i]) && !opt_from)
			source = foreign_find(argv[i]);
		if (!source)
			continue;
		if (direct || opt_try)
			die("%s comes from %s; --root and --try work with the core "
			    "repository only", argv[i], source);
		snprintf(what, sizeof(what), "%s is not in the core repository; it comes from %s%s",
			 argv[i], source, wormhole_exists(source) ? "." :
			 ", which is set up first (a one-time download of its base system).");
		confirm_now(what);
		foreign_install(source, argv[i]);
		snprintf(label, sizeof(label), "%s [%s]", argv[i], source);
		history_add("install", label, 0);
		argv[i] = NULL;
	}
	for (int i = 1; i < argc; i++)
		if (argv[i])
			resolve(&idx, &pl, argv[i], 0, reinstall);
	if (pl.count == 0) {
		if (argc > 1 && !argv[argc - 1])
			return 0;

		info("Nothing to do: already installed.");
		return 0;
	}
	plan_names(&pl, names, sizeof(names));
	{
		char what[1200];

		snprintf(what, sizeof(what), "Packages to install: %s", names);
		confirm(what);
	}
	files = fetch_plan(&pl, repo_url(repo_override));
	stage_begin();
	install_plan(&pl, files);
	/* Asked for again: no longer declined. */
	for (int i = 1; i < argc; i++)
		if (argv[i])
			db_decline(argv[i], 0);
	stage_commit("install", names);
	return 0;
}

static int cmd_remove(int argc, char **argv)
{
	struct package *installed;
	char names[1024] = "";
	size_t len = 0;
	int n;

	if (argc < 2)
		die("usage: wh remove PACKAGE...");
	lock_wh();

	/* Foreign packages first: they are removed right away. */
	for (int i = 1; i < argc; i++) {
		char source[32], what[256], label[256];

		if (!foreign_installed(argv[i], source, sizeof(source)))
			continue;
		snprintf(what, sizeof(what), "Remove %s (from %s)?", argv[i], source);
		confirm_now(what);
		foreign_remove(argv[i]);
		snprintf(label, sizeof(label), "%s [%s]", argv[i], source);
		history_add("remove", label, 0);
		argv[i] = NULL;
	}
	{
		int left = 0;

		for (int i = 1; i < argc; i++)
			left |= argv[i] != NULL;
		if (!left)
			return 0;
		/* Squeeze out the ones handled. */
		for (int i = 1, j = 1; i < argc; i++)
			if (argv[i])
				argv[j++] = argv[i];
		for (int i = argc - 1; i > 0 && !argv[i]; i--)
			argc--;
	}
	require_locked();
	n = db_list(&installed);
	for (int i = 1; i < argc; i++) {
		if (!db_installed(argv[i], NULL))
			die("%s is not installed", argv[i]);
		if (is_essential(argv[i]) && !opt_force)
			die("%s is part of the base system and cannot be removed", argv[i]);
		/* Refuse if something that stays needs it. */
		for (int j = 0; j < n; j++) {
			int leaving = 0;

			for (int k = 1; k < argc; k++)
				leaving |= strcmp(installed[j].name, argv[k]) == 0;
			if (!leaving && word_in(installed[j].depends, argv[i]))
				die("%s is needed by %s", argv[i], installed[j].name);
		}
		len += snprintf(names + len, sizeof(names) - len, "%s%s",
				i > 1 ? " " : "", argv[i]);
	}
	free(installed);
	{
		char what[1200];

		snprintf(what, sizeof(what), "Packages to remove: %s", names);
		confirm(what);
	}

	stage_begin();
	for (int i = 1; i < argc; i++) {
		info("Removing %s", argv[i]);
		if (pkg_remove(argv[i]) < 0)
			die("removing %s failed", argv[i]);
		/* Removed on purpose: no recommendation brings it back. */
		db_decline(argv[i], 1);
		if (strcmp(argv[i], "linux") == 0)
			kernel_changed = 1;
	}
	stage_commit("remove", names);
	return 0;
}

static int cmd_upgrade(int argc, char **argv)
{
	struct package *installed;
	struct plan pl = { 0 };
	struct index idx;
	char names[1024], **files;
	int n;

	(void)argv;
	if (argc != 1)
		die("usage: wh upgrade");
	lock_wh();
	if (!direct)
		foreign_refresh(1);
	require_locked();
	repo_load(&idx);
	n = db_list(&installed);
	for (int i = 0; i < n; i++) {
		struct package *p = index_find(&idx, installed[i].name);

		if (p && strcmp(p->version, installed[i].version) != 0)
			resolve(&idx, &pl, p->name, 0, 1);
	}
	free(installed);
	{
		char edition[NAME_MAX_LEN];

		edition_package(edition, sizeof(edition));
		if (!db_installed(edition, NULL) && !db_declined(edition) &&
		    index_find(&idx, edition))
			resolve(&idx, &pl, edition, 0, 0);
	}
	if (pl.count == 0) {
		info("Everything is up to date.");
		return 0;
	}
	plan_names(&pl, names, sizeof(names));
	{
		char what[1200];

		snprintf(what, sizeof(what), "Packages to upgrade: %s", names);
		confirm(what);
	}
	files = fetch_plan(&pl, repo_url(repo_override));
	stage_begin();
	install_plan(&pl, files);
	stage_commit("upgrade", names);
	return 0;
}

static int cmd_list(int argc, char **argv)
{
	struct package *list;
	int n;

	(void)argv;
	if (argc != 1)
		die("usage: wh list");
	n = db_list(&list);
	for (int i = 0; i < n; i++)
		printf("%-20s %-16s %s\n", list[i].name, list[i].version, list[i].description);
	free(list);
	if (!direct)
		foreign_list();
	return 0;
}

static int cmd_search(int argc, char **argv)
{
	const char *term = argc > 1 ? argv[1] : "";
	struct index idx;
	int found = 0;

	if (argc > 2)
		die("usage: wh search [--from SOURCE] [TERM]");
	if (opt_from && strcmp(opt_from, "core") && !foreign_source_known(opt_from))
		die("unknown source '%s' (see /etc/wh/sources)", opt_from);
	repo_load(&idx);
	for (int i = 0; i < idx.count && (!opt_from || !strcmp(opt_from, "core")); i++) {
		struct package *p = &idx.pkgs[i], cur;
		char mark[96] = "";

		if (!strcasestr(p->name, term) && !strcasestr(p->description, term))
			continue;
		if (db_installed(p->name, &cur))
			snprintf(mark, sizeof(mark), strcmp(cur.version, p->version) ?
				 " [installed %s]" : " [installed]", cur.version);
		printf("%s %s%s\n    %s\n", p->name, p->version, mark, p->description);
		found++;
	}
	if (!direct && !(opt_from && !strcmp(opt_from, "core")))
		found += foreign_search(term, opt_from);
	if (!found)
		printf("No packages match '%s'.\n", term);
	return found ? 0 : 1;
}

static int cmd_info(int argc, char **argv)
{
	struct package *p, cur;
	struct index idx;
	int installed;

	if (argc != 2)
		die("usage: wh info PACKAGE");
	repo_load(&idx);
	p = index_find(&idx, argv[1]);
	installed = db_installed(argv[1], &cur);
	if (!p && !installed)
		die("no package named '%s'", argv[1]);
	if (!p)
		p = &cur;
	printf("Name:          %s\n", p->name);
	printf("Version:       %s\n", p->version);
	printf("Description:   %s\n", p->description);
	printf("Depends on:    %s\n", p->depends[0] ? p->depends : "-");
	if (p->size)
		printf("Download size: %lld KiB\n", (p->size + 1023) / 1024);
	printf("Installed:     %s\n", installed ? cur.version : "no");
	return 0;
}

static int cmd_history(int argc, char **argv)
{
	struct record *list;
	int n;

	(void)argv;
	if (argc != 1)
		die("usage: wh history");
	n = history_read(&list);
	if (!n)
		printf("No package changes yet.\n");
	else
		printf("%4s  %-16s  %-8s  %-8s  %s\n", "ID", "DATE", "ACTION", "SNAPSHOT", "PACKAGES");
	for (int i = 0; i < n; i++) {
		char date[32], snap[16] = "-";
		time_t t = list[i].date;
		struct tm tm;

		localtime_r(&t, &tm);
		strftime(date, sizeof(date), "%Y-%m-%d %H:%M", &tm);
		if (list[i].snapshot)
			snprintf(snap, sizeof(snap), "%d", list[i].snapshot);
		printf("%4d  %-16s  %-8s  %-8s  %s\n", list[i].id, date, list[i].action,
		       snap, list[i].packages);
	}
	free(list);
	return 0;
}

static int cmd_undo(int argc, char **argv)
{
	struct record *list;
	char id[16], label[64];
	int n, target = -1;

	(void)argv;
	if (argc != 1)
		die("usage: wh undo");
	lock_wh();
	if (direct)
		die("undo works on the running system only");
	n = history_read(&list);
	/* The newest change that was not undone already. */
	for (int i = n - 1; i >= 0 && target < 0; i--) {
		int undone = 0;

		if (strcmp(list[i].action, "undo") == 0)
			continue;
		for (int j = i + 1; j < n; j++) {
			snprintf(label, sizeof(label), "#%d", list[i].id);
			undone |= strcmp(list[j].action, "undo") == 0 &&
				  strcmp(list[j].packages, label) == 0;
		}
		if (!undone)
			target = i;
	}
	if (target < 0)
		die("there is no package change to undo");
	if (!list[target].snapshot)
		die("change %d has no snapshot to go back to (automatic snapshots were off)",
		    list[target].id);

	info("Undoing change %d (%s %s): going back to snapshot %d.", list[target].id,
	     list[target].action, list[target].packages, list[target].snapshot);
	snprintf(label, sizeof(label), "#%d", list[target].id);
	history_add("undo", label, list[target].snapshot);
	snprintf(id, sizeof(id), "%d", list[target].snapshot);
	free(list);
	execvp("snap", (char *[]){ "snap", "rewind", id, "-y", NULL });
	die("cannot run snap: %s", strerror(errno));
}

static int cmd_keep(int argc, char **argv)
{
	(void)argv;
	if (argc != 1)
		die("usage: wh keep");
	lock_wh();
	if (run((char *[]){ "snap", "stage", "keep", NULL }, NULL, 0) != 0)
		return 1;
	info("The trial change is kept.");
	return 0;
}

int main(int argc, char **argv)
{
	static const struct option longopts[] = {
		{ "root",  required_argument, NULL, 'r' },
		{ "repo",  required_argument, NULL, 'R' },
		{ "key",   required_argument, NULL, 'k' },
		{ "try",   no_argument,       NULL, 't' },
		{ "later", no_argument,       NULL, 'l' },
		{ "yes",   no_argument,       NULL, 'y' },
		{ "force", no_argument,       NULL, 'f' },
		{ "from",  required_argument, NULL, 'F' },
		{ "help",  no_argument,       NULL, 'h' },
		{ NULL, 0, NULL, 0 },
	};
	const char *cmd;
	int c;

	while ((c = getopt_long(argc, argv, "yhv", longopts, NULL)) != -1) {
		switch (c) {
		case 'r': root = optarg; direct = 1; break;
		case 'R': repo_override = optarg; break;
		case 'k': key_file = optarg; break;
		case 't': opt_try = 1; break;
		case 'l': opt_later = 1; break;
		case 'y': opt_yes = 1; break;
		case 'f': opt_force = 1; break;
		case 'F': opt_from = optarg; break;
		case 'v': verbose = 1; break;
		case 'h': usage(stdout); return 0;
		default:  usage(stderr); return 2;
		}
	}
	argc -= optind;
	argv += optind;
	if (argc == 0) {
		usage(stderr);
		return 2;
	}
	if (opt_try && direct)
		die("--try needs the running system, not --root");
	umask(022);
	set_root(root);

	cmd = argv[0];
	/*
	 * On an edition that lets users install (WormOS Amnesia, which has no
	 * root login), package changes by a user go through the helper.
	 */
	if (geteuid() != 0 && !direct && access(WH_USERS_FLAG, F_OK) == 0 &&
	    (!strcmp(cmd, "install") || !strcmp(cmd, "reinstall") || !strcmp(cmd, "remove") ||
	     !strcmp(cmd, "update") || !strcmp(cmd, "upgrade"))) {
		char *args[64];
		int n = 0;

		args[n++] = "wh-user";
		if (opt_from) {
			args[n++] = "--from";
			args[n++] = (char *)opt_from;
		}
		for (int i = 0; i < argc && n < 63; i++)
			args[n++] = argv[i];
		args[n] = NULL;
		execv(WH_USER_HELPER, args);
		die("cannot run %s: %s", WH_USER_HELPER, strerror(errno));
	}
	live = !direct && !opt_try && access(WH_LIVE_FLAG, F_OK) == 0;
	if (live)
		opt_later = 0;
	if (!strcmp(cmd, "update"))             return cmd_update(argc, argv);
	if (!strcmp(cmd, "install"))            return cmd_install(argc, argv, 0);
	if (!strcmp(cmd, "reinstall"))          return cmd_install(argc, argv, 1);
	if (!strcmp(cmd, "remove"))             return cmd_remove(argc, argv);
	if (!strcmp(cmd, "upgrade"))            return cmd_upgrade(argc, argv);
	if (!strcmp(cmd, "list"))               return cmd_list(argc, argv);
	if (!strcmp(cmd, "search"))             return cmd_search(argc, argv);
	if (!strcmp(cmd, "info"))               return cmd_info(argc, argv);
	if (!strcmp(cmd, "history"))            return cmd_history(argc, argv);
	if (!strcmp(cmd, "undo"))               return cmd_undo(argc, argv);
	if (!strcmp(cmd, "keep"))               return cmd_keep(argc, argv);
	warn("unknown command '%s'", cmd);
	usage(stderr);
	return 2;
}
