/* Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later */

#include "gowl-gesture-handler.h"

G_DEFINE_INTERFACE(GowlGestureHandler, gowl_gesture_handler, G_TYPE_OBJECT)

static void
gowl_gesture_handler_default_init(GowlGestureHandlerInterface *iface)
{
	(void)iface;
}

#define GOWL_GESTURE_CALL(name, ...)                                          \
	do {                                                                  \
		GowlGestureHandlerInterface *iface;                           \
                                                                              \
		g_return_val_if_fail(GOWL_IS_GESTURE_HANDLER(self), FALSE);   \
		iface = GOWL_GESTURE_HANDLER_GET_IFACE(self);                 \
		if (iface->name == NULL)                                      \
			return FALSE;                                         \
		return iface->name(self, __VA_ARGS__);                        \
	} while (0)

gboolean
gowl_gesture_handler_swipe_begin(GowlGestureHandler *self, gpointer compositor,
                                  guint fingers)
{
	GOWL_GESTURE_CALL(swipe_begin, compositor, fingers);
}

gboolean
gowl_gesture_handler_swipe_update(GowlGestureHandler *self, gpointer compositor,
                                   gdouble dx, gdouble dy)
{
	GOWL_GESTURE_CALL(swipe_update, compositor, dx, dy);
}

gboolean
gowl_gesture_handler_swipe_end(GowlGestureHandler *self, gpointer compositor,
                                gboolean cancelled)
{
	GOWL_GESTURE_CALL(swipe_end, compositor, cancelled);
}

gboolean
gowl_gesture_handler_pinch_begin(GowlGestureHandler *self, gpointer compositor,
                                  guint fingers)
{
	GOWL_GESTURE_CALL(pinch_begin, compositor, fingers);
}

gboolean
gowl_gesture_handler_pinch_update(GowlGestureHandler *self, gpointer compositor,
                                   gdouble dx, gdouble dy,
                                   gdouble scale, gdouble rotation)
{
	GOWL_GESTURE_CALL(pinch_update, compositor, dx, dy, scale, rotation);
}

gboolean
gowl_gesture_handler_pinch_end(GowlGestureHandler *self, gpointer compositor,
                                gboolean cancelled)
{
	GOWL_GESTURE_CALL(pinch_end, compositor, cancelled);
}
