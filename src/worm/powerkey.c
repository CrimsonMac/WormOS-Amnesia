/*
 * powerkey - power off cleanly when the power button is pressed, and lock
 * and sleep when a laptop's lid stays closed.
 *
 * Watches every input device that has a power key: the ACPI power button,
 * which is what QEMU's system_powerdown, virt-manager's and VMware's
 * "Shut Down" (without the tools) and a PC's power button press, and
 * keyboards with a power key. Devices that appear later are picked up
 * through inotify on /dev/input. A press asks worminit to power off
 * (SIGUSR2, as busybox poweroff does). The devices are not grabbed, so
 * the desktop still sees the key.
 *
 * It also watches laptop lids (the lid switch). When the lid stays closed
 * for LID_GRACE seconds (closing it for a moment, or by mistake, does
 * nothing), every desktop session is locked: each worm-shell gets SIGUSR1
 * and starts the lock screen, which marks $XDG_RUNTIME_DIR/wormos-locked
 * once it is up. After those marks (or a few seconds) the machine sleeps
 * (/sys/power/state). /etc/worm/lid can say "action = ignore" (WormOS
 * Amnesia: no lock screen, no sleep).
 *
 * Runs as root from the service 03-powerkey.
 */

#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <pwd.h>
#include <shadow.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#define MAX_DEVICES 64
#define LID_GRACE   10          /* seconds the lid stays closed before sleeping */
#define LOCK_WAIT   5           /* seconds to wait for the lock screens */
#define LID_CONFIG  "/etc/worm/lid"
#define BITS_PER_LONG (8 * sizeof(long))

static struct pollfd fds[MAX_DEVICES + 1];
static int nfds = 1;    /* fds[0] is the inotify descriptor */

static time_t lid_deadline;     /* when a closed lid makes the machine sleep, or 0 */

static int has_power_key(int fd)
{
	unsigned long keys[KEY_MAX / BITS_PER_LONG + 1] = {0};

	if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof keys), keys) < 0)
		return 0;
	return (keys[KEY_POWER / BITS_PER_LONG] >> (KEY_POWER % BITS_PER_LONG)) & 1;
}

static int has_lid(int fd)
{
	unsigned long sw[SW_MAX / BITS_PER_LONG + 1] = {0};

	if (ioctl(fd, EVIOCGBIT(EV_SW, sizeof sw), sw) < 0)
		return 0;
	return (sw[SW_LID / BITS_PER_LONG] >> (SW_LID % BITS_PER_LONG)) & 1;
}

static int lid_closed(int fd)
{
	unsigned long sw[SW_MAX / BITS_PER_LONG + 1] = {0};

	if (ioctl(fd, EVIOCGSW(sizeof sw), sw) < 0)
		return 0;
	return (sw[SW_LID / BITS_PER_LONG] >> (SW_LID % BITS_PER_LONG)) & 1;
}

static time_t now(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec;
}

/* "action = sleep" (the default) or "action = ignore". */
static int lid_ignored(void)
{
	char line[128], value[32];
	FILE *f = fopen(LID_CONFIG, "re");
	int ignore = 0;

	if (!f)
		return 0;
	while (fgets(line, sizeof line, f))
		if (sscanf(line, " action = %31s", value) == 1)
			ignore = !strcmp(value, "ignore");
	fclose(f);
	return ignore;
}

/* Has the user a password (a hash a lock screen could check)? */
static int has_password(uid_t uid)
{
	struct passwd *pw = getpwuid(uid);
	struct spwd *sp = pw ? getspnam(pw->pw_name) : NULL;

	return sp && sp->sp_pwdp[0] == '$';
}

/* The name of process PID ("" if gone) and its real user. */
static int process(pid_t pid, char *comm, size_t len, uid_t *uid)
{
	char path[64], line[256];
	FILE *f;
	unsigned u = 0;

	comm[0] = '\0';
	snprintf(path, sizeof path, "/proc/%d/comm", pid);
	if (!(f = fopen(path, "re")))
		return -1;
	if (!fgets(comm, (int)len, f))
		comm[0] = '\0';
	fclose(f);
	comm[strcspn(comm, "\n")] = '\0';
	snprintf(path, sizeof path, "/proc/%d/status", pid);
	if ((f = fopen(path, "re"))) {
		while (fgets(line, sizeof line, f))
			if (sscanf(line, "Uid: %u", &u) == 1)
				break;
		fclose(f);
	}
	*uid = u;
	return 0;
}

/* Is a lock screen of that user running, and has it marked the lock? */
static int locked(uid_t uid)
{
	char mark[64], comm[32];
	DIR *proc;
	struct dirent *d;
	uid_t owner;
	int running = 0;

	snprintf(mark, sizeof mark, "/run/user/%u/wormos-locked", (unsigned)uid);
	if (access(mark, F_OK) < 0 || !(proc = opendir("/proc")))
		return 0;
	while (!running && (d = readdir(proc))) {
		pid_t pid = (pid_t)atoi(d->d_name);

		if (pid > 0 && process(pid, comm, sizeof comm, &owner) == 0 &&
		    !strcmp(comm, "wormos-lock") && owner == uid)
			running = 1;
	}
	closedir(proc);
	return running;
}

/*
 * Ask every desktop session to lock and wait for their lock screens. True
 * when every session whose user has a password is locked (a session of a
 * user without one, such as the live system's, cannot be locked and does
 * not hold up sleep). Otherwise the machine must not sleep: it would wake
 * up unlocked.
 */
static int lock_sessions(void)
{
	uid_t uids[32];
	int n = 0;
	DIR *proc = opendir("/proc");
	struct dirent *d;

	while (proc && (d = readdir(proc))) {
		char comm[32];
		uid_t uid;
		pid_t pid = (pid_t)atoi(d->d_name);

		if (pid <= 0 || process(pid, comm, sizeof comm, &uid) < 0 || strcmp(comm, "worm-shell"))
			continue;
		if (uid && has_password(uid) && n < 32)
			uids[n++] = uid;
		kill(pid, SIGUSR1);
	}
	if (proc)
		closedir(proc);
	for (int tries = 0; tries <= LOCK_WAIT * 10; tries++) {
		int all = 1;

		for (int i = 0; i < n; i++)
			all &= locked(uids[i]);
		if (all)
			return 1;
		usleep(100000);
	}
	return 0;
}

static void lid_sleep(void)
{
	int fd;

	syslog(LOG_NOTICE, "lid closed for %d seconds: locking and sleeping", LID_GRACE);
	if (!lock_sessions()) {
		syslog(LOG_WARNING, "a session did not lock: not sleeping");
		return;
	}
	sync();
	/* "mem": the deepest sleep the machine offers (s2idle on many laptops). */
	if ((fd = open("/sys/power/state", O_WRONLY | O_CLOEXEC)) < 0 ||
	    write(fd, "mem", 3) != 3)
		syslog(LOG_ERR, "cannot sleep: %m");
	if (fd >= 0)
		close(fd);
	syslog(LOG_NOTICE, "woke up");
}

static void add_device(const char *name)
{
	char path[64], label[128] = "?";
	int fd;

	if (strncmp(name, "event", 5) || nfds > MAX_DEVICES)
		return;
	snprintf(path, sizeof path, "/dev/input/%s", name);
	fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0)
		return;
	if (!has_power_key(fd) && !has_lid(fd)) {
		close(fd);
		return;
	}
	if (has_lid(fd) && lid_closed(fd) && !lid_ignored())
		lid_deadline = now() + LID_GRACE;
	ioctl(fd, EVIOCGNAME(sizeof label), label);
	syslog(LOG_INFO, "watching %s (%s)", path, label);
	fds[nfds].fd = fd;
	fds[nfds].events = POLLIN;
	nfds++;
}

static int watched(const char *name)
{
	char path[64];
	struct stat node, open_dev;

	snprintf(path, sizeof path, "/dev/input/%s", name);
	if (stat(path, &node) < 0)
		return 1;
	for (int i = 1; i < nfds; i++)
		if (fstat(fds[i].fd, &open_dev) == 0 && open_dev.st_rdev == node.st_rdev)
			return 1;
	return 0;
}

static void remove_device(int i)
{
	close(fds[i].fd);
	fds[i] = fds[--nfds];
}

/* Reads the device's events: 1 if the power key was pressed. A lid
 * closing starts the grace period, opening it ends it. */
static int power_pressed(int fd)
{
	struct input_event ev[16];
	ssize_t n;
	int pressed = 0;

	while ((n = read(fd, ev, sizeof ev)) > 0)
		for (size_t i = 0; i < n / sizeof ev[0]; i++) {
			if (ev[i].type == EV_KEY && ev[i].code == KEY_POWER && ev[i].value == 1)
				pressed = 1;
			if (ev[i].type == EV_SW && ev[i].code == SW_LID)
				lid_deadline = ev[i].value && !lid_ignored() ? now() + LID_GRACE : 0;
		}
	return n < 0 && errno != EAGAIN ? -1 : pressed;
}

int main(void)
{
	char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
	DIR *dir;
	struct dirent *d;

	openlog("powerkey", 0, LOG_DAEMON);
	fds[0].fd = inotify_init1(IN_CLOEXEC | IN_NONBLOCK);
	fds[0].events = POLLIN;
	if (fds[0].fd < 0 || inotify_add_watch(fds[0].fd, "/dev/input", IN_CREATE | IN_ATTRIB) < 0) {
		perror("powerkey: /dev/input");
		return 1;
	}
	if ((dir = opendir("/dev/input"))) {
		while ((d = readdir(dir)))
			add_device(d->d_name);
		closedir(dir);
	}

	for (;;) {
		int timeout = -1, r;

		if (lid_deadline) {
			time_t left = lid_deadline - now();

			if (left <= 0) {
				lid_deadline = 0;
				lid_sleep();
				/* Woken with the lid still closed (an outside screen, a
				 * key): sleep again after another grace period. */
				for (int i = 1; i < nfds; i++)
					if (has_lid(fds[i].fd) && lid_closed(fds[i].fd) && !lid_ignored())
						lid_deadline = now() + LID_GRACE;
				continue;
			}
			timeout = (int)left * 1000;
		}
		if ((r = poll(fds, nfds, timeout)) < 0) {
			if (errno == EINTR)
				continue;
			perror("powerkey: poll");
			return 1;
		}
		if (r == 0)
			continue;
		if (fds[0].revents & POLLIN) {
			ssize_t n = read(fds[0].fd, buf, sizeof buf);
			for (char *p = buf; n > 0 && p < buf + n; ) {
				struct inotify_event *ev = (struct inotify_event *)p;
				/* udev changes the node after creating it: IN_ATTRIB
				 * comes again for devices already watched. */
				if (ev->len && !watched(ev->name))
					add_device(ev->name);
				p += sizeof *ev + ev->len;
			}
		}
		for (int i = nfds - 1; i >= 1; i--) {
			int r;
			if (!fds[i].revents)
				continue;
			if (fds[i].revents & (POLLHUP | POLLERR) || (r = power_pressed(fds[i].fd)) < 0) {
				remove_device(i);
				continue;
			}
			if (r) {
				syslog(LOG_NOTICE, "power button pressed: powering off");
				sync();
				kill(1, SIGUSR2);
			}
		}
	}
}
