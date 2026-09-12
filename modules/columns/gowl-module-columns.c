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

/*
 * gowl-module-columns.c - One equal column per window, in order.
 *
 * Every window gets a full-height column of equal width, in stack
 * order.  No master and no mfact -- three terminals are three equal
 * columns, and the fourth makes four.
 *
 * The difference from the scrolling layout is that every window is on
 * screen: columns shrink as windows are added instead of the row
 * scrolling sideways.  Past about six it stops being readable, which
 * is the honest limit of the idea.
 *
 * Like every gowl layout this works in the oriented box: the shared
 * helper transposes a portrait output so the arithmetic below only
 * ever has to think in one orientation, and places each window back
 * through the same transform.
 */

#undef G_LOG_DOMAIN
#define G_LOG_DOMAIN "gowl-columns"

#include <glib-object.h>
#include <gmodule.h>

#include "module/gowl-module.h"
#include "interfaces/gowl-layout-provider.h"
#include "core/gowl-compositor.h"
#include "core/gowl-monitor.h"
#include "core/gowl-client.h"
#include <wlr/util/box.h>
#include "../layout-axis.h"

#define GOWL_TYPE_MODULE_COLUMNS (gowl_module_columns_get_type())
G_DECLARE_FINAL_TYPE(GowlModuleColumns, gowl_module_columns,
                     GOWL, MODULE_COLUMNS, GowlModule)

struct _GowlModuleColumns {
	GowlModule parent_instance;
};

static void columns_layout_init(GowlLayoutProviderInterface *iface);

G_DEFINE_TYPE_WITH_CODE(GowlModuleColumns, gowl_module_columns,
	GOWL_TYPE_MODULE,
	G_IMPLEMENT_INTERFACE(GOWL_TYPE_LAYOUT_PROVIDER, columns_layout_init))

static gboolean
columns_activate(GowlModule *mod)
{
	(void)mod;
	return TRUE;
}

static const gchar *
columns_get_name(GowlModule *mod)
{
	(void)mod;
	return "columns";
}

static const gchar *
columns_get_description(GowlModule *mod)
{
	(void)mod;
	return "Column tiling: every window an equal full-height column";
}

static const gchar *
columns_get_version(GowlModule *mod)
{
	(void)mod;
	return "0.1.0";
}

static const gchar *
columns_get_symbol(GowlLayoutProvider *self)
{
	(void)self;
	return "|||";
}

static void
columns_arrange(
	GowlLayoutProvider *self,
	gpointer            monitor,
	GList              *clients,
	gpointer            area
){
	GowlMonitor *m = (GowlMonitor *)monitor;
	struct wlr_box *a = (struct wlr_box *)area;
	struct wlr_box oriented;
	gboolean portrait;
	GowlCompositor *comp;
	GList *l;
	gint n, i;

	(void)self;

	if (m == NULL || a == NULL || clients == NULL)
		return;

	portrait = gowl_layout_is_portrait(m);
	oriented = gowl_layout_axis_box(*a, portrait);
	a = &oriented;

	comp = gowl_monitor_get_compositor(m);
	if (comp == NULL)
		return;

	n = (gint)g_list_length(clients);
	if (n == 0)
		return;

	{
		gint x = a->x;

		i = 0;
		for (l = clients; l != NULL; l = l->next, i++) {
			/* Remaining width over remaining windows: the rounding
			 * error lands on the last column rather than on a gap. */
			gint w = (a->x + a->width - x) / (n - i);

			gowl_layout_place_oriented(comp, (GowlClient *)l->data,
				portrait, x, a->y, w, a->height);
			x += w;
		}
	}
}

static void
columns_layout_init(GowlLayoutProviderInterface *iface)
{
	iface->arrange    = columns_arrange;
	iface->get_symbol = columns_get_symbol;
}

static void
gowl_module_columns_class_init(GowlModuleColumnsClass *klass)
{
	GowlModuleClass *mod_class = GOWL_MODULE_CLASS(klass);

	mod_class->activate        = columns_activate;
	mod_class->get_name        = columns_get_name;
	mod_class->get_description = columns_get_description;
	mod_class->get_version     = columns_get_version;
}

static void
gowl_module_columns_init(GowlModuleColumns *self)
{
	(void)self;
}

G_MODULE_EXPORT GType
gowl_module_register(void)
{
	return GOWL_TYPE_MODULE_COLUMNS;
}
