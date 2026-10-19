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
 * Shatter: the desktop is a pane of glass and the pointer is where it was
 * struck.  Cracks open outward from there, pieces break loose in a wave
 * that spreads from the impact, and each falls away under gravity --
 * kicked a little outward and towards you, tumbling -- onto the next
 * desktop behind.
 *
 * Physics on the linear clock: an eased clock would make gravity speed
 * up and slow down.  The wave's last piece leaves at 50% and every piece
 * takes the remaining 50% to fall clear, so the glass is gone by the end.
 */

#undef G_LOG_DOMAIN
#define G_LOG_DOMAIN "gowl-shatter"

#include "fx/gowl-fx-transition-module.h"

#include <math.h>

/* When the last piece breaks loose, and how long a piece falls. */
#define SHATTER_SPREAD  0.45
#define SHATTER_FALL    0.50

typedef struct {
	GowlFxShard *shards;
	gint         n;
} ShatterState;

static gboolean
shatter_setup(GowlFxTransitionRun *run)
{
	ShatterState *st = g_new0(ShatterState, 1);
	gint rows = 11;
	gint cols = MAX(4, (gint)lround(rows * (gdouble)run->width
	                                       / MAX(run->height, 1)));

	st->shards = gowl_fx_shards_new(run->width, run->height, cols, rows,
	                                run->seed, &st->n);
	run->state = st;
	return st->n > 0;
}

static void
shatter_teardown(GowlFxTransitionRun *run)
{
	ShatterState *st = run->state;

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
shatter_place(const GowlFxShard *s, gint index,
              const GowlFxTransitionRun *run, gdouble eased, gdouble linear,
              gfloat *offset, gfloat *axis, gfloat *angle, gfloat *scale,
              gfloat *shade, gfloat *alpha, gpointer user_data)
{
	gdouble w = (gdouble)run->width, h = (gdouble)run->height;
	gdouble px = run->center[0] * w, py = run->center[1] * h;
	gdouble dx = s->cx - px, dy = s->cy - py;
	gdouble far = sqrt(MAX(px, w - px) * MAX(px, w - px)
	                   + MAX(py, h - py) * MAX(py, h - py));
	gdouble delay = SHATTER_SPREAD * CLAMP(sqrt(dx * dx + dy * dy) / far,
	                                       0.0, 1.0)
	                + 0.05 * s->r[0];
	gdouble t = CLAMP((linear - delay) / SHATTER_FALL, 0.0, 1.0);
	gdouble len;

	(void)index;
	(void)eased;
	(void)user_data;

	/* Cracks first: just before it goes, a piece draws in a hair so the
	 * fracture lines show.  Never at progress 0 -- that frame is the
	 * desktop, whole. */
	*scale = (gfloat)(1.0 - 0.02 * smoothstep(0.0, 0.05, linear)
	                         * smoothstep(delay - 0.10, delay, linear));
	if (t <= 0.0)
		return TRUE;

	/* World units are pixels with +Y up: falling is negative y. */
	offset[0] = (gfloat)(dx * 0.35 * t);
	offset[1] = (gfloat)(-dy * 0.15 * t - 0.95 * h * t * t);
	offset[2] = (gfloat)((80.0 + 280.0 * s->r[1]) * t);

	axis[0] = s->r[1] - 0.5f;
	axis[1] = s->r[2] - 0.5f;
	axis[2] = 0.35f;
	len = sqrt(axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2]);
	axis[0] /= (gfloat)len;
	axis[1] /= (gfloat)len;
	axis[2] /= (gfloat)len;
	*angle = (gfloat)(t * (1.5 + 4.0 * s->r[3]) * G_PI);

	*shade = (gfloat)(1.0 - 0.35 * t);
	*alpha = (gfloat)(1.0 - smoothstep(0.7, 1.0, t));
	return TRUE;
}

static gboolean
shatter_draw(GowlFxPass *pass, GowlFxTransitionRun *run, gdouble eased,
             gdouble linear)
{
	ShatterState *st = run->state;

	return gowl_fx_transition_draw_shards(pass, run, st->shards, st->n,
	                                      shatter_place, NULL, eased,
	                                      linear, 1.0f,
	                                      (gfloat)(0.55 + 0.45 * eased));
}

static const GowlFxTransitionKind shatter_kind = {
	.name           = "shatter",
	.label          = "Shatter",
	.duration_scale = 1.8,
	.setup          = shatter_setup,
	.teardown       = shatter_teardown,
	.draw           = shatter_draw,
};

GOWL_FX_DEFINE_TRANSITION_MODULE(GowlModuleShatter, gowl_module_shatter,
                                 &shatter_kind, "0.1.0",
                                 "Shatters the desktop like glass onto the next tag")
