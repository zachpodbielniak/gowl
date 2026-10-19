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
 * Carousel: the desktops stand round a large ring, facing out, a gap
 * between each, and the ring swings round to the next tag -- passing
 * every tag in between, which it shows (a multi-step kind: the host
 * captures each desktop crossed).
 *
 * Where the cube is one solid seen from outside with its sides touching,
 * this is separate panels on a turntable: the camera eases back mid-turn
 * so the neighbours come into view, and at both ends only the panel
 * face-on is drawn, filling the view exactly -- which is the seam.
 *
 * World units are output pixels, +Y up, the face-on panel at z = 0 and
 * the ring's axis behind it.  Panels are drawn furthest first, there
 * being no depth buffer, and a panel turned away from the camera is
 * skipped.
 */

#undef G_LOG_DOMAIN
#define G_LOG_DOMAIN "gowl-carousel"

#include "fx/gowl-fx-transition-module.h"

#include <math.h>

#define CAROUSEL_FOV_Y  (40.0 * G_PI / 180.0)
/* Angle between neighbouring panels on the ring. */
#define CAROUSEL_STEP   (34.0 * G_PI / 180.0)
/* Room between neighbours, as a share of a panel's width. */
#define CAROUSEL_GAP    0.16
/* How far the camera pulls back at the middle, as a share of its
 * face-on distance. */
#define CAROUSEL_PULL   1.35

typedef struct {
	gint    slot;
	gdouble phi;     /* the panel's angle to the camera */
} Panel;

static gint
panel_cmp(gconstpointer a, gconstpointer b)
{
	const Panel *x = a, *y = b;
	gdouble ax = fabs(x->phi), ay = fabs(y->phi);

	/* Most turned away first: it is the furthest from the camera. */
	return ax > ay ? -1 : (ax < ay ? 1 : 0);
}

static gboolean
carousel_draw(GowlFxPass *pass, GowlFxTransitionRun *run, gdouble eased,
              gdouble linear)
{
	static const gfloat stage[4] = { 0.03f, 0.032f, 0.05f, 1.0f };
	gdouble w = (gdouble)run->width;
	gdouble h = (gdouble)run->height;
	gdouble hw = w * 0.5, hh = h * 0.5;
	gdouble radius = hw * (1.0 + CAROUSEL_GAP) / sin(CAROUSEL_STEP * 0.5);
	gdouble env = pow(sin(G_PI * linear), 2.0);
	gdouble focal = 1.0 / tan(CAROUSEL_FOV_Y * 0.5);
	gdouble dist = hh * focal * (1.0 + CAROUSEL_PULL * env);
	/* Where along the journey the ring is, in panels. */
	gdouble k = eased * (gdouble)run->steps;
	Panel panels[16];
	gint n = 0, j;
	gfloat proj[16], view[16], mvp[16];

	gowl_fx_mat4_perspective(proj, CAROUSEL_FOV_Y, w / h,
	                         MAX(1.0, dist * 0.25), dist + radius * 2.5);
	gowl_fx_mat4_view(view, dist, 0.0);
	gowl_fx_mat4_multiply(mvp, proj, view);

	gowl_fx_pass_clear(pass, stage);
	gowl_fx_pass_backdrop(pass, stage, (gfloat)env);

	for (j = 0; j <= run->steps && n < (gint)G_N_ELEMENTS(panels); j++) {
		gdouble phi = (gdouble)run->dir * ((gdouble)j - k) * CAROUSEL_STEP;

		/* Facing away, or round the back of the ring. */
		if (fabs(phi) >= G_PI * 0.5)
			continue;
		panels[n].slot = j;
		panels[n].phi = phi;
		n++;
	}
	qsort(panels, (gsize)n, sizeof(Panel), panel_cmp);

	for (j = 0; j < n; j++) {
		gdouble phi = panels[j].phi;
		gdouble cx = radius * sin(phi);
		gdouble cz = radius * cos(phi) - radius;
		gdouble tx = cos(phi), tz = -sin(phi);
		gdouble near = 1.0 - CLAMP(fabs((gdouble)panels[j].slot - k), 0.0, 1.0);
		/* The panels being left and arrived at are always there; the
		 * rest come into view with the camera, so neither end shows
		 * anything but the desktop itself. */
		gdouble alpha = MAX(near, MIN(1.0, env * 2.5));
		gfloat pos[12];

		pos[0]  = (gfloat)(cx - tx * hw); pos[1]  = (gfloat)hh;
		pos[2]  = (gfloat)(cz - tz * hw);
		pos[3]  = (gfloat)(cx - tx * hw); pos[4]  = (gfloat)(-hh);
		pos[5]  = (gfloat)(cz - tz * hw);
		pos[6]  = (gfloat)(cx + tx * hw); pos[7]  = (gfloat)hh;
		pos[8]  = (gfloat)(cz + tz * hw);
		pos[9]  = (gfloat)(cx + tx * hw); pos[10] = (gfloat)(-hh);
		pos[11] = (gfloat)(cz + tz * hw);

		gowl_fx_transition_pass_desktop(pass,
			gowl_fx_transition_run_desktop(run, panels[j].slot),
			mvp, pos, (gfloat)alpha,
			(gfloat)(0.5 + 0.5 * cos(phi)));
	}
	return TRUE;
}

static const GowlFxTransitionKind carousel_kind = {
	.name           = "carousel",
	.label          = "Carousel",
	.duration_scale = 1.25,
	.step_scale     = 0.35,
	.multi_step     = TRUE,
	.draw           = carousel_draw,
};

GOWL_FX_DEFINE_TRANSITION_MODULE(GowlModuleCarousel, gowl_module_carousel,
                                 &carousel_kind, "0.1.0",
                                 "Swings the desktops round a ring to the next tag")
