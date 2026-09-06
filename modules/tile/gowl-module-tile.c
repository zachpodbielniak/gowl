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
gowl_compositor_layout_tile(
	GowlCompositor *self,
	GowlMonitor    *m
){
	guint mw, my, ty;
	gint i, n;
	gint ih, iv, oh, ov;
	gint aw, ah, ax, ay;
	GList *l;

	/* Count visible tiling clients */
	n = 0;
	for (l = self->clients; l != NULL; l = l->next) {
		GowlClient *c = (GowlClient *)l->data;
		if (VISIBLEON(c, m) && !c->isfloating && !c->isfullscreen)
			n++;
	}
	if (n == 0)
		return;

	/* Query gap provider for gap values */
	ih = iv = oh = ov = 0;
	if (self->module_mgr != NULL)
		gowl_module_manager_get_gaps(self->module_mgr, (gpointer)m,
		                             &ih, &iv, &oh, &ov);

	/* Compute usable area after outer gaps */
	ax = m->w.x + oh;
	ay = m->w.y + ov;
	aw = m->w.width - 2 * oh;
	ah = m->w.height - 2 * ov;

	if (aw <= 0 || ah <= 0)
		return;

	if (m->vsplit) {
		/* vsplit: master row on top, stack row on bottom, both
		 * subdivided along X.  Transpose of the normal layout:
		 * width and height swap roles, and the inner-gap roles
		 * swap with them (iv -> gap between master/stack rows,
		 * ih -> gap between side-by-side windows in a row).
		 * mfact now sizes the master HEIGHT. */
		guint mh, mx, tx;
		if (n > m->nmaster)
			mh = m->nmaster ? (guint)roundf((float)ah * (float)m->mfact) : 0;
		else
			mh = (guint)ah;
		i = 0;
		mx = tx = 0;
		for (l = self->clients; l != NULL; l = l->next) {
			GowlClient *c = (GowlClient *)l->data;
			struct wlr_box geo;
			gint remaining;
			gint nmaster_count;

			if (!VISIBLEON(c, m) || c->isfloating || c->isfullscreen)
				continue;

			nmaster_count = m->nmaster < n ? m->nmaster : n;

			if (i < nmaster_count) {
				/* master row (top) */
				remaining = nmaster_count - i;
				geo.x = ax + (gint)mx;
				geo.y = ay;
				geo.width = (aw - (gint)mx - (remaining - 1) * ih) / remaining;
				geo.height = (gint)mh - (n > nmaster_count ? iv / 2 : 0);
				gowl_compositor_place_client(self, c, geo.x, geo.y, geo.width, geo.height);
				mx += (guint)c->geom.width + (guint)ih;
			} else {
				/* stack row (bottom) */
				remaining = n - i;
				geo.x = ax + (gint)tx;
				geo.y = ay + (gint)mh + (nmaster_count > 0 ? iv / 2 : 0);
				geo.width = (aw - (gint)tx - (remaining - 1) * ih) / remaining;
				geo.height = ah - (gint)mh - (nmaster_count > 0 ? iv / 2 : 0);
				gowl_compositor_place_client(self, c, geo.x, geo.y, geo.width, geo.height);
				tx += (guint)c->geom.width + (guint)ih;
			}
			i++;
		}
		return;
	}

	if (n > m->nmaster)
		mw = m->nmaster ? (guint)roundf((float)aw * (float)m->mfact) : 0;
	else
		mw = (guint)aw;

	i = 0;
	my = ty = 0;
	for (l = self->clients; l != NULL; l = l->next) {
		GowlClient *c = (GowlClient *)l->data;
		struct wlr_box geo;
		gint remaining;
		gint nmaster_count;

		if (!VISIBLEON(c, m) || c->isfloating || c->isfullscreen)
			continue;

		nmaster_count = m->nmaster < n ? m->nmaster : n;

		if (i < nmaster_count) {
			/* Master area (left side) */
			remaining = nmaster_count - i;
			geo.x = ax;
			geo.y = ay + (gint)my;
			geo.width = (gint)mw - (n > nmaster_count ? ih / 2 : 0);
			geo.height = (ah - (gint)my - (remaining - 1) * iv) / remaining;
			gowl_compositor_place_client(self, c, geo.x, geo.y, geo.width, geo.height);
			my += (guint)c->geom.height + (guint)iv;
		} else {
			/* Stack area (right side) */
			remaining = n - i;
			geo.x = ax + (gint)mw + (nmaster_count > 0 ? ih / 2 : 0);
			geo.y = ay + (gint)ty;
			geo.width = aw - (gint)mw - (nmaster_count > 0 ? ih / 2 : 0);
			geo.height = (ah - (gint)ty - (remaining - 1) * iv) / remaining;
			gowl_compositor_place_client(self, c, geo.x, geo.y, geo.width, geo.height);
			ty += (guint)c->geom.height + (guint)iv;
		}
		i++;
	}
}

typedef struct { GowlModule parent; } TileLayoutModule;
typedef struct { GowlModuleClass parent; } TileLayoutModuleClass;
static void provider_init(GowlLayoutProviderInterface *iface);
G_DEFINE_TYPE_WITH_CODE(TileLayoutModule, gowl_tile_module, GOWL_TYPE_MODULE,
 G_IMPLEMENT_INTERFACE(GOWL_TYPE_LAYOUT_PROVIDER, provider_init))
static gboolean activate(GowlModule *m) { return TRUE; }
static const gchar *name_of(GowlModule *m) { return "tile"; }
static const gchar *symbol_of(GowlLayoutProvider *p) { return "[]="; }
static void arrange(GowlLayoutProvider *p, gpointer monitor, GList *clients, gpointer area)
{
 GowlMonitor *m = monitor;
 if (m != NULL && m->compositor != NULL) gowl_compositor_layout_tile(m->compositor, m);
}
static void provider_init(GowlLayoutProviderInterface *iface)
{ iface->arrange = arrange; iface->get_symbol = symbol_of;  }
static void gowl_tile_module_class_init(TileLayoutModuleClass *klass)
{ GowlModuleClass *m = GOWL_MODULE_CLASS(klass); m->activate = activate; m->get_name = name_of; }
static void gowl_tile_module_init(TileLayoutModule *self) { }
G_MODULE_EXPORT GType gowl_module_register(void) { return gowl_tile_module_get_type(); }
