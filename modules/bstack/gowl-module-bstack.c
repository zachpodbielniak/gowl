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
 * gowl-module-bstack.c - Master on top, stack in a row beneath it.
 *
 * dwm's bstack.  Tile splits the screen down the middle; this splits
 * it across, which is the right way round for a wide screen showing
 * one document and its references: the master gets the full width it
 * wants and the stack is a row of narrow columns rather than a column
 * of short strips.
 *
 * `mfact' is the master's share of the HEIGHT here, not the width --
 * the same key, measured along the axis this layout actually splits.
 *
 * Like every gowl layout this works in the oriented box: the shared
 * helper transposes a portrait output so the arithmetic below only
 * ever has to think in one orientation, and places each window back
 * through the same transform.
 */

#undef G_LOG_DOMAIN
#define G_LOG_DOMAIN "gowl-bstack"

#include <glib-object.h>
#include <gmodule.h>

#include "module/gowl-module.h"
#include "interfaces/gowl-layout-provider.h"
#include "core/gowl-compositor.h"
#include "core/gowl-monitor.h"
#include "core/gowl-client.h"
#include <wlr/util/box.h>
#include "../layout-axis.h"

#define GOWL_TYPE_MODULE_BSTACK (gowl_module_bstack_get_type())
G_DECLARE_FINAL_TYPE(GowlModuleBstack, gowl_module_bstack,
                     GOWL, MODULE_BSTACK, GowlModule)

struct _GowlModuleBstack {
	GowlModule parent_instance;
};

static void bstack_layout_init(GowlLayoutProviderInterface *iface);

G_DEFINE_TYPE_WITH_CODE(GowlModuleBstack, gowl_module_bstack,
	GOWL_TYPE_MODULE,
	G_IMPLEMENT_INTERFACE(GOWL_TYPE_LAYOUT_PROVIDER, bstack_layout_init))

static gboolean
bstack_activate(GowlModule *mod)
{
	(void)mod;
	return TRUE;
}

static const gchar *
bstack_get_name(GowlModule *mod)
{
	(void)mod;
	return "bstack";
}

static const gchar *
bstack_get_description(GowlModule *mod)
{
	(void)mod;
	return "Bottom-stack tiling: master above, stack in a row below";
}

static const gchar *
bstack_get_version(GowlModule *mod)
{
	(void)mod;
	return "0.1.0";
}

static const gchar *
bstack_get_symbol(GowlLayoutProvider *self)
{
	(void)self;
	return "TTT";
}

static void
bstack_arrange(
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
		gint master_h, mx, sx, stack_n;

		if (nmaster < 0)
			nmaster = 0;
		if (mfact <= 0.0 || mfact >= 1.0)
			mfact = 0.55;
		if (nmaster > n)
			nmaster = n;

		/* Nothing in the stack: the masters take the whole area, so a
		 * single window is full-screen rather than half of one. */
		master_h = (nmaster == 0 || nmaster == n)
			? a->height : (gint)((gdouble)a->height * mfact);
		stack_n = n - nmaster;

		mx = a->x;
		sx = a->x;
		i = 0;
		for (l = clients; l != NULL; l = l->next, i++) {
			GowlClient *c = (GowlClient *)l->data;

			if (i < nmaster) {
				/* Remaining width divided by remaining masters, so
				 * rounding lands on the last one instead of leaving a
				 * gap at the edge. */
				gint w = (a->x + a->width - mx) / (nmaster - i);

				gowl_layout_place_oriented(comp, c, portrait,
					mx, a->y, w, master_h);
				mx += w;
			} else {
				gint w = (a->x + a->width - sx) / (n - i);

				gowl_layout_place_oriented(comp, c, portrait,
					sx, a->y + master_h, w, a->height - master_h);
				sx += w;
			}
		}
		(void)stack_n;
	}
}

static void
bstack_layout_init(GowlLayoutProviderInterface *iface)
{
	iface->arrange    = bstack_arrange;
	iface->get_symbol = bstack_get_symbol;
}

static void
gowl_module_bstack_class_init(GowlModuleBstackClass *klass)
{
	GowlModuleClass *mod_class = GOWL_MODULE_CLASS(klass);

	mod_class->activate        = bstack_activate;
	mod_class->get_name        = bstack_get_name;
	mod_class->get_description = bstack_get_description;
	mod_class->get_version     = bstack_get_version;
}

static void
gowl_module_bstack_init(GowlModuleBstack *self)
{
	(void)self;
}

G_MODULE_EXPORT GType
gowl_module_register(void)
{
	return GOWL_TYPE_MODULE_BSTACK;
}
