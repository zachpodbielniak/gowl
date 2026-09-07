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
 * Named easing curves, shared by every presentation module.
 *
 * These were the `animation' module's private table until the `cube'
 * module needed the same names to mean the same thing.  Two tables
 * would have drifted the first time one of them gained a curve.
 */

#include "gowl-easing.h"

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
	 * settles quickly enough to leave time for the jiggle rather than a
	 * window being dragged.  The default, because quint spends longer
	 * in the middle and at these durations that middle feels like lag.
	 */
	{ "ease-out-expo",     0.16, 1.00, 0.30, 1.00 },

	/*
	 * These two overshoot: y1 above 1 carries the value PAST its target
	 * before it comes back.  Not the default, and worth knowing why ---
	 * in a tiling layout windows are adjacent, so an overshoot briefly
	 * puts one on top of its neighbour.  It looks great with gaps and
	 * wrong without them.  A cube rotation has no neighbour to collide
	 * with, so overshoot there is free and reads as weight.
	 */
	{ "ease-out-back",     0.34, 1.56, 0.64, 1.00 },
	{ "spring",            0.16, 1.24, 0.30, 1.00 },
};

#define GOWL_CURVE_DEFAULT 5        /* ease-out-expo */

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

static const GowlCurveDef *
curve_lookup(const gchar *name)
{
	gsize i;

	if (name == NULL)
		return NULL;

	for (i = 0; i < G_N_ELEMENTS(curves); i++) {
		if (g_ascii_strcasecmp(curves[i].name, name) == 0)
			return &curves[i];
	}
	return NULL;
}

gboolean
gowl_easing_name_is_known(const gchar *name)
{
	return curve_lookup(name) != NULL;
}

gdouble
gowl_easing_eval(const gchar *name, gdouble t)
{
	const GowlCurveDef *c;
	gdouble u;

	if (t <= 0.0)
		return 0.0;
	if (t >= 1.0)
		return 1.0;

	c = curve_lookup(name);
	if (c == NULL)
		c = &curves[GOWL_CURVE_DEFAULT];

	/* Linear needs no solve, and it is the curve most likely to be
	 * chosen by someone who wants the cost down. */
	if (c->x1 == 0.0 && c->y1 == 0.0 && c->x2 == 1.0 && c->y2 == 1.0)
		return t;

	u = bezier_solve(t, c->x1, c->x2);
	return bezier_axis(u, c->y1, c->y2);
}
