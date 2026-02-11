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

#ifndef GOWL_MODULE_H
#define GOWL_MODULE_H

#include <glib-object.h>

G_BEGIN_DECLS

#define GOWL_TYPE_MODULE (gowl_module_get_type())

G_DECLARE_DERIVABLE_TYPE(GowlModule, gowl_module, GOWL, MODULE, GObject)

/**
 * GowlModuleClass:
 * @parent_class: the parent #GObjectClass
 * @activate: virtual method called when the module is activated; returns
 *   %TRUE on success, %FALSE on failure
 * @deactivate: virtual method called when the module is deactivated
 * @get_name: returns the human-readable module name
 * @get_description: returns a short description of the module
 * @get_version: returns the module version string
 * @configure: applies configuration data to the module
 * @padding: reserved for future ABI-compatible expansion
 *
 * The class structure for #GowlModule.  Subclasses override the virtual
 * methods to provide module-specific behaviour.
 */
struct _GowlModuleClass {
	GObjectClass parent_class;

	gboolean     (*activate)        (GowlModule *self);
	void         (*deactivate)      (GowlModule *self);
	const gchar *(*get_name)        (GowlModule *self);
	const gchar *(*get_description) (GowlModule *self);
	const gchar *(*get_version)     (GowlModule *self);
	void         (*configure)       (GowlModule *self, gpointer config);

	gpointer padding[8];
};

/* Public API -- each dispatches through the vtable */

gboolean     gowl_module_activate        (GowlModule *self);

void         gowl_module_deactivate      (GowlModule *self);

const gchar *gowl_module_get_name        (GowlModule *self);

const gchar *gowl_module_get_description (GowlModule *self);

const gchar *gowl_module_get_version     (GowlModule *self);

void         gowl_module_configure       (GowlModule *self,
                                           gpointer    config);

/* Property accessors */

gint         gowl_module_get_priority    (GowlModule *self);

void         gowl_module_set_priority    (GowlModule *self,
                                           gint        priority);

gboolean     gowl_module_get_is_active   (GowlModule *self);

G_END_DECLS

#endif /* GOWL_MODULE_H */
