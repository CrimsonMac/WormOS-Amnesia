/* Shared declarations for wh, the WormOS package manager. */

#ifndef WH_H
#define WH_H

#include <stddef.h>
#include <stdio.h>
#include <time.h>

/* Paths, relative to the root being managed. */
#define WH_DB_DIR       "usr/lib/wh/db"
#define WH_DECLINED     "usr/lib/wh/declined"
/* Editions may ship these (WormOS Amnesia): changes go straight into the
 * running system, and users may install through WH_USER_HELPER. */
#define WH_LIVE_FLAG    "/etc/wh/live"
#define WH_USERS_FLAG   "/etc/wh/users-may-install"
#define WH_USER_HELPER  "/usr/lib/wh/user"
#define WH_KEY          "usr/share/wh/keys/core.pub"
#define WH_REPO_CONF    "etc/wh/repo"
#define WH_STATE_DIR    "var/lib/wh"
#define WH_CACHE_DIR    "var/cache/wh"

#define NAME_MAX_LEN    64
#define VERSION_MAX_LEN 64
#define DESC_MAX_LEN    256
#define DEPS_MAX_LEN    512

/* A package as listed in the index or recorded in the database. */
struct package {
	char name[NAME_MAX_LEN];
	char version[VERSION_MAX_LEN];  /* VERSION-RELEASE */
	char description[DESC_MAX_LEN];
	char depends[DEPS_MAX_LEN];     /* space separated */
	char recommends[DEPS_MAX_LEN];  /* installed along unless declined */
	char file[NAME_MAX_LEN + VERSION_MAX_LEN + 16];
	char sha256[65];
	long long size;
	long long installed_size;
	/* Only the files changed since version delta_from (empty: none). */
	char delta_from[VERSION_MAX_LEN];
	char delta_file[NAME_MAX_LEN + 2 * VERSION_MAX_LEN + 24];
	char delta_sha256[65];
	long long delta_size;
};

struct index {
	struct package *pkgs;
	int count;
	long long date;
};

/* Options shared by all modules. */
extern const char *root;        /* "/" or --root DIR */
extern int root_fd;             /* open directory of the root */
extern int verbose;

/* util.c */
void die(const char *fmt, ...) __attribute__((noreturn, format(printf, 1, 2)));
void warn(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void info(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void root_path(char *out, size_t len, const char *rel);
int mkdirs_at(int dfd, const char *rel, int mode);
int open_in_root(const char *rel, int flags);
int run(char *const argv[], char *out, size_t outlen);
char *trim(char *s);
int word_in(const char *list, const char *word);

/* repo.c */
int index_load(struct index *idx, const char *path);
void index_free(struct index *idx);
struct package *index_find(struct index *idx, const char *name);
int repo_update(const char *url);
int repo_load(struct index *idx);
char **repo_fetch_all(const char *url, struct package **pkgs, int n);
void repo_remove_downloads(char **files);
const char *repo_url(const char *override);
int download(const char *url, const char *dest);
int download_many(char *const *urls, char *const *dests, int n);
int file_sha256(const char *path, char hex[65], long long *size);

/* foreign.c */
int foreign_source_known(const char *name);
const char *foreign_find(const char *pkg);
int foreign_installed(const char *pkg, char *source, size_t len);
int foreign_install(const char *source, const char *pkg);
int foreign_remove(const char *pkg);
int foreign_refresh(int upgrade);
void foreign_list(void);
int foreign_search(const char *term, const char *only);
int wormhole_exists(const char *name);

/* db.c */
int db_installed(const char *name, struct package *p);
int db_list(struct package **out);
int db_write(const char *name, const char *pkginfo, FILE *files);
int db_dir_used(const char *path, const char *except);
int db_remove(const char *name);
char *db_owner(const char *path, const char *except);   /* caller frees */
int db_files(const char *name, char ***paths, int *count);
int db_disown(const char *name, const char *path);
int db_adopt(const char *name, char type, const char *path);
int db_declined(const char *name);
void db_decline(const char *name, int declined);
void db_free_files(char **paths, int count);

/* hooks.c */
void hook_touch(const char *path);
void hooks_run(void);

/* pkg.c */
int pkg_install(const char *wpkg, const char *expect_name);
int pkg_delta_usable(const char *wdelta, const char *expect_name);
int pkg_remove(const char *name);
int parse_pkginfo(const char *text, struct package *p);
int tar_unpack(char *const argv[]);

#endif
