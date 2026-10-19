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
 * Burn: the desktop catches fire where the pointer is and burns away
 * along a glowing, ragged edge, the next desktop showing through the
 * ash -- the transition that goes with the embers backdrop.  The fire is
 * one shader (GOWL_FX_BLEND_BURN in src/fx/gowl-fx-blend.c); the seed
 * makes every switch burn a different shape.
 */

#undef G_LOG_DOMAIN
#define G_LOG_DOMAIN "gowl-burn"

#include "fx/gowl-fx-transition-module.h"

static gboolean
burn_draw(GowlFxPass *pass, GowlFxTransitionRun *run, gdouble eased,
          gdouble linear)
{
	GowlFxBlendParams p;

	gowl_fx_blend_params_init(&p);
	p.kind      = GOWL_FX_BLEND_BURN;
	p.from      = run->from.tex;
	p.to        = run->to.tex;
	p.progress  = (gfloat)eased;
	p.center[0] = run->center[0];
	p.center[1] = run->center[1];
	p.time      = (gfloat)(linear * (gdouble)run->dur_us / G_USEC_PER_SEC);
	p.seed      = (gfloat)(run->seed % 1000u) / 997.0f;
	return gowl_fx_pass_blend(pass, &p);
}

static const GowlFxTransitionKind burn_kind = {
	.name           = "burn",
	.label          = "Burn",
	.duration_scale = 1.6,
	.draw           = burn_draw,
};

GOWL_FX_DEFINE_TRANSITION_MODULE(GowlModuleBurn, gowl_module_burn,
                                 &burn_kind, "0.1.0",
                                 "Burns the desktop away to the next tag")
