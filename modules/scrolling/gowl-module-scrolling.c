#undef G_LOG_DOMAIN
#define G_LOG_DOMAIN "gowl-layout"
/* Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later */
#include "core/gowl-core-private.h"
#include "core/gowl-layout-registry.h"
#include "interfaces/gowl-layout-provider.h"
#include <math.h>
#include "../layout-axis.h"

typedef struct {
	gboolean portrait;
	struct wlr_box area;
	gint gap;
	gint size;
} ScrollMetrics;

/* Work along a horizontal virtual axis, transposing for portrait outputs.
 * Arrangement and focus visibility must use exactly the same measurements. */
static ScrollMetrics
scroll_metrics(GowlCompositor *self, GowlMonitor *m)
{
	gint ih = 0, iv = 0, oh = 0, ov = 0;
	gdouble fraction;
	ScrollMetrics metrics;

	if (self->module_mgr != NULL)
		gowl_module_manager_get_gaps(self->module_mgr, m,
		                             &ih, &iv, &oh, &ov);
	metrics.portrait = gowl_layout_is_portrait(m);
	metrics.area = gowl_layout_axis_box((struct wlr_box){
		m->w.x + oh, m->w.y + ov,
		m->w.width - 2 * oh, m->w.height - 2 * ov}, metrics.portrait);
	metrics.gap = metrics.portrait ? iv : ih;
	fraction = self->config != NULL
		? gowl_config_get_scroll_column_width(self->config) : 0.5;
	fraction = CLAMP(fraction, 0.05, 1.0);
	metrics.size = MAX(1, (gint)(metrics.area.width * fraction) - metrics.gap);
	return metrics;
}

void
gowl_compositor_layout_scrolling(GowlCompositor *self, GowlMonitor *m)
{
	ScrollMetrics metrics = scroll_metrics(self, m);
	GList *clients, *l;
	gint n, i, maximum;

	if (metrics.area.width <= 0 || metrics.area.height <= 0)
		return;
	clients = gowl_compositor_tiling_clients(self, m);
	n = (gint)g_list_length(clients);
	maximum = MAX(0, n * (metrics.size + metrics.gap) - metrics.gap
	                 - metrics.area.width);
	/* The legacy scroll_x field is the offset along the current axis. */
	m->scroll_x = CLAMP(m->scroll_x, 0, maximum);
	for (l = clients, i = 0; l != NULL; l = l->next, i++) {
		gowl_layout_place_oriented(self, l->data, metrics.portrait,
			metrics.area.x + i * (metrics.size + metrics.gap) - m->scroll_x,
			metrics.area.y, metrics.size, metrics.area.height);
	}
	g_list_free(clients);
}

static void
scroll_focus(GowlLayoutProvider *provider, gpointer client)
{
	GowlClient *c = client;
	GowlCompositor *self;
	GowlMonitor *m;
	GowlLayoutEntry *entry;
	ScrollMetrics metrics;
	GList *clients;
	gint index, start, end;

	if (c == NULL || c->mon == NULL)
		return;
	self = c->compositor;
	m = c->mon;
	g_return_if_fail(GOWL_IS_COMPOSITOR(self));
	entry = gowl_layout_get(self, m);
	if (entry == NULL || g_strcmp0(entry->name, "scrolling") != 0)
		return;
	clients = gowl_compositor_tiling_clients(self, m);
	index = g_list_index(clients, c);
	g_list_free(clients);
	if (index < 0)
		return;
	metrics = scroll_metrics(self, m);
	if (metrics.area.width <= 0 || metrics.area.height <= 0)
		return;
	start = index * (metrics.size + metrics.gap);
	end = start + metrics.size;
	if (start < m->scroll_x)
		m->scroll_x = start;
	else if (end > m->scroll_x + metrics.area.width)
		m->scroll_x = end - metrics.area.width;
	else
		return;
	gowl_compositor_arrange(self, m);
}

static void
scroll_view(GowlLayoutProvider *provider, gpointer monitor, gint delta)
{
	GowlMonitor *m = monitor;
	if (m == NULL)
		return;
	g_return_if_fail(GOWL_IS_COMPOSITOR(m->compositor));
	m->scroll_x += delta;
	gowl_compositor_arrange(m->compositor, m);
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
