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
 * Pixelate: the desktop dissolves into a mosaic, the mosaic changes, and
 * it resolves into the next desktop.  One shader
 * (GOWL_FX_BLEND_PIXELATE in src/fx/gowl-fx-blend.c).  The biggest block
 * is a fixed share of the output's height, so the effect looks the same
 * on a laptop panel and a 4K monitor.
 */

#undef G_LOG_DOMAIN
#define G_LOG_DOMAIN "gowl-pixelate"

#include "fx/gowl-fx-transition-module.h"

static gboolean
pixelate_draw(GowlFxPass *pass, GowlFxTransitionRun *run, gdouble eased,
              gdouble linear)
{
	GowlFxBlendParams p;

	(void)linear;
	gowl_fx_blend_params_init(&p);
	p.kind     = GOWL_FX_BLEND_PIXELATE;
	p.from     = run->from.tex;
	p.to       = run->to.tex;
	p.progress = (gfloat)eased;
	/* 56 px at 1080 rows: blocks scale with the output. */
	p.strength = run->height > 0 ? (gfloat)run->height / 1080.0f : 1.0f;
	return gowl_fx_pass_blend(pass, &p);
}

static const GowlFxTransitionKind pixelate_kind = {
	.name           = "pixelate",
	.label          = "Pixelate",
	.duration_scale = 1.1,
	.draw           = pixelate_draw,
};

GOWL_FX_DEFINE_TRANSITION_MODULE(GowlModulePixelate, gowl_module_pixelate,
                                 &pixelate_kind, "0.1.0",
                                 "Dissolves through a mosaic to the next tag")
