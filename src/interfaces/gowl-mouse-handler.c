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

#include "gowl-mouse-handler.h"

G_DEFINE_INTERFACE(GowlMouseHandler, gowl_mouse_handler, G_TYPE_OBJECT)

static void
gowl_mouse_handler_default_init(GowlMouseHandlerInterface *iface)
{
	/* Default implementation - no-op */
	(void)iface;
}

/**
 * gowl_mouse_handler_handle_button:
 * @self: a #GowlMouseHandler
 * @button: the mouse button that was pressed or released
 * @state: the button state (pressed or released)
 * @modifiers: bitmask of active modifier keys
 *
 * Handles a mouse button event. Returns %TRUE if the event was consumed.
 *
 * Returns: %TRUE if the button event was handled, %FALSE otherwise
 */
gboolean
gowl_mouse_handler_handle_button(
	GowlMouseHandler *self,
	guint             button,
	guint             state,
	guint             modifiers
){
	GowlMouseHandlerInterface *iface;

	g_return_val_if_fail(GOWL_IS_MOUSE_HANDLER(self), FALSE);

	iface = GOWL_MOUSE_HANDLER_GET_IFACE(self);
	if (iface->handle_button != NULL)
		return iface->handle_button(self, button, state, modifiers);
	return FALSE;
}

/**
 * gowl_mouse_handler_handle_motion:
 * @self: a #GowlMouseHandler
 * @x: the x coordinate of the cursor
 * @y: the y coordinate of the cursor
 *
 * Handles a mouse motion event. Returns %TRUE if the event was consumed.
 *
 * Returns: %TRUE if the motion event was handled, %FALSE otherwise
 */
gboolean
gowl_mouse_handler_handle_motion(
	GowlMouseHandler *self,
	gdouble           x,
	gdouble           y
){
	GowlMouseHandlerInterface *iface;

	g_return_val_if_fail(GOWL_IS_MOUSE_HANDLER(self), FALSE);

	iface = GOWL_MOUSE_HANDLER_GET_IFACE(self);
	if (iface->handle_motion != NULL)
		return iface->handle_motion(self, x, y);
	return FALSE;
}
