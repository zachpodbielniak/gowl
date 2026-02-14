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

#include "gowl-module-manager.h"
#include <gio/gio.h>

#include "interfaces/gowl-layout-provider.h"
#include "interfaces/gowl-keybind-handler.h"
#include "interfaces/gowl-mouse-handler.h"
#include "interfaces/gowl-startup-handler.h"
#include "interfaces/gowl-shutdown-handler.h"
#include "interfaces/gowl-ipc-handler.h"
#include "interfaces/gowl-gap-provider.h"

/**
 * GowlModuleManager:
 *
 * Central registry that owns every loaded #GowlModule.  It keeps
 * separate dispatch arrays for each hookable interface so that event
 * dispatch is a simple priority-ordered iteration rather than a
 * type-check on every module.
 */
struct _GowlModuleManager {
	GObject parent_instance;

	GPtrArray *modules;            /* element-type GowlModule* */
	GPtrArray *gmodules;           /* element-type GModule*     */

	/* Interface dispatch arrays -- one per hookable interface */
	GPtrArray *layout_providers;   /* element-type GowlLayoutProvider* */
	GPtrArray *keybind_handlers;   /* element-type GowlKeybindHandler* */
	GPtrArray *mouse_handlers;     /* element-type GowlMouseHandler*   */
	GPtrArray *startup_handlers;   /* element-type GowlStartupHandler* */
	GPtrArray *shutdown_handlers;  /* element-type GowlShutdownHandler* */
	GPtrArray *ipc_handlers;       /* element-type GowlIpcHandler*     */
	GPtrArray *gap_providers;      /* element-type GowlGapProvider*    */
};

G_DEFINE_FINAL_TYPE(GowlModuleManager, gowl_module_manager, G_TYPE_OBJECT)

/* --- Signal identifiers --- */
enum {
	SIGNAL_MODULE_LOADED,
	SIGNAL_MODULE_ACTIVATED,
	SIGNAL_MODULE_DEACTIVATED,
	SIGNAL_MODULE_ERROR,
	N_SIGNALS
};

static guint signals[N_SIGNALS] = { 0, };

/* --- Entry point symbol that every .so module must export --- */

/**
 * GowlModuleRegisterFunc:
 *
 * Signature for the "gowl_module_register" symbol that every shared
 * object module must export.  It returns the GType of the module's
 * #GowlModule subclass.
 */
typedef GType (*GowlModuleRegisterFunc)(void);

/* --- Priority comparison helper --- */

/**
 * compare_modules_by_priority:
 *
 * qsort-style comparator for GPtrArray: sorts modules in ascending
 * priority order (lower numerical value = higher dispatch priority).
 */
static gint
compare_modules_by_priority(
	gconstpointer a,
	gconstpointer b
){
	GowlModule *mod_a;
	GowlModule *mod_b;

	mod_a = *(GowlModule **)a;
	mod_b = *(GowlModule **)b;

	return gowl_module_get_priority(mod_a) - gowl_module_get_priority(mod_b);
}

/* --- Internal: sort a dispatch array by module priority --- */

static void
sort_dispatch_array(GPtrArray *array)
{
	if (array == NULL || array->len < 2)
		return;

	g_ptr_array_sort(array, compare_modules_by_priority);
}

/* --- GObject lifecycle --- */

static void
gowl_module_manager_dispose(GObject *object)
{
	GowlModuleManager *self;
	guint i;

	self = GOWL_MODULE_MANAGER(object);

	/* Deactivate all modules before tearing down */
	if (self->modules != NULL) {
		for (i = 0; i < self->modules->len; i++) {
			GowlModule *mod;

			mod = (GowlModule *)g_ptr_array_index(self->modules, i);
			if (gowl_module_get_is_active(mod))
				gowl_module_deactivate(mod);
		}
	}

	G_OBJECT_CLASS(gowl_module_manager_parent_class)->dispose(object);
}

static void
gowl_module_manager_finalize(GObject *object)
{
	GowlModuleManager *self;
	guint i;

	self = GOWL_MODULE_MANAGER(object);

	/* Free dispatch arrays (elements are borrowed refs from modules) */
	g_clear_pointer(&self->layout_providers, g_ptr_array_unref);
	g_clear_pointer(&self->keybind_handlers, g_ptr_array_unref);
	g_clear_pointer(&self->mouse_handlers, g_ptr_array_unref);
	g_clear_pointer(&self->startup_handlers, g_ptr_array_unref);
	g_clear_pointer(&self->shutdown_handlers, g_ptr_array_unref);
	g_clear_pointer(&self->ipc_handlers, g_ptr_array_unref);
	g_clear_pointer(&self->gap_providers, g_ptr_array_unref);

	/* Unref all module instances */
	g_clear_pointer(&self->modules, g_ptr_array_unref);

	/* Close all loaded shared objects */
	if (self->gmodules != NULL) {
		for (i = 0; i < self->gmodules->len; i++) {
			GModule *gmod;

			gmod = (GModule *)g_ptr_array_index(self->gmodules, i);
			if (gmod != NULL)
				g_module_close(gmod);
		}
		g_clear_pointer(&self->gmodules, g_ptr_array_unref);
	}

	G_OBJECT_CLASS(gowl_module_manager_parent_class)->finalize(object);
}

static void
gowl_module_manager_class_init(GowlModuleManagerClass *klass)
{
	GObjectClass *object_class;

	object_class = G_OBJECT_CLASS(klass);

	object_class->dispose = gowl_module_manager_dispose;
	object_class->finalize = gowl_module_manager_finalize;

	/**
	 * GowlModuleManager::module-loaded:
	 * @self: the #GowlModuleManager
	 * @module: the #GowlModule that was loaded
	 *
	 * Emitted after a module has been successfully loaded and registered.
	 */
	signals[SIGNAL_MODULE_LOADED] =
		g_signal_new("module-loaded",
		             G_TYPE_FROM_CLASS(klass),
		             G_SIGNAL_RUN_LAST,
		             0, NULL, NULL, NULL,
		             G_TYPE_NONE, 1, GOWL_TYPE_MODULE);

	/**
	 * GowlModuleManager::module-activated:
	 * @self: the #GowlModuleManager
	 * @module: the #GowlModule that was activated
	 *
	 * Emitted after a module has been successfully activated.
	 */
	signals[SIGNAL_MODULE_ACTIVATED] =
		g_signal_new("module-activated",
		             G_TYPE_FROM_CLASS(klass),
		             G_SIGNAL_RUN_LAST,
		             0, NULL, NULL, NULL,
		             G_TYPE_NONE, 1, GOWL_TYPE_MODULE);

	/**
	 * GowlModuleManager::module-deactivated:
	 * @self: the #GowlModuleManager
	 * @module: the #GowlModule that was deactivated
	 *
	 * Emitted after a module has been deactivated.
	 */
	signals[SIGNAL_MODULE_DEACTIVATED] =
		g_signal_new("module-deactivated",
		             G_TYPE_FROM_CLASS(klass),
		             G_SIGNAL_RUN_LAST,
		             0, NULL, NULL, NULL,
		             G_TYPE_NONE, 1, GOWL_TYPE_MODULE);

	/**
	 * GowlModuleManager::module-error:
	 * @self: the #GowlModuleManager
	 * @error_message: a human-readable description of the error
	 *
	 * Emitted when a module operation fails (load, activate, etc.).
	 */
	signals[SIGNAL_MODULE_ERROR] =
		g_signal_new("module-error",
		             G_TYPE_FROM_CLASS(klass),
		             G_SIGNAL_RUN_LAST,
		             0, NULL, NULL, NULL,
		             G_TYPE_NONE, 1, G_TYPE_STRING);
}

static void
gowl_module_manager_init(GowlModuleManager *self)
{
	self->modules = g_ptr_array_new_with_free_func(g_object_unref);
	self->gmodules = g_ptr_array_new();

	self->layout_providers  = g_ptr_array_new();
	self->keybind_handlers  = g_ptr_array_new();
	self->mouse_handlers    = g_ptr_array_new();
	self->startup_handlers  = g_ptr_array_new();
	self->shutdown_handlers = g_ptr_array_new();
	self->ipc_handlers      = g_ptr_array_new();
	self->gap_providers     = g_ptr_array_new();
}

/* --- Internal: classify a module into dispatch arrays --- */

/**
 * classify_module:
 * @self: the module manager
 * @mod: the module to classify
 *
 * Checks which hookable interfaces @mod implements and adds it to the
 * corresponding dispatch arrays.  After insertion the arrays are
 * re-sorted by priority.
 */
static void
classify_module(
	GowlModuleManager *self,
	GowlModule        *mod
){
	if (G_TYPE_CHECK_INSTANCE_TYPE(mod, GOWL_TYPE_LAYOUT_PROVIDER)) {
		g_ptr_array_add(self->layout_providers, (gpointer)mod);
		sort_dispatch_array(self->layout_providers);
	}

	if (G_TYPE_CHECK_INSTANCE_TYPE(mod, GOWL_TYPE_KEYBIND_HANDLER)) {
		g_ptr_array_add(self->keybind_handlers, (gpointer)mod);
		sort_dispatch_array(self->keybind_handlers);
	}

	if (G_TYPE_CHECK_INSTANCE_TYPE(mod, GOWL_TYPE_MOUSE_HANDLER)) {
		g_ptr_array_add(self->mouse_handlers, (gpointer)mod);
		sort_dispatch_array(self->mouse_handlers);
	}

	if (G_TYPE_CHECK_INSTANCE_TYPE(mod, GOWL_TYPE_STARTUP_HANDLER)) {
		g_ptr_array_add(self->startup_handlers, (gpointer)mod);
		sort_dispatch_array(self->startup_handlers);
	}

	if (G_TYPE_CHECK_INSTANCE_TYPE(mod, GOWL_TYPE_SHUTDOWN_HANDLER)) {
		g_ptr_array_add(self->shutdown_handlers, (gpointer)mod);
		sort_dispatch_array(self->shutdown_handlers);
	}

	if (G_TYPE_CHECK_INSTANCE_TYPE(mod, GOWL_TYPE_IPC_HANDLER)) {
		g_ptr_array_add(self->ipc_handlers, (gpointer)mod);
		sort_dispatch_array(self->ipc_handlers);
	}

	if (G_TYPE_CHECK_INSTANCE_TYPE(mod, GOWL_TYPE_GAP_PROVIDER)) {
		g_ptr_array_add(self->gap_providers, (gpointer)mod);
		sort_dispatch_array(self->gap_providers);
	}
}

/* --- Public API --- */

/**
 * gowl_module_manager_new:
 *
 * Creates a new, empty module manager.
 *
 * Returns: (transfer full): a new #GowlModuleManager
 */
GowlModuleManager *
gowl_module_manager_new(void)
{
	return (GowlModuleManager *)g_object_new(GOWL_TYPE_MODULE_MANAGER, NULL);
}

/**
 * gowl_module_manager_load_module:
 * @self: a #GowlModuleManager
 * @path: filesystem path to a shared object (.so) module
 * @error: (nullable): return location for a #GError
 *
 * Opens the shared object at @path, looks up the exported
 * "gowl_module_register" symbol, calls it to obtain the module's
 * #GType, then creates an instance and registers it.
 *
 * Returns: %TRUE on success, %FALSE on error
 */
gboolean
gowl_module_manager_load_module(
	GowlModuleManager *self,
	const gchar       *path,
	GError           **error
){
	GModule *gmod;
	GowlModuleRegisterFunc register_func;
	GType module_type;
	gboolean result;

	g_return_val_if_fail(GOWL_IS_MODULE_MANAGER(self), FALSE);
	g_return_val_if_fail(path != NULL, FALSE);

	/* Open the shared object with lazy binding */
	gmod = g_module_open(path, G_MODULE_BIND_LAZY);
	if (gmod == NULL) {
		g_set_error(error,
		            G_IO_ERROR, G_IO_ERROR_FAILED,
		            "Failed to load module '%s': %s",
		            path, g_module_error());
		g_signal_emit(self, signals[SIGNAL_MODULE_ERROR], 0,
		              g_module_error());
		return FALSE;
	}

	/* Look up the mandatory registration symbol */
	if (!g_module_symbol(gmod, "gowl_module_register",
	                     (gpointer *)&register_func)) {
		g_set_error(error,
		            G_IO_ERROR, G_IO_ERROR_FAILED,
		            "Module '%s' missing 'gowl_module_register' symbol: %s",
		            path, g_module_error());
		g_signal_emit(self, signals[SIGNAL_MODULE_ERROR], 0,
		              "Missing gowl_module_register symbol");
		g_module_close(gmod);
		return FALSE;
	}

	/* Call the registration function to get the GType */
	module_type = register_func();
	if (!g_type_is_a(module_type, GOWL_TYPE_MODULE)) {
		g_set_error(error,
		            G_IO_ERROR, G_IO_ERROR_FAILED,
		            "Module '%s': registered type is not a GowlModule subclass",
		            path);
		g_signal_emit(self, signals[SIGNAL_MODULE_ERROR], 0,
		              "Registered type is not a GowlModule subclass");
		g_module_close(gmod);
		return FALSE;
	}

	/* Keep the GModule handle alive for the lifetime of the manager */
	g_ptr_array_add(self->gmodules, (gpointer)gmod);

	/* Register the module type (creates instance, classifies, emits) */
	result = gowl_module_manager_register(self, module_type, error);

	return result;
}

/**
 * gowl_module_manager_register:
 * @self: a #GowlModuleManager
 * @module_type: the #GType of a #GowlModule subclass to instantiate
 * @error: (nullable): return location for a #GError
 *
 * Creates a new instance of @module_type, adds it to the internal
 * modules array, classifies it into the appropriate dispatch arrays
 * based on which interfaces it implements, and emits "module-loaded".
 *
 * Returns: %TRUE on success, %FALSE on error
 */
gboolean
gowl_module_manager_register(
	GowlModuleManager *self,
	GType              module_type,
	GError           **error
){
	GowlModule *mod;
	const gchar *name;

	g_return_val_if_fail(GOWL_IS_MODULE_MANAGER(self), FALSE);

	/* Validate the type is a concrete subclass of GowlModule */
	if (!g_type_is_a(module_type, GOWL_TYPE_MODULE)) {
		g_set_error(error,
		            G_IO_ERROR, G_IO_ERROR_FAILED,
		            "Type '%s' is not a GowlModule subclass",
		            g_type_name(module_type));
		return FALSE;
	}

	if (G_TYPE_IS_ABSTRACT(module_type)) {
		g_set_error(error,
		            G_IO_ERROR, G_IO_ERROR_FAILED,
		            "Cannot instantiate abstract type '%s'",
		            g_type_name(module_type));
		return FALSE;
	}

	/* Instantiate the module */
	mod = (GowlModule *)g_object_new(module_type, NULL);
	if (mod == NULL) {
		g_set_error(error,
		            G_IO_ERROR, G_IO_ERROR_FAILED,
		            "Failed to instantiate module type '%s'",
		            g_type_name(module_type));
		return FALSE;
	}

	/* Add to the master list (takes ownership of the ref) */
	g_ptr_array_add(self->modules, (gpointer)mod);

	/* Classify into interface-specific dispatch arrays */
	classify_module(self, mod);

	name = gowl_module_get_name(mod);
	g_debug("Module registered: %s", name != NULL ? name : "(unnamed)");

	g_signal_emit(self, signals[SIGNAL_MODULE_LOADED], 0, mod);

	return TRUE;
}

/**
 * gowl_module_manager_activate_all:
 * @self: a #GowlModuleManager
 *
 * Iterates over all registered modules and activates each one that is
 * not already active.  Emits "module-activated" for each successful
 * activation and "module-error" for each failure.
 */
void
gowl_module_manager_activate_all(GowlModuleManager *self)
{
	guint i;

	g_return_if_fail(GOWL_IS_MODULE_MANAGER(self));

	for (i = 0; i < self->modules->len; i++) {
		GowlModule *mod;
		const gchar *name;

		mod = (GowlModule *)g_ptr_array_index(self->modules, i);
		name = gowl_module_get_name(mod);

		if (gowl_module_get_is_active(mod))
			continue;

		if (gowl_module_activate(mod)) {
			g_debug("Module activated: %s",
			        name != NULL ? name : "(unnamed)");
			g_signal_emit(self, signals[SIGNAL_MODULE_ACTIVATED],
			              0, mod);
		} else {
			g_autofree gchar *msg = NULL;

			msg = g_strdup_printf("Failed to activate module: %s",
			                      name != NULL ? name : "(unnamed)");
			g_warning("%s", msg);
			g_signal_emit(self, signals[SIGNAL_MODULE_ERROR], 0, msg);
		}
	}
}

/**
 * gowl_module_manager_deactivate_all:
 * @self: a #GowlModuleManager
 *
 * Deactivates all currently active modules in reverse registration
 * order and emits "module-deactivated" for each.
 */
void
gowl_module_manager_deactivate_all(GowlModuleManager *self)
{
	gint i;

	g_return_if_fail(GOWL_IS_MODULE_MANAGER(self));

	/* Deactivate in reverse order for clean shutdown */
	for (i = (gint)self->modules->len - 1; i >= 0; i--) {
		GowlModule *mod;

		mod = (GowlModule *)g_ptr_array_index(self->modules, (guint)i);

		if (!gowl_module_get_is_active(mod))
			continue;

		gowl_module_deactivate(mod);
		g_signal_emit(self, signals[SIGNAL_MODULE_DEACTIVATED], 0, mod);
	}
}

/**
 * gowl_module_manager_get_modules:
 * @self: a #GowlModuleManager
 *
 * Returns a list of #GowlModuleInfo structures describing all
 * registered modules.  The caller owns the list and its elements and
 * must free them with g_list_free_full(list, (GDestroyNotify)gowl_module_info_free).
 *
 * Returns: (transfer full) (element-type GowlModuleInfo): list of module info
 */
GList *
gowl_module_manager_get_modules(GowlModuleManager *self)
{
	GList *list;
	guint i;

	g_return_val_if_fail(GOWL_IS_MODULE_MANAGER(self), NULL);

	list = NULL;

	for (i = 0; i < self->modules->len; i++) {
		GowlModule *mod;
		GowlModuleInfo *info;

		mod = (GowlModule *)g_ptr_array_index(self->modules, i);
		info = gowl_module_info_new(gowl_module_get_name(mod),
		                            gowl_module_get_description(mod),
		                            gowl_module_get_version(mod));
		list = g_list_prepend(list, (gpointer)info);
	}

	return g_list_reverse(list);
}

/**
 * gowl_module_manager_load_from_directory:
 * @self: a #GowlModuleManager
 * @dir_path: path to a directory containing .so module files
 *
 * Scans @dir_path for files ending in ".so" and attempts to load each
 * one via gowl_module_manager_load_module().  Errors for individual
 * modules are logged but do not prevent loading of the remaining
 * modules.
 */
void
gowl_module_manager_load_from_directory(
	GowlModuleManager *self,
	const gchar       *dir_path
){
	GDir *dir;
	GError *error;
	const gchar *filename;

	g_return_if_fail(GOWL_IS_MODULE_MANAGER(self));
	g_return_if_fail(dir_path != NULL);

	error = NULL;
	dir = g_dir_open(dir_path, 0, &error);
	if (dir == NULL) {
		g_warning("Cannot open module directory '%s': %s",
		          dir_path, error->message);
		g_error_free(error);
		return;
	}

	while ((filename = g_dir_read_name(dir)) != NULL) {
		g_autofree gchar *full_path = NULL;

		if (!g_str_has_suffix(filename, ".so"))
			continue;

		full_path = g_build_filename(dir_path, filename, NULL);

		error = NULL;
		if (!gowl_module_manager_load_module(self, full_path, &error)) {
			g_warning("Failed to load module '%s': %s",
			          full_path, error->message);
			g_error_free(error);
		}
	}

	g_dir_close(dir);
}

/**
 * gowl_module_manager_dispatch_key:
 * @self: a #GowlModuleManager
 * @modifiers: bitmask of active modifier keys
 * @keysym: the keysym for the key event
 * @pressed: %TRUE for key press, %FALSE for release
 *
 * Dispatches a keyboard event to all registered keybind handlers in
 * priority order.  If any handler returns %TRUE the event is considered
 * consumed and no further handlers are called.
 *
 * Returns: %TRUE if a handler consumed the event
 */
gboolean
gowl_module_manager_dispatch_key(
	GowlModuleManager *self,
	guint              modifiers,
	guint              keysym,
	gboolean           pressed
){
	guint i;

	g_return_val_if_fail(GOWL_IS_MODULE_MANAGER(self), FALSE);

	for (i = 0; i < self->keybind_handlers->len; i++) {
		GowlKeybindHandler *handler;

		handler = (GowlKeybindHandler *)g_ptr_array_index(
			self->keybind_handlers, i);

		/* Only dispatch to active modules */
		if (!gowl_module_get_is_active(GOWL_MODULE(handler)))
			continue;

		if (gowl_keybind_handler_handle_key(handler, modifiers,
		                                    keysym, pressed))
			return TRUE;
	}

	return FALSE;
}

/**
 * gowl_module_manager_dispatch_button:
 * @self: a #GowlModuleManager
 * @button: the mouse button
 * @state: the button state
 * @modifiers: bitmask of active modifier keys
 *
 * Dispatches a mouse button event to all registered mouse handlers in
 * priority order.  Stops at the first handler that returns %TRUE.
 *
 * Returns: %TRUE if a handler consumed the event
 */
gboolean
gowl_module_manager_dispatch_button(
	GowlModuleManager *self,
	guint              button,
	guint              state,
	guint              modifiers
){
	guint i;

	g_return_val_if_fail(GOWL_IS_MODULE_MANAGER(self), FALSE);

	for (i = 0; i < self->mouse_handlers->len; i++) {
		GowlMouseHandler *handler;

		handler = (GowlMouseHandler *)g_ptr_array_index(
			self->mouse_handlers, i);

		if (!gowl_module_get_is_active(GOWL_MODULE(handler)))
			continue;

		if (gowl_mouse_handler_handle_button(handler, button,
		                                     state, modifiers))
			return TRUE;
	}

	return FALSE;
}

/**
 * gowl_module_manager_dispatch_startup:
 * @self: a #GowlModuleManager
 * @compositor: (nullable): the compositor instance passed to each handler
 *
 * Broadcasts the startup event to all registered startup handlers.
 * Every handler is called regardless of return value (broadcast
 * semantics, not consumable).
 */
void
gowl_module_manager_dispatch_startup(
	GowlModuleManager *self,
	gpointer           compositor
){
	guint i;

	g_return_if_fail(GOWL_IS_MODULE_MANAGER(self));

	for (i = 0; i < self->startup_handlers->len; i++) {
		GowlStartupHandler *handler;

		handler = (GowlStartupHandler *)g_ptr_array_index(
			self->startup_handlers, i);

		if (!gowl_module_get_is_active(GOWL_MODULE(handler)))
			continue;

		gowl_startup_handler_on_startup(handler, compositor);
	}
}

/**
 * gowl_module_manager_dispatch_shutdown:
 * @self: a #GowlModuleManager
 * @compositor: (nullable): the compositor instance passed to each handler
 *
 * Broadcasts the shutdown event to all registered shutdown handlers.
 * Every handler is called regardless of return value (broadcast
 * semantics).
 */
void
gowl_module_manager_dispatch_shutdown(
	GowlModuleManager *self,
	gpointer           compositor
){
	guint i;

	g_return_if_fail(GOWL_IS_MODULE_MANAGER(self));

	for (i = 0; i < self->shutdown_handlers->len; i++) {
		GowlShutdownHandler *handler;

		handler = (GowlShutdownHandler *)g_ptr_array_index(
			self->shutdown_handlers, i);

		if (!gowl_module_get_is_active(GOWL_MODULE(handler)))
			continue;

		gowl_shutdown_handler_on_shutdown(handler, compositor);
	}
}

/**
 * gowl_module_manager_get_gaps:
 * @self: a #GowlModuleManager
 * @monitor: the monitor being laid out
 * @inner_h: (out): horizontal gap between clients
 * @inner_v: (out): vertical gap between clients
 * @outer_h: (out): horizontal gap at screen edges
 * @outer_v: (out): vertical gap at screen edges
 *
 * Queries the first active gap provider for gap values.
 * If no gap provider is registered, all outputs are set to 0.
 *
 * Returns: %TRUE if a gap provider was found
 */
gboolean
gowl_module_manager_get_gaps(
	GowlModuleManager *self,
	gpointer           monitor,
	gint              *inner_h,
	gint              *inner_v,
	gint              *outer_h,
	gint              *outer_v
){
	guint i;

	g_return_val_if_fail(GOWL_IS_MODULE_MANAGER(self), FALSE);

	/* Default to zero gaps */
	if (inner_h != NULL) *inner_h = 0;
	if (inner_v != NULL) *inner_v = 0;
	if (outer_h != NULL) *outer_h = 0;
	if (outer_v != NULL) *outer_v = 0;

	/* Query the first active gap provider */
	for (i = 0; i < self->gap_providers->len; i++) {
		GowlGapProvider *provider;

		provider = (GowlGapProvider *)g_ptr_array_index(
			self->gap_providers, i);

		if (!gowl_module_get_is_active(GOWL_MODULE(provider)))
			continue;

		gowl_gap_provider_get_gaps(provider, monitor,
		                           inner_h, inner_v,
		                           outer_h, outer_v);
		return TRUE;
	}

	return FALSE;
}

/**
 * gowl_module_manager_configure_all:
 * @self: a #GowlModuleManager
 * @module_configs: a #GHashTable mapping module names (gchar*) to
 *   per-module #GHashTable<gchar*,gchar*> settings.  Typically
 *   obtained from gowl_config_get_all_module_configs().
 *
 * Iterates all registered modules and calls gowl_module_configure()
 * on each one that has a matching entry in @module_configs.
 */
void
gowl_module_manager_configure_all(
	GowlModuleManager *self,
	GHashTable        *module_configs
){
	guint i;

	g_return_if_fail(GOWL_IS_MODULE_MANAGER(self));

	if (module_configs == NULL)
		return;

	for (i = 0; i < self->modules->len; i++) {
		GowlModule *mod;
		const gchar *mod_name;
		GHashTable *mod_cfg;

		mod = (GowlModule *)g_ptr_array_index(self->modules, i);
		mod_name = gowl_module_get_name(mod);
		if (mod_name == NULL)
			continue;

		mod_cfg = (GHashTable *)g_hash_table_lookup(
			module_configs, mod_name);
		if (mod_cfg != NULL) {
			g_message("Configuring module '%s' with %u settings",
			          mod_name, g_hash_table_size(mod_cfg));
			gowl_module_configure(mod, (gpointer)mod_cfg);
		}
	}
}
