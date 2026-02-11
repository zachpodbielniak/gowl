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

#include "gowl-client-placer.h"

G_DEFINE_INTERFACE(GowlClientPlacer, gowl_client_placer, G_TYPE_OBJECT)

static void
gowl_client_placer_default_init(GowlClientPlacerInterface *iface)
{
	/* Default implementation - no-op */
	(void)iface;
}

/**
 * gowl_client_placer_place_new_client:
 * @self: a #GowlClientPlacer
 * @client: (nullable): the new client to place
 * @monitor: (nullable): the monitor on which to place the client
 *
 * Places a newly mapped client on the given monitor. This determines
 * the initial position and tag assignment for the client.
 */
void
gowl_client_placer_place_new_client(
	GowlClientPlacer *self,
	gpointer          client,
	gpointer          monitor
){
	GowlClientPlacerInterface *iface;

	g_return_if_fail(GOWL_IS_CLIENT_PLACER(self));

	iface = GOWL_CLIENT_PLACER_GET_IFACE(self);
	if (iface->place_new_client != NULL)
		iface->place_new_client(self, client, monitor);
}
