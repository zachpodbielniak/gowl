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
 * Samsung Tizen (2016 and later), over the TV's websocket remote API:
 *
 *   wss://HOST:8002/api/v2/channels/samsung.remote.control
 *       ?name=<base64 app name>&token=<token>
 *
 * The first connection puts an "Allow" prompt on the TV.  Nothing comes
 * back until it is accepted; then the TV sends `ms.channel.connect'
 * with a TOKEN in its data, and a connection that presents that token
 * later is let straight in.  The token is written to the device list
 * (mode 0600) through the plugin's save_secret hook.  A refusal arrives
 * as `ms.channel.unauthorized'.
 *
 * The connection is kept open between calls -- a TLS handshake per
 * keypress would be a visible lag -- and anything the TV pushes in the
 * meantime is drained before the next write, so an unread backlog can
 * never fill the socket.
 *
 * Port 8001 (plain ws) still works on older sets and is used when the
 * device's port is set to it.  Pre-2016 sets (the encrypted PIN
 * protocol on 8000) are not supported.
 */

#include <string.h>
#include <gio/gio.h>

#include "bar-tvremote-backend.h"

#define SAMSUNG_APP_NAME   "gowl remote"
#define SAMSUNG_TIMEOUT_S  (4)
#define SAMSUNG_LONGPRESS_US (800 * 1000)

/* A token only ever goes into the URL when it is all digits and
   letters, so a hand-edited config cannot inject a query parameter. */
static gboolean
token_ok(const gchar *token)
{
	const gchar *p;

	if (token == NULL || token[0] == '\0' || strlen(token) > 64)
		return FALSE;
	for (p = token; *p != '\0'; p++)
		if (!g_ascii_isalnum(*p))
			return FALSE;
	return TRUE;
}

static gchar *
samsung_path(const BarTvremoteDevice *dev)
{
	g_autofree gchar *b64 = NULL;
	g_autofree gchar *name = NULL;

	b64 = g_base64_encode((const guchar *)SAMSUNG_APP_NAME,
	                      strlen(SAMSUNG_APP_NAME));
	name = bar_tvremote_urlencode(b64);
	if (token_ok(dev->token))
		return g_strdup_printf("/api/v2/channels/samsung.remote.control"
		                       "?name=%s&token=%s", name, dev->token);
	return g_strdup_printf("/api/v2/channels/samsung.remote.control"
	                       "?name=%s", name);
}

static void
samsung_drop(BarTvremoteSession *s)
{
	g_clear_pointer(&s->ws, bar_tvremote_ws_free);
	s->ready = FALSE;
}

/*
 * Act on one message from the TV.  Returns FALSE (with @error) when it
 * ends the session: a refusal, or the prompt timing out.
 */
static gboolean
samsung_absorb(const BarTvremoteDevice *dev, BarTvremoteSession *s,
               const BarTvremoteTools *t, const gchar *msg, GError **error)
{
	g_autofree gchar *event = NULL;

	event = bar_tvremote_json_string(msg, "event", NULL);
	if (g_strcmp0(event, "ms.channel.connect") == 0) {
		g_autofree gchar *token = NULL;

		s->ready = TRUE;
		s->ws_backoff_s = 0;
		token = bar_tvremote_json_string(msg, "data", "token", NULL);
		if (token_ok(token) && g_strcmp0(token, dev->token) != 0 &&
		    t->save_secret != NULL)
			t->save_secret(t->user_data, dev->id, "token", token);
		if (token != NULL)
			memset(token, 0, strlen(token));
		return TRUE;
	}
	if (g_strcmp0(event, "ms.channel.unauthorized") == 0) {
		samsung_drop(s);
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
		            "the TV refused the connection; allow it under "
		            "Settings > General > External Device Manager > "
		            "Device Connection Manager");
		return FALSE;
	}
	if (g_strcmp0(event, "ms.channel.timeOut") == 0) {
		samsung_drop(s);
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT,
		            "the TV's prompt timed out; try again");
		return FALSE;
	}
	return TRUE;
}

/* Read and act on everything already waiting, without blocking. */
static gboolean
samsung_drain(const BarTvremoteDevice *dev, BarTvremoteSession *s,
              const BarTvremoteTools *t, GError **error)
{
	guint i;

	for (i = 0; i < 64 && bar_tvremote_ws_is_open(s->ws); i++) {
		g_autoptr(GError) local = NULL;
		g_autofree gchar *msg = NULL;

		msg = bar_tvremote_ws_recv_text(s->ws, 0, &local);
		if (msg == NULL) {
			if (g_error_matches(local, G_IO_ERROR, G_IO_ERROR_TIMED_OUT))
				return TRUE;
			samsung_drop(s);
			g_propagate_error(error, g_steal_pointer(&local));
			return FALSE;
		}
		if (!samsung_absorb(dev, s, t, msg, error))
			return FALSE;
	}
	return TRUE;
}

/*
 * Get to a paired connection, waiting up to @wait_ms for the TV to say
 * so.  FALSE with %G_IO_ERROR_PENDING while the prompt is still up --
 * the connection is KEPT so the next poll picks up where this one left
 * off rather than putting up a second prompt.
 */
static gboolean
samsung_connect(const BarTvremoteDevice *dev, BarTvremoteSession *s,
                const BarTvremoteTools *t, gint wait_ms, GError **error)
{
	gint64 deadline;
	gint port;

	if (bar_tvremote_ws_is_open(s->ws) && s->ready)
		return samsung_drain(dev, s, t, error);

	if (!bar_tvremote_ws_is_open(s->ws)) {
		g_autofree gchar *path = NULL;
		GError *local = NULL;

		samsung_drop(s);
		if (g_get_monotonic_time() < s->ws_next_try) {
			g_set_error(error, G_IO_ERROR, G_IO_ERROR_HOST_UNREACHABLE,
			            "cannot reach the TV at %s; retrying",
			            dev->host);
			return FALSE;
		}
		port = bar_tvremote_device_port(dev);
		path = samsung_path(dev);
		s->ws = bar_tvremote_ws_connect(dev->host, port, path,
		                                port == 8002, SAMSUNG_TIMEOUT_S,
		                                &local);
		if (s->ws == NULL) {
			s->ws_backoff_s = (s->ws_backoff_s == 0)
				? 2 : MIN(s->ws_backoff_s * 2, 60);
			s->ws_next_try = g_get_monotonic_time() +
			                 (gint64)s->ws_backoff_s * G_USEC_PER_SEC;
			g_set_error(error, G_IO_ERROR, G_IO_ERROR_HOST_UNREACHABLE,
			            "cannot reach the TV at %s: %s", dev->host,
			            local->message);
			g_error_free(local);
			return FALSE;
		}
	}

	deadline = g_get_monotonic_time() + (gint64)wait_ms * 1000;
	while (!s->ready) {
		g_autoptr(GError) local = NULL;
		g_autofree gchar *msg = NULL;
		gint64 left = (deadline - g_get_monotonic_time()) / 1000;

		msg = bar_tvremote_ws_recv_text(s->ws, (gint)MAX(left, 0), &local);
		if (msg == NULL) {
			if (g_error_matches(local, G_IO_ERROR,
			                    G_IO_ERROR_TIMED_OUT)) {
				g_set_error(error, G_IO_ERROR, G_IO_ERROR_PENDING,
				            "Accept the connection prompt on your TV");
				return FALSE;
			}
			samsung_drop(s);
			g_propagate_error(error, g_steal_pointer(&local));
			return FALSE;
		}
		if (!samsung_absorb(dev, s, t, msg, error))
			return FALSE;
	}
	return TRUE;
}

/* Send one message, reconnecting once if a paired connection broke. */
static gboolean
samsung_send(const BarTvremoteDevice *dev, BarTvremoteSession *s,
             const BarTvremoteTools *t, const gchar *msg, GError **error)
{
	guint attempt;

	for (attempt = 0; attempt < 2; attempt++) {
		g_autoptr(GError) local = NULL;

		if (!samsung_connect(dev, s, t, 1500, error))
			return FALSE;
		if (bar_tvremote_ws_send_text(s->ws, msg, &local))
			return TRUE;
		samsung_drop(s);
		if (attempt == 1) {
			g_propagate_error(error, g_steal_pointer(&local));
			return FALSE;
		}
	}
	return FALSE;
}

static gchar *
samsung_key_message(const gchar *cmd, const gchar *code)
{
	return g_strdup_printf("{\"method\":\"ms.remote.control\",\"params\":"
	                       "{\"Cmd\":\"%s\",\"DataOfCmd\":\"%s\","
	                       "\"Option\":\"false\","
	                       "\"TypeOfRemote\":\"SendRemoteKey\"}}",
	                       cmd, code);
}

static gboolean
samsung_key(const BarTvremoteDevice *dev, BarTvremoteSession *s,
            const BarTvremoteTools *t, BarTvremoteKey key, gboolean longpress,
            GError **error)
{
	const gchar *code = bar_tvremote_samsung_key(key);
	g_autofree gchar *msg = NULL;

	if (code == NULL) {
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
		            "Samsung has no %s key", bar_tvremote_key_name(key));
		return FALSE;
	}
	/* No toggle: alternate pause and play. */
	if (key == BAR_TVREMOTE_KEY_PLAY_PAUSE) {
		code = s->play_next ? "KEY_PLAY" : "KEY_PAUSE";
		s->play_next = !s->play_next;
	}
	if (!longpress) {
		msg = samsung_key_message("Click", code);
		return samsung_send(dev, s, t, msg, error);
	}
	msg = samsung_key_message("Press", code);
	if (!samsung_send(dev, s, t, msg, error))
		return FALSE;
	g_usleep(SAMSUNG_LONGPRESS_US);
	g_free(msg);
	msg = samsung_key_message("Release", code);
	return samsung_send(dev, s, t, msg, error);
}

static gboolean
samsung_text(const BarTvremoteDevice *dev, BarTvremoteSession *s,
             const BarTvremoteTools *t, const gchar *text, guint *dropped,
             GError **error)
{
	g_autofree gchar *clean = NULL;
	g_autofree gchar *b64 = NULL;
	g_autofree gchar *msg = NULL;

	if (dropped != NULL)
		*dropped = 0;
	/* Typed into the TV's on-screen keyboard as base64, so any UTF-8
	   survives; it only lands when a text field has focus. */
	clean = bar_tvremote_clean_text(text, 1000);
	if (clean[0] == '\0') {
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
		            "nothing to type");
		return FALSE;
	}
	b64 = g_base64_encode((const guchar *)clean, strlen(clean));
	msg = g_strdup_printf("{\"method\":\"ms.remote.control\",\"params\":"
	                      "{\"Cmd\":\"%s\",\"DataOfCmd\":\"base64\","
	                      "\"TypeOfRemote\":\"SendInputString\"}}", b64);
	return samsung_send(dev, s, t, msg, error);
}

static BarTvremoteStatus *
samsung_status(const BarTvremoteDevice *dev, BarTvremoteSession *s,
               const BarTvremoteTools *t)
{
	g_autoptr(GError) error = NULL;

	if (samsung_connect(dev, s, t, 1500, &error))
		/* Tizen reports neither the app nor the volume here. */
		return bar_tvremote_status_new(BAR_TVREMOTE_LINK_ONLINE, NULL);
	if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_PENDING))
		return bar_tvremote_status_new(BAR_TVREMOTE_LINK_PAIRING,
		                               error->message);
	return bar_tvremote_status_new(BAR_TVREMOTE_LINK_OFFLINE,
	                               error->message);
}

static GPtrArray *
samsung_apps(const BarTvremoteDevice *dev, BarTvremoteSession *s,
             const BarTvremoteTools *t, GError **error)
{
	static const gchar request[] =
		"{\"method\":\"ms.channel.emit\",\"params\":"
		"{\"event\":\"ed.installedApp.get\",\"to\":\"host\"}}";
	gint64 deadline;

	if (!samsung_send(dev, s, t, request, error))
		return NULL;
	deadline = g_get_monotonic_time() + 4 * G_USEC_PER_SEC;
	for (;;) {
		g_autofree gchar *msg = NULL;
		g_autofree gchar *event = NULL;
		gint64 left = (deadline - g_get_monotonic_time()) / 1000;

		msg = bar_tvremote_ws_recv_text(s->ws, (gint)MAX(left, 0), error);
		if (msg == NULL)
			return NULL;
		if (!samsung_absorb(dev, s, t, msg, error))
			return NULL;
		event = bar_tvremote_json_string(msg, "event", NULL);
		if (g_strcmp0(event, "ed.installedApp.get") == 0)
			return bar_tvremote_json_apps(msg, "appId", "name",
			                              "data", "data", NULL);
	}
}

static gboolean
samsung_launch(const BarTvremoteDevice *dev, BarTvremoteSession *s,
               const BarTvremoteTools *t, const gchar *app_id,
               GError **error)
{
	g_autofree gchar *msg = NULL;

	if (!bar_tvremote_valid_app_id(app_id)) {
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
		            "not a Samsung app id");
		return FALSE;
	}
	msg = g_strdup_printf("{\"method\":\"ms.channel.emit\",\"params\":"
	                      "{\"event\":\"ed.apps.launch\",\"to\":\"host\","
	                      "\"data\":{\"appId\":\"%s\","
	                      "\"action_type\":\"DEEP_LINK\"}}}", app_id);
	return samsung_send(dev, s, t, msg, error);
}

const BarTvremoteBackend bar_tvremote_samsung_backend = {
	samsung_key,
	samsung_text,
	samsung_status,
	samsung_apps,
	samsung_launch
};
