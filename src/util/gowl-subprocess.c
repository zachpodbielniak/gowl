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
 * gowl-subprocess.c - Run a program and hear back on the compositor thread.
 *
 * See the header for why this exists rather than GSubprocess.  The
 * shape: spawn with both output pipes, watch them on the wl_event_loop,
 * and once both are closed reap the child -- polling on a short timer,
 * because a child may close its pipes a moment before it exits.  A
 * deadline timer kills the whole process group when it fires.
 *
 * The child is put in a process group of its own, so a signal reaches
 * a shell pipeline (`sh -c "record | transcribe"') and not only the
 * shell, and its signal mask is cleared: whatever the compositor
 * blocks for its own reasons (the macro watchdog's signal) must not be
 * inherited by a program that expects SIGINT to stop it.
 */

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <wayland-server-core.h>

#include "gowl-subprocess.h"

/* A program that writes more than this is not answering a question.
   The rest is read and thrown away, so the child does not block. */
#define GOWL_SUBPROCESS_MAX_OUTPUT (4 * 1024 * 1024)
/* How often to look for the exit once both pipes are closed. */
#define GOWL_SUBPROCESS_REAP_MS (20)

struct _GowlSubprocess {
	struct wl_event_loop   *loop;
	GPid                    pid;
	gint                    out_fd;
	gint                    err_fd;
	struct wl_event_source *out_source;
	struct wl_event_source *err_source;
	struct wl_event_source *reap_timer;
	struct wl_event_source *deadline;
	GString                *out;
	GString                *err;
	gboolean                timed_out;
	GowlSubprocessDone      done;
	gpointer                user_data;
};

/*
 * child_setup:
 *
 * Runs in the child between fork and exec, so only async-signal-safe
 * calls: a process group of its own, and an empty signal mask.
 */
static void
child_setup(
	gpointer user_data
){
	sigset_t none;

	(void)user_data;
	setpgid(0, 0);
	sigemptyset(&none);
	sigprocmask(SIG_SETMASK, &none, NULL);
}

/* Stops watching one pipe and closes it. */
static void
close_pipe(
	struct wl_event_source **source,
	gint                    *fd
){
	if (*source != NULL) {
		wl_event_source_remove(*source);
		*source = NULL;
	}
	if (*fd >= 0) {
		close(*fd);
		*fd = -1;
	}
}

/* Removes every event source and closes both pipes. */
static void
detach_all(
	GowlSubprocess *self
){
	close_pipe(&self->out_source, &self->out_fd);
	close_pipe(&self->err_source, &self->err_fd);
	if (self->reap_timer != NULL) {
		wl_event_source_remove(self->reap_timer);
		self->reap_timer = NULL;
	}
	if (self->deadline != NULL) {
		wl_event_source_remove(self->deadline);
		self->deadline = NULL;
	}
}

static void
subprocess_free(
	GowlSubprocess *self
){
	detach_all(self);
	g_string_free(self->out, TRUE);
	g_string_free(self->err, TRUE);
	g_free(self);
}

/*
 * try_reap:
 *
 * Called once both pipes are closed.  When the child has exited, hands
 * its output to the callback and frees everything; otherwise arms the
 * reap timer to look again shortly.
 *
 * Returns: %TRUE when the child was reaped (and @self is freed)
 */
static gboolean
try_reap(
	GowlSubprocess *self
){
	gint  status;
	pid_t got;

	if (self->out_fd >= 0 || self->err_fd >= 0)
		return FALSE;

	do {
		got = waitpid(self->pid, &status, WNOHANG);
	} while (got < 0 && errno == EINTR);

	if (got == 0) {
		/* Still running: its pipes closed first.  Look again soon. */
		if (self->reap_timer != NULL)
			wl_event_source_timer_update(self->reap_timer,
			                             GOWL_SUBPROCESS_REAP_MS);
		return FALSE;
	}
	if (got < 0)
		status = -1;   /* someone else reaped it; report a failure */

	if (self->done != NULL)
		self->done(self->timed_out ? -1 : status, self->out->str,
		           self->err->str, self->user_data);
	subprocess_free(self);
	return TRUE;
}

static gint
on_reap_timer(
	gpointer data
){
	try_reap(data);
	return 0;
}

/* The deadline passed: kill the group and let the pipes close. */
static gint
on_deadline(
	gpointer data
){
	GowlSubprocess *self = data;

	self->timed_out = TRUE;
	kill(-self->pid, SIGKILL);
	return 0;
}

/*
 * read_pipe:
 *
 * Drains what is readable from one pipe into @buf.  At end of file (or
 * on an error other than "would block") the pipe is closed and the
 * reaper is given a chance.
 */
static gint
read_pipe(
	GowlSubprocess          *self,
	gint                    *fd,
	struct wl_event_source **source,
	GString                 *buf
){
	gchar   chunk[4096];
	gssize  n;

	for (;;) {
		n = read(*fd, chunk, sizeof chunk);
		if (n > 0) {
			if (buf->len < GOWL_SUBPROCESS_MAX_OUTPUT)
				g_string_append_len(buf, chunk,
					MIN(n, (gssize)(GOWL_SUBPROCESS_MAX_OUTPUT
					                - buf->len)));
			continue;
		}
		if (n < 0 && errno == EINTR)
			continue;
		if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
			return 0;
		break;   /* 0 = end of file, or a real error */
	}
	close_pipe(source, fd);
	try_reap(self);
	return 0;
}

static gint
on_out(
	gint     fd,
	guint32  mask,
	gpointer data
){
	GowlSubprocess *self = data;

	(void)fd;
	(void)mask;
	return read_pipe(self, &self->out_fd, &self->out_source, self->out);
}

static gint
on_err(
	gint     fd,
	guint32  mask,
	gpointer data
){
	GowlSubprocess *self = data;

	(void)fd;
	(void)mask;
	return read_pipe(self, &self->err_fd, &self->err_source, self->err);
}

/**
 * gowl_subprocess_spawn:
 * @loop: the event loop to watch the child on; @done runs on its thread
 * @argv: (array zero-terminated=1): the program and its arguments,
 *   looked up on `$PATH`
 * @timeout_ms: kill the child after this long; 0 for no deadline
 * @done: (scope async): called once when the child is finished
 * @user_data: (closure): passed to @done
 * @error: return location for a spawn failure
 *
 * Starts @argv with stdin from /dev/null and both outputs captured.
 * Nothing blocks: the output is collected as it arrives and @done is
 * called from the event loop when the child is gone.
 *
 * Returns: (transfer none) (nullable): a handle valid until @done has
 *   run or gowl_subprocess_cancel() was called, or %NULL on error
 */
GowlSubprocess *
gowl_subprocess_spawn(
	struct wl_event_loop *loop,
	const gchar * const  *argv,
	guint                 timeout_ms,
	GowlSubprocessDone    done,
	gpointer              user_data,
	GError              **error
){
	GowlSubprocess *self;
	GPid pid;
	gint out_fd;
	gint err_fd;

	g_return_val_if_fail(loop != NULL, NULL);
	g_return_val_if_fail(argv != NULL && argv[0] != NULL, NULL);

	if (!g_spawn_async_with_pipes(NULL, (gchar **)argv, NULL,
	                              G_SPAWN_SEARCH_PATH
	                              | G_SPAWN_DO_NOT_REAP_CHILD
	                              | G_SPAWN_STDIN_FROM_DEV_NULL,
	                              child_setup, NULL, &pid, NULL,
	                              &out_fd, &err_fd, error))
		return NULL;

	fcntl(out_fd, F_SETFL, fcntl(out_fd, F_GETFL) | O_NONBLOCK);
	fcntl(err_fd, F_SETFL, fcntl(err_fd, F_GETFL) | O_NONBLOCK);
	fcntl(out_fd, F_SETFD, FD_CLOEXEC);
	fcntl(err_fd, F_SETFD, FD_CLOEXEC);

	self = g_new0(GowlSubprocess, 1);
	self->loop = loop;
	self->pid = pid;
	self->out_fd = out_fd;
	self->err_fd = err_fd;
	self->out = g_string_new(NULL);
	self->err = g_string_new(NULL);
	self->done = done;
	self->user_data = user_data;

	self->out_source = wl_event_loop_add_fd(loop, out_fd, WL_EVENT_READABLE,
	                                        on_out, self);
	self->err_source = wl_event_loop_add_fd(loop, err_fd, WL_EVENT_READABLE,
	                                        on_err, self);
	self->reap_timer = wl_event_loop_add_timer(loop, on_reap_timer, self);
	if (timeout_ms > 0) {
		self->deadline = wl_event_loop_add_timer(loop, on_deadline, self);
		if (self->deadline != NULL)
			wl_event_source_timer_update(self->deadline,
			                             (gint)MIN(timeout_ms,
			                                       (guint)G_MAXINT));
	}
	return self;
}

/**
 * gowl_subprocess_signal:
 * @self: a running child
 * @signo: the signal
 *
 * Sends @signo to the child's whole process group.  The usual use is
 * SIGINT to a recorder that stops and finishes its work on it; @done
 * still runs when it exits.
 */
void
gowl_subprocess_signal(
	GowlSubprocess *self,
	gint            signo
){
	g_return_if_fail(self != NULL);

	kill(-self->pid, signo);
}

/**
 * gowl_subprocess_cancel:
 * @self: (transfer full): a running child
 *
 * Kills the child's process group, reaps it and frees the handle,
 * without calling @done.  For a module shutting down or an event loop
 * about to be destroyed: nothing is left watching anything.
 */
void
gowl_subprocess_cancel(
	GowlSubprocess *self
){
	pid_t got;

	if (self == NULL)
		return;
	detach_all(self);
	kill(-self->pid, SIGKILL);
	/* SIGKILL cannot be caught, so this wait is short. */
	do {
		got = waitpid(self->pid, NULL, 0);
	} while (got < 0 && errno == EINTR);
	subprocess_free(self);
}

/**
 * gowl_subprocess_get_pid:
 * @self: a running child
 *
 * Returns: the child's process id (also its process group id)
 */
GPid
gowl_subprocess_get_pid(
	GowlSubprocess *self
){
	g_return_val_if_fail(self != NULL, 0);

	return self->pid;
}
