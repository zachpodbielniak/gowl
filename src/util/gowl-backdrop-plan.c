/*
 * gowl - GObject Wayland Compositor
 * Copyright (C) 2026  Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "gowl-backdrop-plan.h"

#include <math.h>

/* Below this a window has no room for an edge effect worth drawing, and
 * the arithmetic that widens the direction field runs out of rectangle. */
#define GOWL_BACKDROP_MIN_SIDE (8)

/* Keep a crop inside its buffer, whatever arrived. */
static void
clamp_src(struct wlr_fbox *src, gint buf_w, gint buf_h)
{
	gdouble w = (gdouble)buf_w;
	gdouble h = (gdouble)buf_h;

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
gowl_backdrop_plan(
	const struct wlr_box *frame,
	const struct wlr_box *monitor,
	gint                  tex_w,
	gint                  tex_h,
	gint                  divisor,
	GowlBackdropPlan        *out
){
	GowlBackdropPlan   plan;
	struct wlr_box  vis;
	gdouble         dev_x, dev_y;

	if (frame == NULL || monitor == NULL || out == NULL)
		return FALSE;
	if (tex_w <= 0 || tex_h <= 0)
		return FALSE;
	if (frame->width < GOWL_BACKDROP_MIN_SIDE
	    || frame->height < GOWL_BACKDROP_MIN_SIDE)
		return FALSE;
	if (divisor < 1)
		divisor = 1;

	/*
	 * Only the part standing on this output has wallpaper behind it: the
	 * texture covers exactly this output and nothing beyond.  An empty
	 * intersection means the window is entirely elsewhere, and its glass
	 * has nothing to show.
	 */
	if (!wlr_box_intersection(&vis, frame, monitor))
		return FALSE;
	if (vis.width <= 0 || vis.height <= 0)
		return FALSE;

	/*
	 * Logical to device, each axis from its own extent: deriving the
	 * vertical ratio from the width is only correct while the output's
	 * pixels are square, and a rotated or non-square mode is exactly when
	 * nobody is looking.
	 */
	dev_x = monitor->width  > 0
		? (gdouble)tex_w / (gdouble)monitor->width  : 1.0;
	dev_y = monitor->height > 0
		? (gdouble)tex_h / (gdouble)monitor->height : 1.0;

	/* ...then down by the divisor, so that from here on "buffer pixels"
	 * is one unit and the bevel, the fringe and the crop are all measured
	 * in it.  Folding the divisor in here rather than at each use is what
	 * keeps a render scaled down mid-drag from also shrinking its own
	 * rim. */
	plan.scale_x = dev_x / (gdouble)divisor;
	plan.scale_y = dev_y / (gdouble)divisor;
	/*
	 * ...and how to get BACK, which is a separate fact and the one that
	 * is easy to leave out.  A half-resolution buffer is not looking at
	 * half the window: it is looking at all of it, less finely.  A shader
	 * handed only the buffer's own units walks the source at buffer pace
	 * and covers a quarter of the area, magnified fourfold -- which is
	 * invisible in review and, because the reduced resolution is only
	 * used while a window is MOVING, very nearly invisible on screen too.
	 */
	plan.src_scale = (gdouble)divisor;

	plan.buf_width  = (gint)floor((gdouble)frame->width  * plan.scale_x + 0.5);
	plan.buf_height = (gint)floor((gdouble)frame->height * plan.scale_y + 0.5);
	if (plan.buf_width < 1 || plan.buf_height < 1)
		return FALSE;

	/* Where the window's top-left is in the wallpaper, in TEXTURE pixels
	 * -- the texture is not scaled down with the render, so this one uses
	 * the device ratio rather than the buffer one. */
	plan.origin_x = ((gdouble)frame->x - (gdouble)monitor->x) * dev_x;
	plan.origin_y = ((gdouble)frame->y - (gdouble)monitor->y) * dev_y;

	plan.src.x      = ((gdouble)vis.x - (gdouble)frame->x) * plan.scale_x;
	plan.src.y      = ((gdouble)vis.y - (gdouble)frame->y) * plan.scale_y;
	plan.src.width  = (gdouble)vis.width  * plan.scale_x;
	plan.src.height = (gdouble)vis.height * plan.scale_y;
	clamp_src(&plan.src, plan.buf_width, plan.buf_height);
	if (plan.src.width <= 0.0 || plan.src.height <= 0.0)
		return FALSE;

	plan.vis = vis;
	*out = plan;
	return TRUE;
}

gboolean
gowl_backdrop_render_stale(
	gboolean             have_buffer,
	const GowlBackdropPlan *cached,
	const GowlBackdropPlan *want,
	guint64              cached_serial,
	guint64              serial,
	guint64              cached_generation,
	guint64              generation
){
	if (!have_buffer || cached == NULL || want == NULL)
		return TRUE;
	if (cached_serial != serial || cached_generation != generation)
		return TRUE;
	if (cached->buf_width != want->buf_width
	    || cached->buf_height != want->buf_height)
		return TRUE;

	/*
	 * A window that moved by less than a pixel has not moved: a drag
	 * reports fractional layout positions that round to the same texture
	 * pixel, and re-rendering for each of them is a full pass over the
	 * window for a picture that comes out identical.
	 */
	if (fabs(cached->origin_x - want->origin_x) >= 0.5
	    || fabs(cached->origin_y - want->origin_y) >= 0.5)
		return TRUE;
	return FALSE;
}
