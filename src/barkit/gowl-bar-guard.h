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

#ifndef GOWL_BAR_GUARD_H
#define GOWL_BAR_GUARD_H

#include <glib.h>

G_BEGIN_DECLS

/**
 * GowlBarGuardFunc:
 * @user_data: the pointer handed to gowl_bar_guard_call()
 *
 * A callback to run under the fault guard.
 */
typedef void (*GowlBarGuardFunc) (gpointer user_data);

/**
 * gowl_bar_guard_init:
 *
 * Installs the fault handlers and an alternate signal stack.  Safe to
 * call repeatedly and from any thread; only the first call does work.
 *
 * The guard exists because bar plugins run inside the compositor, and
 * the compositor is the user's whole session.  A plugin that
 * dereferences NULL in its draw callback would otherwise take the
 * desktop with it.  Under the guard that fault unwinds to the calling
 * frame, the plugin is unloaded, and the session keeps running.
 *
 * The honest limits: this recovers a fault whose damage was confined
 * to the faulting call.  A plugin that corrupts the heap and crashes
 * later, elsewhere, is not recoverable by any in-process mechanism,
 * and the load journal --- see gowl_bar_registry_quarantine() --- is
 * what catches that case on the next start.  Outside a guarded call
 * the previous handler runs unchanged, so genuine compositor crashes
 * still produce a core dump.
 */
void gowl_bar_guard_init (void);

/**
 * gowl_bar_guard_is_available:
 *
 * Returns: %TRUE when the handlers were installed successfully
 */
gboolean gowl_bar_guard_is_available (void);

/**
 * gowl_bar_guard_call:
 * @func: the callback to run
 * @user_data: passed to @func
 * @signo: (out) (optional): the signal that fired, when one did
 * @what: (nullable): a short description used in the warning logged on
 *   a fault, e.g. `plugin "spotify" draw'
 *
 * Runs @func with the fault guard armed for the calling thread.
 *
 * Guards do not nest: a nested call runs @func directly so an inner
 * frame cannot steal the outer frame's landing point.  The caller must
 * treat a %FALSE return as "this plugin is not trustworthy any more"
 * and unload it --- the guard makes no attempt to release whatever the
 * callback was holding when it faulted.
 *
 * Returns: %TRUE when @func returned normally, %FALSE when a fatal
 *   signal was caught
 */
gboolean gowl_bar_guard_call (GowlBarGuardFunc  func,
                               gpointer          user_data,
                               gint             *signo,
                               const gchar      *what);

/**
 * gowl_bar_guard_signal_name:
 * @signo: a signal number
 *
 * Returns: (transfer none): a short name for @signo
 */
const gchar *gowl_bar_guard_signal_name (gint signo);

/**
 * gowl_bar_guard_get_fault_count:
 *
 * Returns: how many faults the guard has caught this session
 */
guint gowl_bar_guard_get_fault_count (void);

G_END_DECLS

#endif /* GOWL_BAR_GUARD_H */
