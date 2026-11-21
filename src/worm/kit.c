/*
 * worm kit - tool kits: the tools for one kind of work (forensics, network
 * analysis...), installed on demand rather than shipped with the system.
 *
 *     worm kit list              the kits, and which are installed
 *     worm kit show NAME         what a kit contains
 *     worm kit install NAME      install its tools (root)
 *     worm kit remove NAME       remove its tools, except those another
 *                                installed kit also has (root)
 *
 * A kit is a list of packages. wh installs each one from the first source
 * that has it (core, Debian, then Kali): tools from Debian and Kali land
 * in their wormholes, containers that share the user's home folder, so the
 * user's files are their workspace and the base system stays untouched.
 * A tool that no source has is reported and skipped.
 *
 * Kits are described in /usr/share/worm/kits/NAME:
 *
 *     title = Forensics
 *     description = One line about what the kit is for
 *     tool = PACKAGE              one line per package from Debian or Kali
 *     core = PACKAGE              one line per package from the core
 *                                 repository (installed together, in one
 *                                 change, after the others)
 *     included = COMMAND...       related tools WormOS already has (shown only)
 *
 * Installed kits are recorded in /var/lib/worm/kits/NAME, one package per
 * line: the ones the kit added (core packages that were already installed
 * are left out, so removing the kit keeps them).
 *
 * Core packages change the system itself: wh installs them into a copy and
 * restarts the system quickly to apply it. They are therefore installed
 * last, all in one change, with the kit recorded beforehand.
 */

#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "worm.h"

#define KIT_DIR    "/usr/share/worm/kits"
#define KIT_STATE  "/var/lib/worm/kits"
#define MAX_TOOLS  64
#define CORE_DB    "/usr/lib/wh/db"

struct kit {
	char name[64];
	char title[128];
	char description[256];
	char included[256];
	char tools[MAX_TOOLS][64];
	char core[MAX_TOOLS];          /* tools[i] is a core package */
	int ntools;
};

static int valid_name(const char *s)
{
	size_t n = strspn(s, "abcdefghijklmnopqrstuvwxyz0123456789-");

	return n > 0 && n < 64 && !s[n] && s[0] != '-';
}

static int valid_package(const char *s)
{
	size_t n = strspn(s, "abcdefghijklmnopqrstuvwxyz0123456789-+.");

	return n > 0 && n < 64 && !s[n] && s[0] != '-' && s[0] != '.';
}

static int load_kit(const char *name, struct kit *k)
{
	char path[PATH_MAX], line[512];
	FILE *f;

	memset(k, 0, sizeof(*k));
	if (!valid_name(name))
		return -1;
	snprintf(path, sizeof(path), "%s/%s", KIT_DIR, name);
	if (!(f = fopen(path, "re")))
		return -1;
	snprintf(k->name, sizeof(k->name), "%s", name);
	while (fgets(line, sizeof(line), f)) {
		char key[32], value[400];

		line[strcspn(line, "#\n")] = '\0';
		if (sscanf(line, " %31[a-z] = %399[^\n]", key, value) != 2)
			continue;
		if (!strcmp(key, "title"))
			snprintf(k->title, sizeof(k->title), "%.127s", value);
		else if (!strcmp(key, "description"))
			snprintf(k->description, sizeof(k->description), "%.255s", value);
		else if (!strcmp(key, "included"))
			snprintf(k->included, sizeof(k->included), "%.255s", value);
		else if ((!strcmp(key, "tool") || !strcmp(key, "core")) && k->ntools < MAX_TOOLS &&
			 valid_package(value)) {
			k->core[k->ntools] = key[0] == 'c';
			snprintf(k->tools[k->ntools++], sizeof(k->tools[0]), "%.63s", value);
		}
	}
	fclose(f);
	return 0;
}

/* The packages an installed kit brought; returns how many, or -1. */
static int installed_tools(const char *name, char tools[][64], int max)
{
	char path[PATH_MAX], line[128];
	int n = 0;
	FILE *f;

	snprintf(path, sizeof(path), "%s/%s", KIT_STATE, name);
	if (!(f = fopen(path, "re")))
		return -1;
	while (n < max && fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\n")] = '\0';
		if (valid_package(line))
			snprintf(tools[n++], 64, "%.63s", line);
	}
	fclose(f);
	return n;
}

/* Is name an installed core package? */
static int core_installed(const char *name)
{
	char path[PATH_MAX];

	snprintf(path, sizeof(path), "%s/%s/info", CORE_DB, name);
	return access(path, F_OK) == 0;
}

static int kit_installed(const char *name)
{
	char path[PATH_MAX];

	snprintf(path, sizeof(path), "%s/%s", KIT_STATE, name);
	return access(path, F_OK) == 0;
}

static int names(char out[][64], int max)
{
	DIR *d = opendir(KIT_DIR);
	struct dirent *e;
	int n = 0;

	while (d && (e = readdir(d)) && n < max)
		if (valid_name(e->d_name))
			snprintf(out[n++], 64, "%.63s", e->d_name);
	if (d)
		closedir(d);
	qsort(out, n, 64, (int (*)(const void *, const void *))strcmp);
	return n;
}

static int cmd_list(void)
{
	char list[64][64];
	int n = names(list, 64);
	int tty = isatty(STDOUT_FILENO);

	if (n == 0) {
		printf("No tool kits are available (%s is empty).\n", KIT_DIR);
		return 0;
	}
	for (int i = 0; i < n; i++) {
		struct kit k;

		if (load_kit(list[i], &k) < 0)
			continue;
		printf("%s%-12s%s %-22s %s%s\n", tty ? "\033[1m" : "", k.name, tty ? "\033[0m" : "",
		       k.title, k.description, kit_installed(k.name) ? "  [installed]" : "");
	}
	printf("\n'worm kit show NAME' lists the tools; 'worm kit install NAME' installs them "
	       "(as root).\n");
	return 0;
}

static int cmd_show(const char *name)
{
	struct kit k;

	if (load_kit(name, &k) < 0)
		die("there is no kit '%s'; worm kit list shows them", name);
	printf("%s: %s\n%s\n", k.name, k.title, k.description);
	for (int pass = 1; pass >= 0; pass--) {
		int shown = 0;

		for (int i = 0; i < k.ntools; i++) {
			if (k.core[i] != pass)
				continue;
			if (!shown++)
				printf(pass ? "\nWormOS packages (core repository):\n "
					    : "\nTools (from Debian or Kali, whichever has them first):\n ");
			printf(" %s%s", k.tools[i], pass && core_installed(k.tools[i]) ? "*" : "");
		}
		if (shown)
			printf("\n");
	}
	for (int i = 0; i < k.ntools; i++)
		if (k.core[i] && core_installed(k.tools[i])) {
			printf("(* already installed)\n");
			break;
		}
	if (k.included[0])
		printf("\nAlready in WormOS: %s\n", k.included);
	printf("\n%s\n", kit_installed(k.name) ? "Installed." :
	       "Not installed: 'worm kit install NAME' (as root).");
	return 0;
}

/* Write the kit's record: the packages in list (count of them). */
static void write_record(const char *name, char list[][64], int count)
{
	char path[PATH_MAX], final[PATH_MAX];
	FILE *f;

	snprintf(path, sizeof(path), "%s/%s.new", KIT_STATE, name);
	snprintf(final, sizeof(final), "%s/%s", KIT_STATE, name);
	if (!(f = fopen(path, "we")))
		die("cannot write %s", path);
	for (int i = 0; i < count; i++)
		fprintf(f, "%s\n", list[i]);
	if (fclose(f) != 0 || rename(path, final) < 0)
		die("cannot write %s", final);
}

static int cmd_install(const char *name)
{
	char failed[1024] = "", record[MAX_TOOLS][64];
	char *argv[MAX_TOOLS + 4] = { "wh", "--yes", "install" };
	int ok = 0, nfailed = 0, nrecord = 0, nforeign = 0, ncore = 0, argc = 3;
	struct kit k;

	if (load_kit(name, &k) < 0)
		die("there is no kit '%s'; worm kit list shows them", name);
	if (fw_network_cut())
		die("the network is cut; 'worm panic off' restores it first%s", "");
	mkdir("/var/lib/worm", 0755);
	mkdir(KIT_STATE, 0755);
	for (int i = 0; i < k.ntools; i++)
		k.core[i] ? ncore++ : nforeign++;
	printf("Installing the %s kit: %d tools.", k.title, k.ntools);
	if (nforeign)
		printf(" Tools from Debian and Kali run in\nwormholes, which share your "
		       "home folder: your files are their workspace.");
	printf("\n\n");

	for (int i = 0, n = 0; i < k.ntools; i++) {
		if (k.core[i])
			continue;
		printf("[%d/%d] %s\n", ++n, nforeign, k.tools[i]);
		fflush(stdout);
		if (run((char *[]){ "wh", "--yes", "install", k.tools[i], NULL }, NULL, 0, 0) == 0) {
			snprintf(record[nrecord++], 64, "%s", k.tools[i]);
			ok++;
		} else {
			size_t len = strlen(failed);

			snprintf(failed + len, sizeof(failed) - len, " %s", k.tools[i]);
			nfailed++;
		}
	}

	/* The core packages, in one change, recorded beforehand: applying it
	 * restarts the system. */
	for (int i = 0; i < k.ntools; i++) {
		if (!k.core[i])
			continue;
		if (core_installed(k.tools[i])) {
			ok++;
			continue;
		}
		argv[argc++] = k.tools[i];
	}
	if (argc > 3) {
		write_record(name, record, nrecord);
		for (int i = 3; i < argc; i++)
			snprintf(record[nrecord++], 64, "%s", argv[i]);
		write_record(name, record, nrecord);
		printf("\n%d core packages (the others are already installed), in one change:\n",
		       argc - 3);
		fflush(stdout);
		argv[argc] = NULL;
		if (run(argv, NULL, 0, 0) == 0) {
			ok += argc - 3;
		} else {
			nrecord -= argc - 3;
			for (int i = 3; i < argc; i++) {
				size_t len = strlen(failed);

				snprintf(failed + len, sizeof(failed) - len, " %s", argv[i]);
				nfailed++;
			}
		}
	}
	if (ok > 0) {
		write_record(name, record, nrecord);
	} else {
		char path[PATH_MAX];

		snprintf(path, sizeof(path), "%s/%s", KIT_STATE, name);
		unlink(path);
	}
	printf("\nThe %s kit: %d of %d tools installed.\n", k.title, ok, k.ntools);
	if (nfailed)
		printf("Not installed (no source has them, or the install failed):%s\n", failed);
	return ok > 0 ? 0 : 1;
}

static int cmd_remove(const char *name)
{
	char tools[MAX_TOOLS][64], other[MAX_TOOLS][64], list[64][64], path[PATH_MAX];
	char *argv[MAX_TOOLS + 4] = { "wh", "--yes", "remove" };
	int n = installed_tools(name, tools, MAX_TOOLS), nkits = names(list, 64), removed = 0;
	int argc = 3;

	if (!valid_name(name) || n < 0)
		die("the kit '%s' is not installed", name);
	for (int i = 0; i < n; i++) {
		int shared = 0;

		/* Keep what another installed kit brought too. */
		for (int j = 0; j < nkits && !shared; j++) {
			int m;

			if (!strcmp(list[j], name))
				continue;
			m = installed_tools(list[j], other, MAX_TOOLS);
			for (int t = 0; t < m && !shared; t++)
				shared = !strcmp(other[t], tools[i]);
		}
		if (shared) {
			printf("Keeping %s (another kit has it).\n", tools[i]);
			continue;
		}
		if (core_installed(tools[i])) {
			argv[argc++] = tools[i];
			continue;
		}
		if (run((char *[]){ "wh", "--yes", "remove", tools[i], NULL }, NULL, 0, 0) == 0)
			removed++;
	}
	snprintf(path, sizeof(path), "%s/%s", KIT_STATE, name);
	unlink(path);
	printf("Removed the kit '%s' (%d tools).\n", name, removed);
	if (argc > 3) {
		/* Core packages: one change, which restarts the system. */
		printf("Removing %d core packages in one change:\n", argc - 3);
		fflush(stdout);
		argv[argc] = NULL;
		if (run(argv, NULL, 0, 0) != 0)
			warn("the core packages could not be removed (another package may "
			     "need one): %s", "wh remove PACKAGE removes them one by one");
	}
	return 0;
}

int cmd_kit(int argc, char **argv)
{
	if (argc == 0 || (argc == 1 && !strcmp(argv[0], "list")))
		return cmd_list();
	if (argc == 2 && !strcmp(argv[0], "show"))
		return cmd_show(argv[1]);
	if (argc == 2 && (!strcmp(argv[0], "install") || !strcmp(argv[0], "remove"))) {
		if (geteuid() != 0) {
			/* Editions without root (WormOS Amnesia) let users install. */
			if (access("/etc/wh/users-may-install", F_OK) == 0)
				execv("/usr/lib/wh/user", (char *[]){ "wh-user", "kit", argv[0],
								      argv[1], NULL });
			die("this needs root, try again with su%s", "");
		}
		return !strcmp(argv[0], "install") ? cmd_install(argv[1]) : cmd_remove(argv[1]);
	}
	return 2;
}
