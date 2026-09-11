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

/* Native scene hooks, called on the compositor thread.  Which provider gets
 * a hook is a property of the hook (src/core/gowl-effects.c):
 *
 *   CONSUMABLE -- client_event, get_geometry, surface_at: offered to the
 *   active providers in priority order, stopping at the first to claim it.
 *   The core uses immediate geometry and ordinary input when none does.
 *   BROADCAST -- alpha_changed, frame, frame_done, monitor_removed, finish,
 *   client_placed: every active provider gets each one.
 *
 * Providers own their state, and must restore live surfaces on
 * deactivate/finish.
 *
 * GEOMETRY runs after bounds are applied, before the final configure.
 * Returning TRUE means the provider placed the scene and its decoration.
 * UNMAP runs before the client's scene tree is destroyed.  DESTROY means
 * "let go of this client", and is sent for two things: a closing window,
 * after UNMAP, by which time its tree is gone; and a window the compositor
 * takes over as an overlay, shows in a panel or gives back, whose tree
 * stays.  A provider that hangs nodes off the client's tree must tell the
 * two apart by the nodes' own destroy signals: forgetting them while the
 * tree stays leaves them drawn on it.  finish runs before scene nodes or
 * the renderer disappear.
 * finish runs at teardown, after which nothing is dispatched, and also when
 * a GPU reset replaces the renderer -- after which hooks carry on, so a
 * provider must rebuild what it let go of, on demand, from the
 * compositor's current renderer rather than keep a pointer to the old one.
 * frame runs before output commit; TRUE requests another frame on that output.
 * frame_done runs after the ordinary visible-surface callbacks.
 * KEYBOARD_FOCUS reports a successful focus-stack change, after the
 * ordinary focus and border updates, excluding pointer focus changes.
 *
 * client_placed runs each time the compositor draws a window's frame
 * (GowlClient.frame): after whoever placed it for GEOMETRY, claimant or
 * core; on every step an effect draws it at; and on the step that lands
 * it.  GEOMETRY stops at its claimant -- the animation module claims it
 * for every tile -- so this is how a provider that only decorates a window
 * follows it, whichever provider moved it.  settled is TRUE when the window
 * is drawn at its own geometry with nothing moving it (no provider claims
 * get_geometry) and FALSE on the steps in between: keep expensive work for
 * TRUE.  It runs inside gowl_compositor_apply_frame_geometry(), so a
 * provider must not draw the frame again from it. */
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
	/* Last, so a module built against the members above keeps their
	 * offsets and simply leaves this one NULL. */
	void (*client_placed) (GowlSceneEffect *, GowlCompositor *, GowlClient *,
	                       gboolean);
#endif
};

G_END_DECLS
#endif
