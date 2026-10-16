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
 * Roku, over ECP: plain HTTP on port 8060, no pairing.
 *
 * Through `curl' rather than a linked HTTP client: bar.so links none
 * (libsoup is only in the MCP module), ECP is a handful of bodiless
 * POSTs and three small GETs, and curl is on every Fedora install.
 *
 * Roku OS 11 added "Control by mobile apps"; when it is set to Limited,
 * every keypress answers 403.  That is reported as such, with where the
 * setting lives, because otherwise it looks exactly like a broken
 * widget.
 */

#include <string.h>
#include <gio/gio.h>

#include "bar-tvremote-backend.h"

#define ROKU_TIMEOUT_MS (6000)
#define ROKU_LONGPRESS_US (800 * 1000)

/*
 * One request.  The URL is built from a validated host and an encoded
 * path, and it is a single argv element, so nothing in it reaches a
 * shell.  `-w' appends the status code on its own line after the body.
 */
static gboolean
roku_http(const BarTvremoteDevice *dev, const BarTvremoteTools *t,
          gboolean post, const gchar *path, gchar **body, GError **error)
{
	g_autofree gchar *url = NULL;
	g_autofree gchar *out = NULL;
	g_autofree gchar *err = NULL;
	const gchar *argv[16];
	gchar *nl;
	gint n = 0, exit_code = -1;
	gint64 code;

	if (body != NULL)
		*body = NULL;
	if (!bar_tvremote_have(t->curl)) {
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
		            "curl is not installed: install %s",
		            bar_tvremote_tool_package(t->curl));
		return FALSE;
	}
	if (!bar_tvremote_valid_host(dev->host)) {
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
		            "not a usable host name");
		return FALSE;
	}
	url = bar_tvremote_http_url(dev->host, bar_tvremote_device_port(dev),
	                            path);
	argv[n++] = t->curl;
	argv[n++] = "-sS";
	argv[n++] = "-m";
	argv[n++] = "4";
	argv[n++] = "--connect-timeout";
	argv[n++] = "2";
	argv[n++] = "-o";
	argv[n++] = "-";
	argv[n++] = "-w";
	argv[n++] = "\n%{http_code}";
	if (post) {
		argv[n++] = "-X";
		argv[n++] = "POST";
		argv[n++] = "-d";
		argv[n++] = "";
	}
	argv[n++] = url;
	argv[n] = NULL;

	if (!bar_tvremote_run(argv, ROKU_TIMEOUT_MS, &out, &err, &exit_code,
	                      error))
		return FALSE;
	if (exit_code != 0) {
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_HOST_UNREACHABLE,
		            "no answer from the Roku at %s", dev->host);
		return FALSE;
	}
	nl = strrchr(out, '\n');
	code = g_ascii_strtoll(nl != NULL ? nl + 1 : out, NULL, 10);
	if (nl != NULL)
		*nl = '\0';
	if (code == 403) {
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
		            "the Roku refuses remote control: set Settings > "
		            "System > Advanced system settings > Control by "
		            "mobile apps > Network access to Default or "
		            "Permissive");
		return FALSE;
	}
	if (code < 200 || code >= 300) {
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
		            "the Roku answered HTTP %" G_GINT64_FORMAT, code);
		return FALSE;
	}
	if (body != NULL)
		*body = g_steal_pointer(&out);
	return TRUE;
}

static gboolean
roku_key(const BarTvremoteDevice *dev, BarTvremoteSession *s,
         const BarTvremoteTools *t, BarTvremoteKey key, gboolean longpress,
         GError **error)
{
	const gchar *name = bar_tvremote_roku_key(key);
	g_autofree gchar *path = NULL;

	(void)s;
	if (name == NULL) {
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
		            "Roku has no %s key", bar_tvremote_key_name(key));
		return FALSE;
	}
	if (!longpress) {
		path = g_strdup_printf("/keypress/%s", name);
		return roku_http(dev, t, TRUE, path, NULL, error);
	}

	/* A hold is a keydown, a pause and a keyup.  The pause is on a
	   worker, holding only this device's I/O lock. */
	path = g_strdup_printf("/keydown/%s", name);
	if (!roku_http(dev, t, TRUE, path, NULL, error))
		return FALSE;
	g_usleep(ROKU_LONGPRESS_US);
	g_free(path);
	path = g_strdup_printf("/keyup/%s", name);
	return roku_http(dev, t, TRUE, path, NULL, error);
}

static gboolean
roku_text(const BarTvremoteDevice *dev, BarTvremoteSession *s,
          const BarTvremoteTools *t, const gchar *text, guint *dropped,
          GError **error)
{
	g_auto(GStrv) paths = NULL;
	guint i;

	(void)s;
	if (dropped != NULL)
		*dropped = 0;
	paths = bar_tvremote_roku_text_paths(text);
	for (i = 0; paths[i] != NULL; i++)
		if (!roku_http(dev, t, TRUE, paths[i], NULL, error))
			return FALSE;
	return TRUE;
}

static BarTvremoteStatus *
roku_status(const BarTvremoteDevice *dev, BarTvremoteSession *s,
            const BarTvremoteTools *t)
{
	BarTvremoteStatus *st;
	g_autofree gchar *info = NULL;
	g_autofree gchar *active = NULL;
	g_autofree gchar *power = NULL;
	g_autoptr(GError) error = NULL;

	(void)s;
	if (!bar_tvremote_have(t->curl)) {
		g_autofree gchar *msg = NULL;

		msg = g_strdup_printf("curl is not installed: install %s",
		                      bar_tvremote_tool_package(t->curl));
		return bar_tvremote_status_new(BAR_TVREMOTE_LINK_NO_TOOL, msg);
	}
	if (!roku_http(dev, t, FALSE, "/query/device-info", &info, &error))
		return bar_tvremote_status_new(BAR_TVREMOTE_LINK_OFFLINE,
		                               error->message);

	st = bar_tvremote_status_new(BAR_TVREMOTE_LINK_ONLINE, NULL);
	power = bar_tvremote_roku_parse_field(info, "power-mode");
	if (power != NULL && strcmp(power, "PowerOn") != 0) {
		/* Standby ("Ready", "DisplayOff"): reachable, nothing on. */
		st->app_name = g_strdup("Standby");
		return st;
	}
	if (roku_http(dev, t, FALSE, "/query/active-app", &active, NULL)) {
		st->app_name = bar_tvremote_roku_parse_active(active);
	}
	/* ECP has no way to READ the volume, only to change it. */
	return st;
}

static GPtrArray *
roku_apps(const BarTvremoteDevice *dev, BarTvremoteSession *s,
          const BarTvremoteTools *t, GError **error)
{
	g_autofree gchar *xml = NULL;

	(void)s;
	if (!roku_http(dev, t, FALSE, "/query/apps", &xml, error))
		return NULL;
	return bar_tvremote_roku_parse_apps(xml);
}

static gboolean
roku_launch(const BarTvremoteDevice *dev, BarTvremoteSession *s,
            const BarTvremoteTools *t, const gchar *app_id, GError **error)
{
	g_autofree gchar *enc = NULL;
	g_autofree gchar *path = NULL;

	(void)s;
	if (!bar_tvremote_valid_app_id(app_id)) {
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
		            "not a Roku channel id");
		return FALSE;
	}
	enc = bar_tvremote_urlencode(app_id);
	path = g_strdup_printf("/launch/%s", enc);
	return roku_http(dev, t, TRUE, path, NULL, error);
}

const BarTvremoteBackend bar_tvremote_roku_backend = {
	roku_key,
	roku_text,
	roku_status,
	roku_apps,
	roku_launch
};
