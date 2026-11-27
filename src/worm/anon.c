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
#include <grp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include "worm.h"

#define ANON_CONFIG  "/etc/worm/anon"
#define TOR          "/usr/bin/tor"
#define TOR_RUN      "/run/tor"
#define TOR_PID      TOR_RUN "/tor.pid"
#ifndef TOR_LOG			/* tests/unit uses another file */
#define TOR_LOG      TOR_RUN "/notices.log"
#endif
#define TOR_DATA     "/var/lib/tor"
#define TOR_CONTROL  "/run/tor-control"
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
	    (mkdir(TOR_RUN, 0750) < 0 && errno != EEXIST) || chown(TOR_RUN, uid, uid) < 0 ||
	    (mkdir(TOR_CONTROL, 0700) < 0 && errno != EEXIST) ||
	    chown(TOR_CONTROL, uid, uid) < 0 || chmod(TOR_CONTROL, 0700) < 0) {
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

/*
 * worm anon new-circuits: Tor's NEWNYM signal through its control socket
 * (root only, cookie authentication): new connections get new circuits,
 * so they cannot be linked to earlier ones by the exit relays they use.
 * Tor Browser uses the system Tor, so its own "New Tor circuit" cannot
 * reach Tor; this does it for every program. Tor limits it to one every
 * 10 seconds.
 */
static int anon_new_circuits(void)
{
	struct sockaddr_un addr = { .sun_family = AF_UNIX };
	unsigned char cookie[32];
	char hex[sizeof(cookie) * 2 + 1], request[160], reply[512], *second;
	size_t got = 0;
	ssize_t r;
	int fd, ok;
	FILE *f;

	if (!anon_enabled())
		die("anonymous mode is off: no Tor to ask (worm anon on)%s", "");
	if (!tor_pid() || !(f = fopen(TOR_CONTROL "/cookie", "re")))
		die("Tor is not running%s", "");
	ok = fread(cookie, 1, sizeof(cookie), f) == sizeof(cookie);
	fclose(f);
	if (!ok)
		die("cannot read Tor's control cookie%s", "");
	for (size_t i = 0; i < sizeof(cookie); i++)
		snprintf(hex + 2 * i, 3, "%02x", cookie[i]);
	explicit_bzero(cookie, sizeof(cookie));
	snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", TOR_CONTROL "/control");
	if ((fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0)) < 0 ||
	    connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
		die("cannot reach Tor's controller: %s", strerror(errno));
	snprintf(request, sizeof(request), "AUTHENTICATE %s\r\nSIGNAL NEWNYM\r\nQUIT\r\n", hex);
	explicit_bzero(hex, sizeof(hex));
	ok = write(fd, request, strlen(request)) == (ssize_t)strlen(request);
	explicit_bzero(request, sizeof(request));
	/* Three replies: AUTHENTICATE, SIGNAL and QUIT, each "250 ..." on success. */
	while (ok && got < sizeof(reply) - 1 &&
	       (r = read(fd, reply + got, sizeof(reply) - 1 - got)) > 0)
		got += (size_t)r;
	close(fd);
	reply[got] = '\0';
	/* The first two lines answer AUTHENTICATE and SIGNAL. */
	second = strchr(reply, '\n');
	if (!ok || strncmp(reply, "250", 3)) {
		reply[strcspn(reply, "\r\n")] = '\0';
		die("Tor refused: %s", reply);
	}
	if (!second || strncmp(second + 1, "250", 3)) {
		second = second ? second + 1 : reply + got;
		second[strcspn(second, "\r\n")] = '\0';
		die("Tor refused: %s", second);
	}
	puts("New connections now use new Tor circuits.");
	return 0;
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

static int bridges_off(void);

/* Write the bridge lines from in as BRIDGES_CONF, named kind. */
static int bridges_write(const char *kind, FILE *in)
{
	char line[MAX_BRIDGE_LINE + 2];
	int n = 0, relock, ok;
	FILE *out;

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

static int bridges_set(const char *kind)
{
	char path[256];
	FILE *in;
	int r;

	if (access(LYREBIRD, X_OK) < 0)
		die("bridges need Tor Browser's transports; 'wh install tor-browser-bundle' first%s", "");
	if (!strcmp(kind, "custom")) {
		if (isatty(STDIN_FILENO))
			printf("Paste the bridge lines (from bridges.torproject.org), then Ctrl+D:\n");
		return bridges_write(kind, stdin);
	}
	if (strchr(kind, '/') || strchr(kind, '.'))
		return 2;
	snprintf(path, sizeof(path), "%s/%s", DEFAULT_BRIDGES, kind);
	if (!(in = fopen(path, "re")))
		die("no default bridges of the kind '%s' (snowflake, obfs4 or meek)", kind);
	r = bridges_write(kind, in);
	fclose(in);
	return r;
}

/* ---- bridges that work here, from the Tor Project (Connection Assist) ---- */

/*
 * Tor Browser's way to its bridge service: a meek tunnel through a CDN
 * (the bridgedb_targets preference), so a censor would have to block the
 * CDN. The circumvention API answers, for the country the request comes
 * from (or the one given), which transports work there: the Tor Project's
 * defaults of a kind, or bridges of its own.
 */
#define MOAT_SETTINGS  "https://bridges.torproject.org/moat/circumvention/settings"
/*
 * meek's argument, "targets=URL|FRONT+FRONT" as Tor Browser passes it, goes
 * in the SOCKS user name and password, which lyrebird joins: split after
 * "https:" and written percent-encoded in curl's proxy URL, so the colon
 * that separates them in the URL is not lost.
 */
#define MEEK_CREDS     "targets%3Dhttps%3A:%2F%2F1723079976.rsc.cdn77.org%7Ccdn.zk.mk%2Bwww.cdn77.com"

/* Copy JSON string text at p (after the opening quote) into out, unescaped. */
static const char *json_string(const char *p, char *out, size_t len)
{
	size_t n = 0;

	while (*p && *p != '"') {
		char c = *p++;

		if (c == '\\' && *p) {
			c = *p++;
			c = c == 'n' ? ' ' : c == 't' ? ' ' : c;    /* \" \\ \/ stay themselves */
		}
		if (n + 1 < len)
			out[n++] = c;
	}
	out[n] = '\0';
	return *p ? p + 1 : p;
}

/*
 * From the answer, the first setting: its transport type, and its bridge
 * lines if the service gave its own (else the defaults of that type are
 * used). Lines go to out, one per line. Returns the type, or NULL.
 */
static const char *moat_parse(const char *json, FILE *out, int *lines)
{
	static char type[32];
	const char *p = strstr(json, "\"settings\""), *strings, *end;
	char line[MAX_BRIDGE_LINE + 2];

	*lines = 0;
	if (!p || !(p = strstr(p, "\"type\"")) || !(p = strchr(p + 6, '"')))
		return NULL;
	json_string(p + 1, type, sizeof(type));
	if (strspn(type, "abcdefghijklmnopqrstuvwxyz0123456789_") != strlen(type) || !*type)
		return NULL;
	strings = strstr(p, "\"bridge_strings\"");
	end = strstr(p, "\"type\"");     /* the next setting, if any */
	if (strings && (!end || strings < end) && (strings = strchr(strings, '['))) {
		for (const char *q = strings + 1; *q && *q != ']'; q++) {
			if (*q != '"')
				continue;
			q = json_string(q + 1, line, sizeof(line)) - 1;
			if (valid_bridge(line)) {
				fprintf(out, "%s\n", line);
				(*lines)++;
			}
		}
	}
	return type;
}

/* Run argv as Tor's user; output to out (len bytes), status returned. */
static int run_as_tor(char *const argv[], char *const envp[], char *out, size_t len, pid_t *bg)
{
	long uid = anon_tor_uid();
	int p[2], status;
	size_t got = 0;
	ssize_t r;
	pid_t pid;

	if (uid < 0 || pipe2(p, O_CLOEXEC) < 0 || (pid = fork()) < 0)
		return -1;
	if (pid == 0) {
		dup2(p[1], STDOUT_FILENO);
		if (setgroups(0, NULL) < 0 || setgid((gid_t)uid) < 0 || setuid((uid_t)uid) < 0 ||
		    chdir(TOR_RUN) < 0)
			_exit(127);
		execve(argv[0], argv, envp);
		_exit(127);
	}
	close(p[1]);
	if (bg) {
		/* A daemon: read its first lines (up to "CMETHODS DONE"). */
		while (got < len - 1 && (r = read(p[0], out + got, len - 1 - got)) > 0) {
			got += (size_t)r;
			out[got] = '\0';
			if (strstr(out, "CMETHODS DONE") || strstr(out, "CMETHOD-ERROR"))
				break;
		}
		out[got] = '\0';
		*bg = pid;
		return 0;
	}
	while (got < len - 1 && (r = read(p[0], out + got, len - 1 - got)) > 0)
		got += (size_t)r;
	out[got] = '\0';
	while (read(p[0], (char[512]){ 0 }, 512) > 0)
		;
	close(p[0]);
	if (waitpid(pid, &status, 0) < 0 || !WIFEXITED(status))
		return -1;
	return WEXITSTATUS(status);
}

static int bridges_auto(const char *country)
{
	static char reply[1 << 16];
	char cmethods[1024], proxy[256], body[160], *port;
	char *pt_env[] = { "TOR_PT_MANAGED_TRANSPORT_VER=1", "TOR_PT_CLIENT_TRANSPORTS=meek_lite",
			   "TOR_PT_STATE_LOCATION=/run/tor/pt_state", "TOR_PT_EXIT_ON_STDIN_CLOSE=0",
			   "HOME=/run/tor", "PATH=/usr/bin", NULL };
	char *curl_env[] = { "PATH=/usr/bin", "HOME=/run/tor", NULL };
	const char *type;
	pid_t meek = 0;
	int lines = 0, r;
	FILE *tmp;

	if (country && (strlen(country) != 2 || strspn(country, "abcdefghijklmnopqrstuvwxyz") != 2))
		die("the country is two small letters, such as de or cn%s", "");
	if (access(LYREBIRD, X_OK) < 0)
		die("bridges need Tor Browser's transports; 'wh install tor-browser-bundle' first%s", "");
	/*
	 * meek bridges first: with them the firewall lets the transports look
	 * names up (encrypted DNS), and they are a working fallback.
	 */
	if (bridges_set("meek") != 0)
		return 1;
	if (fw_reload() != 0)
		warn("cannot reload the firewall rules%s", "");
	printf("Asking the Tor Project which bridges work here (through a meek tunnel)...\n");
	fflush(stdout);
	/* Tor's folder, as tor_start makes it (Tor may not have run yet). */
	if ((mkdir(TOR_RUN, 0750) < 0 && errno != EEXIST) ||
	    chown(TOR_RUN, (uid_t)anon_tor_uid(), (gid_t)anon_tor_uid()) < 0 ||
	    (mkdir(TOR_RUN "/pt_state", 0700) < 0 && errno != EEXIST) ||
	    chown(TOR_RUN "/pt_state", (uid_t)anon_tor_uid(), (gid_t)anon_tor_uid()) < 0)
		warn("cannot prepare the transport's folder%s", "");
	if (run_as_tor((char *[]){ LYREBIRD, NULL }, pt_env, cmethods, sizeof(cmethods), &meek) < 0 ||
	    !(port = strstr(cmethods, "CMETHOD meek_lite socks5 127.0.0.1:"))) {
		if (meek)
			kill(meek, SIGTERM);
		warn("the meek transport did not start; keeping the meek bridges%s", "");
		return 0;
	}
	port += strlen("CMETHOD meek_lite socks5 ");
	port[strcspn(port, "\r\n")] = '\0';
	snprintf(proxy, sizeof(proxy), "socks5h://%s@%s", MEEK_CREDS, port);
	if (country)
		snprintf(body, sizeof(body), "{\"country\":\"%s\",\"transports\":[\"obfs4\",\"snowflake\","
			 "\"webtunnel\",\"meek\"]}", country);
	else
		snprintf(body, sizeof(body), "{\"transports\":[\"obfs4\",\"snowflake\",\"webtunnel\",\"meek\"]}");
	r = run_as_tor((char *[]){ "/usr/bin/curl", "-sS", "--max-time", "90", "-x", proxy, "-X", "POST",
				   "-H", "Content-Type: application/vnd.api+json", "-d", body,
				   MOAT_SETTINGS, NULL }, curl_env, reply, sizeof(reply), NULL);
	kill(meek, SIGTERM);
	waitpid(meek, NULL, 0);
	if (r != 0 || !*reply) {
		warn("no answer from the Tor Project; keeping the meek bridges%s", "");
		return 0;
	}
	if (!(tmp = tmpfile()))
		die("cannot hold the bridges: %s", strerror(errno));
	type = moat_parse(reply, tmp, &lines);
	if (!type) {
		fclose(tmp);
		/* An empty answer: nothing is blocked from here. */
		if (strstr(reply, "\"settings\":[]") || strstr(reply, "\"settings\": []") ||
		    strstr(reply, "\"settings\":null")) {
			printf("Nothing seems blocked from here: Tor connects directly.\n");
			return bridges_off();
		}
		warn("an answer Tor Browser would not expect; keeping the meek bridges%s", "");
		return 0;
	}
	if (lines > 0) {
		rewind(tmp);
		r = bridges_write("auto", tmp);
		fclose(tmp);
		return r;
	}
	fclose(tmp);
	/* The Tor Project's defaults of the kind it named. */
	return bridges_set(!strcmp(type, "meek_lite") ? "meek" : type);
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
		else if (!strcmp(argv[1], "auto"))
			r = bridges_auto(NULL);
		else if ((r = bridges_set(argv[1])) != 0)
			return r;
		bridges_apply();
		return r;
	}
	if (argc == 3 && !strcmp(argv[0], "bridges") && !strcmp(argv[1], "auto")) {
		int r = bridges_auto(argv[2]);

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
	if (!strcmp(argv[0], "new-circuits"))
		return anon_new_circuits();
	if (!strcmp(argv[0], "start"))
		return anon_enabled() && !safe_mode() && access(ANON_HOLD, F_OK) < 0 ?
		       (tor_start() < 0) : 0;
	return 2;
}
