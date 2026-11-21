/*
 * Wormholes: software from other distributions (Debian, then Kali), each
 * in its own container under /var/lib/wormholes (the @wormholes subvolume,
 * mounted nosuid and nodev, so nothing in it can become root on the host).
 *
 * Sources are listed in /etc/wh/sources, in the order they are tried:
 *
 *     [debian]
 *     keyring = /usr/share/wh/keyrings/debian.pgp
 *     mirror = http://deb.debian.org/debian
 *     suite = trixie
 *     apt = deb http://deb.debian.org/debian trixie main
 *
 * Setting up a wormhole works like debootstrap: the suite's InRelease is
 * checked with gpgv against the distribution's archive keyring (only the
 * verified text is used), Packages is checked against the hash it lists,
 * the required packages and apt are downloaded and checked, unpacked, and
 * the distribution's own dpkg then installs them properly inside the
 * container. From then on its own apt manages it.
 *
 * Commands of the packages a user asks for are exported as small wrapper
 * scripts in /usr/local/bin (on /var), which run them through whc. Their
 * desktop entries and icons are exported to /usr/local/share, so the
 * programs appear in the launcher (see export_desktop()).
 */

#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#include <linux/btrfs.h>
#include <linux/openat2.h>

#include "wh.h"

#define SOURCES_CONF   "/etc/wh/sources"
#define WORMHOLES      "/var/lib/wormholes"
#define FOREIGN_DB     "/var/lib/wh/foreign"
#define SOURCE_CACHE   "/var/cache/wh/sources"
#define EXPORT_DIR     "/usr/local/bin"
#define EXPORT_SHARE   "/usr/local/share"
#define MAX_EXPORT_FILE (8 << 20)
#define ARCH           "amd64"
#define MAX_SOURCES    8
#define MAX_APT        8

struct source {
	char name[32];
	char keyring[PATH_MAX];
	char mirror[512];
	char suite[64];
	char apt[MAX_APT][512];
	int napt;
	char packages[PATH_MAX];        /* the checked package list, once fetched */
};

static struct source sources[MAX_SOURCES];
static int nsources = -1;

/* ---- configuration ---- */

static void load_sources(void)
{
	char line[1024];
	struct source *s = NULL;
	FILE *f;

	if (nsources >= 0)
		return;
	nsources = 0;
	if (!(f = fopen(SOURCES_CONF, "re")))
		return;
	while (fgets(line, sizeof(line), f)) {
		char *t = trim(line), *eq, *key, *val;

		if (!*t || *t == '#')
			continue;
		if (*t == '[') {
			char *end = strchr(t, ']');

			if (!end || nsources == MAX_SOURCES) {
				s = NULL;
				continue;
			}
			*end = '\0';
			s = &sources[nsources++];
			memset(s, 0, sizeof(*s));
			snprintf(s->name, sizeof(s->name), "%s", t + 1);
			continue;
		}
		if (!s || !(eq = strchr(t, '=')))
			continue;
		*eq = '\0';
		key = trim(t);
		val = trim(eq + 1);
		if (!strcmp(key, "keyring"))
			snprintf(s->keyring, sizeof(s->keyring), "%s", val);
		else if (!strcmp(key, "mirror"))
			snprintf(s->mirror, sizeof(s->mirror), "%s", val);
		else if (!strcmp(key, "suite"))
			snprintf(s->suite, sizeof(s->suite), "%s", val);
		else if (!strcmp(key, "apt") && s->napt < MAX_APT)
			snprintf(s->apt[s->napt++], sizeof(s->apt[0]), "%s", val);
	}
	fclose(f);
}

static struct source *find_source(const char *name)
{
	load_sources();
	for (int i = 0; i < nsources; i++)
		if (!strcmp(sources[i].name, name))
			return &sources[i];
	return NULL;
}

int foreign_source_known(const char *name)
{
	return find_source(name) != NULL;
}

static void wormhole_dir(char *out, size_t len, const char *name)
{
	snprintf(out, len, "%s/%s", WORMHOLES, name);
}

int wormhole_exists(const char *name)
{
	char dir[PATH_MAX], marker[PATH_MAX + 32];

	wormhole_dir(dir, sizeof(dir), name);
	snprintf(marker, sizeof(marker), "%s/etc/wormhole-ready", dir);
	return access(marker, F_OK) == 0;
}

/* Run a command inside a wormhole as root; out captures stdout if given. */
static int in_wormhole(const char *name, char *const cmd[], char *out, size_t len)
{
	char *argv[32];
	int n = 0;

	argv[n++] = "whc";
	argv[n++] = "--root";
	argv[n++] = (char *)name;
	for (int i = 0; cmd[i] && n < 31; i++)
		argv[n++] = cmd[i];
	argv[n] = NULL;
	return run(argv, out, len);
}

/* ---- the signed package lists ---- */

/* A package stanza from a Packages file. */
struct deb {
	char *name;
	char *version;
	char *depends;          /* Pre-Depends and Depends, comma separated */
	char *provides;
	char *filename;
	char *sha256;
	long long size;
	int required;           /* Priority: required or Essential: yes */
	int selected;
};

struct debs {
	struct deb *v;
	int n;
};

static char *dupstr(const char *s)
{
	char *d = strdup(s ? s : "");

	if (!d)
		die("out of memory");
	return d;
}

static void debs_free(struct debs *d)
{
	for (int i = 0; i < d->n; i++) {
		free(d->v[i].name);
		free(d->v[i].version);
		free(d->v[i].depends);
		free(d->v[i].provides);
		free(d->v[i].filename);
		free(d->v[i].sha256);
	}
	free(d->v);
	memset(d, 0, sizeof(*d));
}

static int parse_packages(const char *path, struct debs *out)
{
	char *line = NULL, *pre = NULL, *dep = NULL;
	size_t cap = 0;
	struct deb cur = { 0 };
	int alloc = 0;
	FILE *f = fopen(path, "re");

	memset(out, 0, sizeof(*out));
	if (!f)
		return -1;
	for (;;) {
		ssize_t n = getline(&line, &cap, f);
		char *val;

		if (n > 0 && line[n - 1] == '\n')
			line[--n] = '\0';
		if (n <= 0) {            /* end of a stanza */
			if (cur.name && cur.filename && cur.sha256) {
				char buf[8192];

				snprintf(buf, sizeof(buf), "%s%s%s", pre ? pre : "",
					 pre && dep ? ", " : "", dep ? dep : "");
				cur.depends = dupstr(buf);
				if (out->n == alloc) {
					alloc = alloc ? alloc * 2 : 4096;
					if (!(out->v = realloc(out->v, alloc * sizeof(cur))))
						die("out of memory");
				}
				out->v[out->n++] = cur;
			} else {
				free(cur.name);
				free(cur.version);
				free(cur.provides);
				free(cur.filename);
				free(cur.sha256);
			}
			free(pre);
			free(dep);
			pre = dep = NULL;
			memset(&cur, 0, sizeof(cur));
			if (n < 0)
				break;
			continue;
		}
		if (line[0] == ' ' || !(val = strchr(line, ':')))
			continue;
		*val++ = '\0';
		while (*val == ' ')
			val++;
		if (!strcmp(line, "Package"))
			cur.name = dupstr(val);
		else if (!strcmp(line, "Version"))
			cur.version = dupstr(val);
		else if (!strcmp(line, "Depends"))
			dep = dupstr(val);
		else if (!strcmp(line, "Pre-Depends"))
			pre = dupstr(val);
		else if (!strcmp(line, "Provides"))
			cur.provides = dupstr(val);
		else if (!strcmp(line, "Filename"))
			cur.filename = dupstr(val);
		else if (!strcmp(line, "SHA256"))
			cur.sha256 = dupstr(val);
		else if (!strcmp(line, "Size"))
			cur.size = atoll(val);
		else if (!strcmp(line, "Priority") && !strcmp(val, "required"))
			cur.required = 1;
		else if (!strcmp(line, "Essential") && !strcmp(val, "yes"))
			cur.required = 1;
	}
	free(line);
	fclose(f);
	return 0;
}

/*
 * Download the suite's package list for main into the cache, verified:
 * InRelease must carry a good signature from the source's keyring, and
 * Packages (xz, gz or plain, whichever is listed first in that order) must
 * match the SHA-256 the signed InRelease gives for it.
 * Leaves the decompressed list in out.
 */
static int fetch_packages(struct source *s, char *out, size_t len)
{
	char dir[PATH_MAX], inrel[PATH_MAX + 32], release[PATH_MAX + 32];
	char xz[PATH_MAX + 32], url[1024], want[80], hex[65], line[1024];
	char part[PATH_MAX + 40], stamp[PATH_MAX + 32];
	char member[64];
	static const char *const exts[] = { ".xz", ".gz", "" };
	static const char *const tools[] = { "xzcat", "zcat", "cat" };
	const char *ext = NULL, *tool = NULL;
	long long size, want_size = -1;
	FILE *f;

	/* Once per run: finding a package and setting up its wormhole both
	 * need the list. */
	if (s->packages[0]) {
		if ((size_t)snprintf(out, len, "%s", s->packages) >= len)
			die("path too long: %s", s->packages);
		return 0;
	}
	snprintf(dir, sizeof(dir), "%s/%s", SOURCE_CACHE, s->name);
	if (mkdirs_at(AT_FDCWD, dir, 0755) < 0)
		die("cannot create %s: %s", dir, strerror(errno));
	snprintf(inrel, sizeof(inrel), "%s/InRelease", dir);
	snprintf(release, sizeof(release), "%s/Release", dir);
	if ((size_t)snprintf(out, len, "%s/Packages", dir) >= len)
		die("path too long: %s", dir);

	snprintf(url, sizeof(url), "%s/dists/%s/InRelease", s->mirror, s->suite);
	if (download(url, inrel) < 0)
		die("cannot download %s", url);
	/*
	 * gpgv writes the verified text. It is named *.part because GnuPG 2.5
	 * otherwise writes FILE.part and leaves the final rename to gpg, which
	 * gpgv never does. Only a successful check counts.
	 */
	snprintf(part, sizeof(part), "%s.part", release);
	unlink(release);
	unlink(part);
	if (run((char *[]){ "gpgv", "-q", "--keyring", s->keyring, "--output", part,
			    inrel, NULL }, NULL, 0) != 0) {
		unlink(inrel);
		unlink(part);
		die("the %s package list has a bad signature, it was not used", s->name);
	}
	if (rename(part, release) < 0)
		die("gpgv did not write the verified %s release: %s", s->name, strerror(errno));

	/* Find main/binary-amd64/Packages* under SHA256 in the verified text. */
	want[0] = '\0';
	for (int e = 0; e < 3 && !want[0]; e++) {
		int in_sha = 0;

		snprintf(member, sizeof(member), "main/binary-%s/Packages%s", ARCH, exts[e]);
		if (!(f = fopen(release, "re")))
			die("cannot read %s", release);

		while (fgets(line, sizeof(line), f)) {
			char sum[80], name[256];
			long long sz;

			if (line[0] != ' ') {
				in_sha = !strncmp(line, "SHA256:", 7);
				continue;
			}
			if (in_sha && sscanf(line, " %79s %lld %255s", sum, &sz, name) == 3 &&
			    !strcmp(name, member)) {
				snprintf(want, sizeof(want), "%s", sum);
				want_size = sz;
				ext = exts[e];
				tool = tools[e];
			}
		}
		fclose(f);
	}
	if (!want[0])
		die("the %s release does not list main/binary-%s/Packages", s->name, ARCH);
	snprintf(xz, sizeof(xz), "%s/Packages.download%s", dir, ext);

	/* The list changes a few times a day; the copy from the last run is
	 * kept when it still matches the signed checksum. "Packages.sha256"
	 * records which compressed list the plain one came from. */
	snprintf(stamp, sizeof(stamp), "%s/Packages.sha256", dir);
	if (file_sha256(xz, hex, &size) == 0 && size == want_size && !strcmp(hex, want) &&
	    (f = fopen(stamp, "re"))) {
		char have[80] = "";

		if (!fgets(have, sizeof(have), f))
			have[0] = '\0';
		fclose(f);
		have[strcspn(have, "\n")] = '\0';
		if (!strcmp(have, want) && access(out, R_OK) == 0)
			goto done;
	}
	snprintf(url, sizeof(url), "%s/dists/%s/%s", s->mirror, s->suite, member);
	if (download(url, xz) < 0)
		die("cannot download %s", url);
	if (file_sha256(xz, hex, &size) < 0 || size != want_size || strcmp(hex, want)) {
		unlink(xz);
		die("the %s package list does not match its signed checksum", s->name);
	}
	{
		char cmd[3 * PATH_MAX];

		unlink(stamp);
		snprintf(cmd, sizeof(cmd), "%s '%s' > '%s.tmp' && mv '%s.tmp' '%s'",
			 tool, xz, out, out, out);
		if (run((char *[]){ "sh", "-c", cmd, NULL }, NULL, 0) != 0)
			die("cannot decompress the %s package list", s->name);
		if (!(f = fopen(stamp, "we")) || fprintf(f, "%s\n", want) < 0 || fclose(f) != 0)
			die("cannot write %s", stamp);
	}
done:
	snprintf(s->packages, sizeof(s->packages), "%s", out);
	return 0;
}

/*
 * Fetch a package list for a search or an update without ending the
 * command when that fails (offline, or a cut network): fetch_packages
 * runs in a child, where its die() only ends the child.
 */
static int try_fetch(struct source *s, char *path, size_t len)
{
	int status;
	pid_t pid;

	fflush(NULL);
	pid = fork();
	if (pid < 0)
		return -1;
	if (pid == 0) {
		fetch_packages(s, path, len);
		_exit(0);
	}
	while (waitpid(pid, &status, 0) < 0)
		if (errno != EINTR)
			return -1;
	return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : -1;
}

/* Is a package (or a package providing it) in the list? */
static struct deb *deb_find(struct debs *d, const char *name)
{
	for (int i = 0; i < d->n; i++)
		if (!strcmp(d->v[i].name, name))
			return &d->v[i];
	for (int i = 0; i < d->n; i++) {
		const char *p = d->v[i].provides;
		size_t n = strlen(name);

		while (p && (p = strstr(p, name))) {
			if ((p == d->v[i].provides || p[-1] == ' ' || p[-1] == ',') &&
			    (p[n] == '\0' || p[n] == ' ' || p[n] == ',' || p[n] == '('))
				return &d->v[i];
			p += n;
		}
	}
	return NULL;
}

/* Select a package and, recursively, what it depends on. */
static void deb_select(struct debs *d, struct deb *p)
{
	char *deps, *clause, *save;

	if (p->selected)
		return;
	p->selected = 1;
	deps = dupstr(p->depends);
	for (clause = strtok_r(deps, ",", &save); clause; clause = strtok_r(NULL, ",", &save)) {
		char *alt, *save2;
		struct deb *pick = NULL;

		/* "a (>= 1) | b": take the first alternative that exists. */
		for (alt = strtok_r(clause, "|", &save2); alt && !pick;
		     alt = strtok_r(NULL, "|", &save2)) {
			char name[128];

			if (sscanf(alt, " %127[^ (:]", name) == 1)
				pick = deb_find(d, name);
		}
		if (pick)
			deb_select(d, pick);
	}
	free(deps);
}

int foreign_has(const char *source, const char *pkg)
{
	char path[PATH_MAX], out[256];
	struct source *s = find_source(source);
	struct debs d;
	int found;

	if (!s)
		return 0;
	if (wormhole_exists(source))
		return in_wormhole(source, (char *[]){ "apt-cache", "show", "--no-all-versions",
						       (char *)pkg, NULL }, out, sizeof(out)) == 0;
	fetch_packages(s, path, sizeof(path));
	if (parse_packages(path, &d) < 0)
		die("cannot read the %s package list", source);
	found = deb_find(&d, pkg) != NULL;
	debs_free(&d);
	return found;
}

/* ---- setting up a wormhole ---- */

/* Unpack the data member of a .deb into the root. */
static int deb_unpack(const char *deb)
{
	char magic[8], hdr[60], name[17], tmp[PATH_MAX + 16];
	int fd = open(deb, O_RDONLY | O_CLOEXEC), ret = -1;

	if (fd < 0 || read(fd, magic, 8) != 8 || memcmp(magic, "!<arch>\n", 8)) {
		warn("%s is not a Debian package", deb);
		goto out;
	}
	while (read(fd, hdr, 60) == 60) {
		long long size = atoll(hdr + 48);
		char *slash;

		snprintf(name, sizeof(name), "%.16s", hdr);
		if ((slash = strchr(name, '/')))
			*slash = '\0';
		trim(name);
		if (!strncmp(name, "data.tar", 8)) {
			const char *tool = strstr(name, ".xz") ? "xzcat" :
					   strstr(name, ".zst") ? "zstd" :
					   strstr(name, ".gz") ? "zcat" : "cat";
			int out;

			/* The decompressors read from a file: copy the member out. */
			if ((size_t)snprintf(tmp, sizeof(tmp), "%s.data", deb) >= sizeof(tmp))
				goto out;
			out = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
			if (out < 0)
				goto out;
			while (size > 0) {
				char buf[65536];
				ssize_t r = read(fd, buf, size < (long long)sizeof(buf) ? size : (long long)sizeof(buf));

				if (r <= 0 || write(out, buf, r) != r) {
					close(out);
					goto out;
				}
				size -= r;
			}
			close(out);
			if (!strcmp(tool, "zstd"))
				ret = tar_unpack((char *[]){ "zstd", "-dcq", tmp, NULL });
			else
				ret = tar_unpack((char *[]){ (char *)tool, tmp, NULL });
			unlink(tmp);
			goto out;
		}
		if (lseek(fd, size + (size & 1), SEEK_CUR) < 0)
			goto out;
	}
	warn("%s has no data", deb);
out:
	if (fd >= 0)
		close(fd);
	return ret;
}

static int write_text(const char *dir, const char *rel, const char *text, int mode)
{
	char path[PATH_MAX + 128];
	FILE *f;

	snprintf(path, sizeof(path), "%s/%s", dir, rel);
	if (!(f = fopen(path, "we")))
		return -1;
	fputs(text, f);
	if (fclose(f) != 0)
		return -1;
	return chmod(path, mode);
}

static int subvol_create(const char *parent, const char *name)
{
	struct btrfs_ioctl_vol_args args = { 0 };
	int fd = open(parent, O_RDONLY | O_DIRECTORY | O_CLOEXEC), ret;

	if (fd < 0)
		return -1;
	snprintf(args.name, sizeof(args.name), "%s", name);
	ret = ioctl(fd, BTRFS_IOC_SUBVOL_CREATE, &args);
	close(fd);
	return ret;
}

static void subvol_remove(const char *parent, const char *name)
{
	struct btrfs_ioctl_vol_args args = { 0 };
	int fd = open(parent, O_RDONLY | O_DIRECTORY | O_CLOEXEC);

	if (fd < 0)
		return;
	snprintf(args.name, sizeof(args.name), "%s", name);
	ioctl(fd, BTRFS_IOC_SNAP_DESTROY, &args);
	close(fd);
}

/* The second stage, run by dpkg inside the new system. */
static const char stage2[] =
	"set -e\n"
	"cd /var/cache/wh-debs\n"
	"export DEBIAN_FRONTEND=noninteractive DEBCONF_NONINTERACTIVE_SEEN=true\n"
	"dpkg='dpkg --force-depends --force-confold --force-unsafe-io'\n"
	"$dpkg --install base-passwd_*.deb\n"
	"$dpkg --install base-files_*.deb\n"
	"$dpkg --unpack *.deb\n"
	"$dpkg --configure --pending || $dpkg --configure --pending\n"
	"dpkg --configure -a\n";

/* Copy a file, replacing to. */
static int copy_file(const char *from, const char *to, mode_t mode)
{
	char buf[65536];
	int in = open(from, O_RDONLY | O_CLOEXEC), out = -1, ret = -1;
	ssize_t r;

	if (in < 0)
		return -1;
	if ((out = open(to, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, mode)) < 0)
		goto out;
	while ((r = read(in, buf, sizeof(buf))) > 0)
		if (write(out, buf, r) != r)
			goto out;
	ret = r < 0 ? -1 : 0;
out:
	if (out >= 0 && close(out) < 0)
		ret = -1;
	close(in);
	return ret;
}

/*
 * apt would download the package list that wh has just checked once more:
 * give it the same files under the names apt uses (the mirror URL without
 * its scheme, slashes as underscores). apt then finds them current. If the
 * names do not match, apt simply downloads the list as before.
 */
static void seed_apt_lists(const struct source *s, const char *dir)
{
	char prefix[600], *p, from[PATH_MAX + 32], to[2 * PATH_MAX];
	static const char *const files[][2] = {
		{ "InRelease", "InRelease" },
		{ "Packages", "main_binary-" ARCH "_Packages" },
	};
	const char *host = strstr(s->mirror, "://");

	snprintf(prefix, sizeof(prefix), "%s_dists_%s_", host ? host + 3 : s->mirror, s->suite);
	for (p = prefix; *p; p++)
		if (*p == '/')
			*p = '_';
	for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
		snprintf(from, sizeof(from), "%s/%s/%s", SOURCE_CACHE, s->name, files[i][0]);
		snprintf(to, sizeof(to), "%s/var/lib/apt/lists/%s%s", dir, prefix, files[i][1]);
		if (copy_file(from, to, 0644) < 0)
			warn("cannot copy %s for apt", from);
	}
}

/* Set the wormhole up from the source's signed repository. */
int wormhole_setup(const char *name)
{
	char list[PATH_MAX], dir[PATH_MAX], newname[64], debs[PATH_MAX];
	char bindopt[2 * PATH_MAX], text[8192], olddir[PATH_MAX];
	struct source *s = find_source(name);
	struct debs d;
	long long total = 0;
	int count = 0, n = 0, saved_fd;
	const char *saved_root;

	if (!s)
		die("no source named '%s' in %s", name, SOURCES_CONF);
	if (!s->keyring[0] || !s->mirror[0] || !s->suite[0] || !s->napt)
		die("the source '%s' in %s is incomplete", name, SOURCES_CONF);

	info("Setting up the %s wormhole (%s %s)...", name, s->mirror, s->suite);
	fetch_packages(s, list, sizeof(list));
	if (parse_packages(list, &d) < 0)
		die("cannot read the %s package list", name);
	for (int i = 0; i < d.n; i++)
		if (d.v[i].required)
			deb_select(&d, &d.v[i]);
	if (!deb_find(&d, "apt"))
		die("the %s repository has no apt", name);
	deb_select(&d, deb_find(&d, "apt"));
	for (int i = 0; i < d.n; i++)
		if (d.v[i].selected) {
			count++;
			total += d.v[i].size;
		}
	info("Downloading %d packages (%lld MiB)", count, (total + (1 << 20) - 1) >> 20);

	snprintf(debs, sizeof(debs), "%s/%s/debs", SOURCE_CACHE, name);
	if (mkdirs_at(AT_FDCWD, debs, 0755) < 0)
		die("cannot create %s: %s", debs, strerror(errno));
	/* Everything not cached yet in one parallel download, then each file
	 * is checked against the signed list. */
	{
		char **urls = calloc(count + 1, sizeof(*urls));
		char **paths = calloc(count + 1, sizeof(*paths));
		struct deb **wanted = calloc(count + 1, sizeof(*wanted));

		if (!urls || !paths || !wanted)
			die("out of memory");
		for (int i = 0; i < d.n; i++) {
			struct deb *p = &d.v[i];
			char path[PATH_MAX + 512], url[1024], hex[65];
			char *base = strrchr(p->filename, '/');
			long long size;

			if (!p->selected)
				continue;
			base = base ? base + 1 : p->filename;
			if (strstr(p->filename, "..") || !*base)
				die("bad file name in the %s package list", name);
			snprintf(path, sizeof(path), "%s/%s", debs, base);
			if (file_sha256(path, hex, &size) == 0 && !strcmp(hex, p->sha256))
				continue;
			snprintf(url, sizeof(url), "%s/%s", s->mirror, p->filename);
			if (verbose)
				info("  %s %s", p->name, p->version);
			urls[n] = dupstr(url);
			paths[n] = dupstr(path);
			wanted[n++] = p;
		}
		if (download_many(urls, paths, n) < 0)
			warn("some downloads failed");
		for (int i = 0; i < n; i++) {
			char hex[65];
			long long size;

			if (file_sha256(paths[i], hex, &size) < 0 || strcmp(hex, wanted[i]->sha256)) {
				unlink(paths[i]);
				die("%s is missing or does not match the signed package list", urls[i]);
			}
			free(urls[i]);
			free(paths[i]);
		}
		free(urls);
		free(paths);
		free(wanted);
	}

	/* Build it in a new subvolume, renamed into place once complete. */
	if (mkdirs_at(AT_FDCWD, WORMHOLES, 0755) < 0)
		die("cannot create %s: %s", WORMHOLES, strerror(errno));
	snprintf(newname, sizeof(newname), "%s.new", name);
	wormhole_dir(dir, sizeof(dir), newname);
	subvol_remove(WORMHOLES, newname);
	if (subvol_create(WORMHOLES, newname) < 0)
		die("cannot create %s: %s", dir, strerror(errno));

	info("Unpacking");
	saved_root = root;
	saved_fd = root_fd;
	root = dir;
	if ((root_fd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC)) < 0)
		die("cannot open %s", dir);
	/* Merged /usr, as current Debian expects before anything is unpacked. */
	mkdirs_at(root_fd, "usr/bin", 0755);
	mkdirs_at(root_fd, "usr/sbin", 0755);
	mkdirs_at(root_fd, "usr/lib64", 0755);
	for (int i = 0; i < 4; i++) {
		static const char *const merged[][2] = {
			{ "usr/bin", "bin" }, { "usr/sbin", "sbin" },
			{ "usr/lib", "lib" }, { "usr/lib64", "lib64" },
		};

		if (symlinkat(merged[i][0], root_fd, merged[i][1]) < 0 && errno != EEXIST)
			die("cannot link /%s to %s: %s", merged[i][1], merged[i][0], strerror(errno));
	}
	mkdirs_at(root_fd, "usr/lib", 0755);
	for (int i = 0; i < d.n; i++) {
		char path[PATH_MAX + 512], *base;

		if (!d.v[i].selected)
			continue;
		base = strrchr(d.v[i].filename, '/');
		snprintf(path, sizeof(path), "%s/%s", debs, base ? base + 1 : d.v[i].filename);
		if (deb_unpack(path) < 0)
			die("cannot unpack %s", d.v[i].name);
	}
	close(root_fd);
	root = saved_root;
	root_fd = saved_fd;

	/* What dpkg needs to start from scratch, and the container's settings. */
	{
		static const char *const dirs[] = {
			"var/lib/dpkg/info", "var/lib/dpkg/updates", "etc/apt/keyrings",
			"etc/apt/apt.conf.d", "etc/dpkg/dpkg.cfg.d", "var/cache/wh-debs",
			"usr/local/share/fonts/wormos", NULL,
		};

		for (int i = 0; dirs[i]; i++) {
			if ((size_t)snprintf(olddir, sizeof(olddir), "%s/%s", dir, dirs[i]) >= sizeof(olddir) ||
			    mkdirs_at(AT_FDCWD, olddir, 0755) < 0)
				die("cannot create %s: %s", olddir, strerror(errno));
		}
	}
	write_text(dir, "var/lib/dpkg/status", "", 0644);
	write_text(dir, "var/lib/dpkg/available", "", 0644);
	write_text(dir, "usr/sbin/policy-rc.d",
		   "#!/bin/sh\n# Services are not started in a wormhole.\nexit 101\n", 0755);
	write_text(dir, "etc/hostname", "wormhole\n", 0644);
	write_text(dir, "etc/dpkg/dpkg.cfg.d/90wormhole",
		   "# Kept small: no documentation or translations.\n"
		   "path-exclude=/usr/share/doc/*\n"
		   "path-include=/usr/share/doc/*/copyright\n"
		   "path-exclude=/usr/share/man/*\n"
		   "path-exclude=/usr/share/info/*\n"
		   "path-exclude=/usr/share/locale/*\n"
		   "path-include=/usr/share/locale/locale.alias\n"
		   "# A container that is rebuilt rather than repaired: dpkg need not\n"
		   "# sync every file it writes.\n"
		   "force-unsafe-io\n", 0644);
	write_text(dir, "etc/apt/apt.conf.d/90wormhole",
		   "APT::Install-Recommends \"false\";\n"
		   "APT::Install-Suggests \"false\";\n"
		   "Acquire::Languages \"none\";\n", 0644);
	{
		char kr[PATH_MAX + 64];

		snprintf(kr, sizeof(kr), "%s/etc/apt/keyrings/wormhole.pgp", dir);
		if (copy_file(s->keyring, kr, 0644) < 0)
			die("cannot copy the %s keyring", name);
	}
	{
		size_t len = 0;

		len += snprintf(text + len, sizeof(text) - len,
				"# Set up by wh. Signed with the keyring wh verified.\n");
		for (int i = 0; i < s->napt; i++) {
			char type[16], rest[512];

			if (sscanf(s->apt[i], "%15s %511[^\n]", type, rest) == 2)
				len += snprintf(text + len, sizeof(text) - len,
						"%s [signed-by=/etc/apt/keyrings/wormhole.pgp] %s\n",
						type, rest);
		}
		write_text(dir, "etc/apt/sources.list", text, 0644);
	}

	info("Installing the base system inside (this takes a while)");
	snprintf(bindopt, sizeof(bindopt), "%s:/var/cache/wh-debs", debs);
	if (run((char *[]){ "whc", "--root", "--dir", dir, "--bind", bindopt, "--",
			    "/bin/sh", "-c", (char *)stage2, NULL }, NULL, 0) != 0) {
		warn("installing the base system failed; the partial wormhole is kept "
		     "in %s for inspection", dir);
		exit(1);
	}
	seed_apt_lists(s, dir);
	if (run((char *[]){ "whc", "--root", "--dir", dir, "--", "apt-get", "-q", "update", NULL },
		NULL, 0) != 0)
		die("apt-get update failed in the new wormhole");
	run((char *[]){ "whc", "--root", "--dir", dir, "--", "apt-get", "clean", NULL }, NULL, 0);
	{
		/* Remove documentation unpacked before the exclusions applied. */
		char cmd[PATH_MAX * 2];

		snprintf(cmd, sizeof(cmd),
			 "cd '%s/usr/share' && rm -rf man info && "
			 "find doc -type f ! -name copyright -delete 2>/dev/null; "
			 "find locale -mindepth 1 -maxdepth 1 ! -name locale.alias "
			 "-exec rm -rf {} + 2>/dev/null; true", dir);
		run((char *[]){ "sh", "-c", cmd, NULL }, NULL, 0);
	}
	write_text(dir, "etc/wormhole-ready", "", 0644);

	wormhole_dir(olddir, sizeof(olddir), name);
	if (rename(dir, olddir) < 0)
		die("cannot finish the %s wormhole: %s", name, strerror(errno));
	debs_free(&d);
	info("The %s wormhole is ready.", name);
	return 0;
}

/* ---- installed foreign packages ---- */

/* Record file: /var/lib/wh/foreign/PKG, "source = NAME" then "export = PATH". */
int foreign_installed(const char *pkg, char *source, size_t len)
{
	char path[PATH_MAX], line[PATH_MAX + 16];
	FILE *f;
	int found = 0;

	snprintf(path, sizeof(path), "%s/%s", FOREIGN_DB, pkg);
	if (!(f = fopen(path, "re")))
		return 0;
	while (fgets(line, sizeof(line), f))
		if (sscanf(line, "source = %31s", source) == 1)
			found = 1;
	(void)len;
	fclose(f);
	return found;
}

/* ---- desktop entries and icons ---- */

/* Paths wh may remove when a package goes (records are on /var). */
static int exported_path(const char *path)
{
	return !strncmp(path, EXPORT_DIR "/", strlen(EXPORT_DIR) + 1) ||
	       !strncmp(path, EXPORT_SHARE "/applications/", strlen(EXPORT_SHARE) + 14) ||
	       !strncmp(path, EXPORT_SHARE "/icons/", strlen(EXPORT_SHARE) + 7) ||
	       !strncmp(path, EXPORT_SHARE "/pixmaps/", strlen(EXPORT_SHARE) + 9);
}

/*
 * Read a file of the wormhole, resolving every link inside it: a package
 * cannot make wh read the host's files. Only regular files, up to 8 MiB.
 */
static char *read_inside(const char *source, const char *path, size_t *len)
{
	struct open_how how = {
		.flags = O_RDONLY | O_NOFOLLOW | O_CLOEXEC,
		.resolve = RESOLVE_IN_ROOT | RESOLVE_NO_MAGICLINKS,
	};
	char dir[PATH_MAX], *buf = NULL;
	struct stat st;
	int dfd, fd = -1;

	wormhole_dir(dir, sizeof(dir), source);
	if ((dfd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC)) < 0)
		return NULL;
	while (*path == '/')
		path++;
	fd = syscall(SYS_openat2, dfd, path, &how, sizeof(how));
	close(dfd);
	if (fd < 0)
		return NULL;
	if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_size <= MAX_EXPORT_FILE &&
	    (buf = malloc(st.st_size + 1))) {
		ssize_t n = 0, r;

		while (n < st.st_size && (r = read(fd, buf + n, st.st_size - n)) > 0)
			n += r;
		buf[n] = '\0';
		*len = n;
	}
	close(fd);
	return buf;
}

static int write_export(const char *dest, const char *data, size_t len)
{
	char dir[PATH_MAX], tmp[PATH_MAX + 16];
	char *slash;
	FILE *f;

	snprintf(dir, sizeof(dir), "%s", dest);
	if ((slash = strrchr(dir, '/')))
		*slash = '\0';
	mkdirs_at(AT_FDCWD, dir, 0755);
	snprintf(tmp, sizeof(tmp), "%s.wh-new", dest);
	if (!(f = fopen(tmp, "we")))
		return -1;
	if (fwrite(data, 1, len, f) != len || fclose(f) != 0 || chmod(tmp, 0644) < 0 ||
	    rename(tmp, dest) < 0) {
		unlink(tmp);
		return -1;
	}
	return 0;
}

static int word_is_exported(const char *cmd, char *const *cmds, int ncmds)
{
	for (int i = 0; i < ncmds; i++)
		if (strcmp(cmd, cmds[i]) == 0)
			return 1;
	return 0;
}

/*
 * A desktop entry for the host: only the [Desktop Entry] group, with Exec
 * pointing at the exported wrapper. Actions, TryExec and D-Bus activation
 * would refer to programs or services the host does not have. Returns
 * NULL if the entry's program was not exported.
 */
static char *host_entry(const char *text, char *const *cmds, int ncmds, size_t *len)
{
	char *out = malloc(strlen(text) + 256), *copy = strdup(text), *line, *save;
	int main_group = 0, has_exec = 0;
	size_t n = 0;

	if (!out || !copy)
		die("out of memory");
	for (line = strtok_r(copy, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
		if (line[0] == '[') {
			main_group = strcmp(line, "[Desktop Entry]") == 0;
			if (main_group)
				n += sprintf(out + n, "%s\n", line);
			continue;
		}
		if (!main_group || !strncmp(line, "Actions=", 8) ||
		    !strncmp(line, "TryExec=", 8) || !strncmp(line, "DBusActivatable=", 16))
			continue;
		if (!strncmp(line, "Exec=", 5)) {
			char *prog = line + 5, *end, *base, quote = 0;

			if (*prog == '"')
				quote = *prog++;
			end = prog + strcspn(prog, quote ? "\"" : " ");
			base = memrchr(prog, '/', end - prog);
			base = base ? base + 1 : prog;
			{
				char name[256];

				snprintf(name, sizeof(name), "%.*s", (int)(end - base), base);
				if (!word_is_exported(name, cmds, ncmds))
					break;
				n += sprintf(out + n, "Exec=%s%s\n", name,
					     *end && quote ? end + 1 : end);
			}
			has_exec = 1;
			continue;
		}
		n += sprintf(out + n, "%s\n", line);
	}
	free(copy);
	if (!has_exec || line) {        /* no Exec, or a program not exported */
		free(out);
		return NULL;
	}
	n += sprintf(out + n, "X-WormOS-Wormhole=true\n");
	*len = n;
	return out;
}

/*
 * Export the desktop entries and icons in the package's file list (list,
 * one path per line), for the commands just exported. Records each file.
 */
static void export_desktop(const char *source, const char *list, char *const *cmds,
			   int ncmds, FILE *record)
{
	char *copy = strdup(list), *line, *save;

	if (!copy)
		die("out of memory");
	for (line = strtok_r(copy, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
		char dest[PATH_MAX + 32], host[PATH_MAX + 32], *data, *entry;
		const char *rel = NULL;
		size_t len, elen;

		if (!strncmp(line, "/usr/share/applications/", 24) &&
		    !strchr(line + 24, '/') && strstr(line, ".desktop")) {
			snprintf(host, sizeof(host), "/usr/share/applications/%s", line + 24);
			if (access(host, F_OK) == 0 || !(data = read_inside(source, line, &len)))
				continue;
			entry = host_entry(data, cmds, ncmds, &elen);
			free(data);
			if (!entry)
				continue;
			snprintf(dest, sizeof(dest), EXPORT_SHARE "/applications/%s", line + 24);
			if (write_export(dest, entry, elen) == 0)
				fprintf(record, "export = %s\n", dest);
			free(entry);
			continue;
		}
		if (!strncmp(line, "/usr/share/icons/", 17))
			rel = line + 17;
		else if (!strncmp(line, "/usr/share/pixmaps/", 19))
			rel = line + 19;
		if (!rel || strstr(rel, "..") || !(data = read_inside(source, line, &len)))
			continue;
		snprintf(dest, sizeof(dest), EXPORT_SHARE "/%s/%s",
			 rel == line + 17 ? "icons" : "pixmaps", rel);
		if (write_export(dest, data, len) == 0)
			fprintf(record, "export = %s\n", dest);
		free(data);
	}
	free(copy);
}

/* Export the commands of pkg as wrappers. Writes the record. */
static void export_commands(const char *source, const char *pkg)
{
	static char out[1 << 20];
	char record[PATH_MAX], *line, *save, *list, *cmds[256];
	int ncmds = 0;
	FILE *f;

	if (in_wormhole(source, (char *[]){ "dpkg-query", "-L", (char *)pkg, NULL },
			out, sizeof(out)) != 0)
		out[0] = '\0';
	mkdirs_at(AT_FDCWD, FOREIGN_DB, 0755);
	mkdirs_at(AT_FDCWD, EXPORT_DIR, 0755);
	snprintf(record, sizeof(record), "%s/%s", FOREIGN_DB, pkg);
	if (!(f = fopen(record, "we")))
		die("cannot record %s: %s", pkg, strerror(errno));
	fprintf(f, "source = %s\n", source);

	if (!(list = strdup(out)))
		die("out of memory");
	for (line = strtok_r(out, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
		static const char *const bindirs[] = { "/usr/bin/", "/usr/sbin/", "/bin/",
						      "/sbin/", "/usr/games/", NULL };
		char inside[PATH_MAX + 64], host[PATH_MAX], wrapper[PATH_MAX + 64];
		const char *cmd = NULL;
		struct stat st;
		FILE *w;

		for (int i = 0; bindirs[i]; i++)
			if (!strncmp(line, bindirs[i], strlen(bindirs[i])) &&
			    !strchr(line + strlen(bindirs[i]), '/'))
				cmd = line + strlen(bindirs[i]);
		if (!cmd || !*cmd)
			continue;
		snprintf(inside, sizeof(inside), "%s/%s%s", WORMHOLES, source, line);
		if (stat(inside, &st) < 0 || !S_ISREG(st.st_mode) || !(st.st_mode & 0111))
			continue;
		/* Never shadow a command of the system itself. */
		snprintf(host, sizeof(host), "/usr/bin/%s", cmd);
		if (access(host, F_OK) == 0 ||
		    (snprintf(host, sizeof(host), "/usr/sbin/%s", cmd), access(host, F_OK) == 0)) {
			warn("not exporting %s: the system has its own", cmd);
			continue;
		}
		snprintf(wrapper, sizeof(wrapper), "%s/%s", EXPORT_DIR, cmd);
		if (access(wrapper, F_OK) == 0) {
			char owner[64] = "";

			/* Another package's export: leave it. */
			if (!(w = fopen(wrapper, "re")))
				continue;
			if (fgets(owner, sizeof(owner), w) && fgets(owner, sizeof(owner), w) &&
			    !strstr(owner, "Exported by wh")) {
				fclose(w);
				warn("not exporting %s: %s already exists", cmd, wrapper);
				continue;
			}
			fclose(w);
		}
		if (!(w = fopen(wrapper, "we")))
			die("cannot write %s: %s", wrapper, strerror(errno));
		fprintf(w, "#!/bin/sh\n# Exported by wh from the %s wormhole (package %s).\n"
			   "exec /usr/bin/whc %s %s \"$@\"\n", source, pkg, source, line);
		fclose(w);
		chmod(wrapper, 0755);
		fprintf(f, "export = %s\n", wrapper);
		info("  %s -> %s (%s)", cmd, line, source);
		if (ncmds < (int)(sizeof(cmds) / sizeof(cmds[0])) && !(cmds[ncmds++] = strdup(cmd)))
			die("out of memory");
	}
	export_desktop(source, list, cmds, ncmds, f);
	for (int i = 0; i < ncmds; i++)
		free(cmds[i]);
	free(list);
	fclose(f);
}

static void unexport(const char *pkg)
{
	char record[PATH_MAX], line[PATH_MAX + 16], path[PATH_MAX];
	FILE *f;

	snprintf(record, sizeof(record), "%s/%s", FOREIGN_DB, pkg);
	if (!(f = fopen(record, "re")))
		return;
	while (fgets(line, sizeof(line), f))
		if (sscanf(line, "export = %4095s", path) == 1 && exported_path(path))
			unlink(path);
	fclose(f);
	unlink(record);
}

int foreign_install(const char *source, const char *pkg)
{
	char existing[32];

	if (foreign_installed(pkg, existing, sizeof(existing)) && strcmp(existing, source))
		die("%s is already installed from %s", pkg, existing);
	if (!wormhole_exists(source))
		wormhole_setup(source);
	/* 32-bit packages: the wormhole's dpkg and apt learn about i386 once. */
	if (strstr(pkg, ":i386")) {
		char archs[256] = "";

		in_wormhole(source, (char *[]){ "dpkg", "--print-foreign-architectures", NULL },
			    archs, sizeof(archs));
		if (!strstr(archs, "i386")) {
			info("Adding 32-bit (i386) packages to the %s wormhole", source);
			if (in_wormhole(source, (char *[]){ "dpkg", "--add-architecture", "i386", NULL },
					NULL, 0) != 0 ||
			    in_wormhole(source, (char *[]){ "apt-get", "-q", "update", NULL }, NULL, 0) != 0)
				die("could not add i386 packages to the %s wormhole", source);
		}
	}
	info("Installing %s from %s", pkg, source);
	if (in_wormhole(source, (char *[]){ "apt-get", "-q", "-y", "install", (char *)pkg, NULL },
			NULL, 0) != 0)
		die("apt could not install %s", pkg);
	export_commands(source, pkg);
	return 0;
}

int foreign_remove(const char *pkg)
{
	char source[32];

	if (!foreign_installed(pkg, source, sizeof(source)))
		return -1;
	info("Removing %s from %s", pkg, source);
	if (in_wormhole(source, (char *[]){ "apt-get", "-q", "-y", "--autoremove", "remove",
					    (char *)pkg, NULL }, NULL, 0) != 0)
		die("apt could not remove %s", pkg);
	unexport(pkg);
	return 0;
}

/* Refresh (and with upgrade, upgrade) every wormhole that is set up. */
int foreign_refresh(int upgrade)
{
	load_sources();
	for (int i = 0; i < nsources; i++) {
		const char *name = sources[i].name;
		DIR *d;
		struct dirent *e;

		if (!wormhole_exists(name)) {
			char path[PATH_MAX];

			/* Keep a package list that searches use up to date. */
			snprintf(path, sizeof(path), "%s/%s/Packages", SOURCE_CACHE, name);
			if (!upgrade && access(path, F_OK) == 0) {
				info("Updating the %s package list", name);
				if (try_fetch(&sources[i], path, sizeof(path)) < 0)
					warn("the %s package list was not updated", name);
			}
			continue;
		}
		info("%s the %s wormhole", upgrade ? "Upgrading" : "Updating", name);
		if (in_wormhole(name, (char *[]){ "apt-get", "-q", "update", NULL }, NULL, 0) != 0)
			warn("apt-get update failed in the %s wormhole", name);
		if (upgrade &&
		    in_wormhole(name, (char *[]){ "apt-get", "-q", "-y", "upgrade", NULL }, NULL, 0) != 0)
			warn("apt-get upgrade failed in the %s wormhole", name);
		/* New versions can add or drop commands: export again. */
		if (upgrade && (d = opendir(FOREIGN_DB))) {
			while ((e = readdir(d))) {
				char src[32];

				if (e->d_name[0] != '.' &&
				    foreign_installed(e->d_name, src, sizeof(src)) &&
				    !strcmp(src, name)) {
					unexport(e->d_name);
					export_commands(name, e->d_name);
				}
			}
			closedir(d);
		}
	}
	return 0;
}

/* List foreign packages: "name version source" lines. */
void foreign_list(void)
{
	struct dirent *e;
	DIR *d = opendir(FOREIGN_DB);

	while (d && (e = readdir(d))) {
		char source[32], version[128] = "?";

		if (e->d_name[0] == '.' || !foreign_installed(e->d_name, source, sizeof(source)))
			continue;
		if (in_wormhole(source, (char *[]){ "dpkg-query", "-W", "-f", "${Version}",
						    e->d_name, NULL }, version, sizeof(version)) != 0)
			snprintf(version, sizeof(version), "?");
		printf("%-20s %-16s [%s]\n", e->d_name, version, source);
	}
	if (d)
		closedir(d);
}

/* Search the wormholes that are set up. */
/* Search a source's checked package list by name (like apt-cache's --names-only). */
static int search_list(const char *path, const char *term, const char *source)
{
	char *line = NULL, name[256] = "", desc[512] = "";
	size_t cap = 0;
	int found = 0;
	FILE *f = fopen(path, "re");

	if (!f)
		return 0;
	for (;;) {
		ssize_t n = getline(&line, &cap, f);

		if (n > 0 && line[n - 1] == '\n')
			line[--n] = '\0';
		if (n <= 0) {            /* end of a stanza */
			/* Kali's lists leave descriptions to translation files. */
			if (name[0] && strcasestr(name, term)) {
				printf("%s [%s]\n    %s\n", name, source, desc[0] ? desc : "(no description)");
				found++;
			}
			name[0] = desc[0] = '\0';
			if (n < 0)
				break;
			continue;
		}
		if (!strncmp(line, "Package: ", 9))
			snprintf(name, sizeof(name), "%s", line + 9);
		else if (!strncmp(line, "Description: ", 13))
			snprintf(desc, sizeof(desc), "%s", line + 13);
	}
	free(line);
	fclose(f);
	return found;
}

/*
 * Packages in Debian and Kali. A source with a wormhole is searched with
 * its own apt; one without uses the checked package list that installs
 * download, which root fetches here when it is missing.
 */
int foreign_search(const char *term, const char *only)
{
	static char out[1 << 20];
	char missing[256] = "";
	int found = 0;

	load_sources();
	for (int i = 0; i < nsources; i++) {
		char *line, *save, path[PATH_MAX];

		if (only && strcmp(only, sources[i].name))
			continue;
		if (!wormhole_exists(sources[i].name)) {
			snprintf(path, sizeof(path), "%s/%s/Packages", SOURCE_CACHE, sources[i].name);
			if (access(path, R_OK) < 0 && geteuid() == 0) {
				info("Downloading the %s package list...", sources[i].name);
				try_fetch(&sources[i], path, sizeof(path));
			}
			if (access(path, R_OK) == 0)
				found += search_list(path, term, sources[i].name);
			else
				snprintf(missing + strlen(missing), sizeof(missing) - strlen(missing),
					 "%s%s", missing[0] ? " and " : "", sources[i].name);
			continue;
		}
		if (in_wormhole(sources[i].name, (char *[]){ "apt-cache", "search", "--names-only",
							      (char *)(*term ? term : "."), NULL },
				out, sizeof(out)) != 0)
			continue;
		for (line = strtok_r(out, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
			char *dash = strstr(line, " - ");

			if (!dash)
				continue;
			*dash = '\0';
			printf("%s [%s]\n    %s\n", line, sources[i].name, dash + 3);
			found++;
		}
	}
	if (missing[0])
		warn("%s not searched: their package lists are not downloaded yet "
		     "('wh search' as root downloads them)", missing);
	return found;
}

/* The first source (in order) that has pkg, or NULL. */
const char *foreign_find(const char *pkg)
{
	load_sources();
	/*
	 * NAME:i386 (32-bit libraries, wine32): the lists are of amd64
	 * packages, so the first source is used and its apt decides.
	 */
	if (strstr(pkg, ":i386"))
		return nsources ? sources[0].name : NULL;
	for (int i = 0; i < nsources; i++) {
		info("Looking for %s in %s...", pkg, sources[i].name);
		if (foreign_has(sources[i].name, pkg))
			return sources[i].name;
	}
	return NULL;
}
