/*
 * Hooks: programs that rebuild caches after a transaction, such as the
 * compiled GSettings schemas, icon theme caches or the font cache. Several
 * packages add files to one cache, so no single package can ship it.
 *
 * A package that provides such a program also ships a hook,
 * usr/share/wh/hooks/NAME.hook:
 *
 *     # Compile the GSettings schemas.
 *     path = usr/share/glib-2.0/schemas
 *     exec = /usr/bin/glib-compile-schemas /usr/share/glib-2.0/schemas
 *
 * After a transaction that installed or removed anything beneath one of the
 * hook's paths (there may be several), or installed the hook itself, wh
 * runs exec. Words starting with '/' are paths inside the root, and the
 * program runs through the root's own dynamic loader and libraries, so the
 * same hook works on the staged copy of a running system and on an image
 * assembled with --root on the build machine.
 */

#define _GNU_SOURCE
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wh.h"

#define HOOK_DIR   "usr/share/wh/hooks"
#define LOADER     "/usr/lib/ld-linux-x86-64.so.2"
#define MAX_PATHS  8
#define MAX_WORDS  32

static char **touched;
static int ntouched, cap_touched;

void hook_touch(const char *path)
{
	if (ntouched == cap_touched) {
		cap_touched = cap_touched ? 2 * cap_touched : 1024;
		touched = realloc(touched, cap_touched * sizeof(*touched));
		if (!touched)
			die("out of memory");
	}
	if (!(touched[ntouched++] = strdup(path)))
		die("out of memory");
}

/* Was anything beneath dir (a path relative to the root) touched? */
static int touched_below(const char *dir)
{
	size_t n = strlen(dir);

	for (int i = 0; i < ntouched; i++)
		if (strncmp(touched[i], dir, n) == 0 &&
		    (touched[i][n] == '\0' || touched[i][n] == '/'))
			return 1;
	return 0;
}

static void run_hook(const char *name, char *exec)
{
	char *words[MAX_WORDS], *argv[MAX_WORDS + 4], abs[MAX_WORDS][PATH_MAX];
	char loader[PATH_MAX], libs[PATH_MAX], share[PATH_MAX], *preload, *data;
	int nwords = 0, argc = 0;

	for (char *w = strtok(exec, " \t"); w; w = strtok(NULL, " \t")) {
		if (nwords == MAX_WORDS) {
			warn("hook %s: too many words in exec", name);
			return;
		}
		words[nwords++] = w;
	}
	if (nwords == 0 || words[0][0] != '/') {
		warn("hook %s: exec must start with an absolute path", name);
		return;
	}
	root_path(loader, sizeof(loader), &LOADER[1]);
	root_path(libs, sizeof(libs), "usr/lib");
	argv[argc++] = loader;
	argv[argc++] = "--library-path";
	argv[argc++] = libs;
	for (int i = 0; i < nwords; i++) {
		if (words[i][0] == '/') {
			root_path(abs[i], sizeof(abs[i]), words[i] + 1);
			argv[argc++] = abs[i];
		} else {
			argv[argc++] = words[i];
		}
	}
	argv[argc] = NULL;
	if (verbose)
		info("Running hook %s", name);
	/*
	 * The root's loader cannot load libraries preloaded for this process
	 * from another system (fakeroot while an image is assembled; the
	 * files are given their owner afterwards).
	 */
	preload = getenv("LD_PRELOAD");
	if (preload && !(preload = strdup(preload)))
		die("out of memory");
	unsetenv("LD_PRELOAD");
	/* Data is looked up in the root being changed. */
	data = getenv("XDG_DATA_DIRS");
	if (data && !(data = strdup(data)))
		die("out of memory");
	root_path(share, sizeof(share), "usr/share");
	setenv("XDG_DATA_DIRS", share, 1);
	if (run(argv, NULL, 0) != 0)
		warn("hook %s failed", name);
	if (preload) {
		setenv("LD_PRELOAD", preload, 1);
		free(preload);
	}
	if (data) {
		setenv("XDG_DATA_DIRS", data, 1);
		free(data);
	} else {
		unsetenv("XDG_DATA_DIRS");
	}
}

void hooks_run(void)
{
	char dir[PATH_MAX];
	struct dirent **list;
	int n;

	if (ntouched == 0)
		return;
	root_path(dir, sizeof(dir), HOOK_DIR);
	n = scandir(dir, &list, NULL, alphasort);
	if (n < 0)
		return;
	for (int i = 0; i < n; i++) {
		char *paths[MAX_PATHS], exec[1024] = "", self[PATH_MAX], line[1024];
		const char *name = list[i]->d_name;
		size_t len = strlen(name);
		int npaths = 0, due;
		FILE *f;

		if (len < 6 || strcmp(name + len - 5, ".hook") != 0) {
			free(list[i]);
			continue;
		}
		snprintf(self, sizeof(self), HOOK_DIR "/%s", name);
		if (!(f = fdopen(open_in_root(self, O_RDONLY), "r"))) {
			warn("cannot read the hook %s", name);
			free(list[i]);
			continue;
		}
		while (fgets(line, sizeof(line), f)) {
			char *key = trim(line), *value = strchr(key, '=');

			if (*key == '#' || !value)
				continue;
			*value++ = '\0';
			key = trim(key);
			value = trim(value);
			value[strcspn(value, "\n")] = '\0';
			if (strcmp(key, "path") == 0 && npaths < MAX_PATHS)
				paths[npaths++] = strdup(value);
			else if (strcmp(key, "exec") == 0)
				snprintf(exec, sizeof(exec), "%s", value);
		}
		fclose(f);

		due = touched_below(self);
		for (int j = 0; j < npaths; j++) {
			due |= touched_below(paths[j]);
			free(paths[j]);
		}
		if (due && *exec)
			run_hook(name, exec);
		free(list[i]);
	}
	free(list);
	for (int i = 0; i < ntouched; i++)
		free(touched[i]);
	ntouched = 0;
}
