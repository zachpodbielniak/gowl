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
#define G_LOG_DOMAIN "gowl-fibonacci"

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
 * GowlModuleFibonacci:
 *
 * Fibonacci spiral tiling layout.  Each successive window occupies
 * half the remaining space, with the split direction alternating
 * between horizontal and vertical.
 *
 * Layout (4 clients):
 *   +----------+-----+
 *   |          |  2  |
 *   |    1     +--+--+
 *   |          | 3| 4|
 *   +----------+--+--+
 *
 * Also known as "dwindle" layout in some window managers.
 */

#define GOWL_TYPE_MODULE_FIBONACCI (gowl_module_fibonacci_get_type())
G_DECLARE_FINAL_TYPE(GowlModuleFibonacci, gowl_module_fibonacci,
                     GOWL, MODULE_FIBONACCI, GowlModule)

struct _GowlModuleFibonacci {
	GowlModule parent_instance;
};

static void fibonacci_layout_init(GowlLayoutProviderInterface *iface);

G_DEFINE_TYPE_WITH_CODE(GowlModuleFibonacci, gowl_module_fibonacci,
	GOWL_TYPE_MODULE,
	G_IMPLEMENT_INTERFACE(GOWL_TYPE_LAYOUT_PROVIDER,
		fibonacci_layout_init))

/* --- GowlModule virtual methods --- */

static gboolean
fibonacci_activate(GowlModule *mod)
{
	(void)mod;
	return TRUE;
}

static const gchar *
fibonacci_get_name(GowlModule *mod)
{
	(void)mod;
	return "fibonacci";
}

static const gchar *
fibonacci_get_description(GowlModule *mod)
{
	(void)mod;
	return "Fibonacci spiral tiling layout";
}

static const gchar *
fibonacci_get_version(GowlModule *mod)
{
	(void)mod;
	return "0.1.0";
}

/* --- GowlLayoutProvider --- */

/**
 * fibonacci_arrange:
 *
 * Fibonacci (dwindle) layout.  The first window takes the full
 * area.  Each subsequent window takes half of the remaining space,
 * alternating the split direction.
 *
 * Portrait monitors start with a horizontal split.
 */
static void
fibonacci_arrange(
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
	gint x, y, w, h;

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

	/*
	 * Spiral: each window takes half of what is left, alternating the
	 * axis it splits on, and the last one takes the whole remainder.
	 * Splitting on the longer edge each time would be the "dwindle"
	 * variant; alternating is what makes it a spiral.
	 */
	x = a->x;
	y = a->y;
	w = a->width;
	h = a->height;

	i = 0;
	for (l = clients; l != NULL; l = l->next, i++) {
		GowlClient *c = (GowlClient *)l->data;
		gint cw = w, ch = h;

		if (l->next != NULL) {
			if (i % 2 == 0)
				cw = w / 2;
			else
				ch = h / 2;
		}

		/* A window narrower or shorter than this is not a window any
		 * more; stop splitting and let the rest share the remainder. */
		if (cw < 40 || ch < 40) {
			cw = w;
			ch = h;
		}

		gowl_layout_place_oriented(comp, c, portrait, x, y, cw, ch);

		if (l->next == NULL)
			break;

		if (cw != w) {
			x += cw;
			w -= cw;
		} else if (ch != h) {
			y += ch;
			h -= ch;
		} else {
			break;      /* remainder too small to split further */
		}
	}
}

static const gchar *
fibonacci_get_symbol(GowlLayoutProvider *self)
{
	(void)self;
	return "[@]";
}

static void
fibonacci_layout_init(GowlLayoutProviderInterface *iface)
{
	iface->arrange    = fibonacci_arrange;
	iface->get_symbol = fibonacci_get_symbol;
}

/* --- GObject lifecycle --- */

static void
gowl_module_fibonacci_class_init(GowlModuleFibonacciClass *klass)
{
	GowlModuleClass *mod_class;

	mod_class = GOWL_MODULE_CLASS(klass);

	mod_class->activate        = fibonacci_activate;
	mod_class->get_name        = fibonacci_get_name;
	mod_class->get_description = fibonacci_get_description;
	mod_class->get_version     = fibonacci_get_version;
}

static void
gowl_module_fibonacci_init(GowlModuleFibonacci *self)
{
	(void)self;
}

/* --- Shared-object entry point --- */

G_MODULE_EXPORT GType
gowl_module_register(void)
{
	return GOWL_TYPE_MODULE_FIBONACCI;
}
