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
 * gowl-fx-embers.c -- a fire below the window, seen through its heat.
 *
 * The thermal opposite of the snow, and it brings two mechanisms that
 * nothing else here has.
 *
 * 1. THE COLOUR IS A TEMPERATURE.
 *
 *    An ember is not orange because somebody picked orange.  It is a
 *    lump of carbon at a temperature, glowing as a black body, and the
 *    colour of a black body is a function of that one number: around
 *    2300 K at the top of the fire, cooling as it rises, dull red at
 *    1000 K, invisible below about 800.  The Planckian locus is
 *    evaluated here directly, so the whole palette of the effect is one
 *    cooling curve and nobody chose any of it.
 *
 *    AND THE BRIGHTNESS GOES AS THE FOURTH POWER.  Stefan-Boltzmann:
 *    radiated power is proportional to T^4, so an ember that has cooled
 *    by half is SIXTEEN TIMES dimmer.  That is why a fire's sparks wink
 *    out so sharply instead of fading gently, and it is the difference
 *    between this and a particle system with an alpha ramp.
 *
 * 2. THE AIR ABOVE A FIRE IS NOT A LENS.
 *
 *    Hot air is less dense, so its refractive index is lower, and the
 *    air above a fire is a turbulent MIXTURE of hot and less hot.  What
 *    that does to a view through it is not a lens and not a blur: it is
 *    a domain warp -- the image is displaced by a smoothly varying,
 *    slowly drifting random field, so straight lines wobble rather than
 *    bending one way.  Strongest just above the source and weakening
 *    with height as the plume spreads and mixes.
 *
 *    Everything else in this directory displaces the sample by the
 *    geometry of a surface.  This one displaces it by noise, and that
 *    is a different look -- shimmer, not refraction.
 *
 * THE REST, in the order the eye notices:
 *
 *   EMBERS DECELERATE.  A spark leaves the fire fast and slows: drag
 *   rises with speed, and as it cools it loses the buoyancy that was
 *   carrying it.  A field of particles at constant speed reads as rain
 *   going the wrong way.
 *
 *   THEY FLICKER.  Combustion is unsteady and a spark is small enough
 *   that its own temperature wanders.  Per ember, not per frame -- a
 *   global flicker is a lamp with a loose connection.
 *
 *   SOME OF THEM ARE ASH.  A fraction have burnt out: dark, cold, and
 *   FALLING rather than rising.  Two directions on screen at once is
 *   most of what makes it read as a fire rather than as an effect.
 *
 * WHY IT IS NOT A GRID.  The same discipline as the fizz next door,
 * which this borrows its layout from wholesale: three column layers at
 * widths that do not divide each other, a sparse hashed subset of
 * columns carrying anything at all, jitter within the column, and a
 * per-window seed translating the whole field.  The structural problem
 * was solved there; only the physics here is new.
 */

#undef G_LOG_DOMAIN
#define G_LOG_DOMAIN "gowl-fx"

#include "gowl-fx-private.h"

#include <math.h>
#include <string.h>

static const gchar embers_vert_src[] =
	"attribute vec2 a_pos;\n"
	"attribute vec2 a_uv;\n"
	"varying vec2 v_uv;\n"
	"void main() {\n"
	"  v_uv = a_uv;\n"
	"  gl_Position = vec4(a_pos, 0.0, 1.0);\n"
	"}\n";

static const gchar embers_frag_src[] =
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
	"uniform vec3  u_rise;          /* three ember clocks, cycles */\n"
	"uniform vec3  u_haze_t;        /* three haze phases */\n"
	"uniform float u_column;        /* px per column of the ember layer */\n"
	"uniform float u_density;       /* how many columns carry embers */\n"
	"uniform float u_ember;         /* ember radius in px */\n"
	"uniform float u_spacing;       /* embers per cycle per column */\n"
	"uniform float u_sway;          /* how far one wanders sideways, px */\n"
	"uniform float u_drag;          /* how hard it decelerates */\n"
	"uniform float u_temperature;   /* kelvin at birth */\n"
	"uniform float u_cool;          /* how fast it cools over its rise */\n"
	"uniform float u_flicker;       /* 0..1 */\n"
	"uniform float u_ash;           /* fraction that are dark and falling */\n"
	"uniform float u_glow;          /* the halo around each ember */\n"
	"uniform float u_hearth;        /* the fire's own light on the pane */\n"
	"uniform float u_haze;          /* heat shimmer, px of displacement */\n"
	"uniform float u_haze_scale;    /* cells of shimmer across the pane */\n"
	"uniform float u_fog;\n"
	"uniform float u_clarity;\n"
	"uniform vec3  u_tint;\n"
	"uniform float u_brightness;\n"
	"uniform float u_alpha;\n"
	"uniform float u_seed;\n"
	"varying vec2 v_uv;\n"
	"\n"
	"#define PI 3.14159265359\n"
	"/* A wood fire, in kelvin: the temperature a spark of brightness 1\n"
	"   leaves at.  Everything hotter is brighter and everything cooler is\n"
	"   dimmer, by the fourth power, and this is where that scale is\n"
	"   anchored. */\n"
	"#define EMBER_REF 2300.0\n"
	"\n"
	"vec3 hash3v(vec3 c) {\n"
	"  vec3 n = vec3(dot(c, vec3(127.1,  89.3,  54.7)),\n"
	"                dot(c, vec3( 71.9, 151.3, 101.7)),\n"
	"                dot(c, vec3(167.3,  61.7, 133.9)));\n"
	"  return fract(sin(n) * vec3(43758.5453, 28001.8384, 19349.6631));\n"
	"}\n"
	"\n"
	"float hash1(vec2 c) {\n"
	"  return fract(sin(dot(c, vec2(127.1, 311.7))) * 43758.5453);\n"
	"}\n"
	"\n"
	"float vnoise(vec2 p) {\n"
	"  vec2 i = floor(p);\n"
	"  vec2 f = fract(p);\n"
	"  vec2 u = f * f * (3.0 - 2.0 * f);\n"
	"  float a = hash1(i);\n"
	"  float b = hash1(i + vec2(1.0, 0.0));\n"
	"  float c = hash1(i + vec2(0.0, 1.0));\n"
	"  float d = hash1(i + vec2(1.0, 1.0));\n"
	"  return mix(mix(a, b, u.x), mix(c, d, u.x), u.y);\n"
	"}\n"
	"\n"
	"/*\n"
	" * THE COLOUR OF A BLACK BODY AT @t KELVIN.\n"
	" *\n"
	" * A fit to the Planckian locus over the range an ember lives in.\n"
	" * Red saturates below about 6500 K, so it is simply 1; green and\n"
	" * blue are logarithmic in the temperature, and blue does not appear\n"
	" * at all below 1900 K -- which is exactly why a cooling spark slides\n"
	" * from yellow-orange to a deep red and never through pink.\n"
	" */\n"
	"vec3 blackbody(float t) {\n"
	"  float k = clamp(t, 700.0, 6500.0) / 100.0;\n"
	"  float g = clamp(0.39008158 * log(k) - 0.63184144, 0.0, 1.0);\n"
	"  float b = k <= 19.0 ? 0.0\n"
	"          : clamp(0.54320679 * log(k - 10.0) - 1.19625409, 0.0, 1.0);\n"
	"  return vec3(1.0, g, b);\n"
	"}\n"
	"\n"
	"/*\n"
	" * How far up an ember of age @u has got, 0 to 1.\n"
	" *\n"
	" * It DECELERATES: drag grows with speed and buoyancy falls with\n"
	" * temperature, so the sparks are quick at the bottom and crawl near\n"
	" * the top.  An exponential approach is the closed form of a linear\n"
	" * drag and costs one exp().\n"
	" */\n"
	"float rise_h(float u, float k) {\n"
	"  float kk = max(k, 0.05);\n"
	"  return (1.0 - exp(-kk * u)) / (1.0 - exp(-kk));\n"
	"}\n"
	"\n"
	"/* Its inverse, so a pixel can ask which ember might be at its height\n"
	"   rather than looping over all of them. */\n"
	"float rise_u(float h, float k) {\n"
	"  float kk = max(k, 0.05);\n"
	"  float e  = 1.0 - exp(-kk);\n"
	"  return -log(max(1.0 - clamp(h, 0.0, 0.9999) * e, 1e-4)) / kk;\n"
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
	"/*\n"
	" * One column layer of embers.\n"
	" *\n"
	" * @acc is (light, glow) accumulated so far; this adds to it.  Embers\n"
	" * are EMISSIVE, so unlike every lens in this directory they simply\n"
	" * add and there is no best-wins accumulator: two sparks overlapping\n"
	" * are brighter, which is what light does.\n"
	" *\n"
	" * Three columns are asked, not one.  The sway moves an ember\n"
	" * sideways by up to a quarter of a column (capped on the CPU), so a\n"
	" * pixel near a boundary can be inside a spark belonging to the\n"
	" * column next door -- and a version that only asks its own column\n"
	" * slices every one of them down a straight vertical line.\n"
	" */\n"
	"void ember_layer(vec2 p, float width, float t, float layer,\n"
	"                 inout vec3 light, inout float glow) {\n"
	"  float col0 = floor(p.x / width);\n"
	"  float i;\n"
	"\n"
	"  for (i = -1.0; i <= 1.0; i += 1.0) {\n"
	"    float col = col0 + i;\n"
	"    vec3  hc  = hash3v(vec3(col, layer, 3.0));\n"
	"\n"
	"    if (hc.x > u_density)\n"
	"      continue;\n"
	"\n"
	"    /* How many embers this column lets go per cycle.  A WHOLE\n"
	"       number, for the reason every cycle clock here carries: a\n"
	"       non-integer multiplier makes the emission index jump at the\n"
	"       wrap and the whole column restarts. */\n"
	"    float per = floor(1.0 + hc.y * 3.0 + u_spacing * 4.0);\n"
	"    float cx  = (col + 0.25 + hc.z * 0.5) * width;\n"
	"\n"
	"    /* Which ember could be at this height, and its neighbours. */\n"
	"    float hh = clamp(1.0 - p.y / u_size.y, 0.0, 1.0);\n"
	"    float un = rise_u(hh, u_drag);\n"
	"    float e0 = floor(t * per - un);\n"
	"    float j;\n"
	"\n"
	"    for (j = 0.0; j <= 1.0; j += 1.0) {\n"
	"      float idx = e0 + j;\n"
	"      float u   = t * per - idx;          /* age, in cycles */\n"
	"      vec3  he  = hash3v(vec3(col, layer, idx));\n"
	"\n"
	"      if (u < 0.0 || u > 1.0)\n"
	"        continue;\n"
	"\n"
	"      /* Ash: burnt out, so it FALLS, and it is not emitting. */\n"
	"      float is_ash = he.x < u_ash ? 1.0 : 0.0;\n"
	"      float h = is_ash > 0.5 ? (1.0 - u) * 0.85 : rise_h(u, u_drag);\n"
	"      float y = u_size.y * (1.0 - h);\n"
	"\n"
	"      /* Sideways: advected turbulence, so neighbours in a column\n"
	"         drift together rather than each picking its own way. */\n"
	"      float sw = (vnoise(vec2(cx * 0.01, h * 4.0 - u_haze_t.x)) - 0.5);\n"
	"      float x  = cx + sw * 2.0 * u_sway;\n"
	"\n"
	"      float r  = u_ember * (0.55 + 0.9 * he.y);\n"
	"      vec2  d  = (p - vec2(x, y)) / max(r, 0.5);\n"
	"      float l2 = dot(d, d);\n"
	"\n"
	"      if (l2 > 36.0)\n"
	"        continue;\n"
	"\n"
	"      if (is_ash > 0.5) {\n"
	"        /* A dark fleck.  Subtracted rather than added, because that\n"
	"           is what something opaque in front of the wallpaper does. */\n"
	"        float cover = 1.0 - smoothstep(0.7, 1.0, sqrt(l2));\n"
	"        light -= vec3(cover * 0.55);\n"
	"        continue;\n"
	"      }\n"
	"\n"
	"      {\n"
	"        /*\n"
	"         * Cooling, and the fourth power that goes with it.\n"
	"         *\n"
	"         * AGAINST A FIXED REFERENCE and not against u_temperature.\n"
	"         * Dividing by the knob would normalise the knob away: a\n"
	"         * fire set to 1200 K would be exactly as bright as one set\n"
	"         * to 2400, which is the opposite of what Stefan-Boltzmann\n"
	"         * says and would make the setting a hue control.  EMBER_REF\n"
	"         * is a wood fire, so 1 is a spark straight out of one.\n"
	"         */\n"
	"        float T0 = u_temperature * (0.80 + 0.40 * he.z);\n"
	"        float T  = T0 * exp(-u_cool * u);\n"
	"        float rel = clamp(T / EMBER_REF, 0.0, 1.6);\n"
	"        float pw  = rel * rel * rel * rel;\n"
	"\n"
	"        /* Per-ember flicker: combustion is unsteady, and a spark is\n"
	"           small enough for its own temperature to wander. */\n"
	"        float fl = 1.0 + u_flicker *\n"
	"                   (vnoise(vec2(idx * 3.7 + col, u * 9.0)) - 0.5);\n"
	"\n"
	"        vec3  c  = blackbody(T) * pw * max(fl, 0.0);\n"
	"        float core = 1.0 - smoothstep(0.6, 1.05, sqrt(l2));\n"
	"        float halo = exp(-l2 * 0.14);\n"
	"\n"
	"        light += c * core;\n"
	"        glow  += (c.r + c.g) * 0.5 * halo * u_glow;\n"
	"      }\n"
	"    }\n"
	"  }\n"
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
	"  vec2  sp = p + vec2(u_seed * 311.0, 0.0);\n"
	"  float up = clamp(1.0 - p.y / u_size.y, 0.0, 1.0);\n"
	"\n"
	"  /*\n"
	"   * THE HEAT HAZE.\n"
	"   *\n"
	"   * Two octaves of noise drifting upward at different rates,\n"
	"   * displacing the sample.  The envelope matters as much as the\n"
	"   * noise: the plume is strongest just above its source and mixes\n"
	"   * itself away with height, so this fades UPWARD -- a shimmer that\n"
	"   * is uniform over the pane reads as a broken shader rather than as\n"
	"   * hot air.\n"
	"   */\n"
	"  float env = (1.0 - up) * (1.0 - up);\n"
	"  vec2  hq  = sp / max(u_size.x, 1.0) * u_haze_scale;\n"
	"  float n1  = vnoise(hq + vec2(0.0, -u_haze_t.y * 2.0));\n"
	"  float n2  = vnoise(hq * 2.7 + vec2(17.0, -u_haze_t.z * 3.4));\n"
	"  vec2  warp = vec2((n1 - 0.5) * 1.6 + (n2 - 0.5) * 0.8,\n"
	"                    (n2 - 0.5) * 0.9) * u_haze * env;\n"
	"\n"
	"  vec3  light = vec3(0.0);\n"
	"  float glow  = 0.0;\n"
	"\n"
	"  /* Three column layers at widths that do not divide each other, so\n"
	"     no two of them ever line up into a lattice. */\n"
	"  ember_layer(sp, u_column,         u_rise.x, 1.0, light, glow);\n"
	"  ember_layer(sp, u_column * 0.618, u_rise.y, 2.0, light, glow);\n"
	"  ember_layer(sp, u_column * 1.593, u_rise.z, 3.0, light, glow);\n"
	"\n"
	"  float fog = u_fog * (1.0 - u_clarity * up);\n"
	"  vec3  col = tap(p + warp, fog);\n"
	"\n"
	"  /*\n"
	"   * The hearth: the fire itself is below the window, so it lights\n"
	"   * the bottom of the pane whether or not a spark happens to be\n"
	"   * there.  Without it the sparks look pasted onto a cold picture.\n"
	"   */\n"
	"  if (u_hearth > 0.001) {\n"
	"    float near = pow(1.0 - up, 2.5);\n"
	"    vec3  warm = blackbody(u_temperature * 0.72);\n"
	"    col += warm * near * u_hearth;\n"
	"    /* The colour cast is part of the hearth and is gated with it:\n"
	"     * a multiply left outside the test means `embers-hearth: 0\'\n"
	"     * takes the glow away and leaves the pane warm, which reads as\n"
	"     * the setting not working. */\n"
	"    col *= mix(vec3(1.0), warm * 0.5 + 0.6,\n"
	"               near * 0.55 * min(u_hearth, 1.0));\n"
	"  }\n"
	"\n"
	"  col += light + vec3(glow) * vec3(1.0, 0.55, 0.20);\n"
	"  col *= mix(vec3(1.0), u_tint, 0.35);\n"
	"  col = clamp(col * u_brightness, 0.0, 1.0);\n"
	"\n"
	"  float a = u_alpha * cov;\n"
	"  gl_FragColor = vec4(col * a, a);\n"
	"}\n";

static gboolean
embers_prog_ensure(GowlFxGl *self)
{
	GowlFxEmbersProg *p = &self->embers;

	if (p->program != 0)
		return TRUE;
	if (self->embers_tried)
		return FALSE;
	self->embers_tried = TRUE;

	p->program = gowl_fx_link_program(embers_vert_src, embers_frag_src);
	if (p->program == 0) {
		g_warning("fx: the embers shader would not build, so ember "
		          "backdrops will sit this session out");
		return FALSE;
	}

	p->u_soft        = glGetUniformLocation(p->program, "u_soft");
	p->u_sharp       = glGetUniformLocation(p->program, "u_sharp");
	p->u_src_origin  = glGetUniformLocation(p->program, "u_src_origin");
	p->u_src_size    = glGetUniformLocation(p->program, "u_src_size");
	p->u_src_scale   = glGetUniformLocation(p->program, "u_src_scale");
	p->u_size        = glGetUniformLocation(p->program, "u_size");
	p->u_radius      = glGetUniformLocation(p->program, "u_radius");
	p->u_rise        = glGetUniformLocation(p->program, "u_rise");
	p->u_haze_t      = glGetUniformLocation(p->program, "u_haze_t");
	p->u_column      = glGetUniformLocation(p->program, "u_column");
	p->u_density     = glGetUniformLocation(p->program, "u_density");
	p->u_ember       = glGetUniformLocation(p->program, "u_ember");
	p->u_spacing     = glGetUniformLocation(p->program, "u_spacing");
	p->u_sway        = glGetUniformLocation(p->program, "u_sway");
	p->u_drag        = glGetUniformLocation(p->program, "u_drag");
	p->u_temperature = glGetUniformLocation(p->program, "u_temperature");
	p->u_cool        = glGetUniformLocation(p->program, "u_cool");
	p->u_flicker     = glGetUniformLocation(p->program, "u_flicker");
	p->u_ash         = glGetUniformLocation(p->program, "u_ash");
	p->u_glow        = glGetUniformLocation(p->program, "u_glow");
	p->u_hearth      = glGetUniformLocation(p->program, "u_hearth");
	p->u_haze        = glGetUniformLocation(p->program, "u_haze");
	p->u_haze_scale  = glGetUniformLocation(p->program, "u_haze_scale");
	p->u_fog         = glGetUniformLocation(p->program, "u_fog");
	p->u_clarity     = glGetUniformLocation(p->program, "u_clarity");
	p->u_tint        = glGetUniformLocation(p->program, "u_tint");
	p->u_brightness  = glGetUniformLocation(p->program, "u_brightness");
	p->u_alpha       = glGetUniformLocation(p->program, "u_alpha");
	p->u_seed        = glGetUniformLocation(p->program, "u_seed");
	p->a_pos         = glGetAttribLocation(p->program, "a_pos");
	p->a_uv          = glGetAttribLocation(p->program, "a_uv");
	return TRUE;
}

void
gowl_fx_embers_params_init(GowlFxEmbersParams *params)
{
	if (params == NULL)
		return;

	memset(params, 0, sizeof(*params));
	/*
	 * A FIRE AND NOT A FEW SPARKS.  The first set of numbers here put
	 * about three visible embers on a nine-hundred-pixel window, which
	 * is not a hearth -- it is a hearth that has gone out.  The count
	 * that matters is columns-that-carry-sparks times emissions per
	 * cycle, and the brightness that matters is how long one stays hot,
	 * so all four went up together.
	 */
	params->column      = 84.0f;
	params->density     = 0.85f;
	params->ember       = 5.5f;
	params->spacing     = 0.80f;
	params->sway        = 18.0f;
	params->drag        = 2.20f;
	params->temperature = 2300.0f;
	params->cool        = 0.65f;
	params->flicker     = 0.55f;
	params->ash         = 0.16f;
	params->glow        = 1.50f;
	params->hearth      = 0.62f;
	params->haze        = 9.0f;
	params->haze_scale  = 3.4f;
	params->fog         = 0.30f;
	params->clarity     = 0.55f;
	params->brightness  = 1.0f;
	params->alpha       = 1.0f;
	params->src_scale   = 1.0f;
	/* A warm cast over the whole pane: a room with a fire in it is not
	 * a neutral room. */
	params->tint[0] = 1.00f;
	params->tint[1] = 0.86f;
	params->tint[2] = 0.72f;
}

void
gowl_fx_embers_advance(GowlFxEmbersClock *clock, gdouble dt, gdouble speed,
                       gdouble haze_speed)
{
	/* Irrational against each other, for the reason every clock here
	 * gives: rates that are really k/100 all return to zero together. */
	static const gdouble rise_rate[3] = {
		0.2500000000000000, 0.1545084971874737, 0.0954915028125263
	};
	static const gdouble haze_rate[3] = {
		0.1100000000000000, 0.0679837398373984, 0.0420162601626016
	};
	gint i;

	if (clock == NULL)
		return;
	if (!(dt > 0.0))
		return;
	if (dt > 0.25)
		dt = 0.25;   /* a stall is not a gust up the chimney */

	for (i = 0; i < 3; i++) {
		clock->rise[i] += dt * speed * rise_rate[i];
		clock->rise[i] = fmod(clock->rise[i], GOWL_FX_EMBERS_CYCLES);
		if (clock->rise[i] < 0.0)
			clock->rise[i] += GOWL_FX_EMBERS_CYCLES;

		clock->haze[i] += dt * haze_speed * haze_rate[i];
		clock->haze[i] = fmod(clock->haze[i], GOWL_FX_EMBERS_CYCLES);
		if (clock->haze[i] < 0.0)
			clock->haze[i] += GOWL_FX_EMBERS_CYCLES;
	}
}

gboolean
gowl_fx_pass_embers(GowlFxPass               *pass,
                    const GowlFxTexture      *soft,
                    const GowlFxTexture      *sharp,
                    const GowlFxEmbersParams *params,
                    const GowlFxEmbersClock  *clock)
{
	GowlFxGl               *gl;
	const GowlFxEmbersProg *p;
	const GowlFxTexture    *clear_src;
	GowlFxEmbersClock       still;
	gfloat                  rise[3], haze[3];
	gfloat                  radius, column;
	gint                    i;

	if (pass == NULL || params == NULL || soft == NULL)
		return FALSE;
	if (params->width <= 0 || params->height <= 0)
		return FALSE;
	if (soft->tex == 0 || soft->width <= 0 || soft->height <= 0)
		return FALSE;

	gl = pass->gl;
	if (!embers_prog_ensure(gl))
		return FALSE;
	p = &gl->embers;

	if (clock == NULL) {
		memset(&still, 0, sizeof(still));
		clock = &still;
	}
	for (i = 0; i < 3; i++) {
		rise[i] = (gfloat)clock->rise[i];
		haze[i] = (gfloat)clock->haze[i];
	}

	radius = CLAMP(params->radius, 0.0f,
	               (gfloat)MIN(params->width, params->height) * 0.5f);
	column = MAX(16.0f, params->column);
	clear_src = (sharp != NULL && sharp->tex != 0) ? sharp : soft;

	glUseProgram(p->program);
	glUniform2f(p->u_src_origin, params->src_origin[0], params->src_origin[1]);
	glUniform2f(p->u_src_size, (gfloat)MAX(1, soft->width),
	            (gfloat)MAX(1, soft->height));
	glUniform1f(p->u_src_scale,
	            params->src_scale > 0.0f ? params->src_scale : 1.0f);
	glUniform2f(p->u_size, (gfloat)params->width, (gfloat)params->height);
	glUniform1f(p->u_radius, radius);
	glUniform3fv(p->u_rise, 1, rise);
	glUniform3fv(p->u_haze_t, 1, haze);
	glUniform1f(p->u_column, column);
	glUniform1f(p->u_density, CLAMP(params->density, 0.0f, 1.0f));
	/*
	 * THE SWAY IS CAPPED AGAINST THE COLUMN, and it has to be.
	 *
	 * ember_layer() asks three columns, which covers an ember that has
	 * wandered up to half a column out of its own.  The sway is applied
	 * as +/- itself, and an ember has a radius on top of that, so a
	 * quarter of a column is the most that fits.  A config that turned
	 * it up would slice every spark off down a straight vertical line at
	 * the column boundary -- a lattice drawn in the negative, which is
	 * the failure mode this whole family is arranged to avoid.
	 */
	glUniform1f(p->u_ember, CLAMP(params->ember, 0.4f, column * 0.12f));
	glUniform1f(p->u_spacing, CLAMP(params->spacing, 0.0f, 1.0f));
	glUniform1f(p->u_sway, CLAMP(params->sway, 0.0f, column * 0.25f));
	glUniform1f(p->u_drag, CLAMP(params->drag, 0.05f, 8.0f));
	glUniform1f(p->u_temperature, CLAMP(params->temperature, 900.0f, 4000.0f));
	glUniform1f(p->u_cool, CLAMP(params->cool, 0.0f, 6.0f));
	glUniform1f(p->u_flicker, CLAMP(params->flicker, 0.0f, 2.0f));
	glUniform1f(p->u_ash, CLAMP(params->ash, 0.0f, 0.8f));
	glUniform1f(p->u_glow, CLAMP(params->glow, 0.0f, 3.0f));
	glUniform1f(p->u_hearth, CLAMP(params->hearth, 0.0f, 2.0f));
	glUniform1f(p->u_haze, CLAMP(params->haze, 0.0f, 60.0f));
	glUniform1f(p->u_haze_scale, CLAMP(params->haze_scale, 0.5f, 24.0f));
	glUniform1f(p->u_fog, CLAMP(params->fog, 0.0f, 1.0f));
	glUniform1f(p->u_clarity, CLAMP(params->clarity, 0.0f, 1.0f));
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
