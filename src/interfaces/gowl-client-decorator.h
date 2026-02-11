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

#ifndef GOWL_CLIENT_DECORATOR_H
#define GOWL_CLIENT_DECORATOR_H

#include <glib-object.h>

G_BEGIN_DECLS

#define GOWL_TYPE_CLIENT_DECORATOR (gowl_client_decorator_get_type())

G_DECLARE_INTERFACE(GowlClientDecorator, gowl_client_decorator, GOWL, CLIENT_DECORATOR, GObject)

struct _GowlClientDecoratorInterface {
	GTypeInterface parent_iface;

	gint     (*get_border_width)   (GowlClientDecorator *self, gpointer client);
	gboolean (*should_draw_border) (GowlClientDecorator *self, gpointer client);
};

/* Public dispatch functions */
gint     gowl_client_decorator_get_border_width   (GowlClientDecorator *self, gpointer client);
gboolean gowl_client_decorator_should_draw_border (GowlClientDecorator *self, gpointer client);

G_END_DECLS

#endif /* GOWL_CLIENT_DECORATOR_H */
