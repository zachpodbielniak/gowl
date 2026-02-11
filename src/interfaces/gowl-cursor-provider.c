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

#include "gowl-cursor-provider.h"

G_DEFINE_INTERFACE(GowlCursorProvider, gowl_cursor_provider, G_TYPE_OBJECT)

static void
gowl_cursor_provider_default_init(GowlCursorProviderInterface *iface)
{
	/* Default implementation - no-op */
	(void)iface;
}

/**
 * gowl_cursor_provider_get_cursor_theme:
 * @self: a #GowlCursorProvider
 *
 * Returns the name of the cursor theme to use.
 *
 * Returns: (transfer none) (nullable): the cursor theme name
 */
const gchar *
gowl_cursor_provider_get_cursor_theme(GowlCursorProvider *self)
{
	GowlCursorProviderInterface *iface;

	g_return_val_if_fail(GOWL_IS_CURSOR_PROVIDER(self), NULL);

	iface = GOWL_CURSOR_PROVIDER_GET_IFACE(self);
	if (iface->get_cursor_theme != NULL)
		return iface->get_cursor_theme(self);
	return NULL;
}

/**
 * gowl_cursor_provider_get_cursor_size:
 * @self: a #GowlCursorProvider
 *
 * Returns the cursor size in pixels.
 *
 * Returns: the cursor size in pixels, or 0 if unset
 */
gint
gowl_cursor_provider_get_cursor_size(GowlCursorProvider *self)
{
	GowlCursorProviderInterface *iface;

	g_return_val_if_fail(GOWL_IS_CURSOR_PROVIDER(self), 0);

	iface = GOWL_CURSOR_PROVIDER_GET_IFACE(self);
	if (iface->get_cursor_size != NULL)
		return iface->get_cursor_size(self);
	return 0;
}
