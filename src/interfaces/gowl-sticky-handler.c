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

#include "gowl-sticky-handler.h"

G_DEFINE_INTERFACE(GowlStickyHandler, gowl_sticky_handler, G_TYPE_OBJECT)

static void
gowl_sticky_handler_default_init(GowlStickyHandlerInterface *iface)
{
	/* Default implementation - no-op */
	(void)iface;
}

/**
 * gowl_sticky_handler_is_sticky:
 * @self: a #GowlStickyHandler
 * @client: (nullable): the client to check
 *
 * Returns whether the given client is sticky (visible on all tags).
 *
 * Returns: %TRUE if the client is sticky, %FALSE otherwise
 */
gboolean
gowl_sticky_handler_is_sticky(
	GowlStickyHandler *self,
	gpointer           client
){
	GowlStickyHandlerInterface *iface;

	g_return_val_if_fail(GOWL_IS_STICKY_HANDLER(self), FALSE);

	iface = GOWL_STICKY_HANDLER_GET_IFACE(self);
	if (iface->is_sticky != NULL)
		return iface->is_sticky(self, client);
	return FALSE;
}

/**
 * gowl_sticky_handler_set_sticky:
 * @self: a #GowlStickyHandler
 * @client: (nullable): the client to modify
 * @sticky: %TRUE to make the client sticky, %FALSE to unstick it
 *
 * Sets whether the given client is sticky (visible on all tags).
 */
void
gowl_sticky_handler_set_sticky(
	GowlStickyHandler *self,
	gpointer           client,
	gboolean           sticky
){
	GowlStickyHandlerInterface *iface;

	g_return_if_fail(GOWL_IS_STICKY_HANDLER(self));

	iface = GOWL_STICKY_HANDLER_GET_IFACE(self);
	if (iface->set_sticky != NULL)
		iface->set_sticky(self, client, sticky);
}
