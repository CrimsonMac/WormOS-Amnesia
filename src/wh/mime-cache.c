/*
 * mime-cache - write DIR/mimeinfo.cache for the desktop entries in DIR,
 * as update-desktop-database does: GLib finds the programs that open a
 * type of file or link (a PDF, https://) only through this cache, so
 * without it "open with", links clicked in programs and gio open found
 * nothing. Each line is TYPE=ENTRY;ENTRY; for the MimeType of every
 * entry, in the order of the entries' names. Hidden entries are left out.
 *
 * Usage: mime-cache DIR...   (a wh hook for /usr/share/applications; wh
 * runs it for /usr/local/share/applications after exporting entries)
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

struct pair {
	char *type;
	char *entry;
};

static int by_type(const void *a, const void *b)
{
	const struct pair *x = a, *y = b;
	int c = strcmp(x->type, y->type);

	return c ? c : strcmp(x->entry, y->entry);
}

static int by_name(const void *a, const void *b)
{
	return strcmp(*(char *const *)a, *(char *const *)b);
}

static int cache(const char *dir)
{
	struct pair *pairs = NULL;
	size_t npairs = 0, cap = 0;
	char **names = NULL, path[PATH_MAX], tmp[PATH_MAX];
	size_t nnames = 0;
	struct dirent *e;
	DIR *d;
	FILE *out;

	if (!(d = opendir(dir)))
		return errno == ENOENT ? 0 : -1;
	while ((e = readdir(d))) {
		size_t n = strlen(e->d_name);

		if (n > 8 && !strcmp(e->d_name + n - 8, ".desktop") && e->d_name[0] != '.') {
			names = realloc(names, (nnames + 1) * sizeof(*names));
			if (!names || !(names[nnames++] = strdup(e->d_name)))
				return -1;
		}
	}
	closedir(d);
	if (nnames)
		qsort(names, nnames, sizeof(*names), by_name);

	for (size_t i = 0; i < nnames; i++) {
		char line[8192], types[8192] = "";
		int main_group = 0, hidden = 0;
		FILE *f;

		snprintf(path, sizeof(path), "%s/%s", dir, names[i]);
		if (!(f = fopen(path, "re")))
			continue;
		while (fgets(line, sizeof(line), f)) {
			line[strcspn(line, "\r\n")] = '\0';
			if (line[0] == '[')
				main_group = !strcmp(line, "[Desktop Entry]");
			else if (main_group && !strncmp(line, "MimeType=", 9))
				snprintf(types, sizeof(types), "%s", line + 9);
			else if (main_group && !strcmp(line, "Hidden=true"))
				hidden = 1;
		}
		fclose(f);
		if (hidden)
			continue;
		for (char *save, *t = strtok_r(types, ";", &save); t; t = strtok_r(NULL, ";", &save)) {
			if (!*t || strchr(t, '=') || !strchr(t, '/'))
				continue;
			if (npairs == cap) {
				cap = cap ? cap * 2 : 256;
				if (!(pairs = realloc(pairs, cap * sizeof(*pairs))))
					return -1;
			}
			pairs[npairs].type = strdup(t);
			pairs[npairs].entry = names[i];
			if (!pairs[npairs].type)
				return -1;
			npairs++;
		}
	}
	if (npairs)
		qsort(pairs, npairs, sizeof(*pairs), by_type);

	snprintf(path, sizeof(path), "%s/mimeinfo.cache", dir);
	snprintf(tmp, sizeof(tmp), "%s/.mimeinfo.cache.new", dir);
	if (!(out = fopen(tmp, "we")))
		return -1;
	fputs("[MIME Cache]\n", out);
	for (size_t i = 0; i < npairs; i++) {
		if (i == 0 || strcmp(pairs[i].type, pairs[i - 1].type))
			fprintf(out, "%s%s=", i ? "\n" : "", pairs[i].type);
		/* The same entry twice for one type (a repeated MimeType). */
		if (i == 0 || strcmp(pairs[i].type, pairs[i - 1].type) ||
		    strcmp(pairs[i].entry, pairs[i - 1].entry))
			fprintf(out, "%s;", pairs[i].entry);
	}
	if (npairs)
		fputc('\n', out);
	if (fclose(out) != 0 || chmod(tmp, 0644) < 0 || rename(tmp, path) < 0) {
		unlink(tmp);
		return -1;
	}
	return 0;
}

int main(int argc, char **argv)
{
	int status = 0;

	for (int i = 1; i < argc; i++)
		if (cache(argv[i]) < 0) {
			fprintf(stderr, "mime-cache: %s: %s\n", argv[i], strerror(errno));
			status = 1;
		}
	return status;
}
