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
 * Melt: the desktop runs down the screen in columns and the next one is
 * behind it -- the screen wipe from a certain 1993 shooter.  One shader
 * (GOWL_FX_BLEND_MELT in src/fx/gowl-fx-blend.c); it runs on its own
 * clock rather than the easing curve, because the columns already
 * accelerate as they fall and easing on top of that makes them hang.
 */

#undef G_LOG_DOMAIN
#define G_LOG_DOMAIN "gowl-melt"

#include "fx/gowl-fx-transition-module.h"

static gboolean
melt_draw(GowlFxPass *pass, GowlFxTransitionRun *run, gdouble eased,
          gdouble linear)
{
	GowlFxBlendParams p;

	(void)eased;
	gowl_fx_blend_params_init(&p);
	p.kind     = GOWL_FX_BLEND_MELT;
	p.from     = run->from.tex;
	p.to       = run->to.tex;
	p.progress = (gfloat)linear;
	p.seed     = (gfloat)(run->seed % 1000u) / 997.0f;
	return gowl_fx_pass_blend(pass, &p);
}

static const GowlFxTransitionKind melt_kind = {
	.name           = "melt",
	.label          = "Melt",
	.duration_scale = 1.7,
	.draw           = melt_draw,
};

GOWL_FX_DEFINE_TRANSITION_MODULE(GowlModuleMelt, gowl_module_melt,
                                 &melt_kind, "0.1.0",
                                 "Melts the desktop down the screen to the next tag")
