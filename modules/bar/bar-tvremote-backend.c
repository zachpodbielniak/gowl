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
 * What the tvremote backends share: status and session objects, the
 * kind -> backend table, and a subprocess runner with a deadline.
 */

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include <gio/gio.h>
#include <glib-unix.h>

#include "bar-tvremote-backend.h"

/* The most output a status command is allowed to produce. */
#define RUN_MAX_OUTPUT (4 * 1024 * 1024)

/**
 * bar_tvremote_status_new:
 * @link: the link state
 * @detail: (nullable): the sentence for the panel
 *
 * Returns: (transfer full): a status with nothing else known
 */
BarTvremoteStatus *
bar_tvremote_status_new(BarTvremoteLink link, const gchar *detail)
{
	BarTvremoteStatus *st;

	st = g_new0(BarTvremoteStatus, 1);
	st->link = link;
	st->volume = -1;
	st->detail = g_strdup(detail);
	return st;
}

/**
 * bar_tvremote_status_copy:
 * @st: a status
 *
 * Returns: (transfer full): a deep copy
 */
BarTvremoteStatus *
bar_tvremote_status_copy(const BarTvremoteStatus *st)
{
	BarTvremoteStatus *copy;

	copy = g_new0(BarTvremoteStatus, 1);
	copy->link = st->link;
	copy->app_id = g_strdup(st->app_id);
	copy->app_name = g_strdup(st->app_name);
	copy->title = g_strdup(st->title);
	copy->volume = st->volume;
	copy->muted = st->muted;
	copy->detail = g_strdup(st->detail);
	return copy;
}

/**
 * bar_tvremote_status_free:
 * @st: (nullable): a status
 */
void
bar_tvremote_status_free(BarTvremoteStatus *st)
{
	if (st == NULL)
		return;
	g_free(st->app_id);
	g_free(st->app_name);
	g_free(st->title);
	g_free(st->detail);
	g_free(st);
}

/**
 * bar_tvremote_session_new:
 *
 * Returns: (transfer full): an idle session
 */
BarTvremoteSession *
bar_tvremote_session_new(void)
{
	BarTvremoteSession *s;

	s = g_new0(BarTvremoteSession, 1);
	g_mutex_init(&s->io);
	s->shell_fd = -1;
	return s;
}

/**
 * bar_tvremote_session_stop_shell:
 * @s: a session
 *
 * Stops the persistent adb shell, if any.  Kills exactly the PID this
 * session started and reaps it -- never a pattern.
 */
void
bar_tvremote_session_stop_shell(BarTvremoteSession *s)
{
	if (s->shell_fd >= 0) {
		close(s->shell_fd);
		s->shell_fd = -1;
	}
	if (s->shell_pid > 0) {
		/* Closing its stdin ends `adb shell' on its own; the
		   signal is for one that is stuck connecting. */
		kill(s->shell_pid, SIGTERM);
		waitpid(s->shell_pid, NULL, 0);
		s->shell_pid = 0;
	}
}

/**
 * bar_tvremote_session_reset:
 * @s: a session
 *
 * Drops every connection the session holds -- the adb shell, the
 * websockets -- and forgets the pairing state, so the next call starts
 * from scratch.  Used when a device is edited or forgotten.
 */
void
bar_tvremote_session_reset(BarTvremoteSession *s)
{
	bar_tvremote_session_stop_shell(s);
	g_clear_pointer(&s->ws, bar_tvremote_ws_free);
	g_clear_pointer(&s->pointer, bar_tvremote_ws_free);
	s->ready = FALSE;
	s->adb_state = BAR_TVREMOTE_ADB_ABSENT;
	s->next_connect = 0;
	s->backoff_s = 0;
	s->ws_next_try = 0;
	s->ws_backoff_s = 0;
}

/**
 * bar_tvremote_session_free:
 * @s: (nullable): a session
 */
void
bar_tvremote_session_free(BarTvremoteSession *s)
{
	if (s == NULL)
		return;
	g_mutex_lock(&s->io);
	bar_tvremote_session_reset(s);
	g_mutex_unlock(&s->io);
	g_mutex_clear(&s->io);
	g_free(s);
}

/**
 * bar_tvremote_backend_for:
 * @kind: a kind
 *
 * Returns: (transfer none) (nullable): the backend, %NULL for unknown
 */
const BarTvremoteBackend *
bar_tvremote_backend_for(BarTvremoteKind kind)
{
	switch (kind) {
	case BAR_TVREMOTE_KIND_FIRETV:
	case BAR_TVREMOTE_KIND_ANDROIDTV:
		return &bar_tvremote_adb_backend;
	case BAR_TVREMOTE_KIND_ROKU:
		return &bar_tvremote_roku_backend;
	case BAR_TVREMOTE_KIND_SAMSUNG:
		return &bar_tvremote_samsung_backend;
	case BAR_TVREMOTE_KIND_LG:
		return &bar_tvremote_lg_backend;
	case BAR_TVREMOTE_KIND_CAST:
		return &bar_tvremote_cast_backend;
	case BAR_TVREMOTE_KIND_APPLETV:
		return &bar_tvremote_appletv_backend;
	case BAR_TVREMOTE_KIND_UNKNOWN:
	default:
		return NULL;
	}
}

/**
 * bar_tvremote_have:
 * @command: (nullable): a command name or path
 *
 * Returns: %TRUE when @command resolves to an executable
 */
gboolean
bar_tvremote_have(const gchar *command)
{
	g_autofree gchar *path = NULL;

	if (command == NULL || command[0] == '\0')
		return FALSE;
	path = g_find_program_in_path(command);
	return path != NULL;
}

/**
 * bar_tvremote_tool_package:
 * @tool: a command name or path
 *
 * Returns: (transfer none): how to install @tool on Fedora
 */
const gchar *
bar_tvremote_tool_package(const gchar *tool)
{
	g_autofree gchar *base = g_path_get_basename(tool != NULL ? tool : "");

	if (strcmp(base, "adb") == 0)
		return "the android-tools package";
	if (strcmp(base, "curl") == 0)
		return "the curl package";
	if (strcmp(base, "catt") == 0)
		return "catt (pip install --user catt)";
	if (strcmp(base, "atvremote") == 0)
		return "pyatv (pip install --user pyatv)";
	return "it";
}

/* Read what @fd has into @buf without blocking; FALSE at EOF. */
static gboolean
drain_fd(gint fd, GString *buf)
{
	gchar chunk[4096];

	for (;;) {
		gssize n = read(fd, chunk, sizeof(chunk));

		if (n > 0) {
			if (buf->len < RUN_MAX_OUTPUT)
				g_string_append_len(buf, chunk, n);
			continue;
		}
		if (n == 0)
			return FALSE;
		if (errno == EINTR)
			continue;
		return errno == EAGAIN || errno == EWOULDBLOCK;
	}
}

/**
 * bar_tvremote_run:
 * @argv: (array zero-terminated=1): the command
 * @timeout_ms: the deadline; the child is killed when it passes
 * @out: (out) (optional) (transfer full): its standard output
 * @err: (out) (optional) (transfer full): its standard error
 * @exit_status: (out) (optional): its exit code, or -1 when killed
 * @error: return location for a #GError
 *
 * bar_run_argv() without a deadline is not enough here: `adb connect'
 * to an address with nothing behind it waits for the TCP timeout, and
 * curl to a sleeping TV can hang for as long as the kernel lets it.
 * Each of those would hold a bar worker thread hostage.
 *
 * Stops reading once the child has EXITED even if a pipe is still
 * open: `adb' starts its server daemon on first use, and a daemon that
 * kept the inherited pipe would otherwise keep this waiting until the
 * deadline.  Stdin is /dev/null, so nothing can wait for input.
 *
 * Returns: %TRUE when the command ran to completion (whatever its exit
 *   status), %FALSE with @error when it could not start or timed out
 */
gboolean
bar_tvremote_run(const gchar * const *argv, gint timeout_ms, gchar **out,
                 gchar **err, gint *exit_status, GError **error)
{
	GPid pid = 0;
	gint out_fd = -1, err_fd = -1;
	GString *obuf, *ebuf;
	gint64 deadline;
	gboolean out_open = TRUE, err_open = TRUE;
	gboolean exited = FALSE, timed_out = FALSE;
	gint status = 0;

	if (out != NULL)
		*out = NULL;
	if (err != NULL)
		*err = NULL;
	if (exit_status != NULL)
		*exit_status = -1;

	if (!g_spawn_async_with_pipes(NULL, (gchar **)argv, NULL,
	                              G_SPAWN_SEARCH_PATH |
	                              G_SPAWN_DO_NOT_REAP_CHILD |
	                              G_SPAWN_STDIN_FROM_DEV_NULL |
	                              G_SPAWN_CLOEXEC_PIPES,
	                              NULL, NULL, &pid, NULL, &out_fd, &err_fd,
	                              error))
		return FALSE;

	g_unix_set_fd_nonblocking(out_fd, TRUE, NULL);
	g_unix_set_fd_nonblocking(err_fd, TRUE, NULL);
	obuf = g_string_new(NULL);
	ebuf = g_string_new(NULL);
	deadline = g_get_monotonic_time() + (gint64)timeout_ms * 1000;

	while (!exited || out_open || err_open) {
		struct pollfd fds[2];
		nfds_t n = 0;
		gint64 left;
		pid_t r;

		r = waitpid(pid, &status, WNOHANG);
		if (r == pid || (r < 0 && errno == ECHILD)) {
			exited = TRUE;
			/* Take what is buffered and stop: see above. */
			if (out_open)
				drain_fd(out_fd, obuf);
			if (err_open)
				drain_fd(err_fd, ebuf);
			break;
		}

		left = deadline - g_get_monotonic_time();
		if (left <= 0) {
			timed_out = TRUE;
			break;
		}
		if (out_open) {
			fds[n].fd = out_fd;
			fds[n].events = POLLIN;
			n++;
		}
		if (err_open) {
			fds[n].fd = err_fd;
			fds[n].events = POLLIN;
			n++;
		}
		if (n == 0) {
			/* Both pipes closed; just wait for the exit. */
			g_usleep(5000);
			continue;
		}
		if (poll(fds, n, (gint)MIN(left / 1000 + 1, 50)) <= 0)
			continue;
		if (out_open)
			out_open = drain_fd(out_fd, obuf);
		if (err_open)
			err_open = drain_fd(err_fd, ebuf);
	}

	if (timed_out) {
		kill(pid, SIGKILL);
		waitpid(pid, &status, 0);
	}
	close(out_fd);
	close(err_fd);
	g_spawn_close_pid(pid);

	if (out != NULL)
		*out = g_string_free(obuf, FALSE);
	else
		g_string_free(obuf, TRUE);
	if (err != NULL)
		*err = g_string_free(ebuf, FALSE);
	else
		g_string_free(ebuf, TRUE);

	if (timed_out) {
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT,
		            "%s did not answer in %d ms", argv[0], timeout_ms);
		return FALSE;
	}
	if (exit_status != NULL)
		*exit_status = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
	return TRUE;
}
