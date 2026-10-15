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

#ifndef GOWL_INPUT_OBSERVER_H
#define GOWL_INPUT_OBSERVER_H

#include <glib-object.h>

G_BEGIN_DECLS

#define GOWL_TYPE_INPUT_OBSERVER (gowl_input_observer_get_type())

G_DECLARE_INTERFACE(GowlInputObserver, gowl_input_observer, GOWL, INPUT_OBSERVER, GObject)

/**
 * GowlInputObserverKey:
 * @focused: (nullable): the #GowlClient with keyboard focus, if any
 * @keycode: the evdev keycode
 * @keysym: the keysym with the live modifiers applied ("A" with Shift)
 * @base_keysym: the keysym on level 0 of the key ("a" with or without
 *   Shift), which is what a rule naming a key means
 * @modifiers: #GowlKeyMod bits held, Caps and Num Lock removed
 * @pressed: %TRUE for a press, %FALSE for a release
 * @synthetic: %TRUE for injected input (a KVM, gowl_compositor_inject_key())
 * @time_msec: the event time
 *
 * One key event, as an input observer sees it.  Valid for the length of
 * the call only.
 */
typedef struct {
	gpointer  focused;
	guint32   keycode;
	guint32   keysym;
	guint32   base_keysym;
	guint32   modifiers;
	gboolean  pressed;
	gboolean  synthetic;
	guint32   time_msec;
} GowlInputObserverKey;

/**
 * GowlInputObserverButton:
 * @client: (nullable): the #GowlClient the click went to
 * @button: the linux/input-event-codes.h button
 * @pressed: %TRUE for a press, %FALSE for a release
 * @sx: x of the click, relative to @client's surface
 * @sy: y of the click, relative to @client's surface
 * @width: @client's surface width, for scaling a click to another window
 * @height: @client's surface height
 * @modifiers: #GowlKeyMod bits held, Caps and Num Lock removed
 * @synthetic: %TRUE for injected input
 * @time_msec: the event time
 *
 * One pointer button event that reached a window.  Valid for the length
 * of the call only.
 */
typedef struct {
	gpointer  client;
	guint32   button;
	gboolean  pressed;
	gdouble   sx;
	gdouble   sy;
	gint      width;
	gint      height;
	guint32   modifiers;
	gboolean  synthetic;
	guint32   time_msec;
} GowlInputObserverButton;

/**
 * GowlInputObserverInterface:
 * @parent_iface: the parent interface
 * @filter_key: (nullable): offered every key early -- after the lock
 *   screen and the pointer-confinement escape, before the KVM
 *   diversion, configured binds, module binds and keyboard-shortcuts-
 *   inhibit -- so a key the observer owns works even while a game
 *   inhibits shortcuts.  Return %TRUE to consume it.
 * @key_forwarded: (nullable): told of a key that was just forwarded to
 *   the focused window -- after every bind had its chance, so a key the
 *   compositor or a module claimed is never reported.
 * @button_forwarded: (nullable): told of a button press or release that
 *   was just forwarded to a window.
 *
 * Watches input on its way to windows, e.g. to repeat it into other
 * windows (the broadcast module).  Every hook is called on the
 * compositor thread in the middle of input handling, for every active
 * implementation in priority order, and the core does no work for it at
 * all while no implementation is active.
 */
struct _GowlInputObserverInterface {
	GTypeInterface parent_iface;

	gboolean (*filter_key)       (GowlInputObserver             *self,
	                              const GowlInputObserverKey    *key);
	void     (*key_forwarded)    (GowlInputObserver             *self,
	                              const GowlInputObserverKey    *key);
	void     (*button_forwarded) (GowlInputObserver             *self,
	                              const GowlInputObserverButton *button);
};

gboolean gowl_input_observer_filter_key       (GowlInputObserver             *self,
                                               const GowlInputObserverKey    *key);
void     gowl_input_observer_key_forwarded    (GowlInputObserver             *self,
                                               const GowlInputObserverKey    *key);
void     gowl_input_observer_button_forwarded (GowlInputObserver             *self,
                                               const GowlInputObserverButton *button);

G_END_DECLS

#endif /* GOWL_INPUT_OBSERVER_H */
