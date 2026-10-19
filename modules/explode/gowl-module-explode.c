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
 * Explode: the desktop blows apart from the pointer.  Every piece flies
 * outward and towards you at once -- fast, then slowing, as debris does
 * -- spinning and fading, while the next desktop behind swells from a
 * little way back into place as though the blast had pushed it there.
 *
 * Where shatter falls (gravity, a wave spreading from the impact), this
 * is one instant: the pieces near the pointer leave a hair sooner, and
 * that is all the order there is.  Built on the shared shards
 * (gowl_fx_transition_draw_shards()).
 */

#undef G_LOG_DOMAIN
#define G_LOG_DOMAIN "gowl-explode"

#include "fx/gowl-fx-transition-module.h"

#include <math.h>

typedef struct {
	GowlFxShard *shards;
	gint         n;
} ExplodeState;

static gboolean
explode_setup(GowlFxTransitionRun *run)
{
	ExplodeState *st = g_new0(ExplodeState, 1);
	gint rows = 13;
	gint cols = MAX(4, (gint)lround(rows * (gdouble)run->width
	                                       / MAX(run->height, 1)));

	st->shards = gowl_fx_shards_new(run->width, run->height, cols, rows,
	                                run->seed, &st->n);
	run->state = st;
	return st->n > 0;
}

static void
explode_teardown(GowlFxTransitionRun *run)
{
	ExplodeState *st = run->state;

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
explode_place(const GowlFxShard *s, gint index,
              const GowlFxTransitionRun *run, gdouble eased, gdouble linear,
              gfloat *offset, gfloat *axis, gfloat *angle, gfloat *scale,
              gfloat *shade, gfloat *alpha, gpointer user_data)
{
	gdouble w = (gdouble)run->width, h = (gdouble)run->height;
	gdouble px = run->center[0] * w, py = run->center[1] * h;
	gdouble dx = s->cx - px, dy = s->cy - py;
	gdouble d = sqrt(dx * dx + dy * dy);
	gdouble far = sqrt(w * w + h * h);
	gdouble delay = 0.08 * d / far;
	gdouble t = CLAMP((linear - delay) / (1.0 - delay), 0.0, 1.0);
	/* Debris: fast out of the blast, slowing as it goes. */
	gdouble go = 1.0 - pow(1.0 - t, 2.0);
	gdouble ux = d > 0.5 ? dx / d : s->r[0] - 0.5;
	gdouble uy = d > 0.5 ? dy / d : s->r[1] - 0.5;
	gdouble reach = (0.18 + 0.45 * s->r[1]) * w;
	gdouble len;

	(void)index;
	(void)eased;
	(void)user_data;
	if (t <= 0.0)
		return TRUE;

	/* Screen y runs down, the world's up. */
	offset[0] = (gfloat)(ux * reach * go);
	offset[1] = (gfloat)(-uy * reach * go);
	offset[2] = (gfloat)(h * (0.08 + 0.42 * s->r[2]) * go);

	axis[0] = s->r[2] - 0.5f;
	axis[1] = s->r[3] - 0.5f;
	axis[2] = s->r[0] - 0.5f;
	len = sqrt(axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2]);
	if (len < 1e-3) {
		axis[0] = 0.0f; axis[1] = 1.0f; axis[2] = 0.0f;
	} else {
		axis[0] /= (gfloat)len;
		axis[1] /= (gfloat)len;
		axis[2] /= (gfloat)len;
	}
	*angle = (gfloat)(go * (1.0 + 3.0 * s->r[3]) * G_PI);
	*shade = (gfloat)(1.0 - 0.3 * go);
	*alpha = (gfloat)(1.0 - smoothstep(0.5, 0.97, t));
	return TRUE;
}

static gboolean
explode_draw(GowlFxPass *pass, GowlFxTransitionRun *run, gdouble eased,
             gdouble linear)
{
	ExplodeState *st = run->state;

	return gowl_fx_transition_draw_shards(pass, run, st->shards, st->n,
	                                      explode_place, NULL, eased,
	                                      linear,
	                                      (gfloat)(0.9 + 0.1 * eased),
	                                      (gfloat)(0.5 + 0.5 * eased));
}

static const GowlFxTransitionKind explode_kind = {
	.name           = "explode",
	.label          = "Explode",
	.duration_scale = 1.5,
	.setup          = explode_setup,
	.teardown       = explode_teardown,
	.draw           = explode_draw,
};

GOWL_FX_DEFINE_TRANSITION_MODULE(GowlModuleExplode, gowl_module_explode,
                                 &explode_kind, "0.1.0",
                                 "Blows the desktop apart from the pointer to the next tag")
