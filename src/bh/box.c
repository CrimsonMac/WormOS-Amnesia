/*
 * Starting a blackhole and serving its sessions.
 *
 * bh clones a child into new user, mount, PID, network, IPC and UTS
 * namespaces and maps root inside to one unprivileged ID outside: the
 * caller's own for a normal user, a dedicated BH_ID_BASE + n for root. The
 * child goes into a cgroup limiting processes, memory and CPU weight
 * (limits.c) and, with --net, gets a network from pasta (net.c).
 *
 * The child becomes the blackhole's init. Its root is a directory of the
 * blackhole's own: the host's /usr and /etc (or a wormhole's, with --from)
 * are bound in read-only, and everything else (/root, /tmp, /var, /home,
 * /run) belongs to the blackhole. Host files are owned by IDs unmapped
 * inside, so they stay read-only even to root in the blackhole; the host's
 * /home, /root, /var and snapshots are not there at all. It adds a fresh
 * /proc, a minimal /dev and the inbox, pivots into the new root, brings up
 * loopback, applies sandbox_apply() and then serves sessions.
 *
 * Sessions: init listens on two sockets. One is a socketpair with the bh
 * that opened the blackhole; the other is BH_DIR/UID/NAME/control, bound
 * before the clone, where a later "bh open NAME" asks for another session
 * (box_join). A request names a program; init starts it in a session of
 * its own on a new pseudo-terminal from the blackhole's devpts, whose
 * master it sends back, or on file descriptors sent along with the
 * request (no terminal). When a program ends, its exit code goes back the
 * same way. When the first session's program ends, init exits and the
 * kernel kills everything left in the blackhole, joined sessions too.
 *
 * Programs never get the host's terminal: bh relays between it and the
 * blackhole's own (relay.c), so no ioctl inside reaches the host terminal
 * and output loses the escape sequences that would set the host's
 * clipboard. Output to a terminal without input from one goes through
 * pipes for the same reason.
 *
 * With --vm the first session's program is a virtual machine monitor
 * (vm.c), and the blackhole's root becomes the guest's root file system.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <net/if.h>
#include <poll.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#include "bh.h"
#include "ns.h"

#define MAX_SESSIONS 64

static pid_t child;
static int limited;

/* init's side of the sessions. */
static struct session {
	pid_t pid;              /* the program; 0 when the slot is free */
	int conn;               /* where its exit code goes */
} sessions[MAX_SESSIONS];

static void kill_child(int sig)
{
	(void)sig;
	if (child > 0)
		kill(child, SIGKILL);
}

void fail(const char *what)
{
	fprintf(stderr, "bh: cannot %s: %s\n", what, strerror(errno));
	_exit(127);
}

void mount_at(const char *root, const char *dst, const char *type,
		     unsigned long flags, const char *data)
{
	char path[PATH_MAX];

	snprintf(path, sizeof(path), "%s%s", root, dst);
	mkdir(path, 0755);
	if (mount(type, path, type, flags, data) < 0) {
		fprintf(stderr, "bh: cannot mount %s: %s\n", dst, strerror(errno));
		_exit(127);
	}
}

/* Device nodes are bound from the host: a user namespace cannot create any. */
void setup_dev(const char *root, int vm)
{
	static const char *const nodes[] = { "null", "zero", "full", "random", "urandom", "tty" };
	static const char *const links[][2] = {
		{ "pts/ptmx", "ptmx" }, { "/proc/self/fd", "fd" }, { "/proc/self/fd/0", "stdin" },
		{ "/proc/self/fd/1", "stdout" }, { "/proc/self/fd/2", "stderr" },
	};
	char src[64], dst[64], path[PATH_MAX];

	mount_at(root, "/dev", "tmpfs", MS_NOSUID | MS_NOEXEC, "mode=0755,size=64k");
	for (size_t i = 0; i < sizeof(nodes) / sizeof(nodes[0]); i++) {
		snprintf(src, sizeof(src), "/dev/%s", nodes[i]);
		if (ns_bind(root, src, src, 0) < 0)
			fail(src);
	}
	if (vm && ns_bind(root, "/dev/kvm", "/dev/kvm", 0) < 0)
		fail("/dev/kvm");
	mount_at(root, "/dev/pts", "devpts", MS_NOSUID | MS_NOEXEC,
		 "newinstance,ptmxmode=0666,mode=0620");
	mount_at(root, "/dev/shm", "tmpfs", MS_NOSUID | MS_NODEV, "mode=1777,size=64m");
	for (size_t i = 0; i < sizeof(links) / sizeof(links[0]); i++) {
		snprintf(dst, sizeof(dst), "/dev/%s", links[i][1]);
		snprintf(path, sizeof(path), "%s%s", root, dst);
		if (symlink(links[i][0], path) < 0)
			fail(dst);
	}
}

/*
 * The blackhole's root is its own and may hold anything a previous session
 * left there: make sure each mount point is a real directory, so no mount
 * follows a symlink planted in it.
 */
static void mount_point(const char *root, const char *name)
{
	char path[PATH_MAX];
	struct stat st;

	snprintf(path, sizeof(path), "%s/%s", root, name);
	if (lstat(path, &st) == 0) {
		if (S_ISDIR(st.st_mode))
			return;
		if (unlink(path) < 0)
			fail(name);
	}
	if (mkdir(path, 0755) < 0)
		fail(name);
}

/*
 * The top-level links (/bin, /lib64...) follow the system the blackhole
 * runs this time: Debian keeps its loader in /usr/lib64, WormOS in
 * /usr/lib. Links the system lacks are left alone.
 */
static void system_links(const char *root, const char *sys)
{
	static const char *const names[] = { "bin", "sbin", "lib", "lib64" };
	char path[PATH_MAX], want[PATH_MAX], have[PATH_MAX];
	struct stat st;

	for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		ssize_t n;

		snprintf(path, sizeof(path), "%s/%s", sys, names[i]);
		if ((n = readlink(path, want, sizeof(want) - 1)) <= 0)
			continue;
		want[n] = '\0';
		/* Only links into usr, as both systems have. */
		if (strncmp(want, "usr/", 4) || strstr(want, ".."))
			continue;
		snprintf(path, sizeof(path), "%s/%s", root, names[i]);
		if ((n = readlink(path, have, sizeof(have) - 1)) > 0) {
			have[n] = '\0';
			if (!strcmp(have, want))
				continue;
		}
		if (lstat(path, &st) == 0 && (S_ISDIR(st.st_mode) || unlink(path) < 0))
			continue;
		if (symlink(want, path) < 0)
			fail(names[i]);
	}
}

/*
 * /etc/resolv.conf points into /run (base-files), which is the blackhole's
 * own: write the name server there. A wormhole's /etc may have a file of
 * its own instead; the blackhole's copy is bound over it.
 */
static void write_resolv(const char *root, int net)
{
	char path[PATH_MAX], etc[PATH_MAX];
	struct stat st;
	int fd;

	snprintf(path, sizeof(path), "%s/run/resolv.conf", root);
	unlink(path);
	fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644);
	if (fd < 0)
		fail("write /run/resolv.conf");
	if (net)
		dprintf(fd, "nameserver %s\n", BH_NET_DNS);
	close(fd);
	snprintf(etc, sizeof(etc), "%s/etc/resolv.conf", root);
	if (lstat(etc, &st) == 0 && S_ISREG(st.st_mode) &&
	    mount(path, etc, NULL, MS_BIND, NULL) < 0)
		fail("mount /etc/resolv.conf");
}

static void loopback_up(void)
{
	struct ifreq ifr = { 0 };
	int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);

	strcpy(ifr.ifr_name, "lo");
	if (fd < 0 || ioctl(fd, SIOCGIFFLAGS, &ifr) < 0)
		fail("find loopback");
	ifr.ifr_flags |= IFF_UP;
	if (ioctl(fd, SIOCSIFFLAGS, &ifr) < 0)
		fail("bring up loopback");
	close(fd);
}

/* In the forked session process: become the program. Never returns. */
static void run_program(const struct box *b, const struct opts *o,
			const struct request *req, char **argv, int slave, int fds[3])
{
	static char *shell[] = { "/bin/sh", NULL };
	sigset_t none;
	char ps1[64];

	sigemptyset(&none);
	sigprocmask(SIG_SETMASK, &none, NULL);
	signal(SIGPIPE, SIG_DFL);
	if (slave >= 0) {
		/* A session of its own, with its own terminal. */
		if (setsid() < 0 || ioctl(slave, TIOCSCTTY, 0) < 0 ||
		    dup2(slave, 0) < 0 || dup2(slave, 1) < 0 || dup2(slave, 2) < 0)
			fail("attach the terminal");
		if (slave > 2)
			close(slave);
	} else {
		if (setsid() < 0 || dup2(fds[0], 0) < 0 || dup2(fds[1], 1) < 0 ||
		    dup2(fds[2], 2) < 0)
			fail("set up the input and output");
		for (int i = 0; i < 3; i++)
			if (fds[i] > 2)
				close(fds[i]);
	}

	snprintf(ps1, sizeof(ps1), "[bh:%.32s] \\w# ", b->name);
	/* Nothing from the host's environment except the terminal type. */
	clearenv();
	setenv("PATH", "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin", 1);
	setenv("HOME", "/root", 1);
	setenv("USER", "root", 1);
	setenv("SHELL", "/bin/sh", 1);
	setenv("TERM", req->term[0] ? req->term : "linux", 1);
	setenv("PS1", ps1, 1);
	setenv("BLACKHOLE", b->name, 1);
	if (o->gui) {
		/* Windows go to the sandboxed display; no GPU, no session bus. */
		mkdir("/run/user", 0700);
		setenv("XDG_RUNTIME_DIR", "/run/user", 1);
		setenv("WAYLAND_DISPLAY", "/run/wayland-0", 1);
		setenv("GDK_BACKEND", "wayland", 1);
		setenv("QT_QPA_PLATFORM", "wayland", 1);
		setenv("MOZ_ENABLE_WAYLAND", "1", 1);
		setenv("GSK_RENDERER", "cairo", 1);
		setenv("LIBGL_ALWAYS_SOFTWARE", "1", 1);
		setenv("GTK_A11Y", "none", 1);
		setenv("NO_AT_BRIDGE", "1", 1);
		setenv("GSETTINGS_BACKEND", "memory", 1);
		setenv("XCURSOR_THEME", "Adwaita", 1);
		setenv("XCURSOR_SIZE", "24", 1);
	}
	if (o->vm) {
		/* The monitor keeps its files in $HOME/.lkvm: the jail's tmpfs. */
		setenv("HOME", "/tmp", 1);
		if (chdir("/tmp") < 0)
			fail("enter /tmp");
		argv = vm_argv(b, o, req, argv);
	} else if (chdir("/root") < 0) {
		fail("enter /root");
	}
	if (sandbox_drop_caps() < 0)
		_exit(127);
	if (!argv)
		argv = shell;
	execvp(argv[0], argv);
	fprintf(stderr, "bh: cannot run %s: %s\n", argv[0], strerror(errno));
	_exit(127);
}

static void send_error(int conn, const char *fmt, const char *arg)
{
	char msg[256];

	msg[0] = SESSION_ERROR;
	snprintf(msg + 1, sizeof(msg) - 1, fmt, arg);
	session_send(conn, msg, strlen(msg), NULL, 0);
}

/*
 * A request arrived on conn: start its program. owner marks the session
 * that opened the blackhole. Returns 0 when a session started, -1 when
 * conn should be closed.
 */
static int start_session(const struct box *b, const struct opts *o, int conn, int owner)
{
	static char buf[SESSION_MAX];
	struct request *req = (struct request *)buf;
	char *argv[256], reply = SESSION_MASTER;
	int fds[3] = { -1, -1, -1 }, master = -1, slave = -1, slot = -1;
	ssize_t len;
	pid_t pid;

	len = session_recv(conn, buf, sizeof(buf) - 1, fds, 3);
	if (len < (ssize_t)sizeof(*req) || req->magic != SESSION_MAGIC || req->argc > 255 ||
	    (!req->pty && (fds[0] < 0 || fds[1] < 0 || fds[2] < 0)))
		goto refuse;
	buf[len] = '\0';
	req->term[sizeof(req->term) - 1] = '\0';
	/* The arguments: argc strings after the header, all inside the message. */
	{
		char *p = req->args, *end = buf + len;

		for (uint32_t i = 0; i < req->argc; i++) {
			char *nul = memchr(p, '\0', end - p);

			if (!nul || p >= end)
				goto refuse;
			argv[i] = p;
			p = nul + 1;
		}
		argv[req->argc] = NULL;
	}
	if (o->vm && !owner) {
		send_error(conn, "'%s' is a virtual machine: it has one console only", b->name);
		goto refuse;
	}
	for (int i = 0; i < MAX_SESSIONS; i++)
		if (!sessions[i].pid) {
			slot = i;
			break;
		}
	if (slot < 0) {
		send_error(conn, "'%s' has too many sessions", b->name);
		goto refuse;
	}

	if (req->pty) {
		if ((master = posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC)) < 0 ||
		    unlockpt(master) < 0 ||
		    (slave = ioctl(master, TIOCGPTPEER, O_RDWR | O_NOCTTY | O_CLOEXEC)) < 0) {
			send_error(conn, "cannot open a terminal in '%s'", b->name);
			goto refuse;
		}
		tcsetattr(slave, TCSANOW, &req->termios);
		ioctl(slave, TIOCSWINSZ, &req->size);
	}
	if ((pid = fork()) < 0) {
		send_error(conn, "cannot start a program in '%s'", b->name);
		goto refuse;
	}
	if (pid == 0) {
		if (master >= 0)
			close(master);
		run_program(b, o, req, req->argc ? argv : NULL, slave, fds);
	}
	sessions[slot].pid = pid;
	sessions[slot].conn = conn;
	if (master >= 0) {
		session_send(conn, &reply, 1, &master, 1);
		close(master);
		close(slave);
	}
	for (int i = 0; i < 3; i++)
		if (fds[i] >= 0)
			close(fds[i]);
	return 0;

refuse:
	for (int i = 0; i < 3; i++)
		if (fds[i] >= 0)
			close(fds[i]);
	if (master >= 0)
		close(master);
	if (slave >= 0)
		close(slave);
	return -1;
}

/* A session's connection is gone: its programs go too. */
static void end_session(struct session *s)
{
	if (s->pid > 0) {
		kill(-s->pid, SIGKILL);
		kill(s->pid, SIGKILL);
	}
	if (s->conn >= 0)
		close(s->conn);
	s->conn = -1;
}

/* As init: serve sessions, reap everything, leave with the first session. */
static void __attribute__((noreturn))
serve(const struct box *b, const struct opts *o, int owner, int listener)
{
	struct pollfd p[2 + MAX_SESSIONS];
	sigset_t chld;
	int sfd;

	sigemptyset(&chld);
	sigaddset(&chld, SIGCHLD);
	sigprocmask(SIG_BLOCK, &chld, NULL);
	if ((sfd = signalfd(-1, &chld, SFD_CLOEXEC | SFD_NONBLOCK)) < 0)
		fail("watch the programs");
	signal(SIGPIPE, SIG_IGN);
	for (int i = 0; i < MAX_SESSIONS; i++)
		sessions[i].conn = -1;

	/* The opening bh's request comes first, on the socketpair. */
	if (start_session(b, o, owner, 1) < 0)
		_exit(127);

	for (;;) {
		int n = 2, idx[MAX_SESSIONS];
		struct signalfd_siginfo si;
		int status;
		pid_t done;

		p[0] = (struct pollfd){ .fd = sfd, .events = POLLIN };
		p[1] = (struct pollfd){ .fd = listener, .events = POLLIN };
		for (int i = 0; i < MAX_SESSIONS; i++)
			if (sessions[i].conn >= 0) {
				idx[n - 2] = i;
				p[n++] = (struct pollfd){ .fd = sessions[i].conn, .events = POLLIN };
			}
		if (poll(p, n, -1) < 0 && errno != EINTR)
			_exit(127);

		while (read(sfd, &si, sizeof(si)) > 0)
			;
		while ((done = waitpid(-1, &status, WNOHANG)) > 0) {
			for (int i = 0; i < MAX_SESSIONS; i++) {
				char msg[1 + sizeof(int)];
				int code;

				if (sessions[i].pid != done)
					continue;
				code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
				if (o->vm && i == 0 && code == 0)
					code = vm_status();
				msg[0] = SESSION_EXIT;
				memcpy(msg + 1, &code, sizeof(int));
				if (sessions[i].conn >= 0)
					session_send(sessions[i].conn, msg, sizeof(msg), NULL, 0);
				if (i == 0)
					_exit(code);
				/* Its terminal's other users get a hangup from the kernel. */
				if (sessions[i].conn >= 0)
					close(sessions[i].conn);
				sessions[i].pid = 0;
				sessions[i].conn = -1;
			}
		}
		/* A message or a hangup on a session's connection ends it. */
		for (int k = 2; k < n; k++)
			if (p[k].revents & (POLLIN | POLLHUP | POLLERR))
				end_session(&sessions[idx[k - 2]]);
		if (listener >= 0 && (p[1].revents & POLLIN)) {
			int conn = accept4(listener, NULL, NULL, SOCK_CLOEXEC);

			if (conn >= 0 && start_session(b, o, conn, 0) < 0)
				close(conn);
		}
	}
}

static void __attribute__((noreturn))
box_init(const struct box *b, const struct opts *o, int owner, int listener)
{
	static const char *const points[] = { "usr", "etc", "proc", "dev", "inbox", "run" };
	char root[300], inbox[300], usr[300], etc[300];
	struct rlimit nproc;
	int null;

	/* Root inside, which is b->id outside. */
	if (setresgid(0, 0, 0) < 0 || setresuid(0, 0, 0) < 0)
		fail("become root in the blackhole");
	/* Changing credentials cleared it. */
	prctl(PR_SET_PDEATHSIG, SIGKILL);
	if (mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) < 0)
		fail("make mounts private");

	snprintf(root, sizeof(root), "%s/rootfs", b->dir);
	snprintf(inbox, sizeof(inbox), "%s/inbox", b->dir);
	/* pivot_root needs the new root to be a mount point. */
	if (mount(root, root, NULL, MS_BIND | MS_REC, NULL) < 0)
		fail("mount the blackhole's root");
	for (size_t i = 0; i < sizeof(points) / sizeof(points[0]); i++)
		mount_point(root, points[i]);
	/* No submounts below these, so they can be bound alone. */
	if (o->from[0]) {
		snprintf(usr, sizeof(usr), "%s/%s/usr", WORMHOLES, o->from);
		snprintf(etc, sizeof(etc), "%s/%s/etc", WORMHOLES, o->from);
	} else {
		strcpy(usr, "/usr");
		strcpy(etc, "/etc");
	}
	if (o->from[0]) {
		char sys[300];

		snprintf(sys, sizeof(sys), "%s/%s", WORMHOLES, o->from);
		system_links(root, sys);
	} else {
		system_links(root, "/");
	}
	if (ns_bind(root, usr, "/usr", MS_RDONLY) < 0 || ns_bind(root, etc, "/etc", MS_RDONLY) < 0)
		fail("mount the system");
	write_resolv(root, o->net);
	if (o->gui) {
		char display[300];

		/* The sandboxed display bh-wayland made (bh open --gui). */
		snprintf(display, sizeof(display), "%s/wayland-0", b->dir);
		if (ns_bind(root, display, "/run/wayland-0", 0) < 0)
			fail("mount the display");
	}
	if (ns_bind(root, inbox, "/inbox", 0) < 0)
		fail("mount the inbox");
	if (o->vm) {
		/* The guest's init, static, so it runs with any /usr (--from). */
		if (ns_bind(root, BH_LIBDIR "/vm-init", "/run/bh-init", MS_RDONLY) < 0)
			fail("mount the guest's init");
		/* The monitor gets a root of its own; this one goes to the guest. */
		vm_jail(root, o->net);
	} else {
		mount_at(root, "/proc", "proc", MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL);
		setup_dev(root, 0);
		if (ns_pivot(root) < 0)
			fail("enter the blackhole");
	}

	/* The parent has moved us into our cgroup by now. */
	if (unshare(CLONE_NEWCGROUP) < 0)
		fail("create a cgroup namespace");
	if (sethostname(b->name, strlen(b->name)) < 0)
		fail("set the host name");
	loopback_up();
	if (o->net && net_routes(o->lan) < 0)
		_exit(127);
	/* Nothing inside may ptrace init or take its descriptors. */
	prctl(PR_SET_DUMPABLE, 0);
	/* Fork bombs stop here, also where the cgroup could not be made. */
	if (getrlimit(RLIMIT_NPROC, &nproc) < 0)
		fail("limit the processes");
	if (nproc.rlim_max == RLIM_INFINITY || nproc.rlim_max > o->pids)
		nproc.rlim_max = o->pids;
	nproc.rlim_cur = nproc.rlim_max;
	if (setrlimit(RLIMIT_NPROC, &nproc) < 0)
		fail("limit the processes");
	if (sandbox_apply() < 0)
		_exit(127);
	/* From here on, init holds nothing of the host but its two sockets. */
	if ((null = open("/dev/null", O_RDWR | O_CLOEXEC)) < 0 ||
	    dup2(null, 0) < 0 || dup2(null, 1) < 0 || dup2(null, 2) < 0)
		fail("let go of the terminal");
	if (dup2(owner, 3) < 0 || (listener >= 0 && dup2(listener, 4) < 0))
		_exit(127);
	/* dup2 clears close-on-exec: programs must not get these. */
	fcntl(3, F_SETFD, FD_CLOEXEC);
	if (listener >= 0)
		fcntl(4, F_SETFD, FD_CLOEXEC);
	close_range(listener >= 0 ? 5 : 4, ~0U, 0);
	serve(b, o, 3, listener >= 0 ? 4 : -1);
}

/* The socket later sessions connect to, readable by the owner only. */
static int control_socket(const struct box *b)
{
	struct sockaddr_un addr = { .sun_family = AF_UNIX };
	int fd;

	if (snprintf(addr.sun_path, sizeof(addr.sun_path), "%s/control", b->dir) >=
	    (int)sizeof(addr.sun_path))
		return -1;
	unlink(addr.sun_path);
	if ((fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0)) < 0)
		return -1;
	if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
	    chmod(addr.sun_path, 0600) < 0 || listen(fd, 8) < 0) {
		close(fd);
		return -1;
	}
	return fd;
}

int box_run(const struct box *b, const struct opts *o, char *const argv[])
{
	struct sigaction kill_sa = { .sa_handler = kill_child };
	int go[2], pair[2], listener, status, code;
	char c = 'x', path[PATH_MAX];

	/* Root's groups must not follow it in. */
	if (getuid() == 0 && setgroups(0, NULL) < 0)
		die("cannot drop supplementary groups: %s", strerror(errno));
	/*
	 * Root's hard process limit depends on the memory (3809 with 1 GiB);
	 * init can only lower it, so root raises it first. Users keep theirs.
	 */
	if (getuid() == 0) {
		struct rlimit nproc = { o->pids, o->pids };

		setrlimit(RLIMIT_NPROC, &nproc);
	}
	if (o->vm && vm_check() < 0)
		exit(1);
	if (o->net && net_collect_host() < 0)
		exit(1);
	if (pipe2(go, O_CLOEXEC) < 0 ||
	    socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, pair) < 0)
		die("cannot create a pipe: %s", strerror(errno));
	if ((listener = control_socket(b)) < 0)
		warn("cannot create %s/control: %s; no other sessions", b->dir, strerror(errno));
	fflush(NULL);
	child = syscall(SYS_clone, CLONE_NEWUSER | CLONE_NEWNS | CLONE_NEWPID | CLONE_NEWNET |
			CLONE_NEWIPC | CLONE_NEWUTS | SIGCHLD, NULL, NULL, NULL, NULL);
	if (child < 0)
		die("cannot create the namespaces: %s", strerror(errno));
	if (child == 0) {
		close(go[1]);
		close(pair[0]);
		prctl(PR_SET_PDEATHSIG, SIGKILL);
		/* EOF instead of the go byte: the parent died or gave up. */
		if (read(go[0], &c, 1) != 1)
			_exit(127);
		close(go[0]);
		box_init(b, o, pair[1], listener);
	}

	close(go[0]);
	close(pair[1]);
	if (listener >= 0)
		close(listener);
	if (ns_map_ids(child, 0, b->id, 0, b->id) < 0) {
		warn("cannot map IDs into the blackhole: %s", strerror(errno));
		kill(child, SIGKILL);
	}
	limited = limits_apply(b, o, child) == 0;
	if (o->net && net_start(child) < 0)
		kill(child, SIGKILL);
	if (write(go[1], &c, 1) != 1)
		kill(child, SIGKILL);
	close(go[1]);

	/* The terminal's interrupts are for the program inside, not for bh. */
	signal(SIGINT, SIG_IGN);
	signal(SIGQUIT, SIG_IGN);
	sigaction(SIGTERM, &kill_sa, NULL);
	sigaction(SIGHUP, &kill_sa, NULL);
	code = session_run(pair[0], argv, b->name);
	close(pair[0]);
	while (waitpid(child, &status, 0) < 0)
		if (errno != EINTR)
			die("cannot wait for the blackhole: %s", strerror(errno));
	child = 0;
	snprintf(path, sizeof(path), "%s/control", b->dir);
	unlink(path);
	if (limited)
		limits_remove(b);
	if (code < 0)
		code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
	return code;
}

static int join_conn = -1;

static void leave(int sig)
{
	(void)sig;
	if (join_conn >= 0)
		shutdown(join_conn, SHUT_RDWR);
}

int box_join(const struct box *b, char *const argv[])
{
	struct sockaddr_un addr = { .sun_family = AF_UNIX };
	struct sigaction sa = { .sa_handler = leave };
	int code;

	snprintf(addr.sun_path, sizeof(addr.sun_path), "%.96s/control", b->dir);
	if ((join_conn = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0)) < 0 ||
	    connect(join_conn, (struct sockaddr *)&addr, sizeof(addr)) < 0)
		die("cannot reach the running blackhole '%s': %s", b->name, strerror(errno));
	signal(SIGINT, SIG_IGN);
	signal(SIGQUIT, SIG_IGN);
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGHUP, &sa, NULL);
	code = session_run(join_conn, argv, b->name);
	close(join_conn);
	if (code < 0) {
		fprintf(stderr, "bh: blackhole %s was closed\n", b->name);
		code = 1;
	}
	return code;
}
