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

#include "gowl-output-mode.h"

G_DEFINE_BOXED_TYPE(GowlOutputMode, gowl_output_mode,
                    gowl_output_mode_copy, gowl_output_mode_free)

/**
 * gowl_output_mode_new:
 * @width: horizontal resolution in pixels
 * @height: vertical resolution in pixels
 * @refresh_mhz: refresh rate in millihertz (e.g. 60000 for 60 Hz)
 *
 * Allocates a new #GowlOutputMode with the given resolution and
 * refresh rate.
 *
 * Returns: (transfer full): a newly allocated #GowlOutputMode. Free with
 *          gowl_output_mode_free().
 */
GowlOutputMode *
gowl_output_mode_new(
	gint width,
	gint height,
	gint refresh_mhz
){
	GowlOutputMode *self;

	self = g_slice_new(GowlOutputMode);
	self->width = width;
	self->height = height;
	self->refresh_mhz = refresh_mhz;

	return self;
}

/**
 * gowl_output_mode_copy:
 * @self: (not nullable): a #GowlOutputMode to copy
 *
 * Creates a deep copy of @self.
 *
 * Returns: (transfer full): a newly allocated copy of @self. Free with
 *          gowl_output_mode_free().
 */
GowlOutputMode *
gowl_output_mode_copy(const GowlOutputMode *self)
{
	g_return_val_if_fail(self != NULL, NULL);

	return gowl_output_mode_new(self->width, self->height, self->refresh_mhz);
}

/**
 * gowl_output_mode_free:
 * @self: (nullable): a #GowlOutputMode to free
 *
 * Releases all memory associated with @self. Safe to call with %NULL.
 */
void
gowl_output_mode_free(GowlOutputMode *self)
{
	if (self != NULL) {
		g_slice_free(GowlOutputMode, self);
	}
}
