/* gowl-wayland-socket.c -- Wayland socket liveness and cleanup.
 * See gowl-wayland-socket.h for the rationale. */

#include "util/gowl-wayland-socket.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

/* The socket this process is serving, and the pid that created it.
 * Both are needed: after a fork() the child inherits the atexit
 * registration and the path, and must not act on either. */
static gchar	*gowl_socket_path = NULL;
static pid_t	 gowl_socket_pid = 0;

/* ------------------------------------------------------------------ */

/**
 * gowl_wayland_socket_path:
 * @display: a display name or an absolute socket path
 *
 * Resolve @display the way libwayland does: an absolute path is used
 * as-is, anything else is relative to $XDG_RUNTIME_DIR.
 *
 * Returns: (transfer full) (nullable): the socket path, or %NULL when
 *   @display is unusable (empty, or relative with no runtime dir).
 */
static gchar *
gowl_wayland_socket_path(const gchar *display)
{
	const gchar	*runtime;

	if (display == NULL || display[0] == '\0')
		return NULL;

	if (g_path_is_absolute(display))
		return g_strdup(display);

	runtime = g_getenv("XDG_RUNTIME_DIR");
	if (runtime == NULL || runtime[0] == '\0')
		return NULL;

	return g_build_filename(runtime, display, NULL);
}

/* ------------------------------------------------------------------ */

gboolean
gowl_wayland_socket_live(const gchar *display)
{
	g_autofree gchar	*path = NULL;
	struct sockaddr_un	 addr;
	gint			 fd;
	gboolean		 live;

	path = gowl_wayland_socket_path(display);
	if (path == NULL)
		return FALSE;

	/* sun_path is a fixed 108 bytes and does not have to be
	 * NUL-terminated when full, so a path that does not fit cannot be
	 * connected to at all -- by us or by any client. */
	if (strlen(path) >= sizeof(addr.sun_path))
		return FALSE;

	fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return FALSE;

	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	memcpy(addr.sun_path, path, strlen(path));

	/* A stale file answers ECONNREFUSED, a missing one ENOENT, and a
	 * live compositor answers immediately -- AF_UNIX connect() does not
	 * block waiting for a peer to accept(). */
	live = connect(fd, (struct sockaddr *) &addr, sizeof(addr)) == 0;

	close(fd);
	return live;
}

/* ------------------------------------------------------------------ */

/* atexit() handler.  Runs in whatever process is exiting, which is not
 * necessarily the one that bound the socket. */
static void
gowl_wayland_socket_cleanup(void)
{
	if (gowl_socket_path == NULL)
		return;

	/* A forked child that exits through exit() -- rather than _exit()
	 * or exec() -- runs the handlers it inherited.  Its parent is still
	 * serving that socket, so removing the file here would break the
	 * live session instead of cleaning up after a dead one. */
	if (getpid() != gowl_socket_pid)
		return;

	unlink(gowl_socket_path);
}

void
gowl_wayland_socket_unlink_on_exit(const gchar *display)
{
	static gboolean	 registered = FALSE;
	gchar		*path;

	path = gowl_wayland_socket_path(display);
	if (path == NULL)
		return;

	g_free(gowl_socket_path);
	gowl_socket_path = path;
	gowl_socket_pid = getpid();

	if (!registered) {
		if (atexit(gowl_wayland_socket_cleanup) != 0) {
			g_debug("gowl: could not register socket cleanup; "
			        "%s may be left behind", gowl_socket_path);
			return;
		}
		registered = TRUE;
	}
}
