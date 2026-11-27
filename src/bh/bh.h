#ifndef BH_H
#define BH_H

#include <limits.h>
#include <stdint.h>
#include <sys/types.h>
#include <termios.h>
#include <sys/ioctl.h>

#define BH_DIR      "/var/lib/blackholes"
#define BH_CGROUP   "/sys/fs/cgroup/blackholes"
#define BH_LIBDIR   "/usr/lib/bh"
#define WORMHOLES   "/var/lib/wormholes"
/* Outside IDs for root inside blackholes opened by root: BH_ID_BASE + n. */
#define BH_ID_BASE  0x40000000u
#define BH_ID_COUNT 65536u
#define NAME_MAX_BH 32

/* Default limits (see limits.c). */
#define BH_PIDS_DEFAULT  4096u
#define BH_PIDS_MAX      65536u
#define BH_CPU_WEIGHT    "50"

/* The network a blackhole gets with --net (see net.c). */
#define BH_NET_ADDR  "100.96.0.2"
#define BH_NET_GW    "100.96.0.1"
#define BH_NET_DNS   "100.96.0.3"
#define BH_NET_PLEN  "29"

struct box {
	char name[NAME_MAX_BH + 1];
	char dir[256];          /* BH_DIR/UID/NAME */
	uid_t id;               /* the outside ID of root inside the box */
};

struct opts {
	int net;                /* network through pasta */
	int lan;                /* ... including the local network */
	int vm;                 /* a virtual machine with its own kernel */
	int gui;                /* windows on the desktop, sandboxed (bh-wayland) */
	char from[NAME_MAX_BH + 1]; /* a wormhole's /usr and /etc instead of the host's */
	unsigned long long memory;  /* bytes; 0 for the default */
	unsigned pids;          /* 0 for the default */
	unsigned cpus;          /* --vm: virtual CPUs */
	unsigned vm_mb;         /* --vm: the guest's memory in MiB */
};

void die(const char *fmt, ...) __attribute__((noreturn, format(printf, 1, 2)));
void warn(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int valid_name(const char *s);

/* Start the box and run argv (a shell when NULL); returns its exit code. */
int box_run(const struct box *b, const struct opts *o, char *const argv[]);

/* Run argv in the running box b, as another session; returns its exit code. */
int box_join(const struct box *b, char *const argv[]);

/*
 * Sessions (session.c). bh asks the box's init for a program over a
 * SOCK_SEQPACKET connection: a request, then init answers with the master
 * of the program's terminal (SESSION_MASTER), an error text
 * (SESSION_ERROR) and, when the program ends, its exit code (SESSION_EXIT).
 */
#define SESSION_MAGIC  0x31534842u   /* "BHS1" */
#define SESSION_MAX    65536
enum { SESSION_MASTER = 'm', SESSION_ERROR = 'e', SESSION_EXIT = 'x' };

struct request {
	uint32_t magic;
	uint32_t pty;           /* on a terminal of its own, else on the fds sent along */
	uint32_t owner;         /* the session that opened the box (set by init) */
	uint32_t argc;          /* 0: a shell */
	struct termios termios;
	struct winsize size;
	char term[64];
	char args[];            /* argc strings, each ending in NUL */
};

/* bh's side: send the request, relay, return the program's exit code. */
int session_run(int conn, char *const argv[], const char *name);
/* Both sides: one message with at most one file descriptor. */
ssize_t session_recv(int sock, void *buf, size_t len, int *fds, int nfds);
int session_send(int sock, const void *buf, size_t len, const int *fds, int nfds);

/* bh's side of the program's terminal or output (relay.c). */
void relay_terminal(int master);
void relay_output(int out, int err);

/* No new privileges, Landlock confined to /, then the seccomp filter. */
int sandbox_apply(void);

/* In each program before exec: keep only the harmless capabilities. */
int sandbox_drop_caps(void);

/*
 * Limits (limits.c): a cgroup BH_CGROUP/UID-NAME with pids.max, memory.max
 * and a lower CPU weight. Root creates it itself, users through the setuid
 * helper BH_LIBDIR/limits.
 */
unsigned long long memory_total(void);
int limits_apply(const struct box *b, const struct opts *o, pid_t child);
void limits_remove(const struct box *b);
int cgroup_create(uid_t uid, const char *name, pid_t pid, unsigned long long memory,
		  unsigned pids);
int cgroup_remove(uid_t uid, const char *name);

/* Network (net.c): pasta for the box's network namespace, from outside. */
int net_collect_host(void);
int net_start(pid_t child);
/* Inside, before the sandbox: routes that keep the local network out. */
int net_routes(int lan);

/* Shared by box.c and vm.c (in init). */
void fail(const char *what) __attribute__((noreturn));
void mount_at(const char *root, const char *dst, const char *type,
	      unsigned long flags, const char *data);
void setup_dev(const char *root, int vm);

/* Virtual machines (vm.c). */
int vm_check(void);
/* In init: the monitor's own root, with the blackhole's at /share. */
void vm_jail(const char *root, int net);
char **vm_argv(const struct box *b, const struct opts *o, const struct request *req,
	       char **argv);
int vm_status(void);

#endif
