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
 * Zoom: the tags are a stack of desktops one behind the other.  Going to
 * a higher tag, the desktop you are on sinks away from you and fades,
 * and the next one arrives from just in front of the glass and settles
 * into place; going back, the one you are on rushes past you and the one
 * behind it comes forward.  Direction reads as depth instead of as left
 * and right, which is the difference from slide.
 */

#undef G_LOG_DOMAIN
#define G_LOG_DOMAIN "gowl-zoom"

#include "fx/gowl-fx-transition-module.h"

#include <math.h>

/* How far a desktop sinks (scale lost) and how close one starts
 * (scale gained) over the whole transition. */
#define ZOOM_AWAY  0.22
#define ZOOM_NEAR  0.14

static gdouble
smoothstep(gdouble a, gdouble b, gdouble x)
{
	gdouble t = CLAMP((x - a) / (b - a), 0.0, 1.0);

	return t * t * (3.0 - 2.0 * t);
}

static void
scaled_rect(gfloat *pos, gdouble w, gdouble h, gdouble scale)
{
	gfloat x0 = (gfloat)(w * 0.5 * (1.0 - scale));
	gfloat x1 = (gfloat)(w * 0.5 * (1.0 + scale));
	gfloat y0 = (gfloat)(h * 0.5 * (1.0 - scale));
	gfloat y1 = (gfloat)(h * 0.5 * (1.0 + scale));

	pos[0] = x0;  pos[1]  = y0;  pos[2]  = 0.0f;
	pos[3] = x0;  pos[4]  = y1;  pos[5]  = 0.0f;
	pos[6] = x1;  pos[7]  = y0;  pos[8]  = 0.0f;
	pos[9] = x1;  pos[10] = y1;  pos[11] = 0.0f;
}

static gboolean
zoom_draw(GowlFxPass *pass, GowlFxTransitionRun *run, gdouble eased,
          gdouble linear)
{
	static const gfloat stage[4] = { 0.025f, 0.027f, 0.04f, 1.0f };
	gdouble w = (gdouble)run->width;
	gdouble h = (gdouble)run->height;
	gdouble out_a = 1.0 - smoothstep(0.05, 0.75, eased);
	gdouble in_a = smoothstep(0.25, 0.95, eased);
	gdouble out_s, in_s;
	gfloat mvp[16];
	gfloat pos[12];

	(void)linear;
	if (run->dir > 0) {
		/* Further in: the old one sinks, the new one settles from
		 * in front. */
		out_s = 1.0 - ZOOM_AWAY * eased;
		in_s = 1.0 + ZOOM_NEAR * (1.0 - eased);
	} else {
		/* Back out: the old one rushes past, the new one comes up
		 * from behind. */
		out_s = 1.0 + ZOOM_NEAR * 2.0 * eased;
		in_s = 1.0 - ZOOM_AWAY * (1.0 - eased);
	}

	gowl_fx_mat4_ortho(mvp, w, h);
	gowl_fx_pass_clear(pass, stage);
	gowl_fx_pass_backdrop(pass, stage, (gfloat)sin(G_PI * eased));

	/* The one going further away is drawn first: no depth buffer, so
	 * back to front by hand. */
	if (run->dir > 0) {
		scaled_rect(pos, w, h, out_s);
		gowl_fx_transition_pass_desktop(pass, &run->from, mvp, pos,
		                                (gfloat)out_a, 1.0f);
		scaled_rect(pos, w, h, in_s);
		gowl_fx_transition_pass_desktop(pass, &run->to, mvp, pos,
		                                (gfloat)in_a, 1.0f);
	} else {
		scaled_rect(pos, w, h, in_s);
		gowl_fx_transition_pass_desktop(pass, &run->to, mvp, pos,
		                                (gfloat)in_a, 1.0f);
		scaled_rect(pos, w, h, out_s);
		gowl_fx_transition_pass_desktop(pass, &run->from, mvp, pos,
		                                (gfloat)out_a, 1.0f);
	}
	return TRUE;
}

static const GowlFxTransitionKind zoom_kind = {
	.name           = "zoom",
	.label          = "Zoom",
	.duration_scale = 1.0,
	.draw           = zoom_draw,
};

GOWL_FX_DEFINE_TRANSITION_MODULE(GowlModuleZoom, gowl_module_zoom,
                                 &zoom_kind, "0.1.0",
                                 "Zooms through a stack of desktops to the next tag")
