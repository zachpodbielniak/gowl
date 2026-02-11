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

#include "gowl-geometry.h"

G_DEFINE_BOXED_TYPE(GowlGeometry, gowl_geometry,
                    gowl_geometry_copy, gowl_geometry_free)

/**
 * gowl_geometry_new:
 * @x: horizontal position
 * @y: vertical position
 * @width: width in pixels
 * @height: height in pixels
 *
 * Allocates a new #GowlGeometry with the given position and dimensions.
 *
 * Returns: (transfer full): a newly allocated #GowlGeometry. Free with
 *          gowl_geometry_free().
 */
GowlGeometry *
gowl_geometry_new(
	gint x,
	gint y,
	gint width,
	gint height
){
	GowlGeometry *self;

	self = g_slice_new(GowlGeometry);
	self->x = x;
	self->y = y;
	self->width = width;
	self->height = height;

	return self;
}

/**
 * gowl_geometry_copy:
 * @self: (not nullable): a #GowlGeometry to copy
 *
 * Creates a deep copy of @self.
 *
 * Returns: (transfer full): a newly allocated copy of @self. Free with
 *          gowl_geometry_free().
 */
GowlGeometry *
gowl_geometry_copy(const GowlGeometry *self)
{
	g_return_val_if_fail(self != NULL, NULL);

	return gowl_geometry_new(self->x, self->y, self->width, self->height);
}

/**
 * gowl_geometry_free:
 * @self: (nullable): a #GowlGeometry to free
 *
 * Releases all memory associated with @self. Safe to call with %NULL.
 */
void
gowl_geometry_free(GowlGeometry *self)
{
	if (self != NULL) {
		g_slice_free(GowlGeometry, self);
	}
}

/**
 * gowl_geometry_equals:
 * @a: (not nullable): first #GowlGeometry
 * @b: (not nullable): second #GowlGeometry
 *
 * Tests whether two geometries have identical position and dimensions.
 *
 * Returns: %TRUE if @a and @b are equal, %FALSE otherwise.
 */
gboolean
gowl_geometry_equals(
	const GowlGeometry *a,
	const GowlGeometry *b
){
	g_return_val_if_fail(a != NULL, FALSE);
	g_return_val_if_fail(b != NULL, FALSE);

	return (a->x == b->x &&
	        a->y == b->y &&
	        a->width == b->width &&
	        a->height == b->height);
}

/**
 * gowl_geometry_contains:
 * @self: (not nullable): a #GowlGeometry
 * @px: x coordinate of the point to test
 * @py: y coordinate of the point to test
 *
 * Tests whether the point (@px, @py) falls inside the rectangle defined
 * by @self.  The test is inclusive on the top-left edge and exclusive on
 * the bottom-right edge.
 *
 * Returns: %TRUE if the point is inside @self, %FALSE otherwise.
 */
gboolean
gowl_geometry_contains(
	const GowlGeometry *self,
	gint                px,
	gint                py
){
	g_return_val_if_fail(self != NULL, FALSE);

	return (px >= self->x &&
	        px < self->x + self->width &&
	        py >= self->y &&
	        py < self->y + self->height);
}
