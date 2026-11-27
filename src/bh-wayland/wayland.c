/*
 * bh-wayland - the display for a graphical blackhole (bh open --gui).
 *
 *     /usr/lib/bh/wayland SOCKET NAME
 *
 * Asks the compositor, through wp-security-context-v1, for a listening
 * socket of its own at SOCKET: every client that connects there is marked
 * as sandboxed (engine "org.wormos.bh", the blackhole's NAME as app ID),
 * and labwc then offers it only the plain protocols a window needs. No
 * screen capture, no clipboard managers (ext/wlr data control), no list or
 * control of other windows, no virtual keyboard or pointer, no output
 * management. bh binds SOCKET into the blackhole as its Wayland display.
 *
 * It prints "ready" once the socket exists, then waits until its standard
 * input closes (bh exits, or the blackhole is closed): the compositor then
 * stops accepting new clients there, and the socket is removed.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <wayland-client.h>

#include "security-context-v1-client-protocol.h"

static struct wp_security_context_manager_v1 *manager;

static void global(void *data, struct wl_registry *registry, uint32_t name,
		   const char *interface, uint32_t version)
{
	(void)data;
	(void)version;
	if (!strcmp(interface, wp_security_context_manager_v1_interface.name))
		manager = wl_registry_bind(registry, name, &wp_security_context_manager_v1_interface, 1);
}

static void global_remove(void *data, struct wl_registry *registry, uint32_t name)
{
	(void)data;
	(void)registry;
	(void)name;
}

static const struct wl_registry_listener registry_listener = { global, global_remove };

__attribute__((noreturn)) static void fail(const char *what)
{
	fprintf(stderr, "bh: %s%s%s\n", what, errno ? ": " : "", errno ? strerror(errno) : "");
	exit(1);
}

int main(int argc, char **argv)
{
	struct sockaddr_un addr = { .sun_family = AF_UNIX };
	struct wp_security_context_v1 *context;
	struct wl_display *display;
	struct wl_registry *registry;
	int listen_fd, close_fds[2];
	char instance[32], buf[64];

	if (argc != 3 || strlen(argv[1]) >= sizeof(addr.sun_path)) {
		fputs("usage: bh-wayland SOCKET NAME\n", stderr);
		return 2;
	}
	errno = 0;
	if (!(display = wl_display_connect(NULL)))
		fail("no desktop to show windows on (bh open --gui needs one)");
	registry = wl_display_get_registry(display);
	wl_registry_add_listener(registry, &registry_listener, NULL);
	if (wl_display_roundtrip(display) < 0)
		fail("the compositor did not answer");
	errno = 0;
	if (!manager)
		fail("the compositor cannot sandbox windows (no security context)");

	snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", argv[1]);
	unlink(addr.sun_path);
	if ((listen_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0)) < 0 ||
	    bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
	    listen(listen_fd, 16) < 0)
		fail("cannot make the blackhole's display");
	/* Root of the blackhole is another user outside: it must connect. */
	if (chmod(addr.sun_path, 0666) < 0)
		fail("cannot open the blackhole's display to it");
	if (pipe2(close_fds, O_CLOEXEC) < 0)
		fail("pipe");

	snprintf(instance, sizeof(instance), "%ld", (long)getppid());
	context = wp_security_context_manager_v1_create_listener(manager, listen_fd, close_fds[0]);
	wp_security_context_v1_set_sandbox_engine(context, "org.wormos.bh");
	wp_security_context_v1_set_app_id(context, argv[2]);
	wp_security_context_v1_set_instance_id(context, instance);
	wp_security_context_v1_commit(context);
	wp_security_context_v1_destroy(context);
	if (wl_display_roundtrip(display) < 0)
		fail("the compositor refused the blackhole's display");
	/* The compositor holds its own copies now. */
	close(listen_fd);
	close(close_fds[0]);
	puts("ready");
	fflush(stdout);

	/* Until bh lets go: then the write end closes and the compositor stops. */
	while (read(STDIN_FILENO, buf, sizeof(buf)) > 0)
		;
	unlink(addr.sun_path);
	close(close_fds[1]);
	wl_display_disconnect(display);
	return 0;
}
