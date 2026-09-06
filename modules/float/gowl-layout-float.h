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

#ifndef GOWL_LAYOUT_FLOAT_H
#define GOWL_LAYOUT_FLOAT_H

#include <glib-object.h>

G_BEGIN_DECLS

/**
 * gowl_layout_float_symbol:
 *
 * Returns: (transfer none): the layout symbol string for float layout
 */
const gchar *
gowl_layout_float_symbol(void);

G_END_DECLS

#endif /* GOWL_LAYOUT_FLOAT_H */
