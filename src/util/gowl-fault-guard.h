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

/*
 * gowl-fault-guard.h - Survive a faulting (or runaway) in-process call.
 *
 * gowl runs user code inside the compositor: bar plugins, macros, C
 * config callbacks.  The compositor is the user's whole session, so a
 * NULL dereference or an endless loop in that code would take the
 * desktop with it.  The guard narrows that: a fatal signal -- or the
 * watchdog firing -- inside a guarded call unwinds back to the call
 * site, and the caller decides what to do with the offender.
 *
 * This is containment, not a sandbox.  It recovers a fault whose
 * damage was confined to the call; it releases nothing the call held
 * (a lock taken inside it stays taken), and a call that corrupted the
 * heap and crashes later, elsewhere, is beyond any in-process remedy.
 * Outside a guarded call the previous handlers run unchanged -- so a
 * genuine compositor crash still dumps core, and under cmacs Emacs's
 * own SIGSEGV handling is untouched.
 */

#ifndef GOWL_FAULT_GUARD_H
#define GOWL_FAULT_GUARD_H

#include <glib.h>

G_BEGIN_DECLS

/**
 * GowlFaultGuardFunc:
 * @user_data: the pointer handed to gowl_fault_guard_call()
 *
 * A callback to run under the fault guard.
 */
typedef void (*GowlFaultGuardFunc) (gpointer user_data);

void         gowl_fault_guard_init              (void);
gboolean     gowl_fault_guard_is_available      (void);
void         gowl_fault_guard_thread_init       (void);
gboolean     gowl_fault_guard_call              (GowlFaultGuardFunc  func,
                                                 gpointer            user_data,
                                                 guint               timeout_ms,
                                                 gint               *signo,
                                                 const gchar        *what);
gboolean     gowl_fault_guard_extend            (guint               timeout_ms);
void         gowl_fault_guard_hold              (void);
void         gowl_fault_guard_release           (void);
gboolean     gowl_fault_guard_is_armed          (void);
gint         gowl_fault_guard_watchdog_signal   (void);
gboolean     gowl_fault_guard_is_watchdog       (gint                signo);
const gchar *gowl_fault_guard_signal_name       (gint                signo);
guint        gowl_fault_guard_get_fault_count   (void);

G_END_DECLS

#endif /* GOWL_FAULT_GUARD_H */
