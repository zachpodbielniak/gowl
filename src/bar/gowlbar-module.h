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

#ifndef GOWLBAR_MODULE_H
#define GOWLBAR_MODULE_H

#include <glib-object.h>

G_BEGIN_DECLS

#define GOWLBAR_TYPE_MODULE (gowlbar_module_get_type())

G_DECLARE_DERIVABLE_TYPE(GowlbarModule, gowlbar_module,
                         GOWLBAR, MODULE, GObject)

/**
 * GowlbarModuleClass:
 * @parent_class: the parent #GObjectClass
 * @activate: virtual method called when the module is activated
 * @deactivate: virtual method called when the module is deactivated
 * @get_name: returns the human-readable module name
 * @get_description: returns a short description
 * @configure: applies YAML configuration data to the module
 * @padding: reserved for future ABI-compatible expansion
 *
 * The class structure for #GowlbarModule.  Subclasses override the
 * virtual methods to provide module-specific behaviour.
 */
struct _GowlbarModuleClass {
	GObjectClass parent_class;

	gboolean     (*activate)        (GowlbarModule *self);
	void         (*deactivate)      (GowlbarModule *self);
	const gchar *(*get_name)        (GowlbarModule *self);
	const gchar *(*get_description) (GowlbarModule *self);
	void         (*configure)       (GowlbarModule *self, gpointer config);

	gpointer padding[8];
};

/* Public dispatch API */

gboolean     gowlbar_module_activate        (GowlbarModule *self);
void         gowlbar_module_deactivate      (GowlbarModule *self);
const gchar *gowlbar_module_get_name        (GowlbarModule *self);
const gchar *gowlbar_module_get_description (GowlbarModule *self);
void         gowlbar_module_configure       (GowlbarModule *self,
                                              gpointer config);

G_END_DECLS

#endif /* GOWLBAR_MODULE_H */
