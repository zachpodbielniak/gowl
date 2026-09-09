/* gowl-wayland-socket.c -- Wayland socket liveness and cleanup.
 * See gowl-wayland-socket.h for the rationale. */

#include "util/gowl-wayland-socket.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

/* The socket this process is serving, identified by more than its name.
 * The pid catches a forked child running the inherited handler; the
 * device and inode catch the name having become somebody else's socket
 * since we registered -- which is exactly what happens if a compositor
 * is disposed (wl_display_destroy unlinks it, freeing the name) and the
 * process lives on to exit later. */
static gchar	*gowl_socket_path = NULL;
static pid_t	 gowl_socket_pid = 0;
static dev_t	 gowl_socket_dev = 0;
static ino_t	 gowl_socket_ino = 0;

/* What a probe found.  Deliberately not public: the header exposes two
 * predicates instead, so that "could not tell" cannot be mistaken for
 * either verdict by a caller that only checked one of them. */
typedef enum {
	GOWL_SOCKET_LIVE,	/* something accepted a connection */
	GOWL_SOCKET_ABSENT,	/* proven: nothing is there */
	GOWL_SOCKET_UNKNOWN	/* the probe could not reach a verdict */
} GowlSocketState;

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

/**
 * gowl_wayland_socket_probe:
 * @display: a display name or an absolute socket path
 *
 * Connect to @display and report what happened.
 *
 * Every step that can fail for a reason unrelated to whether a
 * compositor is running -- an unresolvable name, a path that does not
 * fit sockaddr_un, an exhausted fd table -- yields %GOWL_SOCKET_UNKNOWN
 * rather than being folded into "nothing is there".
 *
 * Returns: what the probe established.
 */
static GowlSocketState
gowl_wayland_socket_probe(const gchar *display)
{
	g_autofree gchar	*path = NULL;
	struct sockaddr_un	 addr;
	gint			 fd;
	gint			 err;

	path = gowl_wayland_socket_path(display);
	if (path == NULL)
		return GOWL_SOCKET_UNKNOWN;

	/* sun_path is a fixed 108 bytes, so a longer path cannot be
	 * connected to -- but neither can we say what is at the other end
	 * of a name we cannot address. */
	if (strlen(path) >= sizeof(addr.sun_path))
		return GOWL_SOCKET_UNKNOWN;

	fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return GOWL_SOCKET_UNKNOWN;

	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	memcpy(addr.sun_path, path, strlen(path));

	/* AF_UNIX connect() does not block waiting for a peer to accept(),
	 * so a live compositor answers immediately. */
	if (connect(fd, (struct sockaddr *) &addr, sizeof(addr)) == 0) {
		close(fd);
		return GOWL_SOCKET_LIVE;
	}

	err = errno;
	close(fd);

	switch (err) {
	case ENOENT:		/* no such path */
	case ENOTDIR:		/* a component of it is not a directory */
	case ECONNREFUSED:	/* the file is there; nobody is listening */
		return GOWL_SOCKET_ABSENT;
	default:
		/* EACCES, EPERM, EMFILE, ENFILE, ENOMEM, ECONNABORTED,
		 * EAGAIN on a full backlog: all say something about us or
		 * about the kernel, and nothing about whether a compositor
		 * is running. */
		return GOWL_SOCKET_UNKNOWN;
	}
}

/* ------------------------------------------------------------------ */

gboolean
gowl_wayland_socket_live(const gchar *display)
{
	return gowl_wayland_socket_probe(display) == GOWL_SOCKET_LIVE;
}

gboolean
gowl_wayland_socket_absent(const gchar *display)
{
	return gowl_wayland_socket_probe(display) == GOWL_SOCKET_ABSENT;
}

/* ------------------------------------------------------------------ */

/**
 * gowl_x_socket_path:
 * @display: the contents of $DISPLAY
 *
 * The local X11 socket a `:N[.S]' display name refers to.
 *
 * Only that form is resolved.  Anything else -- a hostname, an
 * abstract or explicitly-pathed display -- is somebody's deliberate
 * choice, and guessing at it would mean unsetting a display the user
 * meant to set.
 *
 * Returns: (transfer full) (nullable): the socket path, or %NULL.
 */
static gchar *
gowl_x_socket_path(const gchar *display)
{
	const gchar	*p;

	if (display == NULL || display[0] != ':' || display[1] == '\0')
		return NULL;

	for (p = display + 1; *p != '\0' && *p != '.'; p++)
		if (*p < '0' || *p > '9')
			return NULL;

	return g_strdup_printf("/tmp/.X11-unix/X%.*s",
	                       (int) (p - (display + 1)), display + 1);
}

gboolean
gowl_wayland_detect_parent_session(void)
{
	const gchar	*wl_socket;
	const gchar	*wl_display;
	const gchar	*x_display;
	gint		 n;

	/* A parent that handed us an already-connected fd is unambiguous,
	 * and nothing about it can have gone stale. */
	wl_socket = g_getenv("WAYLAND_SOCKET");
	if (wl_socket != NULL && wl_socket[0] != '\0')
		return TRUE;

	wl_display = g_getenv("WAYLAND_DISPLAY");
	if (wl_display != NULL && wl_display[0] != '\0') {
		switch (gowl_wayland_socket_probe(wl_display)) {
		case GOWL_SOCKET_LIVE:
			return TRUE;
		case GOWL_SOCKET_UNKNOWN:
			/* Keep it.  Being wrong this way costs a nested
			 * compositor nothing; being wrong the other way
			 * stops the host session's graphical-session.target
			 * on the way out. */
			g_debug("gowl: could not probe WAYLAND_DISPLAY=%s; "
			        "assuming a parent compositor", wl_display);
			return TRUE;
		case GOWL_SOCKET_ABSENT:
			g_message("gowl: WAYLAND_DISPLAY=%s has no compositor "
			          "listening; ignoring it", wl_display);
			g_unsetenv("WAYLAND_DISPLAY");
			break;
		}
	}

	/* Some terminals do not propagate WAYLAND_DISPLAY, so its absence
	 * is not proof of a seat.  Only a socket that answers counts here:
	 * this is the scan that used to believe a leftover file. */
	for (n = 0; n <= 3; n++) {
		gchar	name[32];

		g_snprintf(name, sizeof name, "wayland-%d", n);
		if (gowl_wayland_socket_probe(name) == GOWL_SOCKET_LIVE) {
			g_setenv("WAYLAND_DISPLAY", name, FALSE);
			return TRUE;
		}
	}

	/*
	 * The same trap one protocol over, with the safe direction
	 * reversed: a live $DISPLAY means somebody else owns this session,
	 * so keeping it can only make us more cautious.  Only a display
	 * proven dead is cleared, and only in the plain `:N' form -- with
	 * no Wayland parent, wlr_backend_autocreate would otherwise pick
	 * the X11 backend on the strength of this variable alone, and
	 * gowl_systemd_should_manage_session() would read it as a host
	 * session that is no longer there.
	 */
	x_display = g_getenv("DISPLAY");
	if (x_display != NULL && x_display[0] != '\0') {
		g_autofree gchar *x_path = gowl_x_socket_path(x_display);

		if (x_path != NULL
		    && !g_file_test(x_path, G_FILE_TEST_EXISTS)) {
			g_message("gowl: DISPLAY=%s has no X server; "
			          "ignoring it", x_display);
			g_unsetenv("DISPLAY");
		}
	}

	return FALSE;
}

/* ------------------------------------------------------------------ */

/* atexit() handler.  Runs in whatever process is exiting, which is not
 * necessarily the one that bound the socket. */
static void
gowl_wayland_socket_cleanup(void)
{
	struct stat	st;

	if (gowl_socket_path == NULL)
		return;

	/* A forked child that exits through exit() -- rather than _exit()
	 * or exec() -- runs the handlers it inherited.  Its parent is still
	 * serving that socket, so removing the file here would break the
	 * live session instead of cleaning up after a dead one. */
	if (getpid() != gowl_socket_pid)
		return;

	/* And only if it is still the file we registered.  A compositor
	 * disposed at run time (gowl-stop) unlinks its own socket and frees
	 * the name; anything that binds it afterwards must not be deleted
	 * by our exit. */
	if (stat(gowl_socket_path, &st) != 0)
		return;
	if (st.st_dev != gowl_socket_dev || st.st_ino != gowl_socket_ino)
		return;

	unlink(gowl_socket_path);
}

void
gowl_wayland_socket_unlink_on_exit(const gchar *display)
{
	static gboolean	 registered = FALSE;
	struct stat	 st;
	gchar		*path;

	path = gowl_wayland_socket_path(display);
	if (path == NULL)
		return;

	/* Without an identity to check at exit, do not arm the handler at
	 * all: an unconditional unlink by name is the failure mode this
	 * whole file exists to avoid. */
	if (stat(path, &st) != 0) {
		g_free(path);
		return;
	}

	g_free(gowl_socket_path);
	gowl_socket_path = path;
	gowl_socket_pid = getpid();
	gowl_socket_dev = st.st_dev;
	gowl_socket_ino = st.st_ino;

	if (!registered) {
		if (atexit(gowl_wayland_socket_cleanup) != 0) {
			g_debug("gowl: could not register socket cleanup; "
			        "%s may be left behind", gowl_socket_path);
			return;
		}
		registered = TRUE;
	}
}
