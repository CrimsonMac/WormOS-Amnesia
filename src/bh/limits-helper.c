/*
 * BH_LIBDIR/limits - set up and remove the cgroup of a user's blackhole.
 * Installed setuid root; bh runs it (see limits.c).
 *
 *     limits create NAME PID MEMORY PIDS
 *     limits remove NAME
 *
 * The cgroup is always BH_CGROUP/UID-NAME with the caller's real UID, so
 * a user can only touch their own. create moves PID into it only if PID
 * is a direct child of the process that ran the helper and belongs to the
 * caller; bh's child waits for bh before it runs anything, so the PID
 * cannot be reused in between. MEMORY and PIDS are clamped to sane ranges.
 * The environment is not used.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
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

static unsigned long long number(const char *s)
{
	char *end;
	unsigned long long v;

	errno = 0;
	v = strtoull(s, &end, 10);
	if (errno || end == s || *end || s[0] == '-')
		die("bad number '%s'", s);
	return v;
}

/* Is pid a direct child of our parent, owned by uid? */
static int our_child(pid_t pid, uid_t uid)
{
	char path[64], line[256];
	long ppid = -1, ruid = -1;
	FILE *f;

	snprintf(path, sizeof(path), "/proc/%d/status", (int)pid);
	if (!(f = fopen(path, "re")))
		return 0;
	while (fgets(line, sizeof(line), f)) {
		sscanf(line, "PPid: %ld", &ppid);
		sscanf(line, "Uid: %ld", &ruid);
	}
	fclose(f);
	return ppid == (long)getppid() && ruid == (long)uid;
}

int main(int argc, char **argv)
{
	uid_t uid = getuid();

	/* Files made as root never inherit the caller's umask (setuid helpers). */
	umask(022);

	if (argc == 6 && !strcmp(argv[1], "create")) {
		unsigned long long memory = number(argv[4]), pids = number(argv[5]);
		unsigned long long total = memory_total();
		pid_t pid = number(argv[3]);

		if (!valid_name(argv[2]))
			die("invalid blackhole name");
		if (pid <= 1 || !our_child(pid, uid))
			die("process %d is not yours", (int)pid);
		if (memory < 64ull << 20)
			memory = 64ull << 20;
		if (total && memory > total)
			memory = total;
		if (pids < 16)
			pids = 16;
		if (pids > BH_PIDS_MAX)
			pids = BH_PIDS_MAX;
		if (cgroup_create(uid, argv[2], pid, memory, pids) < 0)
			die("cannot set up the cgroup: %s", strerror(errno));
		return 0;
	}
	if (argc == 3 && !strcmp(argv[1], "remove")) {
		if (cgroup_remove(uid, argv[2]) < 0)
			die("cannot remove the cgroup: %s", strerror(errno));
		return 0;
	}
	fprintf(stderr, "usage: limits create NAME PID MEMORY PIDS | limits remove NAME\n");
	return 2;
}
