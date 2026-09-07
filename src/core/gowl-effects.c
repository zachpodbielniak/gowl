/* Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later */

/*
 * Dispatch to the scene-effect providers.
 *
 * EVERY HOOK IS ONE OF TWO KINDS, and which one it is is a property of
 * the hook rather than of the module:
 *
 *   CONSUMABLE -- client_event, get_geometry, surface_at.  These decide
 *     where a window IS and what the pointer hits.  Two modules cannot
 *     both answer, so the first provider in priority order that claims
 *     the event owns it and the rest never see it.  This is the rule
 *     that keeps two modules from fighting over one scene node.
 *
 *   BROADCAST -- alpha_changed, frame, frame_done, monitor_removed,
 *     finish.  These are "here is a tick" and "put your things away".
 *     Every provider gets them, because a module that does not receive
 *     its own teardown leaks buffers into a renderer that is going away,
 *     and a module that does not receive a frame simply stops animating.
 *
 * gowl handed the whole interface to a single provider until there were
 * enough effect modules for that to hurt: the loser had to be forwarded
 * to by hand, eight hooks at a time, and a forward left out produced no
 * error --- just a feature that silently stopped working.  Deciding per
 * event instead puts the whole question in this file, where
 * tests/test-effects.c can hold it to it.
 */

#include "gowl-effects.h"
#include "gowl-core-private.h"

/* Providers in priority order, or NULL when there are none.  Callers
 * must free the container. */
static GPtrArray *
providers (GowlCompositor *self)
{
	if (self == NULL || self->module_mgr == NULL)
		return NULL;
	return gowl_module_manager_get_scene_effects (self->module_mgr);
}

#define GOWL_EFFECTS_FOR_EACH(comp, iter)                                     \
	GPtrArray *_p = providers (comp);                                     \
	guint _i;                                                             \
	for (_i = 0; _p != NULL && _i < _p->len                               \
	             && ((iter) = GOWL_SCENE_EFFECT (g_ptr_array_index (_p, _i))); \
	     _i++)

gboolean
gowl_effects_client_event (GowlCompositor *self, GowlClient *c,
                           GowlSceneEffectEvent event, const struct wlr_box *previous,
                           gboolean interactive)
{
	GowlSceneEffect *p;
	gboolean handled = FALSE;

	{
		GOWL_EFFECTS_FOR_EACH (self, p) {
			if (GOWL_SCENE_EFFECT_GET_IFACE (p)->client_event != NULL
			    && GOWL_SCENE_EFFECT_GET_IFACE (p)->client_event (
					p, self, c, event, previous, interactive)) {
				handled = TRUE;
				break;
			}
		}
		g_clear_pointer (&_p, g_ptr_array_unref);
	}
	return handled;
}

struct wlr_box
gowl_effects_geometry (GowlClient *c)
{
	struct wlr_box box = c->geom;
	GowlSceneEffect *p;

	{
		GOWL_EFFECTS_FOR_EACH (c->compositor, p) {
			if (GOWL_SCENE_EFFECT_GET_IFACE (p)->get_geometry != NULL
			    && GOWL_SCENE_EFFECT_GET_IFACE (p)->get_geometry (p, c, &box))
				break;
		}
		g_clear_pointer (&_p, g_ptr_array_unref);
	}
	return box;
}

gboolean
gowl_effects_has_geometry (GowlClient *c)
{
	struct wlr_box box;
	GowlSceneEffect *p;
	gboolean any = FALSE;

	{
		GOWL_EFFECTS_FOR_EACH (c->compositor, p) {
			if (GOWL_SCENE_EFFECT_GET_IFACE (p)->get_geometry != NULL
			    && GOWL_SCENE_EFFECT_GET_IFACE (p)->get_geometry (p, c, &box)) {
				any = TRUE;
				break;
			}
		}
		g_clear_pointer (&_p, g_ptr_array_unref);
	}
	return any;
}

void
gowl_effects_alpha_changed (GowlClient *c, gfloat alpha)
{
	GowlSceneEffect *p;

	GOWL_EFFECTS_FOR_EACH (c->compositor, p) {
		if (GOWL_SCENE_EFFECT_GET_IFACE (p)->alpha_changed != NULL)
			GOWL_SCENE_EFFECT_GET_IFACE (p)->alpha_changed (p, c, alpha);
	}
	g_clear_pointer (&_p, g_ptr_array_unref);
}

struct wlr_surface *
gowl_effects_surface_at (GowlClient *c, gdouble x, gdouble y, gdouble *sx, gdouble *sy)
{
	GowlSceneEffect *p;
	struct wlr_surface *found = NULL;

	{
		GOWL_EFFECTS_FOR_EACH (c->compositor, p) {
			if (GOWL_SCENE_EFFECT_GET_IFACE (p)->surface_at != NULL) {
				found = GOWL_SCENE_EFFECT_GET_IFACE (p)->surface_at (
					p, c, x, y, sx, sy);
				if (found != NULL)
					break;
			}
		}
		g_clear_pointer (&_p, g_ptr_array_unref);
	}
	return found;
}

gboolean
gowl_effects_frame (GowlCompositor *self, GowlMonitor *m, gint64 now)
{
	GowlSceneEffect *p;
	gboolean live = FALSE;

	/*
	 * Every provider is ticked and the results are OR-ed: "keep sending
	 * frames" is true if ANY of them still has something to finish.
	 * Stopping at the first one to say yes would starve the others.
	 */
	GOWL_EFFECTS_FOR_EACH (self, p) {
		if (GOWL_SCENE_EFFECT_GET_IFACE (p)->frame != NULL
		    && GOWL_SCENE_EFFECT_GET_IFACE (p)->frame (p, self, m, now))
			live = TRUE;
	}
	g_clear_pointer (&_p, g_ptr_array_unref);
	return live;
}

void
gowl_effects_frame_done (GowlCompositor *self, GowlMonitor *m,
                         const struct timespec *now)
{
	GowlSceneEffect *p;

	GOWL_EFFECTS_FOR_EACH (self, p) {
		if (GOWL_SCENE_EFFECT_GET_IFACE (p)->frame_done != NULL)
			GOWL_SCENE_EFFECT_GET_IFACE (p)->frame_done (p, self, m, now);
	}
	g_clear_pointer (&_p, g_ptr_array_unref);
}

void
gowl_effects_monitor_removed (GowlCompositor *self, GowlMonitor *m)
{
	GowlSceneEffect *p;

	GOWL_EFFECTS_FOR_EACH (self, p) {
		if (GOWL_SCENE_EFFECT_GET_IFACE (p)->monitor_removed != NULL)
			GOWL_SCENE_EFFECT_GET_IFACE (p)->monitor_removed (p, self, m);
	}
	g_clear_pointer (&_p, g_ptr_array_unref);
}

void
gowl_effects_finish (GowlCompositor *self)
{
	GowlSceneEffect *p;

	/* Nothing may be skipped here.  A provider that does not run its
	 * finish is holding client buffers and GL objects into a renderer
	 * teardown, which is a crash rather than a missing effect. */
	GOWL_EFFECTS_FOR_EACH (self, p) {
		if (GOWL_SCENE_EFFECT_GET_IFACE (p)->finish != NULL)
			GOWL_SCENE_EFFECT_GET_IFACE (p)->finish (p, self);
	}
	g_clear_pointer (&_p, g_ptr_array_unref);
}
