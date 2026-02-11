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

#ifndef GOWL_TAG_MANAGER_H
#define GOWL_TAG_MANAGER_H

#include <glib-object.h>

G_BEGIN_DECLS

#define GOWL_TYPE_TAG_MANAGER (gowl_tag_manager_get_type())

G_DECLARE_INTERFACE(GowlTagManager, gowl_tag_manager, GOWL, TAG_MANAGER, GObject)

struct _GowlTagManagerInterface {
	GTypeInterface parent_iface;

	gchar    *(*get_tag_name)        (GowlTagManager *self, gint tag_index);
	gboolean  (*should_hide_vacant)  (GowlTagManager *self, gint tag_index);
};

/* Public dispatch functions */
gchar    *gowl_tag_manager_get_tag_name       (GowlTagManager *self, gint tag_index);
gboolean  gowl_tag_manager_should_hide_vacant (GowlTagManager *self, gint tag_index);

G_END_DECLS

#endif /* GOWL_TAG_MANAGER_H */
