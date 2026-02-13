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

#include "gowlbar-status-provider.h"

G_DEFINE_INTERFACE(GowlbarStatusProvider, gowlbar_status_provider,
                   G_TYPE_OBJECT)

static void
gowlbar_status_provider_default_init(
	GowlbarStatusProviderInterface *iface
){
	(void)iface;
}

/**
 * gowlbar_status_provider_get_status:
 * @self: a #GowlbarStatusProvider
 *
 * Returns: (transfer full): the current status text, or %NULL
 */
gchar *
gowlbar_status_provider_get_status(GowlbarStatusProvider *self)
{
	GowlbarStatusProviderInterface *iface;

	g_return_val_if_fail(GOWLBAR_IS_STATUS_PROVIDER(self), NULL);

	iface = GOWLBAR_STATUS_PROVIDER_GET_IFACE(self);
	if (iface->get_status != NULL)
		return iface->get_status(self);
	return NULL;
}
