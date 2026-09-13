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
	"varying vec2 v_uv;\n"
	"\n"
	"const float N_WATER = 1.333;\n"
	"const float TAU = 6.2831853;\n"
	"/* How far a resting drop may slide before it fades, in cells.  Bounded\n"
	" * by the nine-cell lookup: 1.0 (jitter) + 0.28 (radius) + this must\n"
	" * not exceed 1.5.  See drop_layer(). */\n"
	"const float RAIN_SLIDE = 0.2;\n"
	"\n"
	"/* Signed distance to a rounded rect centred in @size; negative inside. */\n"
	"float sdf_rect(vec2 p, vec2 size, float r) {\n"
	"  vec2 h = size * 0.5;\n"
	"  vec2 q = abs(p - h) - (h - vec2(r));\n"
	"  return min(max(q.x, q.y), 0.0) + length(max(q, vec2(0.0))) - r;\n"
	"}\n"
	"\n"
	"/*\n"
	" * Three uncorrelated numbers in [0,1) for one cell.\n"
	" *\n"
	" * The three are pulled from DIFFERENT dot products rather than from\n"
	" * one seed nudged three times: sin(n), sin(n+1.7), sin(n+3.3) are\n"
	" * near-neighbours on the same curve, and a drop whose size and\n"
	" * position both came from that is a drop whose size predicts where it\n"
	" * sits.  Rows of big drops and rows of small ones, which is the\n"
	" * lattice arriving by the back door.\n"
	" */\n"
	"vec3 hash3(vec2 c) {\n"
	"  vec3 n = vec3(dot(c, vec2(127.1, 311.7)),\n"
	"                dot(c, vec2(269.5, 183.3)),\n"
	"                dot(c, vec2(419.2,  371.9)));\n"
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
	" * @ang turns the whole lattice, and the caller gives each layer a\n"
	" * different @cell as well, at a ratio that is not a simple fraction.\n"
	" * Turning alone is not enough -- a rotated grid is still a grid -- but\n"
	" * two turned grids of incommensurate pitch, each under half full,\n"
	" * have no shared period for the eye to find.\n"
	" *\n"
	" * THE LIFECYCLE IS THE POINT OF THE LAYER, not decoration.  a is the\n"
	" * drop's age in [0,1), its own phase from its own cell, so the field\n"
	" * is always part new and part old rather than breathing in unison:\n"
	" *\n"
	" *   a < 0.22          growing -- condensation gathering\n"
	" *   0.22 .. 0.78      resting at full size\n"
	" *   a > 0.78          it got heavy: slides downward, accelerating,\n"
	" *                     and fades as it goes\n"
	" *\n"
	" * HOW FAR IT MAY SLIDE IS FIXED BY THE LOOKUP, not by taste.  A pixel\n"
	" * only asks the nine cells around it, which reach 1.5 cells away; a\n"
	" * drop centred anywhere in its own cell needs 1 of that, its radius\n"
	" * needs another 0.28, and what is left -- 0.2 of a cell -- is all the\n"
	" * slide there is room for.  Take more and drops that have started to\n"
	" * go are sliced off along the cell edges into flat-sided crescents,\n"
	" * which is a lattice drawn in the negative and the exact artefact the\n"
	" * whole layer is arranged to avoid.\n"
	" */\n"
	"void drop_layer(vec2 p, float cell, float density, float ang,\n"
	"                inout vec4 best, inout float wet, inout float ring) {\n"
	"  if (density <= 0.0 || cell <= 0.0)\n"
	"    return;\n"
	"\n"
	"  float c = cos(ang);\n"
	"  float s = sin(ang);\n"
	"  mat2  R = mat2(c, -s, s, c);\n"
	"  mat2  Rt = mat2(c, s, -s, c);        /* the way back */\n"
	"  vec2  q = (R * p) / cell;\n"
	"  vec2  base = floor(q);\n"
	"  int   i, j;\n"
	"\n"
	"  for (j = -1; j <= 1; j++) {\n"
	"    for (i = -1; i <= 1; i++) {\n"
	"      vec2  id = base + vec2(float(i), float(j));\n"
	"      vec3  h  = hash3(id);\n"
	"      vec3  h2 = hash3(id + vec2(17.3, 41.7));\n"
	"\n"
	"      /* Most cells are empty, and which ones is fixed -- the field\n"
	"       * must not shimmer in and out as a drop's age crosses zero. */\n"
	"      if (h2.z > density)\n"
	"        continue;\n"
	"\n"
	"      float a  = fract(u_life + h.z);\n"
	"      float rr = cell * (0.09 + 0.19 * h2.x);   /* <= 0.28 cells */\n"
	"      float grow = smoothstep(0.0, 0.22, a);\n"
	"      float slip = max(0.0, a - 0.78) / 0.22;\n"
	"      vec2  cen  = (id + vec2(h.x, h.y)) * cell;\n"
	"\n"
	"      /* Down the SCREEN, not down the lattice: the layer is turned and\n"
	"       * gravity is not, so the slide is applied after coming back out\n"
	"       * of the rotated space. */\n"
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
	" * column is dry most of the time, that its drop sits anywhere across\n"
	" * its width, that it wanders as it falls, and that three of these run\n"
	" * over each other at widths and speeds that do not divide.\n"
	" *\n"
	" * THE SPEED MULTIPLIER IS AN INTEGER.  fract(k*t + phase) is what puts\n"
	" * the head somewhere, and it is continuous across the caller's wrap of\n"
	" * t at 1.0 only when k is whole.  1 and 2 are safe; 1.5 would teleport\n"
	" * every running drop in the layer once a cycle.\n"
	" *\n"
	" * THE TRAIL IS THE PATH, EVALUATED BACKWARDS.  The head's wander is a\n"
	" * function of its height, so the wetness left at height y is that same\n"
	" * function at y -- no history to store, and the trail bends exactly\n"
	" * the way the drop did.\n"
	" */\n"
	"void run_layer(vec2 p, float w, float t, float density, float seed,\n"
	"               inout vec4 best, inout float wet) {\n"
	"  if (density <= 0.0 || w <= 0.0)\n"
	"    return;\n"
	"\n"
	"  int n;\n"
	"\n"
	"  /*\n"
	"   * THREE COLUMNS, and the number is arithmetic rather than taste.\n"
	"   * A drop sits up to a quarter of a width from its column's centre\n"
	"   * and wanders another 0.45 as it falls, so with a radius of up to\n"
	"   * 0.32 it reaches just over one width to either side.  A pixel is\n"
	"   * at least (|k| - 0.5) widths from the centre of the column k away,\n"
	"   * so k of 2 can never reach it and k of 1 can -- ask two columns\n"
	"   * and every drop that wandered left is sliced off down a straight\n"
	"   * vertical line at the column edge it came from.\n"
	"   */\n"
	"  for (n = -1; n <= 1; n++) {\n"
	"    float col = floor(p.x / w + seed) + float(n);\n"
	"    vec3  h   = hash3(vec2(col, seed * 31.7));\n"
	"    vec3  h2  = hash3(vec2(col * 3.1 + 7.0, seed * 11.3));\n"
	"\n"
	"    if (h2.z > density)\n"
	"      continue;\n"
	"\n"
	"    float k    = 1.0 + step(0.62, h.z);        /* 1 or 2, never 1.5 */\n"
	"    float len  = u_run_len * (0.6 + 0.8 * h2.y);\n"
	"    float span = u_size.y + 2.0 * len + w * 2.0;\n"
	"    float head = fract(t * k + h.x) * span - len - w;\n"
	"    float rr   = w * (0.15 + 0.17 * h2.x);\n"
	"    float lane = (col - seed + 0.5) * w + (h.y - 0.5) * w * 0.5;\n"
	"\n"
	"    /* Where the drop is when it is at height y.  Two turns of a slow\n"
	"     * sine, so it leans one way and then the other down the pane\n"
	"     * rather than snaking evenly. */\n"
	"    float wob  = w * 0.28;\n"
	"    float xh   = lane + sin(head * 0.021 + h.x * TAU) * wob\n"
	"                      + sin(head * 0.0067 + h.y * TAU) * wob * 0.6;\n"
	"\n"
	"    /* The head. */\n"
	"    drop_at(p, vec2(xh, head), rr, 1.0, best, wet);\n"
	"\n"
	"    /*\n"
	"     * The wet path behind it, narrowing and drying with distance.\n"
	"     *\n"
	"     * This is the entire reason the trail is visible: it clears the\n"
	"     * frost, so the streak is a SHARP stripe through a soft pane\n"
	"     * rather than a pale line painted over it.  It has to be nearly\n"
	"     * as wide as the head and to dry slowly -- a narrow, fast-fading\n"
	"     * mask reads as a scratch on the glass.\n"
	"     */\n"
	"    float back = head - p.y;\n"
	"    if (back > 0.0 && back < len) {\n"
	"      float u  = back / len;\n"
	"      float xy = lane + sin(p.y * 0.021 + h.x * TAU) * wob\n"
	"                      + sin(p.y * 0.0067 + h.y * TAU) * wob * 0.6;\n"
	"      float tw = rr * (1.05 - 0.5 * u);\n"
	"      float m  = (1.0 - smoothstep(tw * 0.55, tw, abs(p.x - xy)))\n"
	"                 * (1.0 - u * u);\n"
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
	"       * frosted pane is almost nothing at all -- and a streak that\n"
	"       * does not refract is the clearest sign in the picture that it\n"
	"       * was painted on rather than run down.\n"
	"       *\n"
	"       * Claimed at three quarters, so a bead sitting IN the trail\n"
	"       * still wins its own pixels.\n"
	"       */\n"
	"      float tc = m * 0.75;\n"
	"      if (tc > best.w) {\n"
	"        /* Half the slope and two thirds the radius of a drop the same\n"
	"         * width: a run is a shallow film and not a half-pipe, and at\n"
	"         * a drop's curvature its edges throw the wallpaper so far\n"
	"         * sideways that all that is left of the streak is a bright\n"
	"         * hair down the middle. */\n"
	"        best.xy = vec2(clamp((p.x - xy) / max(tw, 1.0), -1.0, 1.0)\n"
	"                       * 0.5, 0.0);\n"
	"        best.z  = tw * 0.6;\n"
	"        best.w  = tc;\n"
	"      }\n"
	"\n"
	"    }\n"
	"\n"
	"    /*\n"
	"     * The beads the trail is actually made of.  A clean wet stripe is\n"
	"     * what a finger leaves; rain leaves a broken line of little drops\n"
	"     * that stop where they are and shrink for a long time afterwards.\n"
	"     * Their spacing is hashed per position down the column, so they do\n"
	"     * not step evenly either.\n"
	"     *\n"
	"     * OUTSIDE the trail test above, deliberately.  A bead exists\n"
	"     * because the head went past IT, which is a fact about the bead's\n"
	"     * own height; asking whether the PIXEL is still inside the band\n"
	"     * cuts every bead that straddles either end of it off along a\n"
	"     * straight horizontal line.  Two bead cells for the same reason\n"
	"     * the columns are three.\n"
	"     */\n"
	"    if (u_beads > 0.001) {\n"
	"      float gap = w * 0.85;\n"
	"      int   b;\n"
	"\n"
	"      for (b = -1; b <= 0; b++) {\n"
	"        float bi  = floor(p.y / gap) + float(b);\n"
	"        vec3  bh  = hash3(vec2(col * 5.0 + seed, bi));\n"
	"        float by  = (bi + bh.x) * gap;\n"
	"        float age = (head - by) / max(len, 1.0);\n"
	"\n"
	"        if (age <= 0.0 || age > 1.6 || bh.z >= u_beads)\n"
	"          continue;\n"
	"        float bx = lane + sin(by * 0.021 + h.x * TAU) * wob\n"
	"                        + sin(by * 0.0067 + h.y * TAU) * wob * 0.6\n"
	"                        + (bh.y - 0.5) * rr;\n"
	"        /* Shrinking, not vanishing: a bead left on a window stays for\n"
	"         * a long time and gets smaller, and stopping it dead at the\n"
	"         * end of the trail makes the whole streak blink. */\n"
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
	"  drop_layer(p, u_cell,        u_density,        0.00, best, wet, ring);\n"
	"  drop_layer(p, u_cell * 1.73, u_density * 0.45, 0.81, best, wet, ring);\n"
	"\n"
	"  /* Three running layers.  Widths and clocks both irrational against\n"
	"   * each other, so no two heads ever pace one another for long. */\n"
	"  run_layer(p, u_run_width,        u_run.x, u_runs,        0.13,\n"
	"            best, wet);\n"
	"  run_layer(p, u_run_width * 1.61, u_run.y, u_runs * 0.72, 0.57,\n"
	"            best, wet);\n"
	"  run_layer(p, u_run_width * 2.53, u_run.z, u_runs * 0.42, 0.91,\n"
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
	params->run_len    = 700.0f;
	params->beads      = 0.55f;
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
	 * on screen all day is noticed.  1, 1/phi and 1/phi^2 have no common
	 * period at all, and 1/phi + 1/phi^2 = 1 keeps them spread rather
	 * than bunched.
	 */
	static const gdouble rate[3] = {
		1.0, 0.6180339887498949, 0.3819660112501051
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
	 * Every clock here is a FRACTION of a cycle, wrapped into [0, 1).
	 *
	 * Everything the shader does with them is fract(clock + something),
	 * which is continuous across that wrap -- so unlike the water's
	 * phases these need no radians, and unlike a seconds-since-start
	 * float they never lose their mantissa.  The one rule that comes
	 * with it is that any multiplier applied inside the shader must be a
	 * whole number, which is why the per-column speed there is 1 or 2.
	 */
	clock->life += dt / life_seconds;
	clock->life = fmod(clock->life, 1.0);
	if (clock->life < 0.0)
		clock->life += 1.0;

	for (i = 0; i < 3; i++) {
		clock->run[i] += dt * speed * rate[i];
		clock->run[i] = fmod(clock->run[i], 1.0);
		if (clock->run[i] < 0.0)
			clock->run[i] += 1.0;
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
