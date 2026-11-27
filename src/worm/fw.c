/*
 * worm fw - the firewall.
 *
 *     worm fw status          is it active, which ports are open
 *     worm fw allow PORT      open an inbound port (22, 22/udp, 6000-6010)
 *     worm fw deny PORT       close it again
 *     worm fw load            load the rules (at boot, before networking)
 *
 * Inbound traffic is dropped unless it belongs to a connection this machine
 * started, comes from loopback, is essential ICMP (errors and neighbour
 * discovery; pings get no answer), or targets an open port. Forwarding is
 * dropped. Outbound traffic is not filtered. The network service starts
 * only once these rules are loaded.
 *
 * Open ports are kept in /etc/worm/firewall, one "tcp 22" or "udp
 * 6000-6010" per line, so they are part of system snapshots and a rewind
 * brings back the rules of that time. The rules are generated into
 * /run/worm/firewall.nft and loaded with nft as one transaction, which
 * replaces the whole "inet worm" table at once.
 *
 * In anonymous mode (anon.c) the table also filters output: only Tor's
 * user may reach the network (with TCP), other TCP connections are
 * redirected to Tor's TransPort and DNS queries to its DNSPort, and
 * everything else is rejected. Root's DHCP renewals are the one exception.
 * Without a tor user, nothing but loopback gets out.
 *
 * The network can also be cut (worm panic, and safe mode from boot): a
 * table that drops everything but loopback, in and out, until 'worm panic
 * off'. The marker /run/worm/network-cut keeps it in force across reloads.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "worm.h"

#define FW_CONFIG  "/etc/worm/firewall"
#define FW_RUNDIR  "/run/worm"
#define FW_RULES   FW_RUNDIR "/firewall.nft"
#define FW_CUT     FW_RUNDIR "/network-cut"
#define MAX_PORTS  256

struct port {
	char proto[4];          /* "tcp" or "udp" */
	unsigned lo, hi;
};

static int parse_number(const char *s, const char *end, unsigned *n)
{
	unsigned v = 0;

	if (s == end || end - s > 5)
		return -1;
	for (; s < end; s++) {
		if (*s < '0' || *s > '9')
			return -1;
		v = v * 10 + (*s - '0');
	}
	if (v < 1 || v > 65535)
		return -1;
	*n = v;
	return 0;
}

/* "22", "6000-6010" with the protocol given separately. */
static int parse_range(const char *s, const char *proto, struct port *p)
{
	const char *dash = strchr(s, '-'), *end = s + strlen(s);

	if (strcmp(proto, "tcp") && strcmp(proto, "udp"))
		return -1;
	strcpy(p->proto, proto);
	if (!dash)
		return parse_number(s, end, &p->lo) < 0 ? -1 : (p->hi = p->lo, 0);
	if (parse_number(s, dash, &p->lo) < 0 || parse_number(dash + 1, end, &p->hi) < 0 ||
	    p->hi < p->lo)
		return -1;
	return 0;
}

/* A command line argument: "22", "22/udp", "6000-6010/tcp". */
static int parse_arg(const char *arg, struct port *p)
{
	char buf[32], *slash;

	if (strlen(arg) >= sizeof(buf))
		return -1;
	strcpy(buf, arg);
	if ((slash = strchr(buf, '/')))
		*slash = '\0';
	return parse_range(buf, slash ? slash + 1 : "tcp", p);
}

static const char *port_str(const struct port *p)
{
	static char buf[32];

	if (p->lo == p->hi)
		snprintf(buf, sizeof(buf), "%u/%.3s", p->lo, p->proto);
	else
		snprintf(buf, sizeof(buf), "%u-%u/%.3s", p->lo, p->hi, p->proto);
	return buf;
}

/* Read the open ports. A missing file means none. Returns -1 on errors. */
static int read_config(struct port *ports, int *n)
{
	char line[256];
	int lineno = 0, ret = 0;
	FILE *f = fopen(FW_CONFIG, "re");

	*n = 0;
	if (!f)
		return errno == ENOENT ? 0 : -1;
	while (fgets(line, sizeof(line), f)) {
		char proto[8], range[32], extra[2];
		int fields;

		lineno++;
		line[strcspn(line, "#\n")] = '\0';
		fields = sscanf(line, "%7s %31s %1s", proto, range, extra);
		if (fields <= 0)
			continue;
		if (fields != 2 || *n == MAX_PORTS || parse_range(range, proto, &ports[*n]) < 0) {
			fprintf(stderr, "worm: %s:%d: invalid line\n", FW_CONFIG, lineno);
			ret = -1;
			continue;
		}
		(*n)++;
	}
	fclose(f);
	return ret;
}

/* Replace the configuration file, briefly making / writable if needed. */
static void write_config(const struct port *ports, int n)
{
	const char *tmp = FW_CONFIG ".new";
	int relock = root_begin_change();
	FILE *f = fopen(tmp, "we");
	int ok = f != NULL;

	if (f) {
		fprintf(f, "# Inbound ports open in the firewall, managed with worm fw.\n");
		for (int i = 0; i < n; i++)
			if (ports[i].lo == ports[i].hi)
				fprintf(f, "%s %u\n", ports[i].proto, ports[i].lo);
			else
				fprintf(f, "%s %u-%u\n", ports[i].proto, ports[i].lo, ports[i].hi);
		ok = fflush(f) == 0 && fsync(fileno(f)) == 0;
		ok = fclose(f) == 0 && ok;
	}
	if (ok && chmod(tmp, 0644) == 0 && rename(tmp, FW_CONFIG) == 0) {
		root_end_change(relock);
		return;
	}
	int err = errno;

	unlink(tmp);
	root_end_change(relock);
	die("cannot write " FW_CONFIG ": %s", strerror(err));
}

static void write_set(FILE *f, const char *name, const char *proto,
		      const struct port *ports, int n)
{
	int first = 1;

	fprintf(f, "\tset %s {\n\t\ttype inet_service\n\t\tflags interval\n"
		"\t\tauto-merge\n", name);
	for (int i = 0; i < n; i++) {
		if (strcmp(ports[i].proto, proto))
			continue;
		fprintf(f, "%s", first ? "\t\telements = { " : ", ");
		if (ports[i].lo == ports[i].hi)
			fprintf(f, "%u", ports[i].lo);
		else
			fprintf(f, "%u-%u", ports[i].lo, ports[i].hi);
		first = 0;
	}
	fprintf(f, "%s\t}\n", first ? "" : " }\n");
}

/* Anonymous mode: only Tor reaches the network, everything else goes to it. */
static void write_anon(FILE *f, long tor)
{
	char skuid[256] = "";

	if (tor >= 0) {
		const char *bridges = anon_bridges();
		struct passwd *ub = getpwnam("unbound");
		size_t n = snprintf(skuid, sizeof(skuid),
				    "\t\tmeta skuid %ld meta l4proto tcp accept\n", tor);

		/*
		 * Bridges: transports look their servers up by name before Tor
		 * is connected (through the resolver, over encrypted DNS), and
		 * snowflake talks WebRTC over UDP.
		 */
		if (strcmp(bridges, "direct") && ub)
			n += snprintf(skuid + n, sizeof(skuid) - n,
				      "\t\tmeta skuid %ld tcp dport 853 accept\n", (long)ub->pw_uid);
		if (!strcmp(bridges, "snowflake"))
			snprintf(skuid + n, sizeof(skuid) - n,
				 "\t\tmeta skuid %ld meta l4proto udp accept\n", tor);
	}
	fprintf(f, "\tchain output {\n"
		"\t\ttype filter hook output priority filter; policy drop;\n"
		"\t\toif lo accept\n"
		/*
		 * What the NAT chain sent to Tor: the packet now goes to
		 * loopback, but oif still names the original device.
		 */
		"\t\tct status dnat ip daddr 127.0.0.1 accept\n"
		"\t\tct status dnat ip6 daddr ::1 accept\n"
		"%s"
		/* Root's DHCP renewals; the first lease uses a raw socket anyway. */
		"\t\tmeta skuid 0 udp sport 68 udp dport 67 accept\n"
		"\t\tmeta l4proto tcp reject with tcp reset\n"
		"\t\treject with icmpx admin-prohibited\n"
		"\t}\n", skuid);
	if (tor >= 0)
		snprintf(skuid, sizeof(skuid), "\t\tmeta skuid %ld return\n", tor);
	fprintf(f, "\tchain anon_nat {\n"
		"\t\ttype nat hook output priority -100;\n"
		"%s"
		"\t\tudp dport 53 redirect to :5353\n"
		"\t\tip daddr 127.0.0.0/8 return\n"
		/*
		 * The system's own connections (root and service accounts:
		 * updates, the clock check) and the users' programs use
		 * different TransPorts, so Tor never puts them on one circuit.
		 */
		"\t\tmeta skuid < 1000 meta nfproto ipv4 meta l4proto tcp redirect to :9040\n"
		"\t\tmeta nfproto ipv4 meta l4proto tcp redirect to :9041\n"
		"\t}\n", skuid);
}

/*
 * /run/worm: others may look up names in it (the panel checks for
 * network-cut) but not list it.
 */
static void run_dir(void)
{
	mkdir(FW_RUNDIR, 0711);
	chmod(FW_RUNDIR, 0711);
}

static int write_rules(const struct port *ports, int n)
{
	FILE *f;

	run_dir();
	if (!(f = fopen(FW_RULES, "we"))) {
		fprintf(stderr, "worm: cannot write %s: %s\n", FW_RULES, strerror(errno));
		return -1;
	}
	fputs("# Generated by worm fw from " FW_CONFIG "; changes here are lost.\n"
	      "destroy table inet worm\n"
	      "table inet worm {\n"
	      "\tcounter dropped {\n\t}\n", f);
	write_set(f, "open_tcp", "tcp", ports, n);
	write_set(f, "open_udp", "udp", ports, n);
	fputs("\tchain input {\n"
	      "\t\ttype filter hook input priority filter; policy drop;\n"
	      "\t\tct state vmap { established : accept, related : accept, invalid : drop }\n"
	      "\t\tiif lo accept\n"
	      "\t\ticmp type { destination-unreachable, time-exceeded, parameter-problem } accept\n"
	      "\t\ticmpv6 type { destination-unreachable, packet-too-big, time-exceeded,"
	      " parameter-problem } accept\n"
	      /* Neighbour discovery only from the local link. */
	      "\t\ticmpv6 type { nd-router-advert, nd-neighbor-solicit, nd-neighbor-advert }"
	      " ip6 hoplimit 255 accept\n"
	      "\t\ticmpv6 type mld-listener-query ip6 saddr fe80::/10 accept\n"
	      "\t\tip6 saddr fe80::/10 udp sport 547 udp dport 546 accept\n"
	      "\t\ttcp dport @open_tcp accept\n"
	      "\t\tudp dport @open_udp accept\n"
	      "\t\tcounter name dropped\n"
	      "\t}\n"
	      "\tchain forward {\n"
	      "\t\ttype filter hook forward priority filter; policy drop;\n"
	      "\t}\n", f);
	if (anon_enabled())
		write_anon(f, anon_tor_uid());
	fputs("}\n", f);
	if (fclose(f) != 0) {
		fprintf(stderr, "worm: cannot write %s: %s\n", FW_RULES, strerror(errno));
		return -1;
	}
	return 0;
}

static int apply(const struct port *ports, int n)
{
	if (write_rules(ports, n) < 0)
		return -1;
	return run((char *[]){ "nft", "-f", FW_RULES, NULL }, NULL, 0, 0) == 0 ? 0 : -1;
}

static int active(void)
{
	char out[512];

	return run((char *[]){ "nft", "list", "chain", "inet", "worm", "input", NULL },
		   out, sizeof(out), 1) == 0;
}

int fw_network_cut(void)
{
	return access(FW_CUT, F_OK) == 0;
}

int fw_cut(void)
{
	FILE *f;
	int fd;

	run_dir();
	if ((fd = open(FW_CUT, O_WRONLY | O_CREAT | O_CLOEXEC, 0600)) >= 0)
		close(fd);
	if (!(f = fopen(FW_RULES, "we")))
		return -1;
	fputs("# Generated by worm: the network is cut.\n"
	      "destroy table inet worm\n"
	      "table inet worm {\n"
	      "\tchain input {\n"
	      "\t\ttype filter hook input priority filter; policy drop;\n"
	      "\t\tiif lo accept\n"
	      "\t}\n"
	      "\tchain forward {\n"
	      "\t\ttype filter hook forward priority filter; policy drop;\n"
	      "\t}\n"
	      "\tchain output {\n"
	      "\t\ttype filter hook output priority filter; policy drop;\n"
	      "\t\toif lo accept\n"
	      "\t}\n"
	      "}\n", f);
	if (fclose(f) != 0)
		return -1;
	return run((char *[]){ "nft", "-f", FW_RULES, NULL }, NULL, 0, 0) == 0 ? 0 : -1;
}

static int cmd_load(void)
{
	struct port ports[MAX_PORTS];
	int n, ret = 0;

	if (safe_mode() || fw_network_cut()) {
		if (fw_cut() == 0)
			return 0;
		fprintf(stderr, "worm: cannot cut the network\n");
		return 1;
	}
	if (read_config(ports, &n) < 0) {
		fprintf(stderr, "worm: loading the firewall with no open ports\n");
		n = 0;
		ret = 1;
	}
	if (apply(ports, n) == 0)
		return ret;
	/* Never leave the machine open because of a bad port list. */
	if (n > 0 && apply(ports, 0) == 0) {
		fprintf(stderr, "worm: loaded the firewall with no open ports instead\n");
		return 1;
	}
	fprintf(stderr, "worm: the firewall is NOT active\n");
	return 1;
}

int fw_reload(void)
{
	return cmd_load();
}

int fw_restore(void)
{
	unlink(FW_CUT);
	return cmd_load();
}

const char *fw_summary(void)
{
	static char buf[64];
	struct port ports[MAX_PORTS];
	int n;

	if (!active())
		return "NOT ACTIVE";
	if (fw_network_cut())
		return "network cut: all traffic blocked";
	if (read_config(ports, &n) < 0)
		return "active, port list has errors";
	snprintf(buf, sizeof(buf), "active, %d open port%s", n, n == 1 ? "" : "s");
	return buf;
}

static int fw_status(void)
{
	struct port ports[MAX_PORTS];
	char out[512], *p;
	int n;

	if (!active()) {
		printf("Firewall: NOT ACTIVE (run 'worm fw load')\n");
		return 1;
	}
	if (fw_network_cut()) {
		printf("Firewall: the network is cut, all traffic is blocked\n"
		       "%s\n", safe_mode() ? "Safe mode: restart normally to use the network." :
		       "'worm panic off' restores it.");
		return 0;
	}
	printf("Firewall: active, inbound traffic is denied by default\n");
	if (read_config(ports, &n) < 0)
		printf("The port list in %s has errors (see above).\n", FW_CONFIG);
	if (n == 0)
		printf("Open ports: none\n");
	else
		printf("Open ports:\n");
	for (int i = 0; i < n; i++)
		printf("  %s\n", port_str(&ports[i]));
	if (run((char *[]){ "nft", "list", "counter", "inet", "worm", "dropped", NULL },
		out, sizeof(out), 1) == 0 && (p = strstr(out, "packets ")))
		printf("Dropped since loaded: %lu packets\n", strtoul(p + 8, NULL, 10));
	return 0;
}

static int same(const struct port *a, const struct port *b)
{
	return !strcmp(a->proto, b->proto) && a->lo == b->lo && a->hi == b->hi;
}

static int cmd_allow(const char *arg)
{
	struct port ports[MAX_PORTS], want;
	int n;

	if (fw_network_cut())
		die("the network is cut; 'worm panic off' restores it first%s", "");
	if (parse_arg(arg, &want) < 0)
		die("invalid port: %s", arg);
	if (read_config(ports, &n) < 0)
		die("fix %s first", FW_CONFIG);
	for (int i = 0; i < n; i++)
		if (same(&ports[i], &want)) {
			printf("%s is already open.\n", port_str(&want));
			return 0;
		}
	if (n == MAX_PORTS)
		die("too many open ports%s", "");
	ports[n++] = want;
	if (apply(ports, n) < 0)
		die("cannot update the firewall; nothing changed%s", "");
	write_config(ports, n);
	printf("Opened %s.\n", port_str(&want));
	return 0;
}

static int cmd_deny(const char *arg)
{
	struct port ports[MAX_PORTS], want;
	int n, found = -1;

	if (fw_network_cut())
		die("the network is cut; 'worm panic off' restores it first%s", "");
	if (parse_arg(arg, &want) < 0)
		die("invalid port: %s", arg);
	if (read_config(ports, &n) < 0)
		die("fix %s first", FW_CONFIG);
	for (int i = 0; i < n; i++) {
		if (same(&ports[i], &want))
			found = i;
		else if (!strcmp(ports[i].proto, want.proto) &&
			 ports[i].lo <= want.lo && want.hi <= ports[i].hi) {
			char range[32];

			strcpy(range, port_str(&ports[i]));
			printf("%s is open as part of %s; deny that instead.\n",
			       port_str(&want), range);
			return 1;
		}
	}
	if (found < 0) {
		printf("%s is not open.\n", port_str(&want));
		return 0;
	}
	memmove(&ports[found], &ports[found + 1], (n - found - 1) * sizeof(ports[0]));
	n--;
	if (apply(ports, n) < 0)
		die("cannot update the firewall; nothing changed%s", "");
	write_config(ports, n);
	printf("Closed %s.\n", port_str(&want));
	return 0;
}

int cmd_fw(int argc, char **argv)
{
	if (argc == 1 && !strcmp(argv[0], "status"))
		return fw_status();
	if (argc == 1 && !strcmp(argv[0], "load"))
		return cmd_load();
	if (argc == 2 && !strcmp(argv[0], "allow"))
		return cmd_allow(argv[1]);
	if (argc == 2 && !strcmp(argv[0], "deny"))
		return cmd_deny(argv[1]);
	return 2;
}
