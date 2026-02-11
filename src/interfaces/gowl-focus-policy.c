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

#include "gowl-focus-policy.h"

G_DEFINE_INTERFACE(GowlFocusPolicy, gowl_focus_policy, G_TYPE_OBJECT)

static void
gowl_focus_policy_default_init(GowlFocusPolicyInterface *iface)
{
	/* Default implementation - no-op */
	(void)iface;
}

/**
 * gowl_focus_policy_focus_on_map:
 * @self: a #GowlFocusPolicy
 * @client: (nullable): the client that was just mapped
 *
 * Determines whether a newly mapped client should receive focus.
 *
 * Returns: %TRUE if the client should be focused on map, %FALSE otherwise
 */
gboolean
gowl_focus_policy_focus_on_map(
	GowlFocusPolicy *self,
	gpointer         client
){
	GowlFocusPolicyInterface *iface;

	g_return_val_if_fail(GOWL_IS_FOCUS_POLICY(self), FALSE);

	iface = GOWL_FOCUS_POLICY_GET_IFACE(self);
	if (iface->focus_on_map != NULL)
		return iface->focus_on_map(self, client);
	return FALSE;
}

/**
 * gowl_focus_policy_focus_on_hover:
 * @self: a #GowlFocusPolicy
 * @client: (nullable): the client being hovered over
 *
 * Determines whether a client should receive focus when the cursor
 * hovers over it (sloppy focus / focus-follows-mouse).
 *
 * Returns: %TRUE if the client should be focused on hover, %FALSE otherwise
 */
gboolean
gowl_focus_policy_focus_on_hover(
	GowlFocusPolicy *self,
	gpointer         client
){
	GowlFocusPolicyInterface *iface;

	g_return_val_if_fail(GOWL_IS_FOCUS_POLICY(self), FALSE);

	iface = GOWL_FOCUS_POLICY_GET_IFACE(self);
	if (iface->focus_on_hover != NULL)
		return iface->focus_on_hover(self, client);
	return FALSE;
}
