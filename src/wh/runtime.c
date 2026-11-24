/*
 * The app runtime of wormholes: what every program in a wormhole needs,
 * whatever it is, so that programs work without fixes of their own.
 *
 *   - Companions (runtime rules, /usr/share/wh/runtime). Wormholes leave
 *     out recommended packages, and some of those decide whether a
 *     program works: without librsvg2-common a GTK program has no icons,
 *     without qt6-wayland a Qt program cannot use Wayland. A rule names a
 *     package pattern and its companions; after every install and upgrade
 *     wh adds the companions of every package present.
 *   - Stand-ins for programs that reach out of the wormhole (sudo,
 *     pkexec, xdg-open, x-terminal-emulator and the browser commands), in
 *     the wormhole's /usr/local/bin. They pass the request to the desktop
 *     session's bridge (/usr/lib/wh/bridge): open a link or a file with
 *     the desktop's programs, open a terminal running a command of the
 *     wormhole, or run one as root in the root terminal.
 *   - Documentation folders are kept: some programs keep the scripts they
 *     run there (impacket's examples). Wormholes set up before 1.13 left
 *     them out; their packages that lost such files are installed again.
 *
 * Everything here runs inside the wormhole (whc --root), so links in it
 * resolve inside and a package cannot make wh write on the host.
 */

#define _GNU_SOURCE
#include <fnmatch.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wh.h"

#define RUNTIME_RULES "/usr/share/wh/runtime"

/* The dpkg settings of a wormhole (etc/dpkg/dpkg.cfg.d/90wormhole). */
const char wormhole_dpkg_cfg[] =
	"# Kept small: no manual pages or translations. Documentation stays:\n"
	"# some programs run scripts kept there (impacket's examples).\n"
	"path-exclude=/usr/share/man/*\n"
	"path-exclude=/usr/share/info/*\n"
	"path-exclude=/usr/share/locale/*\n"
	"path-include=/usr/share/locale/locale.alias\n"
	"# A container that is rebuilt rather than repaired: dpkg need not\n"
	"# sync every file it writes.\n"
	"force-unsafe-io\n";

/*
 * The stand-ins: one script, linked under each name. Requests go to the
 * bridge FIFO as NUL-separated fields (verb, wormhole, folder, arguments)
 * ending with an empty field; timeout keeps a program from hanging when
 * no desktop session reads them.
 */
static const char bridge_script[] =
	"#!/bin/sh\n"
	"# Written by wh (WormOS). Programs in a wormhole reach the desktop\n"
	"# through the session's bridge: links and files open with the desktop's\n"
	"# programs, terminals open on the desktop, and commands that need root\n"
	"# run in the root terminal (wormhole programs run as the user).\n"
	"name=${0##*/}\n"
	"bridge=${WORMHOLE_BRIDGE:-${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/wormhole-bridge}\n"
	"# request VERB ARGS...: the wormhole and folder go first.\n"
	"request() {\n"
	"\tverb=$1; shift\n"
	"\t[ -p \"$bridge\" ] && [ -w \"$bridge\" ] || return 1\n"
	"\ttimeout 5 sh -c 'f=$1; shift; { printf \"%s\\0\" \"$@\"; printf \"\\0\"; } > \"$f\"' \\\n"
	"\t\tsh \"$bridge\" \"$verb\" \"${WORMHOLE:-}\" \"$PWD\" \"$@\" 2>/dev/null\n"
	"}\n"
	"real() {\n"
	"\tfor d in /usr/bin /bin /usr/sbin; do\n"
	"\t\t[ -x \"$d/$name\" ] && exec \"$d/$name\" \"$@\"\n"
	"\tdone\n"
	"\treturn 1\n"
	"}\n"
	"case $name in\n"
	"sudo|pkexec)\n"
	"\tuser=\n"
	"\twhile [ $# -gt 0 ]; do\n"
	"\t\tcase $1 in\n"
	"\t\t-u|--user) user=$2; shift 2 ;;\n"
	"\t\t-u*) user=${1#-u}; shift ;;\n"
	"\t\t--user=*) user=${1#--user=}; shift ;;\n"
	"\t\t--) shift; break ;;\n"
	"\t\t-*) shift ;;\n"
	"\t\t*) break ;;\n"
	"\t\tesac\n"
	"\tdone\n"
	"\tif [ \"$(id -u)\" = 0 ]; then\n"
	"\t\t[ $# -gt 0 ] || set -- \"${SHELL:-/bin/sh}\"\n"
	"\t\tif [ -z \"$user\" ] || [ \"$user\" = root ]; then exec \"$@\"; fi\n"
	"\t\texec runuser -u \"$user\" -- \"$@\"\n"
	"\tfi\n"
	"\tif request root \"$@\"; then\n"
	"\t\techo \"$name: ${1:-a shell} needs root: it opens in the root terminal, which asks for the root password.\" >&2\n"
	"\telse\n"
	"\t\techo \"$name: ${1:-a shell} needs root. Programs in a wormhole run as you: run it from the root terminal.\" >&2\n"
	"\tfi\n"
	"\texit 1 ;;\n"
	"xdg-open|sensible-browser|x-www-browser|www-browser|gnome-www-browser)\n"
	"\t[ $# -gt 0 ] || { echo \"usage: $name FILE|URL\" >&2; exit 1; }\n"
	"\tfor target; do request open \"$target\" || real \"$@\" ||\n"
	"\t\t{ echo \"$name: no desktop session to open $target\" >&2; exit 1; }; done ;;\n"
	"x-terminal-emulator)\n"
	"\t# -e COMMAND [ARGS], as Debian's terminals take it (also -x, --).\n"
	"\twhile [ $# -gt 0 ]; do\n"
	"\t\tcase $1 in\n"
	"\t\t-e|-x|--command|--) shift; break ;;\n"
	"\t\t-T|--title|-t|--working-directory) shift 2 ;;\n"
	"\t\t-*) shift ;;\n"
	"\t\t*) break ;;\n"
	"\t\tesac\n"
	"\tdone\n"
	"\t# One -e argument holding a whole command line.\n"
	"\t[ $# -eq 1 ] && case $1 in *' '*) set -- sh -c \"$1\" ;; esac\n"
	"\trequest terminal \"$@\" || real \"$@\" ||\n"
	"\t\t{ echo \"$name: no desktop session to open a terminal\" >&2; exit 1; } ;;\n"
	"esac\n";

/* Names the stand-in answers to, in the wormhole's /usr/local/bin. */
static const char bridge_names[] =
	"sudo pkexec xdg-open sensible-browser x-www-browser www-browser "
	"gnome-www-browser x-terminal-emulator";

/*
 * Restore the files that wormholes from before 1.13 left out of the
 * documentation folders, where programs need them: scripts and examples.
 * Packages missing one are installed again (from apt's cache when it
 * still has them).
 */
static const char restore_docs[] =
	"cd /var/lib/dpkg/info || exit 0\n"
	"pkgs=$(for l in *.list; do\n"
	"  grep -E '^/usr/share/doc/[^/]+/(.*/)?(examples?/[^/]+|[^/]+\\.(py|pl|rb|sh|php|lua|jar|js|tcl))$' \"$l\" |\n"
	"  while IFS= read -r f; do [ -e \"$f\" ] || [ -L \"$f\" ] || { echo \"${l%.list}\"; break; }; done\n"
	"done | sort -u)\n"
	"[ -n \"$pkgs\" ] || exit 0\n"
	"echo \"Restoring files left out of: $(echo $pkgs)\"\n"
	"apt-get -q -y install --reinstall $pkgs >/dev/null 2>&1 ||\n"
	"  for p in $pkgs; do apt-get -q -y install --reinstall \"$p\" >/dev/null 2>&1; done\n"
	"true\n";

/*
 * Put the runtime in place: the dpkg settings, the stand-ins and, once
 * for wormholes from before 1.13, the documentation files. Cheap when
 * everything is there.
 */
void wormhole_runtime(const char *source)
{
	char script[8192];

	snprintf(script, sizeof(script),
		 "cfg=/etc/dpkg/dpkg.cfg.d/90wormhole\n"
		 "old=; grep -q '^path-exclude=/usr/share/doc' \"$cfg\" 2>/dev/null && old=1\n"
		 "printf '%%s' \"$1\" > \"$cfg.new\" && mv \"$cfg.new\" \"$cfg\"\n"
		 "mkdir -p /usr/local/lib/wormhole /usr/local/bin\n"
		 "printf '%%s' \"$2\" > /usr/local/lib/wormhole/bridge.new &&\n"
		 "  chmod 755 /usr/local/lib/wormhole/bridge.new &&\n"
		 "  mv /usr/local/lib/wormhole/bridge.new /usr/local/lib/wormhole/bridge\n"
		 "for n in %s; do\n"
		 "  [ \"$(readlink \"/usr/local/bin/$n\")\" = ../lib/wormhole/bridge ] ||\n"
		 "    ln -sfn ../lib/wormhole/bridge \"/usr/local/bin/$n\"\n"
		 "done\n"
		 "[ -z \"$old\" ] || sh -c \"$3\"\n", bridge_names);
	in_wormhole(source, (char *[]){ "sh", "-c", script, "sh", (char *)wormhole_dpkg_cfg,
					(char *)bridge_script, (char *)restore_docs, NULL },
		    NULL, 0);
}

/*
 * Add the companions the runtime rules name for the packages installed in
 * the wormhole. A companion the sources do not have is skipped.
 */
void runtime_complete(const char *source)
{
	static char status[1 << 20];
	char line[1024], want[4096] = "";
	size_t wlen = 0;
	FILE *rules;

	if (!(rules = fopen(RUNTIME_RULES, "re")))
		return;
	if (in_wormhole(source, (char *[]){ "dpkg-query", "-W", "-f",
					    "${Package} ${db:Status-Abbrev}\\n", NULL },
			status, sizeof(status)) != 0) {
		fclose(rules);
		return;
	}
	while (fgets(line, sizeof(line), rules)) {
		char *eq = strchr(line, '='), *pattern = line, *list, *save, *p;
		int matched = 0;

		if (line[0] == '#' || !eq)
			continue;
		*eq = '\0';
		pattern = trim(pattern);
		list = trim(eq + 1);
		/* Does an installed package match the pattern? */
		for (char *s = status; *s && !matched; ) {
			char name[256], state[8] = "";
			char *nl = strchr(s, '\n');

			if (sscanf(s, "%255s %7s", name, state) == 2 && !strncmp(state, "ii", 2) &&
			    fnmatch(pattern, name, 0) == 0)
				matched = 1;
			s = nl ? nl + 1 : s + strlen(s);
		}
		if (!matched)
			continue;
		for (p = strtok_r(list, " \t", &save); p; p = strtok_r(NULL, " \t", &save)) {
			char needle[300];

			/* Already installed, or already wanted? */
			snprintf(needle, sizeof(needle), "\n%s ii", p);
			if (!strncmp(status, needle + 1, strlen(needle) - 1) || strstr(status, needle) ||
			    word_in(want, p))
				continue;
			if (wlen + strlen(p) + 2 < sizeof(want))
				wlen += snprintf(want + wlen, sizeof(want) - wlen, "%s%s", wlen ? " " : "", p);
		}
	}
	fclose(rules);
	if (!wlen)
		return;
	info("Adding what programs in the %s wormhole need: %s", source, want);
	{
		char script[4600];

		/* All at once; if one is unknown to the sources, one by one. */
		snprintf(script, sizeof(script),
			 "apt-get -q -y install --no-install-recommends %s >/dev/null 2>&1 ||\n"
			 "for p in %s; do apt-get -q -y install --no-install-recommends \"$p\" "
			 ">/dev/null 2>&1 || echo \"wh: $p is not available in the wormhole\" >&2; done\n"
			 "true", want, want);
		in_wormhole(source, (char *[]){ "sh", "-c", script, NULL }, NULL, 0);
	}
}
