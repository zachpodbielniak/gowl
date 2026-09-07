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
 * gowl-blur-geom.h -- where the backdrop crops the wallpaper.
 *
 * Split out from the module for one reason: getting this wrong kills the
 * session, and it is pure arithmetic, so it can be tested instead of
 * reasoned about.
 *
 * wlr_render_pass_add_texture() asserts that the source box lies inside
 * the texture.  An assert there is not a debug aid -- it aborts the
 * process, on the compositor thread, mid page-flip.  Under `cmacs
 * --gowl' that process is the user's whole desktop session, and because
 * Emacs re-raises the fatal signal on its main thread the report comes
 * back with an idle pselect backtrace that names nothing to do with
 * blur.
 */

#ifndef GOWL_BLUR_GEOM_H
#define GOWL_BLUR_GEOM_H

#include <glib.h>
#include <wlr/util/box.h>

/**
 * gowl_blur_backdrop_box:
 * @frame: the window frame as DRAWN, in layout coordinates
 * @monitor: the monitor's layout geometry
 * @tex_w: backdrop texture width in pixels
 * @tex_h: backdrop texture height in pixels
 * @src_out: (out): crop rectangle into the backdrop texture
 * @vis_out: (out): the visible part of @frame, in layout coordinates
 *
 * Works out which part of a monitor's blurred wallpaper shows through a
 * window, and where.
 *
 * @frame must be the frame as drawn, not the client's layout geometry:
 * a scrolling layout keeps that unclipped on purpose, and a floating
 * window dragged half off the screen is never clipped at all, so it can
 * describe a rectangle largely not on this output.
 *
 * The result is guaranteed to satisfy wlroots' source-box invariant
 * (@src_out inside 0,0..@tex_w,@tex_h) for every input, including
 * frames that lie entirely off the monitor and degenerate monitors.
 *
 * Returns: %FALSE when no part of @frame is on @monitor, in which case
 *   the backdrop has nothing to show and should be hidden
 */
gboolean gowl_blur_backdrop_box (const struct wlr_box *frame,
                                  const struct wlr_box *monitor,
                                  gint                  tex_w,
                                  gint                  tex_h,
                                  struct wlr_fbox      *src_out,
                                  struct wlr_box       *vis_out);

/**
 * gowl_blur_backdrop_stale:
 * @have_buffer: whether a backdrop buffer already exists
 * @cached_tags: the tag set the cached backdrop was captured under
 * @current_tags: the monitor's visible tag set now
 * @cached_w: cached buffer width
 * @cached_h: cached buffer height
 * @out_w: the output's current pixel width
 * @out_h: the output's current pixel height
 *
 * Whether the cached backdrop has to be captured again.
 *
 * The backdrop is the monitor's wallpaper, blurred, with every client
 * layer hidden -- so windows opening, closing or moving do NOT change
 * it, and it is cached rather than rebuilt per frame.  What does change
 * it is the tag set, because wallpapers are per-tag, and the output
 * size.
 *
 * Returns: %TRUE when the backdrop must be rebuilt
 */
gboolean gowl_blur_backdrop_stale (gboolean have_buffer,
                                    guint32  cached_tags,
                                    guint32  current_tags,
                                    gint     cached_w,
                                    gint     cached_h,
                                    gint     out_w,
                                    gint     out_h);

#endif /* GOWL_BLUR_GEOM_H */
