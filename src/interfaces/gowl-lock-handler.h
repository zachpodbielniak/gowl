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

#ifndef GOWL_LOCK_HANDLER_H
#define GOWL_LOCK_HANDLER_H

#include <glib-object.h>

G_BEGIN_DECLS

#define GOWL_TYPE_LOCK_HANDLER (gowl_lock_handler_get_type())

G_DECLARE_INTERFACE(GowlLockHandler, gowl_lock_handler, GOWL, LOCK_HANDLER, GObject)

/**
 * GowlLockHandlerInterface:
 * @parent_iface: the parent interface
 * @on_lock: called when the session should be locked; create lock surfaces
 * @on_unlock: called after successful authentication; destroy lock surfaces
 * @on_key_input: called for every key event while session is locked;
 *   returns %TRUE if the event was consumed
 * @on_output: called when a monitor is added or its geometry changes during lock
 * @on_output_destroy: called when a monitor is removed during lock
 * @on_activity: called on user input activity while unlocked (for idle timer reset)
 *
 * Interface for modules that provide built-in session lock functionality.
 * A lock handler renders its own lock surfaces, captures keyboard input
 * for password entry, and manages authentication.
 */
struct _GowlLockHandlerInterface {
	GTypeInterface parent_iface;

	void     (*on_lock)           (GowlLockHandler *self,
	                               gpointer         compositor);

	void     (*on_unlock)         (GowlLockHandler *self,
	                               gpointer         compositor);

	gboolean (*on_key_input)      (GowlLockHandler *self,
	                               guint            keysym,
	                               guint32          codepoint,
	                               gboolean         pressed);

	void     (*on_output)         (GowlLockHandler *self,
	                               gpointer         compositor,
	                               gpointer         monitor);

	void     (*on_output_destroy) (GowlLockHandler *self,
	                               gpointer         monitor);

	void     (*on_activity)       (GowlLockHandler *self);
};

/**
 * gowl_lock_handler_on_lock:
 * @self: a #GowlLockHandler
 * @compositor: (nullable): the #GowlCompositor instance
 *
 * Notifies the handler that the session should be locked.
 * The handler should create lock surfaces covering all monitors
 * and prepare for password input.
 */
void gowl_lock_handler_on_lock (GowlLockHandler *self,
                                gpointer         compositor);

/**
 * gowl_lock_handler_on_unlock:
 * @self: a #GowlLockHandler
 * @compositor: (nullable): the #GowlCompositor instance
 *
 * Notifies the handler that the session has been unlocked.
 * The handler should destroy all lock surfaces and release
 * any resources held during the locked state.
 */
void gowl_lock_handler_on_unlock (GowlLockHandler *self,
                                  gpointer         compositor);

/**
 * gowl_lock_handler_on_key_input:
 * @self: a #GowlLockHandler
 * @keysym: the XKB keysym value
 * @codepoint: the Unicode codepoint (0 if not printable)
 * @pressed: %TRUE if key was pressed, %FALSE if released
 *
 * Dispatches a key event to the lock handler while the session
 * is locked.  The handler should use this for password input.
 *
 * Returns: %TRUE if the event was consumed, %FALSE otherwise
 */
gboolean gowl_lock_handler_on_key_input (GowlLockHandler *self,
                                         guint            keysym,
                                         guint32          codepoint,
                                         gboolean         pressed);

/**
 * gowl_lock_handler_on_output:
 * @self: a #GowlLockHandler
 * @compositor: (nullable): the #GowlCompositor instance
 * @monitor: (nullable): the #GowlMonitor being added or changed
 *
 * Notifies the handler that a monitor has been added or its
 * geometry has changed while the session is locked.  The handler
 * should create or update its lock surface for this monitor.
 */
void gowl_lock_handler_on_output (GowlLockHandler *self,
                                  gpointer         compositor,
                                  gpointer         monitor);

/**
 * gowl_lock_handler_on_output_destroy:
 * @self: a #GowlLockHandler
 * @monitor: (nullable): the #GowlMonitor being destroyed
 *
 * Notifies the handler that a monitor is about to be destroyed
 * while the session is locked.  The handler should clean up any
 * lock surface associated with this monitor.
 */
void gowl_lock_handler_on_output_destroy (GowlLockHandler *self,
                                          gpointer         monitor);

/**
 * gowl_lock_handler_on_activity:
 * @self: a #GowlLockHandler
 *
 * Notifies the handler that user input activity occurred while
 * the session is unlocked.  Used to reset idle auto-lock timers.
 */
void gowl_lock_handler_on_activity (GowlLockHandler *self);

G_END_DECLS

#endif /* GOWL_LOCK_HANDLER_H */
