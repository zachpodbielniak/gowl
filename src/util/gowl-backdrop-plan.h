/*
 * gowl - GObject Wayland Compositor
 * Copyright (C) 2026  Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

/*
 * gowl-backdrop-plan.h -- where a window's backdrop is rendered, and what
 * part of it the scene shows.
 *
 * Shared by every backdrop module that renders PER WINDOW rather than
 * cropping one shared picture --- the liquid glass and the liquid water,
 * today.  It started inside the glass module; the water needed exactly
 * the same arithmetic, and this is not arithmetic to have two copies of.
 *
 * It is in its own translation unit for the reason gowl-blur-geom.h gives
 * and for one more.  The reason it gives: wlr_render_pass_add_texture()
 * ASSERTS that the source box lies inside its texture, and an assert there
 * aborts the compositor thread mid page-flip -- which under `cmacs --gowl'
 * is the whole desktop session.  Pure arithmetic can be tested instead of
 * reasoned about.
 *
 * The one more: a per-window backdrop does its arithmetic in three
 * coordinate systems at once -- layout coordinates for where the window
 * is, the output's device pixels for the wallpaper it bends, and the
 * render buffer's own pixels, which are the device ones divided by however
 * much the render was scaled down.  Every one of those conversions is a
 * place to be quietly off by a factor of two on a HiDPI screen, and off by
 * a factor of two here does not crash: it draws the wrong quarter of the
 * wallpaper, which is exactly the kind of bug that survives review.
 */

#ifndef GOWL_BACKDROP_PLAN_H
#define GOWL_BACKDROP_PLAN_H

#include <glib.h>
#include <wlr/util/box.h>

/**
 * GowlBackdropPlan:
 * @buf_width: the buffer to render, in its own pixels
 * @buf_height: likewise
 * @scale_x: multiply a logical length by this for a buffer one
 * @scale_y: likewise, vertically; the two differ on an output whose
 *   pixels are not square
 * @src_scale: how many SOURCE pixels one buffer pixel is.  1 at full
 *   resolution, @divisor otherwise.  A buffer that is half the window is
 *   still looking at the whole of the window's wallpaper, and this is
 *   what carries that: without it the shader walks the source at buffer
 *   pace and covers a fraction of the region, magnified
 * @origin_x: where the window's top-left sits in the wallpaper texture,
 *   in texture pixels.  NEGATIVE for a window that starts off this
 *   output, which is not an error --- the shader clamps, and the glass
 *   simply repeats the wallpaper's edge there
 * @origin_y: likewise
 * @src: the crop into the rendered buffer the scene should show
 * @vis: the part of the window standing on this output, in layout
 *   coordinates
 *
 * Everything one window's backdrop needs, in the units each
 * consumer wants.
 */
typedef struct {
	gint            buf_width;
	gint            buf_height;
	gdouble         scale_x;
	gdouble         scale_y;
	gdouble         src_scale;
	gdouble         origin_x;
	gdouble         origin_y;
	struct wlr_fbox src;
	struct wlr_box  vis;
} GowlBackdropPlan;

/**
 * gowl_backdrop_plan:
 * @frame: the window frame as DRAWN, in layout coordinates
 * @monitor: the monitor's layout geometry
 * @tex_w: the wallpaper texture's width in pixels
 * @tex_h: its height
 * @divisor: render at 1/@divisor of device resolution, 1 for full size
 * @out: (out): the plan
 *
 * Works out how big a buffer this window's backdrop needs, where in the
 * wallpaper it starts, and which part of the result the scene shows.
 *
 * @frame must be the frame as DRAWN and not the client's layout
 * geometry: a scrolling layout leaves that unclipped on purpose, and a
 * floating window dragged half off the screen is never clipped at all,
 * so it can describe a rectangle largely not on this output.
 *
 * The whole window is rendered, not just the visible part.  That is
 * deliberate: what these modules draw at any point depends on the distance
 * to the window's OWN edge, so a window rendered as though it ended at the
 * screen's edge would grow a second bevel -- or a second shoreline -- down
 * the middle of the screen.
 *
 * @out->src is guaranteed to lie inside the buffer for every input,
 * including frames entirely off the monitor and degenerate monitors.
 *
 * Returns: %FALSE when no part of @frame is on @monitor, or when the
 *   window is too small to draw a backdrop for, in which case the caller
 *   should hide the node rather than render.
 */
gboolean gowl_backdrop_plan (const struct wlr_box *frame,
                          const struct wlr_box *monitor,
                          gint                  tex_w,
                          gint                  tex_h,
                          gint                  divisor,
                          GowlBackdropPlan        *out);

/**
 * gowl_backdrop_render_stale:
 * @have_buffer: whether a rendered buffer already exists
 * @cached: (nullable): the plan the existing buffer was rendered for
 * @want: the plan wanted now
 * @cached_serial: which capture of the wallpaper the buffer was drawn from
 * @serial: which capture is current
 * @cached_generation: the settings the buffer was rendered under
 * @generation: the settings now
 *
 * Whether the cached render has to be done again.
 *
 * POSITION COUNTS, which is the difference between this and the blur's
 * equivalent.  The blur shows a crop of one shared picture, so a window
 * that merely moved re-points its crop and is correct.  These bend the
 * wallpaper that is behind THIS window, so a window that moved is
 * refracting somewhere else and has to be drawn again.  Leaving position
 * out of this check is what made the blur show the previous monitor's
 * wallpaper after a hotplug, one layer up.
 *
 * An animating backdrop (the water) has nothing to ask here: its picture
 * is stale every frame by definition.  It uses this only for the parts
 * that are not the clock --- a resize, a new capture, a settings change.
 *
 * Returns: %TRUE when the buffer must be rendered again
 */
/**
 * gowl_backdrop_corner_radius:
 * @decor_radius: the radius the client decorator reports, in logical px
 * @border_width: the window's border width
 * @frame_w: the frame's width, in logical px
 * @frame_h: its height
 *
 * The radius of the rounded rect a per-window backdrop must mask itself
 * to, so that it ends exactly where the window does.
 *
 * This is NOT @decor_radius.  modules/roundcorners strokes a path inset
 * by half the border width and clamps that path's radius to half of what
 * is left; the stroke then extends half a border outwards from it.  So
 * the outer edge of the window --- the line a backdrop must not cross,
 * and must not stop short of --- is a rounded rect over the WHOLE frame
 * whose radius is the clamped path radius plus half a border.
 *
 * Using @decor_radius raw leaves a transparent nick inside each corner
 * on a bordered window, and a backdrop that spills past the border on a
 * small one where the decorator clamped and the backdrop did not.
 *
 * Returns: the radius to mask with, in logical pixels; 0 when the
 *   decorator draws square corners or there is no decorator.
 */
gdouble gowl_backdrop_corner_radius (gint decor_radius,
                                     gint border_width,
                                     gint frame_w,
                                     gint frame_h);

gboolean gowl_backdrop_render_stale (gboolean             have_buffer,
                                  const GowlBackdropPlan *cached,
                                  const GowlBackdropPlan *want,
                                  guint64              cached_serial,
                                  guint64              serial,
                                  guint64              cached_generation,
                                  guint64              generation);

#endif /* GOWL_BACKDROP_PLAN_H */
