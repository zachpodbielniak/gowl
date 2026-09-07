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

#include "barkit/gowl-bar-plugin-proxy.h"

/**
 * SECTION:gowl-bar-plugin-proxy
 * @title: GowlBarPluginProxy
 * @short_description: runs a plain-C plugin as a #GowlBarPlugin
 *
 * Everything a #GowlBarPlugin subclass can do, reachable from a struct
 * of function pointers.  A `.c' plugin compiled through crispy defines
 * a handful of static functions and one vtable, and never mentions
 * #GType --- which is exactly what makes it reloadable.
 */

struct _GowlBarPluginProxy {
	GowlBarPlugin parent_instance;

	const GowlBarPluginVTable *vt;   /* borrowed from the module */
	gpointer                   data;
	gchar                     *title;
	gchar                     *description;
};

G_DEFINE_FINAL_TYPE(GowlBarPluginProxy, gowl_bar_plugin_proxy,
                    GOWL_TYPE_BAR_PLUGIN)

static const gchar *
proxy_get_title(GowlBarPlugin *plugin)
{
	GowlBarPluginProxy *self = GOWL_BAR_PLUGIN_PROXY(plugin);

	if (self->title != NULL)
		return self->title;
	return gowl_bar_plugin_get_id(plugin);
}

static const gchar *
proxy_get_description(GowlBarPlugin *plugin)
{
	GowlBarPluginProxy *self = GOWL_BAR_PLUGIN_PROXY(plugin);

	return (self->description != NULL) ? self->description : "";
}

static gboolean
proxy_activate(GowlBarPlugin *plugin, GError **error)
{
	GowlBarPluginProxy *self = GOWL_BAR_PLUGIN_PROXY(plugin);

	if (GOWL_BAR_PLUGIN_VTABLE_HAS(self->vt, create) &&
	    self->data == NULL)
		self->data = self->vt->create(plugin);

	if (GOWL_BAR_PLUGIN_VTABLE_HAS(self->vt, activate))
		return self->vt->activate(plugin, self->data, error);
	return TRUE;
}

static void
proxy_deactivate(GowlBarPlugin *plugin)
{
	GowlBarPluginProxy *self = GOWL_BAR_PLUGIN_PROXY(plugin);

	if (GOWL_BAR_PLUGIN_VTABLE_HAS(self->vt, deactivate))
		self->vt->deactivate(plugin, self->data);
}

static void
proxy_configure(GowlBarPlugin *plugin, GHashTable *settings)
{
	GowlBarPluginProxy *self = GOWL_BAR_PLUGIN_PROXY(plugin);

	/* configure() can arrive before activate(), so the instance data
	   is created here too rather than only in activate. */
	if (GOWL_BAR_PLUGIN_VTABLE_HAS(self->vt, create) &&
	    self->data == NULL)
		self->data = self->vt->create(plugin);

	if (GOWL_BAR_PLUGIN_VTABLE_HAS(self->vt, configure))
		self->vt->configure(plugin, self->data, settings);
}

static gint
proxy_get_interval(GowlBarPlugin *plugin)
{
	GowlBarPluginProxy *self = GOWL_BAR_PLUGIN_PROXY(plugin);

	if (GOWL_BAR_PLUGIN_VTABLE_HAS(self->vt, interval))
		return self->vt->interval(plugin, self->data);
	return 0;
}

static void
proxy_poll(GowlBarPlugin *plugin)
{
	GowlBarPluginProxy *self = GOWL_BAR_PLUGIN_PROXY(plugin);

	if (GOWL_BAR_PLUGIN_VTABLE_HAS(self->vt, poll))
		self->vt->poll(plugin, self->data);
}

static void
proxy_poll_async(GowlBarPlugin *plugin)
{
	GowlBarPluginProxy *self = GOWL_BAR_PLUGIN_PROXY(plugin);

	if (GOWL_BAR_PLUGIN_VTABLE_HAS(self->vt, poll_async))
		self->vt->poll_async(plugin, self->data);
}

static gint
proxy_measure(GowlBarPlugin *plugin, PangoLayout *layout,
              const GowlBarTheme *theme, gint height)
{
	GowlBarPluginProxy *self = GOWL_BAR_PLUGIN_PROXY(plugin);

	if (GOWL_BAR_PLUGIN_VTABLE_HAS(self->vt, measure))
		return self->vt->measure(plugin, self->data, layout, theme,
		                         height);
	return gowl_bar_plugin_measure_label(plugin, layout, theme, height);
}

static void
proxy_draw(GowlBarPlugin *plugin, cairo_t *cr, PangoLayout *layout,
           const GowlBarTheme *theme, gint x, gint y, gint width,
           gint height, gboolean hovered, gboolean panel_open)
{
	GowlBarPluginProxy *self = GOWL_BAR_PLUGIN_PROXY(plugin);

	if (GOWL_BAR_PLUGIN_VTABLE_HAS(self->vt, draw)) {
		self->vt->draw(plugin, self->data, cr, layout, theme, x, y,
		               width, height, hovered, panel_open);
		return;
	}
	gowl_bar_plugin_draw_label(plugin, cr, layout, theme, x, y, width,
	                           height, hovered, panel_open);
}

static gboolean
proxy_on_click(GowlBarPlugin *plugin, guint button, gint x, gint y,
               guint modifiers)
{
	GowlBarPluginProxy *self = GOWL_BAR_PLUGIN_PROXY(plugin);

	if (GOWL_BAR_PLUGIN_VTABLE_HAS(self->vt, click))
		return self->vt->click(plugin, self->data, button, x, y,
		                       modifiers);
	return FALSE;
}

static gboolean
proxy_on_scroll(GowlBarPlugin *plugin, gdouble delta, gint discrete,
                guint modifiers)
{
	GowlBarPluginProxy *self = GOWL_BAR_PLUGIN_PROXY(plugin);

	if (GOWL_BAR_PLUGIN_VTABLE_HAS(self->vt, scroll))
		return self->vt->scroll(plugin, self->data, delta, discrete,
		                        modifiers);
	return FALSE;
}

static gboolean
proxy_has_panel(GowlBarPlugin *plugin)
{
	GowlBarPluginProxy *self = GOWL_BAR_PLUGIN_PROXY(plugin);

	return GOWL_BAR_PLUGIN_VTABLE_HAS(self->vt, panel);
}

static GowlBarPanel *
proxy_build_panel(GowlBarPlugin *plugin)
{
	GowlBarPluginProxy *self = GOWL_BAR_PLUGIN_PROXY(plugin);

	if (GOWL_BAR_PLUGIN_VTABLE_HAS(self->vt, panel))
		return self->vt->panel(plugin, self->data);
	return NULL;
}

static void
proxy_panel_action(GowlBarPlugin *plugin, const gchar *item_id, gint index,
                   gdouble value, guint button)
{
	GowlBarPluginProxy *self = GOWL_BAR_PLUGIN_PROXY(plugin);

	if (GOWL_BAR_PLUGIN_VTABLE_HAS(self->vt, action))
		self->vt->action(plugin, self->data, item_id, index, value,
		                 button);
}

static void
proxy_panel_opened(GowlBarPlugin *plugin)
{
	GowlBarPluginProxy *self = GOWL_BAR_PLUGIN_PROXY(plugin);

	if (GOWL_BAR_PLUGIN_VTABLE_HAS(self->vt, panel_opened))
		self->vt->panel_opened(plugin, self->data);
}

static void
proxy_panel_closed(GowlBarPlugin *plugin)
{
	GowlBarPluginProxy *self = GOWL_BAR_PLUGIN_PROXY(plugin);

	if (GOWL_BAR_PLUGIN_VTABLE_HAS(self->vt, panel_closed))
		self->vt->panel_closed(plugin, self->data);
}

static void
gowl_bar_plugin_proxy_finalize(GObject *object)
{
	GowlBarPluginProxy *self = GOWL_BAR_PLUGIN_PROXY(object);

	if (self->data != NULL &&
	    GOWL_BAR_PLUGIN_VTABLE_HAS(self->vt, destroy))
		self->vt->destroy(GOWL_BAR_PLUGIN(object), self->data);
	self->data = NULL;

	g_free(self->title);
	g_free(self->description);

	G_OBJECT_CLASS(gowl_bar_plugin_proxy_parent_class)->finalize(object);
}

static void
gowl_bar_plugin_proxy_class_init(GowlBarPluginProxyClass *klass)
{
	GowlBarPluginClass *plugin_class = GOWL_BAR_PLUGIN_CLASS(klass);

	G_OBJECT_CLASS(klass)->finalize = gowl_bar_plugin_proxy_finalize;

	plugin_class->get_title       = proxy_get_title;
	plugin_class->get_description = proxy_get_description;
	plugin_class->activate        = proxy_activate;
	plugin_class->deactivate      = proxy_deactivate;
	plugin_class->configure       = proxy_configure;
	plugin_class->get_interval    = proxy_get_interval;
	plugin_class->poll            = proxy_poll;
	/* poll_async is wired unconditionally so gowl_bar_plugin_wants_async
	   is true for every proxy; the forwarder itself checks the vtable.
	   The alternative -- deciding per instance -- would need a per-
	   instance class, which is exactly what the proxy exists to avoid.
	   The cost is one no-op worker dispatch for a plugin with no async
	   poll, and the interval logic means that is at most once per
	   interval. */
	plugin_class->poll_async      = proxy_poll_async;
	plugin_class->measure         = proxy_measure;
	plugin_class->draw            = proxy_draw;
	plugin_class->on_click        = proxy_on_click;
	plugin_class->on_scroll       = proxy_on_scroll;
	plugin_class->has_panel       = proxy_has_panel;
	plugin_class->build_panel     = proxy_build_panel;
	plugin_class->panel_action    = proxy_panel_action;
	plugin_class->panel_opened    = proxy_panel_opened;
	plugin_class->panel_closed    = proxy_panel_closed;
}

static void
gowl_bar_plugin_proxy_init(GowlBarPluginProxy *self)
{
	self->vt   = NULL;
	self->data = NULL;
}

/**
 * gowl_bar_plugin_proxy_new:
 * @id: the instance id
 * @title: (nullable): a human-readable name
 * @description: (nullable): one line describing the plugin
 * @vtable: the plugin's callbacks
 *
 * Returns: (transfer full): a new plugin instance
 */
GowlBarPlugin *
gowl_bar_plugin_proxy_new(const gchar *id, const gchar *title,
                          const gchar *description,
                          const GowlBarPluginVTable *vtable)
{
	GowlBarPluginProxy *self;

	g_return_val_if_fail(vtable != NULL, NULL);

	self = (GowlBarPluginProxy *)g_object_new(GOWL_TYPE_BAR_PLUGIN_PROXY,
	                                          "id", id, NULL);
	self->vt          = vtable;
	self->title       = g_strdup(title);
	self->description = g_strdup(description);
	return GOWL_BAR_PLUGIN(self);
}

/**
 * gowl_bar_plugin_proxy_get_data:
 * @self: a proxy
 *
 * Returns: (transfer none) (nullable): the vtable's instance data
 */
gpointer
gowl_bar_plugin_proxy_get_data(GowlBarPluginProxy *self)
{
	g_return_val_if_fail(GOWL_IS_BAR_PLUGIN_PROXY(self), NULL);
	return self->data;
}
