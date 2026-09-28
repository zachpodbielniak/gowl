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

#include "barkit/gowl-bar-guard.h"
#include "util/gowl-fault-guard.h"

/**
 * SECTION:gowl-bar-guard
 * @title: Bar fault guard
 * @short_description: makes a faulting plugin callback survivable
 *
 * Bar plugins are in-process, which is what lets them read compositor
 * state directly and draw into the bar's own buffer, and is also what
 * makes a bad one lethal.  The guard narrows that: a fault inside a
 * guarded callback unwinds back to the call site and the host unloads
 * the offender, instead of the session ending.
 *
 * Since macros arrived this is a thin wrapper over the general guard in
 * util/gowl-fault-guard.c, which also gives every thread its own
 * alternate signal stack and offers a watchdog.  The bar asks for no
 * watchdog: its callbacks are expected to be short, and a plugin that
 * blocks is handled by moving it to poll_async, not by killing it.
 */

/**
 * gowl_bar_guard_init:
 *
 * Installs the fault handlers.  Idempotent.
 */
void
gowl_bar_guard_init(void)
{
	gowl_fault_guard_thread_init();
}

/**
 * gowl_bar_guard_is_available:
 *
 * Returns: %TRUE when the handlers are installed
 */
gboolean
gowl_bar_guard_is_available(void)
{
	return gowl_fault_guard_is_available();
}

/**
 * gowl_bar_guard_signal_name:
 * @signo: a signal number
 *
 * Returns: (transfer none): a short name for @signo
 */
const gchar *
gowl_bar_guard_signal_name(gint signo)
{
	return gowl_fault_guard_signal_name(signo);
}

/**
 * gowl_bar_guard_get_fault_count:
 *
 * Returns: how many faults have been caught this session (bar, macros
 *   and everything else the guard protects)
 */
guint
gowl_bar_guard_get_fault_count(void)
{
	return gowl_fault_guard_get_fault_count();
}

/**
 * gowl_bar_guard_call:
 * @func: the callback to run
 * @user_data: passed to @func
 * @signo: (out) (optional): the signal that fired
 * @what: (nullable): a description for the log
 *
 * Returns: %TRUE when @func returned normally
 */
gboolean
gowl_bar_guard_call(
	GowlBarGuardFunc  func,
	gpointer          user_data,
	gint             *signo,
	const gchar      *what
){
	return gowl_fault_guard_call((GowlFaultGuardFunc)func, user_data, 0,
	                             signo, what);
}
