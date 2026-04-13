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

#include "gowl-dropdown-provider.h"

G_DEFINE_INTERFACE(GowlDropdownProvider, gowl_dropdown_provider, G_TYPE_OBJECT)

static void
gowl_dropdown_provider_default_init(GowlDropdownProviderInterface *iface)
{
	(void)iface;
}

/**
 * gowl_dropdown_provider_toggle_by_name:
 * @self: a #GowlDropdownProvider
 * @name: the dropdown identifier to toggle
 *
 * Dispatch wrapper for the interface's @toggle_by_name vfunc.
 * Returns %FALSE if the implementation doesn't install a
 * handler or if the name was not found.
 */
gboolean
gowl_dropdown_provider_toggle_by_name(
	GowlDropdownProvider *self,
	const gchar          *name
){
	GowlDropdownProviderInterface *iface;

	g_return_val_if_fail(GOWL_IS_DROPDOWN_PROVIDER(self), FALSE);

	iface = GOWL_DROPDOWN_PROVIDER_GET_IFACE(self);
	if (iface->toggle_by_name != NULL)
		return iface->toggle_by_name(self, name);
	return FALSE;
}

/**
 * gowl_dropdown_provider_refresh:
 * @self: a #GowlDropdownProvider
 *
 * Dispatch wrapper for the interface's @refresh vfunc.  Returns
 * the number of newly adopted entries, or 0 if the
 * implementation doesn't install a handler.
 */
guint
gowl_dropdown_provider_refresh(GowlDropdownProvider *self)
{
	GowlDropdownProviderInterface *iface;

	g_return_val_if_fail(GOWL_IS_DROPDOWN_PROVIDER(self), 0);

	iface = GOWL_DROPDOWN_PROVIDER_GET_IFACE(self);
	if (iface->refresh != NULL)
		return iface->refresh(self);
	return 0;
}
