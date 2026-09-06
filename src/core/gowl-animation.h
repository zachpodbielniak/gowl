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

#ifndef GOWL_ANIMATION_H
#define GOWL_ANIMATION_H

#include <glib.h>

#include <wlr/util/box.h>

G_BEGIN_DECLS

typedef struct _GowlCompositor GowlCompositor;
typedef struct _GowlMonitor GowlMonitor;
typedef struct _GowlClient GowlClient;
typedef struct _GowlCloseAnim GowlCloseAnim;

/**
 * gowl_curve_eval:
 * @name: (nullable): a curve name, or %NULL for the default
 * @t: linear progress, 0.0 to 1.0
 *
 * Evaluates a named easing curve.
 *
 * The curves are cubic Béziers with the endpoints fixed at (0,0) and
 * (1,1), which is what both CSS and Hyprland use, so the two control
 * points are the whole definition and a Hyprland `bezier' line
 * translates directly.  Known names: `linear', `ease-out-quint',
 * `ease-in-out-cubic', `almost-linear', `quick', `ease-out-expo',
 * `ease-out-back' and `spring'.  An unknown name falls back to
 * `ease-out-expo'.
 *
 * The last two OVERSHOOT --- they return values above 1.0 partway
 * through, carrying a window past its target before it settles.  That
 * is deliberate and it is why they are not the default: in a tiling
 * layout windows are adjacent, so an overshoot briefly puts one on top
 * of its neighbour.  With gaps configured it looks great.
 *
 * Returns: eased progress, 0.0 to 1.0.
 */
gdouble gowl_curve_eval (const gchar *name, gdouble t);

/**
 * gowl_animation_start:
 * @self: a #GowlCompositor
 * @client: the client whose geometry is changing
 * @from: the rect the window occupies now
 * @to: the rect the layout wants it to occupy
 *
 * Begins morphing @client from one rect to the other --- position and
 * size together, because a tiling layout changes both and animating
 * only one of them looks broken: the window snaps to its final size in
 * a single frame and then spends the animation sliding, mis-sized
 * relative to where it is, for the whole duration.
 *
 * The client is configured once, to @to.  What actually gets resized
 * each frame is a locked snapshot of its last buffer, so the animation
 * costs no round trips --- see the `anim_ghost' comment in
 * gowl-core-private.h. A move also uses a snapshot for squash/stretch;
 * at zero jiggle strength, pure moves keep the window's live content.
 *
 * A client already animating is retargeted from wherever it currently
 * is rather than restarted from @from, so a second layout change
 * mid-flight bends the path instead of snapping back.
 */
void gowl_animation_start (GowlCompositor       *self,
                            GowlClient           *client,
                            const struct wlr_box *from,
                            const struct wlr_box *to);

/**
 * gowl_animation_cancel:
 * @client: the client
 *
 * Stops a geometry animation, releases the snapshot it was stretching
 * and puts the real surface back on screen.  Used when a client is
 * about to be destroyed or torn out of the scene, and whenever an
 * animation is superseded.
 */
void gowl_animation_cancel (GowlClient *client);

/* Settle after an interactive move/resize, using the grab's initial
 * rectangle to choose the direction. Never changes layout geometry. */
void gowl_animation_settle (GowlCompositor *self, GowlClient *client,
                             const struct wlr_box *grab);

/* Hit-test the live surface through a displayed geometry snapshot.
 * Input coordinates are layout-local; outputs are surface-local. */
struct wlr_surface *gowl_animation_surface_at (GowlClient *client,
                                               gdouble x, gdouble y,
                                               gdouble *sx, gdouble *sy);

/**
 * gowl_animation_open_start:
 * @self: a #GowlCompositor
 * @client: a client that has just been mapped
 *
 * Begins a centered scale-up with its own duration and curve, plus a
 * shorter, non-overshooting fade. Falls back to fade-only without a
 * capturable surface. Embedded clients are exempt.
 */
void gowl_animation_open_start (GowlCompositor *self, GowlClient *client);

/**
 * gowl_animation_reveal_start:
 * @self: a #GowlCompositor
 * @client: a client that has just become visible
 *
 * Fades a client in without scaling it, for tag visibility changes.
 * Does not restart a fade that is already running.
 */
void gowl_animation_reveal_start (GowlCompositor *self, GowlClient *client);

/**
 * gowl_animation_open_cancel:
 * @client: the client
 *
 * Ends an open animation and restores full opacity.  Called when the
 * animation finishes, and when a client is torn down mid-fade.
 */
void gowl_animation_open_cancel (GowlClient *client);

/**
 * gowl_animation_close_start:
 * @self: a #GowlCompositor
 * @client: a client that is unmapping
 *
 * Keeps a closing window on screen long enough to fade and shrink it
 * away.  Must be called while @client's surface still has its buffer
 * --- that is, from the unmap handler and before the scene tree goes.
 *
 * The snapshot preserves all surface buffers and their relative positions,
 * crops and transforms. Mid-morph closes transfer the displayed snapshot
 * without changing its geometry or opacity. Hidden clients do not animate.
 * If the buffers have already been dropped, the window simply disappears.
 */
void gowl_animation_close_start (GowlCompositor *self, GowlClient *client);

/**
 * gowl_animation_close_finish_all:
 * @self: a #GowlCompositor
 *
 * Ends every close animation immediately, releasing the buffers they
 * hold.  For shutdown, where a held client buffer would outlive the
 * renderer.
 */
void gowl_animation_close_finish_all (GowlCompositor *self);

/**
 * gowl_animation_close_forget_monitor:
 * @self: a #GowlCompositor
 * @monitor: the output going away
 *
 * Ends any close animation on @monitor.  They are a decoration measured
 * in milliseconds, so finishing them early on a screen that no longer
 * exists costs nothing and keeps them from pointing at freed memory.
 */
void gowl_animation_close_forget_monitor (GowlCompositor *self,
                                           GowlMonitor    *monitor);

/**
 * gowl_animation_tick:
 * @self: a #GowlCompositor
 * @monitor: (nullable): the output asking, or %NULL for "anywhere"
 * @now_us: the current time in microseconds
 *
 * Advances every live animation and moves the scene nodes.  Called once
 * per output frame, before the scene is built, so that the positions it
 * computes are the ones that frame draws.
 *
 * Every client is advanced regardless of @monitor --- the maths is
 * time-based and idempotent, so two outputs asking gives one answer.
 *
 * Returns: %TRUE while an animation is still running ON @monitor, which
 *   the caller uses to keep scheduling frames --- an idle output stops
 *   redrawing, and a half-finished slide would freeze mid-air.  Scoped
 *   to the one output so a window moving on one screen does not hold
 *   every other screen at full refresh.
 */
gboolean gowl_animation_tick (GowlCompositor *self,
                              GowlMonitor    *monitor,
                              gint64          now_us);

/**
 * gowl_animation_enabled:
 * @self: a #GowlCompositor
 *
 * Returns: %TRUE when animations are configured on and a duration is set.
 */
gboolean gowl_animation_enabled (GowlCompositor *self);

G_END_DECLS

#endif /* GOWL_ANIMATION_H */
