/* Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later */

#include "gowl-effects.h"
#include "gowl-core-private.h"

static GowlSceneEffect *
provider (GowlCompositor *self)
{
	return self != NULL && self->module_mgr != NULL
	           ? gowl_module_manager_get_scene_effect (self->module_mgr)
	           : NULL;
}

gboolean
gowl_effects_client_event (GowlCompositor *self, GowlClient *c,
                           GowlSceneEffectEvent event, const struct wlr_box *previous,
                           gboolean interactive)
{
	GowlSceneEffect *p = provider (self);
	return p != NULL && GOWL_SCENE_EFFECT_GET_IFACE (p)->client_event != NULL
	       && GOWL_SCENE_EFFECT_GET_IFACE (p)->client_event (p, self, c, event,
	                                                         previous, interactive);
}

struct wlr_box
gowl_effects_geometry (GowlClient *c)
{
	struct wlr_box box = c->geom;
	GowlSceneEffect *p = provider (c->compositor);
	if (p != NULL && GOWL_SCENE_EFFECT_GET_IFACE (p)->get_geometry != NULL)
		GOWL_SCENE_EFFECT_GET_IFACE (p)->get_geometry (p, c, &box);
	return box;
}

gboolean
gowl_effects_has_geometry (GowlClient *c)
{
	struct wlr_box box;
	GowlSceneEffect *p = provider (c->compositor);
	return p != NULL && GOWL_SCENE_EFFECT_GET_IFACE (p)->get_geometry != NULL
	       && GOWL_SCENE_EFFECT_GET_IFACE (p)->get_geometry (p, c, &box);
}

void
gowl_effects_alpha_changed (GowlClient *c, gfloat alpha)
{
	GowlSceneEffect *p = provider (c->compositor);
	if (p != NULL && GOWL_SCENE_EFFECT_GET_IFACE (p)->alpha_changed != NULL)
		GOWL_SCENE_EFFECT_GET_IFACE (p)->alpha_changed (p, c, alpha);
}

struct wlr_surface *
gowl_effects_surface_at (GowlClient *c, gdouble x, gdouble y, gdouble *sx, gdouble *sy)
{
	GowlSceneEffect *p = provider (c->compositor);
	return p != NULL && GOWL_SCENE_EFFECT_GET_IFACE (p)->surface_at != NULL
	           ? GOWL_SCENE_EFFECT_GET_IFACE (p)->surface_at (p, c, x, y, sx, sy)
	           : NULL;
}

gboolean
gowl_effects_frame (GowlCompositor *self, GowlMonitor *m, gint64 now)
{
	GowlSceneEffect *p = provider (self);
	return p != NULL && GOWL_SCENE_EFFECT_GET_IFACE (p)->frame != NULL
	       && GOWL_SCENE_EFFECT_GET_IFACE (p)->frame (p, self, m, now);
}

void
gowl_effects_frame_done (GowlCompositor *self, GowlMonitor *m,
                         const struct timespec *now)
{
	GowlSceneEffect *p = provider (self);
	if (p != NULL && GOWL_SCENE_EFFECT_GET_IFACE (p)->frame_done != NULL)
		GOWL_SCENE_EFFECT_GET_IFACE (p)->frame_done (p, self, m, now);
}

void
gowl_effects_monitor_removed (GowlCompositor *self, GowlMonitor *m)
{
	GowlSceneEffect *p = provider (self);
	if (p != NULL && GOWL_SCENE_EFFECT_GET_IFACE (p)->monitor_removed != NULL)
		GOWL_SCENE_EFFECT_GET_IFACE (p)->monitor_removed (p, self, m);
}

void
gowl_effects_finish (GowlCompositor *self)
{
	GowlSceneEffect *p = provider (self);
	if (p != NULL && GOWL_SCENE_EFFECT_GET_IFACE (p)->finish != NULL)
		GOWL_SCENE_EFFECT_GET_IFACE (p)->finish (p, self);
}
