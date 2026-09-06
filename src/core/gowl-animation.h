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

G_BEGIN_DECLS

typedef struct _GowlCompositor GowlCompositor;
typedef struct _GowlMonitor GowlMonitor;
typedef struct _GowlClient GowlClient;

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
 * @client: the client that is moving
 * @from_x: where it is now, in layout space
 * @from_y: ditto
 * @to_x: where it is going
 * @to_y: ditto
 *
 * Begins sliding @client from one position to another.  A client
 * already animating is retargeted from wherever it currently is rather
 * than restarted from @from_x/@from_y, so a second layout change
 * mid-flight bends the path instead of snapping back.
 */
void gowl_animation_start (GowlCompositor *self,
                            GowlClient     *client,
                            gint            from_x,
                            gint            from_y,
                            gint            to_x,
                            gint            to_y);

/**
 * gowl_animation_cancel:
 * @client: the client
 *
 * Stops an animation and leaves the node wherever it is.  Used when a
 * client is about to be destroyed or torn out of the scene.
 */
void gowl_animation_cancel (GowlClient *client);

/**
 * gowl_animation_open_start:
 * @self: a #GowlCompositor
 * @client: a client that has just been mapped
 *
 * Begins a window's open animation: a fade from transparent, and a
 * short rise from just below where the layout put it.  Both are
 * compositor-side, so the client is never told and never re-renders.
 *
 * The rise is deliberately small.  A large one is a slide, and a slide
 * from anywhere but the window's own neighbourhood is a window flying
 * in across the display.
 */
void gowl_animation_open_start (GowlCompositor *self, GowlClient *client);

/**
 * gowl_animation_open_cancel:
 * @client: the client
 *
 * Ends an open animation and restores full opacity.  Called when the
 * animation finishes, and when a client is torn down mid-fade.
 */
void gowl_animation_open_cancel (GowlClient *client);

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
