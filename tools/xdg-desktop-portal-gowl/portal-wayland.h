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

#ifndef PORTAL_WAYLAND_H
#define PORTAL_WAYLAND_H

#include <glib.h>
#include <stdint.h>

#include "portal-eis.h"

G_BEGIN_DECLS

/*
 * PortalWayland -- the Wayland-client half of xdg-desktop-portal-gowl.
 * Connects to the compositor (WAYLAND_DISPLAY), binds the private
 * zgowl_input_capture_manager_v1 global, and drives one capture session
 * and one inject object.  Captured-input events received from the
 * compositor are forwarded into the session's PortalEis server; barrier
 * activation is reported up via a callback for the D-Bus layer.
 *
 * The wl_display fd is integrated into the GLib main loop.
 */
typedef struct _PortalWayland PortalWayland;

/* A zone reported by the compositor's get_zones reply. */
typedef struct {
	uint32_t width;
	uint32_t height;
	int32_t  x;
	int32_t  y;
} PortalZone;

/* Activation callback: fired on barrier crossing (activated TRUE) and on
 * release/deactivation (activated FALSE).  Maps to the D-Bus Activated/
 * Deactivated signals. */
typedef void (*PortalActivationFunc)(guint32  activation_id,
                                     double   x,
                                     double   y,
                                     guint32  barrier_id,
                                     gboolean activated,
                                     gpointer user_data);

/* Zones-changed callback: the layout changed; installed barriers were
 * dropped and the client should re-query zones and reinstall. */
typedef void (*PortalZonesChangedFunc)(guint32 zone_set, gpointer user_data);

PortalWayland * portal_wayland_new   (PortalEis *eis, GError **error);
void            portal_wayland_free  (PortalWayland *self);

void portal_wayland_set_activation_callback (PortalWayland       *self,
                                             PortalActivationFunc cb,
                                             gpointer             user_data);
void portal_wayland_set_zones_changed_callback (PortalWayland          *self,
                                                PortalZonesChangedFunc  cb,
                                                gpointer                user_data);

/* InputCapture control (called from the D-Bus method handlers). */
gboolean portal_wayland_get_zones    (PortalWayland *self,
                                      PortalZone   **zones_out,
                                      guint         *n_zones_out,
                                      guint32       *zone_set_out);
void     portal_wayland_add_barrier  (PortalWayland *self, guint32 id,
                                      int32_t x1, int32_t y1,
                                      int32_t x2, int32_t y2);
void     portal_wayland_set_barriers (PortalWayland *self, guint32 zone_set);
void     portal_wayland_enable       (PortalWayland *self);
void     portal_wayland_disable      (PortalWayland *self);
void     portal_wayland_release      (PortalWayland *self,
                                      guint32 activation_id,
                                      gboolean has_position,
                                      double x, double y);

/* RemoteDesktop injection (called from the D-Bus method handlers). */
void portal_wayland_inject_rel_motion (PortalWayland *self, double dx,
                                       double dy);
void portal_wayland_inject_abs_motion (PortalWayland *self, double nx,
                                       double ny);
void portal_wayland_inject_button     (PortalWayland *self, uint32_t button,
                                       gboolean pressed);
void portal_wayland_inject_axis       (PortalWayland *self, uint32_t axis,
                                       double value);
void portal_wayland_inject_key        (PortalWayland *self, uint32_t keycode,
                                       gboolean pressed);
void portal_wayland_inject_frame      (PortalWayland *self);

G_END_DECLS

#endif /* PORTAL_WAYLAND_H */
