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
 * NOTE: The compositor currently hardcodes tile/monocle.  This
 * layout function is registered for use when the compositor adds
 * module-based layout dispatch.
 */
static void
centeredmaster_arrange(
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
	 * Layout algorithm:
	 * 1. Count visible tiling clients (n)
	 * 2. If n <= nmaster, all go in center at full width
	 * 3. Otherwise:
	 *    - center_w = w * mfact
	 *    - left_w = (w - center_w) / 2
	 *    - right_w = w - center_w - left_w
	 *    - Master windows fill center column, stacked vertically
	 *    - Stack windows alternate left/right, stacked vertically
	 *      in each column
	 *
	 * Implementation deferred until compositor exposes layout
	 * dispatch through the module manager.
	 */
	g_debug("centeredmaster: arrange called (awaiting compositor integration)");
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
