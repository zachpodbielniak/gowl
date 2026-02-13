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

#ifndef GOWLBAR_ENUMS_H
#define GOWLBAR_ENUMS_H

/**
 * GowlbarPosition:
 * @GOWLBAR_POSITION_TOP: bar anchored at top of output
 * @GOWLBAR_POSITION_BOTTOM: bar anchored at bottom of output
 *
 * Determines which edge of the output the bar attaches to.
 */
typedef enum {
	GOWLBAR_POSITION_TOP    = 0,
	GOWLBAR_POSITION_BOTTOM = 1
} GowlbarPosition;

/**
 * GowlbarAlignment:
 * @GOWLBAR_ALIGN_LEFT: left-aligned widgets
 * @GOWLBAR_ALIGN_CENTER: center-expanding widgets
 * @GOWLBAR_ALIGN_RIGHT: right-aligned widgets
 *
 * Widget alignment within the bar layout.
 */
typedef enum {
	GOWLBAR_ALIGN_LEFT   = 0,
	GOWLBAR_ALIGN_CENTER = 1,
	GOWLBAR_ALIGN_RIGHT  = 2
} GowlbarAlignment;

#endif /* GOWLBAR_ENUMS_H */
