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
 * Full-screen tag transitions: one fragment shader per kind, each reading
 * the old desktop and the new one and writing the whole output.
 *
 * Every kind is the same program shape -- the vertex shader below, a
 * shared preamble of samplers and noise, and a body defining
 *
 *     vec4 transition(vec2 uv)
 *
 * -- so adding a transition is one string in the table at the bottom.
 * The one rule each body keeps is the SEAM rule: at u_p == 0 it returns
 * exactly the old desktop and at u_p == 1 exactly the new one, for every
 * pixel.  The first and last frames of a switch are presented, and a
 * body that is "nearly" the desktop there flashes on every tag change.
 * tests/test-transition-render.c holds every kind to it.
 *
 * Texture coordinates have v = 0 at the TOP of the output: captures are
 * stored upright (gowl_fx_capture()) and the screen quad maps its top
 * row to the buffer's first row, which wlroots treats as the top.  So
 * "down" in a body is +v.
 */

#include "gowl-fx-private.h"

#include <string.h>

static const gchar blend_vert_src[] =
	"attribute vec2 a_pos;\n"
	"attribute vec2 a_uv;\n"
	"varying vec2 v_uv;\n"
	"void main() {\n"
	"  v_uv = a_uv;\n"
	"  gl_Position = vec4(a_pos, 0.0, 1.0);\n"
	"}\n";

/*
 * What every body can use.  highp where there is one: the noise hashes
 * multiply coordinates into the thousands, and at mediump the low bits
 * that make them look random are gone, leaving visible banding.
 */
static const gchar blend_preamble_src[] =
	"#ifdef GL_FRAGMENT_PRECISION_HIGH\n"
	"precision highp float;\n"
	"#else\n"
	"precision mediump float;\n"
	"#endif\n"
	"uniform sampler2D u_from;\n"
	"uniform sampler2D u_to;\n"
	"uniform float u_p;        /* 0 = all from, 1 = all to */\n"
	"uniform vec2  u_center;   /* 0..1, v down */\n"
	"uniform float u_aspect;   /* width / height */\n"
	"uniform vec2  u_size;     /* output pixels */\n"
	"uniform float u_dir;      /* +1 forward, -1 back */\n"
	"uniform float u_time;     /* seconds */\n"
	"uniform float u_seed;\n"
	"uniform float u_strength;\n"
	"varying vec2 v_uv;\n"
	"const float PI = 3.14159265358979;\n"
	"vec4 F(vec2 uv) { return texture2D(u_from, clamp(uv, 0.0, 1.0)); }\n"
	"vec4 T(vec2 uv) { return texture2D(u_to, clamp(uv, 0.0, 1.0)); }\n"
	"float hash(vec2 p) {\n"
	"  p = fract(p * vec2(123.34, 456.21) + u_seed);\n"
	"  p += dot(p, p + 45.32);\n"
	"  return fract(p.x * p.y);\n"
	"}\n"
	"float hash1(float x) { return hash(vec2(x, x * 0.37 + 1.7)); }\n"
	"float vnoise(vec2 p) {\n"
	"  vec2 i = floor(p);\n"
	"  vec2 f = fract(p);\n"
	"  vec2 w = f * f * (3.0 - 2.0 * f);\n"
	"  return mix(mix(hash(i), hash(i + vec2(1.0, 0.0)), w.x),\n"
	"             mix(hash(i + vec2(0.0, 1.0)), hash(i + vec2(1.0, 1.0)), w.x),\n"
	"             w.y);\n"
	"}\n"
	"float fbm(vec2 p) {\n"
	"  float v = 0.0;\n"
	"  float a = 0.5;\n"
	"  for (int i = 0; i < 5; i++) {\n"
	"    v += a * vnoise(p);\n"
	"    p = p * 2.03 + vec2(17.1, 9.4);\n"
	"    a *= 0.5;\n"
	"  }\n"
	"  return v;\n"
	"}\n"
	"/* Aspect-correct distance in units of the output's height. */\n"
	"float adist(vec2 a, vec2 b) {\n"
	"  vec2 d = (a - b) * vec2(u_aspect, 1.0);\n"
	"  return length(d);\n"
	"}\n"
	"/* The furthest corner from c, in the same units. */\n"
	"float far_corner(vec2 c) {\n"
	"  return max(max(adist(c, vec2(0.0, 0.0)), adist(c, vec2(1.0, 0.0))),\n"
	"             max(adist(c, vec2(0.0, 1.0)), adist(c, vec2(1.0, 1.0))));\n"
	"}\n";

static const gchar blend_main_src[] =
	"void main() {\n"
	"  vec4 c;\n"
	"  /* The seam, enforced rather than trusted: whatever a body does\n"
	"     mid-flight, the ends are the desktops themselves. */\n"
	"  if (u_p <= 0.0) c = F(v_uv);\n"
	"  else if (u_p >= 1.0) c = T(v_uv);\n"
	"  else c = transition(v_uv);\n"
	"  gl_FragColor = vec4(c.rgb, 1.0);\n"
	"}\n";

/* ── The kinds ───────────────────────────────────────────────────── */

/* A crossfade: the reference body, and the transition for anyone who
 * would rather the desktop did not move. */
static const gchar blend_fade_src[] =
	"vec4 transition(vec2 uv) {\n"
	"  return mix(F(uv), T(uv), u_p);\n"
	"}\n";

/*
 * An iris: a circle opening from u_center showing the new desktop, with a
 * soft edge, a faint lit rim, and the old desktop shadowed just outside
 * it so the opening reads as a hole rather than a paint-over.  Going
 * back (u_dir < 0) it closes instead: the old desktop shrinks to a point
 * at the pointer, the new one already all round it.
 */
static const gchar blend_iris_src[] =
	"vec4 transition(vec2 uv) {\n"
	"  float reach = far_corner(u_center) + 0.06;\n"
	"  float d = adist(uv, u_center);\n"
	"  float edge = 0.02;\n"
	"  bool opening = u_dir > 0.0;\n"
	"  float r = (opening ? u_p : 1.0 - u_p) * reach;\n"
	"  /* 0 inside the circle, 1 outside */\n"
	"  float out_m = smoothstep(r - edge, r, d);\n"
	"  vec4 inner = opening ? T(uv) : F(uv);\n"
	"  vec4 outer = opening ? F(uv) : T(uv);\n"
	"  /* The shadow falls on whichever desktop is underneath: the old one\n"
	"     when opening, the new one when closing. */\n"
	"  float shade = 1.0 - 0.45 * exp(-max(d - r, 0.0) / 0.045);\n"
	"  vec3 c = mix(inner.rgb, outer.rgb * shade, out_m);\n"
	"  float fade = sin(PI * u_p);\n"
	"  float rim = exp(-pow((d - r) / 0.006, 2.0)) * 0.30 * fade;\n"
	"  return vec4(c + vec3(rim), 1.0);\n"
	"}\n";

/*
 * A page turned over: the old desktop is a sheet wrapped round a cylinder
 * whose axis -- tilted, so the bottom corner lifts first -- sweeps from
 * the right edge to past the left (the other way going back).  Working in units of the output's height, so the cylinder
 * is round on screen whatever the aspect:
 *
 *   left of the fold   the sheet still lying flat -- unless the part
 *                      already turned over lies on top of it there, seen
 *                      from behind (pale, like the back of paper)
 *   on the cylinder    its top is the back of the sheet, its underside
 *                      the front curling away, each lit by its slope
 *   right of it        the new desktop, shadowed where the curl hangs
 *                      over it
 *
 * A sheet coordinate past the page's own edge means there is no paper
 * there, which is how the turned part stops at the page's length.
 */
static const gchar blend_pagecurl_src[] =
	"float flip_x(float x) { return u_dir > 0.0 ? x : u_aspect - x; }\n"
	"bool on_page(vec2 q) {\n"
	"  return q.x >= 0.0 && q.x <= u_aspect && q.y >= 0.0 && q.y <= 1.0;\n"
	"}\n"
	"vec4 sheet(vec2 q) {\n"
	"  return F(vec2(flip_x(q.x) / u_aspect, q.y));\n"
	"}\n"
	"vec3 paper_back(vec4 c, float light) {\n"
	"  /* Printing seen through the back of the sheet: washed out. */\n"
	"  return mix(c.rgb, vec3(0.93, 0.92, 0.90), 0.72) * light;\n"
	"}\n"
	"vec4 transition(vec2 uv) {\n"
	"  float r = 0.11;\n"
	"  /* The fold runs at an angle, so the bottom corner lifts first, the\n"
	"     way a page is taken by its corner. */\n"
	"  vec2 n = normalize(vec2(1.0, 0.36));\n"
	"  vec2 P = vec2(flip_x(uv.x * u_aspect), uv.y);\n"
	"  float s = dot(P, n);\n"
	"  float s_max = dot(vec2(u_aspect, 1.0), n);\n"
	"  float pos = mix(s_max + 0.001, -r - 0.02, u_p);\n"
	"  if (s < pos) {\n"
	"    /* the turned-over part, lying flat on top, back side up */\n"
	"    vec2 qo = P + n * (2.0 * pos - 2.0 * s + PI * r);\n"
	"    if (on_page(qo)) {\n"
	"      float edge = smoothstep(0.0, 0.05, pos - s);\n"
	"      return vec4(paper_back(sheet(qo), 0.80 + 0.12 * edge), 1.0);\n"
	"    }\n"
	"    /* still flat and face up */\n"
	"    return F(uv);\n"
	"  }\n"
	"  if (s < pos + r) {\n"
	"    float th = asin(clamp((s - pos) / r, 0.0, 1.0));\n"
	"    vec2 qb = P + n * (pos + r * (PI - th) - s);\n"
	"    vec2 qf = P + n * (pos + r * th - s);\n"
	"    if (on_page(qb))\n"
	"      return vec4(paper_back(sheet(qb), 0.68 + 0.30 * cos(th)), 1.0);\n"
	"    if (on_page(qf))\n"
	"      return vec4(sheet(qf).rgb * (0.55 + 0.45 * cos(th)), 1.0);\n"
	"  }\n"
	"  /* the new desktop, in the curl's shadow near the cylinder */\n"
	"  float d = max(s - pos - r, 0.0);\n"
	"  float shade = 1.0 - 0.55 * (1.0 - u_p) * exp(-d / 0.07);\n"
	"  return vec4(T(uv).rgb * shade, 1.0);\n"
	"}\n";

/*
 * Burn: the old desktop catches near the pointer and burns away along a
 * ragged, glowing edge.  Each pixel's moment to burn is fractal noise
 * blended with its distance from u_center, so the fire spreads out from
 * where you were looking but never as a circle; a threshold sweeping
 * past every value is the fire's front.  Ahead of it the old desktop
 * scorches brown, in the band it glows white-yellow to deep red, and
 * behind it the new desktop shows through, charred dark near the edge.
 * The noise drifts slowly with u_time so the edge flickers rather than
 * sliding.
 */
static const gchar blend_burn_src[] =
	"vec4 transition(vec2 uv) {\n"
	"  vec2 q = uv * vec2(u_aspect, 1.0) * 3.4;\n"
	"  float n = fbm(q + vec2(0.0, u_time * 0.12));\n"
	"  float d = adist(uv, u_center) / far_corner(u_center);\n"
	"  float v = n * 0.62 + d * 0.38;\n"
	"  /* The values bunch between about 0.25 and 0.75 (fbm's spread plus\n"
	"     the distance term); sweeping that range, not 0..1, is what\n"
	"     keeps the fire moving for the whole transition. */\n"
	"  float t = mix(0.16, 0.84, u_p);\n"
	"  float band = 0.045;\n"
	"  if (v < t) {\n"
	"    /* burnt through: the new desktop, charred near the edge */\n"
	"    float c = smoothstep(0.0, 0.06, t - v);\n"
	"    return vec4(T(uv).rgb * (0.25 + 0.75 * c), 1.0);\n"
	"  }\n"
	"  if (v < t + band) {\n"
	"    /* the fire's front: hottest on the burnt side */\n"
	"    float f = (v - t) / band;\n"
	"    float flick = 0.85 + 0.3 * vnoise(q * 6.0 + u_time * 9.0);\n"
	"    vec3 hot = mix(vec3(1.0, 0.93, 0.62), vec3(0.95, 0.30, 0.03),\n"
	"                   smoothstep(0.0, 0.7, f));\n"
	"    vec3 glow = hot * (1.5 - f) * flick;\n"
	"    vec3 under = F(uv).rgb * 0.3;\n"
	"    return vec4(mix(glow, under, smoothstep(0.75, 1.0, f)), 1.0);\n"
	"  }\n"
	"  /* not yet: scorching brown just ahead of the flames */\n"
	"  float sc = 1.0 - smoothstep(0.0, 0.10, v - t - band);\n"
	"  vec3 col = mix(F(uv).rgb, F(uv).rgb * vec3(0.55, 0.38, 0.22), sc * 0.85);\n"
	"  return vec4(col, 1.0);\n"
	"}\n";

/*
 * Ripple: a drop falls where the pointer is.  A wavefront runs out from
 * u_center to past the furthest corner; behind it the new desktop, ahead
 * of it the old, and both seen through the water's surface -- every
 * pixel displaced along the radius by a wave that is tallest at the
 * front, decays in rings behind it and dies away ahead of it.  Crests
 * catch a little light.  The whole swell calms as the transition ends,
 * so the last frames settle on the desktop rather than snapping flat.
 */
static const gchar blend_ripple_src[] =
	"vec4 transition(vec2 uv) {\n"
	"  float reach = far_corner(u_center) + 0.12;\n"
	"  float R = u_p * reach;\n"
	"  vec2 dv = (uv - u_center) * vec2(u_aspect, 1.0);\n"
	"  float d = length(dv);\n"
	"  vec2 rad = d > 1e-4 ? dv / d : vec2(0.0);\n"
	"  float x = d - R;\n"
	"  float calm = 1.0 - u_p;\n"
	"  /* rings trailing behind the front, a short skirt ahead of it */\n"
	"  float env = x < 0.0 ? exp(x / 0.11) : exp(-x / 0.018);\n"
	"  float amp = 0.022 * calm * env;\n"
	"  float k = 42.0;\n"
	"  float wave = sin(x * k - u_time * 6.0);\n"
	"  vec2 off = rad * wave * amp / vec2(u_aspect, 1.0);\n"
	"  float inside = smoothstep(0.0, 0.03, -x);\n"
	"  vec3 c = mix(F(uv + off).rgb, T(uv + off).rgb, inside);\n"
	"  float crest = max(cos(x * k - u_time * 6.0), 0.0);\n"
	"  c += vec3(0.20) * crest * crest * env * calm;\n"
	"  return vec4(c, 1.0);\n"
	"}\n";

/*
 * Indexed by GowlFxBlendKind.  NULL is a kind this build has no shader
 * for yet; each transition module's commit adds its own.
 */
static const gchar *const blend_bodies[GOWL_FX_BLEND_COUNT] = {
	[GOWL_FX_BLEND_FADE]     = blend_fade_src,
	[GOWL_FX_BLEND_IRIS]     = blend_iris_src,
	[GOWL_FX_BLEND_BURN]     = blend_burn_src,
	[GOWL_FX_BLEND_RIPPLE]   = blend_ripple_src,
	[GOWL_FX_BLEND_PIXELATE] = NULL,
	[GOWL_FX_BLEND_GLITCH]   = NULL,
	[GOWL_FX_BLEND_MELT]     = NULL,
	[GOWL_FX_BLEND_PAGECURL] = blend_pagecurl_src,
};

/* ── Program ─────────────────────────────────────────────────────── */

void
gowl_fx_blend_params_init(GowlFxBlendParams *params)
{
	g_return_if_fail(params != NULL);

	memset(params, 0, sizeof(*params));
	params->kind      = GOWL_FX_BLEND_FADE;
	params->center[0] = 0.5f;
	params->center[1] = 0.5f;
	params->dir       = 1.0f;
	params->strength  = 1.0f;
}

gboolean
gowl_fx_blend_supported(GowlFxBlendKind kind)
{
	return (gint)kind >= 0 && kind < GOWL_FX_BLEND_COUNT
	       && blend_bodies[kind] != NULL;
}

/*
 * Build @kind's program on first use, once: a body that fails to compile
 * on this driver is a warning and a transition that declines, not one
 * retried sixty times a second.
 */
static GowlFxBlendProg *
blend_prog(GowlFxGl *self, GowlFxBlendKind kind)
{
	GowlFxBlendProg *p = &self->blend[kind];
	g_autofree gchar *frag = NULL;

	if (self->blend_tried[kind])
		return p->program != 0 ? p : NULL;
	self->blend_tried[kind] = TRUE;

	if (blend_bodies[kind] == NULL)
		return NULL;

	frag = g_strconcat(blend_preamble_src, blend_bodies[kind],
	                   blend_main_src, NULL);
	p->program = gowl_fx_link_program(blend_vert_src, frag);
	if (p->program == 0) {
		g_warning("fx: transition shader %d did not build; that "
		          "transition will switch instantly", (gint)kind);
		return NULL;
	}
	p->u_from     = glGetUniformLocation(p->program, "u_from");
	p->u_to       = glGetUniformLocation(p->program, "u_to");
	p->u_p        = glGetUniformLocation(p->program, "u_p");
	p->u_center   = glGetUniformLocation(p->program, "u_center");
	p->u_aspect   = glGetUniformLocation(p->program, "u_aspect");
	p->u_size     = glGetUniformLocation(p->program, "u_size");
	p->u_dir      = glGetUniformLocation(p->program, "u_dir");
	p->u_time     = glGetUniformLocation(p->program, "u_time");
	p->u_seed     = glGetUniformLocation(p->program, "u_seed");
	p->u_strength = glGetUniformLocation(p->program, "u_strength");
	p->a_pos      = glGetAttribLocation(p->program, "a_pos");
	p->a_uv       = glGetAttribLocation(p->program, "a_uv");
	return p;
}

gboolean
gowl_fx_pass_blend(GowlFxPass *pass, const GowlFxBlendParams *params)
{
	GowlFxBlendProg *p;

	g_return_val_if_fail(pass != NULL, FALSE);
	g_return_val_if_fail(params != NULL, FALSE);

	if (!gowl_fx_blend_supported(params->kind)
	    || params->from == 0 || params->to == 0)
		return FALSE;
	p = blend_prog(pass->gl, params->kind);
	if (p == NULL)
		return FALSE;

	/* Opaque over whatever was there: the shader writes alpha 1. */
	glDisable(GL_BLEND);
	glUseProgram(p->program);
	glUniform1f(p->u_p, CLAMP(params->progress, 0.0f, 1.0f));
	glUniform2f(p->u_center, params->center[0], params->center[1]);
	glUniform1f(p->u_aspect, pass->height > 0
	            ? (gfloat)pass->width / (gfloat)pass->height : 1.0f);
	glUniform2f(p->u_size, (gfloat)pass->width, (gfloat)pass->height);
	glUniform1f(p->u_dir, params->dir >= 0.0f ? 1.0f : -1.0f);
	glUniform1f(p->u_time, params->time);
	glUniform1f(p->u_seed, params->seed);
	glUniform1f(p->u_strength, params->strength);

	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, params->from);
	glUniform1i(p->u_from, 0);
	glActiveTexture(GL_TEXTURE1);
	glBindTexture(GL_TEXTURE_2D, params->to);
	glUniform1i(p->u_to, 1);

	gowl_fx_draw_screen_quad(p->a_pos, p->a_uv);

	glBindTexture(GL_TEXTURE_2D, 0);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, 0);
	glUseProgram(0);
	glEnable(GL_BLEND);
	return TRUE;
}
