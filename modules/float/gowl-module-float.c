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
gowl_compositor_layout_float(GowlCompositor *self, GowlMonitor *m)
{
	GList *l;

	for (l = self->clients; l != NULL; l = l->next) {
		GowlClient *c = (GowlClient *)l->data;

		if (!VISIBLEON(c, m) || c->isfullscreen || c->isembedded || c->isoverlay)
			continue;
		if (c->scene == NULL)
			continue;
		if (c->scene->node.parent == self->layers[GOWL_SCENE_LAYER_FS])
			continue;

		wlr_scene_node_reparent(&c->scene->node,
		                        self->layers[GOWL_SCENE_LAYER_FLOAT]);
	}
}

typedef struct { GowlModule parent; } FloatLayoutModule;
typedef struct { GowlModuleClass parent; } FloatLayoutModuleClass;
static void provider_init(GowlLayoutProviderInterface *iface);
G_DEFINE_TYPE_WITH_CODE(FloatLayoutModule, gowl_float_module, GOWL_TYPE_MODULE,
 G_IMPLEMENT_INTERFACE(GOWL_TYPE_LAYOUT_PROVIDER, provider_init))
static gboolean activate(GowlModule *m) { return TRUE; }
static const gchar *name_of(GowlModule *m) { return "float"; }
static const gchar *symbol_of(GowlLayoutProvider *p) { return "><>"; }
static void arrange(GowlLayoutProvider *p, gpointer monitor, GList *clients, gpointer area)
{
 GowlMonitor *m = monitor;
 if (m != NULL && m->compositor != NULL) gowl_compositor_layout_float(m->compositor, m);
}
static void provider_init(GowlLayoutProviderInterface *iface)
{ iface->arrange = arrange; iface->get_symbol = symbol_of;  }
static void gowl_float_module_class_init(FloatLayoutModuleClass *klass)
{ GowlModuleClass *m = GOWL_MODULE_CLASS(klass); m->activate = activate; m->get_name = name_of; }
static void gowl_float_module_init(FloatLayoutModule *self) { }
G_MODULE_EXPORT GType gowl_module_register(void) { return gowl_float_module_get_type(); }
