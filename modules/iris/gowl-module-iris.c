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
 * Iris: the next desktop opens out of a circle centred on the pointer --
 * the place you were looking -- or on the middle of the output when the
 * pointer is on another one.  Going back to a lower tag the iris closes
 * instead, the desktop you are leaving shrinking into the pointer.  The
 * picture is one shader (GOWL_FX_BLEND_IRIS in src/fx/gowl-fx-blend.c).
 */

#undef G_LOG_DOMAIN
#define G_LOG_DOMAIN "gowl-iris"

#include "fx/gowl-fx-transition-module.h"

static gboolean
iris_draw(GowlFxPass *pass, GowlFxTransitionRun *run, gdouble eased,
          gdouble linear)
{
	GowlFxBlendParams p;

	(void)linear;
	gowl_fx_blend_params_init(&p);
	p.kind      = GOWL_FX_BLEND_IRIS;
	p.from      = run->from.tex;
	p.to        = run->to.tex;
	p.progress  = (gfloat)eased;
	p.center[0] = run->center[0];
	p.center[1] = run->center[1];
	p.dir       = (gfloat)run->dir;
	return gowl_fx_pass_blend(pass, &p);
}

static const GowlFxTransitionKind iris_kind = {
	.name           = "iris",
	.label          = "Iris",
	.duration_scale = 1.0,
	.draw           = iris_draw,
};

GOWL_FX_DEFINE_TRANSITION_MODULE(GowlModuleIris, gowl_module_iris,
                                 &iris_kind, "0.1.0",
                                 "Opens the next tag out of a circle at the pointer")
