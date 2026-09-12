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
 * gowl-module-deck.c - Master beside a deck of full-size stacked windows.
 *
 * dwm's deck patch.  The master column is ordinary tile; the stack is
 * not divided at all -- every stack window gets the whole stack area,
 * piled one on another, and the focused one is on top.
 *
 * That is the point: with eight terminals open, tile gives each a
 * strip too short to read, while deck gives the one you are looking at
 * half the screen and keeps the rest a keystroke away.  The compositor
 * raises the focused window, so no stacking work is needed here.
 *
 * Like every gowl layout this works in the oriented box: the shared
 * helper transposes a portrait output so the arithmetic below only
 * ever has to think in one orientation, and places each window back
 * through the same transform.
 */

#undef G_LOG_DOMAIN
#define G_LOG_DOMAIN "gowl-deck"

#include <glib-object.h>
#include <gmodule.h>

#include "module/gowl-module.h"
#include "interfaces/gowl-layout-provider.h"
#include "core/gowl-compositor.h"
#include "core/gowl-monitor.h"
#include "core/gowl-client.h"
#include <wlr/util/box.h>
#include "../layout-axis.h"

#define GOWL_TYPE_MODULE_DECK (gowl_module_deck_get_type())
G_DECLARE_FINAL_TYPE(GowlModuleDeck, gowl_module_deck,
                     GOWL, MODULE_DECK, GowlModule)

struct _GowlModuleDeck {
	GowlModule parent_instance;
};

static void deck_layout_init(GowlLayoutProviderInterface *iface);

G_DEFINE_TYPE_WITH_CODE(GowlModuleDeck, gowl_module_deck,
	GOWL_TYPE_MODULE,
	G_IMPLEMENT_INTERFACE(GOWL_TYPE_LAYOUT_PROVIDER, deck_layout_init))

static gboolean
deck_activate(GowlModule *mod)
{
	(void)mod;
	return TRUE;
}

static const gchar *
deck_get_name(GowlModule *mod)
{
	(void)mod;
	return "deck";
}

static const gchar *
deck_get_description(GowlModule *mod)
{
	(void)mod;
	return "Deck tiling: master, with the stack piled full-size behind it";
}

static const gchar *
deck_get_version(GowlModule *mod)
{
	(void)mod;
	return "0.1.0";
}

static const gchar *
deck_get_symbol(GowlLayoutProvider *self)
{
	(void)self;
	return "[D]";
}

static void
deck_arrange(
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
		gint master_w, my;

		if (nmaster < 0)
			nmaster = 0;
		if (mfact <= 0.0 || mfact >= 1.0)
			mfact = 0.55;
		if (nmaster > n)
			nmaster = n;

		master_w = (nmaster == 0 || nmaster == n)
			? a->width : (gint)((gdouble)a->width * mfact);

		my = a->y;
		i = 0;
		for (l = clients; l != NULL; l = l->next, i++) {
			GowlClient *c = (GowlClient *)l->data;

			if (i < nmaster) {
				gint h = (a->y + a->height - my) / (nmaster - i);

				gowl_layout_place_oriented(comp, c, portrait,
					a->x, my, master_w, h);
				my += h;
			} else {
				/* Every one of them, the same box.  They overlap by
				 * design; whichever has focus is raised. */
				gowl_layout_place_oriented(comp, c, portrait,
					a->x + master_w, a->y,
					a->width - master_w, a->height);
			}
		}
	}
}

static void
deck_layout_init(GowlLayoutProviderInterface *iface)
{
	iface->arrange    = deck_arrange;
	iface->get_symbol = deck_get_symbol;
}

static void
gowl_module_deck_class_init(GowlModuleDeckClass *klass)
{
	GowlModuleClass *mod_class = GOWL_MODULE_CLASS(klass);

	mod_class->activate        = deck_activate;
	mod_class->get_name        = deck_get_name;
	mod_class->get_description = deck_get_description;
	mod_class->get_version     = deck_get_version;
}

static void
gowl_module_deck_init(GowlModuleDeck *self)
{
	(void)self;
}

G_MODULE_EXPORT GType
gowl_module_register(void)
{
	return GOWL_TYPE_MODULE_DECK;
}
