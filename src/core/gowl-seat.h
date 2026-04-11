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

/**
 * gowl_seat_send_key:
 * @self: a #GowlSeat
 * @keycode: the XKB keycode to inject
 * @pressed: %TRUE for press, %FALSE for release
 *
 * Inject a synthetic key event to the focused surface.
 */
void      gowl_seat_send_key            (GowlSeat     *self,
                                          guint32       keycode,
                                          gboolean      pressed);

/**
 * gowl_seat_send_text:
 * @self: a #GowlSeat
 * @text: UTF-8 text to type
 *
 * Inject synthetic key events to type @text character-by-character
 * on the focused surface.
 */
void      gowl_seat_send_text           (GowlSeat     *self,
                                          const gchar  *text);

/**
 * gowl_seat_send_mouse_move:
 * @self: a #GowlSeat
 * @x: absolute x coordinate
 * @y: absolute y coordinate
 *
 * Move the cursor to the given absolute coordinates.
 */
void      gowl_seat_send_mouse_move     (GowlSeat     *self,
                                          gdouble       x,
                                          gdouble       y);

/**
 * gowl_seat_send_mouse_button:
 * @self: a #GowlSeat
 * @button: the button code (e.g. BTN_LEFT)
 * @pressed: %TRUE for press, %FALSE for release
 *
 * Inject a synthetic mouse button event.
 */
void      gowl_seat_send_mouse_button   (GowlSeat     *self,
                                          guint32       button,
                                          gboolean      pressed);

/**
 * gowl_seat_send_scroll:
 * @self: a #GowlSeat
 * @dx: horizontal scroll delta
 * @dy: vertical scroll delta
 *
 * Inject a synthetic scroll event.
 */
void      gowl_seat_send_scroll         (GowlSeat     *self,
                                          gdouble       dx,
                                          gdouble       dy);

/**
 * gowl_seat_get_clipboard:
 * @self: a #GowlSeat
 *
 * Get the current clipboard text content.
 *
 * Returns: (transfer full) (nullable): the clipboard text, or %NULL
 */
gchar    *gowl_seat_get_clipboard       (GowlSeat     *self);

/**
 * gowl_seat_set_clipboard:
 * @self: a #GowlSeat
 * @text: the text to place on the clipboard
 *
 * Set the clipboard content to @text.
 */
void      gowl_seat_set_clipboard       (GowlSeat     *self,
                                          const gchar  *text);

/**
 * gowl_seat_get_primary_selection:
 * @self: a #GowlSeat
 *
 * Get the current primary selection text.
 *
 * Returns: (transfer full) (nullable): the selection text, or %NULL
 */
gchar    *gowl_seat_get_primary_selection  (GowlSeat   *self);

/**
 * gowl_seat_set_primary_selection:
 * @self: a #GowlSeat
 * @text: the text to place in the primary selection
 *
 * Set the primary selection content to @text.
 */
void      gowl_seat_set_primary_selection  (GowlSeat   *self,
                                            const gchar *text);

/**
 * gowl_seat_emit_clipboard_changed:
 * @self: a #GowlSeat
 *
 * Emits the "clipboard-changed" signal.  Called by the compositor
 * after a client sets the clipboard selection.
 */
void      gowl_seat_emit_clipboard_changed          (GowlSeat *self);

/**
 * gowl_seat_emit_primary_selection_changed:
 * @self: a #GowlSeat
 *
 * Emits the "primary-selection-changed" signal.  Called by the
 * compositor after a client sets the primary selection.
 */
void      gowl_seat_emit_primary_selection_changed   (GowlSeat *self);

G_END_DECLS

#endif /* GOWL_SEAT_H */
