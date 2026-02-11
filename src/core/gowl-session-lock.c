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

#include "gowl-session-lock.h"

/**
 * GowlSessionLock:
 *
 * Manages the ext-session-lock-v1 protocol.  When a lock client
 * (e.g. swaylock) acquires the session lock, all input is routed
 * exclusively to the lock surfaces and normal client rendering is
 * suppressed until the lock is released.
 */
struct _GowlSessionLock {
	GObject   parent_instance;

	gpointer  wlr_lock;   /* struct wlr_session_lock_v1* */
	gboolean  locked;
};

G_DEFINE_FINAL_TYPE(GowlSessionLock, gowl_session_lock, G_TYPE_OBJECT)

/* Signal identifiers */
enum {
	SIGNAL_LOCK,
	SIGNAL_UNLOCK,
	N_SIGNALS
};

static guint lock_signals[N_SIGNALS] = { 0, };

/* --- GObject lifecycle --- */

static void
gowl_session_lock_dispose(GObject *object)
{
	G_OBJECT_CLASS(gowl_session_lock_parent_class)->dispose(object);
}

static void
gowl_session_lock_finalize(GObject *object)
{
	G_OBJECT_CLASS(gowl_session_lock_parent_class)->finalize(object);
}

/* --- class / instance init --- */

static void
gowl_session_lock_class_init(GowlSessionLockClass *klass)
{
	GObjectClass *object_class;

	object_class = G_OBJECT_CLASS(klass);

	object_class->dispose  = gowl_session_lock_dispose;
	object_class->finalize = gowl_session_lock_finalize;

	/**
	 * GowlSessionLock::lock:
	 * @lock: the #GowlSessionLock that emitted the signal
	 *
	 * Emitted when the session is locked.  The compositor should
	 * suppress rendering of normal surfaces and restrict input.
	 */
	lock_signals[SIGNAL_LOCK] =
		g_signal_new("lock",
		             G_TYPE_FROM_CLASS(klass),
		             G_SIGNAL_RUN_LAST,
		             0,
		             NULL, NULL,
		             NULL,
		             G_TYPE_NONE,
		             0);

	/**
	 * GowlSessionLock::unlock:
	 * @lock: the #GowlSessionLock that emitted the signal
	 *
	 * Emitted when the session lock is released.  Normal rendering
	 * and input handling should resume.
	 */
	lock_signals[SIGNAL_UNLOCK] =
		g_signal_new("unlock",
		             G_TYPE_FROM_CLASS(klass),
		             G_SIGNAL_RUN_LAST,
		             0,
		             NULL, NULL,
		             NULL,
		             G_TYPE_NONE,
		             0);
}

static void
gowl_session_lock_init(GowlSessionLock *self)
{
	self->wlr_lock = NULL;
	self->locked   = FALSE;
}

/* --- Public API --- */

/**
 * gowl_session_lock_new:
 *
 * Creates a new #GowlSessionLock in the unlocked state.
 *
 * Returns: (transfer full): a newly allocated #GowlSessionLock
 */
GowlSessionLock *
gowl_session_lock_new(void)
{
	return (GowlSessionLock *)g_object_new(GOWL_TYPE_SESSION_LOCK, NULL);
}

/**
 * gowl_session_lock_is_locked:
 * @self: a #GowlSessionLock
 *
 * Returns whether the session is currently locked.
 *
 * Returns: %TRUE if the session is locked
 */
gboolean
gowl_session_lock_is_locked(GowlSessionLock *self)
{
	g_return_val_if_fail(GOWL_IS_SESSION_LOCK(self), FALSE);

	return self->locked;
}
