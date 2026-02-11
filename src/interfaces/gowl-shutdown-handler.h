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

#ifndef GOWL_SHUTDOWN_HANDLER_H
#define GOWL_SHUTDOWN_HANDLER_H

#include <glib-object.h>

G_BEGIN_DECLS

#define GOWL_TYPE_SHUTDOWN_HANDLER (gowl_shutdown_handler_get_type())

G_DECLARE_INTERFACE(GowlShutdownHandler, gowl_shutdown_handler, GOWL, SHUTDOWN_HANDLER, GObject)

struct _GowlShutdownHandlerInterface {
	GTypeInterface parent_iface;

	void (*on_shutdown) (GowlShutdownHandler *self, gpointer compositor);
};

/* Public dispatch functions */
void gowl_shutdown_handler_on_shutdown (GowlShutdownHandler *self, gpointer compositor);

G_END_DECLS

#endif /* GOWL_SHUTDOWN_HANDLER_H */
