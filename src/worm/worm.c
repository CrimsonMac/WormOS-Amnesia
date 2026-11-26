/*
 * worm - WormOS system features.
 *
 *     worm unlock             make the system writable for manual changes
 *     worm lock [-c TEXT]     make it read-only again and snapshot the changes
 *     worm fw ...             the firewall (see fw.c)
 *     worm status             the state of the system's protections
 *     worm panic [off]        cut the network, stop blackholes, snapshot
 *     worm anon on|off|status all traffic through Tor (see anon.c)
 *     worm anon new-circuits  fresh Tor circuits for new connections
 *
 * Only the mount of / changes (a bind remount), never the filesystem, so
 * /var and /home are not affected. Unlocking is a guard against accidents,
 * not a security boundary: root can always unlock.
 */

#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/statvfs.h>
#include <sys/wait.h>
#include <unistd.h>

#include "worm.h"

void die(const char *fmt, const char *arg)
{
	fprintf(stderr, "worm: ");
	fprintf(stderr, fmt, arg);
	fputc('\n', stderr);
	exit(1);
}

void warn(const char *fmt, const char *arg)
{
	fprintf(stderr, "worm: ");
	fprintf(stderr, fmt, arg);
	fputc('\n', stderr);
}

int safe_mode(void)
{
	char line[4096], *tok, *save;
	FILE *f = fopen("/proc/cmdline", "re");
	int safe = 0;

	if (!f)
		return 0;
	if (fgets(line, sizeof(line), f))
		for (tok = strtok_r(line, " \n", &save); tok; tok = strtok_r(NULL, " \n", &save))
			safe |= strcmp(tok, "wormos.safe") == 0;
	fclose(f);
	return safe;
}

/* Is this system the WormOS edition id (VARIANT_ID in /etc/worm/variant)? */
int variant_is(const char *id)
{
	char line[256], want[128];
	FILE *f = fopen("/etc/worm/variant", "re");
	int found = 0;

	if (!f)
		return 0;
	snprintf(want, sizeof(want), "VARIANT_ID=%s\n", id);
	while (!found && fgets(line, sizeof(line), f))
		found = strcmp(line, want) == 0;
	fclose(f);
	return found;
}

int run(char *const argv[], char *out, size_t len, int quiet)
{
	int pipefd[2], status;
	size_t n = 0;
	pid_t pid;

	if (out && pipe(pipefd) < 0)
		return -1;
	fflush(stdout);
	if ((pid = fork()) < 0)
		return -1;
	if (pid == 0) {
		if (out) {
			dup2(pipefd[1], STDOUT_FILENO);
			close(pipefd[0]);
			close(pipefd[1]);
		}
		if (quiet) {
			int null = open("/dev/null", O_WRONLY);

			if (null >= 0)
				dup2(null, STDERR_FILENO);
		}
		execvp(argv[0], argv);
		_exit(127);
	}
	if (out) {
		char discard[512];
		ssize_t r;

		close(pipefd[1]);
		while (n < len - 1 && (r = read(pipefd[0], out + n, len - 1 - n)) > 0)
			n += r;
		out[n] = '\0';
		/* Drain the rest, or the command may die of SIGPIPE. */
		while (read(pipefd[0], discard, sizeof(discard)) > 0)
			;
		close(pipefd[0]);
	}
	if (waitpid(pid, &status, 0) < 0)
		return -1;
	return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

/* The current per-mount flags of /, as mount(2) flags. */
static unsigned long root_flags(int *readonly)
{
	static const struct { unsigned long st, ms; } map[] = {
		{ ST_NOSUID, MS_NOSUID }, { ST_NODEV, MS_NODEV },
		{ ST_NOEXEC, MS_NOEXEC }, { ST_NOATIME, MS_NOATIME },
		{ ST_NODIRATIME, MS_NODIRATIME }, { ST_RELATIME, MS_RELATIME },
	};
	struct statvfs st;
	unsigned long flags = 0;

	if (statvfs("/", &st) < 0)
		die("cannot read the flags of /: %s", strerror(errno));
	for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++)
		if (st.f_flag & map[i].st)
			flags |= map[i].ms;
	*readonly = (st.f_flag & ST_RDONLY) != 0;
	return flags;
}

/* The mount ID of /, from mountinfo. */
static int root_mount_id(void)
{
	char line[4096], mnt[PATH_MAX];
	FILE *f = fopen("/proc/self/mountinfo", "re");
	int id, found = -1;

	if (!f)
		return -1;
	while (found < 0 && fgets(line, sizeof(line), f))
		if (sscanf(line, "%d %*s %*s %*s %4095s", &id, mnt) == 2 &&
		    strcmp(mnt, "/") == 0)
			found = id;
	fclose(f);
	return found;
}

/* List processes holding a file on / open for writing. */
static void show_writers(void)
{
	int root_id = root_mount_id(), shown = 0;
	struct dirent *p;
	DIR *proc = opendir("/proc");

	while (proc && (p = readdir(proc))) {
		char dir[300], path[PATH_MAX], target[PATH_MAX], comm[64] = "?";
		struct dirent *fd;
		DIR *fds;
		FILE *f;

		if (p->d_name[0] < '0' || p->d_name[0] > '9')
			continue;
		snprintf(dir, sizeof(dir), "/proc/%s/fd", p->d_name);
		if (!(fds = opendir(dir)))
			continue;
		snprintf(path, sizeof(path), "/proc/%s/comm", p->d_name);
		if ((f = fopen(path, "re"))) {
			if (fgets(comm, sizeof(comm), f))
				comm[strcspn(comm, "\n")] = '\0';
			fclose(f);
		}
		while ((fd = readdir(fds))) {
			char line[256];
			int flags = -1, mnt = -1;
			ssize_t n;

			if (fd->d_name[0] == '.')
				continue;
			snprintf(path, sizeof(path), "/proc/%s/fdinfo/%s", p->d_name, fd->d_name);
			if (!(f = fopen(path, "re")))
				continue;
			while (fgets(line, sizeof(line), f)) {
				sscanf(line, "flags: %o", (unsigned *)&flags);
				sscanf(line, "mnt_id: %d", &mnt);
			}
			fclose(f);
			if (mnt != root_id || flags < 0 || (flags & O_ACCMODE) == O_RDONLY)
				continue;
			snprintf(path, sizeof(path), "%s/%s", dir, fd->d_name);
			n = readlink(path, target, sizeof(target) - 1);
			target[n > 0 ? n : 0] = '\0';
			printf("  %s (pid %s): %s\n", comm, p->d_name, target);
			shown++;
		}
		closedir(fds);
	}
	if (proc)
		closedir(proc);
	if (!shown)
		printf("  (none found; a program may have a file mapped for writing)\n");
}

/* Changes to / made now would be lost when a staged package change applies. */
static void refuse_if_pending(void)
{
	char status[128] = "";

	if (run((char *[]){ "snap", "stage", "status", NULL }, status, sizeof(status), 0) == 0 &&
	    strstr(status, "pending"))
		die("a package change is waiting to be applied; restart first%s", "");
}

int root_begin_change(void)
{
	int ro;
	unsigned long flags = root_flags(&ro);

	refuse_if_pending();
	if (!ro)
		return 0;
	if (mount(NULL, "/", NULL, MS_REMOUNT | MS_BIND | flags, NULL) < 0)
		die("cannot make / writable: %s", strerror(errno));
	return 1;
}

void root_end_change(int relock)
{
	int ro;
	unsigned long flags = root_flags(&ro);

	if (!relock || ro)
		return;
	sync();
	if (mount(NULL, "/", NULL, MS_REMOUNT | MS_BIND | MS_RDONLY | flags, NULL) < 0)
		fprintf(stderr, "worm: / stays writable (%s); run 'worm lock'\n",
			strerror(errno));
}

static int cmd_unlock(void)
{
	int ro;
	unsigned long flags = root_flags(&ro);

	if (!ro) {
		printf("The system is already unlocked.\n");
		return 0;
	}
	refuse_if_pending();
	if (mount(NULL, "/", NULL, MS_REMOUNT | MS_BIND | flags, NULL) < 0)
		die("cannot unlock: %s", strerror(errno));
	printf("The system is unlocked: / is writable.\n"
	       "Run 'worm lock' when you are done; your changes are then snapshotted.\n");
	return 0;
}

static int cmd_lock(const char *comment)
{
	char out[256];
	int ro;
	unsigned long flags = root_flags(&ro);

	if (ro) {
		printf("The system is already locked.\n");
		return 0;
	}
	sync();
	if (mount(NULL, "/", NULL, MS_REMOUNT | MS_BIND | MS_RDONLY | flags, NULL) < 0) {
		if (errno != EBUSY)
			die("cannot lock: %s", strerror(errno));
		printf("Cannot lock yet: these programs have files open for writing:\n");
		show_writers();
		printf("Close them and run 'worm lock' again.\n");
		return 1;
	}
	printf("The system is locked.\n");
	if (run((char *[]){ "snap", "now", "--auto", "-c", (char *)comment, NULL },
		out, sizeof(out), 0) == 0 && out[0])
		printf("%s", out);
	return 0;
}

/*
 * Commands a user may run through the settings helper (setuid, for the
 * group wheel): looking at the protections always, and on a system without
 * root (/etc/worm/no-root, WormOS Amnesia) the safe changes nobody else
 * could make: bridges, a clock check, panic. Returns only when the command
 * is not one of them.
 */
static void user_action(int argc, char **argv)
{
	const char *helper = "/usr/lib/worm/settings";
	int no_root = access("/etc/worm/no-root", F_OK) == 0;
	char *args[4] = { (char *)helper, NULL, NULL, NULL };

	if (argc == 2 && !strcmp(argv[1], "status"))
		args[1] = "status";
	else if (argc == 3 && !strcmp(argv[1], "fw") && !strcmp(argv[2], "status"))
		args[1] = "firewall";
	else if (argc == 3 && !strcmp(argv[1], "anon") && !strcmp(argv[2], "new-circuits"))
		args[1] = "new-circuits";
	else if (no_root && argc == 4 && !strcmp(argv[1], "anon") && !strcmp(argv[2], "bridges"))
		args[1] = "bridges", args[2] = argv[3];
	else if (no_root && argc == 3 && !strcmp(argv[1], "time") && !strcmp(argv[2], "sync"))
		args[1] = "time-sync";
	else if (no_root && argc == 2 && !strcmp(argv[1], "panic"))
		args[1] = "panic";
	else if (no_root && argc == 3 && !strcmp(argv[1], "panic") && !strcmp(argv[2], "off"))
		args[1] = "panic-off";
	else
		return;
	if (access(helper, X_OK) != 0)
		return;
	fflush(stdout);
	execv(helper, args);
}

/* worm hostname [NAME]: show or set the machine's name (/etc/hostname). */
static int cmd_hostname(const char *name)
{
	char current[256] = "";
	int relock;
	FILE *f;

	if (!name) {
		gethostname(current, sizeof(current) - 1);
		puts(current);
		return 0;
	}
	if (!*name || strlen(name) > 63 || name[0] == '-' ||
	    strspn(name, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-")
	    != strlen(name))
		die("a host name is 1 to 63 letters, digits and dashes: '%s'", name);
	relock = root_begin_change();
	f = fopen("/etc/hostname.new", "we");
	if (!f || fprintf(f, "%s\n", name) < 0 || fclose(f) != 0 ||
	    rename("/etc/hostname.new", "/etc/hostname") < 0) {
		int err = errno;

		root_end_change(relock);
		die("cannot write /etc/hostname: %s", strerror(err));
	}
	root_end_change(relock);
	if (sethostname(name, strlen(name)) < 0)
		die("cannot set the host name: %s", strerror(errno));
	printf("Host name: %s\n", name);
	return 0;
}

static void usage(FILE *out)
{
	fputs("usage: worm help [COMMAND] | status | panic [off] | anon on|off|status|bridges KIND|new-circuits\n"
	      "       worm unlock | lock [-c COMMENT]\n"
	      "       worm kit [list] | show NAME | install NAME...|all | remove NAME...|all\n"
	      "       worm time [zones [TEXT] | zone NAME | sync]\n"
	      "       worm passwd           change your own password\n"
	      "       worm vm [setup | set KEY VALUE | reset [KEY] | shares [NAME...]]\n"
	      "       worm hostname [NAME]\n"
	      "       worm fw status | allow PORT[/tcp|/udp] | deny PORT[/tcp|/udp] | load\n"
	      "PORT is a number or a range such as 6000-6010; tcp is the default.\n", out);
}

/* worm help: every command that is WormOS's own, in one place. */
static void overview(void)
{
	int tty = isatty(STDOUT_FILENO);
	const char *b = tty ? "\033[1m" : "", *m = tty ? "\033[35m" : "",
		   *r = tty ? "\033[0m" : "";

	printf("%sWormOS commands%s  (worm help COMMAND shows the details of one)\n\n", b, r);
	printf("%sSnapshots%s (root)\n"
	       "  snap now [-c NOTE]       take a snapshot of the system\n"
	       "  snap list                list snapshots\n"
	       "  snap rewind [ID]         go back to the last snapshot (or ID)\n"
	       "  snap diff ID             what changed since a snapshot\n"
	       "  snap restore ID PATH     bring back one file or folder\n"
	       "  snap browse ID           look around inside a snapshot\n"
	       "  snap pin ID / prune      keep one / delete old ones\n"
	       "  wormos-snapshots         the same in a window\n\n", m, r);
	printf("%sSoftware%s (root)\n"
	       "  wh install PKG           install software (core, then Debian, then Kali)\n"
	       "  wh install --from debian PKG   from one source\n"
	       "  wh remove PKG            remove it\n"
	       "  wh search TERM           find packages\n"
	       "  wh upgrade               upgrade everything\n"
	       "  wh undo                  undo the last package change\n"
	       "  wh install --try PKG     try it; the next restart goes back unless 'wh keep'\n\n",
	       m, r);
	printf("%sProtection%s (root)\n"
	       "  worm status              protections at a glance\n"
	       "  worm fw status           the firewall (everything inbound is blocked)\n"
	       "  worm fw allow 22         open a port; worm fw deny 22 closes it\n"
	       "  worm anon on|off         send all traffic through Tor\n"
	       "  worm anon new-circuits   new connections get new Tor circuits (no password)\n"
	       "  worm anon bridges snowflake  reach Tor where it is blocked (obfs4, meek,\n"
	       "                           custom: lines from bridges.torproject.org; off)\n"
	       "  worm unlock / lock       make / writable for a moment, then read-only again\n"
	       "  worm panic [off]         cut the network, close sandboxes, take a snapshot\n\n",
	       m, r);
	printf("%sTool kits%s\n"
	       "  worm kit list            forensics, network, reverse engineering, web...\n"
	       "  worm kit show NAME       what a kit contains\n"
	       "  worm kit install NAME... install their tools (root); all: every kit;\n"
	       "                           basic: everyday tools\n\n", m, r);
	printf("%sTime%s\n"
	       "  worm time                the time, the time zone, the last clock check\n"
	       "  worm time zone NAME      set the time zone (root; worm time zones lists them)\n\n",
	       m, r);
	printf("%sVirtual machines%s\n"
	       "  worm vm                  the hypervisor found, its settings and tips\n"
	       "  worm vm set KEY VALUE    change one (root), e.g. worm vm set cursor software\n"
	       "  worm vm shares           mount the host's shared folders under /media (root)\n\n",
	       m, r);
	printf("%sSandboxes%s\n"
	       "  bh open [NAME]           a disposable sandbox (blackhole): no network,\n"
	       "                           no access to your files or the system;\n"
	       "                           again in another terminal: a second session\n"
	       "  bh open --net            with a network (not your local one)\n"
	       "  bh open --vm             a virtual machine with its own kernel\n"
	       "  bh put NAME FILE...      copy files into it\n"
	       "  bh list / bh close NAME  list / throw away\n\n", m, r);
	printf("%sDesktop and terminal%s\n"
	       "  tor-browser [URL]        Tor Browser\n"
	       "  clip copy / clip paste   the clipboard from the terminal (ls | clip copy)\n"
	       "  welcome [--full]         the greeting of new terminals / the whole guide\n"
	       "  worm passwd              change your password (also: passwd)\n"
	       "  su -                     become root for administration\n", m, r);
}

int main(int argc, char **argv)
{
	if (argc < 2) {
		usage(stderr);
		return 2;
	}
	if (!strcmp(argv[1], "help") && argc == 2) {
		overview();
		return 0;
	}
	if (!strcmp(argv[1], "help") && argc == 3) {
		static const char *const known[] = { "snap", "wh", "worm", "bh", "clip",
						     "tor-browser", NULL };

		for (int i = 0; known[i]; i++) {
			if (!strcmp(argv[2], known[i])) {
				if (!strcmp(argv[2], "worm")) {
					usage(stdout);
					return 0;
				}
				execlp(argv[2], argv[2], "--help", (char *)NULL);
				die("cannot run %s", argv[2]);
			}
		}
		die("no help for %s; worm help lists the commands", argv[2]);
	}
	if (!strcmp(argv[1], "--help")) {
		usage(stdout);
		return 0;
	}
	/* What a user may do through the settings helper (user_action). */
	if (geteuid() != 0)
		user_action(argc, argv);
	/* Changing one's own password works for everyone. */
	if (!strcmp(argv[1], "passwd") && argc == 2)
		return cmd_passwd();
	/* Showing the time and zones works for everyone. */
	if (!strcmp(argv[1], "time")) {
		int r = cmd_time(argc - 2, argv + 2);

		if (r == 2)
			usage(stderr);
		return r;
	}
	/* Listing and showing kits works for everyone. */
	if (!strcmp(argv[1], "kit")) {
		int r = cmd_kit(argc - 2, argv + 2);

		if (r == 2)
			usage(stderr);
		return r;
	}
	/* And looking at the virtual machine and the host name. */
	if (!strcmp(argv[1], "vm") && argc == 2)
		return cmd_vm(0, argv + 2);
	if (!strcmp(argv[1], "hostname") && argc == 2)
		return cmd_hostname(NULL);
	/* So does asking whether anonymous mode is on. */
	if (!strcmp(argv[1], "anon") && argc == 3 && !strcmp(argv[2], "status"))
		return cmd_anon(1, argv + 2);
	if (geteuid() != 0)
		die("this needs root, try again with su%s", "");
	if (!strcmp(argv[1], "fw")) {
		int r = cmd_fw(argc - 2, argv + 2);

		if (r == 2)
			usage(stderr);
		return r;
	}
	if (!strcmp(argv[1], "status") && argc == 2)
		return cmd_status();
	if (!strcmp(argv[1], "anon")) {
		int r = cmd_anon(argc - 2, argv + 2);

		if (r == 2)
			usage(stderr);
		return r;
	}
	if (!strcmp(argv[1], "panic")) {
		int r = cmd_panic(argc - 2, argv + 2);

		if (r == 2)
			usage(stderr);
		return r;
	}
	if (!strcmp(argv[1], "vm")) {
		int r = cmd_vm(argc - 2, argv + 2);

		if (r == 2)
			usage(stderr);
		return r;
	}
	if (!strcmp(argv[1], "hostname") && argc == 3)
		return cmd_hostname(argv[2]);
	if (!strcmp(argv[1], "passwd-apply") && argc == 3)
		return cmd_passwd_apply(argv[2]);
	if (!strcmp(argv[1], "unlock") && argc == 2)
		return cmd_unlock();
	if (!strcmp(argv[1], "lock") && argc == 2)
		return cmd_lock("Manual changes");
	if (!strcmp(argv[1], "lock") && argc == 4 && !strcmp(argv[2], "-c"))
		return cmd_lock(argv[3]);
	usage(stderr);
	return 2;
}
