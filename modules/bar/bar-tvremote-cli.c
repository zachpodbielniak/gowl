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
 * Chromecast and Apple TV, through optional command-line tools.
 *
 * Both protocols (Cast's protobuf-over-TLS, Apple's Companion and MRP)
 * are large, versioned and authenticated in ways not worth re-doing in
 * C for a bar widget.  `catt' and pyatv's `atvremote' already do it
 * well, and are one `pip install' away.  So these two kinds exist only
 * when the tool does: the panel does not offer them otherwise.
 *
 * Apple TV pairing is interactive (a PIN typed back into the CLI), so
 * it is done once in a terminal -- `atvremote -s HOST --protocol
 * companion pair' -- and pyatv keeps the credentials itself.
 */

#include <string.h>
#include <gio/gio.h>

#include "bar-tvremote-backend.h"

#define CLI_TIMEOUT_MS (10000)

/* Run TOOL with argv, return stdout, error with stderr's first line. */
static gchar *
cli_run(const gchar *tool, const gchar * const *args, GError **error)
{
	GPtrArray *argv;
	g_autofree gchar *out = NULL;
	g_autofree gchar *err = NULL;
	gint code = -1;
	guint i;
	gboolean ok;

	if (!bar_tvremote_have(tool)) {
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
		            "%s is not installed: install %s", tool,
		            bar_tvremote_tool_package(tool));
		return NULL;
	}
	argv = g_ptr_array_new();
	g_ptr_array_add(argv, (gpointer)tool);
	for (i = 0; args[i] != NULL; i++)
		g_ptr_array_add(argv, (gpointer)args[i]);
	g_ptr_array_add(argv, NULL);
	ok = bar_tvremote_run((const gchar * const *)argv->pdata,
	                      CLI_TIMEOUT_MS, &out, &err, &code, error);
	g_ptr_array_free(argv, TRUE);
	if (!ok)
		return NULL;
	if (code != 0) {
		gchar *nl = strchr(err, '\n');
		g_autofree gchar *why = NULL;

		if (nl != NULL)
			*nl = '\0';
		why = bar_tvremote_clean_text(err[0] != '\0' ? err : out, 200);
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "%s: %s", tool,
		            why[0] != '\0' ? why : "failed");
		return NULL;
	}
	return g_steal_pointer(&out);
}

/* The value after `Label:' on the first line that has it. */
static gchar *
field_after(const gchar *text, const gchar *label)
{
	g_auto(GStrv) lines = NULL;
	guint i;

	if (text == NULL)
		return NULL;
	lines = g_strsplit(text, "\n", -1);
	for (i = 0; lines[i] != NULL; i++) {
		gchar *l = g_strstrip(lines[i]);

		if (g_ascii_strncasecmp(l, label, strlen(label)) == 0 &&
		    l[strlen(label)] == ':') {
			g_autofree gchar *v = NULL;

			v = g_strstrip(g_strdup(l + strlen(label) + 1));
			if (v[0] == '\0')
				return NULL;
			return bar_tvremote_clean_text(v, 120);
		}
	}
	return NULL;
}

/* ---------------------------------------------------------------- *
 * Chromecast (catt)
 * ---------------------------------------------------------------- */

static gboolean
cast_key(const BarTvremoteDevice *dev, BarTvremoteSession *s,
         const BarTvremoteTools *t, BarTvremoteKey key, gboolean longpress,
         GError **error)
{
	const gchar *cmd = bar_tvremote_catt_command(key);
	const gchar *args[6];
	g_autofree gchar *out = NULL;
	guint n = 0;

	(void)s;
	(void)longpress;
	if (cmd == NULL) {
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
		            "Chromecast has no %s key", bar_tvremote_key_name(key));
		return FALSE;
	}
	args[n++] = "-d";
	args[n++] = dev->host;
	args[n++] = cmd;
	if (key == BAR_TVREMOTE_KEY_VOL_UP || key == BAR_TVREMOTE_KEY_VOL_DOWN)
		args[n++] = "10";
	else if (key == BAR_TVREMOTE_KEY_REWIND ||
	         key == BAR_TVREMOTE_KEY_FAST_FORWARD)
		args[n++] = "30";
	args[n] = NULL;
	out = cli_run(t->catt, args, error);
	return out != NULL;
}

static gboolean
cast_text(const BarTvremoteDevice *dev, BarTvremoteSession *s,
          const BarTvremoteTools *t, const gchar *text, guint *dropped,
          GError **error)
{
	(void)dev;
	(void)s;
	(void)t;
	(void)text;
	if (dropped != NULL)
		*dropped = 0;
	g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
	            "a Chromecast has no text input");
	return FALSE;
}

static BarTvremoteStatus *
cast_status(const BarTvremoteDevice *dev, BarTvremoteSession *s,
            const BarTvremoteTools *t)
{
	const gchar *args[] = { "-d", dev->host, "status", NULL };
	g_autoptr(GError) error = NULL;
	g_autofree gchar *out = NULL;
	g_autofree gchar *vol = NULL;
	g_autofree gchar *mute = NULL;
	BarTvremoteStatus *st;

	(void)s;
	if (!bar_tvremote_have(t->catt)) {
		g_autofree gchar *msg = NULL;

		msg = g_strdup_printf("catt is not installed: install %s",
		                      bar_tvremote_tool_package(t->catt));
		return bar_tvremote_status_new(BAR_TVREMOTE_LINK_NO_TOOL, msg);
	}
	out = cli_run(t->catt, args, &error);
	if (out == NULL)
		return bar_tvremote_status_new(BAR_TVREMOTE_LINK_OFFLINE,
		                               error->message);
	st = bar_tvremote_status_new(BAR_TVREMOTE_LINK_ONLINE, NULL);
	st->title = field_after(out, "Title");
	st->app_name = field_after(out, "App");
	vol = field_after(out, "Volume");
	if (vol != NULL) {
		gint64 v = g_ascii_strtoll(vol, NULL, 10);

		st->volume = (v >= 0 && v <= 100) ? (gint)v : -1;
	}
	mute = field_after(out, "Volume muted");
	st->muted = (mute != NULL && g_ascii_strcasecmp(mute, "true") == 0);
	return st;
}

static GPtrArray *
no_apps(const BarTvremoteDevice *dev, BarTvremoteSession *s,
        const BarTvremoteTools *t, GError **error)
{
	(void)dev;
	(void)s;
	(void)t;
	g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
	            "this device has no app list");
	return NULL;
}

static gboolean
no_launch(const BarTvremoteDevice *dev, BarTvremoteSession *s,
          const BarTvremoteTools *t, const gchar *app_id, GError **error)
{
	(void)dev;
	(void)s;
	(void)t;
	(void)app_id;
	g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
	            "this device cannot launch apps");
	return FALSE;
}

const BarTvremoteBackend bar_tvremote_cast_backend = {
	cast_key,
	cast_text,
	cast_status,
	no_apps,
	no_launch
};

/* ---------------------------------------------------------------- *
 * Apple TV (atvremote)
 * ---------------------------------------------------------------- */

static gboolean
atv_key(const BarTvremoteDevice *dev, BarTvremoteSession *s,
        const BarTvremoteTools *t, BarTvremoteKey key, gboolean longpress,
        GError **error)
{
	const gchar *cmd = bar_tvremote_atv_command(key);
	g_autofree gchar *held = NULL;
	g_autofree gchar *out = NULL;
	const gchar *args[4];

	(void)s;
	if (cmd == NULL) {
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
		            "Apple TV has no %s key", bar_tvremote_key_name(key));
		return FALSE;
	}
	/* pyatv's `=2' is "hold" for the navigation buttons. */
	if (longpress && key <= BAR_TVREMOTE_KEY_MENU)
		held = g_strdup_printf("%s=2", cmd);
	args[0] = "-s";
	args[1] = dev->host;
	args[2] = held != NULL ? held : cmd;
	args[3] = NULL;
	out = cli_run(t->atvremote, args, error);
	return out != NULL;
}

static gboolean
atv_text(const BarTvremoteDevice *dev, BarTvremoteSession *s,
         const BarTvremoteTools *t, const gchar *text, guint *dropped,
         GError **error)
{
	g_autofree gchar *clean = NULL;
	g_autofree gchar *arg = NULL;
	g_autofree gchar *out = NULL;
	const gchar *args[4];

	(void)s;
	if (dropped != NULL)
		*dropped = 0;
	clean = bar_tvremote_clean_text(text, 500);
	if (clean[0] == '\0') {
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
		            "nothing to type");
		return FALSE;
	}
	/* One argv element: no shell sees it. */
	arg = g_strdup_printf("text_set=%s", clean);
	args[0] = "-s";
	args[1] = dev->host;
	args[2] = arg;
	args[3] = NULL;
	out = cli_run(t->atvremote, args, error);
	return out != NULL;
}

static BarTvremoteStatus *
atv_status(const BarTvremoteDevice *dev, BarTvremoteSession *s,
           const BarTvremoteTools *t)
{
	const gchar *args[] = { "-s", dev->host, "playing", "app", "volume",
	                        NULL };
	g_autoptr(GError) error = NULL;
	g_autofree gchar *out = NULL;
	g_auto(GStrv) lines = NULL;
	BarTvremoteStatus *st;
	guint i;

	(void)s;
	if (!bar_tvremote_have(t->atvremote)) {
		g_autofree gchar *msg = NULL;

		msg = g_strdup_printf("atvremote is not installed: install %s",
		                      bar_tvremote_tool_package(t->atvremote));
		return bar_tvremote_status_new(BAR_TVREMOTE_LINK_NO_TOOL, msg);
	}
	out = cli_run(t->atvremote, args, &error);
	if (out == NULL)
		return bar_tvremote_status_new(BAR_TVREMOTE_LINK_OFFLINE,
		                               error->message);
	st = bar_tvremote_status_new(BAR_TVREMOTE_LINK_ONLINE, NULL);
	st->title = field_after(out, "Title");
	st->app_name = field_after(out, "App");
	if (st->app_name != NULL) {
		/* "Netflix (com.netflix.Netflix)" -> "Netflix" */
		gchar *paren = strstr(st->app_name, " (");

		if (paren != NULL)
			*paren = '\0';
	}
	/* `volume' prints a bare number such as 35.0 on its own line. */
	lines = g_strsplit(out, "\n", -1);
	for (i = 0; lines[i] != NULL; i++) {
		gchar *end = NULL;
		gdouble v;

		g_strstrip(lines[i]);
		if (lines[i][0] == '\0')
			continue;
		v = g_ascii_strtod(lines[i], &end);
		if (end != NULL && *end == '\0' && v >= 0.0 && v <= 100.0)
			st->volume = (gint)(v + 0.5);
	}
	return st;
}

const BarTvremoteBackend bar_tvremote_appletv_backend = {
	atv_key,
	atv_text,
	atv_status,
	no_apps,
	no_launch
};
