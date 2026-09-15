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
 * gowl-fx-submerged.c -- the wallpaper seen from under water.
 *
 * THE LIQUID WATER NEXT DOOR IS SEEN FROM OUTSIDE.  It is a surface: a
 * height field between the eye and the wallpaper, and everything it does
 * is refraction at that one boundary.  This is the other side of it --
 * the eye is IN the medium, and what a medium does over a distance is
 * nothing a surface does at all.
 *
 * 1. IT EATS THE RED FIRST, AND THAT IS WHERE THE BLUE COMES FROM.
 *
 *    Water absorbs light exponentially with distance, and the
 *    coefficient depends strongly on wavelength: for clear water it is
 *    roughly 0.45 per metre in the red, 0.07 in the green and 0.02 in
 *    the blue.  So over a few metres red is gone, over ten green is
 *    going, and what is left is blue.
 *
 *        T(lambda) = exp(-k(lambda) * depth)
 *
 *    THE BLUE IS NOT A TINT.  Nothing here multiplies by a blue colour.
 *    Three numbers and one exponential produce it, which is why turning
 *    `submerged-depth' up walks the picture through exactly the sequence
 *    a diver sees -- oranges going first, then yellows, then greens --
 *    instead of just getting bluer.
 *
 * 2. AND IT PUTS LIGHT BACK.  A scattering medium is not only a filter:
 *    it glows with the light scattered INTO the line of sight, which is
 *    why distant things underwater are not black but the colour of the
 *    water.  Same exponential, the other way round, which is the airlight
 *    model -- and it is what makes depth read as distance rather than as
 *    a dimmer.
 *
 * 3. THE CAUSTICS ARE THE SURFACE'S SHADOW.  The waves overhead act as
 *    a lens field, and where neighbouring patches of surface send light
 *    to the same place it piles up: a moving net of bright lines over
 *    everything.  Drawn here as the EDGES between the cells of a Worley
 *    field whose feature points orbit -- which is what a caustic net
 *    geometrically is, the fold lines of a map from the surface to the
 *    bottom.  Not a sine pattern: a caustic is cellular and irregular,
 *    and a periodic one reads as a swimming-pool tile.
 *
 * 4. SHAFTS.  The same bright patches send columns down through the
 *    water, visible because the water scatters.  They lean towards the
 *    sun and they fade with depth as the beams spread.
 *
 * 5. MARINE SNOW.  Particulate drifting DOWN, catching the light.  It is
 *    the cheapest thing in this file and it is the one that makes people
 *    say "underwater" out loud, because nothing else on a desktop has
 *    slow specks falling through it.
 */

#undef G_LOG_DOMAIN
#define G_LOG_DOMAIN "gowl-fx"

#include "gowl-fx-private.h"

#include <math.h>
#include <string.h>

static const gchar submerged_vert_src[] =
	"attribute vec2 a_pos;\n"
	"attribute vec2 a_uv;\n"
	"varying vec2 v_uv;\n"
	"void main() {\n"
	"  v_uv = a_uv;\n"
	"  gl_Position = vec4(a_pos, 0.0, 1.0);\n"
	"}\n";

static const gchar submerged_frag_src[] =
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
	"uniform vec3  u_caustic_t;     /* three caustic phases */\n"
	"uniform float u_drift;         /* the motes' fall clock */\n"
	"uniform float u_surf;          /* the surface's own phase */\n"
	"uniform float u_depth;         /* metres of water in front */\n"
	"uniform vec3  u_extinction;    /* per-metre absorption, r/g/b */\n"
	"uniform float u_murk;          /* extra scattering: how silty */\n"
	"uniform vec3  u_water;         /* the colour scattered back in */\n"
	"uniform float u_caustics;      /* how bright the net is */\n"
	"uniform float u_caustic_scale; /* cells across the pane */\n"
	"uniform float u_shafts;\n"
	"uniform float u_shaft_lean;\n"
	"uniform float u_motes;         /* how many specks, 0..1 */\n"
	"uniform float u_mote_size;     /* px */\n"
	"uniform float u_surface;       /* how visible the surface band is */\n"
	"uniform float u_sway;          /* px the whole view wobbles */\n"
	"uniform float u_fog;\n"
	"uniform float u_clarity;\n"
	"uniform float u_brightness;\n"
	"uniform float u_alpha;\n"
	"uniform float u_seed;\n"
	"varying vec2 v_uv;\n"
	"\n"
	"#define TAU 6.28318530718\n"
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
	" * THE CAUSTIC NET: the edges of a Worley field whose points orbit.\n"
	" *\n"
	" * A caustic is where a map folds -- where light from two different\n"
	" * parts of the wavy surface arrives at the same place.  For a random\n"
	" * surface those fold lines are the boundaries between the regions\n"
	" * each bump sends light to, which is exactly a Worley (cellular)\n"
	" * diagram, and the classic F2 - F1 gives its edges.\n"
	" *\n"
	" * Feature points that ORBIT rather than translate is what makes the\n"
	" * net writhe instead of sliding: a caustic does not move across the\n"
	" * bottom, it reshapes in place, because the waves overhead are\n"
	" * passing through rather than carrying the pattern with them.\n"
	" */\n"
	"float caustic_net(vec2 p, float t) {\n"
	"  vec2  ip = floor(p);\n"
	"  vec2  fp = fract(p);\n"
	"  float f1 = 8.0;\n"
	"  float f2 = 8.0;\n"
	"  int   i, j;\n"
	"\n"
	"  for (j = -1; j <= 1; j++) {\n"
	"    for (i = -1; i <= 1; i++) {\n"
	"      vec2  g = vec2(float(i), float(j));\n"
	"      vec2  o = hash2(ip + g);\n"
	"      vec2  q = 0.5 + 0.42 * sin(t * TAU + o * TAU);\n"
	"      float d = length(g + q - fp);\n"
	"\n"
	"      if (d < f1) { f2 = f1; f1 = d; }\n"
	"      else if (d < f2) { f2 = d; }\n"
	"    }\n"
	"  }\n"
	"  /* Bright where two cells meet and dark inside them, sharpened\n"
	"     hard: a caustic line is thin and very much brighter than what\n"
	"     is around it, and a soft one reads as a stain. */\n"
	"  return pow(clamp(1.0 - (f2 - f1), 0.0, 1.0), 10.0);\n"
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
	" * Marine snow: specks drifting down.\n"
	" *\n"
	" * A jittered grid under half full, which is the standing rule in\n"
	" * this directory -- a full one is a lattice however well jittered.\n"
	" * The fall is the clock's fraction and the whole part is which\n"
	" * speck, so a cell does not drop the same one forever.\n"
	" */\n"
	"float motes(vec2 p, float cell) {\n"
	"  float acc = 0.0;\n"
	"  float i, j;\n"
	"\n"
	"  for (j = -1.0; j <= 1.0; j += 1.0) {\n"
	"    for (i = -1.0; i <= 1.0; i += 1.0) {\n"
	"      vec2  c  = floor(p / cell) + vec2(i, j);\n"
	"      float t  = u_drift * (0.45 + 0.55 * hash1(c + 7.0));\n"
	"      float k  = floor(t);\n"
	"      float u  = fract(t);\n"
	"      vec2  h  = hash2(c + k * 17.0);\n"
	"\n"
	"      if (h.x > u_motes)\n"
	"        continue;\n"
	"\n"
	"      {\n"
	"        /* Down, with a slow sway: a speck this small is carried by\n"
	"           the water rather than falling through it. */\n"
	"        float sx = sin((u * 3.0 + h.y) * TAU) * cell * 0.18;\n"
	"        vec2  at = (c + vec2(0.5 + (h.y - 0.5) * 0.7, u)) * cell\n"
	"                   + vec2(sx, 0.0);\n"
	"        float r  = u_mote_size * (0.5 + h.x * 1.6);\n"
	"        float d  = length(p - at) / max(r, 0.4);\n"
	"\n"
	"        /* Out of focus, because it is between the eye and whatever\n"
	"           the eye is looking at: a soft blob, not a dot. */\n"
	"        acc += exp(-d * d * 1.1) * (0.35 + 0.65 * h.y);\n"
	"      }\n"
	"    }\n"
	"  }\n"
	"  return acc;\n"
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
	"  vec2  sp  = p + vec2(u_seed * 421.0, u_seed * 137.0);\n"
	"  vec2  uvn = clamp(v_uv, 0.0, 1.0);\n"
	"  /* Deeper towards the bottom of the pane, because down is deeper.\n"
	"     The knob is the depth at the MIDDLE of the window, so turning it\n"
	"     up does not merely darken the bottom. */\n"
	"  float depth = max(u_depth, 0.0) * (0.55 + 0.9 * uvn.y);\n"
	"\n"
	"  vec2  cq = sp / max(u_size.x, 1.0) * u_caustic_scale;\n"
	"  float net = caustic_net(cq, u_caustic_t.x) * 0.62\n"
	"            + caustic_net(cq * 1.93 + 11.0, u_caustic_t.y) * 0.38;\n"
	"\n"
	"  /*\n"
	"   * The whole view wobbles.  The surface overhead is moving, and\n"
	"   * everything seen through it moves with it -- which is the one\n"
	"   * thing that stops the caustics looking like a texture laid over a\n"
	"   * still photograph.  The same field drives both, so the light and\n"
	"   * the distortion agree.\n"
	"   */\n"
	"  vec2  warp = vec2(vnoise(cq * 0.7 + vec2(u_caustic_t.z, 0.0)) - 0.5,\n"
	"                    vnoise(cq * 0.7 + vec2(0.0, u_caustic_t.z)) - 0.5)\n"
	"               * 2.0 * u_sway;\n"
	"\n"
	"  float fog = clamp(u_fog + u_murk * 0.6, 0.0, 1.0) * (1.0 - u_clarity);\n"
	"  vec3  col = tap(p + warp, fog);\n"
	"\n"
	"  /*\n"
	"   * ABSORPTION, and the light scattered back in.\n"
	"   *\n"
	"   * Beer-Lambert for what survives the trip, and the airlight model\n"
	"   * for what the water adds: a scattering medium is not a filter,\n"
	"   * it GLOWS, which is why the far end of a pool is pale blue and\n"
	"   * not black.  Both use the same exponential and they are the whole\n"
	"   * of why this reads as distance.\n"
	"   */\n"
	"  {\n"
	"    vec3 k = u_extinction * (1.0 + u_murk * 2.0);\n"
	"    vec3 T = exp(-k * depth);\n"
	"\n"
	"    col = col * T + u_water * (1.0 - T);\n"
	"  }\n"
	"\n"
	"  /*\n"
	"   * The caustic net, and the shafts that come with it.\n"
	"   *\n"
	"   * Added AFTER the absorption, not before: this light has travelled\n"
	"   * from the surface to here, not from the wallpaper to here, so it\n"
	"   * is attenuated by the depth of the viewer and not by the depth of\n"
	"   * the thing being looked at.  Getting that the wrong way round\n"
	"   * leaves the caustics as dim as the wallpaper and the effect looks\n"
	"   * flat.\n"
	"   */\n"
	"  {\n"
	"    vec3  lit  = exp(-u_extinction * depth * 0.55);\n"
	"    float fall = exp(-uvn.y * 1.6);\n"
	"\n"
	"    col += lit * net * u_caustics * fall;\n"
	"\n"
	"    /* Shafts: two drifting noise slices rather than a sine, because\n"
	"       a periodic set of beams is a lattice and reads as one. */\n"
	"    {\n"
	"      float x  = uvn.x + uvn.y * u_shaft_lean;\n"
	"      float s1 = vnoise(vec2(x * 5.0, u_surf * 0.7));\n"
	"      float s2 = vnoise(vec2(x * 11.7 + 3.0, u_surf * 1.13));\n"
	"      float s  = pow(max(s1 * 0.65 + s2 * 0.35, 0.0), 5.0);\n"
	"\n"
	"      col += lit * s * u_shafts * fall * 1.4;\n"
	"    }\n"
	"  }\n"
	"\n"
	"  /* The underside of the surface, at the very top of the pane: a\n"
	"     bright rippling band.  A few pixels of it is enough to say which\n"
	"     way is up, and up is most of what says this is water. */\n"
	"  if (u_surface > 0.001) {\n"
	"    float ripple = vnoise(vec2(uvn.x * 9.0, u_surf * 1.7)) * 0.035;\n"
	"    float band   = 1.0 - smoothstep(0.0, 0.085 + ripple, uvn.y);\n"
	"\n"
	"    col = mix(col, col * 0.4 + vec3(0.62, 0.86, 0.95),\n"
	"              band * u_surface);\n"
	"  }\n"
	"\n"
	"  /* Marine snow, last, because it is between the eye and all of it. */\n"
	"  if (u_motes > 0.001) {\n"
	"    float m = motes(sp, max(u_size.x, 1.0) / 9.0);\n"
	"\n"
	"    col += vec3(0.80, 0.92, 1.00) * m * 0.22;\n"
	"  }\n"
	"\n"
	"  col = clamp(col * u_brightness, 0.0, 1.0);\n"
	"\n"
	"  float a = u_alpha * cov;\n"
	"  gl_FragColor = vec4(col * a, a);\n"
	"}\n";

static gboolean
submerged_prog_ensure(GowlFxGl *self)
{
	GowlFxSubmergedProg *p = &self->submerged;

	if (p->program != 0)
		return TRUE;
	if (self->submerged_tried)
		return FALSE;
	self->submerged_tried = TRUE;

	p->program = gowl_fx_link_program(submerged_vert_src,
	                                  submerged_frag_src);
	if (p->program == 0) {
		g_warning("fx: the submerged shader would not build, so "
		          "submerged backdrops will sit this session out");
		return FALSE;
	}

	p->u_soft          = glGetUniformLocation(p->program, "u_soft");
	p->u_sharp         = glGetUniformLocation(p->program, "u_sharp");
	p->u_src_origin    = glGetUniformLocation(p->program, "u_src_origin");
	p->u_src_size      = glGetUniformLocation(p->program, "u_src_size");
	p->u_src_scale     = glGetUniformLocation(p->program, "u_src_scale");
	p->u_size          = glGetUniformLocation(p->program, "u_size");
	p->u_radius        = glGetUniformLocation(p->program, "u_radius");
	p->u_caustic_t     = glGetUniformLocation(p->program, "u_caustic_t");
	p->u_drift         = glGetUniformLocation(p->program, "u_drift");
	p->u_surf          = glGetUniformLocation(p->program, "u_surf");
	p->u_depth         = glGetUniformLocation(p->program, "u_depth");
	p->u_extinction    = glGetUniformLocation(p->program, "u_extinction");
	p->u_murk          = glGetUniformLocation(p->program, "u_murk");
	p->u_water         = glGetUniformLocation(p->program, "u_water");
	p->u_caustics      = glGetUniformLocation(p->program, "u_caustics");
	p->u_caustic_scale = glGetUniformLocation(p->program, "u_caustic_scale");
	p->u_shafts        = glGetUniformLocation(p->program, "u_shafts");
	p->u_shaft_lean    = glGetUniformLocation(p->program, "u_shaft_lean");
	p->u_motes         = glGetUniformLocation(p->program, "u_motes");
	p->u_mote_size     = glGetUniformLocation(p->program, "u_mote_size");
	p->u_surface       = glGetUniformLocation(p->program, "u_surface");
	p->u_sway          = glGetUniformLocation(p->program, "u_sway");
	p->u_fog           = glGetUniformLocation(p->program, "u_fog");
	p->u_clarity       = glGetUniformLocation(p->program, "u_clarity");
	p->u_brightness    = glGetUniformLocation(p->program, "u_brightness");
	p->u_alpha         = glGetUniformLocation(p->program, "u_alpha");
	p->u_seed          = glGetUniformLocation(p->program, "u_seed");
	p->a_pos           = glGetAttribLocation(p->program, "a_pos");
	p->a_uv            = glGetAttribLocation(p->program, "a_uv");
	return TRUE;
}

void
gowl_fx_submerged_params_init(GowlFxSubmergedParams *params)
{
	if (params == NULL)
		return;

	memset(params, 0, sizeof(*params));
	params->depth      = 3.4f;
	/*
	 * Per-metre extinction for clear coastal water, near enough.  These
	 * three numbers ARE the colour of the effect -- there is no blue
	 * tint anywhere else in it -- and their RATIO is what matters: red
	 * dies about twenty times faster than blue, which is why a few
	 * metres is enough to take the warmth out of anything.
	 */
	params->extinction[0] = 0.42f;
	params->extinction[1] = 0.075f;
	params->extinction[2] = 0.025f;
	params->murk       = 0.12f;
	params->water[0]   = 0.05f;
	params->water[1]   = 0.28f;
	params->water[2]   = 0.38f;
	params->caustics   = 0.55f;
	params->caustic_scale = 4.2f;
	params->shafts     = 0.22f;
	params->shaft_lean = 0.35f;
	params->motes      = 0.38f;
	params->mote_size  = 2.2f;
	params->surface    = 0.45f;
	params->sway       = 6.0f;
	params->fog        = 0.22f;
	params->clarity    = 0.35f;
	params->brightness = 1.0f;
	params->alpha      = 1.0f;
	params->src_scale  = 1.0f;
}

void
gowl_fx_submerged_advance(GowlFxSubmergedClock *clock, gdouble dt,
                          gdouble speed, gdouble drift_seconds)
{
	static const gdouble caustic_rate[3] = {
		0.1300000000000000, 0.0803398874989484, 0.0496601125010516
	};
	gint i;

	if (clock == NULL)
		return;
	if (!(dt > 0.0))
		return;
	if (dt > 0.25)
		dt = 0.25;   /* a stall is not a current */
	if (!(drift_seconds > 0.5))
		drift_seconds = 22.0;

	for (i = 0; i < 3; i++) {
		clock->caustic[i] += dt * speed * caustic_rate[i];
		clock->caustic[i] = fmod(clock->caustic[i], GOWL_FX_SUBMERGED_CYCLES);
		if (clock->caustic[i] < 0.0)
			clock->caustic[i] += GOWL_FX_SUBMERGED_CYCLES;
	}

	/* The motes fall on their own clock, and slowly: marine snow takes
	 * the better part of a minute to cross a window, which is the whole
	 * reason it reads as "suspended in" rather than "falling through". */
	clock->drift += dt / drift_seconds;
	clock->drift = fmod(clock->drift, GOWL_FX_SUBMERGED_CYCLES);
	if (clock->drift < 0.0)
		clock->drift += GOWL_FX_SUBMERGED_CYCLES;

	clock->surf += dt * speed * 0.21;
	clock->surf = fmod(clock->surf, GOWL_FX_SUBMERGED_CYCLES);
	if (clock->surf < 0.0)
		clock->surf += GOWL_FX_SUBMERGED_CYCLES;
}

gboolean
gowl_fx_pass_submerged(GowlFxPass                  *pass,
                       const GowlFxTexture         *soft,
                       const GowlFxTexture         *sharp,
                       const GowlFxSubmergedParams *params,
                       const GowlFxSubmergedClock  *clock)
{
	GowlFxGl                  *gl;
	const GowlFxSubmergedProg *p;
	const GowlFxTexture       *clear_src;
	GowlFxSubmergedClock       still;
	gfloat                     caustic[3];
	gfloat                     radius;
	gint                       i;

	if (pass == NULL || params == NULL || soft == NULL)
		return FALSE;
	if (params->width <= 0 || params->height <= 0)
		return FALSE;
	if (soft->tex == 0 || soft->width <= 0 || soft->height <= 0)
		return FALSE;

	gl = pass->gl;
	if (!submerged_prog_ensure(gl))
		return FALSE;
	p = &gl->submerged;

	if (clock == NULL) {
		memset(&still, 0, sizeof(still));
		clock = &still;
	}
	for (i = 0; i < 3; i++)
		caustic[i] = (gfloat)clock->caustic[i];

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
	glUniform3fv(p->u_caustic_t, 1, caustic);
	glUniform1f(p->u_drift, (gfloat)clock->drift);
	glUniform1f(p->u_surf, (gfloat)clock->surf);
	/* Thirty metres is past where anything is visible at all; the clamp
	 * is there so a typo cannot produce a black window with no clue as
	 * to why. */
	glUniform1f(p->u_depth, CLAMP(params->depth, 0.0f, 30.0f));
	glUniform3fv(p->u_extinction, 1, params->extinction);
	glUniform1f(p->u_murk, CLAMP(params->murk, 0.0f, 2.0f));
	glUniform3fv(p->u_water, 1, params->water);
	glUniform1f(p->u_caustics, CLAMP(params->caustics, 0.0f, 3.0f));
	glUniform1f(p->u_caustic_scale,
	            CLAMP(params->caustic_scale, 0.5f, 24.0f));
	glUniform1f(p->u_shafts, CLAMP(params->shafts, 0.0f, 3.0f));
	glUniform1f(p->u_shaft_lean, CLAMP(params->shaft_lean, -2.0f, 2.0f));
	glUniform1f(p->u_motes, CLAMP(params->motes, 0.0f, 1.0f));
	glUniform1f(p->u_mote_size, CLAMP(params->mote_size, 0.4f, 24.0f));
	glUniform1f(p->u_surface, CLAMP(params->surface, 0.0f, 1.0f));
	glUniform1f(p->u_sway, CLAMP(params->sway, 0.0f, 80.0f));
	glUniform1f(p->u_fog, CLAMP(params->fog, 0.0f, 1.0f));
	glUniform1f(p->u_clarity, CLAMP(params->clarity, 0.0f, 1.0f));
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
