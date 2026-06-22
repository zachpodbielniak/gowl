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

#include "gowl-input-zone.h"

G_DEFINE_BOXED_TYPE(GowlInputZone, gowl_input_zone,
                    gowl_input_zone_copy, gowl_input_zone_free)

/**
 * gowl_input_zone_new:
 * @width: zone width in layout pixels
 * @height: zone height in layout pixels
 * @x: zone left edge, layout-relative
 * @y: zone top edge, layout-relative
 * @output: (nullable): source output name
 *
 * Allocates a new #GowlInputZone.  @output is copied; %NULL is stored as
 * %NULL (not the empty string).
 *
 * Returns: (transfer full): a new #GowlInputZone. Free with
 *          gowl_input_zone_free().
 */
GowlInputZone *
gowl_input_zone_new(
	guint        width,
	guint        height,
	gint         x,
	gint         y,
	const gchar *output
){
	GowlInputZone *self;

	self = g_slice_new0(GowlInputZone);
	self->width  = width;
	self->height = height;
	self->x      = x;
	self->y      = y;
	self->output = g_strdup(output);

	return self;
}

/**
 * gowl_input_zone_copy:
 * @self: (not nullable): a #GowlInputZone to copy
 *
 * Creates a deep copy of @self.
 *
 * Returns: (transfer full): a newly allocated copy. Free with
 *          gowl_input_zone_free().
 */
GowlInputZone *
gowl_input_zone_copy(const GowlInputZone *self)
{
	g_return_val_if_fail(self != NULL, NULL);

	return gowl_input_zone_new(self->width, self->height,
	                           self->x, self->y, self->output);
}

/**
 * gowl_input_zone_free:
 * @self: (nullable): a #GowlInputZone to free
 *
 * Releases @self and its string. Safe to call with %NULL.
 */
void
gowl_input_zone_free(GowlInputZone *self)
{
	if (self != NULL) {
		g_free(self->output);
		g_slice_free(GowlInputZone, self);
	}
}

/**
 * gowl_input_zone_equals:
 * @a: (not nullable): first zone
 * @b: (not nullable): second zone
 *
 * Tests whether two zones describe the same region (same geometry and
 * output name; NULL-safe, two NULL outputs compare equal).
 *
 * Returns: %TRUE if equal, %FALSE otherwise.
 */
gboolean
gowl_input_zone_equals(
	const GowlInputZone *a,
	const GowlInputZone *b
){
	g_return_val_if_fail(a != NULL, FALSE);
	g_return_val_if_fail(b != NULL, FALSE);

	return a->width == b->width
	       && a->height == b->height
	       && a->x == b->x
	       && a->y == b->y
	       && g_strcmp0(a->output, b->output) == 0;
}

/**
 * gowl_input_zone_contains_point:
 * @self: (not nullable): a #GowlInputZone
 * @x: point x, layout-relative
 * @y: point y, layout-relative
 *
 * Tests whether (@x, @y) lies within @self.  The left/top edges are
 * inclusive and the right/bottom edges exclusive (half-open), so adjacent
 * zones in a layout never both claim the same pixel.
 *
 * Returns: %TRUE if the point is inside @self.
 */
gboolean
gowl_input_zone_contains_point(
	const GowlInputZone *self,
	gint                 x,
	gint                 y
){
	g_return_val_if_fail(self != NULL, FALSE);

	return x >= self->x
	       && y >= self->y
	       && x < self->x + (gint)self->width
	       && y < self->y + (gint)self->height;
}
