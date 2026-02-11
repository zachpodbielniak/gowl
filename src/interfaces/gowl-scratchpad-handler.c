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

#include "gowl-scratchpad-handler.h"

G_DEFINE_INTERFACE(GowlScratchpadHandler, gowl_scratchpad_handler, G_TYPE_OBJECT)

static void
gowl_scratchpad_handler_default_init(GowlScratchpadHandlerInterface *iface)
{
	/* Default implementation - no-op */
	(void)iface;
}

/**
 * gowl_scratchpad_handler_is_scratchpad:
 * @self: a #GowlScratchpadHandler
 * @client: (nullable): the client to check
 *
 * Returns whether the given client is a scratchpad window.
 *
 * Returns: %TRUE if the client is a scratchpad, %FALSE otherwise
 */
gboolean
gowl_scratchpad_handler_is_scratchpad(
	GowlScratchpadHandler *self,
	gpointer               client
){
	GowlScratchpadHandlerInterface *iface;

	g_return_val_if_fail(GOWL_IS_SCRATCHPAD_HANDLER(self), FALSE);

	iface = GOWL_SCRATCHPAD_HANDLER_GET_IFACE(self);
	if (iface->is_scratchpad != NULL)
		return iface->is_scratchpad(self, client);
	return FALSE;
}

/**
 * gowl_scratchpad_handler_toggle_scratchpad:
 * @self: a #GowlScratchpadHandler
 * @name: the name of the scratchpad to toggle
 *
 * Toggles the visibility of the named scratchpad window.
 */
void
gowl_scratchpad_handler_toggle_scratchpad(
	GowlScratchpadHandler *self,
	const gchar           *name
){
	GowlScratchpadHandlerInterface *iface;

	g_return_if_fail(GOWL_IS_SCRATCHPAD_HANDLER(self));

	iface = GOWL_SCRATCHPAD_HANDLER_GET_IFACE(self);
	if (iface->toggle_scratchpad != NULL)
		iface->toggle_scratchpad(self, name);
}
