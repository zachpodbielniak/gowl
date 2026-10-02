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
 * gowl-subprocess.h - Run a program and hear back on the compositor thread.
 *
 * A module that wants a program's output -- tesseract reading a
 * screenshot, a speech-to-text command transcribing what was said --
 * cannot wait for it: the compositor's thread IS the desktop.  And it
 * cannot use GSubprocess's async calls either, because under cmacs
 * those complete on the default main context, which is Emacs's thread,
 * where touching the seat or the clipboard races the compositor.
 *
 * So this watches the child's pipes on the compositor's own
 * wl_event_loop and reaps it there.  The callback runs on the thread
 * that owns the event loop, after both pipes reached end of file and
 * the child exited -- or after the deadline, when the child is killed
 * and the callback is told so.
 */

#ifndef GOWL_SUBPROCESS_H
#define GOWL_SUBPROCESS_H

#include <glib.h>

G_BEGIN_DECLS

struct wl_event_loop;

/**
 * GowlSubprocess:
 *
 * A running child, watched on a wl_event_loop.  Opaque.
 */
typedef struct _GowlSubprocess GowlSubprocess;

/**
 * GowlSubprocessDone:
 * @exit_status: the wait status as from waitpid(); use
 *   g_spawn_check_wait_status() or WIFEXITED() on it.  -1 when the
 *   child was killed for running past its deadline.
 * @out: (transfer none): everything the child wrote to stdout
 * @err: (transfer none): everything the child wrote to stderr
 * @user_data: the pointer handed to gowl_subprocess_spawn()
 *
 * Called once, on the event loop's thread, when the child is finished.
 * The #GowlSubprocess is freed after this returns.
 */
typedef void (*GowlSubprocessDone) (gint         exit_status,
                                    const gchar *out,
                                    const gchar *err,
                                    gpointer     user_data);

GowlSubprocess *gowl_subprocess_spawn    (struct wl_event_loop *loop,
                                          const gchar * const  *argv,
                                          guint                 timeout_ms,
                                          GowlSubprocessDone    done,
                                          gpointer              user_data,
                                          GError              **error);
void            gowl_subprocess_signal   (GowlSubprocess       *self,
                                          gint                  signo);
void            gowl_subprocess_cancel   (GowlSubprocess       *self);
GPid            gowl_subprocess_get_pid  (GowlSubprocess       *self);

G_END_DECLS

#endif /* GOWL_SUBPROCESS_H */
