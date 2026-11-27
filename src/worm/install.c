/*
 * install - install an optional core package for the desktop.
 *
 * Installed setuid root as /usr/lib/worm/install:
 *
 *     install PACKAGE          (password) wh update, then wh install PACKAGE
 *
 * Programs that need packages the image leaves out (Tor Browser's
 * libraries) offer to install them. Only packages that an installed
 * package lists in /usr/share/worm/installable (an empty file named after
 * each) are allowed, only from the core repository, and only for root or
 * members of the group wheel who give their own password, read as one line
 * from standard input. The system restarts once the package is installed,
 * as after any core package change.
 */

#define _GNU_SOURCE
#include <grp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "admin.h"

#define WH            "/usr/bin/wh"
#define INSTALLABLE   "/usr/share/worm/installable"
#define NAME_MAX_LEN  64
#define PASSWORD_MAX  512

__attribute__((noreturn)) static void fail(int status, const char *msg)
{
	fprintf(stderr, "install: %s\n", msg);
	exit(status);
}

static const char *check_package(const char *s)
{
	char path[sizeof(INSTALLABLE) + NAME_MAX_LEN + 2];
	size_t n = strspn(s, "abcdefghijklmnopqrstuvwxyz0123456789-");
	struct stat st;

	if (n == 0 || n > NAME_MAX_LEN || s[n] || s[0] == '-')
		fail(2, "not a package name");
	snprintf(path, sizeof(path), "%s/%s", INSTALLABLE, s);
	if (lstat(path, &st) < 0 || !S_ISREG(st.st_mode) || st.st_uid != 0)
		fail(2, "this package cannot be installed from the desktop");
	return s;
}

static void need_password(void)
{
	char password[PASSWORD_MAX + 2];
	size_t n;

	if (!fgets(password, sizeof(password), stdin))
		password[0] = '\0';
	n = strcspn(password, "\n");
	password[n] = '\0';
	if (!admin_password_ok(password)) {
		explicit_bzero(password, sizeof(password));
		fail(3, "wrong password");
	}
	explicit_bzero(password, sizeof(password));
}

int main(int argc, char **argv)
{
	const char *package;
	pid_t pid;
	int status;

	/* Files made as root never inherit the caller's umask (setuid helpers). */
	umask(022);

	clearenv();
	if (!caller_is_admin())
		fail(1, "only administrators (the group wheel) can do this");
	if (argc != 2)
		fail(2, "usage: install PACKAGE");
	package = check_package(argv[1]);
	need_password();

	if (setgroups(0, NULL) < 0 || setgid(0) < 0 || setuid(0) < 0)
		fail(1, "cannot become root");
	setenv("PATH", "/usr/bin:/usr/sbin", 1);
	setenv("LANG", "C.UTF-8", 1);

	/* A fresh index first, so the newest release is installed. */
	pid = fork();
	if (pid < 0)
		fail(1, "cannot run wh");
	if (pid == 0) {
		execl(WH, "wh", "update", (char *)NULL);
		_exit(127);
	}
	if (waitpid(pid, &status, 0) < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0)
		fail(4, "cannot update the package index");

	execl(WH, "wh", "--yes", "--from", "core", "install", package, (char *)NULL);
	fail(1, "cannot run wh");
}
