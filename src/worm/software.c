/*
 * software - package changes for the Software program (wormos-software).
 *
 * Installed setuid root as /usr/lib/worm/software. The caller must be root
 * or in the group wheel.
 *
 *     software search TERM                 wh search TERM (any source)
 *     software refresh                     wh update: the latest package lists
 *     software updates                     installed core packages with a newer
 *                                          version: "NAME OLD NEW" lines
 *     software install [--from SRC] PKG... (password) wh install
 *     software remove PKG...               (password) wh remove
 *     software upgrade                     (password) wh upgrade
 *
 * Searching and refreshing only read signed package lists: no password.
 * Changes need the caller's own password (an administrator confirming),
 * read as one line from standard input,
 * except where root is locked and users may install (WormOS Amnesia:
 * /etc/wh/users-may-install), where they go through wh's own user helper
 * and its rules. Core package changes are made with --later: they are
 * applied at the next restart instead of restarting the session under the
 * user's open windows; programs from Debian or Kali are ready at once.
 * Every argument is checked, the environment is cleared, and wh runs as
 * root with an argument list built here; its output goes to the caller.
 */

#define _GNU_SOURCE
#include <grp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <unistd.h>

#include "admin.h"

#define WH            "/usr/bin/wh"
#define WH_USER       "/usr/lib/wh/user"
#define USERS_FLAG    "/etc/wh/users-may-install"
#define NO_ROOT       "/etc/worm/no-root"
#define PASSWORD_MAX  512
#define MAX_PACKAGES  32

__attribute__((noreturn)) static void fail(int status, const char *msg)
{
	fprintf(stderr, "software: %s\n", msg);
	exit(status);
}

/* Package, source and search words: what wh's names are made of. */
static const char *check_name(const char *s)
{
	size_t n = strspn(s, "abcdefghijklmnopqrstuvwxyz0123456789+.-:");

	if (n == 0 || n >= 64 || s[n] || s[0] == '-' || s[0] == '.')
		fail(2, "invalid package name");
	return s;
}

static const char *check_term(const char *s)
{
	size_t n = strlen(s);

	if (n >= 64 || s[0] == '-')
		fail(2, "invalid search");
	for (size_t i = 0; i < n; i++)
		if ((unsigned char)s[i] < 0x20 || s[i] == 0x7f)
			fail(2, "invalid search");
	return s;
}

static void need_password(void)
{
	char password[PASSWORD_MAX + 2];

	if (!fgets(password, sizeof(password), stdin))
		password[0] = '\0';
	password[strcspn(password, "\n")] = '\0';
	if (!admin_password_ok(password)) {
		explicit_bzero(password, sizeof(password));
		fail(3, "wrong password");
	}
	explicit_bzero(password, sizeof(password));
}

static void become_root(void)
{
	if (setgroups(0, NULL) < 0 || setresgid(0, 0, 0) < 0 || setresuid(0, 0, 0) < 0)
		fail(1, "cannot become root");
	clearenv();
	setenv("PATH", "/usr/sbin:/usr/bin", 1);
	setenv("HOME", "/root", 1);
	setenv("LANG", "C.UTF-8", 1);
	setenv("TERM", "dumb", 1);
	if (chdir("/") < 0)
		fail(1, "cannot change to /");
}

/* Run wh as root and wait; its output goes to ours. */
static int run_wh(char *const argv[], char *out, size_t len)
{
	int pipefd[2], status;
	size_t got = 0;
	ssize_t r;
	pid_t pid;

	if (out && pipe(pipefd) < 0)
		fail(1, "cannot run wh");
	fflush(stdout);
	if ((pid = fork()) < 0)
		fail(1, "cannot run wh");
	if (pid == 0) {
		if (out) {
			dup2(pipefd[1], STDOUT_FILENO);
			close(pipefd[0]);
			close(pipefd[1]);
		}
		execv(WH, argv);
		_exit(127);
	}
	if (out) {
		close(pipefd[1]);
		while (got < len - 1 && (r = read(pipefd[0], out + got, len - 1 - got)) > 0)
			got += (size_t)r;
		/* Drain the rest, so wh never dies of SIGPIPE. */
		while (read(pipefd[0], (char[4096]){ 0 }, 4096) > 0)
			;
		close(pipefd[0]);
		out[got] = '\0';
	}
	if (waitpid(pid, &status, 0) < 0 || !WIFEXITED(status))
		return 1;
	return WEXITSTATUS(status);
}

/*
 * Installed core packages with another version in the index, from
 * 'wh --from core search': "name new [installed old]" lines.
 */
static int list_updates(void)
{
	static char out[1 << 20];
	char *line, *save;

	if (run_wh((char *[]){ WH, "--from", "core", "search", "", NULL }, out, sizeof(out)) > 1)
		fail(1, "cannot read the package index");
	for (line = strtok_r(out, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
		char name[64], new[64], old[64];

		if (line[0] == ' ')
			continue;
		if (sscanf(line, "%63s %63s [installed %63[^]]]", name, new, old) == 3) {
			printf("%s %s %s\n", name, old, new);
		}
	}
	return 0;
}

int main(int argc, char **argv)
{
	const char *cmd = argc > 1 ? argv[1] : "";
	char *args[MAX_PACKAGES + 8];
	int n = 0, users_may = access(USERS_FLAG, F_OK) == 0, i = 2;

	/* Files made as root never inherit the caller's umask (setuid helpers). */
	umask(022);

	if (!caller_is_admin())
		fail(1, "only administrators (the group wheel) can do this");

	if (!strcmp(cmd, "search") && argc == 3) {
		check_term(argv[2]);
		become_root();
		return run_wh((char *[]){ WH, "search", argv[2], NULL }, NULL, 0);
	}
	if (!strcmp(cmd, "refresh") && argc == 2) {
		become_root();
		return run_wh((char *[]){ WH, "update", NULL }, NULL, 0);
	}
	if (!strcmp(cmd, "updates") && argc == 2) {
		become_root();
		return list_updates();
	}

	if (strcmp(cmd, "install") && strcmp(cmd, "remove") && strcmp(cmd, "upgrade"))
		fail(2, "usage: software search TERM | refresh | updates | "
		     "install [--from SOURCE] PACKAGE... | remove PACKAGE... | upgrade");
	/* Where users may install (no root login), wh's own helper decides. */
	args[n++] = users_may ? WH_USER : WH;
	if (!users_may) {
		args[n++] = "--yes";
		args[n++] = "--later";
	}
	if (!strcmp(cmd, "install") && argc > 4 && !strcmp(argv[2], "--from")) {
		args[n++] = "--from";
		args[n++] = (char *)check_name(argv[3]);
		i = 4;
	}
	args[n++] = (char *)cmd;
	if (!strcmp(cmd, "upgrade")) {
		if (argc != 2)
			fail(2, "usage: software upgrade");
	} else {
		if (i >= argc || argc - i > MAX_PACKAGES)
			fail(2, "name one to 32 packages");
		for (; i < argc; i++)
			args[n++] = (char *)check_name(argv[i]);
	}
	args[n] = NULL;
	if (users_may) {
		/* wh-user is setuid itself and checks its caller: run it as the caller. */
		if (setresgid(getgid(), getgid(), getgid()) < 0 ||
		    setresuid(getuid(), getuid(), getuid()) < 0)
			fail(1, "cannot drop privileges");
		execv(WH_USER, args);
		fail(1, "cannot run wh's user helper");
	}
	if (access(NO_ROOT, F_OK) == 0)
		fail(1, "root is locked on this system: packages cannot be changed");
	need_password();
	become_root();
	/* Installs and upgrades fetch the current package lists first. */
	if (strcmp(cmd, "remove") && run_wh((char *[]){ WH, "update", NULL }, NULL, 0) != 0)
		fail(1, "cannot fetch the package lists; is the network up?");
	fflush(stdout);
	execv(WH, args);
	fail(1, "cannot run wh");
}
