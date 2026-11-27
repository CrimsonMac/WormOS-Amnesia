/*
 * worm panic - for when something has gone wrong, such as a program that
 * escaped a sandbox or a machine that behaves strangely:
 *
 *   1. cut the network: every packet but loopback is dropped and the
 *      network devices are taken down;
 *   2. stop every blackhole, of every user (their files are kept);
 *   3. save the system and the homes as a snapshot, pinned and marked
 *      suspect, so the state can be examined later and safe mode never
 *      boots it.
 *
 * worm panic off brings the network back.
 */

#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "worm.h"

#define BH_DIR     "/var/lib/blackholes"
#define BH_CGROUP  "/sys/fs/cgroup/blackholes"

/* Take the hardware network devices up or down. */
static void devices(char *state)
{
	struct dirent *e;
	DIR *d = opendir("/sys/class/net");

	while (d && (e = readdir(d))) {
		char path[300];

		if (e->d_name[0] == '.')
			continue;
		snprintf(path, sizeof(path), "/sys/class/net/%s/device", e->d_name);
		if (access(path, F_OK) == 0)
			run((char *[]){ "ip", "link", "set", "dev", e->d_name, state, NULL },
			    NULL, 0, 0);
	}
	if (d)
		closedir(d);
}

/* The UID owning a process, or -1. */
static long process_uid(pid_t pid)
{
	char path[64], line[256];
	long uid = -1;
	FILE *f;

	snprintf(path, sizeof(path), "/proc/%d/status", (int)pid);
	if (!(f = fopen(path, "re")))
		return -1;
	while (fgets(line, sizeof(line), f))
		if (sscanf(line, "Uid: %ld", &uid) == 1)
			break;
	fclose(f);
	return uid;
}

/* Does pid have a descriptor open on the file st describes? */
static int has_open(long pid, const struct stat *st)
{
	char dir[64], path[384];
	struct dirent *e;
	struct stat fst;
	int found = 0;
	DIR *d;

	snprintf(dir, sizeof(dir), "/proc/%ld/fd", pid);
	if (!(d = opendir(dir)))
		return 0;
	while (!found && (e = readdir(d))) {
		snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
		found = e->d_name[0] != '.' && stat(path, &fst) == 0 &&
			fst.st_dev == st->st_dev && fst.st_ino == st->st_ino;
	}
	closedir(d);
	return found;
}

/*
 * The process holding the flock on lock, from the kernel's lock table:
 * the pid file is written by the blackhole's owner and could name anything.
 * /proc/locks gives the inode; btrfs reports another device number there,
 * so the holder is confirmed through its open descriptors.
 */
static long lock_holder(int lock)
{
	char line[256];
	struct stat st;
	long holder = 0;
	FILE *f;

	if (fstat(lock, &st) < 0 || !(f = fopen("/proc/locks", "re")))
		return 0;
	while (!holder && fgets(line, sizeof(line), f)) {
		unsigned long ino;
		long pid;

		if (sscanf(line, "%*d: FLOCK %*s %*s %ld %*x:%*x:%lu", &pid, &ino) == 2 &&
		    ino == (unsigned long)st.st_ino && pid > 1 && has_open(pid, &st))
			holder = pid;
	}
	fclose(f);
	return holder;
}

/*
 * Visit every blackhole of every user. With stop, kill the bh running each
 * one that is running: its init dies with it, and with init everything
 * inside. The bh is found through the lock it holds, and only killed when
 * it belongs to the owner of the blackhole. Returns the number running;
 * *total gets the number of blackholes.
 */
int blackholes_each(int stop, int *total)
{
	DIR *users = opendir(BH_DIR);
	struct dirent *u;
	int running = 0;

	*total = 0;
	while (users && (u = readdir(users))) {
		struct dirent *b;
		struct stat st;
		int ufd;
		DIR *boxes;

		if (u->d_name[0] < '0' || u->d_name[0] > '9')
			continue;
		ufd = openat(dirfd(users), u->d_name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
		if (ufd < 0 || fstat(ufd, &st) < 0 || !(boxes = fdopendir(ufd))) {
			if (ufd >= 0)
				close(ufd);
			continue;
		}
		while ((b = readdir(boxes))) {
			char path[300];
			int lock;
			long pid;

			if (b->d_name[0] == '.')
				continue;
			snprintf(path, sizeof(path), "%s/lock", b->d_name);
			lock = openat(dirfd(boxes), path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
			if (lock < 0)
				continue;
			(*total)++;
			if (flock(lock, LOCK_SH | LOCK_NB) == 0) {
				close(lock);
				continue;
			}
			running++;
			pid = stop ? lock_holder(lock) : 0;
			if (stop && pid > 1 && process_uid(pid) == (long)st.st_uid &&
			    kill(pid, SIGKILL) == 0)
				/* Released when it is gone; never wait long on a user's lock. */
				for (int i = 0; i < 100 && flock(lock, LOCK_SH | LOCK_NB) < 0; i++)
					usleep(20000);
			close(lock);
		}
		closedir(boxes);
	}
	if (users)
		closedir(users);
	return running;
}

/* Kill whatever is left in the cgroups of root's blackholes, remove them. */
static void clear_cgroups(void)
{
	struct dirent *e;
	DIR *d = opendir(BH_CGROUP);

	while (d && (e = readdir(d))) {
		char path[300];
		int fd;

		if (e->d_name[0] == '.')
			continue;
		snprintf(path, sizeof(path), "%s/%s/cgroup.kill", BH_CGROUP, e->d_name);
		if ((fd = open(path, O_WRONLY | O_CLOEXEC)) >= 0) {
			if (write(fd, "1", 1) != 1)
				warn("cannot kill the processes in %s", path);
			close(fd);
		}
		snprintf(path, sizeof(path), "%s/%s", BH_CGROUP, e->d_name);
		for (int i = 0; i < 50 && rmdir(path) < 0 && errno == EBUSY; i++)
			usleep(20000);
	}
	if (d)
		closedir(d);
}

static int panic_on(void)
{
	char comment[64], out[256], *p, id[16];
	int total, stopped, snapshot = 0, ret = 0;
	time_t now = time(NULL);

	if (fw_cut() == 0) {
		devices("down");
		printf("Network cut: all traffic is blocked and network devices are down.\n");
	} else {
		warn("cannot cut the network%s", "");
		ret = 1;
	}

	stopped = blackholes_each(1, &total);
	clear_cgroups();
	if (stopped)
		printf("Stopped %d blackhole%s (%s files are kept).\n", stopped,
		       stopped == 1 ? "" : "s", stopped == 1 ? "its" : "their");

	strftime(comment, sizeof(comment), "Panic at %Y-%m-%d %H:%M:%S", localtime_r(&now, &(struct tm){ 0 }));
	if (run((char *[]){ "snap", "now", "--all", "--suspect", "-c", comment, NULL },
		out, sizeof(out), 0) == 0 && (p = strstr(out, "Snapshot ")))
		snapshot = atoi(p + 9);
	if (snapshot > 0) {
		snprintf(id, sizeof(id), "%d", snapshot);
		run((char *[]){ "snap", "pin", id, NULL }, out, sizeof(out), 0);
		printf("Saved the system and homes as snapshot %d (pinned; safe mode never "
		       "boots it).\n", snapshot);
	} else {
		warn("cannot take a snapshot%s", "");
		ret = 1;
	}
	printf("'worm panic off' brings the network back.\n");
	return ret;
}

/* Send sig to every process of root running the program name. */
static void signal_program(const char *name, int sig)
{
	struct dirent *e;
	DIR *d = opendir("/proc");

	while (d && (e = readdir(d))) {
		char path[300], comm[64] = "";
		FILE *f;

		if (e->d_name[0] < '0' || e->d_name[0] > '9')
			continue;
		snprintf(path, sizeof(path), "/proc/%s/comm", e->d_name);
		if (!(f = fopen(path, "re")))
			continue;
		if (fgets(comm, sizeof(comm), f))
			comm[strcspn(comm, "\n")] = '\0';
		fclose(f);
		if (!strcmp(comm, name) && process_uid(atoi(e->d_name)) == 0)
			kill(atoi(e->d_name), sig);
	}
	if (d)
		closedir(d);
}

static int panic_off(void)
{
	if (safe_mode())
		die("safe mode keeps the network off; restart normally to use it%s", "");
	if (!fw_network_cut()) {
		printf("The network is not cut.\n");
		return 0;
	}
	if (fw_restore() != 0)
		die("cannot load the firewall; the network stays cut%s", "");
	devices("up");
	/* Taking a device down dropped its routes: renew the lease to get them back. */
	signal_program("udhcpc", SIGUSR1);
	printf("The network is back (normal firewall rules, devices up).\n");
	return 0;
}

int cmd_panic(int argc, char **argv)
{
	if (argc == 0)
		return panic_on();
	if (argc == 1 && !strcmp(argv[0], "off"))
		return panic_off();
	return 2;
}
