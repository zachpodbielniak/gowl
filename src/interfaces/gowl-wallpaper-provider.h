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

#ifndef GOWL_WALLPAPER_PROVIDER_H
#define GOWL_WALLPAPER_PROVIDER_H

#include <glib-object.h>

G_BEGIN_DECLS

#define GOWL_TYPE_WALLPAPER_PROVIDER (gowl_wallpaper_provider_get_type())

G_DECLARE_INTERFACE(GowlWallpaperProvider, gowl_wallpaper_provider, GOWL, WALLPAPER_PROVIDER, GObject)

/**
 * GowlWallpaperProviderInterface:
 * @parent_iface: the parent interface
 * @on_output: called when a monitor is added or its geometry changes
 * @on_output_destroy: called when a monitor is about to be destroyed
 *
 * Interface for modules that provide desktop wallpaper images.
 * The compositor dispatches @on_output from on_layout_change for
 * every monitor, and @on_output_destroy from on_monitor_destroy
 * before the scene output is torn down.
 */
struct _GowlWallpaperProviderInterface {
	GTypeInterface parent_iface;

	void (*on_output)         (GowlWallpaperProvider *self,
	                           gpointer               compositor,
	                           gpointer               monitor);

	void (*on_output_destroy) (GowlWallpaperProvider *self,
	                           gpointer               monitor);
};

/**
 * gowl_wallpaper_provider_on_output:
 * @self: a #GowlWallpaperProvider
 * @compositor: (nullable): the #GowlCompositor instance
 * @monitor: (nullable): the #GowlMonitor being configured
 *
 * Notifies the provider that a monitor has been added or its
 * geometry has changed.  The provider should create or update
 * its wallpaper scene node for this monitor.
 */
void gowl_wallpaper_provider_on_output (GowlWallpaperProvider *self,
                                        gpointer               compositor,
                                        gpointer               monitor);

/**
 * gowl_wallpaper_provider_on_output_destroy:
 * @self: a #GowlWallpaperProvider
 * @monitor: (nullable): the #GowlMonitor being destroyed
 *
 * Notifies the provider that a monitor is about to be destroyed.
 * The provider should clean up any scene nodes associated with
 * this monitor.
 */
void gowl_wallpaper_provider_on_output_destroy (GowlWallpaperProvider *self,
                                                gpointer               monitor);

G_END_DECLS

#endif /* GOWL_WALLPAPER_PROVIDER_H */
