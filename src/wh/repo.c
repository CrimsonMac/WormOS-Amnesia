/*
 * The package repository: a signed index and the packages it lists.
 *
 * "wh update" downloads index and index.sig into /var/lib/wh, checks the
 * Ed25519 signature against the key built into the system, and refuses an
 * index older than the one it already has (a mirror replaying an old index
 * could otherwise hold back security updates). The signature is checked
 * again every time the index is used, since /var is writable.
 *
 * Packages are downloaded into /var/cache/wh, named by their SHA-256, and
 * must match the size and SHA-256 the index gives for them.
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

#include "sha256.h"
#include "sig.h"
#include "wh.h"

extern const char *key_file;

const char *repo_url(const char *override)
{
	static char url[1024];
	char path[PATH_MAX];
	FILE *f;

	if (override) {
		snprintf(url, sizeof(url), "%s", override);
	} else {
		root_path(path, sizeof(path), WH_REPO_CONF);
		if (!(f = fopen(path, "re")))
			die("no repository configured in /%s", WH_REPO_CONF);
		url[0] = '\0';
		while (fgets(url, sizeof(url), f)) {
			char *t = trim(url);

			if (*t && *t != '#') {
				memmove(url, t, strlen(t) + 1);
				break;
			}
			url[0] = '\0';
		}
		fclose(f);
		if (!url[0])
			die("no repository configured in /%s", WH_REPO_CONF);
	}
	if (url[strlen(url) - 1] != '/' && strlen(url) + 1 < sizeof(url))
		strcat(url, "/");
	return url;
}

int download(const char *url, const char *dest)
{
	char *argv[] = { "curl", "-fsSL", "--retry", "2",
			 "--proto", "-all,https,http,file", "-o", (char *)dest,
			 (char *)url, NULL };

	return run(argv, NULL, 0) == 0 ? 0 : -1;
}

/*
 * Download several files with one curl, a few at a time over shared
 * connections: much faster than one curl (and one TLS handshake) per
 * file. Fails if any download fails; callers check every file anyway.
 */
int download_many(char *const *urls, char *const *dests, int n)
{
	static char *const head[] = { "curl", "-fsSL", "--retry", "2",
				      "--proto", "-all,https,http,file",
				      "--parallel", "--parallel-max", "6" };
	int nhead = sizeof(head) / sizeof(head[0]), a = 0, ret;
	char **argv;

	if (n == 0)
		return 0;
	if (!(argv = calloc(nhead + 3 * (size_t)n + 1, sizeof(*argv))))
		die("out of memory");
	for (int i = 0; i < nhead; i++)
		argv[a++] = head[i];
	for (int i = 0; i < n; i++) {
		argv[a++] = "-o";
		argv[a++] = dests[i];
		argv[a++] = urls[i];
	}
	argv[a] = NULL;
	ret = run(argv, NULL, 0) == 0 ? 0 : -1;
	free(argv);
	return ret;
}

int file_sha256(const char *path, char hex[65], long long *size)
{
	unsigned char buf[65536], sum[SHA256_LEN];
	struct sha256 ctx;
	long long n = 0;
	ssize_t r;
	int fd = open(path, O_RDONLY | O_CLOEXEC);

	if (fd < 0)
		return -1;
	sha256_init(&ctx);
	while ((r = read(fd, buf, sizeof(buf))) > 0) {
		sha256_update(&ctx, buf, r);
		n += r;
	}
	close(fd);
	if (r < 0)
		return -1;
	sha256_final(&ctx, sum);
	sha256_hex(sum, hex);
	if (size)
		*size = n;
	return 0;
}

static void load_key(uint8_t key[SIG_PUBLIC_LEN])
{
	char path[PATH_MAX];

	if (key_file)
		snprintf(path, sizeof(path), "%s", key_file);
	else
		root_path(path, sizeof(path), WH_KEY);
	if (read_hex_file(path, key, SIG_PUBLIC_LEN) < 0)
		die("cannot read the repository key %s: %s", path, strerror(errno));
}

int index_load(struct index *idx, const char *path)
{
	char line[1024];
	struct package *p = NULL;
	FILE *f = fopen(path, "re");
	int cap = 0;

	memset(idx, 0, sizeof(*idx));
	if (!f)
		return -1;
	while (fgets(line, sizeof(line), f)) {
		char *t = trim(line), *eq, *key, *val;

		if (!*t || *t == '#')
			continue;
		if (*t == '[') {
			char *end = strchr(t, ']');

			if (!end)
				continue;
			*end = '\0';
			if (idx->count == cap) {
				cap = cap ? cap * 2 : 64;
				idx->pkgs = realloc(idx->pkgs, cap * sizeof(*p));
				if (!idx->pkgs)
					die("out of memory");
			}
			p = &idx->pkgs[idx->count++];
			memset(p, 0, sizeof(*p));
			snprintf(p->name, sizeof(p->name), "%s", t + 1);
			continue;
		}
		if (!(eq = strchr(t, '=')))
			continue;
		*eq = '\0';
		key = trim(t);
		val = trim(eq + 1);
		if (!p) {
			if (strcmp(key, "date") == 0)
				idx->date = atoll(val);
			continue;
		}
		if (strcmp(key, "version") == 0)
			snprintf(p->version, sizeof(p->version), "%s", val);
		else if (strcmp(key, "file") == 0)
			snprintf(p->file, sizeof(p->file), "%s", val);
		else if (strcmp(key, "sha256") == 0)
			snprintf(p->sha256, sizeof(p->sha256), "%s", val);
		else if (strcmp(key, "size") == 0)
			p->size = atoll(val);
		else if (strcmp(key, "installed-size") == 0)
			p->installed_size = atoll(val);
		else if (strcmp(key, "depends") == 0)
			snprintf(p->depends, sizeof(p->depends), "%s", val);
		else if (strcmp(key, "recommends") == 0)
			snprintf(p->recommends, sizeof(p->recommends), "%s", val);
		else if (strcmp(key, "description") == 0)
			snprintf(p->description, sizeof(p->description), "%s", val);
		else if (strcmp(key, "delta") == 0 &&
			 sscanf(val, "%63s %127s %lld %64s", p->delta_from, p->delta_file,
				&p->delta_size, p->delta_sha256) != 4)
			p->delta_from[0] = '\0';
	}
	fclose(f);

	/* Package file names come from the index; never let them leave the repo. */
	for (int i = 0; i < idx->count; i++) {
		struct package *q = &idx->pkgs[i];

		if (strchr(q->file, '/') || q->file[0] == '.' || strlen(q->sha256) != 64)
			die("the package index has an invalid entry for %s", q->name);
		if (q->delta_from[0] && (strchr(q->delta_file, '/') || q->delta_file[0] == '.' ||
					 strlen(q->delta_sha256) != 64 || q->delta_size <= 0))
			q->delta_from[0] = '\0';
	}
	return 0;
}

void index_free(struct index *idx)
{
	free(idx->pkgs);
	memset(idx, 0, sizeof(*idx));
}

struct package *index_find(struct index *idx, const char *name)
{
	for (int i = 0; i < idx->count; i++)
		if (strcmp(idx->pkgs[i].name, name) == 0)
			return &idx->pkgs[i];
	return NULL;
}

int repo_update(const char *url)
{
	char state[PATH_MAX], cur[PATH_MAX + 16], cursig[PATH_MAX + 16];
	char new[PATH_MAX + 16], newsig[PATH_MAX + 16], src[2048];
	uint8_t key[SIG_PUBLIC_LEN];
	struct index fresh, old;

	load_key(key);
	root_path(state, sizeof(state), WH_STATE_DIR);
	if (mkdirs_at(AT_FDCWD, state, 0755) < 0)
		die("cannot create %s: %s", state, strerror(errno));
	snprintf(cur, sizeof(cur), "%s/index", state);
	snprintf(cursig, sizeof(cursig), "%s/index.sig", state);
	snprintf(new, sizeof(new), "%s/index.new", state);
	snprintf(newsig, sizeof(newsig), "%s/index.new.sig", state);

	snprintf(src, sizeof(src), "%sindex", url);
	if (download(src, new) < 0)
		die("cannot download %s", src);
	snprintf(src, sizeof(src), "%sindex.sig", url);
	if (download(src, newsig) < 0)
		die("cannot download %s", src);

	if (sig_verify_file(key, new, newsig) < 0) {
		unlink(new);
		unlink(newsig);
		die("the package index from %s has a bad signature, it was not used", url);
	}
	if (index_load(&fresh, new) < 0)
		die("cannot read the new package index");
	if (index_load(&old, cur) == 0 && fresh.date < old.date) {
		unlink(new);
		unlink(newsig);
		die("the package index from %s is older than the one already here; "
		    "refusing it (an outdated or tampered mirror?)", url);
	}
	index_free(&old);

	if (rename(newsig, cursig) < 0 || rename(new, cur) < 0)
		die("cannot store the package index: %s", strerror(errno));
	info("Package index updated: %d packages", fresh.count);
	index_free(&fresh);
	return 0;
}

int repo_load(struct index *idx)
{
	char path[PATH_MAX], sigpath[PATH_MAX];
	uint8_t key[SIG_PUBLIC_LEN];

	load_key(key);
	root_path(path, sizeof(path), WH_STATE_DIR "/index");
	root_path(sigpath, sizeof(sigpath), WH_STATE_DIR "/index.sig");
	if (access(path, F_OK) < 0)
		die("no package index yet, run 'wh update' first");
	if (sig_verify_file(key, path, sigpath) < 0)
		die("the stored package index has a bad signature; run 'wh update'");
	if (index_load(idx, path) < 0)
		die("cannot read the package index: %s", strerror(errno));
	return 0;
}

static void clean_cache(const char *cache, struct package **pkgs, int n);

/* Is path already the file with this SHA-256 and size? */
static int have_file(const char *path, const char *sha256, long long want)
{
	char hex[65];
	long long size;

	return file_sha256(path, hex, &size) == 0 && size == want && strcmp(hex, sha256) == 0;
}

struct job {
	char path[PATH_MAX + 128], part[PATH_MAX + 136], url[2048];
	const char *file, *sha256;
	long long size;
};

/* Download the jobs not done yet, all in one parallel curl, and check
 * each against the index. Returns the number that failed their check. */
static int fetch_jobs(struct job **jobs, int n)
{
	char **urls = calloc(n + 1, sizeof(char *)), **parts = calloc(n + 1, sizeof(char *));
	int m = 0, bad = 0;

	if (!urls || !parts)
		die("out of memory");
	for (int i = 0; i < n; i++) {
		urls[m] = jobs[i]->url;
		parts[m++] = jobs[i]->part;
	}
	download_many(urls, parts, m);  /* failures show in the checks below */
	for (int i = 0; i < n; i++) {
		struct job *j = jobs[i];

		if (!have_file(j->part, j->sha256, j->size)) {
			unlink(j->part);
			bad++;
			j->path[0] = '\0';
			continue;
		}
		if (rename(j->part, j->path) < 0)
			die("cannot store %s: %s", j->path, strerror(errno));
	}
	free(urls);
	free(parts);
	return bad;
}

/*
 * Download and check the packages of a plan, all at once over shared
 * connections. A package whose installed version is the one the
 * repository's delta was made from comes as that delta (only the files
 * that changed), unless the files it leaves out were changed here; then,
 * and for everything else, the full package. Files already in the cache
 * are used as they are. Returns the paths, in the order of pkgs.
 */
char **repo_fetch_all(const char *url, struct package **pkgs, int n)
{
	char cache[PATH_MAX], **files = calloc(n + 1, sizeof(char *));
	struct job *jobs = calloc(n ? n : 1, sizeof(*jobs));
	struct job **todo = calloc(n ? n : 1, sizeof(*todo));
	long long total = 0, full_total = 0;
	int ntodo = 0, ndelta = 0;

	if (!files || !jobs || !todo)
		die("out of memory");
	root_path(cache, sizeof(cache), WH_CACHE_DIR);
	if (mkdirs_at(AT_FDCWD, cache, 0755) < 0)
		die("cannot create %s: %s", cache, strerror(errno));
	clean_cache(cache, pkgs, n);

	/* Deltas first, where they apply. */
	for (int i = 0; i < n; i++) {
		struct package *p = pkgs[i], cur;
		struct job *j = &jobs[i];
		int use_delta = p->delta_from[0] && db_installed(p->name, &cur) &&
				strcmp(cur.version, p->delta_from) == 0;

		j->file = use_delta ? p->delta_file : p->file;
		j->sha256 = use_delta ? p->delta_sha256 : p->sha256;
		j->size = use_delta ? p->delta_size : p->size;
		snprintf(j->path, sizeof(j->path), "%s/%s.%s", cache, j->sha256,
			 use_delta ? "wdelta" : "wpkg");
		snprintf(j->part, sizeof(j->part), "%s.part", j->path);
		snprintf(j->url, sizeof(j->url), "%s%s", url, j->file);
		full_total += p->size;
		if (have_file(j->path, j->sha256, j->size))
			continue;
		todo[ntodo++] = j;
		total += j->size;
		ndelta += use_delta;
	}
	if (ntodo) {
		if (ndelta)
			info("Downloading %d package%s (%lld KiB; %d as changes only, %lld KiB "
			     "for the full packages)", ntodo, ntodo == 1 ? "" : "s",
			     (total + 1023) / 1024, ndelta, (full_total + 1023) / 1024);
		else
			info("Downloading %d package%s (%lld KiB)", ntodo, ntodo == 1 ? "" : "s",
			     (total + 1023) / 1024);
		fetch_jobs(todo, ntodo);
	}

	/* Then the full package wherever a delta failed or cannot be used. */
	ntodo = 0;
	for (int i = 0; i < n; i++) {
		struct package *p = pkgs[i];
		struct job *j = &jobs[i];
		int delta = j->file == p->delta_file;

		if (j->path[0] && (!delta || pkg_delta_usable(j->path, p->name) == 0))
			continue;
		if (delta && j->path[0]) {
			info("%s: files of the installed version were changed; "
			     "downloading the full package", p->name);
			unlink(j->path);
		}
		j->file = p->file;
		j->sha256 = p->sha256;
		j->size = p->size;
		snprintf(j->path, sizeof(j->path), "%s/%s.wpkg", cache, p->sha256);
		snprintf(j->part, sizeof(j->part), "%s.part", j->path);
		snprintf(j->url, sizeof(j->url), "%s%s", url, p->file);
		if (!have_file(j->path, j->sha256, j->size))
			todo[ntodo++] = j;
	}
	if (ntodo)
		fetch_jobs(todo, ntodo);

	for (int i = 0; i < n; i++) {
		if (!jobs[i].path[0])
			die("cannot download %s, or it does not match the package index "
			    "(corrupted or tampered); it was not used", jobs[i].url);
		if (!(files[i] = strdup(jobs[i].path)))
			die("out of memory");
	}
	free(jobs);
	free(todo);
	return files;
}

/*
 * Leftovers in the download cache (from interrupted changes, or from
 * releases before 1.11, which kept every package): everything but the
 * packages of this plan goes.
 */
static void clean_cache(const char *cache, struct package **pkgs, int n)
{
	char path[PATH_MAX * 2];
	struct dirent *d;
	DIR *dir;

	if (!(dir = opendir(cache)))
		return;
	while ((d = readdir(dir))) {
		const char *dot = strrchr(d->d_name, '.');
		int keep = 0;

		if (!dot || (strcmp(dot, ".wpkg") && strcmp(dot, ".wdelta") && strcmp(dot, ".part")))
			continue;
		for (int i = 0; i < n && !keep; i++)
			keep = !strncmp(d->d_name, pkgs[i]->sha256, 64) ||
			       (pkgs[i]->delta_from[0] && !strncmp(d->d_name, pkgs[i]->delta_sha256, 64));
		if (keep)
			continue;
		snprintf(path, sizeof(path), "%s/%s", cache, d->d_name);
		unlink(path);
	}
	closedir(dir);
}

/* Installed: the plan's downloads are not needed any more. */
void repo_remove_downloads(char **files)
{
	for (int i = 0; files && files[i]; i++)
		unlink(files[i]);
}
