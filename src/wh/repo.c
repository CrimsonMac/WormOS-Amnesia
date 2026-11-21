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
	}
	fclose(f);

	/* Package file names come from the index; never let them leave the repo. */
	for (int i = 0; i < idx->count; i++)
		if (strchr(idx->pkgs[i].file, '/') || idx->pkgs[i].file[0] == '.' ||
		    strlen(idx->pkgs[i].sha256) != 64)
			die("the package index has an invalid entry for %s",
			    idx->pkgs[i].name);
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

/* Download a package into the cache (if needed) and check it. */
int repo_fetch(const char *url, const struct package *p, char *out, size_t len)
{
	char cache[PATH_MAX], part[PATH_MAX + 8], src[2048], hex[65];
	long long size;

	root_path(cache, sizeof(cache), WH_CACHE_DIR);
	if (mkdirs_at(AT_FDCWD, cache, 0755) < 0)
		die("cannot create %s: %s", cache, strerror(errno));
	if ((size_t)snprintf(out, len, "%s/%s.wpkg", cache, p->sha256) >= len)
		die("path too long");

	if (file_sha256(out, hex, &size) == 0 && strcmp(hex, p->sha256) == 0)
		return 0;

	snprintf(part, sizeof(part), "%s.part", out);
	snprintf(src, sizeof(src), "%s%s", url, p->file);
	info("Downloading %s %s (%lld KiB)", p->name, p->version, (p->size + 1023) / 1024);
	if (download(src, part) < 0) {
		unlink(part);
		die("cannot download %s", src);
	}
	if (file_sha256(part, hex, &size) < 0 || size != p->size ||
	    strcmp(hex, p->sha256) != 0) {
		unlink(part);
		die("%s does not match the package index (corrupted or tampered), "
		    "it was not used", p->file);
	}
	if (rename(part, out) < 0)
		die("cannot store %s: %s", out, strerror(errno));
	return 0;
}
