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
 * gowl-fx-soap.c -- the wallpaper through a soap film.
 *
 * THE ONE HERE THAT DOES NOT REFRACT.  Every other backdrop in this
 * directory bends light: a drop is a lens, a bubble is a lens the other
 * way round, the glass is a slab, the water is a surface.  This one
 * INTERFERES, and that is a different piece of physics with a different
 * signature -- colour out of a colourless thing.
 *
 * WHERE THE COLOUR COMES FROM.  Light hits the front face of the film
 * and some reflects; the rest crosses the film, reflects off the back
 * face and comes back out.  The two reflections have travelled
 * different distances -- 2nd further, for a film of thickness d and
 * index n -- so for each wavelength they arrive in step or out of it:
 *
 *      phi(lambda) = 4 pi n d cos(theta) / lambda
 *      R(lambda)   = 4 R0 sin^2(phi / 2)
 *
 * with R0 = ((n-1)/(n+1))^2, the single-surface reflectance.  For soapy
 * water (n = 1.35) that is 2.2%, so a film sends back at most 9% of
 * what hits it.  THAT IS THE WHOLE EFFECT and there is nothing else in
 * it: no colour ramp, no palette, no hand-placed rainbow.  Feed the
 * formula a thickness and it gives back the colour that thickness is.
 *
 * WHAT THE FILM IS DOING WHILE YOU WATCH IT, in the order it matters:
 *
 *   1. IT DRAINS.  Gravity pulls the liquid down, so the film is a
 *      WEDGE -- thin at the top, thick at the bottom -- and the bands
 *      lie in horizontal stripes because a stripe is a line of constant
 *      thickness.  Getting this backwards, or leaving it out, is what
 *      makes a soap effect look like an oil slick.
 *
 *   2. THE COLOUR DRAINS OUT OF THE TOP BEFORE IT POPS.  As d falls
 *      below about a twentieth of a wavelength, phi goes to zero and
 *      sin^2(phi/2) goes to zero WITH IT, for every wavelength at once.
 *      The film stops interacting with light at all.
 *
 *      Seen in REFLECTION that is the famous black film -- the last
 *      thing a soap bubble does before it bursts.  Seen in
 *      TRANSMISSION, which is what a window shows, it is the same limit
 *      from the other side: the top of the film goes clear and the
 *      wallpaper behind it comes through untouched.  Either way it is
 *      not drawn in; it falls out of the formula when d gets small.
 *      Nothing else in this directory has a headline feature that is a
 *      limit.
 *
 *   3. IT SWIRLS UPWARD.  Counter-intuitively, thin patches RISE:
 *      thinner film is lighter, so it floats up through the thicker
 *      film around it.  ("Marginal regeneration" -- the patches detach
 *      from the edges, which is why the plumes here are strongest near
 *      the sides.)  A film whose bands only slide downwards is missing
 *      the thing that makes a real one hypnotic.
 *
 *   4. IT POPS.  A hole opens somewhere and the rim runs outward at the
 *      Taylor-Culick speed, sqrt(2 sigma / rho d) -- FASTER through
 *      thinner film, which is why the hole races once it reaches the
 *      black cap.  The liquid it sweeps up rides in the rim, so the rim
 *      is a thick bright band whose colour is several orders along.
 *
 * And one small thing that keeps it from reading as a decal: a wedge
 * DEVIATES what passes through it, by (n-1) times the wedge angle.  So
 * the wallpaper behind is displaced a little, in proportion to the
 * gradient of the thickness -- most where the bands are tightest, which
 * is exactly where a real film distorts most.
 */

#undef G_LOG_DOMAIN
#define G_LOG_DOMAIN "gowl-fx"

#include "gowl-fx-private.h"

#include <math.h>
#include <string.h>

static const gchar soap_vert_src[] =
	"attribute vec2 a_pos;\n"
	"attribute vec2 a_uv;\n"
	"varying vec2 v_uv;\n"
	"void main() {\n"
	"  v_uv = a_uv;\n"
	"  gl_Position = vec4(a_pos, 0.0, 1.0);\n"
	"}\n";

/*
 * highp where it exists.  It matters here for a specific reason: phi is
 * a thickness in NANOMETRES divided by a wavelength in nanometres and
 * multiplied by 4 pi, so it runs to thirty or forty radians at the
 * bottom of a fresh film.  At mediump the sine of that has lost most of
 * its meaning and the high-order bands turn to noise.
 */
static const gchar soap_frag_src[] =
	"#ifdef GL_FRAGMENT_PRECISION_HIGH\n"
	"precision highp float;\n"
	"#else\n"
	"precision mediump float;\n"
	"#endif\n"
	"\n"
	"uniform sampler2D u_soft;      /* the clouded wallpaper */\n"
	"uniform sampler2D u_sharp;     /* the same wallpaper, unblurred */\n"
	"uniform vec2  u_src_origin;\n"
	"uniform vec2  u_src_size;\n"
	"uniform float u_src_scale;\n"
	"uniform vec2  u_size;          /* the rect, px */\n"
	"uniform float u_radius;        /* corner radius, px */\n"
	"uniform float u_life;          /* which film, and how far through */\n"
	"uniform vec3  u_swirl_t;       /* three plume phases */\n"
	"uniform float u_thickness;     /* nm at the bottom, freshly blown */\n"
	"uniform float u_thin;          /* top/bottom thickness ratio */\n"
	"uniform float u_drain;         /* how fast it thins over its life */\n"
	"uniform float u_turbulence;    /* how much the plumes disturb it */\n"
	"uniform float u_swirl;         /* plume scale, cells across the pane */\n"
	"uniform float u_index;         /* refractive index of the liquid */\n"
	"uniform float u_gain;          /* amplification of a real 9% effect */\n"
	"uniform float u_sheen;         /* the room, reflected */\n"
	"uniform float u_wedge;         /* prism deviation, px per unit slope */\n"
	"uniform float u_dispersion;\n"
	"uniform float u_pop;           /* how much of the life is the pop */\n"
	"uniform float u_meniscus;      /* the thick border, 0..1 */\n"
	"uniform float u_fog;\n"
	"uniform float u_clarity;\n"
	"uniform vec3  u_light;\n"
	"uniform vec3  u_tint;\n"
	"uniform float u_brightness;\n"
	"uniform float u_alpha;\n"
	"uniform float u_seed;\n"
	"varying vec2 v_uv;\n"
	"\n"
	"#define PI 3.14159265359\n"
	"\n"
	"float hash1(vec2 c) {\n"
	"  return fract(sin(dot(c, vec2(127.1, 311.7))) * 43758.5453);\n"
	"}\n"
	"\n"
	"/* Value noise, smoothed with the quintic so its second derivative\n"
	"   is continuous -- the thickness field is DIFFERENTIATED below for\n"
	"   the wedge deviation, and a cubic-smoothed noise shows its cell\n"
	"   grid in that derivative even though it looks fine itself. */\n"
	"float vnoise(vec2 p) {\n"
	"  vec2 i = floor(p);\n"
	"  vec2 f = fract(p);\n"
	"  vec2 u = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);\n"
	"  float a = hash1(i);\n"
	"  float b = hash1(i + vec2(1.0, 0.0));\n"
	"  float c = hash1(i + vec2(0.0, 1.0));\n"
	"  float d = hash1(i + vec2(1.0, 1.0));\n"
	"  return mix(mix(a, b, u.x), mix(c, d, u.x), u.y);\n"
	"}\n"
	"\n"
	"/*\n"
	" * Three octaves, each ADVECTED UPWARD at its own rate.\n"
	" *\n"
	" * The rates come from the clock already wrapped, and they are\n"
	" * separate phases rather than one time times three constants for the\n"
	" * reason every animated shader here gives: a single phase scaled by\n"
	" * non-integers snaps at the wrap.\n"
	" */\n"
	"float plumes(vec2 p) {\n"
	"  float n  = vnoise(p + vec2(0.0, -u_swirl_t.x)) * 0.55;\n"
	"  n += vnoise(p * 2.13 + vec2(11.0, -u_swirl_t.y * 2.0)) * 0.30;\n"
	"  n += vnoise(p * 4.37 + vec2(23.0, -u_swirl_t.z * 4.0)) * 0.15;\n"
	"  return n;\n"
	"}\n"
	"\n"
	"/*\n"
	" * THE THICKNESS, in nanometres.  Everything visible comes from here.\n"
	" *\n"
	" * @age is 0 at the instant the film was blown and 1 when it is about\n"
	" * to pop.  @uv has y = 0 at the TOP.\n"
	" */\n"
	"float thickness(vec2 uv, float age) {\n"
	"  /* Gravity.  A draining vertical film settles into a profile close\n"
	"     to a power of the height; the exponent is not important, the\n"
	"     DIRECTION is -- thin at the top. */\n"
	"  float prof = mix(u_thin, 1.0, pow(clamp(uv.y, 0.0, 1.0), 0.55));\n"
	"\n"
	"  /* The Plateau border: liquid collects where the film meets its\n"
	"     frame, so there is a thick rim all the way round.  It is where\n"
	"     the highest-order bands live and where the plumes come from. */\n"
	"  float edge = min(min(uv.x, 1.0 - uv.x), min(uv.y, 1.0 - uv.y));\n"
	"  float bord = 1.0 - smoothstep(0.0, 0.10, edge);\n"
	"  prof += bord * u_meniscus * 1.6;\n"
	"\n"
	"  /* The whole film thins as it drains away underneath itself. */\n"
	"  float drain = exp(-age * u_drain);\n"
	"\n"
	"  /* Marginal regeneration: thin patches are lighter and rise.\n"
	"     Strongest near the sides, because that is where they detach. */\n"
	"  float side = 1.0 - smoothstep(0.0, 0.45, min(uv.x, 1.0 - uv.x));\n"
	"  float pl   = (plumes(uv * u_swirl + vec2(u_seed * 7.0, 0.0)) - 0.5);\n"
	"  float turb = u_turbulence * (0.55 + 0.9 * side);\n"
	"\n"
	"  return max(u_thickness * prof * drain * (1.0 + turb * pl), 0.0);\n"
	"}\n"
	"\n"
	"/*\n"
	" * The reflectance of the film at three wavelengths.\n"
	" *\n"
	" * Two-beam interference, which is the right model here because R0 is\n"
	" * small: the third and later internal reflections carry R0^2 of the\n"
	" * light, under a twentieth of a percent, and the Airy formula they\n"
	" * would need costs a division per channel to change nothing anybody\n"
	" * could see.\n"
	" */\n"
	"vec3 film_reflect(float d) {\n"
	"  /* Red, green and blue as actual wavelengths in nanometres, because\n"
	"     the thickness is in nanometres and the formula wants both. */\n"
	"  vec3  lambda = vec3(612.0, 549.0, 465.0);\n"
	"  float r0 = (u_index - 1.0) / (u_index + 1.0);\n"
	"  r0 = r0 * r0;\n"
	"  vec3 s = sin((2.0 * PI * u_index * d) / lambda);\n"
	"  /* 4 R0 sin^2(phi/2) with phi = 4 pi n d / lambda, which is the\n"
	"     same as 4 R0 sin^2(2 pi n d / lambda) -- one multiply saved and\n"
	"     the half-angle gone. */\n"
	"  return clamp(4.0 * r0 * s * s * u_gain, 0.0, 1.0);\n"
	"}\n"
	"\n"
	"/* A rounded-rect coverage mask, so the film ends where the window\n"
	"   does.  The same one every backdrop here uses. */\n"
	"float coverage(vec2 p) {\n"
	"  vec2  h = u_size * 0.5;\n"
	"  vec2  q = abs(p - h) - (h - vec2(u_radius));\n"
	"  float d = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - u_radius;\n"
	"  return 1.0 - smoothstep(-1.0, 0.5, d);\n"
	"}\n"
	"\n"
	"vec3 tap(vec2 p, float fog) {\n"
	"  vec2 uv = (u_src_origin + p * u_src_scale) / u_src_size;\n"
	"  uv = clamp(uv, vec2(0.0), vec2(1.0));\n"
	"  vec3 a = texture2D(u_sharp, uv).rgb;\n"
	"  vec3 b = texture2D(u_soft, uv).rgb;\n"
	"  return mix(a, b, clamp(fog, 0.0, 1.0));\n"
	"}\n"
	"\n"
	"void main() {\n"
	"  vec2  p   = v_uv * u_size;\n"
	"  float cov = coverage(p);\n"
	"\n"
	"  if (cov <= 0.001) {\n"
	"    gl_FragColor = vec4(0.0);\n"
	"    return;\n"
	"  }\n"
	"\n"
	"  vec2  uv  = clamp(v_uv, 0.0, 1.0);\n"
	"  float which = floor(u_life);\n"
	"  float age   = fract(u_life);\n"
	"\n"
	"  /*\n"
	"   * THE POP.\n"
	"   *\n"
	"   * The last @u_pop of a film's life.  A hole opens at a hashed\n"
	"   * point and its rim runs outward; outside the rim the film is\n"
	"   * untouched, inside it there is nothing but wallpaper.\n"
	"   *\n"
	"   * The rim ACCELERATES, which is real and is the thing that makes a\n"
	"   * pop look like a pop: Taylor-Culick gives v = sqrt(2 sigma / rho\n"
	"   * d), so the hole runs faster through thinner film -- and by the\n"
	"   * time it is popping, most of the film is thin.  A hole that\n"
	"   * expanded linearly reads as a wipe transition.\n"
	"   */\n"
	"  float intact = 1.0;\n"
	"  float rim    = 0.0;\n"
	"  float popped = max(age - (1.0 - u_pop), 0.0) / max(u_pop, 1e-3);\n"
	"  if (popped > 0.0) {\n"
	"    vec2  at = vec2(hash1(vec2(which, 3.1)), hash1(vec2(which, 7.7)));\n"
	"    at = at * 0.7 + 0.15;\n"
	"    float r  = popped * popped * 1.9;\n"
	"    float dd = distance(uv, at);\n"
	"    intact = smoothstep(r - 0.02, r, dd);\n"
	"    /* The liquid the rim sweeps up rides in it, so the rim is a\n"
	"       THICK band -- several interference orders along from the film\n"
	"       around it, which is why a popping bubble flashes colour at the\n"
	"       edge of the hole. */\n"
	"    rim = exp(-pow((dd - r) / 0.022, 2.0)) * intact;\n"
	"  }\n"
	"\n"
	"  /* Blown in over the first few percent of the life, so a new film\n"
	"     arrives rather than appearing. */\n"
	"  float born = smoothstep(0.0, 0.05, age);\n"
	"\n"
	"  float d  = thickness(uv, age) + rim * u_thickness * 0.9;\n"
	"\n"
	"  /*\n"
	"   * The wedge.  A film whose thickness varies across the pane is a\n"
	"   * prism, and a prism of angle alpha deviates what passes through it\n"
	"   * by (n-1) alpha.  So the wallpaper behind is displaced in\n"
	"   * proportion to the SLOPE of the thickness -- hardest where the\n"
	"   * bands are tightest, which is where a real film distorts most.\n"
	"   *\n"
	"   * Two extra thickness evaluations, which is the most expensive\n"
	"   * thing in this shader and the reason it is worth naming: without\n"
	"   * it the film is a decal and the eye knows.\n"
	"   */\n"
	"  vec2  e  = 1.5 / u_size;\n"
	"  float dx = thickness(uv + vec2(e.x, 0.0), age) - d;\n"
	"  float dy = thickness(uv + vec2(0.0, e.y), age) - d;\n"
	"  vec2  grad = vec2(dx, dy) / max(u_thickness, 1.0);\n"
	"  vec2  disp = grad * (u_index - 1.0) * u_wedge * intact;\n"
	"\n"
	"  vec3  R   = film_reflect(d) * intact * born;\n"
	"  float fog = u_fog * (1.0 - u_clarity) * intact;\n"
	"  vec3  col;\n"
	"\n"
	"  if (u_dispersion > 0.01) {\n"
	"    /* The prism splits as well as deviates, and by the same\n"
	"       arithmetic: the deviation goes as (n(lambda) - 1). */\n"
	"    vec2 sep = disp * u_dispersion * 0.08;\n"
	"    col.r = tap(p + disp - sep, fog).r;\n"
	"    col.g = tap(p + disp, fog).g;\n"
	"    col.b = tap(p + disp + sep, fog).b;\n"
	"  } else {\n"
	"    col = tap(p + disp, fog);\n"
	"  }\n"
	"\n"
	"  /*\n"
	"   * WHAT IS REFLECTED IS NOT TRANSMITTED, and the window is showing\n"
	"   * the transmitted half.\n"
	"   *\n"
	"   * Where the film sends back red, what comes through it is cyan,\n"
	"   * and the two are the same number: T = 1 - R.  Painting a rainbow\n"
	"   * on top would give the REFLECTION, which is what a photograph of\n"
	"   * a bubble shows and not what a window does.\n"
	"   *\n"
	"   * THE EXPOSURE IS NORMALISED OUT AND THE CHROMA IS NOT, which is\n"
	"   * a presentational choice and the only one in this shader.  A real\n"
	"   * film reflects at most nine percent, so its honest effect on a\n"
	"   * view through it is to darken it by a few percent and colour it\n"
	"   * by a few percent -- and a few percent of chroma behind a\n"
	"   * translucent window is nothing anybody would see.  Dividing by\n"
	"   * the mean transmission keeps the colour and throws away the\n"
	"   * dimming, so `soap-gain' buys visible bands instead of a window\n"
	"   * that has quietly gone dark.\n"
	"   *\n"
	"   * It also keeps the black-film limit intact: as the thickness goes\n"
	"   * to zero every R goes to zero together, so T is (1,1,1), the\n"
	"   * mean is 1, and the division changes nothing.\n"
	"   */\n"
	"  vec3  T   = vec3(1.0) - R;\n"
	"  float ex  = (T.r + T.g + T.b) / 3.0;\n"
	"  col *= T / max(ex, 1e-3);\n"
	"\n"
	"  /*\n"
	"   * AND THE HALF THAT BOUNCES OFF, which is what a soap bubble's\n"
	"   * famous colours actually are.\n"
	"   *\n"
	"   * You see a bubble's REFLECTION far more than you see what is\n"
	"   * behind it, because the room is usually brighter than the\n"
	"   * background -- which is why bubbles look most vivid against\n"
	"   * something dark.  That is not a nicety here: the transmitted\n"
	"   * half is a MULTIPLY, so over a dark wallpaper it has almost\n"
	"   * nothing to work with and the film goes invisible.  This term\n"
	"   * adds, so it does not care how dark the wallpaper is.\n"
	"   *\n"
	"   * The two are complements, so over a wallpaper of about the same\n"
	"   * brightness as the room they partly cancel -- which is also what\n"
	"   * really happens, and is why a film against an equally bright\n"
	"   * background is nearly invisible.  The exposure normalisation\n"
	"   * above amplifies the transmitted chroma relative to this one, so\n"
	"   * something survives at every background brightness.\n"
	"   */\n"
	"  col += vec3(0.92, 0.95, 1.00) * R * u_sheen;\n"
	"\n"
	"  /* A soap film is a liquid surface, so it catches a highlight like\n"
	"     one -- but a flat one, wide and weak, because there is no\n"
	"     curvature to concentrate it. */\n"
	"  {\n"
	"    vec3  n = normalize(vec3(grad * 6.0, 1.0));\n"
	"    float s = pow(max(dot(n, normalize(u_light + vec3(0.0, 0.0, 1.0))),\n"
	"                      0.0), 24.0);\n"
	"    col += vec3(s * 0.35 * u_sheen * intact * born);\n"
	"  }\n"
	"\n"
	"  col *= mix(vec3(1.0), u_tint, 0.25);\n"
	"  col = clamp(col * u_brightness, 0.0, 1.0);\n"
	"\n"
	"  float a = u_alpha * cov;\n"
	"  gl_FragColor = vec4(col * a, a);\n"
	"}\n";

static gboolean
soap_prog_ensure(GowlFxGl *self)
{
	GowlFxSoapProg *p = &self->soap;

	if (p->program != 0)
		return TRUE;
	if (self->soap_tried)
		return FALSE;
	self->soap_tried = TRUE;

	p->program = gowl_fx_link_program(soap_vert_src, soap_frag_src);
	if (p->program == 0) {
		g_warning("fx: the soap-film shader would not build, so soap "
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
	p->u_life       = glGetUniformLocation(p->program, "u_life");
	p->u_swirl_t    = glGetUniformLocation(p->program, "u_swirl_t");
	p->u_thickness  = glGetUniformLocation(p->program, "u_thickness");
	p->u_thin       = glGetUniformLocation(p->program, "u_thin");
	p->u_drain      = glGetUniformLocation(p->program, "u_drain");
	p->u_turbulence = glGetUniformLocation(p->program, "u_turbulence");
	p->u_swirl      = glGetUniformLocation(p->program, "u_swirl");
	p->u_index      = glGetUniformLocation(p->program, "u_index");
	p->u_gain       = glGetUniformLocation(p->program, "u_gain");
	p->u_sheen      = glGetUniformLocation(p->program, "u_sheen");
	p->u_wedge      = glGetUniformLocation(p->program, "u_wedge");
	p->u_dispersion = glGetUniformLocation(p->program, "u_dispersion");
	p->u_pop        = glGetUniformLocation(p->program, "u_pop");
	p->u_meniscus   = glGetUniformLocation(p->program, "u_meniscus");
	p->u_fog        = glGetUniformLocation(p->program, "u_fog");
	p->u_clarity    = glGetUniformLocation(p->program, "u_clarity");
	p->u_light      = glGetUniformLocation(p->program, "u_light");
	p->u_tint       = glGetUniformLocation(p->program, "u_tint");
	p->u_brightness = glGetUniformLocation(p->program, "u_brightness");
	p->u_alpha      = glGetUniformLocation(p->program, "u_alpha");
	p->u_seed       = glGetUniformLocation(p->program, "u_seed");
	p->a_pos        = glGetAttribLocation(p->program, "a_pos");
	p->a_uv         = glGetAttribLocation(p->program, "a_uv");
	return TRUE;
}

void
gowl_fx_soap_params_init(GowlFxSoapParams *params)
{
	if (params == NULL)
		return;

	memset(params, 0, sizeof(*params));
	/*
	 * A film a few seconds old.
	 *
	 * 900 nm at the bottom is about five interference orders from top to
	 * bottom -- the classic stack of bands, and thick enough that the
	 * top has somewhere to go before it turns black.  Much more and the
	 * bands at the bottom are finer than a pixel and alias into noise;
	 * much less and there is one band and no rainbow.
	 */
	params->thickness  = 900.0f;
	params->thin       = 0.10f;
	params->drain      = 1.30f;
	params->turbulence = 0.55f;
	params->swirl      = 3.20f;
	params->index      = 1.35f;   /* soapy water */
	params->gain       = 5.00f;
	params->sheen      = 0.55f;
	params->wedge      = 220.0f;
	params->dispersion = 0.70f;
	params->pop        = 0.10f;
	params->meniscus   = 0.55f;
	params->fog        = 0.30f;
	params->clarity    = 0.55f;
	params->brightness = 1.0f;
	params->alpha      = 1.0f;
	params->src_scale  = 1.0f;
	params->tint[0] = 1.00f;
	params->tint[1] = 1.00f;
	params->tint[2] = 1.00f;
	params->light[0] = -0.42f;
	params->light[1] = -0.55f;
	params->light[2] =  0.72f;
}

void
gowl_fx_soap_advance(GowlFxSoapClock *clock, gdouble dt, gdouble life_seconds,
                     gdouble swirl_speed)
{
	/*
	 * The three plume rates, irrational against each other for the
	 * reason every clock here gives: rates that look incommensurate and
	 * are really k/100 all return to zero together once every hundred
	 * seconds and the whole field repeats exactly.
	 */
	static const gdouble rate[3] = {
		0.1700000000000000, 0.1050724573839451, 0.0649275426160549
	};
	gint i;

	if (clock == NULL)
		return;
	if (!(dt > 0.0))
		return;
	if (dt > 0.25)
		dt = 0.25;   /* a stall is not a fast-forward */
	if (!(life_seconds > 0.5))
		life_seconds = 18.0;

	/*
	 * Cycles: the fraction is how far through its life this film is and
	 * the whole part is WHICH film, which is what the pop point is
	 * hashed from.  So the pane blows a new one every @life_seconds
	 * with a new hole in a new place, for as long as anybody watches.
	 */
	clock->life += dt / life_seconds;
	clock->life = fmod(clock->life, GOWL_FX_SOAP_CYCLES);
	if (clock->life < 0.0)
		clock->life += GOWL_FX_SOAP_CYCLES;

	for (i = 0; i < 3; i++) {
		clock->swirl[i] += dt * swirl_speed * rate[i];
		clock->swirl[i] = fmod(clock->swirl[i], GOWL_FX_SOAP_CYCLES);
		if (clock->swirl[i] < 0.0)
			clock->swirl[i] += GOWL_FX_SOAP_CYCLES;
	}
}

gboolean
gowl_fx_pass_soap(GowlFxPass             *pass,
                  const GowlFxTexture    *soft,
                  const GowlFxTexture    *sharp,
                  const GowlFxSoapParams *params,
                  const GowlFxSoapClock  *clock)
{
	GowlFxGl             *gl;
	const GowlFxSoapProg *p;
	const GowlFxTexture  *clear_src;
	GowlFxSoapClock       still;
	gfloat                swirl[3];
	gfloat                radius;
	gint                  i;

	if (pass == NULL || params == NULL || soft == NULL)
		return FALSE;
	if (params->width <= 0 || params->height <= 0)
		return FALSE;
	if (soft->tex == 0 || soft->width <= 0 || soft->height <= 0)
		return FALSE;

	gl = pass->gl;
	if (!soap_prog_ensure(gl))
		return FALSE;
	p = &gl->soap;

	if (clock == NULL) {
		memset(&still, 0, sizeof(still));
		clock = &still;
	}
	for (i = 0; i < 3; i++)
		swirl[i] = (gfloat)clock->swirl[i];

	radius = CLAMP(params->radius, 0.0f,
	               (gfloat)MIN(params->width, params->height) * 0.5f);
	clear_src = (sharp != NULL && sharp->tex != 0) ? sharp : soft;

	glUseProgram(p->program);
	glUniform2f(p->u_src_origin, params->src_origin[0], params->src_origin[1]);
	glUniform2f(p->u_src_size, (gfloat)MAX(1, soft->width),
	            (gfloat)MAX(1, soft->height));
	glUniform1f(p->u_src_scale,
	            params->src_scale > 0.0f ? params->src_scale : 1.0f);
	glUniform2f(p->u_size, (gfloat)params->width, (gfloat)params->height);
	glUniform1f(p->u_radius, radius);
	glUniform1f(p->u_life, (gfloat)clock->life);
	glUniform3fv(p->u_swirl_t, 1, swirl);
	/*
	 * THE THICKNESS IS CAPPED, and not for taste.
	 *
	 * The band spacing on screen is the pane's height divided by the
	 * number of orders, and the number of orders is the thickness over
	 * lambda/2n -- about 170 nm.  Past four or five thousand nanometres
	 * the bands at the bottom are thinner than a pixel and what is drawn
	 * is not a rainbow, it is aliasing: a moire that crawls with the
	 * clock and looks like a broken shader.  A real film that thick
	 * exists for under a second.
	 */
	glUniform1f(p->u_thickness, CLAMP(params->thickness, 60.0f, 3600.0f));
	glUniform1f(p->u_thin, CLAMP(params->thin, 0.0f, 1.0f));
	glUniform1f(p->u_drain, CLAMP(params->drain, 0.0f, 6.0f));
	glUniform1f(p->u_turbulence, CLAMP(params->turbulence, 0.0f, 2.0f));
	glUniform1f(p->u_swirl, CLAMP(params->swirl, 0.5f, 16.0f));
	glUniform1f(p->u_index, CLAMP(params->index, 1.05f, 2.0f));
	glUniform1f(p->u_gain, CLAMP(params->gain, 0.0f, 12.0f));
	glUniform1f(p->u_sheen, CLAMP(params->sheen, 0.0f, 2.0f));
	glUniform1f(p->u_wedge, CLAMP(params->wedge, 0.0f, 2000.0f));
	glUniform1f(p->u_dispersion, MAX(0.0f, params->dispersion));
	glUniform1f(p->u_pop, CLAMP(params->pop, 0.0f, 0.6f));
	glUniform1f(p->u_meniscus, CLAMP(params->meniscus, 0.0f, 2.0f));
	glUniform1f(p->u_fog, CLAMP(params->fog, 0.0f, 1.0f));
	glUniform1f(p->u_clarity, CLAMP(params->clarity, 0.0f, 1.0f));
	glUniform3fv(p->u_light, 1, params->light);
	glUniform3fv(p->u_tint, 1, params->tint);
	glUniform1f(p->u_brightness, MAX(0.0f, params->brightness));
	glUniform1f(p->u_alpha, CLAMP(params->alpha, 0.0f, 1.0f));
	glUniform1f(p->u_seed, (gfloat)fmod(fabs((gdouble)params->seed), 16.0));

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
