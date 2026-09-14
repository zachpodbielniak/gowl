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
 * gowl-fx-rain.c -- rain on the glass, and the wallpaper seen through it.
 *
 * The water next door is one surface over the whole window.  This is the
 * opposite shape of problem: a few hundred SEPARATE surfaces, each a
 * couple of dozen pixels across, each with its own life, and the glass
 * between them merely wet.  Nothing about the water's machinery carries
 * over -- there is no height field here, no Laplacian, no swell.
 *
 * WHAT MAKES IT READ AS RAIN, in the order the eye notices:
 *
 *   1. THE DROPS ARE LENSES AND THE GLASS IS NOT.  A drop is a clean
 *      spherical cap: the wallpaper through it is SHARP, bent, and past a
 *      certain size upside down, because that is what a ball lens does.
 *      The pane around it is frosted.  That contrast is the whole effect;
 *      a field of blurred blobs on blurred glass reads as a smudge.
 *   2. THEY COLLECT, THEN THEY GO.  A resting drop grows from nothing,
 *      sits, and at the end of its life slides downward and fades -- it
 *      got heavy and left.  A window where every drop simply blinks in
 *      and out is uncanny in about ten seconds.
 *   3. THEY RUN STRAIGHT DOWN, AND THEY WANDER WHILE THEY DO IT.  Gravity
 *      has no x component, so a running drop is a column; a real one
 *      still meanders, because the pane is not clean and it follows what
 *      is already wet.
 *   4. THEY LEAVE A TRAIL, AND THE TRAIL IS BEADED.  Behind a head is a
 *      narrow clear path with a line of residual beads in it, which
 *      linger and shrink long after the head has gone.  This is the
 *      single most recognisable thing about rain on a window and the
 *      first thing a naive implementation leaves out.
 *   5. IMPACTS.  A drop that has just landed throws a brief ring.
 *
 * WHY IT IS NOT A GRID.  The liquid-water shader had to be rescued from
 * exactly this, and the cause there was periodic functions: a product of
 * sines is a lattice however you rotate it.  Nothing here is periodic in
 * space.  The resting drops are a JITTERED grid -- a cell decides whether
 * it holds a drop at all (most do not), and where in itself it sits, over
 * the cell's whole area -- which at these fill rates is visually a
 * Poisson field.  Two such layers at an irrational scale ratio and turned
 * against each other, and three column layers of different widths and
 * speeds, leave nothing for the eye to lock on to.
 *
 * THE NEIGHBOURHOOD IS 3x3 AND THAT IS NOT NEGOTIABLE.  A drop centre
 * jittered over its whole cell can reach a pixel a cell and a half away,
 * so a 2x2 lookup would clip drops along cell edges into crescents --
 * which is a grid, drawn in the negative.  Nine cells per layer is what
 * full jitter costs.
 *
 * THE CLOCKS ARE WRAPPED FRACTIONS, NOT A TIME.  Same lesson as the
 * water, reached by a different route: everything here is fract(t + x),
 * which is continuous across a wrap of t at 1.0 -- so the caller keeps
 * each clock in a double, wraps it into [0, 1), and a float holds it
 * exactly forever.  The per-column speed multiplier is therefore an
 * INTEGER: fract(k*(t + 1)) = fract(k*t) only when k is whole, and a
 * multiplier of 1.5 would snap every running drop sideways once a cycle.
 */

#undef G_LOG_DOMAIN
#define G_LOG_DOMAIN "gowl-fx"

#include "gowl-fx-private.h"

#include <math.h>
#include <string.h>

static const gchar rain_vert_src[] =
	"attribute vec2 a_pos;\n"
	"attribute vec2 a_uv;\n"
	"varying vec2 v_uv;\n"
	"void main() {\n"
	"  v_uv = a_uv;\n"
	"  gl_Position = vec4(a_pos, 0.0, 1.0);\n"
	"}\n";

/*
 * highp is asked for conditionally, for the reason the glass and the
 * water shaders both give: naming a precision that does not exist fails
 * to COMPILE rather than falling back.  It is wanted here for the cell
 * coordinates -- a wide window is a hundred and more cells across, and
 * at mediump the hash of a far cell collides with its neighbour's, which
 * shows up as drops in pairs down one side of the screen.
 */
static const gchar rain_frag_src[] =
	"#ifdef GL_FRAGMENT_PRECISION_HIGH\n"
	"precision highp float;\n"
	"#else\n"
	"precision mediump float;\n"
	"#endif\n"
	"\n"
	"uniform sampler2D u_soft;      /* the frosted wallpaper */\n"
	"uniform sampler2D u_sharp;     /* the same wallpaper, unblurred */\n"
	"uniform vec2  u_src_origin;    /* where this rect sits in them, px */\n"
	"uniform vec2  u_src_size;      /* their size, px */\n"
	"uniform float u_src_scale;     /* source px per buffer px */\n"
	"uniform vec2  u_size;          /* the rect, px */\n"
	"uniform float u_radius;        /* corner radius, px */\n"
	"uniform float u_life;          /* resting-drop clock, [0,1) */\n"
	"uniform vec3  u_run;           /* the three running clocks, [0,1) */\n"
	"uniform float u_cell;          /* px per cell of the fine drop layer */\n"
	"uniform float u_density;       /* how many cells hold a drop, 0..1 */\n"
	"uniform float u_bulge;         /* how domed a drop is */\n"
	"uniform float u_depth;         /* ray travel, in drop radii */\n"
	"uniform float u_dispersion;    /* channel separation inside a drop */\n"
	"uniform float u_runs;          /* how many columns run, 0..1 */\n"
	"uniform float u_run_width;     /* px per column of the fine layer */\n"
	"uniform float u_run_len;       /* px of trail behind a head */\n"
	"uniform float u_beads;         /* residual beads left in a trail */\n"
	"uniform float u_fog;           /* how frosted the dry pane is, 0..1 */\n"
	"uniform float u_clarity;       /* how clear a drop is, 0..1 */\n"
	"uniform float u_specular;\n"
	"uniform float u_shine;         /* specular exponent */\n"
	"uniform float u_rim;           /* the dark contact ring */\n"
	"uniform float u_impact;        /* rings thrown by a landing drop */\n"
	"uniform vec3  u_light;\n"
	"uniform vec3  u_tint;\n"
	"uniform float u_absorb;\n"
	"uniform float u_brightness;\n"
	"uniform float u_alpha;\n"
	"uniform float u_seed;         /* which crop of the field, 0..16 */\n"
	"varying vec2 v_uv;\n"
	"\n"
	"const float N_WATER = 1.333;\n"
	"const float TAU = 6.2831853;\n"
	"/* How far a resting drop may slide before it fades, in cells.  Bounded\n"
	" * by the nine-cell lookup: 1.0 (jitter) + 0.28 (radius) + this must\n"
	" * not exceed 1.5.  See drop_layer(). */\n"
	"const float RAIN_SLIDE = 0.2;\n"
	"/*\n"
	" * How many runs, and how many drop lives, before the field repeats.\n"
	" *\n"
	" * The clocks are wrapped at this and the run and life indexes taken\n"
	" * mod it, which is what makes both continuous across the wrap: the\n"
	" * position is fract() either side, and the index changes by a\n"
	" * multiple of RAIN_CYCLES, which is zero mod RAIN_CYCLES.  The number\n"
	" * itself is a trade -- larger repeats later, and quantises the head\n"
	" * position more coarsely because a float has to hold the clock.  At\n"
	" * 256 a run comes round again after some tens of minutes per column,\n"
	" * and the head steps to within a fifth of a pixel.\n"
	" */\n"
	"const float RAIN_CYCLES = 256.0;\n"
	"\n"
	"/* Signed distance to a rounded rect centred in @size; negative inside. */\n"
	"float sdf_rect(vec2 p, vec2 size, float r) {\n"
	"  vec2 h = size * 0.5;\n"
	"  vec2 q = abs(p - h) - (h - vec2(r));\n"
	"  return min(max(q.x, q.y), 0.0) + length(max(q, vec2(0.0))) - r;\n"
	"}\n"
	"\n"
	"/*\n"
	" * Three uncorrelated numbers from THREE inputs.\n"
	" *\n"
	" * Three, because everything here is indexed by where AND by which time\n"
	" * round: a column and the run going down it now, a cell and the drop\n"
	" * sitting in it now.  The first version of this took two, and one of the\n"
	" * two ended up a per-layer constant -- which collapses the hash into a\n"
	" * one-dimensional sin sequence stepped by a fixed angle along the other,\n"
	" * and such a sequence ALIASES.\n"
	" *\n"
	" * That is not a theoretical worry, it was measured.  The presence test\n"
	" * came out bimodal: two of the three running layers had two live columns\n"
	" * out of eighteen where the density asked for six, and three columns\n"
	" * towards the right were dry in every layer at once.  A third of the\n"
	" * window never had a drop run down it, and never would have.\n"
	" */\n"
	"vec3 hash3v(vec3 c) {\n"
	"  vec3 n = vec3(dot(c, vec3(127.1,  89.3,  54.7)),\n"
	"                dot(c, vec3( 71.9, 151.3, 101.7)),\n"
	"                dot(c, vec3(167.3,  61.7, 133.9)));\n"
	"  return fract(sin(n) * vec3(43758.5453, 28001.8384, 19349.6631));\n"
	"}\n"
	"\n"
	"/*\n"
	" * ONE RESTING DROP, folded into the running best.\n"
	" *\n"
	" * @best carries (q.xy, radius, coverage) for whichever drop has the\n"
	" * strongest claim on this pixel, where q is the offset from the drop's\n"
	" * centre in units of its own radius -- everything the lighting and the\n"
	" * refraction need, and nothing that depends on which layer it came\n"
	" * from.  Drops are not blended: two overlapping drops on a window MERGE\n"
	" * into one, they do not become translucent, so the nearer centre simply\n"
	" * wins.\n"
	" */\n"
	"void drop_at(vec2 p, vec2 centre, float r, float alive,\n"
	"             inout vec4 best, inout float wet) {\n"
	"  if (alive <= 0.0 || r <= 0.0)\n"
	"    return;\n"
	"\n"
	"  vec2  d  = p - centre;\n"
	"  float l  = length(d);\n"
	"  float aa = max(1.0, r * 0.12);\n"
	"\n"
	"  /* Edges in INCREASING order, here and everywhere below.  GLSL\n"
	"   * leaves smoothstep(hi, lo, x) undefined; Mesa happens to do the\n"
	"   * sensible thing with it and another driver need not. */\n"
	"  float cov = (1.0 - smoothstep(r - aa, r, l)) * alive;\n"
	"  if (cov <= 0.0)\n"
	"    return;\n"
	"\n"
	"  /* Wet beyond the drop's own edge: a drop sits in a film it has\n"
	"   * pulled in around itself, and a lens with a hard frosted border is\n"
	"   * the one thing that makes these read as stickers. */\n"
	"  wet = max(wet, cov * 0.85\n"
	"            + (1.0 - smoothstep(r * 0.95, r * 1.45, l)) * 0.35 * alive);\n"
	"\n"
	"  if (cov > best.w) {\n"
	"    best.xy = d / max(r, 0.001);\n"
	"    best.z  = r;\n"
	"    best.w  = cov;\n"
	"  }\n"
	"}\n"
	"\n"
	"/*\n"
	" * THE SPLASH, which is a ring and not a star.\n"
	" *\n"
	" * A drop landing on a wet pane throws a circle of disturbed film\n"
	" * outwards for a moment.  It is drawn only across the first eighth of\n"
	" * a lifetime, and separately from the drop itself because it reaches\n"
	" * OUTSIDE the drop: the radius is capped at 1.6 of the drop\'s own,\n"
	" * which is as far as it can go and still be found by the 3x3\n"
	" * neighbourhood the cell was looked up in.  Any further and rings\n"
	" * would be sliced into arcs along the cell edges -- a lattice, drawn\n"
	" * once a lifetime.\n"
	" */\n"
	"void impact_at(vec2 p, vec2 centre, float r, float age,\n"
	"               inout float ring) {\n"
	"  if (u_impact <= 0.001 || age > 0.12 || r <= 0.0)\n"
	"    return;\n"
	"\n"
	"  float t  = age / 0.12;\n"
	"  float rr = r * (0.35 + 1.25 * t);\n"
	"  float aa = max(1.0, r * 0.3);\n"
	"\n"
	"  ring += u_impact * (1.0 - t)\n"
	"          * (1.0 - smoothstep(0.0, aa, abs(length(p - centre) - rr)));\n"
	"}\n"
	"\n"
	"/*\n"
	" * ONE LAYER OF RESTING DROPS: a jittered grid, mostly empty.\n"
	" *\n"
	" * @ang turns the whole lattice and the caller gives each layer a\n"
	" * different @cell as well, at a ratio that is not a simple fraction.\n"
	" * Turning alone is not enough -- a rotated grid is still a grid -- but\n"
	" * two turned grids of incommensurate pitch, each under half full, have\n"
	" * no shared period for the eye to find.\n"
	" *\n"
	" * THE LIFECYCLE IS THE POINT OF THE LAYER, not decoration.  a is the\n"
	" * drop's age in [0,1), its own phase from its own cell, so the field is\n"
	" * always part new and part old rather than breathing in unison:\n"
	" *\n"
	" *   a < 0.22          growing -- condensation gathering\n"
	" *   0.22 .. 0.78      resting at full size\n"
	" *   a > 0.78          it got heavy: slides downward, accelerating, and\n"
	" *                     fades as it goes\n"
	" *\n"
	" * AND EACH LIFE IS A DIFFERENT DROP.  Only the PHASE belongs to the\n"
	" * cell; where the drop sits in it, how big it is and whether the cell\n"
	" * holds one at all are hashed against the life as well.  Pinning those\n"
	" * to the cell instead -- which is what this did first -- gives the same\n"
	" * thirty spots blinking on and off in place for as long as the session\n"
	" * lasts, and about a minute of watching is enough to see it.\n"
	" *\n"
	" * HOW FAR IT MAY SLIDE IS FIXED BY THE LOOKUP, not by taste.  A pixel\n"
	" * only asks the nine cells around it, which reach 1.5 cells away; a drop\n"
	" * centred anywhere in its own cell needs 1 of that, its radius needs\n"
	" * another 0.28, and what is left -- 0.2 of a cell -- is all the slide\n"
	" * there is room for.  Take more and drops that have started to go are\n"
	" * sliced off along the cell edges into flat-sided crescents, which is a\n"
	" * lattice drawn in the negative and the exact artefact the whole layer\n"
	" * is arranged to avoid.\n"
	" */\n"
	"void drop_layer(vec2 p, float cell, float density, float ang, float lyr,\n"
	"                inout vec4 best, inout float wet, inout float ring) {\n"
	"  if (density <= 0.0 || cell <= 0.0)\n"
	"    return;\n"
	"\n"
	"  float c  = cos(ang);\n"
	"  float s  = sin(ang);\n"
	"  mat2  R  = mat2(c, -s, s, c);\n"
	"  mat2  Rt = mat2(c, s, -s, c);        /* the way back */\n"
	"  /* Which crop of the field this window shows.  A pure translation of\n"
	"     the lattice, so nothing about the look changes -- but two windows\n"
	"     side by side stop being the same window twice. */\n"
	"  vec2  off  = vec2(u_seed * 0.41, u_seed * 0.73);\n"
	"  float lid  = lyr * 64.0 + u_seed;\n"
	"  vec2  q    = (R * p) / cell + off;\n"
	"  vec2  base = floor(q);\n"
	"  int   i, j;\n"
	"\n"
	"  for (j = -1; j <= 1; j++) {\n"
	"    for (i = -1; i <= 1; i++) {\n"
	"      vec2  id = base + vec2(float(i), float(j));\n"
	"      /* Permanent, and only the phase: which life this cell is on is\n"
	"         worked out FROM it, so it cannot depend on the life. */\n"
	"      vec3  hc = hash3v(vec3(id.x, id.y, lid));\n"
	"      float u  = u_life + hc.x;\n"
	"      float a  = fract(u);\n"
	"      float li = mod(floor(u), RAIN_CYCLES);\n"
	"      /* Per LIFE.  Wrapping the clock at a whole number of lives is what\n"
	"         lets this be continuous: the age is fract(u) either side of the\n"
	"         wrap, and the life index is taken mod the same number, so it\n"
	"         does not change there.  It changes when a drop ends, which is\n"
	"         the one instant its radius is zero and nobody can see it move. */\n"
	"      vec3  hr = hash3v(vec3(id.x + 37.0 + lid, id.y - 53.0, li));\n"
	"\n"
	"      /* Most cells are empty this time round, and which ones is fixed\n"
	"         for the whole life -- the field must not shimmer in and out as\n"
	"         an age crosses zero. */\n"
	"      if (hr.z > density)\n"
	"        continue;\n"
	"\n"
	"      float rr   = cell * (0.09 + 0.19 * hr.x);   /* <= 0.28 cells */\n"
	"      float grow = smoothstep(0.0, 0.22, a);\n"
	"      float slip = max(0.0, a - 0.78) / 0.22;\n"
	"      vec2  cen  = (id + vec2(hr.y, hc.y) - off) * cell;\n"
	"\n"
	"      /* Down the SCREEN, not down the lattice: the layer is turned and\n"
	"         gravity is not, so the slide is applied after coming back out of\n"
	"         the rotated space. */\n"
	"      cen = Rt * cen;\n"
	"      cen.y += slip * slip * cell * RAIN_SLIDE;\n"
	"\n"
	"      impact_at(p, cen, rr, a, ring);\n"
	"      drop_at(p, cen, rr * grow, grow * (1.0 - slip), best, wet);\n"
	"    }\n"
	"  }\n"
	"}\n"
	"\n"
	"/*\n"
	" * ONE LAYER OF RUNNING DROPS, with their trails.\n"
	" *\n"
	" * Columns, because that is what gravity gives: a drop running down a\n"
	" * pane runs straight down, and a field of drops drifting diagonally is\n"
	" * instantly wrong.  What stops columns READING as columns is that a\n"
	" * column is dry most of the time, that its drop sits anywhere across its\n"
	" * width, that it wanders as it falls, and that three of these run over\n"
	" * each other at widths and speeds that do not divide.\n"
	" *\n"
	" * A TRACK IS NOT A PERMANENT FEATURE, and getting that wrong is what\n"
	" * made the first version of this look like a screensaver.  Only the\n"
	" * cycle -- how fast this column runs and where in the cycle it starts --\n"
	" * belongs to the column.  Whether it runs AT ALL this time round, where\n"
	" * across its width, how big, how long a trail and which way it wanders\n"
	" * are hashed against the RUN, so a column that ran a moment ago may be\n"
	" * dry for the next four cycles and the drop after that is a different\n"
	" * drop on a different line.  Measured on the old code, a third of a\n"
	" * window had no running drop in it at any moment of eighty seconds, and\n"
	" * the columns that did run ran every single cycle.\n"
	" *\n"
	" * THE SPEED MULTIPLIER IS AN INTEGER, and it has to be.  fract(k*t + ph)\n"
	" * is where the head is and floor(k*t + ph) is which run this is; with\n"
	" * the clock wrapped at RAIN_CYCLES, both are continuous across the wrap\n"
	" * only when k is whole -- the position because k*RAIN_CYCLES is an\n"
	" * integer, the run index because it is taken mod RAIN_CYCLES and\n"
	" * k*RAIN_CYCLES is a multiple of it.  A k of 1.5 would teleport every\n"
	" * running drop in the layer once a cycle.\n"
	" *\n"
	" * THE TRAIL IS THE PATH, EVALUATED BACKWARDS.  The head's wander is a\n"
	" * function of its height, so the wetness left at height y is that same\n"
	" * function at y -- no history to store, and the trail bends exactly the\n"
	" * way the drop did.\n"
	" */\n"
	"void run_layer(vec2 p, float w, float t, float density, float lyr,\n"
	"               inout vec4 best, inout float wet) {\n"
	"  if (density <= 0.0 || w <= 0.0)\n"
	"    return;\n"
	"\n"
	"  float lid = lyr * 64.0 + u_seed;\n"
	"  /* Where this layer's column grid starts, per layer and per window. */\n"
	"  float gph = lyr * 0.37 + u_seed * 0.061;\n"
	"  int   n;\n"
	"\n"
	"  /*\n"
	"   * THREE COLUMNS, and the number is arithmetic rather than taste.  A\n"
	"   * drop sits up to a quarter of a width from its column's centre and\n"
	"   * wanders another 0.45 as it falls, so with a radius of up to 0.32 it\n"
	"   * reaches just over one width to either side.  A pixel is at least\n"
	"   * (|k| - 0.5) widths from the centre of the column k away, so k of 2\n"
	"   * can never reach it and k of 1 can -- ask two columns and every drop\n"
	"   * that wandered left is sliced off down a straight vertical line at\n"
	"   * the column edge it came from.\n"
	"   */\n"
	"  for (n = -1; n <= 1; n++) {\n"
	"    float col = floor(p.x / w + gph) + float(n);\n"
	"    /* Per COLUMN and permanent: the rate and the phase.  Neither may\n"
	"       depend on which run it is, because which run it is is worked out\n"
	"       from them. */\n"
	"    vec3  hc = hash3v(vec3(col, lid, 0.0));\n"
	"    float k  = 1.0 + floor(hc.z * 2.999);       /* 1, 2 or 3, never 1.5 */\n"
	"    float u  = t * k + hc.x;\n"
	"    float ri = mod(floor(u), RAIN_CYCLES);\n"
	"    /* Per RUN. */\n"
	"    vec3  hr = hash3v(vec3(col, lid + 8.0, ri));\n"
	"\n"
	"    if (hr.z > density)\n"
	"      continue;\n"
	"\n"
	"    vec3  hs   = hash3v(vec3(col, lid + 24.0, ri));\n"
	"    float len  = u_run_len * (0.6 + 0.8 * hs.x);\n"
	"    float span = u_size.y + 2.0 * len + w * 2.0;\n"
	"    float head = fract(u) * span - len - w;\n"
	"    float rr   = w * (0.15 + 0.17 * hr.x);\n"
	"    float lane = (col - gph + 0.5) * w + (hr.y - 0.5) * w * 0.5;\n"
	"\n"
	"    /* Where the drop is when it is at height y.  Two turns of a slow\n"
	"       sine, so it leans one way and then the other down the pane rather\n"
	"       than snaking evenly -- and both phases are per RUN, so the next\n"
	"       drop down this column does not retrace the last one's line. */\n"
	"    float wob = w * 0.28;\n"
	"    float xh  = lane + sin(head * 0.021 + hs.y * TAU) * wob\n"
	"                     + sin(head * 0.0067 + hs.z * TAU) * wob * 0.6;\n"
	"\n"
	"    /* The head. */\n"
	"    drop_at(p, vec2(xh, head), rr, 1.0, best, wet);\n"
	"\n"
	"    /*\n"
	"     * The wet path behind it, narrowing and drying with distance.\n"
	"     *\n"
	"     * This is the entire reason the trail is visible: it clears the\n"
	"     * frost, so the streak is a SHARP stripe through a soft pane rather\n"
	"     * than a pale line painted over it.\n"
	"     *\n"
	"     * NARROWER than the head, and drying faster than it fades.  A band\n"
	"     * as wide as the drop with an even taper behind it is a COMET,\n"
	"     * which is what this looked like first: a glossy ribbon with a\n"
	"     * ball on the end.  What a run down a window leaves is a thin wet\n"
	"     * line that the beads below are most of.\n"
	"     */\n"
	"    float back = head - p.y;\n"
	"    if (back > 0.0 && back < len) {\n"
	"      float u2 = back / len;\n"
	"      float xy = lane + sin(p.y * 0.021 + hs.y * TAU) * wob\n"
	"                      + sin(p.y * 0.0067 + hs.z * TAU) * wob * 0.6;\n"
	"      float tw = rr * (0.80 - 0.42 * u2);\n"
	"      float m  = (1.0 - smoothstep(tw * 0.5, tw, abs(p.x - xy)))\n"
	"                 * (1.0 - u2) * (1.0 - u2 * 0.6);\n"
	"      wet = max(wet, m);\n"
	"\n"
	"      /*\n"
	"       * AND THE TRAIL IS WATER, so it bends light too.\n"
	"       *\n"
	"       * A trail is a half-cylinder rather than a cap: it curves across\n"
	"       * the column and not along it, so it is exactly a drop whose\n"
	"       * offset has no y component, and the same lens, glint and rim\n"
	"       * below then apply to it for nothing.  Without this a trail is\n"
	"       * only a place where the frost is missing, which on a lightly\n"
	"       * frosted pane is almost nothing at all -- and a streak that does\n"
	"       * not refract is the clearest sign in the picture that it was\n"
	"       * painted on rather than run down.\n"
	"       *\n"
	"       * Claimed at three quarters, so a bead sitting IN the trail still\n"
	"       * wins its own pixels.\n"
	"       */\n"
	"      float tc = m * 0.6;\n"
	"      if (tc > best.w) {\n"
	"        /* Half the slope and two thirds the radius of a drop the same\n"
	"           width: a run is a shallow film and not a half-pipe, and at a\n"
	"           drop's curvature its edges throw the wallpaper so far sideways\n"
	"           that all that is left of the streak is a bright hair down the\n"
	"           middle. */\n"
	"        best.xy = vec2(clamp((p.x - xy) / max(tw, 1.0), -1.0, 1.0) * 0.5,\n"
	"                       0.0);\n"
	"        best.z  = tw * 0.6;\n"
	"        best.w  = tc;\n"
	"      }\n"
	"    }\n"
	"\n"
	"    /*\n"
	"     * The beads the trail is actually made of.  A clean wet stripe is\n"
	"     * what a finger leaves; rain leaves a broken line of little drops\n"
	"     * that stop where they are and shrink for a long time afterwards.\n"
	"     * Their spacing is hashed per position down the column, and per run,\n"
	"     * so successive drops do not leave the same beads in the same places.\n"
	"     *\n"
	"     * OUTSIDE the trail test above, deliberately.  A bead exists because\n"
	"     * the head went past IT, which is a fact about the bead's own\n"
	"     * height; asking whether the PIXEL is still inside the band cuts\n"
	"     * every bead that straddles either end of it off along a straight\n"
	"     * horizontal line.  Two bead cells for the same reason the columns\n"
	"     * are three.\n"
	"     */\n"
	"    if (u_beads > 0.001) {\n"
	"      float gap = w * 0.42;\n"
	"      int   b;\n"
	"\n"
	"      for (b = -1; b <= 0; b++) {\n"
	"        float bi  = floor(p.y / gap) + float(b);\n"
	"        vec3  bh  = hash3v(vec3(col + lid, bi + 211.0, ri));\n"
	"        float by  = (bi + bh.x) * gap;\n"
	"        float age = (head - by) / max(len, 1.0);\n"
	"\n"
	"        if (age <= 0.0 || age > 1.6 || bh.z >= u_beads)\n"
	"          continue;\n"
	"        float bx = lane + sin(by * 0.021 + hs.y * TAU) * wob\n"
	"                        + sin(by * 0.0067 + hs.z * TAU) * wob * 0.6\n"
	"                        + (bh.y - 0.5) * rr;\n"
	"        /* Shrinking, not vanishing: a bead left on a window stays for a\n"
	"           long time and gets smaller, and stopping it dead at the end of\n"
	"           the trail makes the whole streak blink. */\n"
	"        drop_at(p, vec2(bx, by), rr * (0.30 + 0.40 * bh.y)\n"
	"                * clamp(1.25 - age * 0.75, 0.0, 1.0),\n"
	"                clamp(1.5 - age * 0.8, 0.0, 1.0), best, wet);\n"
	"      }\n"
	"    }\n"
	"  }\n"
	"}\n"
	"\n"
	"/* One tap of the wallpaper, @fog of the way towards frosted. */\n"
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
	"  float wet  = 0.0;\n"
	"  float ring = 0.0;\n"
	"\n"
	"  /* Two resting layers, turned against each other at a pitch ratio\n"
	"   * that is not a simple fraction.  The coarse one is sparser: big\n"
	"   * drops are rarer than small ones on a real pane, and a field with\n"
	"   * as many large as small reads as bubble wrap. */\n"
	"  drop_layer(p, u_cell,        u_density,        0.00, 0.0,\n"
	"             best, wet, ring);\n"
	"  drop_layer(p, u_cell * 1.73, u_density * 0.45, 0.81, 1.0,\n"
	"             best, wet, ring);\n"
	"\n"
	"  /* Three running layers.  Widths and clocks both irrational against\n"
	"   * each other, so no two heads ever pace one another for long. */\n"
	"  run_layer(p, u_run_width,        u_run.x, u_runs,        4.0,\n"
	"            best, wet);\n"
	"  run_layer(p, u_run_width * 1.61, u_run.y, u_runs * 0.72, 5.0,\n"
	"            best, wet);\n"
	"  run_layer(p, u_run_width * 2.53, u_run.z, u_runs * 0.42, 6.0,\n"
	"            best, wet);\n"
	"\n"
	"  /*\n"
	"   * THE DROP IS A LENS, AND A THICK ONE.\n"
	"   *\n"
	"   * The shape is a spherical cap, so the normal is (q, sqrt(1-|q|^2))\n"
	"   * with no derivative to take -- @u_bulge flattens it, which is the\n"
	"   * difference between a bead of water and a marble.  That normal is\n"
	"   * what the light bounces off, and it is used below for exactly that.\n"
	"   *\n"
	"   * It is NOT what decides where the drop looks, and getting that\n"
	"   * wrong is what made the first version of this read as glass beads.\n"
	"   * Refracting once at the front surface and marching the ray to the\n"
	"   * wallpaper is a FISHEYE: undeviated in the middle, bending harder\n"
	"   * towards the rim.  A water drop does not do that.  Light crosses\n"
	"   * it and refracts AGAIN on the way out, so the thing behaves as a\n"
	"   * ball lens -- and what a ball lens does, unmistakably, is turn the\n"
	"   * world behind it UPSIDE DOWN.\n"
	"   *\n"
	"   * The thin-lens mapping is the whole of it.  A sphere of radius r\n"
	"   * and index 1.333 has its focus at 2r; a wallpaper @u_depth radii\n"
	"   * behind maps to the drop with a magnification of 1 - depth/2, so\n"
	"   * the sample is simply the pixel reflected through the drop's\n"
	"   * centre and scaled by that.  Negative past a depth of 2, which is\n"
	"   * the inversion, falling out of the arithmetic rather than being\n"
	"   * painted in.  (A depth of exactly 2 is the focal plane: the whole\n"
	"   * drop shows one point, which is a real thing a lens does and not a\n"
	"   * setting anybody wants.)\n"
	"   *\n"
	"   * The last term puts some of the fisheye back at the very rim,\n"
	"   * where a real drop is thick enough that the thin-lens model gives\n"
	"   * up -- that smeared outer ring is half of what says \"water\".\n"
	"   */\n"
	"  vec2  disp = vec2(0.0);\n"
	"  vec3  n    = vec3(0.0, 0.0, 1.0);\n"
	"  float l    = 0.0;\n"
	"\n"
	"  if (best.w > 0.0) {\n"
	"    l = min(length(best.xy), 1.0);\n"
	"    n = normalize(vec3(best.xy * u_bulge, sqrt(max(1.0 - l * l, 1e-4))));\n"
	"    float rim = 0.75 + 0.25 / max(sqrt(max(1.0 - l * l, 1e-4)), 0.3);\n"
	"    disp = -best.xy * best.z * (u_depth * 0.5) * rim * best.w;\n"
	"  }\n"
	"\n"
	"  /*\n"
	"   * How frosted this pixel is.  A dry pane is @u_fog of the way to the\n"
	"   * blurred wallpaper; a drop is a clean lens and lifts it almost\n"
	"   * entirely; a wet path lifts part of it.  This is why the effect\n"
	"   * does not look like the blur with spots painted on: the spots are\n"
	"   * where the blur is NOT.\n"
	"   */\n"
	"  float clear = clamp(max(best.w, wet) * u_clarity, 0.0, 1.0);\n"
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
	"  /* What the water takes out of the light.  Barely anything, by\n"
	"   * default: rain is not a swimming pool and a blue cast over the\n"
	"   * whole window is the first thing that makes it look fake. */\n"
	"  if (u_absorb > 0.001)\n"
	"    col = mix(col, col * u_tint, clamp(u_absorb * best.w, 0.0, 1.0));\n"
	"\n"
	"  /* The contact ring.  Light entering near the rim of a drop leaves\n"
	"   * sideways and never reaches the eye, so the very edge of a drop is\n"
	"   * darker than its middle.  Kept to the OUTER SIXTH: any wider and\n"
	"   * every drop gets an outline, which is what a drawing of a bubble\n"
	"   * has and what a photograph of one does not. */\n"
	"  if (u_rim > 0.001 && best.w > 0.0)\n"
	"    col *= 1.0 - smoothstep(0.84, 1.0, l) * u_rim * best.w;\n"
	"\n"
	"  /*\n"
	"   * The glint, and the focus.\n"
	"   *\n"
	"   * Two highlights, because a drop has two.  The glint is the light\n"
	"   * reflected off the surface -- one tight spot, wherever the normal\n"
	"   * happens to bisect the light and the eye.  The focus is the light\n"
	"   * that went THROUGH: a lens gathers it towards the middle, which is\n"
	"   * why the centre of a real drop is brighter than the pane around\n"
	"   * it however dark the thing behind is.  Without the second one a\n"
	"   * field of lenses reads as dents in the glass.\n"
	"   */\n"
	"  if (u_specular > 0.001 && best.w > 0.0) {\n"
	"    vec3  V = vec3(0.0, 0.0, 1.0);\n"
	"    vec3  hv = normalize(u_light + V);\n"
	"    float s  = pow(max(dot(n, hv), 0.0), max(u_shine, 1.0));\n"
	"    float f  = pow(max(1.0 - l, 0.0), 3.0);\n"
	"    col += vec3((s + f * 0.22) * u_specular * best.w);\n"
	"  }\n"
	"\n"
	"  /* Impacts, last and faint: a ring of disturbed film, not a splash of\n"
	"   * white paint. */\n"
	"  if (ring > 0.0)\n"
	"    col += vec3(clamp(ring, 0.0, 1.0) * 0.22);\n"
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
 * cost the desktop the rain alone, not the cube, the overview, the
 * switcher, the magnifier, the blur, the glass and the water along with
 * it.
 */
static gboolean
rain_prog_ensure(GowlFxGl *self)
{
	GowlFxRainProg *p = &self->rain;

	if (p->program != 0)
		return TRUE;
	if (self->rain_tried)
		return FALSE;
	self->rain_tried = TRUE;

	p->program = gowl_fx_link_program(rain_vert_src, rain_frag_src);
	if (p->program == 0) {
		g_warning("fx: the liquid-rain shader would not build, so rain "
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
	p->u_run        = glGetUniformLocation(p->program, "u_run");
	p->u_cell       = glGetUniformLocation(p->program, "u_cell");
	p->u_density    = glGetUniformLocation(p->program, "u_density");
	p->u_bulge      = glGetUniformLocation(p->program, "u_bulge");
	p->u_depth      = glGetUniformLocation(p->program, "u_depth");
	p->u_dispersion = glGetUniformLocation(p->program, "u_dispersion");
	p->u_runs       = glGetUniformLocation(p->program, "u_runs");
	p->u_run_width  = glGetUniformLocation(p->program, "u_run_width");
	p->u_run_len    = glGetUniformLocation(p->program, "u_run_len");
	p->u_beads      = glGetUniformLocation(p->program, "u_beads");
	p->u_fog        = glGetUniformLocation(p->program, "u_fog");
	p->u_clarity    = glGetUniformLocation(p->program, "u_clarity");
	p->u_specular   = glGetUniformLocation(p->program, "u_specular");
	p->u_shine      = glGetUniformLocation(p->program, "u_shine");
	p->u_rim        = glGetUniformLocation(p->program, "u_rim");
	p->u_impact     = glGetUniformLocation(p->program, "u_impact");
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
gowl_fx_rain_params_init(GowlFxRainParams *params)
{
	if (params == NULL)
		return;

	memset(params, 0, sizeof(*params));
	/*
	 * A steady shower: the default preset's numbers, so a caller that
	 * sets only a size gets rain rather than a clean pane.
	 *
	 * `cell' is what everything else is measured against: a drop runs
	 * from a tenth to a bit over a quarter of one, and most cells are
	 * empty.  Ninety-odd pixels at a third full puts a drop every few
	 * centimetres of a HiDPI screen with radii in the tens of pixels,
	 * which is what a window in a shower looks like.
	 *
	 * The first numbers here were less than half that, and the mistake is
	 * worth recording: a cell that small buries a window under a thousand
	 * drops of four or five pixels each, which does not read as rain at
	 * all.  It reads as GRAIN, and the whole effect looks like a noisy
	 * blur rather than like weather.
	 */
	params->cell       = 95.0f;
	params->density    = 0.30f;
	params->bulge      = 1.0f;
	params->depth      = 6.0f;
	params->dispersion = 0.7f;
	params->runs       = 0.45f;
	params->run_width  = 170.0f;
	params->run_len    = 520.0f;
	params->beads      = 0.60f;
	params->fog        = 0.88f;
	params->clarity    = 0.95f;
	params->specular   = 0.42f;
	params->shine      = 60.0f;
	params->rim        = 0.30f;
	params->impact     = 0.5f;
	params->absorption = 0.10f;
	params->brightness = 1.0f;
	params->alpha      = 1.0f;
	params->src_scale  = 1.0f;
	params->tint[0] = 0.86f;
	params->tint[1] = 0.93f;
	params->tint[2] = 1.0f;
	/* Up and to the left, well off the glass: a light near the plane of
	 * the pane puts the glint on the rim of every drop at once, which
	 * reads as an outline rather than as a highlight. */
	params->light[0] = -0.42f;
	params->light[1] = -0.55f;
	params->light[2] =  0.72f;
}

void
gowl_fx_rain_advance(GowlFxRainClock *clock, gdouble dt, gdouble speed,
                     gdouble life_seconds)
{
	/*
	 * The three column rates, and they are IRRATIONAL on purpose.
	 *
	 * Rates of, say, 1.00, 0.61 and 0.37 look incommensurate and are not:
	 * they are 1/1, 61/100 and 37/100, so all three clocks return to zero
	 * together every hundred seconds and the entire running-drop field
	 * repeats exactly.  A hundred-second loop on something somebody has
	 * on screen all day is noticed.  1/phi and 1/phi^2 have no common
	 * period at all, and 1/phi + 1/phi^2 = 1 keeps them spread rather
	 * than bunched.
	 *
	 * The fifth they are scaled by is the whole difference between rain
	 * and a car wash.  At a rate of 1 a drop crosses the window in a
	 * SECOND, so every column that runs at all runs again immediately,
	 * and the handful of tracks that the old fixed-per-column presence
	 * left live were in constant use.  A fifth of that is five seconds
	 * at the shipped speed, which is about what a drop on a window
	 * actually takes.
	 */
	static const gdouble rate[3] = {
		0.2, 0.1236067977499790, 0.0763932022500210
	};
	gint i;

	if (clock == NULL)
		return;
	if (!(dt > 0.0))
		return;
	if (dt > 0.25)
		dt = 0.25;   /* a long stall is not a long downpour */
	if (!(life_seconds > 0.1))
		life_seconds = 8.0;

	/*
	 * Every clock here counts CYCLES, wrapped into [0, GOWL_FX_RAIN_CYCLES).
	 *
	 * The fractional part is where a drop has got to and the whole part
	 * is WHICH drop it is -- which run is going down this column, which
	 * life this cell is on -- and the shader hashes against that, so a
	 * track is not used by the same drop forever.
	 *
	 * Wrapping at a whole number of cycles is what keeps both continuous
	 * there: the fractional part is unchanged, and the whole part changes
	 * by a multiple of GOWL_FX_RAIN_CYCLES, which the shader has taken
	 * mod GOWL_FX_RAIN_CYCLES.  The one rule that comes with it is that
	 * any multiplier applied inside the shader must be a whole number,
	 * which is why the per-column speed there is 1, 2 or 3.
	 */
	clock->life += dt / life_seconds;
	clock->life = fmod(clock->life, GOWL_FX_RAIN_CYCLES);
	if (clock->life < 0.0)
		clock->life += GOWL_FX_RAIN_CYCLES;

	for (i = 0; i < 3; i++) {
		clock->run[i] += dt * speed * rate[i];
		clock->run[i] = fmod(clock->run[i], GOWL_FX_RAIN_CYCLES);
		if (clock->run[i] < 0.0)
			clock->run[i] += GOWL_FX_RAIN_CYCLES;
	}
}

gboolean
gowl_fx_pass_rain(GowlFxPass             *pass,
                  const GowlFxTexture    *soft,
                  const GowlFxTexture    *sharp,
                  const GowlFxRainParams *params,
                  const GowlFxRainClock  *clock)
{
	GowlFxGl             *gl;
	const GowlFxRainProg *p;
	const GowlFxTexture  *clear_src;
	GowlFxRainClock       still;
	gfloat                run[3];
	gfloat                radius;
	gint                  i;

	if (pass == NULL || params == NULL || soft == NULL)
		return FALSE;
	if (params->width <= 0 || params->height <= 0)
		return FALSE;
	if (soft->tex == 0 || soft->width <= 0 || soft->height <= 0)
		return FALSE;

	gl = pass->gl;
	if (!rain_prog_ensure(gl))
		return FALSE;
	p = &gl->rain;

	if (clock == NULL) {
		memset(&still, 0, sizeof(still));
		clock = &still;
	}
	for (i = 0; i < 3; i++)
		run[i] = (gfloat)clock->run[i];

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
	glUniform3fv(p->u_run, 1, run);
	glUniform1f(p->u_cell, MAX(6.0f, params->cell));
	glUniform1f(p->u_density, CLAMP(params->density, 0.0f, 1.0f));
	glUniform1f(p->u_bulge, CLAMP(params->bulge, 0.1f, 3.0f));
	glUniform1f(p->u_depth, CLAMP(params->depth, 0.0f, 20.0f));
	glUniform1f(p->u_dispersion, MAX(0.0f, params->dispersion));
	glUniform1f(p->u_runs, CLAMP(params->runs, 0.0f, 1.0f));
	glUniform1f(p->u_run_width, MAX(8.0f, params->run_width));
	glUniform1f(p->u_run_len, MAX(0.0f, params->run_len));
	glUniform1f(p->u_beads, CLAMP(params->beads, 0.0f, 1.0f));
	glUniform1f(p->u_fog, CLAMP(params->fog, 0.0f, 1.0f));
	glUniform1f(p->u_clarity, CLAMP(params->clarity, 0.0f, 1.0f));
	glUniform1f(p->u_specular, MAX(0.0f, params->specular));
	glUniform1f(p->u_shine, MAX(1.0f, params->shine));
	glUniform1f(p->u_rim, CLAMP(params->rim, 0.0f, 1.0f));
	glUniform1f(p->u_impact, CLAMP(params->impact, 0.0f, 1.0f));
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
