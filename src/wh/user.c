/*
 * wh-user (/usr/lib/wh/user) - package changes for users, on editions that
 * allow them (WormOS Amnesia, which has no root login). Installed setuid
 * root; it does nothing unless /etc/wh/users-may-install exists.
 *
 *     wh-user [--from SOURCE] install|reinstall|remove PACKAGE...
 *     wh-user update | upgrade
 *     wh-user kit install|remove NAME
 *
 * wh and worm hand these over by themselves when a user runs them there.
 * The helper takes nothing else from its caller: the arguments must be
 * package, kit or source names, the environment is replaced, and it runs
 * wh (with --yes) or worm as root. Packages come only from the signed
 * repositories wh trusts, so a user can add programs, never write
 * anywhere else.
 */

#define _GNU_SOURCE
#include <grp.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>

#include "wh.h"

static void fail(const char *msg)
{
	fprintf(stderr, "wh: %s\n", msg);
	exit(1);
}

static int valid_name(const char *s)
{
	size_t n = strspn(s, "abcdefghijklmnopqrstuvwxyz0123456789+.-:");

	return n > 0 && n < 64 && !s[n] && s[0] != '-' && s[0] != '.';
}

int main(int argc, char **argv)
{
	char *args[72], *term = getenv("TERM"), saved_term[64] = "";
	struct passwd *pw = getpwuid(getuid());
	int n = 0, i = 1;

	if (access(WH_USERS_FLAG, F_OK) < 0)
		fail("packages are managed by root, try again with su");
	if (term && strlen(term) < sizeof(saved_term) && strspn(term,
	    "abcdefghijklmnopqrstuvwxyz0123456789-") == strlen(term))
		snprintf(saved_term, sizeof(saved_term), "%s", term);

	if (argc > 3 && !strcmp(argv[1], "kit")) {
		if ((strcmp(argv[2], "install") && strcmp(argv[2], "remove")) || argc != 4 ||
		    !valid_name(argv[3]))
			fail("usage: worm kit install|remove NAME");
		args[n++] = "/usr/bin/worm";
		args[n++] = "kit";
		args[n++] = argv[2];
		args[n++] = argv[3];
	} else {
		args[n++] = "/usr/bin/wh";
		args[n++] = "--yes";
		if (i + 1 < argc && !strcmp(argv[i], "--from")) {
			if (!valid_name(argv[i + 1]))
				fail("invalid source name");
			args[n++] = "--from";
			args[n++] = argv[i + 1];
			i += 2;
		}
		if (i >= argc)
			fail("usage: wh install|remove PACKAGE... | update | upgrade");
		if (!strcmp(argv[i], "update") || !strcmp(argv[i], "upgrade")) {
			if (argc != i + 1)
				fail("usage: wh update | upgrade");
		} else if (strcmp(argv[i], "install") && strcmp(argv[i], "reinstall") &&
			   strcmp(argv[i], "remove")) {
			fail("only install, reinstall, remove, update and upgrade are open to users");
		} else if (argc == i + 1) {
			fail("name a package");
		}
		args[n++] = argv[i++];
		for (; i < argc && n < 70; i++) {
			if (!valid_name(argv[i]))
				fail("invalid package name");
			args[n++] = argv[i];
		}
	}
	args[n] = NULL;

	openlog("wh-user", LOG_PID, LOG_AUTHPRIV);
	syslog(LOG_NOTICE, "%s: %s %s", pw ? pw->pw_name : "?", args[1], n > 2 ? args[2] : "");
	closelog();
	if (setgroups(0, NULL) < 0 || setresgid(0, 0, 0) < 0 || setresuid(0, 0, 0) < 0)
		fail("cannot become root");
	clearenv();
	setenv("PATH", "/usr/sbin:/usr/bin", 1);
	setenv("HOME", "/root", 1);
	setenv("LANG", "C.UTF-8", 1);
	if (*saved_term)
		setenv("TERM", saved_term, 1);
	if (chdir("/") < 0)
		fail("cannot change to /");
	execv(args[0], args);
	perror("wh: cannot run the change");
	return 1;
}
