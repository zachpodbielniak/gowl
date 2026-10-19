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
 * Fade: one desktop dissolves into the next.  Nothing moves, which makes
 * it the transition for anyone who finds motion on every tag switch too
 * much, and the quickest of them.
 */

#undef G_LOG_DOMAIN
#define G_LOG_DOMAIN "gowl-fade"

#include "fx/gowl-fx-transition-module.h"

static gboolean
fade_draw(GowlFxPass *pass, GowlFxTransitionRun *run, gdouble eased,
          gdouble linear)
{
	GowlFxBlendParams p;

	(void)linear;
	gowl_fx_blend_params_init(&p);
	p.kind     = GOWL_FX_BLEND_FADE;
	p.from     = run->from.tex;
	p.to       = run->to.tex;
	p.progress = (gfloat)eased;
	return gowl_fx_pass_blend(pass, &p);
}

static const GowlFxTransitionKind fade_kind = {
	.name           = "fade",
	.label          = "Fade",
	/* Shorter than the others: a dissolve has no distance to cover,
	 * and a long one reads as the machine being slow. */
	.duration_scale = 0.8,
	.draw           = fade_draw,
};

GOWL_FX_DEFINE_TRANSITION_MODULE(GowlModuleFade, gowl_module_fade,
                                 &fade_kind, "0.1.0",
                                 "Crossfades from one tag to the next")
