/*
 * passwd - let a user change their own password.
 *
 * Installed setuid root as /usr/lib/worm/passwd, for 'worm passwd'. It
 * reads the current password and the new one from standard input, one per
 * line. The current one must match the caller's own (by real user ID, so
 * nobody can change another account), the new one must not be empty or
 * contain ':'. Then, as root, 'worm passwd-apply USER' writes it into
 * /etc/shadow, making / writable only for the moment. Root is not asked
 * for its current password.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <grp.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <unistd.h>

#include "password.h"

#define MAX_LEN 512

__attribute__((noreturn)) static void fail(int status, const char *msg)
{
	fprintf(stderr, "passwd: %s\n", msg);
	exit(status);
}

static void read_line(char *buf, size_t len)
{
	if (!fgets(buf, len, stdin))
		fail(2, "no password given");
	buf[strcspn(buf, "\n")] = '\0';
}

int main(void)
{
	char current[MAX_LEN + 2], new[MAX_LEN + 2], name[64];
	uid_t uid = getuid();
	struct passwd *pw = getpwuid(uid);
	int p[2], status;
	pid_t pid;

	/* Files made as root never inherit the caller's umask (setuid helpers). */
	umask(022);

	clearenv();
	if (!pw || strlen(pw->pw_name) >= sizeof(name))
		fail(1, "who are you?");
	snprintf(name, sizeof(name), "%s", pw->pw_name);
	read_line(current, sizeof(current));
	read_line(new, sizeof(new));
	if (uid != 0 && !password_ok(name, current)) {
		explicit_bzero(current, sizeof(current));
		explicit_bzero(new, sizeof(new));
		fail(3, "the current password is wrong");
	}
	explicit_bzero(current, sizeof(current));
	if (!new[0] || strchr(new, ':'))
		fail(2, "the new password cannot be empty or contain ':'");

	if (setgroups(0, NULL) < 0 || setgid(0) < 0 || setuid(0) < 0)
		fail(1, "cannot become root");
	setenv("PATH", "/usr/bin:/usr/sbin", 1);
	if (pipe(p) < 0 || (pid = fork()) < 0)
		fail(1, "cannot run worm");
	if (pid == 0) {
		dup2(p[0], 0);
		close(p[0]);
		close(p[1]);
		execl("/usr/bin/worm", "worm", "passwd-apply", name, (char *)NULL);
		_exit(127);
	}
	close(p[0]);
	dprintf(p[1], "%s\n", new);
	explicit_bzero(new, sizeof(new));
	close(p[1]);
	while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
		;
	return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}
