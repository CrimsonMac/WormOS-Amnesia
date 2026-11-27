/*
 * mkinitramfs - build the initramfs of a WormOS system.
 *
 *     /usr/lib/wormrd/mkinitramfs ROOT [OUTPUT]
 *
 * Writes ROOT/boot/initramfs.img (or OUTPUT): a zstd-compressed cpio
 * archive (newc) holding wormrd as /init, the programs it runs (BusyBox for
 * the rescue shell, snap for boot-time operations, zstd, and cryptsetup to
 * open an encrypted root) and every shared library they need, found by
 * reading their ELF dynamic sections in ROOT. An installed encrypted system
 * keeps the key that opens its root in ROOT/etc/worm/luks.key; it is copied
 * in as /crypto_keyfile, so the passphrase is asked for once (by GRUB, to
 * read the kernel and this file from the encrypted disk) and not again.
 *
 * wh runs it as a hook (usr/share/wh/hooks/initramfs.hook) after changes
 * to any of those files, on the staged copy of the system and on images
 * assembled on the build machine alike. The archive is written next to
 * OUTPUT and renamed over it, so a failed build leaves the old one.
 */

#define _GNU_SOURCE
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zstd.h>

#define MAX_ENTRIES 512
#define LOADER      "usr/lib/ld-linux-x86-64.so.2"

static const char *root;
static char *names[MAX_ENTRIES];        /* paths in the archive, to skip repeats */
static int nnames;
static ZSTD_CCtx *zc;
static FILE *out;
static unsigned long long written;
static unsigned ino = 1;

__attribute__((noreturn, format(printf, 1, 2))) static void die(const char *fmt, ...)
{
	va_list ap;

	fputs("mkinitramfs: ", stderr);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
	exit(1);
}

/* ---- the compressed output ---- */

static void emit(const void *data, size_t len)
{
	ZSTD_inBuffer in = { data, len, 0 };
	char buf[1 << 16];

	while (in.pos < in.size) {
		ZSTD_outBuffer o = { buf, sizeof(buf), 0 };
		size_t r = ZSTD_compressStream2(zc, &o, &in, ZSTD_e_continue);

		if (ZSTD_isError(r))
			die("zstd: %s", ZSTD_getErrorName(r));
		if (o.pos && fwrite(buf, 1, o.pos, out) != o.pos)
			die("cannot write: %s", strerror(errno));
	}
	written += len;
}

static void finish(void)
{
	char buf[1 << 16];
	size_t r;

	do {
		ZSTD_inBuffer in = { NULL, 0, 0 };
		ZSTD_outBuffer o = { buf, sizeof(buf), 0 };

		r = ZSTD_compressStream2(zc, &o, &in, ZSTD_e_end);
		if (ZSTD_isError(r))
			die("zstd: %s", ZSTD_getErrorName(r));
		if (o.pos && fwrite(buf, 1, o.pos, out) != o.pos)
			die("cannot write: %s", strerror(errno));
	} while (r);
}

static void pad4(void)
{
	static const char zeros[4];

	if (written % 4)
		emit(zeros, 4 - written % 4);
}

/* ---- cpio (newc) ---- */

static int known(const char *name)
{
	for (int i = 0; i < nnames; i++)
		if (!strcmp(names[i], name))
			return 1;
	return 0;
}

/* Is name in the archive already? If not, it is from now on. */
static int seen(const char *name)
{
	if (known(name))
		return 1;
	if (nnames == MAX_ENTRIES)
		die("too many files");
	names[nnames++] = strdup(name);
	return 0;
}

static void header(const char *name, unsigned mode, unsigned long size, unsigned rmaj, unsigned rmin)
{
	char h[111];

	/* Times are 0: the archive is the same for the same files. */
	snprintf(h, sizeof(h), "070701%08X%08X%08X%08X%08X%08X%08lX%08X%08X%08X%08X%08zX%08X",
		 ino++, mode, 0, 0, 1, 0, size, 0, 0, rmaj, rmin, strlen(name) + 1, 0);
	emit(h, 110);
	emit(name, strlen(name) + 1);
	pad4();
}

static void add_dir(const char *name)
{
	if (!seen(name))
		header(name, S_IFDIR | 0755, 0, 0, 0);
}

static void add_node(const char *name, unsigned mode, unsigned maj, unsigned min)
{
	if (!seen(name))
		header(name, S_IFCHR | mode, 0, maj, min);
}

static void add_link(const char *name, const char *target)
{
	if (seen(name))
		return;
	header(name, S_IFLNK | 0777, strlen(target), 0, 0);
	emit(target, strlen(target));
	pad4();
}

/* A file from ROOT/src as name in the archive. */
static void add_file(const char *src, const char *name, unsigned mode)
{
	char path[PATH_MAX], buf[1 << 16];
	struct stat st;
	ssize_t n;
	int fd;

	if (seen(name))
		return;
	snprintf(path, sizeof(path), "%s/%s", root, src);
	if ((fd = open(path, O_RDONLY | O_CLOEXEC)) < 0 || fstat(fd, &st) < 0)
		die("cannot read %s: %s", path, strerror(errno));
	header(name, S_IFREG | mode, (unsigned long)st.st_size, 0, 0);
	while ((n = read(fd, buf, sizeof(buf))) > 0)
		emit(buf, (size_t)n);
	if (n < 0)
		die("cannot read %s: %s", path, strerror(errno));
	close(fd);
	pad4();
}

/* ---- programs and the libraries they need ---- */

/*
 * DT_NEEDED entries of ROOT/path: the dynamic section and its string table,
 * found through the section headers. Calls fn for each name.
 */
static void needed(const char *path, void (*fn)(const char *))
{
	char full[PATH_MAX];
	Elf64_Ehdr eh;
	Elf64_Shdr *sh = NULL;
	char *strtab = NULL;
	Elf64_Dyn *dyn = NULL;
	size_t ndyn = 0;
	int fd;

	snprintf(full, sizeof(full), "%s/%s", root, path);
	if ((fd = open(full, O_RDONLY | O_CLOEXEC)) < 0)
		die("cannot read %s: %s", full, strerror(errno));
	if (pread(fd, &eh, sizeof(eh), 0) != sizeof(eh) || memcmp(eh.e_ident, ELFMAG, SELFMAG) ||
	    eh.e_ident[EI_CLASS] != ELFCLASS64 || eh.e_shentsize != sizeof(Elf64_Shdr))
		die("%s is not a 64-bit ELF file", full);
	sh = calloc(eh.e_shnum, sizeof(*sh));
	if (!sh || pread(fd, sh, eh.e_shnum * sizeof(*sh), (off_t)eh.e_shoff) !=
		   (ssize_t)(eh.e_shnum * sizeof(*sh)))
		die("cannot read the sections of %s", full);
	for (int i = 0; i < eh.e_shnum; i++) {
		Elf64_Shdr *s = &sh[i], *str;

		if (s->sh_type != SHT_DYNAMIC || s->sh_link >= eh.e_shnum)
			continue;
		str = &sh[s->sh_link];
		dyn = malloc(s->sh_size);
		strtab = malloc(str->sh_size + 1);
		if (!dyn || !strtab ||
		    pread(fd, dyn, s->sh_size, (off_t)s->sh_offset) != (ssize_t)s->sh_size ||
		    pread(fd, strtab, str->sh_size, (off_t)str->sh_offset) != (ssize_t)str->sh_size)
			die("cannot read the dynamic section of %s", full);
		strtab[str->sh_size] = '\0';
		ndyn = s->sh_size / sizeof(Elf64_Dyn);
		for (size_t j = 0; j < ndyn && dyn[j].d_tag != DT_NULL; j++)
			if (dyn[j].d_tag == DT_NEEDED && dyn[j].d_un.d_val < str->sh_size)
				fn(strtab + dyn[j].d_un.d_val);
		break;
	}
	free(dyn);
	free(strtab);
	free(sh);
	close(fd);
}

static void add_library(const char *name);

static void add_elf(const char *path, const char *name, unsigned mode)
{
	if (known(name))
		return;
	add_file(path, name, mode);
	needed(path, add_library);
}

static void add_library(const char *lib)
{
	char path[PATH_MAX], name[PATH_MAX];

	if (strchr(lib, '/'))
		die("unexpected library path %s", lib);
	snprintf(path, sizeof(path), "usr/lib/%s", lib);
	snprintf(name, sizeof(name), "usr/lib/%s", lib);
	{
		char full[PATH_MAX];

		snprintf(full, sizeof(full), "%s/%s", root, path);
		if (access(full, R_OK) < 0)
			die("a program needs %s, which %s/usr/lib does not have", lib, root);
	}
	add_elf(path, name, 0755);
}

static int exists(const char *path)
{
	char full[PATH_MAX];

	snprintf(full, sizeof(full), "%s/%s", root, path);
	return access(full, F_OK) == 0;
}

int main(int argc, char **argv)
{
	static const char *const dirs[] = { "dev", "proc", "sys", "run", "tmp", "top", "sysroot",
					    "usr", "usr/bin", "usr/lib", "etc", NULL };
	static const char *const programs[] = { "usr/bin/busybox", "usr/bin/snap", "usr/bin/zstd",
						"usr/bin/cryptsetup", NULL };
	static const char *const applets[] = { "sh", "ash", "cat", "ls", "mkdir", "mount", "umount",
					       "ln", "cp", "mv", "rm", "dmesg", "less", "vi", "reboot",
					       "poweroff", NULL };
	char output[PATH_MAX], tmp[PATH_MAX + 8];
	int fd;

	if (argc < 2 || argc > 3) {
		fputs("usage: mkinitramfs ROOT [OUTPUT]\n", stderr);
		return 2;
	}
	root = argv[1];
	if (argc == 3)
		snprintf(output, sizeof(output), "%s", argv[2]);
	else
		snprintf(output, sizeof(output), "%s/boot/initramfs.img", root);
	if (!exists("usr/lib/wormrd/init"))
		die("%s has no wormrd (usr/lib/wormrd/init)", root);
	snprintf(tmp, sizeof(tmp), "%s.new", output);
	/* It may hold the key to the disk: root only. */
	if ((fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600)) < 0 ||
	    !(out = fdopen(fd, "w")))
		die("cannot write %s: %s", tmp, strerror(errno));
	if (!(zc = ZSTD_createCCtx()))
		die("out of memory");
	ZSTD_CCtx_setParameter(zc, ZSTD_c_compressionLevel, 12);
	ZSTD_CCtx_setParameter(zc, ZSTD_c_checksumFlag, 1);

	for (int i = 0; dirs[i]; i++)
		add_dir(dirs[i]);
	add_node("dev/console", 0600, 5, 1);
	add_node("dev/null", 0666, 1, 3);
	add_link("bin", "usr/bin");
	add_link("sbin", "usr/bin");
	add_link("lib", "usr/lib");
	add_link("lib64", "usr/lib");
	add_elf("usr/lib/wormrd/init", "init", 0755);
	add_elf(LOADER, LOADER, 0755);
	for (int i = 0; programs[i]; i++)
		if (exists(programs[i]))
			add_elf(programs[i], programs[i], 0755);
	if (exists("usr/bin/busybox"))
		for (int i = 0; applets[i]; i++) {
			char name[64];

			snprintf(name, sizeof(name), "usr/bin/%s", applets[i]);
			add_link(name, "busybox");
		}
	/* Development images only (see wormrd's rescue shell). */
	if (exists("etc/worm/rescue-shell"))
		add_file("etc/worm/rescue-shell", "rescue-allowed", 0644);
	if (exists("etc/worm/luks.key"))
		add_file("etc/worm/luks.key", "crypto_keyfile", 0400);
	header("TRAILER!!!", 0, 0, 0, 0);
	finish();
	if (fflush(out) || fsync(fileno(out)) || fclose(out))
		die("cannot write %s: %s", tmp, strerror(errno));
	ZSTD_freeCCtx(zc);
	if (rename(tmp, output) < 0)
		die("cannot replace %s: %s", output, strerror(errno));
	return 0;
}
