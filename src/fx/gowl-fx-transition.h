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

#ifndef GOWL_FX_TRANSITION_H
#define GOWL_FX_TRANSITION_H

/*
 * The shared machinery of a tag transition, so a transition module is a
 * drawing function and nothing else.
 *
 * The cube grew all of this for itself -- noticing a tag switch, asking
 * whether it is the one to animate it, capturing the desktops on either
 * side, parking a sheet over the output, keeping the clock, swallowing
 * the window fade the switch would otherwise start, and taking it all
 * down again when an output or the renderer goes.  Every other
 * transition needs exactly the same, so it lives here once.  A module
 * embeds a #GowlFxTransitionHost, forwards its scene-effect hooks to it,
 * and supplies a #GowlFxTransitionKind whose draw() paints one frame.
 *
 * GL-free like every module: drawing goes through the pass the host
 * hands draw(), and the GL lives in the gowl_fx_pass_* functions.
 */

#include <glib.h>

#include "fx/gowl-fx.h"
#include "interfaces/gowl-scene-effect.h"

G_BEGIN_DECLS

/**
 * GowlFxTransitionRun:
 * @monitor: the output (unowned; runs end with it)
 * @from_tags: the tag set being left
 * @to_tags: the tag set arrived at
 * @from_index: the lowest tag of @from_tags, from 0
 * @to_index: likewise for @to_tags
 * @dir: +1 when moving to a higher tag, -1 to a lower one
 * @steps: how many single-tag steps the journey crosses, at least 1
 * @from: the desktop being left, at full size
 * @to: the desktop arrived at, at full size
 * @chain: (array length=n_chain) (nullable): for a kind that asked for
 *   multi_step, the desktops crossed on the way, at half size; read them
 *   through gowl_fx_transition_run_desktop(), which also gives the ends
 * @n_chain: @steps + 1 when @chain is set, else 0
 * @width: the output's width in buffer pixels
 * @height: likewise its height
 * @start_us: when the run began, monotonic microseconds
 * @dur_us: how long it lasts
 * @center: where the pointer is on the output, 0..1 with v down; the
 *   output's centre when the pointer is elsewhere
 * @seed: a random number for this switch, so noise differs each time
 * @state: the kind's own per-run data, from setup()
 *
 * One transition in flight on one output.
 */
typedef struct {
	GowlMonitor   *monitor;
	guint32        from_tags;
	guint32        to_tags;
	gint           from_index;
	gint           to_index;
	gint           dir;
	gint           steps;
	GowlFxTexture  from;
	GowlFxTexture  to;
	GowlFxTexture *chain;
	gint           n_chain;
	gint           width;
	gint           height;
	gint64         start_us;
	gint64         dur_us;
	gfloat         center[2];
	guint32        seed;
	gpointer       state;

	/*< private >*/
	gpointer       sheet;
} GowlFxTransitionRun;

/**
 * GowlFxTransitionKind:
 * @name: the name `tag-transition' selects it by ("slide")
 * @label: what the toast calls it ("Slide")
 * @duration_scale: its length as a multiple of `tag-transition-duration'
 *   -- a page turn reads better slower than a fade
 * @step_scale: for @multi_step, the extra length per tag crossed beyond
 *   the first, as a multiple of `tag-transition-duration'
 * @multi_step: capture every desktop crossed (@chain), not just the ends
 * @setup: (nullable): make the run's @state once its desktops are
 *   captured; %FALSE declines the run (the switch is then instant)
 * @teardown: (nullable): free @state
 * @draw: paint one frame into @pass, which covers the whole output.
 *   @eased is the progress after `tag-transition-curve', @linear before
 *   it; at 0 and 1 the picture must BE the desktop being left and the
 *   one arrived at, or the switch flashes.  %FALSE ends the run
 *
 * A transition.  Static: the host keeps a pointer to it.
 */
typedef struct {
	const gchar *name;
	const gchar *label;
	gdouble      duration_scale;
	gdouble      step_scale;
	gboolean     multi_step;
	gboolean   (*setup)    (GowlFxTransitionRun *run);
	void       (*teardown) (GowlFxTransitionRun *run);
	gboolean   (*draw)     (GowlFxPass          *pass,
	                        GowlFxTransitionRun *run,
	                        gdouble              eased,
	                        gdouble              linear);
} GowlFxTransitionKind;

typedef struct _GowlFxTransitionHost GowlFxTransitionHost;

/**
 * gowl_fx_transition_host_new:
 * @kind: the transition this host runs; must outlive it
 *
 * Returns: (transfer full): a host
 */
GowlFxTransitionHost *gowl_fx_transition_host_new (const GowlFxTransitionKind *kind);

/**
 * gowl_fx_transition_host_free:
 * @host: (nullable): a host
 *
 * Ends every run, frees the GL, withdraws the kind from the compositor.
 */
void gowl_fx_transition_host_free (GowlFxTransitionHost *host);

/**
 * gowl_fx_transition_host_attach:
 * @host: a host
 * @compositor: the compositor
 *
 * Registers the kind with @compositor, so `tag-transition' and Super+:
 * know it exists.  Idempotent and cheap: call it from every hook that
 * might be the first one to see the compositor.
 */
void gowl_fx_transition_host_attach (GowlFxTransitionHost *host,
                                     GowlCompositor       *compositor);

/**
 * gowl_fx_transition_host_detach:
 * @host: a host
 *
 * Ends every run, frees the GL and withdraws the kind -- the module is
 * being deactivated.
 */
void gowl_fx_transition_host_detach (GowlFxTransitionHost *host);

/**
 * gowl_fx_transition_host_frame:
 * @host: a host
 * @compositor: the compositor
 * @monitor: the output drawing a frame
 * @now: monotonic microseconds
 *
 * The scene effect's frame hook: notices a switch, starts, draws and
 * ends runs.
 *
 * Returns: %TRUE while it is animating @monitor
 */
gboolean gowl_fx_transition_host_frame (GowlFxTransitionHost *host,
                                        GowlCompositor       *compositor,
                                        GowlMonitor          *monitor,
                                        gint64                now);

/**
 * gowl_fx_transition_host_client_event:
 * @host: a host
 * @compositor: the compositor
 * @client: the window
 * @event: the event
 *
 * The scene effect's client_event hook.  Claims ONLY the reveal of a
 * window on an output this transition is about to animate or is
 * animating: the transition's first frame already shows that window, and
 * a fade under it would finish after it had landed.  Everything else is
 * left to the other effect modules (window animation first).
 *
 * Returns: %TRUE when the event is claimed
 */
gboolean gowl_fx_transition_host_client_event (GowlFxTransitionHost *host,
                                               GowlCompositor       *compositor,
                                               GowlClient           *client,
                                               GowlSceneEffectEvent  event);

/**
 * gowl_fx_transition_host_monitor_removed:
 * @host: a host
 * @compositor: the compositor
 * @monitor: the output going
 */
void gowl_fx_transition_host_monitor_removed (GowlFxTransitionHost *host,
                                              GowlCompositor       *compositor,
                                              GowlMonitor          *monitor);

/**
 * gowl_fx_transition_host_finish:
 * @host: a host
 *
 * The renderer is going: end every run and free the GL while it still
 * can be.  The kind stays registered; a new renderer starts afresh.
 */
void gowl_fx_transition_host_finish (GowlFxTransitionHost *host);

/**
 * gowl_fx_transition_host_running:
 * @host: a host
 * @monitor: an output
 *
 * Returns: (transfer none) (nullable): the run on @monitor, for tests
 */
GowlFxTransitionRun *gowl_fx_transition_host_running (GowlFxTransitionHost *host,
                                                      GowlMonitor          *monitor);

/**
 * gowl_fx_transition_run_desktop:
 * @run: a run
 * @j: a step along the journey, 0 being the desktop left and @run->steps
 *   the one arrived at
 *
 * Returns: (transfer none): the desktop at step @j; the full-size ends
 *   for 0 and @steps, a half-size intermediate in between (the nearer
 *   end when one could not be captured)
 */
const GowlFxTexture *gowl_fx_transition_run_desktop (const GowlFxTransitionRun *run,
                                                     gint                       j);

/**
 * gowl_fx_transition_tag_index:
 * @tags: a tag set
 *
 * Returns: the lowest tag in @tags, from 0, or -1 for none
 */
gint gowl_fx_transition_tag_index (guint32 tags);

/**
 * gowl_fx_transition_pass_desktop:
 * @pass: a pass
 * @texture: a desktop
 * @mvp: (nullable) (array fixed-size=16): where to put it; %NULL fills
 *   the output exactly
 * @pos: (nullable) (array fixed-size=12): its four corners as xyz in
 *   strip order (top-left, bottom-left, top-right, bottom-right); %NULL
 *   for the output's own rectangle in pixels, (0,0) to (width,height) --
 *   which is what gowl_fx_mat4_ortho() maps back onto the output
 * @alpha: its opacity
 * @shade: multiplied into its colour (1 for none): a desktop turning
 *   away from the light, or pushed into the distance
 *
 * Draws one whole desktop as a flat quad: what most transitions are made
 * of.  A wrapper over gowl_fx_pass_quad() with the neutral style.
 */
void gowl_fx_transition_pass_desktop (GowlFxPass          *pass,
                                      const GowlFxTexture *texture,
                                      const gfloat        *mvp,
                                      const gfloat        *pos,
                                      gfloat               alpha,
                                      gfloat               shade);

/* ── Shards ──────────────────────────────────────────────────────── */

/**
 * GowlFxShard:
 * @x: the three corners' x, in output pixels
 * @y: the three corners' y, in output pixels, down
 * @cx: the centroid's x
 * @cy: the centroid's y
 * @r: four random numbers in [0, 1), the shard's own, for its spin, its
 *   speed, its delay -- whatever the transition wants to vary
 *
 * One triangle of a desktop broken into pieces.
 */
typedef struct {
	gfloat x[3];
	gfloat y[3];
	gfloat cx;
	gfloat cy;
	gfloat r[4];
} GowlFxShard;

/**
 * gowl_fx_shards_new:
 * @width: the output's width in pixels
 * @height: its height
 * @cols: grid columns
 * @rows: grid rows
 * @seed: makes each switch break differently
 * @n_out: (out): how many shards
 *
 * Breaks the output into triangles: a grid whose inner points are
 * jittered, each cell cut along a random diagonal.  Neighbouring shards
 * share their corners exactly, so at rest they cover the output with no
 * crack -- which is the seam for a transition built of them.
 *
 * Returns: (transfer full) (array length=n_out): the shards; g_free()
 */
GowlFxShard *gowl_fx_shards_new (gint     width,
                                 gint     height,
                                 gint     cols,
                                 gint     rows,
                                 guint32  seed,
                                 gint    *n_out);

/**
 * gowl_fx_shard_emit:
 * @dst: (out caller-allocates) (array fixed-size=21): three mesh vertices
 *   (#GOWL_FX_MESH_STRIDE floats each)
 * @shard: the shard
 * @width: the output's width
 * @height: its height
 * @offset: (array fixed-size=3): how far its centroid has moved, in world
 *   units (pixels, +Y up)
 * @axis: (array fixed-size=3): the unit axis it spins about, through its
 *   centroid
 * @angle: how far it has spun, radians
 * @scale: its size, 1 at rest
 * @shade: multiplied into its colour
 * @alpha: its opacity
 *
 * Places a shard in the world a transition draws in: the output centred
 * on the origin, +Y up, one unit a pixel (what
 * gowl_fx_mat4_perspective() and gowl_fx_mat4_view() at the distance that
 * fills the view face-on expect).  Texture coordinates are the shard's
 * own patch of the desktop, wherever it has flown.
 */
void gowl_fx_shard_emit (gfloat            *dst,
                         const GowlFxShard *shard,
                         gint               width,
                         gint               height,
                         const gfloat      *offset,
                         const gfloat      *axis,
                         gfloat             angle,
                         gfloat             scale,
                         gfloat             shade,
                         gfloat             alpha);

/**
 * GowlFxShardPlaceFunc:
 * @shard: the shard
 * @index: its index
 * @run: the run
 * @eased: progress after the curve
 * @linear: progress before it
 * @offset: (out caller-allocates) (array fixed-size=3): centroid motion
 * @axis: (out caller-allocates) (array fixed-size=3): unit spin axis
 * @angle: (out): spin, radians
 * @scale: (out): size, 1 at rest
 * @shade: (out): colour multiplier
 * @alpha: (out): opacity
 * @user_data: what was given to gowl_fx_transition_draw_shards()
 *
 * Where one shard of the desktop being left is on this frame.  The out
 * values start at rest (no offset, no spin, scale/shade/alpha 1).
 *
 * Returns: %FALSE to leave the shard out of this frame
 */
typedef gboolean (*GowlFxShardPlaceFunc) (const GowlFxShard         *shard,
                                          gint                       index,
                                          const GowlFxTransitionRun *run,
                                          gdouble                    eased,
                                          gdouble                    linear,
                                          gfloat                    *offset,
                                          gfloat                    *axis,
                                          gfloat                    *angle,
                                          gfloat                    *scale,
                                          gfloat                    *shade,
                                          gfloat                    *alpha,
                                          gpointer                   user_data);

/**
 * gowl_fx_transition_draw_shards:
 * @pass: a pass over the output
 * @run: the run; @run->from is what the shards show
 * @shards: (array length=n): the shards, from gowl_fx_shards_new()
 * @n: how many
 * @place: (scope call): where each one is this frame
 * @user_data: passed to @place
 * @eased: progress after the curve
 * @linear: progress before it
 * @to_scale: the desktop arrived at, behind them, scaled about the centre
 * @to_shade: and its colour multiplier
 *
 * One frame of a transition that breaks the desktop into pieces: the
 * camera at the distance that fills the view face-on, the new desktop
 * flat behind, then every shard @place keeps, furthest first, in one
 * mesh draw.  A shard that has flown behind the camera is left out.
 *
 * Returns: %TRUE when it drew
 */
gboolean gowl_fx_transition_draw_shards (GowlFxPass                *pass,
                                         const GowlFxTransitionRun *run,
                                         const GowlFxShard         *shards,
                                         gint                       n,
                                         GowlFxShardPlaceFunc       place,
                                         gpointer                   user_data,
                                         gdouble                    eased,
                                         gdouble                    linear,
                                         gfloat                     to_scale,
                                         gfloat                     to_shade);

G_END_DECLS

#endif /* GOWL_FX_TRANSITION_H */
