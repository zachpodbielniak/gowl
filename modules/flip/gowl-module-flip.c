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
 * Flip: the desktop is a card and the next tag is printed on its back.
 * It turns half a revolution about its vertical axis -- right edge
 * coming towards you for a higher tag, the left for a lower one -- and
 * lands showing the other side.
 *
 * World units are output pixels, with the card centred on the origin and
 * +Y up; the camera stands exactly far enough back that the card fills
 * the view face-on, which is what makes the first and last frames the
 * desktops themselves.  While it turns the camera pulls back a little,
 * or the near edge would sweep past the screen and be clipped.  The
 * corners are rotated on the CPU and the face turned away is skipped
 * (gowl_fx_mat4_perspective()'s Y flip is why culling is not left to GL).
 */

#undef G_LOG_DOMAIN
#define G_LOG_DOMAIN "gowl-flip"

#include "fx/gowl-fx-transition-module.h"

#include <math.h>

#define FLIP_FOV_Y   (40.0 * G_PI / 180.0)
/* Extra camera distance at the edge-on moment, as a share of it. */
#define FLIP_PULL    0.55

/*
 * The card's four corners turned by @theta about +Y, in strip order
 * (TL, BL, TR, BR).  @back mirrors it left to right, which is how the
 * picture on the back reads the right way round once turned over.
 */
static void
card_corners(gfloat *pos, gdouble hw, gdouble hh, gdouble theta, gboolean back)
{
	const gdouble xs[4] = { -hw, -hw, hw, hw };
	const gdouble ys[4] = { hh, -hh, hh, -hh };
	gdouble c = cos(theta), s = sin(theta);
	gint i;

	for (i = 0; i < 4; i++) {
		gdouble x = back ? -xs[i] : xs[i];

		pos[i * 3 + 0] = (gfloat)(x * c);
		pos[i * 3 + 1] = (gfloat)ys[i];
		pos[i * 3 + 2] = (gfloat)(-x * s);
	}
}

static gboolean
flip_draw(GowlFxPass *pass, GowlFxTransitionRun *run, gdouble eased,
          gdouble linear)
{
	static const gfloat stage[4] = { 0.03f, 0.032f, 0.05f, 1.0f };
	gdouble w = (gdouble)run->width;
	gdouble h = (gdouble)run->height;
	gdouble hw = w * 0.5, hh = h * 0.5;
	gdouble theta = G_PI * eased * (gdouble)run->dir;
	gdouble env = sin(G_PI * linear);
	gdouble focal = 1.0 / tan(FLIP_FOV_Y * 0.5);
	gdouble dist = hh * focal * (1.0 + FLIP_PULL * env * env);
	gdouble facing = cos(theta);
	gfloat proj[16], view[16], mvp[16];
	gfloat pos[12];

	gowl_fx_mat4_perspective(proj, FLIP_FOV_Y, w / h,
	                         MAX(1.0, (dist - hw) * 0.5), dist + hw * 2.0);
	gowl_fx_mat4_view(view, dist, 0.0);
	gowl_fx_mat4_multiply(mvp, proj, view);

	gowl_fx_pass_clear(pass, stage);
	gowl_fx_pass_backdrop(pass, stage, (gfloat)env);

	/* Whichever side faces the camera; the other is behind it, and with
	 * no depth buffer drawing both would show the wrong one. */
	if (facing >= 0.0) {
		card_corners(pos, hw, hh, theta, FALSE);
		gowl_fx_transition_pass_desktop(pass, &run->from, mvp, pos, 1.0f,
		                                (gfloat)(0.45 + 0.55 * facing));
	} else {
		card_corners(pos, hw, hh, theta, TRUE);
		gowl_fx_transition_pass_desktop(pass, &run->to, mvp, pos, 1.0f,
		                                (gfloat)(0.45 - 0.55 * facing));
	}
	return TRUE;
}

static const GowlFxTransitionKind flip_kind = {
	.name           = "flip",
	.label          = "Flip",
	.duration_scale = 1.15,
	.draw           = flip_draw,
};

GOWL_FX_DEFINE_TRANSITION_MODULE(GowlModuleFlip, gowl_module_flip,
                                 &flip_kind, "0.1.0",
                                 "Turns the desktop over like a card to the next tag")
