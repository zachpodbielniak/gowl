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
 * Fire TV and Android TV, over adb.
 *
 * The connection is a state machine the status poll drives:
 *
 *   ABSENT --adb connect--> UNAUTHORIZED --user taps Allow--> DEVICE
 *      ^          |                                             |
 *      |       REFUSED (backoff 2,4,8..60 s)        lost: shell dies,
 *      +------------------------------------------- command fails
 *
 * The first `adb connect' to a Fire TV puts "Allow USB debugging?" on
 * the television; until somebody accepts it, get-state answers
 * `unauthorized', and the panel says so in yellow.  Polling continues
 * once a second while that prompt is up so the remote comes alive the
 * moment it is accepted.
 *
 * KEYS go down ONE long-lived `adb -s SERIAL shell' per device, its
 * stdin a socket the worker writes `input keyevent N' lines to.  A
 * fresh `adb shell input keyevent' per key costs a process, a
 * connection and a device-side JVM start -- 300 to 700 ms on a Fire TV
 * Stick, which is the difference between a remote and a slideshow.
 * Down the open shell a key is one write.  The shell is restarted when
 * it dies (the TV slept, the Wi-Fi dropped), at most four times in ten
 * seconds before the device is marked lost and the poll reconnects.
 *
 * The socket rather than a pipe so a write to a dead shell can say
 * EPIPE without raising SIGPIPE in the compositor (MSG_NOSIGNAL exists
 * only for sockets).
 */

#include <errno.h>
#include <stdarg.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include <gio/gio.h>

#include "bar-tvremote-backend.h"

#define ADB_TIMEOUT_MS          (6000)
#define ADB_CONNECT_TIMEOUT_MS  (8000)
#define SHELL_MAX_RESTARTS      (4)
#define SHELL_RESTART_WINDOW_US (10 * G_USEC_PER_SEC)

/* What the status poll asks the device, in ONE round trip: the focused
   app, the music stream's volume, and what is playing.  Constant text;
   nothing from outside is spliced in. */
#define STATUS_SCRIPT \
	"dumpsys window | grep -E 'mCurrentFocus|mFocusedApp'; " \
	"echo ---; " \
	"dumpsys audio | grep -A14 -- '- STREAM_MUSIC:'; " \
	"echo ---; " \
	"dumpsys media_session | grep -E 'description=' | head -5"

#define APPS_SCRIPT "pm list packages -3; echo ---; pm list packages"

static gchar *
device_serial(const BarTvremoteDevice *dev)
{
	return bar_tvremote_adb_serial(dev->host, bar_tvremote_device_port(dev));
}

/* Run `adb ARGS...' with a deadline.  @a1.. are NULL-terminated. */
static gboolean
adb_run(const BarTvremoteTools *t, gint timeout_ms, gchar **out, gchar **err,
        gint *status, GError **error, const gchar *a1, ...)
{
	GPtrArray *argv;
	va_list ap;
	const gchar *a;
	gboolean ok;

	argv = g_ptr_array_new();
	g_ptr_array_add(argv, t->adb);
	va_start(ap, a1);
	for (a = a1; a != NULL; a = va_arg(ap, const gchar *))
		g_ptr_array_add(argv, (gpointer)a);
	va_end(ap);
	g_ptr_array_add(argv, NULL);
	ok = bar_tvremote_run((const gchar * const *)argv->pdata, timeout_ms,
	                      out, err, status, error);
	g_ptr_array_free(argv, TRUE);
	return ok;
}

static BarTvremoteAdbState
adb_get_state(const BarTvremoteTools *t, const gchar *serial)
{
	g_autofree gchar *out = NULL;
	g_autofree gchar *err = NULL;

	if (!adb_run(t, ADB_TIMEOUT_MS, &out, &err, NULL, NULL,
	             "-s", serial, "get-state", NULL))
		return BAR_TVREMOTE_ADB_ABSENT;
	return bar_tvremote_parse_adb_state(out, err);
}

static void
adb_backoff(BarTvremoteSession *s)
{
	s->backoff_s = (s->backoff_s == 0) ? 2 : MIN(s->backoff_s * 2, 60);
	s->next_connect = g_get_monotonic_time() +
	                  (gint64)s->backoff_s * G_USEC_PER_SEC;
}

/*
 * Bring the device to DEVICE if it can be, and return where it got.
 * Cheap when already there (one get-state); backs off after a refused
 * connection so a TV that is unplugged costs one attempt a minute
 * rather than one a second.  The pairing states are never backed off:
 * the user is standing at the TV accepting a prompt.
 */
static BarTvremoteAdbState
adb_ensure(const BarTvremoteDevice *dev, BarTvremoteSession *s,
           const BarTvremoteTools *t)
{
	g_autofree gchar *serial = NULL;
	BarTvremoteAdbState st;

	if (s->adb_state != BAR_TVREMOTE_ADB_DEVICE &&
	    s->adb_state != BAR_TVREMOTE_ADB_UNAUTHORIZED &&
	    s->adb_state != BAR_TVREMOTE_ADB_AUTHORIZING &&
	    g_get_monotonic_time() < s->next_connect)
		return s->adb_state;

	serial = device_serial(dev);
	st = adb_get_state(t, serial);

	if (st == BAR_TVREMOTE_ADB_OFFLINE) {
		/* A stale transport -- the TV slept and woke with the same
		   address.  adb keeps reporting it offline until it is
		   dropped and made again. */
		adb_run(t, ADB_TIMEOUT_MS, NULL, NULL, NULL, NULL,
		        "disconnect", serial, NULL);
	}
	if (st == BAR_TVREMOTE_ADB_ABSENT || st == BAR_TVREMOTE_ADB_OFFLINE) {
		g_autofree gchar *out = NULL;
		g_autofree gchar *err = NULL;
		BarTvremoteAdbState cst = BAR_TVREMOTE_ADB_REFUSED;

		if (adb_run(t, ADB_CONNECT_TIMEOUT_MS, &out, &err, NULL, NULL,
		            "connect", serial, NULL))
			cst = bar_tvremote_parse_adb_connect(out, err);
		if (cst == BAR_TVREMOTE_ADB_REFUSED) {
			adb_backoff(s);
			s->adb_state = BAR_TVREMOTE_ADB_REFUSED;
			bar_tvremote_session_stop_shell(s);
			return s->adb_state;
		}
		st = adb_get_state(t, serial);
		if (cst == BAR_TVREMOTE_ADB_UNAUTHORIZED &&
		    st == BAR_TVREMOTE_ADB_ABSENT)
			st = BAR_TVREMOTE_ADB_UNAUTHORIZED;
	}

	if (st == BAR_TVREMOTE_ADB_DEVICE) {
		s->backoff_s = 0;
		s->next_connect = 0;
	} else if (st == BAR_TVREMOTE_ADB_ABSENT ||
	           st == BAR_TVREMOTE_ADB_OFFLINE) {
		adb_backoff(s);
	}
	s->adb_state = st;
	if (st != BAR_TVREMOTE_ADB_DEVICE)
		bar_tvremote_session_stop_shell(s);
	return st;
}

/* Whether the persistent shell is still running; reaps it if not. */
static gboolean
shell_alive(BarTvremoteSession *s)
{
	pid_t r;

	if (s->shell_pid <= 0 || s->shell_fd < 0)
		return FALSE;
	r = waitpid(s->shell_pid, NULL, WNOHANG);
	if (r == 0)
		return TRUE;
	/* Exited (reaped now) or already reaped by someone else. */
	s->shell_pid = 0;
	close(s->shell_fd);
	s->shell_fd = -1;
	return FALSE;
}

static gboolean
shell_spawn(const BarTvremoteDevice *dev, BarTvremoteSession *s,
            const BarTvremoteTools *t, GError **error)
{
	g_autofree gchar *serial = NULL;
	const gchar *argv[6];
	gint sv[2];
	GPid pid = 0;
	gint64 now;

	/* A shell that dies as soon as it starts means the device is not
	   really there; respawning it in a loop would just burn CPU. */
	now = g_get_monotonic_time();
	if (now - s->restart_window > SHELL_RESTART_WINDOW_US) {
		s->restart_window = now;
		s->restarts = 0;
	}
	if (s->restarts >= SHELL_MAX_RESTARTS) {
		s->adb_state = BAR_TVREMOTE_ADB_ABSENT;
		s->next_connect = 0;
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_CONNECTED,
		            "the adb shell to %s keeps exiting; reconnecting",
		            dev->host);
		return FALSE;
	}
	s->restarts++;

	if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) != 0) {
		g_set_error(error, G_IO_ERROR, g_io_error_from_errno(errno),
		            "socketpair: %s", g_strerror(errno));
		return FALSE;
	}
	serial = device_serial(dev);
	argv[0] = t->adb;
	argv[1] = "-s";
	argv[2] = serial;
	argv[3] = "shell";
	argv[4] = NULL;
	if (!g_spawn_async_with_pipes_and_fds(NULL, argv, NULL,
	                                      G_SPAWN_SEARCH_PATH |
	                                      G_SPAWN_DO_NOT_REAP_CHILD |
	                                      G_SPAWN_STDOUT_TO_DEV_NULL |
	                                      G_SPAWN_STDERR_TO_DEV_NULL,
	                                      NULL, NULL, sv[1], -1, -1,
	                                      NULL, NULL, 0, &pid,
	                                      NULL, NULL, NULL, error)) {
		close(sv[0]);
		close(sv[1]);
		return FALSE;
	}
	close(sv[1]);
	s->shell_fd = sv[0];
	s->shell_pid = pid;
	s->spawns++;
	return TRUE;
}

/* Write one line down the persistent shell, (re)starting it as
   needed: one retry, for a shell that died since the last key. */
static gboolean
shell_send(const BarTvremoteDevice *dev, BarTvremoteSession *s,
           const BarTvremoteTools *t, const gchar *line, GError **error)
{
	g_autofree gchar *msg = g_strconcat(line, "\n", NULL);
	gsize len = strlen(msg);
	guint attempt;

	for (attempt = 0; attempt < 2; attempt++) {
		gsize done = 0;

		if (!shell_alive(s) && !shell_spawn(dev, s, t, error))
			return FALSE;
		while (done < len) {
			gssize n = send(s->shell_fd, msg + done, len - done,
			                MSG_NOSIGNAL);

			if (n < 0 && errno == EINTR)
				continue;
			if (n <= 0)
				break;
			done += (gsize)n;
		}
		if (done == len)
			return TRUE;
		bar_tvremote_session_stop_shell(s);
	}
	s->adb_state = BAR_TVREMOTE_ADB_ABSENT;
	s->next_connect = 0;
	g_set_error(error, G_IO_ERROR, G_IO_ERROR_BROKEN_PIPE,
	            "lost the adb shell to %s", dev->host);
	return FALSE;
}

/* The sentence for a device that is not usable, or NULL when it is. */
static gchar *
state_problem(const BarTvremoteDevice *dev, BarTvremoteAdbState st)
{
	gint port = bar_tvremote_device_port(dev);

	switch (st) {
	case BAR_TVREMOTE_ADB_DEVICE:
		return NULL;
	case BAR_TVREMOTE_ADB_UNAUTHORIZED:
	case BAR_TVREMOTE_ADB_AUTHORIZING:
		return g_strdup("Accept the \xe2\x80\x9c" "Allow USB debugging?"
		                "\xe2\x80\x9d prompt on your TV (tick "
		                "\xe2\x80\x9c" "Always allow" "\xe2\x80\x9d)");
	case BAR_TVREMOTE_ADB_REFUSED:
		return g_strdup_printf("Nothing answered on %s:%d. Turn on ADB "
		                       "debugging in the TV's Developer options.",
		                       dev->host, port);
	case BAR_TVREMOTE_ADB_OFFLINE:
	case BAR_TVREMOTE_ADB_ABSENT:
	default:
		return g_strdup_printf("Cannot reach %s:%d; retrying",
		                       dev->host, port);
	}
}

/* Bring the device up for a command, or say why not. */
static gboolean
adb_require_device(const BarTvremoteDevice *dev, BarTvremoteSession *s,
                   const BarTvremoteTools *t, GError **error)
{
	g_autofree gchar *why = NULL;

	if (!bar_tvremote_have(t->adb)) {
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
		            "adb is not installed: install %s",
		            bar_tvremote_tool_package(t->adb));
		return FALSE;
	}
	if (s->adb_state == BAR_TVREMOTE_ADB_DEVICE)
		return TRUE;
	why = state_problem(dev, adb_ensure(dev, s, t));
	if (why == NULL)
		return TRUE;
	g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_CONNECTED, "%s", why);
	return FALSE;
}

static gboolean
adb_key(const BarTvremoteDevice *dev, BarTvremoteSession *s,
        const BarTvremoteTools *t, BarTvremoteKey key, gboolean longpress,
        GError **error)
{
	g_autofree gchar *line = NULL;

	line = bar_tvremote_adb_key_command(dev->kind, key, longpress);
	if (line == NULL) {
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
		            "%s has no %s key", bar_tvremote_kind_title(dev->kind),
		            bar_tvremote_key_name(key));
		return FALSE;
	}
	if (!adb_require_device(dev, s, t, error))
		return FALSE;
	return shell_send(dev, s, t, line, error);
}

static gboolean
adb_text(const BarTvremoteDevice *dev, BarTvremoteSession *s,
         const BarTvremoteTools *t, const gchar *text, guint *dropped,
         GError **error)
{
	g_auto(GStrv) lines = NULL;
	guint i;

	lines = bar_tvremote_adb_text_commands(text, dropped);
	if (lines[0] == NULL) {
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
		            "nothing typeable: the TV's text input takes plain "
		            "ASCII only");
		return FALSE;
	}
	if (!adb_require_device(dev, s, t, error))
		return FALSE;
	for (i = 0; lines[i] != NULL; i++)
		if (!shell_send(dev, s, t, lines[i], error))
			return FALSE;
	return TRUE;
}

static BarTvremoteStatus *
adb_status(const BarTvremoteDevice *dev, BarTvremoteSession *s,
           const BarTvremoteTools *t)
{
	BarTvremoteStatus *st;
	BarTvremoteAdbState state;
	g_autofree gchar *serial = NULL;
	g_autofree gchar *out = NULL;
	g_autofree gchar *why = NULL;
	g_autofree gchar *pkg = NULL;
	gint code = -1, index, max;
	gboolean muted;

	if (!bar_tvremote_have(t->adb)) {
		g_autofree gchar *msg = NULL;

		msg = g_strdup_printf("adb is not installed: install %s",
		                      bar_tvremote_tool_package(t->adb));
		return bar_tvremote_status_new(BAR_TVREMOTE_LINK_NO_TOOL, msg);
	}

	state = adb_ensure(dev, s, t);
	why = state_problem(dev, state);
	if (why != NULL)
		return bar_tvremote_status_new(
			(state == BAR_TVREMOTE_ADB_UNAUTHORIZED ||
			 state == BAR_TVREMOTE_ADB_AUTHORIZING)
				? BAR_TVREMOTE_LINK_PAIRING
				: BAR_TVREMOTE_LINK_OFFLINE, why);

	serial = device_serial(dev);
	if (!adb_run(t, ADB_TIMEOUT_MS, &out, NULL, &code, NULL,
	             "-s", serial, "shell", STATUS_SCRIPT, NULL) ||
	    code != 0) {
		/* The transport went away between get-state and now. */
		s->adb_state = BAR_TVREMOTE_ADB_ABSENT;
		s->next_connect = 0;
		bar_tvremote_session_stop_shell(s);
		return bar_tvremote_status_new(BAR_TVREMOTE_LINK_OFFLINE,
			"Lost the connection; reconnecting");
	}

	st = bar_tvremote_status_new(BAR_TVREMOTE_LINK_ONLINE, NULL);
	pkg = bar_tvremote_parse_focus(out);
	if (pkg != NULL) {
		const gchar *name;

		name = bar_tvremote_app_friendly_name(dev->kind, pkg);
		st->app_id = g_strdup(pkg);
		st->app_name = g_strdup(name != NULL ? name : pkg);
	}
	if (bar_tvremote_parse_audio_volume(out, &index, &max, &muted)) {
		st->volume = bar_tvremote_volume_percent(index, max);
		st->muted = muted;
	}
	st->title = bar_tvremote_parse_media_title(out);
	return st;
}

static GPtrArray *
adb_apps(const BarTvremoteDevice *dev, BarTvremoteSession *s,
         const BarTvremoteTools *t, GError **error)
{
	g_autofree gchar *serial = NULL;
	g_autofree gchar *out = NULL;
	g_autofree gchar *third = NULL;
	gchar *sep;
	gint code = -1;

	if (!adb_require_device(dev, s, t, error))
		return NULL;
	serial = device_serial(dev);
	if (!adb_run(t, ADB_TIMEOUT_MS + 4000, &out, NULL, &code, error,
	             "-s", serial, "shell", APPS_SCRIPT, NULL))
		return NULL;
	if (code != 0) {
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
		            "the TV would not list its apps");
		return NULL;
	}
	sep = strstr(out, "---");
	if (sep == NULL)
		return bar_tvremote_adb_apps(dev->kind, out, NULL);
	third = g_strndup(out, (gsize)(sep - out));
	return bar_tvremote_adb_apps(dev->kind, third, sep + 3);
}

static gboolean
adb_launch(const BarTvremoteDevice *dev, BarTvremoteSession *s,
           const BarTvremoteTools *t, const gchar *app_id, GError **error)
{
	g_autofree gchar *serial = NULL;
	g_autofree gchar *line = NULL;
	g_autofree gchar *out = NULL;
	guint attempt;

	if (!bar_tvremote_valid_package(app_id)) {
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
		            "not an Android package name");
		return FALSE;
	}
	if (!adb_require_device(dev, s, t, error))
		return FALSE;
	serial = device_serial(dev);

	/* LEANBACK_LAUNCHER first, the plain launcher second. */
	for (attempt = 0; attempt < 2; attempt++) {
		g_clear_pointer(&line, g_free);
		g_clear_pointer(&out, g_free);
		line = bar_tvremote_adb_launch_command(app_id, attempt == 0);
		if (!adb_run(t, ADB_TIMEOUT_MS, &out, NULL, NULL, error,
		             "-s", serial, "shell", line, NULL))
			return FALSE;
		if (strstr(out, "No activities found") == NULL)
			return TRUE;
	}
	g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
	            "%s has nothing to launch", app_id);
	return FALSE;
}

const BarTvremoteBackend bar_tvremote_adb_backend = {
	adb_key,
	adb_text,
	adb_status,
	adb_apps,
	adb_launch
};
