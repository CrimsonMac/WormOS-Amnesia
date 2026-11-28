/* Checking a user's password against /etc/shadow (needs root). */

#define _GNU_SOURCE
#include <crypt.h>
#include <fcntl.h>
#include <shadow.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include "password.h"

/* Wrong passwords in a row per user, root's alone (see password.h). */
#define AUTH_DIR "/run/worm/auth"

/* Compare without stopping at the first difference. */
static int same(const char *a, const char *b)
{
	size_t la = strlen(a), lb = strlen(b);
	unsigned char diff = la != lb;

	for (size_t i = 0; i < la && i < lb; i++)
		diff |= (unsigned char)(a[i] ^ b[i]);
	return diff == 0;
}

/*
 * Lock the user's record of wrong passwords: every check of one user's
 * password waits for the one before it, so starting many at once guesses
 * no faster than one after another. -1 when it cannot be kept (not root).
 */
static int auth_lock(const char *user)
{
	char path[128];
	int fd;

	if (!*user || strchr(user, '/') || user[0] == '.' ||
	    snprintf(path, sizeof(path), AUTH_DIR "/%s", user) >= (int)sizeof(path))
		return -1;
	mkdir("/run/worm", 0711);
	mkdir(AUTH_DIR, 0700);
	fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
	if (fd >= 0 && flock(fd, LOCK_EX) < 0) {
		close(fd);
		return -1;
	}
	return fd;
}

static unsigned auth_failures(int fd)
{
	char buf[16] = "";
	unsigned n = 0;

	if (fd >= 0 && pread(fd, buf, sizeof(buf) - 1, 0) > 0)
		sscanf(buf, "%u", &n);
	return n;
}

static void auth_record(int fd, unsigned failures)
{
	char buf[16];
	int len = snprintf(buf, sizeof(buf), "%u\n", failures);

	if (fd >= 0 && ftruncate(fd, 0) == 0 && pwrite(fd, buf, (size_t)len, 0) != len)
		(void)!ftruncate(fd, 0);
}

int password_ok(const char *user, const char *password)
{
	struct crypt_data data;
	struct spwd *sp = getspnam(user);
	const char *hash;
	unsigned failures;
	int ok = 0, fd = auth_lock(user);

	failures = auth_failures(fd);
	memset(&data, 0, sizeof(data));
	if (sp) {
		if (sp->sp_pwdp[0] == '\0')
			ok = password[0] == '\0';
		else if (sp->sp_pwdp[0] != '!' && sp->sp_pwdp[0] != '*') {
			hash = crypt_r(password, sp->sp_pwdp, &data);
			ok = hash && hash[0] != '*' && same(hash, sp->sp_pwdp);
		}
	}
	explicit_bzero(&data, sizeof(data));
	if (ok) {
		if (failures)
			auth_record(fd, 0);
	} else {
		auth_record(fd, failures + 1);
		/* Held with the lock: the next guess waits for this one. */
		sleep(failures >= 20 ? 30 : failures >= 10 ? 15 : failures >= 5 ? 5 : 2);
	}
	if (fd >= 0)
		close(fd);
	return ok;
}
