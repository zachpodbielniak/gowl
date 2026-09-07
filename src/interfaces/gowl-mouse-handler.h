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

#ifndef GOWL_MOUSE_HANDLER_H
#define GOWL_MOUSE_HANDLER_H

#include <glib-object.h>

G_BEGIN_DECLS

#define GOWL_TYPE_MOUSE_HANDLER (gowl_mouse_handler_get_type())

G_DECLARE_INTERFACE(GowlMouseHandler, gowl_mouse_handler, GOWL, MOUSE_HANDLER, GObject)

/**
 * GowlMouseHandlerInterface:
 * @handle_button: a pointer button changed state
 * @handle_motion: the pointer moved to a layout position
 * @handle_axis: the pointer scrolled; @axis is 0 for vertical and 1 for
 *   horizontal, @delta is in surface units and @discrete in wheel clicks
 *
 * All three are consumable, first claimant wins: returning %TRUE stops
 * other handlers AND keeps the event from reaching the focused client.
 * A handler that claims a scroll with no modifier held would make the
 * mouse wheel stop working everywhere, so claim narrowly.
 */
struct _GowlMouseHandlerInterface {
	GTypeInterface parent_iface;

	gboolean (*handle_button) (GowlMouseHandler *self, guint button, guint state, guint modifiers);
	gboolean (*handle_motion) (GowlMouseHandler *self, gdouble x, gdouble y);
	gboolean (*handle_axis)   (GowlMouseHandler *self, guint axis,
	                           gdouble delta, gint discrete, guint modifiers);
};

/* Public dispatch functions */
gboolean gowl_mouse_handler_handle_button (GowlMouseHandler *self, guint button, guint state, guint modifiers);
gboolean gowl_mouse_handler_handle_motion (GowlMouseHandler *self, gdouble x, gdouble y);
gboolean gowl_mouse_handler_handle_axis   (GowlMouseHandler *self, guint axis,
                                           gdouble delta, gint discrete,
                                           guint modifiers);

G_END_DECLS

#endif /* GOWL_MOUSE_HANDLER_H */
