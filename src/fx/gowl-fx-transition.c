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
 * A tag transition's lifecycle, shared by every transition module.  See
 * gowl-fx-transition.h for the division of labour.
 *
 * This is the cube's run machinery (modules/cube) with the drawing taken
 * out, and every decision in it was made there first, for the reason
 * written beside it:
 *
 *   - switches are noticed by POLLING the output's tags in the frame
 *     hook, against a memory kept on the output itself, rather than by
 *     the monitor's tag-changed signal: by the time a signal would fire
 *     the scene is already rearranged, and polling also covers an output
 *     that appeared after the module loaded;
 *   - every desktop is captured up front, at the start, in one frame:
 *     capturing later would rearrange the scene mid-flight;
 *   - the output is taken with one opaque sheet, never by switching off a
 *     shared scene layer (layers are global, tags per-output: hiding one
 *     blanks the other screen);
 *   - the reveal fade a switch starts is claimed and dropped, because the
 *     first captured frame already shows those windows;
 *   - a switch arriving mid-flight ends the run and starts a fresh one
 *     from where it was headed.
 *
 * What is new is the arbitration: before starting, the host asks the
 * compositor whether its kind is the one to animate this switch
 * (gowl_compositor_tag_transition_is()), so any number of transition
 * modules can be loaded and exactly one draws.
 */

#include "gowl-fx-transition.h"
#include "gowl-fx-private.h"

#include "core/gowl-core-private.h"
#include "core/gowl-compositor.h"
#include "core/gowl-monitor.h"
#include "config/gowl-config.h"
#include "util/gowl-easing.h"

#include <string.h>

#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_output.h>

/* Desktops crossed but not landed on are only seen mid-flight; half size
 * is invisible there and keeps a long journey on a 4K screen affordable. */
#define GOWL_FX_TRANSITION_INTERMEDIATE_DIVISOR 2

/* The most desktops a multi-step kind is given: a jump across all of a
 * monitor's tags at full size would cost far more than it shows. */
#define GOWL_FX_TRANSITION_MAX_CHAIN 10

struct _GowlFxTransitionHost {
	const GowlFxTransitionKind *kind;
	GWeakRef    compositor;
	GowlFxGl   *gl;
	gboolean    gl_tried;
	gboolean    registered;
	GList      *runs;        /* GowlFxTransitionRun* */
	gchar      *memory_key;  /* per-kind tag memory on each output */

	/* Set while a capture is rendering the scene.  A capture toggles
	 * scene visibility, and a hook running in the middle of it would see
	 * a scene that is deliberately lying about which tag is shown. */
	gboolean    capturing;
};

gint
gowl_fx_transition_tag_index(guint32 tags)
{
	gint i;

	if (tags == 0)
		return -1;
	for (i = 0; i < 32; i++)
		if (tags & (1u << i))
			return i;
	return -1;
}

GowlFxTransitionHost *
gowl_fx_transition_host_new(const GowlFxTransitionKind *kind)
{
	GowlFxTransitionHost *host;

	g_return_val_if_fail(kind != NULL && kind->name != NULL, NULL);
	g_return_val_if_fail(kind->draw != NULL, NULL);

	host = g_new0(GowlFxTransitionHost, 1);
	host->kind = kind;
	g_weak_ref_init(&host->compositor, NULL);
	host->memory_key = g_strdup_printf("gowl-fx-transition-tags-%s",
	                                   kind->name);
	return host;
}

/* ── GL ──────────────────────────────────────────────────────────── */

/* Built on first use, not at activate(): a module can load before the
 * compositor has a renderer.  gl_tried keeps a "no" from being asked
 * again every frame. */
static void
host_ensure_gl(GowlFxTransitionHost *host, GowlCompositor *self)
{
	if (host->gl != NULL || host->gl_tried)
		return;
	if (self == NULL || self->renderer == NULL)
		return;
	host->gl_tried = TRUE;
	host->gl = gowl_fx_gl_new(self->renderer);
}

/* ── Runs ────────────────────────────────────────────────────────── */

static void
run_free(GowlFxTransitionHost *host, GowlFxTransitionRun *run)
{
	gint i;

	if (run == NULL)
		return;
	if (host->kind->teardown != NULL && run->state != NULL)
		host->kind->teardown(run);
	g_clear_pointer((GowlFxSheet **)&run->sheet, gowl_fx_sheet_free);
	if (host->gl != NULL) {
		gowl_fx_texture_drop(host->gl, &run->from);
		gowl_fx_texture_drop(host->gl, &run->to);
		for (i = 0; i < run->n_chain; i++)
			gowl_fx_texture_drop(host->gl, &run->chain[i]);
	}
	g_free(run->chain);
	g_free(run);
}

static void
run_end(GowlFxTransitionHost *host, GowlFxTransitionRun *run)
{
	host->runs = g_list_remove(host->runs, run);
	run_free(host, run);
}

static void
runs_end_all(GowlFxTransitionHost *host)
{
	while (host->runs != NULL)
		run_end(host, host->runs->data);
}

GowlFxTransitionRun *
gowl_fx_transition_host_running(GowlFxTransitionHost *host, GowlMonitor *m)
{
	GList *l;

	g_return_val_if_fail(host != NULL, NULL);

	for (l = host->runs; l != NULL; l = l->next) {
		GowlFxTransitionRun *run = l->data;

		if (run->monitor == m)
			return run;
	}
	return NULL;
}

/* ── The tag memory ──────────────────────────────────────────────── */

/* Kept on the output, so it lives and dies with it; per kind, because
 * each host notices switches on its own. */
static guint32
remembered_tags(GowlFxTransitionHost *host, GowlMonitor *m)
{
	return (guint32)GPOINTER_TO_UINT(
		g_object_get_data(G_OBJECT(m), host->memory_key));
}

static void
remember_tags(GowlFxTransitionHost *host, GowlMonitor *m, guint32 tags)
{
	/* Zero means "nothing recorded": an output that genuinely shows no
	 * tags arms nothing, which is right -- there is nothing to leave. */
	g_object_set_data(G_OBJECT(m), host->memory_key, GUINT_TO_POINTER(tags));
}

/* ── Whether to run at all ───────────────────────────────────────── */

static gint64
run_duration_us(GowlFxTransitionHost *host, GowlCompositor *self, gint steps)
{
	gdouble base = (gdouble)gowl_config_get_tag_transition_duration(
		self->config);
	gdouble ms = base * host->kind->duration_scale;

	if (host->kind->multi_step && steps > 1)
		ms += base * host->kind->step_scale * (gdouble)(steps - 1);
	return (gint64)(ms * 1000.0);
}

/*
 * Everything that can say no, before anything is touched: a declined
 * transition costs an instant switch, which is what there was before.
 */
static gboolean
host_wants(GowlFxTransitionHost *host, GowlCompositor *self, GowlMonitor *m,
           guint32 previous, guint32 current)
{
	if (self == NULL || self->config == NULL || m == NULL)
		return FALSE;
	if (self->locked || host->gl == NULL || host->capturing)
		return FALSE;
	if (previous == 0 || current == 0 || previous == current)
		return FALSE;
	if (gowl_config_get_tag_transition_duration(self->config) <= 0
	    || host->kind->duration_scale <= 0.0)
		return FALSE;
	if (!gowl_config_get_tag_transition_all_monitors(self->config)
	    && self->selmon != m)
		return FALSE;
	/* A rotated output sits transitions out: sheets are drawn in the
	 * output's buffer orientation, the captures are upright. */
	if (m->wlr_output == NULL || m->scene_output == NULL
	    || m->wlr_output->transform != WL_OUTPUT_TRANSFORM_NORMAL)
		return FALSE;
	return gowl_compositor_tag_transition_is(self, m, host->kind->name);
}

/* ── Capture ─────────────────────────────────────────────────────── */

/*
 * The scene as it would look on @tags, into a texture the host owns.
 *
 * The layers above the sheet are switched off for the duration -- the
 * bar and notifications stay live ON TOP of the transition, so baking
 * them into both desktops would show two bars -- and so is the recording
 * indicator, which is put back at once: it is a safety light and must
 * never be off on a frame that is actually presented, which this one
 * is not.
 */
static gboolean
capture_desktop(GowlFxTransitionHost *host, GowlCompositor *self,
                GowlMonitor *m, guint32 tags, GowlFxTexture *out,
                gint divisor)
{
	GowlFxVis *vis;
	gboolean   ok;
	gint       i;

	host->capturing = TRUE;
	vis = gowl_fx_vis_begin();

	gowl_fx_vis_hide_layer(vis, self, GOWL_SCENE_LAYER_TOP, FALSE);
	gowl_fx_vis_hide_layer(vis, self, GOWL_SCENE_LAYER_BLOCK, FALSE);
	/* Overlay keeps its embedder-pinned clients (cmacs's buffer-embedded
	 * windows are placed over Emacs, not by tag), so they are on both
	 * desktops rather than on neither. */
	gowl_fx_vis_hide_layer(vis, self, GOWL_SCENE_LAYER_OVERLAY, TRUE);
	for (i = 0; i < 4; i++) {
		if (self->rec_indicator[i] != NULL)
			gowl_fx_vis_set(vis, &self->rec_indicator[i]->node, FALSE);
	}
	gowl_fx_vis_show_tags(vis, self, m, tags);

	ok = gowl_fx_capture(host->gl, self, m, out, divisor);

	gowl_fx_vis_restore(vis);
	host->capturing = FALSE;
	return ok;
}

/* Where the pointer is on @m, 0..1 with v down; the centre otherwise. */
static void
pointer_center(GowlCompositor *self, GowlMonitor *m, gfloat *center)
{
	center[0] = 0.5f;
	center[1] = 0.5f;
	if (self->wlr_cursor == NULL || m->m.width <= 0 || m->m.height <= 0)
		return;
	if (self->wlr_cursor->x < m->m.x
	    || self->wlr_cursor->x >= m->m.x + m->m.width
	    || self->wlr_cursor->y < m->m.y
	    || self->wlr_cursor->y >= m->m.y + m->m.height)
		return;
	center[0] = (gfloat)((self->wlr_cursor->x - m->m.x) / m->m.width);
	center[1] = (gfloat)((self->wlr_cursor->y - m->m.y) / m->m.height);
}

static GowlFxTransitionRun *
run_start(GowlFxTransitionHost *host, GowlCompositor *self, GowlMonitor *m,
          guint32 from_tags, guint32 to_tags, gint64 now_us)
{
	GowlFxTransitionRun *run;
	gint j;

	run = g_new0(GowlFxTransitionRun, 1);
	run->monitor    = m;
	run->from_tags  = from_tags;
	run->to_tags    = to_tags;
	run->from_index = gowl_fx_transition_tag_index(from_tags);
	run->to_index   = gowl_fx_transition_tag_index(to_tags);
	run->dir        = run->to_index >= run->from_index ? 1 : -1;
	run->steps      = MAX(1, ABS(run->to_index - run->from_index));
	run->seed       = g_random_int();
	run->start_us   = now_us;
	pointer_center(self, m, run->center);

	/* The two ends are what make it seamless; without either one the
	 * transition would visibly cut, so either failing declines it. */
	if (!capture_desktop(host, self, m, from_tags, &run->from, 1)
	    || !capture_desktop(host, self, m, to_tags, &run->to, 1)) {
		run_free(host, run);
		return NULL;
	}
	run->width  = run->from.width;
	run->height = run->from.height;

	/*
	 * A kind that shows the desktops on the way (the carousel) gets
	 * every tag crossed, at half size; the ends are the full-size
	 * captures above, reached through gowl_fx_transition_run_desktop().
	 * A journey longer than the cap, or between views that are not
	 * single tags, is shown as one step: the ends are what matter.
	 */
	if (host->kind->multi_step) {
		if (run->steps + 1 > GOWL_FX_TRANSITION_MAX_CHAIN
		    || (from_tags & (from_tags - 1)) != 0
		    || (to_tags & (to_tags - 1)) != 0)
			run->steps = 1;
		run->n_chain = run->steps + 1;
		run->chain = g_new0(GowlFxTexture, run->n_chain);
		for (j = 1; j < run->steps; j++) {
			gint tag = run->from_index + run->dir * j;

			if (tag >= 0 && tag < 32)
				capture_desktop(host, self, m, 1u << tag,
				                &run->chain[j],
				                GOWL_FX_TRANSITION_INTERMEDIATE_DIVISOR);
		}
	} else {
		run->steps = 1;
	}

	run->dur_us = run_duration_us(host, self, run->steps);
	if (run->dur_us <= 0) {
		run_free(host, run);
		return NULL;
	}

	if (host->kind->setup != NULL && !host->kind->setup(run)) {
		run_free(host, run);
		return NULL;
	}

	run->sheet = gowl_fx_sheet_new(self, m, GOWL_FX_SHEET_NONE);
	if (run->sheet == NULL) {
		run_free(host, run);
		return NULL;
	}

	host->runs = g_list_prepend(host->runs, run);
	return run;
}

const GowlFxTexture *
gowl_fx_transition_run_desktop(const GowlFxTransitionRun *run, gint j)
{
	g_return_val_if_fail(run != NULL, NULL);

	if (j <= 0)
		return &run->from;
	if (j >= run->steps)
		return &run->to;
	if (run->chain != NULL && j < run->n_chain && run->chain[j].tex != 0)
		return &run->chain[j];
	/* An intermediate that failed to capture: show the nearer end. */
	return j * 2 < run->steps ? &run->from : &run->to;
}

/* ── Frame ───────────────────────────────────────────────────────── */

static gboolean
run_draw(GowlFxTransitionHost *host, GowlCompositor *self,
         GowlFxTransitionRun *run, gint64 now_us)
{
	GowlFxPass *pass;
	struct wlr_buffer *buffer;
	gdouble linear, eased;
	gboolean ok;

	linear = run->dur_us > 0
		? CLAMP((gdouble)(now_us - run->start_us) / (gdouble)run->dur_us,
		        0.0, 1.0)
		: 1.0;
	eased = gowl_easing_eval(gowl_config_get_tag_transition_curve(
		self->config), linear);

	buffer = gowl_fx_sheet_acquire((GowlFxSheet *)run->sheet);
	if (buffer == NULL)
		return FALSE;
	pass = gowl_fx_pass_begin(host->gl, buffer);
	if (pass == NULL) {
		wlr_buffer_unlock(buffer);
		return FALSE;
	}
	ok = host->kind->draw(pass, run, eased, linear);
	ok = gowl_fx_pass_end(pass) && ok;

	if (ok)
		gowl_fx_sheet_present((GowlFxSheet *)run->sheet, buffer);
	wlr_buffer_unlock(buffer);
	return ok;
}

/* Notice a switch and, if this kind is the one to animate it, start. */
static void
poll_tags(GowlFxTransitionHost *host, GowlCompositor *self, GowlMonitor *m,
          gint64 now_us)
{
	guint32 previous = remembered_tags(host, m);
	guint32 current  = m->tagset[m->seltags];

	if (current == previous)
		return;
	remember_tags(host, m, current);

	if (!host_wants(host, self, m, previous, current))
		return;

	/* A switch arriving mid-flight ends the run in flight and starts
	 * afresh from where it was headed; bending the old one would mean
	 * re-capturing under textures still on screen. */
	if (gowl_fx_transition_host_running(host, m) != NULL)
		run_end(host, gowl_fx_transition_host_running(host, m));

	run_start(host, self, m, previous, current, now_us);
}

void
gowl_fx_transition_host_attach(GowlFxTransitionHost *host,
                               GowlCompositor       *self)
{
	GowlCompositor *held;

	g_return_if_fail(host != NULL);
	if (self == NULL)
		return;

	held = g_weak_ref_get(&host->compositor);
	if (held != self)
		g_weak_ref_set(&host->compositor, self);
	if (held != NULL)
		g_object_unref(held);

	if (!host->registered || held != self) {
		gowl_compositor_register_tag_transition(self, host->kind->name,
		                                        host->kind->label);
		host->registered = TRUE;
	}
}

gboolean
gowl_fx_transition_host_frame(GowlFxTransitionHost *host,
                              GowlCompositor       *self,
                              GowlMonitor          *m,
                              gint64                now)
{
	GowlFxTransitionRun *run;

	g_return_val_if_fail(host != NULL, FALSE);

	gowl_fx_transition_host_attach(host, self);
	if (host->capturing || m == NULL)
		return FALSE;

	host_ensure_gl(host, self);
	poll_tags(host, self, m, now);

	run = gowl_fx_transition_host_running(host, m);
	if (run == NULL)
		return FALSE;

	if (now >= run->start_us + run->dur_us) {
		run_end(host, run);
		return TRUE;
	}
	if (!run_draw(host, self, run, now)) {
		/* A frame that cannot be drawn ends the transition rather than
		 * leaving a stale sheet over the desktop. */
		run_end(host, run);
		return TRUE;
	}
	if (m->wlr_output != NULL)
		wlr_output_schedule_frame(m->wlr_output);
	return TRUE;
}

/*
 * TRUE when @m is one frame away from starting a run, or in one: the
 * reveal the same arrange() started is this kind's to swallow.
 */
static gboolean
host_pending(GowlFxTransitionHost *host, GowlCompositor *self, GowlMonitor *m)
{
	if (m == NULL || host->capturing)
		return FALSE;
	if (gowl_fx_transition_host_running(host, m) != NULL)
		return TRUE;
	host_ensure_gl(host, self);
	return host_wants(host, self, m, remembered_tags(host, m),
	                  m->tagset[m->seltags]);
}

gboolean
gowl_fx_transition_host_client_event(GowlFxTransitionHost *host,
                                     GowlCompositor       *self,
                                     GowlClient           *c,
                                     GowlSceneEffectEvent  event)
{
	g_return_val_if_fail(host != NULL, FALSE);

	gowl_fx_transition_host_attach(host, self);

	/* The ONLY event a transition claims.  FALSE for everything else is
	 * what lets the animation module keep owning window geometry while a
	 * transition owns the output. */
	return event == GOWL_SCENE_EFFECT_REVEAL && c != NULL
	       && host_pending(host, self, c->mon);
}

void
gowl_fx_transition_host_monitor_removed(GowlFxTransitionHost *host,
                                        GowlCompositor       *self,
                                        GowlMonitor          *m)
{
	GowlFxTransitionRun *run;

	g_return_if_fail(host != NULL);
	(void)self;

	/* The run holds a scene tree under a scene losing this output, and
	 * textures to free while the renderer is still there. */
	run = gowl_fx_transition_host_running(host, m);
	if (run != NULL)
		run_end(host, run);
}

void
gowl_fx_transition_host_finish(GowlFxTransitionHost *host)
{
	g_return_if_fail(host != NULL);

	runs_end_all(host);
	g_clear_pointer(&host->gl, gowl_fx_gl_free);
	host->gl_tried = FALSE;
}

void
gowl_fx_transition_host_detach(GowlFxTransitionHost *host)
{
	GowlCompositor *self;

	g_return_if_fail(host != NULL);

	gowl_fx_transition_host_finish(host);
	self = g_weak_ref_get(&host->compositor);
	if (self != NULL) {
		if (host->registered)
			gowl_compositor_unregister_tag_transition(self,
			                                          host->kind->name);
		g_object_unref(self);
	}
	host->registered = FALSE;
	g_weak_ref_set(&host->compositor, NULL);
}

void
gowl_fx_transition_host_free(GowlFxTransitionHost *host)
{
	if (host == NULL)
		return;
	gowl_fx_transition_host_detach(host);
	g_weak_ref_clear(&host->compositor);
	g_free(host->memory_key);
	g_free(host);
}

/* ── Drawing helper ──────────────────────────────────────────────── */

void
gowl_fx_transition_pass_desktop(GowlFxPass          *pass,
                                const GowlFxTexture *texture,
                                const gfloat        *mvp,
                                const gfloat        *pos,
                                gfloat               alpha,
                                gfloat               shade)
{
	GowlFxQuad quad;
	gfloat rect[12];
	gint w, h;

	g_return_if_fail(pass != NULL);
	if (texture == NULL || texture->tex == 0 || alpha <= 0.0f)
		return;

	gowl_fx_quad_init(&quad);
	quad.texture = texture->tex;
	quad.alpha = CLAMP(alpha, 0.0f, 1.0f);
	quad.tint[0] = quad.tint[1] = quad.tint[2] = CLAMP(shade, 0.0f, 4.0f);
	if (mvp != NULL) {
		quad.mvp = mvp;
		if (pos == NULL) {
			/* The output's own rectangle, in pixels, strip order:
			 * top-left, bottom-left, top-right, bottom-right. */
			w = pass->width;
			h = pass->height;
			rect[0] = 0.0f;        rect[1] = 0.0f;        rect[2] = 0.0f;
			rect[3] = 0.0f;        rect[4] = (gfloat)h;   rect[5] = 0.0f;
			rect[6] = (gfloat)w;   rect[7] = 0.0f;        rect[8] = 0.0f;
			rect[9] = (gfloat)w;   rect[10] = (gfloat)h;  rect[11] = 0.0f;
			pos = rect;
		}
		quad.pos = pos;
	}
	gowl_fx_pass_quad(pass, &quad);
}
