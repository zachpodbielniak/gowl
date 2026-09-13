/*
 * gowl - GObject Wayland Compositor
 * Copyright (C) 2026  Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

/*
 * gowl-glass-geom.h -- where a window's glass is rendered, and what part
 * of it the scene shows.
 *
 * Split out of the module for the reason gowl-blur-geom.h gives and for
 * one more.  The reason it gives: wlr_render_pass_add_texture() ASSERTS
 * that the source box lies inside its texture, and an assert there aborts
 * the compositor thread mid page-flip -- which under `cmacs --gowl' is
 * the whole desktop session.  Pure arithmetic can be tested instead of
 * reasoned about.
 *
 * The one more: the glass does its own arithmetic in three coordinate
 * systems at once -- layout coordinates for where the window is, the
 * output's device pixels for the wallpaper it refracts, and the render
 * buffer's own pixels, which are the device ones divided by however much
 * the render was scaled down while the window was moving.  Every one of
 * those conversions is a place to be quietly off by a factor of two on a
 * HiDPI screen, and off by a factor of two here does not crash: it draws
 * the wrong quarter of the wallpaper, which is exactly the kind of bug
 * that survives review.
 */

#ifndef GOWL_GLASS_GEOM_H
#define GOWL_GLASS_GEOM_H

#include <glib.h>
#include <wlr/util/box.h>

/**
 * GowlGlassPlan:
 * @buf_width: the buffer to render, in its own pixels
 * @buf_height: likewise
 * @scale_x: multiply a logical length by this for a buffer one
 * @scale_y: likewise, vertically; the two differ on an output whose
 *   pixels are not square
 * @origin_x: where the window's top-left sits in the wallpaper texture,
 *   in texture pixels.  NEGATIVE for a window that starts off this
 *   output, which is not an error --- the shader clamps, and the glass
 *   simply repeats the wallpaper's edge there
 * @origin_y: likewise
 * @src: the crop into the rendered buffer the scene should show
 * @vis: the part of the window standing on this output, in layout
 *   coordinates
 *
 * Everything one window's glass needs, in the units each consumer wants.
 */
typedef struct {
	gint            buf_width;
	gint            buf_height;
	gdouble         scale_x;
	gdouble         scale_y;
	gdouble         origin_x;
	gdouble         origin_y;
	struct wlr_fbox src;
	struct wlr_box  vis;
} GowlGlassPlan;

/**
 * gowl_glass_plan:
 * @frame: the window frame as DRAWN, in layout coordinates
 * @monitor: the monitor's layout geometry
 * @tex_w: the wallpaper texture's width in pixels
 * @tex_h: its height
 * @divisor: render at 1/@divisor of device resolution, 1 for full size
 * @out: (out): the plan
 *
 * Works out how big a buffer this window's glass needs, where in the
 * wallpaper it starts, and which part of the result the scene shows.
 *
 * @frame must be the frame as DRAWN and not the client's layout
 * geometry: a scrolling layout leaves that unclipped on purpose, and a
 * floating window dragged half off the screen is never clipped at all,
 * so it can describe a rectangle largely not on this output.
 *
 * The whole window is rendered, not just the visible part.  That is
 * deliberate: the refraction at any point depends on the distance to the
 * window's own edge, so a window rendered as though it ended at the
 * screen's edge would grow a second bevel down the middle of the screen.
 *
 * @out->src is guaranteed to lie inside the buffer for every input,
 * including frames entirely off the monitor and degenerate monitors.
 *
 * Returns: %FALSE when no part of @frame is on @monitor, or when the
 *   window is too small to draw glass for, in which case the caller
 *   should hide the node rather than render.
 */
gboolean gowl_glass_plan (const struct wlr_box *frame,
                          const struct wlr_box *monitor,
                          gint                  tex_w,
                          gint                  tex_h,
                          gint                  divisor,
                          GowlGlassPlan        *out);

/**
 * gowl_glass_render_stale:
 * @have_buffer: whether a rendered buffer already exists
 * @cached: (nullable): the plan the existing buffer was rendered for
 * @want: the plan wanted now
 * @cached_serial: which capture of the wallpaper the buffer refracts
 * @serial: which capture is current
 * @cached_generation: the settings the buffer was rendered under
 * @generation: the settings now
 *
 * Whether the cached render has to be done again.
 *
 * POSITION COUNTS, which is the difference between this and the blur's
 * equivalent.  The blur shows a crop of one shared picture, so a window
 * that merely moved re-points its crop and is correct.  Glass bends the
 * wallpaper that is behind THIS window, so a window that moved is
 * refracting somewhere else and has to be drawn again.  Leaving position
 * out of this check is what made the blur show the previous monitor's
 * wallpaper after a hotplug, one layer up.
 *
 * Returns: %TRUE when the buffer must be rendered again
 */
gboolean gowl_glass_render_stale (gboolean             have_buffer,
                                  const GowlGlassPlan *cached,
                                  const GowlGlassPlan *want,
                                  guint64              cached_serial,
                                  guint64              serial,
                                  guint64              cached_generation,
                                  guint64              generation);

#endif /* GOWL_GLASS_GEOM_H */
