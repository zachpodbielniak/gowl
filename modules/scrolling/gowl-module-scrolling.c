#undef G_LOG_DOMAIN
#define G_LOG_DOMAIN "gowl-layout"
/* Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later */
#include "core/gowl-core-private.h"
#include "core/gowl-layout-registry.h"
#include "interfaces/gowl-layout-provider.h"
#include <math.h>
#define VISIBLEON(C,M) ((C)->mon == (M) && !(C)->isembedded && !(C)->isoverlay && ((C)->tags & (M)->tagset[(M)->seltags]))
void
gowl_compositor_layout_scrolling(GowlCompositor *self, GowlMonitor *m)
{
	GList *clients, *l;
	gint ih, iv, oh, ov;
	gint ax, ay, aw, ah;
	gint col_w, strip_w, max_scroll;
	gint n, i;
	gdouble frac;

	clients = gowl_compositor_tiling_clients(self, m);
	n = (gint)g_list_length(clients);
	if (n == 0) {
		g_list_free(clients);
		return;
	}

	ih = iv = oh = ov = 0;
	if (self->module_mgr != NULL)
		gowl_module_manager_get_gaps(self->module_mgr, (gpointer)m,
		                             &ih, &iv, &oh, &ov);

	ax = m->w.x + oh;
	ay = m->w.y + ov;
	aw = m->w.width - 2 * oh;
	ah = m->w.height - 2 * ov;
	if (aw <= 0 || ah <= 0) {
		g_list_free(clients);
		return;
	}

	frac = self->config != NULL
		? gowl_config_get_scroll_column_width(self->config)
		: 0.5;
	if (frac <= 0.05) frac = 0.05;
	if (frac > 1.0)  frac = 1.0;

	col_w = (gint)((gdouble)aw * frac) - ih;
	if (col_w < 1)
		col_w = 1;

	strip_w = n * (col_w + ih) - ih;
	max_scroll = strip_w - aw;
	if (max_scroll < 0)
		max_scroll = 0;
	if (m->scroll_x < 0)
		m->scroll_x = 0;
	if (m->scroll_x > max_scroll)
		m->scroll_x = max_scroll;

	i = 0;
	for (l = clients; l != NULL; l = l->next, i++) {
		GowlClient *c = (GowlClient *)l->data;
		struct wlr_box geo;

		geo.x = ax + i * (col_w + ih) - m->scroll_x;
		geo.y = ay;
		geo.width = col_w;
		geo.height = ah;
		gowl_compositor_place_client(self, c, geo.x, geo.y, geo.width, geo.height);
	}

	g_list_free(clients);
}
void
scroll_focus(GowlLayoutProvider *provider, gpointer client)
{
	GowlClient *c = client;
	GowlCompositor *self = c->compositor;
	GowlMonitor *m;
	GList *clients;
	gint idx, ih, oh, aw, col_w, x0, x1;
	gdouble frac;

	g_return_if_fail(GOWL_IS_COMPOSITOR(self));

	if (c == NULL || c->mon == NULL)
		return;
	m = c->mon;

	{
		GowlLayoutEntry *e = gowl_layout_get(self, m);

		if (e == NULL || g_strcmp0(e->name, "scrolling") != 0)
			return;
	}

	clients = gowl_compositor_tiling_clients(self, m);
	idx = g_list_index(clients, c);
	g_list_free(clients);
	if (idx < 0)
		return;

	ih = oh = 0;
	if (self->module_mgr != NULL)
		gowl_module_manager_get_gaps(self->module_mgr, (gpointer)m,
		                             &ih, NULL, &oh, NULL);

	aw = m->w.width - 2 * oh;
	frac = self->config != NULL
		? gowl_config_get_scroll_column_width(self->config)
		: 0.5;
	if (frac <= 0.05) frac = 0.05;
	if (frac > 1.0)  frac = 1.0;

	col_w = (gint)((gdouble)aw * frac) - ih;
	if (col_w < 1)
		col_w = 1;

	x0 = idx * (col_w + ih);
	x1 = x0 + col_w;

	if (x0 < m->scroll_x)
		m->scroll_x = x0;
	else if (x1 > m->scroll_x + aw)
		m->scroll_x = x1 - aw;
	else
		return;                 /* already fully visible */

	gowl_compositor_arrange(self, m);
}
void
scroll_view(GowlLayoutProvider *provider, gpointer monitor, gint dx)
{
	GowlMonitor *m = monitor;
	GowlCompositor *self = m->compositor;
	g_return_if_fail(GOWL_IS_COMPOSITOR(self));

	if (m == NULL)
		m = self->selmon;
	if (m == NULL)
		return;

	m->scroll_x += dx;
	gowl_compositor_arrange(self, m);
}
static gboolean overflow(GowlLayoutProvider *p) { return TRUE; }

typedef struct { GowlModule parent; } ScrollingLayoutModule;
typedef struct { GowlModuleClass parent; } ScrollingLayoutModuleClass;
static void provider_init(GowlLayoutProviderInterface *iface);
G_DEFINE_TYPE_WITH_CODE(ScrollingLayoutModule, gowl_scrolling_module, GOWL_TYPE_MODULE,
 G_IMPLEMENT_INTERFACE(GOWL_TYPE_LAYOUT_PROVIDER, provider_init))
static gboolean activate(GowlModule *m) { return TRUE; }
static const gchar *name_of(GowlModule *m) { return "scrolling"; }
static const gchar *symbol_of(GowlLayoutProvider *p) { return "|||"; }
static void arrange(GowlLayoutProvider *p, gpointer monitor, GList *clients, gpointer area)
{
 GowlMonitor *m = monitor;
 if (m != NULL && m->compositor != NULL) gowl_compositor_layout_scrolling(m->compositor, m);
}
static void provider_init(GowlLayoutProviderInterface *iface)
{ iface->arrange = arrange; iface->get_symbol = symbol_of; iface->focus_client = scroll_focus; iface->scroll = scroll_view; iface->allows_overflow = overflow; }
static void gowl_scrolling_module_class_init(ScrollingLayoutModuleClass *klass)
{ GowlModuleClass *m = GOWL_MODULE_CLASS(klass); m->activate = activate; m->get_name = name_of; }
static void gowl_scrolling_module_init(ScrollingLayoutModule *self) { }
G_MODULE_EXPORT GType gowl_module_register(void) { return gowl_scrolling_module_get_type(); }
