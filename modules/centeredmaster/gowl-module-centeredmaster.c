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

#undef G_LOG_DOMAIN
#define G_LOG_DOMAIN "gowl-centeredmaster"

#include <glib-object.h>
#include <gmodule.h>

#include "module/gowl-module.h"
#include "interfaces/gowl-layout-provider.h"
#include "core/gowl-compositor.h"
#include "core/gowl-monitor.h"
#include "core/gowl-client.h"
#include <wlr/util/box.h>
#include "../layout-axis.h"

/**
 * GowlModuleCenteredmaster:
 *
 * Centered master layout module.  Master window(s) occupy the center
 * of the screen.  Stack windows are split between left and right
 * sides, alternating assignment.
 *
 * Layout (3 clients, 1 master):
 *   +------+----------+------+
 *   |stack1|  master   |stack2|
 *   +------+----------+------+
 *
 * With 2 masters:
 *   +------+----------+------+
 *   |      | master 1 |      |
 *   |stack1+----------+stack2|
 *   |      | master 2 |      |
 *   +------+----------+------+
 */

#define GOWL_TYPE_MODULE_CENTEREDMASTER (gowl_module_centeredmaster_get_type())
G_DECLARE_FINAL_TYPE(GowlModuleCenteredmaster, gowl_module_centeredmaster,
                     GOWL, MODULE_CENTEREDMASTER, GowlModule)

struct _GowlModuleCenteredmaster {
	GowlModule parent_instance;
};

static void centeredmaster_layout_init(GowlLayoutProviderInterface *iface);

G_DEFINE_TYPE_WITH_CODE(GowlModuleCenteredmaster, gowl_module_centeredmaster,
	GOWL_TYPE_MODULE,
	G_IMPLEMENT_INTERFACE(GOWL_TYPE_LAYOUT_PROVIDER,
		centeredmaster_layout_init))

/* --- GowlModule virtual methods --- */

static gboolean
centeredmaster_activate(GowlModule *mod)
{
	(void)mod;
	return TRUE;
}

static const gchar *
centeredmaster_get_name(GowlModule *mod)
{
	(void)mod;
	return "centeredmaster";
}

static const gchar *
centeredmaster_get_description(GowlModule *mod)
{
	(void)mod;
	return "Centered master tiling layout";
}

static const gchar *
centeredmaster_get_version(GowlModule *mod)
{
	(void)mod;
	return "0.1.0";
}

/* --- GowlLayoutProvider --- */

/**
 * centeredmaster_arrange:
 *
 * Arranges clients with master(s) in the center and stack windows
 * split between left and right sides.
 *
 * @monitor and @clients are passed by the compositor.  @area is
 * a pointer to the usable area (struct wlr_box *) but since this
 * is a layout computation module, we receive geometry through the
 * interface parameters.
 *
 * Portrait monitors transpose the layout into a central horizontal band.
 */
static void
centeredmaster_arrange(
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
	gint n, i, nmaster;
	gint center_w, left_w, right_w, center_x, right_x;
	gint my, ly, ry;
	gint left_n, right_n, stack_n;
	gdouble mfact;

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

	nmaster = gowl_monitor_get_nmaster(m);
	if (nmaster < 0)
		nmaster = 0;
	mfact = gowl_monitor_get_mfact(m);
	if (mfact <= 0.0 || mfact >= 1.0)
		mfact = 0.55;

	/* Everything fits in the master column: one full-width stack, which
	 * is what makes this layout usable with a single window rather than
	 * leaving two empty side columns. */
	if (n <= nmaster || nmaster == 0) {
		my = a->y;
		i = 0;
		for (l = clients; l != NULL; l = l->next, i++) {
			gint h = (a->height - (my - a->y)) / (n - i);

			gowl_layout_place_oriented(comp, (GowlClient *)l->data, portrait,
			                              a->x, my, a->width, h);
			my += h;
		}
		return;
	}

	center_w = (gint)((gdouble)a->width * mfact);
	left_w   = (a->width - center_w) / 2;
	right_w  = a->width - center_w - left_w;
	center_x = a->x + left_w;
	right_x  = center_x + center_w;

	/* Stack alternates left and right, so the left column takes the
	 * ceiling when the count is odd. */
	stack_n = n - nmaster;
	left_n  = (stack_n + 1) / 2;
	right_n = stack_n - left_n;

	my = ly = ry = a->y;
	i = 0;
	for (l = clients; l != NULL; l = l->next, i++) {
		GowlClient *c = (GowlClient *)l->data;

		if (i < nmaster) {
			gint h = (a->height - (my - a->y)) / (nmaster - i);

			gowl_layout_place_oriented(comp, c, portrait, center_x, my,
			                              center_w, h);
			my += h;
		} else if ((i - nmaster) % 2 == 0 && left_n > 0) {
			gint idx = (i - nmaster) / 2;
			gint h = (a->height - (ly - a->y)) / (left_n - idx);

			gowl_layout_place_oriented(comp, c, portrait, a->x, ly,
			                              left_w, h);
			ly += h;
		} else if (right_n > 0) {
			gint idx = (i - nmaster - 1) / 2;
			gint h = (a->height - (ry - a->y)) / (right_n - idx);

			gowl_layout_place_oriented(comp, c, portrait, right_x, ry,
			                              right_w, h);
			ry += h;
		}
	}
}

static const gchar *
centeredmaster_get_symbol(GowlLayoutProvider *self)
{
	(void)self;
	return "|M|";
}

static void
centeredmaster_layout_init(GowlLayoutProviderInterface *iface)
{
	iface->arrange    = centeredmaster_arrange;
	iface->get_symbol = centeredmaster_get_symbol;
}

/* --- GObject lifecycle --- */

static void
gowl_module_centeredmaster_class_init(GowlModuleCenteredmasterClass *klass)
{
	GowlModuleClass *mod_class;

	mod_class = GOWL_MODULE_CLASS(klass);

	mod_class->activate        = centeredmaster_activate;
	mod_class->get_name        = centeredmaster_get_name;
	mod_class->get_description = centeredmaster_get_description;
	mod_class->get_version     = centeredmaster_get_version;
}

static void
gowl_module_centeredmaster_init(GowlModuleCenteredmaster *self)
{
	(void)self;
}

/* --- Shared-object entry point --- */

G_MODULE_EXPORT GType
gowl_module_register(void)
{
	return GOWL_TYPE_MODULE_CENTEREDMASTER;
}
