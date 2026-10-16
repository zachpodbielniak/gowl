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
 * The `wow-gold' widget: World of Warcraft gold, sales and trends via
 * tsmctl.  See docs/bar-widgets/wow-gold.org.
 *
 * tsmctl does the hard part --- reading TradeSkillMaster, Altoholic's
 * DataStore and Syndicator SavedVariables, locally or copied over ssh
 * from the machine that runs the game --- and answers in JSON.  This
 * file only decides WHEN to ask, runs it on a worker, and draws the
 * answer; everything that parses or formats is in bar-wowgold-model.c.
 *
 * Asking is not cheap: each tsmctl call parses every SavedVariables
 * file (about two seconds on a real account), five calls make a
 * reading, and a fetch from the host is an ssh round trip on top.  So:
 *
 *   - every call is on a worker (poll_async or queued work), never on
 *     the compositor thread, which under cmacs --gowl is the editor's;
 *   - a periodic poll reads tsmctl's local CACHE (--offline); only
 *     every `refresh-every' seconds, on the Refresh button, or after a
 *     change of host/filter does the first call of a reading go to the
 *     host, and the other four then read the cache it just filled;
 *   - a failure keeps the last good reading on screen, says why, and
 *     says how old what is showing is.
 */

#include "bar-internal.h"
#include "bar-wowgold-model.h"

#include "barkit/gowl-bar-panel.h"
#include "barkit/gowl-bar-toast.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

/* nf-fa-coins */
#define WG_ICON "\xef\x94\x9e"

/* What one tsmctl call may print before it is stopped.  `get gold' is
   ten kilobytes; this is three orders of magnitude of headroom, and a
   bound on what a broken or hostile command can make the bar hold. */
#define WG_OUT_MAX (8 * 1024 * 1024)
#define WG_ERR_MAX (16 * 1024)

/* Seconds a stopped child gets between SIGTERM and SIGKILL. */
#define WG_KILL_GRACE_S (3)

typedef struct {
	/* Guards everything below.  The worker swaps a fresh reading in
	   while the panel, a click or the configure hook reads the old
	   one on the compositor thread. */
	GMutex          lock;

	/* The last good reading; NULL until the first one. */
	BarWowgoldData *data;

	/* Why the last fetch failed, or NULL.  With a reading on screen
	   it means "this is stale"; without one it is all there is. */
	gchar          *error;
	/* Wall-clock seconds the data on screen was current, while
	   `error' is set; 0 when that is not known. */
	gint64          stale_since;
	/* The data on screen came from tsmctl's cache because the host
	   could not be reached. */
	gboolean        from_cache;

	/* tsmctl is not installed (or not where the setting says). */
	gboolean        missing;
	gchar          *missing_command;

	/* A fetch is running; a Refresh pressed meanwhile sets `pending'
	   and the running fetch goes round once more instead of a second
	   one starting beside it. */
	gboolean        running;
	gboolean        pending;

	/* What is to be fetched (host, filters...), as last configured,
	   and as last fetched.  When they differ the data on screen
	   belongs to another question, and the next poll is NOW. */
	gchar          *want_key;
	gchar          *have_key;

	/* Worker-only: monotonic microseconds of the last time a fetch
	   went to the host, 0 for never; wall-clock seconds of the last
	   time one succeeded. */
	gint64          host_tried_us;
	gint64          host_ok_at;
} WowGold;

/* The settings one fetch runs with, copied: get_setting's pointer is
   only borrowed for the length of a call. */
typedef struct {
	gchar *command;
	gchar *host;
	gchar *realm;
	gchar *character;
	gchar *account;
	gchar *flavor;
	gchar *period;
	guint  sales_count;
	gint   refresh_every;
	gint   timeout;
} WgSettings;

static gchar *
wg_setting_dup(GowlBarPlugin *plugin, const gchar *key, const gchar *fallback)
{
	const gchar *v = gowl_bar_plugin_get_setting(plugin, key);

	return g_strdup((v != NULL && v[0] != '\0') ? v : fallback);
}

static void
wg_settings_read(GowlBarPlugin *plugin, WgSettings *s)
{
	s->command   = wg_setting_dup(plugin, "tsmctl-command", "tsmctl");
	s->host      = wg_setting_dup(plugin, "host", "");
	s->realm     = wg_setting_dup(plugin, "realm", "");
	s->character = wg_setting_dup(plugin, "char", "");
	s->account   = wg_setting_dup(plugin, "account", "");
	s->flavor    = wg_setting_dup(plugin, "flavor", "");
	s->period    = wg_setting_dup(plugin, "period", "30d");
	s->sales_count = (guint)CLAMP(gowl_bar_plugin_get_setting_int(plugin,
		"sales-count", 10), 1, 100);
	s->refresh_every = MAX(gowl_bar_plugin_get_setting_int(plugin,
		"refresh-every", 1800), 60);
	s->timeout = CLAMP(gowl_bar_plugin_get_setting_int(plugin,
		"timeout", 120), 1, 3600);
}

static void
wg_settings_clear(WgSettings *s)
{
	g_free(s->command);
	g_free(s->host);
	g_free(s->realm);
	g_free(s->character);
	g_free(s->account);
	g_free(s->flavor);
	g_free(s->period);
	memset(s, 0, sizeof(*s));
}

static void
wg_settings_args(const WgSettings *s, gboolean offline, BarWowgoldArgs *a)
{
	a->command     = s->command;
	a->host        = s->host;
	a->realm       = s->realm;
	a->character   = s->character;
	a->account     = s->account;
	a->flavor      = s->flavor;
	a->offline     = offline;
	a->period      = s->period;
	a->sales_count = s->sales_count;
}

static gboolean
wg_is_local(const WgSettings *s)
{
	return g_ascii_strcasecmp(s->host, "local") == 0;
}

/* Today's Julian day, local time. */
static guint32
wg_today(void)
{
	g_autoptr(GDateTime) now = g_date_time_new_now_local();
	gchar *iso;
	guint32 day;

	iso = g_date_time_format(now, "%Y-%m-%d");
	day = bar_wowgold_julian(iso);
	g_free(iso);
	return day;
}

static gpointer
wg_create(GowlBarPlugin *plugin)
{
	WowGold *wg;

	(void)plugin;
	wg = g_new0(WowGold, 1);
	g_mutex_init(&wg->lock);
	return wg;
}

static void
wg_destroy(GowlBarPlugin *plugin, gpointer data)
{
	WowGold *wg = data;

	(void)plugin;
	if (wg == NULL)
		return;
	bar_wowgold_data_free(wg->data);
	g_free(wg->error);
	g_free(wg->missing_command);
	g_free(wg->want_key);
	g_free(wg->have_key);
	g_mutex_clear(&wg->lock);
	g_free(wg);
}

/* ----------------------------------------------------------------
 * Running tsmctl (workers only)
 * ---------------------------------------------------------------- */

/* Its own process group, so a timeout stops tsmctl AND the ssh and
   tar it started -- signalling only tsmctl would leave an ssh copying
   SavedVariables into a cache nobody is waiting for. */
static void
wg_child_setup(gpointer user_data)
{
	(void)user_data;
	setpgid(0, 0);
}

static void
wg_set_nonblock(gint fd)
{
	gint flags = fcntl(fd, F_GETFL);

	if (flags >= 0)
		fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

/* Reap @pid, escalating to SIGKILL if SIGTERM was not enough. */
static gint
wg_reap(GPid pid, gboolean signalled)
{
	gint status = 0;
	gint64 give_up;

	if (!signalled) {
		while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
			;
		return status;
	}
	give_up = g_get_monotonic_time() + WG_KILL_GRACE_S * G_USEC_PER_SEC;
	for (;;) {
		pid_t r = waitpid(pid, &status, WNOHANG);

		if (r == pid || (r < 0 && errno != EINTR))
			return status;
		if (g_get_monotonic_time() >= give_up) {
			kill(-pid, SIGKILL);
			while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
				;
			return status;
		}
		g_usleep(50 * 1000);
	}
}

/* The last non-empty line of @text, made safe to show. */
static gchar *
wg_last_line(const gchar *text)
{
	g_autofree gchar *copy = NULL;
	g_autofree gchar *valid = NULL;
	gchar *line, *nl;

	copy = g_strdup(text != NULL ? text : "");
	g_strstrip(copy);
	nl = strrchr(copy, '\n');
	line = (nl != NULL) ? nl + 1 : copy;
	g_strstrip(line);
	valid = g_utf8_make_valid(line, -1);
	if (g_utf8_strlen(valid, -1) > 300) {
		gchar *cut = g_utf8_offset_to_pointer(valid, 300);

		*cut = '\0';
	}
	return g_strdup(valid);
}

/*
 * Run @argv (directly, never through a shell) and collect its stdout.
 * stdout and stderr are drained together with poll() so a child that
 * fills one pipe while we wait on the other cannot deadlock us.  It is
 * reaped here, on this worker --- never through a GLib child watch,
 * which under cmacs would fire on Emacs's thread.
 *
 * Returns TRUE on exit status 0, with @out set; FALSE with @error_msg
 * saying why otherwise.
 */
static gboolean
wg_run(gchar **argv, gint timeout_s, gchar **out, gchar **error_msg)
{
	GPid pid = 0;
	gint out_fd = -1, err_fd = -1, status;
	GError *error = NULL;
	GString *obuf, *ebuf;
	gint64 deadline;
	gboolean timed_out = FALSE, too_big = FALSE;

	*out = NULL;
	*error_msg = NULL;

	if (!g_spawn_async_with_pipes(NULL, argv, NULL,
	                              G_SPAWN_SEARCH_PATH |
	                              G_SPAWN_DO_NOT_REAP_CHILD |
	                              G_SPAWN_STDIN_FROM_DEV_NULL,
	                              wg_child_setup, NULL, &pid, NULL,
	                              &out_fd, &err_fd, &error)) {
		*error_msg = g_strdup_printf("could not run %s: %s", argv[0],
		                             error->message);
		g_clear_error(&error);
		return FALSE;
	}

	wg_set_nonblock(out_fd);
	wg_set_nonblock(err_fd);
	obuf = g_string_new(NULL);
	ebuf = g_string_new(NULL);
	deadline = g_get_monotonic_time() + (gint64)timeout_s * G_USEC_PER_SEC;

	while (out_fd >= 0 || err_fd >= 0) {
		struct pollfd fds[2];
		gint nfds = 0, i, rc;
		gint64 left;

		left = (deadline - g_get_monotonic_time()) / 1000;
		if (left <= 0) {
			timed_out = TRUE;
			break;
		}
		if (out_fd >= 0) {
			fds[nfds].fd = out_fd;
			fds[nfds].events = POLLIN;
			fds[nfds].revents = 0;
			nfds++;
		}
		if (err_fd >= 0) {
			fds[nfds].fd = err_fd;
			fds[nfds].events = POLLIN;
			fds[nfds].revents = 0;
			nfds++;
		}
		rc = poll(fds, (nfds_t)nfds, (gint)MIN(left, 500));
		if (rc < 0 && errno != EINTR)
			break;

		for (i = 0; i < nfds && rc > 0; i++) {
			gchar buf[8192];
			gssize n;

			if (fds[i].revents == 0)
				continue;
			n = read(fds[i].fd, buf, sizeof(buf));
			if (n > 0) {
				if (fds[i].fd == out_fd) {
					g_string_append_len(obuf, buf, n);
					if (obuf->len > WG_OUT_MAX)
						too_big = TRUE;
				} else if (ebuf->len < WG_ERR_MAX) {
					g_string_append_len(ebuf, buf, n);
				}
				continue;
			}
			if (n < 0 && (errno == EAGAIN || errno == EINTR))
				continue;
			if (fds[i].fd == out_fd) {
				close(out_fd);
				out_fd = -1;
			} else {
				close(err_fd);
				err_fd = -1;
			}
		}
		if (too_big)
			break;
	}

	if (out_fd >= 0)
		close(out_fd);
	if (err_fd >= 0)
		close(err_fd);
	if (timed_out || too_big)
		kill(-pid, SIGTERM);
	status = wg_reap(pid, timed_out || too_big);
	g_spawn_close_pid(pid);

	if (timed_out) {
		*error_msg = g_strdup_printf("tsmctl did not finish within "
		                             "%d s and was stopped", timeout_s);
	} else if (too_big) {
		*error_msg = g_strdup("tsmctl printed far more than a report "
		                      "and was stopped");
	} else if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
		g_autofree gchar *last = wg_last_line(ebuf->str);

		if (last[0] != '\0')
			*error_msg = g_steal_pointer(&last);
		else if (WIFEXITED(status))
			*error_msg = g_strdup_printf("tsmctl exited with "
				"status %d", WEXITSTATUS(status));
		else
			*error_msg = g_strdup("tsmctl was killed");
	}

	g_string_free(ebuf, TRUE);
	if (*error_msg != NULL) {
		g_string_free(obuf, TRUE);
		return FALSE;
	}
	*out = g_string_free(obuf, FALSE);
	return TRUE;
}

/* Is the program word of @argv there to run? */
static gboolean
wg_command_present(const gchar *program)
{
	g_autofree gchar *found = NULL;

	if (program == NULL || program[0] == '\0')
		return FALSE;
	if (strchr(program, '/') != NULL)
		return g_file_test(program, G_FILE_TEST_IS_EXECUTABLE) &&
		       !g_file_test(program, G_FILE_TEST_IS_DIR);
	found = g_find_program_in_path(program);
	return found != NULL;
}

/* ----------------------------------------------------------------
 * Presentation
 * ---------------------------------------------------------------- */

static void wg_apply_locked(GowlBarPlugin *plugin, WowGold *wg);

/*
 * Label, icon, colour and tooltip from the current state.  Called from
 * the worker after a fetch and from configure on the compositor thread;
 * the setters are thread-safe and everything else read here is behind
 * the lock.
 */
static void
wg_apply(GowlBarPlugin *plugin, WowGold *wg)
{
	g_mutex_lock(&wg->lock);
	wg_apply_locked(plugin, wg);
	g_mutex_unlock(&wg->lock);
}

static void
wg_apply_locked(GowlBarPlugin *plugin, WowGold *wg)
{
	BarWowgoldMoneyFormat fmt;
	BarWowgoldTrend trend;
	BarWowgoldDirection dir;
	g_autofree gchar *label = NULL;
	g_autofree gchar *tip = NULL;
	const gchar *icon;
	gboolean compact;

	icon = gowl_bar_plugin_get_setting(plugin, "icon");
	gowl_bar_plugin_set_icon(plugin, (icon != NULL && icon[0] != '\0')
	                                 ? icon : WG_ICON);

	if (wg->missing) {
		g_autofree gchar *why = g_strdup_printf(
			"WoW gold: %s not found",
			(wg->missing_command != NULL) ? wg->missing_command
			                              : "tsmctl");

		gowl_bar_plugin_set_label(plugin, NULL);
		gowl_bar_plugin_set_color(plugin, GOWL_BAR_COLOR_OVERLAY);
		gowl_bar_plugin_set_tooltip(plugin, why);
		return;
	}

	if (wg->data == NULL) {
		if (wg->error != NULL) {
			tip = g_strdup_printf("WoW gold: %s", wg->error);
			gowl_bar_plugin_set_label(plugin, "!");
			bar_plugin_apply_color(plugin,
				gowl_bar_plugin_get_setting(plugin,
				                            "color-error"),
				GOWL_BAR_COLOR_RED);
			gowl_bar_plugin_set_tooltip(plugin, tip);
		} else {
			gowl_bar_plugin_set_label(plugin, NULL);
			gowl_bar_plugin_set_color(plugin, GOWL_BAR_COLOR_MUTED);
			gowl_bar_plugin_set_tooltip(plugin,
				"WoW gold: reading your addon data");
		}
		return;
	}

	fmt = bar_wowgold_money_format_parse(
		gowl_bar_plugin_get_setting(plugin, "money-format"));
	compact = (g_strcmp0(gowl_bar_plugin_get_setting(plugin,
		"label-format"), "full") != 0);

	bar_wowgold_trend(wg->data, wg_today(),
		bar_wowgold_period_days(gowl_bar_plugin_get_setting(plugin,
		                                                    "period")),
		&trend);
	label = bar_wowgold_label(wg->data, &trend, compact,
		gowl_bar_plugin_get_setting_bool(plugin, "show-delta", FALSE),
		fmt);
	tip = bar_wowgold_tooltip(wg->data, &trend, fmt,
	                          g_get_real_time() / G_USEC_PER_SEC);
	if (wg->error != NULL) {
		gchar *more = g_strdup_printf("%s\nStale: %s", tip, wg->error);

		g_free(tip);
		tip = more;
	}

	dir = bar_wowgold_direction(&trend);
	switch (dir) {
	case BAR_WOWGOLD_UP:
		bar_plugin_apply_color(plugin,
			gowl_bar_plugin_get_setting(plugin, "color-up"),
			GOWL_BAR_COLOR_GREEN);
		break;
	case BAR_WOWGOLD_DOWN:
		bar_plugin_apply_color(plugin,
			gowl_bar_plugin_get_setting(plugin, "color-down"),
			GOWL_BAR_COLOR_RED);
		break;
	case BAR_WOWGOLD_FLAT:
	default:
		bar_plugin_apply_color(plugin,
			gowl_bar_plugin_get_setting(plugin, "color-flat"),
			GOWL_BAR_COLOR_SUBTEXT);
		break;
	}
	gowl_bar_plugin_set_label(plugin, label);
	gowl_bar_plugin_set_tooltip(plugin, tip);
	bar_wowgold_trend_clear(&trend);
}

/* ----------------------------------------------------------------
 * Fetching (workers only)
 * ---------------------------------------------------------------- */

/* Record a failed fetch.  The reading on screen stays; it is now
   stale, and from when is remembered the first time it goes stale. */
static void
wg_fail(WowGold *wg, const gchar *message)
{
	g_mutex_lock(&wg->lock);
	g_free(wg->error);
	wg->error = g_strdup(message);
	if (wg->stale_since == 0 && wg->data != NULL)
		wg->stale_since = wg->from_cache ? wg->host_ok_at
		                                 : wg->data->fetched_at;
	g_mutex_unlock(&wg->lock);
}

/* One tsmctl call, parsed by @parse into @fresh.  @ran is set when
   tsmctl ran and exited 0, whether or not its output parsed: only a
   call that did not run is a reason to try the cache instead. */
static gboolean
wg_ask(const WgSettings *s, gboolean offline, BarWowgoldCommand command,
       gboolean (*parse)(BarWowgoldData *, const gchar *),
       BarWowgoldData *fresh, const gchar *what, gchar **error_msg,
       gboolean *ran)
{
	BarWowgoldArgs a;
	g_auto(GStrv) argv = NULL;
	g_autofree gchar *out = NULL;
	GError *error = NULL;

	if (ran != NULL)
		*ran = FALSE;
	wg_settings_args(s, offline, &a);
	argv = bar_wowgold_build_argv(&a, command, &error);
	if (argv == NULL) {
		*error_msg = g_strdup(error->message);
		g_clear_error(&error);
		return FALSE;
	}
	if (!wg_run(argv, s->timeout, &out, error_msg))
		return FALSE;
	if (ran != NULL)
		*ran = TRUE;
	if (!parse(fresh, out)) {
		*error_msg = g_strdup_printf("tsmctl %s did not print tsmctl "
		                             "JSON", what);
		return FALSE;
	}
	return TRUE;
}

/*
 * One complete reading.  @to_host: the first call goes to the host
 * (no --offline) to bring tsmctl's cache up to date; the rest always
 * read that cache, so a reading costs one ssh round trip, not five.
 */
static void
wg_fetch_once(GowlBarPlugin *plugin, WowGold *wg, gboolean to_host)
{
	WgSettings s;
	BarWowgoldArgs a;
	g_auto(GStrv) probe = NULL;
	g_autofree gchar *err = NULL;
	g_autofree gchar *host_err = NULL;
	g_autofree gchar *key = NULL;
	BarWowgoldData *fresh, *old;
	GError *error = NULL;
	gboolean local, online, ok, ran = FALSE;
	gint64 now_us, now;

	wg_settings_read(plugin, &s);
	wg_settings_args(&s, FALSE, &a);
	key = bar_wowgold_fetch_key(&a);

	/* Recorded before anything can fail, so a setting that cannot
	   work is tried once per interval and not once a second. */
	g_mutex_lock(&wg->lock);
	g_free(wg->have_key);
	wg->have_key = g_strdup(key);
	g_mutex_unlock(&wg->lock);

	/* A setting that cannot go into an argv (an ssh option dressed
	   as a host, a control character) is reported, not run. */
	probe = bar_wowgold_build_argv(&a, BAR_WOWGOLD_CMD_GOLD, &error);
	if (probe == NULL) {
		wg_fail(wg, error->message);
		g_clear_error(&error);
		wg_settings_clear(&s);
		return;
	}

	g_mutex_lock(&wg->lock);
	wg->missing = !wg_command_present(probe[0]);
	g_free(wg->missing_command);
	wg->missing_command = g_strdup(probe[0]);
	g_mutex_unlock(&wg->lock);
	if (wg->missing) {
		wg_settings_clear(&s);
		return;
	}

	local  = wg_is_local(&s);
	now_us = g_get_monotonic_time();
	online = !local && (to_host || wg->host_tried_us == 0 ||
	         now_us - wg->host_tried_us >=
	             (gint64)s.refresh_every * G_USEC_PER_SEC);
	if (online)
		wg->host_tried_us = now_us;

	fresh = bar_wowgold_data_new();
	ok = wg_ask(&s, !online, BAR_WOWGOLD_CMD_GOLD,
	            bar_wowgold_parse_gold, fresh, "get gold", &err, &ran);
	if (!ok && online && !ran) {
		/* The host is down or asleep; tsmctl's cache may still have
		   last night's copy, which beats a blank widget. */
		host_err = g_steal_pointer(&err);
		ok = wg_ask(&s, TRUE, BAR_WOWGOLD_CMD_GOLD,
		            bar_wowgold_parse_gold, fresh, "get gold", &err,
		            NULL);
	}
	if (ok)
		ok = wg_ask(&s, TRUE, BAR_WOWGOLD_CMD_HISTORY,
		            bar_wowgold_parse_history, fresh,
		            "get gold-history", &err, NULL);
	if (ok)
		ok = wg_ask(&s, TRUE, BAR_WOWGOLD_CMD_SALES,
		            bar_wowgold_parse_sales, fresh, "get sales", &err,
		            NULL);
	if (ok)
		ok = wg_ask(&s, TRUE, BAR_WOWGOLD_CMD_PNL,
		            bar_wowgold_parse_pnl, fresh, "pnl", &err, NULL);
	if (ok) {
		g_autofree gchar *ignored = NULL;

		/* Optional: a tsmctl from before `attention' existed still
		   gives a whole widget, without that one section. */
		if (!wg_ask(&s, TRUE, BAR_WOWGOLD_CMD_ATTENTION,
		            bar_wowgold_parse_attention, fresh, "attention",
		            &ignored, NULL))
			fresh->have_attention = FALSE;
	}

	if (!ok) {
		g_autofree gchar *msg = NULL;

		/* The same failure twice (a broken tsmctl, not a down host)
		   is said once. */
		msg = (host_err != NULL && g_strcmp0(host_err, err) != 0)
		      ? g_strdup_printf("could not reach the host (%s), and "
		                        "the cached copy failed too: %s",
		                        host_err, err)
		      : g_strdup(err);
		bar_wowgold_data_free(fresh);
		wg_fail(wg, msg);
		wg_settings_clear(&s);
		return;
	}

	now = g_get_real_time() / G_USEC_PER_SEC;
	fresh->fetched_at = now;

	g_mutex_lock(&wg->lock);
	old = wg->data;
	wg->data = fresh;
	g_free(wg->error);
	wg->error = NULL;
	if (host_err != NULL) {
		wg->error = g_strdup_printf("could not reach the host: %s. "
		                            "Showing tsmctl's cached copy.",
		                            host_err);
		wg->from_cache = TRUE;
		wg->stale_since = wg->host_ok_at;
	} else {
		if (online)
			wg->host_ok_at = now;
		/* An offline read between host syncs is as fresh as the
		   last sync, which succeeded, so nothing is stale. */
		wg->from_cache = FALSE;
		wg->stale_since = 0;
	}
	g_mutex_unlock(&wg->lock);

	/* Freed outside the lock and after the swap: nobody can reach
	   it any more, and the panel never waits on a free. */
	bar_wowgold_data_free(old);
	wg_settings_clear(&s);
}

/*
 * The fetch entry point for both the periodic poll and the Refresh
 * button.  At most one runs; one that finds another running leaves a
 * note (when it was asked to reach the host) and returns, and the
 * running one goes round again for it.
 */
static void
wg_fetch(GowlBarPlugin *plugin, WowGold *wg, gboolean to_host)
{
	gboolean again;

	g_mutex_lock(&wg->lock);
	if (wg->running) {
		if (to_host)
			wg->pending = TRUE;
		g_mutex_unlock(&wg->lock);
		return;
	}
	wg->running = TRUE;
	/* New host or filters: what is on screen answers a different
	   question, so this fetch goes to the source. */
	if (g_strcmp0(wg->want_key, wg->have_key) != 0)
		to_host = TRUE;
	g_mutex_unlock(&wg->lock);
	gowl_bar_plugin_request_panel_refresh(plugin);

	do {
		wg_fetch_once(plugin, wg, to_host);

		g_mutex_lock(&wg->lock);
		again = wg->pending;
		wg->pending = FALSE;
		if (!again)
			wg->running = FALSE;
		g_mutex_unlock(&wg->lock);
		to_host = TRUE;
	} while (again);

	wg_apply(plugin, wg);
	gowl_bar_plugin_request_panel_refresh(plugin);
}

static void
wg_poll_async(GowlBarPlugin *plugin, gpointer data)
{
	wg_fetch(plugin, data, FALSE);
}

static void
wg_refresh_work(GowlBarPlugin *plugin, gpointer user_data)
{
	wg_fetch(plugin, user_data, TRUE);
}

static gint
wg_interval(GowlBarPlugin *plugin, gpointer data)
{
	WowGold *wg = data;
	gboolean stale_key;

	/* A changed host or filter is polled on the next tick rather
	   than up to five minutes later. */
	g_mutex_lock(&wg->lock);
	stale_key = (g_strcmp0(wg->want_key, wg->have_key) != 0) &&
	            !wg->running;
	g_mutex_unlock(&wg->lock);
	if (stale_key)
		return 1;
	return MAX(gowl_bar_plugin_get_setting_int(plugin, "interval", 300),
	           30);
}

static void
wg_configure(GowlBarPlugin *plugin, gpointer data, GHashTable *settings)
{
	WowGold *wg = data;
	WgSettings s;
	BarWowgoldArgs a;

	(void)settings;
	wg_settings_read(plugin, &s);
	wg_settings_args(&s, FALSE, &a);
	g_mutex_lock(&wg->lock);
	g_free(wg->want_key);
	wg->want_key = bar_wowgold_fetch_key(&a);
	g_mutex_unlock(&wg->lock);
	wg_settings_clear(&s);

	/* Colours, formats and the icon change without a fetch. */
	wg_apply(plugin, wg);
}

/* ----------------------------------------------------------------
 * Panel
 * ---------------------------------------------------------------- */

static void
wg_add_buttons(GowlBarPanel *panel, WowGold *wg, gboolean have_data)
{
	GowlBarPanelItem *row;

	row = gowl_bar_panel_add_buttons(panel, "actions");
	gowl_bar_panel_add_button(row,
		wg->running ? "Refreshing\xe2\x80\xa6" : "Refresh", FALSE);
	gowl_bar_panel_add_button(row, "Open report", FALSE);
	if (have_data)
		gowl_bar_panel_add_button(row, "Copy summary", FALSE);
}

/* "14:05, 3h ago" */
static gchar *
wg_when(gint64 then)
{
	g_autoptr(GDateTime) dt = NULL;
	g_autofree gchar *clock = NULL;
	g_autofree gchar *ago = NULL;

	if (then <= 0)
		return g_strdup("an unknown time");
	dt = g_date_time_new_from_unix_local(then);
	if (dt == NULL)
		return g_strdup("an unknown time");
	clock = g_date_time_format(dt, "%H:%M");
	ago = bar_wowgold_format_ago(then, g_get_real_time() / G_USEC_PER_SEC);
	return g_strdup_printf("%s, %s", clock, ago);
}

static const gchar *
wg_source_name(GowlBarPlugin *plugin)
{
	const gchar *host = gowl_bar_plugin_get_setting(plugin, "host");

	if (host == NULL || host[0] == '\0')
		return "tsmctl's configured source";
	if (g_ascii_strcasecmp(host, "local") == 0)
		return "this machine";
	return host;
}

static void
wg_panel_missing(GowlBarPanel *panel, WowGold *wg)
{
	g_autofree gchar *text = NULL;

	gowl_bar_panel_add_hero(panel, WG_ICON, "tsmctl not found",
	                        "World of Warcraft gold needs it");
	text = g_strdup_printf(
		"This widget reads your TradeSkillMaster, Altoholic and "
		"Syndicator addon data through tsmctl, which could not be "
		"found as \"%s\".  tsmctl is a script from Zach's dotfiles "
		"(~/.dotfiles/bin/scripts/tsmctl), not a Fedora package.  "
		"Put it on the compositor's PATH or set "
		"wow-gold.tsmctl-command to its full path.  It needs "
		"python3, and openssh-clients to read another machine.",
		(wg->missing_command != NULL) ? wg->missing_command
		                              : "tsmctl");
	gowl_bar_panel_add_label(panel, text);
	{
		GowlBarPanelItem *row = gowl_bar_panel_add_buttons(panel,
		                                                   "missing");

		gowl_bar_panel_add_button(row, "Check again", FALSE);
	}
}

static GowlBarPanel *
wg_panel(GowlBarPlugin *plugin, gpointer data)
{
	WowGold *wg = data;
	GowlBarPanel *panel;
	GowlBarPanelItem *item;
	BarWowgoldMoneyFormat fmt;
	BarWowgoldTrend trend;
	const BarWowgoldData *d;
	guint i, days, shown, limit;

	panel = gowl_bar_panel_new();
	gowl_bar_panel_set_width(panel, 440);

	g_mutex_lock(&wg->lock);

	if (wg->missing) {
		wg_panel_missing(panel, wg);
		g_mutex_unlock(&wg->lock);
		return panel;
	}

	if (wg->data == NULL) {
		gowl_bar_panel_add_hero(panel, WG_ICON,
			"World of Warcraft gold",
			wg->running ? "Reading your addon data\xe2\x80\xa6"
			: (wg->error != NULL) ? "No data yet"
			: "Waiting for the first refresh");
		if (wg->error != NULL) {
			g_autofree gchar *why = g_strdup_printf(
				"Last refresh failed: %s", wg->error);

			item = gowl_bar_panel_add_label(panel, why);
			gowl_bar_panel_item_set_color(item, GOWL_BAR_COLOR_RED);
		}
		wg_add_buttons(panel, wg, FALSE);
		g_mutex_unlock(&wg->lock);
		return panel;
	}

	d = wg->data;
	fmt = bar_wowgold_money_format_parse(
		gowl_bar_plugin_get_setting(plugin, "money-format"));
	days = bar_wowgold_period_days(gowl_bar_plugin_get_setting(plugin,
	                                                           "period"));
	bar_wowgold_trend(d, wg_today(), days, &trend);

	/* The headline: the whole fortune and how today went. */
	{
		g_autofree gchar *total = bar_wowgold_format_money(d->total,
		                                                   fmt);
		g_autofree gchar *sub = NULL;

		if (trend.n_samples == 0) {
			sub = g_strdup("No gold history yet");
		} else if (!trend.have_day) {
			sub = g_strdup("No change recorded for today yet");
		} else if (trend.day_delta == 0) {
			sub = g_strdup("No change today");
		} else {
			g_autofree gchar *delta = bar_wowgold_format_delta(
				trend.day_delta, fmt, FALSE);

			sub = g_strdup_printf("%s %s today",
				(trend.day_delta > 0) ? "\xe2\x96\xb2"
				                      : "\xe2\x96\xbc",
				delta);
		}
		item = gowl_bar_panel_add_hero(panel, WG_ICON, total, sub);
		gowl_bar_panel_item_set_color(item, GOWL_BAR_COLOR_YELLOW);
	}

	if (wg->error != NULL) {
		g_autofree gchar *why = g_strdup_printf("Last refresh failed: %s",
		                                        wg->error);
		g_autofree gchar *since = wg_when(wg->stale_since);
		g_autofree gchar *stale = g_strdup_printf("Stale since %s",
		                                          since);

		item = gowl_bar_panel_add_label(panel, why);
		gowl_bar_panel_item_set_color(item, GOWL_BAR_COLOR_RED);
		item = gowl_bar_panel_add_label(panel, stale);
		gowl_bar_panel_item_set_color(item, GOWL_BAR_COLOR_SUBTEXT);
	}

	if (trend.n_samples > 0) {
		g_autofree gchar *title = g_strdup_printf("%u-day trend", days);

		gowl_bar_panel_add_graph(panel, title, trend.samples,
		                         trend.n_samples);
	}

	/* The two closing balances, then the two changes. */
	{
		g_autofree gchar *today = trend.have_today
			? bar_wowgold_format_money(trend.today, fmt)
			: g_strdup("\xe2\x80\x94");
		g_autofree gchar *yday = trend.have_yesterday
			? bar_wowgold_format_money(trend.yesterday, fmt)
			: g_strdup("\xe2\x80\x94");
		g_autofree gchar *week = trend.have_week
			? bar_wowgold_format_delta(trend.week_delta, fmt, FALSE)
			: g_strdup("\xe2\x80\x94");
		g_autofree gchar *period = trend.have_period
			? bar_wowgold_format_delta(trend.period_delta, fmt,
			                           FALSE)
			: g_strdup("\xe2\x80\x94");
		g_autofree gchar *period_label = g_strdup_printf(
			"%u-day change", days);

		gowl_bar_panel_add_field_pair(panel, "Today", today,
		                              "Yesterday", yday);
		gowl_bar_panel_add_field_pair(panel, "7-day change", week,
		                              period_label, period);
	}

	if (d->have_pnl) {
		g_autofree gchar *rev = bar_wowgold_format_money(d->pnl.revenue,
		                                                 fmt);
		g_autofree gchar *spent = bar_wowgold_format_money(
			d->pnl.purchases + d->pnl.expenses, fmt);
		g_autofree gchar *net = bar_wowgold_format_delta(d->pnl.net,
		                                                 fmt, FALSE);

		gowl_bar_panel_add_field_pair(panel, "Sales today", rev,
		                              "Spent today", spent);
		item = gowl_bar_panel_add_field(panel, "Profit today", net);
		if (d->pnl.net != 0)
			gowl_bar_panel_item_set_value_color(item,
				(d->pnl.net > 0) ? GOWL_BAR_COLOR_GREEN
				                 : GOWL_BAR_COLOR_RED);
	}

	/* Last sales, newest first.  A click copies the line. */
	gowl_bar_panel_add_section(panel, "Last sales");
	limit = (guint)CLAMP(gowl_bar_plugin_get_setting_int(plugin,
		"sales-count", 10), 1, 100);
	shown = MIN(limit, d->sales->len);
	for (i = 0; i < shown; i++) {
		const BarWowgoldSale *s = g_ptr_array_index(d->sales, i);
		g_autofree gchar *id = g_strdup_printf("sale:%u", i);
		g_autofree gchar *title = bar_wowgold_sale_title(s);
		g_autofree gchar *ago = bar_wowgold_format_ago(s->when,
			g_get_real_time() / G_USEC_PER_SEC);
		g_autofree gchar *sub = NULL;
		g_autofree gchar *price = bar_wowgold_format_money(s->total,
		                                                   fmt);

		sub = g_strdup_printf("%s%s%s%s%s",
			s->character,
			(s->source[0] != '\0') ? " \xc2\xb7 " : "", s->source,
			(ago[0] != '\0') ? " \xc2\xb7 " : "", ago);
		item = gowl_bar_panel_add_row(panel, id, NULL, title, sub);
		gowl_bar_panel_item_set_value(item, price);
	}
	if (shown == 0)
		gowl_bar_panel_add_label(panel, "No sales recorded.");

	/* Where the gold is.  Zero balances are left out: an account with
	   forty alts has thirty with nothing to say. */
	gowl_bar_panel_add_section(panel, "Characters");
	limit = (guint)CLAMP(gowl_bar_plugin_get_setting_int(plugin,
		"character-count", 10), 1, 200);
	shown = 0;
	for (i = 0; i < d->chars->len; i++) {
		const BarWowgoldChar *c = g_ptr_array_index(d->chars, i);
		g_autofree gchar *id = NULL;
		g_autofree gchar *title = NULL;
		g_autofree gchar *gold = NULL;
		const gchar *sub;

		if (c->copper == 0)
			continue;
		if (shown >= limit) {
			shown++;
			continue;
		}
		id = g_strdup_printf("char:%u", i);
		title = (c->realm[0] != '\0')
		        ? g_strdup_printf("%s-%s", c->name, c->realm)
		        : g_strdup(c->name);
		gold = bar_wowgold_format_money(c->copper, fmt);
		sub = (g_strcmp0(c->kind, "character") != 0 &&
		       c->kind[0] != '\0') ? c->kind : NULL;
		item = gowl_bar_panel_add_row(panel, id, NULL, title, sub);
		gowl_bar_panel_item_set_value(item, gold);
		shown++;
	}
	if (shown > limit) {
		g_autofree gchar *more = g_strdup_printf("+%u more with gold",
		                                         shown - limit);

		gowl_bar_panel_add_label(panel, more);
	} else if (shown == 0) {
		gowl_bar_panel_add_label(panel, "No gold on any character.");
	}

	/* What needs a login, when tsmctl knows. */
	if (d->have_attention && d->attention->len > 0) {
		g_autofree gchar *head = g_strdup_printf("Needs attention (%u)",
		                                         d->attention->len);

		gowl_bar_panel_add_section(panel, head);
		limit = (guint)CLAMP(gowl_bar_plugin_get_setting_int(plugin,
			"attention-count", 8), 1, 100);
		shown = MIN(limit, d->attention->len);
		for (i = 0; i < shown; i++) {
			const BarWowgoldAttention *at =
				g_ptr_array_index(d->attention, i);
			g_autofree gchar *id = g_strdup_printf("attn:%u", i);
			g_autofree gchar *who = NULL;
			g_autofree gchar *value = NULL;

			who = (at->realm[0] != '\0')
			      ? g_strdup_printf("%s-%s", at->character,
			                        at->realm)
			      : g_strdup(at->character);
			value = (at->value > 0)
			        ? bar_wowgold_format_money(at->value, fmt)
			        : g_strdup_printf("\xc3\x97%" G_GINT64_FORMAT,
			                          at->count);
			item = gowl_bar_panel_add_row(panel, id, NULL,
			                              at->what, who);
			gowl_bar_panel_item_set_value(item, value);
		}
	}

	{
		g_autofree gchar *when = wg_when(d->fetched_at);
		g_autofree gchar *foot = g_strdup_printf("Read %s from %s",
			when, wg_source_name(plugin));

		item = gowl_bar_panel_add_label(panel, foot);
		gowl_bar_panel_item_set_color(item, GOWL_BAR_COLOR_MUTED);
	}

	wg_add_buttons(panel, wg, TRUE);
	bar_wowgold_trend_clear(&trend);
	g_mutex_unlock(&wg->lock);
	return panel;
}

/* ----------------------------------------------------------------
 * Actions
 * ---------------------------------------------------------------- */

/* Puts @text on the clipboard: `printf %s TEXT | wl-copy', detached.
   The text is quoted whole, so a character called `$(rm -rf ~)' is
   pasted rather than run. */
static void
wg_copy(GowlBarPlugin *plugin, const gchar *text)
{
	g_autofree gchar *quoted = NULL;
	g_autofree gchar *line = NULL;
	const gchar *copy;

	copy = gowl_bar_plugin_get_setting(plugin, "copy-command");
	quoted = g_shell_quote(text);
	line = g_strdup_printf("printf %%s %s | %s", quoted,
		(copy != NULL && copy[0] != '\0') ? copy : "wl-copy");
	gowl_bar_plugin_spawn(plugin, line);
	gowl_bar_plugin_notify(plugin, GOWL_BAR_TOAST_LOW, "Copied", text);
}

/* The line a clicked row copies, or NULL when the row is gone. */
static gchar *
wg_row_text(GowlBarPlugin *plugin, WowGold *wg, const gchar *item_id)
{
	BarWowgoldMoneyFormat fmt;
	const BarWowgoldData *d = wg->data;
	guint64 index;
	const gchar *num;

	fmt = bar_wowgold_money_format_parse(
		gowl_bar_plugin_get_setting(plugin, "money-format"));
	num = strchr(item_id, ':');
	if (d == NULL || num == NULL)
		return NULL;
	index = g_ascii_strtoull(num + 1, NULL, 10);

	if (g_str_has_prefix(item_id, "sale:") && index < d->sales->len) {
		const BarWowgoldSale *s = g_ptr_array_index(d->sales,
		                                            (guint)index);
		g_autofree gchar *title = bar_wowgold_sale_title(s);
		g_autofree gchar *price = bar_wowgold_format_money(s->total,
		                                                   fmt);

		return g_strdup_printf("%s sold for %s by %s", title, price,
		                       s->character);
	}
	if (g_str_has_prefix(item_id, "char:") && index < d->chars->len) {
		const BarWowgoldChar *c = g_ptr_array_index(d->chars,
		                                            (guint)index);
		g_autofree gchar *gold = bar_wowgold_format_money(c->copper,
		                                                  fmt);

		return (c->realm[0] != '\0')
		       ? g_strdup_printf("%s-%s: %s", c->name, c->realm, gold)
		       : g_strdup_printf("%s: %s", c->name, gold);
	}
	if (g_str_has_prefix(item_id, "attn:") &&
	    index < d->attention->len) {
		const BarWowgoldAttention *a = g_ptr_array_index(d->attention,
		                                                 (guint)index);

		return g_strdup_printf("%s-%s: %s", a->character, a->realm,
		                       a->what);
	}
	return NULL;
}

/*
 * Open the full report.  `report-command' wins when set (under cmacs
 * it can be an `emacsctl eval' that opens an org buffer); otherwise a
 * terminal runs `tsmctl ... report' into a pager.  Every word of the
 * tsmctl argv is quoted, and the whole is quoted again for `sh -c',
 * because the filters are user text.
 */
static void
wg_open_report(GowlBarPlugin *plugin)
{
	const gchar *custom, *term;
	WgSettings s;
	BarWowgoldArgs a;
	g_auto(GStrv) argv = NULL;
	g_autoptr(GString) inner = NULL;
	g_autofree gchar *quoted = NULL;
	g_autofree gchar *line = NULL;
	GError *error = NULL;
	guint i;

	custom = gowl_bar_plugin_get_setting(plugin, "report-command");
	if (custom != NULL && custom[0] != '\0') {
		gowl_bar_plugin_spawn(plugin, custom);
		return;
	}

	wg_settings_read(plugin, &s);
	wg_settings_args(&s, TRUE, &a);
	argv = bar_wowgold_build_argv(&a, BAR_WOWGOLD_CMD_REPORT, &error);
	wg_settings_clear(&s);
	if (argv == NULL) {
		gowl_bar_plugin_notify(plugin, GOWL_BAR_TOAST_NORMAL,
			"Cannot open the report", error->message);
		g_clear_error(&error);
		return;
	}

	inner = g_string_new(NULL);
	for (i = 0; argv[i] != NULL; i++) {
		g_autofree gchar *word = g_shell_quote(argv[i]);

		if (i > 0)
			g_string_append_c(inner, ' ');
		g_string_append(inner, word);
	}
	g_string_append(inner, " | less -R");
	quoted = g_shell_quote(inner->str);

	term = g_getenv("TERMINAL");
	if (term == NULL || term[0] == '\0')
		term = "gst";
	line = g_strdup_printf("%s -e sh -c %s", term, quoted);
	gowl_bar_plugin_spawn(plugin, line);
}

static void
wg_refresh(GowlBarPlugin *plugin, WowGold *wg)
{
	gowl_bar_plugin_queue_work(plugin, wg_refresh_work, wg, NULL);
}

static void
wg_copy_summary(GowlBarPlugin *plugin, WowGold *wg)
{
	g_autofree gchar *line = NULL;
	BarWowgoldTrend trend;

	g_mutex_lock(&wg->lock);
	if (wg->data != NULL) {
		bar_wowgold_trend(wg->data, wg_today(),
			bar_wowgold_period_days(gowl_bar_plugin_get_setting(
				plugin, "period")),
			&trend);
		line = bar_wowgold_summary(wg->data, &trend,
			bar_wowgold_money_format_parse(
				gowl_bar_plugin_get_setting(plugin,
				                            "money-format")));
		bar_wowgold_trend_clear(&trend);
	}
	g_mutex_unlock(&wg->lock);

	if (line == NULL) {
		gowl_bar_plugin_notify(plugin, GOWL_BAR_TOAST_LOW,
			"Nothing to copy yet", "No gold has been read.");
		return;
	}
	wg_copy(plugin, line);
}

static void
wg_action(GowlBarPlugin *plugin, gpointer data, const gchar *item_id,
          gint index, gdouble value, guint button)
{
	WowGold *wg = data;

	(void)value;
	(void)button;

	if (g_strcmp0(item_id, "missing") == 0) {
		wg_refresh(plugin, wg);
		return;
	}
	if (g_strcmp0(item_id, "actions") == 0) {
		switch (index) {
		case 0:
			wg_refresh(plugin, wg);
			break;
		case 1:
			wg_open_report(plugin);
			break;
		case 2:
			wg_copy_summary(plugin, wg);
			break;
		default:
			break;
		}
		return;
	}
	if (item_id != NULL) {
		g_autofree gchar *line = NULL;

		/* Copy the text out from under the lock, then spawn. */
		g_mutex_lock(&wg->lock);
		line = wg_row_text(plugin, wg, item_id);
		g_mutex_unlock(&wg->lock);
		if (line != NULL)
			wg_copy(plugin, line);
	}
}

/* r refreshes, o opens the report, c copies the summary. */
static gboolean
wg_panel_key(GowlBarPlugin *plugin, gpointer data, guint keysym,
             guint modifiers, gint focused_item)
{
	WowGold *wg = data;

	(void)focused_item;
	if (modifiers != 0)
		return FALSE;
	switch (keysym) {
	case 'r':
		wg_refresh(plugin, wg);
		return TRUE;
	case 'o':
		wg_open_report(plugin);
		return TRUE;
	case 'c':
		wg_copy_summary(plugin, wg);
		return TRUE;
	default:
		return FALSE;
	}
}

static const GowlBarPluginVTable wowgold_vtable = {
	sizeof(GowlBarPluginVTable),
	wg_create, wg_destroy,
	NULL, NULL, wg_configure,
	wg_interval, NULL, wg_poll_async,
	NULL, NULL,
	NULL, NULL,
	wg_panel, wg_action,
	NULL, NULL,
	wg_panel_key,
	NULL,
	NULL, NULL
};

/**
 * bar_register_wowgold_plugins:
 * @registry: the registry to add to
 *
 * Registers `wow-gold' (alias `wowgold').
 */
void
bar_register_wowgold_plugins(GowlBarRegistry *registry)
{
	gowl_bar_registry_register_vtable(registry, "wow-gold",
		"WoW gold",
		"World of Warcraft gold, sales and trends from your addons, "
		"via tsmctl",
		&wowgold_vtable);
	gowl_bar_registry_register_alias(registry, "wowgold", "wow-gold");
}
