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

#include "gowl-gaps.h"

G_DEFINE_BOXED_TYPE(GowlGaps, gowl_gaps,
                    gowl_gaps_copy, gowl_gaps_free)

/**
 * gowl_gaps_new:
 * @inner_h: horizontal inner gap in pixels
 * @inner_v: vertical inner gap in pixels
 * @outer_h: horizontal outer gap in pixels
 * @outer_v: vertical outer gap in pixels
 *
 * Allocates a new #GowlGaps with the given gap sizes.
 *
 * Returns: (transfer full): a newly allocated #GowlGaps. Free with
 *          gowl_gaps_free().
 */
GowlGaps *
gowl_gaps_new(
	gint inner_h,
	gint inner_v,
	gint outer_h,
	gint outer_v
){
	GowlGaps *self;

	self = g_slice_new(GowlGaps);
	self->inner_h = inner_h;
	self->inner_v = inner_v;
	self->outer_h = outer_h;
	self->outer_v = outer_v;

	return self;
}

/**
 * gowl_gaps_copy:
 * @self: (not nullable): a #GowlGaps to copy
 *
 * Creates a deep copy of @self.
 *
 * Returns: (transfer full): a newly allocated copy of @self. Free with
 *          gowl_gaps_free().
 */
GowlGaps *
gowl_gaps_copy(const GowlGaps *self)
{
	g_return_val_if_fail(self != NULL, NULL);

	return gowl_gaps_new(self->inner_h, self->inner_v,
	                      self->outer_h, self->outer_v);
}

/**
 * gowl_gaps_free:
 * @self: (nullable): a #GowlGaps to free
 *
 * Releases all memory associated with @self. Safe to call with %NULL.
 */
void
gowl_gaps_free(GowlGaps *self)
{
	if (self != NULL) {
		g_slice_free(GowlGaps, self);
	}
}

/**
 * gowl_gaps_equals:
 * @a: (not nullable): first #GowlGaps
 * @b: (not nullable): second #GowlGaps
 *
 * Tests whether two gap configurations are identical.
 *
 * Returns: %TRUE if all fields of @a and @b match, %FALSE otherwise.
 */
gboolean
gowl_gaps_equals(
	const GowlGaps *a,
	const GowlGaps *b
){
	g_return_val_if_fail(a != NULL, FALSE);
	g_return_val_if_fail(b != NULL, FALSE);

	return (a->inner_h == b->inner_h &&
	        a->inner_v == b->inner_v &&
	        a->outer_h == b->outer_h &&
	        a->outer_v == b->outer_v);
}
