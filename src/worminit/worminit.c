/*
 * worminit - the WormOS init system (PID 1).
 *
 * At boot it mounts the kernel pseudo filesystems, sets the hostname and
 * starts every service described in /etc/worm/services, in name order.
 * It then reaps orphaned processes and restarts services marked "respawn".
 *
 * Service files are simple key = value lists:
 *
 *     # /etc/worm/services/90-console
 *     exec    = /sbin/getty -L 115200 ttyS0 vt100
 *     type    = respawn          # oneshot | daemon | respawn
 *
 * oneshot  run to completion before the next service starts
 * daemon   start and leave running
 * respawn  start and restart whenever it exits
 *
 * "safe = no" leaves a service out in safe mode (wormos.safe on the kernel
 * command line, from the boot menu), such as the network.
 *
 * Shutdown is requested with signals (compatible with busybox halt, poweroff
 * and reboot): SIGUSR1 halts, SIGUSR2 powers off, SIGTERM and SIGINT reboot.
 *
 * Before a shutdown unmounts the filesystems, after every process has
 * stopped, the programs in /etc/worm/shutdown.d run in name order (at most
 * SHUTDOWN_HOOK_SECS each), such as WormOS Amnesia wiping what the session
 * left in memory. A soft reboot does not run them.
 *
 * SIGHUP soft-reboots: all processes are stopped and the filesystems
 * unmounted, then wormrd takes over as PID 1 again to apply pending
 * snapshot operations and start the system afresh, without restarting the
 * kernel or the firmware. snap rewind uses this.
 */

#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define SERVICE_DIR   "/etc/worm/services"
#define SHUTDOWN_DIR  "/etc/worm/shutdown.d"
#define SHUTDOWN_HOOK_SECS 30
#define WORMRD        "/usr/lib/wormrd/init"
#define MAX_SERVICES  64
#define MAX_EXEC      1024

/* A respawning service that dies this quickly is considered failing. */
#define FAST_EXIT_SECS   2
#define FAST_EXIT_LIMIT  5
#define BACKOFF_SECS     30

enum svc_type { SVC_ONESHOT, SVC_DAEMON, SVC_RESPAWN };

struct service {
	char name[NAME_MAX + 1];
	char exec[MAX_EXEC];
	enum svc_type type;
	int unsafe;             /* not started in safe mode */
	pid_t pid;
	time_t started;
	time_t restart_at;      /* 0 when no restart is pending */
	int fast_exits;
};

static struct service services[MAX_SERVICES];
static int nservices;
static int shutting_down;

static void log_msg(const char *fmt, ...)
{
	va_list ap;
	FILE *con = fopen("/dev/console", "w");

	if (!con)
		con = stderr;
	fputs("worminit: ", con);
	va_start(ap, fmt);
	vfprintf(con, fmt, ap);
	va_end(ap);
	fputc('\n', con);
	if (con != stderr)
		fclose(con);
}

static time_t now(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec;
}

static void mount_fs(const char *src, const char *dst, const char *type,
		     unsigned long flags, const char *data)
{
	mkdir(dst, 0755);
	if (mount(src, dst, type, flags, data) < 0 && errno != EBUSY)
		log_msg("cannot mount %s: %s", dst, strerror(errno));
}

static void mount_pseudo_filesystems(void)
{
	const unsigned long nosuid_nodev_noexec = MS_NOSUID | MS_NODEV | MS_NOEXEC;

	/* Users see only their own processes. */
	mount_fs("proc", "/proc", "proc", nosuid_nodev_noexec, "hidepid=invisible");
	mount_fs("sysfs", "/sys", "sysfs", nosuid_nodev_noexec, NULL);
	mount_fs("securityfs", "/sys/kernel/security", "securityfs",
		 nosuid_nodev_noexec, NULL);
	mount_fs("devtmpfs", "/dev", "devtmpfs", MS_NOSUID | MS_NOEXEC, "mode=0755");
	mount_fs("devpts", "/dev/pts", "devpts", MS_NOSUID | MS_NOEXEC,
		 "gid=5,mode=0620,ptmxmode=0666");
	mount_fs("tmpfs", "/dev/shm", "tmpfs", nosuid_nodev_noexec, "mode=1777");
	mount_fs("tmpfs", "/run", "tmpfs", nosuid_nodev_noexec, "mode=0755");
	/* /tmp stays executable: build tools and installers run things there. */
	mount_fs("tmpfs", "/tmp", "tmpfs", MS_NOSUID | MS_NODEV, "mode=1777");
	mount_fs("cgroup2", "/sys/fs/cgroup", "cgroup2", nosuid_nodev_noexec, NULL);
}

static void set_hostname(void)
{
	char name[HOST_NAME_MAX + 1] = "wormos";
	FILE *f = fopen("/etc/hostname", "r");

	if (f) {
		if (fgets(name, sizeof(name), f))
			name[strcspn(name, " \t\r\n")] = '\0';
		fclose(f);
	}
	if (name[0] && sethostname(name, strlen(name)) < 0)
		log_msg("cannot set hostname: %s", strerror(errno));
}

static char *trim(char *s)
{
	char *end;

	while (*s == ' ' || *s == '\t')
		s++;
	end = s + strlen(s);
	while (end > s && (end[-1] == ' ' || end[-1] == '\t' ||
			   end[-1] == '\n' || end[-1] == '\r'))
		*--end = '\0';
	return s;
}

static int parse_service(const char *dir, const char *file, struct service *svc)
{
	char path[PATH_MAX], line[MAX_EXEC + 64];
	FILE *f;
	int lineno = 0;

	memset(svc, 0, sizeof(*svc));
	snprintf(svc->name, sizeof(svc->name), "%s", file);
	svc->type = SVC_DAEMON;

	snprintf(path, sizeof(path), "%s/%s", dir, file);
	f = fopen(path, "r");
	if (!f) {
		log_msg("cannot open %s: %s", path, strerror(errno));
		return -1;
	}

	while (fgets(line, sizeof(line), f)) {
		char *key, *value, *eq;

		lineno++;
		key = trim(line);
		if (*key == '\0' || *key == '#')
			continue;
		eq = strchr(key, '=');
		if (!eq) {
			log_msg("%s:%d: expected key = value", path, lineno);
			continue;
		}
		*eq = '\0';
		key = trim(key);
		value = trim(eq + 1);

		if (strcmp(key, "exec") == 0) {
			snprintf(svc->exec, sizeof(svc->exec), "%s", value);
		} else if (strcmp(key, "type") == 0) {
			if (strcmp(value, "oneshot") == 0)
				svc->type = SVC_ONESHOT;
			else if (strcmp(value, "daemon") == 0)
				svc->type = SVC_DAEMON;
			else if (strcmp(value, "respawn") == 0)
				svc->type = SVC_RESPAWN;
			else
				log_msg("%s:%d: unknown type '%s'", path, lineno, value);
		} else if (strcmp(key, "safe") == 0) {
			svc->unsafe = strcmp(value, "no") == 0;
		} else {
			log_msg("%s:%d: unknown key '%s'", path, lineno, key);
		}
	}
	fclose(f);

	if (svc->exec[0] == '\0') {
		log_msg("%s: no exec line, ignoring", path);
		return -1;
	}
	return 0;
}

static int name_filter(const struct dirent *d)
{
	return d->d_name[0] != '.';
}

/* Safe mode: wormos.safe on the kernel command line. */
static int safe_mode(void)
{
	char line[4096], *tok, *save;
	FILE *f = fopen("/proc/cmdline", "re");
	int safe = 0;

	if (!f)
		return 0;
	if (fgets(line, sizeof(line), f))
		for (tok = strtok_r(line, " \n", &save); tok; tok = strtok_r(NULL, " \n", &save))
			if (strcmp(tok, "wormos.safe") == 0)
				safe = 1;
	fclose(f);
	return safe;
}

static void load_services(void)
{
	struct dirent **list;
	int n, i, safe = safe_mode();

	n = scandir(SERVICE_DIR, &list, name_filter, alphasort);
	if (n < 0) {
		log_msg("cannot read %s: %s", SERVICE_DIR, strerror(errno));
		return;
	}
	for (i = 0; i < n; i++) {
		if (nservices < MAX_SERVICES &&
		    parse_service(SERVICE_DIR, list[i]->d_name,
				  &services[nservices]) == 0) {
			if (safe && services[nservices].unsafe)
				log_msg("safe mode: not starting %s", list[i]->d_name);
			else
				nservices++;
		}
		free(list[i]);
	}
	free(list);
}

static pid_t spawn(const char *cmd)
{
	sigset_t all;
	pid_t pid = fork();

	if (pid != 0)
		return pid;

	/* Child: clean signal state, own session, console as stdio. */
	sigemptyset(&all);
	sigprocmask(SIG_SETMASK, &all, NULL);
	setsid();

	int fd = open("/dev/console", O_RDWR | O_NOCTTY);
	if (fd >= 0) {
		dup2(fd, 0);
		dup2(fd, 1);
		dup2(fd, 2);
		if (fd > 2)
			close(fd);
	}

	execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
	_exit(127);
}

static void start_service(struct service *svc)
{
	svc->pid = spawn(svc->exec);
	svc->started = now();
	svc->restart_at = 0;

	if (svc->pid < 0) {
		log_msg("%s: fork failed: %s", svc->name, strerror(errno));
		svc->pid = 0;
		return;
	}

	if (svc->type == SVC_ONESHOT) {
		int status;

		while (waitpid(svc->pid, &status, 0) < 0 && errno == EINTR)
			;
		if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
			log_msg("%s: failed", svc->name);
		svc->pid = 0;
	}
}

static struct service *find_by_pid(pid_t pid)
{
	for (int i = 0; i < nservices; i++)
		if (services[i].pid == pid)
			return &services[i];
	return NULL;
}

static void schedule_restart(struct service *svc)
{
	time_t t = now();

	if (t - svc->started < FAST_EXIT_SECS)
		svc->fast_exits++;
	else
		svc->fast_exits = 0;

	if (svc->fast_exits >= FAST_EXIT_LIMIT) {
		log_msg("%s: exiting too quickly, retrying in %ds",
			svc->name, BACKOFF_SECS);
		svc->restart_at = t + BACKOFF_SECS;
		svc->fast_exits = 0;
	} else {
		/*
		 * A second's pause: a login that ends hangs its terminal up,
		 * which the kernel finishes a moment later. A getty started
		 * at once on the same port could open it before that and sit
		 * silent (seen on the serial console early in boot).
		 */
		svc->restart_at = t + 1;
	}
}

static void reap_children(void)
{
	pid_t pid;
	int status;

	while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
		struct service *svc = find_by_pid(pid);

		if (!svc)
			continue;       /* an orphan we adopted */
		svc->pid = 0;
		if (svc->type == SVC_RESPAWN && !shutting_down)
			schedule_restart(svc);
	}
}

static void run_pending_restarts(void)
{
	time_t t = now();

	for (int i = 0; i < nservices; i++)
		if (services[i].restart_at && services[i].restart_at <= t)
			start_service(&services[i]);
}

static int next_timeout(void)
{
	time_t t = now(), soonest = 0;

	for (int i = 0; i < nservices; i++) {
		time_t r = services[i].restart_at;

		if (r && (!soonest || r < soonest))
			soonest = r;
	}
	if (!soonest)
		return -1;
	return soonest > t ? (int)(soonest - t) : 0;
}

static void kill_everything(void)
{
	time_t deadline;

	log_msg("stopping all processes");
	kill(-1, SIGTERM);

	/* Give processes a few seconds to exit cleanly. */
	deadline = now() + 3;
	while (now() < deadline) {
		if (waitpid(-1, NULL, WNOHANG) < 0 && errno == ECHILD)
			return;
		usleep(100000);
	}
	kill(-1, SIGKILL);
	while (waitpid(-1, NULL, WNOHANG) > 0)
		;
}

/* Kernel filesystems kept across a soft reboot. */
static int kernel_fs(const char *dir)
{
	static const char *const keep[] = { "/proc", "/sys", "/dev", NULL };

	for (int i = 0; keep[i]; i++) {
		size_t n = strlen(keep[i]);

		if (strncmp(dir, keep[i], n) == 0 && (dir[n] == '\0' || dir[n] == '/'))
			return 1;
	}
	return 0;
}

static void unmount_all(int keep_kernel_fs)
{
	char mounts[64][PATH_MAX];
	char line[PATH_MAX * 2];
	int n = 0;
	FILE *f = fopen("/proc/self/mounts", "r");

	if (f) {
		while (n < 64 && fgets(line, sizeof(line), f)) {
			char dev[PATH_MAX], dir[PATH_MAX];

			if (sscanf(line, "%4095s %4095s", dev, dir) == 2 &&
			    strcmp(dir, "/") != 0 &&
			    !(keep_kernel_fs && kernel_fs(dir)))
				snprintf(mounts[n++], PATH_MAX, "%s", dir);
		}
		fclose(f);
	}

	/* Unmount in reverse order so nested mounts go first. */
	while (n-- > 0)
		umount2(mounts[n], MNT_DETACH);

	/*
	 * On a soft reboot the filesystem stays in use (a rewind is about to
	 * write to it), and making it read-only here would have the next
	 * read-write mount reset its options.
	 */
	if (!keep_kernel_fs &&
	    mount(NULL, "/", NULL, MS_REMOUNT | MS_RDONLY, NULL) < 0)
		log_msg("cannot remount / read-only: %s", strerror(errno));
}

/* Run the shutdown hooks, each until it exits or its time is up. */
static void shutdown_hooks(void)
{
	struct dirent **list;
	int n = scandir(SHUTDOWN_DIR, &list, name_filter, alphasort);

	for (int i = 0; i < n; i++) {
		char path[PATH_MAX];
		struct stat st;
		pid_t pid;

		snprintf(path, sizeof(path), "%s/%s", SHUTDOWN_DIR, list[i]->d_name);
		free(list[i]);
		if (stat(path, &st) < 0 || !S_ISREG(st.st_mode) || !(st.st_mode & S_IXUSR))
			continue;
		log_msg("running %s", path);
		if ((pid = fork()) == 0) {
			sigset_t none;

			sigemptyset(&none);
			sigprocmask(SIG_SETMASK, &none, NULL);
			execl(path, path, (char *)NULL);
			_exit(127);
		}
		if (pid < 0)
			continue;
		for (time_t deadline = now() + SHUTDOWN_HOOK_SECS;;) {
			pid_t r = waitpid(pid, NULL, WNOHANG);

			if (r == pid || (r < 0 && errno != EINTR))
				break;
			if (now() >= deadline) {
				log_msg("%s took too long", path);
				kill(pid, SIGKILL);
				waitpid(pid, NULL, 0);
				break;
			}
			usleep(50000);
		}
	}
	if (n > 0)
		free(list);
}

static void shutdown_system(int how)
{
	shutting_down = 1;
	kill_everything();
	shutdown_hooks();
	sync();
	unmount_all(0);
	sync();

	switch (how) {
	case RB_HALT_SYSTEM:  log_msg("system halted"); break;
	case RB_POWER_OFF:    log_msg("powering off"); break;
	default:              log_msg("rebooting"); break;
	}
	reboot(how);
	/* reboot() only returns on failure; never let PID 1 exit. */
	for (;;)
		pause();
}

static void soft_reboot(void)
{
	sigset_t none;

	log_msg("soft reboot");
	shutting_down = 1;
	kill_everything();
	sync();
	unmount_all(1);
	sync();

	/* wormrd runs as PID 1 with a clean signal state. */
	sigemptyset(&none);
	sigprocmask(SIG_SETMASK, &none, NULL);
	execl(WORMRD, WORMRD, "--soft-reboot", (char *)NULL);

	log_msg("cannot start %s: %s, rebooting", WORMRD, strerror(errno));
	shutdown_system(RB_AUTOBOOT);
}

int main(void)
{
	sigset_t set;

	if (getpid() != 1) {
		fprintf(stderr, "worminit: must be run as PID 1\n");
		return 1;
	}

	/* Handle signals synchronously from the main loop. */
	sigemptyset(&set);
	sigaddset(&set, SIGCHLD);
	sigaddset(&set, SIGTERM);
	sigaddset(&set, SIGINT);
	sigaddset(&set, SIGUSR1);
	sigaddset(&set, SIGUSR2);
	sigaddset(&set, SIGHUP);
	sigprocmask(SIG_BLOCK, &set, NULL);

	/* Let ctrl-alt-del send SIGINT to init instead of rebooting at once. */
	reboot(RB_DISABLE_CAD);

	umask(022);
	setenv("PATH", "/usr/sbin:/usr/bin:/sbin:/bin", 1);
	if (chdir("/") < 0)
		log_msg("cannot chdir to /: %s", strerror(errno));

	mount_pseudo_filesystems();
	set_hostname();
	log_msg("starting WormOS");

	load_services();
	for (int i = 0; i < nservices; i++)
		start_service(&services[i]);

	for (;;) {
		struct timespec ts, *tsp = NULL;
		siginfo_t info;
		int timeout = next_timeout();
		int sig;

		if (timeout >= 0) {
			ts.tv_sec = timeout;
			ts.tv_nsec = 0;
			tsp = &ts;
		}

		sig = sigtimedwait(&set, &info, tsp);
		switch (sig) {
		case SIGCHLD:
			reap_children();
			break;
		case SIGUSR1:
			shutdown_system(RB_HALT_SYSTEM);
			break;
		case SIGUSR2:
			shutdown_system(RB_POWER_OFF);
			break;
		case SIGTERM:
		case SIGINT:
			shutdown_system(RB_AUTOBOOT);
			break;
		case SIGHUP:
			soft_reboot();
			break;
		}
		run_pending_restarts();
	}
}
