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
 * gowl-fx-dew.c -- an orb web, strung with dew.
 *
 * THE OPTICS ARE THE RAIN'S AND THE PLACEMENT IS NOT.  A dew drop is the
 * same thing a raindrop is -- a ball of water, so a converging lens that
 * turns what is behind it upside down past a depth of two radii -- and
 * that arithmetic is worked out at length in gowl-fx-rain.c.  What is
 * different, and what the whole effect is, is WHERE the drops are: not
 * scattered over a pane but threaded onto a structure.
 *
 * THE STRUCTURE IS A REAL ONE.
 *
 *   RADIALS, from a hub that is NOT in the middle.  An orb weaver builds
 *   its hub above centre, because it sits head-down and drops on prey
 *   -- so a web with a centred hub looks like a logo and one with the
 *   hub high looks like a web.
 *
 *   A CAPTURE SPIRAL, Archimedean: the radius grows by a constant amount
 *   per turn, because the spider lays it by walking outward a body
 *   length at a time.
 *
 *   THE DEW IS ON THE SPIRAL AND NOT ON THE RADIALS, and that is not an
 *   economy.  The capture spiral is the sticky one -- it is coated in
 *   glue droplets and the radials are dry silk -- so water beads on it
 *   and runs off them.  Anybody who has looked at a wet web has seen
 *   exactly this and would not be able to say why the other way round
 *   looks wrong.
 *
 *   THE BEADS ARE EVENLY SPACED, which is the Rayleigh-Plateau
 *   instability and not a decision: a cylinder of liquid on a fibre is
 *   unstable to a wavelength a few times its own circumference, so a
 *   coated thread does not stay coated -- it breaks into a regular
 *   string of drops.  The spacing knob is that wavelength.
 *
 *   AND THE THREAD SAGS UNDER THEM.  A line loaded along its length
 *   hangs in a catenary, which over a short span is a parabola, and the
 *   sag goes as the load.  A web drawn with straight segments between
 *   radials is the single most common way this is drawn wrong.
 *
 * It breathes, too -- a web is under tension in moving air and never
 * quite still -- but slowly, because a web that swings is a web with
 * something caught in it.
 */

#undef G_LOG_DOMAIN
#define G_LOG_DOMAIN "gowl-fx"

#include "gowl-fx-private.h"

#include <math.h>
#include <string.h>

static const gchar dew_vert_src[] =
	"attribute vec2 a_pos;\n"
	"attribute vec2 a_uv;\n"
	"varying vec2 v_uv;\n"
	"void main() {\n"
	"  v_uv = a_uv;\n"
	"  gl_Position = vec4(a_pos, 0.0, 1.0);\n"
	"}\n";

static const gchar dew_frag_src[] =
	"#ifdef GL_FRAGMENT_PRECISION_HIGH\n"
	"precision highp float;\n"
	"#else\n"
	"precision mediump float;\n"
	"#endif\n"
	"\n"
	"uniform sampler2D u_soft;\n"
	"uniform sampler2D u_sharp;\n"
	"uniform vec2  u_src_origin;\n"
	"uniform vec2  u_src_size;\n"
	"uniform float u_src_scale;\n"
	"uniform vec2  u_size;\n"
	"uniform float u_radius;\n"
	"uniform vec3  u_sway_t;        /* three breathing phases */\n"
	"uniform float u_shimmer;       /* the per-drop glint clock */\n"
	"uniform float u_radials;       /* how many spokes */\n"
	"uniform float u_pitch;         /* px the spiral gains per turn */\n"
	"uniform float u_thread;        /* silk width, px */\n"
	"uniform float u_drop;          /* bead radius, px */\n"
	"uniform float u_spacing;       /* px between beads along the thread */\n"
	"uniform float u_sag;           /* px the loaded thread hangs */\n"
	"uniform float u_depth;         /* wallpaper distance, in drop radii */\n"
	"uniform float u_bulge;\n"
	"uniform float u_dispersion;\n"
	"uniform float u_silk;          /* how bright the dry thread is */\n"
	"uniform float u_glint;\n"
	"uniform float u_shine;\n"
	"uniform float u_rim;\n"
	"uniform float u_sway;          /* px the web breathes */\n"
	"uniform vec2  u_hub;           /* 0..1 of the pane */\n"
	"uniform float u_fog;\n"
	"uniform float u_clarity;\n"
	"uniform vec3  u_light;\n"
	"uniform vec3  u_tint;\n"
	"uniform float u_absorb;\n"
	"uniform float u_brightness;\n"
	"uniform float u_alpha;\n"
	"uniform float u_seed;\n"
	"varying vec2 v_uv;\n"
	"\n"
	"#define TAU 6.28318530718\n"
	"#define PI  3.14159265359\n"
	"\n"
	"vec2 hash2(vec2 c) {\n"
	"  vec2 n = vec2(dot(c, vec2(127.1, 311.7)), dot(c, vec2(269.5, 183.3)));\n"
	"  return fract(sin(n) * 43758.5453);\n"
	"}\n"
	"\n"
	"float hash1(vec2 c) {\n"
	"  return fract(sin(dot(c, vec2(127.1, 311.7))) * 43758.5453);\n"
	"}\n"
	"\n"
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
	"  /*\n"
	"   * THE BREATH.\n"
	"   *\n"
	"   * Applied to the QUERY POINT rather than to the web, which is the\n"
	"   * same thing and one subtraction instead of moving every thread.\n"
	"   * Three phases with no common period, so it never returns to where\n"
	"   * it started and never looks like a loop.\n"
	"   */\n"
	"  vec2 breath = vec2(sin(u_sway_t.x * TAU) * 0.7\n"
	"                     + sin(u_sway_t.y * TAU) * 0.3,\n"
	"                     sin(u_sway_t.z * TAU) * 0.55) * u_sway;\n"
	"\n"
	"  vec2  hub = u_hub * u_size;\n"
	"  vec2  q   = p - hub - breath;\n"
	"  float r   = length(q);\n"
	"  float a   = atan(q.y, q.x);\n"
	"\n"
	"  float n     = max(u_radials, 3.0);\n"
	"  float step  = TAU / n;\n"
	"\n"
	"  /*\n"
	"   * THE NEAREST RADIAL.\n"
	"   *\n"
	"   * Its angle is jittered per spoke: a spider does not measure, and\n"
	"   * a web whose spokes are exactly 360/n apart is a wheel.  The\n"
	"   * jitter is a fraction of the gap so neighbouring spokes cannot\n"
	"   * cross.\n"
	"   */\n"
	"  float ki    = floor(a / step + 0.5);\n"
	"  float jit   = (hash1(vec2(ki, u_seed * 31.0)) - 0.5) * step * 0.34;\n"
	"  float as    = ki * step + jit;\n"
	"  float d_rad = abs(r * sin(a - as));\n"
	"\n"
	"  /*\n"
	"   * THE NEAREST TURN OF THE CAPTURE SPIRAL.\n"
	"   *\n"
	"   * Archimedean: r = pitch * (angle / TAU).  Which turn a pixel is\n"
	"   * nearest is one rounding, so no loop over turns is needed.\n"
	"   */\n"
	"  float ang   = a;\n"
	"  float turns = (r / max(u_pitch, 1.0)) - ang / TAU;\n"
	"  float m     = floor(turns + 0.5);\n"
	"  float r_s   = u_pitch * (m + ang / TAU);\n"
	"\n"
	"  /*\n"
	"   * THE SAG.  The span between two radials is loaded along its\n"
	"   * length by the water on it, so it hangs -- a catenary, which over\n"
	"   * one span is a parabola to well within a pixel.  It is DOWNWARD\n"
	"   * in world terms, not outward: gravity does not know where the hub\n"
	"   * is.\n"
	"   */\n"
	"  float f     = fract((a - as) / step + 0.5);\n"
	"  float span  = 4.0 * f * (1.0 - f);        /* 0 at the radials, 1 between */\n"
	"  float sag   = u_sag * span;\n"
	"  float r_sag = length(q - vec2(0.0, sag)) - r_s;\n"
	"  float d_spi = abs(r_sag);\n"
	"\n"
	"  /*\n"
	"   * THE BEADS.\n"
	"   *\n"
	"   * Arc position along this turn, quantised to the Rayleigh-Plateau\n"
	"   * spacing.  Two candidates are tested -- the nearest bead and the\n"
	"   * one after it -- because a pixel between two of them is inside\n"
	"   * neither centre and a single candidate clips every drop in half\n"
	"   * along a line.\n"
	"   */\n"
	"  float bestc = 0.0;\n"
	"  vec2  local = vec2(0.0);\n"
	"  vec2  bestoff = vec2(0.0);\n"
	"  float bestr = 1.0;\n"
	"  float arc   = r_s * (ang + m * TAU);\n"
	"  float sp    = max(u_spacing, 8.0);\n"
	"  float bi;\n"
	"\n"
	"  /*\n"
	"   * THE THREAD\'S OWN AXES, in screen terms.\n"
	"   *\n"
	"   * A bead is measured ALONG the thread and ACROSS it, because that\n"
	"   * is the frame it is shaped in -- surface tension draws it out\n"
	"   * lengthwise.  But the LENS displaces what is behind it in SCREEN\n"
	"   * terms, and the spiral runs in a different direction at every\n"
	"   * point of it, so the two frames have to be kept apart.  Using the\n"
	"   * thread coordinates directly as a screen offset sends the image\n"
	"   * sideways by an amount that depends on where round the web the\n"
	"   * bead happens to sit, which looks like a lens everywhere except\n"
	"   * where you check it.\n"
	"   */\n"
	"  vec2 tang = vec2(-sin(a), cos(a));\n"
	"  vec2 norm = vec2(cos(a), sin(a));\n"
	"\n"
	"  for (bi = -1.0; bi <= 1.0; bi += 1.0) {\n"
	"    float idx = floor(arc / sp + 0.5) + bi;\n"
	"    vec2  hb  = hash2(vec2(idx, m + u_seed * 53.0));\n"
	"\n"
	"    /* Not every station carries a drop: some have run together and\n"
	"       some have fallen.  Two thirds is what a wet web looks like. */\n"
	"    if (hb.x > 0.66)\n"
	"      continue;\n"
	"\n"
	"    {\n"
	"      float da = (arc - idx * sp);\n"
	"      float rr = u_drop * (0.55 + 0.95 * hb.y);\n"
	"      /* A drop on a fibre is not a sphere: surface tension draws it\n"
	"         out ALONG the thread, so it is an ellipse about a third\n"
	"         longer than it is wide. */\n"
	"      vec2  d  = vec2(da / (rr * 1.35), r_sag / rr);\n"
	"      float l  = length(d);\n"
	"\n"
	"      if (l < 1.0 && (1.0 - l) > bestc) {\n"
	"        bestc = 1.0 - l;\n"
	"        local = d;\n"
	"        bestr = rr;\n"
	"        bestoff = da * tang + r_sag * norm;\n"
	"      }\n"
	"    }\n"
	"  }\n"
	"\n"
	"  /* Only inside the web, and only where the spiral has actually been\n"
	"     laid: an orb weaver leaves the hub bare. */\n"
	"  float inweb = smoothstep(u_pitch * 0.6, u_pitch * 1.4, r);\n"
	"  bestc *= inweb;\n"
	"\n"
	"  /*\n"
	"   * THE LENS.  Straight out of the rain: a sphere of water of radius\n"
	"   * rr with the wallpaper @u_depth radii behind it maps to a\n"
	"   * magnification of 1 - depth/2, so the sample is the point\n"
	"   * reflected through the drop's centre and scaled -- negative past\n"
	"   * a depth of two, which is the inversion, and which is the single\n"
	"   * most recognisable thing a drop does.\n"
	"   */\n"
	"  vec2  disp = vec2(0.0);\n"
	"  vec3  nrm  = vec3(0.0, 0.0, 1.0);\n"
	"  float l    = 0.0;\n"
	"\n"
	"  if (bestc > 0.0) {\n"
	"    l = min(length(local), 1.0);\n"
	"    nrm = normalize(vec3((local.x * tang + local.y * norm) * u_bulge,\n"
	"                         sqrt(max(1.0 - l * l, 1e-4))));\n"
	"    disp = -bestoff * (u_depth * 0.5);\n"
	"  }\n"
	"\n"
	"  float wet = clamp(bestc * 1.6, 0.0, 1.0);\n"
	"  float fog = u_fog * (1.0 - wet * u_clarity);\n"
	"  vec3  col;\n"
	"\n"
	"  if (u_dispersion > 0.01 && bestc > 0.0) {\n"
	"    vec2 sep = disp * u_dispersion * 0.06;\n"
	"    col.r = tap(p + disp - sep, fog).r;\n"
	"    col.g = tap(p + disp, fog).g;\n"
	"    col.b = tap(p + disp + sep, fog).b;\n"
	"  } else {\n"
	"    col = tap(p + disp, fog);\n"
	"  }\n"
	"\n"
	"  if (u_absorb > 0.001)\n"
	"    col = mix(col, col * u_tint, clamp(u_absorb * wet, 0.0, 1.0));\n"
	"\n"
	"  /*\n"
	"   * THE SILK.\n"
	"   *\n"
	"   * Thin, and BRIGHT rather than dark: spider silk is a transparent\n"
	"   * fibre a couple of microns across, so what reaches the eye from it\n"
	"   * is scattered light.  A web drawn in dark lines is a drawing of a\n"
	"   * web; a photograph of one is always pale threads on whatever is\n"
	"   * behind.\n"
	"   *\n"
	"   * The radials are dry and even; the spiral carries the water, so\n"
	"   * between its beads the thread is thinner and fainter.\n"
	"   */\n"
	"  {\n"
	"    float w   = max(u_thread, 0.5);\n"
	"    float rad = (1.0 - smoothstep(w * 0.5, w * 1.5, d_rad))\n"
	"                * smoothstep(u_pitch * 0.15, u_pitch * 0.5, r);\n"
	"    float spi = (1.0 - smoothstep(w * 0.4, w * 1.2, d_spi)) * inweb;\n"
	"\n"
	"    col += vec3(1.0, 0.99, 0.95) * (rad * 0.75 + spi * 0.55)\n"
	"           * u_silk * (1.0 - bestc);\n"
	"  }\n"
	"\n"
	"  /*\n"
	"   * The glint and the focus, as the rain has them: one tight spot\n"
	"   * where the surface normal bisects the light and the eye, and the\n"
	"   * gathered light in the middle that a lens produces however dark\n"
	"   * the thing behind it is.\n"
	"   *\n"
	"   * The shimmer is the extra: a drop on a thread that is breathing\n"
	"   * turns slightly, and the glint on a real wet web winks on and off\n"
	"   * as it does.\n"
	"   */\n"
	"  if (bestc > 0.0 && u_glint > 0.001) {\n"
	"    vec3  V  = vec3(0.0, 0.0, 1.0);\n"
	"    vec3  hv = normalize(u_light + V);\n"
	"    float s  = pow(max(dot(nrm, hv), 0.0), max(u_shine, 1.0));\n"
	"    float fc = pow(max(1.0 - l, 0.0), 3.0);\n"
	"    float wink = 0.65 + 0.35 * sin((u_shimmer + hash1(local + bestr))\n"
	"                                   * TAU);\n"
	"\n"
	"    col += vec3((s * wink + fc * 0.25) * u_glint);\n"
	"  }\n"
	"\n"
	"  /* The contact ring: light entering near the rim of a drop leaves\n"
	"     sideways and never reaches the eye. */\n"
	"  if (bestc > 0.0 && u_rim > 0.001)\n"
	"    col *= 1.0 - smoothstep(0.82, 1.0, l) * u_rim;\n"
	"\n"
	"  col = clamp(col * u_brightness, 0.0, 1.0);\n"
	"\n"
	"  float al = u_alpha * cov;\n"
	"  gl_FragColor = vec4(col * al, al);\n"
	"}\n";

static gboolean
dew_prog_ensure(GowlFxGl *self)
{
	GowlFxDewProg *p = &self->dew;

	if (p->program != 0)
		return TRUE;
	if (self->dew_tried)
		return FALSE;
	self->dew_tried = TRUE;

	p->program = gowl_fx_link_program(dew_vert_src, dew_frag_src);
	if (p->program == 0) {
		g_warning("fx: the dew shader would not build, so dew backdrops "
		          "will sit this session out");
		return FALSE;
	}

	p->u_soft       = glGetUniformLocation(p->program, "u_soft");
	p->u_sharp      = glGetUniformLocation(p->program, "u_sharp");
	p->u_src_origin = glGetUniformLocation(p->program, "u_src_origin");
	p->u_src_size   = glGetUniformLocation(p->program, "u_src_size");
	p->u_src_scale  = glGetUniformLocation(p->program, "u_src_scale");
	p->u_size       = glGetUniformLocation(p->program, "u_size");
	p->u_radius     = glGetUniformLocation(p->program, "u_radius");
	p->u_sway_t     = glGetUniformLocation(p->program, "u_sway_t");
	p->u_shimmer    = glGetUniformLocation(p->program, "u_shimmer");
	p->u_radials    = glGetUniformLocation(p->program, "u_radials");
	p->u_pitch      = glGetUniformLocation(p->program, "u_pitch");
	p->u_thread     = glGetUniformLocation(p->program, "u_thread");
	p->u_drop       = glGetUniformLocation(p->program, "u_drop");
	p->u_spacing    = glGetUniformLocation(p->program, "u_spacing");
	p->u_sag        = glGetUniformLocation(p->program, "u_sag");
	p->u_depth      = glGetUniformLocation(p->program, "u_depth");
	p->u_bulge      = glGetUniformLocation(p->program, "u_bulge");
	p->u_dispersion = glGetUniformLocation(p->program, "u_dispersion");
	p->u_silk       = glGetUniformLocation(p->program, "u_silk");
	p->u_glint      = glGetUniformLocation(p->program, "u_glint");
	p->u_shine      = glGetUniformLocation(p->program, "u_shine");
	p->u_rim        = glGetUniformLocation(p->program, "u_rim");
	p->u_sway       = glGetUniformLocation(p->program, "u_sway");
	p->u_hub        = glGetUniformLocation(p->program, "u_hub");
	p->u_fog        = glGetUniformLocation(p->program, "u_fog");
	p->u_clarity    = glGetUniformLocation(p->program, "u_clarity");
	p->u_light      = glGetUniformLocation(p->program, "u_light");
	p->u_tint       = glGetUniformLocation(p->program, "u_tint");
	p->u_absorb     = glGetUniformLocation(p->program, "u_absorb");
	p->u_brightness = glGetUniformLocation(p->program, "u_brightness");
	p->u_alpha      = glGetUniformLocation(p->program, "u_alpha");
	p->u_seed       = glGetUniformLocation(p->program, "u_seed");
	p->a_pos        = glGetAttribLocation(p->program, "a_pos");
	p->a_uv         = glGetAttribLocation(p->program, "a_uv");
	return TRUE;
}

void
gowl_fx_dew_params_init(GowlFxDewParams *params)
{
	if (params == NULL)
		return;

	memset(params, 0, sizeof(*params));
	params->radials    = 15.0f;
	params->pitch      = 74.0f;
	params->thread     = 1.3f;
	params->drop       = 9.0f;
	params->spacing    = 46.0f;
	params->sag        = 7.0f;
	params->depth      = 5.6f;
	params->bulge      = 1.0f;
	params->dispersion = 0.6f;
	/* The thread is PALE, not bright: at the first value tried the web
	 * read as a technical drawing over the wallpaper rather than as
	 * silk catching the light.  The beads carry the effect and they
	 * want the light, so the glint went up as the silk came down. */
	params->silk       = 0.30f;
	params->glint      = 1.25f;
	params->shine      = 58.0f;
	params->rim        = 0.26f;
	params->sway       = 3.2f;
	params->hub[0]     = 0.48f;
	/* Above centre, because an orb weaver sits head-down at the hub and
	 * builds it high.  A centred hub reads as a logo. */
	params->hub[1]     = 0.38f;
	params->fog        = 0.34f;
	params->clarity    = 0.92f;
	params->absorption = 0.06f;
	params->brightness = 1.0f;
	params->alpha      = 1.0f;
	params->src_scale  = 1.0f;
	params->tint[0] = 0.96f;
	params->tint[1] = 0.99f;
	params->tint[2] = 1.00f;
	params->light[0] = -0.42f;
	params->light[1] = -0.55f;
	params->light[2] =  0.72f;
}

void
gowl_fx_dew_advance(GowlFxDewClock *clock, gdouble dt, gdouble speed)
{
	/* Slow, and irrational against each other.  A web under tension in
	 * moving air is never quite still and never obviously moving; rates
	 * that shared a period would make it a pendulum. */
	static const gdouble rate[3] = {
		0.0730000000000000, 0.0451246117974981, 0.0278753882025019
	};
	gint i;

	if (clock == NULL)
		return;
	if (!(dt > 0.0))
		return;
	if (dt > 0.25)
		dt = 0.25;

	for (i = 0; i < 3; i++) {
		clock->sway[i] += dt * speed * rate[i];
		clock->sway[i] = fmod(clock->sway[i], GOWL_FX_DEW_CYCLES);
		if (clock->sway[i] < 0.0)
			clock->sway[i] += GOWL_FX_DEW_CYCLES;
	}

	clock->shimmer += dt * speed * 0.117;
	clock->shimmer = fmod(clock->shimmer, GOWL_FX_DEW_CYCLES);
	if (clock->shimmer < 0.0)
		clock->shimmer += GOWL_FX_DEW_CYCLES;
}

gboolean
gowl_fx_pass_dew(GowlFxPass            *pass,
                 const GowlFxTexture   *soft,
                 const GowlFxTexture   *sharp,
                 const GowlFxDewParams *params,
                 const GowlFxDewClock  *clock)
{
	GowlFxGl            *gl;
	const GowlFxDewProg *p;
	const GowlFxTexture *clear_src;
	GowlFxDewClock       still;
	gfloat               sway[3];
	gfloat               radius, pitch, spacing;
	gint                 i;

	if (pass == NULL || params == NULL || soft == NULL)
		return FALSE;
	if (params->width <= 0 || params->height <= 0)
		return FALSE;
	if (soft->tex == 0 || soft->width <= 0 || soft->height <= 0)
		return FALSE;

	gl = pass->gl;
	if (!dew_prog_ensure(gl))
		return FALSE;
	p = &gl->dew;

	if (clock == NULL) {
		memset(&still, 0, sizeof(still));
		clock = &still;
	}
	for (i = 0; i < 3; i++)
		sway[i] = (gfloat)clock->sway[i];

	radius  = CLAMP(params->radius, 0.0f,
	                (gfloat)MIN(params->width, params->height) * 0.5f);
	pitch   = MAX(16.0f, params->pitch);
	spacing = MAX(8.0f, params->spacing);

	clear_src = (sharp != NULL && sharp->tex != 0) ? sharp : soft;

	glUseProgram(p->program);
	glUniform2f(p->u_src_origin, params->src_origin[0], params->src_origin[1]);
	glUniform2f(p->u_src_size, (gfloat)MAX(1, soft->width),
	            (gfloat)MAX(1, soft->height));
	glUniform1f(p->u_src_scale,
	            params->src_scale > 0.0f ? params->src_scale : 1.0f);
	glUniform2f(p->u_size, (gfloat)params->width, (gfloat)params->height);
	glUniform1f(p->u_radius, radius);
	glUniform3fv(p->u_sway_t, 1, sway);
	glUniform1f(p->u_shimmer, (gfloat)clock->shimmer);
	glUniform1f(p->u_radials, CLAMP(params->radials, 3.0f, 48.0f));
	glUniform1f(p->u_pitch, pitch);
	glUniform1f(p->u_thread, CLAMP(params->thread, 0.4f, 12.0f));
	/*
	 * A BEAD MUST FIT BETWEEN ITS NEIGHBOURS AND INSIDE ITS TURN.
	 *
	 * Two bounds, both arithmetic.  Along the thread the shader tests
	 * the nearest bead and one either side, and a bead is 1.35 times
	 * longer than it is wide, so half the spacing is the most it can be
	 * -- past that the drops merge into a sausage, which is what a real
	 * over-loaded thread does but is not what anybody set `dew-drop'
	 * expecting.  Across the thread the drop must not reach the next
	 * turn of the spiral, or the turns weld together into a disc.
	 */
	glUniform1f(p->u_drop, CLAMP(params->drop, 0.5f,
	                             MIN(spacing * 0.37f, pitch * 0.40f)));
	glUniform1f(p->u_spacing, spacing);
	/* And the sag cannot reach the next turn either. */
	glUniform1f(p->u_sag, CLAMP(params->sag, 0.0f, pitch * 0.30f));
	glUniform1f(p->u_depth, CLAMP(params->depth, 0.0f, 20.0f));
	glUniform1f(p->u_bulge, CLAMP(params->bulge, 0.0f, 3.0f));
	glUniform1f(p->u_dispersion, MAX(0.0f, params->dispersion));
	glUniform1f(p->u_silk, CLAMP(params->silk, 0.0f, 3.0f));
	glUniform1f(p->u_glint, MAX(0.0f, params->glint));
	glUniform1f(p->u_shine, MAX(1.0f, params->shine));
	glUniform1f(p->u_rim, CLAMP(params->rim, 0.0f, 1.0f));
	glUniform1f(p->u_sway, CLAMP(params->sway, 0.0f, 40.0f));
	glUniform2f(p->u_hub, CLAMP(params->hub[0], 0.05f, 0.95f),
	            CLAMP(params->hub[1], 0.05f, 0.95f));
	glUniform1f(p->u_fog, CLAMP(params->fog, 0.0f, 1.0f));
	glUniform1f(p->u_clarity, CLAMP(params->clarity, 0.0f, 1.0f));
	glUniform3fv(p->u_light, 1, params->light);
	glUniform3fv(p->u_tint, 1, params->tint);
	glUniform1f(p->u_absorb, CLAMP(params->absorption, 0.0f, 1.0f));
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
