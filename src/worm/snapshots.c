/*
 * snapshots - snapshot actions for the desktop (the Snapshots program).
 *
 * Installed setuid root as /usr/lib/worm/snapshots. The caller must be
 * root or in the group wheel. Actions that only add or describe run right
 * away; actions that remove protection or change the system need root's
 * password too, read as one line from standard input, because a program
 * running as the user must not be able to wipe or rewind the snapshots:
 *
 *     list                     snap list --raw
 *     now SCOPE COMMENT        take a snapshot (SCOPE: system, home or all)
 *     pin ID                   keep it from being pruned
 *     comment ID TEXT          set its comment
 *     diff ID                  what changed in the system since ID
 *
 *     unpin ID                 (password)
 *     delete ID                (password)
 *     rewind ID SCOPE          (password) go back; the system restarts
 *     restore ID PATH          (password) bring back one file or folder
 *     auto on|off              (password) snapshots before package changes
 *
 * Every argument is checked, the environment is cleared, and snap runs
 * as root with an argument list built here.
 */

#define _GNU_SOURCE
#include <grp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "admin.h"

#define SNAP          "/usr/bin/snap"
#define COMMENT_MAX   200
#define PASSWORD_MAX  512

__attribute__((noreturn)) static void fail(int status, const char *msg)
{
	fprintf(stderr, "snapshots: %s\n", msg);
	exit(status);
}

static const char *check_id(const char *s)
{
	size_t n = strspn(s, "0123456789");

	if (n == 0 || n > 9 || s[n] || s[0] == '0')
		fail(2, "not a snapshot number");
	return s;
}

static const char *check_scope(const char *s)
{
	if (!strcmp(s, "system"))
		return "--system";
	if (!strcmp(s, "home"))
		return "--home";
	if (!strcmp(s, "all"))
		return "--all";
	fail(2, "the scope is system, home or all");
}

static const char *check_text(const char *s)
{
	if (strlen(s) > COMMENT_MAX)
		fail(2, "the comment is too long");
	for (const unsigned char *c = (const unsigned char *)s; *c; c++)
		if (*c < ' ' || *c == 0x7f)
			fail(2, "the comment contains control characters");
	return s;
}

static const char *check_path(const char *s)
{
	if (s[0] != '/' || strlen(s) > 1024 || strstr(s, "/../") ||
	    (strlen(s) >= 3 && !strcmp(s + strlen(s) - 3, "/..")))
		fail(2, "the path must be absolute, without ..");
	for (const unsigned char *c = (const unsigned char *)s; *c; c++)
		if (*c < ' ' || *c == 0x7f)
			fail(2, "the path contains control characters");
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
	if (!root_password_ok(password)) {
		explicit_bzero(password, sizeof(password));
		fail(3, "wrong password");
	}
	explicit_bzero(password, sizeof(password));
}

int main(int argc, char **argv)
{
	const char *cmd = argc > 1 ? argv[1] : "";
	const char *args[8] = { SNAP };
	int n = 1;

	/* Files made as root never inherit the caller's umask (setuid helpers). */
	umask(022);

	clearenv();
	if (!caller_is_admin())
		fail(1, "only administrators (the group wheel) can do this");

	if (!strcmp(cmd, "list") && argc == 2) {
		args[n++] = "list";
		args[n++] = "--raw";
	} else if (!strcmp(cmd, "now") && argc == 4) {
		args[n++] = "now";
		args[n++] = check_scope(argv[2]);
		args[n++] = "-c";
		args[n++] = check_text(argv[3]);
	} else if (!strcmp(cmd, "pin") && argc == 3) {
		args[n++] = "pin";
		args[n++] = check_id(argv[2]);
	} else if (!strcmp(cmd, "comment") && argc == 4) {
		args[n++] = "comment";
		args[n++] = check_id(argv[2]);
		args[n++] = check_text(argv[3]);
	} else if (!strcmp(cmd, "diff") && argc == 3) {
		args[n++] = "diff";
		args[n++] = check_id(argv[2]);
	} else if (!strcmp(cmd, "unpin") && argc == 3) {
		need_password();
		args[n++] = "unpin";
		args[n++] = check_id(argv[2]);
	} else if (!strcmp(cmd, "delete") && argc == 3) {
		need_password();
		args[n++] = "delete";
		args[n++] = check_id(argv[2]);
	} else if (!strcmp(cmd, "rewind") && argc == 4) {
		args[n++] = "rewind";
		args[n++] = check_id(argv[2]);
		args[n++] = check_scope(argv[3]);
		args[n++] = "-y";
		need_password();
	} else if (!strcmp(cmd, "restore") && argc == 4) {
		args[n++] = "restore";
		args[n++] = check_id(argv[2]);
		args[n++] = check_path(argv[3]);
		need_password();
	} else if (!strcmp(cmd, "auto") && argc == 3 &&
		   (!strcmp(argv[2], "on") || !strcmp(argv[2], "off"))) {
		need_password();
		args[n++] = "auto";
		args[n++] = argv[2];
	} else {
		fail(2, "usage: see the comment at the top of snapshots.c");
	}
	args[n] = NULL;

	/* Become root completely: snap works on the top of the filesystem. */
	if (setgroups(0, NULL) < 0 || setgid(0) < 0 || setuid(0) < 0)
		fail(1, "cannot become root");
	setenv("PATH", "/usr/bin:/usr/sbin", 1);
	setenv("LANG", "C.UTF-8", 1);
	execv(SNAP, (char **)args);
	fail(1, "cannot run snap");
}
