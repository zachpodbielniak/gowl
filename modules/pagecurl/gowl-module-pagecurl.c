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
 * Page curl: the desktop is turned like a page of a book -- from the
 * right edge for a higher tag, from the left for a lower one -- its back
 * showing pale as it comes over and the next desktop underneath in its
 * shadow.  The geometry is one shader (GOWL_FX_BLEND_PAGECURL in
 * src/fx/gowl-fx-blend.c); slower than the others, because a page turned
 * quickly just looks like a wipe.
 */

#undef G_LOG_DOMAIN
#define G_LOG_DOMAIN "gowl-pagecurl"

#include "fx/gowl-fx-transition-module.h"

static gboolean
pagecurl_draw(GowlFxPass *pass, GowlFxTransitionRun *run, gdouble eased,
              gdouble linear)
{
	GowlFxBlendParams p;

	(void)linear;
	gowl_fx_blend_params_init(&p);
	p.kind     = GOWL_FX_BLEND_PAGECURL;
	p.from     = run->from.tex;
	p.to       = run->to.tex;
	p.progress = (gfloat)eased;
	p.dir      = (gfloat)run->dir;
	return gowl_fx_pass_blend(pass, &p);
}

static const GowlFxTransitionKind pagecurl_kind = {
	.name           = "pagecurl",
	.label          = "Page curl",
	.duration_scale = 1.45,
	.draw           = pagecurl_draw,
};

GOWL_FX_DEFINE_TRANSITION_MODULE(GowlModulePagecurl, gowl_module_pagecurl,
                                 &pagecurl_kind, "0.1.0",
                                 "Turns the desktop like a page to the next tag")
