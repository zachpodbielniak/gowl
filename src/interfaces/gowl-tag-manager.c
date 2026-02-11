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

#include "gowl-tag-manager.h"

G_DEFINE_INTERFACE(GowlTagManager, gowl_tag_manager, G_TYPE_OBJECT)

static void
gowl_tag_manager_default_init(GowlTagManagerInterface *iface)
{
	/* Default implementation - no-op */
	(void)iface;
}

/**
 * gowl_tag_manager_get_tag_name:
 * @self: a #GowlTagManager
 * @tag_index: the zero-based index of the tag
 *
 * Returns the display name for the tag at the given index.
 *
 * Returns: (transfer full) (nullable): the tag name, or %NULL
 */
gchar *
gowl_tag_manager_get_tag_name(
	GowlTagManager *self,
	gint            tag_index
){
	GowlTagManagerInterface *iface;

	g_return_val_if_fail(GOWL_IS_TAG_MANAGER(self), NULL);

	iface = GOWL_TAG_MANAGER_GET_IFACE(self);
	if (iface->get_tag_name != NULL)
		return iface->get_tag_name(self, tag_index);
	return NULL;
}

/**
 * gowl_tag_manager_should_hide_vacant:
 * @self: a #GowlTagManager
 * @tag_index: the zero-based index of the tag
 *
 * Returns whether a vacant (empty) tag should be hidden from display.
 *
 * Returns: %TRUE if the vacant tag should be hidden, %FALSE otherwise
 */
gboolean
gowl_tag_manager_should_hide_vacant(
	GowlTagManager *self,
	gint            tag_index
){
	GowlTagManagerInterface *iface;

	g_return_val_if_fail(GOWL_IS_TAG_MANAGER(self), FALSE);

	iface = GOWL_TAG_MANAGER_GET_IFACE(self);
	if (iface->should_hide_vacant != NULL)
		return iface->should_hide_vacant(self, tag_index);
	return FALSE;
}
