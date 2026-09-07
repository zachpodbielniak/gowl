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

#ifndef PORTAL_EIS_H
#define PORTAL_EIS_H

#include <glib.h>
#include <stdint.h>
#include <stdbool.h>

G_BEGIN_DECLS

/*
 * PortalEis -- the libeis EIS server behind the InputCapture and
 * RemoteDesktop sessions.  Its eis_get_fd() is integrated into the GLib
 * main loop; ConnectToEIS hands a per-client fd to the caller.
 *
 * IT SERVES BOTH DIRECTIONS, and they are not symmetric.  libei clients
 * declare themselves sender or receiver at connect time, and the EIS
 * implementation takes the opposite role:
 *
 *   RECEIVER client -- deskflow acting as a KVM *server*, sharing this
 *     machine's keyboard and mouse.  We create the device and PUSH the
 *     input the compositor captured at it.  This is the InputCapture
 *     portal.
 *
 *   SENDER client -- deskflow acting as a KVM *client*, driven by
 *     another machine's keyboard and mouse.  We create a device and it
 *     PUSHES events at us, which we forward into the compositor.  This
 *     is the RemoteDesktop portal.
 *
 * Rejecting senders is what made client mode fail: the D-Bus handshake
 * succeeded, ConnectToEIS handed over a working fd, and the connection
 * was then closed the instant libei said which way round it wanted to
 * be.  There is nothing to see in a log at that point but a client that
 * quietly does nothing.
 */
typedef struct _PortalEis PortalEis;

/**
 * PortalEisInjectOps:
 * @rel_motion: relative pointer motion, in pixels
 * @abs_motion: absolute pointer position, in LAYOUT coordinates (the
 *   same space the zones use), not normalised
 * @button: a pointer button, evdev code
 * @axis: scroll; @axis is 0 for vertical and 1 for horizontal
 * @key: a key, evdev keycode
 * @frame: end of one logical input frame
 *
 * Where a sender client's events go.  The EIS layer knows nothing about
 * Wayland; the D-Bus layer owns both halves and wires them together.
 */
typedef struct {
	void (*rel_motion) (gpointer user_data, double dx, double dy);
	void (*abs_motion) (gpointer user_data, double x, double y);
	void (*button)     (gpointer user_data, uint32_t button, bool pressed);
	void (*axis)       (gpointer user_data, uint32_t axis, double value);
	void (*key)        (gpointer user_data, uint32_t keycode, bool pressed);
	void (*frame)      (gpointer user_data);
} PortalEisInjectOps;

/* Where to forward a sender client's events.  Without this a sender is
 * still accepted and its events are dropped, which is the right
 * behaviour for a portal whose Wayland side failed to come up. */
void        portal_eis_set_inject_ops (PortalEis                *self,
                                       const PortalEisInjectOps *ops,
                                       gpointer                  user_data);

/* Create an EIS server with the fd-passing backend.  Returns NULL on
 * failure (e.g. libeis context setup failed). */
PortalEis * portal_eis_new (void);

void        portal_eis_free (PortalEis *self);

/* Mint a new client fd to hand to deskflow over the ConnectToEIS D-Bus
 * reply.  Returns a fd (caller passes ownership to D-Bus) or -1. */
int         portal_eis_connect_fd (PortalEis *self);

/* Set the capture zones (monitor geometry, in the same coordinate space as
 * the reported zones / Activated cursor_position).  Each zone becomes a
 * region on the EIS device.  This is REQUIRED, not optional: the libei
 * *receiver* client (deskflow) derives its screen shape from the union of
 * the device's ei_regions (EiScreen::updateShape sums them).  With no
 * region the client's screen is 1x1, so a pointer-barrier crossing at
 * (x,y) normalises to a fraction far outside [0,1], getNeighbor() finds no
 * neighbour in any direction, and the cursor can never switch to the
 * remote machine.  (Mutter sets these, which is why deskflow works under
 * GNOME.)  Call before the client binds the seat -- e.g. from GetZones;
 * the regions are applied when the device is (re)created. */
void        portal_eis_clear_zones (PortalEis *self);
void        portal_eis_add_zone    (PortalEis *self, int32_t x, int32_t y,
                                    uint32_t width, uint32_t height);

/* Begin/end an emulation sequence around a burst of captured events.
 * Called on portal `activated`/`deactivated`. */
void        portal_eis_start (PortalEis *self);
void        portal_eis_stop  (PortalEis *self);

/* Stream one captured event to the receiver device.  No-op until a
 * device is resumed by the client.  Each logical input frame should end
 * with portal_eis_frame(). */
void        portal_eis_rel_motion (PortalEis *self, double dx, double dy);
void        portal_eis_button     (PortalEis *self, uint32_t button,
                                   bool pressed);
void        portal_eis_scroll     (PortalEis *self, uint32_t axis,
                                   double value);
void        portal_eis_key        (PortalEis *self, uint32_t keycode,
                                   bool pressed);
void        portal_eis_modifiers  (PortalEis *self, uint32_t depressed,
                                   uint32_t latched, uint32_t locked,
                                   uint32_t group);
void        portal_eis_frame      (PortalEis *self);

/* TRUE once a sender (injecting) client has a device it is emulating on.
 * The RemoteDesktop portal reports this so a caller can tell "connected"
 * from "connected and actually able to move the pointer". */
gboolean    portal_eis_injecting  (PortalEis *self);

G_END_DECLS

#endif /* PORTAL_EIS_H */
