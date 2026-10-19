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
 * Slide: the desktop is pushed off sideways and the next one follows it
 * in, towards the left for a higher tag and the right for a lower one --
 * the direction a row of workspaces would move.
 *
 * Mid-flight both desktops lift a little off the screen (a few percent
 * smaller, with a dark gap between them) and the one leaving dims, which
 * is what makes two flat pictures read as two places rather than one
 * long image scrolling.  Every bit of that is scaled by an envelope that
 * is zero at both ends, so the first and last frames are the desktops
 * themselves, corner to corner.
 */

#undef G_LOG_DOMAIN
#define G_LOG_DOMAIN "gowl-slide"

#include "fx/gowl-fx-transition-module.h"

#include <math.h>

/* How far the desktops shrink at the middle of the slide. */
#define SLIDE_LIFT 0.06
/* The gap between them at the middle, as a share of the width. */
#define SLIDE_GAP  0.03
/* How dark the desktop leaving gets by the time it is gone. */
#define SLIDE_DIM  0.35

/* A desktop-sized rectangle, its left edge at @left, scaled by @scale
 * about its own centre, in strip order: TL, BL, TR, BR. */
static void
desktop_rect(gfloat *pos, gdouble left, gdouble w, gdouble h, gdouble scale)
{
	gdouble cx = left + w * 0.5;
	gdouble cy = h * 0.5;
	gfloat x0 = (gfloat)(cx - w * 0.5 * scale);
	gfloat x1 = (gfloat)(cx + w * 0.5 * scale);
	gfloat y0 = (gfloat)(cy - h * 0.5 * scale);
	gfloat y1 = (gfloat)(cy + h * 0.5 * scale);

	pos[0] = x0;  pos[1]  = y0;  pos[2]  = 0.0f;
	pos[3] = x0;  pos[4]  = y1;  pos[5]  = 0.0f;
	pos[6] = x1;  pos[7]  = y0;  pos[8]  = 0.0f;
	pos[9] = x1;  pos[10] = y1;  pos[11] = 0.0f;
}

static gboolean
slide_draw(GowlFxPass *pass, GowlFxTransitionRun *run, gdouble eased,
           gdouble linear)
{
	static const gfloat stage[4] = { 0.035f, 0.038f, 0.055f, 1.0f };
	gdouble w = (gdouble)run->width;
	gdouble h = (gdouble)run->height;
	/* sin(pi t)^2: zero AND flat at both ends, so the lift eases in and
	 * out rather than starting with a jolt. */
	gdouble env = pow(sin(G_PI * linear), 2.0);
	gdouble scale = 1.0 - SLIDE_LIFT * env;
	gdouble span = w * (1.0 + SLIDE_GAP * env);
	gdouble off = eased * span * (gdouble)run->dir;
	gfloat mvp[16];
	gfloat pos[12];

	gowl_fx_mat4_ortho(mvp, w, h);
	gowl_fx_pass_clear(pass, stage);

	/* The one leaving, pushed the way the journey goes... */
	desktop_rect(pos, -off, w, h, scale);
	gowl_fx_transition_pass_desktop(pass, &run->from, mvp, pos, 1.0f,
	                                (gfloat)(1.0 - SLIDE_DIM * eased));

	/* ...and the one arriving, a span behind it. */
	desktop_rect(pos, (gdouble)run->dir * span - off, w, h, scale);
	gowl_fx_transition_pass_desktop(pass, &run->to, mvp, pos, 1.0f,
	                                (gfloat)(1.0 - SLIDE_DIM * (1.0 - eased)));
	return TRUE;
}

static const GowlFxTransitionKind slide_kind = {
	.name           = "slide",
	.label          = "Slide",
	.duration_scale = 1.0,
	.draw           = slide_draw,
};

GOWL_FX_DEFINE_TRANSITION_MODULE(GowlModuleSlide, gowl_module_slide,
                                 &slide_kind, "0.1.0",
                                 "Slides the desktop sideways to the next tag")
