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
 * gowl-pointer-confine.c -- compositor-initiated pointer confinement,
 * the pure half.  See gowl-pointer-confine.h for why it exists and
 * gowl-compositor.c ("Compositor pointer confinement") for the half
 * that acts on these answers.
 */

#include "gowl-pointer-confine.h"

/* A box with no area confines nothing. */
static gboolean
box_empty(const struct wlr_box *b)
{
	return b == NULL || b->width <= 0 || b->height <= 0;
}

/*
 * The overlap of two boxes, written here rather than borrowed from
 * wlroots so this file stays arithmetic: the tests link it without
 * having to stand up anything.  An empty result has zero size.
 */
static void
box_intersect(
	const struct wlr_box *a,
	const struct wlr_box *b,
	struct wlr_box       *out
){
	gint x1, y1, x2, y2;

	x1 = MAX(a->x, b->x);
	y1 = MAX(a->y, b->y);
	x2 = MIN(a->x + a->width, b->x + b->width);
	y2 = MIN(a->y + a->height, b->y + b->height);

	out->x = x1;
	out->y = y1;
	out->width = (x2 > x1) ? x2 - x1 : 0;
	out->height = (y2 > y1) ? y2 - y1 : 0;
}

/* @b shrunk by @inset on every side; never negative in size. */
static void
box_inset(const struct wlr_box *b, gint inset, struct wlr_box *out)
{
	out->x = b->x + inset;
	out->y = b->y + inset;
	out->width = MAX(b->width - 2 * inset, 0);
	out->height = MAX(b->height - 2 * inset, 0);
}

gboolean
gowl_pointer_confine_compute_box(
	const struct wlr_box *geom,
	guint                 bw,
	const struct wlr_box *frame,
	const struct wlr_box *output,
	struct wlr_box       *out
){
	struct wlr_box inner;

	g_return_val_if_fail(geom != NULL, FALSE);
	g_return_val_if_fail(out != NULL, FALSE);

	/* The surface sits inside the border; the border itself is the
	 * compositor's, and a pointer resting on it is over no window. */
	box_inset(geom, (gint)bw, &inner);
	if (box_empty(&inner))
		return FALSE;

	/* Only the part of the window that is drawn.  The frame carries
	 * the border too, so it is inset the same way.  No overlap at all
	 * means the frame is not where the window is yet -- an animation
	 * moving it -- and the geometry, where it is going, stands. */
	if (!box_empty(frame)) {
		struct wlr_box drawn;
		struct wlr_box cut;

		box_inset(frame, (gint)bw, &drawn);
		box_intersect(&inner, &drawn, &cut);
		if (!box_empty(&cut))
			inner = cut;
	}

	/* And only its own output, when anything of it is there.  A
	 * window hanging over the edge into a gap between outputs would
	 * otherwise offer the pointer a region the layout cannot hold. */
	if (!box_empty(output)) {
		struct wlr_box cut;

		box_intersect(&inner, output, &cut);
		if (!box_empty(&cut))
			inner = cut;
	}

	*out = inner;
	return TRUE;
}

/*
 * The last position the pointer may take along an axis that starts at
 * @origin and is @extent logical pixels long: one physical pixel short
 * of the far edge.  Never before @origin, so a box narrower than a
 * physical pixel still has somewhere to put the pointer.
 */
static gdouble
axis_max(gint origin, gint extent, gdouble scale)
{
	gdouble edge;
	gdouble last;

	edge = (scale > 0.0) ? 1.0 / scale : 1.0;
	last = (gdouble)origin + (gdouble)extent - edge;
	return (last < (gdouble)origin) ? (gdouble)origin : last;
}

gboolean
gowl_pointer_confine_clamp(
	const struct wlr_box *box,
	gdouble               scale,
	gdouble              *x,
	gdouble              *y
){
	gdouble nx, ny;

	g_return_val_if_fail(box != NULL, FALSE);
	g_return_val_if_fail(x != NULL && y != NULL, FALSE);

	if (box_empty(box))
		return FALSE;

	nx = CLAMP(*x, (gdouble)box->x, axis_max(box->x, box->width, scale));
	ny = CLAMP(*y, (gdouble)box->y, axis_max(box->y, box->height, scale));

	if (nx == *x && ny == *y)
		return FALSE;
	*x = nx;
	*y = ny;
	return TRUE;
}

gboolean
gowl_pointer_confine_contains(
	const struct wlr_box *box,
	gdouble               scale,
	gdouble               x,
	gdouble               y
){
	gdouble cx = x;
	gdouble cy = y;

	g_return_val_if_fail(box != NULL, FALSE);

	if (box_empty(box))
		return FALSE;
	return !gowl_pointer_confine_clamp(box, scale, &cx, &cy);
}

GowlConfineFocusStep
gowl_pointer_confine_focus_step(
	gboolean confined,
	gboolean target_null,
	gboolean same_client,
	gboolean target_unmanaged,
	gboolean target_ruled,
	gboolean target_suppressed
){
	/* A popup taking the keyboard belongs to whatever opened it. */
	if (!target_null && target_unmanaged)
		return GOWL_CONFINE_FOCUS_KEEP;

	/* Focus back on the window already confined to: nothing moved. */
	if (confined && !target_null && same_client)
		return GOWL_CONFINE_FOCUS_KEEP;

	/* A ruled window takes the confinement with it as it takes focus,
	 * unless the user has just let go of it by hand. */
	if (!target_null && target_ruled && !target_suppressed)
		return GOWL_CONFINE_FOCUS_CONFINE;

	/* Anything else is the user going somewhere else. */
	return confined ? GOWL_CONFINE_FOCUS_RELEASE : GOWL_CONFINE_FOCUS_KEEP;
}

gboolean
gowl_pointer_confine_pin_blocks(
	gboolean pinned,
	gboolean target_is_confined,
	gboolean same_process
){
	return pinned && !target_is_confined && !same_process;
}

gboolean
gowl_pointer_confine_view_lost(
	guint32  tagset_then,
	guint32  tagset_now,
	gboolean visible
){
	return !visible || tagset_then != tagset_now;
}

gboolean
gowl_pointer_confine_escape_matches(
	guint want_mods,
	guint want_sym,
	guint mods,
	guint sym
){
	return want_sym != 0 && sym == want_sym && mods == want_mods;
}
