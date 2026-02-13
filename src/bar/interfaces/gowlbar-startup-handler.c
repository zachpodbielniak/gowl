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

#include "gowlbar-startup-handler.h"

G_DEFINE_INTERFACE(GowlbarStartupHandler, gowlbar_startup_handler,
                   G_TYPE_OBJECT)

static void
gowlbar_startup_handler_default_init(
	GowlbarStartupHandlerInterface *iface
){
	(void)iface;
}

/**
 * gowlbar_startup_handler_on_startup:
 * @self: a #GowlbarStartupHandler
 * @app: the bar application object
 *
 * Called when the bar application starts up.
 */
void
gowlbar_startup_handler_on_startup(
	GowlbarStartupHandler *self,
	gpointer               app
){
	GowlbarStartupHandlerInterface *iface;

	g_return_if_fail(GOWLBAR_IS_STARTUP_HANDLER(self));

	iface = GOWLBAR_STARTUP_HANDLER_GET_IFACE(self);
	if (iface->on_startup != NULL)
		iface->on_startup(self, app);
}
