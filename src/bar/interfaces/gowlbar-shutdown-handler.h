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

#ifndef GOWLBAR_SHUTDOWN_HANDLER_H
#define GOWLBAR_SHUTDOWN_HANDLER_H

#include <glib-object.h>

G_BEGIN_DECLS

#define GOWLBAR_TYPE_SHUTDOWN_HANDLER (gowlbar_shutdown_handler_get_type())

G_DECLARE_INTERFACE(GowlbarShutdownHandler, gowlbar_shutdown_handler,
                    GOWLBAR, SHUTDOWN_HANDLER, GObject)

struct _GowlbarShutdownHandlerInterface {
	GTypeInterface parent_iface;

	void (*on_shutdown) (GowlbarShutdownHandler *self, gpointer app);
};

void gowlbar_shutdown_handler_on_shutdown(GowlbarShutdownHandler *self,
                                            gpointer app);

G_END_DECLS

#endif /* GOWLBAR_SHUTDOWN_HANDLER_H */
