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
 * gowl-fx-glass.c -- refraction through a bevelled slab, in one pass.
 *
 * WHAT THIS IS NOT.  It is not a blur with a highlight painted on.  The
 * rim's every feature -- how far the picture is pulled, where it darkens,
 * where the light sits, how wide the colour fringe is -- falls out of one
 * ray traced through one surface.  That is the whole reason it reads as
 * glass rather than as a gradient: a painted rim has to be re-tuned for
 * every size and still looks wrong at the corners, where a traced one
 * cannot.
 *
 * THE MODEL.  The window is a slab of glass `thickness' deep whose edge
 * is rounded off over `bevel' pixels.  A viewer's ray comes straight down,
 * meets that rounded edge at an angle, refracts towards the surface
 * normal (n = 1.5), and travels through the glass still under it before
 * it reaches the wallpaper.  Where it lands is not where it started, and
 * that offset -- inward, because the surface leans outward -- is the whole
 * effect.  The centre of the slab is flat, so the ray goes straight
 * through and nothing moves.
 *
 * The arithmetic follows hyalite (MIT, VII-Cae), which worked it out for
 * the web out of SVG displacement maps.  Three of its findings are worth
 * repeating here because each one is invisible in the source and obvious
 * on screen:
 *
 *   - The direction the picture is pulled comes from a WIDER rounded rect
 *     (radius + bevel), not from the window's own.  Taken from the
 *     window's own outline, the turn from "pull down" to "pull right"
 *     happens over a few pixels at each corner and the corner reads as a
 *     ridge.
 *
 *   - Geometry scales; optics does not.  The bevel and the displacement
 *     are proportions of the window.  The colour fringe, the bright line
 *     and the dark hairline under it are a fixed number of pixels --
 *     they are one or two pixels of real glass whatever the window's
 *     size.  Scaling those with the bevel turns a wide rim into grey mud.
 *
 *   - Past a decay slope of 1 the displacement FOLDS: the same wallpaper
 *     shows up twice along the rim.  That is not a bug to clamp away, it
 *     is where the liquid look comes from.  It is still a cliff -- the
 *     folded zone has unbounded stretch -- so it wants a narrow bevel and
 *     a frosted source to cover for it.
 *
 * WHY IT IS HERE AND NOT IN THE MODULE.  Everything in src/fx exists so
 * that GL lives in one place with one set of tests rather than in each
 * module that wants it (gowl-fx.h says so at length).  A module gets
 * #GowlFxGl as an opaque pointer; this is the entry point the liquid
 * glass module drives it through.
 */

#undef G_LOG_DOMAIN
#define G_LOG_DOMAIN "gowl-fx"

#include "gowl-fx-private.h"

#include <math.h>
#include <string.h>

/* Refractive index of ordinary glass.  Shared between the shader and the
 * host-side maximum below, which must agree or the colour fringe is
 * scaled against the wrong reference. */
#define GOWL_FX_GLASS_IOR (1.5)

/* How many points the host samples the profile at to find its maximum.
 * The profile is monotone for the circle and squircle bevels, so the
 * maximum is at the rim; `lip' is not, which is why this is a sweep
 * rather than one evaluation. */
#define GOWL_FX_GLASS_PROFILE_TAPS (96)

/*
 * The shader.
 *
 * Written against GLES2, so: no integer switches on uniforms worth the
 * trouble, no textureLod, and highp has to be asked for conditionally --
 * a fragment shader that names highp on an implementation without it
 * fails to COMPILE rather than falling back, which would take the whole
 * effect down on exactly the hardware least able to afford a second
 * attempt.  The SDF work genuinely needs the range: at mediump a
 * 2880-pixel-wide window's distance field quantises into visible steps
 * along the bevel.
 */
static const gchar glass_vert_src[] =
	"attribute vec2 a_pos;\n"
	"attribute vec2 a_uv;\n"
	"varying vec2 v_uv;\n"
	"void main() {\n"
	"  v_uv = a_uv;\n"
	"  gl_Position = vec4(a_pos, 0.0, 1.0);\n"
	"}\n";

static const gchar glass_frag_src[] =
	"#ifdef GL_FRAGMENT_PRECISION_HIGH\n"
	"precision highp float;\n"
	"#else\n"
	"precision mediump float;\n"
	"#endif\n"
	"\n"
	"uniform sampler2D u_soft;      /* the frosted wallpaper */\n"
	"uniform sampler2D u_sharp;     /* the same wallpaper, unblurred */\n"
	"uniform vec2  u_src_origin;    /* where this rect sits in them, px */\n"
	"uniform float u_src_scale;   /* source px per buffer px */\n"
	"uniform vec2  u_src_size;      /* their size, px */\n"
	"uniform vec2  u_size;          /* the rect, px */\n"
	"uniform float u_radius;        /* corner radius, px */\n"
	"uniform float u_bevel;         /* bent zone along the edge, px */\n"
	"uniform float u_thickness;     /* slab depth, px */\n"
	"uniform float u_slope;         /* displacement decay cap, px/px */\n"
	"uniform float u_maxd;          /* the largest displacement, px */\n"
	"uniform float u_shape;         /* 0 circle, 1 squircle, 2 lip */\n"
	"uniform float u_dispersion;    /* channel separation, px */\n"
	"uniform float u_rim;           /* light the edge sends back */\n"
	"uniform float u_shade;         /* how much the edge darkens */\n"
	"uniform float u_edge_w;        /* how far shading and sheen reach, px */\n"
	"uniform vec2  u_light;         /* light direction */\n"
	"uniform float u_sat;           /* saturation inside the ring */\n"
	"uniform float u_clarity;       /* how unfrosted the ring is, 0..1 */\n"
	"uniform float u_centre_clarity;/* ...and how unfrosted the middle is */\n"
	"uniform float u_lens;          /* whole-surface magnification, 0..1 */\n"
	"uniform float u_sheen;         /* highlight off the dome */\n"
	"uniform vec3  u_tint;\n"
	"uniform float u_brightness;\n"
	"uniform float u_alpha;\n"
	"varying vec2 v_uv;\n"
	"\n"
	"const float N_GLASS = 1.5;\n"
	"/* Ratios inside `shade' and `rim'.  They are not uniforms because the\n"
	" * BALANCE between them is what took the tuning, and two knobs that\n"
	" * have to move together are worse than one. */\n"
	"const float ABSORB = 1.0869565;   /* 0.50 / 0.46 */\n"
	"const float GLOW   = 0.2272727;   /* 0.40 / 1.76 */\n"
	"const float SHARP  = 44.0;        /* exponent of the tight specular line */\n"
	"const float LIP    = 0.3;         /* how far the lip profile dips */\n"
	"const float PI     = 3.14159265;\n"
	"\n"
	"/* Signed distance to a rounded rect centred in @size; negative inside. */\n"
	"float sdf_rect(vec2 p, vec2 size, float r) {\n"
	"  vec2 h = size * 0.5;\n"
	"  vec2 q = abs(p - h) - (h - vec2(r));\n"
	"  return min(max(q.x, q.y), 0.0) + length(max(q, vec2(0.0))) - r;\n"
	"}\n"
	"\n"
	"/* Bevel surface height.  t = 0 at the outer rim (lowest), 1 where it\n"
	" * meets the flat slab.  Only the slope matters for the tilt; the\n"
	" * height itself says how much glass is still above the ray. */\n"
	"float h_circle(float t) {\n"
	"  float u = 1.0 - t;\n"
	"  return sqrt(max(0.0, 1.0 - u * u));\n"
	"}\n"
	"float h_at(float t) {\n"
	"  float u = 1.0 - t;\n"
	"  if (u_shape < 0.5)\n"
	"    return h_circle(t);\n"
	"  if (u_shape < 1.5) {\n"
	"    /* The squircle, which meets the slab more gently than a quarter\n"
	"     * circle does -- Apple's profile. */\n"
	"    float u4 = u * u * u * u;\n"
	"    return pow(max(0.0, 1.0 - u4), 0.25);\n"
	"  }\n"
	"  /* A raised rim over a shallow dip.  The perturbation reaches zero in\n"
	"   * both value AND slope at each end, or the bevel does not meet the\n"
	"   * slab and the join shows as a crease. */\n"
	"  float s = sin(PI * t);\n"
	"  return h_circle(t) + LIP * s * s * cos(PI * t);\n"
	"}\n"
	"\n"
	"/* The surface tilt at depth @d, and the offset it buys.\n"
	" *\n"
	" * A vertical ray refracts towards the normal and then crosses the\n"
	" * glass still above it, so the offset is the remaining thickness times\n"
	" * tan(alpha - beta).  The decay cap is the closed form of hyalite's\n"
	" * inside-out integration: the displacement must reach zero at the\n"
	" * inner edge, and may not fall faster than `slope' on the way there. */\n"
	"vec2 glass_profile(float d) {\n"
	"  float x  = clamp(d / u_bevel, 0.0, 1.0);\n"
	"  float e  = 1e-3;\n"
	"  float x1 = min(1.0, x + e);\n"
	"  float x0 = max(0.0, x - e);\n"
	"  float alpha = atan((h_at(x1) - h_at(x0)) / max(x1 - x0, 1e-6));\n"
	"  float sa = sin(abs(alpha));\n"
	"  float beta = asin(min(1.0, sa / N_GLASS));\n"
	"  if (alpha < 0.0)\n"
	"    beta = -beta;\n"
	"  float raw = (u_thickness + u_bevel * h_circle(x)) * tan(alpha - beta);\n"
	"  return vec2(min(raw, u_slope * max(0.0, u_bevel - d)), alpha);\n"
	"}\n"
	"\n"
	"/* Fresnel reflectance, unpolarised average: 0.04 head-on, 1 at grazing. */\n"
	"float fresnel(float a) {\n"
	"  a = abs(a);\n"
	"  float st = sin(a) / N_GLASS;\n"
	"  if (st >= 1.0)\n"
	"    return 1.0;\n"
	"  if (a < 1e-4)\n"
	"    return 0.04;\n"
	"  float b  = asin(st);\n"
	"  float rs = sin(a - b) / sin(a + b);\n"
	"  float rp = tan(a - b) / tan(a + b);\n"
	"  return min(1.0, (rs * rs + rp * rp) * 0.5);\n"
	"}\n"
	"\n"
	"/* One tap of the wallpaper, frosted in the flat centre and clear along\n"
	" * the bevel: thick glass diffuses, a lens does not, and the ring is\n"
	" * where the lens is. */\n"
	"vec3 tap(vec2 px, float clear) {\n"
	"  vec2 uv = (u_src_origin + px * u_src_scale) / u_src_size;\n"
	"  uv = clamp(uv, vec2(0.0), vec2(1.0));\n"
	"  vec3 soft  = texture2D(u_soft,  uv).rgb;\n"
	"  vec3 sharp = texture2D(u_sharp, uv).rgb;\n"
	"  return mix(soft, sharp, clear);\n"
	"}\n"
	"\n"
	"void main() {\n"
	"  vec2  p    = v_uv * u_size;\n"
	"  float sd   = sdf_rect(p, u_size, u_radius);\n"
	"  float cov  = clamp(0.5 - sd, 0.0, 1.0);\n"
	"  if (cov <= 0.0) {\n"
	"    gl_FragColor = vec4(0.0);\n"
	"    return;\n"
	"  }\n"
	"\n"
	"  float d  = max(0.0, -sd);\n"
	"  vec2  pr = glass_profile(d);\n"
	"  float m  = pr.x;\n"
	"  float alpha = pr.y;\n"
	"\n"
	"  /* Direction from a WIDER rounded rect, so the corners turn over an\n"
	"   * arc instead of a few pixels.  Capped at half the short side: past\n"
	"   * that the rounded-rect formula stops holding and the gradient flips\n"
	"   * sign across the axes, which showed up as a cross-shaped seam. */\n"
	"  float cap = min(u_size.x, u_size.y) * 0.5 - 0.5;\n"
	"  float rd  = min(u_radius + u_bevel, max(cap, 1.0));\n"
	"  float ge  = 0.5;\n"
	"  float sdc = sdf_rect(p, u_size, rd);\n"
	"  float sxp = sdf_rect(p + vec2(ge, 0.0), u_size, rd);\n"
	"  float sxm = sdf_rect(p - vec2(ge, 0.0), u_size, rd);\n"
	"  float syp = sdf_rect(p + vec2(0.0, ge), u_size, rd);\n"
	"  float sym = sdf_rect(p - vec2(0.0, ge), u_size, rd);\n"
	"  vec2  g   = vec2(sxp - sxm, syp - sym);\n"
	"  float gl = length(g);\n"
	"  g = gl > 1e-5 ? g / gl : vec2(0.0);\n"
	"\n"
	"  /*\n"
	"   * HOW CURVED THE EDGE IS HERE --- which is the whole reason a corner\n"
	"   * is a different lens from a straight edge.\n"
	"   *\n"
	"   * A straight edge is a cylinder: it bends light in ONE direction and\n"
	"   * the band of wallpaper it gathers is as long after the bend as\n"
	"   * before.  A corner is a piece of a torus.  It bends in two, and the\n"
	"   * arc it gathers from is SHORTER than the arc it spreads over, so\n"
	"   * the same displacement concentrates much more there.\n"
	"   *\n"
	"   * For a distance field the Laplacian IS the curvature of the level\n"
	"   * set: zero along the straight runs, one over the corner radius\n"
	"   * around the arcs.  The five samples above give it for nothing.\n"
	"   * Without this term every point at the same depth looked identical\n"
	"   * whether it was halfway along an edge or in the middle of a\n"
	"   * corner, which is exactly what made the corners look merely bent\n"
	"   * rather than lensed.\n"
	"   */\n"
	"  float curv = max((sxp + sxm + syp + sym - 4.0 * sdc) / (ge * ge), 0.0);\n"
	"\n"
	"  /* Inward.  The ray bends towards a normal that leans outward, so it\n"
	"   * lands further IN than it entered: the rim shows -- magnified --\n"
	"   * what is behind the middle. */\n"
	"  vec2 dir = -g;\n"
	"  float ring = clamp(m / max(u_maxd, 1e-4), 0.0, 1.0);\n"
	"  /*\n"
	"   * THE MIDDLE IS NOT PURE FROST.  It was, and that is exactly why\n"
	"   * glass looked like blur: the bevel is a narrow band, so nine tenths\n"
	"   * of a window showed nothing but the blurred wallpaper -- which is\n"
	"   * the other module's entire job.  What makes glass read as glass is\n"
	"   * that you can SEE through it, distorted.\n"
	"   */\n"
	"  float clear = clamp(u_clarity * mix(u_centre_clarity, 1.0, ring),\n"
	"                      0.0, 1.0);\n"
	"\n"
	"  /* Dispersion is a fixed number of pixels, not a share of the\n"
	"   * displacement: the index of glass differs between red and blue by a\n"
	"   * material constant, which has nothing to do with how strong this\n"
	"   * particular lens is. */\n"
	"  /*\n"
	"   * A VERY SHALLOW DOME over the whole slab, not a flat pane.\n"
	"   *\n"
	"   * A perfectly flat slab passes light straight through, so its middle\n"
	"   * can only ever be the wallpaper -- blurred or not.  Real glass of\n"
	"   * this kind is domed a little, and that little is what magnifies the\n"
	"   * middle and gives the surface a slope for light to catch.  Pulling\n"
	"   * the sample towards the centre by a fraction IS a thin lens: it\n"
	"   * magnifies by 1/(1 - lens), uniformly.\n"
	"   */\n"
	"  vec2 centre = u_size * 0.5;\n"
	"  vec2 base = mix(p + dir * m, centre, u_lens);\n"
	"  /* A tighter piece of edge is a stronger lens, and a stronger lens\n"
	"   * separates the colours further.  This is why the fringe is widest\n"
	"   * in the corners, which is where glass actually shows it. */\n"
	"  vec2 sep  = dir * (u_dispersion * ring * (1.0 + curv * m));\n"
	"  vec3 c;\n"
	"  if (u_dispersion > 0.01) {\n"
	"    c.r = tap(base - sep, clear).r;\n"
	"    c.g = tap(base,       clear).g;\n"
	"    c.b = tap(base + sep, clear).b;\n"
	"  } else {\n"
	"    c = tap(base, clear);\n"
	"  }\n"
	"\n"
	"  /* The signed edge profile.  Positive is light coming back, negative\n"
	"   * is the backdrop being spread thin and partly reflected away.  Both\n"
	"   * fall out of the same field: the caustic from how fast the\n"
	"   * displacement decays, the loss and the sheen from Fresnel. */\n"
	"  float h  = 0.5;\n"
	"  float mp = (glass_profile(d + h).x - m) / h;\n"
	"  /*\n"
	"   * Area, not length.  (1 + m') is how the field stretches ALONG the\n"
	"   * gradient; (1 - curv * m) is how it stretches ACROSS it, which is\n"
	"   * one only where the edge is straight.  Multiplying them is the\n"
	"   * two-dimensional gain, and it is what darkens a corner more than\n"
	"   * the edge beside it at the same depth.\n"
	"   */\n"
	"  float gain  = max(0.02, (1.0 + mp) * max(0.05, 1.0 - curv * m));\n"
	"  float gainS = pow(gain, 1.0 / 2.2);\n"
	"  float win   = exp(-2.5 * d / max(u_edge_w, 0.5));\n"
	"  float t     = max(0.0, sin(alpha));\n"
	"  float F     = fresnel(alpha);\n"
	"  float ee    = ((F * u_rim * GLOW + pow(t, SHARP) * u_rim)\n"
	"                 - ((1.0 - gainS) * u_shade + F * u_shade * ABSORB)) * win;\n"
	"\n"
	"  /*\n"
	"   * The highlight the dome catches.\n"
	"   *\n"
	"   * Derived from the dome, not painted on: a lens tilts its surface\n"
	"   * radially, by more the further from the middle, so there is a real\n"
	"   * normal here to put against the light.  That is why the highlight\n"
	"   * sits off-centre towards the light and moves when the light does,\n"
	"   * instead of being a gradient stuck to the window.\n"
	"   */\n"
	"  float sheen = 0.0;\n"
	"  if (u_sheen > 0.001 && u_lens > 0.0001) {\n"
	"    vec2  rv    = p - centre;\n"
	"    float halfm = max(min(u_size.x, u_size.y) * 0.5, 1.0);\n"
	"    vec2  slope = rv / halfm * (u_lens * 6.0);\n"
	"    vec3  nd    = normalize(vec3(-slope, 1.0));\n"
	"    vec3  hv    = normalize(vec3(u_light, 1.0) + vec3(0.0, 0.0, 1.0));\n"
	"    sheen = pow(max(dot(nd, hv), 0.0), 12.0) * u_sheen;\n"
	"  }\n"
	"\n"
	"  /* Only the lit half is steered by the light.  A shade is what the\n"
	"   * geometry took away and is the same all round. */\n"
	"  float lit = ee;\n"
	"  if (ee > 0.0) {\n"
	"    float f = dot(g, u_light);\n"
	"    lit = ee * (max(0.0, f) * 0.78 + max(0.0, -f) * 0.30);\n"
	"  }\n"
	"\n"
	"  /* Darkening MULTIPLIES -- subtracting would drive an already dark\n"
	"   * wallpaper negative -- and light adds. */\n"
	"  c *= 1.0 + min(lit, 0.0);\n"
	"  c += vec3(max(lit, 0.0) + sheen);\n"
	"\n"
	"  /* Saturation, in the ring only.  Folding shows the same wallpaper\n"
	"   * twice and dispersion pulls the channels apart, which together\n"
	"   * muddy the colour along the rim; the centre is never touched. */\n"
	"  float lum = dot(c, vec3(0.2126, 0.7152, 0.0722));\n"
	"  c = mix(vec3(lum), c, mix(1.0, u_sat, ring));\n"
	"  c = clamp(c * u_tint * u_brightness, 0.0, 1.0);\n"
	"\n"
	"  float a = u_alpha * cov;\n"
	"  gl_FragColor = vec4(c * a, a);\n"
	"}\n";

/* ── The largest displacement, host side ─────────────────────────── */

static gdouble
glass_height(gint shape, gdouble t)
{
	gdouble u = 1.0 - t;
	gdouble circle = sqrt(MAX(0.0, 1.0 - u * u));
	gdouble s;

	if (shape <= 0)
		return circle;
	if (shape == 1)
		return pow(MAX(0.0, 1.0 - u * u * u * u), 0.25);

	s = sin(G_PI * t);
	return circle + 0.3 * s * s * cos(G_PI * t);
}

/*
 * The same profile the shader computes, in C.
 *
 * It exists only to find the maximum, which the shader needs as a
 * uniform: the colour fringe is a share of "how far this pixel is pulled
 * relative to the furthest any pixel is pulled", and a fragment shader
 * cannot sweep the whole bevel to find that.  Keeping the two in step is
 * a real obligation -- a divergence shows up as a fringe that is too wide
 * at one bevel and invisible at another -- so they are written next to
 * each other, deliberately.
 */
static gdouble
glass_displacement(gint shape, gdouble d, gdouble bevel, gdouble thickness,
                   gdouble slope)
{
	gdouble x, e, x1, x0, alpha, beta, raw;

	x  = CLAMP(d / bevel, 0.0, 1.0);
	e  = 1e-3;
	x1 = MIN(1.0, x + e);
	x0 = MAX(0.0, x - e);

	alpha = atan((glass_height(shape, x1) - glass_height(shape, x0))
	             / MAX(x1 - x0, 1e-6));
	beta  = asin(MIN(1.0, sin(fabs(alpha)) / GOWL_FX_GLASS_IOR));
	if (alpha < 0.0)
		beta = -beta;

	raw = (thickness + bevel * glass_height(0, x)) * tan(alpha - beta);
	return MIN(raw, slope * MAX(0.0, bevel - d));
}

gdouble
gowl_fx_glass_max_displacement(const GowlFxGlassParams *p)
{
	gdouble bevel, best = 1e-4;
	gint    i;

	if (p == NULL)
		return 1e-4;

	bevel = MAX(1.0, (gdouble)p->bevel);
	for (i = 0; i <= GOWL_FX_GLASS_PROFILE_TAPS; i++) {
		gdouble d = bevel * (gdouble)i / (gdouble)GOWL_FX_GLASS_PROFILE_TAPS;
		gdouble m = glass_displacement((gint)p->shape, d, bevel,
		                               (gdouble)p->thickness,
		                               (gdouble)p->slope);

		best = MAX(best, fabs(m));
	}
	return best;
}

/* ── The pass ────────────────────────────────────────────────────── */

/*
 * Compiled on first use rather than in gowl_fx_gl_new().
 *
 * A shader that fails to build is survivable HERE -- the module sits the
 * session out and the desktop is exactly as it was -- but building it up
 * front would make the same failure take the whole effect layer down with
 * it, and with it the cube, the overview, the switcher, the magnifier and
 * the blur.  A driver that chokes on one atan is not a reason to lose the
 * other five.
 */
static gboolean
glass_prog_ensure(GowlFxGl *self)
{
	GowlFxGlassProg *p = &self->glass;

	if (p->program != 0)
		return TRUE;
	if (self->glass_tried)
		return FALSE;
	self->glass_tried = TRUE;

	p->program = gowl_fx_link_program(glass_vert_src, glass_frag_src);
	if (p->program == 0) {
		g_warning("fx: the liquid-glass shader would not build, so glass "
		          "backdrops will sit this session out");
		return FALSE;
	}

	p->u_soft       = glGetUniformLocation(p->program, "u_soft");
	p->u_sharp      = glGetUniformLocation(p->program, "u_sharp");
	p->u_src_origin = glGetUniformLocation(p->program, "u_src_origin");
	p->u_src_size   = glGetUniformLocation(p->program, "u_src_size");
	p->u_src_scale  = glGetUniformLocation(p->program, "u_src_scale");
	p->u_size       = glGetUniformLocation(p->program, "u_size");
	p->u_radius     = glGetUniformLocation(p->program, "u_radius");
	p->u_bevel      = glGetUniformLocation(p->program, "u_bevel");
	p->u_thickness  = glGetUniformLocation(p->program, "u_thickness");
	p->u_slope      = glGetUniformLocation(p->program, "u_slope");
	p->u_maxd       = glGetUniformLocation(p->program, "u_maxd");
	p->u_shape      = glGetUniformLocation(p->program, "u_shape");
	p->u_dispersion = glGetUniformLocation(p->program, "u_dispersion");
	p->u_rim        = glGetUniformLocation(p->program, "u_rim");
	p->u_shade      = glGetUniformLocation(p->program, "u_shade");
	p->u_edge_w     = glGetUniformLocation(p->program, "u_edge_w");
	p->u_light      = glGetUniformLocation(p->program, "u_light");
	p->u_sat        = glGetUniformLocation(p->program, "u_sat");
	p->u_clarity    = glGetUniformLocation(p->program, "u_clarity");
	p->u_centre_clarity = glGetUniformLocation(p->program, "u_centre_clarity");
	p->u_lens       = glGetUniformLocation(p->program, "u_lens");
	p->u_sheen      = glGetUniformLocation(p->program, "u_sheen");
	p->u_tint       = glGetUniformLocation(p->program, "u_tint");
	p->u_brightness = glGetUniformLocation(p->program, "u_brightness");
	p->u_alpha      = glGetUniformLocation(p->program, "u_alpha");
	p->a_pos        = glGetAttribLocation(p->program, "a_pos");
	p->a_uv         = glGetAttribLocation(p->program, "a_uv");
	return TRUE;
}

void
gowl_fx_glass_params_init(GowlFxGlassParams *params)
{
	if (params == NULL)
		return;

	memset(params, 0, sizeof(*params));
	/* The set hyalite's author tuned by eye: a narrow bevel over a very
	 * thick slab with a folding slope, so the centre stays clear while
	 * the edge concentrates the wallpaper into a coloured band. */
	params->bevel      = 37.0f;
	params->thickness  = 59.0f;
	params->slope      = 2.7f;
	params->shape      = 1.0f;
	params->dispersion = 1.6f;
	params->rim        = 1.76f;
	params->shade      = 0.46f;
	params->edge_width = 8.0f;
	params->saturation = 0.86f;
	params->clarity    = 1.0f;
	params->brightness = 1.0f;
	params->alpha      = 1.0f;
	params->src_scale = 1.0f;
	params->tint[0] = params->tint[1] = params->tint[2] = 1.0f;
	/* -140 degrees, the angle it was tuned at: light from the upper left.
	 * 0 is straight above and positive turns clockwise, with screen y
	 * pointing down. */
	params->light[0] = (gfloat)sin(-140.0 * G_PI / 180.0);
	params->light[1] = (gfloat)-cos(-140.0 * G_PI / 180.0);
}

gboolean
gowl_fx_pass_glass(GowlFxPass              *pass,
                   const GowlFxTexture     *soft,
                   const GowlFxTexture     *sharp,
                   const GowlFxGlassParams *params)
{
	GowlFxGl              *gl;
	const GowlFxGlassProg *p;
	GowlFxGlassParams      use;
	const GowlFxTexture   *clear_src;
	gfloat                 bevel, cap;

	if (pass == NULL || params == NULL || soft == NULL)
		return FALSE;
	if (params->width <= 0 || params->height <= 0)
		return FALSE;
	if (soft->tex == 0 || soft->width <= 0 || soft->height <= 0)
		return FALSE;

	gl = pass->gl;
	if (!glass_prog_ensure(gl))
		return FALSE;
	p = &gl->glass;

	use = *params;

	/*
	 * The bevel cannot exceed half the short side.  Not a taste clamp:
	 * past it the rounded-rect distance field the direction comes from
	 * has a negative half-extent, its gradient flips sign across the
	 * axes, and a window narrower than two bevels grows a cross-shaped
	 * seam down the middle.  A small window therefore gets a
	 * proportionally smaller lens rather than a broken one.
	 */
	cap   = (gfloat)MIN(use.width, use.height) * 0.5f - 1.0f;
	bevel = CLAMP(use.bevel, 1.0f, MAX(cap, 1.0f));
	use.bevel = bevel;
	use.radius = CLAMP(use.radius, 0.0f,
	                   (gfloat)MIN(use.width, use.height) * 0.5f);

	/* Without a sharp copy the ring simply stays frosted; it is an
	 * improvement, not a requirement, and a module that has not built one
	 * yet must still get glass. */
	clear_src = (sharp != NULL && sharp->tex != 0) ? sharp : soft;

	glUseProgram(p->program);
	glUniform2f(p->u_src_origin, use.src_origin[0], use.src_origin[1]);
	glUniform2f(p->u_src_size, (gfloat)MAX(1, soft->width),
	            (gfloat)MAX(1, soft->height));
	glUniform1f(p->u_src_scale, use.src_scale > 0.0f ? use.src_scale : 1.0f);
	glUniform2f(p->u_size, (gfloat)use.width, (gfloat)use.height);
	glUniform1f(p->u_radius, use.radius);
	glUniform1f(p->u_bevel, use.bevel);
	glUniform1f(p->u_thickness, MAX(0.0f, use.thickness));
	glUniform1f(p->u_slope, CLAMP(use.slope, 0.2f, 4.0f));
	glUniform1f(p->u_maxd, (gfloat)gowl_fx_glass_max_displacement(&use));
	glUniform1f(p->u_shape, use.shape);
	glUniform1f(p->u_dispersion, MAX(0.0f, use.dispersion));
	glUniform1f(p->u_rim, MAX(0.0f, use.rim));
	glUniform1f(p->u_shade, MAX(0.0f, use.shade));
	glUniform1f(p->u_edge_w, MAX(0.5f, use.edge_width));
	glUniform2f(p->u_light, use.light[0], use.light[1]);
	glUniform1f(p->u_sat, MAX(0.0f, use.saturation));
	glUniform1f(p->u_clarity, CLAMP(use.clarity, 0.0f, 1.0f));
	glUniform1f(p->u_centre_clarity,
	            CLAMP(use.centre_clarity, 0.0f, 1.0f));
	glUniform1f(p->u_lens, CLAMP(use.lens, 0.0f, 0.5f));
	glUniform1f(p->u_sheen, MAX(0.0f, use.sheen));
	glUniform3fv(p->u_tint, 1, use.tint);
	glUniform1f(p->u_brightness, MAX(0.0f, use.brightness));
	glUniform1f(p->u_alpha, CLAMP(use.alpha, 0.0f, 1.0f));

	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, soft->tex);
	glUniform1i(p->u_soft, 0);
	glActiveTexture(GL_TEXTURE1);
	glBindTexture(GL_TEXTURE_2D, clear_src->tex);
	glUniform1i(p->u_sharp, 1);

	gowl_fx_draw_screen_quad(p->a_pos, p->a_uv);

	glBindTexture(GL_TEXTURE_2D, 0);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, 0);
	glUseProgram(0);
	return TRUE;
}
