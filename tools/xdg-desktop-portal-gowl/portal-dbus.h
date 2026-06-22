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

#ifndef PORTAL_DBUS_H
#define PORTAL_DBUS_H

#include <glib.h>

#include "portal-wayland.h"
#include "portal-eis.h"

G_BEGIN_DECLS

/*
 * PortalDbus -- owns the org.freedesktop.impl.portal.desktop.gowl bus
 * name and implements the InputCapture + RemoteDesktop impl-portal
 * interfaces, delegating to PortalWayland (compositor protocol) and
 * PortalEis (the EIS server).
 */
typedef struct _PortalDbus PortalDbus;

PortalDbus * portal_dbus_new  (PortalWayland *wl, PortalEis *eis);
void         portal_dbus_free (PortalDbus *self);

G_END_DECLS

#endif /* PORTAL_DBUS_H */
