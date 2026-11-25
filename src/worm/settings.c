/*
 * settings - system settings for the desktop (the Settings program).
 *
 * Installed setuid root as /usr/lib/worm/settings. The caller must be root
 * or in the group wheel. Reading runs right away; changes to the system
 * need root's password too, read as one line from standard input:
 *
 *     firewall                 worm fw status
 *     anon-status              worm anon status
 *     status                   worm status
 *
 *     zone NAME                (password) worm time zone NAME
 *     anon on|off              (password) worm anon on|off
 *     hostname NAME            (password) worm hostname NAME
 *     vm KEY VALUE             (password) worm vm set KEY VALUE
 *     vm-reset KEY             (password) worm vm reset KEY
 *
 *     bridges KIND             (password*) worm anon bridges KIND
 *     time-sync                (password*) worm time sync
 *     panic / panic-off        (password*) worm panic [off]
 *
 * (*) On a system without root (/etc/worm/no-root, WormOS Amnesia) nobody
 * could ever give root's password, and these change nothing that is not
 * safe for the session's own user: they run without one.
 * Every argument is checked, the environment is cleared, and worm runs as
 * root with an argument list built here.
 */

#define _GNU_SOURCE
#include <grp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "admin.h"

#define WORM         "/usr/bin/worm"
#define NO_ROOT      "/etc/worm/no-root"
#define PASSWORD_MAX 512

__attribute__((noreturn)) static void fail(int status, const char *msg)
{
	fprintf(stderr, "settings: %s\n", msg);
	exit(status);
}

/* Letters, digits and a few separators, as in time zones and host names. */
static const char *check_word(const char *s, const char *extra)
{
	size_t n = strlen(s);

	if (n == 0 || n > 64 || s[0] == '-' || s[0] == '.' || strstr(s, ".."))
		fail(2, "invalid value");
	for (const char *c = s; *c; c++)
		if (!((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') ||
		      (*c >= '0' && *c <= '9') || strchr(extra, *c)))
			fail(2, "invalid value");
	return s;
}

static void need_password(void)
{
	char password[PASSWORD_MAX + 2];

	if (!fgets(password, sizeof(password), stdin))
		password[0] = '\0';
	password[strcspn(password, "\n")] = '\0';
	if (!root_password_ok(password)) {
		explicit_bzero(password, sizeof(password));
		fail(3, "wrong password");
	}
	explicit_bzero(password, sizeof(password));
}

/* Root's password, unless the system has no root at all. */
static void need_password_unless_no_root(void)
{
	if (access(NO_ROOT, F_OK) != 0)
		need_password();
}

int main(int argc, char **argv)
{
	const char *cmd = argc > 1 ? argv[1] : "";
	const char *args[8] = { WORM };
	int n = 1;

	clearenv();
	if (!caller_is_admin())
		fail(1, "only administrators (the group wheel) can do this");

	if (!strcmp(cmd, "firewall") && argc == 2) {
		args[n++] = "fw";
		args[n++] = "status";
	} else if (!strcmp(cmd, "status") && argc == 2) {
		args[n++] = "status";
	} else if (!strcmp(cmd, "bridges") && argc == 3 &&
		   (!strcmp(argv[2], "snowflake") || !strcmp(argv[2], "obfs4") ||
		    !strcmp(argv[2], "meek") || !strcmp(argv[2], "off") ||
		    !strcmp(argv[2], "status"))) {
		args[n++] = "anon";
		args[n++] = "bridges";
		args[n++] = argv[2];
		if (strcmp(argv[2], "status"))
			need_password_unless_no_root();
	} else if (!strcmp(cmd, "time-sync") && argc == 2) {
		args[n++] = "time";
		args[n++] = "sync";
		need_password_unless_no_root();
	} else if (!strcmp(cmd, "panic") && argc == 2) {
		args[n++] = "panic";
		need_password_unless_no_root();
	} else if (!strcmp(cmd, "panic-off") && argc == 2) {
		args[n++] = "panic";
		args[n++] = "off";
		need_password_unless_no_root();
	} else if (!strcmp(cmd, "anon-status") && argc == 2) {
		args[n++] = "anon";
		args[n++] = "status";
	} else if (!strcmp(cmd, "zone") && argc == 3) {
		args[n++] = "time";
		args[n++] = "zone";
		args[n++] = check_word(argv[2], "/_-+");
		need_password();
	} else if (!strcmp(cmd, "anon") && argc == 3 &&
		   (!strcmp(argv[2], "on") || !strcmp(argv[2], "off"))) {
		args[n++] = "anon";
		args[n++] = argv[2];
		need_password();
	} else if (!strcmp(cmd, "hostname") && argc == 3) {
		args[n++] = "hostname";
		args[n++] = check_word(argv[2], "-");
		need_password();
	} else if (!strcmp(cmd, "vm") && argc == 4) {
		args[n++] = "vm";
		args[n++] = "set";
		args[n++] = check_word(argv[2], "-");
		args[n++] = check_word(argv[3], ".");
		need_password();
	} else if (!strcmp(cmd, "vm-reset") && argc == 3) {
		args[n++] = "vm";
		args[n++] = "reset";
		args[n++] = check_word(argv[2], "-");
		need_password();
	} else {
		fail(2, "usage: see the comment at the top of settings.c");
	}
	args[n] = NULL;

	if (setgroups(0, NULL) < 0 || setgid(0) < 0 || setuid(0) < 0)
		fail(1, "cannot become root");
	setenv("PATH", "/usr/bin:/usr/sbin", 1);
	setenv("LANG", "C.UTF-8", 1);
	execv(WORM, (char **)args);
	fail(1, "cannot run worm");
}
