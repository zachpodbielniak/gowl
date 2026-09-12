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
 * gowl-module-grid.c - Every window an equal cell of a gapless grid.
 *
 * dwm's gaplessgrid.  No master, no mfact: the windows are laid out in
 * as square a grid as the count allows, and the columns that have to
 * carry an extra row are the leftmost ones, so the grid stays full
 * rather than leaving a hole at the end.
 *
 * The layout for looking at everything at once -- a wall of logs, a
 * contact sheet -- where "which one is the master" is the wrong
 * question.
 *
 * Like every gowl layout this works in the oriented box: the shared
 * helper transposes a portrait output so the arithmetic below only
 * ever has to think in one orientation, and places each window back
 * through the same transform.
 */

#undef G_LOG_DOMAIN
#define G_LOG_DOMAIN "gowl-grid"

#include <glib-object.h>
#include <gmodule.h>

#include "module/gowl-module.h"
#include "interfaces/gowl-layout-provider.h"
#include "core/gowl-compositor.h"
#include "core/gowl-monitor.h"
#include "core/gowl-client.h"
#include <wlr/util/box.h>
#include "../layout-axis.h"

#define GOWL_TYPE_MODULE_GRID (gowl_module_grid_get_type())
G_DECLARE_FINAL_TYPE(GowlModuleGrid, gowl_module_grid,
                     GOWL, MODULE_GRID, GowlModule)

struct _GowlModuleGrid {
	GowlModule parent_instance;
};

static void grid_layout_init(GowlLayoutProviderInterface *iface);

G_DEFINE_TYPE_WITH_CODE(GowlModuleGrid, gowl_module_grid,
	GOWL_TYPE_MODULE,
	G_IMPLEMENT_INTERFACE(GOWL_TYPE_LAYOUT_PROVIDER, grid_layout_init))

static gboolean
grid_activate(GowlModule *mod)
{
	(void)mod;
	return TRUE;
}

static const gchar *
grid_get_name(GowlModule *mod)
{
	(void)mod;
	return "grid";
}

static const gchar *
grid_get_description(GowlModule *mod)
{
	(void)mod;
	return "Grid tiling: equal cells, no master";
}

static const gchar *
grid_get_version(GowlModule *mod)
{
	(void)mod;
	return "0.1.0";
}

static const gchar *
grid_get_symbol(GowlLayoutProvider *self)
{
	(void)self;
	return "HHH";
}

static void
grid_arrange(
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
	gint n;

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
		gint cols, rows, col, placed_in_col, rows_here, cw, cx, cy;

		/* The squarest grid that holds n: grow the column count until
		 * it is at least as large as the row count it implies. */
		for (cols = 1; cols * cols < n; cols++)
			;
		if (cols > 1 && (cols - 1) * cols >= n)
			cols--;
		rows = (n + cols - 1) / cols;

		col = 0;
		placed_in_col = 0;
		/* The first (n % cols) columns take one extra row each. */
		rows_here = rows - ((n % cols) != 0 && col >= (n % cols) ? 1 : 0);
		cw = a->width / cols;
		cx = a->x;
		cy = a->y;
		for (l = clients; l != NULL; l = l->next) {
			GowlClient *c = (GowlClient *)l->data;
			gint ch;

			if (rows_here <= 0)
				rows_here = 1;
			ch = (a->y + a->height - cy) / (rows_here - placed_in_col);
			/* The last column takes what rounding left over. */
			gowl_layout_place_oriented(comp, c, portrait, cx, cy,
				(col == cols - 1) ? (a->x + a->width - cx) : cw, ch);
			cy += ch;
			if (++placed_in_col >= rows_here) {
				col++;
				placed_in_col = 0;
				cx += cw;
				cy = a->y;
				rows_here = rows
					- (((n % cols) != 0 && col >= (n % cols)) ? 1 : 0);
			}
		}
	}
}

static void
grid_layout_init(GowlLayoutProviderInterface *iface)
{
	iface->arrange    = grid_arrange;
	iface->get_symbol = grid_get_symbol;
}

static void
gowl_module_grid_class_init(GowlModuleGridClass *klass)
{
	GowlModuleClass *mod_class = GOWL_MODULE_CLASS(klass);

	mod_class->activate        = grid_activate;
	mod_class->get_name        = grid_get_name;
	mod_class->get_description = grid_get_description;
	mod_class->get_version     = grid_get_version;
}

static void
gowl_module_grid_init(GowlModuleGrid *self)
{
	(void)self;
}

G_MODULE_EXPORT GType
gowl_module_register(void)
{
	return GOWL_TYPE_MODULE_GRID;
}
