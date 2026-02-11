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

#include "gowl-border-spec.h"

G_DEFINE_BOXED_TYPE(GowlBorderSpec, gowl_border_spec,
                    gowl_border_spec_copy, gowl_border_spec_free)

/**
 * gowl_border_spec_new:
 * @width: border width in pixels
 * @focus_color: (not nullable): colour for focused borders
 * @unfocus_color: (not nullable): colour for unfocused borders
 * @urgent_color: (not nullable): colour for urgent borders
 *
 * Allocates a new #GowlBorderSpec.  The colour values are copied into the
 * struct by value so the caller retains ownership of the originals.
 *
 * Returns: (transfer full): a newly allocated #GowlBorderSpec. Free with
 *          gowl_border_spec_free().
 */
GowlBorderSpec *
gowl_border_spec_new(
	gint             width,
	const GowlColor *focus_color,
	const GowlColor *unfocus_color,
	const GowlColor *urgent_color
){
	GowlBorderSpec *self;

	g_return_val_if_fail(focus_color != NULL, NULL);
	g_return_val_if_fail(unfocus_color != NULL, NULL);
	g_return_val_if_fail(urgent_color != NULL, NULL);

	self = g_slice_new(GowlBorderSpec);
	self->width = width;
	self->focus_color = *focus_color;
	self->unfocus_color = *unfocus_color;
	self->urgent_color = *urgent_color;

	return self;
}

/**
 * gowl_border_spec_copy:
 * @self: (not nullable): a #GowlBorderSpec to copy
 *
 * Creates a deep copy of @self.
 *
 * Returns: (transfer full): a newly allocated copy of @self. Free with
 *          gowl_border_spec_free().
 */
GowlBorderSpec *
gowl_border_spec_copy(const GowlBorderSpec *self)
{
	g_return_val_if_fail(self != NULL, NULL);

	return gowl_border_spec_new(self->width,
	                             &self->focus_color,
	                             &self->unfocus_color,
	                             &self->urgent_color);
}

/**
 * gowl_border_spec_free:
 * @self: (nullable): a #GowlBorderSpec to free
 *
 * Releases all memory associated with @self. Safe to call with %NULL.
 */
void
gowl_border_spec_free(GowlBorderSpec *self)
{
	if (self != NULL) {
		g_slice_free(GowlBorderSpec, self);
	}
}
