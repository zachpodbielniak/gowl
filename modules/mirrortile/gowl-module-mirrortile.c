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
 * gowl-module-mirrortile.c - Tile, mirrored: the stack on the left, master on the right.
 *
 * Tile with the columns swapped.  Trivial arithmetic, and worth a
 * layout of its own for two reasons: a left-handed setup where the
 * pointer lives on the left, and a multi-monitor desk where the
 * master columns of two screens should meet in the middle rather than
 * both hugging the left edge.
 *
 * Like every gowl layout this works in the oriented box: the shared
 * helper transposes a portrait output so the arithmetic below only
 * ever has to think in one orientation, and places each window back
 * through the same transform.
 */

#undef G_LOG_DOMAIN
#define G_LOG_DOMAIN "gowl-mirrortile"

#include <glib-object.h>
#include <gmodule.h>

#include "module/gowl-module.h"
#include "interfaces/gowl-layout-provider.h"
#include "core/gowl-compositor.h"
#include "core/gowl-monitor.h"
#include "core/gowl-client.h"
#include <wlr/util/box.h>
#include "../layout-axis.h"

#define GOWL_TYPE_MODULE_MIRRORTILE (gowl_module_mirrortile_get_type())
G_DECLARE_FINAL_TYPE(GowlModuleMirrortile, gowl_module_mirrortile,
                     GOWL, MODULE_MIRRORTILE, GowlModule)

struct _GowlModuleMirrortile {
	GowlModule parent_instance;
};

static void mirrortile_layout_init(GowlLayoutProviderInterface *iface);

G_DEFINE_TYPE_WITH_CODE(GowlModuleMirrortile, gowl_module_mirrortile,
	GOWL_TYPE_MODULE,
	G_IMPLEMENT_INTERFACE(GOWL_TYPE_LAYOUT_PROVIDER, mirrortile_layout_init))

static gboolean
mirrortile_activate(GowlModule *mod)
{
	(void)mod;
	return TRUE;
}

static const gchar *
mirrortile_get_name(GowlModule *mod)
{
	(void)mod;
	return "mirrortile";
}

static const gchar *
mirrortile_get_description(GowlModule *mod)
{
	(void)mod;
	return "Mirrored tiling: stack left, master right";
}

static const gchar *
mirrortile_get_version(GowlModule *mod)
{
	(void)mod;
	return "0.1.0";
}

static const gchar *
mirrortile_get_symbol(GowlLayoutProvider *self)
{
	(void)self;
	return "[]=";
}

static void
mirrortile_arrange(
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
		gint nmaster = gowl_monitor_get_nmaster(m);
		gdouble mfact = gowl_monitor_get_mfact(m);
		gint master_w, stack_w, master_x, my, sy;

		if (nmaster < 0)
			nmaster = 0;
		if (mfact <= 0.0 || mfact >= 1.0)
			mfact = 0.55;
		if (nmaster > n)
			nmaster = n;

		master_w = (nmaster == 0 || nmaster == n)
			? a->width : (gint)((gdouble)a->width * mfact);
		stack_w = a->width - master_w;
		/* The only difference from tile: master starts where the stack
		 * ends, not at the left edge. */
		master_x = a->x + stack_w;

		my = a->y;
		sy = a->y;
		i = 0;
		for (l = clients; l != NULL; l = l->next, i++) {
			GowlClient *c = (GowlClient *)l->data;

			if (i < nmaster) {
				gint h = (a->y + a->height - my) / (nmaster - i);

				gowl_layout_place_oriented(comp, c, portrait,
					master_x, my, master_w, h);
				my += h;
			} else {
				gint h = (a->y + a->height - sy) / (n - i);

				gowl_layout_place_oriented(comp, c, portrait,
					a->x, sy, stack_w, h);
				sy += h;
			}
		}
	}
}

static void
mirrortile_layout_init(GowlLayoutProviderInterface *iface)
{
	iface->arrange    = mirrortile_arrange;
	iface->get_symbol = mirrortile_get_symbol;
}

static void
gowl_module_mirrortile_class_init(GowlModuleMirrortileClass *klass)
{
	GowlModuleClass *mod_class = GOWL_MODULE_CLASS(klass);

	mod_class->activate        = mirrortile_activate;
	mod_class->get_name        = mirrortile_get_name;
	mod_class->get_description = mirrortile_get_description;
	mod_class->get_version     = mirrortile_get_version;
}

static void
gowl_module_mirrortile_init(GowlModuleMirrortile *self)
{
	(void)self;
}

G_MODULE_EXPORT GType
gowl_module_register(void)
{
	return GOWL_TYPE_MODULE_MIRRORTILE;
}
