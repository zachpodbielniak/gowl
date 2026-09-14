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
 * gowl-fx-snow.c -- snow on the window, and what becomes of it.
 *
 * The rain next door is the parent: a field of separate little things on
 * a pane, each with its own life, stateless, hashed.  Snow is the same
 * problem with one extra dimension, and that dimension is the whole
 * reason this file is the biggest of the family --
 *
 *   A SETTLED FLAKE IS THREE DIFFERENT OBJECTS IN SUCCESSION.
 *
 * It lands as a CRYSTAL, which does not refract at all: snow is mostly
 * air, and a flake on glass is a bright diffusing patch that SCATTERS.
 * Drawing it as a lens is the single most common way to get snow wrong,
 * and the result looks like broken glass.
 *
 * It melts into a BEAD, which is a raindrop and refracts exactly like
 * one.  And it is a great deal smaller: a flake is perhaps a twentieth
 * water by volume, so a centimetre of crystal becomes a couple of
 * millimetres of water.  That collapse is the most recognisable part of
 * the melt and the part a naive version leaves out -- it cross-fades
 * white to clear at constant size, which reads as a flake turning
 * invisible rather than as a flake turning into water.
 *
 * And then it RUNS, because a bead of melt-water on a cold pane gets
 * heavy like any other and goes down it leaving a beaded trail.
 *
 * So the settled layer walks one phase through all three, morphing the
 * SHAPE (six-fold profile towards a circle), the SIZE (down by `shrink')
 * and the OPTICS (scatter towards lens) on the same parameter.  Doing it
 * on one parameter rather than three is what keeps the transitions
 * physical: a half-melted flake is half-rounded, half-shrunk and
 * half-refracting at the same instant, which is what a half-melted flake
 * looks like.
 *
 * THE REST OF WHAT MAKES IT READ AS SNOW:
 *
 *   1. SIX-FOLD, AND NOT ALL THE SAME.  Ice grows hexagonally, so every
 *      flake has six identical arms -- but plates, sectored plates,
 *      stellar dendrites and needles are all snow, and a screen of one
 *      archetype is a texture.  The wedge fold gives the symmetry for a
 *      sixth of the work and the archetype is a hash.
 *   2. THEY DO NOT FALL LIKE RAIN.  A flake has almost no mass and a
 *      great deal of drag: it drifts, wanders, and turns slowly on the
 *      way down.  Rain's straight fast columns look wrong immediately.
 *   3. THEY GLITTER.  A crystal facet catches the light for an instant
 *      as it turns.  Without it settled snow is a set of white blobs.
 *   4. FROST CREEPS IN FROM THE EDGES.  A cold pane grows feathery ice
 *      from its corners inwards, and it does not repeat, so it is drawn
 *      from warped ridged noise rather than from a lattice of anything.
 *
 * WHY IT IS NOT A GRID.  Same discipline as the rain: falling flakes are
 * hashed columns at three widths that do not divide, settled ones are two
 * jittered lattices turned against each other, the runs are the rain's
 * own column machinery, and the frost is noise.  Nothing is periodic in
 * space.
 */

#undef G_LOG_DOMAIN
#define G_LOG_DOMAIN "gowl-fx"

#include "gowl-fx-private.h"

#include <math.h>
#include <string.h>

static const gchar snow_vert_src[] =
	"attribute vec2 a_pos;\n"
	"attribute vec2 a_uv;\n"
	"varying vec2 v_uv;\n"
	"void main() {\n"
	"  v_uv = a_uv;\n"
	"  gl_Position = vec4(a_pos, 0.0, 1.0);\n"
	"}\n";

/*
 * highp conditionally, for the reason the whole family gives: naming a
 * precision that does not exist fails to COMPILE rather than falling
 * back.  It is wanted here for the folded wedge coordinates -- the
 * dendrite branches are thin, and at mediump the fold seam shows as six
 * hairline cracks across every flake.
 */
static const gchar snow_frag_src[] =
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
	"uniform float u_settle_t;      /* settled-flake clock, cycles */\n"
	"uniform vec3  u_fall;          /* the three falling clocks, cycles */\n"
	"uniform vec2  u_run;           /* the two melt-water clocks, cycles */\n"
	"uniform float u_frost_t;       /* how far the frost has grown */\n"
	"uniform float u_flake;         /* a falling flake's radius, px */\n"
	"uniform float u_cell;          /* px per cell of the settled layer */\n"
	"uniform float u_settled;       /* how many cells hold one, 0..1 */\n"
	"uniform float u_column;        /* px per column of the falling layer */\n"
	"uniform float u_falling;       /* how many columns carry one, 0..1 */\n"
	"uniform float u_arms;          /* how dendritic, 0..1 */\n"
	"uniform float u_drift;         /* steady sideways wind, px per fall */\n"
	"uniform float u_flutter;       /* wander, as a fraction of a column */\n"
	"uniform float u_spin;          /* turns per fall */\n"
	"uniform float u_melt;          /* where in a life the melt starts */\n"
	"uniform float u_shrink;        /* bead size, as a share of the flake */\n"
	"uniform float u_bulge;         /* how domed the bead is */\n"
	"uniform float u_depth;         /* ray travel, in bead radii */\n"
	"uniform float u_dispersion;    /* channel separation in a bead */\n"
	"uniform float u_runs;          /* how many columns run, 0..1 */\n"
	"uniform float u_run_width;     /* px per column of the run layer */\n"
	"uniform float u_run_len;       /* px of trail behind a head */\n"
	"uniform float u_beads;         /* residual beads in a trail */\n"
	"uniform float u_frost;         /* frost from the edges, 0..1 */\n"
	"uniform float u_frost_scale;   /* px per feather of that frost */\n"
	"uniform float u_sparkle;       /* how much a crystal glitters */\n"
	"uniform float u_fog;           /* how frosted the bare pane is */\n"
	"uniform float u_clarity;       /* how much a bead lifts it */\n"
	"uniform float u_glow;          /* how bright a dry crystal is */\n"
	"uniform float u_specular;\n"
	"uniform float u_shine;         /* specular exponent */\n"
	"uniform float u_rim;           /* the dark contact ring on a bead */\n"
	"uniform vec3  u_light;\n"
	"uniform vec3  u_tint;\n"
	"uniform float u_absorb;\n"
	"uniform float u_brightness;\n"
	"uniform float u_alpha;\n"
	"uniform float u_seed;          /* which crop of the snow, 0..16 */\n"
	"varying vec2 v_uv;\n"
	"\n"
	"const float TAU = 6.2831853;\n"
	"const float PI  = 3.14159265;\n"
	"const float PI3 = 1.04719755;   /* pi/3: one sixth of a turn */\n"
	"/* How many falls, settled lives and runs before the field repeats.\n"
	" * The rain's trade and the rain's number: larger repeats later and\n"
	" * quantises a position more coarsely, because a float has to hold\n"
	" * the clock. */\n"
	"const float SNOW_CYCLES = 256.0;\n"
	"/* How far a melting flake may creep before the run layer takes over,\n"
	" * in cells.  Bounded by the nine-cell lookup exactly as the rain's\n"
	" * slide is: the flake reaches 0.40 of a cell and the guarantee is\n"
	" * 1.0, so this is well inside it. */\n"
	"const float SNOW_CREEP = 0.30;\n"
	"\n"
	"/* Signed distance to a rounded rect centred in @size; negative inside. */\n"
	"float sdf_rect(vec2 p, vec2 size, float r) {\n"
	"  vec2 h = size * 0.5;\n"
	"  vec2 q = abs(p - h) - (h - vec2(r));\n"
	"  return min(max(q.x, q.y), 0.0) + length(max(q, vec2(0.0))) - r;\n"
	"}\n"
	"\n"
	"/* Three uncorrelated numbers from three inputs; gowl-fx-rain.c says\n"
	" * at length why it must be three and not two. */\n"
	"vec3 hash3v(vec3 c) {\n"
	"  vec3 n = vec3(dot(c, vec3(127.1,  89.3,  54.7)),\n"
	"                dot(c, vec3( 71.9, 151.3, 101.7)),\n"
	"                dot(c, vec3(167.3,  61.7, 133.9)));\n"
	"  return fract(sin(n) * vec3(43758.5453, 28001.8384, 19349.6631));\n"
	"}\n"
	"\n"
	"/* Distance from @p to the segment ab. */\n"
	"float sdf_seg(vec2 p, vec2 a, vec2 b) {\n"
	"  vec2  pa = p - a;\n"
	"  vec2  ba = b - a;\n"
	"  float h  = clamp(dot(pa, ba) / max(dot(ba, ba), 1e-5), 0.0, 1.0);\n"
	"  return length(pa - ba * h);\n"
	"}\n"
	"\n"
	"/*\n"
	" * ONE SNOW CRYSTAL, as a signed distance in its own frame.\n"
	" *\n"
	" * SIX-FOLD FOR THE PRICE OF ONE SIXTH.  Ice grows on a hexagonal\n"
	" * lattice, so a flake has exactly six identical arms 60 degrees\n"
	" * apart, and each arm is itself mirror-symmetric.  Fold the point\n"
	" * into one 30-degree wedge and everything drawn there appears twelve\n"
	" * times, correctly, with no loop over arms at all.  The fold is two\n"
	" * lines and it is the only reason this is affordable.\n"
	" *\n"
	" * FOUR ARCHETYPES, blended by @kind, because a screen of one shape is\n"
	" * a texture and not weather:\n"
	" *\n"
	" *   plate             a hexagonal disc, no arms to speak of\n"
	" *   sectored plate    a disc with ridges out to the points\n"
	" *   stellar dendrite  a small core and six long branched arms\n"
	" *   needle            a long thin cross of arms, almost no core\n"
	" *\n"
	" * @melt rounds the whole thing off towards a circle: the arms are the\n"
	" * first thing to go when ice turns to water, because they are thin\n"
	" * and they have the most surface for their volume.  @melt of 1 is a\n"
	" * plain disc, which is exactly the bead it is about to become.\n"
	" */\n"
	"float flake_sdf(vec2 q, float kind, float melt) {\n"
	"  float l = length(q);\n"
	"  if (l > 1.15)\n"
	"    return l - 1.0;\n"
	"\n"
	"  /* Fold into a 30-degree wedge: one rotation to the nearest sixth,\n"
	"     then a mirror about the wedge axis. */\n"
	"  float a = atan(q.y, q.x);\n"
	"  float s = a - PI3 * floor(a / PI3 + 0.5);\n"
	"  vec2  w = vec2(cos(s), abs(sin(s))) * l;\n"
	"\n"
	"  /* The core: a disc of @core, which is most of a plate and almost\n"
	"     none of a dendrite. */\n"
	"  float core   = mix(0.62, 0.10, kind);\n"
	"  float armw   = mix(0.085, 0.030, kind) * (0.5 + 0.5 * u_arms);\n"
	"  float armlen = mix(0.72, 1.00, kind);\n"
	"  float d      = l - core;\n"
	"\n"
	"  /* The main arm, out along the wedge axis. */\n"
	"  d = min(d, sdf_seg(w, vec2(0.0, 0.0), vec2(armlen, 0.0)) - armw);\n"
	"\n"
	"  /*\n"
	"   * The side branches.  Three of them at 60 degrees off the arm, at\n"
	"   * fractions of the way out -- which is where a real dendrite puts\n"
	"   * them, because each is a secondary instability of the primary and\n"
	"   * they space themselves.  Only above @u_arms: a plate has none, and\n"
	"   * that is what makes it a plate.\n"
	"   */\n"
	"  if (u_arms > 0.02) {\n"
	"    float bl = armlen * 0.30 * u_arms * (0.4 + 0.9 * kind);\n"
	"    float t;\n"
	"    int   i;\n"
	"\n"
	"    for (i = 1; i <= 3; i++) {\n"
	"      t = armlen * (0.22 + 0.24 * float(i));\n"
	"      d = min(d, sdf_seg(w, vec2(t, 0.0),\n"
	"                         vec2(t + bl * 0.5, bl * 0.866))\n"
	"                 - armw * 0.62);\n"
	"    }\n"
	"  }\n"
	"\n"
	"  /* Melting rounds it off.  Towards a disc of the core radius, not of\n"
	"     the arm length: the bead that is left is small. */\n"
	"  return mix(d, l - 0.72, clamp(melt, 0.0, 1.0));\n"
	"}\n"
	"\n"
	"/*\n"
	" * VALUE NOISE, and the ridged, warped stack the frost is made of.\n"
	" *\n"
	" * Frost is not a lattice of anything and must not be drawn from one.\n"
	" * What it IS, structurally, is a branching front -- so ridged noise\n"
	" * (the fold |n - 0.5| turns smooth blobs into creases) warped by more\n"
	" * noise gives feathered, branching, non-repeating ice for three\n"
	" * lookups.  The alternative, a hexagonal dendrite grid, reads as\n"
	" * wallpaper the moment two cells are on screen at once.\n"
	" */\n"
	"float vnoise(vec2 p) {\n"
	"  vec2 i = floor(p);\n"
	"  vec2 f = fract(p);\n"
	"  vec2 u = f * f * (3.0 - 2.0 * f);\n"
	"  float a = hash3v(vec3(i, 0.0)).x;\n"
	"  float b = hash3v(vec3(i + vec2(1.0, 0.0), 0.0)).x;\n"
	"  float c = hash3v(vec3(i + vec2(0.0, 1.0), 0.0)).x;\n"
	"  float d = hash3v(vec3(i + vec2(1.0, 1.0), 0.0)).x;\n"
	"  return mix(mix(a, b, u.x), mix(c, d, u.x), u.y);\n"
	"}\n"
	"\n"
	"float frost_at(vec2 p, float edge) {\n"
	"  if (u_frost <= 0.002 || edge <= 0.0)\n"
	"    return 0.0;\n"
	"\n"
	"  vec2  q = p / max(u_frost_scale, 4.0) + vec2(u_seed * 3.1, u_seed * 7.7);\n"
	"  /* Warp first: an unwarped ridge field is a set of parallel creases. */\n"
	"  vec2  w = q + vec2(vnoise(q * 0.5), vnoise(q * 0.5 + 19.0)) * 1.8;\n"
	"  /*\n"
	"   * Ridged, and CUBED.  1 - |n - 0.5|*2 is 1 exactly on a crest and\n"
	"   * falls away either side; raising it to a power narrows the crest\n"
	"   * into a line.  Without that the field is broad and smooth and the\n"
	"   * threshold below lets whole REGIONS through, which is what frost\n"
	"   * is not: it is a set of thin branching feathers, and a solid patch\n"
	"   * of it reads as a smudge on the lens.\n"
	"   */\n"
	"  float r1 = 1.0 - abs(vnoise(w) - 0.5) * 2.0;\n"
	"  float r2 = 1.0 - abs(vnoise(w * 2.3 + 7.0) - 0.5) * 2.0;\n"
	"  float n  = max(r1 * r1 * r1, r2 * r2 * r2 * 0.85);\n"
	"  /*\n"
	"   * The GROWTH FRONT.  The threshold is how far the ice has got, so\n"
	"   * lowering it with @u_frost_t is the front advancing rather than the\n"
	"   * whole pattern fading up -- feathers appear at their tips and reach\n"
	"   * inwards, which is what frost does and what an alpha ramp does\n"
	"   * not.  It starts above the field's own maximum, so a pane with the\n"
	"   * clock at zero is clean glass.\n"
	"   */\n"
	"  float thr = 1.00 - clamp(u_frost_t, 0.0, 1.0) * 0.44 * edge;\n"
	"  return clamp(smoothstep(thr, thr + 0.09, n), 0.0, 1.0) * u_frost;\n"
	"}\n"
	"\n"
	"/*\n"
	" * ONE WATER BEAD, folded into the running best.\n"
	" *\n"
	" * The rain's drop_at() exactly: @best carries (q.xy, radius,\n"
	" * coverage) with q the offset from the centre in units of the radius,\n"
	" * and beads MERGE rather than blending.  Shared by the melt-water,\n"
	" * the runs and the residual beads, all three of which are ordinary\n"
	" * rain once the snow is done with them.\n"
	" */\n"
	"void bead_at(vec2 p, vec2 centre, float r, float alive,\n"
	"             inout vec4 best, inout float wet) {\n"
	"  if (alive <= 0.0 || r <= 0.0)\n"
	"    return;\n"
	"\n"
	"  vec2  d  = p - centre;\n"
	"  float l  = length(d);\n"
	"  float aa = max(1.0, r * 0.12);\n"
	"  float cov = (1.0 - smoothstep(r - aa, r, l)) * alive;\n"
	"\n"
	"  if (cov <= 0.0)\n"
	"    return;\n"
	"\n"
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
	" * ONE CRYSTAL, folded into a SEPARATE accumulator.\n"
	" *\n"
	" * Separate from the beads on purpose, and this is the structural\n"
	" * decision the whole file turns on: a crystal and a bead are not the\n"
	" * same kind of thing.  A bead bends light and contributes a normal\n"
	" * and a displacement; a crystal scatters light and contributes a\n"
	" * brightness and a haze.  Folding both into one winner-takes-all\n"
	" * would force a half-melted flake to be one or the other, and a\n"
	" * half-melted flake is visibly both.\n"
	" *\n"
	" * @ice carries (coverage, glitter phase, how molten, unused).\n"
	" */\n"
	"void crystal_at(vec2 p, vec2 centre, float r, float rot, float kind,\n"
	"                float melt, float alive, vec3 h, inout vec3 ice) {\n"
	"  if (alive <= 0.0 || r <= 0.0)\n"
	"    return;\n"
	"\n"
	"  vec2 d = p - centre;\n"
	"  if (dot(d, d) > r * r * 1.35)\n"
	"    return;\n"
	"\n"
	"  float c  = cos(rot);\n"
	"  float s  = sin(rot);\n"
	"  vec2  q  = vec2(c * d.x + s * d.y, -s * d.x + c * d.y) / r;\n"
	"  float aa = max(0.03, 1.6 / max(r, 1.0));\n"
	"  float sd = flake_sdf(q, kind, melt);\n"
	"  float cv = (1.0 - smoothstep(-aa, aa, sd)) * alive;\n"
	"\n"
	"  if (cv <= ice.x)\n"
	"    return;\n"
	"\n"
	"  ice.x = cv;\n"
	"  /* Which way this crystal's facets are turned, for the glitter. */\n"
	"  ice.y = h.x * TAU + rot;\n"
	"  ice.z = melt;\n"
	"}\n"
	"\n"
	"/*\n"
	" * ONE LAYER OF FALLING FLAKES.\n"
	" *\n"
	" * Columns, for the reason the rain's runs are columns: gravity has no\n"
	" * x component.  What stops a column reading as one is that it is\n"
	" * empty most of the time, that the flake sits anywhere across it,\n"
	" * that it wanders, and that three layers of different widths and\n"
	" * speeds run over each other.\n"
	" *\n"
	" * THREE COLUMNS, and the number is arithmetic.  The nearest column a\n"
	" * pixel does NOT ask is two away, whose lane is at least 2 + 0.5 -\n"
	" * 0.22 - 1 = 1.28 widths off it.  A flake sits up to 0.22 of a width\n"
	" * off its own lane, wanders another 0.40 and reaches 0.36, which is\n"
	" * 0.98 -- inside that.\n"
	" *\n"
	" * WHICH FLAKE IS PER FALL, not per column: a column that dropped one\n"
	" * a moment ago may be empty for several cycles, and the next is a\n"
	" * different archetype at a different size turning at a different\n"
	" * rate.  gowl-fx-rain.c records what pinning that to the column\n"
	" * actually looked like when it shipped.\n"
	" */\n"
	"void snow_fall_layer(vec2 p, float w, float t, float density, float lyr,\n"
	"                     float size, inout vec3 ice) {\n"
	"  if (density <= 0.0 || w <= 0.0)\n"
	"    return;\n"
	"\n"
	"  float lid = lyr * 64.0 + u_seed;\n"
	"  float gph = lyr * 0.27 + u_seed * 0.059;\n"
	"  int   n;\n"
	"\n"
	"  for (n = -1; n <= 1; n++) {\n"
	"    float col = floor(p.x / w + gph) + float(n);\n"
	"    /* Per COLUMN and permanent: the rate and the phase only. */\n"
	"    vec3  hc = hash3v(vec3(col, lid, 0.0));\n"
	"    float k  = 1.0 + floor(hc.z * 2.999);        /* 1, 2 or 3 */\n"
	"    float u  = t * k + hc.x;\n"
	"    float fi = mod(floor(u), SNOW_CYCLES);\n"
	"    vec3  hr = hash3v(vec3(col, lid + 3.0, fi));\n"
	"\n"
	"    if (hr.z > density)\n"
	"      continue;\n"
	"\n"
	"    vec3  hs   = hash3v(vec3(col, lid + 23.0, fi));\n"
	"    float r    = size * (0.62 + 0.70 * hs.x);\n"
	"    float marg = r * 1.6;\n"
	"    float span = u_size.y + 2.0 * marg;\n"
	"    float a    = fract(u);\n"
	"    float y    = -marg + a * span;\n"
	"    /*\n"
	"     * THE WANDER.  Two slow sines at rates that do not divide, so a\n"
	"     * flake drifts one way for a while and then the other rather than\n"
	"     * weaving evenly.  A flake has almost no mass and a great deal of\n"
	"     * drag: it does not fall, it is carried, and this is the whole\n"
	"     * difference from the rain's near-vertical runs.\n"
	"     */\n"
	"    float lane = (col - gph + 0.5) * w + (hr.x - 0.5) * w * 0.44;\n"
	"    float x    = lane\n"
	"                 + sin(a * TAU * (0.7 + 0.9 * hs.y) + hs.z * TAU)\n"
	"                   * w * u_flutter\n"
	"                 + sin(a * TAU * (1.9 + 1.3 * hs.z) + hs.y * TAU)\n"
	"                   * w * u_flutter * 0.35\n"
	"                 + u_drift * a;\n"
	"\n"
	"    crystal_at(p, vec2(x, y), r,\n"
	"               a * TAU * u_spin * (0.6 + 0.8 * hr.y) + hs.x * TAU,\n"
	"               hs.y, 0.0,\n"
	"               /* Fade in and out rather than clipping: a flake that\n"
	"                  appears at a fixed height draws a line across the\n"
	"                  window once a cycle. */\n"
	"               clamp(min(a, 1.0 - a) * 22.0, 0.0, 1.0),\n"
	"               hr, ice);\n"
	"  }\n"
	"}\n"
	"\n"
	"/*\n"
	" * FLAKES THAT HAVE LANDED, through their whole life.\n"
	" *\n"
	" * A jittered grid, mostly empty, and one phase drives all of it:\n"
	" *\n"
	" *   a < 0.05        LANDING.  Scales up from nothing as it touches.\n"
	" *   .. u_melt       CRYSTAL.  Intact, bright, scattering, glittering.\n"
	" *   .. 0.82         MELTING.  The arms round off, the whole thing\n"
	" *                   collapses to `shrink\' of its size, and the optics\n"
	" *                   cross from scatter to lens.\n"
	" *   a > 0.82        RUNNING.  It got heavy: creeps downward,\n"
	" *                   accelerating, and fades as the run layer takes\n"
	" *                   the water on down the pane.\n"
	" *\n"
	" * ONE PARAMETER FOR SHAPE, SIZE AND OPTICS, which is what keeps it\n"
	" * honest: a flake half way through is half-rounded, half-collapsed\n"
	" * and half-refracting at the same instant.  Cross-fading white to\n"
	" * clear at a constant size -- the obvious implementation -- reads as\n"
	" * a flake going invisible rather than as one turning into water.\n"
	" *\n"
	" * As with the rain, only the PHASE belongs to the cell.  Size,\n"
	" * position within it, archetype and whether the cell holds anything\n"
	" * at all are hashed against the life, so the same spots do not blink\n"
	" * on and off in place for the whole session.\n"
	" */\n"
	"void settled_layer(vec2 p, float cell, float density, float ang,\n"
	"                   float lyr, float size,\n"
	"                   inout vec4 best, inout float wet, inout vec3 ice) {\n"
	"  if (density <= 0.0 || cell <= 0.0)\n"
	"    return;\n"
	"\n"
	"  float c  = cos(ang);\n"
	"  float s  = sin(ang);\n"
	"  mat2  R  = mat2(c, -s, s, c);\n"
	"  mat2  Rt = mat2(c, s, -s, c);\n"
	"  vec2  off  = vec2(u_seed * 0.39, u_seed * 0.71);\n"
	"  float lid  = lyr * 64.0 + u_seed;\n"
	"  vec2  q    = (R * p) / cell + off;\n"
	"  vec2  base = floor(q);\n"
	"  int   i, j;\n"
	"\n"
	"  for (j = -1; j <= 1; j++) {\n"
	"    for (i = -1; i <= 1; i++) {\n"
	"      vec2  id = base + vec2(float(i), float(j));\n"
	"      vec3  hc = hash3v(vec3(id.x, id.y, lid));\n"
	"      float u  = u_settle_t + hc.x;\n"
	"      float a  = fract(u);\n"
	"      float li = mod(floor(u), SNOW_CYCLES);\n"
	"      vec3  hr = hash3v(vec3(id.x + 43.0 + lid, id.y - 61.0, li));\n"
	"\n"
	"      if (hr.z > density)\n"
	"        continue;\n"
	"\n"
	"      /* Its own melting point: a flake on a cold corner of the pane\n"
	"         lasts a good deal longer than one over a warm spot, and a\n"
	"         window where they all go at once is a dissolve. */\n"
	"      float m0   = clamp(u_melt * (0.65 + 0.70 * hr.y), 0.02, 0.80);\n"
	"      float land = smoothstep(0.0, 0.05, a);\n"
	"      float melt = clamp((a - m0) / max(0.82 - m0, 0.05), 0.0, 1.0);\n"
	"      float slip = max(0.0, a - 0.82) / 0.18;\n"
	"      /* The collapse: a flake is mostly air, so the water it leaves\n"
	"         is a fraction of its size.  Applied on the same parameter as\n"
	"         the rounding, so the two cannot disagree. */\n"
	"      float r    = size * (0.68 + 0.64 * hr.x)\n"
	"                   * mix(1.0, u_shrink, melt) * land;\n"
	"      vec2  cen  = (id + vec2(hr.y, hc.y) - off) * cell;\n"
	"\n"
	"      /* Down the SCREEN, not down the lattice: the layer is turned\n"
	"         and gravity is not. */\n"
	"      cen = Rt * cen;\n"
	"      cen.y += slip * slip * cell * SNOW_CREEP;\n"
	"\n"
	"      {\n"
	"        float alive = land * (1.0 - slip);\n"
	"\n"
	"        /* The crystal half, fading out as it melts. */\n"
	"        crystal_at(p, cen, r, hc.z * TAU + hr.y * 0.4, hr.x, melt,\n"
	"                   alive * (1.0 - melt * 0.92), hr, ice);\n"
	"        /* And the water half, fading in.  Both at once in the middle,\n"
	"           which is the point. */\n"
	"        if (melt > 0.02)\n"
	"          bead_at(p, cen, r * 0.92, alive * melt, best, wet);\n"
	"      }\n"
	"    }\n"
	"  }\n"
	"}\n"
	"\n"
	"/*\n"
	" * MELT-WATER RUNNING DOWN THE PANE.\n"
	" *\n"
	" * This is the rain's run_layer(), and deliberately so: once a flake\n"
	" * has melted, what is on the glass IS a raindrop and there is nothing\n"
	" * snow-specific left to model.  A head running down a wandering\n"
	" * column, a narrow wet path behind it that clears the frost, and a\n"
	" * line of residual beads that linger and shrink -- the beads being\n"
	" * the single most recognisable thing about water on a window, and the\n"
	" * first thing a naive version leaves out.\n"
	" *\n"
	" * The trail refracts, because it is water: a half-cylinder is exactly\n"
	" * a bead whose offset has no y component, so the lens, glint and rim\n"
	" * below apply to it for nothing.  A streak that does not refract is\n"
	" * the clearest sign in the picture that it was painted on.\n"
	" *\n"
	" * THE SPEED MULTIPLIER IS AN INTEGER, and it has to be: fract(k*t+ph)\n"
	" * is where the head is and floor(k*t+ph) is which run this is, and\n"
	" * both are continuous across the clock's wrap only when k is whole.\n"
	" */\n"
	"void melt_run_layer(vec2 p, float w, float t, float density, float lyr,\n"
	"                    inout vec4 best, inout float wet) {\n"
	"  if (density <= 0.0 || w <= 0.0)\n"
	"    return;\n"
	"\n"
	"  float lid = lyr * 64.0 + u_seed;\n"
	"  float gph = lyr * 0.43 + u_seed * 0.067;\n"
	"  int   n, b;\n"
	"\n"
	"  for (n = -1; n <= 1; n++) {\n"
	"    float col = floor(p.x / w + gph) + float(n);\n"
	"    vec3  hc  = hash3v(vec3(col, lid, 0.0));\n"
	"    float k   = 1.0 + floor(hc.z * 2.999);\n"
	"    float u   = t * k + hc.x;\n"
	"    float ri  = mod(floor(u), SNOW_CYCLES);\n"
	"    vec3  hr  = hash3v(vec3(col, lid + 8.0, ri));\n"
	"\n"
	"    if (hr.z > density)\n"
	"      continue;\n"
	"\n"
	"    vec3  hs   = hash3v(vec3(col, lid + 24.0, ri));\n"
	"    float len  = u_run_len * (0.6 + 0.8 * hs.x);\n"
	"    float span = u_size.y + 2.0 * len + w * 2.0;\n"
	"    float head = fract(u) * span - len - w;\n"
	"    /* A good deal finer than the rain's heads, and it has to be: this\n"
	"       water is what one flake left behind, so a bead the size of a\n"
	"       raindrop is bigger than the crystal it came out of. */\n"
	"    float rr   = w * (0.075 + 0.085 * hr.x);\n"
	"    float lane = (col - gph + 0.5) * w + (hr.y - 0.5) * w * 0.5;\n"
	"    float wob  = w * 0.26;\n"
	"    /* Where the head is at height y.  Two turns of a slow sine, both\n"
	"       phases per RUN, so the next drop down this column does not\n"
	"       retrace the last one's line. */\n"
	"    float xh   = lane + sin(head * 0.021 + hs.y * TAU) * wob\n"
	"                      + sin(head * 0.0067 + hs.z * TAU) * wob * 0.6;\n"
	"\n"
	"    bead_at(p, vec2(xh, head), rr, 1.0, best, wet);\n"
	"\n"
	"    {\n"
	"      float back = head - p.y;\n"
	"\n"
	"      if (back > 0.0 && back < len) {\n"
	"        float u2 = back / len;\n"
	"        /* THE TRAIL IS THE PATH, EVALUATED BACKWARDS: the head's\n"
	"           wander is a function of its height, so the wetness left at\n"
	"           height y is that same function at y.  No history to keep,\n"
	"           and the trail bends exactly the way the drop did. */\n"
	"        float xy = lane + sin(p.y * 0.021 + hs.y * TAU) * wob\n"
	"                        + sin(p.y * 0.0067 + hs.z * TAU) * wob * 0.6;\n"
	"        float tw = rr * (0.80 - 0.42 * u2);\n"
	"        float m  = (1.0 - smoothstep(tw * 0.5, tw, abs(p.x - xy)))\n"
	"                   * (1.0 - u2) * (1.0 - u2 * 0.6);\n"
	"        float tc = m * 0.6;\n"
	"\n"
	"        wet = max(wet, m);\n"
	"        if (tc > best.w) {\n"
	"          /* Half the slope and two thirds the radius of a bead the\n"
	"             same width: a run is a shallow film, not a half-pipe, and\n"
	"             at a bead's curvature all that is left of the streak is a\n"
	"             bright hair down the middle. */\n"
	"          best.xy = vec2(clamp((p.x - xy) / max(tw, 1.0), -1.0, 1.0) * 0.5,\n"
	"                         0.0);\n"
	"          best.z  = tw * 0.6;\n"
	"          best.w  = tc;\n"
	"        }\n"
	"      }\n"
	"    }\n"
	"\n"
	"    /*\n"
	"     * The beads.  OUTSIDE the trail test above, deliberately: a bead\n"
	"     * exists because the head went past IT, which is a fact about the\n"
	"     * bead's own height, and asking whether the PIXEL is still inside\n"
	"     * the band cuts every bead that straddles either end of it off\n"
	"     * along a straight horizontal line.\n"
	"     */\n"
	"    if (u_beads > 0.001) {\n"
	"      float gap = w * 0.42;\n"
	"\n"
	"      for (b = -1; b <= 0; b++) {\n"
	"        float bi  = floor(p.y / gap) + float(b);\n"
	"        vec3  bh  = hash3v(vec3(col + lid, bi + 211.0, ri));\n"
	"        float by  = (bi + bh.x) * gap;\n"
	"        float age = (head - by) / max(len, 1.0);\n"
	"\n"
	"        if (age <= 0.0 || age > 1.6 || bh.z >= u_beads)\n"
	"          continue;\n"
	"        {\n"
	"          float bx = lane + sin(by * 0.021 + hs.y * TAU) * wob\n"
	"                          + sin(by * 0.0067 + hs.z * TAU) * wob * 0.6\n"
	"                          + (bh.y - 0.5) * rr;\n"
	"          /* Shrinking, not vanishing: a bead stays a long time and\n"
	"             gets smaller, and stopping it dead at the end of the\n"
	"             trail makes the whole streak blink. */\n"
	"          bead_at(p, vec2(bx, by), rr * (0.30 + 0.40 * bh.y)\n"
	"                  * clamp(1.25 - age * 0.75, 0.0, 1.0),\n"
	"                  clamp(1.5 - age * 0.8, 0.0, 1.0), best, wet);\n"
	"        }\n"
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
	"  vec4  best = vec4(0.0);   /* the water: q, radius, coverage */\n"
	"  vec3  ice  = vec3(0.0);   /* the crystal: coverage, facet, melt */\n"
	"  float wet  = 0.0;\n"
	"\n"
	"  /* Three depths of falling flakes: the far ones smaller and on a\n"
	"     different clock.  Widths that do not divide, so no two layers\n"
	"     ever pace each other. */\n"
	"  snow_fall_layer(p, u_column * 1.87, u_fall.z, u_falling * 0.9, 0.0,\n"
	"                  u_flake * 0.52, ice);\n"
	"  snow_fall_layer(p, u_column * 1.33, u_fall.y, u_falling,       1.0,\n"
	"                  u_flake * 0.76, ice);\n"
	"  snow_fall_layer(p, u_column,        u_fall.x, u_falling * 0.8, 2.0,\n"
	"                  u_flake,        ice);\n"
	"\n"
	"  /* And what is already on the glass: two turned lattices at an\n"
	"     incommensurate pitch, for the reason the rain gives -- a rotated\n"
	"     grid is still a grid, two that share no period are not. */\n"
	"  settled_layer(p, u_cell,        u_settled,       0.00, 3.0,\n"
	"                u_flake * 1.05, best, wet, ice);\n"
	"  settled_layer(p, u_cell * 1.69, u_settled * 0.5, 0.77, 4.0,\n"
	"                u_flake * 0.72, best, wet, ice);\n"
	"\n"
	"  /* The melt-water on its way down. */\n"
	"  melt_run_layer(p, u_run_width,        u_run.x, u_runs,        5.0,\n"
	"                 best, wet);\n"
	"  melt_run_layer(p, u_run_width * 1.61, u_run.y, u_runs * 0.65, 6.0,\n"
	"                 best, wet);\n"
	"\n"
	"  /*\n"
	"   * THE BEAD IS A LENS, exactly as the rain's drop is -- because it\n"
	"   * IS the rain's drop.  Thin lens through a sphere of index 1.333:\n"
	"   * the focus is at 2r, so a wallpaper @u_depth radii behind maps\n"
	"   * with a magnification of 1 - depth/2, and the sample is the pixel\n"
	"   * reflected through the centre and scaled by that.  Negative past a\n"
	"   * depth of 2, which is the inversion, falling out of the arithmetic\n"
	"   * rather than being painted in.\n"
	"   */\n"
	"  vec2  disp = vec2(0.0);\n"
	"  vec3  n    = vec3(0.0, 0.0, 1.0);\n"
	"  float l    = 0.0;\n"
	"\n"
	"  if (best.w > 0.0) {\n"
	"    l = min(length(best.xy), 1.0);\n"
	"    n = normalize(vec3(best.xy * u_bulge, sqrt(max(1.0 - l * l, 1e-4))));\n"
	"    float rr = 0.75 + 0.25 / max(sqrt(max(1.0 - l * l, 1e-4)), 0.3);\n"
	"    disp = -best.xy * best.z * (u_depth * 0.5) * rr * best.w;\n"
	"  }\n"
	"\n"
	"  /*\n"
	"   * How frosted this pixel is.  Three things raise or lower it: the\n"
	"   * pane's own haze, the ice growing in from the edges, and every\n"
	"   * bead and wet path, which CLEAR it.  That last is why the effect\n"
	"   * does not look like the blur with white spots painted on: the\n"
	"   * water is where the blur is not.\n"
	"   */\n"
	"  float edge  = 1.0 - smoothstep(0.0, 0.42,\n"
	"                                 min(min(p.x, u_size.x - p.x),\n"
	"                                     min(p.y, u_size.y - p.y))\n"
	"                                 / max(min(u_size.x, u_size.y), 1.0));\n"
	"  float ice_f = frost_at(p, edge);\n"
	"  float clear = clamp(max(best.w, wet) * u_clarity, 0.0, 1.0);\n"
	"  float fog   = clamp(u_fog * (1.0 - clear) + ice_f * 0.65, 0.0, 1.0);\n"
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
	"  /* What the melt-water takes out of the light.  Barely anything by\n"
	"     default: water on a window is not a swimming pool, and a blue\n"
	"     cast over the whole window is the first thing that makes it look\n"
	"     fake. */\n"
	"  if (u_absorb > 0.001)\n"
	"    col = mix(col, col * u_tint, clamp(u_absorb * best.w, 0.0, 1.0));\n"
	"\n"
	"  /* Frost is ice, so it is pale as well as blurring. */\n"
	"  if (ice_f > 0.001)\n"
	"    col = mix(col, col * 0.86 + vec3(0.26), ice_f * 0.50);\n"
	"\n"
	"  /*\n"
	"   * THE CRYSTAL, and it does NOT refract.\n"
	"   *\n"
	"   * Snow is a foam of ice and air with an enormous number of internal\n"
	"   * surfaces, so light entering it is scattered out again in every\n"
	"   * direction rather than being bent once and passed on.  What that\n"
	"   * looks like is a bright, flat, slightly translucent patch -- the\n"
	"   * wallpaper behind it survives as a faint wash and nothing more.\n"
	"   *\n"
	"   * Drawn AFTER the refraction and over the top of it for exactly\n"
	"   * that reason: a crystal is opaque to the optics beneath it, and\n"
	"   * as it melts it gets out of the way and lets the bead's lens show\n"
	"   * through -- which is the hand-off, and it is one mix.\n"
	"   */\n"
	"  if (ice.x > 0.0) {\n"
	"    vec3  behind = tap(p, 1.0);\n"
	"    vec3  snowc  = behind * 0.35 + vec3(0.62, 0.66, 0.72);\n"
	"    float solid  = ice.x * (1.0 - ice.z * 0.55);\n"
	"\n"
	"    /*\n"
	"     * THE GLITTER.  A facet catches the light for an instant as the\n"
	"     * crystal turns.  Hashed off the flake's own facet angle and the\n"
	"     * light direction, so it is a property of THIS crystal at THIS\n"
	"     * orientation rather than a twinkle laid over everything -- and it\n"
	"     * dies as the flake melts, because water has no facets.\n"
	"     */\n"
	"    if (u_sparkle > 0.001) {\n"
	"      float g = pow(max(sin(ice.y) * 0.5 + 0.5, 0.0), 22.0);\n"
	"      snowc += vec3(g * u_sparkle * (1.0 - ice.z));\n"
	"    }\n"
	"\n"
	"    col = mix(col, clamp(snowc * u_glow, 0.0, 1.4), solid);\n"
	"  }\n"
	"\n"
	"  /* The contact ring on a bead.  Light entering near the rim leaves\n"
	"     sideways and never reaches the eye, so the very edge is darker\n"
	"     than the middle.  The outer sixth only: wider and every bead gets\n"
	"     an outline, which a drawing of one has and a photograph does\n"
	"     not. */\n"
	"  if (u_rim > 0.001 && best.w > 0.0)\n"
	"    col *= 1.0 - smoothstep(0.84, 1.0, l) * u_rim * best.w * (1.0 - ice.x);\n"
	"\n"
	"  /* The glint and the focus on a bead: one tight highlight off the\n"
	"     surface, and the brightening a lens gives its own middle by\n"
	"     gathering light towards it.  Without the second a field of beads\n"
	"     reads as dents in the glass. */\n"
	"  if (u_specular > 0.001 && best.w > 0.0) {\n"
	"    vec3  V  = vec3(0.0, 0.0, 1.0);\n"
	"    vec3  hv = normalize(u_light + V);\n"
	"    float s  = pow(max(dot(n, hv), 0.0), max(u_shine, 1.0));\n"
	"    float f  = pow(max(1.0 - l, 0.0), 3.0);\n"
	"    col += vec3((s + f * 0.22) * u_specular * best.w * (1.0 - ice.x));\n"
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
 * gowl-fx-glass.c gives at length.
 */
static gboolean
snow_prog_ensure(GowlFxGl *self)
{
	GowlFxSnowProg *p = &self->snow;

	if (p->program != 0)
		return TRUE;
	if (self->snow_tried)
		return FALSE;
	self->snow_tried = TRUE;

	p->program = gowl_fx_link_program(snow_vert_src, snow_frag_src);
	if (p->program == 0) {
		g_warning("fx: the snow shader would not build, so snow backdrops "
		          "will sit this session out");
		return FALSE;
	}

	p->u_soft        = glGetUniformLocation(p->program, "u_soft");
	p->u_sharp       = glGetUniformLocation(p->program, "u_sharp");
	p->u_src_origin  = glGetUniformLocation(p->program, "u_src_origin");
	p->u_src_size    = glGetUniformLocation(p->program, "u_src_size");
	p->u_src_scale   = glGetUniformLocation(p->program, "u_src_scale");
	p->u_size        = glGetUniformLocation(p->program, "u_size");
	p->u_radius      = glGetUniformLocation(p->program, "u_radius");
	p->u_settle_t    = glGetUniformLocation(p->program, "u_settle_t");
	p->u_fall        = glGetUniformLocation(p->program, "u_fall");
	p->u_run         = glGetUniformLocation(p->program, "u_run");
	p->u_frost_t     = glGetUniformLocation(p->program, "u_frost_t");
	p->u_flake       = glGetUniformLocation(p->program, "u_flake");
	p->u_cell        = glGetUniformLocation(p->program, "u_cell");
	p->u_settled     = glGetUniformLocation(p->program, "u_settled");
	p->u_column      = glGetUniformLocation(p->program, "u_column");
	p->u_falling     = glGetUniformLocation(p->program, "u_falling");
	p->u_arms        = glGetUniformLocation(p->program, "u_arms");
	p->u_drift       = glGetUniformLocation(p->program, "u_drift");
	p->u_flutter     = glGetUniformLocation(p->program, "u_flutter");
	p->u_spin        = glGetUniformLocation(p->program, "u_spin");
	p->u_melt        = glGetUniformLocation(p->program, "u_melt");
	p->u_shrink      = glGetUniformLocation(p->program, "u_shrink");
	p->u_bulge       = glGetUniformLocation(p->program, "u_bulge");
	p->u_depth       = glGetUniformLocation(p->program, "u_depth");
	p->u_dispersion  = glGetUniformLocation(p->program, "u_dispersion");
	p->u_runs        = glGetUniformLocation(p->program, "u_runs");
	p->u_run_width   = glGetUniformLocation(p->program, "u_run_width");
	p->u_run_len     = glGetUniformLocation(p->program, "u_run_len");
	p->u_beads       = glGetUniformLocation(p->program, "u_beads");
	p->u_frost       = glGetUniformLocation(p->program, "u_frost");
	p->u_frost_scale = glGetUniformLocation(p->program, "u_frost_scale");
	p->u_sparkle     = glGetUniformLocation(p->program, "u_sparkle");
	p->u_fog         = glGetUniformLocation(p->program, "u_fog");
	p->u_clarity     = glGetUniformLocation(p->program, "u_clarity");
	p->u_glow        = glGetUniformLocation(p->program, "u_glow");
	p->u_specular    = glGetUniformLocation(p->program, "u_specular");
	p->u_shine       = glGetUniformLocation(p->program, "u_shine");
	p->u_rim         = glGetUniformLocation(p->program, "u_rim");
	p->u_light       = glGetUniformLocation(p->program, "u_light");
	p->u_tint        = glGetUniformLocation(p->program, "u_tint");
	p->u_absorb      = glGetUniformLocation(p->program, "u_absorb");
	p->u_brightness  = glGetUniformLocation(p->program, "u_brightness");
	p->u_alpha       = glGetUniformLocation(p->program, "u_alpha");
	p->u_seed        = glGetUniformLocation(p->program, "u_seed");
	p->a_pos         = glGetAttribLocation(p->program, "a_pos");
	p->a_uv          = glGetAttribLocation(p->program, "a_uv");
	return TRUE;
}

void
gowl_fx_snow_params_init(GowlFxSnowParams *params)
{
	if (params == NULL)
		return;

	memset(params, 0, sizeof(*params));
	/*
	 * A steady fall on a pane just above freezing: the middle preset's
	 * numbers, so a caller that sets only a size gets snow rather than a
	 * clean window.
	 *
	 * `flake' is the ruler and everything is measured against it.  32 px
	 * is a crystal about sixty across on a HiDPI panel, which is large
	 * enough that the six arms are legible -- and legible arms are the
	 * whole point, because a flake too small to resolve is a white dot
	 * and a window full of white dots is dust, not snow.
	 *
	 * `shrink' at 0.35 is the collapse from crystal to bead.  It looks
	 * severe written down and it is not severe enough to be wrong: a
	 * snowflake is mostly air.
	 *
	 * `run-width' is half what the rain uses, and that is the same
	 * argument from the other end.  The rain's running heads are BIGGER
	 * than its resting drops, because a run is water that has gathered on
	 * its way down; melt-water has gathered nothing, so a run head here
	 * has to be smaller than the crystal it came from or the arithmetic
	 * of the whole effect is visibly wrong.
	 */
	params->flake       = 30.0f;
	params->cell        = 165.0f;
	params->settled     = 0.30f;
	params->column      = 95.0f;
	params->falling     = 0.70f;
	params->arms        = 0.75f;
	params->drift       = 120.0f;
	params->flutter     = 0.30f;
	params->spin        = 0.8f;
	params->melt        = 0.45f;
	params->shrink      = 0.35f;
	params->bulge       = 1.0f;
	params->depth       = 5.5f;
	params->dispersion  = 0.6f;
	params->runs        = 0.30f;
	params->run_width   = 95.0f;
	params->run_len     = 300.0f;
	params->beads       = 0.62f;
	params->frost       = 0.45f;
	params->frost_scale = 24.0f;
	params->sparkle     = 0.55f;
	params->fog         = 0.60f;
	params->clarity     = 0.92f;
	params->glow        = 1.12f;
	params->specular    = 0.40f;
	params->shine       = 60.0f;
	params->rim         = 0.28f;
	params->absorption  = 0.08f;
	params->brightness  = 1.0f;
	params->alpha       = 1.0f;
	params->src_scale   = 1.0f;
	/* The faintest cold cast in the melt-water. */
	params->tint[0] = 0.88f;
	params->tint[1] = 0.94f;
	params->tint[2] = 1.00f;
	/* Up and to the left, well off the glass: a light near the plane of
	 * the pane puts the glint on every rim at once, which reads as an
	 * outline rather than as a highlight. */
	params->light[0] = -0.42f;
	params->light[1] = -0.55f;
	params->light[2] =  0.72f;
}

void
gowl_fx_snow_advance(GowlFxSnowClock *clock, gdouble dt, gdouble speed,
                     gdouble life_seconds, gdouble frost_rate)
{
	/*
	 * The three fall rates and the two run rates, all irrational against
	 * each other for the reason gowl_fx_rain_advance() sets out: rates
	 * that look incommensurate and are really k/100 all return to zero
	 * together every hundred seconds, and the whole field repeats exactly.
	 *
	 * A twelfth, where the rain uses a fifth.  A flake takes a long,
	 * wandering time to cross a window -- at a rate of 1 it would cross
	 * in a second, which is sleet -- and that unhurriedness is most of
	 * what makes it read as snow rather than as rain in a white coat.
	 */
	static const gdouble fall_rate[3] = {
		0.0833333333333333, 0.0515028323958246, 0.0318305009375087
	};
	/* The runs are slower still: melt-water is a trickle, not a
	 * downpour, and a bead that took four seconds to form should not
	 * cross the pane in one. */
	static const gdouble run_rate[2] = {
		0.1236067977499790, 0.0763932022500210
	};
	gint i;

	if (clock == NULL)
		return;
	if (!(dt > 0.0))
		return;
	if (dt > 0.25)
		dt = 0.25;   /* a long stall is not a long winter */
	if (!(life_seconds > 0.1))
		life_seconds = 30.0;

	/*
	 * Cycles, wrapped into [0, GOWL_FX_SNOW_CYCLES), exactly as the rain:
	 * the fraction is where a flake has got to and the whole part is
	 * which flake it is.  The one rule that comes with it is that every
	 * multiplier applied inside the shader must be whole, which is why
	 * the per-column rate there is 1, 2 or 3.
	 */
	clock->settle += dt / life_seconds;
	clock->settle = fmod(clock->settle, GOWL_FX_SNOW_CYCLES);
	if (clock->settle < 0.0)
		clock->settle += GOWL_FX_SNOW_CYCLES;

	for (i = 0; i < 3; i++) {
		clock->fall[i] += dt * speed * fall_rate[i];
		clock->fall[i] = fmod(clock->fall[i], GOWL_FX_SNOW_CYCLES);
		if (clock->fall[i] < 0.0)
			clock->fall[i] += GOWL_FX_SNOW_CYCLES;
	}

	for (i = 0; i < 2; i++) {
		clock->run[i] += dt * run_rate[i];
		clock->run[i] = fmod(clock->run[i], GOWL_FX_SNOW_CYCLES);
		if (clock->run[i] < 0.0)
			clock->run[i] += GOWL_FX_SNOW_CYCLES;
	}

	/*
	 * THE FROST IS NOT A CYCLE and must not be wrapped like one.
	 *
	 * Ice grows and then it is there; wrapping this would take the whole
	 * pane from thick frost back to clear glass in one frame, once, some
	 * minutes in.  It saturates at 1 instead -- a fully frosted pane is
	 * as frosted as it gets -- and the shader only ever uses it smoothly,
	 * so there is nothing to be continuous across.
	 */
	if (frost_rate > 0.0) {
		clock->frost += dt * frost_rate;
		if (clock->frost > 1.0)
			clock->frost = 1.0;
	} else {
		/* Turned off: melt back rather than snapping, so switching the
		 * preset does not take a window's frost away between frames. */
		clock->frost -= dt * 0.35;
		if (clock->frost < 0.0)
			clock->frost = 0.0;
	}
}

gboolean
gowl_fx_pass_snow(GowlFxPass             *pass,
                  const GowlFxTexture    *soft,
                  const GowlFxTexture    *sharp,
                  const GowlFxSnowParams *params,
                  const GowlFxSnowClock  *clock)
{
	GowlFxGl             *gl;
	const GowlFxSnowProg *p;
	const GowlFxTexture  *clear_src;
	GowlFxSnowClock       still;
	gfloat                fall[3];
	gfloat                run[2];
	gfloat                radius;
	gfloat                cell, column, flake;
	gint                  i;

	if (pass == NULL || params == NULL || soft == NULL)
		return FALSE;
	if (params->width <= 0 || params->height <= 0)
		return FALSE;
	if (soft->tex == 0 || soft->width <= 0 || soft->height <= 0)
		return FALSE;

	gl = pass->gl;
	if (!snow_prog_ensure(gl))
		return FALSE;
	p = &gl->snow;

	if (clock == NULL) {
		memset(&still, 0, sizeof(still));
		clock = &still;
	}
	for (i = 0; i < 3; i++)
		fall[i] = (gfloat)clock->fall[i];
	for (i = 0; i < 2; i++)
		run[i] = (gfloat)clock->run[i];

	radius = CLAMP(params->radius, 0.0f,
	               (gfloat)MIN(params->width, params->height) * 0.5f);
	cell   = MAX(12.0f, params->cell);
	column = MAX(12.0f, params->column);

	/*
	 * THE FLAKE SIZE IS CAPPED AGAINST BOTH LAYOUTS, and it is load
	 * bearing: the three-column reach in snow_fall_layer() and the
	 * nine-cell reach in settled_layer() are both worked out with a flake
	 * of at most 0.36 of a column and 0.40 of a cell in them.  A config
	 * that asked for flakes the size of the spacing would have them
	 * sliced off along straight lines at the layer boundaries -- a
	 * lattice drawn in the negative -- so the cap lives here rather than
	 * being trusted to whoever wrote the preset.
	 *
	 * 1.32 is the largest per-flake size jitter either layer applies and
	 * 1.05 the largest layer scaling.
	 */
	flake = MAX(2.0f, params->flake);
	flake = MIN(flake, cell * (0.40f / (1.32f * 1.05f)));
	flake = MIN(flake, column * (0.36f / 1.32f));

	clear_src = (sharp != NULL && sharp->tex != 0) ? sharp : soft;

	glUseProgram(p->program);
	glUniform2f(p->u_src_origin, params->src_origin[0], params->src_origin[1]);
	glUniform2f(p->u_src_size, (gfloat)MAX(1, soft->width),
	            (gfloat)MAX(1, soft->height));
	glUniform1f(p->u_src_scale,
	            params->src_scale > 0.0f ? params->src_scale : 1.0f);
	glUniform2f(p->u_size, (gfloat)params->width, (gfloat)params->height);
	glUniform1f(p->u_radius, radius);
	glUniform1f(p->u_settle_t, (gfloat)clock->settle);
	glUniform3fv(p->u_fall, 1, fall);
	glUniform2fv(p->u_run, 1, run);
	glUniform1f(p->u_frost_t, (gfloat)CLAMP(clock->frost, 0.0, 1.0));
	glUniform1f(p->u_flake, flake);
	glUniform1f(p->u_cell, cell);
	glUniform1f(p->u_settled, CLAMP(params->settled, 0.0f, 1.0f));
	glUniform1f(p->u_column, column);
	glUniform1f(p->u_falling, CLAMP(params->falling, 0.0f, 1.0f));
	glUniform1f(p->u_arms, CLAMP(params->arms, 0.0f, 1.0f));
	glUniform1f(p->u_drift, params->drift);
	/* Capped at 0.40 of a column: the three-column reach above is worked
	 * out with that number in it. */
	glUniform1f(p->u_flutter, CLAMP(params->flutter, 0.0f, 0.40f));
	glUniform1f(p->u_spin, CLAMP(params->spin, 0.0f, 8.0f));
	glUniform1f(p->u_melt, CLAMP(params->melt, 0.0f, 0.80f));
	glUniform1f(p->u_shrink, CLAMP(params->shrink, 0.05f, 1.0f));
	glUniform1f(p->u_bulge, CLAMP(params->bulge, 0.1f, 3.0f));
	glUniform1f(p->u_depth, CLAMP(params->depth, 0.0f, 20.0f));
	glUniform1f(p->u_dispersion, MAX(0.0f, params->dispersion));
	glUniform1f(p->u_runs, CLAMP(params->runs, 0.0f, 1.0f));
	glUniform1f(p->u_run_width, MAX(8.0f, params->run_width));
	glUniform1f(p->u_run_len, MAX(0.0f, params->run_len));
	glUniform1f(p->u_beads, CLAMP(params->beads, 0.0f, 1.0f));
	glUniform1f(p->u_frost, CLAMP(params->frost, 0.0f, 1.0f));
	glUniform1f(p->u_frost_scale, MAX(4.0f, params->frost_scale));
	glUniform1f(p->u_sparkle, CLAMP(params->sparkle, 0.0f, 2.0f));
	glUniform1f(p->u_fog, CLAMP(params->fog, 0.0f, 1.0f));
	glUniform1f(p->u_clarity, CLAMP(params->clarity, 0.0f, 1.0f));
	glUniform1f(p->u_glow, CLAMP(params->glow, 0.0f, 3.0f));
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
