/*
 * worm time - the clock and the time zone.
 *
 *     worm time                  the time, the time zone, the last check
 *     worm time zones [TEXT]     time zones (those containing TEXT)
 *     worm time zone NAME        set the time zone, e.g. Europe/Berlin (root)
 *     worm time sync             set the clock from the network now (root)
 *
 * The time zone is the link /etc/localtime into /usr/share/zoneinfo, so it
 * is part of system snapshots; without it the system runs on UTC.
 *
 * The clock is checked over HTTPS rather than NTP: the Date header of a
 * TLS-authenticated answer cannot be forged on the way (plain NTP can), and
 * HTTPS also works in anonymous mode, through Tor. It is accurate to about
 * a second, which is what certificates, Tor and logs need. The service
 * 35-time runs a check after the network comes up and every six hours;
 * the result is kept in /run/wormos/time (readable by everyone).
 *
 * In anonymous mode HTTPS needs Tor, and Tor needs a clock within about
 * a day: a computer whose clock battery is empty starts years off, and
 * Tor never connects. While Tor is not
 * connected, the check uses the skew Tor's relays report over their
 * authenticated connections (in Tor's log), sets the clock by it and
 * restarts Tor; the HTTPS check follows once Tor is up.
 *
 * Live systems (the ISO, WormOS Amnesia) never write the computer's
 * hardware clock: they change nothing on the machine they run on.
 */

#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#include "worm.h"

#define ZONEINFO   "/usr/share/zoneinfo"
#define LOCALTIME  "/etc/localtime"
#define TIME_STATE "/run/wormos/time"

/* Servers asked for the time, in order; the first answer wins. */
static const char *const servers[] = {
	"https://www.quad9.net/", "https://www.torproject.org/", "https://www.debian.org/",
};

static int valid_zone(const char *z)
{
	char path[PATH_MAX];
	struct stat st;

	if (!*z || *z == '/' || strstr(z, "..") || strlen(z) > 64)
		return 0;
	for (const char *p = z; *p; p++)
		if (!isalnum((unsigned char)*p) && !strchr("/_+-", *p))
			return 0;
	snprintf(path, sizeof(path), "%s/%s", ZONEINFO, z);
	return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static const char *current_zone(void)
{
	static char buf[PATH_MAX];
	ssize_t n = readlink(LOCALTIME, buf, sizeof(buf) - 1);
	const char *p;

	if (n <= 0)
		return "UTC";
	buf[n] = '\0';
	p = strstr(buf, "zoneinfo/");
	return p ? p + 9 : buf;
}

static int cmd_show(void)
{
	char now[64], state[256] = "";
	time_t t = time(NULL);
	FILE *f;

	strftime(now, sizeof(now), "%Y-%m-%d %H:%M:%S %Z", localtime_r(&t, &(struct tm){ 0 }));
	printf("Time:      %s\nTime zone: %s\n", now, current_zone());
	if ((f = fopen(TIME_STATE, "re"))) {
		if (fgets(state, sizeof(state), f))
			state[strcspn(state, "\n")] = '\0';
		fclose(f);
	}
	printf("Clock:     %s\n", state[0] ? state : "not checked since the system started");
	return 0;
}

static int cmd_zones(const char *filter)
{
	char line[512];
	FILE *f = fopen(ZONEINFO "/zone1970.tab", "re");
	int n = 0;

	if (!f)
		die("cannot read %s (is tzdata installed?)", ZONEINFO "/zone1970.tab");
	if (!filter || strcasestr("UTC", filter))
		puts("UTC"), n++;
	while (fgets(line, sizeof(line), f)) {
		char cc[64], coords[64], zone[128];

		if (line[0] == '#' || sscanf(line, "%63s %63s %127s", cc, coords, zone) != 3)
			continue;
		if (!filter || strcasestr(zone, filter))
			puts(zone), n++;
	}
	fclose(f);
	if (!n)
		printf("No time zone contains '%s'.\n", filter);
	return n ? 0 : 1;
}

static int cmd_zone(const char *zone)
{
	char target[PATH_MAX];
	int relock;

	if (!valid_zone(zone))
		die("unknown time zone '%s'; worm time zones lists them", zone);
	snprintf(target, sizeof(target), "%s/%s", ZONEINFO, zone);
	relock = root_begin_change();
	unlink(LOCALTIME ".new");
	if (strcmp(zone, "UTC") ? symlink(target, LOCALTIME ".new") < 0 ||
				  rename(LOCALTIME ".new", LOCALTIME) < 0
				: unlink(LOCALTIME) < 0 && errno != ENOENT) {
		int err = errno;

		root_end_change(relock);
		die("cannot set the time zone: %s", strerror(err));
	}
	root_end_change(relock);
	printf("Time zone: %s. Programs started from now on use it; log out and in for the "
	       "desktop's clock.\n", zone);
	return 0;
}

/* The Date header from an HTTPS answer, as a time; -1 if none. */
static time_t server_time(const char *url)
{
	char out[8192], *p;
	struct tm tm = { 0 };

	if (run((char *[]){ "curl", "-sSI", "--max-time", "15", "--proto", "=https",
			    "--tlsv1.2", (char *)url, NULL }, out, sizeof(out), 1) != 0)
		return -1;
	for (p = out; p && *p; p = strchr(p, '\n') ? strchr(p, '\n') + 1 : NULL)
		if (!strncasecmp(p, "date:", 5)) {
			p += 5;
			while (*p == ' ')
				p++;
			if (strptime(p, "%a, %d %b %Y %H:%M:%S GMT", &tm))
				return timegm(&tm);
		}
	return -1;
}

/* Note the result of a check, with the time it was made. */
static void save_state(const char *result)
{
	char now[64];
	time_t t = time(NULL);
	FILE *f;

	strftime(now, sizeof(now), "%Y-%m-%d %H:%M", localtime_r(&t, &(struct tm){ 0 }));
	/* Corrections also go to the system log (logread). */
	if (!strncmp(result, "corrected", 9))
		syslog(LOG_NOTICE, "worm time: clock %s", result);
	mkdir("/run/wormos", 0755);
	if ((f = fopen(TIME_STATE ".new", "we"))) {
		fprintf(f, "%s at %s\n", result, now);
		fclose(f);
		chmod(TIME_STATE ".new", 0644);
		rename(TIME_STATE ".new", TIME_STATE);
	}
	printf("%s at %s\n", result, now);
}

static int set_clock(long diff)
{
	struct timespec ts = { .tv_sec = time(NULL) + diff };

	if (clock_settime(CLOCK_REALTIME, &ts) < 0)
		die("cannot set the clock: %s", strerror(errno));
	/* The hardware clock keeps UTC for the next start, except on live systems. */
	if (access("/run/wormos/live", F_OK) < 0)
		run((char *[]){ "hwclock", "--systohc", "--utc", NULL }, NULL, 0, 1);
	return 0;
}

static int cmd_sync(void)
{
	char msg[160];
	long skew;

	if (fw_network_cut())
		die("the network is cut; 'worm panic off' restores it first%s", "");
	if (anon_enabled() && !anon_connected() && anon_tor_skew(&skew) == 0 &&
	    labs(skew) >= 60 && labs(skew) <= 40L * 366 * 86400) {
		set_clock(skew);
		snprintf(msg, sizeof(msg), "corrected by %+ld s (reported by Tor's relays)", skew);
		save_state(msg);
		if (anon_restart_tor() < 0)
			warn("Tor did not start again%s", "");
		/* Not checked over HTTPS yet: the next check follows soon. */
		return 1;
	}
	for (size_t i = 0; i < sizeof(servers) / sizeof(servers[0]); i++) {
		time_t before = time(NULL), theirs = server_time(servers[i]), after;
		long diff;

		if (theirs < 0)
			continue;
		after = time(NULL);
		/* The header has whole seconds: compare with the middle of the request. */
		diff = (long)(theirs - (before + (after - before) / 2));
		if (labs(diff) > 2) {
			set_clock(diff);
			snprintf(msg, sizeof(msg), "corrected by %+ld s (%s)", diff, servers[i]);
		} else {
			snprintf(msg, sizeof(msg), "right, within %ld s (%s)", labs(diff), servers[i]);
		}
		save_state(msg);
		return 0;
	}
	save_state("not checked: no server answered");
	return 1;
}

int cmd_time(int argc, char **argv)
{
	if (argc == 0)
		return cmd_show();
	if (!strcmp(argv[0], "zones") && argc <= 2)
		return cmd_zones(argc == 2 ? argv[1] : NULL);
	if (geteuid() != 0 && (!strcmp(argv[0], "zone") || !strcmp(argv[0], "sync")))
		die("this needs root, try again with su%s", "");
	if (!strcmp(argv[0], "zone") && argc == 2)
		return cmd_zone(argv[1]);
	if (!strcmp(argv[0], "sync") && argc == 1)
		return cmd_sync();
	return 2;
}
