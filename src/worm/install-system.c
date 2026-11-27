/*
 * install-system - install WormOS from the live ISO onto a disk.
 *
 * Installed setuid root as /usr/lib/worm/install-system, for the installer
 * (wormos-installer). It works only in the live system (wormos.live on the
 * kernel command line) and only for root or members of the group wheel:
 *
 *     install-system list         the disks WormOS can go on, one per line:
 *                                 NAME BYTES REMOVABLE MODEL
 *     install-system install NAME erase disk NAME and install WormOS on it
 *     install-system install NAME --encrypt
 *                                 the same with the root encrypted (LUKS2);
 *                                 the passphrase is read from standard input
 *
 * install prints its progress as lines "step TEXT" and "progress PERCENT",
 * then "done", or an error on standard error with a non-zero status.
 *
 * The disk gets the layout of the WormOS disk images (scripts/mkimage.sh):
 * a GPT with an EFI system partition (1), the btrfs root (2, the rest of
 * the disk) and a BIOS boot partition (3, before the first). The pieces
 * come from the ISO, which is mounted again (wormrd let go of it after
 * copying the live system into memory): GRUB's boot code and core.img
 * from wormos/head.img, the EFI system partition from wormos/esp.img.zst
 * and the root filesystem from wormos/system.img.zst, which is then grown to
 * fill its partition. The installed system starts with the first-boot
 * setup, like a fresh disk image.
 *
 * Encrypted, the root partition is LUKS2 with PBKDF2 (which GRUB can open:
 * it asks for the passphrase at boot, reads the kernel and the initramfs
 * from the encrypted disk, and the snapshot entries keep working) and a
 * second key slot holding a random key, which goes into the new system as
 * /etc/worm/luks.key and into its initramfs (mkinitramfs), so the disk is
 * opened again there without asking. The system image is copied into the
 * opened device, /dev/mapper/wormos-install.
 *
 * The root filesystem has the same UUID as the live system's (the boot
 * menu on the EFI partition names it). The kernel mounts such a copy
 * under a temporary ID for the resize (btrfs temp-fsid).
 */

#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <limits.h>
#include <sched.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <linux/btrfs.h>
#include <linux/fs.h>

#include "admin.h"

#define LIVE_VOLUME  "WORMOS_LIVE"
#define ISO_DIR      "/run/worm/install-iso"
#define TARGET_DIR   "/run/worm/install-target"
#define MIN_BYTES    (8ULL << 30)
#define SECTOR       512ULL
#define MiB          (1024 * 1024 / SECTOR)

/* The layout of scripts/mkimage.sh, in sectors. */
#define BIOS_START   40ULL
#define ESP_START    MiB
#define ESP_SIZE     (128 * MiB)
#define ROOT_START   (ESP_START + ESP_SIZE)

#define TYPE_ESP   "C12A7328-F81F-11D2-BA4B-00A0C93EC93B"
#define TYPE_ROOT  "4F68BCE3-E8CD-4DB1-96E7-FBCAF984B709"
#define TYPE_BIOS  "21686148-6449-6E6F-744E-656564454649"

__attribute__((noreturn, format(printf, 1, 2))) static void fail(const char *fmt, ...)
{
	va_list ap;

	fputs("install-system: ", stderr);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
	exit(1);
}

static int live_system(void)
{
	char buf[4096];
	FILE *f = fopen("/proc/cmdline", "re");
	int live = 0;

	if (f && fgets(buf, sizeof(buf), f))
		for (char *t = strtok(buf, " \n"); t; t = strtok(NULL, " \n"))
			live |= !strcmp(t, "wormos.live");
	if (f)
		fclose(f);
	return live;
}

static int read_sys(const char *name, const char *file, char *out, size_t len)
{
	char path[PATH_MAX];
	FILE *f;

	snprintf(path, sizeof(path), "/sys/class/block/%s/%s", name, file);
	if (!(f = fopen(path, "re")))
		return -1;
	if (!fgets(out, len, f))
		out[0] = '\0';
	fclose(f);
	out[strcspn(out, "\n")] = '\0';
	return 0;
}

/* Is the device the WormOS ISO (a CD, or a USB stick written with it)? */
static int is_live_iso(const char *dev)
{
	char pvd[2048], want[32];
	int fd = open(dev, O_RDONLY | O_CLOEXEC), ok;

	if (fd < 0)
		return 0;
	ok = pread(fd, pvd, sizeof(pvd), 16 * 2048) == (ssize_t)sizeof(pvd);
	close(fd);
	memset(want, ' ', sizeof(want));
	memcpy(want, LIVE_VOLUME, strlen(LIVE_VOLUME));
	return ok && pvd[0] == 1 && !memcmp(pvd + 1, "CD001", 5) &&
	       !memcmp(pvd + 40, want, sizeof(want));
}

/* Is the disk, or one of its partitions, mounted or used as swap? */
static int disk_in_use(const char *name)
{
	char line[1024], dev[300];
	size_t n = strlen(name);
	int used = 0;
	FILE *f;

	for (int i = 0; i < 2 && !used; i++) {
		if (!(f = fopen(i ? "/proc/swaps" : "/proc/mounts", "re")))
			continue;
		while (!used && fgets(line, sizeof(line), f))
			used = sscanf(line, "%299s", dev) == 1 && !strncmp(dev, "/dev/", 5) &&
			       !strncmp(dev + 5, name, n);
		fclose(f);
	}
	return used;
}

/* Disks: whole devices that are not loop, RAM, CD or the ISO itself. */
static int usable_disk(const char *name, unsigned long long *bytes)
{
	char buf[64], dev[300];

	if (name[0] == '.' || !strncmp(name, "loop", 4) || !strncmp(name, "ram", 3) ||
	    !strncmp(name, "zram", 4) || !strncmp(name, "sr", 2) || !strncmp(name, "dm-", 3) ||
	    !strncmp(name, "md", 2) || strlen(name) > 64)
		return 0;
	if (read_sys(name, "partition", buf, sizeof(buf)) == 0)
		return 0;
	if (read_sys(name, "size", buf, sizeof(buf)) < 0)
		return 0;
	*bytes = strtoull(buf, NULL, 10) * SECTOR;
	snprintf(dev, sizeof(dev), "/dev/%s", name);
	return *bytes > 0 && !is_live_iso(dev);
}

static int cmd_list(void)
{
	DIR *d = opendir("/sys/class/block");
	struct dirent *e;

	while (d && (e = readdir(d))) {
		char removable[8] = "0", model[128] = "", vendor[64] = "";
		unsigned long long bytes;

		if (!usable_disk(e->d_name, &bytes) || bytes < MIN_BYTES || disk_in_use(e->d_name))
			continue;
		read_sys(e->d_name, "removable", removable, sizeof(removable));
		read_sys(e->d_name, "device/vendor", vendor, sizeof(vendor));
		read_sys(e->d_name, "device/model", model, sizeof(model));
		if (!model[0])
			snprintf(model, sizeof(model), "Disk");
		for (char *p = model; *p; p++)
			if (*p < ' ' || *p > '~')
				*p = ' ';
		printf("%s %llu %s %s%s%s\n", e->d_name, bytes, removable, vendor,
		       vendor[0] ? " " : "", model);
	}
	if (d)
		closedir(d);
	return 0;
}

/* ---- the partition table ---- */

static uint32_t crc32(const void *data, size_t len)
{
	static uint32_t table[256];
	const unsigned char *p = data;
	uint32_t c = 0xffffffff;

	if (!table[1])
		for (uint32_t i = 0; i < 256; i++) {
			uint32_t v = i;

			for (int k = 0; k < 8; k++)
				v = v & 1 ? 0xedb88320 ^ (v >> 1) : v >> 1;
			table[i] = v;
		}
	while (len--)
		c = table[(c ^ *p++) & 0xff] ^ (c >> 8);
	return c ^ 0xffffffff;
}

static void put16(unsigned char *p, uint16_t v) { p[0] = v; p[1] = v >> 8; }
static void put32(unsigned char *p, uint32_t v) { put16(p, v); put16(p + 2, v >> 16); }
static void put64(unsigned char *p, uint64_t v) { put32(p, v); put32(p + 4, v >> 32); }

/* A GUID in its on-disk form (the first three fields little-endian). */
static void guid(unsigned char out[16], const char *text)
{
	unsigned char b[16];

	for (int i = 0, j = 0; i < 16; j++) {
		if (text[j] == '-')
			continue;
		sscanf(text + j, "%2hhx", &b[i++]);
		j++;
	}
	out[0] = b[3]; out[1] = b[2]; out[2] = b[1]; out[3] = b[0];
	out[4] = b[5]; out[5] = b[4]; out[6] = b[7]; out[7] = b[6];
	memcpy(out + 8, b + 8, 8);
}

static void random_guid(unsigned char out[16])
{
	int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);

	if (fd < 0 || read(fd, out, 16) != 16)
		fail("cannot read random numbers");
	close(fd);
	out[7] = (out[7] & 0x0f) | 0x40;        /* version 4 (little-endian field) */
	out[8] = (out[8] & 0x3f) | 0x80;
}

static void entry(unsigned char *e, const char *type, uint64_t first, uint64_t last,
		  const char *name)
{
	guid(e, type);
	random_guid(e + 16);
	put64(e + 32, first);
	put64(e + 40, last);
	for (int i = 0; i < 36 && name[i]; i++)
		put16(e + 56 + 2 * i, (unsigned char)name[i]);
}

static void pwrite_all(int fd, const void *buf, size_t len, uint64_t off)
{
	const char *p = buf;

	while (len > 0) {
		ssize_t w = pwrite(fd, p, len, off);

		if (w < 0 && errno == EINTR)
			continue;
		if (w <= 0)
			fail("cannot write to the disk: %s", strerror(errno));
		p += w;
		len -= w;
		off += w;
	}
}

static void header(unsigned char *h, uint64_t self, uint64_t other, uint64_t first_usable,
		   uint64_t last_usable, const unsigned char disk[16], uint64_t entries_lba,
		   uint32_t entries_crc)
{
	memset(h, 0, SECTOR);
	memcpy(h, "EFI PART", 8);
	put32(h + 8, 0x00010000);
	put32(h + 12, 92);
	put64(h + 24, self);
	put64(h + 32, other);
	put64(h + 40, first_usable);
	put64(h + 48, last_usable);
	memcpy(h + 56, disk, 16);
	put64(h + 72, entries_lba);
	put32(h + 80, 128);
	put32(h + 84, 128);
	put32(h + 88, entries_crc);
	put32(h + 16, crc32(h, 92));
}

/* Returns the root partition's size in sectors. */
static uint64_t write_gpt(int fd, uint64_t sectors, const unsigned char *head)
{
	unsigned char mbr[SECTOR], h[SECTOR], entries[128 * 128] = { 0 }, disk[16];
	uint64_t last = sectors - 1, last_usable = sectors - 34;
	uint64_t root_end = (last_usable + 1) / MiB * MiB - 1;
	uint32_t ecrc;

	/* Protective MBR, with GRUB's boot code from the ISO. */
	memset(mbr, 0, sizeof(mbr));
	memcpy(mbr, head, 440);
	mbr[446 + 1] = 0x00;
	mbr[446 + 2] = 0x02;
	mbr[446 + 4] = 0xee;
	mbr[446 + 5] = 0xff;
	mbr[446 + 6] = 0xff;
	mbr[446 + 7] = 0xff;
	put32(mbr + 446 + 8, 1);
	put32(mbr + 446 + 12, last > 0xffffffff ? 0xffffffff : (uint32_t)last);
	mbr[510] = 0x55;
	mbr[511] = 0xaa;

	entry(entries, TYPE_ESP, ESP_START, ESP_START + ESP_SIZE - 1, "EFI");
	entry(entries + 128, TYPE_ROOT, ROOT_START, root_end, "WormOS");
	entry(entries + 256, TYPE_BIOS, BIOS_START, MiB - 1, "BIOS boot");
	ecrc = crc32(entries, sizeof(entries));
	random_guid(disk);

	pwrite_all(fd, mbr, SECTOR, 0);
	header(h, 1, last, 34, last_usable, disk, 2, ecrc);
	pwrite_all(fd, h, SECTOR, SECTOR);
	pwrite_all(fd, entries, sizeof(entries), 2 * SECTOR);
	pwrite_all(fd, entries, sizeof(entries), (last - 32) * SECTOR);
	header(h, last, 1, 34, last_usable, disk, last - 32, ecrc);
	pwrite_all(fd, h, SECTOR, last * SECTOR);
	return root_end - ROOT_START + 1;
}

/* ---- copying ---- */

static void progress(int pct)
{
	static int last = -1;

	if (pct != last) {
		printf("progress %d\n", pct);
		fflush(stdout);
		last = pct;
	}
}

/*
 * Copy everything from in to the disk at off (at most limit bytes). Progress
 * goes from base to base + span percent over total bytes (0: unknown).
 */
static void copy_to(int in, int out, uint64_t off, uint64_t limit, uint64_t total,
		    int base, int span)
{
	static char buf[4 << 20];
	uint64_t done = 0;
	ssize_t r;

	while ((r = read(in, buf, sizeof(buf))) != 0) {
		if (r < 0) {
			if (errno == EINTR)
				continue;
			fail("cannot read the installation files: %s", strerror(errno));
		}
		if (done + r > limit)
			fail("the installation files do not fit the partition");
		pwrite_all(out, buf, r, off + done);
		done += r;
		if (total)
			progress(base + (int)(done * span / total));
	}
	progress(base + span);
}

static void step(const char *text)
{
	printf("step %s\n", text);
	fflush(stdout);
}

/* The ISO, mounted where only this process sees it. */
static void mount_iso(void)
{
	DIR *d = opendir("/sys/class/block");
	struct dirent *e;
	char dev[300];
	int found = 0;

	while (!found && d && (e = readdir(d))) {
		if (e->d_name[0] == '.' || !strncmp(e->d_name, "loop", 4))
			continue;
		snprintf(dev, sizeof(dev), "/dev/%s", e->d_name);
		found = is_live_iso(dev);
	}
	if (d)
		closedir(d);
	if (!found)
		fail("the WormOS ISO is not there any more; connect it again");
	mkdir("/run/worm", 0755);
	mkdir(ISO_DIR, 0700);
	if (mount(dev, ISO_DIR, "iso9660", MS_RDONLY | MS_NOSUID | MS_NODEV, NULL) < 0)
		fail("cannot mount the ISO: %s", strerror(errno));
}

static int open_iso(const char *file, uint64_t *size)
{
	char path[PATH_MAX];
	struct stat st;
	int fd;

	snprintf(path, sizeof(path), "%s/wormos/%s", ISO_DIR, file);
	if ((fd = open(path, O_RDONLY | O_CLOEXEC)) < 0 || fstat(fd, &st) < 0)
		fail("cannot open %s on the ISO: %s", file, strerror(errno));
	*size = st.st_size;
	return fd;
}

static uint64_t iso_number(const char *file)
{
	char buf[32] = "";
	uint64_t size;
	int fd = open_iso(file, &size);
	ssize_t r = read(fd, buf, sizeof(buf) - 1);

	close(fd);
	if (r <= 0 || (size = strtoull(buf, NULL, 10)) == 0)
		fail("%s on the ISO is damaged", file);
	return size;
}

/* The decompressed contents of a zstd image on the ISO. */
static int unzstd(int in, pid_t *pid)
{
	int p[2];

	if (pipe2(p, O_CLOEXEC) < 0 || (*pid = fork()) < 0)
		fail("cannot run zstd: %s", strerror(errno));
	if (*pid == 0) {
		dup2(in, 0);
		dup2(p[1], 1);
		execl("/usr/bin/zstd", "zstd", "-dqc", (char *)NULL);
		_exit(127);
	}
	close(p[1]);
	return p[0];
}

static void partition_path(const char *disk, int n, char *out, size_t len)
{
	size_t l = strlen(disk);

	/* nvme0n1 -> nvme0n1p2, mmcblk0 -> mmcblk0p2, sda -> sda2 */
	snprintf(out, len, "/dev/%s%s%d", disk, l && disk[l - 1] >= '0' && disk[l - 1] <= '9' ?
		 "p" : "", n);
}

/* Run argv with input (or NULL) as its standard input; its status. */
static int run_with_input(char *const argv[], const char *input, size_t len)
{
	int p[2], status;
	pid_t pid;

	if (pipe2(p, O_CLOEXEC) < 0 || (pid = fork()) < 0)
		fail("cannot run %s: %s", argv[0], strerror(errno));
	if (pid == 0) {
		dup2(p[0], 0);
		execv(argv[0], argv);
		_exit(127);
	}
	close(p[0]);
	if (input && len && write(p[1], input, len) != (ssize_t)len)
		fail("cannot pass the passphrase on: %s", strerror(errno));
	close(p[1]);
	if (waitpid(pid, &status, 0) < 0 || !WIFEXITED(status))
		return 1;
	return WEXITSTATUS(status);
}

#define CRYPTSETUP "/usr/bin/cryptsetup"
#define CRYPT_NAME "wormos-install"

/*
 * Encrypt the root partition: LUKS2 with the passphrase, a second slot with
 * a random key (kept in a memfd, given to cryptsetup by its /proc path),
 * then open it. Returns the key's memfd.
 */
static int encrypt_root(const char *part, const char *passphrase)
{
	unsigned char key[64];
	char keypath[64];
	int keyfd;

	if (getrandom(key, sizeof(key), 0) != (ssize_t)sizeof(key))
		fail("no random numbers for the key: %s", strerror(errno));
	if ((keyfd = memfd_create("luks-key", 0)) < 0 ||
	    write(keyfd, key, sizeof(key)) != (ssize_t)sizeof(key))
		fail("cannot hold the key: %s", strerror(errno));
	explicit_bzero(key, sizeof(key));
	snprintf(keypath, sizeof(keypath), "/proc/self/fd/%d", keyfd);

	step("Encrypting the disk");
	/* PBKDF2 and 512-byte sectors: what GRUB can open. */
	if (run_with_input((char *[]){ CRYPTSETUP, "luksFormat", "--batch-mode", "--type", "luks2",
				       "--pbkdf", "pbkdf2", "--iter-time", "1500", "--sector-size", "512",
				       "--key-file", "-", (char *)part, NULL },
			   passphrase, strlen(passphrase)) != 0)
		fail("cannot encrypt %s", part);
	if (run_with_input((char *[]){ CRYPTSETUP, "luksAddKey", "--batch-mode", "--pbkdf", "pbkdf2",
				       "--iter-time", "100", "--key-file", "-", (char *)part, keypath,
				       NULL }, passphrase, strlen(passphrase)) != 0)
		fail("cannot add the system's key to %s", part);
	if (run_with_input((char *[]){ CRYPTSETUP, "open", "--key-file", keypath, (char *)part,
				       CRYPT_NAME, NULL }, NULL, 0) != 0)
		fail("cannot open the encrypted disk");
	return keyfd;
}

/* The key into the new system (root only), and its initramfs built again. */
static void store_key(int keyfd)
{
	unsigned char key[64];
	char path[PATH_MAX], root[256];
	int out;

	if (pread(keyfd, key, sizeof(key), 0) != (ssize_t)sizeof(key))
		fail("cannot read the key back");
	snprintf(root, sizeof(root), "%s/@", TARGET_DIR);
	snprintf(path, sizeof(path), "%s/etc/worm", root);
	mkdir(path, 0755);
	snprintf(path, sizeof(path), "%s/etc/worm/luks.key", root);
	if ((out = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0400)) < 0 ||
	    write(out, key, sizeof(key)) != (ssize_t)sizeof(key) || fsync(out) < 0)
		fail("cannot store the key in the new system: %s", strerror(errno));
	close(out);
	explicit_bzero(key, sizeof(key));
	if (run_with_input((char *[]){ "/usr/lib/wormrd/mkinitramfs", root, NULL }, NULL, 0) != 0)
		fail("cannot build the new system's initramfs");
}

static int cmd_install(const char *name, const char *passphrase)
{
	char dev[300], part[320];
	struct btrfs_ioctl_vol_args grow = { 0 };
	unsigned char head[MiB * SECTOR];
	unsigned long long bytes;
	uint64_t size, unused, root_sectors, target_off, target_len;
	int fd, in, z, top, status, target, keyfd = -1;
	pid_t pid;

	if (strchr(name, '/') || !usable_disk(name, &bytes))
		fail("%s is not a disk WormOS can be installed on", name);
	if (bytes < MIN_BYTES)
		fail("the disk is too small (WormOS needs at least 8 GB)");
	if (disk_in_use(name))
		fail("the disk is in use");
	snprintf(dev, sizeof(dev), "/dev/%s", name);

	if (unshare(CLONE_NEWNS) < 0 || mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) < 0)
		fail("cannot set up a private mount namespace: %s", strerror(errno));
	step("Reading the installation files");
	mount_iso();
	in = open_iso("head.img", &size);
	if (size != sizeof(head) || read(in, head, sizeof(head)) != (ssize_t)sizeof(head))
		fail("head.img on the ISO is damaged");
	close(in);

	if ((fd = open(dev, O_RDWR | O_EXCL | O_CLOEXEC)) < 0)
		fail("cannot open %s: %s", dev, strerror(errno));

	step("Partitioning the disk");
	root_sectors = write_gpt(fd, bytes / SECTOR, head);
	/* GRUB for BIOS, in the BIOS boot partition. */
	pwrite_all(fd, head + BIOS_START * SECTOR, (MiB - BIOS_START) * SECTOR,
		   BIOS_START * SECTOR);

	step("Writing the boot loader");
	in = open_iso("esp.img.zst", &size);
	z = unzstd(in, &pid);
	close(in);
	copy_to(z, fd, ESP_START * SECTOR, ESP_SIZE * SECTOR, 0, 0, 5);
	close(z);
	if (waitpid(pid, &status, 0) < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0)
		fail("cannot unpack the EFI system partition");

	partition_path(name, 2, part, sizeof(part));
	if (passphrase) {
		uint64_t mapped;

		/* The kernel reads the new partition table first. */
		if (fsync(fd) < 0)
			fail("cannot write to the disk: %s", strerror(errno));
		for (int i = 0; ioctl(fd, BLKRRPART) < 0; i++) {
			if (i == 20)
				fail("the kernel does not see the new partitions: %s", strerror(errno));
			usleep(250000);
		}
		for (int i = 0; i < 100 && access(part, F_OK) < 0; i++)
			usleep(50000);
		/* An exclusive hold on the disk keeps cryptsetup from claiming
		 * the partition: reopen it without one. */
		close(fd);
		if ((fd = open(dev, O_RDWR | O_CLOEXEC)) < 0)
			fail("cannot open %s: %s", dev, strerror(errno));
		keyfd = encrypt_root(part, passphrase);
		if ((target = open("/dev/mapper/" CRYPT_NAME, O_RDWR | O_CLOEXEC)) < 0 ||
		    ioctl(target, BLKGETSIZE64, &mapped) < 0)
			fail("cannot open the encrypted disk: %s", strerror(errno));
		target_off = 0;
		target_len = mapped;
	} else {
		target = fd;
		target_off = ROOT_START * SECTOR;
		target_len = root_sectors * SECTOR;
	}

	step("Copying the system");
	size = iso_number("system.size");
	in = open_iso("system.img.zst", &unused);
	z = unzstd(in, &pid);
	close(in);
	copy_to(z, target, target_off, target_len, size, 5, 90);
	close(z);
	if (waitpid(pid, &status, 0) < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0)
		fail("the system image on the ISO is damaged");
	umount2(ISO_DIR, MNT_DETACH);

	step("Finishing");
	if (fsync(target) < 0 || fsync(fd) < 0)
		fail("cannot write to the disk: %s", strerror(errno));
	if (passphrase) {
		close(target);
	} else {
		/* The kernel reads the new partition table. */
		for (int i = 0; ioctl(fd, BLKRRPART) < 0; i++) {
			if (i == 20)
				fail("the kernel does not see the new partitions: %s", strerror(errno));
			usleep(250000);
		}
	}
	close(fd);

	/* Grow the root filesystem to fill its partition. */
	for (int i = 0; i < 100 && access(part, F_OK) < 0; i++)
		usleep(50000);
	mkdir(TARGET_DIR, 0700);
	if (mount(passphrase ? "/dev/mapper/" CRYPT_NAME : part, TARGET_DIR, "btrfs",
		  MS_NOSUID | MS_NODEV, "subvolid=5") < 0)
		fail("cannot mount the new system (%s): %s", part, strerror(errno));
	snprintf(grow.name, sizeof(grow.name), "max");
	if ((top = open(TARGET_DIR, O_RDONLY | O_DIRECTORY | O_CLOEXEC)) < 0 ||
	    ioctl(top, BTRFS_IOC_RESIZE, &grow) < 0)
		fail("cannot grow the new system's filesystem: %s", strerror(errno));
	if (passphrase)
		store_key(keyfd);
	syncfs(top);
	close(top);
	if (umount(TARGET_DIR) < 0)
		fail("cannot unmount the new system: %s", strerror(errno));
	if (passphrase) {
		close(keyfd);
		run_with_input((char *[]){ CRYPTSETUP, "close", CRYPT_NAME, NULL }, NULL, 0);
	}
	sync();
	printf("progress 100\ndone\n");
	return 0;
}

int main(int argc, char **argv)
{
	clearenv();
	setenv("PATH", "/usr/bin:/usr/sbin", 1);
	if (!live_system())
		fail("WormOS can only be installed from the live system (the ISO)");
	if (!caller_is_admin())
		fail("only administrators (the group wheel) can do this");
	if (setgroups(0, NULL) < 0 || setgid(0) < 0 || setuid(0) < 0)
		fail("cannot become root");
	umask(022);
	if (argc == 2 && !strcmp(argv[1], "list"))
		return cmd_list();
	if (argc == 3 && !strcmp(argv[1], "install"))
		return cmd_install(argv[2], NULL);
	if (argc == 4 && !strcmp(argv[1], "install") && !strcmp(argv[3], "--encrypt")) {
		static char passphrase[1024];

		if (!fgets(passphrase, sizeof(passphrase), stdin))
			passphrase[0] = '\0';
		passphrase[strcspn(passphrase, "\n")] = '\0';
		if (strlen(passphrase) < 8)
			fail("the passphrase needs at least 8 characters");
		return cmd_install(argv[2], passphrase);
	}
	fprintf(stderr, "usage: install-system list | install DISK [--encrypt]\n");
	return 2;
}
