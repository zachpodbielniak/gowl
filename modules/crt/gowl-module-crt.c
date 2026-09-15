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

/*
 * The whole desktop, on a cathode ray tube.
 *
 * Every other effect in gowl draws BEHIND a window.  This one draws in
 * front of all of them, because the thing it is imitating is the glass:
 * a curved faceplate, scan lines, phosphor triads, halation in thick
 * glass, and three guns that do not quite agree at the corners.  The
 * optics are in src/fx/gowl-fx-crt.c, which is where the physics is
 * written down; this file is the plumbing.
 *
 * The shape is the magnifier's, and for the same reason: an effect that
 * shows LIVE content has to re-capture the screen every frame, and it is
 * drawing on the screen it is photographing.  Two things follow.
 *
 *   ITS SHEET IS A FILTER.  gowl_fx_capture() takes every
 *   GOWL_FX_SHEET_FILTER sheet out of the scene for the length of its
 *   render, so this module's capture cannot contain this module's last
 *   frame -- and, just as important, the magnifier's and the cube's
 *   captures cannot contain it either, which would have put a second
 *   tube through an already-curved picture.
 *
 *   ITS SHEET IS ABOVE EVERY LAYER.  Not because the tube must cover the
 *   bar -- though it must, a curved desktop under a flat panel is worse
 *   than no tube at all -- but because the other placements HIDE the
 *   clients they cannot cover, and a client switched off to make room
 *   would be missing from the capture as well.  A fullscreen video would
 *   simply vanish.  The session lock stays above the sheet: a lock a
 *   shader could hide is not a lock, and the module stops drawing while
 *   the session is locked anyway.
 *
 * THE POINTER IS NOT CURVED, and that is worth knowing before turning
 * this on for a working day.  The cursor is on a hardware plane, not in
 * the scene, so the capture never contains it and the shader never sees
 * it: it sits where it really is while everything under it has moved.
 * With the default curvature that is about a dozen pixels at 1080p, at
 * its worst two thirds of the way out from the middle, and none at all
 * in the centre or at the edges.  Locking software cursors would put the
 * pointer in the capture and curve it -- and would also leave wlroots
 * drawing a second, flat one on top of the sheet, because the real frame
 * renders through the same path.  `crt-curvature: 0' is the answer for
 * anybody who wants the phosphor without the parallax, and the `flat'
 * preset is exactly that.
 */

#undef G_LOG_DOMAIN
#define G_LOG_DOMAIN "gowl-crt"

#include "core/gowl-core-private.h"
#include "core/gowl-compositor.h"
#include "core/gowl-monitor.h"
#include "config/gowl-config.h"
#include "fx/gowl-fx.h"
#include "interfaces/gowl-scene-effect.h"
#include "interfaces/gowl-shutdown-handler.h"

#include <string.h>

#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_scene.h>

#include <gmodule.h>

#define GOWL_TYPE_MODULE_CRT (gowl_module_crt_get_type())
G_DECLARE_FINAL_TYPE(GowlModuleCrt, gowl_module_crt,
                     GOWL, MODULE_CRT, GowlModule)

/**
 * GowlCrtState:
 *
 * One output's tube.  Per-monitor because the sheet, the capture and the
 * glow all are; the clock is too, so two screens do not have to hum in
 * step.
 */
typedef struct {
	GowlMonitor   *monitor;      /* unowned */
	GowlFxSheet   *sheet;
	GowlFxTexture  frame;
	GowlFxTexture  glow;
	GowlFxCrtClock clock;
	gint64         last_us;
	gboolean       warned;

	/* What is on the sheet now, and what it was drawn from.  A picture
	 * of a screen that has not changed is still the right picture. */
	gboolean        drawn;
	GowlFxCrtParams drew_with;
	gint            drew_glow_scale;
	gint            drew_glow_passes;
} GowlCrtState;

struct _GowlModuleCrt {
	GowlModule       parent_instance;
	GowlFxGl        *gl;
	gboolean         gl_tried;
	GList           *states;      /* GowlCrtState* */
	gboolean         capturing;

	/* What the config said last time, so a redraw does not re-read
	 * twenty keys and so a change can be noticed at all. */
	GowlFxCrtParams  params;
	gint             glow_scale;
	gint             glow_passes;
};

static void crt_effect_init(GowlSceneEffectInterface *iface);
static void crt_shutdown_init(GowlShutdownHandlerInterface *iface);

G_DEFINE_TYPE_WITH_CODE(GowlModuleCrt, gowl_module_crt,
	GOWL_TYPE_MODULE,
	G_IMPLEMENT_INTERFACE(GOWL_TYPE_SCENE_EFFECT, crt_effect_init)
	G_IMPLEMENT_INTERFACE(GOWL_TYPE_SHUTDOWN_HANDLER, crt_shutdown_init))

/* ── Plumbing ────────────────────────────────────────────────────── */

/*
 * No weak reference to the compositor, unlike its neighbours.
 *
 * The magnifier keeps one because a scroll or a key arrives without a
 * compositor and it has to find one; every hook this module implements
 * is handed the compositor it needs.  A reference kept only so that
 * teardown can drop it is one more thing to get wrong.
 */
static void
crt_ensure_gl(GowlModuleCrt *mod, GowlCompositor *self)
{
	if (mod->gl != NULL || mod->gl_tried)
		return;
	if (self == NULL || self->renderer == NULL)
		return;

	mod->gl_tried = TRUE;
	mod->gl = gowl_fx_gl_new(self->renderer);
}

static gboolean
crt_enabled(GowlModuleCrt *mod, GowlCompositor *self)
{
	return self != NULL
	       && self->config != NULL
	       && !self->locked
	       && gowl_config_get_crt(self->config)
	       && mod->gl != NULL;
}

/* ── Settings ────────────────────────────────────────────────────── */

/*
 * Read the tube out of the config.
 *
 * Every tick, and compared with memcmp() rather than trusted: there is
 * no "config changed" signal a module can hang a shader rebuild on, and
 * twenty getters is cheaper than being wrong about whether somebody has
 * just moved `crt-curvature'.  The only thing the comparison is used
 * for is the glow, which has to be rebuilt when its blur changes.
 */
static void
crt_read_params(GowlModuleCrt *mod, GowlCompositor *self)
{
	GowlConfig *config = self->config;

	gowl_fx_crt_params_init(&mod->params);
	mod->params.curvature   = (gfloat)gowl_config_get_crt_curvature(config);
	mod->params.curvature_y = (gfloat)gowl_config_get_crt_curvature_y(config);
	mod->params.lines       = (gfloat)gowl_config_get_crt_lines(config);
	mod->params.scanline    = (gfloat)gowl_config_get_crt_scanline(config);
	mod->params.beam        = (gfloat)gowl_config_get_crt_beam(config);
	mod->params.beam_bloom  = (gfloat)gowl_config_get_crt_beam_bloom(config);
	mod->params.mask        = (gfloat)gowl_config_get_crt_mask(config);
	mod->params.mask_kind   = gowl_config_get_crt_mask_kind(config);
	mod->params.mask_size   = (gfloat)gowl_config_get_crt_mask_size(config);
	mod->params.bloom       = (gfloat)gowl_config_get_crt_bloom(config);
	mod->params.bloom_cut   = (gfloat)gowl_config_get_crt_bloom_cut(config);
	mod->params.vignette    = (gfloat)gowl_config_get_crt_vignette(config);
	mod->params.corner      = (gfloat)gowl_config_get_crt_corner(config);
	mod->params.convergence = (gfloat)gowl_config_get_crt_convergence(config);
	mod->params.hum         = (gfloat)gowl_config_get_crt_hum(config);
	mod->params.gamma       = (gfloat)gowl_config_get_crt_gamma(config);
	mod->params.brightness  = (gfloat)gowl_config_get_crt_brightness(config);

	mod->glow_scale  = gowl_config_get_crt_glow_scale(config);
	mod->glow_passes = gowl_config_get_crt_glow_passes(config);
}

/* ── State ───────────────────────────────────────────────────────── */

static GowlCrtState *
crt_state_for(GowlModuleCrt *mod, GowlMonitor *m, gboolean create)
{
	GList        *l;
	GowlCrtState *state;

	for (l = mod->states; l != NULL; l = l->next) {
		state = l->data;
		if (state->monitor == m)
			return state;
	}
	if (!create || m == NULL)
		return NULL;

	state = g_new0(GowlCrtState, 1);
	state->monitor = m;
	mod->states = g_list_prepend(mod->states, state);
	return state;
}

static void
crt_state_free(GowlModuleCrt *mod, GowlCrtState *state)
{
	if (state == NULL)
		return;
	g_clear_pointer(&state->sheet, gowl_fx_sheet_free);
	gowl_fx_texture_drop(mod->gl, &state->frame);
	gowl_fx_texture_drop(mod->gl, &state->glow);
	g_free(state);
}

static void
crt_state_end(GowlModuleCrt *mod, GowlCrtState *state)
{
	mod->states = g_list_remove(mod->states, state);
	crt_state_free(mod, state);
}

static void
crt_end_all(GowlModuleCrt *mod)
{
	while (mod->states != NULL)
		crt_state_end(mod, mod->states->data);
}

/* ── Frame ───────────────────────────────────────────────────────── */

static gboolean
crt_draw(GowlModuleCrt *mod, GowlCompositor *self, GowlCrtState *state)
{
	struct wlr_buffer   *buffer;
	GowlFxPass          *pass;
	const GowlFxTexture *glow = NULL;
	gboolean             ok;

	/*
	 * The capture.  This module's own sheet is a filter sheet, so
	 * gowl_fx_capture() has already taken it out of the scene for us --
	 * see GOWL_FX_SHEET_FILTER.  The `capturing' flag is the other half
	 * of the same guard: the capture renders the scene, which can arrive
	 * back here as a frame.
	 */
	mod->capturing = TRUE;
	ok = gowl_fx_capture(mod->gl, self, state->monitor, &state->frame, 1);
	mod->capturing = FALSE;
	if (!ok)
		return FALSE;

	/* Resampling through the curve lands between pixels everywhere but
	 * the middle, so the capture is read smoothly rather than nearest. */
	gowl_fx_texture_set_filter(mod->gl, &state->frame, TRUE);

	/*
	 * Halation, from a wide cheap blur of the same picture.  Only built
	 * when it is going to be used: it is another output-sized texture,
	 * which on a 4K screen is thirty-odd megabytes for a glow somebody
	 * may have turned off.
	 */
	if (mod->params.bloom > 0.0f) {
		if (gowl_fx_texture_blur(mod->gl, &state->glow, &state->frame,
		                         mod->glow_scale, mod->glow_passes))
			glow = &state->glow;
	} else if (state->glow.tex != 0) {
		gowl_fx_texture_drop(mod->gl, &state->glow);
	}

	buffer = gowl_fx_sheet_acquire(state->sheet);
	if (buffer == NULL)
		return FALSE;

	pass = gowl_fx_pass_begin(mod->gl, buffer);
	if (pass == NULL) {
		wlr_buffer_unlock(buffer);
		return FALSE;
	}

	ok = gowl_fx_pass_crt(pass, &state->frame, glow, &mod->params,
	                      &state->clock);
	gowl_fx_pass_end(pass);

	if (!ok) {
		/* The shader would not build.  Say so once and put the tube
		 * away rather than present an empty sheet over the desktop. */
		if (!state->warned) {
			g_warning("crt: no CRT shader on this renderer; the screen "
			          "stays flat");
			state->warned = TRUE;
		}
		wlr_buffer_unlock(buffer);
		return FALSE;
	}

	gowl_fx_sheet_present(state->sheet, buffer);
	wlr_buffer_unlock(buffer);
	return TRUE;
}

static gboolean
crt_frame(GowlSceneEffect *effect, GowlCompositor *self, GowlMonitor *m,
          gint64 now)
{
	GowlModuleCrt *mod = GOWL_MODULE_CRT(effect);
	GowlCrtState  *state;
	gdouble        dt;

	if (mod->capturing || m == NULL)
		return FALSE;

	crt_ensure_gl(mod, self);
	if (!crt_enabled(mod, self)) {
		/* Off, or locked: put the sheet and both textures away, which
		 * costs the session exactly nothing until it is switched on
		 * again. */
		state = crt_state_for(mod, m, FALSE);
		if (state != NULL) {
			crt_state_end(mod, state);
			return TRUE;   /* one more frame, to show the bare desktop */
		}
		return FALSE;
	}

	crt_read_params(mod, self);
	state = crt_state_for(mod, m, TRUE);

	/*
	 * NOTHING HAS MOVED, SO DO NOT TAKE ANOTHER PHOTOGRAPH.
	 *
	 * The capture forces whole damage on the output so that the frame
	 * after it draws everything, which means an effect that asks for
	 * another frame every time is asking FOREVER -- two full scene
	 * renders, a wide blur and a shader pass at the refresh rate, to
	 * show a picture identical to the one already on screen.  On a
	 * laptop that is the battery, and on any machine it is a whole
	 * core of a GPU spent on nothing.
	 *
	 * So the tube redraws when the scene under it has something new to
	 * show, or when its own settings have moved, and otherwise leaves
	 * the sheet exactly as it is and lets the output go back to sleep.
	 * Returning FALSE here is what allows that: it is the only thing
	 * that stops gowl scheduling the next frame.
	 *
	 * The cost is that the hum bar only drifts while something else on
	 * the screen is happening.  On a still desktop it holds still, which
	 * nobody can see and nobody would trade a core for.
	 */
	if (state->drawn && state->sheet != NULL && m->scene_output != NULL
	    && state->drew_glow_scale == mod->glow_scale
	    && state->drew_glow_passes == mod->glow_passes
	    && memcmp(&state->drew_with, &mod->params,
	              sizeof(state->drew_with)) == 0
	    && !wlr_scene_output_needs_frame(m->scene_output))
		return FALSE;

	/* Since the last DRAW, not since the last frame: the clock must not
	 * be handed the time the tube spent asleep. */
	dt = state->last_us > 0
		? (gdouble)(now - state->last_us) / 1000000.0 : 0.016;
	state->last_us = now;
	gowl_fx_crt_advance(&state->clock, CLAMP(dt, 0.0, 0.25));

	if (state->sheet == NULL) {
		/*
		 * Above every layer a client can be in, and marked as a filter
		 * over the finished screen.  See the head of this file for why
		 * both of those are load bearing rather than tidy.
		 */
		state->sheet = gowl_fx_sheet_new(self, m,
			GOWL_FX_SHEET_ABOVE_OVERLAY | GOWL_FX_SHEET_FILTER);
		if (state->sheet == NULL) {
			crt_state_end(mod, state);
			return FALSE;
		}
	}

	if (!crt_draw(mod, self, state)) {
		crt_state_end(mod, state);
		return TRUE;
	}

	state->drawn            = TRUE;
	state->drew_with        = mod->params;
	state->drew_glow_scale  = mod->glow_scale;
	state->drew_glow_passes = mod->glow_passes;

	/*
	 * One more frame, so the sheet this drew actually reaches the
	 * screen; the check at the top of this function is what stops the
	 * one after that.
	 */
	return TRUE;
}

static void
crt_monitor_removed(GowlSceneEffect *effect, GowlCompositor *self,
                    GowlMonitor *m)
{
	GowlModuleCrt *mod = GOWL_MODULE_CRT(effect);
	GowlCrtState  *state = crt_state_for(mod, m, FALSE);

	if (state != NULL)
		crt_state_end(mod, state);
}

static void
crt_finish(GowlSceneEffect *effect, GowlCompositor *self)
{
	GowlModuleCrt *mod = GOWL_MODULE_CRT(effect);

	crt_end_all(mod);
	g_clear_pointer(&mod->gl, gowl_fx_gl_free);
	mod->gl_tried = FALSE;
}

static void
crt_effect_init(GowlSceneEffectInterface *iface)
{
	iface->frame           = crt_frame;
	iface->monitor_removed = crt_monitor_removed;
	iface->finish          = crt_finish;
}

static void
crt_shutdown(GowlShutdownHandler *handler, gpointer compositor)
{
	crt_finish(GOWL_SCENE_EFFECT(handler), compositor);
}

static void
crt_shutdown_init(GowlShutdownHandlerInterface *iface)
{
	iface->on_shutdown = crt_shutdown;
}

/* ── Module ──────────────────────────────────────────────────────── */

/*
 * Last of all, after the magnifier.
 *
 * gowl-effects.c ticks providers in priority order, and the tube is the
 * physical glass in front of the screen: whatever the cube, the expo,
 * the switcher or the magnifier put up, the tube shows THAT curved.  So
 * it has to draw after every one of them, which a higher number gets.
 */
#define GOWL_CRT_PRIORITY (110)

static gboolean
crt_activate(GowlModule *base)
{
	gowl_module_set_priority(base, GOWL_CRT_PRIORITY);
	return TRUE;
}

static void
crt_deactivate(GowlModule *base)
{
	GowlModuleCrt *mod = GOWL_MODULE_CRT(base);

	crt_end_all(mod);
	g_clear_pointer(&mod->gl, gowl_fx_gl_free);
	mod->gl_tried = FALSE;
}

static const gchar *crt_name(GowlModule *m)    { return "crt"; }
static const gchar *crt_version(GowlModule *m) { return "0.1.0"; }
static const gchar *crt_description(GowlModule *m)
{
	return "Puts the whole output through a cathode ray tube";
}

static void
crt_finalize(GObject *object)
{
	crt_deactivate(GOWL_MODULE(object));
	G_OBJECT_CLASS(gowl_module_crt_parent_class)->finalize(object);
}

static void
gowl_module_crt_class_init(GowlModuleCrtClass *klass)
{
	GowlModuleClass *mod = GOWL_MODULE_CLASS(klass);

	mod->activate        = crt_activate;
	mod->deactivate      = crt_deactivate;
	mod->get_name        = crt_name;
	mod->get_description = crt_description;
	mod->get_version     = crt_version;
	G_OBJECT_CLASS(klass)->finalize = crt_finalize;
}

static void
gowl_module_crt_init(GowlModuleCrt *mod)
{
	gowl_fx_crt_params_init(&mod->params);
	mod->glow_scale  = 6;
	mod->glow_passes = 3;
	gowl_module_set_priority(GOWL_MODULE(mod), GOWL_CRT_PRIORITY);
}

G_MODULE_EXPORT GType
gowl_module_register(void)
{
	return GOWL_TYPE_MODULE_CRT;
}
