/*
 * x11-allow-root - let root's programs open windows on the desktop's
 * Xwayland. Xwayland accepts only the user who runs it (no cookie file),
 * so Java programs such as Ghidra, started from the root terminal, failed
 * with "Authorization required". This adds root to the server's access
 * list, as "xhost +si:localuser:root" would. Run by the user, before su.
 */
#include <stdint.h>
#include <stdlib.h>
#include <xcb/xcb.h>

int main(void)
{
	static const char addr[] = "localuser\0root";
	xcb_connection_t *c = xcb_connect(NULL, NULL);
	xcb_generic_error_t *e;

	if (xcb_connection_has_error(c))
		return 1;
	e = xcb_request_check(c, xcb_change_hosts_checked(c, XCB_HOST_MODE_INSERT,
			XCB_FAMILY_SERVER_INTERPRETED, sizeof(addr) - 1, (const uint8_t *)addr));
	xcb_disconnect(c);
	free(e);
	return e != NULL;
}
