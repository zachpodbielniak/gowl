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

#include "gowl-idle-manager.h"

/**
 * GowlIdleManager:
 *
 * Manages idle detection and idle-inhibit behaviour.  Tracks the
 * idle timeout and current state (active vs idle).  When the idle
 * timeout elapses without user input, the "idle" signal is emitted;
 * when input resumes, the "resume" signal is emitted.  Idle inhibit
 * clients can prevent the transition to idle.
 */
struct _GowlIdleManager {
	GObject   parent_instance;

	gpointer  wlr_idle_notifier;          /* struct wlr_idle_notifier_v1* */
	gpointer  wlr_idle_inhibit_manager;   /* struct wlr_idle_inhibit_manager_v1* */
	gint      timeout_secs;
	gint      state;                       /* 0 = ACTIVE, 1 = IDLE */
};

G_DEFINE_FINAL_TYPE(GowlIdleManager, gowl_idle_manager, G_TYPE_OBJECT)

/* Signal identifiers */
enum {
	SIGNAL_IDLE,
	SIGNAL_RESUME,
	N_SIGNALS
};

static guint idle_signals[N_SIGNALS] = { 0, };

/* --- GObject lifecycle --- */

static void
gowl_idle_manager_dispose(GObject *object)
{
	G_OBJECT_CLASS(gowl_idle_manager_parent_class)->dispose(object);
}

static void
gowl_idle_manager_finalize(GObject *object)
{
	G_OBJECT_CLASS(gowl_idle_manager_parent_class)->finalize(object);
}

/* --- class / instance init --- */

static void
gowl_idle_manager_class_init(GowlIdleManagerClass *klass)
{
	GObjectClass *object_class;

	object_class = G_OBJECT_CLASS(klass);

	object_class->dispose  = gowl_idle_manager_dispose;
	object_class->finalize = gowl_idle_manager_finalize;

	/**
	 * GowlIdleManager::idle:
	 * @manager: the #GowlIdleManager that emitted the signal
	 *
	 * Emitted when the idle timeout elapses and the session
	 * transitions to the idle state.
	 */
	idle_signals[SIGNAL_IDLE] =
		g_signal_new("idle",
		             G_TYPE_FROM_CLASS(klass),
		             G_SIGNAL_RUN_LAST,
		             0,
		             NULL, NULL,
		             NULL,
		             G_TYPE_NONE,
		             0);

	/**
	 * GowlIdleManager::resume:
	 * @manager: the #GowlIdleManager that emitted the signal
	 *
	 * Emitted when user input is received after the session was idle,
	 * transitioning back to the active state.
	 */
	idle_signals[SIGNAL_RESUME] =
		g_signal_new("resume",
		             G_TYPE_FROM_CLASS(klass),
		             G_SIGNAL_RUN_LAST,
		             0,
		             NULL, NULL,
		             NULL,
		             G_TYPE_NONE,
		             0);
}

static void
gowl_idle_manager_init(GowlIdleManager *self)
{
	self->wlr_idle_notifier        = NULL;
	self->wlr_idle_inhibit_manager = NULL;
	self->timeout_secs             = 300;
	self->state                    = 0;
}

/* --- Public API --- */

/**
 * gowl_idle_manager_new:
 *
 * Creates a new #GowlIdleManager with a default timeout of 300
 * seconds and in the active state.
 *
 * Returns: (transfer full): a newly allocated #GowlIdleManager
 */
GowlIdleManager *
gowl_idle_manager_new(void)
{
	return (GowlIdleManager *)g_object_new(GOWL_TYPE_IDLE_MANAGER, NULL);
}

/**
 * gowl_idle_manager_get_state:
 * @self: a #GowlIdleManager
 *
 * Returns the current idle state.  0 means active, 1 means idle.
 *
 * Returns: the state value
 */
gint
gowl_idle_manager_get_state(GowlIdleManager *self)
{
	g_return_val_if_fail(GOWL_IS_IDLE_MANAGER(self), 0);

	return self->state;
}

/**
 * gowl_idle_manager_get_timeout:
 * @self: a #GowlIdleManager
 *
 * Returns the idle timeout in seconds.
 *
 * Returns: the timeout in seconds
 */
gint
gowl_idle_manager_get_timeout(GowlIdleManager *self)
{
	g_return_val_if_fail(GOWL_IS_IDLE_MANAGER(self), 300);

	return self->timeout_secs;
}

/**
 * gowl_idle_manager_set_timeout:
 * @self: a #GowlIdleManager
 * @timeout_secs: the new idle timeout in seconds
 *
 * Sets the idle timeout.  A value of 0 disables idle detection.
 */
void
gowl_idle_manager_set_timeout(
	GowlIdleManager *self,
	gint              timeout_secs
){
	g_return_if_fail(GOWL_IS_IDLE_MANAGER(self));

	self->timeout_secs = timeout_secs;
}
