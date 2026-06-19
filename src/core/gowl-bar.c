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

#include "gowl-core-private.h"

/**
 * GowlBar:
 *
 * Represents the built-in status bar rendered by the compositor.
 * Each monitor may have its own bar instance.  The bar owns a
 * scene buffer for rendering and exposes height/visibility controls.
 *
 * The struct definition lives in gowl-core-private.h.
 */

G_DEFINE_FINAL_TYPE(GowlBar, gowl_bar, G_TYPE_OBJECT)

/* Signal identifiers */
enum {
	SIGNAL_RENDER,
	SIGNAL_CLICK,
	N_SIGNALS
};

static guint bar_signals[N_SIGNALS] = { 0, };

/* --- GObject lifecycle --- */

static void
gowl_bar_dispose(GObject *object)
{
	GowlBar *self;

	self = GOWL_BAR(object);
	self->monitor = NULL;

	G_OBJECT_CLASS(gowl_bar_parent_class)->dispose(object);
}

static void
gowl_bar_finalize(GObject *object)
{
	G_OBJECT_CLASS(gowl_bar_parent_class)->finalize(object);
}

/* --- class / instance init --- */

static void
gowl_bar_class_init(GowlBarClass *klass)
{
	GObjectClass *object_class;

	object_class = G_OBJECT_CLASS(klass);

	object_class->dispose  = gowl_bar_dispose;
	object_class->finalize = gowl_bar_finalize;

	/**
	 * GowlBar::render:
	 * @bar: the #GowlBar that emitted the signal
	 *
	 * Emitted when the bar needs to be redrawn.
	 */
	bar_signals[SIGNAL_RENDER] =
		g_signal_new("render",
		             G_TYPE_FROM_CLASS(klass),
		             G_SIGNAL_RUN_LAST,
		             0,
		             NULL, NULL,
		             NULL,
		             G_TYPE_NONE,
		             0);

	/**
	 * GowlBar::click:
	 * @bar: the #GowlBar that emitted the signal
	 * @x: the x coordinate of the click within the bar
	 * @y: the y coordinate of the click within the bar
	 * @button: the mouse button that was clicked
	 *
	 * Emitted when the bar receives a mouse click.
	 */
	bar_signals[SIGNAL_CLICK] =
		g_signal_new("click",
		             G_TYPE_FROM_CLASS(klass),
		             G_SIGNAL_RUN_LAST,
		             0,
		             NULL, NULL,
		             NULL,
		             G_TYPE_NONE,
		             3,
		             G_TYPE_INT,
		             G_TYPE_INT,
		             G_TYPE_UINT);
}

static void
gowl_bar_init(GowlBar *self)
{
	self->scene_buffer = NULL;
	self->height       = 20;
	self->visible      = TRUE;
	self->monitor      = NULL;
}

/* --- Public API --- */

/**
 * gowl_bar_new:
 *
 * Creates a new #GowlBar with default height (20) and visible state.
 *
 * Returns: (transfer full): a newly allocated #GowlBar
 */
GowlBar *
gowl_bar_new(void)
{
	return (GowlBar *)g_object_new(GOWL_TYPE_BAR, NULL);
}

/**
 * gowl_bar_get_height:
 * @self: a #GowlBar
 *
 * Returns the bar height in pixels.
 *
 * Returns: the height
 */
gint
gowl_bar_get_height(GowlBar *self)
{
	g_return_val_if_fail(GOWL_IS_BAR(self), 20);

	return self->height;
}

/**
 * gowl_bar_set_height:
 * @self: a #GowlBar
 * @height: the new bar height in pixels
 *
 * Sets the bar height.  The monitor layout will be recalculated
 * on the next frame.
 */
void
gowl_bar_set_height(
	GowlBar *self,
	gint     height
){
	g_return_if_fail(GOWL_IS_BAR(self));

	self->height = height;
}

/**
 * gowl_bar_is_visible:
 * @self: a #GowlBar
 *
 * Returns whether the bar is currently visible.
 *
 * Returns: %TRUE if visible
 */
gboolean
gowl_bar_is_visible(GowlBar *self)
{
	g_return_val_if_fail(GOWL_IS_BAR(self), FALSE);

	return self->visible;
}

/**
 * gowl_bar_set_visible:
 * @self: a #GowlBar
 * @visible: %TRUE to show, %FALSE to hide
 *
 * Sets the bar visibility.  The usable area for tiling will be
 * adjusted accordingly on the next layout pass.
 */
void
gowl_bar_set_visible(
	GowlBar  *self,
	gboolean  visible
){
	g_return_if_fail(GOWL_IS_BAR(self));

	self->visible = visible;
}

gint
gowl_bar_tag_hit(
	gint x,
	gint y,
	gint monitor_height,
	gint top_height,
	gint bottom_height,
	gint pad,
	gint tag_count
){
	gint box_w, idx;

	/* Which slot's vertical band (if any) does @y fall in?  A height
	   of 0 means that slot draws no tag row, so it is skipped. */
	if (top_height > 0 && y >= 0 && y < top_height) {
		box_w = top_height;
	} else if (bottom_height > 0 &&
	           y >= monitor_height - bottom_height &&
	           y < monitor_height) {
		box_w = bottom_height;
	} else {
		return -1;
	}

	if (tag_count <= 0 || box_w <= 0)
		return -1;

	/* Boxes run left to right from @pad, each @box_w wide. */
	if (x < pad || x >= pad + tag_count * box_w)
		return -1;

	idx = (x - pad) / box_w;
	if (idx < 0 || idx >= tag_count)
		return -1;

	return idx;
}
