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

#ifndef GOWL_SEAT_H
#define GOWL_SEAT_H

#include <glib-object.h>

G_BEGIN_DECLS

#define GOWL_TYPE_SEAT (gowl_seat_get_type())

G_DECLARE_FINAL_TYPE(GowlSeat, gowl_seat, GOWL, SEAT, GObject)

/**
 * gowl_seat_new:
 *
 * Creates a new #GowlSeat.
 *
 * Returns: (transfer full): a newly created #GowlSeat
 */
GowlSeat *gowl_seat_new                (void);

/**
 * gowl_seat_get_focused_client:
 * @self: a #GowlSeat
 *
 * Returns the currently focused client.
 *
 * Returns: (transfer none) (nullable): the focused #GowlClient, or %NULL
 */
gpointer  gowl_seat_get_focused_client (GowlSeat *self);

/**
 * gowl_seat_set_focused_client:
 * @self: a #GowlSeat
 * @client: (nullable): the #GowlClient to focus, or %NULL to clear
 *
 * Sets the focused client and emits "focus-changed".
 */
void      gowl_seat_set_focused_client (GowlSeat *self,
                                        gpointer  client);

G_END_DECLS

#endif /* GOWL_SEAT_H */
