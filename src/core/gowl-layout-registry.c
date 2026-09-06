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

/* Module-backed layout registry with independent state for each tag view. */

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
	g_clear_object((GObject **)&e->provider);
	g_free(e);
}

void
gowl_layout_registry_init(GowlCompositor *self)
{
	g_return_if_fail(GOWL_IS_COMPOSITOR(self));

	if (self->layouts != NULL)
		return;

	self->layouts = g_ptr_array_new_with_free_func(layout_entry_free);

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
			g_set_object((GObject **)&e->provider, provider);
			return TRUE;
		}
	}

	e = g_new0(GowlLayoutEntry, 1);
	e->name = g_strdup(name);
	e->symbol = g_strdup(symbol ? symbol : name);
	e->arrange = arrange;
	e->provider = provider != NULL ? g_object_ref(provider) : NULL;
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

		if (e->provider == NULL || gowl_module_get_is_active(e->provider))
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

typedef struct {
 gchar *name;
 gint scroll;
} TagLayout;
typedef struct {
 guint32 current;
 GHashTable *tags;
} LayoutViews;
static void tag_layout_free(gpointer data)
{
 TagLayout *tag = data;
 g_free(tag->name);
 g_free(tag);
}
static void layout_views_free(gpointer data)
{
 LayoutViews *views = data;
 g_hash_table_unref(views->tags);
 g_free(views);
}
static void sync_view(GowlMonitor *m)
{
 LayoutViews *views = g_object_get_data(G_OBJECT(m), "gowl-layout-views");
 TagLayout *tag;
 guint32 mask = m->tagset[m->seltags];
 if (views == NULL) {
  views = g_new0(LayoutViews, 1);
  views->tags = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, tag_layout_free);
  views->current = mask;
  g_object_set_data_full(G_OBJECT(m), "gowl-layout-views", views, layout_views_free);
  return;
 }
 if (views->current == mask) return;
 tag = g_new0(TagLayout, 1);
 tag->name = g_strdup(m->layout_name);
 tag->scroll = m->scroll_x;
 g_hash_table_replace(views->tags, GUINT_TO_POINTER(views->current), tag);
 tag = g_hash_table_lookup(views->tags, GUINT_TO_POINTER(mask));
 g_free(m->layout_name);
 m->layout_name = tag != NULL ? g_strdup(tag->name) : NULL;
 m->scroll_x = tag != NULL ? tag->scroll : 0;
 views->current = mask;
}

GowlLayoutEntry *
gowl_layout_get(GowlCompositor *self, GowlMonitor *monitor)
{
	GowlMonitor *m;

	g_return_val_if_fail(GOWL_IS_COMPOSITOR(self), NULL);

	m = resolve_monitor(self, monitor);
	if (m != NULL) sync_view(m);
	if (m == NULL || self->layouts == NULL || self->layouts->len == 0)
		return NULL;

	/* A monitor with no name yet uses the first registered layout,
	 * which is how a freshly created monitor starts in tile without
	 * every creation path having to say so. */
	/* A missing or disabled selection falls back to the first active plugin. */

	{
		GowlLayoutEntry *entry = gowl_layout_lookup(self, m->layout_name);
		guint i;
		if (entry != NULL && (entry->provider == NULL || gowl_module_get_is_active(entry->provider)))
			return entry;
		for (i = 0; i < self->layouts->len; i++) {
			entry = g_ptr_array_index(self->layouts, i);
			if (entry->provider == NULL || gowl_module_get_is_active(entry->provider)) return entry;
		}
		return NULL;
	}
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

	sync_view(m);
	e = gowl_layout_lookup(self, name);
	if (e == NULL || (e->provider != NULL && !gowl_module_get_is_active(e->provider))) {
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

		guint tried = 0;
		while (e->provider != NULL && !gowl_module_get_is_active(e->provider)) {
			if (++tried >= n) return NULL;
			idx = (idx + (step < 0 ? -1 : 1) + (gint)n) % (gint)n;
			e = g_ptr_array_index(self->layouts, idx);
		}
		gowl_layout_set(self, m, e->name);
		return e->name;
	}
}

/* ── Applying ────────────────────────────────────────────────────── */

void
gowl_layout_apply(GowlCompositor *self, GowlMonitor *monitor)
{
	GowlLayoutEntry *e;
	const gchar *previous;
	gboolean changed;

	g_return_if_fail(GOWL_IS_COMPOSITOR(self));
	g_return_if_fail(monitor != NULL);

	e = gowl_layout_get(self, monitor);
	if (e == NULL) {
		g_free(monitor->layout_symbol);
		monitor->layout_symbol = g_strdup("--");
		return;
	}

	previous = g_object_get_data(G_OBJECT(monitor), "gowl-presented-layout");
	changed = previous != NULL && g_strcmp0(previous, e->name) != 0;
	g_object_set_data_full(G_OBJECT(monitor), "gowl-presented-layout", g_strdup(e->name), g_free);

	/* The symbol comes from the layout that is about to run, rather
	 * than being hardcoded.  That one line is why every surface
	 * reported tile regardless of what was actually arranged. */
	g_free(monitor->layout_symbol);
	monitor->layout_symbol = g_strdup(e->symbol);

	if (e->arrange != NULL) {
		e->arrange(self, monitor);
	}

	else if (e->provider != NULL) {
		struct wlr_box area = monitor->w;
		GList *clients = gowl_compositor_tiling_clients(self, monitor);

		gowl_layout_provider_arrange(
			(GowlLayoutProvider *)e->provider,
			(gpointer)monitor, clients, (gpointer)&area);
		g_list_free(clients);
	}
	if (changed) {
		g_signal_emit_by_name(monitor, "layout-changed");
		g_signal_emit_by_name(self, "layout-changed", monitor, e->name);
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

	{
		const gchar *defaults[] = { "tile", "monocle", "float", "scrolling" };
		guint d;
		for (d = 0; d < G_N_ELEMENTS(defaults); d++) {
			GowlModule *mod = gowl_module_manager_find_module(self->module_mgr, defaults[d]);
			if (mod != NULL && gowl_module_get_is_active(mod) && GOWL_IS_LAYOUT_PROVIDER(mod))
				gowl_layout_register(self, defaults[d],
					gowl_layout_provider_get_symbol(GOWL_LAYOUT_PROVIDER(mod)), NULL, mod);
		}
	}
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

gboolean
gowl_layout_allows_overflow(GowlCompositor *self, GowlMonitor *m)
{
 GowlLayoutEntry *e = gowl_layout_get(self, m);
 GowlLayoutProviderInterface *iface;
 if (e == NULL || e->provider == NULL) return FALSE;
 iface = GOWL_LAYOUT_PROVIDER_GET_IFACE(e->provider);
 return iface->allows_overflow != NULL && iface->allows_overflow(e->provider);
}
