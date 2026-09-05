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
};

#define GOWL_CURVE_DEFAULT 1        /* ease-out-quint */

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

gboolean
gowl_animation_tick(GowlCompositor *self, gint64 now_us)
{
	GList *l;
	gboolean live = FALSE;
	const gchar *curve;

	g_return_val_if_fail(GOWL_IS_COMPOSITOR(self), FALSE);

	curve = self->config != NULL
		? gowl_config_get_animation_curve(self->config)
		: NULL;

	for (l = self->clients; l != NULL; l = l->next) {
		GowlClient *c = (GowlClient *)l->data;
		gdouble t, e;

		if (!c->anim_active || c->scene == NULL)
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
		live = TRUE;
	}

	return live;
}
