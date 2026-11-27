/*
 * elevate - become root for the root terminal and sudo, with your own
 * password.
 *
 * Installed setuid root as /usr/lib/worm/elevate. Administrators (root or
 * members of the group wheel) confirm with their own password, read from
 * the terminal without echo (three tries); root needs none. Then it runs
 * /usr/lib/worm/root-shell as root with the arguments given, in a clean
 * environment like a login's (HOME, USER, SHELL from root's account, the
 * system PATH, the terminal type kept). Nothing else can be run through
 * it: root-shell is what sets up the desktop's display and runs a command
 * or root's shell. On systems where root is locked (/etc/worm/no-root)
 * it refuses. Every use is logged (authpriv).
 */

#define _GNU_SOURCE
#include <grp.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <syslog.h>
#include <termios.h>
#include <unistd.h>

#include "admin.h"

#define ROOT_SHELL   "/usr/lib/worm/root-shell"
#define NO_ROOT      "/etc/worm/no-root"
#define PASSWORD_MAX 512
#define MAX_ARGS     16

__attribute__((noreturn)) static void fail(const char *msg)
{
	fprintf(stderr, "%s\n", msg);
	exit(1);
}

/* One line from the terminal, without echo. */
static int ask(FILE *tty, const char *prompt, char *buf, size_t len)
{
	struct termios old, quiet;
	int ok, have_term = tcgetattr(fileno(tty), &old) == 0;

	fputs(prompt, stderr);
	fflush(stderr);
	if (have_term) {
		quiet = old;
		quiet.c_lflag &= ~(tcflag_t)(ECHO | ECHONL);
		tcsetattr(fileno(tty), TCSAFLUSH, &quiet);
	}
	ok = fgets(buf, (int)len, tty) != NULL;
	if (have_term)
		tcsetattr(fileno(tty), TCSAFLUSH, &old);
	fputc('\n', stderr);
	if (ok)
		buf[strcspn(buf, "\n")] = '\0';
	return ok;
}

int main(int argc, char **argv)
{
	char password[PASSWORD_MAX + 2], *args[MAX_ARGS + 2], *term = getenv("TERM");
	char saved_term[64] = "", prompt[128];
	struct passwd *me = getpwuid(getuid()), *root = getpwuid(0);
	int ok = getuid() == 0;
	FILE *tty;

	umask(022);
	if (access(NO_ROOT, F_OK) == 0)
		fail("This system has no administrator: root is locked, so nothing can run as root.");
	if (!caller_is_admin() || !me || !root)
		fail("Only administrators (the group wheel) can do this; su - asks for root's password.");
	if (argc - 1 > MAX_ARGS)
		fail("elevate: too many arguments");
	if (term && strlen(term) < sizeof(saved_term) &&
	    strspn(term, "abcdefghijklmnopqrstuvwxyz0123456789-") == strlen(term))
		snprintf(saved_term, sizeof(saved_term), "%s", term);

	if (!ok) {
		if (!(tty = fopen("/dev/tty", "r+e")))
			fail("elevate: no terminal to ask for your password on");
		snprintf(prompt, sizeof(prompt), "Password for %s (to act as root): ", me->pw_name);
		for (int i = 0; i < 3 && !ok; i++) {
			if (!ask(tty, prompt, password, sizeof(password)))
				break;
			ok = admin_password_ok(password);
			explicit_bzero(password, sizeof(password));
			if (!ok)
				fputs("Wrong password.\n", stderr);
		}
		fclose(tty);
	}
	openlog("elevate", LOG_PID, LOG_AUTHPRIV);
	syslog(ok ? LOG_NOTICE : LOG_WARNING, "%s %s root", me->pw_name,
	       ok ? "became" : "failed to become");
	closelog();
	if (!ok)
		return 1;

	if (initgroups("root", 0) < 0 || setresgid(0, 0, 0) < 0 || setresuid(0, 0, 0) < 0)
		fail("elevate: cannot become root");
	clearenv();
	setenv("HOME", root->pw_dir, 1);
	setenv("USER", "root", 1);
	setenv("LOGNAME", "root", 1);
	setenv("SHELL", root->pw_shell && *root->pw_shell ? root->pw_shell : "/bin/sh", 1);
	setenv("PATH", "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin", 1);
	setenv("LANG", "C.UTF-8", 1);
	if (*saved_term)
		setenv("TERM", saved_term, 1);
	if (chdir(root->pw_dir) < 0 && chdir("/") < 0)
		fail("elevate: cannot change folder");
	args[0] = "root-shell";
	for (int i = 1; i < argc; i++)
		args[i] = argv[i];
	args[argc] = NULL;
	execv(ROOT_SHELL, args);
	perror("elevate: " ROOT_SHELL);
	return 1;
}
