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
 * gowl-fx-water.c -- a moving water surface, refracted properly.
 *
 * The liquid glass next door traces a ray through a FIXED surface, so it
 * is drawn when the window moves and not otherwise.  This traces one
 * through a surface that is never the same twice, which changes
 * everything about the shape of the thing: there is a clock, there is a
 * frame rate, and the picture is stale the moment it is drawn.
 *
 * THE SURFACE IS A REAL HEIGHT FIELD, and everything else is derived from
 * it rather than painted on:
 *
 *   - Four directional waves, each shorter, weaker and turned away from
 *     the last, so the pattern never visibly repeats.  Sharpened towards
 *     the crest by `choppiness': a plain sine is a swell, and pulling the
 *     troughs flat and the crests narrow is what wind does to one.
 *   - Up to six expanding damped rings, which is what a calm pool or a
 *     fountain ACTUALLY is -- not a wave field but a few disturbances
 *     spreading out and dying.  Each picks a new place every time it
 *     restarts, because a drop that always lands on the same spot reads
 *     as a bug within about a minute.
 *   - The whole thing damped towards the window's edge, so the water is
 *     contained rather than sliced off.  The damping is INSIDE the height
 *     function, not applied after it, so its own slope is part of the
 *     surface: that slope is the meniscus where the water meets the glass.
 *
 * FIVE SAMPLES, THREE RESULTS.  The height is evaluated at the pixel and
 * four neighbours.  Central differences of those give the surface normal
 * (which is what refracts, and what catches the light), and the same five
 * give the Laplacian -- the curvature -- which is where the caustics come
 * from.  Light gathers where the surface is concave; that is what a
 * caustic is, and computing it from the curvature rather than painting a
 * pattern is why it moves correctly with the waves.
 *
 * Refraction is Snell's law at n = 1.333, the same machinery as the
 * glass: the ray bends towards the normal, crosses `depth' of water, and
 * lands somewhere else.  Where the surface is flat it lands where it
 * started, which is why still water shows the wallpaper undisturbed.
 *
 * THE CLOCK IS FOUR PHASES, NOT A TIME.  A single seconds-since-start
 * float loses its mantissa: after an hour a 32-bit float resolves about a
 * quarter of a second, and the waves visibly stutter.  Wrapping it makes
 * every wave jump at once, because the four are at incommensurate rates
 * and no wrap point is a whole number of cycles for all of them.  So the
 * caller accumulates each phase in a double and hands them over already
 * wrapped into [0, 2pi): exact in a float, forever, with no jump.  The
 * ripple clock is separate and wraps far out, where a repeat of the drop
 * positions is invisible.
 */

#undef G_LOG_DOMAIN
#define G_LOG_DOMAIN "gowl-fx"

#include "gowl-fx-private.h"

#include <math.h>
#include <string.h>

/* Refractive index of water. */
#define GOWL_FX_WATER_IOR (1.333)

static const gchar water_vert_src[] =
	"attribute vec2 a_pos;\n"
	"attribute vec2 a_uv;\n"
	"varying vec2 v_uv;\n"
	"void main() {\n"
	"  v_uv = a_uv;\n"
	"  gl_Position = vec4(a_pos, 0.0, 1.0);\n"
	"}\n";

/*
 * highp is asked for conditionally for the reason the glass shader gives:
 * naming it where it does not exist fails to COMPILE rather than falling
 * back.  The height field genuinely needs the range -- the wave phase is a
 * position in pixels times a frequency, and at mediump a wide window's
 * far corner quantises into visible steps.
 */
static const gchar water_frag_src[] =
	"#ifdef GL_FRAGMENT_PRECISION_HIGH\n"
	"precision highp float;\n"
	"#else\n"
	"precision mediump float;\n"
	"#endif\n"
	"\n"
	"uniform sampler2D u_soft;       /* the frosted wallpaper */\n"
	"uniform sampler2D u_sharp;      /* the same wallpaper, unblurred */\n"
	"uniform vec2  u_src_origin;     /* where this rect sits in them, px */\n"
	"uniform float u_src_scale;   /* source px per buffer px */\n"
	"uniform vec2  u_src_size;       /* their size, px */\n"
	"uniform vec2  u_size;           /* the rect, px */\n"
	"uniform float u_radius;         /* corner radius, px */\n"
	"uniform vec4  u_phase;          /* the four swell phases, 0..2pi */\n"
	"uniform float u_drop_t;         /* the ripple clock */\n"
	"uniform float u_amp;            /* wave height, px */\n"
	"uniform float u_wavelength;     /* of the longest wave, px */\n"
	"uniform float u_choppy;         /* crest sharpening, 0..1 */\n"
	"uniform float u_depth;          /* how far the ray travels, px */\n"
	"uniform float u_drops;          /* how many ripple sources, 0..6 */\n"
	"uniform float u_drop_amp;\n"
	"uniform float u_shore;          /* how far from the edge it calms, px */\n"
	"uniform float u_dispersion;     /* channel separation, px */\n"
	"uniform float u_specular;\n"
	"uniform float u_shine;          /* specular exponent */\n"
	"uniform float u_fresnel;\n"
	"uniform float u_reflect;        /* how far the fake reflection reaches */\n"
	"uniform float u_caustics;\n"
	"uniform float u_foam;\n"
	"uniform float u_meniscus;       /* light along the waterline */\n"
	"uniform vec3  u_light;\n"
	"uniform vec3  u_tint;\n"
	"uniform float u_absorb;\n"
	"uniform float u_clarity;        /* how unfrosted the water is, 0..1 */\n"
	"uniform float u_brightness;\n"
	"uniform float u_alpha;\n"
	"varying vec2 v_uv;\n"
	"\n"
	"const float N_WATER = 1.333;\n"
	"const float TAU = 6.2831853;\n"
	"\n"
	"/* Signed distance to a rounded rect centred in @size; negative inside. */\n"
	"float sdf_rect(vec2 p, vec2 size, float r) {\n"
	"  vec2 h = size * 0.5;\n"
	"  vec2 q = abs(p - h) - (h - vec2(r));\n"
	"  return min(max(q.x, q.y), 0.0) + length(max(q, vec2(0.0))) - r;\n"
	"}\n"
	"\n"
	"vec2 hash2(float n) {\n"
	"  return fract(vec2(sin(n * 12.9898), sin(n * 78.2330)) * 43758.5453);\n"
	"}\n"
	"\n"
	"/*\n"
	" * DOMAIN WARP.\n"
	" *\n"
	" * The octave below is a product of two one-dimensional patterns, so\n"
	" * left alone it is a GRID: cells in rows and columns, however the\n"
	" * domain is turned.  Rotating each octave hides it for one octave and\n"
	" * no further -- the eye finds the lattice anyway, and on a large\n"
	" * window it is the first thing it finds.\n"
	" *\n"
	" * Bending the domain before sampling is what breaks it: the rows stop\n"
	" * being rows.  Two sines is enough, one displacing x by y and one y by\n"
	" * x, which is the cheapest thing that is not separable.\n"
	" *\n"
	" * THE PHASE COEFFICIENTS MUST BE WHOLE NUMBERS.  Everything here is\n"
	" * 2pi-periodic in the phase, which is what makes the wrap in\n"
	" * gowl_fx_water_advance() exact; a phase multiplied by 1.31 inside a\n"
	" * sine would have period 2pi/1.31 and the whole surface would snap\n"
	" * sideways at every wrap.  1 and 2 are safe.  1.31 is not.\n"
	" */\n"
	"vec2 warp(vec2 uv, float amt, float ph) {\n"
	"  return uv + amt * vec2(sin(uv.y * 1.13 + ph),\n"
	"                         sin(uv.x * 0.97 - ph));\n"
	"}\n"
	"\n"
	"/*\n"
	" * ONE OCTAVE OF SURFACE, AND WHY IT IS BUILT THIS WAY.\n"
	" *\n"
	" * The obvious construction -- a sum of directional sine waves -- was\n"
	" * the first thing here, and it does not work.  A sine is a plane wave:\n"
	" * it varies along ONE direction and is constant along the other, so a\n"
	" * few of them summed and lit give regular parallel bands.  The window\n"
	" * came out looking like brushed metal.  No amount of retuning fixes\n"
	" * that, because the stripes are what a plane wave IS.\n"
	" *\n"
	" * This is Alexander Alekseev's construction (the `Seascape' shader,\n"
	" * MIT): take 1 - |sin| along each axis, blend it towards |cos| by\n"
	" * itself, and MULTIPLY the two axes together.  The product of two\n"
	" * one-dimensional patterns is a two-dimensional one -- a field of\n"
	" * cells rather than a set of bands -- and that is the whole difference\n"
	" * between this reading as water and reading as corrugated iron.\n"
	" * @choppy is an exponent: raising the result narrows the crests and\n"
	" * flattens the troughs, which is what wind does to a swell.\n"
	" */\n"
	"float sea_octave(vec2 uv, float choppy) {\n"
	"  vec2 wv  = 1.0 - abs(sin(uv));\n"
	"  vec2 swv = abs(cos(uv));\n"
	"  wv = mix(wv, swv, wv);\n"
	"  return pow(1.0 - pow(wv.x * wv.y, 0.65), choppy);\n"
	"}\n"
	"\n"
	"/*\n"
	" * Four iterations, two octaves each, the domain warped and then\n"
	" * turned and doubled between them.\n"
	" *\n"
	" * The two octaves of a pair run in OPPOSITE directions (+phase and\n"
	" * -phase).  One direction alone marches; two against each other churn,\n"
	" * which is what an open surface actually does.\n"
	" *\n"
	" * The warp is strongest on the largest octave and fades out, because a\n"
	" * warp on the finest detail is just noise: what has to stop looking\n"
	" * like a lattice is the structure big enough to see.\n"
	" *\n"
	" * NOTHING HERE IS EVALUATED OUTSIDE A SINE, and every phase enters one\n"
	" * with a whole-number coefficient.  That is what makes the phase wrap\n"
	" * in gowl_fx_water_advance() exact: a phase wrapped into [0, 2pi)\n"
	" * draws the identical surface.  An earlier version warped with value\n"
	" * noise, which is periodic on the integer lattice and not at 2pi at\n"
	" * all --- every wrap would have snapped the whole surface sideways.\n"
	" */\n"
	"float swell(vec2 p) {\n"
	"  mat2  turn   = mat2(1.6, 1.2, -1.2, 1.6);\n"
	"  /* Turned off the axes before the first octave, or the primary cells\n"
	"   * line up with the window's own edges. */\n"
	"  mat2  tilt   = mat2(0.8, 0.6, -0.6, 0.8);\n"
	"  vec2  uv     = tilt * (p * (TAU / max(u_wavelength, 4.0)));\n"
	"  float amp    = 1.0;\n"
	"  float wa     = 0.55;\n"
	"  float choppy = 1.0 + u_choppy * 3.0;\n"
	"  float h = 0.0, norm = 0.0, d;\n"
	"  vec2  w;\n"
	"\n"
	"  w  = warp(uv, wa, u_phase.x);\n"
	"  d  = sea_octave(w + u_phase.x, choppy);\n"
	"  d += sea_octave(w - u_phase.y, choppy);\n"
	"  h += d * amp; norm += 2.0 * amp;\n"
	"  uv = turn * uv; amp *= 0.34; wa *= 0.55;\n"
	"  choppy = mix(choppy, 1.0, 0.2);\n"
	"\n"
	"  w  = warp(uv, wa, u_phase.y);\n"
	"  d  = sea_octave(w + u_phase.y, choppy);\n"
	"  d += sea_octave(w - u_phase.z, choppy);\n"
	"  h += d * amp; norm += 2.0 * amp;\n"
	"  uv = turn * uv; amp *= 0.34; wa *= 0.55;\n"
	"  choppy = mix(choppy, 1.0, 0.2);\n"
	"\n"
	"  w  = warp(uv, wa, u_phase.z);\n"
	"  d  = sea_octave(w + u_phase.z, choppy);\n"
	"  d += sea_octave(w - u_phase.w, choppy);\n"
	"  h += d * amp; norm += 2.0 * amp;\n"
	"  uv = turn * uv; amp *= 0.34; wa *= 0.55;\n"
	"  choppy = mix(choppy, 1.0, 0.2);\n"
	"\n"
	"  w  = warp(uv, wa, u_phase.w);\n"
	"  d  = sea_octave(w + u_phase.w, choppy);\n"
	"  d += sea_octave(w - u_phase.x, choppy);\n"
	"  h += d * amp; norm += 2.0 * amp;\n"
	"\n"
	"  /* Every octave is non-negative, so without centring the surface has\n"
	"   * a constant offset and `how high is this crest' means nothing --\n"
	"   * which is what the foam threshold asks. */\n"
	"  return h / norm * 2.0 - 1.0;\n"
	"}\n"
	"\n"
	"/* Expanding damped rings.  A drop picks a NEW place every time it\n"
	" * restarts: one that always lands on the same spot reads as a bug\n"
	" * within about a minute. */\n"
	"float ripples(vec2 p) {\n"
	"  float h = 0.0;\n"
	"  float reach = max(u_size.x, u_size.y) * 0.75;\n"
	"  float ring_w = max(u_wavelength * 0.22, 6.0);\n"
	"  float ring_f = TAU / max(u_wavelength * 0.5, 4.0);\n"
	"  int   n = int(u_drops + 0.5);\n"
	"  int   i;\n"
	"  for (i = 0; i < 6; i++) {\n"
	"    if (i >= n) break;\n"
	"    float fi = float(i);\n"
	"    float clock = u_drop_t + fract(sin(fi * 3.77) * 1234.5);\n"
	"    float life = fract(clock);\n"
	"    vec2  c = u_size * (0.15 + 0.7 * hash2(fi + floor(clock) * 7.0));\n"
	"    float r = life * reach;\n"
	"    float d = distance(p, c) - r;\n"
	"    float x = d / ring_w;\n"
	"    h += sin(d * ring_f) * exp(-x * x) * exp(-2.5 * life);\n"
	"  }\n"
	"  return h * u_drop_amp;\n"
	"}\n"
	"\n"
	"/*\n"
	" * The surface.\n"
	" *\n"
	" * The shoreline damping is INSIDE this, not applied to the result,\n"
	" * because its own slope is part of the surface: that slope is the\n"
	" * meniscus where the water climbs the glass.  Applied afterwards the\n"
	" * water would end in a cliff with no normal to catch the light.\n"
	" */\n"
	"float height(vec2 p) {\n"
	"  float d = max(0.0, -sdf_rect(p, u_size, u_radius));\n"
	"  float shore = u_shore > 0.5 ? smoothstep(0.0, u_shore, d) : 1.0;\n"
	"  return (swell(p) + ripples(p)) * u_amp * shore;\n"
	"}\n"
	"\n"
	"/* One tap of the wallpaper, frosted or clear. */\n"
	"vec3 tap(vec2 px) {\n"
	"  vec2 uv = clamp((u_src_origin + px * u_src_scale) / u_src_size,\n"
	"                 vec2(0.0), vec2(1.0));\n"
	"  return mix(texture2D(u_soft, uv).rgb, texture2D(u_sharp, uv).rgb,\n"
	"             u_clarity);\n"
	"}\n"
	"\n"
	"void main() {\n"
	"  vec2  p   = v_uv * u_size;\n"
	"  float sd  = sdf_rect(p, u_size, u_radius);\n"
	"  float cov = clamp(0.5 - sd, 0.0, 1.0);\n"
	"  if (cov <= 0.0) {\n"
	"    gl_FragColor = vec4(0.0);\n"
	"    return;\n"
	"  }\n"
	"\n"
	"  /*\n"
	"   * Five samples: the pixel and its four neighbours.  Central\n"
	"   * differences of them are the surface normal, and the same five are\n"
	"   * the Laplacian -- the curvature the caustics come from.\n"
	"   *\n"
	"   * THE STEP FOLLOWS THE WAVE, it is not a fixed number of pixels.\n"
	"   * sea_octave() is built out of 1 - |sin|, which has a kink wherever\n"
	"   * the sine crosses zero, and the second derivative of a kink is a\n"
	"   * spike.  Sampled a pixel and a half apart the Laplacian found those\n"
	"   * spikes and drew them: the window came out covered in a grid of\n"
	"   * white crosses.  A step scaled to the finest octave steps OVER the\n"
	"   * kinks and measures the curvature that is actually there.\n"
	"   */\n"
	"  float e   = clamp(u_wavelength / 32.0, 1.5, 9.0);\n"
	"  float h   = height(p);\n"
	"  float hx1 = height(p + vec2(e, 0.0));\n"
	"  float hx0 = height(p - vec2(e, 0.0));\n"
	"  float hy1 = height(p + vec2(0.0, e));\n"
	"  float hy0 = height(p - vec2(0.0, e));\n"
	"\n"
	"  vec3 n = normalize(vec3(-(hx1 - hx0) / (2.0 * e),\n"
	"                          -(hy1 - hy0) / (2.0 * e), 1.0));\n"
	"  float lap = (hx1 + hx0 + hy1 + hy0 - 4.0 * h) / (e * e);\n"
	"\n"
	"  /* Snell, at n = 1.333.  The ray bends towards the normal, crosses\n"
	"   * `depth' of water, and lands somewhere else -- nowhere else at all\n"
	"   * where the surface is flat, which is why still water shows the\n"
	"   * wallpaper undisturbed. */\n"
	"  vec3  R = refract(vec3(0.0, 0.0, -1.0), n, 1.0 / N_WATER);\n"
	"  vec2  disp = R.xy * (u_depth / max(-R.z, 1e-3));\n"
	"\n"
	"  vec2 base = p + disp;\n"
	"  vec3 col;\n"
	"  if (u_dispersion > 0.01) {\n"
	"    vec2 sep = disp * (u_dispersion / max(u_depth, 1.0));\n"
	"    col.r = tap(base - sep).r;\n"
	"    col.g = tap(base).g;\n"
	"    col.b = tap(base + sep).b;\n"
	"  } else {\n"
	"    col = tap(base);\n"
	"  }\n"
	"\n"
	"  /* Water takes the red out first, which is why a deep pool is blue.\n"
	"   * A tint rather than a Beer-Lambert curve: there is no real depth\n"
	"   * here to integrate over, only a look. */\n"
	"  col = mix(col, col * u_tint, clamp(u_absorb, 0.0, 1.0));\n"
	"\n"
	"  /* Caustics from the curvature.  Light gathers where the surface is\n"
	"   * concave; computing it from the field rather than painting a\n"
	"   * pattern is why it moves correctly with the waves. */\n"
	"  if (u_caustics > 0.001) {\n"
	"    /* Normalised by the field's OWN characteristic curvature\n"
	"     * (amplitude over wavelength squared), so the knob means the same\n"
	"     * thing on a pool as on a storm.  A raw Laplacian does not: it\n"
	"     * scales with amplitude and inversely with wavelength squared, so\n"
	"     * one tuned setting would be invisible at one preset and a white\n"
	"     * sheet at another. */\n"
	"    float k = u_wavelength * u_wavelength / max(u_amp * 900.0, 1.0);\n"
	"    float caust = clamp(-lap * k, 0.0, 1.2) * u_caustics;\n"
	"    /* Light gathered, not light added: a caustic BRIGHTENS what is\n"
	"     * already there.  Added as white it fogs the dark parts of the\n"
	"     * wallpaper and the water stops looking transparent. */\n"
	"    col *= 1.0 + caust * (0.55 + 0.45 * u_tint);\n"
	"  }\n"
	"\n"
	"  /* Fresnel, with the reflection faked out of the wallpaper itself.\n"
	"   * Real water reflects what is around it; with only the wallpaper to\n"
	"   * hand, a piece of it dragged along the normal reads far better than\n"
	"   * a flat sky colour, because it MOVES with the waves. */\n"
	"  vec3  V = vec3(0.0, 0.0, 1.0);\n"
	"  float f = 0.02 + 0.98 * pow(1.0 - max(dot(n, V), 0.0), 5.0);\n"
	"  if (u_fresnel > 0.001)\n"
	"    col = mix(col, tap(p - n.xy * u_reflect),\n"
	"              clamp(f * u_fresnel, 0.0, 0.9));\n"
	"\n"
	"  /* The glint on the crests.  This is the single most water-like\n"
	"   * thing here: a surface that refracts but never catches the light\n"
	"   * reads as warped glass, not as a liquid. */\n"
	"  if (u_specular > 0.001) {\n"
	"    vec3  hv = normalize(u_light + V);\n"
	"    float s  = pow(max(dot(n, hv), 0.0), max(u_shine, 1.0));\n"
	"    col += vec3(s * u_specular);\n"
	"  }\n"
	"\n"
	"  /* Whitecaps, on the crests only, and never quite opaque. */\n"
	"  if (u_foam > 0.001) {\n"
	"    float crest = clamp(h / max(u_amp, 0.01), 0.0, 1.0);\n"
	"    col = mix(col, vec3(1.0),\n"
	"              clamp(smoothstep(0.55, 1.0, crest) * u_foam, 0.0, 0.85));\n"
	"  }\n"
	"\n"
	"  /* The waterline: where the surface climbs the edge it turns towards\n"
	"   * the viewer and brightens, which is what stops the window looking\n"
	"   * like a rectangle cut out of a pond. */\n"
	"  if (u_meniscus > 0.001)\n"
	"    col += vec3(exp(-max(0.0, -sd) / 3.5) * u_meniscus);\n"
	"\n"
	"  col = clamp(col * u_brightness, 0.0, 1.0);\n"
	"\n"
	"  float a = u_alpha * cov;\n"
	"  gl_FragColor = vec4(col * a, a);\n"
	"}\n";

/* ── The pass ────────────────────────────────────────────────────── */

/*
 * Built on first use, and its failure survivable, for the reason
 * gowl-fx-glass.c gives at length: a driver that cannot compile this must
 * cost the desktop the water alone, not the cube, the overview, the
 * switcher, the magnifier, the blur and the glass along with it.
 */
static gboolean
water_prog_ensure(GowlFxGl *self)
{
	GowlFxWaterProg *p = &self->water;

	if (p->program != 0)
		return TRUE;
	if (self->water_tried)
		return FALSE;
	self->water_tried = TRUE;

	p->program = gowl_fx_link_program(water_vert_src, water_frag_src);
	if (p->program == 0) {
		g_warning("fx: the liquid-water shader would not build, so water "
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
	p->u_phase      = glGetUniformLocation(p->program, "u_phase");
	p->u_drop_t     = glGetUniformLocation(p->program, "u_drop_t");
	p->u_amp        = glGetUniformLocation(p->program, "u_amp");
	p->u_wavelength = glGetUniformLocation(p->program, "u_wavelength");
	p->u_choppy     = glGetUniformLocation(p->program, "u_choppy");
	p->u_depth      = glGetUniformLocation(p->program, "u_depth");
	p->u_drops      = glGetUniformLocation(p->program, "u_drops");
	p->u_drop_amp   = glGetUniformLocation(p->program, "u_drop_amp");
	p->u_shore      = glGetUniformLocation(p->program, "u_shore");
	p->u_dispersion = glGetUniformLocation(p->program, "u_dispersion");
	p->u_specular   = glGetUniformLocation(p->program, "u_specular");
	p->u_shine      = glGetUniformLocation(p->program, "u_shine");
	p->u_fresnel    = glGetUniformLocation(p->program, "u_fresnel");
	p->u_reflect    = glGetUniformLocation(p->program, "u_reflect");
	p->u_caustics   = glGetUniformLocation(p->program, "u_caustics");
	p->u_foam       = glGetUniformLocation(p->program, "u_foam");
	p->u_meniscus   = glGetUniformLocation(p->program, "u_meniscus");
	p->u_light      = glGetUniformLocation(p->program, "u_light");
	p->u_tint       = glGetUniformLocation(p->program, "u_tint");
	p->u_absorb     = glGetUniformLocation(p->program, "u_absorb");
	p->u_clarity    = glGetUniformLocation(p->program, "u_clarity");
	p->u_brightness = glGetUniformLocation(p->program, "u_brightness");
	p->u_alpha      = glGetUniformLocation(p->program, "u_alpha");
	p->a_pos        = glGetAttribLocation(p->program, "a_pos");
	p->a_uv         = glGetAttribLocation(p->program, "a_uv");
	return TRUE;
}

void
gowl_fx_water_params_init(GowlFxWaterParams *params)
{
	if (params == NULL)
		return;

	memset(params, 0, sizeof(*params));
	/* A quiet pond: the default preset's numbers, so a caller that sets
	 * only a size gets something that looks like water rather than
	 * something that looks like nothing. */
	/*
	 * Amplitude is a SHARE OF WAVELENGTH in disguise: what reaches the
	 * refraction is the slope, roughly amplitude * 2pi / wavelength, and
	 * the displacement is depth * (1 - 1/1.333) * slope.  The first set of
	 * numbers tried here -- 2.2 px over a 190 px wave, 26 px deep -- work
	 * out to under HALF A PIXEL of displacement, which is to say no
	 * refraction at all.  Real waves run about a twentieth of their
	 * wavelength; these do too, over water deep enough to bend something.
	 */
	params->amplitude  = 6.6f;
	params->wavelength = 190.0f;
	params->choppiness = 0.35f;
	params->depth      = 260.0f;
	params->drops      = 2.0f;
	params->drop_amp   = 0.8f;
	params->shore      = 44.0f;
	params->dispersion = 0.8f;
	params->specular   = 0.55f;
	params->shine      = 48.0f;
	params->fresnel    = 0.45f;
	params->reflect    = 90.0f;
	params->caustics   = 0.5f;
	params->foam       = 0.0f;
	params->meniscus   = 0.16f;
	params->absorption = 0.35f;
	params->clarity    = 0.45f;
	params->brightness = 1.0f;
	params->alpha      = 1.0f;
	params->src_scale  = 1.0f;
	params->tint[0] = 0.62f;
	params->tint[1] = 0.86f;
	params->tint[2] = 1.0f;
	/* Up and to the left, and well above the surface: a light at the
	 * horizon puts the glints in a band rather than on the crests. */
	params->light[0] = -0.42f;
	params->light[1] = -0.55f;
	params->light[2] =  0.72f;
}

void
gowl_fx_water_advance(GowlFxWaterClock *clock, gdouble dt, gdouble speed)
{
	/*
	 * The four swell rates, deliberately incommensurate: at any rational
	 * ratio the pattern comes back round to itself, and on a surface
	 * somebody looks at all day that shows up as a slow pulse.
	 */
	static const gdouble rate[4] = { 1.00, 1.27, 1.61, 2.13 };
	gint i;

	if (clock == NULL)
		return;
	if (!(dt > 0.0))
		return;
	if (dt > 0.25)
		dt = 0.25;   /* a long stall is not a long wave */

	for (i = 0; i < 4; i++) {
		clock->phase[i] += dt * speed * rate[i];
		/*
		 * Wrapped HERE, in a double, into exactly the range a float
		 * represents well.  Handing the shader a seconds-since-start
		 * float instead loses the mantissa -- after an hour it resolves
		 * about a quarter of a second and the waves visibly stutter --
		 * and wrapping it there would jump all four at once, since no
		 * wrap point is a whole number of cycles for rates like these.
		 */
		clock->phase[i] = fmod(clock->phase[i], 2.0 * G_PI);
		if (clock->phase[i] < 0.0)
			clock->phase[i] += 2.0 * G_PI;
	}

	/* The ripple clock counts lifetimes rather than radians: its integer
	 * part is which drop this is and chooses where it lands.  Wrapped far
	 * out, where a repeat of the positions is invisible and a float still
	 * resolves a lifetime finely. */
	clock->drop += dt * speed;
	clock->drop = fmod(clock->drop, 1024.0);
	if (clock->drop < 0.0)
		clock->drop += 1024.0;
}

gboolean
gowl_fx_pass_water(GowlFxPass              *pass,
                   const GowlFxTexture     *soft,
                   const GowlFxTexture     *sharp,
                   const GowlFxWaterParams *params,
                   const GowlFxWaterClock  *clock)
{
	GowlFxGl              *gl;
	const GowlFxWaterProg *p;
	const GowlFxTexture   *clear_src;
	GowlFxWaterClock       still;
	gfloat                 phase[4];
	gfloat                 radius;
	gint                   i;

	if (pass == NULL || params == NULL || soft == NULL)
		return FALSE;
	if (params->width <= 0 || params->height <= 0)
		return FALSE;
	if (soft->tex == 0 || soft->width <= 0 || soft->height <= 0)
		return FALSE;

	gl = pass->gl;
	if (!water_prog_ensure(gl))
		return FALSE;
	p = &gl->water;

	if (clock == NULL) {
		memset(&still, 0, sizeof(still));
		clock = &still;
	}
	for (i = 0; i < 4; i++)
		phase[i] = (gfloat)clock->phase[i];

	radius = CLAMP(params->radius, 0.0f,
	               (gfloat)MIN(params->width, params->height) * 0.5f);

	clear_src = (sharp != NULL && sharp->tex != 0) ? sharp : soft;

	glUseProgram(p->program);
	glUniform2f(p->u_src_origin, params->src_origin[0], params->src_origin[1]);
	glUniform2f(p->u_src_size, (gfloat)MAX(1, soft->width),
	            (gfloat)MAX(1, soft->height));
	glUniform1f(p->u_src_scale, params->src_scale > 0.0f ? params->src_scale : 1.0f);
	glUniform2f(p->u_size, (gfloat)params->width, (gfloat)params->height);
	glUniform1f(p->u_radius, radius);
	glUniform4fv(p->u_phase, 1, phase);
	glUniform1f(p->u_drop_t, (gfloat)clock->drop);
	glUniform1f(p->u_amp, MAX(0.0f, params->amplitude));
	glUniform1f(p->u_wavelength, MAX(4.0f, params->wavelength));
	glUniform1f(p->u_choppy, CLAMP(params->choppiness, 0.0f, 1.0f));
	glUniform1f(p->u_depth, MAX(0.0f, params->depth));
	glUniform1f(p->u_drops, CLAMP(params->drops, 0.0f, 6.0f));
	glUniform1f(p->u_drop_amp, MAX(0.0f, params->drop_amp));
	glUniform1f(p->u_shore, MAX(0.0f, params->shore));
	glUniform1f(p->u_dispersion, MAX(0.0f, params->dispersion));
	glUniform1f(p->u_specular, MAX(0.0f, params->specular));
	glUniform1f(p->u_shine, MAX(1.0f, params->shine));
	glUniform1f(p->u_fresnel, MAX(0.0f, params->fresnel));
	glUniform1f(p->u_reflect, MAX(0.0f, params->reflect));
	glUniform1f(p->u_caustics, MAX(0.0f, params->caustics));
	glUniform1f(p->u_foam, CLAMP(params->foam, 0.0f, 1.0f));
	glUniform1f(p->u_meniscus, MAX(0.0f, params->meniscus));
	glUniform3fv(p->u_light, 1, params->light);
	glUniform3fv(p->u_tint, 1, params->tint);
	glUniform1f(p->u_absorb, CLAMP(params->absorption, 0.0f, 1.0f));
	glUniform1f(p->u_clarity, CLAMP(params->clarity, 0.0f, 1.0f));
	glUniform1f(p->u_brightness, MAX(0.0f, params->brightness));
	glUniform1f(p->u_alpha, CLAMP(params->alpha, 0.0f, 1.0f));

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
