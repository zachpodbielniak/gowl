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
 * The `tvremote' widget: a remote control for the TV across the room.
 *
 * Fire TV and Android TV over adb, Roku over ECP, Samsung and LG over
 * their websocket APIs, and -- when the tools are installed --
 * Chromecast through `catt' and Apple TV through `atvremote'.  The bar
 * shows what the default TV is playing and its volume; the wheel is the
 * TV's volume, right-click play/pause, middle-click mute.  The panel is
 * a drawn remote (a canvas: D-pad ring, back/home/menu, transport,
 * volume), a text field that types on the TV, its apps, and the
 * add / pair / scan flow.
 *
 * THREADS.  Nothing here talks to a TV on the compositor thread.  The
 * async poll (a worker) reloads the device list when it changes, asks
 * the default device -- and the panel's, while it is open -- for its
 * status, and swaps the answers in under `lock'.  Every click, key and
 * text submit becomes a TvJob on gowl_bar_plugin_queue_work(), which
 * runs the backend on a worker.  The panel, the canvas, the label and
 * the hit-test only ever read what is already cached.
 *
 * The device half lives in bar-tvremote-*.c: the model (keymaps,
 * parsers, layout -- pure), the config file, the websocket client and
 * one backend per kind.
 */

#include <linux/input-event-codes.h>
#include <xkbcommon/xkbcommon-keysyms.h>
#include <string.h>
#include <sys/stat.h>
#include <glib/gstdio.h>
#include <pango/pangocairo.h>

#include "gowl-enums.h"
#include "bar-internal.h"
#include "bar-tvremote-model.h"
#include "bar-tvremote-backend.h"

#define TV_ICON         "\xef\x89\xac"     /* fa-tv */
#define TV_PANEL_WIDTH  (360)
#define TV_ERROR_SHOW_US (30 * G_USEC_PER_SEC)
#define TV_ARM_US        (5 * G_USEC_PER_SEC)
#define TV_PRESS_FLASH_US (600 * 1000)
#define TV_MAX_APP_ROWS  (40)

typedef struct {
	/* Guards every field below.  The worker builds fresh values and
	   swaps them in; readers on the compositor thread copy what they
	   need out and let go. */
	GMutex           lock;

	GPtrArray       *devices;        /* BarTvremoteDevice* */
	gchar           *default_id;
	gchar           *selected_id;    /* the panel's device */
	gchar           *config_path;
	gint64           config_mtime;   /* of what was loaded; -1: absent */
	gboolean         config_loaded;  /* config_mtime means something */
	gchar           *config_error;

	/* id -> BarTvremoteSession*.  Sessions are created on first use
	   and never removed before destroy, so a pointer taken under the
	   lock stays valid after it is released. */
	GHashTable      *sessions;
	GHashTable      *statuses;       /* id -> BarTvremoteStatus* */

	GPtrArray       *apps;           /* BarTvremoteApp*, for apps_device */
	gchar           *apps_device;
	gboolean         apps_loading;
	gchar           *apps_error;

	gchar           *last_error;
	gint64           last_error_at;
	gchar           *text_note;

	gboolean         have_adb;
	gboolean         have_curl;
	gboolean         have_catt;
	gboolean         have_atv;

	gboolean         panel_open;
	gint             canvas_index;   /* panel item of the pad, -1 */
	gint             canvas_w;       /* as last painted */
	gint             canvas_h;
	gint             last_key;
	gint64           last_key_at;

	gchar           *add_host;
	BarTvremoteKind  add_kind;
	guint            add_rev;
	gchar           *add_error;
	guint            text_rev;

	gboolean         scanning;
	GPtrArray       *scan_results;   /* BarTvremoteFound* */
	gchar           *scan_error;

	gchar           *forget_armed;
	gint64           forget_armed_at;
	guint            jobs;           /* in flight, for the busy marks */
} TvData;

/* ----------------------------------------------------------------
 * Settings and colours
 * ---------------------------------------------------------------- */

/* The device-list path: the `config' setting, else XDG config. */
static gchar *
tv_config_path(GowlBarPlugin *plugin)
{
	const gchar *set = gowl_bar_plugin_get_setting(plugin, "config");

	if (set != NULL && set[0] != '\0')
		return bar_expand_tilde(set);
	return g_build_filename(g_get_user_config_dir(), "gowl",
	                        "tvremote.yaml", NULL);
}

static gchar *
tv_setting_dup(GowlBarPlugin *plugin, const gchar *key, const gchar *fallback)
{
	const gchar *v = gowl_bar_plugin_get_setting(plugin, key);

	return g_strdup((v != NULL && v[0] != '\0') ? v : fallback);
}

/*
 * A `color-*' setting for something the PLUGIN paints (a panel item, a
 * canvas): the bar's three-way rule -- a role name, a `#rrggbb'
 * literal, else the fallback role.  Role names are matched against
 * gowl_bar_theme_color_name() so that the literal is never dropped on
 * the floor by a role-only parser.
 *
 * Returns: %TRUE when @spec is a literal, with @rgb filled; %FALSE with
 *   @role set otherwise
 */
static gboolean
tv_color(const gchar *spec, GowlBarColor fallback, GowlBarColor *role,
         gdouble rgb[3])
{
	guint i;

	*role = fallback;
	if (spec == NULL || spec[0] == '\0')
		return FALSE;
	if (bar_tvremote_parse_hex(spec, rgb))
		return TRUE;
	for (i = 0; i < GOWL_BAR_COLOR_COUNT; i++) {
		if (g_ascii_strcasecmp(spec,
		        gowl_bar_theme_color_name((GowlBarColor)i)) == 0) {
			*role = (GowlBarColor)i;
			return FALSE;
		}
	}
	return FALSE;
}

/* The role a panel item uses for @key (a literal falls back: panel
   items take roles only). */
static GowlBarColor
tv_role(GowlBarPlugin *plugin, const gchar *key, GowlBarColor fallback)
{
	GowlBarColor role;
	gdouble rgb[3];

	if (tv_color(gowl_bar_plugin_get_setting(plugin, key), fallback, &role,
	             rgb))
		return fallback;
	return role;
}

/* ----------------------------------------------------------------
 * Lifecycle
 * ---------------------------------------------------------------- */

static gpointer
tv_create(GowlBarPlugin *plugin)
{
	TvData *td;

	(void)plugin;
	td = g_new0(TvData, 1);
	g_mutex_init(&td->lock);
	td->devices = g_ptr_array_new_with_free_func(
		(GDestroyNotify)bar_tvremote_device_free);
	td->sessions = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
		(GDestroyNotify)bar_tvremote_session_free);
	td->statuses = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
		(GDestroyNotify)bar_tvremote_status_free);
	td->config_mtime = -1;
	td->canvas_index = -1;
	td->last_key = -1;
	td->add_kind = BAR_TVREMOTE_KIND_FIRETV;
	return td;
}

static void
tv_destroy(GowlBarPlugin *plugin, gpointer data)
{
	TvData *td = data;

	(void)plugin;
	if (td == NULL)
		return;
	/* The host holds a reference on the plugin for every queued job,
	   so nothing is running on a worker by now. */
	g_hash_table_unref(td->sessions);
	g_hash_table_unref(td->statuses);
	g_ptr_array_unref(td->devices);
	if (td->apps != NULL)
		g_ptr_array_unref(td->apps);
	if (td->scan_results != NULL)
		g_ptr_array_unref(td->scan_results);
	g_free(td->default_id);
	g_free(td->selected_id);
	g_free(td->config_path);
	g_free(td->config_error);
	g_free(td->apps_device);
	g_free(td->apps_error);
	g_free(td->last_error);
	g_free(td->text_note);
	g_free(td->add_host);
	g_free(td->add_error);
	g_free(td->scan_error);
	g_free(td->forget_armed);
	g_mutex_clear(&td->lock);
	g_free(td);
}

/*
 * Stop every persistent connection (adb shells, websockets).  A
 * session a worker is using right now is skipped -- waiting for it
 * here would block the compositor for as long as that call takes -- and
 * is stopped when the plugin is destroyed instead.
 */
static void
tv_deactivate(GowlBarPlugin *plugin, gpointer data)
{
	TvData *td = data;
	GHashTableIter it;
	gpointer value;

	(void)plugin;
	g_mutex_lock(&td->lock);
	g_hash_table_iter_init(&it, td->sessions);
	while (g_hash_table_iter_next(&it, NULL, &value)) {
		BarTvremoteSession *s = value;

		if (g_mutex_trylock(&s->io)) {
			bar_tvremote_session_reset(s);
			g_mutex_unlock(&s->io);
		}
	}
	g_mutex_unlock(&td->lock);
}

/* ----------------------------------------------------------------
 * State helpers (call with td->lock held)
 * ---------------------------------------------------------------- */

static BarTvremoteDevice *
tv_default_locked(TvData *td)
{
	BarTvremoteDevice *d;

	d = bar_tvremote_device_find(td->devices, td->default_id);
	if (d == NULL && td->devices->len > 0)
		d = g_ptr_array_index(td->devices, 0);
	return d;
}

static BarTvremoteDevice *
tv_selected_locked(TvData *td)
{
	BarTvremoteDevice *d;

	d = bar_tvremote_device_find(td->devices, td->selected_id);
	return (d != NULL) ? d : tv_default_locked(td);
}

static BarTvremoteSession *
tv_session_locked(TvData *td, const gchar *id)
{
	BarTvremoteSession *s;

	s = g_hash_table_lookup(td->sessions, id);
	if (s == NULL) {
		s = bar_tvremote_session_new();
		g_hash_table_insert(td->sessions, g_strdup(id), s);
	}
	return s;
}

static void
tv_set_error_locked(TvData *td, const gchar *msg)
{
	g_free(td->last_error);
	td->last_error = (msg != NULL) ? bar_tvremote_clean_text(msg, 300)
	                               : NULL;
	td->last_error_at = g_get_monotonic_time();
}

static const gchar *
tv_recent_error_locked(TvData *td)
{
	if (td->last_error == NULL ||
	    g_get_monotonic_time() - td->last_error_at > TV_ERROR_SHOW_US)
		return NULL;
	return td->last_error;
}

/* ----------------------------------------------------------------
 * The bar label
 * ---------------------------------------------------------------- */

/* Label, colour and tooltip from the cached status of the default
   device.  Called from the worker after a poll and from the
   compositor thread after a configure; only uses the thread-safe
   setters. */
static void
tv_apply_state(GowlBarPlugin *plugin, TvData *td)
{
	g_autofree gchar *label = NULL;
	g_autofree gchar *tip = NULL;
	BarTvremoteDevice *dev;
	BarTvremoteStatus *st = NULL;
	BarTvremoteLink link = BAR_TVREMOTE_LINK_UNKNOWN;
	const gchar *err;

	g_mutex_lock(&td->lock);
	dev = tv_default_locked(td);
	if (dev != NULL)
		st = g_hash_table_lookup(td->statuses, dev->id);
	if (st != NULL)
		link = st->link;
	err = tv_recent_error_locked(td);

	if (dev == NULL) {
		tip = g_strdup(td->config_error != NULL
			? td->config_error
			: "TV remote: no TVs yet \xe2\x80\x94 click to add one");
	} else if (st == NULL) {
		tip = g_strdup_printf("%s: connecting\xe2\x80\xa6", dev->name);
	} else if (link == BAR_TVREMOTE_LINK_ONLINE) {
		GString *t = g_string_new(dev->name);

		label = bar_tvremote_format_label(link, st->app_name, st->volume,
		                                  st->muted);
		if (st->app_name != NULL)
			g_string_append_printf(t, ": %s", st->app_name);
		if (st->title != NULL)
			g_string_append_printf(t, " \xc2\xb7 %s", st->title);
		if (st->muted)
			g_string_append(t, " \xc2\xb7 muted");
		else if (st->volume >= 0)
			g_string_append_printf(t, " \xc2\xb7 volume %d", st->volume);
		g_string_append(t, "\nScroll: volume \xc2\xb7 right-click: "
		                   "play/pause \xc2\xb7 middle-click: mute");
		tip = g_string_free(t, FALSE);
	} else {
		label = bar_tvremote_format_label(link, NULL, -1, FALSE);
		tip = g_strdup_printf("%s: %s", dev->name,
			st->detail != NULL ? st->detail : "offline");
	}
	if (err != NULL) {
		gchar *with = g_strdup_printf("%s\n%s", tip, err);

		g_free(tip);
		tip = with;
	}
	g_mutex_unlock(&td->lock);

	gowl_bar_plugin_set_icon(plugin, TV_ICON);
	gowl_bar_plugin_set_label(plugin, label);
	gowl_bar_plugin_set_tooltip(plugin, tip);
	if (link == BAR_TVREMOTE_LINK_ONLINE)
		bar_plugin_apply_color(plugin,
			gowl_bar_plugin_get_setting(plugin, "color-online"),
			GOWL_BAR_COLOR_GREEN);
	else if (link == BAR_TVREMOTE_LINK_PAIRING)
		bar_plugin_apply_color(plugin,
			gowl_bar_plugin_get_setting(plugin, "color-pairing"),
			GOWL_BAR_COLOR_YELLOW);
	else
		bar_plugin_apply_color(plugin,
			gowl_bar_plugin_get_setting(plugin, "color-offline"),
			GOWL_BAR_COLOR_SUBTEXT);
}

static void
tv_configure(GowlBarPlugin *plugin, gpointer data, GHashTable *settings)
{
	TvData *td = data;
	g_autofree gchar *path = NULL;

	(void)settings;
	/* A changed `config' path is a different device list: forget the
	   loaded one so the next poll reads the new file. */
	path = tv_config_path(plugin);
	g_mutex_lock(&td->lock);
	if (g_strcmp0(path, td->config_path) != 0) {
		g_free(td->config_path);
		td->config_path = g_steal_pointer(&path);
		td->config_loaded = FALSE;
	}
	g_mutex_unlock(&td->lock);
	tv_apply_state(plugin, td);
}

static gint
tv_interval(GowlBarPlugin *plugin, gpointer data)
{
	TvData *td = data;
	GHashTableIter it;
	gpointer value;
	gboolean pairing = FALSE;
	gboolean open;

	g_mutex_lock(&td->lock);
	g_hash_table_iter_init(&it, td->statuses);
	while (g_hash_table_iter_next(&it, NULL, &value))
		if (((BarTvremoteStatus *)value)->link == BAR_TVREMOTE_LINK_PAIRING)
			pairing = TRUE;
	open = td->panel_open;
	g_mutex_unlock(&td->lock);

	/* Once a second while somebody is at the TV accepting a prompt,
	   so the remote comes alive the moment they do. */
	if (pairing)
		return 1;
	if (open)
		return 3;
	return MAX(gowl_bar_plugin_get_setting_int(plugin, "refresh", 15), 2);
}

/* ----------------------------------------------------------------
 * Worker side
 * ---------------------------------------------------------------- */

/* The commands from the settings, plus the secret-saving hook.  Built
   per job on the worker; settings may change between jobs. */
static void tv_save_secret(gpointer user_data, const gchar *device_id,
                           const gchar *field, const gchar *value);

static BarTvremoteTools *
tv_tools_new(GowlBarPlugin *plugin, TvData *td)
{
	BarTvremoteTools *t;

	t = g_new0(BarTvremoteTools, 1);
	t->adb = tv_setting_dup(plugin, "adb", "adb");
	t->curl = tv_setting_dup(plugin, "curl", "curl");
	t->catt = tv_setting_dup(plugin, "catt", "catt");
	t->atvremote = tv_setting_dup(plugin, "atvremote", "atvremote");
	t->save_secret = tv_save_secret;
	t->user_data = td;
	return t;
}

static void
tv_tools_free(BarTvremoteTools *t)
{
	if (t == NULL)
		return;
	g_free(t->adb);
	g_free(t->curl);
	g_free(t->catt);
	g_free(t->atvremote);
	g_free(t);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC(BarTvremoteTools, tv_tools_free)

static gint64
tv_file_mtime(const gchar *path)
{
	GStatBuf st;

	if (path == NULL || g_stat(path, &st) != 0)
		return -1;
	return (gint64)st.st_mtime * G_USEC_PER_SEC +
	       (gint64)(st.st_mtim.tv_nsec / 1000);
}

/*
 * Write the device list.  The list is copied under the lock and
 * written outside it, so the compositor thread is never waiting on a
 * file write; the new mtime is recorded so the next poll does not
 * mistake our own write for an outside edit and reload.
 */
static gboolean
tv_save(TvData *td, GError **error)
{
	g_autoptr(GPtrArray) copy = NULL;
	g_autofree gchar *path = NULL;
	g_autofree gchar *def = NULL;
	gboolean ok;
	guint i;

	copy = g_ptr_array_new_with_free_func(
		(GDestroyNotify)bar_tvremote_device_free);
	g_mutex_lock(&td->lock);
	for (i = 0; i < td->devices->len; i++)
		g_ptr_array_add(copy, bar_tvremote_device_copy(
			g_ptr_array_index(td->devices, i)));
	path = g_strdup(td->config_path);
	def = g_strdup(td->default_id);
	g_mutex_unlock(&td->lock);

	if (path == NULL) {
		g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_NOENT,
		            "no device list path");
		return FALSE;
	}
	ok = bar_tvremote_config_save(path, copy, def, error);
	g_mutex_lock(&td->lock);
	if (ok && g_strcmp0(path, td->config_path) == 0) {
		td->config_mtime = tv_file_mtime(path);
		g_clear_pointer(&td->config_error, g_free);
	}
	g_mutex_unlock(&td->lock);
	return ok;
}

/* Pairing handed back a token or client key: keep it.  Runs on the
   worker inside a backend call. */
static void
tv_save_secret(gpointer user_data, const gchar *device_id,
               const gchar *field, const gchar *value)
{
	TvData *td = user_data;
	BarTvremoteDevice *dev;
	g_autoptr(GError) error = NULL;

	g_mutex_lock(&td->lock);
	dev = bar_tvremote_device_find(td->devices, device_id);
	if (dev != NULL) {
		gchar **slot = (g_strcmp0(field, "token") == 0) ? &dev->token
		                                                : &dev->client_key;

		if (*slot != NULL)
			memset(*slot, 0, strlen(*slot));
		g_free(*slot);
		*slot = g_strdup(value);
	}
	g_mutex_unlock(&td->lock);
	if (dev != NULL && !tv_save(td, &error))
		g_message("tvremote: could not store the pairing key: %s",
		          error->message);
}

/* Whether @a and @b would talk to the same thing the same way. */
static gboolean
tv_same_endpoint(const BarTvremoteDevice *a, const BarTvremoteDevice *b)
{
	return a->kind == b->kind && g_strcmp0(a->host, b->host) == 0 &&
	       bar_tvremote_device_port(a) == bar_tvremote_device_port(b);
}

/*
 * Reload the device list if the file changed.  Sessions of devices
 * that disappeared or now point somewhere else are reset (under their
 * own I/O lock, on this worker), so a stale adb shell or websocket
 * does not keep driving the old address.
 */
static void
tv_reload_config(GowlBarPlugin *plugin, TvData *td)
{
	g_autofree gchar *path = NULL;
	g_autofree gchar *def = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(GPtrArray) stale = NULL;
	GPtrArray *fresh;
	gint64 mtime;
	guint i;

	g_mutex_lock(&td->lock);
	if (td->config_path == NULL)
		td->config_path = tv_config_path(plugin);
	path = g_strdup(td->config_path);
	g_mutex_unlock(&td->lock);

	mtime = tv_file_mtime(path);
	g_mutex_lock(&td->lock);
	if (td->config_loaded && mtime == td->config_mtime) {
		g_mutex_unlock(&td->lock);
		return;
	}
	g_mutex_unlock(&td->lock);

	fresh = bar_tvremote_config_load(path, &def, &error);

	stale = g_ptr_array_new();
	g_mutex_lock(&td->lock);
	td->config_mtime = mtime;
	td->config_loaded = TRUE;
	g_clear_pointer(&td->config_error, g_free);
	if (fresh == NULL) {
		/* Refused (mode) or unreadable: say so, drive nothing. */
		td->config_error = bar_tvremote_clean_text(error->message, 300);
		fresh = g_ptr_array_new_with_free_func(
			(GDestroyNotify)bar_tvremote_device_free);
	}
	for (i = 0; i < td->devices->len; i++) {
		BarTvremoteDevice *old = g_ptr_array_index(td->devices, i);
		BarTvremoteDevice *now = bar_tvremote_device_find(fresh, old->id);
		BarTvremoteSession *s;

		if (now != NULL && tv_same_endpoint(old, now))
			continue;
		s = g_hash_table_lookup(td->sessions, old->id);
		if (s != NULL)
			g_ptr_array_add(stale, s);
		g_hash_table_remove(td->statuses, old->id);
	}
	g_ptr_array_unref(td->devices);
	td->devices = fresh;
	g_free(td->default_id);
	td->default_id = g_steal_pointer(&def);
	g_mutex_unlock(&td->lock);

	for (i = 0; i < stale->len; i++) {
		BarTvremoteSession *s = g_ptr_array_index(stale, i);

		g_mutex_lock(&s->io);
		bar_tvremote_session_reset(s);
		g_mutex_unlock(&s->io);
	}
}

/* Ask one device for its status and cache the answer. */
static void
tv_refresh_device(GowlBarPlugin *plugin, TvData *td, const gchar *id)
{
	g_autoptr(BarTvremoteTools) tools = NULL;
	BarTvremoteDevice *copy = NULL;
	BarTvremoteSession *s = NULL;
	const BarTvremoteBackend *be;
	BarTvremoteStatus *st;

	g_mutex_lock(&td->lock);
	if (bar_tvremote_device_find(td->devices, id) != NULL) {
		copy = bar_tvremote_device_copy(
			bar_tvremote_device_find(td->devices, id));
		s = tv_session_locked(td, id);
	}
	g_mutex_unlock(&td->lock);
	if (copy == NULL)
		return;

	be = bar_tvremote_backend_for(copy->kind);
	tools = tv_tools_new(plugin, td);
	g_mutex_lock(&s->io);
	st = be->status(copy, s, tools);
	g_mutex_unlock(&s->io);

	g_mutex_lock(&td->lock);
	if (bar_tvremote_device_find(td->devices, id) != NULL)
		g_hash_table_replace(td->statuses, g_strdup(id), st);
	else
		bar_tvremote_status_free(st);
	g_mutex_unlock(&td->lock);
	bar_tvremote_device_free(copy);
}

static void
tv_poll_async(GowlBarPlugin *plugin, gpointer data)
{
	TvData *td = data;
	g_autofree gchar *def_id = NULL;
	g_autofree gchar *sel_id = NULL;
	g_autofree gchar *adb = NULL;
	g_autofree gchar *curl = NULL;
	g_autofree gchar *catt = NULL;
	g_autofree gchar *atv = NULL;
	gboolean open;
	BarTvremoteDevice *d;

	tv_reload_config(plugin, td);

	adb = tv_setting_dup(plugin, "adb", "adb");
	curl = tv_setting_dup(plugin, "curl", "curl");
	catt = tv_setting_dup(plugin, "catt", "catt");
	atv = tv_setting_dup(plugin, "atvremote", "atvremote");

	g_mutex_lock(&td->lock);
	td->have_adb = bar_tvremote_have(adb);
	td->have_curl = bar_tvremote_have(curl);
	td->have_catt = bar_tvremote_have(catt);
	td->have_atv = bar_tvremote_have(atv);
	d = tv_default_locked(td);
	def_id = (d != NULL) ? g_strdup(d->id) : NULL;
	open = td->panel_open;
	d = tv_selected_locked(td);
	sel_id = (open && d != NULL) ? g_strdup(d->id) : NULL;
	g_mutex_unlock(&td->lock);

	if (def_id != NULL)
		tv_refresh_device(plugin, td, def_id);
	if (sel_id != NULL && g_strcmp0(sel_id, def_id) != 0)
		tv_refresh_device(plugin, td, sel_id);

	tv_apply_state(plugin, td);
	if (open)
		gowl_bar_plugin_request_panel_refresh(plugin);
}

/* ----------------------------------------------------------------
 * Jobs: everything a click or a key asks of a device
 * ---------------------------------------------------------------- */

typedef enum {
	JOB_KEY,
	JOB_TEXT,
	JOB_APPS,
	JOB_LAUNCH,
	JOB_ADD,
	JOB_FORGET,
	JOB_DEFAULT,
	JOB_SCAN,
	JOB_REFRESH
} TvJobKind;

typedef struct {
	TvData          *td;
	TvJobKind        kind;
	gchar           *device_id;
	gint             key;
	gboolean         longpress;
	guint            repeat;
	gchar           *arg;
	BarTvremoteKind  add_kind;
} TvJob;

static void
tv_job_free(gpointer p)
{
	TvJob *job = p;

	g_free(job->device_id);
	if (job->arg != NULL)
		memset(job->arg, 0, strlen(job->arg));
	g_free(job->arg);
	g_free(job);
}

/* Run @be's call for @id with the session's I/O lock held.  Returns
   the device copy (caller frees) or NULL when the id is gone. */
static BarTvremoteDevice *
tv_job_device(TvData *td, const gchar *id, BarTvremoteSession **s)
{
	BarTvremoteDevice *dev, *copy = NULL;

	g_mutex_lock(&td->lock);
	dev = bar_tvremote_device_find(td->devices, id);
	if (dev != NULL) {
		copy = bar_tvremote_device_copy(dev);
		*s = tv_session_locked(td, id);
	}
	g_mutex_unlock(&td->lock);
	return copy;
}

static void
tv_job_key(GowlBarPlugin *plugin, TvData *td, TvJob *job,
           BarTvremoteTools *tools)
{
	const BarTvremoteDevice *target;
	BarTvremoteDevice *dev;
	BarTvremoteSession *s = NULL;
	g_autofree gchar *target_id = NULL;
	g_autoptr(GError) error = NULL;
	const BarTvremoteBackend *be;
	gboolean ok = TRUE;
	guint i;

	/* Volume keys go where the sound goes (volume-via). */
	g_mutex_lock(&td->lock);
	target = bar_tvremote_route_volume(td->devices, job->device_id,
	                                   (BarTvremoteKey)job->key);
	target_id = (target != NULL) ? g_strdup(target->id) : NULL;
	g_mutex_unlock(&td->lock);
	if (target_id == NULL)
		return;

	dev = tv_job_device(td, target_id, &s);
	if (dev == NULL)
		return;
	be = bar_tvremote_backend_for(dev->kind);
	g_mutex_lock(&s->io);
	for (i = 0; i < MAX(job->repeat, 1u) && ok; i++)
		ok = be->key(dev, s, tools, (BarTvremoteKey)job->key,
		             job->longpress, &error);
	g_mutex_unlock(&s->io);

	g_mutex_lock(&td->lock);
	tv_set_error_locked(td, ok ? NULL : error->message);
	g_mutex_unlock(&td->lock);

	/* A volume or power change shows in the label at once rather than
	   at the next poll. */
	if (ok && (bar_tvremote_key_is_volume((BarTvremoteKey)job->key) ||
	           job->key == BAR_TVREMOTE_KEY_POWER))
		tv_refresh_device(plugin, td, target_id);
	bar_tvremote_device_free(dev);
}

static void
tv_job_text(TvData *td, TvJob *job, BarTvremoteTools *tools)
{
	BarTvremoteDevice *dev;
	BarTvremoteSession *s = NULL;
	g_autoptr(GError) error = NULL;
	guint dropped = 0;
	gboolean ok;

	dev = tv_job_device(td, job->device_id, &s);
	if (dev == NULL)
		return;
	g_mutex_lock(&s->io);
	ok = bar_tvremote_backend_for(dev->kind)->text(dev, s, tools, job->arg,
	                                               &dropped, &error);
	g_mutex_unlock(&s->io);

	g_mutex_lock(&td->lock);
	tv_set_error_locked(td, ok ? NULL : error->message);
	g_clear_pointer(&td->text_note, g_free);
	if (ok && dropped > 0)
		td->text_note = g_strdup_printf("%u character%s could not be "
		                                "typed: this TV takes plain "
		                                "ASCII only", dropped,
		                                dropped == 1 ? "" : "s");
	g_mutex_unlock(&td->lock);
	bar_tvremote_device_free(dev);
}

static void
tv_job_apps(TvData *td, TvJob *job, BarTvremoteTools *tools)
{
	BarTvremoteDevice *dev;
	BarTvremoteSession *s = NULL;
	g_autoptr(GError) error = NULL;
	GPtrArray *apps;

	dev = tv_job_device(td, job->device_id, &s);
	if (dev == NULL)
		return;
	g_mutex_lock(&s->io);
	apps = bar_tvremote_backend_for(dev->kind)->apps(dev, s, tools, &error);
	g_mutex_unlock(&s->io);

	g_mutex_lock(&td->lock);
	td->apps_loading = FALSE;
	g_clear_pointer(&td->apps_error, g_free);
	if (apps == NULL)
		td->apps_error = bar_tvremote_clean_text(error->message, 300);
	if (td->apps != NULL)
		g_ptr_array_unref(td->apps);
	td->apps = apps;
	g_free(td->apps_device);
	td->apps_device = g_strdup(job->device_id);
	g_mutex_unlock(&td->lock);
	bar_tvremote_device_free(dev);
}

static void
tv_job_launch(GowlBarPlugin *plugin, TvData *td, TvJob *job,
              BarTvremoteTools *tools)
{
	BarTvremoteDevice *dev;
	BarTvremoteSession *s = NULL;
	g_autoptr(GError) error = NULL;
	gboolean ok;

	dev = tv_job_device(td, job->device_id, &s);
	if (dev == NULL)
		return;
	g_mutex_lock(&s->io);
	ok = bar_tvremote_backend_for(dev->kind)->launch(dev, s, tools,
	                                                 job->arg, &error);
	g_mutex_unlock(&s->io);
	g_mutex_lock(&td->lock);
	tv_set_error_locked(td, ok ? NULL : error->message);
	g_mutex_unlock(&td->lock);
	if (ok)
		tv_refresh_device(plugin, td, job->device_id);
	bar_tvremote_device_free(dev);
}

static void
tv_job_add(GowlBarPlugin *plugin, TvData *td, TvJob *job)
{
	g_autofree gchar *host = NULL;
	g_autofree gchar *id = NULL;
	g_autoptr(GError) error = NULL;
	BarTvremoteDevice *dev;
	gchar *colon;
	gint port = 0;

	/* `HOST' or `HOST:PORT' (IPv4 / a name; an IPv6 address has more
	   than one colon and is taken whole). */
	host = g_strstrip(g_strdup(job->arg != NULL ? job->arg : ""));
	colon = strchr(host, ':');
	if (colon != NULL && strchr(colon + 1, ':') == NULL) {
		gint64 p = g_ascii_strtoll(colon + 1, NULL, 10);

		*colon = '\0';
		if (p > 0 && p < 65536)
			port = (gint)p;
	}

	g_mutex_lock(&td->lock);
	g_clear_pointer(&td->add_error, g_free);
	if (!bar_tvremote_valid_host(host)) {
		td->add_error = g_strdup("Type the TV's IP address first");
		g_mutex_unlock(&td->lock);
		return;
	}
	if (td->config_error != NULL) {
		td->add_error = g_strdup("The device list cannot be read; fix it "
		                         "first (see above)");
		g_mutex_unlock(&td->lock);
		return;
	}
	id = bar_tvremote_device_make_id(td->devices, job->add_kind, host);
	dev = bar_tvremote_device_new(id, job->add_kind, host);
	dev->port = port;
	g_ptr_array_add(td->devices, dev);
	if (td->default_id == NULL ||
	    bar_tvremote_device_find(td->devices, td->default_id) == NULL) {
		g_free(td->default_id);
		td->default_id = g_strdup(id);
	}
	g_free(td->selected_id);
	td->selected_id = g_strdup(id);
	g_clear_pointer(&td->add_host, g_free);
	td->add_rev++;
	g_mutex_unlock(&td->lock);

	if (!tv_save(td, &error)) {
		g_mutex_lock(&td->lock);
		td->add_error = g_strdup_printf("Added, but not saved: %s",
		                                error->message);
		g_mutex_unlock(&td->lock);
	}
	/* Connecting now is what puts the pairing prompt on the TV. */
	tv_refresh_device(plugin, td, id);
}

static void
tv_job_forget(TvData *td, TvJob *job)
{
	g_autoptr(GError) error = NULL;
	BarTvremoteSession *s;
	guint i;

	g_mutex_lock(&td->lock);
	for (i = 0; i < td->devices->len; i++) {
		BarTvremoteDevice *d = g_ptr_array_index(td->devices, i);

		if (g_strcmp0(d->id, job->device_id) == 0) {
			g_ptr_array_remove_index(td->devices, i);
			break;
		}
	}
	if (g_strcmp0(td->default_id, job->device_id) == 0) {
		g_free(td->default_id);
		td->default_id = (td->devices->len > 0)
			? g_strdup(((BarTvremoteDevice *)
			            g_ptr_array_index(td->devices, 0))->id)
			: NULL;
	}
	if (g_strcmp0(td->selected_id, job->device_id) == 0)
		g_clear_pointer(&td->selected_id, g_free);
	g_hash_table_remove(td->statuses, job->device_id);
	s = g_hash_table_lookup(td->sessions, job->device_id);
	g_mutex_unlock(&td->lock);

	if (s != NULL) {
		g_mutex_lock(&s->io);
		bar_tvremote_session_reset(s);
		g_mutex_unlock(&s->io);
	}
	if (!tv_save(td, &error)) {
		g_mutex_lock(&td->lock);
		tv_set_error_locked(td, error->message);
		g_mutex_unlock(&td->lock);
	}
}

static void
tv_job_default(TvData *td, TvJob *job)
{
	g_autoptr(GError) error = NULL;

	g_mutex_lock(&td->lock);
	if (bar_tvremote_device_find(td->devices, job->device_id) != NULL) {
		g_free(td->default_id);
		td->default_id = g_strdup(job->device_id);
	}
	g_mutex_unlock(&td->lock);
	if (!tv_save(td, &error)) {
		g_mutex_lock(&td->lock);
		tv_set_error_locked(td, error->message);
		g_mutex_unlock(&td->lock);
	}
}

static void
tv_job_scan(GowlBarPlugin *plugin, TvData *td, BarTvremoteTools *tools)
{
	g_autofree gchar *subnet = NULL;
	g_autoptr(GError) error = NULL;
	GPtrArray *found;

	subnet = tv_setting_dup(plugin, "scan-subnet", "auto");
	found = bar_tvremote_scan(tools, subnet, &error);
	g_mutex_lock(&td->lock);
	td->scanning = FALSE;
	g_clear_pointer(&td->scan_error, g_free);
	if (td->scan_results != NULL)
		g_ptr_array_unref(td->scan_results);
	td->scan_results = found;
	if (found == NULL)
		td->scan_error = g_strdup(error->message);
	else if (found->len == 0)
		td->scan_error = g_strdup("No TVs answered. A Fire TV only "
		                          "shows up with ADB debugging on.");
	g_mutex_unlock(&td->lock);
}

static void
tv_job_run(GowlBarPlugin *plugin, gpointer user_data)
{
	TvJob *job = user_data;
	TvData *td = job->td;
	g_autoptr(BarTvremoteTools) tools = tv_tools_new(plugin, td);

	switch (job->kind) {
	case JOB_KEY:
		tv_job_key(plugin, td, job, tools);
		break;
	case JOB_TEXT:
		tv_job_text(td, job, tools);
		break;
	case JOB_APPS:
		tv_job_apps(td, job, tools);
		break;
	case JOB_LAUNCH:
		tv_job_launch(plugin, td, job, tools);
		break;
	case JOB_ADD:
		tv_job_add(plugin, td, job);
		break;
	case JOB_FORGET:
		tv_job_forget(td, job);
		break;
	case JOB_DEFAULT:
		tv_job_default(td, job);
		break;
	case JOB_SCAN:
		tv_job_scan(plugin, td, tools);
		break;
	case JOB_REFRESH:
		if (job->device_id != NULL)
			tv_refresh_device(plugin, td, job->device_id);
		break;
	default:
		break;
	}

	g_mutex_lock(&td->lock);
	if (td->jobs > 0)
		td->jobs--;
	g_mutex_unlock(&td->lock);
	tv_apply_state(plugin, td);
	gowl_bar_plugin_request_panel_refresh(plugin);
}

/* Queue a job; call WITHOUT td->lock held. */
static void
tv_queue(GowlBarPlugin *plugin, TvData *td, TvJob *job)
{
	job->td = td;
	g_mutex_lock(&td->lock);
	td->jobs++;
	g_mutex_unlock(&td->lock);
	gowl_bar_plugin_queue_work(plugin, tv_job_run, job, tv_job_free);
}

static TvJob *
tv_job_new(TvJobKind kind, const gchar *device_id)
{
	TvJob *job = g_new0(TvJob, 1);

	job->kind = kind;
	job->device_id = g_strdup(device_id);
	job->key = -1;
	job->repeat = 1;
	return job;
}

/* Press @key on the panel's device (or the default, @on_default). */
static gboolean
tv_press(GowlBarPlugin *plugin, TvData *td, BarTvremoteKey key,
         gboolean longpress, guint repeat, gboolean on_default)
{
	BarTvremoteDevice *dev;
	TvJob *job;

	g_mutex_lock(&td->lock);
	dev = on_default ? tv_default_locked(td) : tv_selected_locked(td);
	if (dev == NULL) {
		g_mutex_unlock(&td->lock);
		return FALSE;
	}
	job = tv_job_new(JOB_KEY, dev->id);
	td->last_key = (gint)key;
	td->last_key_at = g_get_monotonic_time();
	g_mutex_unlock(&td->lock);

	job->key = (gint)key;
	job->longpress = longpress;
	job->repeat = repeat;
	tv_queue(plugin, td, job);
	return TRUE;
}

/* ----------------------------------------------------------------
 * The bar widget's own clicks
 * ---------------------------------------------------------------- */

static gboolean
tv_click(GowlBarPlugin *plugin, gpointer data, guint button, gint x, gint y,
         guint modifiers)
{
	TvData *td = data;

	(void)x;
	(void)y;
	(void)modifiers;
	/* Left opens the panel (the default); these two are the things
	   reached for without looking. */
	if (button == BTN_RIGHT)
		return tv_press(plugin, td, BAR_TVREMOTE_KEY_PLAY_PAUSE, FALSE, 1,
		                TRUE);
	if (button == BTN_MIDDLE)
		return tv_press(plugin, td, BAR_TVREMOTE_KEY_MUTE, FALSE, 1, TRUE);
	return FALSE;
}

static gboolean
tv_scroll(GowlBarPlugin *plugin, gpointer data, gdouble delta,
          gint discrete, guint modifiers)
{
	TvData *td = data;
	gint steps;

	(void)modifiers;
	/* A touchpad has no discrete steps, only a delta. */
	steps = (discrete != 0) ? -discrete : ((delta > 0.0) ? -1 : 1);
	if (steps == 0)
		return FALSE;
	return tv_press(plugin, td,
	                steps > 0 ? BAR_TVREMOTE_KEY_VOL_UP
	                          : BAR_TVREMOTE_KEY_VOL_DOWN,
	                FALSE, (guint)MIN(ABS(steps), 5), TRUE);
}

/* ----------------------------------------------------------------
 * The panel
 * ---------------------------------------------------------------- */

static const gchar *
tv_link_word(BarTvremoteLink link)
{
	switch (link) {
	case BAR_TVREMOTE_LINK_ONLINE:     return "online";
	case BAR_TVREMOTE_LINK_PAIRING:    return "waiting for the TV";
	case BAR_TVREMOTE_LINK_CONNECTING: return "connecting";
	case BAR_TVREMOTE_LINK_NO_TOOL:    return "tool missing";
	case BAR_TVREMOTE_LINK_OFFLINE:    return "offline";
	case BAR_TVREMOTE_LINK_UNKNOWN:
	default:                           return "\xe2\x80\xa6";
	}
}

/* The kinds the Add form offers: always the five native ones, the two
   CLI kinds only when their tool is installed.  The panel builder and
   the action both use this, so a button index means the same kind in
   both.  Returns how many were written. */
static guint
tv_offered_kinds(TvData *td, BarTvremoteKind *out)
{
	guint n = 0;

	out[n++] = BAR_TVREMOTE_KIND_FIRETV;
	out[n++] = BAR_TVREMOTE_KIND_ANDROIDTV;
	out[n++] = BAR_TVREMOTE_KIND_ROKU;
	out[n++] = BAR_TVREMOTE_KIND_SAMSUNG;
	out[n++] = BAR_TVREMOTE_KIND_LG;
	if (td->have_catt)
		out[n++] = BAR_TVREMOTE_KIND_CAST;
	if (td->have_atv)
		out[n++] = BAR_TVREMOTE_KIND_APPLETV;
	return n;
}

/* Which tool @kind needs and whether it is there, for the hint line. */
static const gchar *
tv_missing_tool_locked(TvData *td, BarTvremoteKind kind)
{
	if (bar_tvremote_kind_uses_adb(kind) && !td->have_adb)
		return "adb is not installed: install the android-tools package";
	if (kind == BAR_TVREMOTE_KIND_ROKU && !td->have_curl)
		return "curl is not installed: install the curl package";
	return NULL;
}

static void
tv_panel_device_section(GowlBarPlugin *plugin, TvData *td,
                        GowlBarPanel *panel, BarTvremoteDevice *sel)
{
	GowlBarPanelItem *item;
	guint i;

	if (td->devices->len == 0)
		return;
	gowl_bar_panel_add_section(panel, "Devices");
	for (i = 0; i < td->devices->len; i++) {
		BarTvremoteDevice *d = g_ptr_array_index(td->devices, i);
		BarTvremoteStatus *st = g_hash_table_lookup(td->statuses, d->id);
		g_autofree gchar *id = g_strdup_printf("dev:%s", d->id);
		g_autofree gchar *sub = NULL;
		gboolean is_default;

		is_default = (tv_default_locked(td) == d);
		sub = g_strdup_printf("%s \xc2\xb7 %s \xc2\xb7 %s%s",
			bar_tvremote_kind_title(d->kind), d->host,
			tv_link_word(st != NULL ? st->link
			                        : BAR_TVREMOTE_LINK_UNKNOWN),
			d->volume_via != NULL ? " \xc2\xb7 sound via another device"
			                      : "");
		item = gowl_bar_panel_add_row(panel, id, TV_ICON, d->name, sub);
		gowl_bar_panel_item_set_selected(item, d == sel);
		if (is_default)
			gowl_bar_panel_item_set_badge(item, "default");
	}
	if (sel != NULL) {
		gboolean armed;

		armed = g_strcmp0(td->forget_armed, sel->id) == 0 &&
		        g_get_monotonic_time() - td->forget_armed_at < TV_ARM_US;
		item = gowl_bar_panel_add_buttons(panel, "devops");
		if (tv_default_locked(td) != sel)
			gowl_bar_panel_add_button(item, "Make default", FALSE);
		gowl_bar_panel_add_button(item,
			armed ? "Click again to forget" : "Forget", armed);
	}
	(void)plugin;
}

static void
tv_panel_remote_section(GowlBarPlugin *plugin, TvData *td,
                        GowlBarPanel *panel, BarTvremoteDevice *sel)
{
	GowlBarPanelItem *item;
	g_autofree gchar *rev = NULL;
	guint i;

	td->canvas_index = (gint)gowl_bar_panel_n_items(panel);
	item = gowl_bar_panel_add_canvas(panel, "pad",
	                                 bar_tvremote_canvas_height(sel->kind));
	/* The keyboard starts on the remote: it is what the panel is for. */
	gowl_bar_panel_item_set_prop(item, GOWL_BAR_ENTRY_PROP_AUTOFOCUS,
	                             "true");
	gowl_bar_panel_add_label(panel,
		"Keys on the remote: arrows, w a s d or j k move "
		"\xc2\xb7 Return or Space OK \xc2\xb7 BackSpace back \xc2\xb7 h home "
		"\xc2\xb7 m menu \xc2\xb7 p play \xc2\xb7 r / f rewind / forward "
		"\xc2\xb7 + \xe2\x88\x92 or the wheel volume \xc2\xb7 0 mute \xc2\xb7 "
		"L hold OK");

	if (sel->kind != BAR_TVREMOTE_KIND_CAST) {
		item = gowl_bar_panel_add_entry(panel, "text",
		                                "Type text on the TV, Return sends",
		                                "");
		rev = g_strdup_printf("%u", td->text_rev);
		gowl_bar_panel_item_set_prop(item, GOWL_BAR_ENTRY_PROP_REVISION,
		                             rev);
		if (td->text_note != NULL)
			gowl_bar_panel_add_label(panel, td->text_note);
	}

	if (sel->kind == BAR_TVREMOTE_KIND_CAST ||
	    sel->kind == BAR_TVREMOTE_KIND_APPLETV)
		return;
	gowl_bar_panel_add_section(panel, "Apps");
	if (td->apps != NULL && g_strcmp0(td->apps_device, sel->id) == 0) {
		for (i = 0; i < td->apps->len && i < TV_MAX_APP_ROWS; i++) {
			BarTvremoteApp *app = g_ptr_array_index(td->apps, i);
			g_autofree gchar *id = g_strdup_printf("app:%s", app->id);

			gowl_bar_panel_add_row(panel, id, NULL, app->name,
				g_strcmp0(app->name, app->id) != 0 ? app->id : NULL);
		}
		if (td->apps->len == 0)
			gowl_bar_panel_add_label(panel, "No launchable apps found.");
	}
	item = gowl_bar_panel_add_row(panel, "load-apps", NULL,
		(td->apps != NULL && g_strcmp0(td->apps_device, sel->id) == 0)
			? "Refresh apps" : "Show apps", NULL);
	gowl_bar_panel_item_set_busy(item, td->apps_loading);
	if (td->apps_error != NULL && g_strcmp0(td->apps_device, sel->id) == 0) {
		item = gowl_bar_panel_add_label(panel, td->apps_error);
		gowl_bar_panel_item_set_color(item, GOWL_BAR_COLOR_RED);
	}
	(void)plugin;
}

static void
tv_panel_add_section(GowlBarPlugin *plugin, TvData *td, GowlBarPanel *panel)
{
	GowlBarPanelItem *item;
	BarTvremoteKind kinds[BAR_TVREMOTE_N_KINDS];
	g_autofree gchar *rev = NULL;
	const gchar *missing;
	guint n, i;

	gowl_bar_panel_add_section(panel, "Add a TV");
	item = gowl_bar_panel_add_entry(panel, "add-host",
		"IP address of the TV, e.g. 192.168.1.20",
		td->add_host != NULL ? td->add_host : "");
	rev = g_strdup_printf("%u", td->add_rev);
	gowl_bar_panel_item_set_prop(item, GOWL_BAR_ENTRY_PROP_REVISION, rev);
	if (td->devices->len == 0)
		gowl_bar_panel_item_set_prop(item, GOWL_BAR_ENTRY_PROP_AUTOFOCUS,
		                             "true");

	n = tv_offered_kinds(td, kinds);
	item = gowl_bar_panel_add_buttons(panel, "add-kind");
	for (i = 0; i < n; i++)
		gowl_bar_panel_add_button(item, bar_tvremote_kind_title(kinds[i]),
		                          kinds[i] == td->add_kind);
	item = gowl_bar_panel_add_buttons(panel, "add");
	gowl_bar_panel_add_button(item, "Add and pair", FALSE);

	missing = tv_missing_tool_locked(td, td->add_kind);
	if (missing != NULL) {
		item = gowl_bar_panel_add_label(panel, missing);
		gowl_bar_panel_item_set_color(item, GOWL_BAR_COLOR_YELLOW);
	}
	if (td->add_kind == BAR_TVREMOTE_KIND_FIRETV)
		gowl_bar_panel_add_label(panel,
			"Fire TV: Settings > My Fire TV > About, select the device "
			"name seven times, then Developer options > ADB debugging "
			"ON. The IP is under About > Network.");
	if (td->add_error != NULL) {
		item = gowl_bar_panel_add_label(panel, td->add_error);
		gowl_bar_panel_item_set_color(item, GOWL_BAR_COLOR_RED);
	}

	item = gowl_bar_panel_add_row(panel, "scan", NULL,
		td->scanning ? "Scanning the network\xe2\x80\xa6" : "Scan network",
		"Find TVs on this network");
	gowl_bar_panel_item_set_busy(item, td->scanning);
	if (td->scan_results != NULL) {
		for (i = 0; i < td->scan_results->len; i++) {
			BarTvremoteFound *f = g_ptr_array_index(td->scan_results, i);
			g_autofree gchar *id = NULL;
			g_autofree gchar *sub = NULL;

			id = g_strdup_printf("found:%s:%s",
			                     bar_tvremote_kind_name(f->kind), f->host);
			sub = g_strdup_printf("%s \xc2\xb7 %s",
			                      bar_tvremote_kind_title(f->kind),
			                      f->how != NULL ? f->how : "");
			gowl_bar_panel_add_row(panel, id, TV_ICON, f->host, sub);
		}
	}
	if (td->scan_error != NULL)
		gowl_bar_panel_add_label(panel, td->scan_error);
	(void)plugin;
}

static GowlBarPanel *
tv_panel(GowlBarPlugin *plugin, gpointer data)
{
	TvData *td = data;
	GowlBarPanel *panel;
	GowlBarPanelItem *item;
	BarTvremoteDevice *sel;
	BarTvremoteStatus *st = NULL;
	const gchar *err;

	panel = gowl_bar_panel_new();
	gowl_bar_panel_set_width(panel, TV_PANEL_WIDTH);

	g_mutex_lock(&td->lock);
	sel = tv_selected_locked(td);
	if (sel != NULL)
		st = g_hash_table_lookup(td->statuses, sel->id);
	td->canvas_index = -1;

	/* The hero: what the TV is doing, or what is in the way. */
	if (sel == NULL) {
		item = gowl_bar_panel_add_hero(panel, TV_ICON, "TV remote",
			"No TVs yet: add one below");
		gowl_bar_panel_item_set_color(item,
			tv_role(plugin, "color-offline", GOWL_BAR_COLOR_SUBTEXT));
	} else {
		g_autofree gchar *sub = NULL;
		GowlBarColor role;

		if (st != NULL && st->link == BAR_TVREMOTE_LINK_ONLINE) {
			GString *s = g_string_new(NULL);

			g_string_append(s, st->app_name != NULL ? st->app_name
			                                        : "Online");
			if (st->title != NULL)
				g_string_append_printf(s, " \xc2\xb7 %s", st->title);
			if (st->muted)
				g_string_append(s, " \xc2\xb7 muted");
			else if (st->volume >= 0)
				g_string_append_printf(s, " \xc2\xb7 volume %d",
				                       st->volume);
			sub = g_string_free(s, FALSE);
			role = tv_role(plugin, "color-online", GOWL_BAR_COLOR_GREEN);
		} else if (st != NULL && st->link == BAR_TVREMOTE_LINK_PAIRING) {
			sub = g_strdup(st->detail);
			role = tv_role(plugin, "color-pairing", GOWL_BAR_COLOR_YELLOW);
		} else {
			sub = g_strdup(st != NULL && st->detail != NULL
			               ? st->detail : "Connecting\xe2\x80\xa6");
			role = tv_role(plugin, "color-offline",
			               GOWL_BAR_COLOR_SUBTEXT);
		}
		item = gowl_bar_panel_add_hero(panel, TV_ICON, sel->name, sub);
		gowl_bar_panel_item_set_color(item, role);
		gowl_bar_panel_item_set_busy(item, td->jobs > 0);
	}

	if (td->config_error != NULL) {
		item = gowl_bar_panel_add_label(panel, td->config_error);
		gowl_bar_panel_item_set_color(item, GOWL_BAR_COLOR_RED);
	}
	err = tv_recent_error_locked(td);
	if (err != NULL) {
		item = gowl_bar_panel_add_label(panel, err);
		gowl_bar_panel_item_set_color(item, GOWL_BAR_COLOR_RED);
	}

	tv_panel_device_section(plugin, td, panel, sel);
	if (sel != NULL) {
		const gchar *missing = tv_missing_tool_locked(td, sel->kind);

		if (missing != NULL) {
			item = gowl_bar_panel_add_label(panel, missing);
			gowl_bar_panel_item_set_color(item, GOWL_BAR_COLOR_YELLOW);
		}
		tv_panel_remote_section(plugin, td, panel, sel);
	}
	tv_panel_add_section(plugin, td, panel);
	g_mutex_unlock(&td->lock);
	return panel;
}

/* Fill the Add form from a scan result `found:KIND:HOST'. */
static void
tv_action_found_locked(TvData *td, const gchar *rest)
{
	const gchar *colon = strchr(rest, ':');
	g_autofree gchar *kind = NULL;

	if (colon == NULL)
		return;
	kind = g_strndup(rest, (gsize)(colon - rest));
	if (!bar_tvremote_valid_host(colon + 1))
		return;
	td->add_kind = bar_tvremote_kind_from_name(kind);
	if (td->add_kind == BAR_TVREMOTE_KIND_UNKNOWN)
		td->add_kind = BAR_TVREMOTE_KIND_FIRETV;
	g_free(td->add_host);
	td->add_host = g_strdup(colon + 1);
	td->add_rev++;
}

static void
tv_action(GowlBarPlugin *plugin, gpointer data, const gchar *item_id,
          gint index, gdouble value, guint button)
{
	TvData *td = data;
	TvJob *job = NULL;
	BarTvremoteDevice *sel;

	if (item_id == NULL)
		return;

	if (strcmp(item_id, "pad") == 0) {
		gint key;
		BarTvremoteKind kind;
		gint w, h;

		/* The wheel over the remote is its volume. */
		if (button == GOWL_BAR_BUTTON_SCROLL_UP) {
			tv_press(plugin, td, BAR_TVREMOTE_KEY_VOL_UP, FALSE, 1,
			         FALSE);
			return;
		}
		if (button == GOWL_BAR_BUTTON_SCROLL_DOWN) {
			tv_press(plugin, td, BAR_TVREMOTE_KEY_VOL_DOWN, FALSE, 1,
			         FALSE);
			return;
		}

		/* Return on the focused canvas arrives as (-1, -1): OK. */
		if (index < 0) {
			tv_press(plugin, td, BAR_TVREMOTE_KEY_SELECT, FALSE, 1, FALSE);
			return;
		}
		g_mutex_lock(&td->lock);
		sel = tv_selected_locked(td);
		kind = (sel != NULL) ? sel->kind : BAR_TVREMOTE_KIND_UNKNOWN;
		w = td->canvas_w;
		h = td->canvas_h;
		g_mutex_unlock(&td->lock);
		if (w <= 0 || h <= 0)
			return;
		key = bar_tvremote_hit(kind, w, h, (gdouble)index, value);
		if (key >= 0)
			tv_press(plugin, td, (BarTvremoteKey)key, FALSE, 1, FALSE);
		return;
	}

	g_mutex_lock(&td->lock);
	sel = tv_selected_locked(td);

	if (g_str_has_prefix(item_id, "dev:")) {
		g_free(td->selected_id);
		td->selected_id = g_strdup(item_id + 4);
		g_clear_pointer(&td->forget_armed, g_free);
		job = tv_job_new(JOB_REFRESH, item_id + 4);
	} else if (strcmp(item_id, "devops") == 0 && sel != NULL) {
		gboolean has_default_btn = (tv_default_locked(td) != sel);

		if (has_default_btn && index == 0) {
			job = tv_job_new(JOB_DEFAULT, sel->id);
		} else if (g_strcmp0(td->forget_armed, sel->id) == 0 &&
		           g_get_monotonic_time() - td->forget_armed_at <
		           TV_ARM_US) {
			/* Second click inside the window: really forget. */
			g_clear_pointer(&td->forget_armed, g_free);
			job = tv_job_new(JOB_FORGET, sel->id);
		} else {
			g_free(td->forget_armed);
			td->forget_armed = g_strdup(sel->id);
			td->forget_armed_at = g_get_monotonic_time();
		}
	} else if (strcmp(item_id, "load-apps") == 0 && sel != NULL) {
		if (!td->apps_loading) {
			td->apps_loading = TRUE;
			job = tv_job_new(JOB_APPS, sel->id);
		}
	} else if (g_str_has_prefix(item_id, "app:") && sel != NULL) {
		job = tv_job_new(JOB_LAUNCH, sel->id);
		job->arg = g_strdup(item_id + 4);
	} else if (strcmp(item_id, "add-kind") == 0) {
		BarTvremoteKind kinds[BAR_TVREMOTE_N_KINDS];
		guint n = tv_offered_kinds(td, kinds);

		if (index >= 0 && (guint)index < n)
			td->add_kind = kinds[index];
	} else if (strcmp(item_id, "add") == 0) {
		job = tv_job_new(JOB_ADD, NULL);
		job->arg = g_strdup(td->add_host);
		job->add_kind = td->add_kind;
	} else if (strcmp(item_id, "scan") == 0) {
		if (!td->scanning) {
			td->scanning = TRUE;
			g_clear_pointer(&td->scan_error, g_free);
			job = tv_job_new(JOB_SCAN, NULL);
		}
	} else if (g_str_has_prefix(item_id, "found:")) {
		tv_action_found_locked(td, item_id + 6);
	}
	g_mutex_unlock(&td->lock);

	if (job != NULL)
		tv_queue(plugin, td, job);
}

static void
tv_panel_text(GowlBarPlugin *plugin, gpointer data, const gchar *item_id,
              const gchar *text, gboolean submitted)
{
	TvData *td = data;
	TvJob *job = NULL;
	BarTvremoteDevice *sel;

	g_mutex_lock(&td->lock);
	if (g_strcmp0(item_id, "add-host") == 0) {
		g_free(td->add_host);
		td->add_host = bar_tvremote_clean_text(text, 253);
		if (submitted) {
			job = tv_job_new(JOB_ADD, NULL);
			job->arg = g_strdup(td->add_host);
			job->add_kind = td->add_kind;
		}
	} else if (g_strcmp0(item_id, "text") == 0 && submitted) {
		sel = tv_selected_locked(td);
		if (sel != NULL && text != NULL && text[0] != '\0') {
			job = tv_job_new(JOB_TEXT, sel->id);
			job->arg = g_strdup(text);
			/* Clear the field: the host takes the plugin's
			   (empty) text again when the revision moves. */
			td->text_rev++;
		}
	}
	g_mutex_unlock(&td->lock);
	if (job != NULL)
		tv_queue(plugin, td, job);
}

static gboolean
tv_panel_key(GowlBarPlugin *plugin, gpointer data, guint keysym,
             guint modifiers, gint focused_item)
{
	TvData *td = data;
	gboolean on_pad;
	gint key = -1;
	gboolean hold = FALSE;

	g_mutex_lock(&td->lock);
	on_pad = (td->canvas_index >= 0 && focused_item == td->canvas_index);
	g_mutex_unlock(&td->lock);
	/* Only while the remote itself has the keyboard; and never a chord
	   the window manager may want. */
	if (!on_pad || (modifiers & ~(guint)GOWL_KEY_MOD_SHIFT) != 0)
		return FALSE;

	switch (keysym) {
	case XKB_KEY_Left:  case XKB_KEY_a: key = BAR_TVREMOTE_KEY_LEFT; break;
	case XKB_KEY_Right: case XKB_KEY_d: key = BAR_TVREMOTE_KEY_RIGHT; break;
	/* A focused canvas hears Up, Down, j, k, Return and Space before
	   the panel's navigation does, so the remote can have real arrows;
	   w/a/s/d stay for the left hand. */
	case XKB_KEY_Up:    case XKB_KEY_w: case XKB_KEY_k:
		key = BAR_TVREMOTE_KEY_UP;
		break;
	case XKB_KEY_Down:  case XKB_KEY_s: case XKB_KEY_j:
		key = BAR_TVREMOTE_KEY_DOWN;
		break;
	case XKB_KEY_Return: case XKB_KEY_KP_Enter: case XKB_KEY_space:
		key = BAR_TVREMOTE_KEY_SELECT;
		break;
	case XKB_KEY_BackSpace:             key = BAR_TVREMOTE_KEY_BACK; break;
	case XKB_KEY_h:                     key = BAR_TVREMOTE_KEY_HOME; break;
	case XKB_KEY_m:                     key = BAR_TVREMOTE_KEY_MENU; break;
	case XKB_KEY_p:                     key = BAR_TVREMOTE_KEY_PLAY_PAUSE; break;
	case XKB_KEY_r:                     key = BAR_TVREMOTE_KEY_REWIND; break;
	case XKB_KEY_f:                     key = BAR_TVREMOTE_KEY_FAST_FORWARD; break;
	case XKB_KEY_plus: case XKB_KEY_equal: case XKB_KEY_KP_Add:
		key = BAR_TVREMOTE_KEY_VOL_UP;
		break;
	case XKB_KEY_minus: case XKB_KEY_KP_Subtract:
		key = BAR_TVREMOTE_KEY_VOL_DOWN;
		break;
	case XKB_KEY_0: case XKB_KEY_KP_0:
		key = BAR_TVREMOTE_KEY_MUTE;
		break;
	case XKB_KEY_l: case XKB_KEY_L:
		/* No key-repeat reaches a panel, so "hold" is its own key. */
		key = BAR_TVREMOTE_KEY_SELECT;
		hold = TRUE;
		break;
	default:
		return FALSE;
	}
	return tv_press(plugin, td, (BarTvremoteKey)key, hold, 1, FALSE);
}

static void
tv_panel_opened(GowlBarPlugin *plugin, gpointer data)
{
	TvData *td = data;
	BarTvremoteDevice *sel;
	TvJob *job = NULL;

	g_mutex_lock(&td->lock);
	td->panel_open = TRUE;
	sel = tv_selected_locked(td);
	if (sel != NULL)
		job = tv_job_new(JOB_REFRESH, sel->id);
	g_mutex_unlock(&td->lock);
	/* Fresh status the moment it opens, not at the next tick. */
	if (job != NULL)
		tv_queue(plugin, td, job);
}

static void
tv_panel_closed(GowlBarPlugin *plugin, gpointer data)
{
	TvData *td = data;

	(void)plugin;
	g_mutex_lock(&td->lock);
	td->panel_open = FALSE;
	td->canvas_index = -1;
	g_clear_pointer(&td->forget_armed, g_free);
	g_mutex_unlock(&td->lock);
}

/* ----------------------------------------------------------------
 * The drawn remote
 * ---------------------------------------------------------------- */

static void
tv_rounded_rect(cairo_t *cr, gdouble x, gdouble y, gdouble w, gdouble h,
                gdouble r)
{
	r = MIN(r, MIN(w, h) / 2.0);
	cairo_new_sub_path(cr);
	cairo_arc(cr, x + w - r, y + r, r, -G_PI / 2.0, 0.0);
	cairo_arc(cr, x + w - r, y + h - r, r, 0.0, G_PI / 2.0);
	cairo_arc(cr, x + r, y + h - r, r, G_PI / 2.0, G_PI);
	cairo_arc(cr, x + r, y + r, r, G_PI, 3.0 * G_PI / 2.0);
	cairo_close_path(cr);
}

static void
tv_canvas_text(cairo_t *cr, PangoLayout *layout, const gchar *text,
               gdouble cx, gdouble cy)
{
	gint tw, th;

	pango_layout_set_text(layout, text, -1);
	pango_layout_get_pixel_size(layout, &tw, &th);
	cairo_move_to(cr, cx - (gdouble)tw / 2.0, cy - (gdouble)th / 2.0);
	pango_cairo_show_layout(cr, layout);
}

/*
 * Paint the remote.  Compositor thread, inside a render: only cached
 * state, no I/O.  The rectangles come from bar_tvremote_layout(), the
 * same function the click hit-test uses, and the size it was painted
 * at is recorded for that hit-test.
 */
static void
tv_panel_canvas(GowlBarPlugin *plugin, gpointer data, const gchar *item_id,
                cairo_t *cr, const GowlBarTheme *theme, gint width,
                gint height)
{
	TvData *td = data;
	BarTvremoteButton buttons[BAR_TVREMOTE_MAX_BUTTONS];
	BarTvremoteDevice *sel;
	BarTvremoteKind kind;
	PangoLayout *layout;
	PangoFontDescription *font;
	GowlBarColor accent_role;
	gdouble accent_rgb[3];
	gboolean accent_hex;
	gdouble cx, cy, r;
	gint flash_key = -1;
	guint n, i;

	if (g_strcmp0(item_id, "pad") != 0)
		return;

	g_mutex_lock(&td->lock);
	sel = tv_selected_locked(td);
	kind = (sel != NULL) ? sel->kind : BAR_TVREMOTE_KIND_FIRETV;
	td->canvas_w = width;
	td->canvas_h = height;
	if (td->last_key >= 0 &&
	    g_get_monotonic_time() - td->last_key_at < TV_PRESS_FLASH_US)
		flash_key = td->last_key;
	g_mutex_unlock(&td->lock);

	accent_hex = tv_color(gowl_bar_plugin_get_setting(plugin,
	                                                  "color-accent"),
	                      GOWL_BAR_COLOR_ACCENT, &accent_role, accent_rgb);

	n = bar_tvremote_layout(kind, width, height, buttons, &cx, &cy, &r);

	layout = pango_cairo_create_layout(cr);
	font = pango_font_description_from_string(
		gowl_bar_theme_get_font(theme) != NULL
			? gowl_bar_theme_get_font(theme) : "Sans 10");
	pango_layout_set_font_description(layout, font);

	/* The D-pad ring behind its four arrows. */
	if (r > 0.0) {
		gowl_bar_theme_cairo_set(theme, cr, GOWL_BAR_COLOR_SURFACE);
		cairo_arc(cr, cx, cy, r, 0.0, 2.0 * G_PI);
		cairo_fill(cr);
	}

	for (i = 0; i < n; i++) {
		BarTvremoteButton *b = &buttons[i];
		gboolean pressed = ((gint)b->key == flash_key);
		gboolean on_accent = b->round || pressed;
		gboolean arrow = (b->key <= BAR_TVREMOTE_KEY_RIGHT);

		if (b->round) {
			cairo_arc(cr, b->x + b->w / 2.0, b->y + b->h / 2.0,
			          MIN(b->w, b->h) / 2.0, 0.0, 2.0 * G_PI);
		} else if (!arrow) {
			tv_rounded_rect(cr, b->x, b->y, b->w, b->h, 8.0);
		} else if (pressed) {
			tv_rounded_rect(cr, b->x, b->y, b->w, b->h, 8.0);
		} else {
			cairo_new_path(cr);
		}
		if (on_accent) {
			if (accent_hex)
				cairo_set_source_rgb(cr, accent_rgb[0], accent_rgb[1],
				                     accent_rgb[2]);
			else
				gowl_bar_theme_cairo_set(theme, cr, accent_role);
		} else {
			gowl_bar_theme_cairo_set(theme, cr,
			                         arrow ? GOWL_BAR_COLOR_SURFACE
			                               : GOWL_BAR_COLOR_SURFACE_ALT);
		}
		cairo_fill(cr);

		gowl_bar_theme_cairo_set(theme, cr,
		                         on_accent ? GOWL_BAR_COLOR_BASE
		                                   : GOWL_BAR_COLOR_TEXT);
		tv_canvas_text(cr, layout, bar_tvremote_key_label(b->key),
		               b->x + b->w / 2.0, b->y + b->h / 2.0);
	}

	pango_font_description_free(font);
	g_object_unref(layout);
}

static const GowlBarPluginVTable tvremote_vtable = {
	sizeof(GowlBarPluginVTable),
	tv_create, tv_destroy,
	NULL, tv_deactivate, tv_configure,
	tv_interval, NULL, tv_poll_async,
	NULL, NULL,
	tv_click, tv_scroll,
	tv_panel, tv_action,
	tv_panel_opened, tv_panel_closed,
	tv_panel_key,
	NULL,
	tv_panel_text, tv_panel_canvas
};

/**
 * bar_register_tvremote_plugins:
 * @registry: the bar's plugin registry
 *
 * Registers `tvremote' (alias `tv').
 */
void
bar_register_tvremote_plugins(GowlBarRegistry *registry)
{
	gowl_bar_registry_register_vtable(registry, "tvremote", "TV remote",
		"Remote for Fire TV, Android TV, Roku, Samsung and LG",
		&tvremote_vtable);
	gowl_bar_registry_register_alias(registry, "tv", "tvremote");
}
