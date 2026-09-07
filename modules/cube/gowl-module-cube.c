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
 * The desktop cube: a tag switch turns the desktop instead of cutting to
 * it, one face-step per tag crossed, so going from tag 1 to tag 4 turns
 * three times and shows tags 2 and 3 on the way past.
 *
 * How the pieces divide up:
 *
 *   gowl-cube-plan.c   decides WHAT turns and for how long.  No wlroots.
 *   gowl-cube-gl.c     the shape of the solid and where the camera
 *                      stands.  No compositor.
 *   src/fx/ (core)     every bit of GL, capture and scene plumbing,
 *                      shared with the other effect modules.
 *   this file          connects them to gowl.
 *
 * IT COEXISTS WITH THE OTHER EFFECT MODULES.  gowl-effects.c dispatches
 * per event rather than handing the whole interface to one module: the
 * cube claims a visibility reveal on a tag it is about to animate and
 * implements the frame and teardown hooks, and everything else --- window
 * geometry, opacity, hit-testing --- reaches the animation module
 * untouched.  So the cube implements exactly what it needs and returns
 * FALSE for the rest; there is nothing to forward.
 */

#undef G_LOG_DOMAIN
#define G_LOG_DOMAIN "gowl-cube"

#include "gowl-cube-plan.h"
#include "gowl-cube-gl.h"

#include "core/gowl-core-private.h"
#include "core/gowl-compositor.h"
#include "core/gowl-monitor.h"
#include "core/gowl-effects.h"
#include "config/gowl-config.h"
#include "boxed/gowl-color.h"
#include "fx/gowl-fx.h"
#include "util/gowl-easing.h"
#include "interfaces/gowl-scene-effect.h"
#include "interfaces/gowl-gesture-handler.h"
#include "interfaces/gowl-shutdown-handler.h"

#include <math.h>

#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_output.h>
#include <wlr/util/box.h>

#include <gmodule.h>

/* Intermediate desktops are only ever seen mid-spin, at an angle, under
 * motion blur.  Storing them at half size is invisible and is what keeps
 * an eight-step rotation on a 4K screen from costing a third of a
 * gigabyte of texture memory. */
#define GOWL_CUBE_INTERMEDIATE_DIVISOR 2

/* Angular speed, in radians per second, that counts as "full blur". */
#define GOWL_CUBE_BLUR_REFERENCE 6.0

#define GOWL_TYPE_MODULE_CUBE (gowl_module_cube_get_type())
G_DECLARE_FINAL_TYPE(GowlModuleCube, gowl_module_cube, GOWL, MODULE_CUBE, GowlModule)

/**
 * GowlCubeRun:
 *
 * One rotation in flight, on one output.  Multi-monitor setups can have
 * several: tags are per-monitor in gowl, so two screens can be turning
 * different journeys at once and neither may touch the other's scene.
 */
typedef struct {
	GowlMonitor  *monitor;      /* unowned; runs end with the output */
	GowlCubePlan  plan;

	/* Stored desktops, indexed by slot.  Slots outside 0..steps have no
	 * desktop and draw as blank sides. */
	GowlFxTexture face[GOWL_CUBE_MAX_STEPS + 1];

	GowlFxSheet  *sheet;

	/*
	 * A rotation the user is dragging rather than one that is playing.
	 * The plan's clock is ignored while this is set; `scrub_progress' is
	 * where the fingers have pushed it to, and letting go either finishes
	 * the journey or rewinds it, whichever is nearer.
	 */
	gboolean      scrubbing;
	gdouble       scrub_progress;
	gdouble       scrub_accum;   /* pixels of travel since the gesture began */
} GowlCubeRun;

struct _GowlModuleCube {
	GowlModule  parent_instance;
	GWeakRef    compositor;
	GowlFxGl   *gl;
	gboolean    gl_tried;
	GList      *runs;        /* GowlCubeRun* */

	/* Set while a capture is rendering the scene.  Every hook checks it:
	 * a capture toggles scene visibility, and a reveal or a frame that
	 * ran in the middle of that would see a scene that is deliberately
	 * lying about which tag is on screen. */
	gboolean    capturing;

	/* A three-finger swipe in progress, and which output it started on. */
	gboolean      gesture_active;
	GowlMonitor  *gesture_monitor;
	gdouble       gesture_dx;
} ;

static void cube_effect_init(GowlSceneEffectInterface *iface);
static void cube_shutdown_init(GowlShutdownHandlerInterface *iface);
static void cube_gesture_init(GowlGestureHandlerInterface *iface);
G_DEFINE_TYPE_WITH_CODE(GowlModuleCube, gowl_module_cube, GOWL_TYPE_MODULE,
	G_IMPLEMENT_INTERFACE(GOWL_TYPE_SCENE_EFFECT, cube_effect_init)
	G_IMPLEMENT_INTERFACE(GOWL_TYPE_GESTURE_HANDLER, cube_gesture_init)
	G_IMPLEMENT_INTERFACE(GOWL_TYPE_SHUTDOWN_HANDLER, cube_shutdown_init))

static void
cube_bind(GowlModuleCube *mod, GowlCompositor *self)
{
	GowlCompositor *held = g_weak_ref_get(&mod->compositor);

	if (held == self) {
		g_clear_object(&held);
		return;
	}
	g_clear_object(&held);
	g_weak_ref_set(&mod->compositor, self);
}

/*
 * Build the GL side on first use, once.
 *
 * Not at activate(): a module can be loaded before the compositor has a
 * renderer, and the answer to "is this renderer one whose context we can
 * borrow" is only available afterwards.  @gl_tried keeps a `no' from
 * being re-asked sixty times a second for the rest of the session.
 */
static void
cube_ensure_gl(GowlModuleCube *mod, GowlCompositor *self)
{
	if (mod->gl != NULL || mod->gl_tried)
		return;
	if (self == NULL || self->renderer == NULL)
		return;

	mod->gl_tried = TRUE;
	mod->gl = gowl_fx_gl_new(self->renderer);
}

/* ── Configuration ───────────────────────────────────────────────── */

static gboolean
cube_enabled(GowlModuleCube *mod, GowlCompositor *self)
{
	return self != NULL
	       && self->config != NULL
	       && !self->locked
	       && gowl_config_get_cube(self->config)
	       && gowl_config_get_cube_duration(self->config) > 0
	       && mod->gl != NULL;
}

static void
cube_backdrop_rgb(GowlCompositor *self, gfloat *out)
{
	const gchar *spec = self->config != NULL
		? gowl_config_get_cube_backdrop_color(self->config) : NULL;
	GowlColor *color = spec != NULL ? gowl_color_new_from_hex(spec) : NULL;

	/* A colour that does not parse is a typo in a config file, not a
	 * reason to abandon the rotation. */
	if (color == NULL) {
		out[0] = 0.071f; out[1] = 0.078f; out[2] = 0.122f;
		return;
	}
	out[0] = (gfloat)color->r;
	out[1] = (gfloat)color->g;
	out[2] = (gfloat)color->b;
	gowl_color_free(color);
}

/* ── Per-monitor tag memory ──────────────────────────────────────── */

/*
 * The last tag set the cube knows about, kept on the monitor itself so it
 * lives and dies with the output and needs no bookkeeping when one is
 * unplugged.  Tag changes are noticed by comparing against this on each
 * frame rather than by listening for a signal: by the time a signal would
 * arrive the scene has already been rearranged, and the cube has to
 * reconstruct every tag's desktop from scratch anyway.
 */
#define GOWL_CUBE_TAG_KEY "gowl-cube-last-tags"

static guint32
cube_remembered_tags(GowlMonitor *m)
{
	return (guint32)GPOINTER_TO_UINT(
		g_object_get_data(G_OBJECT(m), GOWL_CUBE_TAG_KEY));
}

static void
cube_remember_tags(GowlMonitor *m, guint32 tags)
{
	/* Zero means "nothing recorded", so a monitor that genuinely shows no
	 * tags simply does not arm the cube --- which is right: there is no
	 * desktop to turn away from. */
	g_object_set_data(G_OBJECT(m), GOWL_CUBE_TAG_KEY,
	                  GUINT_TO_POINTER(tags));
}

static GowlCubeRun *
cube_run_for(GowlModuleCube *mod, GowlMonitor *m)
{
	GList *l;

	for (l = mod->runs; l != NULL; l = l->next) {
		GowlCubeRun *run = l->data;

		if (run->monitor == m)
			return run;
	}
	return NULL;
}

/* ── Capture ─────────────────────────────────────────────────────── */

/*
 * Render the scene as it would look on @tags into a texture the cube owns.
 *
 * The layers above the sheet are switched off for the duration: the bar
 * and any notifications stay live ON TOP of the rotation, so baking them
 * into every face as well would show four bars turning past.  The
 * recording indicator is switched off for the same reason and put back
 * immediately --- it is a safety light and must never be off for a frame
 * that is actually presented, which this one is not.
 */
static gboolean
cube_capture_face(GowlModuleCube *mod, GowlCompositor *self, GowlMonitor *m,
                   guint32 tags, GowlFxTexture *face, gint divisor)
{
	GowlFxVis *vis;
	gboolean   ok;
	gint       i;

	if (m->scene_output == NULL || m->wlr_output == NULL)
		return FALSE;

	mod->capturing = TRUE;
	vis = gowl_fx_vis_begin();

	gowl_fx_vis_hide_layer(vis, self, GOWL_SCENE_LAYER_TOP, FALSE);
	gowl_fx_vis_hide_layer(vis, self, GOWL_SCENE_LAYER_BLOCK, FALSE);
	/* The overlay layer keeps its embedder-pinned clients: they are
	 * placed over the embedder's own surface rather than by tag, so they
	 * belong on every side rather than on none. */
	gowl_fx_vis_hide_layer(vis, self, GOWL_SCENE_LAYER_OVERLAY, TRUE);
	for (i = 0; i < 4; i++) {
		if (self->rec_indicator[i] != NULL)
			gowl_fx_vis_set(vis, &self->rec_indicator[i]->node, FALSE);
	}
	gowl_fx_vis_show_tags(vis, self, m, tags);

	ok = gowl_fx_capture(mod->gl, self, m, face, divisor);

	gowl_fx_vis_restore(vis);
	mod->capturing = FALSE;
	return ok;
}

/* ── Run lifecycle ───────────────────────────────────────────────── */

static void
cube_run_free(GowlModuleCube *mod, GowlCubeRun *run)
{
	gint i;

	if (run == NULL)
		return;

	g_clear_pointer(&run->sheet, gowl_fx_sheet_free);
	for (i = 0; i <= GOWL_CUBE_MAX_STEPS; i++)
		gowl_fx_texture_drop(mod->gl, &run->face[i]);
	g_free(run);
}

static void
cube_run_end(GowlModuleCube *mod, GowlCubeRun *run)
{
	mod->runs = g_list_remove(mod->runs, run);
	cube_run_free(mod, run);
}

static void
cube_end_all(GowlModuleCube *mod)
{
	while (mod->runs != NULL)
		cube_run_end(mod, mod->runs->data);
}

/*
 * Begin a rotation on @m from @from_tags to @to_tags, or decline.
 *
 * Everything that can refuse does so before anything is touched, so a
 * declined rotation costs the user nothing but an instant tag switch,
 * which is what they had before this module existed.
 */
static GowlCubeRun *
cube_run_start(GowlModuleCube *mod, GowlCompositor *self, GowlMonitor *m,
                guint32 from_tags, guint32 to_tags, gint64 now_us)
{
	GowlCubeRun *run;
	gint         j;

	if (m->wlr_output == NULL || m->scene_output == NULL
	    || m->m.width <= 0 || m->m.height <= 0)
		return NULL;

	/*
	 * A rotated output renders into a buffer whose axes are not the
	 * screen's, and the cube's projection assumes they are.  Rather than
	 * present a sideways desktop, sit the rotation out.
	 */
	if (m->wlr_output->transform != WL_OUTPUT_TRANSFORM_NORMAL)
		return NULL;

	run = g_new0(GowlCubeRun, 1);
	run->monitor = m;

	if (!gowl_cube_plan_init(&run->plan,
	                         gowl_cube_tag_index(from_tags),
	                         gowl_cube_tag_index(to_tags),
	                         gowl_config_get_cube_faces(self->config),
	                         gowl_config_get_cube_duration(self->config),
	                         gowl_config_get_cube_step_duration(self->config),
	                         now_us)) {
		g_free(run);
		return NULL;
	}

	/*
	 * Capture every desktop in the journey up front.  It is the whole
	 * cost of the effect and it lands in one frame at the start, which is
	 * the right place for it: a capture taken later, while the solid is
	 * already turning, would have to rearrange the scene mid-rotation.
	 */
	for (j = 0; j <= run->plan.steps; j++) {
		gboolean endpoint = (j == 0 || j == run->plan.steps);

		if (!cube_capture_face(mod, self, m,
		                       1u << run->plan.tag[j], &run->face[j],
		                       endpoint ? 1 : GOWL_CUBE_INTERMEDIATE_DIVISOR)) {
			/* The two ends are what make the effect seamless; without
			 * one of them the rotation would visibly cut. */
			if (endpoint) {
				cube_run_free(mod, run);
				return NULL;
			}
		}
	}

	run->sheet = gowl_fx_sheet_new(self, m, GOWL_FX_SHEET_NONE);
	if (run->sheet == NULL) {
		cube_run_free(mod, run);
		return NULL;
	}

	mod->runs = g_list_prepend(mod->runs, run);
	return run;
}

/* ── Frame ───────────────────────────────────────────────────────── */

static gdouble
cube_eased(GowlCompositor *self, const GowlCubePlan *plan, gint64 at_us)
{
	const gchar *curve = self->config != NULL
		? gowl_config_get_cube_curve(self->config) : NULL;

	return gowl_easing_eval(curve, gowl_cube_plan_progress(plan, at_us));
}

/* Linear progress, whether the clock or the user's fingers are driving. */
static gdouble
cube_linear(GowlCubeRun *run, gint64 now_us)
{
	return run->scrubbing
	           ? CLAMP(run->scrub_progress, 0.0, 1.0)
	           : gowl_cube_plan_progress(&run->plan, now_us);
}

static gboolean
cube_draw(GowlModuleCube *mod, GowlCompositor *self, GowlCubeRun *run,
           gint64 now_us)
{
	GowlCubeFrame      frame;
	GowlFxTexture      window[4];
	struct wlr_buffer *buffer;
	gdouble            linear, eased, eased_prev, omega;
	gint               first, last, j;

	memset(&frame, 0, sizeof(frame));
	memset(window, 0, sizeof(window));

	linear = cube_linear(run, now_us);
	if (run->scrubbing) {
		/*
		 * A dragged rotation follows the fingers exactly.  Easing a
		 * direct manipulation is the classic mistake: the desktop would
		 * lag behind the touchpad and then catch up, which reads as the
		 * machine being slow rather than as a curve.
		 */
		eased      = linear;
		eased_prev = linear;
	} else {
		eased      = cube_eased(self, &run->plan, now_us);
		eased_prev = cube_eased(self, &run->plan, now_us - 8000);
	}

	/* Angular speed by difference rather than by differentiating the
	 * curve: the curves are Béziers solved numerically, so a difference
	 * over a fixed short interval is both simpler and exactly as good for
	 * something that only scales a blur. */
	omega = fabs(eased - eased_prev) * (gdouble)run->plan.steps
	        * gowl_cube_plan_face_angle(&run->plan) / 0.008;

	gowl_cube_plan_slot_window(&run->plan, eased, &first, &last);
	/* The planner already caps the span at four and at the face count;
	 * this is the belt to that braces, because overrunning `window'
	 * would read past the end of a stack array. */
	if (last - first >= (gint)G_N_ELEMENTS(window))
		last = first + (gint)G_N_ELEMENTS(window) - 1;

	for (j = first; j <= last; j++) {
		if (j >= 0 && j <= run->plan.steps)
			window[j - first] = run->face[j];
	}

	frame.faces       = run->plan.faces;
	frame.dir         = run->plan.dir;
	frame.rotation    = gowl_cube_plan_rotation(&run->plan, eased);
	frame.face_angle  = gowl_cube_plan_face_angle(&run->plan);
	frame.bump        = gowl_cube_plan_bump(linear);
	frame.speed       = CLAMP(omega / GOWL_CUBE_BLUR_REFERENCE, 0.0, 1.0);
	frame.zoom        = gowl_config_get_cube_zoom(self->config);
	frame.pitch_deg   = gowl_config_get_cube_pitch(self->config);
	frame.shading     = gowl_config_get_cube_shading(self->config);
	frame.reflection  = gowl_config_get_cube_reflection(self->config);
	frame.motion_blur = gowl_config_get_cube_motion_blur(self->config);
	frame.caps        = gowl_config_get_cube_caps(self->config);
	frame.first_slot  = first;
	frame.last_slot   = last;
	frame.slot        = window;
	cube_backdrop_rgb(self, frame.backdrop);

	buffer = gowl_fx_sheet_acquire(run->sheet);
	if (buffer == NULL)
		return FALSE;

	if (!gowl_cube_draw(mod->gl, buffer, &frame)) {
		wlr_buffer_unlock(buffer);
		return FALSE;
	}

	gowl_fx_sheet_present(run->sheet, buffer);
	wlr_buffer_unlock(buffer);
	return TRUE;
}

/*
 * Notice a tag change and, if it is one worth turning for, start.
 *
 * Called from the frame hook rather than from the monitor's tag-changed
 * signal.  The signal fires before the scene has been rearranged, but the
 * cube needs every tag's desktop, not just the old one, and builds them
 * all by hand anyway --- so there is nothing the earlier callback would
 * buy, and polling here also catches outputs that appeared after the
 * module loaded, which a per-monitor signal connection would miss.
 */
static void
cube_poll_tags(GowlModuleCube *mod, GowlCompositor *self, GowlMonitor *m,
                gint64 now_us)
{
	guint32 previous = cube_remembered_tags(m);
	guint32 current  = m->tagset[m->seltags];

	if (current == previous)
		return;

	cube_remember_tags(m, current);

	if (previous == 0 || !cube_enabled(mod, self))
		return;
	if (!gowl_config_get_cube_all_monitors(self->config) && self->selmon != m)
		return;

	/*
	 * A tag change arriving mid-rotation ends the one in flight and
	 * starts a fresh one from where the old journey was headed.  Bending
	 * the path instead would mean re-capturing every desktop on the new
	 * route while the old textures are still on screen; a switch inside
	 * half a second is rare enough that the honest cut is the better
	 * trade.
	 */
	if (cube_run_for(mod, m) != NULL)
		cube_run_end(mod, cube_run_for(mod, m));

	cube_run_start(mod, self, m, previous, current, now_us);
}

static gboolean
cube_frame(GowlSceneEffect *effect, GowlCompositor *self, GowlMonitor *m,
            gint64 now)
{
	GowlModuleCube *mod = GOWL_MODULE_CUBE(effect);
	GowlCubeRun    *run;

	cube_bind(mod, self);

	if (mod->capturing || m == NULL)
		return FALSE;

	cube_ensure_gl(mod, self);
	cube_poll_tags(mod, self, m, now);

	run = cube_run_for(mod, m);
	if (run == NULL)
		return FALSE;

	if (!run->scrubbing && gowl_cube_plan_finished(&run->plan, now)) {
		cube_run_end(mod, run);
		return TRUE;
	}

	if (!cube_draw(mod, self, run, now)) {
		/* A frame that cannot be drawn is the end of the rotation, not a
		 * reason to leave a stale sheet over the user's desktop. */
		cube_run_end(mod, run);
		return TRUE;
	}
	return TRUE;
}

/* ── Scene-effect hooks ──────────────────────────────────────────── */

/*
 * TRUE when this monitor is one frame away from starting a rotation.
 *
 * gowl reveals newly visible windows with a fade, and that fade is
 * started by the same arrange() call that changed the tags --- before the
 * cube has seen anything.  Left alone it would run underneath the
 * rotation and finish long before it, so the windows the cube already
 * showed at full strength would be there; but a slower fade, or a shorter
 * rotation, would end with the desktop fading UP after it had landed.
 * Swallowing the reveal is both cheaper and unconditionally right: the
 * cube's first captured face already shows them.
 */
static gboolean
cube_pending(GowlModuleCube *mod, GowlCompositor *self, GowlMonitor *m)
{
	guint32 previous;

	if (m == NULL || !cube_enabled(mod, self) || mod->capturing)
		return FALSE;
	if (cube_run_for(mod, m) != NULL)
		return TRUE;

	previous = cube_remembered_tags(m);
	if (previous == 0 || previous == m->tagset[m->seltags])
		return FALSE;
	if (!gowl_config_get_cube_all_monitors(self->config) && self->selmon != m)
		return FALSE;

	return gowl_cube_tag_index(previous)
	       != gowl_cube_tag_index(m->tagset[m->seltags]);
}

static gboolean
cube_client_event(GowlSceneEffect *effect, GowlCompositor *self, GowlClient *c,
                   GowlSceneEffectEvent event, const struct wlr_box *previous,
                   gboolean interactive)
{
	GowlModuleCube *mod = GOWL_MODULE_CUBE(effect);

	cube_bind(mod, self);

	/* The ONLY event the cube claims.  Returning FALSE for everything
	 * else is what lets the animation module keep owning window geometry
	 * while the cube owns the output. */
	return event == GOWL_SCENE_EFFECT_REVEAL && c != NULL
	       && cube_pending(mod, self, c->mon);
}

static void
cube_monitor_removed(GowlSceneEffect *effect, GowlCompositor *self,
                      GowlMonitor *m)
{
	GowlModuleCube *mod = GOWL_MODULE_CUBE(effect);
	GowlCubeRun    *run = cube_run_for(mod, m);

	/* The run holds a scene tree parented under a scene that is about to
	 * lose this output, and textures it must free while the renderer is
	 * still alive. */
	if (run != NULL)
		cube_run_end(mod, run);
	if (mod->gesture_monitor == m) {
		mod->gesture_active = FALSE;
		mod->gesture_monitor = NULL;
	}
}

/*
 * Everything GL must go before the renderer does, and `finish' is the one
 * hook gowl guarantees runs while the scene and renderer are still there.
 */
static void
cube_finish(GowlSceneEffect *effect, GowlCompositor *self)
{
	GowlModuleCube *mod = GOWL_MODULE_CUBE(effect);

	cube_end_all(mod);
	g_clear_pointer(&mod->gl, gowl_fx_gl_free);
	mod->gl_tried = FALSE;
	mod->gesture_active = FALSE;
	mod->gesture_monitor = NULL;
	g_weak_ref_set(&mod->compositor, NULL);
}

static void
cube_effect_init(GowlSceneEffectInterface *iface)
{
	iface->client_event    = cube_client_event;
	iface->frame           = cube_frame;
	iface->monitor_removed = cube_monitor_removed;
	iface->finish          = cube_finish;
}

/* ── Touchpad scrub ──────────────────────────────────────────────── */

/*
 * Three fingers turn the cube directly.
 *
 * A transition you TRIGGER and a transition you DRAG are different
 * things.  The first is feedback -- it tells you something happened.  The
 * second is the object itself: the desktop is a solid, your fingers are
 * on it, and it is exactly as far round as you have pushed it.  That is
 * the whole reason this exists, and it is why the drag is not eased: the
 * picture has to be where the fingers are, not where a curve thinks they
 * should be.
 *
 * The rotation it drags is an ordinary run, so it shares the cube's
 * capture, sheet, camera and shading with the keyboard path.  Letting go
 * hands the run back to its clock, aimed at whichever end is nearer.
 */

/* Fingers.  Two is a scroll, four is usually the window manager's. */
#define GOWL_CUBE_SWIPE_FINGERS 3

/* How far the fingers travel for one whole face-step, in pixels of
 * touchpad motion.  Roughly a comfortable full swipe. */
#define GOWL_CUBE_SWIPE_TRAVEL 320.0

static gboolean
cube_gesture_begin(GowlGestureHandler *handler, gpointer compositor,
                    guint fingers)
{
	GowlModuleCube *mod = GOWL_MODULE_CUBE(handler);
	GowlCompositor *self = compositor;

	if (fingers != GOWL_CUBE_SWIPE_FINGERS)
		return FALSE;

	cube_bind(mod, self);
	cube_ensure_gl(mod, self);

	if (!cube_enabled(mod, self) || self->selmon == NULL
	    || !gowl_config_get_cube_gesture(self->config))
		return FALSE;

	/* A rotation already playing is left alone: interrupting it would
	 * mean re-capturing mid-flight, and the swipe is better spent on the
	 * next one. */
	if (cube_run_for(mod, self->selmon) != NULL)
		return FALSE;

	mod->gesture_active  = TRUE;
	mod->gesture_monitor = self->selmon;
	mod->gesture_dx      = 0.0;
	/* Claim the gesture so it is not also relayed to the focused client:
	 * a browser scrolling under a desktop switch is nobody's intent. */
	return TRUE;
}

/*
 * Start the run lazily, on the first update that has actually travelled.
 *
 * The capture costs a scene render per tag, so paying it on `begin' would
 * put a hitch at the start of every three-finger gesture including the
 * ones the user abandons.  Waiting for real movement also gives the
 * direction, which decides which tag the journey is towards.
 */
static GowlCubeRun *
cube_gesture_run(GowlModuleCube *mod, GowlCompositor *self, gdouble dx)
{
	GowlMonitor *m = mod->gesture_monitor;
	GowlCubeRun *run;
	guint32      current;
	gint         from, to;

	run = cube_run_for(mod, m);
	if (run != NULL)
		return run;

	if (fabs(dx) < 8.0)
		return NULL;

	current = m->tagset[m->seltags];
	from = gowl_cube_tag_index(current);
	if (from < 0)
		return NULL;

	/* Swiping left moves forward through the tags, the way a page does. */
	to = dx < 0.0 ? from + 1 : from - 1;
	if (to < 0 || to >= GOWL_CUBE_MAX_TAGS)
		return NULL;

	run = cube_run_start(mod, self, m, current, 1u << to,
	                     g_get_monotonic_time());
	if (run == NULL)
		return NULL;

	run->scrubbing = TRUE;
	run->scrub_progress = 0.0;
	return run;
}

static gboolean
cube_gesture_update(GowlGestureHandler *handler, gpointer compositor,
                     gdouble dx, gdouble dy)
{
	GowlModuleCube *mod = GOWL_MODULE_CUBE(handler);
	GowlCompositor *self = compositor;
	GowlCubeRun    *run;

	if (!mod->gesture_active || mod->gesture_monitor == NULL)
		return FALSE;

	mod->gesture_dx += dx;

	run = cube_gesture_run(mod, self, mod->gesture_dx);
	if (run == NULL)
		return TRUE;   /* still ours, just not moving anything yet */

	/*
	 * Travel is measured from where the run began, and the sign follows
	 * the journey's direction, so pushing further always advances and
	 * pulling back always rewinds --- including past zero, which simply
	 * stops at the tag the user started on.
	 */
	run->scrub_accum += dx * (run->plan.dir > 0 ? -1.0 : 1.0);
	run->scrub_progress = CLAMP(run->scrub_accum / GOWL_CUBE_SWIPE_TRAVEL,
	                            0.0, 1.0);

	if (mod->gesture_monitor->wlr_output != NULL)
		wlr_output_schedule_frame(mod->gesture_monitor->wlr_output);
	return TRUE;
}

static gboolean
cube_gesture_end(GowlGestureHandler *handler, gpointer compositor,
                  gboolean cancelled)
{
	GowlModuleCube *mod = GOWL_MODULE_CUBE(handler);
	GowlCompositor *self = compositor;
	GowlCubeRun    *run;
	gboolean        claimed = mod->gesture_active;
	gboolean        commit;

	if (!claimed)
		return FALSE;

	mod->gesture_active = FALSE;
	run = mod->gesture_monitor != NULL
		? cube_run_for(mod, mod->gesture_monitor) : NULL;
	mod->gesture_monitor = NULL;

	if (run == NULL || !run->scrubbing)
		return claimed;

	/*
	 * Let go and it goes to the nearer end, on the clock, from where it
	 * actually is.  Rewinding is not a special case: the plan is simply
	 * re-aimed at slot 0, so the same curve, capture and camera carry it
	 * back.
	 */
	commit = !cancelled && run->scrub_progress >= 0.5;
	run->scrubbing = FALSE;

	if (commit) {
		/* Re-time the run so it resumes from where the fingers left it
		 * rather than snapping back to the beginning of the curve. */
		run->plan.start_us = g_get_monotonic_time()
			- (gint64)(run->scrub_progress * (gdouble)run->plan.dur_us);
		/* And actually change tag, which is what the whole gesture was
		 * for.  The tag memory is updated first so the frame hook does
		 * not see this as a NEW change and start a second rotation. */
		cube_remember_tags(run->monitor, 1u << run->plan.tag[run->plan.steps]);
		gowl_compositor_view_tags(self, run->monitor,
		                          1u << run->plan.tag[run->plan.steps]);
	} else {
		gowl_cube_plan_reverse(&run->plan, run->scrub_progress,
		                       g_get_monotonic_time());
	}

	if (run->monitor->wlr_output != NULL)
		wlr_output_schedule_frame(run->monitor->wlr_output);
	return claimed;
}

static void
cube_gesture_init(GowlGestureHandlerInterface *iface)
{
	iface->swipe_begin  = cube_gesture_begin;
	iface->swipe_update = cube_gesture_update;
	iface->swipe_end    = cube_gesture_end;
}

static void
cube_shutdown(GowlShutdownHandler *handler, gpointer compositor)
{
	cube_finish(GOWL_SCENE_EFFECT(handler), compositor);
}

static void
cube_shutdown_init(GowlShutdownHandlerInterface *iface)
{
	iface->on_shutdown = cube_shutdown;
}

/* ── Module ──────────────────────────────────────────────────────── */

/*
 * Ahead of the animation module, which sits at the default zero.
 *
 * Priority decides who is ASKED first, and the only consumable event the
 * cube answers is the reveal it needs to suppress --- so being ahead is
 * what lets it swallow that one before the animation module starts a fade
 * nobody will see.
 */
#define GOWL_CUBE_PRIORITY (-10)

static gboolean
cube_activate(GowlModule *base)
{
	gowl_module_set_priority(base, GOWL_CUBE_PRIORITY);
	return TRUE;
}

static void
cube_deactivate(GowlModule *base)
{
	GowlModuleCube *mod = GOWL_MODULE_CUBE(base);

	cube_end_all(mod);
	g_clear_pointer(&mod->gl, gowl_fx_gl_free);
	mod->gl_tried = FALSE;
	mod->gesture_active = FALSE;
	mod->gesture_monitor = NULL;
}

static const gchar *cube_name(GowlModule *m)        { return "cube"; }
static const gchar *cube_version(GowlModule *m)     { return "0.2.0"; }
static const gchar *cube_description(GowlModule *m)
{
	return "Rotates the desktop as a solid when the tag changes";
}

static void
cube_finalize(GObject *object)
{
	GowlModuleCube *mod = GOWL_MODULE_CUBE(object);

	cube_deactivate(GOWL_MODULE(object));
	g_weak_ref_clear(&mod->compositor);
	G_OBJECT_CLASS(gowl_module_cube_parent_class)->finalize(object);
}

static void
gowl_module_cube_class_init(GowlModuleCubeClass *klass)
{
	GowlModuleClass *mod = GOWL_MODULE_CLASS(klass);

	mod->activate        = cube_activate;
	mod->deactivate      = cube_deactivate;
	mod->get_name        = cube_name;
	mod->get_description = cube_description;
	mod->get_version     = cube_version;
	G_OBJECT_CLASS(klass)->finalize = cube_finalize;
}

static void
gowl_module_cube_init(GowlModuleCube *mod)
{
	g_weak_ref_init(&mod->compositor, NULL);
	gowl_module_set_priority(GOWL_MODULE(mod), GOWL_CUBE_PRIORITY);
}

G_MODULE_EXPORT GType
gowl_module_register(void)
{
	return GOWL_TYPE_MODULE_CUBE;
}
