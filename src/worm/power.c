/*
 * power - shut down or restart the machine from the desktop, or panic.
 *
 * Installed setuid root as /usr/lib/worm/power for the shell's power menu.
 * It takes one word, "off", "reboot" or "panic", and nothing else from the
 * caller:
 * the environment is cleared, and the caller (by real user and groups)
 * must be root or a member of the group wheel. Then it signals worminit
 * (SIGUSR2 powers off, SIGTERM restarts), as busybox poweroff and reboot
 * do for root. "panic" runs 'worm panic' as root: it only protects (cuts
 * the network, stops blackholes, takes a snapshot), so it asks for no
 * password; bringing the network back ('worm panic off') needs root.
 */

#define _GNU_SOURCE
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <grp.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "admin.h"

int main(int argc, char **argv)
{
	int sig;

	/* Files made as root never inherit the caller's umask (setuid helpers). */
	umask(022);

	clearenv();
	if (argc != 2 || (strcmp(argv[1], "off") && strcmp(argv[1], "reboot") &&
			  strcmp(argv[1], "panic"))) {
		fputs("usage: power off|reboot|panic\n", stderr);
		return 2;
	}
	if (!caller_is_admin()) {
		fputs("power: only administrators (the group wheel) can do this\n", stderr);
		return 1;
	}
	if (!strcmp(argv[1], "panic")) {
		if (setgroups(0, NULL) < 0 || setgid(0) < 0 || setuid(0) < 0) {
			perror("power");
			return 1;
		}
		setenv("PATH", "/usr/bin:/usr/sbin", 1);
		execl("/usr/bin/worm", "worm", "panic", (char *)NULL);
		perror("power: worm");
		return 1;
	}
	sig = strcmp(argv[1], "off") == 0 ? SIGUSR2 : SIGTERM;
	sync();
	if (kill(1, sig) < 0) {
		perror("power");
		return 1;
	}
	return 0;
}
