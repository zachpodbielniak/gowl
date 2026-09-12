/*
 * gowl - GObject Wayland Compositor
 * Copyright (C) 2026  Zach Podbielniak
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#ifndef PORTAL_SCREENCAST_H
#define PORTAL_SCREENCAST_H

#include <glib.h>
#include <gio/gio.h>

G_BEGIN_DECLS

/*
 * PortalScreenCast -- org.freedesktop.impl.portal.ScreenCast.
 *
 * Registered on the same bus name as the InputCapture and RemoteDesktop
 * backends, on the same object path the frontend expects.  It exists so
 * a gowl session can share ONE WINDOW: portal-wlr addresses outputs
 * only, because wlr-screencopy does, and "share this window" there
 * means sharing the whole screen.
 */
typedef struct _PortalScreenCast PortalScreenCast;

/**
 * portal_screencast_new:
 * @conn: the session bus connection the backend already owns
 * @error: return location for an error
 *
 * Exports the ScreenCast backend.  Returns %NULL with @error set when
 * the compositor offers no image-copy-capture or PipeWire is not
 * running, in which case the caller should carry on without it: an
 * InputCapture session is still useful, and the frontend falls back to
 * another backend for casting.
 *
 * Returns: (transfer full): the backend, or %NULL
 */
PortalScreenCast *portal_screencast_new (GDBusConnection *conn,
                                         GError         **error);

void portal_screencast_free (PortalScreenCast *self);

G_END_DECLS

#endif /* PORTAL_SCREENCAST_H */
