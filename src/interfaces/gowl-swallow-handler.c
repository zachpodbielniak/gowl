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

#include "gowl-swallow-handler.h"

G_DEFINE_INTERFACE(GowlSwallowHandler, gowl_swallow_handler, G_TYPE_OBJECT)

static void
gowl_swallow_handler_default_init(GowlSwallowHandlerInterface *iface)
{
	/* Default implementation - no-op */
	(void)iface;
}

/**
 * gowl_swallow_handler_should_swallow:
 * @self: a #GowlSwallowHandler
 * @parent: (nullable): the parent client (e.g. terminal)
 * @child: (nullable): the child client spawned by the parent
 *
 * Determines whether the parent window should be swallowed (hidden)
 * when the child window appears.
 *
 * Returns: %TRUE if the parent should be swallowed, %FALSE otherwise
 */
gboolean
gowl_swallow_handler_should_swallow(
	GowlSwallowHandler *self,
	gpointer            parent,
	gpointer            child
){
	GowlSwallowHandlerInterface *iface;

	g_return_val_if_fail(GOWL_IS_SWALLOW_HANDLER(self), FALSE);

	iface = GOWL_SWALLOW_HANDLER_GET_IFACE(self);
	if (iface->should_swallow != NULL)
		return iface->should_swallow(self, parent, child);
	return FALSE;
}

/**
 * gowl_swallow_handler_swallow:
 * @self: a #GowlSwallowHandler
 * @parent: (nullable): the parent client to hide
 * @child: (nullable): the child client that replaces it
 *
 * Performs the swallow operation, hiding the parent and placing
 * the child in its position.
 */
void
gowl_swallow_handler_swallow(
	GowlSwallowHandler *self,
	gpointer            parent,
	gpointer            child
){
	GowlSwallowHandlerInterface *iface;

	g_return_if_fail(GOWL_IS_SWALLOW_HANDLER(self));

	iface = GOWL_SWALLOW_HANDLER_GET_IFACE(self);
	if (iface->swallow != NULL)
		iface->swallow(self, parent, child);
}

/**
 * gowl_swallow_handler_unswallow:
 * @self: a #GowlSwallowHandler
 * @parent: (nullable): the parent client to restore
 *
 * Reverses a swallow operation, restoring the parent client
 * after the child has been destroyed.
 */
void
gowl_swallow_handler_unswallow(
	GowlSwallowHandler *self,
	gpointer            parent
){
	GowlSwallowHandlerInterface *iface;

	g_return_if_fail(GOWL_IS_SWALLOW_HANDLER(self));

	iface = GOWL_SWALLOW_HANDLER_GET_IFACE(self);
	if (iface->unswallow != NULL)
		iface->unswallow(self, parent);
}
