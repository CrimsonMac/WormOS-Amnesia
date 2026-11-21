/*
 * The database of installed packages, in /usr/lib/wh/db/NAME/ inside the
 * root, so it always matches the system it describes (and rewinds with it):
 *
 *     info    the package's .PKGINFO
 *     files   every path the package installed, one per line, prefixed by
 *             its type: f (file), d (directory), l (symlink), h (hard link)
 */

#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "sig.h"
#include "wh.h"

#define OWNER_BUCKETS  (1 << 15)

struct owner {
	struct owner *next;
	char *path;
	char *pkg;
	char type;
};

static struct owner **owners;   /* loaded on first use */

static void db_dir(char *out, size_t len, const char *name)
{
	char rel[PATH_MAX];

	snprintf(rel, sizeof(rel), "%s/%s", WH_DB_DIR, name ? name : "");
	root_path(out, len, rel);
}

/* A whole file as a string (caller frees), or NULL. */
static char *read_text(const char *path)
{
	size_t len;
	uint8_t *data = read_all(path, &len);
	char *text;

	if (!data)
		return NULL;
	if (!(text = realloc(data, len + 1)))
		die("out of memory");
	text[len] = '\0';
	return text;
}

int db_installed(const char *name, struct package *p)
{
	char dir[PATH_MAX], path[PATH_MAX + 8], *text;

	db_dir(dir, sizeof(dir), name);
	snprintf(path, sizeof(path), "%s/info", dir);
	if (!(text = read_text(path)))
		return 0;
	if (p && parse_pkginfo(text, p) < 0) {
		free(text);
		return 0;
	}
	free(text);
	return 1;
}

static int cmp_pkg(const void *a, const void *b)
{
	return strcmp(((const struct package *)a)->name, ((const struct package *)b)->name);
}

int db_list(struct package **out)
{
	char dir[PATH_MAX];
	struct package *list = NULL;
	struct dirent *e;
	int n = 0, cap = 0;
	DIR *d;

	db_dir(dir, sizeof(dir), NULL);
	*out = NULL;
	if (!(d = opendir(dir)))
		return 0;
	while ((e = readdir(d))) {
		if (e->d_name[0] == '.')
			continue;
		if (n == cap) {
			cap = cap ? cap * 2 : 64;
			if (!(list = realloc(list, cap * sizeof(*list))))
				die("out of memory");
		}
		if (db_installed(e->d_name, &list[n]))
			n++;
	}
	closedir(d);
	if (n)
		qsort(list, n, sizeof(*list), cmp_pkg);
	*out = list;
	return n;
}

static int write_file(const char *path, const char *data)
{
	char tmp[PATH_MAX + 8];
	FILE *f;
	int ok;

	snprintf(tmp, sizeof(tmp), "%s.tmp", path);
	if (!(f = fopen(tmp, "we")))
		return -1;
	ok = fputs(data, f) >= 0;
	ok &= fclose(f) == 0;
	if (!ok || rename(tmp, path) < 0) {
		unlink(tmp);
		return -1;
	}
	return 0;
}

static void owners_free(void)
{
	if (!owners)
		return;
	for (int i = 0; i < OWNER_BUCKETS; i++)
		for (struct owner *o = owners[i], *next; o; o = next) {
			next = o->next;
			free(o->path);
			free(o->pkg);
			free(o);
		}
	free(owners);
	owners = NULL;
}

int db_write(const char *name, const char *pkginfo, FILE *files)
{
	char dir[PATH_MAX], path[PATH_MAX + 8], *list = NULL;
	size_t listlen = 0;
	FILE *mem;
	int ret;

	db_dir(dir, sizeof(dir), name);
	if (mkdirs_at(AT_FDCWD, dir, 0755) < 0)
		return -1;

	/* Copy the file list from the stream given. */
	if (!(mem = open_memstream(&list, &listlen)))
		return -1;
	{
		char buf[8192];
		size_t n;

		rewind(files);
		while ((n = fread(buf, 1, sizeof(buf), files)) > 0)
			fwrite(buf, 1, n, mem);
	}
	fclose(mem);

	snprintf(path, sizeof(path), "%s/files", dir);
	ret = write_file(path, list);
	free(list);
	snprintf(path, sizeof(path), "%s/info", dir);
	if (ret == 0)
		ret = write_file(path, pkginfo);
	owners_free();
	return ret;
}

int db_remove(const char *name)
{
	char dir[PATH_MAX], path[PATH_MAX + 8];

	db_dir(dir, sizeof(dir), name);
	snprintf(path, sizeof(path), "%s/files", dir);
	unlink(path);
	snprintf(path, sizeof(path), "%s/info", dir);
	unlink(path);
	owners_free();
	return rmdir(dir);
}

int db_files(const char *name, char ***paths, int *count)
{
	char dir[PATH_MAX], path[PATH_MAX + 8], *line = NULL;
	size_t cap = 0;
	int n = 0, alloc = 0;
	ssize_t len;
	FILE *f;

	*paths = NULL;
	*count = 0;
	db_dir(dir, sizeof(dir), name);
	snprintf(path, sizeof(path), "%s/files", dir);
	if (!(f = fopen(path, "re")))
		return -1;
	while ((len = getline(&line, &cap, f)) > 0) {
		if (line[len - 1] == '\n')
			line[--len] = '\0';
		if (len < 3)
			continue;
		if (n == alloc) {
			alloc = alloc ? alloc * 2 : 256;
			if (!(*paths = realloc(*paths, alloc * sizeof(char *))))
				die("out of memory");
		}
		if (!((*paths)[n++] = strdup(line)))
			die("out of memory");
	}
	free(line);
	fclose(f);
	*count = n;
	return 0;
}

/* Drop path from the file list of package name (another package takes it
 * over). */
int db_disown(const char *name, const char *path)
{
	char dir[PATH_MAX], file[PATH_MAX + 8], **paths, *list = NULL;
	size_t listlen = 0;
	int count, ret;
	FILE *mem;

	if (db_files(name, &paths, &count) < 0)
		return -1;
	if (!(mem = open_memstream(&list, &listlen)))
		die("out of memory");
	for (int i = 0; i < count; i++)
		if (strcmp(paths[i] + 2, path) != 0)
			fprintf(mem, "%s\n", paths[i]);
	fclose(mem);
	db_free_files(paths, count);
	db_dir(dir, sizeof(dir), name);
	snprintf(file, sizeof(file), "%s/files", dir);
	ret = write_file(file, list);
	free(list);
	owners_free();
	return ret;
}

/* Add path (of the given type: f, l, h, d) to the file list of package name. */
int db_adopt(const char *name, char type, const char *path)
{
	char dir[PATH_MAX], file[PATH_MAX + 8];
	FILE *f;
	int ret;

	db_dir(dir, sizeof(dir), name);
	snprintf(file, sizeof(file), "%s/files", dir);
	if (!(f = fopen(file, "ae")))
		return -1;
	fprintf(f, "%c %s\n", type, path);
	ret = fclose(f);
	owners_free();
	return ret;
}

/*
 * Declined packages: ones the user removed. A package that recommends
 * them does not bring them back (WH_DECLINED, one name per line, part of
 * the system so that snapshots and undo cover it).
 */
int db_declined(const char *name)
{
	char path[PATH_MAX], line[NAME_MAX_LEN + 2];
	int found = 0;
	FILE *f;

	root_path(path, sizeof(path), WH_DECLINED);
	if (!(f = fopen(path, "re")))
		return 0;
	while (!found && fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\n")] = '\0';
		found = strcmp(line, name) == 0;
	}
	fclose(f);
	return found;
}

void db_decline(const char *name, int declined)
{
	char path[PATH_MAX], tmp[PATH_MAX + 8], line[NAME_MAX_LEN + 2];
	FILE *in, *out;

	if (db_declined(name) == declined)
		return;
	root_path(path, sizeof(path), WH_DECLINED);
	snprintf(tmp, sizeof(tmp), "%s.new", path);
	if (!(out = fopen(tmp, "we")))
		return;
	if ((in = fopen(path, "re"))) {
		while (fgets(line, sizeof(line), in)) {
			line[strcspn(line, "\n")] = '\0';
			if (*line && strcmp(line, name) != 0)
				fprintf(out, "%s\n", line);
		}
		fclose(in);
	}
	if (declined)
		fprintf(out, "%s\n", name);
	if (fclose(out) == 0)
		rename(tmp, path);
	else
		unlink(tmp);
}

void db_free_files(char **paths, int count)
{
	for (int i = 0; i < count; i++)
		free(paths[i]);
	free(paths);
}

static unsigned hash_path(const char *s)
{
	unsigned h = 2166136261u;

	while (*s)
		h = (h ^ (unsigned char)*s++) * 16777619u;
	return h & (OWNER_BUCKETS - 1);
}

static void owners_load(void)
{
	struct package *list;
	int n = db_list(&list);

	if (!(owners = calloc(OWNER_BUCKETS, sizeof(*owners))))
		die("out of memory");
	for (int i = 0; i < n; i++) {
		char **paths;
		int count;

		if (db_files(list[i].name, &paths, &count) < 0)
			continue;
		for (int j = 0; j < count; j++) {
			struct owner *o = malloc(sizeof(*o));
			unsigned h = hash_path(paths[j] + 2);

			if (!o || !(o->path = strdup(paths[j] + 2)) ||
			    !(o->pkg = strdup(list[i].name)))
				die("out of memory");
			o->type = paths[j][0];
			o->next = owners[h];
			owners[h] = o;
		}
		db_free_files(paths, count);
	}
	free(list);
}

/*
 * The package (other than except) that owns path as a file or link, or
 * NULL. Directories are shared between packages and have no single owner.
 */
char *db_owner(const char *path, const char *except)
{
	if (!owners)
		owners_load();
	for (struct owner *o = owners[hash_path(path)]; o; o = o->next)
		if (o->type != 'd' && strcmp(o->path, path) == 0 &&
		    (!except || strcmp(o->pkg, except) != 0))
			return strdup(o->pkg);
	return NULL;
}

/* Is the directory path used by a package other than except? */
int db_dir_used(const char *path, const char *except)
{
	if (!owners)
		owners_load();
	for (struct owner *o = owners[hash_path(path)]; o; o = o->next)
		if (o->type == 'd' && strcmp(o->path, path) == 0 &&
		    strcmp(o->pkg, except) != 0)
			return 1;
	return 0;
}
