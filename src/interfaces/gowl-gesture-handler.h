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

#ifndef GOWL_GESTURE_HANDLER_H
#define GOWL_GESTURE_HANDLER_H

#include <glib-object.h>

G_BEGIN_DECLS

#define GOWL_TYPE_GESTURE_HANDLER (gowl_gesture_handler_get_type())

G_DECLARE_INTERFACE(GowlGestureHandler, gowl_gesture_handler,
                    GOWL, GESTURE_HANDLER, GObject)

/**
 * GowlGestureHandlerInterface:
 * @swipe_begin: a multi-finger swipe started; return %TRUE to claim it
 * @swipe_update: the fingers moved by @dx, @dy since the last update
 * @swipe_end: the swipe finished, or was cancelled
 * @pinch_begin: a pinch started; return %TRUE to claim it
 * @pinch_update: the pinch changed by @dx, @dy, @scale and @rotation
 * @pinch_end: the pinch finished, or was cancelled
 *
 * Touchpad gestures, offered to modules BEFORE they are relayed to the
 * focused client.
 *
 * Consumable, first claimant wins: a module that returns %TRUE from a
 * begin owns the whole gesture, and the client never sees it.  That is
 * the point --- a three-finger swipe that both turns the desktop AND
 * scrolls the browser underneath it is nobody's intent --- but it also
 * means claiming must be conservative.  A handler that cannot actually
 * act (wrong finger count, the feature is configured off, no output
 * focused) must decline in @swipe_begin rather than claim and do
 * nothing, because a claimed gesture is one the application will never
 * receive.
 *
 * @swipe_update and @swipe_end are only delivered to the handler that
 * claimed the begin, so a handler does not have to guard against
 * gestures that are not its own.
 */
struct _GowlGestureHandlerInterface {
	GTypeInterface parent_iface;

	gboolean (*swipe_begin)  (GowlGestureHandler *self, gpointer compositor,
	                          guint fingers);
	gboolean (*swipe_update) (GowlGestureHandler *self, gpointer compositor,
	                          gdouble dx, gdouble dy);
	gboolean (*swipe_end)    (GowlGestureHandler *self, gpointer compositor,
	                          gboolean cancelled);

	gboolean (*pinch_begin)  (GowlGestureHandler *self, gpointer compositor,
	                          guint fingers);
	gboolean (*pinch_update) (GowlGestureHandler *self, gpointer compositor,
	                          gdouble dx, gdouble dy,
	                          gdouble scale, gdouble rotation);
	gboolean (*pinch_end)    (GowlGestureHandler *self, gpointer compositor,
	                          gboolean cancelled);
};

/* Public dispatch functions */
gboolean gowl_gesture_handler_swipe_begin  (GowlGestureHandler *self,
                                            gpointer            compositor,
                                            guint               fingers);
gboolean gowl_gesture_handler_swipe_update (GowlGestureHandler *self,
                                            gpointer            compositor,
                                            gdouble             dx,
                                            gdouble             dy);
gboolean gowl_gesture_handler_swipe_end    (GowlGestureHandler *self,
                                            gpointer            compositor,
                                            gboolean            cancelled);
gboolean gowl_gesture_handler_pinch_begin  (GowlGestureHandler *self,
                                            gpointer            compositor,
                                            guint               fingers);
gboolean gowl_gesture_handler_pinch_update (GowlGestureHandler *self,
                                            gpointer            compositor,
                                            gdouble             dx,
                                            gdouble             dy,
                                            gdouble             scale,
                                            gdouble             rotation);
gboolean gowl_gesture_handler_pinch_end    (GowlGestureHandler *self,
                                            gpointer            compositor,
                                            gboolean            cancelled);

G_END_DECLS

#endif /* GOWL_GESTURE_HANDLER_H */
