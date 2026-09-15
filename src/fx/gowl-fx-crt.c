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
 * The desktop on a cathode ray tube.
 *
 * Unlike everything else in this directory this is not a backdrop.  A
 * backdrop is what shows THROUGH a translucent window; this is the glass
 * in FRONT of everything, so it is a whole-output filter -- capture the
 * finished screen, put it back through a tube.  modules/crt drives it.
 *
 * Five things make a CRT look like a CRT, and every one of them is a
 * physical part of the machine rather than a filter somebody liked:
 *
 *   1. THE FACEPLATE IS CURVED.  The phosphor is painted on the inside
 *      of a glass shell, so the picture lives on a piece of a sphere and
 *      what the eye gets is that sphere projected flat.  A point whose
 *      projection sits r from the middle is really R*asin(r/R) of ARC
 *      from the middle, and arc is always the longer of the two.  So the
 *      further out a pixel is the more of the picture is packed behind
 *      it: the middle appears magnified, straight lines bow outward, and
 *      the corners tuck in.  That is barrel distortion, and it is not an
 *      effect applied to a CRT -- it is what looking at one IS.
 *
 *      A Trinitron is a cylinder rather than a sphere: curved across,
 *      dead flat down.  `curve_y' is the vertical share of the
 *      curvature, so 0 is Sony's tube and 1 is everybody else's.
 *
 *   2. THE BEAM IS A SPOT, NOT A LINE.  It is swept across, so a scan
 *      line is a Gaussian ridge of light with dark glass between it and
 *      the next one.  The width of that ridge is the spot size, and the
 *      spot GROWS WITH BEAM CURRENT: a bright line is a fat line and a
 *      dim one is a thread.  That one fact is the whole difference
 *      between scanlines that look like a tube and scanlines that look
 *      like a striped PNG laid over the screen, because it means the
 *      pattern breathes with the picture instead of sitting on it.
 *
 *      The ridge here is normalised to MEAN ONE over its period (a
 *      periodised Gaussian integrates to sigma*sqrt(2pi), which is
 *      divided straight back out), so turning scanlines up cannot change
 *      how bright the desktop is.  That is worth having on a desktop and
 *      it is also what tests/test-crt-render.c measures.
 *
 *   3. THE PHOSPHOR IS IN THREADS OF THREE.  Behind the glass is an
 *      aperture grille or a shadow mask, and behind that red, green and
 *      blue phosphor in stripes or dots.  White is not white anywhere on
 *      the screen -- it is a very fine RGB texture that the eye adds up
 *      from a normal viewing distance, and seeing it is most of why a
 *      photograph of a CRT is unmistakable.  Each channel's gain is a
 *      raised cosine one third of a pitch out of step with its
 *      neighbours, which has mean one by construction, so the mask is a
 *      texture and not a dimmer.  (A real mask costs two thirds of the
 *      light and the tube answers with more beam current.  Taking that
 *      literally would just make the desktop dark.)
 *
 *   4. THE GLASS SCATTERS.  Light leaving the phosphor bounces about
 *      inside a thick faceplate before it gets out -- halation.  A white
 *      window on a black background is ringed with a soft glow, and that
 *      glow fills the gaps between scan lines, which is why CRT whites
 *      read as solid while CRT greys stay obviously striped.  So the
 *      bloom is added AFTER the beam and the mask, never before.
 *
 *   5. THE THREE GUNS DO NOT QUITE AGREE.  Convergence is adjusted at
 *      the middle and drifts towards the corners, so edges of things far
 *      from the centre carry a red fringe one way and a blue fringe the
 *      other.  Radial, and growing as the square of the distance out.
 *
 * ALL OF THIS IS DONE IN LIGHT.  A CRT's transfer curve is a power law
 * near 2.4 -- that is the historical reason sRGB is shaped the way it
 * is -- so the captured value is a drive level, the light is that value
 * to the 2.4, the beam and the mask and the glow are all multiplications
 * and additions of LIGHT, and the result goes back through the same
 * exponent at the end.  Modulating gamma-encoded values instead gets
 * scanlines that are far too bright and a bloom that greys out the
 * blacks, which is the usual reason a CRT filter looks like a sticker.
 *
 * WHAT THIS DELIBERATELY DOES NOT DO is resample the desktop down to a
 * 480-line raster.  A real tube fed 480 lines genuinely has no detail
 * between them, but a desktop is a thing somebody is READING: the point
 * here is a screen that looks like a tube, not a screen that has thrown
 * away the text.  The beam modulates full-resolution pixels.
 *
 * Nor does it keep a history buffer for phosphor persistence.  P22
 * decays in a few milliseconds and the trail it leaves is real, but on a
 * desktop it smears the words you are in the middle of reading, and it
 * would cost an accumulation texture per output to do it.
 */

#undef G_LOG_DOMAIN
#define G_LOG_DOMAIN "gowl-fx"

#include "gowl-fx-private.h"

#include <math.h>
#include <string.h>

static const gchar crt_vert_src[] =
	"attribute vec2 a_pos;\n"
	"attribute vec2 a_uv;\n"
	"varying vec2 v_uv;\n"
	"void main() {\n"
	"  v_uv = a_uv;\n"
	"  gl_Position = vec4(a_pos, 0.0, 1.0);\n"
	"}\n";

/*
 * highp where it exists.  The mask is a cosine of the screen position
 * divided by three pixels, so its argument runs to a couple of thousand
 * radians at the right-hand edge of a 4K screen; at mediump the phase
 * there has lost enough bits that the stripes drift out of step with the
 * pixel grid and beat against it.
 */
static const gchar crt_frag_src[] =
	"#ifdef GL_FRAGMENT_PRECISION_HIGH\n"
	"precision highp float;\n"
	"#else\n"
	"precision mediump float;\n"
	"#endif\n"
	"\n"
	"uniform sampler2D u_screen;   /* the finished desktop */\n"
	"uniform sampler2D u_glow;     /* the same, blurred, for halation */\n"
	"uniform vec2  u_size;         /* output pixels */\n"
	"uniform float u_aspect;       /* height / width */\n"
	"uniform float u_curve;        /* 1/R, in half screen widths */\n"
	"uniform float u_curve_y;      /* vertical share of it, 0..1 */\n"
	"uniform vec2  u_fit;          /* edge stretch, so nothing is cropped */\n"
	"uniform float u_lines;        /* scan lines down the raster */\n"
	"uniform float u_scanline;     /* how deep they cut, 0..1 */\n"
	"uniform float u_beam;         /* spot sigma at black, scan lines */\n"
	"uniform float u_beam_bloom;   /* how much wider at full white */\n"
	"uniform float u_mask;         /* phosphor triad depth, 0..1 */\n"
	"uniform float u_mask_kind;    /* 0 none 1 grille 2 shadow 3 slot */\n"
	"uniform float u_mask_size;    /* output pixels per triad */\n"
	"uniform float u_bloom;        /* halation gain */\n"
	"uniform float u_bloom_cut;    /* the light level it starts from */\n"
	"uniform float u_glow_on;      /* 0 when u_glow is not a picture */\n"
	"uniform float u_vignette;\n"
	"uniform float u_corner;       /* tube corner radius, half heights */\n"
	"uniform float u_converge;     /* gun error at the corner, pixels */\n"
	"uniform float u_hum;          /* the drifting brightness bar */\n"
	"uniform float u_hum_phase;\n"
	"uniform float u_gamma;        /* the tube's transfer exponent */\n"
	"uniform float u_bright;\n"
	"\n"
	"varying vec2 v_uv;\n"
	"\n"
	"#define PI 3.14159265358979\n"
	"\n"
	"/* Arc along a tube of radius 1/k whose flat projection is at t.\n"
	"   The clamp is where the faceplate would pass the sphere's own\n"
	"   equator -- unbuildable as a tube, but asked for by a big enough\n"
	"   `crt-curvature', and a fisheye is a better answer than a NaN. */\n"
	"float crt_arc(float t, float k)\n"
	"{\n"
	"  if (k < 1.0e-4) return t;\n"
	"  return asin(min(t * k, 0.9995)) / k;\n"
	"}\n"
	"\n"
	"/* Where on the flat raster the eye is looking, when it looks at a\n"
	"   point @p of the curved faceplate.  Both in half screen widths, so\n"
	"   that the curvature is the same amount of glass in both axes. */\n"
	"vec2 crt_uncurve(vec2 p, float k, float cy)\n"
	"{\n"
	"  float r = length(vec2(p.x, p.y * cy));\n"
	"  float f;\n"
	"\n"
	"  if (r < 1.0e-6 || k < 1.0e-4) return p;\n"
	"  f = crt_arc(r, k) / r;\n"
	"  return vec2(p.x * f, p.y * mix(1.0, f, cy));\n"
	"}\n"
	"\n"
	"/* One scan line's worth of beam, as a share of the light that would\n"
	"   be there with no scan lines at all.  Gaussian, periodised over the\n"
	"   line spacing and divided by sigma*sqrt(2pi) -- which is exactly\n"
	"   the area of a Gaussian -- so its mean over a period is 1 whatever\n"
	"   sigma is, and deepening the lines cannot dim the screen.  Three\n"
	"   terms: at the sigma this is clamped to, the fourth is 1e-6. */\n"
	"float crt_beam(float f, float s)\n"
	"{\n"
	"  float g;\n"
	"\n"
	"  g  = exp(-0.5 * (f / s) * (f / s));\n"
	"  g += exp(-0.5 * ((f - 1.0) / s) * ((f - 1.0) / s));\n"
	"  g += exp(-0.5 * ((f + 1.0) / s) * ((f + 1.0) / s));\n"
	"  return g / (s * 2.5066282746310002);\n"
	"}\n"
	"\n"
	"/* The phosphor, as a per-channel gain.  Each channel is a raised\n"
	"   cosine a third of a pitch behind the last, which has mean 1 over\n"
	"   the pitch -- so this is a colour texture and not a dimmer.\n"
	"\n"
	"   In SCREEN pixels, not raster ones.  The mask is a physical part of\n"
	"   the tube and does curve with the glass, but its pitch is three\n"
	"   pixels: let the curvature stretch that by the few percent it\n"
	"   stretches everything else and the pattern beats against the pixel\n"
	"   grid in slow moire bands, which is far more visible than the\n"
	"   bowing it would have bought. */\n"
	"vec3 crt_mask(vec2 sp, float kind, float size, float depth)\n"
	"{\n"
	"  vec3  phase = vec3(0.0, 1.0, 2.0) / 3.0;\n"
	"  vec3  g;\n"
	"  float x, y, row, sx;\n"
	"\n"
	"  if (depth <= 0.001 || kind < 0.5) return vec3(1.0);\n"
	"\n"
	"  size = max(size, 1.0);\n"
	"  x = sp.x / size;\n"
	"  y = sp.y / size;\n"
	"\n"
	"  if (kind < 1.5) {\n"
	"    /* Aperture grille: uninterrupted vertical stripes, the whole\n"
	"       height of the tube.  A Trinitron. */\n"
	"    return vec3(1.0) + depth * cos(2.0 * PI * (vec3(x) - phase));\n"
	"  }\n"
	"  if (kind < 2.5) {\n"
	"    /* Shadow mask: dot triads, every other row of holes offset by\n"
	"       half a pitch, which is what makes the array hexagonal. */\n"
	"    row = floor(y);\n"
	"    sx  = x + 0.5 * mod(row, 2.0);\n"
	"    g   = vec3(1.0) + depth * cos(2.0 * PI * (vec3(sx) - phase));\n"
	"    return g * (1.0 + depth * 0.5 * cos(2.0 * PI * y));\n"
	"  }\n"
	"  /* Slot mask: the grille's stripes, broken into slots two rows\n"
	"     tall and staggered -- the compromise tube, and the commonest. */\n"
	"  row = floor(y * 0.5);\n"
	"  sx  = x + 0.5 * mod(row, 2.0);\n"
	"  g   = vec3(1.0) + depth * cos(2.0 * PI * (vec3(sx) - phase));\n"
	"  return g * (1.0 + depth * 0.35 * cos(PI * y));\n"
	"}\n"
	"\n"
	"void main()\n"
	"{\n"
	"  vec2  sp = v_uv * u_size;              /* screen pixels */\n"
	"  vec2  p  = vec2(v_uv.x * 2.0 - 1.0,\n"
	"                  (v_uv.y * 2.0 - 1.0) * u_aspect);\n"
	"  vec2  src, n, tc, texel, off;\n"
	"  vec3  col, light, glow, mask;\n"
	"  float inx, iny, inside, luma, sigma, ppl, fade, depth;\n"
	"  float r2, rim, vig, hum, dist, rad;\n"
	"  vec2  half_size, corner_d;\n"
	"\n"
	"  /* Where on the raster this bit of glass is looking, normalised so\n"
	"     that the middle of each edge of the raster lands on the middle\n"
	"     of the matching edge of the screen.  Nothing is cropped: the\n"
	"     picture is pulled in at the corners and the tube shows black\n"
	"     there, which is the shape a barrel-distorted rectangle has. */\n"
	"  src = crt_uncurve(p, u_curve, u_curve_y);\n"
	"  n   = vec2(src.x / u_fit.x, (src.y / u_aspect) / u_fit.y);\n"
	"  tc  = n * 0.5 + 0.5;\n"
	"\n"
	"  /* One pixel of feather on the raster's edge, so the bow is a\n"
	"     curve and not a staircase. */\n"
	"  inx    = clamp((1.0 - abs(n.x)) * u_size.x * 0.5, 0.0, 1.0);\n"
	"  iny    = clamp((1.0 - abs(n.y)) * u_size.y * 0.5, 0.0, 1.0);\n"
	"  inside = inx * iny;\n"
	"\n"
	"  /* Convergence: red lands long and blue lands short, radially, by\n"
	"     an amount that goes as the square of the distance from where\n"
	"     the guns were adjusted -- the middle. */\n"
	"  texel = 1.0 / max(u_size, vec2(1.0));\n"
	"  r2    = dot(n, n);\n"
	"  off   = (r2 > 1.0e-8 ? normalize(n) : vec2(0.0))\n"
	"          * (u_converge * r2) * texel;\n"
	"\n"
	"  col = vec3(texture2D(u_screen, tc + off).r,\n"
	"             texture2D(u_screen, tc).g,\n"
	"             texture2D(u_screen, tc - off).b);\n"
	"\n"
	"  /* Drive level to light. */\n"
	"  light = pow(max(col, 0.0), vec3(u_gamma));\n"
	"\n"
	"  /* The beam.  Its width comes from how hard this part of the\n"
	"     picture is driving the gun, which is the whole trick. */\n"
	"  ppl   = u_size.y / max(u_lines, 1.0);\n"
	"  luma  = dot(light, vec3(0.2126, 0.7152, 0.0722));\n"
	"  sigma = u_beam + u_beam_bloom * clamp(luma, 0.0, 1.0);\n"
	"\n"
	"  /* A spot finer than the pixel grid cannot be drawn on it, and\n"
	"     trying gives a moire of the two.  Widen it to what the grid can\n"
	"     carry and, below two pixels a line, stop drawing lines at all --\n"
	"     the pattern fades out instead of turning into noise. */\n"
	"  sigma = clamp(max(sigma, 0.80 / max(ppl, 1.0e-3)), 0.10, 0.50);\n"
	"  fade  = clamp((ppl - 2.0) * 0.5, 0.0, 1.0);\n"
	"  depth = clamp(u_scanline, 0.0, 1.0) * fade;\n"
	"  light *= mix(1.0, crt_beam(fract(tc.y * u_lines) - 0.5, sigma), depth);\n"
	"\n"
	"  /* The phosphor. */\n"
	"  mask   = crt_mask(sp, u_mask_kind, u_mask_size, clamp(u_mask, 0.0, 1.0));\n"
	"  light *= mask;\n"
	"\n"
	"  /* Halation: light that has already left the phosphor, bounced\n"
	"     about in the faceplate and come back out somewhere else.  It\n"
	"     is what fills the dark glass between scan lines on anything\n"
	"     bright, so it goes on AFTER the beam and the mask.\n"
	"\n"
	"     IT IS MOVED, NOT MADE.  What arrives here is the blurred\n"
	"     picture; what left here is this pixel's own share, and both\n"
	"     terms are in the sum.  Adding only the arrival -- which is\n"
	"     what a bloom pass usually does -- gives a big white window\n"
	"     more light than it started with everywhere at once, and every\n"
	"     pixel of it clips to the same flat value: the scan lines and\n"
	"     the phosphor vanish from exactly the place they would be most\n"
	"     visible.  With the departure subtracted the inside of a broad\n"
	"     highlight is left alone, its edge gives up a little, and the\n"
	"     dark beside it gains what the edge lost, which is what\n"
	"     halation looks like and is where its energy comes from. */\n"
	"  if (u_glow_on > 0.5 && u_bloom > 0.0) {\n"
	"    vec3 here;\n"
	"\n"
	"    glow  = pow(max(texture2D(u_glow, tc).rgb, 0.0), vec3(u_gamma));\n"
	"    here  = pow(max(col, 0.0), vec3(u_gamma));\n"
	"    light += u_bloom * (max(glow - vec3(u_bloom_cut), vec3(0.0))\n"
	"                        - max(here - vec3(u_bloom_cut), vec3(0.0)));\n"
	"    light  = max(light, vec3(0.0));\n"
	"  }\n"
	"\n"
	"  /* A slow bright band drifting up the screen: the beat between the\n"
	"     mains and the field rate on a supply that is not quite up to\n"
	"     it.  Every tired CRT has one. */\n"
	"  hum = 1.0 + u_hum * sin(2.0 * PI * (n.y * 0.5 + u_hum_phase));\n"
	"\n"
	"  /* The glass at the edge is seen at a slant and through more of\n"
	"     itself. */\n"
	"  vig = 1.0 - clamp(u_vignette, 0.0, 1.0) * clamp(r2, 0.0, 2.0) * 0.5;\n"
	"\n"
	"  /* The rim of the tube, which is a rounded rectangle and not the\n"
	"     rectangle the screen is. */\n"
	"  half_size = vec2(1.0, u_aspect);\n"
	"  rad       = clamp(u_corner, 0.0, 1.0) * u_aspect;\n"
	"  corner_d  = abs(p) - (half_size - vec2(rad));\n"
	"  dist      = length(max(corner_d, 0.0))\n"
	"              + min(max(corner_d.x, corner_d.y), 0.0) - rad;\n"
	"  rim       = 1.0 - smoothstep(-2.0 / u_size.x, 0.0, dist);\n"
	"\n"
	"  light *= hum * vig;\n"
	"\n"
	"  /* Light back to a drive level for whatever is actually going to\n"
	"     show this.  The coverage terms are applied after, because they\n"
	"     are how much of the pixel is tube rather than how bright it is. */\n"
	"  col = pow(clamp(light * u_bright, 0.0, 1.0), vec3(1.0 / u_gamma));\n"
	"  col *= inside * rim;\n"
	"\n"
	"  gl_FragColor = vec4(col, 1.0);\n"
	"}\n";

static gboolean
crt_prog_ensure(GowlFxGl *self)
{
	GowlFxCrtProg *p = &self->crt;

	if (p->program != 0)
		return TRUE;
	if (self->crt_tried)
		return FALSE;
	self->crt_tried = TRUE;

	p->program = gowl_fx_link_program(crt_vert_src, crt_frag_src);
	if (p->program == 0) {
		g_warning("fx: the CRT shader would not build, so the screen "
		          "will stay flat this session");
		return FALSE;
	}

	p->u_screen     = glGetUniformLocation(p->program, "u_screen");
	p->u_glow       = glGetUniformLocation(p->program, "u_glow");
	p->u_size       = glGetUniformLocation(p->program, "u_size");
	p->u_aspect     = glGetUniformLocation(p->program, "u_aspect");
	p->u_curve      = glGetUniformLocation(p->program, "u_curve");
	p->u_curve_y    = glGetUniformLocation(p->program, "u_curve_y");
	p->u_fit        = glGetUniformLocation(p->program, "u_fit");
	p->u_lines      = glGetUniformLocation(p->program, "u_lines");
	p->u_scanline   = glGetUniformLocation(p->program, "u_scanline");
	p->u_beam       = glGetUniformLocation(p->program, "u_beam");
	p->u_beam_bloom = glGetUniformLocation(p->program, "u_beam_bloom");
	p->u_mask       = glGetUniformLocation(p->program, "u_mask");
	p->u_mask_kind  = glGetUniformLocation(p->program, "u_mask_kind");
	p->u_mask_size  = glGetUniformLocation(p->program, "u_mask_size");
	p->u_bloom      = glGetUniformLocation(p->program, "u_bloom");
	p->u_bloom_cut  = glGetUniformLocation(p->program, "u_bloom_cut");
	p->u_glow_on    = glGetUniformLocation(p->program, "u_glow_on");
	p->u_vignette   = glGetUniformLocation(p->program, "u_vignette");
	p->u_corner     = glGetUniformLocation(p->program, "u_corner");
	p->u_converge   = glGetUniformLocation(p->program, "u_converge");
	p->u_hum        = glGetUniformLocation(p->program, "u_hum");
	p->u_hum_phase  = glGetUniformLocation(p->program, "u_hum_phase");
	p->u_gamma      = glGetUniformLocation(p->program, "u_gamma");
	p->u_bright     = glGetUniformLocation(p->program, "u_bright");
	p->a_pos        = glGetAttribLocation(p->program, "a_pos");
	p->a_uv         = glGetAttribLocation(p->program, "a_uv");
	return TRUE;
}

void
gowl_fx_crt_params_init(GowlFxCrtParams *params)
{
	if (params == NULL)
		return;

	memset(params, 0, sizeof(*params));

	/*
	 * A consumer tube of the late nineties: gently spherical, a slot
	 * mask, and scan lines you can see without having to look for them.
	 *
	 * 0.45 is 1/R with R in half screen widths, so a faceplate whose
	 * radius is a little over twice the width of the picture.  Real
	 * flat-square tubes were between one and three of those; below about
	 * 0.2 nobody can tell it from flat and above about 0.8 the corners
	 * are eating a serious amount of desktop.
	 */
	params->curvature   = 0.45f;
	params->curvature_y = 1.0f;
	params->lines       = 0.0f;    /* derived from the output */
	params->scanline    = 0.55f;
	params->beam        = 0.16f;
	params->beam_bloom  = 0.18f;
	params->mask        = 0.30f;
	params->mask_kind   = GOWL_CRT_MASK_SLOT;
	params->mask_size   = 0.0f;    /* derived from the output */
	params->bloom       = 0.45f;
	params->bloom_cut   = 0.45f;
	params->vignette    = 0.22f;
	params->corner      = 0.06f;
	params->convergence = 1.20f;
	params->hum         = 0.020f;
	params->gamma       = 2.40f;
	params->brightness  = 1.03f;
}

void
gowl_fx_crt_advance(GowlFxCrtClock *clock, gdouble dt)
{
	if (clock == NULL)
		return;
	if (!(dt > 0.0))
		return;
	if (dt > 0.25)
		dt = 0.25;   /* a stall is not a fast-forward */

	/*
	 * The hum bar drifts one screen height every seven seconds or so.
	 * Slower than that and it reads as a gradient somebody baked in;
	 * faster and it is a strobe.  Kept in [0, 1) because it is a phase
	 * and single precision in a uniform runs out of mantissa on a
	 * session that has been up for a week.
	 */
	clock->hum_phase += dt / 7.0;
	clock->hum_phase = fmod(clock->hum_phase, 1.0);
	if (clock->hum_phase < 0.0)
		clock->hum_phase += 1.0;
}

gint
gowl_fx_crt_auto_lines(gint height)
{
	/*
	 * Four output pixels to a scan line.
	 *
	 * The beam is a Gaussian about a third of a line wide, so at four
	 * pixels a line it is a bit over one pixel across: the narrowest
	 * ridge the grid can carry without the shader having to widen it.
	 * Fewer lines than that and the pattern is coarse and obvious;
	 * more and it is being drawn at the resolution limit, where the
	 * shader fades it out rather than alias.
	 */
	if (height <= 0)
		return 240;
	/* The floor is the config's own lower bound rather than anything
	 * about screens: below sixty lines this is not a raster, it is a
	 * venetian blind, and a headless 256-pixel test surface is entitled
	 * to a raster it can actually draw. */
	return CLAMP(height / 4, 60, 1080);
}

gint
gowl_fx_crt_auto_mask_size(gint width)
{
	/*
	 * A triad every three pixels is the classic, and it is the classic
	 * because a 640-wide picture on a 13" tube put the triads about
	 * there.  Scale with the screen so the phosphor stays the same
	 * SIZE rather than the same number of pixels: on a 4K panel a
	 * three-pixel pitch is under half a millimetre and invisible.
	 */
	if (width <= 0)
		return 3;
	return CLAMP((width + 320) / 640, 3, 12);
}

void
gowl_fx_crt_fit(gdouble  curvature,
                gdouble  curvature_y,
                gdouble  aspect,
                gfloat  *fit_x,
                gfloat  *fit_y)
{
	gdouble k  = MAX(curvature, 0.0);
	gdouble cy = CLAMP(curvature_y, 0.0, 1.0);
	gdouble ry, f, sx, sy;

	/*
	 * How much the arc stretches the middle of each edge.
	 *
	 * Dividing the raster coordinate by this is what makes the picture
	 * FIT: the middle of every edge of the raster lands exactly on the
	 * middle of the matching edge of the screen, the corners come in
	 * from there, and no part of the desktop is pushed off.  Scaling to
	 * the corners instead would fill the screen and lose the four
	 * corners of the desktop -- overscan, which a tube really did do,
	 * and which on a desktop eats window buttons.
	 *
	 * Here rather than in the shader because it is the same two numbers
	 * for every one of eight million fragments, and because arithmetic
	 * in a C file can be tested.
	 */
	if (aspect <= 0.0)
		aspect = 1.0;

	if (k < 1.0e-4) {
		sx = 1.0;
		sy = 1.0;
	} else {
		/* (1, 0): the radius is 1 whatever the vertical share is. */
		sx = asin(MIN(k, 0.9995)) / k;

		/* (0, aspect): the radius is the vertical share of it. */
		ry = aspect * cy;
		if (ry < 1.0e-6) {
			sy = 1.0;
		} else {
			f  = asin(MIN(ry * k, 0.9995)) / (ry * k);
			sy = 1.0 + cy * (f - 1.0);
		}
	}

	if (fit_x != NULL)
		*fit_x = (gfloat)sx;
	if (fit_y != NULL)
		*fit_y = (gfloat)sy;
}

gboolean
gowl_fx_pass_crt(GowlFxPass            *pass,
                 const GowlFxTexture   *screen,
                 const GowlFxTexture   *glow,
                 const GowlFxCrtParams *params,
                 const GowlFxCrtClock  *clock)
{
	GowlFxGl            *gl;
	const GowlFxCrtProg *p;
	GowlFxCrtClock       still;
	gfloat               fit_x, fit_y;
	gfloat               aspect;
	gfloat               lines, mask_size;
	gboolean             have_glow;

	if (pass == NULL || params == NULL || screen == NULL)
		return FALSE;
	if (screen->tex == 0 || screen->width <= 0 || screen->height <= 0)
		return FALSE;
	if (pass->width <= 0 || pass->height <= 0)
		return FALSE;

	gl = pass->gl;
	if (!crt_prog_ensure(gl))
		return FALSE;
	p = &gl->crt;

	if (clock == NULL) {
		memset(&still, 0, sizeof(still));
		clock = &still;
	}

	aspect = (gfloat)pass->height / (gfloat)MAX(1, pass->width);
	gowl_fx_crt_fit(params->curvature, params->curvature_y,
	                aspect, &fit_x, &fit_y);

	lines = params->lines > 0.0f
		? params->lines : (gfloat)gowl_fx_crt_auto_lines(pass->height);
	mask_size = params->mask_size > 0.0f
		? params->mask_size
		: (gfloat)gowl_fx_crt_auto_mask_size(pass->width);

	have_glow = glow != NULL && glow->tex != 0 && params->bloom > 0.0f;

	glUseProgram(p->program);
	glUniform2f(p->u_size, (gfloat)pass->width, (gfloat)pass->height);
	glUniform1f(p->u_aspect, aspect);
	glUniform1f(p->u_curve, CLAMP(params->curvature, 0.0f, 0.9f));
	glUniform1f(p->u_curve_y, CLAMP(params->curvature_y, 0.0f, 1.0f));
	glUniform2f(p->u_fit, fit_x, fit_y);
	glUniform1f(p->u_lines, CLAMP(lines, 60.0f, 4320.0f));
	glUniform1f(p->u_scanline, CLAMP(params->scanline, 0.0f, 1.0f));
	glUniform1f(p->u_beam, CLAMP(params->beam, 0.08f, 0.45f));
	glUniform1f(p->u_beam_bloom, CLAMP(params->beam_bloom, 0.0f, 0.40f));
	glUniform1f(p->u_mask, CLAMP(params->mask, 0.0f, 1.0f));
	glUniform1f(p->u_mask_kind, (gfloat)CLAMP((gint)params->mask_kind,
	                                          GOWL_CRT_MASK_NONE,
	                                          GOWL_CRT_MASK_SLOT));
	glUniform1f(p->u_mask_size, CLAMP(mask_size, 1.0f, 32.0f));
	glUniform1f(p->u_bloom, CLAMP(params->bloom, 0.0f, 4.0f));
	glUniform1f(p->u_bloom_cut, CLAMP(params->bloom_cut, 0.0f, 1.0f));
	glUniform1f(p->u_glow_on, have_glow ? 1.0f : 0.0f);
	glUniform1f(p->u_vignette, CLAMP(params->vignette, 0.0f, 1.0f));
	glUniform1f(p->u_corner, CLAMP(params->corner, 0.0f, 1.0f));
	glUniform1f(p->u_converge, CLAMP(params->convergence, 0.0f, 16.0f));
	glUniform1f(p->u_hum, CLAMP(params->hum, 0.0f, 0.5f));
	glUniform1f(p->u_hum_phase, (gfloat)clock->hum_phase);
	glUniform1f(p->u_gamma, CLAMP(params->gamma, 1.0f, 3.2f));
	glUniform1f(p->u_bright, CLAMP(params->brightness, 0.0f, 4.0f));

	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, screen->tex);
	glUniform1i(p->u_screen, 0);
	glActiveTexture(GL_TEXTURE1);
	/* Always bound: sampling an unbound unit is undefined, and this
	 * shader reads u_glow behind a uniform branch rather than a compile
	 * time one. */
	glBindTexture(GL_TEXTURE_2D, have_glow ? glow->tex : screen->tex);
	glUniform1i(p->u_glow, 1);

	gowl_fx_draw_screen_quad(p->a_pos, p->a_uv);

	glBindTexture(GL_TEXTURE_2D, 0);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, 0);
	glUseProgram(0);
	return TRUE;
}
