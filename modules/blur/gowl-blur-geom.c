/*
 * gowl - GObject Wayland Compositor
 * Copyright (C) 2026  Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "gowl-blur-geom.h"

/*
 * clamp_src:
 *
 * Forces @src inside the texture, preserving the left/top edge where it
 * can.  The caller has already intersected with the monitor, which makes
 * this true in exact arithmetic; this absorbs what a fractional output
 * scale leaves behind, where x + width lands a fraction past the edge.
 */
static void
clamp_src(struct wlr_fbox *src, gint tex_w, gint tex_h)
{
	gdouble w = (gdouble)tex_w;
	gdouble h = (gdouble)tex_h;

	if (src->x < 0.0) {
		src->width += src->x;
		src->x = 0.0;
	}
	if (src->y < 0.0) {
		src->height += src->y;
		src->y = 0.0;
	}
	if (src->x > w)
		src->x = w;
	if (src->y > h)
		src->y = h;
	if (src->width < 0.0)
		src->width = 0.0;
	if (src->height < 0.0)
		src->height = 0.0;
	if (src->x + src->width > w)
		src->width = w - src->x;
	if (src->y + src->height > h)
		src->height = h - src->y;
}

gboolean
gowl_blur_backdrop_box(
	const struct wlr_box *frame,
	const struct wlr_box *monitor,
	gint                  tex_w,
	gint                  tex_h,
	struct wlr_fbox      *src_out,
	struct wlr_box       *vis_out
){
	struct wlr_box  vis;
	struct wlr_fbox src;
	gdouble         scale_x, scale_y;

	if (frame == NULL || monitor == NULL || tex_w <= 0 || tex_h <= 0)
		return FALSE;

	/*
	 * Only the part standing on this monitor has wallpaper behind it:
	 * the backdrop texture covers exactly this output and nothing
	 * beyond.  An empty intersection means the window is entirely
	 * elsewhere.
	 */
	if (!wlr_box_intersection(&vis, frame, monitor))
		return FALSE;
	if (vis.width <= 0 || vis.height <= 0)
		return FALSE;

	/*
	 * The source box is in BUFFER pixels while the geometry is logical,
	 * so it is scaled by the output's ratio -- on a HiDPI screen the two
	 * differ by a factor of two and the crop would otherwise come from
	 * the top-left quarter of the wallpaper.  Each axis is scaled from
	 * its own extent: deriving the vertical scale from the width is only
	 * correct while the output's pixels are square.
	 */
	scale_x = monitor->width  > 0
		? (gdouble)tex_w / (gdouble)monitor->width  : 1.0;
	scale_y = monitor->height > 0
		? (gdouble)tex_h / (gdouble)monitor->height : 1.0;

	src.x      = ((gdouble)vis.x - (gdouble)monitor->x) * scale_x;
	src.y      = ((gdouble)vis.y - (gdouble)monitor->y) * scale_y;
	src.width  = (gdouble)vis.width  * scale_x;
	src.height = (gdouble)vis.height * scale_y;
	clamp_src(&src, tex_w, tex_h);

	if (src_out != NULL)
		*src_out = src;
	if (vis_out != NULL)
		*vis_out = vis;
	return TRUE;
}
