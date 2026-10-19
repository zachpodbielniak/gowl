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
 * Implode: the desktop is pulled into the pointer, the opposite of
 * explode.  It breaks into pieces that are drawn in -- the ones nearest
 * the pointer first, the far corners last -- each accelerating as it
 * goes, shrinking to nothing and spinning as it reaches the point, while
 * the next desktop settles into place behind from slightly too close.
 *
 * Every piece arrives by the end: the last starts at 35% and takes the
 * remaining 65% to land.  Built on the shared shards
 * (gowl_fx_transition_draw_shards()).
 */

#undef G_LOG_DOMAIN
#define G_LOG_DOMAIN "gowl-implode"

#include "fx/gowl-fx-transition-module.h"

#include <math.h>

#define IMPLODE_SPREAD 0.35

typedef struct {
	GowlFxShard *shards;
	gint         n;
} ImplodeState;

static gboolean
implode_setup(GowlFxTransitionRun *run)
{
	ImplodeState *st = g_new0(ImplodeState, 1);
	gint rows = 12;
	gint cols = MAX(4, (gint)lround(rows * (gdouble)run->width
	                                       / MAX(run->height, 1)));

	st->shards = gowl_fx_shards_new(run->width, run->height, cols, rows,
	                                run->seed, &st->n);
	run->state = st;
	return st->n > 0;
}

static void
implode_teardown(GowlFxTransitionRun *run)
{
	ImplodeState *st = run->state;

	g_free(st->shards);
	g_free(st);
	run->state = NULL;
}

static gdouble
smoothstep(gdouble a, gdouble b, gdouble x)
{
	gdouble t = CLAMP((x - a) / (b - a), 0.0, 1.0);

	return t * t * (3.0 - 2.0 * t);
}

static gboolean
implode_place(const GowlFxShard *s, gint index,
              const GowlFxTransitionRun *run, gdouble eased, gdouble linear,
              gfloat *offset, gfloat *axis, gfloat *angle, gfloat *scale,
              gfloat *shade, gfloat *alpha, gpointer user_data)
{
	gdouble w = (gdouble)run->width, h = (gdouble)run->height;
	gdouble px = run->center[0] * w, py = run->center[1] * h;
	gdouble dx = s->cx - px, dy = s->cy - py;
	gdouble far = sqrt(MAX(px, w - px) * MAX(px, w - px)
	                   + MAX(py, h - py) * MAX(py, h - py));
	gdouble delay = IMPLODE_SPREAD
	                * CLAMP(sqrt(dx * dx + dy * dy) / far, 0.0, 1.0);
	gdouble t = CLAMP((linear - delay) / (1.0 - IMPLODE_SPREAD), 0.0, 1.0);
	/* Drawn in: slow to let go, then faster and faster. */
	gdouble pull = pow(t, 2.2);
	gdouble len;

	(void)index;
	(void)eased;
	(void)user_data;
	if (t <= 0.0)
		return TRUE;

	/* Towards the pointer; world y up, screen y down. */
	offset[0] = (gfloat)(-dx * pull);
	offset[1] = (gfloat)(dy * pull);
	offset[2] = (gfloat)(-h * 0.25 * pull);

	axis[0] = s->r[1] - 0.5f;
	axis[1] = s->r[2] - 0.5f;
	axis[2] = 0.6f;
	len = sqrt(axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2]);
	axis[0] /= (gfloat)len;
	axis[1] /= (gfloat)len;
	axis[2] /= (gfloat)len;
	*angle = (gfloat)(pull * (1.0 + 2.5 * s->r[3]) * G_PI);
	*scale = (gfloat)(1.0 - pull);
	*shade = (gfloat)(1.0 - 0.4 * pull);
	*alpha = (gfloat)(1.0 - smoothstep(0.8, 1.0, t));
	return TRUE;
}

static gboolean
implode_draw(GowlFxPass *pass, GowlFxTransitionRun *run, gdouble eased,
             gdouble linear)
{
	ImplodeState *st = run->state;

	return gowl_fx_transition_draw_shards(pass, run, st->shards, st->n,
	                                      implode_place, NULL, eased,
	                                      linear,
	                                      (gfloat)(1.06 - 0.06 * eased),
	                                      (gfloat)(0.45 + 0.55 * eased));
}

static const GowlFxTransitionKind implode_kind = {
	.name           = "implode",
	.label          = "Implode",
	.duration_scale = 1.5,
	.setup          = implode_setup,
	.teardown       = implode_teardown,
	.draw           = implode_draw,
};

GOWL_FX_DEFINE_TRANSITION_MODULE(GowlModuleImplode, gowl_module_implode,
                                 &implode_kind, "0.1.0",
                                 "Pulls the desktop into the pointer to the next tag")
