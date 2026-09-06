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
 * gowl-animation.c -- sliding windows into place
 *
 * What is animated is the scene node's POSITION, and only that.  The
 * client is configured to its final size the moment the layout decides
 * it, and the node slides to meet it.
 *
 * Animating the size too would look better and cost far more: every
 * intermediate size is a configure the client has to acknowledge and
 * re-render for, so a 200 ms resize becomes a dozen round trips per
 * window per move, and a client that renders slowly falls visibly
 * behind its own frame.  Position is a compositor-side transform of
 * something already rendered, so it is free.
 *
 * The state lives on the client rather than in a list on the
 * compositor.  A client destroyed mid-slide takes its animation with
 * it, which a separate list would have to be told about.
 */

#include "gowl-animation.h"
#include "gowl-core-private.h"
#include "gowl-compositor.h"

#include <math.h>

#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_output.h>

/* ── Curves ──────────────────────────────────────────────────────── */

typedef struct {
	const gchar *name;
	gdouble x1, y1, x2, y2;
} GowlCurveDef;

/*
 * The same curves Omarchy configures Hyprland with, by the same names
 * in kebab-case.  Endpoints are fixed at (0,0) and (1,1), so these two
 * control points are the whole definition.
 */
static const GowlCurveDef curves[] = {
	{ "linear",            0.00, 0.00, 1.00, 1.00 },
	{ "ease-out-quint",    0.23, 1.00, 0.32, 1.00 },
	{ "ease-in-out-cubic", 0.65, 0.05, 0.36, 1.00 },
	{ "almost-linear",     0.50, 0.50, 0.75, 1.00 },
	{ "quick",             0.15, 0.00, 0.10, 1.00 },

	/*
	 * Leaves almost the whole distance in the first third and then
	 * settles, which at 180 ms reads as decisive rather than as a
	 * window being dragged.  The default, because quint spends longer
	 * in the middle and at these durations that middle feels like lag.
	 */
	{ "ease-out-expo",     0.16, 1.00, 0.30, 1.00 },

	/*
	 * These two overshoot: y1 above 1 carries the window PAST its
	 * target before it comes back.  Not the default, and worth
	 * knowing why --- in a tiling layout windows are adjacent, so an
	 * overshoot briefly puts one on top of its neighbour.  It looks
	 * great with gaps and wrong without them.
	 */
	{ "ease-out-back",     0.34, 1.56, 0.64, 1.00 },
	{ "spring",            0.16, 1.24, 0.30, 1.00 },
};

#define GOWL_CURVE_DEFAULT 5        /* ease-out-expo */

/*
 * How far below its final position a window starts.  Small on purpose:
 * a big offset is a slide, and a slide from anywhere but the window's
 * own neighbourhood is the corner-sweep bug wearing a different hat.
 * This is a nudge that the fade does most of the work of hiding.
 */
#define GOWL_ANIM_OPEN_RISE (24)

static gdouble
bezier_axis(gdouble t, gdouble p1, gdouble p2)
{
	gdouble mt = 1.0 - t;

	/* Cubic Bézier with P0 = 0 and P3 = 1. */
	return 3.0 * mt * mt * t * p1
	     + 3.0 * mt * t * t * p2
	     + t * t * t;
}

/*
 * A Bézier is parameterised by its own t, not by x, so evaluating
 * "y at x = 0.4" means solving for the t whose x is 0.4 first.  Bisection
 * rather than Newton: these curves can have a near-zero derivative at
 * the ends (ease-out-quint does), where Newton diverges, and 24
 * bisection steps resolve far finer than a pixel.
 */
static gdouble
bezier_solve(gdouble x, gdouble x1, gdouble x2)
{
	gdouble lo = 0.0, hi = 1.0, mid = x;
	gint i;

	for (i = 0; i < 24; i++) {
		gdouble cx;

		mid = 0.5 * (lo + hi);
		cx = bezier_axis(mid, x1, x2);
		if (cx < x)
			lo = mid;
		else
			hi = mid;
	}
	return mid;
}

gdouble
gowl_curve_eval(const gchar *name, gdouble t)
{
	const GowlCurveDef *c = &curves[GOWL_CURVE_DEFAULT];
	gsize i;
	gdouble u;

	if (t <= 0.0)
		return 0.0;
	if (t >= 1.0)
		return 1.0;

	if (name != NULL) {
		for (i = 0; i < G_N_ELEMENTS(curves); i++) {
			if (g_ascii_strcasecmp(curves[i].name, name) == 0) {
				c = &curves[i];
				break;
			}
		}
	}

	/* Linear needs no solve, and it is the curve most likely to be
	 * chosen by someone who wants the cost down. */
	if (c->x1 == 0.0 && c->y1 == 0.0 && c->x2 == 1.0 && c->y2 == 1.0)
		return t;

	u = bezier_solve(t, c->x1, c->x2);
	return bezier_axis(u, c->y1, c->y2);
}

/* ── Configuration ───────────────────────────────────────────────── */

gboolean
gowl_animation_enabled(GowlCompositor *self)
{
	g_return_val_if_fail(GOWL_IS_COMPOSITOR(self), FALSE);

	if (self->config == NULL)
		return FALSE;
	if (!gowl_config_get_animations(self->config))
		return FALSE;

	return gowl_config_get_animation_duration(self->config) > 0;
}

/*
 * How long a move should take, and how long an open should.  Opening
 * gets its own duration because the two are not the same gesture: a
 * re-tile is a correction and wants to be over quickly, while an open
 * is an arrival and can afford the extra beat that makes it read as
 * one.  Either falls back to the single `animation-duration'.
 */
static gint
open_duration(GowlCompositor *self)
{
	gint d;

	if (self->config == NULL)
		return 0;

	d = gowl_config_get_animation_duration_open(self->config);
	if (d < 0)
		d = gowl_config_get_animation_duration(self->config);

	return d;
}

/* ── Opening ─────────────────────────────────────────────────────── */

void
gowl_animation_open_start(GowlCompositor *self, GowlClient *c)
{
	gint duration;

	g_return_if_fail(GOWL_IS_COMPOSITOR(self));
	g_return_if_fail(c != NULL);

	if (!gowl_animation_enabled(self) || c->scene == NULL)
		return;

	duration = open_duration(self);
	if (duration <= 0)
		return;

	c->anim_opening = TRUE;
	c->anim_open_start_us = g_get_monotonic_time();
	c->anim_open_dur_us = (gint64)duration * 1000;

	/* Start invisible.  Set here rather than left to the first tick so
	 * that the frame between mapping and the first tick does not show
	 * the window at full opacity --- one frame of pop is still a pop. */
	gowl_client_set_anim_alpha(c, 0.0f);
}

void
gowl_animation_open_cancel(GowlClient *c)
{
	if (c == NULL || !c->anim_opening)
		return;

	c->anim_opening = FALSE;
	gowl_client_set_anim_alpha(c, 1.0f);
}

/*
 * Advances one client's open animation.  Returns TRUE while it is
 * still running.
 */
static gboolean
open_tick(GowlCompositor *self, GowlClient *c, gint64 now_us,
          const gchar *curve)
{
	gdouble t, e;
	gint rise;

	if (!c->anim_opening)
		return FALSE;

	if (c->anim_open_dur_us <= 0) {
		gowl_animation_open_cancel(c);
		return FALSE;
	}

	t = (gdouble)(now_us - c->anim_open_start_us)
		/ (gdouble)c->anim_open_dur_us;
	if (t < 0.0)
		t = 0.0;
	if (t >= 1.0) {
		gowl_animation_open_cancel(c);
		/* The rise is over, so put the node exactly where the layout
		 * wants it --- unless a move animation has since taken over,
		 * which owns the position from then on. */
		if (!c->anim_active)
			wlr_scene_node_set_position(&c->scene->node,
			                            c->geom.x, c->geom.y);
		return FALSE;
	}

	e = gowl_curve_eval(curve, t);
	gowl_client_set_anim_alpha(c, (gfloat)e);

	/*
	 * The rise, in the same easing.  A move animation started
	 * mid-open owns the position, so the rise steps aside rather than
	 * fighting it for the same node --- the fade carries on either
	 * way, which is the half that does most of the work.
	 */
	if (!c->anim_active) {
		rise = (gint)lround((1.0 - e) * (gdouble)GOWL_ANIM_OPEN_RISE);
		wlr_scene_node_set_position(&c->scene->node,
		                            c->geom.x, c->geom.y + rise);
	}

	return TRUE;
}

/* ── Driving ─────────────────────────────────────────────────────── */

void
gowl_animation_start(GowlCompositor *self, GowlClient *c,
                      gint from_x, gint from_y, gint to_x, gint to_y)
{
	gint duration;

	g_return_if_fail(GOWL_IS_COMPOSITOR(self));
	g_return_if_fail(c != NULL);

	if (!gowl_animation_enabled(self) || c->scene == NULL)
		return;

	/* Nothing to slide.  Checked here rather than at every call site,
	 * because a layout re-runs on every arrange and most clients do not
	 * move. */
	if (from_x == to_x && from_y == to_y) {
		gowl_animation_cancel(c);
		return;
	}

	duration = gowl_config_get_animation_duration(self->config);

	/* Retarget from where the node actually is, not from the caller's
	 * idea of "before".  A second layout change mid-flight should bend
	 * the path, not teleport the window back to restart it. */
	if (c->anim_active) {
		from_x = c->anim_cur_x;
		from_y = c->anim_cur_y;
	}

	c->anim_active = TRUE;
	c->anim_from_x = from_x;
	c->anim_from_y = from_y;
	c->anim_to_x = to_x;
	c->anim_to_y = to_y;
	c->anim_cur_x = from_x;
	c->anim_cur_y = from_y;
	c->anim_start_us = g_get_monotonic_time();
	c->anim_dur_us = (gint64)duration * 1000;

	/* Hold the node at the start position; the frame loop moves it. */
	wlr_scene_node_set_position(&c->scene->node, from_x, from_y);
}

void
gowl_animation_cancel(GowlClient *c)
{
	if (c == NULL)
		return;
	c->anim_active = FALSE;
}

/*
 * Whether a client counts towards the asking monitor's liveness.  Every
 * client is still advanced on every tick --- the maths is time-based
 * and idempotent, so two monitors asking gives one answer --- but only
 * the ones actually on this output decide whether it keeps redrawing.
 * A NULL monitor means "anywhere", which is what a test wants.
 */
static gboolean
client_on(GowlClient *c, GowlMonitor *m)
{
	return m == NULL || c->mon == m;
}

gboolean
gowl_animation_tick(GowlCompositor *self, GowlMonitor *m, gint64 now_us)
{
	GList *l;
	gboolean live = FALSE;
	const gchar *curve;

	/* The frame handler runs before the compositor is fully torn down,
	 * so a NULL here is a shutdown race rather than a caller bug. */
	if (self == NULL)
		return FALSE;
	g_return_val_if_fail(GOWL_IS_COMPOSITOR(self), FALSE);

	curve = self->config != NULL
		? gowl_config_get_animation_curve(self->config)
		: NULL;

	for (l = self->clients; l != NULL; l = l->next) {
		GowlClient *c = (GowlClient *)l->data;
		gdouble t, e;

		if (c->scene == NULL)
			continue;

		/* Opening runs alongside a move rather than instead of it: a
		 * window can be re-tiled while it is still fading in. */
		if (open_tick(self, c, now_us, curve) && client_on(c, m))
			live = TRUE;

		if (!c->anim_active)
			continue;

		if (c->anim_dur_us <= 0) {
			c->anim_active = FALSE;
			wlr_scene_node_set_position(&c->scene->node,
			                            c->anim_to_x, c->anim_to_y);
			continue;
		}

		t = (gdouble)(now_us - c->anim_start_us) / (gdouble)c->anim_dur_us;
		if (t >= 1.0) {
			c->anim_active = FALSE;
			c->anim_cur_x = c->anim_to_x;
			c->anim_cur_y = c->anim_to_y;
			wlr_scene_node_set_position(&c->scene->node,
			                            c->anim_to_x, c->anim_to_y);
			continue;
		}
		if (t < 0.0)
			t = 0.0;

		e = gowl_curve_eval(curve, t);
		c->anim_cur_x = c->anim_from_x
			+ (gint)lround((gdouble)(c->anim_to_x - c->anim_from_x) * e);
		c->anim_cur_y = c->anim_from_y
			+ (gint)lround((gdouble)(c->anim_to_y - c->anim_from_y) * e);

		wlr_scene_node_set_position(&c->scene->node,
		                            c->anim_cur_x, c->anim_cur_y);
		if (client_on(c, m))
			live = TRUE;
	}

	return live;
}
