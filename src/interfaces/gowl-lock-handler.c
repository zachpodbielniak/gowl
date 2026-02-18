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

#include "gowl-lock-handler.h"

G_DEFINE_INTERFACE(GowlLockHandler, gowl_lock_handler, G_TYPE_OBJECT)

static void
gowl_lock_handler_default_init(GowlLockHandlerInterface *iface)
{
	/* Default implementation - no-op */
	(void)iface;
}

/**
 * gowl_lock_handler_on_lock:
 * @self: a #GowlLockHandler
 * @compositor: (nullable): the compositor instance
 *
 * Notifies the handler that the session should be locked.
 * The handler should create lock surfaces and prepare for
 * password input.
 */
void
gowl_lock_handler_on_lock(
	GowlLockHandler *self,
	gpointer         compositor
){
	GowlLockHandlerInterface *iface;

	g_return_if_fail(GOWL_IS_LOCK_HANDLER(self));

	iface = GOWL_LOCK_HANDLER_GET_IFACE(self);
	if (iface->on_lock != NULL)
		iface->on_lock(self, compositor);
}

/**
 * gowl_lock_handler_on_unlock:
 * @self: a #GowlLockHandler
 * @compositor: (nullable): the compositor instance
 *
 * Notifies the handler that the session has been unlocked.
 * The handler should destroy all lock surfaces.
 */
void
gowl_lock_handler_on_unlock(
	GowlLockHandler *self,
	gpointer         compositor
){
	GowlLockHandlerInterface *iface;

	g_return_if_fail(GOWL_IS_LOCK_HANDLER(self));

	iface = GOWL_LOCK_HANDLER_GET_IFACE(self);
	if (iface->on_unlock != NULL)
		iface->on_unlock(self, compositor);
}

/**
 * gowl_lock_handler_on_key_input:
 * @self: a #GowlLockHandler
 * @keysym: the XKB keysym value
 * @codepoint: the Unicode codepoint (0 if not printable)
 * @pressed: %TRUE if key was pressed
 *
 * Dispatches a key event to the lock handler during locked state.
 *
 * Returns: %TRUE if the event was consumed
 */
gboolean
gowl_lock_handler_on_key_input(
	GowlLockHandler *self,
	guint            keysym,
	guint32          codepoint,
	gboolean         pressed
){
	GowlLockHandlerInterface *iface;

	g_return_val_if_fail(GOWL_IS_LOCK_HANDLER(self), FALSE);

	iface = GOWL_LOCK_HANDLER_GET_IFACE(self);
	if (iface->on_key_input != NULL)
		return iface->on_key_input(self, keysym, codepoint, pressed);
	return FALSE;
}

/**
 * gowl_lock_handler_on_output:
 * @self: a #GowlLockHandler
 * @compositor: (nullable): the compositor instance
 * @monitor: (nullable): the monitor being added or changed
 *
 * Notifies the handler that a monitor was added or changed
 * geometry while the session is locked.
 */
void
gowl_lock_handler_on_output(
	GowlLockHandler *self,
	gpointer         compositor,
	gpointer         monitor
){
	GowlLockHandlerInterface *iface;

	g_return_if_fail(GOWL_IS_LOCK_HANDLER(self));

	iface = GOWL_LOCK_HANDLER_GET_IFACE(self);
	if (iface->on_output != NULL)
		iface->on_output(self, compositor, monitor);
}

/**
 * gowl_lock_handler_on_output_destroy:
 * @self: a #GowlLockHandler
 * @monitor: (nullable): the monitor being destroyed
 *
 * Notifies the handler that a monitor is about to be destroyed
 * while the session is locked.
 */
void
gowl_lock_handler_on_output_destroy(
	GowlLockHandler *self,
	gpointer         monitor
){
	GowlLockHandlerInterface *iface;

	g_return_if_fail(GOWL_IS_LOCK_HANDLER(self));

	iface = GOWL_LOCK_HANDLER_GET_IFACE(self);
	if (iface->on_output_destroy != NULL)
		iface->on_output_destroy(self, monitor);
}

/**
 * gowl_lock_handler_on_activity:
 * @self: a #GowlLockHandler
 *
 * Notifies the handler that user input activity occurred
 * while unlocked.  Used to reset idle auto-lock timers.
 */
void
gowl_lock_handler_on_activity(
	GowlLockHandler *self
){
	GowlLockHandlerInterface *iface;

	g_return_if_fail(GOWL_IS_LOCK_HANDLER(self));

	iface = GOWL_LOCK_HANDLER_GET_IFACE(self);
	if (iface->on_activity != NULL)
		iface->on_activity(self);
}
