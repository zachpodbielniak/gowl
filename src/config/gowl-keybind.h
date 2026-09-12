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

#ifndef GOWL_KEYBIND_H
#define GOWL_KEYBIND_H

#include <glib.h>

G_BEGIN_DECLS

/**
 * GOWL_BUTTON_WHEEL_UP:
 * GOWL_BUTTON_WHEEL_DOWN:
 * GOWL_BUTTON_WHEEL_LEFT:
 * GOWL_BUTTON_WHEEL_RIGHT:
 *
 * Pseudo button codes for scroll wheel steps in a mouse bind, chosen
 * above every BTN_* value linux/input-event-codes.h defines.
 */
#define GOWL_BUTTON_WHEEL_UP    (0x10000)
#define GOWL_BUTTON_WHEEL_DOWN  (0x10001)
#define GOWL_BUTTON_WHEEL_LEFT  (0x10002)
#define GOWL_BUTTON_WHEEL_RIGHT (0x10003)

/**
 * GowlGestureKind:
 * @GOWL_GESTURE_SWIPE: a multi-finger swipe
 * @GOWL_GESTURE_PINCH: a multi-finger pinch
 *
 * What kind of touchpad gesture a gesture bind names.
 */
typedef enum {
	GOWL_GESTURE_SWIPE,
	GOWL_GESTURE_PINCH
} GowlGestureKind;

/**
 * GowlGestureDirection:
 * @GOWL_GESTURE_LEFT: swipe left
 * @GOWL_GESTURE_RIGHT: swipe right
 * @GOWL_GESTURE_UP: swipe up
 * @GOWL_GESTURE_DOWN: swipe down
 * @GOWL_GESTURE_IN: pinch in (fingers together)
 * @GOWL_GESTURE_OUT: pinch out (fingers apart)
 * @GOWL_GESTURE_NONE: no direction decided yet
 *
 * Which way a gesture went.
 */
typedef enum {
	GOWL_GESTURE_NONE,
	GOWL_GESTURE_LEFT,
	GOWL_GESTURE_RIGHT,
	GOWL_GESTURE_UP,
	GOWL_GESTURE_DOWN,
	GOWL_GESTURE_IN,
	GOWL_GESTURE_OUT
} GowlGestureDirection;

/**
 * gowl_mousebind_parse:
 * @str: a mouse bind string such as "Super+Button1" or "Super+WheelUp"
 * @out_modifiers: (out): the modifier bitmask
 * @out_button: (out): the BTN_* code, or a %GOWL_BUTTON_WHEEL_* value
 *
 * Like gowl_keybind_parse(), with the last token a button: `Button1'
 * .. `Button9', or the names `Left', `Middle', `Right', `Side',
 * `Extra', `Forward', `Back', `Task', `WheelUp', `WheelDown',
 * `WheelLeft', `WheelRight' (case-insensitive).
 *
 * Returns: %TRUE on success
 */
gboolean gowl_mousebind_parse     (const gchar *str,
                                   guint       *out_modifiers,
                                   guint       *out_button);

/**
 * gowl_mousebind_to_string:
 * @modifiers: a modifier bitmask
 * @button: a BTN_* code or %GOWL_BUTTON_WHEEL_* value
 *
 * Returns: (transfer full): the bind string gowl_mousebind_parse()
 *   accepts
 */
gchar   *gowl_mousebind_to_string (guint modifiers, guint button);

/**
 * gowl_gesture_parse:
 * @str: a gesture string: "swipe-left-3", "swipe-up-4", "pinch-in-3",
 *   "pinch-out-4"; the finger count is 3 or 4 (libinput reports no
 *   gesture for fewer, and a touchpad rarely sees more)
 * @out_kind: (out): swipe or pinch
 * @out_direction: (out): which way
 * @out_fingers: (out): how many fingers
 *
 * Returns: %TRUE on success
 */
gboolean gowl_gesture_parse       (const gchar          *str,
                                   GowlGestureKind      *out_kind,
                                   GowlGestureDirection *out_direction,
                                   guint                *out_fingers);

/**
 * gowl_gesture_to_string:
 * @kind: swipe or pinch
 * @direction: which way
 * @fingers: how many
 *
 * Returns: (transfer full): the string gowl_gesture_parse() accepts
 */
gchar   *gowl_gesture_to_string   (GowlGestureKind      kind,
                                   GowlGestureDirection direction,
                                   guint                fingers);

/**
 * gowl_gesture_classify_swipe:
 * @dx: total horizontal travel of the swipe, in pixels
 * @dy: total vertical travel
 * @threshold: the least travel that counts, in pixels
 *
 * Decides which way a finished swipe went: the dominant axis, if it
 * moved at least @threshold.
 *
 * Returns: the direction, or %GOWL_GESTURE_NONE for a swipe too short
 *   to mean anything
 */
GowlGestureDirection gowl_gesture_classify_swipe (gdouble dx, gdouble dy,
                                                  gdouble threshold);

/**
 * gowl_gesture_classify_pinch:
 * @scale: the pinch's final scale, 1.0 being where it started
 *
 * Returns: %GOWL_GESTURE_IN below 0.8, %GOWL_GESTURE_OUT above 1.25,
 *   %GOWL_GESTURE_NONE between
 */
GowlGestureDirection gowl_gesture_classify_pinch (gdouble scale);

/**
 * gowl_keybind_parse:
 * @str: a keybind string such as "Super+Shift+Return"
 * @out_modifiers: (out): location for the parsed modifier bitmask
 * @out_keysym: (out): location for the parsed XKB keysym
 *
 * Parses a human-readable keybind string into a modifier bitmask
 * and an XKB keysym value. The string is split on "+" delimiters;
 * all tokens except the last are treated as modifier names (matched
 * case-insensitively), and the last token is resolved via
 * xkb_keysym_from_name().
 *
 * Recognised modifier names:
 *  - "Super", "Logo" -> GOWL_KEY_MOD_LOGO  (64)
 *  - "Shift"         -> GOWL_KEY_MOD_SHIFT  (1)
 *  - "Ctrl", "Control" -> GOWL_KEY_MOD_CTRL (4)
 *  - "Alt", "Mod1"   -> GOWL_KEY_MOD_ALT    (8)
 *  - "Mod2"          -> GOWL_KEY_MOD_MOD2   (16)
 *  - "Mod3"          -> GOWL_KEY_MOD_MOD3   (32)
 *  - "Mod5"          -> GOWL_KEY_MOD_MOD5  (128)
 *
 * Returns: %TRUE on success, %FALSE if the string is invalid
 */
gboolean
gowl_keybind_parse(
	const gchar *str,
	guint       *out_modifiers,
	guint       *out_keysym
);

/**
 * gowl_keybind_to_string:
 * @modifiers: a modifier bitmask (#GowlKeyMod flags)
 * @keysym: an XKB keysym value
 *
 * Converts a modifier bitmask and keysym back to a human-readable
 * string in the form "Mod+Mod+Key". Caller must free the returned
 * string with g_free().
 *
 * Returns: (transfer full): a newly allocated keybind string
 */
gchar *
gowl_keybind_to_string(
	guint modifiers,
	guint keysym
);

G_END_DECLS

#endif /* GOWL_KEYBIND_H */
