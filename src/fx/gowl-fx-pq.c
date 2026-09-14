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
 * gowl-fx-pq.c -- the desktop, encoded for an HDR output.
 *
 * THE ONE THING WLROOTS DOES NOT DO FOR US, and the reason HDR looked
 * broken rather than absent.
 *
 * An SDR pixel value is RELATIVE: 1.0 means "whatever this display's
 * maximum is", and the display decides what that is worth in real light.
 * A PQ pixel value is ABSOLUTE: it names a brightness in candelas per
 * square metre, and 1.0 means TEN THOUSAND of them.  So committing an
 * image description does not merely put the panel into a mode -- it
 * changes what every number already in the framebuffer MEANS.
 *
 * Nothing then rewrites those numbers.  wlroots does that conversion in
 * the renderer, gated on wlr_renderer.features.output_color_transform,
 * and implements it in its Vulkan renderer alone; the GLES2 renderer
 * gowl needs for every one of its effects has four fragment shaders and
 * all of them are a texture fetch times an alpha.  So sRGB white went to
 * the panel as a request for 10,000 cd/m2 where it should have asked for
 * 203, the backlight ran flat out, and the only correctly scaled window
 * on the screen was the one application that honoured the colour
 * management protocol and encoded itself properly -- which is what got
 * reported as "Electron goes dark in HDR".
 *
 * This is that conversion, done in gowl's own pass over the finished
 * scene.  Four steps and no cleverness:
 *
 *   1. sRGB EOTF: the code values the scene composited in are not light,
 *      they are an encoding of it.  Undo it.
 *   2. BT.709 to BT.2020 primaries.  The signal says BT.2020, so a red
 *      that was sRGB's red has to be re-stated in the wider space or
 *      everything comes out oversaturated by a third.
 *   3. Scale so SDR diffuse white lands on the reference white an HDR
 *      signal uses -- 203 cd/m2, from ITU-R BT.2408.  This is the step
 *      that decides whether the panel idles or runs at its peak.
 *   4. The inverse PQ EOTF (SMPTE ST 2084) to get back to code values.
 *
 * WHAT IT IS NOT.  It is an OUTPUT transform: it takes a desktop that is
 * entirely SDR and states it correctly in an HDR signal.  It does not
 * give a client a way to hand over PQ content of its own -- that is an
 * INPUT transform, it would have to happen per surface before the scene
 * composites, and gowl has no hook there.  A player wanting to deliver
 * real HDR still needs a renderer that colour-manages.
 */

#undef G_LOG_DOMAIN
#define G_LOG_DOMAIN "gowl-fx"

#include "gowl-fx-private.h"

#include <math.h>
#include <string.h>

static const gchar pq_vert_src[] =
	"attribute vec2 a_pos;\n"
	"attribute vec2 a_uv;\n"
	"varying vec2 v_uv;\n"
	"void main() {\n"
	"  v_uv = a_uv;\n"
	"  gl_Position = vec4(a_pos, 0.0, 1.0);\n"
	"}\n";

/*
 * highp is required rather than preferred here, and unusually it is
 * worth failing over.
 *
 * The PQ curve raises to the power of 78.84, and mediump is ten bits of
 * mantissa: the error in the exponent lands directly on the luminance,
 * and at the top of the curve a thousandth of a code value is hundreds
 * of candelas.  A shader that cannot have highp would produce a picture
 * worse than the uncorrected one, so it declines instead and the caller
 * falls back to passing the scene through.
 */
static const gchar pq_frag_src[] =
	"#ifdef GL_FRAGMENT_PRECISION_HIGH\n"
	"precision highp float;\n"
	"#else\n"
	"#error the PQ encode needs highp\n"
	"#endif\n"
	"\n"
	"uniform sampler2D u_src;\n"
	"uniform float u_white;      /* SDR diffuse white, cd/m2 */\n"
	"uniform float u_peak;       /* the panel's own peak, cd/m2 */\n"
	"varying vec2 v_uv;\n"
	"\n"
	"/* SMPTE ST 2084, written as the standard writes it. */\n"
	"const float PQ_M1 = 0.1593017578125;\n"
	"const float PQ_M2 = 78.84375;\n"
	"const float PQ_C1 = 0.8359375;\n"
	"const float PQ_C2 = 18.8515625;\n"
	"const float PQ_C3 = 18.6875;\n"
	"\n"
	"/* The sRGB EOTF: what the numbers in the framebuffer are an\n"
	" * encoding OF.  The linear segment near black is not a rounding of\n"
	" * the power curve, it is part of the definition. */\n"
	"vec3 srgb_to_linear(vec3 c) {\n"
	"  return mix(c / 12.92,\n"
	"             pow((c + vec3(0.055)) / 1.055, vec3(2.4)),\n"
	"             step(vec3(0.04045), c));\n"
	"}\n"
	"\n"
	"/*\n"
	" * BT.709 to BT.2020, through XYZ, folded into one matrix.\n"
	" *\n"
	" * Skipping it does not merely shift the hues: BT.2020's primaries\n"
	" * are further out, so leaving sRGB's numbers alone in that space\n"
	" * stretches every colour towards the edge of a much larger gamut.\n"
	" * A plain red desktop accent comes out luminous.\n"
	" */\n"
	"vec3 bt709_to_bt2020(vec3 c) {\n"
	"  return vec3(\n"
	"    dot(c, vec3(0.6274039, 0.3292830, 0.0433131)),\n"
	"    dot(c, vec3(0.0690973, 0.9195404, 0.0113623)),\n"
	"    dot(c, vec3(0.0163914, 0.0880133, 0.8955953)));\n"
	"}\n"
	"\n"
	"/* The inverse PQ EOTF: absolute luminance, normalised to the\n"
	" * standard's 10,000 cd/m2 ceiling, back to a code value. */\n"
	"vec3 pq_encode(vec3 y) {\n"
	"  vec3 p = pow(max(y, vec3(0.0)), vec3(PQ_M1));\n"
	"  return pow((vec3(PQ_C1) + PQ_C2 * p) / (vec3(1.0) + PQ_C3 * p),\n"
	"             vec3(PQ_M2));\n"
	"}\n"
	"\n"
	"void main() {\n"
	"  vec3 lin = srgb_to_linear(clamp(texture2D(u_src, v_uv).rgb,\n"
	"                                  0.0, 1.0));\n"
	"\n"
	"  lin = bt709_to_bt2020(lin);\n"
	"\n"
	"  /*\n"
	"   * Absolute now, and this is the whole point: white becomes 203\n"
	"   * candelas rather than a request for ten thousand.  Clamped to\n"
	"   * what the panel actually reaches, because asking for more than\n"
	"   * that is how the backlight ends up pinned -- and the EDID told\n"
	"   * us what it reaches.\n"
	"   */\n"
	"  lin = clamp(lin * u_white, 0.0, u_peak) / 10000.0;\n"
	"\n"
	"  gl_FragColor = vec4(pq_encode(lin), 1.0);\n"
	"}\n";

/* ── The pass ────────────────────────────────────────────────────── */

static gboolean
pq_prog_ensure(GowlFxGl *self)
{
	GowlFxPqProg *p = &self->pq;

	if (p->program != 0)
		return TRUE;
	if (self->pq_tried)
		return FALSE;
	self->pq_tried = TRUE;

	p->program = gowl_fx_link_program(pq_vert_src, pq_frag_src);
	if (p->program == 0) {
		g_warning("fx: the PQ encode shader would not build, so HDR "
		          "outputs will show SDR content uncorrected");
		return FALSE;
	}

	p->u_src   = glGetUniformLocation(p->program, "u_src");
	p->u_white = glGetUniformLocation(p->program, "u_white");
	p->u_peak  = glGetUniformLocation(p->program, "u_peak");
	p->a_pos   = glGetAttribLocation(p->program, "a_pos");
	p->a_uv    = glGetAttribLocation(p->program, "a_uv");
	return TRUE;
}

gboolean
gowl_fx_pass_pq(GowlFxPass          *pass,
                const GowlFxTexture *scene,
                gdouble              sdr_white,
                gdouble              peak)
{
	GowlFxGl           *gl;
	const GowlFxPqProg *p;

	if (pass == NULL || scene == NULL || scene->tex == 0)
		return FALSE;

	gl = pass->gl;
	if (!pq_prog_ensure(gl))
		return FALSE;
	p = &gl->pq;

	if (!(sdr_white > 0.0))
		sdr_white = GOWL_FX_PQ_SDR_WHITE;
	if (!(peak > 0.0))
		peak = 1000.0;
	/*
	 * A panel that cannot reach diffuse white gets a dimmer white, not a
	 * raised ceiling.
	 *
	 * Raising the ceiling to meet the reference -- which is what this did
	 * first -- asks a 100 cd/m2 panel for 203 and has it clip, which is
	 * the exact failure the ceiling exists to prevent.  You cannot have
	 * 203 candelas of white on a panel that makes 100; what you can have
	 * is 100, correctly encoded.
	 */
	if (sdr_white > peak)
		sdr_white = peak;

	glUseProgram(p->program);
	glUniform1f(p->u_white, (gfloat)sdr_white);
	glUniform1f(p->u_peak, (gfloat)peak);

	/* Straight replacement, not a composite: this is an encoding of what
	 * is already there and blending it over anything is meaningless. */
	glDisable(GL_BLEND);

	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, scene->tex);
	glUniform1i(p->u_src, 0);

	gowl_fx_draw_screen_quad(p->a_pos, p->a_uv);

	glBindTexture(GL_TEXTURE_2D, 0);
	glEnable(GL_BLEND);
	glUseProgram(0);
	return TRUE;
}
