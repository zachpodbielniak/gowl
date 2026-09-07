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

#ifndef GOWL_BAR_PLUGIN_PROXY_H
#define GOWL_BAR_PLUGIN_PROXY_H

#include <glib-object.h>

#include "barkit/gowl-bar-plugin.h"

G_BEGIN_DECLS

/**
 * GowlBarPluginVTable:
 * @size: `sizeof (GowlBarPluginVTable)' as the plugin saw it.  The
 *   proxy checks every callback lies inside @size before calling it,
 *   so a plugin built against an older barkit still runs.
 * @create: (nullable): allocate the plugin's instance data
 * @destroy: (nullable): free what @create returned
 * @activate: (nullable): last chance to refuse to run
 * @deactivate: (nullable): release runtime resources
 * @configure: (nullable): apply settings; called again on reconfigure
 * @interval: (nullable): poll period in seconds, 0 for every tick
 * @poll: (nullable): cheap refresh on the compositor thread
 * @poll_async: (nullable): blocking refresh on a worker thread
 * @measure: (nullable): natural width; omit for the default label
 * @draw: (nullable): custom painting; omit for the default label
 * @click: (nullable): handle a button; %TRUE consumes it
 * @scroll: (nullable): handle a scroll; %TRUE consumes it
 * @panel: (nullable): build the dropdown; omit for no dropdown
 * @action: (nullable): a panel item was activated
 * @panel_opened: (nullable): the dropdown just became visible
 * @panel_closed: (nullable): the dropdown was dismissed
 *
 * A plugin written as plain C function pointers rather than a GObject
 * subclass.
 *
 * This is the form a loadable plugin should normally use, and the only
 * one that hot-reloads cleanly: a #GType cannot be unregistered, so a
 * plugin that defines its own class can be dropped from the bar but its
 * code stays mapped for the life of the session and a recompiled
 * version of it cannot re-register the same class name.  A vtable has
 * no such problem --- reloading swaps a struct of pointers.
 */
typedef struct _GowlBarPluginVTable GowlBarPluginVTable;

struct _GowlBarPluginVTable {
	gsize size;

	gpointer (*create)     (GowlBarPlugin *plugin);
	void     (*destroy)    (GowlBarPlugin *plugin, gpointer data);

	gboolean (*activate)   (GowlBarPlugin *plugin, gpointer data,
	                        GError **error);
	void     (*deactivate) (GowlBarPlugin *plugin, gpointer data);
	void     (*configure)  (GowlBarPlugin *plugin, gpointer data,
	                        GHashTable *settings);

	gint     (*interval)   (GowlBarPlugin *plugin, gpointer data);
	void     (*poll)       (GowlBarPlugin *plugin, gpointer data);
	void     (*poll_async) (GowlBarPlugin *plugin, gpointer data);

	gint     (*measure)    (GowlBarPlugin *plugin, gpointer data,
	                        PangoLayout *layout,
	                        const GowlBarTheme *theme, gint height);
	void     (*draw)       (GowlBarPlugin *plugin, gpointer data,
	                        cairo_t *cr, PangoLayout *layout,
	                        const GowlBarTheme *theme,
	                        gint x, gint y, gint width, gint height,
	                        gboolean hovered, gboolean panel_open);

	gboolean (*click)      (GowlBarPlugin *plugin, gpointer data,
	                        guint button, gint x, gint y, guint modifiers);
	gboolean (*scroll)     (GowlBarPlugin *plugin, gpointer data,
	                        gdouble delta, gint discrete, guint modifiers);

	GowlBarPanel *(*panel) (GowlBarPlugin *plugin, gpointer data);
	void     (*action)     (GowlBarPlugin *plugin, gpointer data,
	                        const gchar *item_id, gint index,
	                        gdouble value, guint button);

	/* Appended after the first release of the ABI.  Reached only
	   through GOWL_BAR_PLUGIN_VTABLE_HAS, so a plugin built against
	   the shorter struct still loads. */
	void     (*panel_opened) (GowlBarPlugin *plugin, gpointer data);
	void     (*panel_closed) (GowlBarPlugin *plugin, gpointer data);
};

/**
 * GOWL_BAR_PLUGIN_VTABLE_HAS:
 * @vt: a #GowlBarPluginVTable pointer
 * @field: a member name
 *
 * Whether @vt is long enough to carry @field and has it set.  Every
 * call through a vtable goes via this, which is what lets the struct
 * grow without breaking already-built plugins.
 */
#define GOWL_BAR_PLUGIN_VTABLE_HAS(vt, field) \
	((vt) != NULL && \
	 (vt)->size >= (G_STRUCT_OFFSET(GowlBarPluginVTable, field) \
	                + sizeof ((vt)->field)) && \
	 (vt)->field != NULL)

#define GOWL_TYPE_BAR_PLUGIN_PROXY (gowl_bar_plugin_proxy_get_type())

G_DECLARE_FINAL_TYPE(GowlBarPluginProxy, gowl_bar_plugin_proxy,
                     GOWL, BAR_PLUGIN_PROXY, GowlBarPlugin)

/**
 * gowl_bar_plugin_proxy_new:
 * @id: the instance id
 * @title: (nullable): a human-readable name
 * @description: (nullable): one line describing the plugin
 * @vtable: the plugin's callbacks; borrowed and must outlive the proxy
 *
 * Wraps @vtable in a #GowlBarPlugin.
 *
 * Returns: (transfer full): a new plugin instance
 */
GowlBarPlugin *gowl_bar_plugin_proxy_new (const gchar *id,
                                           const gchar *title,
                                           const gchar *description,
                                           const GowlBarPluginVTable *vtable);

/**
 * gowl_bar_plugin_proxy_get_data:
 * @self: a proxy
 *
 * Returns: (transfer none) (nullable): whatever the vtable's @create
 *   returned
 */
gpointer gowl_bar_plugin_proxy_get_data (GowlBarPluginProxy *self);

G_END_DECLS

#endif /* GOWL_BAR_PLUGIN_PROXY_H */
