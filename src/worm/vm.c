/*
 * worm vm - running in a virtual machine.
 *
 *     worm vm                    what WormOS found and the settings in use
 *     worm vm setup              (boot, root) detect, apply, write /run/worm/vm
 *     worm vm set KEY VALUE      (root) change a setting in /etc/worm/vm.conf
 *     worm vm reset [KEY]        (root) back to the default
 *     worm vm shares             mount the host's shared folders again (root)
 *
 * Detection reads the firmware tables (DMI: the vendor and product a
 * hypervisor reports), the CPU's hypervisor leaf (KVM or QEMU's emulator
 * underneath), the graphics driver and the virtio ports and shares the VM
 * was given. The settings come from built-in defaults, then the file of
 * the hypervisor found in /usr/share/worm/vm (HYPERVISOR.conf), then
 * /etc/worm/vm.conf, so an image carries base settings for every
 * hypervisor and each machine can change them. "auto" means: decided from
 * what was found. The result, detection and settings, is written to
 * /run/worm/vm (KEY=VALUE lines) for the desktop session, the panel, the
 * Settings program and the guest agent.
 */

#define _GNU_SOURCE
#include <cpuid.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>

#include "worm.h"

#define VM_RUN      "/run/worm/vm"
#define VM_CONF     "/etc/worm/vm.conf"
#define VM_PROFILES "/usr/share/worm/vm"
#define MEDIA       "/media"

struct setting {
	const char *key, *def, *values, *help;
	char value[64];
};

static struct setting settings[] = {
	{ "renderer",  "auto", "auto gles software",
	  "how the desktop draws: auto picks OpenGL ES on a real or 3D-capable GPU", "" },
	{ "cursor",    "auto", "auto software hardware",
	  "software draws the pointer into the picture (for viewers that lose it)", "" },
	{ "scale",     "auto", "auto 0.75 0.8 0.9 1 1.25 1.5 1.75 2",
	  "display scale for users who have not chosen one", "" },
	{ "clipboard", "on",   "on off",
	  "the SPICE agent: shared clipboard, pointer and window size", "" },
	{ "agent",     "on",   "on off",
	  "the guest agent: the host sees the IP address, shuts down cleanly, sets the clock", "" },
	{ "shares",    "on",   "on off",
	  "mount the host's shared folders (virtiofs, 9p) under /media", "" },
	{ "tuning",    "on",   "on off",
	  "disk and memory settings for virtual disks (no I/O scheduler, early writeback)", "" },
};
#define NSETTINGS (sizeof(settings) / sizeof(settings[0]))

struct vm {
	char hypervisor[32];    /* qemu, virtualbox, vmware, hyperv, xen, parallels, none */
	char accel[16];         /* kvm, tcg (QEMU) */
	char vendor[128], product[128];
	char gpu[32];           /* DRM driver of the first card */
	int gl;                 /* the virtual GPU has 3D (virgl, vmwgfx) */
	int spice, agent_port, balloon;
	char shares[512];       /* tags, space separated */
	char share_types[512];  /* virtiofs or 9p for each tag */
};

static void read_line(const char *path, char *buf, size_t len)
{
	FILE *f = fopen(path, "re");

	buf[0] = '\0';
	if (!f)
		return;
	if (fgets(buf, len, f))
		buf[strcspn(buf, "\n")] = '\0';
	fclose(f);
}

static void link_name(const char *path, char *buf, size_t len)
{
	char target[PATH_MAX];
	ssize_t n = readlink(path, target, sizeof(target) - 1);
	const char *base;

	buf[0] = '\0';
	if (n <= 0)
		return;
	target[n] = '\0';
	base = strrchr(target, '/');
	snprintf(buf, len, "%s", base ? base + 1 : target);
}

static void cpu_hypervisor(char *sig, size_t len)
{
	unsigned a, b, c, d;
	char s[13];

	sig[0] = '\0';
	if (!__get_cpuid(1, &a, &b, &c, &d) || !(c & (1u << 31)))
		return;
	__cpuid(0x40000000, a, b, c, d);
	memcpy(s, &b, 4);
	memcpy(s + 4, &c, 4);
	memcpy(s + 8, &d, 4);
	s[12] = '\0';
	snprintf(sig, len, "%s", s);
	/* QEMU with Hyper-V enlightenments reports KVM in the next block. */
	if (!strncmp(s, "Microsoft Hv", 12)) {
		__cpuid(0x40000100, a, b, c, d);
		memcpy(s, &b, 4);
		memcpy(s + 4, &c, 4);
		memcpy(s + 8, &d, 4);
		if (!strncmp(s, "KVMKVMKVM", 9))
			snprintf(sig, len, "KVMKVMKVM");
	}
}

static void detect_gpu(struct vm *vm)
{
	DIR *d = opendir("/sys/class/drm");
	struct dirent *e;

	while (d && (e = readdir(d))) {
		char path[PATH_MAX], driver[64];

		if (strncmp(e->d_name, "card", 4) || strchr(e->d_name, '-'))
			continue;
		snprintf(path, sizeof(path), "/sys/class/drm/%s/device/driver", e->d_name);
		link_name(path, driver, sizeof(driver));
		if (!driver[0])
			continue;
		if (!strcmp(driver, "virtio-pci")) {
			/* The GPU is the virtio device below the PCI one; bit 0
			 * of its features is VIRTIO_GPU_F_VIRGL. */
			DIR *v;
			struct dirent *ve;

			snprintf(path, sizeof(path), "/sys/class/drm/%s/device", e->d_name);
			v = opendir(path);
			while (v && (ve = readdir(v))) {
				char p2[PATH_MAX], drv[64], features[256];

				if (strncmp(ve->d_name, "virtio", 6))
					continue;
				snprintf(p2, sizeof(p2), "%s/%s/driver", path, ve->d_name);
				link_name(p2, drv, sizeof(drv));
				if (strcmp(drv, "virtio_gpu"))
					continue;
				snprintf(p2, sizeof(p2), "%s/%s/features", path, ve->d_name);
				read_line(p2, features, sizeof(features));
				vm->gl = features[0] == '1';
				snprintf(driver, sizeof(driver), "virtio_gpu");
			}
			if (v)
				closedir(v);
		} else if (!strcmp(driver, "vmwgfx")) {
			/* 3D on when the render node works; the session falls back
			 * to software otherwise. */
			vm->gl = access("/dev/dri/renderD128", F_OK) == 0;
		}
		/* simpledrm (the firmware's frame buffer) only if nothing else. */
		if (!vm->gpu[0] || !strcmp(vm->gpu, "simpledrm"))
			snprintf(vm->gpu, sizeof(vm->gpu), "%s", driver);
	}
	if (d)
		closedir(d);
}

static void add_share(struct vm *vm, const char *tag, const char *type)
{
	/* Tags become folder names: plain characters only. */
	if (!tag[0] || strlen(tag) > 64 || strspn(tag, "abcdefghijklmnopqrstuvwxyz"
		"ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-") != strlen(tag) || tag[0] == '.')
		return;
	if (strlen(vm->shares) + strlen(tag) + 2 >= sizeof(vm->shares))
		return;
	if (vm->shares[0]) {
		strcat(vm->shares, " ");
		strcat(vm->share_types, " ");
	}
	strcat(vm->shares, tag);
	strcat(vm->share_types, type);
}

static void detect_devices(struct vm *vm)
{
	DIR *d;
	struct dirent *e;
	char path[PATH_MAX], value[128];

	if ((d = opendir("/sys/class/virtio-ports"))) {
		while ((e = readdir(d))) {
			snprintf(path, sizeof(path), "/sys/class/virtio-ports/%s/name", e->d_name);
			read_line(path, value, sizeof(value));
			vm->spice |= !strcmp(value, "com.redhat.spice.0");
			vm->agent_port |= !strcmp(value, "org.qemu.guest_agent.0");
		}
		closedir(d);
	}
	if ((d = opendir("/sys/bus/virtio/drivers/virtio_balloon"))) {
		while ((e = readdir(d)))
			vm->balloon |= !strncmp(e->d_name, "virtio", 6);
		closedir(d);
	}
	if ((d = opendir("/sys/fs/virtiofs"))) {
		while ((e = readdir(d))) {
			if (e->d_name[0] == '.')
				continue;
			snprintf(path, sizeof(path), "/sys/fs/virtiofs/%s/tag", e->d_name);
			read_line(path, value, sizeof(value));
			add_share(vm, value, "virtiofs");
		}
		closedir(d);
	}
	if ((d = opendir("/sys/bus/virtio/drivers/9pnet_virtio"))) {
		while ((e = readdir(d))) {
			if (strncmp(e->d_name, "virtio", 6))
				continue;
			snprintf(path, sizeof(path), "/sys/bus/virtio/drivers/9pnet_virtio/%s/mount_tag",
				 e->d_name);
			read_line(path, value, sizeof(value));
			add_share(vm, value, "9p");
		}
		closedir(d);
	}
	/* VirtualBox and VMware balloons are modules of their own. */
	vm->balloon |= access("/sys/module/vmw_balloon", F_OK) == 0 ||
		       access("/sys/module/hv_balloon", F_OK) == 0;
}

static void detect(struct vm *vm)
{
	char sig[16], lower[128];

	memset(vm, 0, sizeof(*vm));
	read_line("/sys/class/dmi/id/sys_vendor", vm->vendor, sizeof(vm->vendor));
	read_line("/sys/class/dmi/id/product_name", vm->product, sizeof(vm->product));
	cpu_hypervisor(sig, sizeof(sig));
	snprintf(lower, sizeof(lower), "%s %s", vm->vendor, vm->product);
	for (char *c = lower; *c; c++)
		if (*c >= 'A' && *c <= 'Z')
			*c += 'a' - 'A';

	if (strstr(lower, "qemu") || !strcmp(sig, "TCGTCGTCGTCG"))
		strcpy(vm->hypervisor, "qemu");
	else if (strstr(lower, "virtualbox") || strstr(lower, "innotek") || !strcmp(sig, "VBoxVBoxVBox"))
		strcpy(vm->hypervisor, "virtualbox");
	else if (strstr(lower, "vmware") || !strcmp(sig, "VMwareVMware"))
		strcpy(vm->hypervisor, "vmware");
	else if ((strstr(lower, "microsoft") && strstr(lower, "virtual")) ||
		 !strcmp(sig, "Microsoft Hv"))
		strcpy(vm->hypervisor, "hyperv");
	else if (strstr(lower, "xen") || !strcmp(sig, "XenVMMXenVMM"))
		strcpy(vm->hypervisor, "xen");
	else if (strstr(lower, "parallels") || !strcmp(sig, " lrpepyh  vr"))
		strcpy(vm->hypervisor, "parallels");
	else if (!strcmp(sig, "KVMKVMKVM"))
		strcpy(vm->hypervisor, "kvm");      /* another KVM front end */
	else if (sig[0])
		strcpy(vm->hypervisor, "other");
	else
		strcpy(vm->hypervisor, "none");
	if (!strcmp(vm->hypervisor, "qemu") || !strcmp(vm->hypervisor, "kvm"))
		strcpy(vm->accel, !strcmp(sig, "KVMKVMKVM") ? "kvm" : !strcmp(sig, "TCGTCGTCGTCG")
		       ? "tcg" : "");
	detect_gpu(vm);
	detect_devices(vm);
}

/* ---- settings ---- */

static struct setting *setting_get(const char *key)
{
	for (size_t i = 0; i < NSETTINGS; i++)
		if (!strcmp(settings[i].key, key))
			return &settings[i];
	return NULL;
}

static int value_valid(const struct setting *s, const char *value)
{
	size_t n = strlen(value);
	const char *p = s->values;

	if (!n)
		return 0;
	while ((p = strstr(p, value))) {
		if ((p == s->values || p[-1] == ' ') && (p[n] == ' ' || p[n] == '\0'))
			return 1;
		p += n;
	}
	return 0;
}

/* KEY = VALUE lines; # starts a comment. Unknown keys and values are
 * skipped with a warning when loud. */
static void read_settings(const char *path, int loud)
{
	char line[256];
	FILE *f = fopen(path, "re");

	if (!f)
		return;
	while (fgets(line, sizeof(line), f)) {
		char key[64], value[64];
		struct setting *s;

		line[strcspn(line, "#\n")] = '\0';
		if (sscanf(line, " %63[a-z-] = %63s", key, value) != 2)
			continue;
		if (!(s = setting_get(key)) || !value_valid(s, value)) {
			if (loud)
				fprintf(stderr, "worm: %s: ignoring '%s = %s'\n", path, key, value);
			continue;
		}
		snprintf(s->value, sizeof(s->value), "%s", value);
	}
	fclose(f);
}

static void load_settings(const struct vm *vm, int loud)
{
	char path[PATH_MAX];

	for (size_t i = 0; i < NSETTINGS; i++)
		snprintf(settings[i].value, sizeof(settings[i].value), "%s", settings[i].def);
	snprintf(path, sizeof(path), VM_PROFILES "/%s.conf", vm->hypervisor);
	read_settings(path, loud);
	read_settings(VM_CONF, loud);
}

/* What "auto" comes to here. */
static const char *resolved(const struct vm *vm, const struct setting *s)
{
	if (strcmp(s->value, "auto"))
		return s->value;
	if (!strcmp(s->key, "renderer")) {
		if (!strcmp(vm->gpu, "i915") || !strcmp(vm->gpu, "xe") || vm->gl)
			return "gles";
		return "software";
	}
	if (!strcmp(s->key, "cursor")) {
		/* virgl showed the cursor upside down; VMware's and VirtualBox's
		 * cursor planes stay invisible with wlroots. */
		if ((!strcmp(vm->gpu, "virtio_gpu") && vm->gl) || !strcmp(vm->gpu, "vmwgfx") ||
		    !strcmp(vm->gpu, "vboxvideo"))
			return "software";
		return "hardware";
	}
	return s->value;
}

static int in_vm(const struct vm *vm)
{
	return strcmp(vm->hypervisor, "none") != 0;
}

/* ---- applying ---- */

static void write_sys(const char *path, const char *value)
{
	int fd = open(path, O_WRONLY | O_CLOEXEC);

	if (fd < 0)
		return;
	if (write(fd, value, strlen(value)) < 0) {
		/* not every disk offers every scheduler */
	}
	close(fd);
}

/* Virtual disks: the host schedules I/O already, so none here; writeback
 * starts earlier, so a VM with little memory does not stall on a burst. */
static void apply_tuning(void)
{
	DIR *d = opendir("/sys/block");
	struct dirent *e;

	while (d && (e = readdir(d))) {
		char path[PATH_MAX];

		if (strncmp(e->d_name, "vd", 2) && strncmp(e->d_name, "sd", 2) &&
		    strncmp(e->d_name, "nvme", 4) && strncmp(e->d_name, "xvd", 3))
			continue;
		snprintf(path, sizeof(path), "/sys/block/%s/queue/scheduler", e->d_name);
		write_sys(path, "none");
		snprintf(path, sizeof(path), "/sys/block/%s/queue/add_random", e->d_name);
		write_sys(path, "0");
	}
	if (d)
		closedir(d);
	write_sys("/proc/sys/vm/dirty_background_ratio", "5");
	write_sys("/proc/sys/vm/dirty_ratio", "10");
}

static int is_mountpoint(const char *path)
{
	struct stat a, b;
	char parent[PATH_MAX];

	snprintf(parent, sizeof(parent), "%s/..", path);
	return stat(path, &a) == 0 && stat(parent, &b) == 0 && a.st_dev != b.st_dev;
}

/* The host's shared folders, each at /media/TAG. / is read-only, so a
 * small tmpfs on /media holds the mount points. Returns how many. */
static int mount_shares(const struct vm *vm, int loud)
{
	char tags[sizeof(vm->shares)], types[sizeof(vm->share_types)];
	char *tag, *type, *s1, *s2;
	int mounted = 0;

	if (!vm->shares[0])
		return 0;
	mkdir(MEDIA, 0755);
	if (!is_mountpoint(MEDIA) &&
	    mount("media", MEDIA, "tmpfs", MS_NOSUID | MS_NODEV | MS_NOEXEC, "mode=0755,size=1m") < 0) {
		warn("cannot prepare " MEDIA ": %s", strerror(errno));
		return 0;
	}
	snprintf(tags, sizeof(tags), "%s", vm->shares);
	snprintf(types, sizeof(types), "%s", vm->share_types);
	for (tag = strtok_r(tags, " ", &s1), type = strtok_r(types, " ", &s2); tag && type;
	     tag = strtok_r(NULL, " ", &s1), type = strtok_r(NULL, " ", &s2)) {
		char dir[PATH_MAX];
		int r;

		snprintf(dir, sizeof(dir), MEDIA "/%s", tag);
		if (is_mountpoint(dir)) {
			mounted++;
			continue;
		}
		mkdir(dir, 0755);
		if (!strcmp(type, "virtiofs"))
			r = mount(tag, dir, "virtiofs", MS_NOSUID | MS_NODEV, NULL);
		else
			r = mount(tag, dir, "9p", MS_NOSUID | MS_NODEV,
				  "trans=virtio,version=9p2000.L,msize=262144,access=any");
		if (r < 0) {
			if (loud)
				fprintf(stderr, "worm: cannot mount the shared folder %s: %s\n", tag,
					strerror(errno));
			rmdir(dir);
			continue;
		}
		mounted++;
	}
	return mounted;
}

static void write_run(const struct vm *vm)
{
	FILE *f;

	mkdir("/run/worm", 0711);
	if (!(f = fopen(VM_RUN ".new", "we")))
		die("cannot write " VM_RUN ": %s", strerror(errno));
	fprintf(f, "hypervisor=%s\naccel=%s\nvendor=%s\nproduct=%s\ngpu=%s\ngl=%s\n"
		"spice=%s\nagent-port=%s\nballoon=%s\nshares=%s\n",
		vm->hypervisor, vm->accel, vm->vendor, vm->product, vm->gpu,
		vm->gl ? "yes" : "no", vm->spice ? "yes" : "no", vm->agent_port ? "yes" : "no",
		vm->balloon ? "yes" : "no", vm->shares);
	for (size_t i = 0; i < NSETTINGS; i++)
		fprintf(f, "%s=%s\n", settings[i].key, resolved(vm, &settings[i]));
	fchmod(fileno(f), 0644);
	if (fclose(f) != 0 || rename(VM_RUN ".new", VM_RUN) < 0)
		die("cannot write " VM_RUN ": %s", strerror(errno));
}

static int cmd_setup(void)
{
	struct vm vm;

	detect(&vm);
	load_settings(&vm, 1);
	if (in_vm(&vm)) {
		if (!strcmp(setting_get("tuning")->value, "on"))
			apply_tuning();
		if (!strcmp(setting_get("shares")->value, "on"))
			mount_shares(&vm, 1);
		/* The balloons of VMware and Hyper-V are modules. */
		if (!strcmp(vm.hypervisor, "vmware"))
			run((char *[]){ "modprobe", "-q", "vmw_balloon", NULL }, NULL, 0, 1);
		else if (!strcmp(vm.hypervisor, "hyperv"))
			run((char *[]){ "modprobe", "-q", "hv_balloon", NULL }, NULL, 0, 1);
		else if (!strcmp(vm.hypervisor, "virtualbox")) {
			run((char *[]){ "modprobe", "-q", "vboxguest", NULL }, NULL, 0, 1);
			run((char *[]){ "modprobe", "-q", "vboxsf", NULL }, NULL, 0, 1);
		}
	}
	write_run(&vm);
	return 0;
}

/* ---- showing ---- */

static const char *hypervisor_name(const struct vm *vm)
{
	static const char *const names[][2] = {
		{ "qemu", "QEMU" }, { "kvm", "KVM" }, { "virtualbox", "VirtualBox" },
		{ "vmware", "VMware" }, { "hyperv", "Hyper-V" }, { "xen", "Xen" },
		{ "parallels", "Parallels" }, { "other", "an unknown hypervisor" },
	};

	for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
		if (!strcmp(vm->hypervisor, names[i][0]))
			return names[i][1];
	return vm->hypervisor;
}

static const char *gpu_name(const char *driver)
{
	static const char *const names[][2] = {
		{ "virtio_gpu", "virtio-gpu" }, { "qxl", "QXL" }, { "bochs-drm", "standard VGA" },
		{ "bochs", "standard VGA" }, { "cirrus", "Cirrus" }, { "vmwgfx", "VMware SVGA" },
		{ "vboxvideo", "VirtualBox VGA" }, { "hyperv_drm", "Hyper-V video" },
		{ "simpledrm", "firmware frame buffer" }, { "i915", "Intel" }, { "xe", "Intel" },
	};

	for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
		if (!strcmp(driver, names[i][0]))
			return names[i][1];
	return driver[0] ? driver : "none";
}

static int agent_running(void)
{
	char pid[32], path[64];

	read_line("/run/worm/guest-agent.pid", pid, sizeof(pid));
	if (!pid[0] || strspn(pid, "0123456789") != strlen(pid))
		return 0;
	snprintf(path, sizeof(path), "/proc/%s", pid);
	return access(path, F_OK) == 0;
}

static void tip(int *n, const char *text)
{
	if (!(*n)++)
		printf("\nTips for the host:\n");
	printf("  - %s\n", text);
}

static int cmd_show(void)
{
	struct vm vm;
	int tips = 0, tty = isatty(STDOUT_FILENO);
	const char *b = tty ? "\033[1m" : "", *r = tty ? "\033[0m" : "";

	detect(&vm);
	load_settings(&vm, 0);
	if (!in_vm(&vm)) {
		printf("Not a virtual machine (%s %s).\n", vm.vendor, vm.product);
		return 0;
	}
	printf("%sVirtual machine:%s %s%s%s", b, r, hypervisor_name(&vm),
	       !strcmp(vm.accel, "kvm") ? " with KVM" : !strcmp(vm.accel, "tcg") ?
	       " (emulated, no KVM)" : "", vm.product[0] ? "" : "\n");
	if (vm.product[0])
		printf(" (%s)\n", vm.product);
	printf("Graphics:        %s%s, drawn with %s\n", gpu_name(vm.gpu),
	       vm.gl ? " with 3D" : "",
	       !strcmp(resolved(&vm, setting_get("renderer")), "gles") ? "OpenGL ES" : "software");
	if (!strcmp(vm.hypervisor, "qemu") || !strcmp(vm.hypervisor, "kvm")) {
		printf("SPICE agent:     %s\n", !vm.spice ? "no channel" :
		       !strcmp(setting_get("clipboard")->value, "off") ? "off" :
		       "clipboard, pointer and window size");
		printf("Guest agent:     %s\n", !vm.agent_port ? "no channel" :
		       !strcmp(setting_get("agent")->value, "off") ? "off" :
		       agent_running() ? "running" : "not running");
		printf("Memory balloon:  %s\n", vm.balloon ? "yes" : "no");
	}
	if (vm.shares[0]) {
		char tags[sizeof(vm.shares)], *tag, *save;

		printf("Shared folders: ");
		snprintf(tags, sizeof(tags), "%s", vm.shares);
		for (tag = strtok_r(tags, " ", &save); tag; tag = strtok_r(NULL, " ", &save))
			printf(" " MEDIA "/%s", tag);
		putchar('\n');
	}

	printf("\n%sSettings%s (worm vm set KEY VALUE, as root):\n", b, r);
	for (size_t i = 0; i < NSETTINGS; i++) {
		const char *v = resolved(&vm, &settings[i]);

		printf("  %-10s %-9s%s%s%s  %s\n", settings[i].key, settings[i].value,
		       strcmp(v, settings[i].value) ? "(" : "",
		       strcmp(v, settings[i].value) ? v : "",
		       strcmp(v, settings[i].value) ? ")" : "", settings[i].help);
	}

	if (!strcmp(vm.hypervisor, "qemu") || !strcmp(vm.hypervisor, "kvm")) {
		if (!strcmp(vm.accel, "tcg"))
			tip(&tips, "KVM is off: everything is emulated and slow. Use KVM "
			    "(virt-manager: Hypervisor KVM; qemu: -accel kvm).");
		if (strcmp(vm.gpu, "virtio_gpu"))
			tip(&tips, "Video model Virtio follows the window size best "
			    "(virt-manager: Video > Model: Virtio).");
		else if (!vm.gl)
			tip(&tips, "Smoother drawing with 3D: virt-manager Video > 3D acceleration, "
			    "Display Spice > Listen type None, OpenGL on (or qemu -device "
			    "virtio-vga-gl -display gtk,gl=on).");
		if (!vm.spice)
			tip(&tips, "Add a SPICE agent channel for a shared clipboard and window "
			    "resizing (virt-manager: Add Hardware > Channel > com.redhat.spice.0).");
		if (!vm.agent_port)
			tip(&tips, "Add a guest agent channel so the host shows the IP address and "
			    "shuts down cleanly (Add Hardware > Channel > org.qemu.guest_agent.0).");
	} else if (!strcmp(vm.hypervisor, "virtualbox")) {
		tip(&tips, "Graphics controller VMSVGA with 3D acceleration on works best; "
		    "VirtualBox's own clipboard and window resizing need its Guest Additions, "
		    "which WormOS does not ship (set the size with the display scale).");
	} else if (!strcmp(vm.hypervisor, "vmware")) {
		tip(&tips, "VMware is not supported or tested; it starts, without "
		    "open-vm-tools.");
	}
	return 0;
}

/* ---- changing ---- */

static int cmd_set(const char *key, const char *value)
{
	struct setting *s = setting_get(key);
	char line[256], out[4096] = "";
	size_t used = 0;
	int relock, found = 0;
	FILE *f;

	if (!s) {
		fprintf(stderr, "worm: no setting '%s'; worm vm lists them\n", key);
		return 2;
	}
	if (value && !value_valid(s, value)) {
		fprintf(stderr, "worm: %s is one of: %s\n", key, s->values);
		return 2;
	}
	/* Keep the other lines (and comments) of the file. */
	if ((f = fopen(VM_CONF, "re"))) {
		while (fgets(line, sizeof(line), f)) {
			char k[64];

			if (sscanf(line, " %63[a-z-] =", k) == 1 && !strcmp(k, key)) {
				if (found++ || !value)
					continue;
				snprintf(line, sizeof(line), "%s = %s\n", key, value);
			}
			used += snprintf(out + used, sizeof(out) - used, "%s", line);
			if (used >= sizeof(out))
				die("%s is too long", VM_CONF);
		}
		fclose(f);
	} else {
		used = snprintf(out, sizeof(out), "# Virtual machine settings (worm vm). Defaults: "
				VM_PROFILES "/HYPERVISOR.conf\n");
	}
	if (value && !found)
		used += snprintf(out + used, sizeof(out) - used, "%s = %s\n", key, value);

	relock = root_begin_change();
	mkdir("/etc/worm", 0755);
	f = fopen(VM_CONF ".new", "we");
	if (!f || fputs(out, f) == EOF || fclose(f) != 0 || rename(VM_CONF ".new", VM_CONF) < 0) {
		int err = errno;

		root_end_change(relock);
		die("cannot write " VM_CONF ": %s", strerror(err));
	}
	root_end_change(relock);
	cmd_setup();
	if (value)
		printf("%s = %s.", key, value);
	else
		printf("%s is back to its default.", key);
	if (!strcmp(key, "renderer") || !strcmp(key, "cursor") || !strcmp(key, "clipboard") ||
	    !strcmp(key, "scale"))
		printf(" The desktop uses it from the next login.");
	else if (!strcmp(key, "agent"))
		printf(" The guest agent follows within a few seconds.");
	putchar('\n');
	return 0;
}

static int cmd_reset_all(void)
{
	int relock = root_begin_change();

	if (unlink(VM_CONF) < 0 && errno != ENOENT) {
		int err = errno;

		root_end_change(relock);
		die("cannot remove " VM_CONF ": %s", strerror(err));
	}
	root_end_change(relock);
	cmd_setup();
	puts("Every virtual machine setting is back to its default.");
	return 0;
}

int cmd_vm(int argc, char **argv)
{
	struct vm vm;

	if (argc == 0)
		return cmd_show();
	if (geteuid() != 0)
		die("this needs root, try again with su%s", "");
	if (!strcmp(argv[0], "setup") && argc == 1)
		return cmd_setup();
	if (!strcmp(argv[0], "set") && argc == 3)
		return cmd_set(argv[1], argv[2]);
	if (!strcmp(argv[0], "reset") && argc == 2)
		return cmd_set(argv[1], NULL);
	if (!strcmp(argv[0], "reset") && argc == 1)
		return cmd_reset_all();
	if (!strcmp(argv[0], "shares") && argc == 1) {
		int n;

		detect(&vm);
		if (!vm.shares[0]) {
			puts("The host shares no folders with this machine.");
			return 0;
		}
		n = mount_shares(&vm, 1);
		printf("%d shared folder%s under " MEDIA ".\n", n, n == 1 ? "" : "s");
		return 0;
	}
	return 2;
}
