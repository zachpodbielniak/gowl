/* gowl-systemd.h -- best-effort systemd user-session integration.
 *
 * A bare wlroots compositor launched straight from the display manager
 * (gowl, or cmacs --gowl) does not bootstrap the systemd user session the
 * way gnome-session does.  Two things then break for graphical apps,
 * flatpaks in particular:
 *
 *   - graphical-session.target is never pulled in.  It has
 *     RefuseManualStart=yes, so it can only be activated by a unit that
 *     Wants= it; with the target dead, xdg-desktop-portal (whose unit
 *     carries Requisite=graphical-session.target) fails to start and
 *     flatpak apps get no org.freedesktop.portal.Settings -- so they
 *     never learn the color-scheme and draw a light title bar even on a
 *     dark-themed host.
 *   - The user manager's environment is stale.  Nothing runs
 *     `systemctl --user import-environment`, so the manager keeps the
 *     desktop/WAYLAND_DISPLAY of a prior session and DBus-activated
 *     helpers inherit that instead of the live compositor.
 *
 * gowl_systemd_start() fixes both, once WAYLAND_DISPLAY is set and the
 * backend has started.  gowl_systemd_stop() tears the session target
 * down on quit.  Everything is best-effort: if systemctl is absent or
 * the user manager is unreachable we warn and continue -- the
 * compositor still runs.  Set GOWL_DISABLE_SYSTEMD=1 to skip entirely.
 *
 * A third thing it fixes, in a real seat session only: a long-lived
 * xdg-desktop-portal frontend from a *previous* login survives in the
 * user manager and keeps the prior session's XDG_CURRENT_DESKTOP -- the
 * value it read once at startup to choose portal backends.  After a
 * re-login that picks a different desktop (or after we publish a
 * corrected XDG_CURRENT_DESKTOP) the stale frontend still routes
 * e.g. ScreenCast to the wrong backend, so screen-sharing silently
 * fails.  We therefore restart xdg-desktop-portal so it re-reads the
 * freshly-imported environment.  Skipped when nested (seat_session
 * FALSE), where the portal belongs to the host session and must not be
 * touched.
 */

#ifndef GOWL_SYSTEMD_H
#define GOWL_SYSTEMD_H

#include <glib.h>

G_BEGIN_DECLS

/* Call near the end of gowl_compositor_start(), once the compositor's
 * WAYLAND_DISPLAY is published and the backend is started.  @seat_session
 * is TRUE for a real display-manager seat session, FALSE when gowl is
 * nested inside another compositor (just gowl): the portal-restart step
 * is seat-only so a nested gowl never disturbs the host's portal. */
void	gowl_systemd_start	(gboolean seat_session);

/* Call from the quit/shutdown path.  Idempotent and best-effort. */
void	gowl_systemd_stop	(void);

G_END_DECLS

#endif /* GOWL_SYSTEMD_H */