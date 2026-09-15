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
 * gowl-fx-bokeh.c -- the wallpaper thrown out of focus by a real lens.
 *
 * This is the blur module's other kernel, and the difference between the
 * two is the whole reason it exists.
 *
 * A GAUSSIAN BLUR IS NOT WHAT A LENS DOES.  An out-of-focus point of
 * light does not become a soft smudge that fades away from its middle:
 * it becomes the SHAPE OF THE APERTURE, evenly filled, with a hard edge.
 * That is the entire difference between a photograph's background and a
 * blurred screenshot, and three things follow from it:
 *
 *   1. THE KERNEL IS A DISC, or a polygon.  A camera stopped down has
 *      straight aperture blades, and every out-of-focus highlight in the
 *      frame is a little hexagon or heptagon with the same rotation.
 *      Wide open it is a circle.  `blades' picks; 0 is wide open.
 *
 *   2. THE MATH HAS TO BE DONE IN LIGHT, not in pixel values.  Display
 *      values are encoded with roughly a square root, so averaging them
 *      directly averages the square roots of the light -- which drags
 *      every bright thing down towards its neighbours and is the single
 *      biggest reason a naive blur looks flat and grey.  The samples
 *      here are squared on the way in and square-rooted on the way out,
 *      which is a gamma of 2.0: not exactly sRGB's 2.2, close enough
 *      that nobody could pick it out, and one multiply instead of a
 *      pow().
 *
 *   3. BRIGHT SPOTS TAKE OVER THEIR DISC.  In a real lens a highlight is
 *      often hundreds of times brighter than the wall behind it, so its
 *      disc is what you see and the wall's disc is not.  A wallpaper has
 *      no values above 1.0 to carry that, so the range has to be put
 *      back by hand: a sample's weight rises with how far its luminance
 *      is above `threshold'.  Without this the discs are there and
 *      nobody can see them, which is the failure mode that makes people
 *      conclude bokeh "does not work" on SDR images.
 *
 * And one more that is real optics and reads as expensive: a lens with
 * uncorrected spherical aberration puts MORE light at the rim of the
 * disc than in its middle -- "soap-bubble" bokeh, the look of old and of
 * deliberately imperfect glass.  `edge' is how much.
 *
 * WHAT IT COSTS.  One pass, once per output per tag switch, exactly
 * where the box blur was paid for.  Nothing here is per frame and
 * nothing here is per window: like the blur, the result is one
 * output-sized picture that every window crops out of.
 */

#undef G_LOG_DOMAIN
#define G_LOG_DOMAIN "gowl-fx"

#include "gowl-fx-private.h"

#include <math.h>
#include <string.h>

static const gchar bokeh_vert_src[] =
	"attribute vec2 a_pos;\n"
	"attribute vec2 a_uv;\n"
	"varying vec2 v_uv;\n"
	"void main() {\n"
	"  v_uv = a_uv;\n"
	"  gl_Position = vec4(a_pos, 0.0, 1.0);\n"
	"}\n";

/*
 * highp where it exists, for the usual reason: naming a precision the
 * driver does not have fails to COMPILE rather than falling back.  It
 * matters less here than in the animated shaders -- there is no clock
 * and no hash -- but the golden-angle accumulation runs to 64 terms and
 * mediump would visibly band the disc edge.
 */
static const gchar bokeh_frag_src[] =
	"#ifdef GL_FRAGMENT_PRECISION_HIGH\n"
	"precision highp float;\n"
	"#else\n"
	"precision mediump float;\n"
	"#endif\n"
	"\n"
	"uniform sampler2D u_tex;\n"
	"uniform vec2  u_texel;      /* one source pixel, in uv */\n"
	"uniform float u_radius;     /* the disc, in source pixels */\n"
	"uniform float u_blades;     /* aperture blades; 0 is wide open */\n"
	"uniform float u_rot;        /* how the aperture is turned, radians */\n"
	"uniform float u_highlight;  /* how much a bright sample dominates */\n"
	"uniform float u_threshold;  /* the luminance it has to beat */\n"
	"uniform float u_edge;       /* spherical aberration: a bright rim */\n"
	"uniform float u_taps;       /* how many samples, 8 to 64 */\n"
	"varying vec2 v_uv;\n"
	"\n"
	"#define MAX_TAPS 128\n"
	"\n"
	"/*\n"
	" * The aperture's radius at an angle, as a fraction of its widest.\n"
	" *\n"
	" * A regular n-gon with circumradius 1 has boundary radius\n"
	" * cos(pi/n)/cos(t) where t is the angle from the nearest vertex\n"
	" * bisector -- 1 at a vertex, cos(pi/n) at the middle of an edge.\n"
	" * Multiplying a disc sample by it maps the disc onto the polygon,\n"
	" * which is what an aperture actually is.\n"
	" */\n"
	"float aperture(float a) {\n"
	"  if (u_blades < 2.5)\n"
	"    return 1.0;\n"
	"  float n = u_blades;\n"
	"  float s = 6.28318530718 / n;\n"
	"  float t = mod(a + u_rot, s) - s * 0.5;\n"
	"  return cos(3.14159265359 / n) / max(cos(t), 1e-3);\n"
	"}\n"
	"\n"
	"/* A stable pseudo-random number from a position. */\n"
	"float hash1(vec2 c) {\n"
	"  return fract(sin(dot(c, vec2(127.1, 311.7))) * 43758.5453);\n"
	"}\n"
	"\n"
	"void main() {\n"
	"  vec3  sum  = vec3(0.0);\n"
	"  float wsum = 0.0;\n"
	"  float taps = clamp(u_taps, 8.0, float(MAX_TAPS));\n"
	"  float thr  = clamp(u_threshold, 0.0, 0.99);\n"
	"  /*\n"
	"   * THE WHOLE SPIRAL IS TURNED BY A PER-PIXEL ANGLE.\n"
	"   *\n"
	"   * Without it every pixel samples the SAME set of offsets, so a\n"
	"   * source with detail finer than the tap spacing does not blur --\n"
	"   * it beats against the sample pattern and comes out as a fixed\n"
	"   * woven texture.  A wallpaper full of fine feathers showed it\n"
	"   * plainly while the out-of-focus highlights beside it were\n"
	"   * perfect.\n"
	"   *\n"
	"   * Turning the set by a hashed angle converts that structure into\n"
	"   * noise, which is the standard trade and the right one: the eye\n"
	"   * forgives grain and does not forgive moire.  The APERTURE stays\n"
	"   * put, because its shape is a function of the absolute direction\n"
	"   * a sample went in -- so every highlight is still the same\n"
	"   * hexagon turned the same way.\n"
	"   */\n"
	"  float spin = hash1(floor(v_uv / max(u_texel, vec2(1e-6)))) * 6.28318530718;\n"
	"  int   i;\n"
	"\n"
	"  for (i = 0; i < MAX_TAPS; i++) {\n"
	"    float fi = float(i);\n"
	"    if (fi >= taps)\n"
	"      break;\n"
	"\n"
	"    /*\n"
	"     * A golden-angle spiral, which covers a disc about as evenly as\n"
	"     * anything can with no pattern to alias against.  sqrt() on the\n"
	"     * radius is what makes it even by AREA rather than crowding the\n"
	"     * middle -- the disc has to be uniformly filled, because that is\n"
	"     * the thing being drawn.\n"
	"     */\n"
	"    float a = (fi + 0.5) * 2.39996323 + spin;\n"
	"    float r = sqrt((fi + 0.5) / taps);\n"
	"    vec2  o = vec2(cos(a), sin(a)) * r * aperture(a) * u_radius;\n"
	"    vec3  c = texture2D(u_tex, v_uv + o * u_texel).rgb;\n"
	"\n"
	"    /* Into light.  See the note at the top: averaging display\n"
	"     * values averages the square roots of the light. */\n"
	"    c = c * c;\n"
	"\n"
	"    float lum = dot(c, vec3(0.2126, 0.7152, 0.0722));\n"
	"    float h   = max(lum - thr, 0.0) / max(1.0 - thr, 1e-3);\n"
	"    float w   = 1.0 + u_highlight * h * h;\n"
	"\n"
	"    /* The rim is brighter than the middle in a lens that was not\n"
	"     * corrected for it, which is most interesting glass. */\n"
	"    w *= 1.0 + u_edge * smoothstep(0.55, 1.0, r);\n"
	"\n"
	"    sum  += c * w;\n"
	"    wsum += w;\n"
	"  }\n"
	"\n"
	"  vec3 col = wsum > 0.0 ? sum / wsum : texture2D(u_tex, v_uv).rgb;\n"
	"  gl_FragColor = vec4(sqrt(max(col, 0.0)), 1.0);\n"
	"}\n";

static gboolean
bokeh_prog_ensure(GowlFxGl *self)
{
	GowlFxBokehProg *p = &self->bokeh;

	if (p->program != 0)
		return TRUE;
	if (self->bokeh_tried)
		return FALSE;
	self->bokeh_tried = TRUE;

	p->program = gowl_fx_link_program(bokeh_vert_src, bokeh_frag_src);
	if (p->program == 0) {
		g_warning("fx: the bokeh shader would not build, so bokeh "
		          "backdrops will fall back to the box blur");
		return FALSE;
	}

	p->u_tex       = glGetUniformLocation(p->program, "u_tex");
	p->u_texel     = glGetUniformLocation(p->program, "u_texel");
	p->u_radius    = glGetUniformLocation(p->program, "u_radius");
	p->u_blades    = glGetUniformLocation(p->program, "u_blades");
	p->u_rot       = glGetUniformLocation(p->program, "u_rot");
	p->u_highlight = glGetUniformLocation(p->program, "u_highlight");
	p->u_threshold = glGetUniformLocation(p->program, "u_threshold");
	p->u_edge      = glGetUniformLocation(p->program, "u_edge");
	p->u_taps      = glGetUniformLocation(p->program, "u_taps");
	p->a_pos       = glGetAttribLocation(p->program, "a_pos");
	p->a_uv        = glGetAttribLocation(p->program, "a_uv");
	return TRUE;
}

void
gowl_fx_bokeh_params_init(GowlFxBokehParams *params)
{
	if (params == NULL)
		return;

	memset(params, 0, sizeof(*params));
	params->radius     = 34.0f;
	params->downscale  = 2;
	params->samples    = 128;
	params->blades     = 6;
	params->rotation   = 12.0f;
	params->highlight  = 5.0f;
	params->threshold  = 0.55f;
	params->edge       = 0.22f;
}

gboolean
gowl_fx_texture_bokeh(
	GowlFxGl                *self,
	GowlFxTexture           *dst,
	const GowlFxTexture     *src,
	const GowlFxBokehParams *params
){
	GowlFxEglSave          save;
	const GowlFxBokehProg *p;
	GowlFxBokehParams      fallback;
	gint                   small_w, small_h, down;
	gfloat                 radius;
	gboolean               ok;

	if (self == NULL || dst == NULL || src == NULL || src->tex == 0)
		return FALSE;
	if (src->width <= 0 || src->height <= 0)
		return FALSE;

	if (params == NULL) {
		gowl_fx_bokeh_params_init(&fallback);
		params = &fallback;
	}

	if (!gowl_fx_egl_enter(self, &save))
		return FALSE;
	if (!bokeh_prog_ensure(self)) {
		gowl_fx_egl_leave(&save);
		return FALSE;
	}
	p = &self->bokeh;

	/*
	 * Worked at a reduced size, like the box blur and for the same
	 * reason -- but the trade is different and worth naming.  A box blur
	 * shrinks because the SOFTNESS is free that way.  This shrinks only
	 * to afford the taps, and it pays for it: a one-pixel highlight
	 * averaged into a 2x2 block before the disc is applied has lost
	 * three quarters of its excess before the weighting can favour it.
	 * Two is the compromise; `bokeh-downscale: 1` is there for anybody
	 * who would rather spend the milliseconds, and the cost is paid once
	 * per tag switch either way.
	 */
	down    = CLAMP(params->downscale, 1, 4);
	small_w = MAX(1, src->width / down);
	small_h = MAX(1, src->height / down);
	radius  = MAX(0.0f, params->radius) / (gfloat)down;

	/* The size fields are alloc's to write, not ours: see the note in
	 * gowl_fx_texture_blur().  Setting them here makes it keep a
	 * texture of the WRONG size, which the box blur next door leaves
	 * behind at whatever `blur-downscale' it last used. */
	ok = gowl_fx_texture_alloc(&self->scratch_a, small_w, small_h);

	if (ok) {
		glBindFramebuffer(GL_FRAMEBUFFER, self->scratch_fbo);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
		                       GL_TEXTURE_2D, self->scratch_a.tex, 0);
		if (glCheckFramebufferStatus(GL_FRAMEBUFFER)
		    == GL_FRAMEBUFFER_COMPLETE) {
			glViewport(0, 0, small_w, small_h);
			glDisable(GL_BLEND);
			glUseProgram(p->program);
			glUniform2f(p->u_texel, 1.0f / (gfloat)src->width * (gfloat)down,
			            1.0f / (gfloat)src->height * (gfloat)down);
			glUniform1f(p->u_radius, radius);
			glUniform1f(p->u_blades,
			            params->blades >= 3 ? (gfloat)MIN(params->blades, 12)
			                                : 0.0f);
			glUniform1f(p->u_rot,
			            (gfloat)(params->rotation * G_PI / 180.0));
			glUniform1f(p->u_highlight, CLAMP(params->highlight, 0.0f, 32.0f));
			glUniform1f(p->u_threshold, CLAMP(params->threshold, 0.0f, 0.99f));
			glUniform1f(p->u_edge, CLAMP(params->edge, 0.0f, 2.0f));
			glUniform1f(p->u_taps, (gfloat)CLAMP(params->samples, 8, 128));
			glActiveTexture(GL_TEXTURE0);
			glBindTexture(GL_TEXTURE_2D, src->tex);
			glUniform1i(p->u_tex, 0);
			gowl_fx_draw_screen_quad(p->a_pos, p->a_uv);
			glBindTexture(GL_TEXTURE_2D, 0);
			glUseProgram(0);
		} else {
			ok = FALSE;
		}
	}

	/* Back up to the output's size, so the caller's crop arithmetic is
	 * the same for this as it is for the box blur. */
	if (ok && gowl_fx_texture_alloc(dst, src->width, src->height)) {
		glBindFramebuffer(GL_FRAMEBUFFER, self->scratch_fbo);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
		                       GL_TEXTURE_2D, dst->tex, 0);
		if (glCheckFramebufferStatus(GL_FRAMEBUFFER)
		    == GL_FRAMEBUFFER_COMPLETE) {
			glViewport(0, 0, dst->width, dst->height);
			glDisable(GL_BLEND);
			glUseProgram(self->copy_2d.program);
			glActiveTexture(GL_TEXTURE0);
			glBindTexture(GL_TEXTURE_2D, self->scratch_a.tex);
			glUniform1i(self->copy_2d.u_tex, 0);
			gowl_fx_draw_screen_quad(self->copy_2d.a_pos,
			                         self->copy_2d.a_uv);
			glBindTexture(GL_TEXTURE_2D, 0);
			glUseProgram(0);
		} else {
			ok = FALSE;
		}
	} else {
		ok = FALSE;
	}

	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
	                       GL_TEXTURE_2D, 0, 0);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	gowl_fx_egl_leave(&save);
	return ok;
}
