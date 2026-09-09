/* gowl-wayland-socket.h -- is anything actually listening on that socket?
 *
 * $XDG_RUNTIME_DIR/wayland-N is a file, and its presence proves nothing.
 * libwayland removes it when a compositor destroys its wl_display; a
 * compositor that exits any other way -- a crash, or simply calling
 * exit() without disposing, which is what `emacs --gowl' does on every
 * logout -- leaves the file behind for the rest of the boot.
 *
 * Treating that leftover as "a compositor is running" is how a session
 * kills the NEXT login rather than its own: the next launch decides it
 * is nested, forces the wlroots `wayland' backend, and fails to connect
 * to a socket with nothing on the other end.  The user is dropped back
 * at the display manager with no visible reason, and logging into a
 * different desktop and out again appears to "fix" it -- because that
 * desktop's clean shutdown removed the stale file.
 *
 * So ask the question the backend is about to ask: connect to it.
 *
 * The lock file (wayland-N.lock, flock()ed by libwayland for as long as
 * the socket is bound) would answer the same question without opening a
 * connection, but only for sockets libwayland created, and only while
 * the lock file itself survives.  A connect() is what every client does
 * and needs no cooperation from the other end.
 */

#ifndef GOWL_WAYLAND_SOCKET_H
#define GOWL_WAYLAND_SOCKET_H

#include <glib.h>

G_BEGIN_DECLS

/**
 * gowl_wayland_socket_live:
 * @display: (nullable): a Wayland display name (`wayland-0') or an
 *   absolute path to a socket, i.e. the two forms $WAYLAND_DISPLAY takes
 *
 * Whether a compositor is listening on @display right now.
 *
 * A relative name is resolved against $XDG_RUNTIME_DIR, exactly as
 * libwayland resolves it.  %NULL, an empty string, a name that resolves
 * to nothing, and a socket file whose owner is gone all return %FALSE.
 *
 * The probe connects and immediately closes, which the compositor on
 * the other end sees as a client that connected and went away.
 *
 * Returns: %TRUE if a connection succeeded.
 */
gboolean	gowl_wayland_socket_live	(const gchar *display);

/**
 * gowl_wayland_socket_unlink_on_exit:
 * @display: (nullable): the display name this process is serving,
 *   i.e. what wl_display_add_socket_auto() returned
 *
 * Remove @display's socket file when this process exits.
 *
 * For the common case where a compositor never destroys its wl_display
 * -- gowl's own main() and cmacs both exit() with the compositor still
 * alive -- so the file that libwayland would have removed is removed
 * anyway, and the next login is not lied to by our own leftovers.
 *
 * Registered with atexit(), and a no-op in any process that inherited
 * the registration through fork(): the socket belongs to the parent and
 * is still live there.
 *
 * Best-effort, and deliberately not the load-bearing half of the fix.
 * atexit() handlers do not run for a process killed by a signal --- a
 * crash, or the display manager tearing down the session scope --- so
 * leftovers remain possible.  What makes them harmless is asking
 * gowl_wayland_socket_live() rather than trusting the file.
 *
 * The lock file is deliberately left alone.  It carries no data, the
 * next compositor to take the name flock()s it in place, and removing
 * one out from under a compositor that holds it would let two bind the
 * same socket.
 */
void		gowl_wayland_socket_unlink_on_exit	(const gchar *display);

G_END_DECLS

#endif /* GOWL_WAYLAND_SOCKET_H */
