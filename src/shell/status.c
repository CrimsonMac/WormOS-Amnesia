/*
 * The state of the system as a normal user can see it: the snapshot
 * summary snap keeps in /run/snap-status, whether all traffic goes through
 * Tor (/etc/worm/anon) or the network is cut (/run/worm/network-cut), and
 * whether a network device is connected.
 */

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "shell.h"

static void read_snap_status(struct status *s)
{
	char line[512];
	FILE *f = fopen("/run/snap-status", "re");

	s->snapshots = -1;
	if (!f)
		return;
	while (fgets(line, sizeof(line), f)) {
		char *value = strstr(line, " = ");
		long long n;

		if (!value)
			continue;
		*value = '\0';
		value += 3;
		value[strcspn(value, "\n")] = '\0';
		if (!strcmp(line, "count") && sscanf(value, "%lld", &n) == 1)
			s->snapshots = (int)n;
		else if (!strcmp(line, "last") && sscanf(value, "%lld", &n) == 1)
			s->last_id = (int)n;
		else if (!strcmp(line, "last_date") && sscanf(value, "%lld", &n) == 1)
			s->last_date = n;
		else if (!strcmp(line, "last_comment"))
			g_strlcpy(s->last_comment, value, sizeof(s->last_comment));
		else if (!strcmp(line, "auto"))
			s->auto_snapshots = !strcmp(value, "on");
		else if (!strcmp(line, "pending"))
			g_strlcpy(s->pending, value, sizeof(s->pending));
	}
	fclose(f);
}

/* A hardware device (it has a "device" link) with a carrier. */
static gboolean device_online(const char *name)
{
	char path[256], carrier = '0';
	FILE *f;

	snprintf(path, sizeof(path), "/sys/class/net/%s/device", name);
	if (access(path, F_OK) != 0)
		return FALSE;
	snprintf(path, sizeof(path), "/sys/class/net/%s/carrier", name);
	if ((f = fopen(path, "re"))) {
		if (fread(&carrier, 1, 1, f) != 1)
			carrier = '0';
		fclose(f);
	}
	return carrier == '1';
}

static void read_network(struct status *s)
{
	struct ifaddrs *list;

	s->online = FALSE;
	s->address[0] = '\0';
	if (getifaddrs(&list) < 0)
		return;
	for (struct ifaddrs *i = list; i; i = i->ifa_next) {
		if (!(i->ifa_flags & IFF_UP) || (i->ifa_flags & IFF_LOOPBACK) ||
		    !device_online(i->ifa_name))
			continue;
		s->online = TRUE;
		if (i->ifa_addr && i->ifa_addr->sa_family == AF_INET && !s->address[0])
			inet_ntop(AF_INET, &((struct sockaddr_in *)i->ifa_addr)->sin_addr,
				  s->address, sizeof(s->address));
	}
	freeifaddrs(list);
}

/*
 * How far Tor has connected, from its log (readable by the group tor, which
 * WormOS Amnesia's live user is in); -1 for everyone else.
 */
static int read_tor_percent(void)
{
	char line[512], *p;
	int percent = -1;
	FILE *f = fopen("/run/tor/notices.log", "re");

	if (!f)
		return -1;
	while (fgets(line, sizeof(line), f))
		if ((p = strstr(line, "Bootstrapped ")))
			percent = atoi(p + 13);
	fclose(f);
	return percent < 0 ? 0 : percent;
}

void status_read(struct status *s)
{
	memset(s, 0, sizeof(*s));
	read_snap_status(s);
	s->anon = access("/etc/worm/anon", F_OK) == 0;
	s->network_cut = access("/run/worm/network-cut", F_OK) == 0;
	s->tor_percent = s->anon ? read_tor_percent() : -1;
	read_network(s);
}

char *relative_time(gint64 unix_time)
{
	gint64 age = g_get_real_time() / G_USEC_PER_SEC - unix_time;

	if (age < 60)
		return g_strdup("just now");
	if (age < 3600)
		return g_strdup_printf("%d minute%s ago", (int)(age / 60), age < 120 ? "" : "s");
	if (age < 86400)
		return g_strdup_printf("%d hour%s ago", (int)(age / 3600), age < 7200 ? "" : "s");
	return g_strdup_printf("%d day%s ago", (int)(age / 86400), age < 172800 ? "" : "s");
}
