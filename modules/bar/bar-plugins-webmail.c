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
 * SECTION:bar-plugins-webmail
 * @short_description: a themed web mail window behind a bar icon
 *
 * The `webmail' widget (aliases `outlook', `mail-web').  A mail glyph
 * in the bar that owns one web mail window: click to start it, click
 * again to hide it, click again to bring it back.  The window is a
 * gsurf kiosk running a configuration this file generates, so it gets
 *
 *  - its own cookie jar under $XDG_DATA_HOME/gowl-webmail, apart from
 *    the browser the user browses with;
 *  - a stylesheet made from the bar's palette (Fluent UI tokens for
 *    Outlook, a generic set for anything else), rewritten when the
 *    palette changes;
 *  - a userscript that writes the unread count into the window title,
 *    where the bar reads it back off the compositor's client list.
 *
 * Who does what, and on which thread:
 *
 *  - Writing files and starting the browser are LAUNCH JOBS run with
 *    gowl_bar_plugin_queue_work() on a worker.  The browser is started
 *    with g_spawn_async() rather than gowl_bar_plugin_spawn() because
 *    the widget needs its PID: that is how it finds its window among
 *    the user's other gsurf windows, which share an app-id, and how it
 *    knows the window is still alive.  GLib double-forks, so the PID
 *    is the browser's own and it is reaped by init, never by us.
 *  - Everything that touches the compositor -- finding the window,
 *    minimizing, focusing -- happens in the poll, a click or a panel
 *    action, all of which run on the compositor thread holding the
 *    gowl lock.  The compositor's own minimize and focus calls are
 *    used, so focus still goes through gowl_focus_decide().
 */

#include <errno.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <glib/gstdio.h>
#include <linux/input-event-codes.h>
#include <wlr/types/wlr_keyboard.h>

#include "core/gowl-compositor.h"
#include "core/gowl-client.h"
#include "core/gowl-monitor.h"
#include "barkit/gowl-bar-host.h"
#include "barkit/gowl-bar-panel.h"
#include "barkit/gowl-bar-theme.h"

#include "bar-internal.h"
#include "bar-webmail-model.h"

/* Glyphs (Font Awesome code points, as in every Nerd Font). */
#define WM_ICON_MAIL     "\xef\x83\xa0"   /* U+F0E0 envelope */
#define WM_ICON_RELOAD   "\xef\x80\xa1"   /* U+F021 refresh  */
#define WM_ICON_WARN     "\xef\x81\xb1"   /* U+F071 warning  */

#define WM_DEFAULT_URL     "https://outlook.cloud.microsoft/mail/"
#define WM_DEFAULT_COMMAND "gsurf --kiosk --no-c-config -c {config} {url}"
#define WM_DEFAULT_APP_ID  "gsurf"

/* Bounds on what is read from outside. */
#define WM_MAX_TITLE       (512)
#define WM_MAX_EXTRA_CSS   (256 * 1024)

/* A window that has not mapped this long after a launch is probably
   one the widget cannot recognise (a changed browser-command). */
#define WM_MAP_PATIENCE    (20 * G_TIME_SPAN_SECOND)

/* run-in-background's brake: this many exits, each within
   WM_QUICK_DEATH of its launch, and it stops relaunching. */
#define WM_QUICK_DEATH     (15 * G_TIME_SPAN_SECOND)
#define WM_MAX_QUICK_DEATHS (3)

typedef struct {
	/*
	 * Guards the fields up to `last_have_check': the launch worker
	 * writes them while the compositor thread reads them for the
	 * poll, the panel and the clicks.  Everything after is touched
	 * on the compositor thread only.
	 */
	GMutex     lock;
	GPid       pid;           /* the browser we started, 0 for none */
	gboolean   launching;     /* a launch job is queued or running  */
	gboolean   missing;       /* the browser command is not on PATH */
	gchar     *missing_cmd;
	gchar     *last_error;
	gchar     *running_sig;   /* the theme the live window loaded   */
	gchar     *written_sig;   /* the theme in the files on disk     */
	gint64     last_have_check; /* when the worker last looked for the
	                               browser; 0 asks it to look again  */

	gint64     launched_at;
	gboolean   user_quit;     /* the exit was asked for: no relaunch */
	gboolean   hide_on_map;   /* minimize the window when it maps   */
	gboolean   restored;      /* the PID file has been looked at    */
	gboolean   autostarted;
	guint      client_id;     /* the window, by its never-reused id */
	BarWebmailWindow state;
	gchar     *title;         /* the window title, as shown         */
	gint       unread;
	gboolean   plus;
	gchar     *sig;           /* the theme signature last computed  */
	gint64     quit_armed_at;
	guint      quick_deaths;
	gboolean   gave_up;
	GRegex    *title_re;      /* `title-match', compiled            */
} Webmail;

/* What a launch job needs, all of it copied: the job outlives the
   call that queued it and must not read the settings table. */
typedef struct {
	Webmail          *wm;
	gchar           **argv;          /* NULL: rewrite the files only */
	gboolean          secondary;     /* compose: a window not tracked */
	GPid              kill_pid;      /* a window to replace first    */
	gchar            *run_dir;
	gchar            *profile_dir;
	gchar            *cache_dir;
	gchar            *config_path;
	gchar            *css_path;
	gchar            *js_path;
	gchar            *pid_path;
	gchar            *config_text;
	gchar            *extra_css_path;
	gchar            *sig;
	BarWebmailPalette palette;
	BarWebmailProvider provider;
} WmJob;

static void
wm_job_free(gpointer data)
{
	WmJob *job = data;

	g_strfreev(job->argv);
	g_free(job->run_dir);
	g_free(job->profile_dir);
	g_free(job->cache_dir);
	g_free(job->config_path);
	g_free(job->css_path);
	g_free(job->js_path);
	g_free(job->pid_path);
	g_free(job->config_text);
	g_free(job->extra_css_path);
	g_free(job->sig);
	g_free(job);
}

/* ----------------------------------------------------------------
 * Settings and paths
 * ---------------------------------------------------------------- */

static const gchar *
wm_setting(GowlBarPlugin *plugin, const gchar *key, const gchar *fallback)
{
	const gchar *v = gowl_bar_plugin_get_setting(plugin, key);

	return (v != NULL && *v != '\0') ? v : fallback;
}

static const gchar *
wm_url(GowlBarPlugin *plugin)
{
	return wm_setting(plugin, "url", WM_DEFAULT_URL);
}

/*
 * Only http(s) is accepted.  The URL becomes the browser's last
 * argument and its homepage; `file:' would point a kiosk with
 * notifications allowed at the local disk, and anything starting with
 * `-' would be read as an option.
 */
static gboolean
wm_url_ok(const gchar *url)
{
	return g_str_has_prefix(url, "https://") ||
	       g_str_has_prefix(url, "http://");
}

/* $XDG_RUNTIME_DIR/gowl-webmail: the generated files.  Runtime, not
   config: they are rewritten every launch and mean nothing after a
   reboot. */
static gchar *
wm_run_dir(GowlBarPlugin *plugin)
{
	const gchar *v = gowl_bar_plugin_get_setting(plugin, "runtime-dir");

	if (v != NULL && *v != '\0')
		return bar_expand_tilde(v);
	return g_build_filename(g_get_user_runtime_dir(), "gowl-webmail",
	                        NULL);
}

/* $XDG_DATA_HOME/gowl-webmail: the profile -- cookies, so a sign-in
   survives a restart. */
static gchar *
wm_profile_dir(GowlBarPlugin *plugin)
{
	const gchar *v = gowl_bar_plugin_get_setting(plugin, "profile-dir");

	if (v != NULL && *v != '\0')
		return bar_expand_tilde(v);
	return g_build_filename(g_get_user_data_dir(), "gowl-webmail", NULL);
}

static gchar *
wm_cache_dir(GowlBarPlugin *plugin)
{
	g_autofree gchar *profile = NULL;
	const gchar *v = gowl_bar_plugin_get_setting(plugin, "profile-dir");

	/* A custom profile keeps its cache beside it, so two widgets
	   with two profiles never share one. */
	if (v != NULL && *v != '\0') {
		profile = bar_expand_tilde(v);
		return g_build_filename(profile, "cache", NULL);
	}
	return g_build_filename(g_get_user_cache_dir(), "gowl-webmail", NULL);
}

static gchar *
wm_run_file(GowlBarPlugin *plugin, const gchar *name)
{
	g_autofree gchar *dir = wm_run_dir(plugin);

	return g_build_filename(dir, name, NULL);
}

/* ----------------------------------------------------------------
 * The palette
 * ---------------------------------------------------------------- */

/* The role each part follows when the user has not set it. */
static const GowlBarColor wm_part_roles[BAR_WEBMAIL_N_PARTS] = {
	GOWL_BAR_COLOR_BASE,      /* background */
	GOWL_BAR_COLOR_SURFACE,   /* surface    */
	GOWL_BAR_COLOR_TEXT,      /* text       */
	GOWL_BAR_COLOR_SUBTEXT,   /* subtext    */
	GOWL_BAR_COLOR_ACCENT,    /* accent     */
	GOWL_BAR_COLOR_OVERLAY    /* border     */
};

/*
 * A role by its name.  Walks the theme's own names rather than parsing
 * the spelling here, so a role added to the theme is accepted with no
 * change to this widget.
 */
static gboolean
wm_role_by_name(const gchar *name, GowlBarColor *out)
{
	guint r;

	for (r = 0; r < GOWL_BAR_COLOR_COUNT; r++) {
		if (g_ascii_strcasecmp(gowl_bar_theme_color_name(
				(GowlBarColor)r), name) == 0) {
			*out = (GowlBarColor)r;
			return TRUE;
		}
	}
	return FALSE;
}

static void
wm_role_rgb(const GowlBarTheme *theme, GowlBarColor role, BarWebmailRgb *out)
{
	const gdouble *c = gowl_bar_theme_color(theme, role);

	out->r = c[0];
	out->g = c[1];
	out->b = c[2];
}

/*
 * The colours the page is themed with, by the same three-way rule as
 * every other widget colour: a `color-<part>' setting naming a role
 * takes that role, one spelled `#rrggbb' takes exactly that colour,
 * and an unset part follows the bar's palette -- unless
 * `follow-theme' is off, in which case it is left to the site.
 */
static void
wm_resolve_palette(GowlBarPlugin *plugin, BarWebmailPalette *out)
{
	GowlBarHost *host = gowl_bar_plugin_get_host(plugin);
	const GowlBarTheme *theme = (host != NULL)
		? gowl_bar_host_get_theme(host) : NULL;
	gboolean follow;
	guint i;

	memset(out, 0, sizeof(*out));
	follow = gowl_bar_plugin_get_setting_bool(plugin, "follow-theme",
	                                          TRUE);

	for (i = 0; i < BAR_WEBMAIL_N_PARTS; i++) {
		g_autofree gchar *key = NULL;
		const gchar *spec;
		GowlBarColor role;

		key = g_strdup_printf("color-%s",
			bar_webmail_part_name((BarWebmailPart)i));
		spec = gowl_bar_plugin_get_setting(plugin, key);
		if (spec != NULL && *spec != '\0') {
			if (bar_webmail_parse_hex(spec, &out->colors[i])) {
				out->set[i] = TRUE;
				continue;
			}
			if (theme != NULL && wm_role_by_name(spec, &role)) {
				wm_role_rgb(theme, role, &out->colors[i]);
				out->set[i] = TRUE;
				continue;
			}
			/* Neither: fall through to the palette, as a bad
			   `color' does for every other widget. */
		}
		if (follow && theme != NULL) {
			wm_role_rgb(theme, wm_part_roles[i], &out->colors[i]);
			out->set[i] = TRUE;
		}
	}
}

/*
 * Everything that decides what the generated files say.  When this
 * moves, the files are stale -- a palette switch, a new override, a
 * different URL or extra stylesheet.
 */
static gchar *
wm_signature(GowlBarPlugin *plugin, const BarWebmailPalette *palette)
{
	g_autofree gchar *colours = bar_webmail_palette_signature(palette);

	return g_strdup_printf("%s|%s|%s|%s", colours, wm_url(plugin),
		wm_setting(plugin, "extra-css", ""),
		gowl_bar_plugin_get_setting_bool(plugin,
			"third-party-cookies", TRUE) ? "3p" : "1p");
}

/* ----------------------------------------------------------------
 * Processes
 * ---------------------------------------------------------------- */

/* Read a small /proc file.  /proc files report size 0, so the read is
   bounded by hand. */
static gchar *
wm_read_proc(pid_t pid, const gchar *what, gsize *len)
{
	g_autofree gchar *path = NULL;
	gchar *text = NULL;
	gsize n = 0;

	path = g_strdup_printf("/proc/%d/%s", (gint)pid, what);
	if (!g_file_get_contents(path, &text, &n, NULL))
		return NULL;
	if (n > 64 * 1024) {
		g_free(text);
		return NULL;
	}
	if (len != NULL)
		*len = n;
	return text;
}

/*
 * Whether @pid is a live browser that this widget started, by the
 * argument nobody else's process has: the path of our generated
 * config.  Asked before trusting a PID from the file and before
 * signalling one -- PIDs are reused.
 */
static gboolean
wm_pid_is_ours(pid_t pid, const gchar *config_path)
{
	g_autofree gchar *cmdline = NULL;
	g_autofree gchar *stat = NULL;
	const gchar *state;
	gsize len = 0;

	if (pid <= 1)
		return FALSE;
	stat = wm_read_proc(pid, "stat", NULL);
	if (stat == NULL)
		return FALSE;
	/* A zombie still answers kill(pid, 0), and nothing will run in
	   it again. */
	state = strrchr(stat, ')');
	if (state != NULL && state[1] == ' ' && state[2] == 'Z')
		return FALSE;
	cmdline = wm_read_proc(pid, "cmdline", &len);
	return bar_webmail_cmdline_has_arg(cmdline, len, config_path);
}

/* Whether @pid is still alive, cheaply: no /proc read for the common
   case of a window that is simply still running. */
static gboolean
wm_pid_alive(pid_t pid)
{
	g_autofree gchar *stat = NULL;
	const gchar *state;

	if (pid <= 1 || kill(pid, 0) != 0)
		return FALSE;
	stat = wm_read_proc(pid, "stat", NULL);
	if (stat == NULL)
		return FALSE;
	state = strrchr(stat, ')');
	return !(state != NULL && state[1] == ' ' && state[2] == 'Z');
}

/*
 * Whether @pid is @ancestor or descends from it, within a few
 * generations.  A browser-command such as `env FOO=1 gsurf ...' execs
 * in place and keeps the PID; one such as `flatpak run ...' does not,
 * and the window belongs to a grandchild.
 */
static gboolean
wm_pid_descends(pid_t pid, pid_t ancestor)
{
	guint depth;

	for (depth = 0; depth < 6 && pid > 1; depth++) {
		g_autofree gchar *stat = NULL;

		if (pid == ancestor)
			return TRUE;
		stat = wm_read_proc(pid, "stat", NULL);
		pid = bar_webmail_stat_ppid(stat);
	}
	return FALSE;
}

/* Ask our browser to stop.  Only a PID that is provably ours. */
static void
wm_terminate(GPid pid, const gchar *config_path)
{
	if (pid > 1 && wm_pid_is_ours(pid, config_path))
		kill(pid, SIGTERM);
}

/* The grandchild starts its own session, so a terminal's Ctrl+C or a
   bar reload in gowl's process group never reaches the mail window. */
static void
wm_child_setup(gpointer user_data)
{
	(void)user_data;
	setsid();
}

/* ----------------------------------------------------------------
 * Launch jobs (worker thread)
 * ---------------------------------------------------------------- */

static gboolean
wm_make_private_dir(const gchar *dir, GError **error)
{
	if (g_mkdir_with_parents(dir, 0700) != 0) {
		g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno),
		            "cannot create %s: %s", dir, g_strerror(errno));
		return FALSE;
	}
	/* An existing directory keeps whatever mode it was made with;
	   these hold session cookies, so they are ours alone. */
	if (g_chmod(dir, 0700) != 0) {
		g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno),
		            "cannot restrict %s: %s", dir, g_strerror(errno));
		return FALSE;
	}
	return TRUE;
}

static gboolean
wm_write_private(const gchar *path, const gchar *text, GError **error)
{
	return g_file_set_contents_full(path, text, -1,
		G_FILE_SET_CONTENTS_CONSISTENT, 0600, error);
}

/* The user's extra stylesheet, bounded and made valid UTF-8. */
static gchar *
wm_read_extra_css(const gchar *path_setting, gchar **why)
{
	g_autofree gchar *path = NULL;
	g_autofree gchar *raw = NULL;
	GStatBuf st;
	gsize len = 0;

	if (path_setting == NULL || *path_setting == '\0')
		return NULL;
	path = bar_expand_tilde(path_setting);
	if (g_stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
		*why = g_strdup_printf("extra-css %s is not a readable file",
		                       path);
		return NULL;
	}
	if (st.st_size > WM_MAX_EXTRA_CSS) {
		*why = g_strdup_printf("extra-css %s is over %d KiB; ignored",
		                       path, WM_MAX_EXTRA_CSS / 1024);
		return NULL;
	}
	if (!g_file_get_contents(path, &raw, &len, NULL)) {
		*why = g_strdup_printf("cannot read extra-css %s", path);
		return NULL;
	}
	return g_utf8_make_valid(raw, (gssize)len);
}

static gboolean
wm_command_present(const gchar *cmd)
{
	if (cmd == NULL)
		return FALSE;
	if (strchr(cmd, '/') != NULL)
		return g_file_test(cmd, G_FILE_TEST_IS_EXECUTABLE);
	return bar_have_command(cmd);
}

static void
wm_set_error(Webmail *wm, gchar *message /* takes */)
{
	g_mutex_lock(&wm->lock);
	g_free(wm->last_error);
	wm->last_error = message;
	g_mutex_unlock(&wm->lock);
}

/*
 * Write the generated files, then (for a launch) start the browser.
 *
 * Runs on a worker: it creates directories, reads the user's extra
 * stylesheet, writes three files and forks, any of which may wait on
 * the disk.  It touches the shared struct only under the lock.
 */
static void
wm_job_run(GowlBarPlugin *plugin, gpointer user_data)
{
	WmJob *job = user_data;
	Webmail *wm = job->wm;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *extra = NULL;
	g_autofree gchar *extra_why = NULL;
	g_autofree gchar *css = NULL;
	GPid pid = 0;

	if (!wm_make_private_dir(job->run_dir, &error) ||
	    !wm_make_private_dir(job->profile_dir, &error) ||
	    !wm_make_private_dir(job->cache_dir, &error))
		goto failed;

	extra = wm_read_extra_css(job->extra_css_path, &extra_why);
	css = bar_webmail_css(&job->palette, job->provider, extra,
	                      job->extra_css_path);
	if (!wm_write_private(job->css_path, css, &error) ||
	    !wm_write_private(job->js_path, bar_webmail_unread_script(),
	                      &error) ||
	    !wm_write_private(job->config_path, job->config_text, &error))
		goto failed;

	g_mutex_lock(&wm->lock);
	g_free(wm->written_sig);
	wm->written_sig = g_strdup(job->sig);
	g_free(wm->last_error);
	wm->last_error = g_steal_pointer(&extra_why);
	g_mutex_unlock(&wm->lock);

	if (job->argv == NULL)
		goto done;

	if (!wm_command_present(job->argv[0])) {
		g_mutex_lock(&wm->lock);
		wm->missing = TRUE;
		g_free(wm->missing_cmd);
		wm->missing_cmd = g_strdup(job->argv[0]);
		wm->launching = FALSE;
		g_mutex_unlock(&wm->lock);
		gowl_bar_plugin_request_panel_refresh(plugin);
		return;
	}

	/* Replacing a window (a theme restart): let the old one go
	   first, or two of them share the cookie jar for a moment and
	   the user briefly sees both. */
	if (job->kill_pid > 1) {
		guint waited;

		wm_terminate(job->kill_pid, job->config_path);
		for (waited = 0; waited < 40 && wm_pid_alive(job->kill_pid);
		     waited++)
			g_usleep(50 * 1000);
	}

	if (!g_spawn_async(NULL, job->argv, NULL,
	                   G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL,
	                   wm_child_setup, NULL, &pid, &error))
		goto failed;

	g_mutex_lock(&wm->lock);
	wm->missing = FALSE;
	if (!job->secondary) {
		wm->pid = pid;
		g_free(wm->running_sig);
		wm->running_sig = g_strdup(job->sig);
	}
	wm->launching = FALSE;
	g_mutex_unlock(&wm->lock);

	if (!job->secondary) {
		g_autofree gchar *text = g_strdup_printf("%d\n", (gint)pid);

		/* Lets a reloaded bar find the window it did not start. */
		if (!wm_write_private(job->pid_path, text, &error))
			g_warning("webmail: %s", error->message);
	}
	goto done;

failed:
	g_warning("webmail: %s", error->message);
	g_mutex_lock(&wm->lock);
	g_free(wm->last_error);
	wm->last_error = g_strdup(error->message);
	wm->launching = FALSE;
	g_mutex_unlock(&wm->lock);
done:
	gowl_bar_plugin_request_panel_refresh(plugin);
}

/*
 * Build a job from the current settings (compositor thread: string
 * work only) and queue it.  @launch_url NULL rewrites the files and
 * starts nothing.
 */
static gboolean
wm_queue_job(GowlBarPlugin *plugin, Webmail *wm, const gchar *launch_url,
             gboolean secondary, GPid kill_pid)
{
	g_autoptr(GError) error = NULL;
	BarWebmailGsurfSpec spec;
	WmJob *job;
	const gchar *url = wm_url(plugin);
	g_autofree gchar *dl = NULL;

	if (!wm_url_ok(url)) {
		wm_set_error(wm, g_strdup_printf(
			"url must start with https:// (got %s)", url));
		return FALSE;
	}

	job = g_new0(WmJob, 1);
	job->wm          = wm;
	job->secondary   = secondary;
	job->kill_pid    = kill_pid;
	job->run_dir     = wm_run_dir(plugin);
	job->profile_dir = wm_profile_dir(plugin);
	job->cache_dir   = wm_cache_dir(plugin);
	job->config_path = g_build_filename(job->run_dir, "gsurf.yaml", NULL);
	job->css_path    = g_build_filename(job->run_dir, "theme.css", NULL);
	job->js_path     = g_build_filename(job->run_dir, "unread.js", NULL);
	job->pid_path    = g_build_filename(job->run_dir, "pid", NULL);
	job->extra_css_path = g_strdup(gowl_bar_plugin_get_setting(plugin,
	                                                          "extra-css"));
	job->provider    = bar_webmail_provider(url);
	wm_resolve_palette(plugin, &job->palette);
	job->sig         = wm_signature(plugin, &job->palette);

	dl = g_strdup(gowl_bar_plugin_get_setting(plugin, "download-dir"));
	memset(&spec, 0, sizeof(spec));
	spec.url          = url;
	spec.profile_dir  = job->profile_dir;
	spec.cache_dir    = job->cache_dir;
	spec.css_path     = job->css_path;
	spec.js_path      = job->js_path;
	spec.download_dir = (dl != NULL && *dl != '\0') ? dl : NULL;
	spec.dark         = bar_webmail_palette_dark(&job->palette);
	spec.third_party_cookies = gowl_bar_plugin_get_setting_bool(plugin,
		"third-party-cookies", TRUE);
	job->config_text  = bar_webmail_gsurf_config(&spec);

	if (launch_url != NULL) {
		job->argv = bar_webmail_build_argv(
			wm_setting(plugin, "browser-command", WM_DEFAULT_COMMAND),
			job->config_path, launch_url, &error);
		if (job->argv == NULL) {
			wm_set_error(wm, g_strdup_printf(
				"browser-command: %s", error->message));
			wm_job_free(job);
			return FALSE;
		}
		if (!secondary) {
			g_mutex_lock(&wm->lock);
			wm->launching = TRUE;
			g_mutex_unlock(&wm->lock);
			wm->launched_at = g_get_monotonic_time();
			wm->client_id = 0;
			wm->user_quit = FALSE;
		}
	}

	gowl_bar_plugin_queue_work(plugin, wm_job_run, job, wm_job_free);
	return TRUE;
}

static gboolean
wm_launch(GowlBarPlugin *plugin, Webmail *wm, gboolean hidden)
{
	wm->hide_on_map = hidden;
	return wm_queue_job(plugin, wm, wm_url(plugin), FALSE, 0);
}

/* Replace the running window with one that loads the current theme.
   gsurf reads its stylesheets once, when a view is created, so there
   is no reloading them in place. */
static void
wm_restart(GowlBarPlugin *plugin, Webmail *wm, gboolean hidden)
{
	GPid old;

	g_mutex_lock(&wm->lock);
	old = wm->pid;
	g_mutex_unlock(&wm->lock);
	wm->hide_on_map = hidden;
	wm_queue_job(plugin, wm, wm_url(plugin), FALSE, old);
}

/* ----------------------------------------------------------------
 * The window (compositor thread)
 * ---------------------------------------------------------------- */

static GowlCompositor *
wm_compositor(void)
{
	const BarEnv *env = bar_env();

	return (env != NULL && env->compositor != NULL)
		? GOWL_COMPOSITOR(env->compositor) : NULL;
}

static gboolean
wm_title_matches(Webmail *wm, GowlClient *c)
{
	const gchar *title = gowl_client_get_title(c);

	return wm->title_re != NULL && title != NULL &&
	       g_utf8_validate(title, -1, NULL) &&
	       g_regex_match(wm->title_re, title, 0, NULL);
}

/*
 * Find the mail window among the compositor's clients.
 *
 * By PID first -- ours, or a descendant of ours -- because every gsurf
 * window shares the app-id `gsurf' and the user's ordinary browser
 * windows must never be minimized by a mail icon.  Without a PID (the
 * browser was started some other way) a window is adopted only on
 * evidence the user gave: a `title-match' regex (with the app-id, when
 * one is set), or an `app-id' that is not gsurf's shared one.
 */
static GowlClient *
wm_find_client(GowlBarPlugin *plugin, Webmail *wm, GowlCompositor *comp,
               GPid pid)
{
	const gchar *app_id;
	gboolean explicit_app_id;
	GList *l;

	/* The window we already know, if it is still there. */
	if (wm->client_id != 0) {
		for (l = gowl_compositor_get_clients(comp); l != NULL;
		     l = l->next) {
			GowlClient *c = l->data;

			if (gowl_client_get_id(c) == wm->client_id)
				return c;
		}
		wm->client_id = 0;
	}

	if (pid > 1) {
		for (l = gowl_compositor_get_clients(comp); l != NULL;
		     l = l->next)
			if (gowl_client_get_pid(l->data) == pid)
				return l->data;
		for (l = gowl_compositor_get_clients(comp); l != NULL;
		     l = l->next)
			if (wm_pid_descends(gowl_client_get_pid(l->data), pid))
				return l->data;
	}

	app_id = gowl_bar_plugin_get_setting(plugin, "app-id");
	explicit_app_id = (app_id != NULL && *app_id != '\0' &&
	                   g_strcmp0(app_id, WM_DEFAULT_APP_ID) != 0);
	if (wm->title_re == NULL && !explicit_app_id)
		return NULL;

	for (l = gowl_compositor_get_clients(comp); l != NULL; l = l->next) {
		GowlClient *c = l->data;
		gboolean id_ok;

		id_ok = (app_id == NULL || *app_id == '\0' ||
		         g_strcmp0(gowl_client_get_app_id(c), app_id) == 0);
		if (wm->title_re != NULL) {
			if (id_ok && wm_title_matches(wm, c))
				return c;
		} else if (explicit_app_id && id_ok) {
			return c;
		}
	}
	return NULL;
}

static BarWebmailWindow
wm_state_of(GowlCompositor *comp, GowlClient *c)
{
	GowlMonitor *mon;

	if (gowl_client_get_minimized(c))
		return BAR_WEBMAIL_WINDOW_MINIMIZED;
	if (gowl_compositor_get_focused_client(comp) == c)
		return BAR_WEBMAIL_WINDOW_FOCUSED;
	mon = (GowlMonitor *)gowl_client_get_monitor(c);
	if (mon != NULL &&
	    (gowl_client_get_tags(c) & gowl_monitor_get_tags(mon)) != 0)
		return BAR_WEBMAIL_WINDOW_VISIBLE;
	return BAR_WEBMAIL_WINDOW_ELSEWHERE;
}

/* The title as the panel shows it: valid, bounded, and without the
   " — gsurf" gsurf appends to every window. */
static gchar *
wm_clean_title(const gchar *raw)
{
	g_autofree gchar *valid = NULL;
	gchar *cut;

	if (raw == NULL)
		return NULL;
	valid = g_utf8_make_valid(raw, -1);
	if (strlen(valid) > WM_MAX_TITLE) {
		const gchar *end = g_utf8_find_prev_char(valid,
			valid + WM_MAX_TITLE);

		if (end != NULL)
			*(gchar *)end = '\0';
	}
	cut = g_strrstr(valid, " \xe2\x80\x94 gsurf");
	if (cut != NULL && cut[strlen(" \xe2\x80\x94 gsurf")] == '\0')
		*cut = '\0';
	return g_steal_pointer(&valid);
}

/*
 * Carry out a decided action.  Panel callers have closed the panel
 * first: an open dropdown holds the keyboard, and focusing a window
 * under it would be refused by the focus rules.
 */
static void
wm_do(GowlBarPlugin *plugin, Webmail *wm, BarWebmailAction action)
{
	GowlCompositor *comp = wm_compositor();
	GowlClient *c = NULL;
	GPid pid;

	g_mutex_lock(&wm->lock);
	pid = wm->pid;
	g_mutex_unlock(&wm->lock);

	if (action == BAR_WEBMAIL_ACTION_LAUNCH) {
		wm_launch(plugin, wm, FALSE);
		return;
	}
	if (action == BAR_WEBMAIL_ACTION_WAIT ||
	    action == BAR_WEBMAIL_ACTION_NONE || comp == NULL)
		return;

	c = wm_find_client(plugin, wm, comp, pid);
	if (c == NULL)
		return;

	switch (action) {
	case BAR_WEBMAIL_ACTION_HIDE:
		if (gowl_compositor_minimize_client(comp, c))
			wm->state = BAR_WEBMAIL_WINDOW_MINIMIZED;
		break;
	case BAR_WEBMAIL_ACTION_RESTORE:
		gowl_compositor_unminimize_client(comp, c,
			(GowlMonitor *)bar_plugin_monitor(plugin), 0);
		break;
	case BAR_WEBMAIL_ACTION_FOCUS:
	case BAR_WEBMAIL_ACTION_JUMP:
		gowl_compositor_show_client(comp, c);
		break;
	case BAR_WEBMAIL_ACTION_BRING:
		/* Minimize-and-restore is the compositor's own "put this
		   window where the user is": it keeps the tag bookkeeping,
		   the taskbar state and the focus rules all in one place. */
		if (gowl_compositor_minimize_client(comp, c))
			gowl_compositor_unminimize_client(comp, c,
				(GowlMonitor *)bar_plugin_monitor(plugin), 0);
		else
			gowl_compositor_show_client(comp, c);
		break;
	case BAR_WEBMAIL_ACTION_NONE:
	case BAR_WEBMAIL_ACTION_LAUNCH:
	case BAR_WEBMAIL_ACTION_WAIT:
	default:
		break;
	}
}

static gboolean
wm_bring_here(GowlBarPlugin *plugin)
{
	return g_strcmp0(wm_setting(plugin, "show-on", "here"), "jump") != 0;
}

/* ----------------------------------------------------------------
 * The plugin
 * ---------------------------------------------------------------- */

static gpointer
wm_create(GowlBarPlugin *plugin)
{
	Webmail *wm;

	(void)plugin;
	wm = g_new0(Webmail, 1);
	g_mutex_init(&wm->lock);
	wm->unread = -1;
	return wm;
}

static void
wm_destroy(GowlBarPlugin *plugin, gpointer data)
{
	Webmail *wm = data;

	(void)plugin;
	if (wm == NULL)
		return;
	/* The window is deliberately left running: a bar reload must
	   not close the user's mail.  The PID file lets the next
	   instance pick it up again. */
	g_free(wm->missing_cmd);
	g_free(wm->last_error);
	g_free(wm->running_sig);
	g_free(wm->written_sig);
	g_free(wm->title);
	g_free(wm->sig);
	g_clear_pointer(&wm->title_re, g_regex_unref);
	g_mutex_clear(&wm->lock);
	g_free(wm);
}

static void
wm_configure(GowlBarPlugin *plugin, gpointer data, GHashTable *settings)
{
	Webmail *wm = data;
	const gchar *pattern;

	(void)settings;
	g_clear_pointer(&wm->title_re, g_regex_unref);
	pattern = gowl_bar_plugin_get_setting(plugin, "title-match");
	if (pattern != NULL && *pattern != '\0') {
		g_autoptr(GError) error = NULL;

		wm->title_re = g_regex_new(pattern, G_REGEX_OPTIMIZE, 0,
		                           &error);
		if (wm->title_re == NULL)
			wm_set_error(wm, g_strdup_printf("title-match: %s",
			                                 error->message));
	}
	/* Recompute the theme on the next poll: a colour or URL setting
	   may have moved. */
	g_clear_pointer(&wm->sig, g_free);
	g_mutex_lock(&wm->lock);
	wm->last_have_check = 0;
	g_mutex_unlock(&wm->lock);
	gowl_bar_plugin_set_dirty(plugin);
}

static gint
wm_interval(GowlBarPlugin *plugin, gpointer data)
{
	Webmail *wm = data;
	gboolean launching;

	g_mutex_lock(&wm->lock);
	launching = wm->launching;
	g_mutex_unlock(&wm->lock);
	/* Quick while a window is on its way (to catch it and, for a
	   hidden start, minimize it before it is much looked at). */
	if (launching || wm->hide_on_map ||
	    wm->state == BAR_WEBMAIL_WINDOW_STARTING)
		return 1;
	return CLAMP(gowl_bar_plugin_get_setting_int(plugin, "interval", 2),
	             1, 60);
}

/*
 * A bar that was reloaded (or a gowl restarted in the same session)
 * finds the window the previous instance started.  The PID file alone
 * is not trusted: the process must carry our config path.
 */
static void
wm_restore_pid(GowlBarPlugin *plugin, Webmail *wm)
{
	g_autofree gchar *pid_path = wm_run_file(plugin, "pid");
	g_autofree gchar *config_path = wm_run_file(plugin, "gsurf.yaml");
	g_autofree gchar *text = NULL;
	gint64 v;

	wm->restored = TRUE;
	if (!g_file_get_contents(pid_path, &text, NULL, NULL))
		return;
	v = g_ascii_strtoll(text, NULL, 10);
	if (v > 1 && v < G_MAXINT32 && wm_pid_is_ours((pid_t)v, config_path)) {
		g_mutex_lock(&wm->lock);
		if (wm->pid == 0)
			wm->pid = (GPid)v;
		g_mutex_unlock(&wm->lock);
	}
}

/* The browser exited.  Relaunch it hidden when asked to keep one in
   the background, unless it keeps dying young. */
static void
wm_process_gone(GowlBarPlugin *plugin, Webmail *wm, gint64 now)
{
	gboolean relaunch;

	g_mutex_lock(&wm->lock);
	wm->pid = 0;
	g_mutex_unlock(&wm->lock);
	wm->client_id = 0;
	wm->state = BAR_WEBMAIL_WINDOW_NONE;
	wm->unread = -1;
	g_clear_pointer(&wm->title, g_free);

	relaunch = !wm->user_quit && !wm->gave_up &&
		gowl_bar_plugin_get_setting_bool(plugin, "run-in-background",
		                                 FALSE);
	wm->user_quit = FALSE;
	if (!relaunch)
		return;

	if (now - wm->launched_at < WM_QUICK_DEATH)
		wm->quick_deaths++;
	else
		wm->quick_deaths = 0;
	if (wm->quick_deaths >= WM_MAX_QUICK_DEATHS) {
		wm->gave_up = TRUE;
		wm_set_error(wm, g_strdup(
			"The mail window keeps exiting right after it starts; "
			"run-in-background stopped restarting it."));
		gowl_bar_plugin_notify(plugin, GOWL_BAR_TOAST_NORMAL,
			"Web mail keeps exiting",
			"Not restarting it in the background. Open the panel "
			"for details.");
		return;
	}
	wm_launch(plugin, wm, TRUE);
}

/* Label, colour and tooltip from the state just computed. */
static void
wm_apply(GowlBarPlugin *plugin, Webmail *wm, gboolean running,
         gboolean missing)
{
	g_autofree gchar *label = NULL;
	g_autofree gchar *tip = NULL;
	const gchar *name;

	name = bar_webmail_provider_name(bar_webmail_provider(wm_url(plugin)));
	gowl_bar_plugin_set_icon(plugin, WM_ICON_MAIL);

	switch (bar_webmail_look(running, wm->unread)) {
	case BAR_WEBMAIL_LOOK_UNREAD:
		bar_plugin_apply_color(plugin,
			gowl_bar_plugin_get_setting(plugin, "color-unread"),
			GOWL_BAR_COLOR_ACCENT);
		break;
	case BAR_WEBMAIL_LOOK_ON:
		bar_plugin_apply_color(plugin,
			gowl_bar_plugin_get_setting(plugin, "color-on"),
			GOWL_BAR_COLOR_TEXT);
		break;
	case BAR_WEBMAIL_LOOK_OFF:
	default:
		bar_plugin_apply_color(plugin,
			gowl_bar_plugin_get_setting(plugin, "color-off"),
			GOWL_BAR_COLOR_MUTED);
		break;
	}

	label = running ? bar_webmail_label(wm->unread, wm->plus,
		gowl_bar_plugin_get_setting_bool(plugin, "show-count", TRUE))
	                : NULL;
	gowl_bar_plugin_set_label(plugin, label);

	if (missing)
		tip = g_strdup_printf("%s: the browser is not installed "
		                      "(gsurf; runtime: webkit2gtk4.1)", name);
	else if (!running)
		tip = g_strdup_printf("%s: not running \xe2\x80\x94 click to "
		                      "open", name);
	else if (wm->state == BAR_WEBMAIL_WINDOW_STARTING)
		tip = g_strdup_printf("%s: starting\xe2\x80\xa6", name);
	else if (wm->unread > 0)
		tip = g_strdup_printf("%s: %d%s unread%s%s", name, wm->unread,
			wm->plus ? "+" : "",
			(wm->title != NULL) ? "\n" : "",
			(wm->title != NULL) ? wm->title : "");
	else
		tip = g_strdup_printf("%s%s%s", name,
			(wm->title != NULL) ? "\n" : "",
			(wm->title != NULL) ? wm->title : "");
	gowl_bar_plugin_set_tooltip(plugin, tip);
}

/*
 * The poll, on the compositor thread: is the browser alive, where is
 * its window, what does its title say, has the palette moved.
 * Nothing here blocks: /proc reads of a few hundred bytes and walks
 * of the client list.
 */
static void
wm_poll(GowlBarPlugin *plugin, gpointer data)
{
	Webmail *wm = data;
	GowlCompositor *comp;
	BarWebmailPalette palette;
	g_autofree gchar *sig = NULL;
	gint64 now = g_get_monotonic_time();
	gboolean launching, missing, running;
	GPid pid;

	if (!wm->restored)
		wm_restore_pid(plugin, wm);

	g_mutex_lock(&wm->lock);
	pid = wm->pid;
	launching = wm->launching;
	missing = wm->missing;
	g_mutex_unlock(&wm->lock);

	/* 1. The process. */
	running = (pid > 1 && wm_pid_alive(pid));
	if (pid > 1 && !running && !launching)
		wm_process_gone(plugin, wm, now);
	g_mutex_lock(&wm->lock);
	pid = wm->pid;
	launching = wm->launching;
	g_mutex_unlock(&wm->lock);
	running = running || launching;

	/* 2. The window. */
	comp = wm_compositor();
	if (!running) {
		wm->state = BAR_WEBMAIL_WINDOW_NONE;
	} else if (comp == NULL || launching) {
		/* No compositor (a test, or a bar outside gowl): the
		   process is all there is to go on.  And while a launch
		   job is in flight the window that can be found may be
		   the one being REPLACED: a hidden restart would spend
		   its hide on the old window, and the new one would map
		   in the user's face. */
		wm->state = BAR_WEBMAIL_WINDOW_STARTING;
	} else {
		GowlClient *c = wm_find_client(plugin, wm, comp, pid);

		if (c == NULL) {
			wm->state = BAR_WEBMAIL_WINDOW_STARTING;
		} else {
			wm->client_id = gowl_client_get_id(c);
			if (wm->hide_on_map) {
				wm->hide_on_map = FALSE;
				gowl_compositor_minimize_client(comp, c);
			}
			wm->state = wm_state_of(comp, c);
			g_free(wm->title);
			wm->title = wm_clean_title(gowl_client_get_title(c));
			wm->unread = bar_webmail_parse_unread(wm->title,
			                                      &wm->plus);
		}
	}

	/* 3. The theme.  A moved palette rewrites the files now, so the
	   next launch has it; the live window needs a restart, done at
	   once when nobody is looking at it. */
	wm_resolve_palette(plugin, &palette);
	sig = wm_signature(plugin, &palette);
	if (g_strcmp0(sig, wm->sig) != 0) {
		gboolean stale_files;

		g_free(wm->sig);
		wm->sig = g_strdup(sig);
		g_mutex_lock(&wm->lock);
		stale_files = (wm->written_sig != NULL &&
		               g_strcmp0(wm->written_sig, sig) != 0);
		g_mutex_unlock(&wm->lock);
		if (stale_files && !launching)
			wm_queue_job(plugin, wm, NULL, FALSE, 0);
	}
	if (running && !launching &&
	    wm->state == BAR_WEBMAIL_WINDOW_MINIMIZED &&
	    gowl_bar_plugin_get_setting_bool(plugin, "live-reload", TRUE)) {
		gboolean stale_window;

		g_mutex_lock(&wm->lock);
		stale_window = (wm->running_sig != NULL &&
		                g_strcmp0(wm->running_sig, sig) != 0);
		g_mutex_unlock(&wm->lock);
		if (stale_window)
			wm_restart(plugin, wm, TRUE);
	}

	/* 4. Start hidden at login, once, when asked. */
	if (!wm->autostarted) {
		wm->autostarted = TRUE;
		if (!running && !missing &&
		    gowl_bar_plugin_get_setting_bool(plugin, "autostart",
		                                     FALSE))
			wm_launch(plugin, wm, TRUE);
	}

	wm_apply(plugin, wm, running, missing);
}

/* The worker half: only whether the browser is installed, checked
   once a minute -- a PATH walk is cheap but not free. */
static void
wm_poll_async(GowlBarPlugin *plugin, gpointer data)
{
	Webmail *wm = data;
	g_auto(GStrv) argv = NULL;
	gint64 now = g_get_monotonic_time();
	gboolean present;

	g_mutex_lock(&wm->lock);
	if (wm->last_have_check != 0 &&
	    now - wm->last_have_check < 60 * G_TIME_SPAN_SECOND) {
		g_mutex_unlock(&wm->lock);
		return;
	}
	wm->last_have_check = now;
	g_mutex_unlock(&wm->lock);

	argv = bar_webmail_build_argv(
		wm_setting(plugin, "browser-command", WM_DEFAULT_COMMAND),
		"", WM_DEFAULT_URL, NULL);
	present = (argv != NULL && wm_command_present(argv[0]));

	g_mutex_lock(&wm->lock);
	wm->missing = !present;
	g_free(wm->missing_cmd);
	wm->missing_cmd = g_strdup((argv != NULL) ? argv[0] : "gsurf");
	g_mutex_unlock(&wm->lock);
}

/* Middle click and the Compose button: a second kiosk window on the
   compose page, sharing the profile so it is signed in. */
static void
wm_compose(GowlBarPlugin *plugin, Webmail *wm)
{
	g_autofree gchar *derived = NULL;
	const gchar *url;

	url = gowl_bar_plugin_get_setting(plugin, "compose-url");
	if (url == NULL || *url == '\0') {
		derived = bar_webmail_compose_url(wm_url(plugin));
		url = derived;
	}
	if (url == NULL || !wm_url_ok(url)) {
		gowl_bar_plugin_notify(plugin, GOWL_BAR_TOAST_NORMAL,
			"No compose page for this web mail",
			"Set webmail.compose-url to the page that opens a new "
			"message.");
		return;
	}
	wm_queue_job(plugin, wm, url, TRUE, 0);
}

static gboolean
wm_click(GowlBarPlugin *plugin, gpointer data, guint button, gint x, gint y,
         guint modifiers)
{
	Webmail *wm = data;
	gboolean missing;

	(void)x;
	(void)y;
	g_mutex_lock(&wm->lock);
	missing = wm->missing;
	g_mutex_unlock(&wm->lock);

	if (button == BTN_LEFT) {
		/* Shift or Ctrl opens the panel; so does a click that
		   could do nothing but fail, so the reason is in view. */
		if ((modifiers & (WLR_MODIFIER_SHIFT | WLR_MODIFIER_CTRL)) ||
		    g_strcmp0(wm_setting(plugin, "click", "toggle"),
		              "panel") == 0 ||
		    (missing && wm->state == BAR_WEBMAIL_WINDOW_NONE))
			return FALSE;
		if (wm->state == BAR_WEBMAIL_WINDOW_STARTING &&
		    g_get_monotonic_time() - wm->launched_at > WM_MAP_PATIENCE)
			return FALSE;
		wm_do(plugin, wm, bar_webmail_decide_toggle(wm->state,
			wm_bring_here(plugin)));
		gowl_bar_plugin_poll(plugin);
		return TRUE;
	}
	if (button == BTN_RIGHT) {
		wm_do(plugin, wm, bar_webmail_decide_show(wm->state,
			wm_bring_here(plugin)));
		gowl_bar_plugin_poll(plugin);
		return TRUE;
	}
	if (button == BTN_MIDDLE) {
		wm_compose(plugin, wm);
		return TRUE;
	}
	return FALSE;
}

static const gchar *
wm_state_text(Webmail *wm, gboolean running)
{
	if (!running)
		return "Not running";
	switch (wm->state) {
	case BAR_WEBMAIL_WINDOW_STARTING:  return "Starting\xe2\x80\xa6";
	case BAR_WEBMAIL_WINDOW_MINIMIZED: return "Hidden";
	case BAR_WEBMAIL_WINDOW_FOCUSED:   return "In front";
	case BAR_WEBMAIL_WINDOW_VISIBLE:   return "On screen";
	case BAR_WEBMAIL_WINDOW_ELSEWHERE: return "On another workspace";
	case BAR_WEBMAIL_WINDOW_NONE:
	default:                           return "Not running";
	}
}

static GowlBarPanel *
wm_panel(GowlBarPlugin *plugin, gpointer data)
{
	Webmail *wm = data;
	GowlBarPanel *panel;
	GowlBarPanelItem *item;
	g_autofree gchar *host = NULL;
	g_autofree gchar *unread = NULL;
	g_autofree gchar *missing_cmd = NULL;
	g_autofree gchar *error = NULL;
	gboolean running, missing, launching, stale;
	BarWebmailProvider provider;
	const gchar *url = wm_url(plugin);
	gint64 now = g_get_monotonic_time();

	/* Copied out under the lock; the worker replaces these. */
	g_mutex_lock(&wm->lock);
	running   = (wm->pid > 1) || wm->launching;
	launching = wm->launching;
	missing   = wm->missing;
	missing_cmd = g_strdup(wm->missing_cmd);
	error     = g_strdup(wm->last_error);
	stale     = (wm->pid > 1 && wm->running_sig != NULL &&
	             wm->sig != NULL &&
	             g_strcmp0(wm->running_sig, wm->sig) != 0);
	g_mutex_unlock(&wm->lock);

	provider = bar_webmail_provider(url);
	host = bar_webmail_url_host(url);

	panel = gowl_bar_panel_new();
	gowl_bar_panel_set_width(panel, 420);

	item = gowl_bar_panel_add_hero(panel, WM_ICON_MAIL,
		(wm->title != NULL && *wm->title != '\0')
			? wm->title : bar_webmail_provider_name(provider),
		wm_state_text(wm, running));
	if (wm->unread > 0)
		gowl_bar_panel_item_set_color(item, GOWL_BAR_COLOR_ACCENT);

	if (missing && !running) {
		g_autofree gchar *text = NULL;

		text = g_strdup_printf(
			"%s is not on PATH. The window is a gsurf kiosk: gsurf "
			"is built with cmacs (deps/gsurf) and ships in the "
			"Immutablue images; its runtime needs the webkit2gtk4.1 "
			"package. Or point webmail.browser-command at another "
			"browser.",
			(missing_cmd != NULL) ? missing_cmd : "gsurf");
		item = gowl_bar_panel_add_label(panel, text);
		gowl_bar_panel_item_set_color(item, GOWL_BAR_COLOR_YELLOW);
	}
	if (error != NULL) {
		item = gowl_bar_panel_add_row(panel, "error", WM_ICON_WARN,
		                              "Problem", error);
		gowl_bar_panel_item_set_color(item, GOWL_BAR_COLOR_RED);
	}

	unread = (wm->unread >= 0)
		? g_strdup_printf("%d%s", wm->unread, wm->plus ? "+" : "")
		: g_strdup(running ? "unknown" : "\xe2\x80\x94");
	gowl_bar_panel_add_field_pair(panel, "Unread", unread,
		"Service", bar_webmail_provider_name(provider));
	gowl_bar_panel_add_field(panel, "Address",
	                         (host != NULL) ? host : url);

	if (wm->state == BAR_WEBMAIL_WINDOW_STARTING && !launching &&
	    now - wm->launched_at > WM_MAP_PATIENCE) {
		item = gowl_bar_panel_add_label(panel,
			"The browser is running but its window has not been "
			"recognised. With a custom browser-command, set "
			"webmail.app-id or webmail.title-match.");
		gowl_bar_panel_item_set_color(item, GOWL_BAR_COLOR_SUBTEXT);
	}

	if (stale) {
		item = gowl_bar_panel_add_row(panel, "restart", WM_ICON_RELOAD,
			"Restart to apply the theme",
			"The palette changed; the open window has the old one");
		gowl_bar_panel_item_set_color(item, GOWL_BAR_COLOR_YELLOW);
	}

	gowl_bar_panel_add_separator(panel);
	item = gowl_bar_panel_add_buttons(panel, "window");
	gowl_bar_panel_add_button(item,
		(wm->state == BAR_WEBMAIL_WINDOW_FOCUSED ||
		 wm->state == BAR_WEBMAIL_WINDOW_VISIBLE) ? "Hide" : "Open",
		FALSE);
	gowl_bar_panel_add_button(item, "Compose", FALSE);

	item = gowl_bar_panel_add_buttons(panel, "tools");
	gowl_bar_panel_add_button(item, "Reload theme", FALSE);
	gowl_bar_panel_add_button(item,
		bar_webmail_confirm_armed(wm->quit_armed_at, now)
			? "Click again to quit" : "Quit",
		bar_webmail_confirm_armed(wm->quit_armed_at, now));
	if (!running)
		gowl_bar_panel_item_set_disabled(
			gowl_bar_panel_item_get_child(item, 1), TRUE);
	return panel;
}

static void
wm_close_panel(GowlBarPlugin *plugin)
{
	GowlBarHost *host = gowl_bar_plugin_get_host(plugin);

	if (host != NULL)
		gowl_bar_host_close_panel(host);
}

/* Quit: the window's own close first, so gsurf exits the way it would
   from Ctrl+q and flushes its cookies; a signal only when there is no
   window to ask. */
static void
wm_quit(GowlBarPlugin *plugin, Webmail *wm)
{
	GowlCompositor *comp = wm_compositor();
	g_autofree gchar *config_path = wm_run_file(plugin, "gsurf.yaml");
	GowlClient *c = NULL;
	GPid pid;

	g_mutex_lock(&wm->lock);
	pid = wm->pid;
	g_mutex_unlock(&wm->lock);
	wm->user_quit = TRUE;
	wm->hide_on_map = FALSE;
	if (comp != NULL)
		c = wm_find_client(plugin, wm, comp, pid);
	if (c != NULL)
		gowl_client_close(c);
	else
		wm_terminate(pid, config_path);
}

static void
wm_action(GowlBarPlugin *plugin, gpointer data, const gchar *item_id,
          gint index, gdouble value, guint button)
{
	Webmail *wm = data;
	gint64 now = g_get_monotonic_time();

	(void)value;
	(void)button;

	if (g_strcmp0(item_id, "window") == 0 && index == 0) {
		BarWebmailAction act;

		act = (wm->state == BAR_WEBMAIL_WINDOW_FOCUSED ||
		       wm->state == BAR_WEBMAIL_WINDOW_VISIBLE)
			? BAR_WEBMAIL_ACTION_HIDE
			: bar_webmail_decide_show(wm->state,
			                          wm_bring_here(plugin));
		wm_close_panel(plugin);
		wm_do(plugin, wm, act);
		return;
	}
	if (g_strcmp0(item_id, "window") == 0 && index == 1) {
		wm_close_panel(plugin);
		wm_compose(plugin, wm);
		return;
	}
	if (g_strcmp0(item_id, "tools") == 0 && index == 0) {
		gboolean running, hidden;

		g_mutex_lock(&wm->lock);
		running = (wm->pid > 1);
		g_mutex_unlock(&wm->lock);
		hidden = (wm->state == BAR_WEBMAIL_WINDOW_MINIMIZED);
		/* Rewrite always; restart only a window that exists. */
		g_clear_pointer(&wm->sig, g_free);
		if (running)
			wm_restart(plugin, wm, hidden);
		else
			wm_queue_job(plugin, wm, NULL, FALSE, 0);
		return;
	}
	if (g_strcmp0(item_id, "restart") == 0) {
		wm_restart(plugin, wm,
		           wm->state == BAR_WEBMAIL_WINDOW_MINIMIZED);
		return;
	}
	if (g_strcmp0(item_id, "tools") == 0 && index == 1) {
		if (!bar_webmail_confirm_armed(wm->quit_armed_at, now)) {
			wm->quit_armed_at = now;
			return;
		}
		wm->quit_armed_at = 0;
		wm_quit(plugin, wm);
		return;
	}
	if (g_strcmp0(item_id, "error") == 0)
		wm_set_error(wm, NULL);
}

static void
wm_panel_closed(GowlBarPlugin *plugin, gpointer data)
{
	Webmail *wm = data;

	(void)plugin;
	/* An armed Quit does not survive the panel closing. */
	wm->quit_armed_at = 0;
}

static void
wm_panel_opened(GowlBarPlugin *plugin, gpointer data)
{
	(void)data;
	/* Show the state as of now, not as of the last poll. */
	gowl_bar_plugin_poll(plugin);
}

static const GowlBarPluginVTable webmail_vtable = {
	sizeof(GowlBarPluginVTable),
	wm_create, wm_destroy,
	NULL, NULL, wm_configure,
	wm_interval, wm_poll, wm_poll_async,
	NULL, NULL,
	wm_click, NULL,
	wm_panel, wm_action,
	wm_panel_opened, wm_panel_closed,
	NULL,
	NULL,
	NULL, NULL
};

/**
 * bar_register_webmail_plugins:
 * @registry: the registry to add to
 *
 * Registers `webmail' and its aliases `outlook' and `mail-web'.
 */
void
bar_register_webmail_plugins(GowlBarRegistry *registry)
{
	gowl_bar_registry_register_vtable(registry, "webmail", "Web mail",
		"A themed web mail window with an unread count",
		&webmail_vtable);
	gowl_bar_registry_register_alias(registry, "outlook", "webmail");
	gowl_bar_registry_register_alias(registry, "mail-web", "webmail");
}
