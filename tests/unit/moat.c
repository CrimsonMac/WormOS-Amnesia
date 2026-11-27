/*
 * moat_parse() (src/worm/anon.c): the answers of the Tor Project's
 * circumvention service, as worm anon bridges auto reads them.
 */

#define TOR_LOG "/nonexistent"

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
int variant_is(const char *id) { (void)id; return 0; }

static const char *parse(const char *json, char *lines, size_t len, int *n)
{
	FILE *f = tmpfile();
	const char *type = moat_parse(json, f, n);
	size_t got;

	rewind(f);
	got = fread(lines, 1, len - 1, f);
	lines[got] = '\0';
	fclose(f);
	return type;
}

int main(void)
{
	char lines[2048];
	const char *type;
	int n;

	type = parse("{\"settings\":[{\"bridges\":{\"type\":\"snowflake\",\"source\":\"builtin\"}}],"
		     "\"country\":\"cn\"}", lines, sizeof(lines), &n);
	CHECK(type && !strcmp(type, "snowflake") && n == 0);

	type = parse("{\"settings\":[{\"bridges\":{\"type\":\"obfs4\",\"source\":\"bridgedb\","
		     "\"bridge_strings\":[\"obfs4 192.0.2.1:443 0123456789ABCDEF0123456789ABCDEF01234567 "
		     "cert=ab\\/cd+ef iat-mode=0\",\"obfs4 192.0.2.2:80 1123456789ABCDEF0123456789ABCDEF01234567 "
		     "cert=xyz iat-mode=0\"]}},{\"bridges\":{\"type\":\"snowflake\",\"source\":\"builtin\"}}]}",
		     lines, sizeof(lines), &n);
	CHECK(type && !strcmp(type, "obfs4"));
	CHECK(n == 2);
	CHECK(strstr(lines, "cert=ab/cd+ef iat-mode=0\n") != NULL);
	CHECK(strstr(lines, "192.0.2.2:80") != NULL);

	type = parse("{\"settings\":[]}", lines, sizeof(lines), &n);
	CHECK(type == NULL && n == 0);
	type = parse("{\"errors\":[{\"code\":406,\"detail\":\"bad\"}]}", lines, sizeof(lines), &n);
	CHECK(type == NULL);
	/* A type that could not name a transport is refused. */
	type = parse("{\"settings\":[{\"bridges\":{\"type\":\"../x\"}}]}", lines, sizeof(lines), &n);
	CHECK(type == NULL);
	return unit_report("moat_parse");
}
