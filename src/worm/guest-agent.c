/*
 * guest-agent - a small QEMU guest agent for WormOS.
 *
 * Installed as /usr/lib/worm/guest-agent and started at boot (service
 * 45-guest-agent). It answers the host (libvirt, virt-manager, virsh) on
 * the virtio port org.qemu.guest_agent.0 in the guest agent's protocol:
 * one JSON object per command, one per answer.
 *
 * Only commands that report on the machine or that a VM's host can do
 * anyway are offered: the IP addresses (virt-manager shows them), the
 * system's name and version, the time and time zone, setting the clock
 * (after the host slept), shutting down or restarting cleanly, and the
 * users logged in. Commands that run programs or read and write files in
 * the guest (guest-exec, guest-file-*) are not: the host could use them to
 * reach into a system whose point is to stay closed.
 *
 * When the VM has no such port, or "agent = off" (worm vm), it waits and
 * looks again now and then, so the service need not be restarted.
 */

#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <limits.h>
#include <net/if.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define PORT_NAME "org.qemu.guest_agent.0"
#define PID_FILE  "/run/worm/guest-agent.pid"
#define VM_RUN    "/run/worm/vm"
#define MAX_CMD   65536

static int port = -1;

/* ---- output ---- */

struct out {
	char buf[32768];
	size_t len;
	int overflow;
};

static void out_add(struct out *o, const char *fmt, ...)
{
	va_list ap;
	int n;

	if (o->overflow)
		return;
	va_start(ap, fmt);
	n = vsnprintf(o->buf + o->len, sizeof(o->buf) - o->len, fmt, ap);
	va_end(ap);
	if (n < 0 || (size_t)n >= sizeof(o->buf) - o->len)
		o->overflow = 1;
	else
		o->len += n;
}

/* A JSON string, escaped. */
static void out_str(struct out *o, const char *s)
{
	out_add(o, "\"");
	for (const unsigned char *c = (const unsigned char *)s; *c; c++) {
		if (*c == '"' || *c == '\\')
			out_add(o, "\\%c", *c);
		else if (*c < 0x20)
			out_add(o, "\\u%04x", *c);
		else
			out_add(o, "%c", *c);
	}
	out_add(o, "\"");
}

static void send_all(const char *data, size_t len)
{
	while (len > 0) {
		ssize_t n = write(port, data, len);

		if (n < 0 && errno == EINTR)
			continue;
		if (n < 0 && errno == EAGAIN) {
			struct pollfd p = { port, POLLOUT, 0 };

			poll(&p, 1, 1000);
			continue;
		}
		if (n <= 0)
			return;
		data += n;
		len -= n;
	}
}

/* Send { "return": VALUE } (or the error) with the request's id. */
static void reply(struct out *value, const char *id)
{
	struct out o = { .len = 0 };

	if (value->overflow) {
		out_add(&o, "{\"error\": {\"class\": \"GenericError\", \"desc\": \"answer too long\"}");
	} else {
		out_add(&o, "{\"return\": %.*s", (int)value->len, value->buf);
	}
	if (id && *id)
		out_add(&o, ", \"id\": %s", id);
	out_add(&o, "}\n");
	send_all(o.buf, o.len);
}

static void error(const char *class, const char *desc, const char *id)
{
	struct out o = { .len = 0 };

	out_add(&o, "{\"error\": {\"class\": ");
	out_str(&o, class);
	out_add(&o, ", \"desc\": ");
	out_str(&o, desc);
	out_add(&o, "}");
	if (id && *id)
		out_add(&o, ", \"id\": %s", id);
	out_add(&o, "}\n");
	send_all(o.buf, o.len);
}

/* ---- reading JSON (just enough) ---- */

static const char *ws(const char *p)
{
	while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
		p++;
	return p;
}

/* A string at p into out (escapes other than \" and \\ are kept as is). */
static const char *json_string(const char *p, char *out, size_t len)
{
	size_t n = 0;

	if (*p != '"')
		return NULL;
	for (p++; *p && *p != '"'; p++) {
		if (*p == '\\' && p[1])
			p++;
		if (n + 1 < len)
			out[n++] = *p;
	}
	if (len)
		out[n] = '\0';
	return *p == '"' ? p + 1 : NULL;
}

/* The end of the value at p, or NULL. */
static const char *json_skip(const char *p, int depth)
{
	char scratch[2];

	p = ws(p);
	if (depth > 32)
		return NULL;
	if (*p == '"')
		return json_string(p, scratch, sizeof(scratch));
	if (*p == '{' || *p == '[') {
		char close = *p == '{' ? '}' : ']';

		p = ws(p + 1);
		if (*p == close)
			return p + 1;
		for (;;) {
			if (close == '}') {
				if (!(p = json_string(ws(p), scratch, sizeof(scratch))))
					return NULL;
				p = ws(p);
				if (*p++ != ':')
					return NULL;
			}
			if (!(p = json_skip(p, depth + 1)))
				return NULL;
			p = ws(p);
			if (*p == close)
				return p + 1;
			if (*p++ != ',')
				return NULL;
		}
	}
	while (*p && strchr("0123456789+-.eEtruefalsn", *p))
		p++;
	return p;
}

struct request {
	char execute[64];
	char id[128];           /* raw JSON, echoed back */
	long long arg_id, arg_time;
	int has_arg_id, has_arg_time;
	char mode[32];
};

static int parse_arguments(const char *p, struct request *r)
{
	char key[64];

	p = ws(p);
	if (*p++ != '{')
		return -1;
	p = ws(p);
	if (*p == '}')
		return 0;
	for (;;) {
		const char *value, *end;

		if (!(p = json_string(ws(p), key, sizeof(key))))
			return -1;
		p = ws(p);
		if (*p++ != ':')
			return -1;
		value = ws(p);
		if (!(end = json_skip(value, 1)))
			return -1;
		if (!strcmp(key, "id")) {
			r->arg_id = strtoll(value, NULL, 10);
			r->has_arg_id = 1;
		} else if (!strcmp(key, "time")) {
			r->arg_time = strtoll(value, NULL, 10);
			r->has_arg_time = 1;
		} else if (!strcmp(key, "mode")) {
			json_string(value, r->mode, sizeof(r->mode));
		}
		p = ws(end);
		if (*p == '}')
			return 0;
		if (*p++ != ',')
			return -1;
	}
}

static int parse(const char *p, struct request *r)
{
	char key[64];

	memset(r, 0, sizeof(*r));
	p = ws(p);
	if (*p++ != '{')
		return -1;
	for (;;) {
		const char *value, *end;

		if (!(p = json_string(ws(p), key, sizeof(key))))
			return -1;
		p = ws(p);
		if (*p++ != ':')
			return -1;
		value = ws(p);
		if (!(end = json_skip(value, 0)))
			return -1;
		if (!strcmp(key, "execute")) {
			json_string(value, r->execute, sizeof(r->execute));
		} else if (!strcmp(key, "arguments")) {
			if (parse_arguments(value, r) < 0)
				return -1;
		} else if (!strcmp(key, "id") && (size_t)(end - value) < sizeof(r->id)) {
			memcpy(r->id, value, end - value);
			r->id[end - value] = '\0';
		}
		p = ws(end);
		if (*p == '}')
			return r->execute[0] ? 0 : -1;
		if (*p++ != ',')
			return -1;
	}
}

/* ---- commands ---- */

static const char *const commands[] = {
	"guest-sync", "guest-sync-delimited", "guest-ping", "guest-info", "guest-get-osinfo",
	"guest-get-hostname", "guest-get-timezone", "guest-get-time", "guest-set-time",
	"guest-shutdown", "guest-network-get-interfaces", "guest-get-users",
	"guest-fsfreeze-status", "guest-get-vcpus",
};
#define NCOMMANDS (sizeof(commands) / sizeof(commands[0]))

static void os_release(const char *key, char *out, size_t len)
{
	char line[256];
	size_t klen = strlen(key);
	FILE *f = fopen("/etc/os-release", "re");

	out[0] = '\0';
	while (f && fgets(line, sizeof(line), f)) {
		char *v;

		if (strncmp(line, key, klen) || line[klen] != '=')
			continue;
		v = line + klen + 1;
		v[strcspn(v, "\n")] = '\0';
		if (*v == '"') {
			v++;
			v[strcspn(v, "\"")] = '\0';
		}
		snprintf(out, len, "%s", v);
	}
	if (f)
		fclose(f);
}

static void cmd_info(struct out *o)
{
	char version[64];

	os_release("VERSION_ID", version, sizeof(version));
	out_add(o, "{\"version\": ");
	out_str(o, version);
	out_add(o, ", \"supported_commands\": [");
	for (size_t i = 0; i < NCOMMANDS; i++) {
		out_add(o, "%s{\"name\": \"%s\", \"enabled\": true, \"success-response\": %s}",
			i ? ", " : "", commands[i], strcmp(commands[i], "guest-shutdown") ?
			"true" : "false");
	}
	out_add(o, "]}");
}

static void cmd_osinfo(struct out *o)
{
	struct utsname u;
	char id[64], name[64], pretty[128], version[64], version_id[64], variant[64], vid[64];

	uname(&u);
	os_release("ID", id, sizeof(id));
	os_release("NAME", name, sizeof(name));
	os_release("PRETTY_NAME", pretty, sizeof(pretty));
	os_release("VERSION", version, sizeof(version));
	os_release("VERSION_ID", version_id, sizeof(version_id));
	os_release("VARIANT", variant, sizeof(variant));
	os_release("VARIANT_ID", vid, sizeof(vid));
	out_add(o, "{\"kernel-release\": ");
	out_str(o, u.release);
	out_add(o, ", \"kernel-version\": ");
	out_str(o, u.version);
	out_add(o, ", \"machine\": ");
	out_str(o, u.machine);
	out_add(o, ", \"id\": ");
	out_str(o, id);
	out_add(o, ", \"name\": ");
	out_str(o, name);
	out_add(o, ", \"pretty-name\": ");
	out_str(o, pretty);
	out_add(o, ", \"version\": ");
	out_str(o, version);
	out_add(o, ", \"version-id\": ");
	out_str(o, version_id);
	if (variant[0]) {
		out_add(o, ", \"variant\": ");
		out_str(o, variant);
		out_add(o, ", \"variant-id\": ");
		out_str(o, vid);
	}
	out_add(o, "}");
}

static void cmd_interfaces(struct out *o)
{
	struct ifaddrs *list, *a;
	struct if_nameindex *names = if_nameindex();
	int first = 1;

	if (getifaddrs(&list) < 0)
		list = NULL;
	out_add(o, "[");
	for (struct if_nameindex *n = names; n && n->if_index; n++) {
		char path[128], mac[64] = "";
		FILE *f;
		int firstip = 1;

		snprintf(path, sizeof(path), "/sys/class/net/%s/address", n->if_name);
		if ((f = fopen(path, "re"))) {
			if (fgets(mac, sizeof(mac), f))
				mac[strcspn(mac, "\n")] = '\0';
			fclose(f);
		}
		out_add(o, "%s{\"name\": ", first ? "" : ", ");
		out_str(o, n->if_name);
		if (mac[0]) {
			out_add(o, ", \"hardware-address\": ");
			out_str(o, mac);
		}
		out_add(o, ", \"ip-addresses\": [");
		for (a = list; a; a = a->ifa_next) {
			char ip[INET6_ADDRSTRLEN];
			int prefix = 0;
			const unsigned char *mask;
			size_t mlen;

			if (!a->ifa_addr || strcmp(a->ifa_name, n->if_name) ||
			    (a->ifa_addr->sa_family != AF_INET && a->ifa_addr->sa_family != AF_INET6))
				continue;
			if (a->ifa_addr->sa_family == AF_INET) {
				inet_ntop(AF_INET, &((struct sockaddr_in *)a->ifa_addr)->sin_addr, ip,
					  sizeof(ip));
				mask = a->ifa_netmask ? (const unsigned char *)
					&((struct sockaddr_in *)a->ifa_netmask)->sin_addr : NULL;
				mlen = 4;
			} else {
				inet_ntop(AF_INET6, &((struct sockaddr_in6 *)a->ifa_addr)->sin6_addr, ip,
					  sizeof(ip));
				mask = a->ifa_netmask ? (const unsigned char *)
					&((struct sockaddr_in6 *)a->ifa_netmask)->sin6_addr : NULL;
				mlen = 16;
			}
			for (size_t i = 0; mask && i < mlen; i++)
				prefix += __builtin_popcount(mask[i]);
			out_add(o, "%s{\"ip-address-type\": \"%s\", \"ip-address\": ",
				firstip ? "" : ", ", a->ifa_addr->sa_family == AF_INET ? "ipv4" : "ipv6");
			out_str(o, ip);
			out_add(o, ", \"prefix\": %d}", prefix);
			firstip = 0;
		}
		out_add(o, "]}");
		first = 0;
	}
	out_add(o, "]");
	if (list)
		freeifaddrs(list);
	if (names)
		if_freenameindex(names);
}

/* Users with a desktop or login session: those with a runtime folder. */
static void cmd_users(struct out *o)
{
	DIR *d = opendir("/run/user");
	struct dirent *e;
	int first = 1;

	out_add(o, "[");
	while (d && (e = readdir(d))) {
		char path[PATH_MAX];
		struct passwd *pw;
		struct stat st;
		char *end;
		long uid = strtol(e->d_name, &end, 10);

		if (*end || e->d_name[0] == '\0' || uid < 1000 || !(pw = getpwuid(uid)))
			continue;
		snprintf(path, sizeof(path), "/run/user/%s/wayland-0", e->d_name);
		if (stat(path, &st) < 0)
			continue;
		out_add(o, "%s{\"user\": ", first ? "" : ", ");
		out_str(o, pw->pw_name);
		out_add(o, ", \"login-time\": %lld.0}", (long long)st.st_mtime);
		first = 0;
	}
	if (d)
		closedir(d);
	out_add(o, "]");
}

static void cmd_timezone(struct out *o)
{
	time_t now = time(NULL);
	struct tm tm;

	tzset();
	localtime_r(&now, &tm);
	out_add(o, "{\"zone\": ");
	out_str(o, tm.tm_zone ? tm.tm_zone : "UTC");
	out_add(o, ", \"offset\": %ld}", tm.tm_gmtoff);
}

static int set_time(struct request *r, const char *id)
{
	if (r->has_arg_time) {
		struct timespec ts = { r->arg_time / 1000000000LL, r->arg_time % 1000000000LL };

		if (r->arg_time <= 0 || clock_settime(CLOCK_REALTIME, &ts) < 0) {
			error("GenericError", "cannot set the clock", id);
			return -1;
		}
	} else {
		/* No time given: from the hardware clock, which the host keeps. */
		pid_t pid = fork();
		int status;

		if (pid == 0) {
			int null = open("/dev/null", O_RDWR);

			dup2(null, 1);
			dup2(null, 2);
			execl("/bin/busybox", "hwclock", "-s", "-u", (char *)NULL);
			_exit(127);
		}
		if (pid < 0 || waitpid(pid, &status, 0) < 0 || !WIFEXITED(status) ||
		    WEXITSTATUS(status)) {
			error("GenericError", "cannot read the hardware clock", id);
			return -1;
		}
	}
	return 0;
}

static void handle(const char *text)
{
	struct request r;
	struct out o = { .len = 0 };

	if (parse(text, &r) < 0) {
		error("GenericError", "invalid JSON", NULL);
		return;
	}
	if (!strcmp(r.execute, "guest-sync") || !strcmp(r.execute, "guest-sync-delimited")) {
		if (!r.has_arg_id) {
			error("GenericError", "the id is missing", r.id);
			return;
		}
		/* The marker that tells the host to drop what came before. */
		if (!strcmp(r.execute, "guest-sync-delimited"))
			send_all("\xff", 1);
		out_add(&o, "%lld", r.arg_id);
	} else if (!strcmp(r.execute, "guest-ping")) {
		out_add(&o, "{}");
	} else if (!strcmp(r.execute, "guest-info")) {
		cmd_info(&o);
	} else if (!strcmp(r.execute, "guest-get-osinfo")) {
		cmd_osinfo(&o);
	} else if (!strcmp(r.execute, "guest-get-hostname")) {
		char name[256] = "";

		gethostname(name, sizeof(name) - 1);
		out_add(&o, "{\"host-name\": ");
		out_str(&o, name);
		out_add(&o, "}");
	} else if (!strcmp(r.execute, "guest-get-timezone")) {
		cmd_timezone(&o);
	} else if (!strcmp(r.execute, "guest-get-time")) {
		struct timespec ts;

		clock_gettime(CLOCK_REALTIME, &ts);
		out_add(&o, "%lld", (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec);
	} else if (!strcmp(r.execute, "guest-set-time")) {
		if (set_time(&r, r.id) < 0)
			return;
		out_add(&o, "{}");
	} else if (!strcmp(r.execute, "guest-shutdown")) {
		/* As busybox poweroff, halt and reboot: worminit's signals. No
		 * answer on success, like QEMU's agent. */
		int sig = !r.mode[0] || !strcmp(r.mode, "powerdown") ? SIGUSR2 :
			  !strcmp(r.mode, "halt") ? SIGUSR1 : !strcmp(r.mode, "reboot") ? SIGTERM : 0;

		if (!sig) {
			error("InvalidParameter", "mode is powerdown, halt or reboot", r.id);
			return;
		}
		if (kill(1, sig) < 0)
			error("GenericError", "cannot signal init", r.id);
		return;
	} else if (!strcmp(r.execute, "guest-network-get-interfaces")) {
		cmd_interfaces(&o);
	} else if (!strcmp(r.execute, "guest-get-users")) {
		cmd_users(&o);
	} else if (!strcmp(r.execute, "guest-fsfreeze-status")) {
		out_add(&o, "\"thawed\"");
	} else if (!strcmp(r.execute, "guest-get-vcpus")) {
		long n = sysconf(_SC_NPROCESSORS_CONF);

		out_add(&o, "[");
		for (long i = 0; i < n; i++)
			out_add(&o, "%s{\"logical-id\": %ld, \"online\": true, \"can-offline\": false}",
				i ? ", " : "", i);
		out_add(&o, "]");
	} else {
		char desc[128];

		snprintf(desc, sizeof(desc), "The command %s has not been found", r.execute);
		error("CommandNotFound", desc, r.id);
		return;
	}
	reply(&o, r.id);
}

/* ---- the port ---- */

static int enabled(void)
{
	char line[128];
	FILE *f = fopen(VM_RUN, "re");
	int on = 1;

	while (f && fgets(line, sizeof(line), f))
		if (!strcmp(line, "agent=off\n"))
			on = 0;
	if (f)
		fclose(f);
	return on;
}

static int open_port(void)
{
	DIR *d = opendir("/sys/class/virtio-ports");
	struct dirent *e;
	int fd = -1;

	while (d && fd < 0 && (e = readdir(d))) {
		char path[PATH_MAX], name[128] = "";
		FILE *f;

		if (e->d_name[0] == '.')
			continue;
		snprintf(path, sizeof(path), "/sys/class/virtio-ports/%s/name", e->d_name);
		if (!(f = fopen(path, "re")))
			continue;
		if (fgets(name, sizeof(name), f))
			name[strcspn(name, "\n")] = '\0';
		fclose(f);
		if (strcmp(name, PORT_NAME))
			continue;
		snprintf(path, sizeof(path), "/dev/%s", e->d_name);
		fd = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC | O_NOCTTY);
	}
	if (d)
		closedir(d);
	return fd;
}

int main(void)
{
	static char buf[MAX_CMD + 1];
	size_t len = 0;
	int depth = 0, in_string = 0, escaped = 0;
	FILE *pid;

	signal(SIGPIPE, SIG_IGN);
	if ((pid = fopen(PID_FILE, "we"))) {
		fprintf(pid, "%d\n", getpid());
		fclose(pid);
	}
	for (;;) {
		struct pollfd p;
		char chunk[4096];
		ssize_t n;

		if (port < 0) {
			if (!enabled() || (port = open_port()) < 0) {
				sleep(10);
				continue;
			}
			len = depth = in_string = escaped = 0;
		}
		p.fd = port;
		p.events = POLLIN;
		if (poll(&p, 1, 5000) < 0 && errno != EINTR)
			break;
		if (!enabled()) {
			close(port);
			port = -1;
			continue;
		}
		/* Nobody on the host's end: wait for it. */
		if (p.revents & POLLHUP && !(p.revents & POLLIN)) {
			sleep(1);
			continue;
		}
		if (!(p.revents & POLLIN))
			continue;
		n = read(port, chunk, sizeof(chunk));
		if (n <= 0) {
			if (n < 0 && (errno == EAGAIN || errno == EINTR))
				continue;
			sleep(1);
			continue;
		}
		/* Split the stream into top-level JSON objects. */
		for (ssize_t i = 0; i < n; i++) {
			unsigned char c = chunk[i];

			if (c == 0xff) {        /* the host resetting the parser */
				len = depth = in_string = escaped = 0;
				continue;
			}
			if (depth == 0 && c != '{')
				continue;
			if (len >= MAX_CMD) {   /* too long: drop it */
				len = depth = in_string = escaped = 0;
				error("GenericError", "command too long", NULL);
				continue;
			}
			buf[len++] = c;
			if (in_string) {
				if (escaped)
					escaped = 0;
				else if (c == '\\')
					escaped = 1;
				else if (c == '"')
					in_string = 0;
			} else if (c == '"') {
				in_string = 1;
			} else if (c == '{' || c == '[') {
				depth++;
			} else if (c == '}' || c == ']') {
				if (--depth == 0) {
					buf[len] = '\0';
					handle(buf);
					len = 0;
				}
			}
		}
	}
	return 1;
}
