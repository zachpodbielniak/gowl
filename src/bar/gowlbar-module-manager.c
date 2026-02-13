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

#include "gowlbar-module-manager.h"
#include "interfaces/gowlbar-startup-handler.h"
#include "interfaces/gowlbar-shutdown-handler.h"

#include <gio/gio.h>
#include <gmodule.h>

/**
 * GowlbarModuleManager:
 *
 * Manages the lifecycle of bar extension modules.  Loads .so plugins
 * that export `GType gowlbar_module_register(void)`, activates them,
 * and dispatches lifecycle events to interface implementors.
 */
struct _GowlbarModuleManager {
	GObject parent_instance;

	GList   *modules;  /* GList of GowlbarModule* */
};

G_DEFINE_FINAL_TYPE(GowlbarModuleManager, gowlbar_module_manager,
                    G_TYPE_OBJECT)

/* --- GObject lifecycle --- */

static void
gowlbar_module_manager_finalize(GObject *object)
{
	GowlbarModuleManager *self;

	self = GOWLBAR_MODULE_MANAGER(object);

	/* Deactivate and free all modules */
	g_list_free_full(self->modules, g_object_unref);
	self->modules = NULL;

	G_OBJECT_CLASS(gowlbar_module_manager_parent_class)->finalize(object);
}

static void
gowlbar_module_manager_class_init(GowlbarModuleManagerClass *klass)
{
	GObjectClass *object_class;

	object_class = G_OBJECT_CLASS(klass);

	object_class->finalize = gowlbar_module_manager_finalize;
}

static void
gowlbar_module_manager_init(GowlbarModuleManager *self)
{
	self->modules = NULL;
}

/* --- Public API --- */

/**
 * gowlbar_module_manager_new:
 *
 * Creates a new bar module manager.
 *
 * Returns: (transfer full): a new #GowlbarModuleManager
 */
GowlbarModuleManager *
gowlbar_module_manager_new(void)
{
	return (GowlbarModuleManager *)g_object_new(
		GOWLBAR_TYPE_MODULE_MANAGER, NULL);
}

/**
 * gowlbar_module_manager_load_module:
 * @self: the module manager
 * @path: path to the .so file
 * @error: (nullable): return location for a #GError
 *
 * Loads a module from a shared object.  The .so must export
 * `GType gowlbar_module_register(void)` which returns the
 * GType of the module to instantiate.
 *
 * Returns: %TRUE on success
 */
gboolean
gowlbar_module_manager_load_module(
	GowlbarModuleManager  *self,
	const gchar            *path,
	GError                **error
){
	GModule *gmodule;
	gpointer symbol;
	GType (*register_fn)(void);
	GType module_type;
	GowlbarModule *module;

	g_return_val_if_fail(GOWLBAR_IS_MODULE_MANAGER(self), FALSE);
	g_return_val_if_fail(path != NULL, FALSE);

	gmodule = g_module_open(path, G_MODULE_BIND_LAZY);
	if (gmodule == NULL) {
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
		            "Failed to open module '%s': %s",
		            path, g_module_error());
		return FALSE;
	}

	if (!g_module_symbol(gmodule, "gowlbar_module_register", &symbol)) {
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
		            "Symbol 'gowlbar_module_register' not found in '%s'",
		            path);
		g_module_close(gmodule);
		return FALSE;
	}

	register_fn = (GType (*)(void))symbol;
	module_type = register_fn();

	if (!g_type_is_a(module_type, GOWLBAR_TYPE_MODULE)) {
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
		            "Module type from '%s' is not a GowlbarModule",
		            path);
		g_module_close(gmodule);
		return FALSE;
	}

	module = (GowlbarModule *)g_object_new(module_type, NULL);
	self->modules = g_list_append(self->modules, module);

	/* Keep the module open so symbols remain available */
	g_module_make_resident(gmodule);

	g_debug("gowlbar: loaded module '%s' from %s",
	        gowlbar_module_get_name(module), path);

	return TRUE;
}

/**
 * gowlbar_module_manager_load_from_directory:
 * @self: the module manager
 * @dir_path: directory to scan for .so module files
 *
 * Scans @dir_path for files ending in ".so" and attempts to load
 * each one as a bar module.
 */
void
gowlbar_module_manager_load_from_directory(
	GowlbarModuleManager *self,
	const gchar          *dir_path
){
	GDir *dir;
	const gchar *filename;

	g_return_if_fail(GOWLBAR_IS_MODULE_MANAGER(self));
	g_return_if_fail(dir_path != NULL);

	dir = g_dir_open(dir_path, 0, NULL);
	if (dir == NULL)
		return;

	while ((filename = g_dir_read_name(dir)) != NULL) {
		g_autofree gchar *path = NULL;
		g_autoptr(GError) error = NULL;

		if (!g_str_has_suffix(filename, ".so"))
			continue;

		path = g_build_filename(dir_path, filename, NULL);

		if (!gowlbar_module_manager_load_module(self, path, &error)) {
			g_warning("gowlbar: failed to load module '%s': %s",
			          path, error->message);
		}
	}

	g_dir_close(dir);
}

/**
 * gowlbar_module_manager_activate_all:
 * @self: the module manager
 *
 * Activates all loaded modules.
 */
void
gowlbar_module_manager_activate_all(GowlbarModuleManager *self)
{
	GList *l;

	g_return_if_fail(GOWLBAR_IS_MODULE_MANAGER(self));

	for (l = self->modules; l != NULL; l = l->next) {
		GowlbarModule *module;

		module = (GowlbarModule *)l->data;
		if (!gowlbar_module_activate(module)) {
			g_warning("gowlbar: module '%s' failed to activate",
			          gowlbar_module_get_name(module));
		}
	}
}

/**
 * gowlbar_module_manager_deactivate_all:
 * @self: the module manager
 *
 * Deactivates all loaded modules.
 */
void
gowlbar_module_manager_deactivate_all(GowlbarModuleManager *self)
{
	GList *l;

	g_return_if_fail(GOWLBAR_IS_MODULE_MANAGER(self));

	for (l = self->modules; l != NULL; l = l->next) {
		GowlbarModule *module;

		module = (GowlbarModule *)l->data;
		gowlbar_module_deactivate(module);
	}
}

/**
 * gowlbar_module_manager_get_modules:
 * @self: the module manager
 *
 * Returns: (transfer none) (element-type GowlbarModule): loaded modules
 */
GList *
gowlbar_module_manager_get_modules(GowlbarModuleManager *self)
{
	g_return_val_if_fail(GOWLBAR_IS_MODULE_MANAGER(self), NULL);

	return self->modules;
}

/**
 * gowlbar_module_manager_dispatch_startup:
 * @self: the module manager
 * @app: the bar application object
 *
 * Calls on_startup on all modules implementing GowlbarStartupHandler.
 */
void
gowlbar_module_manager_dispatch_startup(
	GowlbarModuleManager *self,
	gpointer              app
){
	GList *l;

	g_return_if_fail(GOWLBAR_IS_MODULE_MANAGER(self));

	for (l = self->modules; l != NULL; l = l->next) {
		GowlbarModule *module;

		module = (GowlbarModule *)l->data;

		if (GOWLBAR_IS_STARTUP_HANDLER(module)) {
			gowlbar_startup_handler_on_startup(
				GOWLBAR_STARTUP_HANDLER(module), app);
		}
	}
}

/**
 * gowlbar_module_manager_dispatch_shutdown:
 * @self: the module manager
 * @app: the bar application object
 *
 * Calls on_shutdown on all modules implementing GowlbarShutdownHandler.
 */
void
gowlbar_module_manager_dispatch_shutdown(
	GowlbarModuleManager *self,
	gpointer              app
){
	GList *l;

	g_return_if_fail(GOWLBAR_IS_MODULE_MANAGER(self));

	for (l = self->modules; l != NULL; l = l->next) {
		GowlbarModule *module;

		module = (GowlbarModule *)l->data;

		if (GOWLBAR_IS_SHUTDOWN_HANDLER(module)) {
			gowlbar_shutdown_handler_on_shutdown(
				GOWLBAR_SHUTDOWN_HANDLER(module), app);
		}
	}
}
