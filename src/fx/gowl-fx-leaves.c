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
 * gowl-fx-leaves.c -- autumn against the window.
 *
 * THIS IS THE ODD ONE OUT OF THE FAMILY, and the difference is worth
 * stating before anything else: every other backdrop here BENDS the
 * wallpaper.  The glass refracts it, the water refracts it, the rain
 * refracts it, the bubbles refract it the other way.  A leaf does not
 * refract anything.  It is an OPAQUE OBJECT that covers what is behind
 * it, and the only thing that saves it from being a brown sticker is
 * that it is BACKLIT: a leaf on a window has daylight behind it, so it
 * glows with its own colour and its veins show through dark.
 *
 * That changes the whole shape of the shader.  There is no displacement
 * and no normal; there is a silhouette, a colour, a set of veins and a
 * shadow.  What it keeps from the rain is the bookkeeping -- jittered
 * layers, wrapped cycle clocks, a best-wins accumulator -- because that
 * machinery is about avoiding lattices, and lattices do not care what is
 * being drawn on them.
 *
 * WHAT MAKES IT READ AS FALLING LEAVES, in the order the eye notices:
 *
 *   1. THEY GO EDGE-ON AND BACK.  A falling leaf tumbles about its long
 *      axis, and twice a turn it presents its edge to you and collapses
 *      to a LINE.  Nothing else on a screen does that.  It is one
 *      multiply -- the blade is squashed across by |cos(tumble)| -- and
 *      it is worth more than every other cue put together.
 *
 *   2. THEY DO NOT FALL STRAIGHT AND THEY DO NOT FALL EVENLY.  A leaf
 *      swings side to side, leans INTO the swing, and slows at the ends
 *      of each swing where it has to turn around.  A leaf on a straight
 *      line at constant speed is a sprite.
 *
 *   3. THE FRONT AND THE BACK ARE DIFFERENT COLOURS.  The underside of a
 *      leaf is paler and duller.  Since the tumble already tells us
 *      which face is towards us -- it is the sign of cos(tumble) -- this
 *      is free, and it makes the tumble legible instead of merely thin.
 *
 *   4. THEY STICK, AND THEY STICK FLAT.  A leaf that lands on wet glass
 *      lies against it with a hard little contact shadow and stops
 *      tumbling entirely.  Stuck leaves are what make the window look
 *      lived in rather than looked through.
 *
 *   5. THE WIND IS ONE WIND.  When it gets up, EVERY stuck leaf shivers
 *      at once and the falling ones are all thrown the same way.  A gust
 *      that each leaf decides on for itself is not a gust, it is noise;
 *      this is why the wind lives in the clock, on the CPU, as a single
 *      number the whole shader reads.
 *
 *   6. THEY PEEL, THEY DO NOT SLIDE.  A leaf leaves the glass by lifting
 *      at one edge and pivoting about its stem until it lets go.  It is
 *      also the only exit that FITS: see the bound on LEAF_FLY below.
 *
 *   7. THEY DRY OUT.  A leaf that has been there a while curls at the
 *      edges, so it touches only in the middle and its shadow softens
 *      from the rim inwards.
 *
 * WHY IT IS NOT A GRID.  The same discipline as the rain, and the same
 * two mechanisms: falling leaves are hashed COLUMNS at three widths that
 * do not divide, stuck leaves are two jittered lattices turned against
 * each other at an incommensurate pitch, and the seed translates the
 * whole field per window.  Nothing here is periodic in space.
 */

#undef G_LOG_DOMAIN
#define G_LOG_DOMAIN "gowl-fx"

#include "gowl-fx-private.h"

#include <math.h>
#include <string.h>

static const gchar leaf_vert_src[] =
	"attribute vec2 a_pos;\n"
	"attribute vec2 a_uv;\n"
	"varying vec2 v_uv;\n"
	"void main() {\n"
	"  v_uv = a_uv;\n"
	"  gl_Position = vec4(a_pos, 0.0, 1.0);\n"
	"}\n";

/*
 * highp conditionally, for the reason the rest of the family gives:
 * naming a precision that does not exist fails to COMPILE rather than
 * falling back.  It is wanted here for the leaf-local coordinates -- the
 * venation is a high-frequency function of them, and at mediump the
 * chevrons break up into moire across a large leaf.
 */
static const gchar leaf_frag_src[] =
	"#ifdef GL_FRAGMENT_PRECISION_HIGH\n"
	"precision highp float;\n"
	"#else\n"
	"precision mediump float;\n"
	"#endif\n"
	"\n"
	"uniform sampler2D u_soft;      /* the hazed wallpaper */\n"
	"uniform sampler2D u_sharp;     /* the same wallpaper, unblurred */\n"
	"uniform vec2  u_src_origin;    /* where this rect sits in them, px */\n"
	"uniform vec2  u_src_size;      /* their size, px */\n"
	"uniform float u_src_scale;     /* source px per buffer px */\n"
	"uniform vec2  u_size;          /* the rect, px */\n"
	"uniform float u_radius;        /* corner radius, px */\n"
	"uniform float u_stick_t;       /* stuck-leaf tenure clock, cycles */\n"
	"uniform vec3  u_fall;          /* the three falling clocks, cycles */\n"
	"uniform float u_gust;          /* how hard it is blowing NOW, 0..1 */\n"
	"uniform float u_sway;          /* the wind's phase, radians */\n"
	"uniform float u_leaf;          /* a leaf's radius, px */\n"
	"uniform float u_cell;          /* px per cell of the stuck layer */\n"
	"uniform float u_stuck;         /* how many cells hold a leaf, 0..1 */\n"
	"uniform float u_column;        /* px per column of the falling layer */\n"
	"uniform float u_falling;       /* how many columns carry one, 0..1 */\n"
	"uniform float u_flutter;       /* swing, as a fraction of a column */\n"
	"uniform float u_tumble;        /* turns per fall */\n"
	"uniform float u_wind;          /* steady drift, px per fall */\n"
	"uniform float u_gust_push;     /* how far a gust throws things, px */\n"
	"uniform float u_curl;          /* how dried and curled, 0..1 */\n"
	"uniform float u_veins;         /* venation strength, 0..1 */\n"
	"uniform float u_translucency;  /* how backlit a leaf is, 0..1 */\n"
	"uniform float u_gloss;         /* how wet the leaves are */\n"
	"uniform float u_shadow;        /* the contact shadow, 0..1 */\n"
	"uniform float u_fog;           /* how hazy the pane is, 0..1 */\n"
	"uniform float u_clarity;       /* how much a wet patch lifts it */\n"
	"uniform float u_shine;         /* specular exponent */\n"
	"uniform vec3  u_tint_warm;     /* just turned: reds */\n"
	"uniform vec3  u_tint_gold;     /* at its peak: oranges, yellows */\n"
	"uniform vec3  u_tint_dry;      /* down a while: browns */\n"
	"uniform vec3  u_light;\n"
	"uniform float u_brightness;\n"
	"uniform float u_alpha;\n"
	"uniform float u_seed;          /* which crop of the fall, 0..16 */\n"
	"varying vec2 v_uv;\n"
	"\n"
	"const float TAU = 6.2831853;\n"
	"const float PI  = 3.14159265;\n"
	"/*\n"
	" * How many falls, and how many tenures, before the field repeats.\n"
	" * The rain's trade: larger repeats later and quantises a position\n"
	" * more coarsely because a float has to hold the clock.\n"
	" */\n"
	"const float LEAF_CYCLES = 128.0;\n"
	"/*\n"
	" * How far a leaf may travel while it is being blown off, in cells.\n"
	" *\n"
	" * The bound is arithmetic rather than taste.  A pixel asks the nine\n"
	" * cells around it, so the nearest leaf it does NOT ask sits in the\n"
	" * cell two away; with the centre jittered over the whole cell that\n"
	" * leaf is at least 2 - 1 = 1.0 cells off.  Everything a leaf reaches\n"
	" * has to fit inside that: the blade is capped at 0.44 of a cell (see\n"
	" * the clamp on u_leaf in gowl_fx_pass_leaves) and this is the rest,\n"
	" * leaving a quarter of a cell spare.  Take more and a departing leaf\n"
	" * is sliced off along a straight cell edge -- which is a lattice,\n"
	" * drawn once a tenure, and the exact artefact the layer is arranged\n"
	" * to avoid.\n"
	" *\n"
	" * Spending the budget on ROTATION and SCALE instead of on travel is\n"
	" * what makes the exit both bounded and correct: a leaf coming off wet\n"
	" * glass really does pivot about its stem and flip away rather than\n"
	" * sliding sideways like a hockey puck.\n"
	" */\n"
	"const float LEAF_FLY = 0.30;\n"
	"\n"
	"/* Signed distance to a rounded rect centred in @size; negative inside. */\n"
	"float sdf_rect(vec2 p, vec2 size, float r) {\n"
	"  vec2 h = size * 0.5;\n"
	"  vec2 q = abs(p - h) - (h - vec2(r));\n"
	"  return min(max(q.x, q.y), 0.0) + length(max(q, vec2(0.0))) - r;\n"
	"}\n"
	"\n"
	"/* Three uncorrelated numbers from three inputs; see gowl-fx-rain.c\n"
	" * for why it must be three and not two. */\n"
	"vec3 hash3v(vec3 c) {\n"
	"  vec3 n = vec3(dot(c, vec3(127.1,  89.3,  54.7)),\n"
	"                dot(c, vec3( 71.9, 151.3, 101.7)),\n"
	"                dot(c, vec3(167.3,  61.7, 133.9)));\n"
	"  return fract(sin(n) * vec3(43758.5453, 28001.8384, 19349.6631));\n"
	"}\n"
	"\n"
	"/*\n"
	" * HOW FAR INSIDE A LEAF A POINT IS, in leaf-local units.\n"
	" *\n"
	" * @q is the point in the leaf's own frame, scaled so the blade runs\n"
	" * from the base at the origin out to about 1 at the tip, with +y\n"
	" * towards the tip.  @kind picks the silhouette, @lobe how deep the\n"
	" * lobes are cut.  Returns a signed value, positive inside.\n"
	" *\n"
	" * THREE SHAPES, because one is a logo.  A window under a tree is\n"
	" * under ONE tree, but a window in a street is not, and a screen full\n"
	" * of identical maples reads as a flag.\n"
	" *\n"
	" *   kind < 0.40   OVATE.  Birch, beech, elm: a blade pointed at both\n"
	" *                 ends.  The half-width is sin(pi*t) raised to a\n"
	" *                 power below one, which is what gives the drawn-out\n"
	" *                 point rather than the blunt end an ellipse has.\n"
	" *   kind < 0.75   MAPLE.  Five lobes on a polar profile, oriented so\n"
	" *                 a lobe points at the tip and the notch between the\n"
	" *                 lowest pair sits where the stem joins -- which is\n"
	" *                 exactly where a real one's sinus is, and it falls\n"
	" *                 out of the phase rather than being carved.\n"
	" *   otherwise     OAK.  The ovate profile with its half-width rippled\n"
	" *                 along the blade: lobes down either side of a\n"
	" *                 midrib, which is what pinnate lobing is and what a\n"
	" *                 polar profile cannot give however elongated.\n"
	" */\n"
	"float leaf_shape(vec2 q, float kind, float lobe) {\n"
	"  if (kind < 0.40) {\n"
	"    /* Ovate.  Along the blade first: nothing outside [-0.1, 1]. */\n"
	"    float t = clamp((q.y + 0.10) / 1.10, 0.0, 1.0);\n"
	"    float w = 0.46 * pow(max(sin(PI * t), 0.0), 0.62);\n"
	"    float ends = min(q.y + 0.10, 1.0 - q.y);\n"
	"    return min(w - abs(q.x), ends * 2.0);\n"
	"  }\n"
	"\n"
	"  if (kind < 0.75) {\n"
	"    /*\n"
	"     * Maple: a POLAR profile, because a maple really is radial ---\n"
	"     * five lobes spreading from one point where the stem joins.\n"
	"     * |cos(2.5*(a - pi/2))| has five maxima around the circle and a\n"
	"     * zero at a = -pi/2, so the tip lands at the top and the notch\n"
	"     * between the lowest pair lands exactly where the sinus is, out\n"
	"     * of the phase rather than carved in.\n"
	"     *\n"
	"     * THE EXPONENT DECIDES WHETHER IT IS A MAPLE OR A CLOUD, and the\n"
	"     * first attempt at this was a cloud.  Below 1 the profile stays\n"
	"     * near its maximum for most of the turn and dives only at the\n"
	"     * notches, which is five fat round bulges; above 1 it is near its\n"
	"     * minimum except at the peaks, which is a star.  1.4 is the\n"
	"     * broad-but-pointed lobe a maple actually has.\n"
	"     */\n"
	"    float a = atan(q.y, q.x);\n"
	"    float f = abs(cos(2.5 * (a - PI * 0.5)));\n"
	"    float b = lobe * 0.62;\n"
	"    return (1.0 - b) + b * pow(f, 1.4) - length(q);\n"
	"  }\n"
	"\n"
	"  {\n"
	"    /*\n"
	"     * Oak: the ovate profile with the half-width RIPPLED along the\n"
	"     * blade, which is what a pinnately lobed leaf is --- lobes down\n"
	"     * either side of a midrib, not around a centre.\n"
	"     *\n"
	"     * Drawing it polar like the maple, which is what this did first,\n"
	"     * gives a lobed DISC however much it is elongated, and a lobed\n"
	"     * disc is a flower.  The lobes have to run along the blade\n"
	"     * because that is the axis an oak leaf has.\n"
	"     */\n"
	"    float t    = clamp((q.y + 0.12) / 1.12, 0.0, 1.0);\n"
	"    float base = 0.40 * pow(max(sin(PI * t), 0.0), 0.42);\n"
	"    float w    = base * (1.0 + lobe * 0.40 * cos(t * TAU * 2.5 + 0.9));\n"
	"    float ends = min(q.y + 0.12, 1.0 - q.y);\n"
	"    return min(w - abs(q.x), ends * 2.0);\n"
	"  }\n"
	"}\n"
	"\n"
	"/* Distance from @p to the segment ab; used for the stem. */\n"
	"float sdf_seg(vec2 p, vec2 a, vec2 b) {\n"
	"  vec2  pa = p - a;\n"
	"  vec2  ba = b - a;\n"
	"  float h  = clamp(dot(pa, ba) / max(dot(ba, ba), 1e-5), 0.0, 1.0);\n"
	"  return length(pa - ba * h);\n"
	"}\n"
	"\n"
	"/*\n"
	" * THE VENATION.\n"
	" *\n"
	" * Pinnate: one midrib up the middle and lateral veins branching off\n"
	" * it at an angle, towards the tip.  Both come out of one trick --\n"
	" * shear the leaf-local y by |x| and the level sets of the result are\n"
	" * chevrons pointing at the tip, so fract() of the sheared coordinate\n"
	" * IS the lateral venation and no loop is needed.\n"
	" *\n"
	" * It is drawn as ABSORPTION rather than as paint.  A vein is thicker\n"
	" * tissue, so backlit it is DARKER than the blade around it; painting\n"
	" * pale lines on top is what a diagram of a leaf looks like and not\n"
	" * what one on a window looks like.\n"
	" */\n"
	"float leaf_veins(vec2 q, float nv, float aa) {\n"
	"  float mid = 1.0 - smoothstep(0.011, 0.011 + aa, abs(q.x));\n"
	"  float sh  = q.y - abs(q.x) * 0.85;\n"
	"  /* 1 ON a vein line and 0 between: fract() of the sheared\n"
	"     coordinate is zero at each vein, and folding it about a half\n"
	"     turns that into a ridge.  The test has to select the RIDGE --\n"
	"     lighting everything under it instead washes the whole blade and\n"
	"     shows no venation at all. */\n"
	"  float lat = abs(fract(sh * nv) - 0.5) * 2.0;\n"
	"  float e   = clamp(aa * nv * 1.2, 0.02, 0.35);\n"
	"  float lats = smoothstep(0.88 - e, 0.88 + e, lat)\n"
	"               /* Thinning towards the rim, where the veins run out. */\n"
	"               * (1.0 - smoothstep(0.25, 1.0, length(q)) * 0.55);\n"
	"  return clamp(mid + lats * 0.60, 0.0, 1.0);\n"
	"}\n"
	"\n"
	"/*\n"
	" * ONE LEAF, folded into the running best.\n"
	" *\n"
	" * @best carries (coverage, face, shade, tint-index) and @local the\n"
	" * leaf-local point and radius, because unlike a drop a leaf needs its\n"
	" * own frame kept for the venation.  Leaves do not blend: one in front\n"
	" * of another simply hides it, so the greater coverage wins outright\n"
	" * and a tie is broken by whichever is nearer the viewer.\n"
	" *\n"
	" * @rot turns it, @squash is |cos(tumble)| -- the collapse to an edge\n"
	" * -- and @face is its sign, so a caller gets the underside for free.\n"
	" */\n"
	"void leaf_at(vec2 p, vec2 centre, float r, float rot, float squash,\n"
	"             float face, float alive, vec3 h, float depth,\n"
	"             inout vec4 best, inout vec4 local, inout float shade) {\n"
	"  if (alive <= 0.0 || r <= 0.0 || squash <= 0.001)\n"
	"    return;\n"
	"\n"
	"  vec2  d  = p - centre;\n"
	"  /* Cheap reject before the trigonometry: nothing outside the\n"
	"     circumscribing circle can be inside the blade. */\n"
	"  if (dot(d, d) > r * r * 1.45)\n"
	"    return;\n"
	"\n"
	"  float c  = cos(rot);\n"
	"  float s  = sin(rot);\n"
	"  vec2  q  = vec2(c * d.x + s * d.y, -s * d.x + c * d.y) / r;\n"
	"  /* The tumble, and it is applied ACROSS the blade only: a leaf seen\n"
	"     edge-on is as long as it ever was and as wide as a line. */\n"
	"  q.x /= max(squash, 0.06);\n"
	"\n"
	"  float aa = max(0.02, 1.4 / max(r, 1.0));\n"
	"  float sh = leaf_shape(q - vec2(0.0, 0.05), h.x, 0.55 + 0.4 * h.y);\n"
	"  /* The stem, running back from the base.  Thin, and it is what makes\n"
	"     a silhouette read as a leaf rather than as a petal. */\n"
	"  float st = 0.055 - sdf_seg(q, vec2(0.0, -0.06), vec2(0.0, -0.42));\n"
	"  float cover = max(smoothstep(-aa, aa, sh),\n"
	"                    smoothstep(-aa, aa, st) * 0.9) * alive;\n"
	"\n"
	"  if (cover <= 0.0)\n"
	"    return;\n"
	"\n"
	"  /* The contact shadow, and only for leaves ON the glass: a leaf\n"
	"     falling past outside the window is nowhere near the pane and\n"
	"     casts nothing on it.  @depth of 1 or more is what says \"stuck\".\n"
	"     Accumulated whether or not this leaf wins the pixel, because one\n"
	"     behind another still darkens the glass beside it. */\n"
	"  if (depth >= 1.0)\n"
	"    shade = max(shade, cover);\n"
	"\n"
	"  /*\n"
	"   * WHICH LEAF THIS PIXEL BELONGS TO.\n"
	"   *\n"
	"   * Coverage is the UNION -- two leaves overlapping cover the glass\n"
	"   * between them, they do not each cover it half -- but the colour\n"
	"   * comes from whichever is in front.  A nearer leaf takes the pixel\n"
	"   * only once it is actually solid there (0.85), so the antialiased\n"
	"   * rim of one in front does not punch a translucent notch through\n"
	"   * the one behind; inside the rim the nearer leaf is opaque and the\n"
	"   * question does not arise.\n"
	"   */\n"
	"  float un = max(cover, best.x);\n"
	"  bool  front = best.x > 0.0 && depth > best.z + 0.01 && cover > 0.85;\n"
	"\n"
	"  if (front || cover > best.x) {\n"
	"    best  = vec4(un, face, depth, h.z);\n"
	"    local = vec4(q, r, h.y);\n"
	"  } else {\n"
	"    best.x = un;\n"
	"  }\n"
	"}\n"
	"\n"
	"/*\n"
	" * ONE LAYER OF FALLING LEAVES.\n"
	" *\n"
	" * Columns, for the reason the rain's runs are columns: gravity has no\n"
	" * x component, so the average path is vertical however much a given\n"
	" * leaf wanders off it.  What stops a column reading AS a column is\n"
	" * that it is empty most of the time, that the leaf sits anywhere\n"
	" * across its width, and that three layers of different widths and\n"
	" * speeds run over each other.\n"
	" *\n"
	" * THREE COLUMNS, and the number is arithmetic.  The nearest column a\n"
	" * pixel does NOT ask is two away, whose lane is at least 2 + 0.5 -\n"
	" * 0.22 - 1 = 1.28 widths off it.  A leaf sits up to 0.22 of a width\n"
	" * off its own lane, swings another `flutter' (capped at 0.45) and its\n"
	" * blade reaches 0.42, which is 1.09 -- inside that, with room to\n"
	" * spare.  Ask only two columns and every leaf that swung left is\n"
	" * sliced off down a straight vertical line.\n"
	" *\n"
	" * WHICH LEAF IS PER FALL, not per column.  A column that dropped a\n"
	" * leaf a moment ago may be empty for the next several cycles and the\n"
	" * one after that is a different species at a different size on a\n"
	" * different line.  Pinning any of that to the column gives the same\n"
	" * dozen leaves falling down the same dozen lines forever, which is\n"
	" * the failure the rain documents having actually shipped once.\n"
	" */\n"
	"void fall_layer(vec2 p, float w, float t, float density, float lyr,\n"
	"                float size, float depth,\n"
	"                inout vec4 best, inout vec4 local, inout float shade) {\n"
	"  if (density <= 0.0 || w <= 0.0)\n"
	"    return;\n"
	"\n"
	"  float lid = lyr * 64.0 + u_seed;\n"
	"  float gph = lyr * 0.31 + u_seed * 0.053;\n"
	"  int   n;\n"
	"\n"
	"  for (n = -1; n <= 1; n++) {\n"
	"    float col = floor(p.x / w + gph) + float(n);\n"
	"    /* Per COLUMN and permanent: only the rate and the phase, because\n"
	"       which fall it is is worked out from them. */\n"
	"    vec3  hc = hash3v(vec3(col, lid, 0.0));\n"
	"    float k  = 1.0 + floor(hc.z * 2.999);       /* 1, 2 or 3 */\n"
	"    float u  = t * k + hc.x;\n"
	"    float fi = mod(floor(u), LEAF_CYCLES);\n"
	"    vec3  hr = hash3v(vec3(col, lid + 5.0, fi));\n"
	"\n"
	"    if (hr.z > density)\n"
	"      continue;\n"
	"\n"
	"    vec3  hs   = hash3v(vec3(col, lid + 21.0, fi));\n"
	"    float r    = size * (0.68 + 0.62 * hs.x);\n"
	"    float marg = r * 1.8;\n"
	"    float span = u_size.y + 2.0 * marg;\n"
	"    float a    = fract(u);\n"
	"    /*\n"
	"     * THE SWING, and everything hangs off it.\n"
	"     *\n"
	"     * One slow sine across the column plus a slower, weaker one, so a\n"
	"     * leaf leans one way for a while and then the other rather than\n"
	"     * metronoming.  Both phases are per FALL.\n"
	"     */\n"
	"    float sw   = 1.0 + 1.4 * hs.y;\n"
	"    float ph   = hs.z * TAU;\n"
	"    float swing = sin(a * TAU * sw + ph);\n"
	"    float lean  = cos(a * TAU * sw + ph);\n"
	"    /*\n"
	"     * A leaf DOES NOT FALL AT A CONSTANT RATE.  It sinks fastest\n"
	"     * through the middle of a swing, where it is edge-on and slipping,\n"
	"     * and stalls at each end where it has to turn over.  The height is\n"
	"     * therefore the phase plus a small term in the swing -- still\n"
	"     * monotonic, because the term is a fifteenth of the phase, but\n"
	"     * visibly unsteady.\n"
	"     */\n"
	"    /* The coefficient is not free: d(drop)/da stays positive only\n"
	"       while it is under 1/(TAU*sw), and sw tops out at 2.4.  0.055\n"
	"       spends 83 per cent of that, which swings the sink rate between\n"
	"       a sixth and nearly twice the average and never backwards. */\n"
	"    float drop = a - 0.055 * swing * swing + 0.055;\n"
	"    float y    = -marg + drop * span;\n"
	"    /* The wind: a steady drift down the fall, plus whatever the gust\n"
	"       is doing right now.  Both are the SAME for every leaf on\n"
	"       screen, which is what makes a gust look like one gust. */\n"
	"    float lane = (col - gph + 0.5) * w + (hr.x - 0.5) * w * 0.44;\n"
	"    float x    = lane + swing * w * u_flutter\n"
	"                 + u_wind * drop\n"
	"                 + u_gust * u_gust_push * (0.6 + 0.8 * hs.x) * drop;\n"
	"\n"
	"    /*\n"
	"     * THE TUMBLE.  Turns per fall, hashed per leaf so no two are in\n"
	"     * step, and the in-plane rotation leans INTO the swing -- a leaf\n"
	"     * slipping sideways points the way it is going.\n"
	"     */\n"
	"    float tum   = a * TAU * u_tumble * (0.7 + 0.9 * hr.y) + hs.x * TAU;\n"
	"    float flip  = cos(tum);\n"
	"    float rot   = lean * 0.55 + hr.y * TAU + a * TAU * 0.12 * (hs.y - 0.5);\n"
	"\n"
	"    leaf_at(p, vec2(x, y), r, rot, abs(flip), sign(flip),\n"
	"            /* Fade the first and last sliver rather than clipping:\n"
	"               a leaf that appears at a fixed height draws a line\n"
	"               across the window once a cycle. */\n"
	"            clamp(min(drop, 1.0 - drop) * 18.0, 0.0, 1.0),\n"
	"            hr, depth, best, local, shade);\n"
	"  }\n"
	"}\n"
	"\n"
	"/*\n"
	" * LEAVES ALREADY ON THE GLASS.\n"
	" *\n"
	" * A jittered grid, mostly empty, with a tenure rather than a fall:\n"
	" *\n"
	" *   a < 0.06            ARRIVING.  It drops the last of the way in and\n"
	" *                       settles, decelerating, still turning a little.\n"
	" *   0.06 .. release     STUCK.  Flat, still, shivering with the wind.\n"
	" *   a > release         GOING.  It peels from one edge, pivots about\n"
	" *                       its stem and lifts away.\n"
	" *\n"
	" * WHEN IT GOES IS DECIDED BY THE WIND, and that is the point of the\n"
	" * layer.  Each leaf has its own adhesion, hashed per tenure; the\n"
	" * release point is that adhesion pulled EARLIER by however hard it is\n"
	" * blowing at this instant.  Since every leaf reads the same gust, a\n"
	" * gust takes several of them at once and calm air takes almost none\n"
	" * -- which is a correlated departure with no state anywhere, and it\n"
	" * is the difference between wind and noise.\n"
	" *\n"
	" * HOW FAR IT MAY GO IS FIXED BY THE LOOKUP.  See LEAF_FLY: the exit\n"
	" * is mostly rotation and scale because that is what fits in what the\n"
	" * nine-cell neighbourhood has left over.\n"
	" */\n"
	"void stuck_layer(vec2 p, float cell, float density, float ang, float lyr,\n"
	"                 float size, float depth,\n"
	"                 inout vec4 best, inout vec4 local, inout float shade) {\n"
	"  if (density <= 0.0 || cell <= 0.0)\n"
	"    return;\n"
	"\n"
	"  float c  = cos(ang);\n"
	"  float s  = sin(ang);\n"
	"  mat2  R  = mat2(c, -s, s, c);\n"
	"  mat2  Rt = mat2(c, s, -s, c);\n"
	"  vec2  off  = vec2(u_seed * 0.43, u_seed * 0.67);\n"
	"  float lid  = lyr * 64.0 + u_seed;\n"
	"  vec2  q    = (R * p) / cell + off;\n"
	"  vec2  base = floor(q);\n"
	"  int   i, j;\n"
	"\n"
	"  for (j = -1; j <= 1; j++) {\n"
	"    for (i = -1; i <= 1; i++) {\n"
	"      vec2  id = base + vec2(float(i), float(j));\n"
	"      /* Permanent, and only the phase: which tenure this cell is on\n"
	"         is worked out FROM it, so it cannot depend on the tenure. */\n"
	"      vec3  hc = hash3v(vec3(id.x, id.y, lid));\n"
	"      float u  = u_stick_t + hc.x;\n"
	"      float a  = fract(u);\n"
	"      float li = mod(floor(u), LEAF_CYCLES);\n"
	"      vec3  hr = hash3v(vec3(id.x + 31.0 + lid, id.y - 17.0, li));\n"
	"\n"
	"      if (hr.z > density)\n"
	"        continue;\n"
	"\n"
	"      vec3  hs  = hash3v(vec3(id.x - 9.0, id.y + 47.0 + lid, li));\n"
	"      /* Its own grip, and how far the wind has to get to beat it. */\n"
	"      float hold = 0.52 + 0.40 * hs.z;\n"
	"      float rel  = clamp(hold - u_gust * 0.42, 0.10, 0.99);\n"
	"      if (a > rel + 0.14)\n"
	"        continue;\n"
	"\n"
	"      float land = smoothstep(0.0, 0.06, a);\n"
	"      float go   = clamp((a - rel) / 0.14, 0.0, 1.0);\n"
	"      float r    = size * (0.72 + 0.56 * hr.x);\n"
	"      /* Centre, jittered over the WHOLE cell -- which is what makes a\n"
	"         sparse lattice read as a scatter, and it fits: the blade\n"
	"         (0.44) plus LEAF_FLY (0.30) is 0.74 of the 1.0 cells the\n"
	"         nine-cell lookup guarantees. */\n"
	"      vec2  cen  = (id + vec2(hr.y, hc.y) - off) * cell;\n"
	"\n"
	"      cen = Rt * cen;\n"
	"      /* Arriving: the last of the drop, easing in from above. */\n"
	"      cen.y -= (1.0 - land) * (1.0 - land) * cell * 0.5;\n"
	"      /*\n"
	"       * SHIVERING.  Every leaf on the glass reads the same u_sway and\n"
	"       * the same u_gust, offset only by its own phase, so when it\n"
	"       * blows they all move together.  Small -- a stuck leaf is stuck\n"
	"       * -- but the correlation is what the eye picks up.\n"
	"       */\n"
	"      cen += vec2(sin(u_sway + hc.z * TAU), cos(u_sway * 0.7 + hr.y * TAU))\n"
	"             * u_gust * cell * 0.022;\n"
	"      /* Going: thrown downwind and up, accelerating. */\n"
	"      cen += vec2(sign(u_wind + 0.001), -0.55)\n"
	"             * go * go * cell * LEAF_FLY;\n"
	"\n"
	"      /*\n"
	"       * THE PEEL.  A leaf on wet glass does not slide off, it lifts\n"
	"       * at one edge and pivots about its stem.  Modelled as a fast\n"
	"       * turn plus a squash that runs from flat-on towards edge-on:\n"
	"       * once it is off the glass it is tumbling again, and the last\n"
	"       * thing seen of it is a line.\n"
	"       */\n"
	"      float rot    = hr.y * TAU + hc.z * 0.6\n"
	"                     /* The lift itself is a turn about the stem. */\n"
	"                     + go * go * 2.4 * (hs.x < 0.5 ? -1.0 : 1.0)\n"
	"                     /* And a shiver in the wind while it still holds. */\n"
	"                     + sin(u_sway * 1.3 + hc.x * TAU) * u_gust * 0.09;\n"
	"      float squash = mix(1.0, abs(cos(go * 2.2)), go);\n"
	"      /* Arriving leaves are still turning over; landed ones are not. */\n"
	"      squash = mix(abs(cos((1.0 - land) * 3.0)), squash, land);\n"
	"\n"
	"      leaf_at(p, cen, r * (1.0 + go * 0.12), rot, squash,\n"
	"              hr.y < 0.5 ? -1.0 : 1.0,\n"
	"              land * (1.0 - go * go),\n"
	"              /* On the glass, so in front of everything falling past\n"
	"                 outside it, and a leaf in the act of peeling off is\n"
	"                 nearer still. */\n"
	"              hr, depth + go, best, local, shade);\n"
	"    }\n"
	"  }\n"
	"}\n"
	"\n"
	"/* One tap of the wallpaper, @fog of the way towards hazed. */\n"
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
	"  /* (coverage, which face, depth, colour index) and the leaf-local\n"
	"     point that goes with it. */\n"
	"  vec4  best  = vec4(0.0);\n"
	"  vec4  local = vec4(0.0);\n"
	"  float shade = 0.0;\n"
	"\n"
	"  /*\n"
	"   * Three depths of falling leaves.  The far ones are smaller, run on\n"
	"   * a different clock and are hazed towards the background below --\n"
	"   * which is the only parallax available to a single flat pass, and\n"
	"   * it is enough: the eye reads size-with-blur as distance long before\n"
	"   * it reads differential motion.\n"
	"   */\n"
	"  fall_layer(p, u_column * 1.90, u_fall.z, u_falling * 0.85, 0.0,\n"
	"             u_leaf * 0.55, 0.0, best, local, shade);\n"
	"  fall_layer(p, u_column * 1.37, u_fall.y, u_falling * 0.95, 1.0,\n"
	"             u_leaf * 0.78, 0.4, best, local, shade);\n"
	"  fall_layer(p, u_column,        u_fall.x, u_falling,        2.0,\n"
	"             u_leaf,        0.8, best, local, shade);\n"
	"\n"
	"  /* And the ones already on the glass, in front of all of it.  Two\n"
	"     turned lattices at an incommensurate pitch, for the reason the\n"
	"     rain gives: a rotated grid is still a grid. */\n"
	"  stuck_layer(p, u_cell,        u_stuck,       0.00, 4.0,\n"
	"              u_leaf * 0.95, 3.0, best, local, shade);\n"
	"  stuck_layer(p, u_cell * 1.71, u_stuck * 0.5, 0.83, 5.0,\n"
	"              u_leaf * 1.15, 3.6, best, local, shade);\n"
	"\n"
	"  /*\n"
	"   * THE GLASS FIRST, then whatever is on it.\n"
	"   *\n"
	"   * A stuck leaf holds a film of water under it and around its rim,\n"
	"   * which is the one place this effect gets to borrow the rain's\n"
	"   * trick: the wet patch lifts the haze, so the pane is sharper where\n"
	"   * a leaf is than where it is not.\n"
	"   */\n"
	"  float wet   = clamp(shade * u_clarity, 0.0, 1.0);\n"
	"  float fog   = u_fog * (1.0 - wet);\n"
	"  vec3  col   = tap(p, fog);\n"
	"\n"
	"  /*\n"
	"   * THE CONTACT SHADOW.\n"
	"   *\n"
	"   * Offset down and away from the light, and soft: what is being cast\n"
	"   * is not a shadow on a floor but the darkening of glass under\n"
	"   * something lying on it.  Sampled from the accumulated coverage\n"
	"   * rather than re-walking the layers, which is why leaf_at() records\n"
	"   * @shade for every leaf and not only the winner.\n"
	"   */\n"
	"  if (u_shadow > 0.001 && shade > 0.0)\n"
	"    col *= 1.0 - shade * u_shadow * 0.55;\n"
	"\n"
	"  if (best.x > 0.0) {\n"
	"    vec2  q    = local.xy;\n"
	"    float r    = local.z;\n"
	"    float lq   = length(q);\n"
	"    float aa   = max(0.02, 1.4 / max(r, 1.0));\n"
	"    /*\n"
	"     * THE COLOUR.  Three tints blended by one hashed number, so a\n"
	"     * fall runs from just-turned red through gold to the browns of\n"
	"     * leaves that have been down a while, and no two neighbours are\n"
	"     * the same.  Blending two ramps rather than picking from a set\n"
	"     * keeps the family related -- a window of unrelated colours looks\n"
	"     * like confetti.\n"
	"     */\n"
	"    float k    = best.w;\n"
	"    vec3  leafc = k < 0.5\n"
	"                  ? mix(u_tint_warm, u_tint_gold, k * 2.0)\n"
	"                  : mix(u_tint_gold, u_tint_dry, (k - 0.5) * 2.0);\n"
	"\n"
	"    /* The underside is paler and duller, so the tumble is legible as\n"
	"       a turn rather than only as a narrowing. */\n"
	"    if (best.y < 0.0)\n"
	"      leafc = mix(leafc, vec3(dot(leafc, vec3(0.33))), 0.42) * 1.16;\n"
	"\n"
	"    /*\n"
	"     * BACKLIT, which is the whole of why this works.\n"
	"     *\n"
	"     * The wallpaper behind the leaf is not replaced, it is FILTERED:\n"
	"     * multiplied by the leaf's colour and dimmed.  A leaf drawn as\n"
	"     * flat paint over the desktop is a sticker; one that lets the\n"
	"     * picture through, coloured, is a leaf on a window with the day\n"
	"     * behind it.\n"
	"     */\n"
	"    /* The wallpaper through a leaf is DIFFUSED, not merely tinted: a\n"
	"       blade is a stack of cells and what comes out the other side has\n"
	"       lost its detail.  Sampling the sharp picture here -- which is\n"
	"       the obvious thing to write -- prints the desktop on the leaf and\n"
	"       turns it into a decal, which is exactly how it looked. */\n"
	"    vec3 through = tap(p, mix(fog, 1.0, 0.94)) * leafc\n"
	"                   * (0.55 + 0.75 * u_translucency);\n"
	"    vec3 body    = mix(leafc * 0.85, through, u_translucency);\n"
	"\n"
	"    /* The veins, drawn as absorption: thicker tissue passes less. */\n"
	"    if (u_veins > 0.001) {\n"
	"      float v = leaf_veins(q, 5.0 + 4.0 * local.w, aa);\n"
	"      body *= 1.0 - v * u_veins * 0.55;\n"
	"    }\n"
	"\n"
	"    /*\n"
	"     * THE CURL.  A dried leaf lifts at the rim, so the rim catches\n"
	"     * more light than the middle and the middle keeps the contact.\n"
	"     * One smoothstep on the local radius, which is all the geometry\n"
	"     * anybody can see at this size.\n"
	"     */\n"
	"    if (u_curl > 0.001) {\n"
	"      float lift = smoothstep(0.45, 1.0, lq);\n"
	"      body *= 1.0 + lift * u_curl * 0.42;\n"
	"      body *= 1.0 - (1.0 - lift) * u_curl * 0.10;\n"
	"    }\n"
	"\n"
	"    /* And a darker edge where the blade turns away from us, which is\n"
	"       what stops a leaf looking cut out of paper. */\n"
	"    body *= 1.0 - smoothstep(0.86, 1.02, lq) * 0.28;\n"
	"\n"
	"    /*\n"
	"     * WET LEAVES ARE SHINY.  A broad, soft highlight -- a leaf is not\n"
	"     * a sphere, so the normal is nearly flat and the glint is a wash\n"
	"     * across the upwind half rather than a point.\n"
	"     */\n"
	"    if (u_gloss > 0.001) {\n"
	"      vec3  n  = normalize(vec3(q * 0.35, 1.0));\n"
	"      vec3  hv = normalize(u_light + vec3(0.0, 0.0, 1.0));\n"
	"      float sp = pow(max(dot(n, hv), 0.0), max(u_shine, 1.0));\n"
	"      body += vec3(sp * u_gloss);\n"
	"    }\n"
	"\n"
	"    /*\n"
	"     * AERIAL PERSPECTIVE.  A leaf three trees away is not merely\n"
	"     * smaller, it is LOWER IN CONTRAST: there is air in front of it.\n"
	"     * Sinking the far layers towards the glass they are seen against\n"
	"     * is the whole of the depth cue available to one flat pass, and\n"
	"     * the eye reads it as distance long before it reads differential\n"
	"     * motion.\n"
	"     */\n"
	"    float near = clamp(best.z, 0.0, 1.0);\n"
	"    col = mix(col, body, best.x * (0.82 + 0.18 * near));\n"
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
leaf_prog_ensure(GowlFxGl *self)
{
	GowlFxLeafProg *p = &self->leaf;

	if (p->program != 0)
		return TRUE;
	if (self->leaf_tried)
		return FALSE;
	self->leaf_tried = TRUE;

	p->program = gowl_fx_link_program(leaf_vert_src, leaf_frag_src);
	if (p->program == 0) {
		g_warning("fx: the falling-leaves shader would not build, so leaf "
		          "backdrops will sit this session out");
		return FALSE;
	}

	p->u_soft         = glGetUniformLocation(p->program, "u_soft");
	p->u_sharp        = glGetUniformLocation(p->program, "u_sharp");
	p->u_src_origin   = glGetUniformLocation(p->program, "u_src_origin");
	p->u_src_size     = glGetUniformLocation(p->program, "u_src_size");
	p->u_src_scale    = glGetUniformLocation(p->program, "u_src_scale");
	p->u_size         = glGetUniformLocation(p->program, "u_size");
	p->u_radius       = glGetUniformLocation(p->program, "u_radius");
	p->u_stick_t      = glGetUniformLocation(p->program, "u_stick_t");
	p->u_fall         = glGetUniformLocation(p->program, "u_fall");
	p->u_gust         = glGetUniformLocation(p->program, "u_gust");
	p->u_sway         = glGetUniformLocation(p->program, "u_sway");
	p->u_leaf         = glGetUniformLocation(p->program, "u_leaf");
	p->u_cell         = glGetUniformLocation(p->program, "u_cell");
	p->u_stuck        = glGetUniformLocation(p->program, "u_stuck");
	p->u_column       = glGetUniformLocation(p->program, "u_column");
	p->u_falling      = glGetUniformLocation(p->program, "u_falling");
	p->u_flutter      = glGetUniformLocation(p->program, "u_flutter");
	p->u_tumble       = glGetUniformLocation(p->program, "u_tumble");
	p->u_wind         = glGetUniformLocation(p->program, "u_wind");
	p->u_gust_push    = glGetUniformLocation(p->program, "u_gust_push");
	p->u_curl         = glGetUniformLocation(p->program, "u_curl");
	p->u_veins        = glGetUniformLocation(p->program, "u_veins");
	p->u_translucency = glGetUniformLocation(p->program, "u_translucency");
	p->u_gloss        = glGetUniformLocation(p->program, "u_gloss");
	p->u_shadow       = glGetUniformLocation(p->program, "u_shadow");
	p->u_fog          = glGetUniformLocation(p->program, "u_fog");
	p->u_clarity      = glGetUniformLocation(p->program, "u_clarity");
	p->u_shine        = glGetUniformLocation(p->program, "u_shine");
	p->u_tint_warm    = glGetUniformLocation(p->program, "u_tint_warm");
	p->u_tint_gold    = glGetUniformLocation(p->program, "u_tint_gold");
	p->u_tint_dry     = glGetUniformLocation(p->program, "u_tint_dry");
	p->u_light        = glGetUniformLocation(p->program, "u_light");
	p->u_brightness   = glGetUniformLocation(p->program, "u_brightness");
	p->u_alpha        = glGetUniformLocation(p->program, "u_alpha");
	p->u_seed         = glGetUniformLocation(p->program, "u_seed");
	p->a_pos          = glGetAttribLocation(p->program, "a_pos");
	p->a_uv           = glGetAttribLocation(p->program, "a_uv");
	return TRUE;
}

void
gowl_fx_leaf_params_init(GowlFxLeafParams *params)
{
	if (params == NULL)
		return;

	memset(params, 0, sizeof(*params));
	/*
	 * A steady fall on a breezy day: the middle preset's numbers, so a
	 * caller that sets only a size gets autumn rather than a clean pane.
	 *
	 * `leaf' is the ruler and everything else is measured against it.
	 * 110 px is a blade a little over two hundred across on a HiDPI
	 * panel, which is about what a maple looks like against a window from
	 * a few feet away.  The first attempt at this was a third of that and
	 * the mistake is the rain's exactly: small enough and it stops being
	 * leaves and becomes TEXTURE.
	 *
	 * The columns are a good deal wider than the leaves, on purpose:
	 * leaves fall sparsely and a window full of them at once is a
	 * compost heap, not a tree.  But the first set of numbers overdid
	 * that and put TWO leaves on a window nine hundred pixels across,
	 * which reads as a bug rather than as restraint.  What a column
	 * buys is the guarantee that leaves do not pile into each other;
	 * what it costs is that the blade can be no more than a third of
	 * one, so more leaves means more columns rather than a fuller one.
	 *
	 * And the count has to be read with the TUMBLE in mind: a leaf spends
	 * a fair part of every fall edge-on, where it is a line and not a
	 * leaf.  Eight objects on screen is five or six visible ones, which
	 * is the number that was actually being counted when these went up.
	 */
	params->leaf         = 76.0f;
	params->cell         = 300.0f;
	params->stuck        = 0.48f;
	params->column       = 240.0f;
	params->falling      = 0.85f;
	params->flutter      = 0.30f;
	params->tumble       = 2.4f;
	params->wind         = 90.0f;
	params->gust_push    = 220.0f;
	params->curl         = 0.35f;
	params->veins        = 0.55f;
	params->translucency = 0.62f;
	params->gloss        = 0.16f;
	params->shadow       = 0.45f;
	params->fog          = 0.22f;
	params->clarity      = 0.70f;
	params->shine        = 18.0f;
	params->brightness   = 1.0f;
	params->alpha        = 1.0f;
	params->src_scale    = 1.0f;
	/* The three points of the autumn ramp.  Saturated, because they are
	 * MULTIPLIED into the wallpaper rather than painted over it -- a
	 * filter loses saturation, so a tint that looks right as a swatch
	 * comes out as mud. */
	params->tint_warm[0] = 0.86f;
	params->tint_warm[1] = 0.22f;
	params->tint_warm[2] = 0.13f;
	params->tint_gold[0] = 0.95f;
	params->tint_gold[1] = 0.62f;
	params->tint_gold[2] = 0.14f;
	params->tint_dry[0]  = 0.64f;
	params->tint_dry[1]  = 0.39f;
	params->tint_dry[2]  = 0.17f;
	params->light[0] = -0.42f;
	params->light[1] = -0.55f;
	params->light[2] =  0.72f;
}

void
gowl_fx_leaf_advance(GowlFxLeafClock *clock, gdouble dt, gdouble speed,
                     gdouble tenure_seconds, gdouble gustiness)
{
	/*
	 * The three fall rates, irrational against each other for the reason
	 * gowl_fx_rain_advance() sets out at length: rates that look
	 * incommensurate and are really k/100 all return to zero together
	 * every hundred seconds, and the whole field repeats exactly.
	 *
	 * An eighth, rather than the rain's fifth: a leaf takes a good deal
	 * longer to come down than a raindrop takes to run down a pane, and
	 * that unhurriedness is most of what makes it read as a leaf.
	 */
	static const gdouble rate[3] = {
		0.125, 0.0772542485937369, 0.0477457514062631
	};
	/*
	 * THE WIND, and it is the reason this advance is not the rain's.
	 *
	 * Three phases in radians, at rates that are irrational against each
	 * other so the wind never repeats, and ONE of them carries the whole
	 * swing while the other two only modulate it.  That asymmetry is
	 * deliberate: a SUM of three sines is a poor gust, because three
	 * things that rarely agree spend nearly all their time cancelling --
	 * the first version of this was exactly that, and it produced a wind
	 * that sat at a quarter strength forever and never once got up.
	 *
	 * The squaring at the end is the other half.  A sine, however
	 * modulated, spends half its life above the middle of its range,
	 * which is a fan; squared, it spends most of its life near calm and
	 * occasionally gets up, which is weather.
	 *
	 * Kept on the CPU in doubles because the whole point is that it is
	 * ONE number every leaf reads; worked out per pixel it would be the
	 * same number, but worked out per layer it would not, and a wind that
	 * differs between layers is not a wind.
	 *
	 * Periods of about eighteen, twenty-eight and forty-six seconds: a
	 * gust arrives every half minute or so, which is what a breezy
	 * afternoon does.
	 */
	static const gdouble breeze_rate[3] = { 0.349, 0.2157, 0.1333 };
	gdouble m[3], w;
	gint    i;

	if (clock == NULL)
		return;
	if (!(dt > 0.0))
		return;
	if (dt > 0.25)
		dt = 0.25;   /* a long stall is not a long afternoon */
	if (!(tenure_seconds > 0.1))
		tenure_seconds = 26.0;

	clock->stick += dt / tenure_seconds;
	clock->stick = fmod(clock->stick, GOWL_FX_LEAF_CYCLES);
	if (clock->stick < 0.0)
		clock->stick += GOWL_FX_LEAF_CYCLES;

	for (i = 0; i < 3; i++) {
		clock->fall[i] += dt * speed * rate[i];
		clock->fall[i] = fmod(clock->fall[i], GOWL_FX_LEAF_CYCLES);
		if (clock->fall[i] < 0.0)
			clock->fall[i] += GOWL_FX_LEAF_CYCLES;
	}

	/* The shiver everything on the glass moves to, a good deal faster
	 * than the gust itself: a stuck leaf trembles in the wind rather than
	 * swaying with it.  Wrapped at a turn, which a float holds exactly
	 * for as long as a session lasts. */
	clock->sway += dt * 2.6;
	clock->sway = fmod(clock->sway, 2.0 * G_PI);

	/* Each phase wrapped on its OWN turn.  Deriving all three from one
	 * accumulator and scaling it -- which is the obvious economy -- makes
	 * the scaled values jump at every wrap, because they do not reach a
	 * whole turn together. */
	for (i = 0; i < 3; i++) {
		clock->breeze[i] += dt * breeze_rate[i];
		clock->breeze[i] = fmod(clock->breeze[i], 2.0 * G_PI);
		m[i] = 0.5 + 0.5 * sin(clock->breeze[i]);
	}

	/* The first sine is the gust; the other two only decide how strong
	 * this one turns out to be.  Their floors are what stop a gust from
	 * being cancelled away entirely -- the point of them is that no two
	 * gusts are the same size, not that some gusts do not happen. */
	w = m[0] * (0.62 + 0.38 * m[1]) * (0.80 + 0.20 * m[2]);
	w = w * w;                           /* calm mostly, strong briefly */
	clock->gust = CLAMP(w * CLAMP(gustiness, 0.0, 2.0), 0.0, 1.0);
}

gboolean
gowl_fx_pass_leaves(GowlFxPass             *pass,
                    const GowlFxTexture    *soft,
                    const GowlFxTexture    *sharp,
                    const GowlFxLeafParams *params,
                    const GowlFxLeafClock  *clock)
{
	GowlFxGl             *gl;
	const GowlFxLeafProg *p;
	const GowlFxTexture  *clear_src;
	GowlFxLeafClock       still;
	gfloat                fall[3];
	gfloat                radius;
	gfloat                cell, column, leaf;
	gint                  i;

	if (pass == NULL || params == NULL || soft == NULL)
		return FALSE;
	if (params->width <= 0 || params->height <= 0)
		return FALSE;
	if (soft->tex == 0 || soft->width <= 0 || soft->height <= 0)
		return FALSE;

	gl = pass->gl;
	if (!leaf_prog_ensure(gl))
		return FALSE;
	p = &gl->leaf;

	if (clock == NULL) {
		memset(&still, 0, sizeof(still));
		clock = &still;
	}
	for (i = 0; i < 3; i++)
		fall[i] = (gfloat)clock->fall[i];

	radius = CLAMP(params->radius, 0.0f,
	               (gfloat)MIN(params->width, params->height) * 0.5f);
	cell   = MAX(16.0f, params->cell);
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
	glUniform1f(p->u_stick_t, (gfloat)clock->stick);
	glUniform3fv(p->u_fall, 1, fall);
	glUniform1f(p->u_gust, (gfloat)CLAMP(clock->gust, 0.0, 1.0));
	glUniform1f(p->u_sway, (gfloat)clock->sway);
	/*
	 * THE LEAF SIZE IS CAPPED AGAINST BOTH LAYOUTS, and this is load
	 * bearing: the three-column reach in fall_layer() and the nine-cell
	 * reach in stuck_layer() are both worked out with a blade of at most
	 * 0.42 of a column and 0.44 of a cell in them.  A config that asked
	 * for leaves the size of the spacing would have them sliced off along
	 * straight lines at the layer boundaries -- a lattice, drawn in the
	 * negative -- so the cap is applied here rather than trusted to
	 * whoever wrote the preset.
	 *
	 * The 1.30 is the largest per-leaf size jitter either layer applies.
	 */
	leaf = MAX(4.0f, params->leaf);
	leaf = MIN(leaf, cell * (0.44f / 1.30f));
	leaf = MIN(leaf, column * (0.42f / 1.30f));

	glUniform1f(p->u_leaf, leaf);
	glUniform1f(p->u_cell, cell);
	glUniform1f(p->u_stuck, CLAMP(params->stuck, 0.0f, 1.0f));
	glUniform1f(p->u_column, column);
	glUniform1f(p->u_falling, CLAMP(params->falling, 0.0f, 1.0f));
	/* Capped at 0.45 of a column: the three-column reach in fall_layer()
	 * is worked out with that number in it. */
	glUniform1f(p->u_flutter, CLAMP(params->flutter, 0.0f, 0.45f));
	glUniform1f(p->u_tumble, CLAMP(params->tumble, 0.0f, 12.0f));
	glUniform1f(p->u_wind, params->wind);
	glUniform1f(p->u_gust_push, params->gust_push);
	glUniform1f(p->u_curl, CLAMP(params->curl, 0.0f, 1.0f));
	glUniform1f(p->u_veins, CLAMP(params->veins, 0.0f, 1.0f));
	glUniform1f(p->u_translucency,
	            CLAMP(params->translucency, 0.0f, 1.0f));
	glUniform1f(p->u_gloss, MAX(0.0f, params->gloss));
	glUniform1f(p->u_shadow, CLAMP(params->shadow, 0.0f, 1.0f));
	glUniform1f(p->u_fog, CLAMP(params->fog, 0.0f, 1.0f));
	glUniform1f(p->u_clarity, CLAMP(params->clarity, 0.0f, 1.0f));
	glUniform1f(p->u_shine, MAX(1.0f, params->shine));
	glUniform3fv(p->u_tint_warm, 1, params->tint_warm);
	glUniform3fv(p->u_tint_gold, 1, params->tint_gold);
	glUniform3fv(p->u_tint_dry, 1, params->tint_dry);
	glUniform3fv(p->u_light, 1, params->light);
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
