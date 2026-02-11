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

#include "gowl-startup-handler.h"

G_DEFINE_INTERFACE(GowlStartupHandler, gowl_startup_handler, G_TYPE_OBJECT)

static void
gowl_startup_handler_default_init(GowlStartupHandlerInterface *iface)
{
	/* Default implementation - no-op */
	(void)iface;
}

/**
 * gowl_startup_handler_on_startup:
 * @self: a #GowlStartupHandler
 * @compositor: (nullable): the compositor instance that is starting up
 *
 * Called when the compositor starts up. Implementations can use this
 * to launch autostart programs, restore session state, etc.
 */
void
gowl_startup_handler_on_startup(
	GowlStartupHandler *self,
	gpointer            compositor
){
	GowlStartupHandlerInterface *iface;

	g_return_if_fail(GOWL_IS_STARTUP_HANDLER(self));

	iface = GOWL_STARTUP_HANDLER_GET_IFACE(self);
	if (iface->on_startup != NULL)
		iface->on_startup(self, compositor);
}
