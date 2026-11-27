/*
 * wormrd - the WormOS initramfs init.
 *
 * Runs as PID 1 from the initramfs. It finds the btrfs filesystem holding
 * the system, gives the snapshot tool a chance to apply pending operations
 * (such as a rewind) while nothing from the system is running yet, mounts
 * the system root read-only and hands over to the real init.
 *
 * With --soft-reboot it does the same from the installed system: worminit
 * execs it after stopping all processes, so a rewind takes effect without
 * restarting the kernel. The new root replaces the old one with pivot_root.
 *
 * Kernel command line:
 *
 *     root=UUID=...|LABEL=...|/dev/...   filesystem holding the system
 *     rootflags=...                       btrfs mount options (default subvol=@)
 *     rw                                  mount the root read-write
 *     init=PATH                           init to start (default /sbin/init)
 *     wormos.rescue                       open a shell before mounting the root
 *     wormos.rewind=ID                    rewind the system to snapshot ID first
 *                                         (used by the boot menu entries)
 *     wormos.live                         live system: the system image on the
 *                                         WormOS ISO, copied into memory
 *
 * An encrypted root (LUKS2, made by the installer) is opened as
 * /dev/mapper/wormos-root when the root= filesystem is not found on any
 * plain device: with /crypto_keyfile (the installed system's key, put in
 * the initramfs by mkinitramfs, so GRUB alone asks for the passphrase)
 * or, without it, by cryptsetup asking for the passphrase on the console.
 *
 * Live mode needs no root=: wormrd finds the ISO by its volume ID, unpacks
 * wormos/system.img.zst (a btrfs image with no free space) into a tmpfs file
 * as large as three quarters of the memory, attaches it to a loop device
 * and grows the filesystem to the file's size. Everything then works as on
 * a disk, in memory, and the boot medium can be removed. The device the
 * root came from is linked as /dev/wormos-root (fstab mounts the other
 * subvolumes from it, and a soft reboot finds the root there again); the
 * live system's boot medium as /dev/wormos-medium.
 */

#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <linux/loop.h>
#include <time.h>
#include <unistd.h>
#include <linux/btrfs.h>

static const char *sysroot = "/sysroot";
static const char *topdir = "/top";
#define SOFT_RUN      "/run"
#define SOFT_SYSROOT  "/run/wormrd/sysroot"
#define SOFT_TOPDIR   "/run/wormrd/top"
#define SNAP_TOOL     "/usr/bin/snap"
#define CRYPTSETUP    "/usr/bin/cryptsetup"
#define KEYFILE       "/crypto_keyfile"
#define CRYPT_NAME    "wormos-root"
#define RESCUE_SHELL  "/bin/sh"
#define ROOT_WAIT_MS  10000

/* Btrfs superblock: 64 KiB into the device. */
#define SB_OFFSET     65536
#define SB_FSID       0x20
#define SB_MAGIC      0x40
#define SB_LABEL      0x12b
#define SB_LABEL_SIZE 256
#define SB_SIZE       (SB_LABEL + SB_LABEL_SIZE)
#define BTRFS_MAGIC   "_BHRfS_M"

static char opt_root[256];
static char opt_rootflags[512] = "subvol=@";
static char opt_init[PATH_MAX] = "/sbin/init";
static char opt_rewind[16];
static int opt_rw;
static int opt_rescue;
static int opt_live;
static int soft_reboot;

#define ROOT_LINK     "/dev/wormos-root"
#define MEDIUM_LINK   "/dev/wormos-medium"
#define LIVE_VOLUME   "WORMOS_LIVE"
#define LIVE_ISO      "/iso"
#define LIVE_RAM      "/live"
#define LIVE_IMAGE    LIVE_ISO "/wormos/system.img.zst"
#define ZSTD          "/usr/bin/zstd"
#define LIVE_WAIT_MS  30000

static void log_msg(const char *fmt, ...)
{
	va_list ap;

	fputs("wormrd: ", stderr);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
}

static void run_shell(void)
{
	pid_t pid;

	if (access(RESCUE_SHELL, X_OK) < 0) {
		log_msg("no rescue shell available");
		return;
	}
	log_msg("starting rescue shell, exit it to continue");
	pid = fork();
	if (pid == 0) {
		setsid();
		ioctl(0, TIOCSCTTY, 1);
		execl(RESCUE_SHELL, "sh", (char *)NULL);
		_exit(127);
	}
	if (pid > 0)
		while (waitpid(pid, NULL, 0) < 0 && errno == EINTR)
			;
}

static __attribute__((noreturn)) void fatal(const char *fmt, ...)
{
	va_list ap;

	fputs("wormrd: ", stderr);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);

	run_shell();
	log_msg("cannot boot, rebooting in 10 seconds");
	sleep(10);
	reboot(RB_AUTOBOOT);
	for (;;)
		pause();
}

static void mount_fs(const char *src, const char *dst, const char *type,
		     unsigned long flags, const char *data)
{
	mkdir(dst, 0755);
	if (mount(src, dst, type, flags, data) < 0)
		fatal("cannot mount %s: %s", dst, strerror(errno));
}

static void parse_cmdline(void)
{
	char buf[4096], *tok, *save;
	FILE *f = fopen("/proc/cmdline", "r");

	if (!f)
		fatal("cannot read /proc/cmdline: %s", strerror(errno));
	if (!fgets(buf, sizeof(buf), f))
		buf[0] = '\0';
	fclose(f);

	for (tok = strtok_r(buf, " \t\n", &save); tok;
	     tok = strtok_r(NULL, " \t\n", &save)) {
		if (strncmp(tok, "root=", 5) == 0)
			snprintf(opt_root, sizeof(opt_root), "%s", tok + 5);
		else if (strncmp(tok, "rootflags=", 10) == 0)
			snprintf(opt_rootflags, sizeof(opt_rootflags), "%s", tok + 10);
		else if (strncmp(tok, "init=", 5) == 0)
			snprintf(opt_init, sizeof(opt_init), "%s", tok + 5);
		else if (strcmp(tok, "rw") == 0)
			opt_rw = 1;
		else if (strcmp(tok, "ro") == 0)
			opt_rw = 0;
		else if (strcmp(tok, "wormos.live") == 0)
			opt_live = 1;
		else if (strcmp(tok, "wormos.rescue") == 0)
			opt_rescue = !soft_reboot;
		else if (strncmp(tok, "wormos.rewind=", 14) == 0 && !soft_reboot &&
			 strspn(tok + 14, "0123456789") == strlen(tok + 14))
			snprintf(opt_rewind, sizeof(opt_rewind), "%s", tok + 14);
	}
	if (!opt_root[0] && !opt_live)
		fatal("no root= on the kernel command line");
}

static void format_uuid(const unsigned char *u, char *out)
{
	sprintf(out, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-"
		"%02x%02x%02x%02x%02x%02x",
		u[0], u[1], u[2], u[3], u[4], u[5], u[6], u[7],
		u[8], u[9], u[10], u[11], u[12], u[13], u[14], u[15]);
}

/* Register a btrfs device with the kernel (needed for multi-device filesystems). */
static void btrfs_scan(const char *dev)
{
	struct btrfs_ioctl_vol_args args = { 0 };
	int fd = open("/dev/btrfs-control", O_RDWR | O_CLOEXEC);

	if (fd < 0)
		return;
	snprintf(args.name, sizeof(args.name), "%s", dev);
	ioctl(fd, BTRFS_IOC_SCAN_DEV, &args);
	close(fd);
}

/* Does the btrfs filesystem on dev match a UUID= or LABEL= spec? */
static int device_matches(const char *dev, const char *spec)
{
	unsigned char sb[SB_SIZE];
	char uuid[37], label[SB_LABEL_SIZE + 1];
	int fd = open(dev, O_RDONLY | O_CLOEXEC);
	ssize_t n;

	if (fd < 0)
		return 0;
	n = pread(fd, sb, sizeof(sb), SB_OFFSET);
	close(fd);
	if (n != (ssize_t)sizeof(sb) || memcmp(sb + SB_MAGIC, BTRFS_MAGIC, 8) != 0)
		return 0;

	btrfs_scan(dev);
	if (strncmp(spec, "UUID=", 5) == 0) {
		format_uuid(sb + SB_FSID, uuid);
		return strcasecmp(uuid, spec + 5) == 0;
	}
	if (strncmp(spec, "LABEL=", 6) == 0) {
		memcpy(label, sb + SB_LABEL, SB_LABEL_SIZE);
		label[SB_LABEL_SIZE] = '\0';
		return strcmp(label, spec + 6) == 0;
	}
	return 0;
}

static int scan_devices(const char *spec, char *out, size_t outlen)
{
	DIR *d = opendir("/sys/class/block");
	struct dirent *e;
	int found = 0;

	if (!d)
		return 0;
	while (!found && (e = readdir(d))) {
		char dev[PATH_MAX];

		if (e->d_name[0] == '.')
			continue;
		snprintf(dev, sizeof(dev), "/dev/%s", e->d_name);
		if (device_matches(dev, spec)) {
			snprintf(out, outlen, "%s", dev);
			found = 1;
		}
	}
	closedir(d);
	return found;
}

/* Is there a LUKS header at the start of dev? */
static int is_luks(const char *dev)
{
	unsigned char magic[6];
	int fd = open(dev, O_RDONLY | O_CLOEXEC);
	ssize_t n;

	if (fd < 0)
		return 0;
	n = pread(fd, magic, sizeof(magic), 0);
	close(fd);
	return n == 6 && !memcmp(magic, "LUKS\xba\xbe", 6);
}

/* Run cryptsetup open on dev, with the key file when there is one. */
static int luks_open_one(const char *dev, int keyfile)
{
	pid_t pid;
	int status;

	pid = fork();
	if (pid == 0) {
		if (keyfile)
			execl(CRYPTSETUP, "cryptsetup", "open", "--key-file", KEYFILE, dev, CRYPT_NAME,
			      (char *)NULL);
		else
			execl(CRYPTSETUP, "cryptsetup", "open", "--tries", "5", dev, CRYPT_NAME,
			      (char *)NULL);
		_exit(127);
	}
	if (pid < 0 || waitpid(pid, &status, 0) < 0)
		return 0;
	return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

/*
 * The root filesystem may be inside an encrypted partition: open the first
 * LUKS device found (the installer makes one). Returns whether one opened.
 */
static int luks_open(void)
{
	DIR *d;
	struct dirent *e;
	int opened = 0;

	if (access(CRYPTSETUP, X_OK) < 0 || access("/dev/mapper/" CRYPT_NAME, F_OK) == 0)
		return 0;
	if (!(d = opendir("/sys/class/block")))
		return 0;
	mkdir("/run/cryptsetup", 0700);
	while (!opened && (e = readdir(d))) {
		char dev[PATH_MAX];

		if (e->d_name[0] == '.')
			continue;
		snprintf(dev, sizeof(dev), "/dev/%s", e->d_name);
		if (!is_luks(dev))
			continue;
		if (access(KEYFILE, R_OK) == 0 && luks_open_one(dev, 1)) {
			opened = 1;
			break;
		}
		fprintf(stderr, "\nWormOS: the disk is encrypted. Type its passphrase.\n");
		opened = luks_open_one(dev, 0);
		if (!opened)
			fatal("the encrypted disk could not be opened");
	}
	closedir(d);
	return opened;
}

/* Resolve root= to a device node, waiting for it to appear. */
static void find_root(char *dev, size_t len)
{
	struct timespec delay = { 0, 100 * 1000000L };
	int luks_tried = 0;

	for (int waited = 0; waited < ROOT_WAIT_MS; waited += 100) {
		if (opt_root[0] == '/') {
			if (access(opt_root, F_OK) == 0) {
				snprintf(dev, len, "%s", opt_root);
				btrfs_scan(dev);
				return;
			}
		} else if (scan_devices(opt_root, dev, len)) {
			return;
		}
		/* Not on a plain device: perhaps inside an encrypted one. */
		if (!luks_tried && waited >= 500) {
			luks_tried = 1;
			if (luks_open())
				continue;
		}
		nanosleep(&delay, NULL);
	}
	fatal("root device %s not found", opt_root);
}

/*
 * Mount options for the top-level subvolume: rootflags without the subvolume
 * selection. Filesystem-wide options must match, or a read-write mount of an
 * already mounted filesystem would reset them.
 */
static void top_options(char *out, size_t len)
{
	char buf[sizeof(opt_rootflags)], *tok, *save;
	size_t n = 0;

	snprintf(buf, sizeof(buf), "%s", opt_rootflags);
	for (tok = strtok_r(buf, ",", &save); tok; tok = strtok_r(NULL, ",", &save))
		if (strncmp(tok, "subvol=", 7) && strncmp(tok, "subvolid=", 9) &&
		    n < len)
			n += snprintf(out + n, len - n, "%s,", tok);
	if (n < len)
		snprintf(out + n, len - n, "subvolid=5");
}

/*
 * Let the snapshot tool apply pending operations on the top-level subvolume,
 * before any program from the system root has run.
 */
static void run_snapshot_hooks(const char *dev)
{
	char opts[sizeof(opt_rootflags) + 16];
	pid_t pid;
	int status;

	if (access(SNAP_TOOL, X_OK) < 0)
		return;
	top_options(opts, sizeof(opts));
	mount_fs(dev, topdir, "btrfs", MS_NOSUID | MS_NODEV, opts);

	pid = fork();
	if (pid == 0) {
		if (opt_rewind[0])
			execl(SNAP_TOOL, "snap", "--top", topdir, "boot",
			      "--rewind", opt_rewind, (char *)NULL);
		else
			execl(SNAP_TOOL, "snap", "--top", topdir, "boot", (char *)NULL);
		_exit(127);
	}
	if (pid < 0 || waitpid(pid, &status, 0) < 0)
		log_msg("cannot run %s: %s", SNAP_TOOL, strerror(errno));
	else if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
		log_msg("snapshot boot tasks failed, booting the current system");

	if (umount(topdir) < 0)
		fatal("cannot unmount %s: %s", topdir, strerror(errno));
}

/* ---- live mode ---- */

/* Is dev an ISO 9660 filesystem with the WormOS live volume ID? */
static int is_live_iso(const char *dev)
{
	char pvd[2048], want[32];
	int fd = open(dev, O_RDONLY | O_CLOEXEC), ok;

	if (fd < 0)
		return 0;
	ok = pread(fd, pvd, sizeof(pvd), 16 * 2048) == (ssize_t)sizeof(pvd);
	close(fd);
	/* The volume ID is padded with spaces to 32 characters. */
	memset(want, ' ', sizeof(want));
	memcpy(want, LIVE_VOLUME, strlen(LIVE_VOLUME));
	return ok && pvd[0] == 1 && !memcmp(pvd + 1, "CD001", 5) &&
	       !memcmp(pvd + 40, want, sizeof(want));
}

static int find_live_iso(char *out, size_t len)
{
	struct timespec delay = { 0, 200 * 1000000L };

	for (int waited = 0; waited < LIVE_WAIT_MS; waited += 200) {
		DIR *d = opendir("/sys/class/block");
		struct dirent *e;

		while (d && (e = readdir(d))) {
			if (e->d_name[0] == '.' || !strncmp(e->d_name, "loop", 4) ||
			    !strncmp(e->d_name, "ram", 3))
				continue;
			snprintf(out, len, "/dev/%s", e->d_name);
			if (is_live_iso(out)) {
				closedir(d);
				return 0;
			}
		}
		if (d)
			closedir(d);
		nanosleep(&delay, NULL);
	}
	return -1;
}

static unsigned long long memory_bytes(void)
{
	unsigned long long kb = 0;
	char line[128];
	FILE *f = fopen("/proc/meminfo", "re");

	while (f && fgets(line, sizeof(line), f))
		if (sscanf(line, "MemTotal: %llu kB", &kb) == 1)
			break;
	if (f)
		fclose(f);
	return kb * 1024;
}

/* Copy in to out, leaving holes where the image is all zeros. */
static void copy_sparse(int in, int out)
{
	static char buf[1 << 20];
	off_t pos = 0;
	ssize_t r;

	while ((r = read(in, buf, sizeof(buf))) > 0) {
		for (ssize_t off = 0; off < r; off += 65536) {
			ssize_t n = r - off < 65536 ? r - off : 65536, i;

			for (i = 0; i < n && !buf[off + i]; i++)
				;
			if (i < n && pwrite(out, buf + off, n, pos + off) != n)
				fatal("cannot copy the system into memory: %s", strerror(errno));
		}
		pos += r;
	}
	if (r < 0 || ftruncate(out, pos) < 0)
		fatal("cannot read the system image: %s", strerror(errno));
}

/* A pipe with the decompressed contents of a zstd file. */
static int unpack(const char *path, pid_t *pid)
{
	int p[2];

	if (pipe2(p, O_CLOEXEC) < 0 || (*pid = fork()) < 0)
		fatal("cannot run zstd: %s", strerror(errno));
	if (*pid == 0) {
		dup2(p[1], 1);
		execl(ZSTD, "zstd", "-dqc", path, (char *)NULL);
		_exit(127);
	}
	close(p[1]);
	return p[0];
}

static void live_setup(char *dev, size_t len)
{
	char iso[PATH_MAX], opts[64];
	struct loop_config cfg = { 0 };
	struct btrfs_ioctl_vol_args grow = { 0 };
	unsigned long long size = memory_bytes() / 4 * 3;
	int in, img, ctl, loop, n, top, status;
	pid_t zstd;

	if (find_live_iso(iso, sizeof(iso)) < 0)
		fatal("the WormOS ISO (volume %s) was not found", LIVE_VOLUME);
	/* The medium it started from (WormOS Amnesia powers off when it goes). */
	unlink(MEDIUM_LINK);
	if (symlink(iso, MEDIUM_LINK) < 0)
		log_msg("cannot link %s: %s", MEDIUM_LINK, strerror(errno));
	mount_fs(iso, LIVE_ISO, "iso9660", MS_RDONLY | MS_NOSUID | MS_NODEV, NULL);
	snprintf(opts, sizeof(opts), "size=%llu,mode=0700", size);
	mount_fs("tmpfs", LIVE_RAM, "tmpfs", MS_NOSUID | MS_NODEV, opts);

	log_msg("copying the system into memory");
	if ((img = open(LIVE_RAM "/system.img", O_RDWR | O_CREAT | O_CLOEXEC, 0600)) < 0)
		fatal("cannot create the system in memory: %s", strerror(errno));
	in = unpack(LIVE_IMAGE, &zstd);
	copy_sparse(in, img);
	close(in);
	if (waitpid(zstd, &status, 0) < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0)
		fatal("the system image on the ISO is damaged");
	umount2(LIVE_ISO, MNT_DETACH);
	/* Room to grow: the rest is a hole until written. */
	if (ftruncate(img, size) < 0)
		fatal("cannot size the system in memory: %s", strerror(errno));

	if ((ctl = open("/dev/loop-control", O_RDWR | O_CLOEXEC)) < 0 ||
	    (n = ioctl(ctl, LOOP_CTL_GET_FREE)) < 0)
		fatal("no loop device: %s", strerror(errno));
	close(ctl);
	snprintf(dev, len, "/dev/loop%d", n);
	for (int i = 0; i < 50 && access(dev, F_OK) < 0; i++)
		usleep(20000);
	cfg.fd = img;
	if ((loop = open(dev, O_RDWR | O_CLOEXEC)) < 0 || ioctl(loop, LOOP_CONFIGURE, &cfg) < 0)
		fatal("cannot set up %s: %s", dev, strerror(errno));
	close(loop);
	close(img);
	/* The loop device keeps the file. */
	umount2(LIVE_RAM, MNT_DETACH);
	btrfs_scan(dev);

	mount_fs(dev, topdir, "btrfs", MS_NOSUID | MS_NODEV, "subvolid=5,compress=zstd");
	snprintf(grow.name, sizeof(grow.name), "max");
	if ((top = open(topdir, O_RDONLY | O_DIRECTORY | O_CLOEXEC)) < 0 ||
	    ioctl(top, BTRFS_IOC_RESIZE, &grow) < 0)
		log_msg("cannot grow the filesystem: %s", strerror(errno));
	if (top >= 0)
		close(top);
	if (umount(topdir) < 0)
		fatal("cannot unmount %s: %s", topdir, strerror(errno));
}

/* Delete everything on the initramfs to free its memory. */
static void remove_tree(int dfd, dev_t rootdev)
{
	DIR *d = fdopendir(dfd);
	struct dirent *e;

	if (!d) {
		close(dfd);
		return;
	}
	while ((e = readdir(d))) {
		struct stat st;

		if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
			continue;
		if (fstatat(dirfd(d), e->d_name, &st, AT_SYMLINK_NOFOLLOW) < 0 ||
		    st.st_dev != rootdev)
			continue;       /* a mount point, such as /sysroot */
		if (S_ISDIR(st.st_mode)) {
			int sub = openat(dirfd(d), e->d_name,
					 O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
			if (sub >= 0)
				remove_tree(sub, rootdev);
			unlinkat(dirfd(d), e->d_name, AT_REMOVEDIR);
		} else {
			unlinkat(dirfd(d), e->d_name, 0);
		}
	}
	closedir(d);
}

static __attribute__((noreturn)) void switch_root(void)
{
	struct stat st;
	int fd;

	umount2("/dev", MNT_DETACH);
	umount2("/sys", MNT_DETACH);
	umount2("/proc", MNT_DETACH);

	fd = open("/", O_RDONLY | O_DIRECTORY);
	if (fd >= 0 && fstat(fd, &st) == 0)
		remove_tree(fd, st.st_dev);
	else if (fd >= 0)
		close(fd);

	if (chdir(sysroot) < 0 ||
	    mount(".", "/", NULL, MS_MOVE, NULL) < 0 ||
	    chroot(".") < 0 || chdir("/") < 0)
		fatal("cannot switch to the root filesystem: %s", strerror(errno));

	execl(opt_init, opt_init, (char *)NULL);
	fatal("cannot start %s: %s", opt_init, strerror(errno));
}

/*
 * Soft reboot: the old root is a normal mount, so pivot into the new one
 * and detach the old root with everything still mounted under it.
 */
static __attribute__((noreturn)) void pivot_root_to_new(void)
{
	umount2("/dev", MNT_DETACH);
	umount2("/sys", MNT_DETACH);
	umount2("/proc", MNT_DETACH);

	if (chdir(sysroot) < 0 ||
	    syscall(SYS_pivot_root, ".", ".") < 0 ||
	    umount2(".", MNT_DETACH) < 0 || chdir("/") < 0)
		fatal("cannot switch to the new root filesystem: %s", strerror(errno));

	execl(opt_init, opt_init, (char *)NULL);
	fatal("cannot start %s: %s", opt_init, strerror(errno));
}

int main(int argc, char **argv)
{
	char dev[PATH_MAX], init[PATH_MAX + 32];

	if (getpid() != 1) {
		fprintf(stderr, "wormrd: must be run as PID 1\n");
		return 1;
	}
	soft_reboot = argc > 1 && strcmp(argv[1], "--soft-reboot") == 0;

	if (soft_reboot) {
		/* /proc, /sys and /dev are still mounted; /run is gone. */
		if (mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) < 0)
			fatal("cannot make mounts private: %s", strerror(errno));
		mount_fs("tmpfs", SOFT_RUN, "tmpfs", MS_NOSUID | MS_NODEV, "mode=0700");
		mkdir("/run/wormrd", 0700);
		sysroot = SOFT_SYSROOT;
		topdir = SOFT_TOPDIR;
	} else {
		mount_fs("proc", "/proc", "proc", MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL);
		mount_fs("sysfs", "/sys", "sysfs", MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL);
		mount_fs("devtmpfs", "/dev", "devtmpfs", MS_NOSUID, "mode=0755");
	}
	setenv("PATH", "/usr/bin:/bin", 1);

	parse_cmdline();
	if (opt_rescue)
		run_shell();

	if (opt_live && soft_reboot) {
		/* The system in memory, which the first boot linked. */
		ssize_t n = readlink(ROOT_LINK, dev, sizeof(dev) - 1);

		if (n <= 0)
			fatal("cannot find the live system: %s", strerror(errno));
		dev[n] = '\0';
	} else if (opt_live) {
		live_setup(dev, sizeof(dev));
	} else {
		find_root(dev, sizeof(dev));
	}
	/* fstab mounts the other subvolumes from the same device. */
	unlink(ROOT_LINK);
	if (symlink(dev, ROOT_LINK) < 0)
		log_msg("cannot link %s: %s", ROOT_LINK, strerror(errno));
	run_snapshot_hooks(dev);

	mount_fs(dev, sysroot, "btrfs",
		 (opt_rw ? 0 : MS_RDONLY) | MS_NOATIME, opt_rootflags);
	snprintf(init, sizeof(init), "%s%s", sysroot, opt_init);
	if (access(init, X_OK) < 0)
		fatal("%s not found on the root filesystem", opt_init);

	if (soft_reboot)
		pivot_root_to_new();
	switch_root();
}
