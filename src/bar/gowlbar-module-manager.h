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

#ifndef GOWLBAR_MODULE_MANAGER_H
#define GOWLBAR_MODULE_MANAGER_H

#include <glib-object.h>
#include <gmodule.h>

#include "gowlbar-module.h"

G_BEGIN_DECLS

#define GOWLBAR_TYPE_MODULE_MANAGER (gowlbar_module_manager_get_type())

G_DECLARE_FINAL_TYPE(GowlbarModuleManager, gowlbar_module_manager,
                     GOWLBAR, MODULE_MANAGER, GObject)

/**
 * gowlbar_module_manager_new:
 *
 * Creates a new bar module manager.
 *
 * Returns: (transfer full): a new #GowlbarModuleManager
 */
GowlbarModuleManager *gowlbar_module_manager_new(void);

/**
 * gowlbar_module_manager_load_module:
 * @self: the module manager
 * @path: path to the .so file
 * @error: (nullable): return location for a #GError
 *
 * Loads a single module from the given shared object path.
 * The .so must export `GType gowlbar_module_register(void)`.
 *
 * Returns: %TRUE on success
 */
gboolean gowlbar_module_manager_load_module(
	GowlbarModuleManager *self,
	const gchar          *path,
	GError              **error
);

/**
 * gowlbar_module_manager_load_from_directory:
 * @self: the module manager
 * @dir_path: directory to scan for .so module files
 *
 * Scans a directory and loads all .so files as modules.
 */
void gowlbar_module_manager_load_from_directory(
	GowlbarModuleManager *self,
	const gchar          *dir_path
);

/**
 * gowlbar_module_manager_activate_all:
 * @self: the module manager
 *
 * Activates all loaded modules.
 */
void gowlbar_module_manager_activate_all(GowlbarModuleManager *self);

/**
 * gowlbar_module_manager_deactivate_all:
 * @self: the module manager
 *
 * Deactivates all loaded modules.
 */
void gowlbar_module_manager_deactivate_all(GowlbarModuleManager *self);

/**
 * gowlbar_module_manager_get_modules:
 * @self: the module manager
 *
 * Returns: (transfer none) (element-type GowlbarModule): the loaded modules
 */
GList *gowlbar_module_manager_get_modules(GowlbarModuleManager *self);

/**
 * gowlbar_module_manager_dispatch_startup:
 * @self: the module manager
 * @app: the bar application object
 *
 * Dispatches startup event to all modules implementing
 * GowlbarStartupHandler.
 */
void gowlbar_module_manager_dispatch_startup(
	GowlbarModuleManager *self,
	gpointer              app
);

/**
 * gowlbar_module_manager_dispatch_shutdown:
 * @self: the module manager
 * @app: the bar application object
 *
 * Dispatches shutdown event to all modules implementing
 * GowlbarShutdownHandler.
 */
void gowlbar_module_manager_dispatch_shutdown(
	GowlbarModuleManager *self,
	gpointer              app
);

G_END_DECLS

#endif /* GOWLBAR_MODULE_MANAGER_H */
