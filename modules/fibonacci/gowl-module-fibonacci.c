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
 * NOTE: Registered for use when the compositor adds module-based
 * layout dispatch.
 */
static void
fibonacci_arrange(
	GowlLayoutProvider *self,
	gpointer            monitor,
	GList              *clients,
	gpointer            area
){
	(void)self;
	(void)monitor;
	(void)clients;
	(void)area;

	/*
	 * Fibonacci layout algorithm:
	 *
	 *   cx = area.x, cy = area.y, cw = area.w, ch = area.h
	 *   for i = 0..n-1:
	 *     if i < n-1:
	 *       if i % 2 == 0:  (vertical split)
	 *         client[i].w = cw / 2
	 *         client[i].h = ch
	 *         cw -= client[i].w
	 *         cx += client[i].w
	 *       else:            (horizontal split)
	 *         client[i].w = cw
	 *         client[i].h = ch / 2
	 *         ch -= client[i].h
	 *         cy += client[i].h
	 *     else:
	 *       client[i] gets remaining (cx, cy, cw, ch)
	 *
	 * Implementation deferred until compositor exposes layout
	 * dispatch through the module manager.
	 */
	g_debug("fibonacci: arrange called (awaiting compositor integration)");
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
