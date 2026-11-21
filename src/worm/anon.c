/*
 * worm anon - anonymous mode: all traffic of the machine through Tor.
 *
 *     worm anon on | off      switch it (kept across restarts)
 *     worm anon status        is it on, how far Tor has connected (for
 *                             everyone; users in the group tor also see
 *                             whether Tor runs and how far it got)
 *     worm anon start         start Tor if anonymous mode is on (at boot),
 *                             unless ANON_HOLD exists: an edition's start
 *                             screen asks first how Tor should connect
 *                             (WormOS Amnesia), then removes it and runs
 *                             "worm anon start" itself
 *     worm anon bridges snowflake|obfs4|meek|custom|off|status
 *                             reach Tor through bridges, on networks that
 *                             block it: the Tor Project's default bridges
 *                             of one kind, or bridge lines given on the
 *                             standard input (custom)
 *
 * The switch is the file /etc/worm/anon, so it is part of system snapshots
 * like the firewall's ports. The firewall (fw.c) then lets only Tor's own
 * user reach the network, sends every other TCP connection to Tor's
 * TransPort and every DNS query to its DNSPort, and rejects the rest (UDP,
 * ICMP, IPv6). The rules load before the network comes up and before Tor
 * starts, so nothing leaks while Tor connects; until then connections fail.
 *
 * Bridges go in BRIDGES_CONF, which the torrc includes: UseBridges, the
 * transport (lyrebird from Tor Browser's bundle, which speaks obfs4,
 * webtunnel, snowflake and meek) and Bridge lines. The default bridges are
 * the ones Tor Browser ships (/usr/share/tor/bridges, taken from it when
 * the bundle is built). With bridges the firewall also lets the resolver
 * look up names for Tor's user (transports find their servers by name
 * before Tor is connected) and, for snowflake, lets Tor's user send UDP.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "worm.h"

#define ANON_CONFIG  "/etc/worm/anon"
#define TOR          "/usr/bin/tor"
#define TOR_RUN      "/run/tor"
#define TOR_PID      TOR_RUN "/tor.pid"
#define TOR_LOG      TOR_RUN "/notices.log"
#define TOR_DATA     "/var/lib/tor"
#define ANON_HOLD    "/run/worm/anon-hold"
#define LYREBIRD     "/opt/tor-browser/Browser/TorBrowser/Tor/PluggableTransports/lyrebird"
#define DEFAULT_BRIDGES "/usr/share/tor/bridges"
#define MAX_BRIDGE_LINE 2048

int anon_enabled(void)
{
	return access(ANON_CONFIG, F_OK) == 0;
}

long anon_tor_uid(void)
{
	struct passwd *pw = getpwnam("tor");

	return pw ? (long)pw->pw_uid : -1;
}

/* The running Tor's PID, or 0. */
static pid_t tor_pid(void)
{
	char path[64], comm[32] = "";
	long pid = 0;
	FILE *f;

	if ((f = fopen(TOR_PID, "re"))) {
		if (fscanf(f, "%ld", &pid) != 1)
			pid = 0;
		fclose(f);
	}
	if (pid <= 1)
		return 0;
	snprintf(path, sizeof(path), "/proc/%ld/comm", pid);
	if (!(f = fopen(path, "re"))) {
		/*
		 * A user cannot see Tor's process (/proc has hidepid), but a
		 * signal 0 tells whether it exists. Users reach the pid file
		 * only through the tor group (WormOS Amnesia's live user).
		 */
		if (geteuid() != 0 && kill((pid_t)pid, 0) < 0 && errno == EPERM)
			return (pid_t)pid;
		return 0;
	}
	if (fgets(comm, sizeof(comm), f))
		comm[strcspn(comm, "\n")] = '\0';
	fclose(f);
	return strcmp(comm, "tor") == 0 ? (pid_t)pid : 0;
}

/* How far Tor has connected, from its log: 0 to 100, or -1. */
static int bootstrapped(void)
{
	char line[512], *p;
	int percent = -1;
	FILE *f = fopen(TOR_LOG, "re");

	while (f && fgets(line, sizeof(line), f))
		if ((p = strstr(line, "Bootstrapped ")))
			percent = atoi(p + 13);
	if (f)
		fclose(f);
	return percent;
}

int anon_connected(void)
{
	return bootstrapped() == 100;
}

/*
 * The clock skew Tor has seen, from its log: relays and directories say
 * "It seems that our clock is behind by 5 hours, 2 minutes" (or ahead)
 * when their time, from an authenticated connection, differs from ours.
 * 0 and the offset to add to our clock (the last report), or -1.
 */
/* "2 days, 3 hours, 4 minutes" in seconds; *end after the last unit. */
static long parse_interval(const char *p, const char **end)
{
	long total = 0, n;
	char unit[16];
	int used;

	while (sscanf(p, "%ld %15[a-z]%n", &n, unit, &used) == 2) {
		if (!strncmp(unit, "day", 3))
			total += n * 86400;
		else if (!strncmp(unit, "hour", 4))
			total += n * 3600;
		else if (!strncmp(unit, "minute", 6))
			total += n * 60;
		else if (!strncmp(unit, "second", 6))
			total += n;
		else
			break;
		p += used;
		*end = p;
		if (*p != ',')
			break;
		p++;
		while (*p == ' ')
			p++;
	}
	return total;
}

int anon_tor_skew(long *seconds)
{
	char line[1024], *p;
	const char *end;
	int found = -1;
	FILE *f = fopen(TOR_LOG, "re");

	while (f && fgets(line, sizeof(line), f)) {
		long total = 0;
		int sign = 0;

		if (!(p = strstr(line, "ur clock is ")))
			continue;
		p += 12;
		end = p;
		/* Relays: "our clock is behind by 5 hours, 2 minutes". */
		if (!strncmp(p, "behind by ", 10) || !strncmp(p, "ahead by ", 9)) {
			sign = *p == 'b' ? 1 : -1;
			total = parse_interval(strstr(p, " by ") + 4, &end);
		/* The consensus: "Our clock is 5 hours, 2 minutes behind the time". */
		} else if ((total = parse_interval(p, &end)) > 0) {
			if (!strncmp(end, " behind", 7))
				sign = 1;
			else if (!strncmp(end, " ahead", 6))
				sign = -1;
		}
		if (sign && total > 0) {
			*seconds = sign * total;
			found = 0;
		}
	}
	if (f)
		fclose(f);
	return found;
}

static int tor_start(void);
static void tor_stop(void);

int anon_restart_tor(void)
{
	tor_stop();
	return tor_start();
}

static int tor_start(void)
{
	long uid = anon_tor_uid();

	if (tor_pid())
		return 0;
	if (uid < 0 || access(TOR, X_OK) < 0) {
		warn("Tor is not installed (wh install tor)%s", "");
		return -1;
	}
	/* Tor insists on owning its directories. */
	if ((mkdir(TOR_DATA, 0700) < 0 && errno != EEXIST) ||
	    chown(TOR_DATA, uid, uid) < 0 || chmod(TOR_DATA, 0700) < 0 ||
	    (mkdir(TOR_RUN, 0750) < 0 && errno != EEXIST) || chown(TOR_RUN, uid, uid) < 0) {
		warn("cannot prepare Tor's directories: %s", strerror(errno));
		return -1;
	}
	unlink(TOR_LOG);
	if (run((char *[]){ TOR, "-f", "/etc/tor/torrc", "--quiet", NULL }, NULL, 0, 0) != 0) {
		warn("Tor did not start (see %s)", TOR_LOG);
		return -1;
	}
	return 0;
}

static void tor_stop(void)
{
	pid_t pid = tor_pid();

	if (!pid)
		return;
	kill(pid, SIGTERM);
	for (int i = 0; i < 100 && tor_pid(); i++)
		usleep(50000);
	if (tor_pid())
		kill(pid, SIGKILL);
}

static void write_switch(int on)
{
	int relock = root_begin_change();
	int ok = 1;

	if (on) {
		FILE *f = fopen(ANON_CONFIG, "wxe");

		ok = f && fputs("# Anonymous mode is on (worm anon): all traffic goes through Tor.\n",
				f) >= 0;
		ok = f && fclose(f) == 0 && ok;
	} else {
		ok = unlink(ANON_CONFIG) == 0 || errno == ENOENT;
	}
	int err = errno;

	root_end_change(relock);
	if (!ok)
		die("cannot change " ANON_CONFIG ": %s", strerror(err));
}

static int anon_on(void)
{
	if (anon_tor_uid() < 0 || access(TOR, X_OK) < 0)
		die("Tor is not installed; 'wh install tor' first%s", "");
	if (!anon_enabled())
		write_switch(1);
	/* Rules first, then Tor: nothing gets out directly in between. */
	if (fw_reload() != 0)
		die("cannot load the firewall rules for anonymous mode%s", "");
	if (tor_start() < 0)
		return 1;
	printf("Anonymous mode is on: all traffic goes through Tor, and everything\n"
	       "else is blocked. Tor is connecting; 'worm anon status' shows how far.\n");
	return 0;
}

static int anon_off(void)
{
	if (variant_is("amnesia"))
		die("WormOS Amnesia always sends everything through Tor%s", "");
	if (anon_enabled())
		write_switch(0);
	if (fw_reload() != 0)
		die("cannot load the normal firewall rules%s", "");
	tor_stop();
	printf("Anonymous mode is off: traffic goes out directly again.\n");
	return 0;
}

const char *anon_summary(void)
{
	static char buf[96];
	int percent = bootstrapped();

	if (!anon_enabled())
		return "off";
	if (!tor_pid())
		return "on, but Tor is NOT RUNNING: no traffic gets out";
	if (percent >= 100)
		return "on: all traffic through Tor (connected)";
	snprintf(buf, sizeof(buf), "on: all traffic through Tor (connecting, %d%%)",
		 percent < 0 ? 0 : percent);
	return buf;
}

/* A bridge line as Tor takes it: printable, no newlines, a sane length. */
static int valid_bridge(const char *line)
{
	size_t n = strlen(line);

	if (n < 8 || n >= MAX_BRIDGE_LINE)
		return 0;
	for (size_t i = 0; i < n; i++)
		if ((unsigned char)line[i] < 0x20 || line[i] == 0x7f)
			return 0;
	return 1;
}

static int bridges_set(const char *kind)
{
	char path[256], line[MAX_BRIDGE_LINE + 2];
	int n = 0, relock, ok;
	FILE *in, *out;

	if (access(LYREBIRD, X_OK) < 0)
		die("bridges need Tor Browser's transports; 'wh install tor-browser-bundle' first%s", "");
	if (!strcmp(kind, "custom")) {
		if (isatty(STDIN_FILENO))
			printf("Paste the bridge lines (from bridges.torproject.org), then Ctrl+D:\n");
		in = stdin;
	} else {
		if (strchr(kind, '/') || strchr(kind, '.'))
			return 2;
		snprintf(path, sizeof(path), "%s/%s", DEFAULT_BRIDGES, kind);
		if (!(in = fopen(path, "re")))
			die("no default bridges of the kind '%s' (snowflake, obfs4 or meek)", kind);
	}
	relock = root_begin_change();
	mkdir(TORRC_D, 0755);
	if (!(out = fopen(BRIDGES_CONF ".new", "we"))) {
		root_end_change(relock);
		die("cannot write " BRIDGES_CONF ": %s", strerror(errno));
	}
	fprintf(out, "# Bridges for Tor (worm anon bridges %s).\nUseBridges 1\n"
		"ClientTransportPlugin meek_lite,obfs4,webtunnel,snowflake exec " LYREBIRD "\n", kind);
	while (fgets(line, sizeof(line), in)) {
		char *p = line;

		line[strcspn(line, "\r\n")] = '\0';
		while (*p == ' ' || *p == '\t')
			p++;
		if (!strncmp(p, "Bridge ", 7))
			p += 7;
		if (!*p || *p == '#')
			continue;
		if (!valid_bridge(p)) {
			warn("skipping a line that is not a bridge: %.40s", p);
			continue;
		}
		fprintf(out, "Bridge %s\n", p);
		n++;
	}
	if (in != stdin)
		fclose(in);
	ok = fclose(out) == 0 && n > 0 && rename(BRIDGES_CONF ".new", BRIDGES_CONF) == 0;
	if (!ok)
		unlink(BRIDGES_CONF ".new");
	root_end_change(relock);
	if (!ok)
		die(n ? "cannot write " BRIDGES_CONF "%s" : "no bridge lines given%s", "");
	printf("Tor reaches the network through %d %s bridge%s.\n", n,
	       strcmp(kind, "custom") ? kind : "custom", n == 1 ? "" : "s");
	return 0;
}

static int bridges_off(void)
{
	int relock = root_begin_change();
	int ok = unlink(BRIDGES_CONF) == 0 || errno == ENOENT;

	root_end_change(relock);
	if (!ok)
		die("cannot remove " BRIDGES_CONF ": %s", strerror(errno));
	printf("Tor connects directly again (no bridges).\n");
	return 0;
}

/* Bridges changed: new firewall rules and, if it runs, a Tor that uses them. */
static void bridges_apply(void)
{
	if (fw_reload() != 0)
		warn("cannot reload the firewall rules%s", "");
	if (anon_enabled() && tor_pid()) {
		tor_stop();
		if (tor_start() == 0)
			printf("Tor restarted; 'worm anon status' shows how far it got.\n");
	}
}

/* How Tor reaches the network: "direct", or the kind of bridges. */
const char *anon_bridges(void)
{
	static char kind[32];
	char line[128];
	FILE *f = fopen(BRIDGES_CONF, "re");

	if (!f)
		return "direct";
	snprintf(kind, sizeof(kind), "bridges");
	if (fgets(line, sizeof(line), f))
		sscanf(line, "# Bridges for Tor (worm anon bridges %31[a-z0-9]", kind);
	fclose(f);
	return kind;
}

int cmd_anon(int argc, char **argv)
{
	if (argc == 2 && !strcmp(argv[0], "bridges")) {
		int r;

		if (!strcmp(argv[1], "status")) {
			printf("Tor connects: %s\n", !strcmp(anon_bridges(), "direct") ? "directly" :
			       anon_bridges());
			return 0;
		}
		if (!strcmp(argv[1], "off"))
			r = bridges_off();
		else if ((r = bridges_set(argv[1])) != 0)
			return r;
		bridges_apply();
		return r;
	}
	if (argc != 1)
		return 2;
	if (!strcmp(argv[0], "on"))
		return anon_on();
	if (!strcmp(argv[0], "off"))
		return anon_off();
	if (!strcmp(argv[0], "status")) {
		printf("Anonymous mode: %s\n", anon_summary());
		if (anon_enabled() && strcmp(anon_bridges(), "direct"))
			printf("Tor connects through %s bridges.\n", anon_bridges());
		return 0;
	}
	if (!strcmp(argv[0], "start"))
		return anon_enabled() && !safe_mode() && access(ANON_HOLD, F_OK) < 0 ?
		       (tor_start() < 0) : 0;
	return 2;
}
