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

#include "gowl-layout-provider.h"

G_DEFINE_INTERFACE(GowlLayoutProvider, gowl_layout_provider, G_TYPE_OBJECT)

static void
gowl_layout_provider_default_init(GowlLayoutProviderInterface *iface)
{
	/* Default implementation - no-op */
	(void)iface;
}

/**
 * gowl_layout_provider_arrange:
 * @self: a #GowlLayoutProvider
 * @monitor: (nullable): the monitor to arrange clients on
 * @clients: (element-type gpointer): list of clients to arrange
 * @area: (nullable): the area within which to arrange
 *
 * Arranges the given clients on the specified monitor within the given area.
 */
void
gowl_layout_provider_arrange(
	GowlLayoutProvider *self,
	gpointer            monitor,
	GList              *clients,
	gpointer            area
){
	GowlLayoutProviderInterface *iface;

	g_return_if_fail(GOWL_IS_LAYOUT_PROVIDER(self));

	iface = GOWL_LAYOUT_PROVIDER_GET_IFACE(self);
	if (iface->arrange != NULL)
		iface->arrange(self, monitor, clients, area);
}

/**
 * gowl_layout_provider_get_symbol:
 * @self: a #GowlLayoutProvider
 *
 * Returns the display symbol for this layout (e.g. "[]=", "|M|").
 *
 * Returns: (transfer none) (nullable): the layout symbol string
 */
const gchar *
gowl_layout_provider_get_symbol(GowlLayoutProvider *self)
{
	GowlLayoutProviderInterface *iface;

	g_return_val_if_fail(GOWL_IS_LAYOUT_PROVIDER(self), NULL);

	iface = GOWL_LAYOUT_PROVIDER_GET_IFACE(self);
	if (iface->get_symbol != NULL)
		return iface->get_symbol(self);
	return NULL;
}
