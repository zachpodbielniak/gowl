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
 * Glitch: the signal breaks up -- torn slices, split colour, scanlines,
 * the odd block from the wrong place -- and comes back on the next tag.
 * One shader (GOWL_FX_BLEND_GLITCH in src/fx/gowl-fx-blend.c), quick by
 * design: a glitch that lingers stops being a glitch.
 */

#undef G_LOG_DOMAIN
#define G_LOG_DOMAIN "gowl-glitch"

#include "fx/gowl-fx-transition-module.h"

static gboolean
glitch_draw(GowlFxPass *pass, GowlFxTransitionRun *run, gdouble eased,
            gdouble linear)
{
	GowlFxBlendParams p;

	gowl_fx_blend_params_init(&p);
	p.kind     = GOWL_FX_BLEND_GLITCH;
	p.from     = run->from.tex;
	p.to       = run->to.tex;
	/* Linear on purpose: easing would spend the middle -- where the
	 * slices flip -- at speed, and the ends lingering clean. */
	p.progress = (gfloat)linear;
	p.time     = (gfloat)(linear * (gdouble)run->dur_us / G_USEC_PER_SEC);
	p.seed     = (gfloat)(run->seed % 1000u) / 997.0f;
	(void)eased;
	return gowl_fx_pass_blend(pass, &p);
}

static const GowlFxTransitionKind glitch_kind = {
	.name           = "glitch",
	.label          = "Glitch",
	.duration_scale = 0.75,
	.draw           = glitch_draw,
};

GOWL_FX_DEFINE_TRANSITION_MODULE(GowlModuleGlitch, gowl_module_glitch,
                                 &glitch_kind, "0.1.0",
                                 "Glitches through a broken signal to the next tag")
