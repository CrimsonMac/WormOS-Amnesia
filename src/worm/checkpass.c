/*
 * checkpass - check the caller's own password, for the lock screen.
 *
 * Installed setuid root as /usr/lib/worm/checkpass (only root can read
 * /etc/shadow). It checks the password of the user running it (the real
 * user ID), never anyone else's:
 *
 *     checkpass            password on standard input (one line):
 *                          exit 0 if it is right, 1 if not (after a
 *                          two-second pause, so guessing is slow)
 *     checkpass --status   exit 0 if the user has a password, 2 if not
 *                          (an empty or locked one, or an old DES hash
 *                          libxcrypt cannot check: a lock screen could
 *                          then never be opened, so it is not used)
 *
 * Wrong passwords in a row make the next check wait (/run/worm/checkpass/UID,
 * root's alone): 30 seconds after 5, two minutes after 10, ten minutes
 * after 20. While waiting, checkpass checks nothing and exits 3, printing
 * the seconds left. A right password clears the count.
 *
 * Failures are logged (authpriv).
 */

#define _GNU_SOURCE
#include <pwd.h>
#include <shadow.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <time.h>
#include <syslog.h>
#include <unistd.h>

#include "password.h"

#define MAX_LEN 512

#define TRIES_DIR "/run/worm/checkpass"

/*
 * The caller's record of wrong passwords, locked: checks started at once
 * run one after another, each seeing the count the one before it left.
 */
static int tries_open(uid_t uid)
{
	char path[64];
	int fd;

	mkdir("/run/worm", 0711);
	mkdir(TRIES_DIR, 0700);
	snprintf(path, sizeof(path), TRIES_DIR "/%u", (unsigned)uid);
	fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
	if (fd >= 0 && flock(fd, LOCK_EX) < 0) {
		close(fd);
		return -1;
	}
	return fd;
}

/* Wrong passwords in a row, and when the last one was. */
static void tries_read(int fd, unsigned *count, time_t *last)
{
	char buf[64] = "";
	long long t = 0;

	*count = 0;
	*last = 0;
	if (fd >= 0 && pread(fd, buf, sizeof(buf) - 1, 0) > 0 &&
	    sscanf(buf, "%u %lld", count, &t) == 2)
		*last = (time_t)t;
	else
		*count = 0;
}

static void tries_write(int fd, unsigned count)
{
	char buf[64];
	int len;

	if (fd < 0 || ftruncate(fd, 0) < 0 || !count)
		return;
	len = snprintf(buf, sizeof(buf), "%u %lld\n", count, (long long)time(NULL));
	if (pwrite(fd, buf, (size_t)len, 0) != len)
		(void)!ftruncate(fd, 0);
}

static unsigned wait_after(unsigned count)
{
	return count >= 20 ? 600 : count >= 10 ? 120 : count >= 5 ? 30 : 0;
}

int main(int argc, char **argv)
{
	char password[MAX_LEN + 2];
	struct passwd *pw;
	struct spwd *sp;
	size_t len;
	int ok, fd;
	unsigned count, wait;
	time_t last, now;

	umask(022);
	if (!(pw = getpwuid(getuid())))
		return 1;
	if (argc == 2 && !strcmp(argv[1], "--status")) {
		/* Only a hash libxcrypt can check ($id$..., not old DES ones). */
		sp = getspnam(pw->pw_name);
		return sp && sp->sp_pwdp[0] == '$' ? 0 : 2;
	}
	if (argc != 1) {
		fputs("usage: checkpass [--status] (password on standard input)\n", stderr);
		return 1;
	}
	if (!fgets(password, sizeof(password), stdin))
		return 1;
	/* Held until the end: the next check waits for this one. */
	fd = tries_open(getuid());
	tries_read(fd, &count, &last);
	wait = wait_after(count);
	now = time(NULL);
	if (wait && now < last + (time_t)wait) {
		explicit_bzero(password, sizeof(password));
		printf("%lld\n", (long long)(last + (time_t)wait - now));
		return 3;
	}
	len = strcspn(password, "\n");
	if (len > MAX_LEN) {
		explicit_bzero(password, sizeof(password));
		return 1;
	}
	password[len] = '\0';
	/* An empty password never unlocks, even for an account without one. */
	ok = len > 0 && password_ok(pw->pw_name, password);
	explicit_bzero(password, sizeof(password));
	tries_write(fd, ok ? 0 : count + 1);
	if (!ok) {
		openlog("checkpass", 0, LOG_AUTHPRIV);
		syslog(LOG_NOTICE, "wrong password for %s at the lock screen", pw->pw_name);
		closelog();
	}
	return ok ? 0 : 1;
}
