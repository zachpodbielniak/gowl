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

#include "gowl-session-provider.h"

G_DEFINE_INTERFACE(GowlSessionProvider, gowl_session_provider, G_TYPE_OBJECT)

static void
gowl_session_provider_default_init(GowlSessionProviderInterface *iface)
{
	(void)iface;
}

gboolean
gowl_session_provider_save(GowlSessionProvider *self,
                            gpointer             compositor,
                            GFile               *dest,
                            GError             **error)
{
	GowlSessionProviderInterface *iface;

	g_return_val_if_fail(GOWL_IS_SESSION_PROVIDER(self), FALSE);
	g_return_val_if_fail(G_IS_FILE(dest), FALSE);

	iface = GOWL_SESSION_PROVIDER_GET_IFACE(self);
	if (iface->save == NULL) {
		g_set_error_literal(error, G_IO_ERROR,
		                    G_IO_ERROR_NOT_SUPPORTED,
		                    "session provider has no save vfunc");
		return FALSE;
	}

	return iface->save(self, compositor, dest, error);
}

gboolean
gowl_session_provider_load(GowlSessionProvider *self,
                            gpointer             compositor,
                            GFile               *src,
                            GError             **error)
{
	GowlSessionProviderInterface *iface;

	g_return_val_if_fail(GOWL_IS_SESSION_PROVIDER(self), FALSE);
	g_return_val_if_fail(G_IS_FILE(src), FALSE);

	iface = GOWL_SESSION_PROVIDER_GET_IFACE(self);
	if (iface->load == NULL) {
		g_set_error_literal(error, G_IO_ERROR,
		                    G_IO_ERROR_NOT_SUPPORTED,
		                    "session provider has no load vfunc");
		return FALSE;
	}

	return iface->load(self, compositor, src, error);
}
