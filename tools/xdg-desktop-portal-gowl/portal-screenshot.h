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

#ifndef PORTAL_SCREENSHOT_H
#define PORTAL_SCREENSHOT_H

#include <glib.h>
#include <gio/gio.h>

#include "portal-capture.h"

G_BEGIN_DECLS

/*
 * PortalScreenshot -- org.freedesktop.impl.portal.Screenshot, v3.
 *
 * The same ext-image-copy-capture path ScreenCast uses, one frame at a
 * time: a whole output, a window (chosen, or the active one), or an
 * area picked with slurp.  Unlike ScreenCast it needs no PipeWire, so
 * it is served whenever the compositor offers image-copy-capture.
 */
typedef struct _PortalScreenshot PortalScreenshot;

/**
 * portal_screenshot_new:
 * @conn: the session bus connection the backend already owns
 * @capture: the Wayland capture context, borrowed; it must outlive
 *   the backend
 * @error: return location for an error
 *
 * Exports the Screenshot backend.  Returns %NULL with @error set when
 * the compositor offers no image-copy-capture, in which case the
 * interface is simply not there and the frontend routes elsewhere.
 *
 * Returns: (transfer full) (nullable): the backend
 */
PortalScreenshot *portal_screenshot_new (GDBusConnection *conn,
                                         PortalCapture   *capture,
                                         GError         **error);

void portal_screenshot_free (PortalScreenshot *self);

G_END_DECLS

#endif /* PORTAL_SCREENSHOT_H */
