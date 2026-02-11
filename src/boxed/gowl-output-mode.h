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

#ifndef GOWL_OUTPUT_MODE_H
#define GOWL_OUTPUT_MODE_H

#include "gowl-types.h"

G_BEGIN_DECLS

#define GOWL_TYPE_OUTPUT_MODE (gowl_output_mode_get_type())

/**
 * GowlOutputMode:
 * @width: Horizontal resolution in pixels.
 * @height: Vertical resolution in pixels.
 * @refresh_mhz: Refresh rate in millihertz (e.g. 60000 for 60 Hz).
 *
 * Describes a display output mode (resolution and refresh rate).
 */
struct _GowlOutputMode {
	gint width;
	gint height;
	gint refresh_mhz;
};

GType           gowl_output_mode_get_type (void) G_GNUC_CONST;

GowlOutputMode *gowl_output_mode_new     (gint                    width,
                                           gint                    height,
                                           gint                    refresh_mhz);

GowlOutputMode *gowl_output_mode_copy    (const GowlOutputMode   *self);

void            gowl_output_mode_free    (GowlOutputMode          *self);

G_END_DECLS

#endif /* GOWL_OUTPUT_MODE_H */
