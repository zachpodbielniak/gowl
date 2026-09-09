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
 * THREE ANSWERS, NOT TWO.  A probe can also fail to reach a verdict --
 * no $XDG_RUNTIME_DIR to resolve a relative name against, a path too
 * long for sockaddr_un, an exhausted fd table.  Collapsing that into
 * "nothing is there" is dangerous in a way the original bug was not:
 * the two variables this evidence feeds, $WAYLAND_DISPLAY and $DISPLAY,
 * are what gowl_systemd_should_manage_session() reads to decide whether
 * it owns the systemd user session.  Concluding "no parent" inside a
 * GNOME session makes gowl start gowl-session.target and, on the way
 * out, stop graphical-session.target -- which is GNOME's, and stopping
 * it ends the user's entire desktop, every application in it, and any
 * Emacs daemon parented to it.
 *
 * Hence the deliberately awkward pair of predicates below: live and
 * absent are BOTH false when we could not tell, so a caller cannot
 * accidentally read uncertainty as either verdict.  Uncertainty must
 * resolve toward "there is a parent", which costs a nested compositor
 * nothing and protects the session hosting it.
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
 * libwayland resolves it.  The probe connects and immediately closes,
 * which the compositor on the other end sees as a client that connected
 * and went away.
 *
 * %FALSE means "not proven live", which includes the case where the
 * probe could not run at all --- see gowl_wayland_socket_absent().
 *
 * Returns: %TRUE if a connection succeeded.
 */
gboolean	gowl_wayland_socket_live	(const gchar *display);

/**
 * gowl_wayland_socket_absent:
 * @display: (nullable): as for gowl_wayland_socket_live()
 *
 * Whether nothing is behind @display, *proven* rather than assumed.
 *
 * True only for a name that resolves to a path that does not exist, or
 * that refuses the connection outright.  An unresolvable name, a path
 * too long to connect to, or a probe that could not be attempted all
 * return %FALSE here *and* %FALSE from gowl_wayland_socket_live(): that
 * pair means "no verdict", and the caller must decide which way to err.
 *
 * Returns: %TRUE if nothing is there.
 */
gboolean	gowl_wayland_socket_absent	(const gchar *display);

/**
 * gowl_wayland_detect_parent_session:
 *
 * Whether another compositor already owns this graphical session, and
 * so whether we are about to nest inside one.
 *
 * Consults $WAYLAND_SOCKET (a connected fd from a parent is
 * unambiguous), then $WAYLAND_DISPLAY, then the conventional socket
 * names in $XDG_RUNTIME_DIR.  A $WAYLAND_DISPLAY or $DISPLAY *proven*
 * to name nothing is unset, not merely ignored: wlr_backend_autocreate
 * chooses its backend from the presence of those variables alone, and
 * gowl_systemd_should_manage_session() reads them to decide session
 * ownership.  Both leak across logins through the systemd user manager,
 * which is imported into at session start and never cleaned at session
 * end.
 *
 * Errs toward %TRUE.  Anything short of proof that a display is dead
 * leaves it in place: a nested compositor that wrongly believes it has
 * a parent merely declines to manage the user session, while one that
 * wrongly believes it has none tears down the desktop hosting it.
 *
 * Returns: %TRUE when a Wayland parent exists (or may exist).
 */
gboolean	gowl_wayland_detect_parent_session	(void);

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
 * Registered with atexit().  It removes the file only if it is still
 * the same one: the device and inode recorded here must match, so a
 * compositor that already tore its display down and let another process
 * claim the name cannot have that name deleted out from under it.  It
 * is likewise a no-op in any process that inherited the registration
 * through fork(), where the socket belongs to the parent and is live.
 *
 * Best-effort, and deliberately not the load-bearing half of the fix.
 * atexit() handlers do not run for a process killed by a signal --- a
 * crash, or the display manager tearing down the session scope --- so
 * leftovers remain possible.  What makes them harmless is asking rather
 * than trusting the file.
 *
 * The lock file is deliberately left alone.  It carries no data, the
 * next compositor to take the name flock()s it in place, and removing
 * one out from under a compositor that holds it would let two bind the
 * same socket.
 */
void		gowl_wayland_socket_unlink_on_exit	(const gchar *display);

G_END_DECLS

#endif /* GOWL_WAYLAND_SOCKET_H */
