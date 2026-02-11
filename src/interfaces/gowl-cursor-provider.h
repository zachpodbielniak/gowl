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

#ifndef GOWL_CURSOR_PROVIDER_H
#define GOWL_CURSOR_PROVIDER_H

#include <glib-object.h>

G_BEGIN_DECLS

#define GOWL_TYPE_CURSOR_PROVIDER (gowl_cursor_provider_get_type())

G_DECLARE_INTERFACE(GowlCursorProvider, gowl_cursor_provider, GOWL, CURSOR_PROVIDER, GObject)

struct _GowlCursorProviderInterface {
	GTypeInterface parent_iface;

	const gchar *(*get_cursor_theme) (GowlCursorProvider *self);
	gint         (*get_cursor_size)  (GowlCursorProvider *self);
};

/* Public dispatch functions */
const gchar *gowl_cursor_provider_get_cursor_theme (GowlCursorProvider *self);
gint         gowl_cursor_provider_get_cursor_size  (GowlCursorProvider *self);

G_END_DECLS

#endif /* GOWL_CURSOR_PROVIDER_H */
