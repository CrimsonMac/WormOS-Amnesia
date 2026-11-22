/*
 * Installing and removing packages in the root.
 *
 * A .wpkg is a zstd-compressed pax tar starting with .PKGINFO. It is read
 * as a stream from zstd and unpacked by our own tar reader, which:
 *
 *   - only accepts relative paths without "..",
 *   - resolves every parent directory inside the root (openat2 with
 *     RESOLVE_IN_ROOT), so symlinks in the root or in a package can never
 *     lead outside it,
 *   - checks each file against the SHA-256 listed in .PKGINFO,
 *   - writes files and links under a temporary name and renames them into
 *     place, owned by root.
 *
 * A path already owned by another package, or present but owned by none,
 * is a conflict and stops the installation. The one exception is a busybox
 * applet link: a package with the full program takes it over. On an upgrade, files the new
 * version no longer ships are removed afterwards.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/xattr.h>
#include <unistd.h>
#include <linux/capability.h>

#include "sha256.h"
#include "wh.h"

#define BLOCK  512

/* A set of strings (paths), with an optional value (a SHA-256). */
#define SET_BUCKETS  (1 << 14)

struct item {
	struct item *next;
	char *key;
	char *value;
	int seen;
};

struct set {
	struct item *b[SET_BUCKETS];
};

static unsigned hash_str(const char *s)
{
	unsigned h = 2166136261u;

	while (*s)
		h = (h ^ (unsigned char)*s++) * 16777619u;
	return h & (SET_BUCKETS - 1);
}

static struct item *set_get(struct set *s, const char *key)
{
	for (struct item *i = s->b[hash_str(key)]; i; i = i->next)
		if (strcmp(i->key, key) == 0)
			return i;
	return NULL;
}

static void set_add(struct set *s, const char *key, const char *value)
{
	struct item *i = calloc(1, sizeof(*i));
	unsigned h = hash_str(key);

	if (!i || !(i->key = strdup(key)) || (value && !(i->value = strdup(value))))
		die("out of memory");
	i->next = s->b[h];
	s->b[h] = i;
}

static void set_free(struct set *s)
{
	for (int n = 0; n < SET_BUCKETS; n++)
		for (struct item *i = s->b[n], *next; i; i = next) {
			next = i->next;
			free(i->key);
			free(i->value);
			free(i);
		}
	free(s);
}

static struct set *set_new(void)
{
	struct set *s = calloc(1, sizeof(*s));

	if (!s)
		die("out of memory");
	return s;
}

int parse_pkginfo(const char *text, struct package *p)
{
	char version[VERSION_MAX_LEN] = "", release[16] = "";
	const char *line = text;

	memset(p, 0, sizeof(*p));
	while (line && *line) {
		const char *end = strchr(line, '\n');
		size_t n = end ? (size_t)(end - line) : strlen(line);
		char buf[1024], *eq, *key, *val;

		snprintf(buf, sizeof(buf), "%.*s", (int)n, line);
		line = end ? end + 1 : NULL;
		if (!(eq = strchr(buf, '=')))
			continue;
		*eq = '\0';
		key = trim(buf);
		val = trim(eq + 1);
		if (strcmp(key, "name") == 0)
			snprintf(p->name, sizeof(p->name), "%s", val);
		else if (strcmp(key, "version") == 0)
			snprintf(version, sizeof(version), "%s", val);
		else if (strcmp(key, "release") == 0)
			snprintf(release, sizeof(release), "%s", val);
		else if (strcmp(key, "description") == 0)
			snprintf(p->description, sizeof(p->description), "%s", val);
		else if (strcmp(key, "depends") == 0)
			snprintf(p->depends, sizeof(p->depends), "%s", val);
		else if (strcmp(key, "recommends") == 0)
			snprintf(p->recommends, sizeof(p->recommends), "%s", val);
		else if (strcmp(key, "size") == 0)
			p->installed_size = atoll(val);
	}
	if (!p->name[0] || !version[0])
		return -1;
	snprintf(p->version, sizeof(p->version), "%s-%s", version,
		 release[0] ? release : "1");
	return 0;
}

/* ---- reading the tar stream ---- */

struct stream {
	int fd;
	pid_t pid;
};

static int read_full(struct stream *s, void *buf, size_t n)
{
	size_t done = 0;

	while (done < n) {
		ssize_t r = read(s->fd, (char *)buf + done, n - done);

		if (r < 0 && errno == EINTR)
			continue;
		if (r <= 0)
			return -1;
		done += r;
	}
	return 0;
}

static int skip(struct stream *s, unsigned long long n)
{
	char buf[BLOCK * 16];

	while (n > 0) {
		size_t take = n < sizeof(buf) ? n : sizeof(buf);

		if (read_full(s, buf, take) < 0)
			return -1;
		n -= take;
	}
	return 0;
}

static unsigned long long padded(unsigned long long n)
{
	return (n + BLOCK - 1) / BLOCK * BLOCK;
}

static unsigned long long octal(const char *p, size_t n)
{
	unsigned long long v = 0;

	while (n && (*p == ' ' || *p == '\0')) {
		p++;
		n--;
	}
	while (n-- && *p >= '0' && *p <= '7')
		v = v * 8 + (*p++ - '0');
	return v;
}

struct entry {
	char path[PATH_MAX];
	char link[PATH_MAX];
	char type;
	unsigned mode;
	unsigned long long size;
	long long mtime;
	unsigned uid, gid;
};

/* Read a pax extended header's records into e. */
static int read_pax(struct stream *s, unsigned long long size, struct entry *e,
		    int *have_path, int *have_link, int *have_size)
{
	char *buf;
	unsigned long long pos = 0;

	if (size > 1 << 20 || !(buf = malloc(padded(size) + 1)))
		return -1;
	if (read_full(s, buf, padded(size)) < 0) {
		free(buf);
		return -1;
	}
	buf[size] = '\0';
	while (pos < size) {
		char *rec = buf + pos, *sp = strchr(rec, ' '), *eq;
		unsigned long long len = strtoull(rec, NULL, 10);

		if (!sp || len == 0 || pos + len > size)
			break;
		rec[len - 1] = '\0';            /* the record's newline */
		if ((eq = strchr(sp + 1, '='))) {
			*eq = '\0';
			if (strcmp(sp + 1, "path") == 0) {
				snprintf(e->path, sizeof(e->path), "%s", eq + 1);
				*have_path = 1;
			} else if (strcmp(sp + 1, "linkpath") == 0) {
				snprintf(e->link, sizeof(e->link), "%s", eq + 1);
				*have_link = 1;
			} else if (strcmp(sp + 1, "size") == 0) {
				e->size = strtoull(eq + 1, NULL, 10);
				*have_size = 1;
			} else if (strcmp(sp + 1, "uid") == 0) {
				e->uid = strtoul(eq + 1, NULL, 10) + 1;   /* +1: set */
			} else if (strcmp(sp + 1, "gid") == 0) {
				e->gid = strtoul(eq + 1, NULL, 10) + 1;
			}
		}
		pos += len;
	}
	free(buf);
	return 0;
}

/* Read the next entry header. Returns 1 for an entry, 0 at the end. */
static int next_entry(struct stream *s, struct entry *e)
{
	unsigned char h[BLOCK];
	int have_path = 0, have_link = 0, have_size = 0;

	memset(e, 0, sizeof(*e));
	for (;;) {
		unsigned sum = 0;
		unsigned long long size;
		char type;

		if (read_full(s, h, BLOCK) < 0)
			return -1;
		if (h[0] == '\0') {
			for (int i = 0; i < BLOCK; i++)
				if (h[i])
					return -1;
			return 0;               /* end of archive */
		}
		for (int i = 0; i < BLOCK; i++)
			sum += (i >= 148 && i < 156) ? ' ' : h[i];
		if (sum != octal((char *)h + 148, 8))
			return -1;

		type = h[156];
		size = octal((char *)h + 124, 12);
		if (type == 'x') {
			if (read_pax(s, size, e, &have_path, &have_link, &have_size) < 0)
				return -1;
			continue;
		}
		if (type == 'g') {              /* global header: not used */
			if (skip(s, padded(size)) < 0)
				return -1;
			continue;
		}
		if (type == 'L' || type == 'K') {       /* GNU long name or link */
			char *dst = type == 'L' ? e->path : e->link;

			if (size >= PATH_MAX || read_full(s, dst, size) < 0 ||
			    skip(s, padded(size) - size) < 0)
				return -1;
			dst[size] = '\0';
			*(type == 'L' ? &have_path : &have_link) = 1;
			continue;
		}

		if (!have_path) {
			if (h[345] && memcmp(h + 257, "ustar", 5) == 0)
				snprintf(e->path, sizeof(e->path), "%.155s/%.100s",
					 (char *)h + 345, (char *)h);
			else
				snprintf(e->path, sizeof(e->path), "%.100s", (char *)h);
		}
		if (!have_link)
			snprintf(e->link, sizeof(e->link), "%.100s", (char *)h + 157);
		if (!have_size)
			e->size = size;
		e->type = type ? type : '0';
		e->mode = octal((char *)h + 100, 8) & 07777;
		e->mtime = octal((char *)h + 136, 12);
		/* Owner from a pax header (stored +1) wins over the ustar field. */
		e->uid = e->uid ? e->uid - 1 : octal((char *)h + 108, 8);
		e->gid = e->gid ? e->gid - 1 : octal((char *)h + 116, 8);
		return 1;
	}
}

/* Read the output of a decompressor (argv) as the tar stream. */
static int open_stream_cmd(char *const argv[], struct stream *s)
{
	int pipefd[2];

	if (pipe2(pipefd, O_CLOEXEC) < 0)
		return -1;
	s->pid = fork();
	if (s->pid < 0)
		return -1;
	if (s->pid == 0) {
		dup2(pipefd[1], STDOUT_FILENO);
		execvp(argv[0], argv);
		_exit(127);
	}
	close(pipefd[1]);
	s->fd = pipefd[0];
	return 0;
}

static int open_stream(const char *wpkg, struct stream *s)
{
	return open_stream_cmd((char *[]){ "zstd", "-dcq", (char *)wpkg, NULL }, s);
}

static int close_stream(struct stream *s)
{
	char buf[BLOCK * 16];
	int status;

	/*
	 * Read to the end: tar archives are padded past their end marker, and
	 * closing the pipe early could kill the decompressor with SIGPIPE
	 * while it still writes the padding, which looks like a failure.
	 */
	while (read(s->fd, buf, sizeof(buf)) > 0)
		;
	close(s->fd);
	while (waitpid(s->pid, &status, 0) < 0)
		if (errno != EINTR)
			return -1;
	return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : -1;
}

/* ---- paths ---- */

/* Clean a package path: strip "./" and trailing "/", refuse unsafe ones. */
static int clean_path(char *p)
{
	char *q;

	while (p[0] == '.' && p[1] == '/')
		memmove(p, p + 2, strlen(p + 2) + 1);
	q = p + strlen(p);
	while (q > p && q[-1] == '/')
		*--q = '\0';
	if (!*p || p[0] == '/')
		return -1;
	for (q = p; q; q = strchr(q, '/')) {
		if (*q == '/')
			q++;
		if (strncmp(q, "..", 2) == 0 && (q[2] == '/' || q[2] == '\0'))
			return -1;
	}
	return 0;
}

/* Open the parent directory of path inside the root; leaf points at the name. */
static int open_parent(const char *path, const char **leaf, int create)
{
	char parent[PATH_MAX];
	const char *slash = strrchr(path, '/');
	int fd;

	*leaf = slash ? slash + 1 : path;
	snprintf(parent, sizeof(parent), "%.*s", slash ? (int)(slash - path) : 0, path);
	fd = open_in_root(parent, O_RDONLY | O_DIRECTORY);
	if (fd < 0 && errno == ENOENT && create) {
		if (mkdirs_at(root_fd, parent, 0755) < 0)
			return -1;
		fd = open_in_root(parent, O_RDONLY | O_DIRECTORY);
	}
	return fd;
}

/* ---- capabilities, groups and users ---- */

static const char *const cap_names[] = {
	"chown", "dac_override", "dac_read_search", "fowner", "fsetid", "kill",
	"setgid", "setuid", "setpcap", "linux_immutable", "net_bind_service",
	"net_broadcast", "net_admin", "net_raw", "ipc_lock", "ipc_owner",
	"sys_module", "sys_rawio", "sys_chroot", "sys_ptrace", "sys_pacct",
	"sys_admin", "sys_boot", "sys_nice", "sys_resource", "sys_time",
	"sys_tty_config", "mknod", "lease", "audit_write", "audit_control",
	"setfcap", "mac_override", "mac_admin", "syslog", "wake_alarm",
	"block_suspend", "audit_read", "perfmon", "bpf", "checkpoint_restore",
};

/* Set file capabilities from text such as "cap_net_raw,cap_net_admin=ep". */
static int set_caps(const char *path, const char *text)
{
	struct vfs_cap_data data;
	char buf[512], *flags, *tok, *save;
	uint64_t bits = 0;
	int fd, ret;

	snprintf(buf, sizeof(buf), "%s", text);
	flags = strpbrk(buf, "=+");
	if (!flags)
		return -1;
	*flags++ = '\0';
	for (tok = strtok_r(buf, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
		size_t i, n = sizeof(cap_names) / sizeof(cap_names[0]);

		if (strncmp(tok, "cap_", 4) == 0)
			tok += 4;
		for (i = 0; i < n && strcmp(cap_names[i], tok) != 0; i++)
			;
		if (i == n) {
			warn("unknown capability '%s'", tok);
			return -1;
		}
		bits |= 1ULL << i;
	}

	memset(&data, 0, sizeof(data));
	data.magic_etc = VFS_CAP_REVISION_2 | (strchr(flags, 'e') ? VFS_CAP_FLAGS_EFFECTIVE : 0);
	if (strchr(flags, 'p')) {
		data.data[0].permitted = bits & 0xffffffff;
		data.data[1].permitted = bits >> 32;
	}
	if (strchr(flags, 'i')) {
		data.data[0].inheritable = bits & 0xffffffff;
		data.data[1].inheritable = bits >> 32;
	}

	fd = open_in_root(path, O_RDONLY | O_NOFOLLOW);
	if (fd < 0)
		return -1;
	ret = fsetxattr(fd, "security.capability", &data, sizeof(data), 0);
	close(fd);
	return ret;
}

/*
 * Add a system group to /etc/group if it is missing. Returns its GID, or -1.
 * New system IDs are taken from 999 downwards.
 */
static int ensure_group(const char *name)
{
	char line[1024], used[1000] = { 0 };
	int gid = 0, fd;
	FILE *f;

	fd = open_in_root("etc/group", O_RDWR | O_APPEND);
	if (fd < 0 || !(f = fdopen(fd, "r+"))) {
		if (fd >= 0)
			close(fd);
		return -1;
	}
	while (fgets(line, sizeof(line), f)) {
		char *c1 = strchr(line, ':'), *c2;
		int g;

		if (!c1 || !(c2 = strchr(c1 + 1, ':')))
			continue;
		g = atoi(c2 + 1);
		if ((size_t)(c1 - line) == strlen(name) && strncmp(line, name, c1 - line) == 0) {
			fclose(f);
			return g;
		}
		if (g >= 0 && g < 1000)
			used[g] = 1;
	}
	for (gid = 999; gid >= 100 && used[gid]; gid--)
		;
	if (gid < 100) {
		fclose(f);
		return -1;
	}
	fprintf(f, "%s:x:%d:\n", name, gid);
	return fclose(f) == 0 ? gid : -1;
}

/*
 * Add a system user (and its group) for a daemon: no password, no shell,
 * no home. It gets its group's ID as UID when that is free.
 */
static int ensure_user(const char *name)
{
	char line[1024], used[1000] = { 0 };
	int gid = ensure_group(name), uid, fd;
	FILE *f;

	if (gid < 0)
		return -1;
	fd = open_in_root("etc/passwd", O_RDWR | O_APPEND);
	if (fd < 0 || !(f = fdopen(fd, "r+"))) {
		if (fd >= 0)
			close(fd);
		return -1;
	}
	while (fgets(line, sizeof(line), f)) {
		char *c1 = strchr(line, ':'), *c2;
		int u;

		if (!c1 || !(c2 = strchr(c1 + 1, ':')))
			continue;
		if ((size_t)(c1 - line) == strlen(name) && strncmp(line, name, c1 - line) == 0) {
			fclose(f);
			return 0;
		}
		u = atoi(c2 + 1);
		if (u >= 0 && u < 1000)
			used[u] = 1;
	}
	uid = gid;
	if (uid >= 1000 || used[uid])
		for (uid = 999; uid >= 100 && used[uid]; uid--)
			;
	if (uid < 100) {
		fclose(f);
		return -1;
	}
	fprintf(f, "%s:x:%d:%d:%s:/nonexistent:/usr/sbin/nologin\n", name, uid, gid, name);
	if (fclose(f) != 0)
		return -1;
	/* A locked password, so no login is possible even with a shell. */
	fd = open_in_root("etc/shadow", O_WRONLY | O_APPEND);
	if (fd < 0)
		return errno == ENOENT ? 0 : -1;
	snprintf(line, sizeof(line), "%s:!:1::::::\n", name);
	if (write(fd, line, strlen(line)) != (ssize_t)strlen(line)) {
		close(fd);
		return -1;
	}
	return close(fd);
}

/* ---- install ---- */

/*
 * The file this package installed at leaf, if it is still exactly what the
 * new version ships (content, mode, owner): opened read-only, or -1. For a
 * file a delta leaves out (e is NULL), its content and owner: the delta
 * holds every file whose content or mode changed.
 */
static int unchanged_file(int pfd, const char *leaf, const struct entry *e,
			  const char *expect)
{
	unsigned char buf[65536], sum[SHA256_LEN];
	char hex[SHA256_HEX];
	struct sha256 ctx;
	struct stat st;
	ssize_t n;
	int fd;

	if (!expect)
		return -1;
	fd = openat(pfd, leaf, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
	if (fd < 0)
		return -1;
	if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode) ||
	    (e && ((unsigned long long)st.st_size != e->size ||
		   (st.st_mode & 07777) != (e->mode & 07777))) ||
	    (geteuid() == 0 && (st.st_uid != 0 || st.st_gid != 0)))
		goto no;
	sha256_init(&ctx);
	while ((n = read(fd, buf, sizeof(buf))) > 0)
		sha256_update(&ctx, buf, n);
	if (n < 0)
		goto no;
	sha256_final(&ctx, sum);
	sha256_hex(sum, hex);
	if (strcmp(hex, expect) == 0)
		return fd;
no:
	close(fd);
	return -1;
}

/*
 * Write one regular file from the stream, checking its hash. On an upgrade,
 * a file the package already installed and that has not changed is left in
 * place (returns 1): nothing is written, and the system keeps sharing its
 * blocks with the snapshots taken before. Returns 0 when the file was
 * written, -1 on errors.
 */
static int extract_file(struct stream *s, const struct entry *e, int pfd,
			const char *leaf, const char *tmp, const char *expect, int owned)
{
	unsigned char buf[65536], sum[SHA256_LEN];
	struct timespec times[2] = { { 0, UTIME_OMIT }, { e->mtime, 0 } };
	unsigned long long left = e->size;
	struct sha256 ctx;
	char hex[SHA256_HEX];
	int fd;

	if (owned && (fd = unchanged_file(pfd, leaf, e, expect)) >= 0) {
		/* Still read (and check) the package's copy. */
		sha256_init(&ctx);
		while (left > 0) {
			size_t take = left < sizeof(buf) ? left : sizeof(buf);

			if (read_full(s, buf, take) < 0) {
				close(fd);
				return -1;
			}
			sha256_update(&ctx, buf, take);
			left -= take;
		}
		sha256_final(&ctx, sum);
		sha256_hex(sum, hex);
		if (strcmp(hex, expect) != 0) {
			close(fd);
			errno = EBADMSG;
			return -1;
		}
		/* Capabilities are set again afterwards from the new version. */
		if (fremovexattr(fd, "security.capability") < 0 && errno != ENODATA) {
			close(fd);
			return -1;
		}
		futimens(fd, times);
		close(fd);
		return skip(s, padded(e->size) - e->size) < 0 ? -1 : 1;
	}

	unlinkat(pfd, tmp, 0);
	fd = openat(pfd, tmp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
	if (fd < 0)
		return -1;
	sha256_init(&ctx);
	while (left > 0) {
		size_t take = left < sizeof(buf) ? left : sizeof(buf);

		if (read_full(s, buf, take) < 0 || write(fd, buf, take) != (ssize_t)take) {
			close(fd);
			unlinkat(pfd, tmp, 0);
			return -1;
		}
		sha256_update(&ctx, buf, take);
		left -= take;
	}
	sha256_final(&ctx, sum);
	sha256_hex(sum, hex);
	if (!expect || strcmp(hex, expect) != 0) {
		close(fd);
		unlinkat(pfd, tmp, 0);
		errno = EBADMSG;
		return -1;
	}
	if ((geteuid() == 0 && fchown(fd, 0, 0) < 0) || fchmod(fd, e->mode) < 0 ||
	    futimens(fd, times) < 0 || close(fd) < 0) {
		unlinkat(pfd, tmp, 0);
		return -1;
	}
	if (renameat(pfd, tmp, pfd, leaf) < 0) {
		unlinkat(pfd, tmp, 0);
		return -1;
	}
	return skip(s, padded(e->size) - e->size) < 0 ? -1 : 0;
}

/* Is something at leaf that this package may not replace? */
static int check_conflict(const char *name, const char *path, int pfd,
			  const char *leaf, struct set *old)
{
	struct stat st;
	char *owner;

	if (fstatat(pfd, leaf, &st, AT_SYMLINK_NOFOLLOW) < 0)
		return 0;                       /* nothing there */
	if (S_ISDIR(st.st_mode)) {
		warn("%s: %s is a directory on this system", name, path);
		return -1;
	}
	if ((owner = db_owner(path, name)) && !strcmp(owner, "busybox") &&
	    S_ISLNK(st.st_mode)) {
		char target[64];
		ssize_t n = readlinkat(pfd, leaf, target, sizeof(target) - 1);

		if (n > 0) {
			target[n] = '\0';
			if (!strcmp(target, "busybox") || !strcmp(target, "../bin/busybox")) {
				free(owner);
				return db_disown("busybox", path) < 0 ? -1 : 0;
			}
		}
	}
	if (owner) {
		warn("%s: %s is already installed by %s", name, path, owner);
		free(owner);
		return -1;
	}
	if (!set_get(old, path)) {
		warn("%s: %s exists but belongs to no package", name, path);
		return -1;
	}
	return 0;
}

static int is_busybox_link(const char *target)
{
	return !strcmp(target, "busybox") || !strcmp(target, "../bin/busybox");
}

/*
 * BusyBox applets give way to full versions of the same commands (GNU grep,
 * coreutils...): another package took the link over (db_disown), so a new
 * busybox leaves it alone instead of failing on the conflict.
 */
static int busybox_yields(const char *name, const struct entry *e)
{
	char *owner;

	if (strcmp(name, "busybox") != 0 || !is_busybox_link(e->link))
		return 0;
	if (!(owner = db_owner(e->path, "busybox")))
		return 0;
	free(owner);
	return 1;
}

/*
 * The package that took over BusyBox applet links is going away: point
 * them at busybox again, so the commands do not disappear. The applets
 * busybox ships are listed in BUSYBOX_LINKS.
 */
#define BUSYBOX_LINKS "usr/share/busybox/links"

static void busybox_restore(char **paths, int count)
{
	char line[PATH_MAX];
	struct set *removed = set_new();
	FILE *f;

	if (!db_installed("busybox", NULL)) {
		set_free(removed);
		return;
	}
	for (int i = 0; i < count; i++)
		if (paths[i][0] != 'd')
			set_add(removed, paths[i] + 2, NULL);
	{
		int fd = open_in_root(BUSYBOX_LINKS, O_RDONLY | O_CLOEXEC);

		if (fd < 0 || !(f = fdopen(fd, "r"))) {
			if (fd >= 0)
				close(fd);
			set_free(removed);
			return;
		}
	}
	while (fgets(line, sizeof(line), f)) {
		const char *leaf, *target;
		int pfd;

		line[strcspn(line, "\n")] = '\0';
		if (!*line || !set_get(removed, line) || clean_path(line) < 0)
			continue;
		target = strncmp(line, "usr/sbin/", 9) == 0 ? "../bin/busybox" : "busybox";
		if ((pfd = open_parent(line, &leaf, 0)) < 0)
			continue;
		if (symlinkat(target, pfd, leaf) == 0)
			db_adopt("busybox", 'l', line);
		close(pfd);
	}
	fclose(f);
	set_free(removed);
}

static struct set *old_files(const char *name)
{
	struct set *old = set_new();
	char **paths;
	int count;

	if (db_files(name, &paths, &count) == 0) {
		for (int i = 0; i < count; i++)
			set_add(old, paths[i] + 2, paths[i]);
		db_free_files(paths, count);
	}
	return old;
}

/* Remove what the old version had and the new one does not. */
static void remove_leftovers(const char *name, struct set *now)
{
	char **paths;
	int count;

	if (db_files(name, &paths, &count) < 0)
		return;
	for (int i = count - 1; i >= 0; i--) {
		const char *path = paths[i] + 2, *leaf;
		char type = paths[i][0];
		int pfd;

		if (set_get(now, path))
			continue;
		if (type == 'd' && db_dir_used(path, name))
			continue;
		if ((pfd = open_parent(path, &leaf, 0)) < 0)
			continue;
		if (unlinkat(pfd, leaf, type == 'd' ? AT_REMOVEDIR : 0) == 0)
			hook_touch(path);
		close(pfd);
	}
	db_free_files(paths, count);
}

/*
 * Can this delta be installed here? It must have been made from the
 * installed version, and every file it leaves out must still be the one
 * that version installed. Otherwise the full package is used.
 */
int pkg_delta_usable(const char *wdelta, const char *expect_name)
{
	struct set *hashes = set_new(), *old = NULL;
	char *pkginfo = NULL, delta_from[VERSION_MAX_LEN] = "";
	struct package p, cur;
	struct stream s;
	struct entry e;
	int r, ok = 0;

	if (open_stream(wdelta, &s) < 0)
		goto out_free;
	if (next_entry(&s, &e) != 1 || strcmp(e.path, ".PKGINFO") != 0 ||
	    e.size > (1 << 24) || !(pkginfo = malloc(padded(e.size) + 1)) ||
	    read_full(&s, pkginfo, padded(e.size)) < 0)
		goto out;
	pkginfo[e.size] = '\0';
	if (parse_pkginfo(pkginfo, &p) < 0 || strcmp(p.name, expect_name) != 0)
		goto out;
	for (char *line = pkginfo; line && *line; ) {
		char *end = strchr(line, '\n'), sum[65], path[PATH_MAX];

		if (end)
			*end = '\0';
		if (sscanf(line, "file = %64s %4095[^\n]", sum, path) == 2)
			set_add(hashes, path, sum);
		else if (strncmp(line, "delta-from = ", 13) == 0)
			snprintf(delta_from, sizeof(delta_from), "%s", line + 13);
		line = end ? end + 1 : NULL;
	}
	if (!delta_from[0] || !db_installed(p.name, &cur) || strcmp(cur.version, delta_from) != 0)
		goto out;
	while ((r = next_entry(&s, &e)) == 1) {
		struct item *h = set_get(hashes, e.path);

		if (h)
			h->seen = 1;
		if (e.size && skip(&s, padded(e.size)) < 0)
			goto out;
	}
	if (r < 0)
		goto out;
	old = old_files(p.name);
	ok = 1;
	for (int i = 0; i < SET_BUCKETS && ok; i++)
		for (struct item *h = hashes->b[i]; h && ok; h = h->next) {
			struct item *was;
			const char *leaf;
			int pfd, fd = -1;

			if (h->seen)
				continue;
			if ((was = set_get(old, h->key)) && was->value && was->value[0] == 'f' &&
			    (pfd = open_parent(h->key, &leaf, 0)) >= 0) {
				fd = unchanged_file(pfd, leaf, NULL, h->value);
				close(pfd);
			}
			if (fd < 0)
				ok = 0;
			else
				close(fd);
		}
out:
	if (close_stream(&s) < 0)
		ok = 0;
out_free:
	free(pkginfo);
	set_free(hashes);
	if (old)
		set_free(old);
	return ok ? 0 : -1;
}

int pkg_install(const char *wpkg, const char *expect_name)
{
	struct set *hashes = set_new(), *now = set_new(), *old = NULL;
	char *pkginfo = NULL, *list = NULL, tmp[NAME_MAX];
	char **caps = NULL, **groups = NULL, **users = NULL;
	int ncaps = 0, ngroups = 0, nusers = 0, r, ret = -1, delta = 0;
	char delta_from[VERSION_MAX_LEN] = "";
	size_t listlen = 0;
	struct package p;
	struct stream s;
	struct entry e;
	FILE *files;

	if (open_stream(wpkg, &s) < 0)
		die("cannot read %s: %s", wpkg, strerror(errno));

	/* .PKGINFO comes first. */
	if (next_entry(&s, &e) != 1 || strcmp(e.path, ".PKGINFO") != 0 ||
	    e.size > (1 << 24) || !(pkginfo = malloc(padded(e.size) + 1)) ||
	    read_full(&s, pkginfo, padded(e.size)) < 0)
		die("%s is not a valid package", wpkg);
	pkginfo[e.size] = '\0';
	if (parse_pkginfo(pkginfo, &p) < 0 || strcmp(p.name, expect_name) != 0)
		die("%s does not contain the package %s", wpkg, expect_name);

	for (char *line = pkginfo; line && *line; ) {
		char *end = strchr(line, '\n'), sum[65], path[PATH_MAX];

		if (end)
			*end = '\0';
		if (sscanf(line, "file = %64s %4095[^\n]", sum, path) == 2) {
			set_add(hashes, path, sum);
		} else if (strncmp(line, "delta-from = ", 13) == 0) {
			delta = 1;
			snprintf(delta_from, sizeof(delta_from), "%s", line + 13);
		} else if (strncmp(line, "cap = ", 6) == 0) {
			caps = realloc(caps, (ncaps + 1) * sizeof(char *));
			caps[ncaps++] = strdup(line + 6);
		} else if (strncmp(line, "group = ", 8) == 0) {
			groups = realloc(groups, (ngroups + 1) * sizeof(char *));
			groups[ngroups++] = strdup(line + 8);
		} else if (strncmp(line, "user = ", 7) == 0) {
			users = realloc(users, (nusers + 1) * sizeof(char *));
			users[nusers++] = strdup(line + 7);
		}
		if (end)
			*end = '\n';
		line = end ? end + 1 : NULL;
	}

	/* A delta holds only what changed since the version it was made
	 * from, which must be the one installed. */
	if (delta) {
		struct package cur;

		if (!db_installed(p.name, &cur) || strcmp(cur.version, delta_from) != 0)
			die("%s: the update was made for version %s, not the one installed",
			    p.name, delta_from);
	}
	old = old_files(p.name);
	if (!(files = open_memstream(&list, &listlen)))
		die("out of memory");
	snprintf(tmp, sizeof(tmp), ".wh-new-%d", (int)getpid());

	while ((r = next_entry(&s, &e)) == 1) {
		const char *leaf;
		struct item *h;
		int pfd;

		if (clean_path(e.path) < 0) {
			warn("%s: unsafe path '%s' in the package", p.name, e.path);
			goto out;
		}
		if ((pfd = open_parent(e.path, &leaf, 1)) < 0) {
			warn("%s: cannot open the folder of %s: %s", p.name, e.path,
			     strerror(errno));
			goto out;
		}

		switch (e.type) {
		case '5': {
			struct stat st;

			if (mkdirat(pfd, leaf, e.mode) == 0) {
				/* mkdir applies the umask; set the exact mode. */
				if ((geteuid() == 0 &&
				     fchownat(pfd, leaf, 0, 0, AT_SYMLINK_NOFOLLOW) < 0) ||
				    fchmodat(pfd, leaf, e.mode, 0) < 0)
					warn("cannot set the owner or mode of %s", e.path);
			} else if (errno != EEXIST || fstatat(pfd, leaf, &st, 0) < 0 ||
				   !S_ISDIR(st.st_mode)) {
				warn("%s: cannot create the folder %s", p.name, e.path);
				close(pfd);
				goto out;
			}
			fprintf(files, "d %s\n", e.path);
			break;
		}
		case '0':
		case '7': {
			struct item *was = set_get(old, e.path);
			int kept;

			h = set_get(hashes, e.path);
			if (check_conflict(p.name, e.path, pfd, leaf, old) < 0)
				goto fail_entry;
			kept = extract_file(&s, &e, pfd, leaf, tmp, h ? h->value : NULL,
					    was && was->value && was->value[0] == 'f');
			if (kept < 0) {
				if (errno == EBADMSG)
					warn("%s: %s does not match its checksum", p.name, e.path);
				else
					warn("%s: cannot install %s: %s", p.name, e.path, strerror(errno));
				close(pfd);
				goto out;
			}
			if (h)
				h->seen = 1;
			e.size = 0;     /* already consumed */
			fprintf(files, "f %s\n", e.path);
			set_add(now, e.path, NULL);
			/* An unchanged file changes no cache a hook builds. */
			if (!kept)
				hook_touch(e.path);
			close(pfd);
			continue;
		}
		case '2':
			if (busybox_yields(p.name, &e)) {
				/* A full version of this command is installed. */
				close(pfd);
				continue;
			}
			if (check_conflict(p.name, e.path, pfd, leaf, old) < 0)
				goto fail_entry;
			unlinkat(pfd, tmp, 0);
			if (symlinkat(e.link, pfd, tmp) < 0 ||
			    (geteuid() == 0 && fchownat(pfd, tmp, 0, 0, AT_SYMLINK_NOFOLLOW) < 0) ||
			    renameat(pfd, tmp, pfd, leaf) < 0) {
				warn("%s: cannot create the link %s: %s", p.name, e.path,
				     strerror(errno));
				goto fail_entry;
			}
			fprintf(files, "l %s\n", e.path);
			break;
		case '1': {
			const char *tleaf;
			int tfd;

			if (clean_path(e.link) < 0 ||
			    check_conflict(p.name, e.path, pfd, leaf, old) < 0 ||
			    (tfd = open_parent(e.link, &tleaf, 0)) < 0)
				goto fail_entry;
			unlinkat(pfd, tmp, 0);
			if (linkat(tfd, tleaf, pfd, tmp, 0) < 0 ||
			    renameat(pfd, tmp, pfd, leaf) < 0) {
				warn("%s: cannot create the hard link %s: %s", p.name, e.path,
				     strerror(errno));
				close(tfd);
				goto fail_entry;
			}
			close(tfd);
			fprintf(files, "h %s\n", e.path);
			break;
		}
		default:
			warn("%s: skipping %s (unsupported file type)", p.name, e.path);
			break;
		}
		set_add(now, e.path, NULL);
		hook_touch(e.path);
		close(pfd);
		if (e.size && skip(&s, padded(e.size)) < 0)
			goto out;
		continue;
fail_entry:
		close(pfd);
		goto out;
	}
	if (r < 0) {
		warn("%s: the package archive is damaged", p.name);
		goto out;
	}
	for (int i = 0; i < SET_BUCKETS; i++)
		for (struct item *h = hashes->b[i]; h; h = h->next) {
			struct item *was;
			const char *leaf;
			int pfd, fd = -1;

			if (h->seen)
				continue;
			/* Left out of a delta: unchanged, already in place. */
			if (delta && (was = set_get(old, h->key)) && was->value &&
			    was->value[0] == 'f' && (pfd = open_parent(h->key, &leaf, 0)) >= 0) {
				fd = unchanged_file(pfd, leaf, NULL, h->value);
				close(pfd);
			}
			if (fd < 0) {
				warn("%s: %s is listed but missing from the package", p.name, h->key);
				goto out;
			}
			/* Capabilities are set again below. */
			if (fremovexattr(fd, "security.capability") < 0 && errno != ENODATA) {
				close(fd);
				warn("%s: cannot reset %s: %s", p.name, h->key, strerror(errno));
				goto out;
			}
			close(fd);
			fprintf(files, "f %s\n", h->key);
			set_add(now, h->key, NULL);
		}

	remove_leftovers(p.name, now);
	for (int i = 0; i < ngroups; i++)
		if (ensure_group(groups[i]) < 0) {
			warn("%s: cannot add the group %s", p.name, groups[i]);
			goto out;
		}
	for (int i = 0; i < nusers; i++)
		if (ensure_user(users[i]) < 0) {
			warn("%s: cannot add the user %s", p.name, users[i]);
			goto out;
		}
	for (int i = 0; i < ncaps; i++) {
		char path[PATH_MAX], text[256];

		if (sscanf(caps[i], "%4095s %255s", path, text) != 2 ||
		    set_caps(path, text) < 0) {
			warn("%s: cannot set capabilities '%s': %s", p.name, caps[i],
			     strerror(errno));
			goto out;
		}
	}

	fflush(files);
	if (db_write(p.name, pkginfo, files) < 0) {
		warn("%s: cannot record the installation: %s", p.name, strerror(errno));
		goto out;
	}
	ret = 0;
out:
	fclose(files);
	free(list);
	if (close_stream(&s) < 0 && ret == 0) {
		warn("%s: cannot decompress the package", p.name);
		ret = -1;
	}
	for (int i = 0; i < ncaps; i++)
		free(caps[i]);
	for (int i = 0; i < ngroups; i++)
		free(groups[i]);
	for (int i = 0; i < nusers; i++)
		free(users[i]);
	free(caps);
	free(groups);
	free(users);
	free(pkginfo);
	set_free(hashes);
	set_free(now);
	if (old)
		set_free(old);
	return ret;
}

/* ---- remove ---- */

int pkg_remove(const char *name)
{
	char **paths;
	int count;

	if (db_files(name, &paths, &count) < 0)
		return -1;
	for (int i = count - 1; i >= 0; i--) {
		const char *path = paths[i] + 2, *leaf;
		char type = paths[i][0], *owner;
		int pfd;

		if (type == 'd') {
			if (db_dir_used(path, name))
				continue;
		} else if ((owner = db_owner(path, name))) {
			free(owner);
			continue;
		}
		if ((pfd = open_parent(path, &leaf, 0)) < 0)
			continue;
		if (unlinkat(pfd, leaf, type == 'd' ? AT_REMOVEDIR : 0) == 0)
			hook_touch(path);
		else if (errno != ENOENT && errno != ENOTEMPTY && errno != EEXIST)
			warn("cannot remove /%s: %s", path, strerror(errno));
		close(pfd);
	}
	if (strcmp(name, "busybox") != 0)
		busybox_restore(paths, count);
	db_free_files(paths, count);
	return db_remove(name);
}

/* ---- plain unpacking ---- */

/*
 * Unpack a tar stream (the output of the decompressor argv) into the root,
 * keeping owners and modes. Paths are cleaned and resolved inside the root
 * as for packages, but nothing is recorded or checked against hashes: this
 * is for archives already verified by other means (Debian packages being
 * bootstrapped, checked against their signed index).
 */
int tar_unpack(char *const argv[])
{
	struct stream s;
	struct entry e;
	char tmp[NAME_MAX];
	int r, ret = -1;

	if (open_stream_cmd(argv, &s) < 0)
		return -1;
	snprintf(tmp, sizeof(tmp), ".wh-new-%d", (int)getpid());
	while ((r = next_entry(&s, &e)) == 1) {
		struct timespec times[2] = { { 0, UTIME_OMIT }, { e.mtime, 0 } };
		const char *leaf;
		int pfd, ok = 1;

		/* The archive's own top directory ("./") is the root itself. */
		if (strcmp(e.path, ".") == 0 || strcmp(e.path, "./") == 0) {
			if (e.size && skip(&s, padded(e.size)) < 0)
				goto out;
			continue;
		}
		if (clean_path(e.path) < 0) {
			warn("unsafe path '%s' in the archive", e.path);
			goto out;
		}
		if ((pfd = open_parent(e.path, &leaf, 1)) < 0) {
			warn("cannot open the folder of %s: %s", e.path, strerror(errno));
			goto out;
		}
		switch (e.type) {
		case '5': {
			struct stat st;

			if (mkdirat(pfd, leaf, 0700) == 0 || errno == EEXIST)
				ok = fstatat(pfd, leaf, &st, 0) == 0 && S_ISDIR(st.st_mode) &&
				     fchownat(pfd, leaf, e.uid, e.gid, 0) == 0 &&
				     fchmodat(pfd, leaf, e.mode, 0) == 0;
			else
				ok = 0;
			break;
		}
		case '0':
		case '7': {
			unsigned char buf[65536];
			unsigned long long left = e.size;
			int fd;

			unlinkat(pfd, tmp, 0);
			fd = openat(pfd, tmp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
			if (fd < 0) {
				ok = 0;
				break;
			}
			while (ok && left > 0) {
				size_t take = left < sizeof(buf) ? left : sizeof(buf);

				ok = read_full(&s, buf, take) == 0 &&
				     write(fd, buf, take) == (ssize_t)take;
				left -= take;
			}
			ok = ok && fchown(fd, e.uid, e.gid) == 0 && fchmod(fd, e.mode) == 0 &&
			     futimens(fd, times) == 0;
			ok = close(fd) == 0 && ok && renameat(pfd, tmp, pfd, leaf) == 0;
			if (!ok)
				unlinkat(pfd, tmp, 0);
			ok = ok && skip(&s, padded(e.size) - e.size) == 0;
			e.size = 0;
			break;
		}
		case '2':
			unlinkat(pfd, tmp, 0);
			ok = symlinkat(e.link, pfd, tmp) == 0 &&
			     fchownat(pfd, tmp, e.uid, e.gid, AT_SYMLINK_NOFOLLOW) == 0 &&
			     renameat(pfd, tmp, pfd, leaf) == 0;
			break;
		case '1': {
			const char *tleaf;
			int tfd;

			if (clean_path(e.link) < 0 || (tfd = open_parent(e.link, &tleaf, 0)) < 0) {
				ok = 0;
				break;
			}
			unlinkat(pfd, tmp, 0);
			ok = linkat(tfd, tleaf, pfd, tmp, 0) == 0 &&
			     renameat(pfd, tmp, pfd, leaf) == 0;
			close(tfd);
			break;
		}
		default:
			break;          /* devices and fifos: dpkg creates them later */
		}
		close(pfd);
		if (!ok) {
			warn("cannot unpack %s: %s", e.path, strerror(errno));
			goto out;
		}
		if (e.size && skip(&s, padded(e.size)) < 0)
			goto out;
	}
	ret = r == 0 ? 0 : -1;
	if (r < 0)
		warn("the archive is damaged");
out:
	if (close_stream(&s) < 0 && ret == 0) {
		warn("cannot decompress the archive");
		ret = -1;
	}
	return ret;
}
