/*
 * amnesia-update - bring WormOS Amnesia's USB stick up to the latest
 * release without downloading the whole ISO.
 *
 *     amnesia-update            update the stick (asks first)
 *     amnesia-update --check    only say whether an update exists
 *     amnesia-update --yes      update without asking
 *
 * Each release on WormOS-Amnesia's release page publishes, next to its
 * ISO, a manifest (MANIFEST; UPDATE_URL follows the latest release) signed
 * with the WormOS key: the version, the ISO's name, size
 * and SHA-256, and a weak (rolling) and a strong checksum for every block
 * of BLOCK bytes. Amnesia runs from memory, so the stick it started from
 * (/dev/wormos-medium) is free. The new ISO is put together in memory:
 * every block the old ISO on the stick already holds, wherever it moved
 * (a rolling checksum finds it at any offset, as rsync and zsync do), is
 * copied from there; only the others are downloaded, through Tor like
 * everything here. The result must match the signed SHA-256 before it is
 * written to the stick, which is then read back and checked again.
 *
 * Installed setuid root: it only ever writes a signed WormOS Amnesia image
 * to the device Amnesia started from, and only if that is a USB stick. It
 * takes no paths, URLs or data from the user.
 */

#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <linux/fs.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "sha256.h"
#include "sig.h"

#define MANIFEST    "amnesia.update"
#define MAX_BLOCKS  (1 << 20)

/*
 * A test build (-DAMNESIA_UPDATE_TEST, tools/amnesia-update-test) takes
 * these from the environment and works on an ISO file instead of a stick;
 * the installed program has them fixed.
 */
#ifdef AMNESIA_UPDATE_TEST
#define SETTING(name, fixed) (getenv("TEST_" #name) ? getenv("TEST_" #name) : fixed)
#else
#define SETTING(name, fixed) (fixed)
#endif
#define UPDATE_URL  SETTING(URL, "https://github.com/CrimsonMac/WormOS-Amnesia/releases/latest/download/")
#define PUBLIC_KEY  SETTING(KEY, "/usr/share/wh/keys/core.pub")
#define MEDIUM      "/dev/wormos-medium"
#ifdef AMNESIA_UPDATE_TEST
#define PROTO "=http,https"
#else
#define PROTO "=https"
#endif
static char WORK[256] = "/var/cache/amnesia-update";

/* A path in the work folder. */
static const char *work(const char *name)
{
	static char paths[4][PATH_MAX];
	static int next;
	char *p = paths[next++ % 4];

	snprintf(p, PATH_MAX, "%s/%s", WORK, name);
	return p;
}

struct block {
	uint32_t weak;
	uint8_t strong[16];
	int have;              /* found in the old ISO */
	int next;              /* next block with the same weak hash bucket */
};

static struct block *blocks;
static long nblocks, block_size;
static long long new_size;
static char new_name[256], new_version[32], new_sha[65];
static int *buckets;
#define NBUCKETS (1 << 18)

static __attribute__((noreturn)) void die(const char *fmt, const char *arg)
{
	fprintf(stderr, "amnesia-update: ");
	fprintf(stderr, fmt, arg);
	fputc('\n', stderr);
	exit(1);
}

static int run(char *const argv[])
{
	int status;
	pid_t pid = fork();

	if (pid < 0)
		return -1;
	if (pid == 0) {
		execv(argv[0], argv);
		_exit(127);
	}
	while (waitpid(pid, &status, 0) < 0)
		if (errno != EINTR)
			return -1;
	return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : -1;
}

/* Download URL_BASE/name to path (through Tor: the firewall sends it there). */
static int fetch(const char *name, const char *path)
{
	char url[512];

	snprintf(url, sizeof(url), "%s%s", UPDATE_URL, name);
	return run((char *[]){ "/usr/bin/curl", "-fsSL", "--retry", "3", "--proto", PROTO,
			       "-o", (char *)path, url, NULL });
}

static void strong_sum(const uint8_t *p, long n, uint8_t out[16])
{
	struct sha256 ctx;
	uint8_t full[32];

	sha256_init(&ctx);
	sha256_update(&ctx, p, n);
	sha256_final(&ctx, full);
	memcpy(out, full, 16);
}

/* Parse the (already verified) manifest. */
static void read_manifest(const char *path)
{
	char line[512];
	FILE *f = fopen(path, "re");
	long i = 0;

	if (!f)
		die("cannot read the manifest%s", "");
	while (fgets(line, sizeof(line), f)) {
		char hex[40], strong[40];
		unsigned weak;
		long idx;

		if (sscanf(line, "version = %31s", new_version) == 1 ||
		    sscanf(line, "iso = %255s", new_name) == 1 ||
		    sscanf(line, "sha256 = %64s", new_sha) == 1 ||
		    sscanf(line, "size = %lld", &new_size) == 1)
			continue;
		if (sscanf(line, "block = %ld", &block_size) == 1) {
			if (block_size < 4096 || block_size > (1 << 24) || new_size <= 0)
				die("a bad manifest%s", "");
			nblocks = (new_size + block_size - 1) / block_size;
			if (nblocks > MAX_BLOCKS)
				die("a bad manifest%s", "");
			blocks = calloc(nblocks, sizeof(*blocks));
			continue;
		}
		if (blocks && sscanf(line, "%ld %8s %32s", &idx, hex, strong) == 3) {
			if (idx != i || idx >= nblocks || sscanf(hex, "%x", &weak) != 1 ||
			    hex_decode(strong, blocks[i].strong, 16) < 0)
				die("a bad manifest%s", "");
			blocks[i].weak = weak;
			i++;
		}
	}
	fclose(f);
	for (const char *p = new_name; *p; p++)
		if (!isalnum((unsigned char)*p) && !strchr("._-", *p))
			die("a bad manifest%s", "");
	if (!blocks || i != nblocks || strlen(new_sha) != 64 || !new_version[0] || !new_name[0])
		die("a bad manifest%s", "");
}

/* Version numbers compared part by part: 1.10 is newer than 1.9. */
static int newer(const char *a, const char *b)
{
	while (*a || *b) {
		long x = strtol(a, (char **)&a, 10), y = strtol(b, (char **)&b, 10);

		if (x != y)
			return x > y;
		if (*a == '.')
			a++;
		if (*b == '.')
			b++;
		if (!*a && !*b)
			break;
	}
	return 0;
}

static void current_version(char *out, size_t len)
{
	char line[128];
	FILE *f = fopen("/etc/os-release", "re");

	snprintf(out, len, "0");
	while (f && fgets(line, sizeof(line), f))
		if (sscanf(line, "VERSION_ID=%31s", out) == 1)
			break;
	if (f)
		fclose(f);
}

/* The stick Amnesia started from: a whole USB disk. */
static void medium(char *dev, size_t len)
{
	char target[PATH_MAX], path[PATH_MAX], real[PATH_MAX], name[64];
	ssize_t n = readlink(MEDIUM, target, sizeof(target) - 1);
	FILE *f;
	int removable = 0;

	if (n <= 0)
		die("Amnesia did not start from a stick it can update%s", "");
	target[n] = '\0';
	snprintf(name, sizeof(name), "%s", strrchr(target, '/') ? strrchr(target, '/') + 1 : target);
	if (!strncmp(name, "sr", 2))
		die("Amnesia started from a disc, which cannot be rewritten: write the new ISO "
		    "to a USB stick instead%s", "");
	/* A partition: its disk is the parent in sysfs. */
	snprintf(path, sizeof(path), "/sys/class/block/%s/partition", name);
	if (access(path, F_OK) == 0) {
		snprintf(path, sizeof(path), "/sys/class/block/%s/..", name);
		if (!realpath(path, real))
			die("cannot find the stick%s", "");
		snprintf(name, sizeof(name), "%s", strrchr(real, '/') + 1);
	}
	snprintf(path, sizeof(path), "/sys/class/block/%s/removable", name);
	if ((f = fopen(path, "re"))) {
		removable = fgetc(f) == '1';
		fclose(f);
	}
	snprintf(path, sizeof(path), "/sys/class/block/%s", name);
	if (!realpath(path, real) || !strstr(real, "/usb"))
		die("the device Amnesia started from is not a USB stick%s", "");
	(void)removable;
	snprintf(dev, len, "/dev/%s", name);
}

/* The size of the ISO on the stick, from its primary volume descriptor. */
static long long old_iso_size(int fd)
{
	uint8_t pvd[2048];

	if (pread(fd, pvd, sizeof(pvd), 16 * 2048) != (ssize_t)sizeof(pvd) ||
	    pvd[0] != 1 || memcmp(pvd + 1, "CD001", 5))
		die("the stick does not hold a WormOS ISO%s", "");
	return (long long)(pvd[80] | pvd[81] << 8 | pvd[82] << 16 | (uint32_t)pvd[83] << 24) * 2048;
}

/* Find the new ISO's blocks in the old one and copy them into out. */
static long reuse(int old, long long old_size, int out)
{
	long found = 0;
	uint8_t *buf;
	long long base = 0;
	long len, pos;
	uint32_t a = 0, b = 0;

	for (int i = 0; i < NBUCKETS; i++)
		buckets[i] = -1;
	for (long i = 0; i < nblocks; i++) {
		int h = blocks[i].weak & (NBUCKETS - 1);

		blocks[i].next = buckets[h];
		buckets[h] = i;
	}
	/* The window slides over the old ISO, read in large chunks. */
	long chunk = 64L << 20;
	if (!(buf = malloc(chunk + block_size)))
		die("out of memory%s", "");
	len = pread(old, buf, chunk + block_size, 0);
	if (len < block_size)
		goto done;
	a = b = 0;
	for (long i = 0; i < block_size; i++) {
		a += buf[i];
		b += (uint32_t)(block_size - i) * buf[i];
	}
	pos = 0;
	for (;;) {
		uint32_t weak = (a & 0xffff) | (b << 16);
		int hit = 0;

		for (int i = buckets[weak & (NBUCKETS - 1)]; i >= 0; i = blocks[i].next) {
			uint8_t strong[16];

			if (blocks[i].weak != weak || blocks[i].have)
				continue;
			/* The last block may be shorter: it is downloaded. */
			if ((long long)(i + 1) * block_size > new_size)
				continue;
			strong_sum(buf + pos, block_size, strong);
			if (memcmp(strong, blocks[i].strong, 16))
				continue;
			/* The same content may stand for several new blocks. */
			for (int j = i; j >= 0; j = blocks[j].next)
				if (blocks[j].weak == weak && !blocks[j].have &&
				    (long long)(j + 1) * block_size <= new_size &&
				    !memcmp(blocks[j].strong, strong, 16)) {
					if (pwrite(out, buf + pos, block_size,
						   (off_t)j * block_size) != block_size)
						die("cannot write the new ISO: %s", strerror(errno));
					blocks[j].have = 1;
					found++;
				}
			hit = 1;
			break;
		}
		/* Next window: a whole block on after a match, else one byte. */
		long step = hit ? block_size : 1;
		for (long s = 0; s < step; s++) {
			if (base + pos + block_size >= old_size)
				goto done;
			if (pos + block_size >= len) {
				/* Refill: keep the current window at the front. */
				base += pos;
				len = pread(old, buf, chunk + block_size, base);
				pos = 0;
				if (len < block_size)
					goto done;
			}
			uint8_t out_byte = buf[pos], in_byte = buf[pos + block_size];

			a += in_byte - out_byte;
			b += a - (uint32_t)block_size * out_byte;
			pos++;
		}
	}
done:
	free(buf);
	return found;
}

/* Download the blocks not found, in merged ranges, several at a time. */
static int download_missing(const char *iso_path)
{
	char config[PATH_MAX], url[512];
	FILE *f;
	long ranges = 0;

	snprintf(config, sizeof(config), "%s", work("ranges"));
	f = fopen(config, "we");

	if (!f)
		return -1;
	snprintf(url, sizeof(url), "%s%s", UPDATE_URL, new_name);
	for (long i = 0; i < nblocks; ) {
		long j;

		if (blocks[i].have) {
			i++;
			continue;
		}
		for (j = i; j < nblocks && !blocks[j].have && j - i < 256; j++)
			;
		long long from = (long long)i * block_size;
		long long to = (long long)j * block_size - 1;

		if (to >= new_size)
			to = new_size - 1;
		/* Each transfer its own options: "next" starts the next one. */
		fprintf(f, "%surl = \"%s\"\nrange = \"%lld-%lld\"\noutput = \"%s/part.%ld\"\n"
			"fail\nlocation\nretry = 3\nproto = \"%s\"\n",
			ranges ? "next\n" : "", url, from, to, WORK, i, PROTO);
		ranges++;
		i = j;
	}
	if (fclose(f) != 0)
		return -1;
	if (ranges && run((char *[]){ "/usr/bin/curl", "-sS", "-Z", "--parallel-max", "6",
				      "-K", config, NULL }) < 0)
		return -1;
	/* Put the parts in place. */
	int out = open(iso_path, O_WRONLY | O_CLOEXEC);
	if (out < 0)
		return -1;
	for (long i = 0; i < nblocks; ) {
		char part[PATH_MAX];
		long j;
		size_t n;
		uint8_t *data;

		if (blocks[i].have) {
			i++;
			continue;
		}
		for (j = i; j < nblocks && !blocks[j].have && j - i < 256; j++)
			;
		snprintf(part, sizeof(part), "%s/part.%ld", WORK, i);
		if (!(data = read_all(part, &n)))
			return -1;
		/* Exactly the range asked for: a server that ignores ranges
		 * sends the whole file. */
		{
			long long want = (long long)j * block_size;

			if (want > new_size)
				want = new_size;
			if ((long long)n != want - (long long)i * block_size) {
				free(data);
				return -1;
			}
		}
		if (pwrite(out, data, n, (off_t)i * block_size) != (ssize_t)n)
			return -1;
		free(data);
		unlink(part);
		i = j;
	}
	return close(out);
}

static int file_sha256(int fd, long long size, char hex[65])
{
	struct sha256 ctx;
	uint8_t buf[1 << 20], sum[32];
	long long done = 0;

	sha256_init(&ctx);
	while (done < size) {
		ssize_t n = pread(fd, buf, size - done < (long long)sizeof(buf) ?
				  (size_t)(size - done) : sizeof(buf), done);

		if (n <= 0)
			return -1;
		sha256_update(&ctx, buf, n);
		done += n;
	}
	sha256_final(&ctx, sum);
	hex_encode(sum, 32, hex);
	return 0;
}

int main(int argc, char **argv)
{
	char version[32], dev[PATH_MAX], sha[65], iso_path[PATH_MAX];
	uint8_t key[SIG_PUBLIC_LEN];
	int check = argc > 1 && !strcmp(argv[1], "--check");
	int yes = argc > 1 && !strcmp(argv[1], "--yes");
	int old, out;
	long long old_size;
	long found;

	if (argc > 2 || (argc == 2 && !check && !yes))
		die("usage: amnesia-update [--check | --yes]%s", "");
#ifdef AMNESIA_UPDATE_TEST
	snprintf(WORK, sizeof(WORK), "%s", SETTING(WORK, WORK));
#else
	clearenv();
	setenv("PATH", "/usr/bin:/usr/sbin", 1);
	if (setresgid(0, 0, 0) < 0 || setresuid(0, 0, 0) < 0)
		die("not installed setuid root%s", "");
#endif
	umask(077);
	mkdir(WORK, 0700);
	snprintf(iso_path, sizeof(iso_path), "%s", work("new.iso"));
	printf("Looking for an update (through Tor)...\n");
	fflush(stdout);
	if (fetch(MANIFEST, work("manifest")) < 0 || fetch(MANIFEST ".sig", work("manifest.sig")) < 0)
		die("cannot reach the update server; is Tor connected? (worm anon status)%s", "");
	if (read_hex_file(PUBLIC_KEY, key, sizeof(key)) < 0 ||
	    sig_verify_file(key, work("manifest"), work("manifest.sig")) < 0)
		die("the update's signature is not WormOS's: refusing it%s", "");
	read_manifest(work("manifest"));
	current_version(version, sizeof(version));
#ifdef AMNESIA_UPDATE_TEST
	snprintf(version, sizeof(version), "%s", SETTING(VERSION, version));
#endif
	if (!newer(new_version, version)) {
		printf("WormOS Amnesia %s is the latest version.\n", version);
		return 0;
	}
	printf("WormOS Amnesia %s is available (this is %s).\n", new_version, version);
	if (check)
		return 0;
#ifdef AMNESIA_UPDATE_TEST
	snprintf(dev, sizeof(dev), "%s", SETTING(MEDIUM, ""));
#else
	medium(dev, sizeof(dev));
#endif
	if (!yes) {
		char answer[16] = "";

		printf("Update the stick (%s) now? The new version starts next time. [y/N] ", dev);
		fflush(stdout);
		if (!fgets(answer, sizeof(answer), stdin) || (answer[0] != 'y' && answer[0] != 'Y'))
			return 1;
	}
	if ((old = open(dev, O_RDONLY | O_CLOEXEC)) < 0)
		die("cannot read the stick: %s", strerror(errno));
	old_size = old_iso_size(old);
	if ((out = open(iso_path, O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0600)) < 0 ||
	    ftruncate(out, new_size) < 0)
		die("cannot make room for the new version: %s", strerror(errno));
	if (!(buckets = malloc(NBUCKETS * sizeof(int))))
		die("out of memory%s", "");
	found = reuse(old, old_size, out);
	printf("%ld of %ld blocks are already on the stick; downloading %ld (about %lld MB)...\n",
	       found, nblocks, nblocks - found, (long long)(nblocks - found) * block_size >> 20);
	fflush(stdout);
	if (download_missing(iso_path) < 0)
		die("the download failed; nothing was changed%s", "");
	if (file_sha256(out, new_size, sha) < 0 || strcmp(sha, new_sha))
		die("the new version does not match its signed checksum; nothing was changed%s", "");
	close(old);

	printf("Writing the stick...\n");
	fflush(stdout);
	int w = open(dev, O_WRONLY | O_CLOEXEC);
	uint8_t *buf = malloc(1 << 22);
	long long done = 0;

	if (w < 0 || !buf)
		die("cannot write the stick: %s", strerror(errno));
	while (done < new_size) {
		size_t n = new_size - done < (1 << 22) ? (size_t)(new_size - done) : (1 << 22);

		if (pread(out, buf, n, done) != (ssize_t)n || pwrite(w, buf, n, done) != (ssize_t)n)
			die("writing the stick failed: %s; start the old version from a "
			    "fresh ISO if it no longer starts", strerror(errno));
		done += n;
	}
	if (fsync(w) < 0)
		die("writing the stick failed: %s", strerror(errno));
	close(w);
	/* Read it back from the stick itself, not from the cache. */
	if ((w = open(dev, O_RDONLY | O_CLOEXEC)) >= 0)
		ioctl(w, BLKFLSBUF, 0);
	if (w < 0 || file_sha256(w, new_size, sha) < 0 || strcmp(sha, new_sha))
		die("the stick does not read back correctly: write the ISO to it again from "
		    "another computer%s", "");
	close(w);
	unlink(iso_path);
	printf("Done: the stick now holds WormOS Amnesia %s. Restart to use it.\n", new_version);
	return 0;
}
