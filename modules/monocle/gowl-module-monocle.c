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
gowl_compositor_layout_monocle(
	GowlCompositor *self,
	GowlMonitor    *m
){
	GList *l;
	GowlClient *top;
	gint n;
	gint oh, ov;
	struct wlr_box area;

	/* Query gap provider for outer gaps only (monocle has no inner gaps) */
	oh = ov = 0;
	if (self->module_mgr != NULL)
		gowl_module_manager_get_gaps(self->module_mgr, (gpointer)m,
		                             NULL, NULL, &oh, &ov);

	area.x = m->w.x + oh;
	area.y = m->w.y + ov;
	area.width = m->w.width - 2 * oh;
	area.height = m->w.height - 2 * ov;

	n = 0;
	for (l = self->clients; l != NULL; l = l->next) {
		GowlClient *c = (GowlClient *)l->data;
		if (!VISIBLEON(c, m) || c->isfloating || c->isfullscreen)
			continue;
		gowl_compositor_place_client(self, c, area.x, area.y, area.width, area.height);
		n++;
	}
	if (n > 0) {
		g_free(m->layout_symbol);
		m->layout_symbol = g_strdup_printf("[%d]", n);
	}
	top = gowl_compositor_get_focused_client(self);
	if (top != NULL)
		wlr_scene_node_raise_to_top(&top->scene->node);
}

typedef struct { GowlModule parent; } MonocleLayoutModule;
typedef struct { GowlModuleClass parent; } MonocleLayoutModuleClass;
static void provider_init(GowlLayoutProviderInterface *iface);
G_DEFINE_TYPE_WITH_CODE(MonocleLayoutModule, gowl_monocle_module, GOWL_TYPE_MODULE,
 G_IMPLEMENT_INTERFACE(GOWL_TYPE_LAYOUT_PROVIDER, provider_init))
static gboolean activate(GowlModule *m) { return TRUE; }
static const gchar *name_of(GowlModule *m) { return "monocle"; }
static const gchar *symbol_of(GowlLayoutProvider *p) { return "[M]"; }
static void arrange(GowlLayoutProvider *p, gpointer monitor, GList *clients, gpointer area)
{
 GowlMonitor *m = monitor;
 if (m != NULL && m->compositor != NULL) gowl_compositor_layout_monocle(m->compositor, m);
}
static void provider_init(GowlLayoutProviderInterface *iface)
{ iface->arrange = arrange; iface->get_symbol = symbol_of;  }
static void gowl_monocle_module_class_init(MonocleLayoutModuleClass *klass)
{ GowlModuleClass *m = GOWL_MODULE_CLASS(klass); m->activate = activate; m->get_name = name_of; }
static void gowl_monocle_module_init(MonocleLayoutModule *self) { }
G_MODULE_EXPORT GType gowl_module_register(void) { return gowl_monocle_module_get_type(); }
