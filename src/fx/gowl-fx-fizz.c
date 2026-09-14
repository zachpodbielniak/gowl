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
 * gowl-fx-fizz.c -- the wallpaper through a glass of something fizzy.
 *
 * The rain next door is the nearest relative: a few hundred separate
 * little lenses rather than one surface.  Everything structural carries
 * over from it -- the jittered layers, the wrapped cycle clocks, the
 * best-wins accumulator -- and everything PHYSICAL is the opposite,
 * because a bubble of gas in liquid is the inverse of a drop of liquid
 * in gas.
 *
 * WHAT MAKES IT READ AS CARBONATION, in the order the eye notices:
 *
 *   1. TRAINS, NOT SCATTER.  Bubbles do not appear at random through the
 *      drink.  They nucleate at a handful of imperfections on the glass
 *      and stream upward from those points in files, and a field of
 *      evenly-spread rising dots reads as a screensaver in about two
 *      seconds.  The nucleation sites are the whole effect.
 *
 *   2. THE FILE SPREADS AS IT RISES.  A bubble collects dissolved gas on
 *      the way up and the pressure above it drops, so it GROWS; a bigger
 *      bubble rises faster -- terminal velocity goes as the square of the
 *      radius while the flow is creeping -- so successive bubbles in a
 *      train draw apart towards the top.  That widening gap is the second
 *      thing anybody would recognise and it is not decoration here: the
 *      height is the exact solution of dh/dt proportional to (1 + g*h)^2,
 *      so growth and spreading are ONE number (see rise_h()).
 *
 *   3. THE BIG ONES ZIGZAG AND THE SMALL ONES DO NOT.  A bubble's
 *      straight-line path goes unstable above a critical size and it
 *      begins to spiral.  Wobbling all of them equally is wrong in a way
 *      that is hard to name and easy to see.
 *
 *   4. A BUBBLE IS A DIVERGING LENS.  Gas in liquid: the ray bends the
 *      other way from a raindrop's, so a bubble MINIFIES and never
 *      inverts, however big it gets.  Drawing it with the rain's sign is
 *      the single most common way to get this wrong, and it looks like
 *      glass marbles in juice.
 *
 *   5. THE OUTER QUARTER IS A MIRROR.  Light inside the liquid meeting
 *      the bubble wall past the critical angle is totally reflected, and
 *      for water against air sin(theta_c) = 1/1.333 = 0.75 -- so the
 *      silvered ring starts at exactly three quarters of the radius.  It
 *      is why bubbles in a glass look like rings rather than like dots.
 *      That 0.75 is arithmetic, not taste.
 *
 *   6. SOME OF THEM ARE STUCK.  Bubbles cling to the wall of the glass,
 *      grow in place, and let go when they are buoyant enough.  A drink
 *      where nothing is stuck to the side looks like it is being filmed
 *      from inside the liquid.
 *
 *   7. THERE IS A HEAD.  Small, tightly packed, popping.
 *
 * WHY IT IS NOT A GRID.  Same discipline as the rain, reached the same
 * way: nothing is periodic in space.  Sites are a sparse hashed subset of
 * columns at three widths that do not divide, the clinging layer is a
 * jittered grid under half full, and the seed translates the whole field
 * per window.
 *
 * THE ONE THING THE RAIN DID NOT HAVE: a train is indexed by EMISSION
 * NUMBER, not by phase.  A column at clock t with N bubbles per cycle has
 * emitted floor(t*k*N) of them; the one a given pixel might be inside is
 * found by inverting rise_h(), which is why that inverse exists.  The
 * consequence is that N must be a WHOLE number -- otherwise the emission
 * index does not change by a multiple of the cycle count when the clock
 * wraps, and every train jumps once a wrap.
 */

#undef G_LOG_DOMAIN
#define G_LOG_DOMAIN "gowl-fx"

#include "gowl-fx-private.h"

#include <math.h>
#include <string.h>

static const gchar fizz_vert_src[] =
	"attribute vec2 a_pos;\n"
	"attribute vec2 a_uv;\n"
	"varying vec2 v_uv;\n"
	"void main() {\n"
	"  v_uv = a_uv;\n"
	"  gl_Position = vec4(a_pos, 0.0, 1.0);\n"
	"}\n";

/*
 * highp conditionally, for the reason every other shader here gives:
 * naming a precision that does not exist fails to COMPILE rather than
 * falling back.  It matters more here than anywhere else in the tree --
 * the emission index of a train is the clock times a rate times a count,
 * so the numbers the hash sees run into the thousands, and at mediump
 * neighbouring emissions in one column collide into the same bubble.
 */
static const gchar fizz_frag_src[] =
	"#ifdef GL_FRAGMENT_PRECISION_HIGH\n"
	"precision highp float;\n"
	"#else\n"
	"precision mediump float;\n"
	"#endif\n"
	"\n"
	"uniform sampler2D u_soft;      /* the clouded wallpaper */\n"
	"uniform sampler2D u_sharp;     /* the same wallpaper, unblurred */\n"
	"uniform vec2  u_src_origin;    /* where this rect sits in them, px */\n"
	"uniform vec2  u_src_size;      /* their size, px */\n"
	"uniform float u_src_scale;     /* source px per buffer px */\n"
	"uniform vec2  u_size;          /* the rect, px */\n"
	"uniform float u_radius;        /* corner radius, px */\n"
	"uniform float u_cling_t;       /* clinging-bubble clock, cycles */\n"
	"uniform vec3  u_rise;          /* the three rising clocks, cycles */\n"
	"uniform float u_cell;          /* px per cell of the clinging layer */\n"
	"uniform float u_bubble;        /* release radius, fraction of a cell */\n"
	"uniform float u_growth;        /* how much bigger at the top */\n"
	"uniform float u_sites;         /* how many columns nucleate, 0..1 */\n"
	"uniform float u_site_width;    /* px per column of the train layer */\n"
	"uniform float u_spacing;       /* how closely a site emits, 0..1 */\n"
	"uniform float u_stray;         /* loose bubbles between the trains */\n"
	"uniform float u_cling;         /* how many cells hold a stuck bubble */\n"
	"uniform float u_wobble;        /* sideways wander, px */\n"
	"uniform float u_foam;          /* the head, 0..1 */\n"
	"uniform float u_foam_depth;    /* how far down the head reaches, px */\n"
	"uniform float u_depth;         /* ray travel, in bubble radii */\n"
	"uniform float u_dispersion;    /* channel separation in a bubble */\n"
	"uniform float u_mirror;        /* the silvered ring, 0..1 */\n"
	"uniform float u_fog;           /* how cloudy the drink is, 0..1 */\n"
	"uniform float u_clarity;       /* how much a bubble lifts it */\n"
	"uniform float u_specular;\n"
	"uniform float u_shine;         /* specular exponent */\n"
	"uniform float u_rim;           /* the dark contact edge */\n"
	"uniform vec3  u_light;\n"
	"uniform vec3  u_tint;\n"
	"uniform float u_absorb;\n"
	"uniform float u_brightness;\n"
	"uniform float u_alpha;\n"
	"uniform float u_seed;          /* which crop of the field, 0..16 */\n"
	"varying vec2 v_uv;\n"
	"\n"
	"const float TAU = 6.2831853;\n"
	"/*\n"
	" * Where total internal reflection starts, as a fraction of the\n"
	" * radius.\n"
	" *\n"
	" * sin(theta_c) = n_gas / n_liquid = 1.0 / 1.333, and for a sphere\n"
	" * seen head-on the sine of the angle of incidence IS the distance\n"
	" * from the centre in units of the radius.  So the outer quarter of\n"
	" * every bubble is a mirror, exactly, and this number is not a knob.\n"
	" */\n"
	"const float TIR = 0.75;\n"
	"/*\n"
	" * How many rises, and how many clinging lives, before the field\n"
	" * repeats.  Smaller than the rain's 256 because the integer hashed\n"
	" * against here is the EMISSION index -- the clock times the column\n"
	" * rate (up to 3) times its bubbles per cycle (up to 8) -- so 64\n"
	" * already puts it past fifteen hundred.\n"
	" */\n"
	"const float FIZZ_CYCLES = 64.0;\n"
	"/* How far a clinging bubble may creep before it lets go, in cells.\n"
	" * Bounded by the nine-cell lookup exactly as the rain's slide is:\n"
	" * 1.0 (jitter) + 0.22 (radius) + this must stay under 1.5. */\n"
	"const float FIZZ_CREEP = 0.24;\n"
	"\n"
	"/* Signed distance to a rounded rect centred in @size; negative inside. */\n"
	"float sdf_rect(vec2 p, vec2 size, float r) {\n"
	"  vec2 h = size * 0.5;\n"
	"  vec2 q = abs(p - h) - (h - vec2(r));\n"
	"  return min(max(q.x, q.y), 0.0) + length(max(q, vec2(0.0))) - r;\n"
	"}\n"
	"\n"
	"/* Three uncorrelated numbers from three inputs.  Three for the\n"
	" * reason gowl-fx-rain.c sets out at length: everything here is\n"
	" * indexed by WHERE and by WHICH ONE, and a hash that takes only two\n"
	" * collapses into a stepped sin sequence that aliases. */\n"
	"vec3 hash3v(vec3 c) {\n"
	"  vec3 n = vec3(dot(c, vec3(127.1,  89.3,  54.7)),\n"
	"                dot(c, vec3( 71.9, 151.3, 101.7)),\n"
	"                dot(c, vec3(167.3,  61.7, 133.9)));\n"
	"  return fract(sin(n) * vec3(43758.5453, 28001.8384, 19349.6631));\n"
	"}\n"
	"\n"
	"/*\n"
	" * HOW FAR UP A BUBBLE HAS GOT, from how long it has been rising.\n"
	" *\n"
	" * This is the physics, in closed form, and it is worth the four\n"
	" * lines because it delivers three separate things the effect needs\n"
	" * for the price of one.\n"
	" *\n"
	" * A bubble's radius grows roughly linearly with height as gas comes\n"
	" * out of solution into it: r = r0 * (1 + g*h).  In the creeping\n"
	" * regime its rise speed goes as the square of the radius, so\n"
	" *\n"
	" *   dh/dt = c * (1 + g*h)^2\n"
	" *\n"
	" * which integrates to h(u) = (1/g) * (1/(1 - u*g/(1+g)) - 1), scaled\n"
	" * so that h(0) = 0 and h(1) = 1.  What comes out of it:\n"
	" *\n"
	" *   - bubbles ACCELERATE up the glass, which they do;\n"
	" *   - a train SPREADS towards the top, because equal intervals in u\n"
	" *     map to growing intervals in h.  That widening is the single\n"
	" *     most recognisable thing about a glass of soda and here it is\n"
	" *     not drawn, it falls out;\n"
	" *   - and it is INVERTIBLE, which is what lets a pixel ask which\n"
	" *     bubble of a train it might be inside instead of testing all of\n"
	" *     them.\n"
	" */\n"
	"float rise_h(float u, float g) {\n"
	"  float gg = max(g, 0.02);\n"
	"  float s  = 1.0 - u * gg / (1.0 + gg);\n"
	"  return (1.0 / gg) * (1.0 / max(s, 0.02) - 1.0);\n"
	"}\n"
	"\n"
	"/* The inverse of rise_h(): u = (1+g)*h / (1 + g*h). */\n"
	"float rise_u(float h, float g) {\n"
	"  float gg = max(g, 0.02);\n"
	"  return (1.0 + gg) * h / (1.0 + gg * h);\n"
	"}\n"
	"\n"
	"/*\n"
	" * ONE BUBBLE, folded into the running best.\n"
	" *\n"
	" * @best carries (q.xy, radius, coverage) where q is the offset from\n"
	" * the centre in units of the radius -- everything the optics need and\n"
	" * nothing about which layer it came from.  Bubbles that overlap MERGE\n"
	" * rather than blending, which is what bubbles do, so the nearer\n"
	" * centre simply wins.\n"
	" *\n"
	" * @halo is the film of disturbed drink a bubble drags with it; it is\n"
	" * what lifts the cloudiness a little way outside the bubble itself,\n"
	" * and without it every bubble has a hard cloudy border and reads as a\n"
	" * sticker.\n"
	" */\n"
	"void bubble_at(vec2 p, vec2 centre, float r, float alive,\n"
	"               inout vec4 best, inout float halo) {\n"
	"  if (alive <= 0.0 || r <= 0.0)\n"
	"    return;\n"
	"\n"
	"  vec2  d  = p - centre;\n"
	"  float l  = length(d);\n"
	"  float aa = max(0.8, r * 0.14);\n"
	"\n"
	"  /* Edges in increasing order: GLSL leaves smoothstep(hi, lo, x)\n"
	"     undefined and Mesa's guess is not a contract. */\n"
	"  float cov = (1.0 - smoothstep(r - aa, r, l)) * alive;\n"
	"  if (cov <= 0.0)\n"
	"    return;\n"
	"\n"
	"  halo = max(halo, cov * 0.9\n"
	"             + (1.0 - smoothstep(r * 0.98, r * 1.5, l)) * 0.3 * alive);\n"
	"\n"
	"  if (cov > best.w) {\n"
	"    best.xy = d / max(r, 0.001);\n"
	"    best.z  = r;\n"
	"    best.w  = cov;\n"
	"  }\n"
	"}\n"
	"\n"
	"/*\n"
	" * ONE RISING BUBBLE of a train or a stray, given its phase.\n"
	" *\n"
	" * Shared by both layers because the motion is identical and only the\n"
	" * bookkeeping differs: grow with height, accelerate with size, and\n"
	" * zigzag only once big enough for the straight path to go unstable.\n"
	" *\n"
	" * The wobble threshold is expressed against the LAYER'S OWN WIDTH\n"
	" * rather than in pixels, so a window rendered at half size wobbles\n"
	" * the same bubbles and not a different set.\n"
	" */\n"
	"void rising_at(vec2 p, float u, float lane, float w, float r0,\n"
	"               vec3 h, inout vec4 best, inout float halo) {\n"
	"  if (u < 0.0 || u >= 1.0)\n"
	"    return;\n"
	"\n"
	"  /* Margin enough that a bubble is fully outside before it is cut. */\n"
	"  float marg = w * 1.2;\n"
	"  float span = u_size.y + 2.0 * marg;\n"
	"  float hh   = rise_h(u, u_growth);\n"
	"  float y    = u_size.y + marg - hh * span;\n"
	"  float r    = r0 * (1.0 + u_growth * hh);\n"
	"  /* Straight while small, spiralling once past the instability. */\n"
	"  float amp  = u_wobble * smoothstep(w * 0.030, w * 0.085, r);\n"
	"  float turn = 1.5 + 2.5 * h.x;\n"
	"  float x    = lane + sin(hh * TAU * turn + h.y * TAU) * amp\n"
	"                    + sin(hh * TAU * turn * 0.41 + h.z * TAU) * amp * 0.5;\n"
	"\n"
	"  /* Fade the last sliver away at the surface rather than clipping it:\n"
	"     a bubble that vanishes at a fixed height draws a horizontal line\n"
	"     across the window once per cycle. */\n"
	"  bubble_at(p, vec2(x, y), r, clamp((1.0 - hh) * 14.0, 0.0, 1.0),\n"
	"            best, halo);\n"
	"}\n"
	"\n"
	"/*\n"
	" * ONE LAYER OF NUCLEATION SITES: columns that stream.\n"
	" *\n"
	" * A site is a fixed point on the glass, so its column and its rate\n"
	" * are PERMANENT -- unlike the rain, where a track that ran forever\n"
	" * was the bug.  That is the right answer here for the same reason it\n"
	" * was the wrong one there: a scratch in the glass does not move, and\n"
	" * a train that wanders between columns is not a train.  What varies\n"
	" * is which bubble: every emission is hashed separately, so size,\n"
	" * lane offset and wobble phase differ down the file.\n"
	" *\n"
	" * THREE COLUMNS, and the number is arithmetic.  The nearest column a\n"
	" * pixel does NOT ask is two away, whose lane is at least 2 + 0.5 -\n"
	" * 0.28 - 1 = 1.22 widths off it.  A bubble sits up to 0.28 of a width\n"
	" * off its own lane, wanders another 0.375 (the wobble is capped at a\n"
	" * quarter of a width and the second harmonic adds half again) and\n"
	" * reaches 0.35 once fully grown, which is 1.005 -- inside that.  Ask\n"
	" * only two columns and every bubble that wandered left is sliced off\n"
	" * down a straight vertical line at the column edge it came from.\n"
	" *\n"
	" * THE EMISSION INDEX IS THE POINT.  Rather than walking every bubble\n"
	" * in the file, the pixel's own height is turned back into a phase\n"
	" * (rise_u) and that phase into the emission that would be there now;\n"
	" * three of them either side covers the rounding.  N -- the bubbles\n"
	" * per cycle -- is quantised to a whole number because the index has\n"
	" * to change by a multiple of FIZZ_CYCLES when the clock wraps.\n"
	" */\n"
	"void train_layer(vec2 p, float w, float t, float density, float lyr,\n"
	"                 inout vec4 best, inout float halo) {\n"
	"  if (density <= 0.0 || w <= 0.0)\n"
	"    return;\n"
	"\n"
	"  float lid  = lyr * 64.0 + u_seed;\n"
	"  float gph  = lyr * 0.29 + u_seed * 0.047;\n"
	"  float marg = w * 1.2;\n"
	"  float span = u_size.y + 2.0 * marg;\n"
	"  /* Which phase a bubble sitting at THIS pixel's height would be on. */\n"
	"  float hp   = clamp((u_size.y + marg - p.y) / span, 0.0, 1.0);\n"
	"  float up   = rise_u(hp, u_growth);\n"
	"  int   n, b;\n"
	"\n"
	"  for (n = -1; n <= 1; n++) {\n"
	"    float col = floor(p.x / w + gph) + float(n);\n"
	"    /* Permanent, per site: whether there is one at all, how fast it\n"
	"       runs, how closely it emits and where across the column it sits. */\n"
	"    vec3  hc = hash3v(vec3(col, lid, 0.0));\n"
	"    if (hc.z > density)\n"
	"      continue;\n"
	"\n"
	"    vec3  hd   = hash3v(vec3(col, lid + 11.0, 1.0));\n"
	"    float k    = 1.0 + floor(hd.x * 2.999);       /* 1, 2 or 3 */\n"
	"    /* Whole, so the emission index stays continuous across a wrap. */\n"
	"    float cnt  = 1.0 + floor(u_spacing * (2.0 + 6.0 * hd.y) * 0.999);\n"
	"    float lane = (col - gph + 0.5) * w + (hc.x - 0.5) * w * 0.4;\n"
	"    float tk   = t * k;\n"
	"    float base = floor((tk - up) * cnt);\n"
	"\n"
	"    for (b = -1; b <= 1; b++) {\n"
	"      float e  = base + float(b);\n"
	"      float u  = tk - e / cnt;\n"
	"      float ei = mod(e, FIZZ_CYCLES);\n"
	"      vec3  he = hash3v(vec3(col + lid, ei, 7.0));\n"
	"      /* A site does not emit perfectly evenly: some of the file is\n"
	"         missing, which is what stops it looking machined. */\n"
	"      if (he.z > 0.82)\n"
	"        continue;\n"
	"      rising_at(p, u, lane + (he.x - 0.5) * w * 0.16, w,\n"
	"                w * u_bubble * (0.72 + 0.56 * he.y), he, best, halo);\n"
	"    }\n"
	"  }\n"
	"}\n"
	"\n"
	"/*\n"
	" * ONE LAYER OF STRAYS: single bubbles that did not come from a site.\n"
	" *\n"
	" * Torn off the head, knocked loose by a stuck one letting go, or\n"
	" * simply nucleated in the bulk.  Unlike a site these are NOT\n"
	" * permanent -- whether a column carries one at all is hashed per\n"
	" * rise, so the space between the trains keeps changing, which is what\n"
	" * stops the drink looking like a set of parallel wires.\n"
	" */\n"
	"void stray_layer(vec2 p, float w, float t, float density, float lyr,\n"
	"                 inout vec4 best, inout float halo) {\n"
	"  if (density <= 0.0 || w <= 0.0)\n"
	"    return;\n"
	"\n"
	"  float lid = lyr * 64.0 + u_seed;\n"
	"  float gph = lyr * 0.53 + u_seed * 0.083;\n"
	"  int   n;\n"
	"\n"
	"  for (n = -1; n <= 1; n++) {\n"
	"    float col = floor(p.x / w + gph) + float(n);\n"
	"    vec3  hc  = hash3v(vec3(col, lid, 2.0));\n"
	"    float k   = 1.0 + floor(hc.z * 2.999);\n"
	"    float u   = t * k + hc.x;\n"
	"    float ri  = mod(floor(u), FIZZ_CYCLES);\n"
	"    vec3  hr  = hash3v(vec3(col, lid + 19.0, ri));\n"
	"\n"
	"    if (hr.z > density)\n"
	"      continue;\n"
	"\n"
	"    rising_at(p, fract(u),\n"
	"              (col - gph + 0.5) * w + (hr.x - 0.5) * w * 0.5, w,\n"
	"              w * u_bubble * (0.55 + 0.75 * hr.y), hr, best, halo);\n"
	"  }\n"
	"}\n"
	"\n"
	"/*\n"
	" * BUBBLES STUCK TO THE GLASS.\n"
	" *\n"
	" * A jittered grid, mostly empty, with a lifecycle that is buoyancy\n"
	" * rather than evaporation: a bubble nucleates at a scratch, GROWS\n"
	" * while it stays put, creeps upward as it gets light, and then lets\n"
	" * go and is gone.  It is the rain's resting-drop layer run backwards\n"
	" * and it is bounded the same way -- FIZZ_CREEP is what is left of the\n"
	" * nine-cell reach once the jitter and the radius are paid for.\n"
	" *\n"
	" *   a < 0.70          growing in place, slowly\n"
	" *   0.70 .. 0.88      creeping up, accelerating\n"
	" *   a > 0.88          gone: it detached and joined the drink\n"
	" *\n"
	" * As with the rain, only the PHASE belongs to the cell; size, position\n"
	" * within the cell and whether the cell holds one at all are hashed\n"
	" * against the life, so the same dozen spots do not blink on and off\n"
	" * in place for the whole session.\n"
	" */\n"
	"void cling_layer(vec2 p, float cell, float density, float ang, float lyr,\n"
	"                 inout vec4 best, inout float halo) {\n"
	"  if (density <= 0.0 || cell <= 0.0)\n"
	"    return;\n"
	"\n"
	"  float c  = cos(ang);\n"
	"  float s  = sin(ang);\n"
	"  mat2  R  = mat2(c, -s, s, c);\n"
	"  mat2  Rt = mat2(c, s, -s, c);\n"
	"  vec2  off  = vec2(u_seed * 0.37, u_seed * 0.61);\n"
	"  float lid  = lyr * 64.0 + u_seed;\n"
	"  vec2  q    = (R * p) / cell + off;\n"
	"  vec2  base = floor(q);\n"
	"  int   i, j;\n"
	"\n"
	"  for (j = -1; j <= 1; j++) {\n"
	"    for (i = -1; i <= 1; i++) {\n"
	"      vec2  id = base + vec2(float(i), float(j));\n"
	"      vec3  hc = hash3v(vec3(id.x, id.y, lid));\n"
	"      float u  = u_cling_t + hc.x;\n"
	"      float a  = fract(u);\n"
	"      float li = mod(floor(u), FIZZ_CYCLES);\n"
	"      vec3  hr = hash3v(vec3(id.x + 29.0 + lid, id.y - 41.0, li));\n"
	"\n"
	"      if (hr.z > density || a > 0.88)\n"
	"        continue;\n"
	"\n"
	"      /* Grows steadily and then stops growing as it goes: a bubble\n"
	"         about to detach is not still gaining gas, it is being pulled\n"
	"         off by the gas it already has. */\n"
	"      float grow = smoothstep(0.0, 0.70, a);\n"
	"      float lift = max(0.0, a - 0.70) / 0.18;\n"
	"      float rr   = cell * u_bubble * (0.55 + 0.85 * hr.x) * (0.3 + 0.7 * grow);\n"
	"      vec2  cen  = (id + vec2(hr.y, hc.y) - off) * cell;\n"
	"\n"
	"      /* Up the SCREEN, not up the rotated lattice: the layer is\n"
	"         turned and buoyancy is not. */\n"
	"      cen = Rt * cen;\n"
	"      cen.y -= lift * lift * cell * FIZZ_CREEP;\n"
	"\n"
	"      bubble_at(p, cen, rr, 1.0 - smoothstep(0.80, 0.88, a), best, halo);\n"
	"    }\n"
	"  }\n"
	"}\n"
	"\n"
	"/*\n"
	" * THE HEAD.\n"
	" *\n"
	" * A dense raft of small bubbles at the top of the pane, packed hard\n"
	" * enough that they overlap -- which the best-wins accumulator turns\n"
	" * into the polyhedral look of real foam for nothing, because two\n"
	" * touching bubbles share a flat wall rather than blending.\n"
	" *\n"
	" * They jiggle and they POP.  A static head is the giveaway: foam is\n"
	" * always collapsing, and a bubble that vanishes leaves its neighbours\n"
	" * to close the gap.\n"
	" */\n"
	"void foam_layer(vec2 p, float cell, float lyr,\n"
	"                inout vec4 best, inout float halo) {\n"
	"  if (u_foam <= 0.001 || u_foam_depth <= 1.0 || cell <= 0.0)\n"
	"    return;\n"
	"\n"
	"  float band = 1.0 - smoothstep(0.0, u_foam_depth, p.y);\n"
	"  if (band <= 0.0)\n"
	"    return;\n"
	"\n"
	"  float lid  = lyr * 64.0 + u_seed;\n"
	"  vec2  off  = vec2(u_seed * 0.23, u_seed * 0.91);\n"
	"  vec2  q    = p / cell + off;\n"
	"  vec2  base = floor(q);\n"
	"  int   i, j;\n"
	"\n"
	"  for (j = -1; j <= 1; j++) {\n"
	"    for (i = -1; i <= 1; i++) {\n"
	"      vec2  id = base + vec2(float(i), float(j));\n"
	"      vec3  hc = hash3v(vec3(id.x, id.y, lid));\n"
	"      float u  = u_cling_t * 2.0 + hc.x;\n"
	"      float a  = fract(u);\n"
	"      float li = mod(floor(u), FIZZ_CYCLES);\n"
	"      vec3  hr = hash3v(vec3(id.x - 13.0 + lid, id.y + 7.0, li));\n"
	"      /* Thicker at the very top, thinning downwards, with the edge of\n"
	"         the raft hashed per cell so the head has a ragged underside\n"
	"         rather than a ruled line. */\n"
	"      /* pow() rather than the band itself: a linear falloff over a\n"
	"         raft only two or three bubbles deep crosses the threshold\n"
	"         within one row, which draws the underside of the head as a\n"
	"         ruled line.  The curve spreads the thinning over the whole\n"
	"         depth, and the per-cell term ragged it. */\n"
	"      float here = pow(band, 0.65) * u_foam * (0.55 + 0.90 * hr.z);\n"
	"\n"
	"      if (here < 0.30)\n"
	"        continue;\n"
	"\n"
	"      float rr  = cell * (0.34 + 0.20 * hr.x);\n"
	"      /* Most of a life at full size, then a quick collapse. */\n"
	"      float pop = 1.0 - smoothstep(0.86, 1.0, a);\n"
	"      vec2  cen = (id + vec2(0.25 + 0.5 * hr.y, 0.25 + 0.5 * hc.y) - off)\n"
	"                  * cell;\n"
	"\n"
	"      cen.x += sin(a * TAU + hc.z * TAU) * cell * 0.06;\n"
	"      bubble_at(p, cen, rr * (0.45 + 0.55 * pop), pop, best, halo);\n"
	"    }\n"
	"  }\n"
	"}\n"
	"\n"
	"/* One tap of the wallpaper, @fog of the way towards clouded. */\n"
	"vec3 tap(vec2 px, float fog) {\n"
	"  vec2 uv = clamp((u_src_origin + px * u_src_scale) / u_src_size,\n"
	"                  vec2(0.0), vec2(1.0));\n"
	"  return mix(texture2D(u_sharp, uv).rgb, texture2D(u_soft, uv).rgb, fog);\n"
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
	"  vec4  best = vec4(0.0);\n"
	"  float halo = 0.0;\n"
	"\n"
	"  /* Three files of trains at widths that do not divide, so no two\n"
	"     sites ever line up into a visible rank.  The widest carries the\n"
	"     biggest bubbles and the fewest sites: a large bubble needs a\n"
	"     large imperfection, and those are rarer. */\n"
	"  train_layer(p, u_site_width,        u_rise.x, u_sites,        0.0,\n"
	"              best, halo);\n"
	"  train_layer(p, u_site_width * 1.57, u_rise.y, u_sites * 0.66, 1.0,\n"
	"              best, halo);\n"
	"  train_layer(p, u_site_width * 2.41, u_rise.z, u_sites * 0.34, 2.0,\n"
	"              best, halo);\n"
	"\n"
	"  /* And the loose ones between them. */\n"
	"  stray_layer(p, u_site_width * 0.83, u_rise.z, u_stray,        3.0,\n"
	"              best, halo);\n"
	"  stray_layer(p, u_site_width * 1.93, u_rise.x, u_stray * 0.58, 4.0,\n"
	"              best, halo);\n"
	"\n"
	"  /* Stuck to the glass: two turned lattices at an incommensurate\n"
	"     pitch, for the reason the rain gives -- a rotated grid is still a\n"
	"     grid, two that do not share a period are not. */\n"
	"  cling_layer(p, u_cell,        u_cling,        0.00, 5.0, best, halo);\n"
	"  cling_layer(p, u_cell * 1.67, u_cling * 0.5,  0.74, 6.0, best, halo);\n"
	"\n"
	"  foam_layer(p, u_cell * 0.20, 7.0, best, halo);\n"
	"\n"
	"  /*\n"
	"   * THE BUBBLE IS A DIVERGING LENS, WHICH IS THE WHOLE POINT.\n"
	"   *\n"
	"   * A raindrop is liquid in gas and converges: past a certain size it\n"
	"   * turns the world behind it upside down.  A bubble is gas in\n"
	"   * liquid, the index step has the other sign, and it does the\n"
	"   * opposite -- it is a tiny fisheye that MINIFIES what is behind it\n"
	"   * and never inverts however large it gets.\n"
	"   *\n"
	"   * Thin lens again: magnification 1/(1 + depth/2), so the sample sits\n"
	"   * FURTHER from the centre than the pixel does, and the displacement\n"
	"   * is +q rather than the rain's -q.  That single sign is the\n"
	"   * difference between a glass of soda and a bowl of marbles.\n"
	"   */\n"
	"  vec2  disp = vec2(0.0);\n"
	"  vec3  n    = vec3(0.0, 0.0, 1.0);\n"
	"  float l    = 0.0;\n"
	"  float tir  = 0.0;\n"
	"\n"
	"  if (best.w > 0.0) {\n"
	"    l = min(length(best.xy), 1.0);\n"
	"    n = normalize(vec3(best.xy, sqrt(max(1.0 - l * l, 1e-4))));\n"
	"    disp = best.xy * best.z * (u_depth * 0.5) * best.w;\n"
	"    /* Where the wall turns mirror.  A hair of softness either side\n"
	"       only: the transition really is abrupt, and smoothing it over\n"
	"       half the radius turns the ring into a vignette. */\n"
	"    tir = smoothstep(TIR - 0.05, TIR + 0.08, l) * best.w;\n"
	"  }\n"
	"\n"
	"  /* How cloudy this pixel is.  The drink itself is never perfectly\n"
	"     clear; a bubble is a clean gas lens and lifts most of it. */\n"
	"  float clear = clamp(max(best.w, halo) * u_clarity, 0.0, 1.0);\n"
	"  float fog   = u_fog * (1.0 - clear);\n"
	"  vec2  sp    = p + disp;\n"
	"  vec3  col;\n"
	"\n"
	"  if (u_dispersion > 0.01 && best.w > 0.0) {\n"
	"    vec2 sep = disp * u_dispersion * 0.06;\n"
	"    col.r = tap(sp - sep, fog).r;\n"
	"    col.g = tap(sp, fog).g;\n"
	"    col.b = tap(sp + sep, fog).b;\n"
	"  } else {\n"
	"    col = tap(sp, fog);\n"
	"  }\n"
	"\n"
	"  /*\n"
	"   * What the drink takes out of the light, and unlike the rain this\n"
	"   * applies to the WHOLE pane rather than only inside the bubbles.\n"
	"   * The window is a glass of something: the liquid is in front of all\n"
	"   * of it, so a tint that appears only where a bubble is would be a\n"
	"   * tinted bubble in clear air, which is backwards.\n"
	"   */\n"
	"  if (u_absorb > 0.001)\n"
	"    col = mix(col, col * u_tint, clamp(u_absorb, 0.0, 1.0));\n"
	"\n"
	"  /*\n"
	"   * THE SILVERED RING.\n"
	"   *\n"
	"   * Past the critical angle nothing gets through: the wall reflects\n"
	"   * what is beside the bubble back at the eye.  Approximated by\n"
	"   * sampling the drink WELL OUTSIDE the bubble -- which is what a\n"
	"   * grazing reflection sees -- and lifting it, which gives the bright\n"
	"   * rim its colour from the surroundings instead of painting it white.\n"
	"   */\n"
	"  if (u_mirror > 0.001 && tir > 0.0) {\n"
	"    vec3 refl = tap(p - best.xy * best.z * 1.9, u_fog * 0.4);\n"
	"    col = mix(col, refl * 1.18 + vec3(0.10), tir * u_mirror);\n"
	"  }\n"
	"\n"
	"  /* And the very edge is still darker than the ring: the wall is thin\n"
	"     and the contact line scatters.  The outer twelfth only. */\n"
	"  if (u_rim > 0.001 && best.w > 0.0)\n"
	"    col *= 1.0 - smoothstep(0.92, 1.0, l) * u_rim * best.w;\n"
	"\n"
	"  /*\n"
	"   * The glint.  One tight highlight off the front of the wall,\n"
	"   * wherever the normal bisects the light and the eye -- a bubble has\n"
	"   * no second, focused highlight, because it does not gather light\n"
	"   * towards its middle the way a drop does.  That absence is part of\n"
	"   * how a bubble reads as hollow.\n"
	"   */\n"
	"  if (u_specular > 0.001 && best.w > 0.0) {\n"
	"    vec3  V  = vec3(0.0, 0.0, 1.0);\n"
	"    vec3  hv = normalize(u_light + V);\n"
	"    float s  = pow(max(dot(n, hv), 0.0), max(u_shine, 1.0));\n"
	"    col += vec3(s * u_specular * best.w);\n"
	"  }\n"
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
 * cost the desktop the fizz alone.
 */
static gboolean
fizz_prog_ensure(GowlFxGl *self)
{
	GowlFxFizzProg *p = &self->fizz;

	if (p->program != 0)
		return TRUE;
	if (self->fizz_tried)
		return FALSE;
	self->fizz_tried = TRUE;

	p->program = gowl_fx_link_program(fizz_vert_src, fizz_frag_src);
	if (p->program == 0) {
		g_warning("fx: the carbonation shader would not build, so fizz "
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
	p->u_cling_t    = glGetUniformLocation(p->program, "u_cling_t");
	p->u_rise       = glGetUniformLocation(p->program, "u_rise");
	p->u_cell       = glGetUniformLocation(p->program, "u_cell");
	p->u_bubble     = glGetUniformLocation(p->program, "u_bubble");
	p->u_growth     = glGetUniformLocation(p->program, "u_growth");
	p->u_sites      = glGetUniformLocation(p->program, "u_sites");
	p->u_site_width = glGetUniformLocation(p->program, "u_site_width");
	p->u_spacing    = glGetUniformLocation(p->program, "u_spacing");
	p->u_stray      = glGetUniformLocation(p->program, "u_stray");
	p->u_cling      = glGetUniformLocation(p->program, "u_cling");
	p->u_wobble     = glGetUniformLocation(p->program, "u_wobble");
	p->u_foam       = glGetUniformLocation(p->program, "u_foam");
	p->u_foam_depth = glGetUniformLocation(p->program, "u_foam_depth");
	p->u_depth      = glGetUniformLocation(p->program, "u_depth");
	p->u_dispersion = glGetUniformLocation(p->program, "u_dispersion");
	p->u_mirror     = glGetUniformLocation(p->program, "u_mirror");
	p->u_fog        = glGetUniformLocation(p->program, "u_fog");
	p->u_clarity    = glGetUniformLocation(p->program, "u_clarity");
	p->u_specular   = glGetUniformLocation(p->program, "u_specular");
	p->u_shine      = glGetUniformLocation(p->program, "u_shine");
	p->u_rim        = glGetUniformLocation(p->program, "u_rim");
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
gowl_fx_fizz_params_init(GowlFxFizzParams *params)
{
	if (params == NULL)
		return;

	memset(params, 0, sizeof(*params));
	/*
	 * A soda: the middle preset's numbers, so a caller that sets only a
	 * size gets a fizzy drink rather than a flat one.
	 *
	 * `cell' is the ruler and `bubble' is a FRACTION of it, so the two
	 * move together and a bigger cell does not merely spread the same
	 * bubbles further apart.  At 120 px with a bubble fraction of 0.085
	 * the release radius is about ten pixels, growing to sixteen at the
	 * top -- which on a HiDPI panel is a bubble you can see is a ring.
	 *
	 * The trains are what carry the effect, so `sites' is generous and
	 * `stray' is not: a drink that is mostly loose bubbles is a drink
	 * somebody has shaken.
	 *
	 * The first set of numbers here put about ten bubbles on a window
	 * nine hundred pixels across, which is not a fizzy drink -- it is a
	 * flat one with a few stragglers.  The count that matters is sites
	 * per screen times bubbles per cycle, so both went up together: a
	 * narrower column with the same site fraction is more trains, and
	 * more trains is the thing the eye is actually counting.
	 */
	params->cell       = 110.0f;
	params->bubble     = 0.085f;
	params->growth     = 0.60f;
	params->sites      = 0.46f;
	params->site_width = 92.0f;
	params->spacing    = 0.72f;
	params->stray      = 0.30f;
	params->cling      = 0.26f;
	params->wobble     = 14.0f;
	params->foam       = 0.55f;
	params->foam_depth = 150.0f;
	params->depth      = 3.2f;
	params->dispersion = 0.6f;
	params->mirror     = 0.70f;
	params->fog        = 0.55f;
	params->clarity    = 0.90f;
	params->specular   = 0.55f;
	params->shine      = 70.0f;
	params->rim        = 0.28f;
	params->absorption = 0.14f;
	params->brightness = 1.0f;
	params->alpha      = 1.0f;
	params->src_scale  = 1.0f;
	/* A pale straw, which is what most fizzy things are.  Kept weak:
	 * the tint applies across the whole pane here rather than only in
	 * the bubbles, so a strong one is a coloured filter over the
	 * desktop. */
	params->tint[0] = 1.00f;
	params->tint[1] = 0.96f;
	params->tint[2] = 0.86f;
	/* Up and to the left, well off the glass, for the reason the rain
	 * gives: a light near the plane puts the glint on every rim at once. */
	params->light[0] = -0.42f;
	params->light[1] = -0.55f;
	params->light[2] =  0.72f;
}

void
gowl_fx_fizz_advance(GowlFxFizzClock *clock, gdouble dt, gdouble speed,
                     gdouble cling_seconds)
{
	/*
	 * The three rise rates, irrational against each other for the reason
	 * gowl_fx_rain_advance() sets out: rates that look incommensurate and
	 * are actually k/100 all return to zero together once every hundred
	 * seconds, and the whole field repeats exactly.  1/phi and 1/phi^2
	 * have no common period, and they sum to 1, which keeps them spread
	 * rather than bunched.
	 *
	 * Scaled to a third: at a rate of 1 a bubble would cross the glass in
	 * a second, which is a jet and not a drink.  A third of that is about
	 * three seconds bottom to top at the shipped speed, which is what a
	 * bubble in a soda takes.
	 */
	static const gdouble rate[3] = {
		0.3333333333333333, 0.2060113295832983, 0.1273220037500350
	};
	gint i;

	if (clock == NULL)
		return;
	if (!(dt > 0.0))
		return;
	if (dt > 0.25)
		dt = 0.25;   /* a long stall is not a long pour */
	if (!(cling_seconds > 0.1))
		cling_seconds = 14.0;

	/*
	 * Cycles, wrapped into [0, GOWL_FX_FIZZ_CYCLES), exactly as the rain:
	 * the fraction is where a bubble has got to and the whole part is
	 * which bubble it is.  Every multiplier applied inside the shader is
	 * a whole number -- the per-column rise rate is 1, 2 or 3 and the
	 * bubbles per cycle is an integer -- so both halves stay continuous
	 * across the wrap.
	 */
	clock->cling += dt / cling_seconds;
	clock->cling = fmod(clock->cling, GOWL_FX_FIZZ_CYCLES);
	if (clock->cling < 0.0)
		clock->cling += GOWL_FX_FIZZ_CYCLES;

	for (i = 0; i < 3; i++) {
		clock->rise[i] += dt * speed * rate[i];
		clock->rise[i] = fmod(clock->rise[i], GOWL_FX_FIZZ_CYCLES);
		if (clock->rise[i] < 0.0)
			clock->rise[i] += GOWL_FX_FIZZ_CYCLES;
	}
}

gboolean
gowl_fx_pass_fizz(GowlFxPass             *pass,
                  const GowlFxTexture    *soft,
                  const GowlFxTexture    *sharp,
                  const GowlFxFizzParams *params,
                  const GowlFxFizzClock  *clock)
{
	GowlFxGl             *gl;
	const GowlFxFizzProg *p;
	const GowlFxTexture  *clear_src;
	GowlFxFizzClock       still;
	gfloat                rise[3];
	gfloat                radius;
	gfloat                width, bubble, growth;
	gint                  i;

	if (pass == NULL || params == NULL || soft == NULL)
		return FALSE;
	if (params->width <= 0 || params->height <= 0)
		return FALSE;
	if (soft->tex == 0 || soft->width <= 0 || soft->height <= 0)
		return FALSE;

	gl = pass->gl;
	if (!fizz_prog_ensure(gl))
		return FALSE;
	p = &gl->fizz;

	if (clock == NULL) {
		memset(&still, 0, sizeof(still));
		clock = &still;
	}
	for (i = 0; i < 3; i++)
		rise[i] = (gfloat)clock->rise[i];

	radius = CLAMP(params->radius, 0.0f,
	               (gfloat)MIN(params->width, params->height) * 0.5f);
	width  = MAX(12.0f, params->site_width);

	clear_src = (sharp != NULL && sharp->tex != 0) ? sharp : soft;

	glUseProgram(p->program);
	glUniform2f(p->u_src_origin, params->src_origin[0], params->src_origin[1]);
	glUniform2f(p->u_src_size, (gfloat)MAX(1, soft->width),
	            (gfloat)MAX(1, soft->height));
	glUniform1f(p->u_src_scale,
	            params->src_scale > 0.0f ? params->src_scale : 1.0f);
	glUniform2f(p->u_size, (gfloat)params->width, (gfloat)params->height);
	glUniform1f(p->u_radius, radius);
	glUniform1f(p->u_cling_t, (gfloat)clock->cling);
	glUniform3fv(p->u_rise, 1, rise);
	/*
	 * THE RELEASE RADIUS IS CAPPED AGAINST THE GROWTH, and it has to be.
	 *
	 * A bubble ends its rise (1 + growth) times the size it started, so
	 * the reach of a train is a function of BOTH knobs -- and the
	 * three-column lookup in train_layer() is worked out with a grown
	 * bubble of at most 0.35 of a column in it.  A config that turned the
	 * growth up without turning the bubble down would have the big ones
	 * near the top sliced off down a straight vertical line at the column
	 * edge, which is a lattice drawn in the negative.  1.28 is the
	 * largest per-bubble size jitter the layer applies.
	 *
	 * The wobble is capped the same way and for the same sum: it is a
	 * length in pixels, so it is the one term that does not scale itself
	 * with the layout.
	 */
	growth = CLAMP(params->growth, 0.0f, 3.0f);
	bubble = CLAMP(params->bubble, 0.01f, 0.22f);
	bubble = MIN(bubble, 0.35f / (1.28f * (1.0f + growth)));

	glUniform1f(p->u_cell, MAX(8.0f, params->cell));
	glUniform1f(p->u_bubble, bubble);
	glUniform1f(p->u_growth, growth);
	glUniform1f(p->u_sites, CLAMP(params->sites, 0.0f, 1.0f));
	glUniform1f(p->u_site_width, width);
	glUniform1f(p->u_spacing, CLAMP(params->spacing, 0.0f, 1.0f));
	glUniform1f(p->u_stray, CLAMP(params->stray, 0.0f, 1.0f));
	glUniform1f(p->u_cling, CLAMP(params->cling, 0.0f, 1.0f));
	glUniform1f(p->u_wobble, CLAMP(params->wobble, 0.0f, width * 0.25f));
	glUniform1f(p->u_foam, CLAMP(params->foam, 0.0f, 1.0f));
	glUniform1f(p->u_foam_depth, MAX(0.0f, params->foam_depth));
	glUniform1f(p->u_depth, CLAMP(params->depth, 0.0f, 20.0f));
	glUniform1f(p->u_dispersion, MAX(0.0f, params->dispersion));
	glUniform1f(p->u_mirror, CLAMP(params->mirror, 0.0f, 1.0f));
	glUniform1f(p->u_fog, CLAMP(params->fog, 0.0f, 1.0f));
	glUniform1f(p->u_clarity, CLAMP(params->clarity, 0.0f, 1.0f));
	glUniform1f(p->u_specular, MAX(0.0f, params->specular));
	glUniform1f(p->u_shine, MAX(1.0f, params->shine));
	glUniform1f(p->u_rim, CLAMP(params->rim, 0.0f, 1.0f));
	glUniform3fv(p->u_light, 1, params->light);
	glUniform3fv(p->u_tint, 1, params->tint);
	glUniform1f(p->u_absorb, CLAMP(params->absorption, 0.0f, 1.0f));
	glUniform1f(p->u_brightness, MAX(0.0f, params->brightness));
	glUniform1f(p->u_alpha, CLAMP(params->alpha, 0.0f, 1.0f));
	/* Wrapped rather than clamped: the caller hands over whatever
	 * identifies the window and any value is as good as any other. */
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
