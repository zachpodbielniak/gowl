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

#include "gowl-keybind-handler.h"

G_DEFINE_INTERFACE(GowlKeybindHandler, gowl_keybind_handler, G_TYPE_OBJECT)

static void
gowl_keybind_handler_default_init(GowlKeybindHandlerInterface *iface)
{
	/* Default implementation - no-op */
	(void)iface;
}

/**
 * gowl_keybind_handler_handle_key:
 * @self: a #GowlKeybindHandler
 * @modifiers: bitmask of active modifier keys
 * @keysym: the keysym that was pressed or released
 * @pressed: %TRUE if the key was pressed, %FALSE if released
 *
 * Handles a keyboard event. Returns %TRUE if the event was consumed.
 *
 * Returns: %TRUE if the key event was handled, %FALSE otherwise
 */
gboolean
gowl_keybind_handler_handle_key(
	GowlKeybindHandler *self,
	guint               modifiers,
	guint               keysym,
	gboolean            pressed
){
	GowlKeybindHandlerInterface *iface;

	g_return_val_if_fail(GOWL_IS_KEYBIND_HANDLER(self), FALSE);

	iface = GOWL_KEYBIND_HANDLER_GET_IFACE(self);
	if (iface->handle_key != NULL)
		return iface->handle_key(self, modifiers, keysym, pressed);
	return FALSE;
}
