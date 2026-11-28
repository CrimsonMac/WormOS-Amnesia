/* Checking a user's password against /etc/shadow (needs root). */

#ifndef PASSWORD_H
#define PASSWORD_H

/*
 * Does password match the user's? An empty field (development images)
 * takes an empty password; a locked one ("!", "*") or a missing user
 * takes none. Waits after a wrong one: two seconds, five after five
 * wrong ones in a row, fifteen after ten, thirty after twenty (counted per
 * user in /run/worm/auth, root's alone). Checks of the same user's password
 * run one after another, waits included, even when started at once.
 */
int password_ok(const char *user, const char *password);

#endif
