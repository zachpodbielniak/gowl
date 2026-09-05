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
 * gowl-layout-registry.c -- one list of layouts, built-in and modular
 *
 * gowl advertised a GowlLayoutProvider interface as one of its typed
 * extension points, the module manager collected every module that
 * implemented it into a priority-sorted array -- and nothing ever read
 * that array.  arrange() called tile() or monocle() directly, chosen by
 * a `sellt' index that could only be 0 or 1.
 *
 * The consequences were all silent:
 *
 *   - `centeredmaster' and `fibonacci' shipped as modules and did
 *     nothing when loaded.
 *   - the `float' layout was unreachable: SET_LAYOUT mapped every arg
 *     that was not "monocle" onto tile, so selecting float gave tile.
 *   - arrange() overwrote layout_symbol with a hardcoded "[]=" on every
 *     pass, so the bar, gowl-get-layout and the IPC event all reported
 *     tile no matter what was running.
 *
 * This replaces the index with a registry keyed by name, in which a
 * built-in and a module layout are peers.  A monitor stores the name it
 * selected, so a layout module that loads later, or unloads, cannot
 * leave a monitor pointing at a stale index into a changed array.
 */

#include "gowl-layout-registry.h"
#include "gowl-core-private.h"
#include "gowl-compositor.h"
#include "../interfaces/gowl-layout-provider.h"
#include "../module/gowl-module.h"
#include "../module/gowl-module-manager.h"

/* ── Entry lifetime ──────────────────────────────────────────────── */

static void
layout_entry_free(gpointer data)
{
	GowlLayoutEntry *e = data;

	if (e == NULL)
		return;
	g_free(e->name);
	g_free(e->symbol);
	g_free(e);
}

void
gowl_layout_registry_init(GowlCompositor *self)
{
	g_return_if_fail(GOWL_IS_COMPOSITOR(self));

	if (self->layouts != NULL)
		return;

	self->layouts = g_ptr_array_new_with_free_func(layout_entry_free);

	/* The built-ins.  Registration order is cycle order, and the first
	 * is the default a monitor starts in. */
	gowl_layout_register(self, "tile", "[]=",
	                      gowl_compositor_layout_tile, NULL);
	gowl_layout_register(self, "monocle", "[M]",
	                      gowl_compositor_layout_monocle, NULL);
	gowl_layout_register(self, "float", "><>",
	                      gowl_compositor_layout_float, NULL);
	gowl_layout_register(self, "scrolling", "|||",
	                      gowl_compositor_layout_scrolling, NULL);
}

void
gowl_layout_registry_finish(GowlCompositor *self)
{
	g_return_if_fail(GOWL_IS_COMPOSITOR(self));

	g_clear_pointer(&self->layouts, g_ptr_array_unref);
}

/* ── Registration ────────────────────────────────────────────────── */

gboolean
gowl_layout_register(GowlCompositor        *self,
                      const gchar           *name,
                      const gchar           *symbol,
                      GowlLayoutArrangeFunc  arrange,
                      gpointer               provider)
{
	GowlLayoutEntry *e;
	guint i;

	g_return_val_if_fail(GOWL_IS_COMPOSITOR(self), FALSE);
	g_return_val_if_fail(name != NULL, FALSE);
	g_return_val_if_fail(arrange != NULL || provider != NULL, FALSE);

	if (self->layouts == NULL)
		return FALSE;

	/* Replace in place rather than appending a duplicate: a module that
	 * takes a built-in's name is overriding it, and two entries with the
	 * same name would make lookup order decide which wins. */
	for (i = 0; i < self->layouts->len; i++) {
		e = g_ptr_array_index(self->layouts, i);
		if (g_strcmp0(e->name, name) == 0) {
			g_free(e->symbol);
			e->symbol = g_strdup(symbol ? symbol : name);
			e->arrange = arrange;
			e->provider = provider;
			return TRUE;
		}
	}

	e = g_new0(GowlLayoutEntry, 1);
	e->name = g_strdup(name);
	e->symbol = g_strdup(symbol ? symbol : name);
	e->arrange = arrange;
	e->provider = provider;
	g_ptr_array_add(self->layouts, e);
	return TRUE;
}

gboolean
gowl_layout_unregister(GowlCompositor *self, const gchar *name)
{
	guint i;

	g_return_val_if_fail(GOWL_IS_COMPOSITOR(self), FALSE);
	g_return_val_if_fail(name != NULL, FALSE);

	if (self->layouts == NULL)
		return FALSE;

	for (i = 0; i < self->layouts->len; i++) {
		GowlLayoutEntry *e = g_ptr_array_index(self->layouts, i);

		if (g_strcmp0(e->name, name) != 0)
			continue;

		g_ptr_array_remove_index(self->layouts, i);

		/* Any monitor still naming it falls back to the first
		 * layout, so unloading a module cannot leave a monitor
		 * pointing at a layout that no longer exists. */
		{
			GList *l;

			for (l = self->monitors; l != NULL; l = l->next) {
				GowlMonitor *m = l->data;

				if (g_strcmp0(m->layout_name, name) == 0) {
					g_free(m->layout_name);
					m->layout_name =
						self->layouts->len > 0
						? g_strdup(((GowlLayoutEntry *)
						            g_ptr_array_index(
							    self->layouts, 0))->name)
						: NULL;
					gowl_compositor_arrange(self, m);
				}
			}
		}
		return TRUE;
	}
	return FALSE;
}

GowlLayoutEntry *
gowl_layout_lookup(GowlCompositor *self, const gchar *name)
{
	guint i;

	g_return_val_if_fail(GOWL_IS_COMPOSITOR(self), NULL);

	if (self->layouts == NULL || name == NULL)
		return NULL;

	for (i = 0; i < self->layouts->len; i++) {
		GowlLayoutEntry *e = g_ptr_array_index(self->layouts, i);

		if (g_strcmp0(e->name, name) == 0)
			return e;
	}
	return NULL;
}

GList *
gowl_layout_list(GowlCompositor *self)
{
	GList *out = NULL;
	guint i;

	g_return_val_if_fail(GOWL_IS_COMPOSITOR(self), NULL);

	if (self->layouts == NULL)
		return NULL;

	for (i = 0; i < self->layouts->len; i++) {
		GowlLayoutEntry *e = g_ptr_array_index(self->layouts, i);

		out = g_list_prepend(out, e->name);
	}
	return g_list_reverse(out);
}

/* ── Selection ───────────────────────────────────────────────────── */

static GowlMonitor *
resolve_monitor(GowlCompositor *self, GowlMonitor *monitor)
{
	return monitor != NULL ? monitor : self->selmon;
}

GowlLayoutEntry *
gowl_layout_get(GowlCompositor *self, GowlMonitor *monitor)
{
	GowlMonitor *m;

	g_return_val_if_fail(GOWL_IS_COMPOSITOR(self), NULL);

	m = resolve_monitor(self, monitor);
	if (m == NULL || self->layouts == NULL || self->layouts->len == 0)
		return NULL;

	/* A monitor with no name yet uses the first registered layout,
	 * which is how a freshly created monitor starts in tile without
	 * every creation path having to say so. */
	if (m->layout_name == NULL)
		return g_ptr_array_index(self->layouts, 0);

	return gowl_layout_lookup(self, m->layout_name);
}

gboolean
gowl_layout_set(GowlCompositor *self, GowlMonitor *monitor, const gchar *name)
{
	GowlMonitor *m;
	GowlLayoutEntry *e;

	g_return_val_if_fail(GOWL_IS_COMPOSITOR(self), FALSE);

	m = resolve_monitor(self, monitor);
	if (m == NULL)
		return FALSE;

	e = gowl_layout_lookup(self, name);
	if (e == NULL) {
		/* Naming a layout that is not registered is a config or
		 * keybind error, and silently doing tile is how `float'
		 * looked like it worked for so long. */
		g_warning("gowl: no such layout '%s'", name ? name : "(null)");
		return FALSE;
	}

	g_free(m->layout_name);
	m->layout_name = g_strdup(e->name);
	gowl_compositor_arrange(self, m);
	return TRUE;
}

const gchar *
gowl_layout_cycle(GowlCompositor *self, GowlMonitor *monitor, gint step)
{
	GowlMonitor *m;
	GowlLayoutEntry *cur;
	guint i, n;
	gint idx = 0;

	g_return_val_if_fail(GOWL_IS_COMPOSITOR(self), NULL);

	m = resolve_monitor(self, monitor);
	if (m == NULL || self->layouts == NULL)
		return NULL;

	n = self->layouts->len;
	if (n == 0)
		return NULL;

	cur = gowl_layout_get(self, m);
	for (i = 0; i < n; i++) {
		if (g_ptr_array_index(self->layouts, i) == cur) {
			idx = (gint)i;
			break;
		}
	}

	/* Wrap in both directions: C's % keeps the sign of the dividend, so
	 * a negative step would otherwise index out of the array. */
	idx = ((idx + step) % (gint)n + (gint)n) % (gint)n;

	{
		GowlLayoutEntry *e = g_ptr_array_index(self->layouts,
		                                        (guint)idx);

		g_free(m->layout_name);
		m->layout_name = g_strdup(e->name);
		gowl_compositor_arrange(self, m);
		return e->name;
	}
}

/* ── Applying ────────────────────────────────────────────────────── */

void
gowl_layout_apply(GowlCompositor *self, GowlMonitor *monitor)
{
	GowlLayoutEntry *e;

	g_return_if_fail(GOWL_IS_COMPOSITOR(self));
	g_return_if_fail(monitor != NULL);

	e = gowl_layout_get(self, monitor);
	if (e == NULL) {
		/* No registry (a compositor built but never started) --
		 * fall back to tile so a monitor is never left unarranged. */
		gowl_compositor_layout_tile(self, monitor);
		return;
	}

	/* The symbol comes from the layout that is about to run, rather
	 * than being hardcoded.  That one line is why every surface
	 * reported tile regardless of what was actually arranged. */
	g_free(monitor->layout_symbol);
	monitor->layout_symbol = g_strdup(e->symbol);

	if (e->arrange != NULL) {
		e->arrange(self, monitor);
		return;
	}

	if (e->provider != NULL) {
		struct wlr_box area = monitor->w;
		GList *clients = gowl_compositor_tiling_clients(self, monitor);

		gowl_layout_provider_arrange(
			(GowlLayoutProvider *)e->provider,
			(gpointer)monitor, clients, (gpointer)&area);
		g_list_free(clients);
	}
}

/* ── Module providers ────────────────────────────────────────────── */

guint
gowl_layout_adopt_providers(GowlCompositor *self)
{
	GPtrArray *providers;
	guint i, adopted = 0;

	g_return_val_if_fail(GOWL_IS_COMPOSITOR(self), 0);

	if (self->module_mgr == NULL || self->layouts == NULL)
		return 0;

	providers = gowl_module_manager_get_layout_providers(self->module_mgr);
	if (providers == NULL)
		return 0;

	for (i = 0; i < providers->len; i++) {
		gpointer p = g_ptr_array_index(providers, i);
		const gchar *name;
		const gchar *symbol;

		if (!gowl_module_get_is_active(GOWL_MODULE(p)))
			continue;

		name = gowl_module_get_name(GOWL_MODULE(p));
		if (name == NULL)
			continue;

		symbol = gowl_layout_provider_get_symbol(
			(GowlLayoutProvider *)p);

		if (gowl_layout_register(self, name, symbol, NULL, p))
			adopted++;
	}

	return adopted;
}
