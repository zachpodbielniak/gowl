/* Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later */

#ifndef GOWL_SCENE_EFFECT_H
#define GOWL_SCENE_EFFECT_H

#include <glib-object.h>
#include <time.h>

G_BEGIN_DECLS

typedef struct _GowlCompositor GowlCompositor;
typedef struct _GowlClient GowlClient;
typedef struct _GowlMonitor GowlMonitor;
struct wlr_surface;
struct wlr_box;

typedef enum {
	GOWL_SCENE_EFFECT_GEOMETRY,
	GOWL_SCENE_EFFECT_REVEAL,
	GOWL_SCENE_EFFECT_RELEASE,
	GOWL_SCENE_EFFECT_UNMAP,
	GOWL_SCENE_EFFECT_DESTROY,
	GOWL_SCENE_EFFECT_KEYBOARD_FOCUS,
	GOWL_SCENE_EFFECT_OVERLAY_SHOW,
	GOWL_SCENE_EFFECT_OVERLAY_HIDE
} GowlSceneEffectEvent;

#define GOWL_TYPE_SCENE_EFFECT (gowl_scene_effect_get_type ())
G_DECLARE_INTERFACE (GowlSceneEffect, gowl_scene_effect, GOWL, SCENE_EFFECT, GObject)

/* Native scene hooks, called on the compositor thread. The first active
 * provider by module priority owns presentation; the core uses immediate
 * geometry and ordinary input when none is active. Providers own their
 * state, and must restore live surfaces on deactivate/finish.
 *
 * GEOMETRY runs after bounds are applied, before the final configure.
 * Returning TRUE means the provider placed the scene and its decoration.
 * UNMAP/DESTROY and finish run before scene nodes or the renderer disappear.
 * frame runs before output commit; TRUE requests another frame on that output.
 * frame_done runs after the ordinary visible-surface callbacks.
 * KEYBOARD_FOCUS reports a successful focus-stack change, after the
 * ordinary focus and border updates, excluding pointer focus changes. */
struct _GowlSceneEffectInterface {
	GTypeInterface parent_iface;
#ifndef __GI_SCANNER__
	/* Native wlroots hooks; only the interface type is introspected. */
	gboolean (*client_event) (GowlSceneEffect *, GowlCompositor *, GowlClient *,
	                          GowlSceneEffectEvent, const struct wlr_box *,
	                          gboolean);
	gboolean (*get_geometry) (GowlSceneEffect *, GowlClient *, struct wlr_box *);
	void (*alpha_changed) (GowlSceneEffect *, GowlClient *, gfloat);
	struct wlr_surface *(*surface_at) (GowlSceneEffect *, GowlClient *, gdouble,
	                                   gdouble, gdouble *, gdouble *);
	gboolean (*frame) (GowlSceneEffect *, GowlCompositor *, GowlMonitor *, gint64);
	void (*frame_done) (GowlSceneEffect *, GowlCompositor *, GowlMonitor *,
	                    const struct timespec *);
	void (*monitor_removed) (GowlSceneEffect *, GowlCompositor *, GowlMonitor *);
	void (*finish) (GowlSceneEffect *, GowlCompositor *);
#endif
};

G_END_DECLS
#endif
