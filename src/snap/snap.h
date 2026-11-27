/* Shared declarations for snap, the WormOS snapshot tool. */

#ifndef SNAP_H
#define SNAP_H

#include <stddef.h>
#include <stdio.h>
#include <sys/types.h>
#include <time.h>

#include "sha256.h"

/* Subvolumes on the top level of the btrfs filesystem. */
#define SUBVOL_SYSTEM    "@"
#define SUBVOL_HOME      "@home"
#define SUBVOL_STORE     "@snapshots"
#define SUBVOL_NEXT      "@.next"         /* staged system change */
#define SUBVOL_WORMHOLES "@wormholes"     /* one subvolume per wormhole inside */

/* Inside each snapshot directory @snapshots/<id>/. */
#define SNAP_SYSTEM      "system"
#define SNAP_HOME        "home"
#define SNAP_INFO        "info"
#define SNAP_WORMHOLES   "wormholes"      /* the wormholes, with system snapshots */

/* Files at the top of the store. */
#define STORE_LOCK       ".lock"
#define STORE_CONFIG     "config"
#define STORE_PENDING    "pending"
#define STORE_ACTIVATE   "activate"
#define STORE_TRIAL      "trial"

#define SCOPE_SYSTEM     1
#define SCOPE_HOME       2
#define SCOPE_ALL        (SCOPE_SYSTEM | SCOPE_HOME)

#define COMMENT_MAX      256

struct snapshot {
	int id;
	time_t date;
	int scope;                      /* SCOPE_* bits present */
	char type[16];                  /* manual, auto */
	int pinned;
	int suspect;                    /* not a known-good state (safe mode skips it) */
	char comment[COMMENT_MAX];
	char sum[2][SHA256_HEX];        /* manifest checksums: system, home */
};

/* Sorted directory entries, from read_names(). */
struct names {
	char **v;
	int n;
};

/* Path of the mounted top-level subvolume. */
extern const char *top;
extern int top_given;          /* --top: not the running system */

/* util.c */
void die(const char *fmt, ...) __attribute__((noreturn, format(printf, 1, 2)));
void warn(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void path_join(char *out, size_t len, const char *a, const char *b);
const char *scope_name(int scope);
const char *scope_desc(int scope);
const char *scope_path(int scope);   /* "/" or "/home" */
const char *scope_subvol(int scope); /* @ or @home */
const char *scope_dir(int scope);    /* system or home */
int scope_index(int scope);          /* 0 for system, 1 for home */
int remove_tree_at(int dfd, const char *name);
void read_names(int dfd, struct names *out);
void free_names(struct names *n);

/* btrfs.c */
int subvol_snapshot(const char *src, const char *dstdir, const char *name,
		    int readonly);
int subvol_delete(const char *parent, const char *name);
int subvol_readonly(const char *path);

/* store.c */
void store_lock(void);
int store_list(struct snapshot **out);    /* sorted by id, caller frees */
int store_get(int id, struct snapshot *s);
int store_create(int scope, const char *type, const char *comment);
int store_delete(int id);
void store_cleanup(void);
void subvol_delete_old(const char *subvol);
/* Wormholes (each a subvolume in @wormholes) alongside system snapshots. */
int wormholes_snapshot(const char *dir);
void wormholes_delete(const char *dir);
int wormholes_restore(int id);
int store_write_info(const struct snapshot *s);
int store_mark_suspect(int id);
void store_entry_path(char *out, size_t len, int id, int scope);
int config_auto(void);
void config_set_auto(int on);
int pending_write(int id, int scope);
void pending_clear(void);
int pending_read(int *id, int *scope);
int store_verify(const struct snapshot *s, int scope);

/* manifest.c */
int manifest_create(const char *root, const char *out, const char *prev,
		    char sum[SHA256_HEX]);
int manifest_verify(const char *root, const char *manifest, const char *expected,
		    FILE *report);

/* diff.c */
int diff_trees(const char *a, const char *b, const char *prefix);

/* restore.c */
int restore_path(int id, const char *path);

/* menu.c */
void menu_update(void);

/* status.c */
void status_update(void);
void status_print(void);

/* stage.c */
int stage_begin(void);
int stage_commit(int trial, const char *comment);
int stage_keep(void);
void stage_discard(void);
int stage_pending(void);
int stage_trial(void);
int stage_boot_trial(void);
int stage_boot_activate(void);

/* boot.c */
int boot_tasks(int rewind_id);
int boot_rewind(int id, int scope);

#endif
