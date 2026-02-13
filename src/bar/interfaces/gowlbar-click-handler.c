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

#include "gowlbar-click-handler.h"

G_DEFINE_INTERFACE(GowlbarClickHandler, gowlbar_click_handler,
                   G_TYPE_OBJECT)

static void
gowlbar_click_handler_default_init(
	GowlbarClickHandlerInterface *iface
){
	(void)iface;
}

/**
 * gowlbar_click_handler_on_click:
 * @self: a #GowlbarClickHandler
 * @x: click x coordinate
 * @y: click y coordinate
 * @button: mouse button number
 *
 * Returns: %TRUE if the click was consumed
 */
gboolean
gowlbar_click_handler_on_click(
	GowlbarClickHandler *self,
	gint                  x,
	gint                  y,
	guint                 button
){
	GowlbarClickHandlerInterface *iface;

	g_return_val_if_fail(GOWLBAR_IS_CLICK_HANDLER(self), FALSE);

	iface = GOWLBAR_CLICK_HANDLER_GET_IFACE(self);
	if (iface->on_click != NULL)
		return iface->on_click(self, x, y, button);
	return FALSE;
}
