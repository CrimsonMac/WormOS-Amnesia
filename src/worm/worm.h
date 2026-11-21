#ifndef WORM_H
#define WORM_H

#include <stddef.h>

void die(const char *fmt, const char *arg) __attribute__((noreturn));

/*
 * Run a command and return its exit status (-1 if it could not run). With
 * out, its standard output is captured there; quiet discards its errors.
 */
int run(char *const argv[], char *out, size_t len, int quiet);

/*
 * Make / writable for a short change to system configuration. Returns
 * whether root_end_change must make it read-only again (it was locked).
 */
int root_begin_change(void);
void root_end_change(int relock);

void warn(const char *fmt, const char *arg);

/* Booted in safe mode (wormos.safe on the kernel command line). */
int safe_mode(void);
int variant_is(const char *id);
#define TORRC_D      "/etc/tor/torrc.d"
#define BRIDGES_CONF TORRC_D "/bridges.conf"
const char *anon_bridges(void);

int cmd_fw(int argc, char **argv);
int cmd_panic(int argc, char **argv);
int cmd_status(void);
int cmd_anon(int argc, char **argv);
int cmd_kit(int argc, char **argv);
int cmd_time(int argc, char **argv);
int cmd_passwd(void);
int cmd_passwd_apply(const char *user);

/* Anonymous mode (anon.c). */
int anon_enabled(void);
long anon_tor_uid(void);
const char *anon_summary(void);
int anon_connected(void);
int anon_tor_skew(long *seconds);
int anon_restart_tor(void);

/* The firewall (fw.c): cutting the network and bringing it back. */
int fw_cut(void);
int fw_reload(void);
int fw_restore(void);
int fw_network_cut(void);
const char *fw_summary(void);

/* Count blackholes of all users; with stop, kill the running ones. */
int blackholes_each(int stop, int *total);

#endif
