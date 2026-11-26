/*
 * snap - WormOS snapshots.
 *
 * Snapshots live in the @snapshots subvolume, which the running system
 * never mounts. snap mounts the top level of the filesystem in a private
 * mount namespace of its own, so the store is only reachable while a snap
 * command runs and never appears in the system's mount table.
 *
 * Rewinds are not applied to the running system: snap records the request
 * and restarts userland (a soft reboot through worminit and wormrd), and
 * the rewind is applied (snap boot) before anything from the system root
 * runs again.
 */

#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <getopt.h>
#include <limits.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "snap.h"

#define RUNDIR   "/run/snap"
#define TOPMNT   RUNDIR "/top"
#define VIEWMNT  RUNDIR "/view"

const char *top;
int top_given;

static void usage(FILE *out)
{
	fputs(
"usage: snap COMMAND [ARGS]\n"
"\n"
"  now [-c COMMENT]         take a snapshot (--suspect: never used by safe mode)\n"
"  list [--raw]             list snapshots (--raw: tab-separated, for programs)\n"
"  rewind [ID] [-y]         go back to the last snapshot (or ID) and restart\n"
"  diff ID [ID2]            show what changed since ID, or between ID and ID2\n"
"  restore ID PATH          bring back one file or folder from ID\n"
"  browse ID                open a read-only shell inside a snapshot\n"
"  delete ID...             delete snapshots\n"
"  prune --keep N           delete all but the newest N unpinned snapshots\n"
"  pin ID / unpin ID        protect a snapshot from prune\n"
"  comment ID TEXT          set a snapshot's comment\n"
"  verify [ID...]           check snapshots have not been tampered with\n"
"  auto [on|off]            snapshots before package changes (default on)\n"
"  status                   a summary (also kept in /run/snap-status)\n"
"  stage begin|commit|abort|keep|status\n"
"                           staged system changes, used by wh\n"
"\n"
"Scope (now, rewind, diff, browse): --system (default), --home or --all.\n",
	      out);
}

static int parse_id_arg(const char *arg)
{
	char *end;
	long id = strtol(arg, &end, 10);

	if (*arg == '\0' || *end || id <= 0 || id > INT_MAX)
		die("not a snapshot ID: %s", arg);
	return (int)id;
}

static void format_date(time_t t, char *buf, size_t len)
{
	struct tm tm;

	localtime_r(&t, &tm);
	strftime(buf, len, "%Y-%m-%d %H:%M", &tm);
}

/*
 * Keep the filesystem-wide options (compression and so on) when mounting
 * the top level: if the filesystem is read-only, a read-write mount
 * reconfigures it with the options given, and would drop the others.
 */
static void top_options(const char *super, char *out, size_t len)
{
	char buf[1024], *tok, *save;
	size_t n = 0;

	snprintf(buf, sizeof(buf), "%s", super);
	out[0] = '\0';
	for (tok = strtok_r(buf, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
		if (!strcmp(tok, "rw") || !strcmp(tok, "ro") ||
		    !strncmp(tok, "subvol=", 7) || !strncmp(tok, "subvolid=", 9))
			continue;
		n += snprintf(out + n, len - n, "%s,", tok);
		if (n >= len)
			die("mount options too long");
	}
	snprintf(out + n, len - n, "subvolid=5");
}

/* Find the device holding the running root filesystem, and its options. */
static void root_device(char *dev, size_t len, char *opts, size_t optlen)
{
	char line[4096];
	FILE *f = fopen("/proc/self/mountinfo", "re");

	if (!f)
		die("cannot read /proc/self/mountinfo: %s", strerror(errno));
	while (fgets(line, sizeof(line), f)) {
		char mnt[PATH_MAX], type[64], src[PATH_MAX], super[1024], *sep;

		if (sscanf(line, "%*s %*s %*s %*s %4095s", mnt) != 1 ||
		    strcmp(mnt, "/") != 0 || !(sep = strstr(line, " - ")) ||
		    sscanf(sep + 3, "%63s %4095s %1023s", type, src, super) != 3)
			continue;
		fclose(f);
		if (strcmp(type, "btrfs") != 0)
			die("the root filesystem is not btrfs");
		snprintf(dev, len, "%s", src);
		top_options(super, opts, optlen);
		return;
	}
	fclose(f);
	die("cannot find the root filesystem");
}

/* Mount the top level of the filesystem where only this process sees it. */
static void open_store(void)
{
	char dev[PATH_MAX], opts[1024];

	if (top_given)
		return;
	if (geteuid() != 0)
		die("snapshots are managed by root, try again with su");

	root_device(dev, sizeof(dev), opts, sizeof(opts));
	mkdir(RUNDIR, 0700);
	mkdir(TOPMNT, 0700);
	if (unshare(CLONE_NEWNS) < 0 ||
	    mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) < 0)
		die("cannot create a private mount namespace: %s", strerror(errno));
	if (mount(dev, TOPMNT, "btrfs", MS_NOSUID | MS_NODEV | MS_NOEXEC, opts) < 0)
		die("cannot mount %s: %s", dev, strerror(errno));
	top = TOPMNT;
}

/* Parse --system/--home/--all and other options shared by commands. */
struct opts {
	int scope;
	int yes;
	int automatic;
	int keep;
	int trial;
	int suspect;
	const char *comment;
};

static int parse_opts(int argc, char **argv, struct opts *o, const char *allowed)
{
	static const struct option longopts[] = {
		{ "system",  no_argument,       NULL, 'S' },
		{ "home",    no_argument,       NULL, 'H' },
		{ "all",     no_argument,       NULL, 'A' },
		{ "yes",     no_argument,       NULL, 'y' },
		{ "comment", required_argument, NULL, 'c' },
		{ "auto",    no_argument,       NULL, 'a' },
		{ "keep",    required_argument, NULL, 'k' },
		{ "try",     no_argument,       NULL, 'T' },
		{ "suspect", no_argument,       NULL, 'u' },
		{ NULL, 0, NULL, 0 },
	};
	int c;

	memset(o, 0, sizeof(*o));
	optind = 1;
	while ((c = getopt_long(argc, argv, "c:y", longopts, NULL)) != -1) {
		if (c == '?' || !strchr(allowed, c))
			die("unsupported option for %s, see 'snap help'", argv[0]);
		switch (c) {
		case 'S': o->scope = SCOPE_SYSTEM; break;
		case 'H': o->scope = SCOPE_HOME; break;
		case 'A': o->scope = SCOPE_ALL; break;
		case 'y': o->yes = 1; break;
		case 'c': o->comment = optarg; break;
		case 'a': o->automatic = 1; break;
		case 'k': o->keep = parse_id_arg(optarg); break;
		case 'T': o->trial = 1; break;
		case 'u': o->suspect = 1; break;
		}
	}
	if (!o->scope)
		o->scope = SCOPE_SYSTEM;
	return optind;
}

static int confirm(const char *question)
{
	char answer[16];

	printf("%s [y/N] ", question);
	fflush(stdout);
	if (!isatty(STDIN_FILENO)) {
		printf("\n");
		die("not a terminal, pass -y to confirm");
	}
	if (!fgets(answer, sizeof(answer), stdin))
		return 0;
	return answer[0] == 'y' || answer[0] == 'Y';
}

static int cmd_now(int argc, char **argv)
{
	struct opts o;
	int id;

	if (parse_opts(argc, argv, &o, "SHAcau") != argc)
		die("usage: snap now [-c COMMENT] [--system|--home|--all] [--suspect]");
	open_store();
	store_lock();
	if (o.automatic && !config_auto())
		return 0;
	id = store_create(o.scope, o.automatic ? "auto" : "manual", o.comment);
	if (id < 0)
		return 1;
	if (o.suspect && store_mark_suspect(id) < 0)
		warn("cannot mark snapshot %d as suspect", id);
	printf("Snapshot %d created (%s)\n", id, scope_name(o.scope));
	return 0;
}

/*
 * For programs (the Snapshots application): one line per snapshot, fields
 * separated by tabs: ID, date (Unix time), scope, type, pinned (0 or 1),
 * suspect (0 or 1), comment. Tabs and line breaks in comments become
 * spaces.
 */
static int list_raw(void)
{
	struct snapshot *list;
	int n;

	open_store();
	n = store_list(&list);
	for (int i = 0; i < n; i++) {
		for (char *c = list[i].comment; *c; c++)
			if (*c == '\t' || *c == '\n' || *c == '\r')
				*c = ' ';
		printf("%d\t%lld\t%s\t%s\t%d\t%d\t%s\n", list[i].id, (long long)list[i].date,
		       scope_name(list[i].scope), list[i].type, list[i].pinned != 0,
		       list[i].suspect != 0, list[i].comment);
	}
	free(list);
	return 0;
}

static int cmd_list(int argc, char **argv)
{
	struct snapshot *list;
	char date[32];
	int n, pid, pscope;

	if (argc == 2 && strcmp(argv[1], "--raw") == 0)
		return list_raw();
	if (argc != 1)
		die("usage: snap list [--raw]");
	open_store();
	n = store_list(&list);
	if (n == 0) {
		printf("No snapshots yet. Take one with: snap now\n");
		free(list);
		return 0;
	}
	printf("%4s  %-16s  %-6s  %-6s  %s\n", "ID", "DATE", "SCOPE", "TYPE", "COMMENT");
	for (int i = 0; i < n; i++) {
		format_date(list[i].date, date, sizeof(date));
		printf("%4d%c %-16s  %-6s  %-6s  %s\n", list[i].id,
		       list[i].pinned ? '*' : ' ', date, scope_name(list[i].scope),
		       list[i].type, list[i].comment);
	}
	if (pending_read(&pid, &pscope) > 0)
		printf("\nRewind of %s to snapshot %d will happen at the next boot.\n",
		       scope_desc(pscope), pid);
	if (stage_pending())
		printf("\nA package change will be applied at the next restart.\n");
	if ((pid = stage_trial()))
		printf("\nA trial is running: the next restart returns to snapshot %d"
		       " unless it is kept (wh keep).\n", pid);
	free(list);
	return 0;
}

static int cmd_rewind(int argc, char **argv)
{
	struct snapshot s, *list;
	char question[COMMENT_MAX + 256], date[32];
	struct opts o;
	int i, id = 0, n;

	i = parse_opts(argc, argv, &o, "SHAy");
	if (argc - i > 1)
		die("usage: snap rewind [ID] [--system|--home|--all] [-y]");
	if (argc - i == 1)
		id = parse_id_arg(argv[i]);

	open_store();
	store_lock();
	if (id) {
		if (store_get(id, &s) < 0)
			die("no snapshot %d", id);
	} else {
		n = store_list(&list);
		while (n > 0 && (list[n - 1].scope & o.scope) != o.scope)
			n--;
		if (n == 0)
			die("no %s snapshot to rewind to", scope_name(o.scope));
		s = list[n - 1];
		free(list);
	}
	if ((s.scope & o.scope) != o.scope)
		die("snapshot %d does not include %s (it is a %s snapshot)",
		    s.id, scope_name(o.scope), scope_name(s.scope));

	if (store_verify(&s, o.scope) < 0)
		die("snapshot %d failed verification, not rewinding to it", s.id);

	format_date(s.date, date, sizeof(date));
	printf("Rewind %s to snapshot %d (%s%s%s%s).\n",
	       scope_desc(o.scope),
	       s.id, date, s.comment[0] ? ", \"" : "", s.comment,
	       s.comment[0] ? "\"" : "");
	printf("The current state is saved as a new snapshot first.\n");
	snprintf(question, sizeof(question), "%s",
		 top_given ? "Continue?" : "The system restarts now. Continue?");
	if (!o.yes && !confirm(question)) {
		printf("Cancelled.\n");
		return 1;
	}

	if (stage_pending()) {
		printf("The package change waiting to be applied is cancelled.\n");
		stage_discard();
	}
	if (pending_write(s.id, o.scope) < 0)
		die("cannot record the rewind: %s", strerror(errno));
	sync();
	if (top_given) {
		printf("Rewind recorded.\n");
		return 0;
	}
	/* worminit soft-reboots on SIGHUP: userland restarts, the kernel stays. */
	printf("Restarting...\n");
	fflush(stdout);
	if (kill(1, SIGHUP) < 0)
		die("cannot restart: %s", strerror(errno));
	return 0;
}

static int cmd_diff(int argc, char **argv)
{
	char a[PATH_MAX], b[PATH_MAX];
	struct snapshot s;
	struct opts o;
	int i, id1, id2 = 0;

	i = parse_opts(argc, argv, &o, "SH");
	if (argc - i < 1 || argc - i > 2)
		die("usage: snap diff ID [ID2] [--system|--home]");
	id1 = parse_id_arg(argv[i]);
	if (argc - i == 2)
		id2 = parse_id_arg(argv[i + 1]);

	open_store();
	if (store_get(id1, &s) < 0)
		die("no snapshot %d", id1);
	if (!(s.scope & o.scope))
		die("snapshot %d does not include %s", id1, scope_path(o.scope));
	store_entry_path(a, sizeof(a), id1, o.scope);
	if (id2) {
		if (store_get(id2, &s) < 0)
			die("no snapshot %d", id2);
		if (!(s.scope & o.scope))
			die("snapshot %d does not include %s", id2, scope_path(o.scope));
		store_entry_path(b, sizeof(b), id2, o.scope);
	} else {
		path_join(b, sizeof(b), top, scope_subvol(o.scope));
	}

	if (diff_trees(a, b, o.scope == SCOPE_HOME ? "/home" : "") == 0)
		printf("No changes.\n");
	return 0;
}

static int cmd_restore(int argc, char **argv)
{
	if (argc != 3)
		die("usage: snap restore ID PATH");
	open_store();
	store_lock();
	return restore_path(parse_id_arg(argv[1]), argv[2]);
}

static int cmd_browse(int argc, char **argv)
{
	char src[PATH_MAX], prompt[64];
	const char *shell = getenv("SHELL");
	struct snapshot s;
	struct opts o;
	int i, id;

	i = parse_opts(argc, argv, &o, "SH");
	if (argc - i != 1)
		die("usage: snap browse ID [--system|--home]");
	id = parse_id_arg(argv[i]);
	if (top_given)
		die("browse is not available with --top");

	open_store();
	if (store_get(id, &s) < 0)
		die("no snapshot %d", id);
	if (!(s.scope & o.scope))
		o.scope = s.scope;
	store_entry_path(src, sizeof(src), id, o.scope);

	/* Expose only this snapshot, read-only, then drop the store mount. */
	mkdir(VIEWMNT, 0700);
	if (mount(src, VIEWMNT, NULL, MS_BIND, NULL) < 0 ||
	    mount(NULL, VIEWMNT, NULL, MS_BIND | MS_REMOUNT | MS_RDONLY |
		  MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL) < 0)
		die("cannot open snapshot %d: %s", id, strerror(errno));
	if (umount2(TOPMNT, MNT_DETACH) < 0)
		die("cannot release the snapshot store: %s", strerror(errno));
	if (chdir(VIEWMNT) < 0)
		die("cannot enter snapshot %d: %s", id, strerror(errno));

	printf("Browsing %s snapshot %d (read-only) in %s. Exit the shell to leave.\n",
	       scope_name(o.scope), id, VIEWMNT);
	snprintf(prompt, sizeof(prompt), "[snap %d browse] \\w# ", id);
	setenv("PS1", prompt, 1);
	if (!shell || !*shell)
		shell = "/bin/sh";
	execl(shell, shell, (char *)NULL);
	die("cannot start %s: %s", shell, strerror(errno));
}

static int cmd_delete(int argc, char **argv)
{
	struct snapshot s;
	int pid, pscope, ret = 0;

	if (argc < 2)
		die("usage: snap delete ID...");
	for (int i = 1; i < argc; i++)
		parse_id_arg(argv[i]);
	open_store();
	store_lock();
	for (int i = 1; i < argc; i++) {
		int id = parse_id_arg(argv[i]);

		if (store_get(id, &s) < 0) {
			warn("no snapshot %d", id);
			ret = 1;
		} else if (pending_read(&pid, &pscope) > 0 && pid == id) {
			warn("snapshot %d has a pending rewind, not deleted", id);
			ret = 1;
		} else if (store_delete(id) < 0) {
			ret = 1;
		} else {
			printf("Deleted snapshot %d\n", id);
		}
	}
	return ret;
}

static int cmd_prune(int argc, char **argv)
{
	struct snapshot *list;
	struct opts o;
	int n, unpinned = 0, deleted = 0, pid = 0, pscope;

	if (parse_opts(argc, argv, &o, "k") != argc || !o.keep)
		die("usage: snap prune --keep N");
	open_store();
	store_lock();
	n = store_list(&list);
	for (int i = 0; i < n; i++)
		unpinned += !list[i].pinned;
	if (pending_read(&pid, &pscope) <= 0)
		pid = 0;
	for (int i = 0; i < n && unpinned > o.keep; i++) {
		if (list[i].pinned || list[i].id == pid)
			continue;
		if (store_delete(list[i].id) == 0)
			deleted++;
		unpinned--;
	}
	printf("Deleted %d snapshot%s\n", deleted, deleted == 1 ? "" : "s");
	free(list);
	return 0;
}

static int cmd_pin(int argc, char **argv)
{
	struct snapshot s;
	int pin = strcmp(argv[0], "pin") == 0;

	if (argc != 2)
		die("usage: snap %s ID", argv[0]);
	open_store();
	store_lock();
	if (store_get(parse_id_arg(argv[1]), &s) < 0)
		die("no snapshot %s", argv[1]);
	s.pinned = pin;
	if (store_write_info(&s) < 0)
		die("cannot update snapshot %d: %s", s.id, strerror(errno));
	printf("Snapshot %d %s\n", s.id, pin ? "pinned" : "unpinned");
	return 0;
}

static int cmd_comment(int argc, char **argv)
{
	struct snapshot s;

	if (argc != 3)
		die("usage: snap comment ID TEXT");
	open_store();
	store_lock();
	if (store_get(parse_id_arg(argv[1]), &s) < 0)
		die("no snapshot %s", argv[1]);
	memset(s.comment, 0, sizeof(s.comment));
	for (size_t i = 0; argv[2][i] && i < sizeof(s.comment) - 1; i++)
		s.comment[i] = iscntrl((unsigned char)argv[2][i]) ? ' ' : argv[2][i];
	if (store_write_info(&s) < 0)
		die("cannot update snapshot %d: %s", s.id, strerror(errno));
	return 0;
}

static int cmd_verify(int argc, char **argv)
{
	struct snapshot s, *list;
	int n, ret = 0;

	for (int i = 1; i < argc; i++)
		parse_id_arg(argv[i]);
	open_store();
	if (argc > 1) {
		for (int i = 1; i < argc; i++) {
			if (store_get(parse_id_arg(argv[i]), &s) < 0) {
				warn("no snapshot %s", argv[i]);
				ret = 1;
			} else if (store_verify(&s, SCOPE_ALL) < 0) {
				ret = 1;
			}
		}
		return ret;
	}

	n = store_list(&list);
	if (n == 0)
		printf("No snapshots to verify.\n");
	for (int i = 0; i < n; i++)
		if (store_verify(&list[i], SCOPE_ALL) < 0)
			ret = 1;
	free(list);
	return ret;
}

static int cmd_auto(int argc, char **argv)
{
	if (argc == 1) {
		open_store();
		printf("Automatic snapshots are %s\n", config_auto() ? "on" : "off");
		return 0;
	}
	if (argc == 2 && (!strcmp(argv[1], "on") || !strcmp(argv[1], "off"))) {
		open_store();
		store_lock();
		config_set_auto(strcmp(argv[1], "on") == 0);
		status_update();
		printf("Automatic snapshots turned %s\n", argv[1]);
		return 0;
	}
	if (argc >= 2 && strcmp(argv[1], "timer") == 0)
		die("timed snapshots are not available yet");
	die("usage: snap auto [on|off]");
}

static int cmd_stage(int argc, char **argv)
{
	const char *sub = argc > 1 ? argv[1] : "";
	struct opts o;
	int id;

	open_store();
	store_lock();
	if (strcmp(sub, "begin") == 0 && argc == 2)
		return stage_begin();
	if (strcmp(sub, "abort") == 0 && argc == 2) {
		stage_discard();
		return 0;
	}
	if (strcmp(sub, "status") == 0 && argc == 2) {
		/* For scripts: "pending" and/or "trial ID", or "none". */
		int trial = stage_trial();

		if (stage_pending())
			printf("pending\n");
		if (trial)
			printf("trial %d\n", trial);
		if (!trial && !stage_pending())
			printf("none\n");
		return 0;
	}
	if (strcmp(sub, "keep") == 0 && argc == 2) {
		if (stage_keep() < 0)
			die("no trial is running");
		return 0;
	}
	if (strcmp(sub, "commit") == 0) {
		/* Prints the ID of the "before" snapshot (0 if none). */
		if (parse_opts(argc - 1, argv + 1, &o, "Tc") != argc - 1)
			die("usage: snap stage commit [--try] [-c COMMENT]");
		id = stage_commit(o.trial, o.comment);
		printf("%d\n", id);
		return 0;
	}
	die("usage: snap stage begin | commit [--try] [-c COMMENT] | abort | keep | status");
}

static int cmd_status(int argc, char **argv)
{
	(void)argv;
	if (argc != 1)
		die("usage: snap status");
	open_store();
	status_print();
	status_update();
	return 0;
}

static int cmd_boot(int argc, char **argv)
{
	int rewind = 0;

	if (!top_given)
		die("boot is run by the initramfs");
	if (argc == 3 && strcmp(argv[1], "--rewind") == 0)
		rewind = parse_id_arg(argv[2]);
	else if (argc != 1)
		die("usage: snap --top DIR boot [--rewind ID]");
	return boot_tasks(rewind);
}

static const struct command {
	const char *name;
	int (*fn)(int argc, char **argv);
} commands[] = {
	{ "now",     cmd_now },
	{ "list",    cmd_list },
	{ "ls",      cmd_list },
	{ "rewind",  cmd_rewind },
	{ "diff",    cmd_diff },
	{ "restore", cmd_restore },
	{ "browse",  cmd_browse },
	{ "delete",  cmd_delete },
	{ "rm",      cmd_delete },
	{ "prune",   cmd_prune },
	{ "pin",     cmd_pin },
	{ "unpin",   cmd_pin },
	{ "comment", cmd_comment },
	{ "verify",  cmd_verify },
	{ "auto",    cmd_auto },
	{ "stage",   cmd_stage },
	{ "status",  cmd_status },
	{ "boot",    cmd_boot },
	{ NULL, NULL },
};

int main(int argc, char **argv)
{
	argv++;
	argc--;

	/* Files made as root never inherit the caller's umask (setuid helpers). */
	umask(022);

	/* --top DIR: work on an already mounted top level (initramfs, tests). */
	if (argc >= 2 && strcmp(argv[0], "--top") == 0) {
		top = argv[1];
		top_given = 1;
		argv += 2;
		argc -= 2;
	}
	if (argc == 0 || !strcmp(argv[0], "help") || !strcmp(argv[0], "--help") ||
	    !strcmp(argv[0], "-h")) {
		usage(argc == 0 ? stderr : stdout);
		return argc == 0 ? 2 : 0;
	}

	for (const struct command *c = commands; c->name; c++)
		if (strcmp(argv[0], c->name) == 0)
			return c->fn(argc, argv);
	warn("unknown command '%s'", argv[0]);
	usage(stderr);
	return 2;
}
