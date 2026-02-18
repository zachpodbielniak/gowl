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

#include "gowl-wallpaper-provider.h"

G_DEFINE_INTERFACE(GowlWallpaperProvider, gowl_wallpaper_provider, G_TYPE_OBJECT)

static void
gowl_wallpaper_provider_default_init(GowlWallpaperProviderInterface *iface)
{
	/* Default implementation - no-op */
	(void)iface;
}

/**
 * gowl_wallpaper_provider_on_output:
 * @self: a #GowlWallpaperProvider
 * @compositor: (nullable): the compositor instance
 * @monitor: (nullable): the monitor being configured
 *
 * Notifies the provider that a monitor has been added or its
 * geometry has changed.  The provider should create or update
 * its wallpaper scene node for this monitor.
 */
void
gowl_wallpaper_provider_on_output(
	GowlWallpaperProvider *self,
	gpointer               compositor,
	gpointer               monitor
){
	GowlWallpaperProviderInterface *iface;

	g_return_if_fail(GOWL_IS_WALLPAPER_PROVIDER(self));

	iface = GOWL_WALLPAPER_PROVIDER_GET_IFACE(self);
	if (iface->on_output != NULL)
		iface->on_output(self, compositor, monitor);
}

/**
 * gowl_wallpaper_provider_on_output_destroy:
 * @self: a #GowlWallpaperProvider
 * @monitor: (nullable): the monitor being destroyed
 *
 * Notifies the provider that a monitor is about to be destroyed.
 * The provider should clean up any scene nodes associated with
 * this monitor.
 */
void
gowl_wallpaper_provider_on_output_destroy(
	GowlWallpaperProvider *self,
	gpointer               monitor
){
	GowlWallpaperProviderInterface *iface;

	g_return_if_fail(GOWL_IS_WALLPAPER_PROVIDER(self));

	iface = GOWL_WALLPAPER_PROVIDER_GET_IFACE(self);
	if (iface->on_output_destroy != NULL)
		iface->on_output_destroy(self, monitor);
}
