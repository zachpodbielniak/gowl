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

#include "gowlbar-shutdown-handler.h"

G_DEFINE_INTERFACE(GowlbarShutdownHandler, gowlbar_shutdown_handler,
                   G_TYPE_OBJECT)

static void
gowlbar_shutdown_handler_default_init(
	GowlbarShutdownHandlerInterface *iface
){
	(void)iface;
}

/**
 * gowlbar_shutdown_handler_on_shutdown:
 * @self: a #GowlbarShutdownHandler
 * @app: the bar application object
 *
 * Called when the bar application shuts down.
 */
void
gowlbar_shutdown_handler_on_shutdown(
	GowlbarShutdownHandler *self,
	gpointer                app
){
	GowlbarShutdownHandlerInterface *iface;

	g_return_if_fail(GOWLBAR_IS_SHUTDOWN_HANDLER(self));

	iface = GOWLBAR_SHUTDOWN_HANDLER_GET_IFACE(self);
	if (iface->on_shutdown != NULL)
		iface->on_shutdown(self, app);
}
