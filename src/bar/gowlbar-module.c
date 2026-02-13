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

#include "gowlbar-module.h"

/**
 * GowlbarModule:
 *
 * Abstract base class for bar modules.  Each module has a name,
 * description, and lifecycle methods (activate/deactivate/configure).
 * Modules are loaded as .so files that export
 * `GType gowlbar_module_register(void)`.
 */
G_DEFINE_ABSTRACT_TYPE(GowlbarModule, gowlbar_module, G_TYPE_OBJECT)

/* --- Default vfunc implementations --- */

static gboolean
gowlbar_module_default_activate(GowlbarModule *self)
{
	(void)self;
	return TRUE;
}

static void
gowlbar_module_default_deactivate(GowlbarModule *self)
{
	(void)self;
}

static const gchar *
gowlbar_module_default_get_name(GowlbarModule *self)
{
	(void)self;
	return "unnamed";
}

static const gchar *
gowlbar_module_default_get_description(GowlbarModule *self)
{
	(void)self;
	return "";
}

static void
gowlbar_module_default_configure(GowlbarModule *self, gpointer config)
{
	(void)self;
	(void)config;
}

/* --- Class / instance init --- */

static void
gowlbar_module_class_init(GowlbarModuleClass *klass)
{
	klass->activate        = gowlbar_module_default_activate;
	klass->deactivate      = gowlbar_module_default_deactivate;
	klass->get_name        = gowlbar_module_default_get_name;
	klass->get_description = gowlbar_module_default_get_description;
	klass->configure       = gowlbar_module_default_configure;
}

static void
gowlbar_module_init(GowlbarModule *self)
{
	(void)self;
}

/* --- Public API (dispatch to vfuncs) --- */

/**
 * gowlbar_module_activate:
 * @self: the module
 *
 * Activates the module.
 *
 * Returns: %TRUE on success, %FALSE on failure
 */
gboolean
gowlbar_module_activate(GowlbarModule *self)
{
	GowlbarModuleClass *klass;

	g_return_val_if_fail(GOWLBAR_IS_MODULE(self), FALSE);

	klass = GOWLBAR_MODULE_GET_CLASS(self);
	if (klass->activate != NULL)
		return klass->activate(self);
	return FALSE;
}

/**
 * gowlbar_module_deactivate:
 * @self: the module
 *
 * Deactivates the module.
 */
void
gowlbar_module_deactivate(GowlbarModule *self)
{
	GowlbarModuleClass *klass;

	g_return_if_fail(GOWLBAR_IS_MODULE(self));

	klass = GOWLBAR_MODULE_GET_CLASS(self);
	if (klass->deactivate != NULL)
		klass->deactivate(self);
}

/**
 * gowlbar_module_get_name:
 * @self: the module
 *
 * Returns: (transfer none): the module name
 */
const gchar *
gowlbar_module_get_name(GowlbarModule *self)
{
	GowlbarModuleClass *klass;

	g_return_val_if_fail(GOWLBAR_IS_MODULE(self), NULL);

	klass = GOWLBAR_MODULE_GET_CLASS(self);
	if (klass->get_name != NULL)
		return klass->get_name(self);
	return NULL;
}

/**
 * gowlbar_module_get_description:
 * @self: the module
 *
 * Returns: (transfer none): the module description
 */
const gchar *
gowlbar_module_get_description(GowlbarModule *self)
{
	GowlbarModuleClass *klass;

	g_return_val_if_fail(GOWLBAR_IS_MODULE(self), NULL);

	klass = GOWLBAR_MODULE_GET_CLASS(self);
	if (klass->get_description != NULL)
		return klass->get_description(self);
	return NULL;
}

/**
 * gowlbar_module_configure:
 * @self: the module
 * @config: (transfer none): configuration data (typically a YamlMapping)
 *
 * Passes configuration data to the module.
 */
void
gowlbar_module_configure(GowlbarModule *self, gpointer config)
{
	GowlbarModuleClass *klass;

	g_return_if_fail(GOWLBAR_IS_MODULE(self));

	klass = GOWLBAR_MODULE_GET_CLASS(self);
	if (klass->configure != NULL)
		klass->configure(self, config);
}
