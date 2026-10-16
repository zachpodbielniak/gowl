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
 * LG webOS, over SSAP: JSON requests on a websocket (wss on 3001 for
 * current firmware, ws on 3000 for older).
 *
 * Pairing: the client sends `register' with a manifest of the
 * permissions it wants.  The TV answers `response' with pairingType
 * PROMPT and puts a prompt on screen; once accepted it sends
 * `registered' carrying a CLIENT KEY.  A later register that presents
 * the key is let straight in.  The key is written to the device list
 * through save_secret.
 *
 * The remote is split in two.  Volume, power, text, apps and status are
 * SSAP requests on the main socket.  Arrows, OK, back, home and the
 * media buttons are not: the TV hands out the URL of a second
 * "pointer input" socket (getPointerInputSocket) that takes plain-text
 * `type:button\nname:UP\n\n' messages.
 *
 * The manifest is UNSIGNED.  LG's own apps send a signed one; every TV
 * tried so far accepts an unsigned manifest with a PROMPT pairing, but
 * a firmware that insists on the signature would refuse to register,
 * and the panel then shows the TV's error text.
 */

#include <string.h>
#include <gio/gio.h>

#include "bar-tvremote-backend.h"

#define LG_TIMEOUT_S       (4)
#define LG_REPLY_MS        (3000)

#define LG_PERMISSIONS \
	"\"LAUNCH\",\"LAUNCH_WEBAPP\",\"APP_TO_APP\",\"CONTROL_AUDIO\"," \
	"\"CONTROL_DISPLAY\",\"CONTROL_INPUT_JOYSTICK\"," \
	"\"CONTROL_INPUT_MEDIA_PLAYBACK\",\"CONTROL_INPUT_TEXT\"," \
	"\"CONTROL_MOUSE_AND_KEYBOARD\",\"CONTROL_INPUT_TV\"," \
	"\"CONTROL_POWER\",\"READ_APP_STATUS\",\"READ_CURRENT_CHANNEL\"," \
	"\"READ_INPUT_DEVICE_LIST\",\"READ_INSTALLED_APPS\"," \
	"\"READ_RUNNING_APPS\",\"READ_TV_CURRENT_TIME\"," \
	"\"READ_POWER_STATE\",\"WRITE_NOTIFICATION_TOAST\""

/* Friendly names for the app ids webOS reports in the foreground. */
static const struct { const gchar *id; const gchar *name; } lg_names[] = {
	{ "netflix",                    "Netflix" },
	{ "youtube.leanback.v4",        "YouTube" },
	{ "amazon",                     "Prime Video" },
	{ "com.disney.disneyplus-prod", "Disney+" },
	{ "cdp-30",                     "Plex" },
	{ "com.webos.app.home",         "Home" },
	{ "com.webos.app.livetv",       "Live TV" },
	{ "com.webos.app.hdmi1",        "HDMI 1" },
	{ "com.webos.app.hdmi2",        "HDMI 2" },
	{ "com.webos.app.hdmi3",        "HDMI 3" },
	{ "com.webos.app.hdmi4",        "HDMI 4" },
	{ "com.webos.app.browser",      "Web Browser" }
};

static const gchar *
lg_app_name(const gchar *id)
{
	guint i;

	for (i = 0; i < G_N_ELEMENTS(lg_names); i++)
		if (g_strcmp0(lg_names[i].id, id) == 0)
			return lg_names[i].name;
	return id;
}

/* A client key is hex; anything else is not put in a message. */
static gboolean
key_ok(const gchar *key)
{
	const gchar *p;

	if (key == NULL || key[0] == '\0' || strlen(key) > 128)
		return FALSE;
	for (p = key; *p != '\0'; p++)
		if (!g_ascii_isalnum(*p))
			return FALSE;
	return TRUE;
}

static void
lg_drop(BarTvremoteSession *s)
{
	g_clear_pointer(&s->pointer, bar_tvremote_ws_free);
	g_clear_pointer(&s->ws, bar_tvremote_ws_free);
	s->ready = FALSE;
}

/* Act on one message; FALSE (with @error) when it ends the session. */
static gboolean
lg_absorb(const BarTvremoteDevice *dev, BarTvremoteSession *s,
          const BarTvremoteTools *t, const gchar *msg, GError **error)
{
	g_autofree gchar *type = NULL;
	g_autofree gchar *id = NULL;

	type = bar_tvremote_json_string(msg, "type", NULL);
	id = bar_tvremote_json_string(msg, "id", NULL);
	if (g_strcmp0(type, "registered") == 0) {
		g_autofree gchar *key = NULL;

		s->ready = TRUE;
		s->ws_backoff_s = 0;
		key = bar_tvremote_json_string(msg, "payload", "client-key", NULL);
		if (key_ok(key) && g_strcmp0(key, dev->client_key) != 0 &&
		    t->save_secret != NULL)
			t->save_secret(t->user_data, dev->id, "client-key", key);
		if (key != NULL)
			memset(key, 0, strlen(key));
		return TRUE;
	}
	if (g_strcmp0(type, "error") == 0 &&
	    g_strcmp0(id, "register_0") == 0) {
		g_autofree gchar *why = NULL;

		why = bar_tvremote_json_string(msg, "error", NULL);
		lg_drop(s);
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
		            "the TV refused to pair: %s",
		            why != NULL ? why : "no reason given");
		return FALSE;
	}
	return TRUE;
}

static gchar *
lg_register_message(const BarTvremoteDevice *dev)
{
	g_autofree gchar *key = NULL;

	if (key_ok(dev->client_key))
		key = g_strdup_printf("\"client-key\":\"%s\",", dev->client_key);
	return g_strdup_printf("{\"type\":\"register\",\"id\":\"register_0\","
	                       "\"payload\":{\"forcePairing\":false,"
	                       "\"pairingType\":\"PROMPT\",%s"
	                       "\"manifest\":{\"manifestVersion\":1,"
	                       "\"appVersion\":\"1.1\","
	                       "\"permissions\":[" LG_PERMISSIONS "]}}}",
	                       key != NULL ? key : "");
}

/* Read everything already waiting, acting on what matters. */
static gboolean
lg_drain(const BarTvremoteDevice *dev, BarTvremoteSession *s,
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
			lg_drop(s);
			g_propagate_error(error, g_steal_pointer(&local));
			return FALSE;
		}
		if (!lg_absorb(dev, s, t, msg, error))
			return FALSE;
	}
	return TRUE;
}

/* As samsung_connect: FALSE with G_IO_ERROR_PENDING while the prompt is
   up, keeping the socket so the next poll continues the same pairing. */
static gboolean
lg_connect(const BarTvremoteDevice *dev, BarTvremoteSession *s,
           const BarTvremoteTools *t, gint wait_ms, GError **error)
{
	gint64 deadline;

	if (bar_tvremote_ws_is_open(s->ws) && s->ready)
		return lg_drain(dev, s, t, error);

	if (!bar_tvremote_ws_is_open(s->ws)) {
		g_autofree gchar *reg = NULL;
		GError *local = NULL;
		gint port;

		lg_drop(s);
		if (g_get_monotonic_time() < s->ws_next_try) {
			g_set_error(error, G_IO_ERROR, G_IO_ERROR_HOST_UNREACHABLE,
			            "cannot reach the TV at %s; retrying",
			            dev->host);
			return FALSE;
		}
		port = bar_tvremote_device_port(dev);
		s->ws = bar_tvremote_ws_connect(dev->host, port, "/",
		                                port == 3001, LG_TIMEOUT_S,
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
		reg = lg_register_message(dev);
		if (!bar_tvremote_ws_send_text(s->ws, reg, error)) {
			lg_drop(s);
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
			lg_drop(s);
			g_propagate_error(error, g_steal_pointer(&local));
			return FALSE;
		}
		if (!lg_absorb(dev, s, t, msg, error))
			return FALSE;
	}
	return TRUE;
}

/*
 * One SSAP request.  @payload is a JSON object or NULL.  With @reply,
 * waits for the response with the same id and returns the whole
 * message; other messages that arrive meanwhile are absorbed.
 */
static gboolean
lg_request(const BarTvremoteDevice *dev, BarTvremoteSession *s,
           const BarTvremoteTools *t, const gchar *uri, const gchar *payload,
           gchar **reply, GError **error)
{
	g_autofree gchar *id = NULL;
	g_autofree gchar *msg = NULL;
	gint64 deadline;

	if (reply != NULL)
		*reply = NULL;
	if (!lg_connect(dev, s, t, 1500, error))
		return FALSE;
	id = g_strdup_printf("gowl_%u", ++s->msg_id);
	if (payload != NULL)
		msg = g_strdup_printf("{\"type\":\"request\",\"id\":\"%s\","
		                      "\"uri\":\"%s\",\"payload\":%s}",
		                      id, uri, payload);
	else
		msg = g_strdup_printf("{\"type\":\"request\",\"id\":\"%s\","
		                      "\"uri\":\"%s\"}", id, uri);
	if (!bar_tvremote_ws_send_text(s->ws, msg, error)) {
		lg_drop(s);
		return FALSE;
	}
	if (reply == NULL)
		return TRUE;

	deadline = g_get_monotonic_time() + (gint64)LG_REPLY_MS * 1000;
	for (;;) {
		g_autofree gchar *in = NULL;
		g_autofree gchar *in_id = NULL;
		g_autofree gchar *in_type = NULL;
		gint64 left = (deadline - g_get_monotonic_time()) / 1000;

		in = bar_tvremote_ws_recv_text(s->ws, (gint)MAX(left, 0), error);
		if (in == NULL)
			return FALSE;
		if (!lg_absorb(dev, s, t, in, error))
			return FALSE;
		in_id = bar_tvremote_json_string(in, "id", NULL);
		if (g_strcmp0(in_id, id) != 0)
			continue;
		in_type = bar_tvremote_json_string(in, "type", NULL);
		if (g_strcmp0(in_type, "error") == 0) {
			g_autofree gchar *why = NULL;

			why = bar_tvremote_json_string(in, "error", NULL);
			g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
			            "the TV refused %s: %s", uri,
			            why != NULL ? why : "error");
			return FALSE;
		}
		*reply = g_steal_pointer(&in);
		return TRUE;
	}
}

/* Open the pointer-input socket the TV names, if not already open. */
static gboolean
lg_pointer(const BarTvremoteDevice *dev, BarTvremoteSession *s,
           const BarTvremoteTools *t, GError **error)
{
	g_autofree gchar *reply = NULL;
	g_autofree gchar *url = NULL;
	g_autofree gchar *host = NULL;
	const gchar *path;
	gint port;

	if (bar_tvremote_ws_is_open(s->pointer))
		return TRUE;
	g_clear_pointer(&s->pointer, bar_tvremote_ws_free);
	if (!lg_request(dev, s, t,
	                "ssap://com.webos.service.networkinput/"
	                "getPointerInputSocket", NULL, &reply, error))
		return FALSE;
	url = bar_tvremote_json_string(reply, "payload", "socketPath", NULL);
	if (!bar_tvremote_url_host_port(url, &host, &port)) {
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
		            "the TV gave no usable button socket");
		return FALSE;
	}
	/* The path is everything after the authority: the first `/'
	   past the scheme's "//". */
	path = strstr(url, "//");
	if (path != NULL)
		path = strchr(path + 2, '/');
	s->pointer = bar_tvremote_ws_connect(host, port,
	                                     path != NULL ? path : "/",
	                                     g_str_has_prefix(url, "wss://"),
	                                     LG_TIMEOUT_S, error);
	return s->pointer != NULL;
}

static gboolean
lg_button(const BarTvremoteDevice *dev, BarTvremoteSession *s,
          const BarTvremoteTools *t, const gchar *name, GError **error)
{
	g_autofree gchar *msg = NULL;
	guint attempt;

	msg = g_strdup_printf("type:button\nname:%s\n\n", name);
	for (attempt = 0; attempt < 2; attempt++) {
		g_autoptr(GError) local = NULL;

		if (!lg_pointer(dev, s, t, error))
			return FALSE;
		if (bar_tvremote_ws_send_text(s->pointer, msg, &local))
			return TRUE;
		g_clear_pointer(&s->pointer, bar_tvremote_ws_free);
		if (attempt == 1) {
			g_propagate_error(error, g_steal_pointer(&local));
			return FALSE;
		}
	}
	return FALSE;
}

/* Current mute state, or -1 when the TV does not say. */
static gint
lg_muted(const BarTvremoteDevice *dev, BarTvremoteSession *s,
         const BarTvremoteTools *t, gint *volume)
{
	g_autofree gchar *reply = NULL;
	gint muted;

	*volume = -1;
	if (!lg_request(dev, s, t, "ssap://audio/getVolume", NULL, &reply,
	                NULL))
		return -1;
	/* Older firmware: payload.volume / .muted; newer nests both in
	   payload.volumeStatus as .volume / .muteStatus. */
	*volume = (gint)bar_tvremote_json_int(reply, -1, "payload", "volume",
	                                      NULL);
	if (*volume < 0)
		*volume = (gint)bar_tvremote_json_int(reply, -1, "payload",
		                                      "volumeStatus", "volume",
		                                      NULL);
	muted = bar_tvremote_json_bool(reply, "payload", "muted", NULL);
	if (muted < 0)
		muted = bar_tvremote_json_bool(reply, "payload", "volumeStatus",
		                               "muteStatus", NULL);
	return muted;
}

static gboolean
lg_key(const BarTvremoteDevice *dev, BarTvremoteSession *s,
       const BarTvremoteTools *t, BarTvremoteKey key, gboolean longpress,
       GError **error)
{
	const gchar *cmd = bar_tvremote_lg_key(key);

	(void)longpress;   /* webOS has no hold over this API */
	if (cmd == NULL) {
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
		            "LG has no %s key", bar_tvremote_key_name(key));
		return FALSE;
	}
	if (g_str_has_prefix(cmd, "btn:")) {
		const gchar *name = cmd + 4;

		if (key == BAR_TVREMOTE_KEY_PLAY_PAUSE) {
			name = s->play_next ? "PLAY" : "PAUSE";
			s->play_next = !s->play_next;
		}
		return lg_button(dev, s, t, name, error);
	}
	cmd += 5;      /* "ssap:" */
	if (key == BAR_TVREMOTE_KEY_MUTE) {
		gint volume;
		gint muted = lg_muted(dev, s, t, &volume);

		return lg_request(dev, s, t, cmd,
		                  (muted == 1) ? "{\"mute\":false}"
		                               : "{\"mute\":true}",
		                  NULL, error);
	}
	return lg_request(dev, s, t, cmd, NULL, NULL, error);
}

static gboolean
lg_text(const BarTvremoteDevice *dev, BarTvremoteSession *s,
        const BarTvremoteTools *t, const gchar *text, guint *dropped,
        GError **error)
{
	g_autofree gchar *clean = NULL;
	g_autofree gchar *esc = NULL;
	g_autofree gchar *payload = NULL;

	if (dropped != NULL)
		*dropped = 0;
	clean = bar_tvremote_clean_text(text, 1000);
	if (clean[0] == '\0') {
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
		            "nothing to type");
		return FALSE;
	}
	esc = bar_tvremote_json_escape(clean);
	payload = g_strdup_printf("{\"text\":\"%s\",\"replace\":0}", esc);
	return lg_request(dev, s, t, "ssap://com.webos.service.ime/insertText",
	                  payload, NULL, error);
}

static BarTvremoteStatus *
lg_status(const BarTvremoteDevice *dev, BarTvremoteSession *s,
          const BarTvremoteTools *t)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *reply = NULL;
	g_autofree gchar *app = NULL;
	BarTvremoteStatus *st;
	gint volume, muted;

	if (!lg_connect(dev, s, t, 1500, &error)) {
		if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_PENDING))
			return bar_tvremote_status_new(BAR_TVREMOTE_LINK_PAIRING,
			                               error->message);
		return bar_tvremote_status_new(BAR_TVREMOTE_LINK_OFFLINE,
		                               error->message);
	}
	st = bar_tvremote_status_new(BAR_TVREMOTE_LINK_ONLINE, NULL);
	if (lg_request(dev, s, t,
	               "ssap://com.webos.applicationManager/"
	               "getForegroundAppInfo", NULL, &reply, NULL)) {
		app = bar_tvremote_json_string(reply, "payload", "appId", NULL);
		if (bar_tvremote_valid_app_id(app)) {
			st->app_id = g_strdup(app);
			st->app_name = bar_tvremote_clean_text(lg_app_name(app), 120);
		}
	}
	muted = lg_muted(dev, s, t, &volume);
	st->volume = (volume >= 0 && volume <= 100) ? volume : -1;
	st->muted = (muted == 1);
	return st;
}

static GPtrArray *
lg_apps(const BarTvremoteDevice *dev, BarTvremoteSession *s,
        const BarTvremoteTools *t, GError **error)
{
	g_autofree gchar *reply = NULL;

	if (!lg_request(dev, s, t,
	                "ssap://com.webos.applicationManager/listLaunchPoints",
	                NULL, &reply, error))
		return NULL;
	return bar_tvremote_json_apps(reply, "id", "title", "payload",
	                              "launchPoints", NULL);
}

static gboolean
lg_launch(const BarTvremoteDevice *dev, BarTvremoteSession *s,
          const BarTvremoteTools *t, const gchar *app_id, GError **error)
{
	g_autofree gchar *payload = NULL;

	if (!bar_tvremote_valid_app_id(app_id)) {
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
		            "not a webOS app id");
		return FALSE;
	}
	payload = g_strdup_printf("{\"id\":\"%s\"}", app_id);
	return lg_request(dev, s, t, "ssap://system.launcher/launch", payload,
	                  NULL, error);
}

const BarTvremoteBackend bar_tvremote_lg_backend = {
	lg_key,
	lg_text,
	lg_status,
	lg_apps,
	lg_launch
};
