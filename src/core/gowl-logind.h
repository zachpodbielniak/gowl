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

/**
 * GowlLogind:
 *
 * The seat side of locking: the three things systemd-logind expects a
 * graphical session to do, and which gowl did none of.
 *
 * 1. Lock before the machine sleeps.  logind announces a suspend with
 *    `PrepareForSleep(true)' and then suspends; a session that merely
 *    listens is racing the kernel and loses, so this takes a *delay*
 *    inhibitor at startup and holds it.  logind will not suspend while
 *    the inhibitor fd is open, so the lock is up before the screen goes
 *    dark and a wake-up lands on the password prompt rather than on the
 *    desktop.  The fd is released as soon as the lock is up (or a short
 *    deadline passes, so a broken lock program cannot wedge suspend),
 *    and taken again on resume.
 *
 * 2. Honour `loginctl lock-session' and `unlock-session', which arrive
 *    as Lock/Unlock signals on this process's own login session object.
 *    This is what a dock, a hotkey daemon, or another seat uses to lock
 *    a session from outside it.
 *
 * 3. Report the state back with `SetLockedHint', so `loginctl
 *    show-session' and anything reading it agree with the screen.  Only
 *    when gowl owns the seat session: a gowl nested inside GNOME shares
 *    GNOME's session object, and would otherwise be telling logind that
 *    GNOME is locked.
 *
 * Threading mirrors the notification daemon: GDBus wants a GMainContext
 * and the compositor's loop is a wl_event_loop, so the bus lives on its
 * own thread and wakes the compositor thread through an eventfd on the
 * Wayland loop.  That makes it work identically under standalone gowl
 * and under `cmacs --gowl', where the compositor thread is the one
 * holding the dispatch lock.
 *
 * Everything is best-effort.  A machine with no logind, or a session
 * this process cannot resolve, logs one line and carries on: the
 * compositor still runs and Super+Shift+l still locks.
 */

#ifndef GOWL_LOGIND_H
#define GOWL_LOGIND_H

#include <glib-object.h>

#include "gowl-types.h"

G_BEGIN_DECLS

#define GOWL_TYPE_LOGIND (gowl_logind_get_type())

G_DECLARE_FINAL_TYPE(GowlLogind, gowl_logind, GOWL, LOGIND, GObject)

/**
 * gowl_logind_new:
 * @compositor: (transfer none): the compositor to lock and unlock
 *
 * Creates the client.  Nothing touches the bus until
 * gowl_logind_start().
 *
 * Returns: (transfer full): a new #GowlLogind
 */
GowlLogind *gowl_logind_new (GowlCompositor *compositor);

/**
 * gowl_logind_start:
 * @self: a #GowlLogind
 * @inhibit_sleep: whether to take the sleep inhibitor and lock on suspend
 *
 * Connects to the system bus on a thread of its own and subscribes.
 * Idempotent.  With @inhibit_sleep %FALSE the Lock/Unlock signals are
 * still honoured -- that is the session asking, not the kernel -- but
 * nothing delays a suspend.
 */
void        gowl_logind_start (GowlLogind *self,
                               gboolean    inhibit_sleep);

/**
 * gowl_logind_stop:
 * @self: a #GowlLogind
 *
 * Drops the inhibitor, stops the bus thread and joins it.  Idempotent,
 * and safe to call from the compositor's shutdown path.
 */
void        gowl_logind_stop  (GowlLogind *self);

/**
 * gowl_logind_set_locked:
 * @self: a #GowlLogind
 * @locked: the state the session is in now
 *
 * Tells the bus thread what the screen is doing.  Two readers: the
 * suspend path, which waits for this to become %TRUE before releasing
 * the inhibitor, and `SetLockedHint'.  Called from the compositor
 * thread.
 */
void        gowl_logind_set_locked (GowlLogind *self,
                                    gboolean    locked);

G_END_DECLS

#endif /* GOWL_LOGIND_H */
