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

#include "gowl-shutdown-handler.h"

G_DEFINE_INTERFACE(GowlShutdownHandler, gowl_shutdown_handler, G_TYPE_OBJECT)

static void
gowl_shutdown_handler_default_init(GowlShutdownHandlerInterface *iface)
{
	/* Default implementation - no-op */
	(void)iface;
}

/**
 * gowl_shutdown_handler_on_shutdown:
 * @self: a #GowlShutdownHandler
 * @compositor: (nullable): the compositor instance that is shutting down
 *
 * Called when the compositor shuts down. Implementations can use this
 * to save session state, clean up resources, etc.
 */
void
gowl_shutdown_handler_on_shutdown(
	GowlShutdownHandler *self,
	gpointer             compositor
){
	GowlShutdownHandlerInterface *iface;

	g_return_if_fail(GOWL_IS_SHUTDOWN_HANDLER(self));

	iface = GOWL_SHUTDOWN_HANDLER_GET_IFACE(self);
	if (iface->on_shutdown != NULL)
		iface->on_shutdown(self, compositor);
}
