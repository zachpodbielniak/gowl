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

#ifndef GOWL_BAR_REGISTRY_H
#define GOWL_BAR_REGISTRY_H

#include <glib-object.h>
/* A plugin file declares its query symbol G_MODULE_EXPORT, so the
   header that defines the symbol's contract has to supply it. */
#include <gmodule.h>

#include "barkit/gowl-barkit-types.h"
#include "barkit/gowl-bar-plugin.h"
#include "barkit/gowl-bar-plugin-proxy.h"

G_BEGIN_DECLS

/**
 * GowlBarPluginFactory:
 * @name: the registered name being instantiated
 * @param: (nullable): the text after the colon in the spec, e.g. the
 *   `/var' of `disk:/var'
 * @user_data: the pointer given at registration
 *
 * Builds a plugin instance.  A factory rather than a plain #GType lets
 * one registration cover a family --- every system reading in the bar
 * comes from one factory keyed on @name.
 *
 * Returns: (transfer full) (nullable): a new plugin, or %NULL
 */
typedef GowlBarPlugin *(*GowlBarPluginFactory) (const gchar *name,
                                                 const gchar *param,
                                                 gpointer     user_data);

/**
 * GowlBarPluginDesc:
 * @abi: must equal %GOWL_BAR_PLUGIN_ABI
 * @name: the registry name, e.g. `spotify'
 * @title: (nullable): a human-readable name
 * @description: (nullable): one line describing the plugin
 * @version: (nullable): the plugin's own version string
 * @vtable: (nullable): the callback table; the preferred form
 * @factory: (nullable): an alternative to @vtable for a plugin that
 *   wants to be a real #GObject subclass
 * @reserved: must be zeroed
 *
 * What a loadable plugin file publishes about itself.
 */
typedef struct {
	guint                      abi;
	const gchar               *name;
	const gchar               *title;
	const gchar               *description;
	const gchar               *version;
	const GowlBarPluginVTable *vtable;
	GowlBarPluginFactory       factory;
	gpointer                   reserved[4];
} GowlBarPluginDesc;

/**
 * GowlBarPluginQueryFunc:
 * @n_descs: (out): how many descriptors were returned
 *
 * The symbol a plugin file exports, named
 * `gowl_bar_plugin_query'.  One file may publish several plugins ---
 * a `system.c' that provides cpu, memory and disk is one compile and
 * one reload rather than three.
 *
 * Returns: (array length=n_descs) (transfer none): the descriptors,
 *   which must stay valid for as long as the file is loaded
 */
typedef const GowlBarPluginDesc *(*GowlBarPluginQueryFunc) (guint *n_descs);

#define GOWL_TYPE_BAR_REGISTRY (gowl_bar_registry_get_type())

G_DECLARE_FINAL_TYPE(GowlBarRegistry, gowl_bar_registry,
                     GOWL, BAR_REGISTRY, GObject)

/**
 * gowl_bar_registry_new:
 * @state_dir: (nullable): a writable directory for the load journal,
 *   or %NULL to derive one from the XDG state directory
 *
 * Returns: (transfer full): a new registry
 */
GowlBarRegistry *gowl_bar_registry_new (const gchar *state_dir);

/* --- Registration ------------------------------------------------- */

/**
 * gowl_bar_registry_register_type:
 * @self: a registry
 * @name: the registry name
 * @type: a #GowlBarPlugin subtype
 * @title: (nullable): a human-readable name
 * @description: (nullable): one line describing the plugin
 *
 * Registers a plugin compiled into the host.  Built-ins cannot be
 * unloaded --- there is nothing to unload --- but they can be
 * quarantined, so a built-in that faults is still kept out of the bar
 * on the next start.
 */
void gowl_bar_registry_register_type (GowlBarRegistry *self,
                                       const gchar     *name,
                                       GType            type,
                                       const gchar     *title,
                                       const gchar     *description);

/**
 * gowl_bar_registry_register_factory:
 * @self: a registry
 * @name: the registry name
 * @factory: builds instances
 * @user_data: passed to @factory
 * @destroy: (nullable): frees @user_data when the entry goes away
 * @title: (nullable): a human-readable name
 * @description: (nullable): one line describing the plugin
 */
void gowl_bar_registry_register_factory (GowlBarRegistry      *self,
                                          const gchar          *name,
                                          GowlBarPluginFactory  factory,
                                          gpointer              user_data,
                                          GDestroyNotify        destroy,
                                          const gchar          *title,
                                          const gchar          *description);

/**
 * gowl_bar_registry_register_vtable:
 * @self: a registry
 * @name: the registry name
 * @title: (nullable): a human-readable name
 * @description: (nullable): one line describing the plugin
 * @vtable: the callbacks; must outlive the registry
 *
 * Registers a plain-C plugin the host itself supplies.  The shipped
 * plugins use this rather than defining a #GType each: a bar widget is
 * a poll function and a panel builder, and a class per widget would be
 * three hundred lines of boilerplate for no behaviour.
 */
void gowl_bar_registry_register_vtable (GowlBarRegistry *self,
                                         const gchar     *name,
                                         const gchar     *title,
                                         const gchar     *description,
                                         const GowlBarPluginVTable *vtable);

/**
 * gowl_bar_registry_register_alias:
 * @self: a registry
 * @alias: the alternative name
 * @target: the name it resolves to
 *
 * Aliases are how the widget names that predate the plugin system keep
 * working: `mem' still means `memory', `bat' still means `battery'.
 */
void gowl_bar_registry_register_alias (GowlBarRegistry *self,
                                        const gchar     *alias,
                                        const gchar     *target);

/* --- Loading ------------------------------------------------------ */

/**
 * gowl_bar_registry_load_file:
 * @self: a registry
 * @path: a `.so' plugin or a `.c' source compiled through crispy
 * @error: (nullable): return location for a #GError
 *
 * Loads a plugin file and registers everything it publishes.
 *
 * A `.c' file is compiled to a cached shared object first, keyed on a
 * hash of its contents and flags, so an unchanged source loads without
 * invoking the compiler.  The source may set `CRISPY_PARAMS' to add
 * compiler flags, exactly as a C configuration file does.
 *
 * The load is journalled before it begins and the journal entry is
 * cleared once it completes, so a plugin that takes the session down
 * while loading is quarantined the next time the bar starts instead of
 * taking it down again.
 *
 * Returns: %TRUE when at least one plugin was registered
 */
/**
 * gowl_bar_registry_set_search_path:
 * @self: a registry
 * @dirs: (array zero-terminated=1) (nullable): directories to search
 *
 * Sets where a plugin named without a path is looked for.
 *
 * Naming a plugin rather than spelling out a path is the point: a
 * configuration that has to carry `/home/<user>/.config/gowl/
 * bar-plugins/foo.c' is not portable between machines and not
 * copy-pasteable between people.
 */
void gowl_bar_registry_set_search_path (GowlBarRegistry     *self,
                                         const gchar * const *dirs);

/**
 * gowl_bar_registry_resolve_file:
 * @self: a registry
 * @spec: a path, or a bare plugin name
 * @error: (nullable): return location for a #GError
 *
 * Turns @spec into a plugin file.
 *
 * An existing path is used as given.  Otherwise each search directory
 * is tried in turn for @spec, `@spec.so' and `@spec.c' --- in that
 * order, so a compiled plugin wins over the source it was built from
 * when a user has both sitting side by side.
 *
 * Returns: (transfer full) (nullable): the resolved path, or %NULL with
 *   @error set to a message naming every directory that was searched
 */
gchar *gowl_bar_registry_resolve_file (GowlBarRegistry *self,
                                        const gchar     *spec,
                                        GError         **error);

gboolean gowl_bar_registry_load_file (GowlBarRegistry  *self,
                                       const gchar      *path,
                                       GError          **error);

/**
 * gowl_bar_registry_load_directory:
 * @self: a registry
 * @dir_path: a directory to scan
 *
 * Loads every `.so' and `.c' in @dir_path, in name order.  A file that
 * fails to load is warned about and skipped; one bad plugin must not
 * cost the user the rest of their bar.
 *
 * Returns: how many files loaded successfully
 */
gint gowl_bar_registry_load_directory (GowlBarRegistry *self,
                                        const gchar     *dir_path);

/**
 * gowl_bar_registry_unload:
 * @self: a registry
 * @name: a registered name
 * @error: (nullable): return location for a #GError
 *
 * Removes @name from the registry.  Instances already in the bar are
 * the host's to drop --- see the `plugin-unloaded' signal.
 *
 * The shared object itself stays mapped.  Unmapping code that a live
 * #GType, a queued worker or a pending callback may still point into
 * is how a hot-unload turns into a crash, and the memory saved is a
 * few tens of kilobytes.
 *
 * Returns: %TRUE when @name was registered and has been removed
 */
gboolean gowl_bar_registry_unload (GowlBarRegistry  *self,
                                    const gchar      *name,
                                    GError          **error);

/**
 * gowl_bar_registry_reload:
 * @self: a registry
 * @name: a registered name loaded from a file
 * @error: (nullable): return location for a #GError
 *
 * Recompiles and reloads the file @name came from.  Only meaningful
 * for a vtable plugin: see #GowlBarPluginVTable for why.
 *
 * Returns: %TRUE on success
 */
gboolean gowl_bar_registry_reload (GowlBarRegistry  *self,
                                    const gchar      *name,
                                    GError          **error);

/* --- Instantiation ------------------------------------------------ */

/**
 * gowl_bar_registry_instantiate:
 * @self: a registry
 * @spec: a widget spec: `name', `name:param', `name@seconds' or
 *   `name:param@seconds'
 * @error: (nullable): return location for a #GError
 *
 * Builds one plugin instance from a configuration spec.  The instance
 * id is the whole spec, so two `disk:' widgets on different mounts are
 * separately addressable.
 *
 * Returns: (transfer full) (nullable): a new plugin
 */
GowlBarPlugin *gowl_bar_registry_instantiate (GowlBarRegistry  *self,
                                               const gchar      *spec,
                                               GError          **error);

/**
 * gowl_bar_registry_has:
 * @self: a registry
 * @name: a name or alias
 *
 * Returns: %TRUE when @name resolves to a registered plugin
 */
gboolean gowl_bar_registry_has (GowlBarRegistry *self, const gchar *name);

/**
 * gowl_bar_registry_list:
 * @self: a registry
 *
 * Returns: (transfer full) (array zero-terminated=1): the registered
 *   names in sorted order.  Free with g_strfreev().
 */
gchar **gowl_bar_registry_list (GowlBarRegistry *self);

/**
 * gowl_bar_registry_describe:
 * @self: a registry
 * @name: a registered name
 *
 * Returns: (transfer full) (nullable): a one-line description of the
 *   entry, suitable for an IPC reply
 */
gchar *gowl_bar_registry_describe (GowlBarRegistry *self,
                                    const gchar     *name);

/* --- Quarantine --------------------------------------------------- */

/**
 * gowl_bar_registry_quarantine:
 * @self: a registry
 * @name: the plugin to hold back
 * @reason: why, shown to the user
 *
 * Marks @name as not to be loaded, now or on the next start, until the
 * user clears it.  Called when a plugin faults under the guard and
 * when a journalled load is found unfinished at startup.
 */
void gowl_bar_registry_quarantine (GowlBarRegistry *self,
                                    const gchar     *name,
                                    const gchar     *reason);

gboolean     gowl_bar_registry_is_quarantined     (GowlBarRegistry *self,
                                                    const gchar     *name);
const gchar *gowl_bar_registry_quarantine_reason  (GowlBarRegistry *self,
                                                    const gchar     *name);
gboolean     gowl_bar_registry_clear_quarantine   (GowlBarRegistry *self,
                                                    const gchar     *name);

/**
 * gowl_bar_registry_list_quarantined:
 * @self: a registry
 *
 * Returns: (transfer full) (array zero-terminated=1): the quarantined
 *   names.  Free with g_strfreev().
 */
gchar **gowl_bar_registry_list_quarantined (GowlBarRegistry *self);

/**
 * gowl_bar_registry_recover_journal:
 * @self: a registry
 *
 * Looks for a load that was recorded as started but never finished ---
 * the signature of a plugin that crashed the session while loading ---
 * and quarantines whatever it was.
 *
 * Call this once, before loading anything.
 *
 * Returns: (transfer full) (nullable): the name that was quarantined,
 *   or %NULL when the last session shut down cleanly
 */
gchar *gowl_bar_registry_recover_journal (GowlBarRegistry *self);

G_END_DECLS

#endif /* GOWL_BAR_REGISTRY_H */
