/*
 * worm passwd - change your own password.
 *
 * Asks for the current password and the new one (twice) on the terminal
 * and hands them to the setuid helper /usr/lib/worm/passwd (passwd.c),
 * which checks the current one and runs 'worm passwd-apply' as root.
 * passwd-apply writes the new password with chpasswd while / is writable
 * for the moment, like any change worm makes to /etc.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#include "worm.h"

#define HELPER "/usr/lib/worm/passwd"

static void ask(const char *prompt, char *buf, size_t len)
{
	struct termios old, quiet;
	int tty = tcgetattr(STDIN_FILENO, &old) == 0;

	fputs(prompt, stderr);
	if (tty) {
		quiet = old;
		quiet.c_lflag &= ~ECHO;
		tcsetattr(STDIN_FILENO, TCSAFLUSH, &quiet);
	}
	if (!fgets(buf, len, stdin))
		buf[0] = '\0';
	if (tty) {
		tcsetattr(STDIN_FILENO, TCSAFLUSH, &old);
		fputc('\n', stderr);
	}
	buf[strcspn(buf, "\n")] = '\0';
}

/* Feed lines to a program on its standard input; returns its status. */
static int feed(char *const argv[], const char *input)
{
	int p[2], status;
	pid_t pid;

	if (pipe(p) < 0 || (pid = fork()) < 0)
		return -1;
	if (pid == 0) {
		dup2(p[0], 0);
		close(p[0]);
		close(p[1]);
		execvp(argv[0], argv);
		_exit(127);
	}
	close(p[0]);
	if (write(p[1], input, strlen(input)) < 0) {
		/* the program ended early; its status says why */
	}
	close(p[1]);
	while (waitpid(pid, &status, 0) < 0)
		if (errno != EINTR)
			return -1;
	return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

int cmd_passwd(void)
{
	char current[514] = "", new[514], again[514], input[1100];
	int r;

	if (getuid() != 0)
		ask("Current password: ", current, sizeof(current));
	ask("New password: ", new, sizeof(new));
	ask("New password again: ", again, sizeof(again));
	if (strcmp(new, again)) {
		explicit_bzero(new, sizeof(new));
		explicit_bzero(again, sizeof(again));
		die("the new passwords differ; nothing changed%s", "");
	}
	snprintf(input, sizeof(input), "%s\n%s\n", current, new);
	explicit_bzero(current, sizeof(current));
	explicit_bzero(new, sizeof(new));
	explicit_bzero(again, sizeof(again));
	r = feed((char *[]){ HELPER, NULL }, input);
	explicit_bzero(input, sizeof(input));
	if (r == 0)
		printf("Password changed.\n");
	return r == 0 ? 0 : 1;
}

/* As root, from the helper: the new password of USER on standard input. */
int cmd_passwd_apply(const char *user)
{
	char pw[514], line[600];
	int relock, r;

	if (!getpwnam(user))
		die("no user %s", user);
	if (!fgets(pw, sizeof(pw), stdin) || !pw[0] || pw[0] == '\n')
		die("no password given%s", "");
	pw[strcspn(pw, "\n")] = '\0';
	snprintf(line, sizeof(line), "%s:%s\n", user, pw);
	explicit_bzero(pw, sizeof(pw));
	relock = root_begin_change();
	r = feed((char *[]){ "chpasswd", "-c", "yescrypt", NULL }, line);
	explicit_bzero(line, sizeof(line));
	root_end_change(relock);
	if (r != 0)
		die("chpasswd failed; the password is unchanged%s", "");
	return 0;
}
