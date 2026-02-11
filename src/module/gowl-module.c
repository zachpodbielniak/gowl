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

#include "gowl-module.h"

/**
 * GowlModulePrivate:
 * @priority: dispatch priority; lower values run first (default 0)
 * @is_active: %TRUE when the module has been successfully activated
 *
 * Private data for #GowlModule instances.
 */
typedef struct {
	gint     priority;
	gboolean is_active;
} GowlModulePrivate;

G_DEFINE_ABSTRACT_TYPE_WITH_PRIVATE(GowlModule, gowl_module, G_TYPE_OBJECT)

/* Property identifiers */
enum {
	PROP_0,
	PROP_PRIORITY,
	PROP_IS_ACTIVE,
	N_PROPERTIES
};

static GParamSpec *obj_properties[N_PROPERTIES] = { NULL, };

/* --- Default virtual method implementations --- */

static gboolean
gowl_module_real_activate(GowlModule *self)
{
	(void)self;
	return FALSE;
}

static void
gowl_module_real_deactivate(GowlModule *self)
{
	(void)self;
}

static const gchar *
gowl_module_real_get_name(GowlModule *self)
{
	(void)self;
	return NULL;
}

static const gchar *
gowl_module_real_get_description(GowlModule *self)
{
	(void)self;
	return NULL;
}

static const gchar *
gowl_module_real_get_version(GowlModule *self)
{
	(void)self;
	return NULL;
}

static void
gowl_module_real_configure(
	GowlModule *self,
	gpointer    config
){
	(void)self;
	(void)config;
}

/* --- GObject property accessors --- */

static void
gowl_module_set_property(
	GObject      *object,
	guint         prop_id,
	const GValue *value,
	GParamSpec   *pspec
){
	GowlModule *self;
	GowlModulePrivate *priv;

	self = GOWL_MODULE(object);
	priv = gowl_module_get_instance_private(self);

	switch (prop_id) {
	case PROP_PRIORITY:
		priv->priority = g_value_get_int(value);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
		break;
	}
}

static void
gowl_module_get_property(
	GObject    *object,
	guint       prop_id,
	GValue     *value,
	GParamSpec *pspec
){
	GowlModule *self;
	GowlModulePrivate *priv;

	self = GOWL_MODULE(object);
	priv = gowl_module_get_instance_private(self);

	switch (prop_id) {
	case PROP_PRIORITY:
		g_value_set_int(value, priv->priority);
		break;
	case PROP_IS_ACTIVE:
		g_value_set_boolean(value, priv->is_active);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
		break;
	}
}

/* --- GObject class/instance init --- */

static void
gowl_module_class_init(GowlModuleClass *klass)
{
	GObjectClass *object_class;

	object_class = G_OBJECT_CLASS(klass);

	object_class->set_property = gowl_module_set_property;
	object_class->get_property = gowl_module_get_property;

	/* Wire up default virtual method implementations */
	klass->activate        = gowl_module_real_activate;
	klass->deactivate      = gowl_module_real_deactivate;
	klass->get_name        = gowl_module_real_get_name;
	klass->get_description = gowl_module_real_get_description;
	klass->get_version     = gowl_module_real_get_version;
	klass->configure       = gowl_module_real_configure;

	/**
	 * GowlModule:priority:
	 *
	 * The dispatch priority for this module.  Modules with lower priority
	 * values are invoked before those with higher values.  The default is 0.
	 */
	obj_properties[PROP_PRIORITY] =
		g_param_spec_int("priority",
		                 "Priority",
		                 "Module dispatch priority (lower = earlier)",
		                 G_MININT, G_MAXINT, 0,
		                 G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

	/**
	 * GowlModule:is-active:
	 *
	 * Whether the module is currently activated.  This property is
	 * read-only; it is set internally when activate/deactivate is called
	 * through the module manager.
	 */
	obj_properties[PROP_IS_ACTIVE] =
		g_param_spec_boolean("is-active",
		                     "Is Active",
		                     "Whether the module is currently active",
		                     FALSE,
		                     G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

	g_object_class_install_properties(object_class, N_PROPERTIES, obj_properties);
}

static void
gowl_module_init(GowlModule *self)
{
	GowlModulePrivate *priv;

	priv = gowl_module_get_instance_private(self);
	priv->priority = 0;
	priv->is_active = FALSE;
}

/* --- Public API --- */

/**
 * gowl_module_activate:
 * @self: a #GowlModule
 *
 * Activates the module by calling the subclass activate() vfunc.
 * On success the internal is-active flag is set to %TRUE.
 *
 * Returns: %TRUE if activation succeeded, %FALSE otherwise
 */
gboolean
gowl_module_activate(GowlModule *self)
{
	GowlModuleClass *klass;
	GowlModulePrivate *priv;
	gboolean result;

	g_return_val_if_fail(GOWL_IS_MODULE(self), FALSE);

	klass = GOWL_MODULE_GET_CLASS(self);
	priv = gowl_module_get_instance_private(self);

	if (klass->activate != NULL)
		result = klass->activate(self);
	else
		result = FALSE;

	if (result)
		priv->is_active = TRUE;

	return result;
}

/**
 * gowl_module_deactivate:
 * @self: a #GowlModule
 *
 * Deactivates the module by calling the subclass deactivate() vfunc
 * and clearing the internal is-active flag.
 */
void
gowl_module_deactivate(GowlModule *self)
{
	GowlModuleClass *klass;
	GowlModulePrivate *priv;

	g_return_if_fail(GOWL_IS_MODULE(self));

	klass = GOWL_MODULE_GET_CLASS(self);
	priv = gowl_module_get_instance_private(self);

	if (klass->deactivate != NULL)
		klass->deactivate(self);

	priv->is_active = FALSE;
}

/**
 * gowl_module_get_name:
 * @self: a #GowlModule
 *
 * Returns the human-readable name of the module.
 *
 * Returns: (transfer none) (nullable): the module name
 */
const gchar *
gowl_module_get_name(GowlModule *self)
{
	GowlModuleClass *klass;

	g_return_val_if_fail(GOWL_IS_MODULE(self), NULL);

	klass = GOWL_MODULE_GET_CLASS(self);
	if (klass->get_name != NULL)
		return klass->get_name(self);
	return NULL;
}

/**
 * gowl_module_get_description:
 * @self: a #GowlModule
 *
 * Returns a short description of the module.
 *
 * Returns: (transfer none) (nullable): the module description
 */
const gchar *
gowl_module_get_description(GowlModule *self)
{
	GowlModuleClass *klass;

	g_return_val_if_fail(GOWL_IS_MODULE(self), NULL);

	klass = GOWL_MODULE_GET_CLASS(self);
	if (klass->get_description != NULL)
		return klass->get_description(self);
	return NULL;
}

/**
 * gowl_module_get_version:
 * @self: a #GowlModule
 *
 * Returns the version string of the module.
 *
 * Returns: (transfer none) (nullable): the module version string
 */
const gchar *
gowl_module_get_version(GowlModule *self)
{
	GowlModuleClass *klass;

	g_return_val_if_fail(GOWL_IS_MODULE(self), NULL);

	klass = GOWL_MODULE_GET_CLASS(self);
	if (klass->get_version != NULL)
		return klass->get_version(self);
	return NULL;
}

/**
 * gowl_module_configure:
 * @self: a #GowlModule
 * @config: (nullable): opaque configuration data for the module
 *
 * Passes configuration data to the module.  The interpretation of
 * @config is entirely up to the subclass implementation.
 */
void
gowl_module_configure(
	GowlModule *self,
	gpointer    config
){
	GowlModuleClass *klass;

	g_return_if_fail(GOWL_IS_MODULE(self));

	klass = GOWL_MODULE_GET_CLASS(self);
	if (klass->configure != NULL)
		klass->configure(self, config);
}

/**
 * gowl_module_get_priority:
 * @self: a #GowlModule
 *
 * Returns the dispatch priority of the module.
 *
 * Returns: the priority value
 */
gint
gowl_module_get_priority(GowlModule *self)
{
	GowlModulePrivate *priv;

	g_return_val_if_fail(GOWL_IS_MODULE(self), 0);

	priv = gowl_module_get_instance_private(self);
	return priv->priority;
}

/**
 * gowl_module_set_priority:
 * @self: a #GowlModule
 * @priority: the new priority value
 *
 * Sets the dispatch priority of the module.  Lower values are
 * dispatched first.
 */
void
gowl_module_set_priority(
	GowlModule *self,
	gint        priority
){
	GowlModulePrivate *priv;

	g_return_if_fail(GOWL_IS_MODULE(self));

	priv = gowl_module_get_instance_private(self);
	priv->priority = priority;
	g_object_notify_by_pspec(G_OBJECT(self), obj_properties[PROP_PRIORITY]);
}

/**
 * gowl_module_get_is_active:
 * @self: a #GowlModule
 *
 * Returns whether the module is currently activated.
 *
 * Returns: %TRUE if the module is active
 */
gboolean
gowl_module_get_is_active(GowlModule *self)
{
	GowlModulePrivate *priv;

	g_return_val_if_fail(GOWL_IS_MODULE(self), FALSE);

	priv = gowl_module_get_instance_private(self);
	return priv->is_active;
}
