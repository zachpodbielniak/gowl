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

#include "gowl-input-observer.h"

G_DEFINE_INTERFACE(GowlInputObserver, gowl_input_observer, G_TYPE_OBJECT)

static void
gowl_input_observer_default_init(GowlInputObserverInterface *iface)
{
	/* Default implementation - observes nothing, consumes nothing */
	(void)iface;
}

/**
 * gowl_input_observer_filter_key:
 * @self: a #GowlInputObserver
 * @key: the key event
 *
 * Returns: %TRUE when @self consumed the key
 */
gboolean
gowl_input_observer_filter_key(
	GowlInputObserver          *self,
	const GowlInputObserverKey *key
){
	GowlInputObserverInterface *iface;

	g_return_val_if_fail(GOWL_IS_INPUT_OBSERVER(self), FALSE);
	g_return_val_if_fail(key != NULL, FALSE);

	iface = GOWL_INPUT_OBSERVER_GET_IFACE(self);
	if (iface->filter_key == NULL)
		return FALSE;
	return iface->filter_key(self, key);
}

/**
 * gowl_input_observer_key_forwarded:
 * @self: a #GowlInputObserver
 * @key: the key event that just reached the focused window
 */
void
gowl_input_observer_key_forwarded(
	GowlInputObserver          *self,
	const GowlInputObserverKey *key
){
	GowlInputObserverInterface *iface;

	g_return_if_fail(GOWL_IS_INPUT_OBSERVER(self));
	g_return_if_fail(key != NULL);

	iface = GOWL_INPUT_OBSERVER_GET_IFACE(self);
	if (iface->key_forwarded != NULL)
		iface->key_forwarded(self, key);
}

/**
 * gowl_input_observer_button_forwarded:
 * @self: a #GowlInputObserver
 * @button: the button event that just reached a window
 */
void
gowl_input_observer_button_forwarded(
	GowlInputObserver             *self,
	const GowlInputObserverButton *button
){
	GowlInputObserverInterface *iface;

	g_return_if_fail(GOWL_IS_INPUT_OBSERVER(self));
	g_return_if_fail(button != NULL);

	iface = GOWL_INPUT_OBSERVER_GET_IFACE(self);
	if (iface->button_forwarded != NULL)
		iface->button_forwarded(self, button);
}
