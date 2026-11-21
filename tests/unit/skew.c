/*
 * anon_tor_skew() (src/worm/anon.c): the clock offset read from Tor's
 * log, in every phrasing Tor uses, and nothing from lines that only look
 * similar.
 */

#define TOR_LOG unit_log
static const char *unit_log;

#include "../../src/worm/anon.c"

#include "unit.h"

/* What anon.c needs from the rest of worm. */
void die(const char *fmt, const char *arg) { (void)fmt; (void)arg; exit(99); }
void warn(const char *fmt, const char *arg) { (void)fmt; (void)arg; }
int run(char *const argv[], char *out, size_t len, int quiet) { (void)argv; (void)out; (void)len; (void)quiet; return -1; }
int safe_mode(void) { return 0; }
int fw_reload(void) { return 0; }
int root_begin_change(void) { return 0; }
void root_end_change(int relock) { (void)relock; }

static int skew_of(const char *text, long *seconds)
{
	static char path[] = "/tmp/wormos-unit-skew-XXXXXX";
	int fd = mkstemp(path), r;

	if (fd < 0 || write(fd, text, strlen(text)) < 0)
		return -2;
	close(fd);
	unit_log = path;
	r = anon_tor_skew(seconds);
	unlink(path);
	memcpy(path + strlen(path) - 6, "XXXXXX", 6);
	return r;
}

int main(void)
{
	long s = 0;

	CHECK(skew_of("[warn] Received NETINFO cell with skewed time (OR:1.2.3.4:443): It seems "
		      "that our clock is behind by 5 hours, 2 minutes, or that theirs is ahead.\n",
		      &s) == 0 && s == 5 * 3600 + 2 * 60);
	CHECK(skew_of("[warn] Our clock is 3 hours, 0 minutes behind the time published in the "
		      "consensus network status document (2026-11-21 12:00:00 UTC).\n", &s) == 0 &&
	      s == 3 * 3600);
	CHECK(skew_of("[warn] Received directory with skewed time (DIRSERV:x): It seems that our "
		      "clock is ahead by 1 days, 2 hours, 0 minutes, or that theirs is behind.\n",
		      &s) == 0 && s == -(86400 + 2 * 3600));
	CHECK(skew_of("[warn] It seems that our clock is ahead by 4 minutes, 30 seconds.\n", &s) == 0 &&
	      s == -(4 * 60 + 30));
	/* The last report wins. */
	CHECK(skew_of("x our clock is behind by 2 hours, 0 minutes\n"
		      "x our clock is ahead by 1 hours, 0 minutes\n", &s) == 0 && s == -3600);
	/* Nothing to go by. */
	CHECK(skew_of("[notice] Bootstrapped 100% (done): Done\n", &s) == -1);
	CHECK(skew_of("[warn] our clock is fine\n", &s) == -1);
	CHECK(skew_of("", &s) == -1);
	unit_log = "/nonexistent/tor.log";
	CHECK(anon_tor_skew(&s) == -1);
	return unit_report("anon_tor_skew");
}
