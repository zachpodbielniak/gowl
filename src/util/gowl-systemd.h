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
 */

#ifndef GOWL_SYSTEMD_H
#define GOWL_SYSTEMD_H

#include <glib.h>

G_BEGIN_DECLS

/* Call near the end of gowl_compositor_start(), once the compositor's
 * WAYLAND_DISPLAY is published and the backend is started. */
void	gowl_systemd_start	(void);

/* Call from the quit/shutdown path.  Idempotent and best-effort. */
void	gowl_systemd_stop	(void);

G_END_DECLS

#endif /* GOWL_SYSTEMD_H */