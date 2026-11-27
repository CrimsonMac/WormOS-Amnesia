/* Checks shared by the setuid helpers (power, snapshots, install). */

#define _GNU_SOURCE
#include <grp.h>
#include <pwd.h>
#include <string.h>
#include <unistd.h>

#include "admin.h"
#include "password.h"

int caller_is_admin(void)
{
	struct group *wheel;
	gid_t groups[256];
	int n;

	if (getuid() == 0)
		return 1;
	if (!(wheel = getgrnam("wheel")))
		return 0;
	if (getgid() == wheel->gr_gid)
		return 1;
	n = getgroups(sizeof(groups) / sizeof(groups[0]), groups);
	for (int i = 0; i < n; i++)
		if (groups[i] == wheel->gr_gid)
			return 1;
	return 0;
}

int root_password_ok(const char *password)
{
	return password_ok("root", password);
}

int admin_password_ok(const char *password)
{
	struct passwd *pw = getpwuid(getuid());

	return pw && password_ok(pw->pw_name, password);
}
