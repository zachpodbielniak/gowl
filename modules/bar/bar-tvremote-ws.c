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
 * A minimal websocket client.  See bar-tvremote-ws.h.
 */

#include <string.h>
#include <gio/gio.h>

#include "bar-tvremote-ws.h"

#define WS_GUID "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

/* ----------------------------------------------------------------
 * Framing
 * ---------------------------------------------------------------- */

/**
 * bar_tvremote_ws_accept_key:
 * @key: the client's Sec-WebSocket-Key
 *
 * base64(SHA-1(key + the RFC's GUID)): what a server must echo in
 * Sec-WebSocket-Accept to prove it understood the upgrade.
 *
 * Returns: (transfer full): the expected accept value
 */
gchar *
bar_tvremote_ws_accept_key(const gchar *key)
{
	g_autoptr(GChecksum) sha = NULL;
	guint8 digest[20];
	gsize len = sizeof(digest);

	sha = g_checksum_new(G_CHECKSUM_SHA1);
	g_checksum_update(sha, (const guchar *)key, (gssize)strlen(key));
	g_checksum_update(sha, (const guchar *)WS_GUID, (gssize)strlen(WS_GUID));
	g_checksum_get_digest(sha, digest, &len);
	return g_base64_encode(digest, len);
}

/**
 * bar_tvremote_ws_new_key:
 *
 * Returns: (transfer full): sixteen random bytes, base64-encoded
 */
gchar *
bar_tvremote_ws_new_key(void)
{
	guint8 raw[16];
	guint i;

	for (i = 0; i < sizeof(raw); i++)
		raw[i] = (guint8)g_random_int_range(0, 256);
	return g_base64_encode(raw, sizeof(raw));
}

/**
 * bar_tvremote_ws_handshake:
 * @host: the server host
 * @port: its port
 * @path: the request path, query included
 * @key: a key from bar_tvremote_ws_new_key()
 *
 * Returns: (transfer full): the upgrade request
 */
gchar *
bar_tvremote_ws_handshake(const gchar *host, gint port, const gchar *path,
                          const gchar *key)
{
	const gchar *open = "", *close = "";

	if (strchr(host, ':') != NULL && host[0] != '[') {
		open = "[";
		close = "]";
	}
	return g_strdup_printf("GET %s HTTP/1.1\r\n"
	                       "Host: %s%s%s:%d\r\n"
	                       "Upgrade: websocket\r\n"
	                       "Connection: Upgrade\r\n"
	                       "Sec-WebSocket-Key: %s\r\n"
	                       "Sec-WebSocket-Version: 13\r\n"
	                       "\r\n",
	                       path, open, host, close, port, key);
}

/**
 * bar_tvremote_ws_check_response:
 * @response: the server's reply headers
 * @key: the key that was sent
 * @error: return location for a #GError
 *
 * The status must be 101 and Sec-WebSocket-Accept must match.  A
 * mismatch means a proxy or something that is not a websocket server
 * answered, and talking frames at it would be talking to the wrong
 * thing.
 *
 * Returns: %TRUE when the upgrade was accepted
 */
gboolean
bar_tvremote_ws_check_response(const gchar *response, const gchar *key,
                               GError **error)
{
	g_auto(GStrv) lines = NULL;
	g_autofree gchar *want = NULL;
	gboolean accepted = FALSE;
	guint i;

	if (response == NULL ||
	    (!g_str_has_prefix(response, "HTTP/1.1 101") &&
	     !g_str_has_prefix(response, "HTTP/1.0 101"))) {
		g_autofree gchar *first = NULL;

		first = g_strndup(response != NULL ? response : "", 40);
		g_strdelimit(first, "\r\n", ' ');
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
		            "the TV refused the websocket upgrade (%s)", first);
		return FALSE;
	}
	want = bar_tvremote_ws_accept_key(key);
	lines = g_strsplit(response, "\r\n", -1);
	for (i = 1; lines[i] != NULL; i++) {
		gchar *colon = strchr(lines[i], ':');

		if (colon == NULL)
			continue;
		if (g_ascii_strncasecmp(lines[i], "Sec-WebSocket-Accept",
		                        (gsize)(colon - lines[i])) == 0 &&
		    (gsize)(colon - lines[i]) == strlen("Sec-WebSocket-Accept")) {
			accepted = (strcmp(g_strstrip(colon + 1), want) == 0);
			break;
		}
	}
	if (!accepted)
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
		            "the server's Sec-WebSocket-Accept did not match");
	return accepted;
}

/**
 * bar_tvremote_ws_frame:
 * @opcode: the frame type
 * @payload: (array length=len) (nullable): the data
 * @len: its length
 * @mask: (array fixed-size=4) (nullable): the masking key, or %NULL
 *   for an unmasked (server) frame
 *
 * One complete frame with FIN set.  A client MUST mask every frame
 * (RFC 6455 5.3); a server that receives an unmasked one closes the
 * connection, which on a Samsung looks like the TV simply ignoring
 * every key.
 *
 * Returns: (transfer full): the encoded frame
 */
GByteArray *
bar_tvremote_ws_frame(guint8 opcode, const guint8 *payload, gsize len,
                      const guint8 *mask)
{
	GByteArray *out;
	guint8 head[14];
	gsize n = 0;
	gsize i;

	head[n++] = (guint8)(0x80 | (opcode & 0x0f));
	if (len < 126) {
		head[n++] = (guint8)((mask != NULL ? 0x80 : 0) | len);
	} else if (len <= 0xffff) {
		head[n++] = (guint8)((mask != NULL ? 0x80 : 0) | 126);
		head[n++] = (guint8)((len >> 8) & 0xff);
		head[n++] = (guint8)(len & 0xff);
	} else {
		guint s;

		head[n++] = (guint8)((mask != NULL ? 0x80 : 0) | 127);
		for (s = 0; s < 8; s++)
			head[n++] = (guint8)(((guint64)len >> (56 - 8 * s)) & 0xff);
	}
	if (mask != NULL) {
		memcpy(head + n, mask, 4);
		n += 4;
	}
	out = g_byte_array_sized_new((guint)(n + len));
	g_byte_array_append(out, head, (guint)n);
	for (i = 0; i < len; i++) {
		guint8 b = payload[i];

		if (mask != NULL)
			b ^= mask[i % 4];
		g_byte_array_append(out, &b, 1);
	}
	return out;
}

/**
 * bar_tvremote_ws_parse:
 * @data: (array length=len): received bytes
 * @len: how many
 * @opcode: (out): the frame's opcode
 * @fin: (out): whether it ends a message
 * @masked: (out) (optional): whether it was masked
 * @payload: (out) (transfer full): the unmasked payload
 *
 * Returns: the bytes the frame occupied; 0 when @data holds less than
 *   one whole frame; -1 when the frame is malformed or larger than
 *   %BAR_TVREMOTE_WS_MAX_PAYLOAD
 */
gssize
bar_tvremote_ws_parse(const guint8 *data, gsize len, guint8 *opcode,
                      gboolean *fin, gboolean *masked, GBytes **payload)
{
	guint64 plen;
	gsize off = 2;
	gboolean is_masked;
	const guint8 *key = NULL;
	guint8 *buf;
	guint64 i;

	*payload = NULL;
	if (len < 2)
		return 0;
	*fin = (data[0] & 0x80) != 0;
	if ((data[0] & 0x70) != 0)
		return -1;              /* reserved bits: no extensions here */
	*opcode = (guint8)(data[0] & 0x0f);
	is_masked = (data[1] & 0x80) != 0;
	if (masked != NULL)
		*masked = is_masked;
	plen = data[1] & 0x7f;
	if (plen == 126) {
		if (len < 4)
			return 0;
		plen = ((guint64)data[2] << 8) | data[3];
		off = 4;
	} else if (plen == 127) {
		guint s;

		if (len < 10)
			return 0;
		plen = 0;
		for (s = 0; s < 8; s++)
			plen = (plen << 8) | data[2 + s];
		off = 10;
	}
	if (plen > BAR_TVREMOTE_WS_MAX_PAYLOAD)
		return -1;
	if (is_masked) {
		if (len < off + 4)
			return 0;
		key = data + off;
		off += 4;
	}
	if ((guint64)len < (guint64)off + plen)
		return 0;

	buf = g_malloc((gsize)plen + 1);
	for (i = 0; i < plen; i++)
		buf[i] = (guint8)(data[off + i] ^ (key != NULL ? key[i % 4] : 0));
	buf[plen] = '\0';
	*payload = g_bytes_new_take(buf, (gsize)plen);
	return (gssize)(off + plen);
}

/* ----------------------------------------------------------------
 * The client
 * ---------------------------------------------------------------- */

struct _BarTvremoteWs {
	GSocketClient     *client;
	GIOStream         *conn;
	GSocket           *socket;
	GInputStream      *in;
	GOutputStream     *out;
	GByteArray        *rbuf;      /* received, not yet framed */
	GByteArray        *message;   /* fragments of a message so far */
	gboolean           open;
};

/*
 * Accept the TV's certificate.  Both Samsung (8002) and LG (3001) use
 * self-signed certificates with no name a client could check, so
 * verification can only fail; the alternative to accepting it is not
 * talking to the TV at all.  What this does NOT protect against is
 * someone on the LAN impersonating the TV -- the same exposure as the
 * plain-text ports, and documented as such.
 */
static gboolean
ws_accept_certificate(GTlsConnection *conn, GTlsCertificate *cert,
                      GTlsCertificateFlags errors, gpointer user_data)
{
	(void)conn;
	(void)cert;
	(void)errors;
	(void)user_data;
	return TRUE;
}

static void
ws_client_event(GSocketClient *client, GSocketClientEvent event,
                GSocketConnectable *connectable, GIOStream *connection,
                gpointer user_data)
{
	(void)client;
	(void)connectable;
	(void)user_data;
	if (event == G_SOCKET_CLIENT_TLS_HANDSHAKING &&
	    G_IS_TLS_CLIENT_CONNECTION(connection))
		g_signal_connect(connection, "accept-certificate",
		                 G_CALLBACK(ws_accept_certificate), NULL);
}

/* Read whatever is available within @deadline (monotonic µs) into the
   receive buffer.  Returns FALSE with @error on EOF, error or timeout. */
static gboolean
ws_fill(BarTvremoteWs *ws, gint64 deadline, GError **error)
{
	guint8 buf[8192];

	for (;;) {
		GError *local = NULL;
		gssize n;
		gint64 left;

		if (G_IS_POLLABLE_INPUT_STREAM(ws->in) &&
		    g_pollable_input_stream_can_poll(G_POLLABLE_INPUT_STREAM(ws->in))) {
			n = g_pollable_input_stream_read_nonblocking(
				G_POLLABLE_INPUT_STREAM(ws->in), buf, sizeof(buf),
				NULL, &local);
		} else {
			n = g_input_stream_read(ws->in, buf, sizeof(buf), NULL,
			                        &local);
		}
		if (n > 0) {
			g_byte_array_append(ws->rbuf, buf, (guint)n);
			return TRUE;
		}
		if (n == 0) {
			ws->open = FALSE;
			g_set_error(error, G_IO_ERROR, G_IO_ERROR_CLOSED,
			            "the TV closed the connection");
			return FALSE;
		}
		if (!g_error_matches(local, G_IO_ERROR, G_IO_ERROR_WOULD_BLOCK)) {
			ws->open = FALSE;
			g_propagate_error(error, local);
			return FALSE;
		}
		g_clear_error(&local);

		left = deadline - g_get_monotonic_time();
		if (left <= 0) {
			g_set_error(error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT,
			            "no answer from the TV");
			return FALSE;
		}
		/* A TLS record arriving in pieces wakes this up without
		   making the stream readable; the loop just waits again. */
		if (!g_socket_condition_timed_wait(ws->socket, G_IO_IN, left,
		                                   NULL, &local)) {
			if (g_error_matches(local, G_IO_ERROR,
			                    G_IO_ERROR_TIMED_OUT)) {
				g_clear_error(&local);
				g_set_error(error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT,
				            "no answer from the TV");
				return FALSE;
			}
			g_propagate_error(error, local);
			return FALSE;
		}
	}
}

static gboolean
ws_write_frame(BarTvremoteWs *ws, guint8 opcode, const guint8 *payload,
               gsize len, GError **error)
{
	g_autoptr(GByteArray) frame = NULL;
	guint8 mask[4];
	guint i;

	for (i = 0; i < 4; i++)
		mask[i] = (guint8)g_random_int_range(0, 256);
	frame = bar_tvremote_ws_frame(opcode, payload, len, mask);
	if (!g_output_stream_write_all(ws->out, frame->data, frame->len,
	                               NULL, NULL, error)) {
		ws->open = FALSE;
		return FALSE;
	}
	return TRUE;
}

/**
 * bar_tvremote_ws_connect:
 * @host: the TV
 * @port: its websocket port
 * @path: the request path, query included
 * @tls: wss rather than ws
 * @timeout_s: the deadline for the connection and the upgrade
 * @error: return location for a #GError
 *
 * Returns: (transfer full) (nullable): an open connection, or %NULL
 */
BarTvremoteWs *
bar_tvremote_ws_connect(const gchar *host, gint port, const gchar *path,
                        gboolean tls, guint timeout_s, GError **error)
{
	BarTvremoteWs *ws;
	GSocketConnection *conn;
	g_autofree gchar *key = NULL;
	g_autofree gchar *req = NULL;
	gint64 deadline;
	const gchar *end = NULL;

	ws = g_new0(BarTvremoteWs, 1);
	ws->rbuf = g_byte_array_new();
	ws->message = g_byte_array_new();
	ws->client = g_socket_client_new();
	g_socket_client_set_timeout(ws->client, timeout_s);
	if (tls) {
		g_socket_client_set_tls(ws->client, TRUE);
		g_signal_connect(ws->client, "event",
		                 G_CALLBACK(ws_client_event), NULL);
	}
	conn = g_socket_client_connect_to_host(ws->client, host, (guint16)port,
	                                       NULL, error);
	if (conn == NULL) {
		bar_tvremote_ws_free(ws);
		return NULL;
	}
	ws->conn = G_IO_STREAM(conn);
	ws->socket = g_socket_connection_get_socket(conn);
	g_socket_set_timeout(ws->socket, timeout_s);
	ws->in = g_io_stream_get_input_stream(ws->conn);
	ws->out = g_io_stream_get_output_stream(ws->conn);
	ws->open = TRUE;

	key = bar_tvremote_ws_new_key();
	req = bar_tvremote_ws_handshake(host, port, path, key);
	if (!g_output_stream_write_all(ws->out, req, strlen(req), NULL, NULL,
	                               error)) {
		bar_tvremote_ws_free(ws);
		return NULL;
	}

	/* Read the reply headers.  Bytes after the blank line are already
	   frames (Samsung sends its channel-connect event at once) and
	   stay in the buffer. */
	deadline = g_get_monotonic_time() + (gint64)timeout_s * G_USEC_PER_SEC;
	while (end == NULL) {
		g_autofree gchar *text = NULL;

		if (ws->rbuf->len > 16384) {
			g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
			            "oversized websocket reply");
			bar_tvremote_ws_free(ws);
			return NULL;
		}
		/* An empty GByteArray's data may be NULL, and g_strndup()
		   of NULL is NULL: nothing to look at yet. */
		text = (ws->rbuf->len > 0)
			? g_strndup((const gchar *)ws->rbuf->data, ws->rbuf->len)
			: g_strdup("");
		end = strstr(text, "\r\n\r\n");
		if (end != NULL) {
			gsize head_len = (gsize)(end - text) + 4;

			text[head_len] = '\0';
			if (!bar_tvremote_ws_check_response(text, key, error)) {
				bar_tvremote_ws_free(ws);
				return NULL;
			}
			g_byte_array_remove_range(ws->rbuf, 0, (guint)head_len);
			break;
		}
		if (!ws_fill(ws, deadline, error)) {
			bar_tvremote_ws_free(ws);
			return NULL;
		}
	}
	return ws;
}

/**
 * bar_tvremote_ws_send_text:
 * @ws: a connection
 * @text: a UTF-8 message
 * @error: return location for a #GError
 *
 * Returns: %TRUE when the frame was written
 */
gboolean
bar_tvremote_ws_send_text(BarTvremoteWs *ws, const gchar *text,
                          GError **error)
{
	if (ws == NULL || !ws->open) {
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_CONNECTED,
		            "not connected to the TV");
		return FALSE;
	}
	return ws_write_frame(ws, BAR_TVREMOTE_WS_TEXT, (const guint8 *)text,
	                      strlen(text), error);
}

/**
 * bar_tvremote_ws_recv_text:
 * @ws: a connection
 * @timeout_ms: how long to wait; 0 only takes what has arrived
 * @error: return location for a #GError
 *
 * The next text message.  Pings are answered on the way; a close frame
 * marks the connection closed and is an error; binary messages are
 * skipped.
 *
 * Returns: (transfer full) (nullable): the message, or %NULL with
 *   @error set (%G_IO_ERROR_TIMED_OUT when nothing came in time)
 */
gchar *
bar_tvremote_ws_recv_text(BarTvremoteWs *ws, gint timeout_ms,
                          GError **error)
{
	gint64 deadline;

	if (ws == NULL || !ws->open) {
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_CONNECTED,
		            "not connected to the TV");
		return NULL;
	}
	deadline = g_get_monotonic_time() + (gint64)timeout_ms * 1000;

	for (;;) {
		guint8 opcode = 0;
		gboolean fin = FALSE;
		g_autoptr(GBytes) payload = NULL;
		gssize used;
		gsize plen;
		const guint8 *pdata;

		used = bar_tvremote_ws_parse(ws->rbuf->data, ws->rbuf->len,
		                             &opcode, &fin, NULL, &payload);
		if (used < 0) {
			ws->open = FALSE;
			g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
			            "malformed websocket frame from the TV");
			return NULL;
		}
		if (used == 0) {
			if (!ws_fill(ws, deadline, error))
				return NULL;
			continue;
		}
		g_byte_array_remove_range(ws->rbuf, 0, (guint)used);
		pdata = g_bytes_get_data(payload, &plen);

		switch (opcode) {
		case BAR_TVREMOTE_WS_PING:
			if (!ws_write_frame(ws, BAR_TVREMOTE_WS_PONG, pdata, plen,
			                    error))
				return NULL;
			continue;
		case BAR_TVREMOTE_WS_PONG:
			continue;
		case BAR_TVREMOTE_WS_CLOSE:
			ws->open = FALSE;
			g_set_error(error, G_IO_ERROR, G_IO_ERROR_CLOSED,
			            "the TV closed the connection");
			return NULL;
		default:
			break;
		}

		/* Text, binary or a continuation: gather until FIN. */
		if (ws->message->len + plen > BAR_TVREMOTE_WS_MAX_PAYLOAD) {
			ws->open = FALSE;
			g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
			            "oversized message from the TV");
			return NULL;
		}
		if (opcode != 0)
			g_byte_array_set_size(ws->message, 0);
		g_byte_array_append(ws->message, pdata, (guint)plen);
		if (!fin)
			continue;
		if (opcode == BAR_TVREMOTE_WS_BINARY) {
			g_byte_array_set_size(ws->message, 0);
			continue;
		}
		{
			gchar *text;

			text = g_strndup((const gchar *)ws->message->data,
			                 ws->message->len);
			g_byte_array_set_size(ws->message, 0);
			if (!g_utf8_validate(text, -1, NULL)) {
				gchar *valid = g_utf8_make_valid(text, -1);

				g_free(text);
				text = valid;
			}
			return text;
		}
	}
}

/**
 * bar_tvremote_ws_is_open:
 * @ws: (nullable): a connection
 *
 * Returns: %TRUE until a write fails, a read fails or a close arrives
 */
gboolean
bar_tvremote_ws_is_open(BarTvremoteWs *ws)
{
	return ws != NULL && ws->open;
}

/**
 * bar_tvremote_ws_free:
 * @ws: (nullable): a connection
 *
 * Sends a close frame if the connection is still open, then closes.
 */
void
bar_tvremote_ws_free(BarTvremoteWs *ws)
{
	if (ws == NULL)
		return;
	if (ws->open) {
		static const guint8 normal[2] = { 0x03, 0xe8 };   /* 1000 */

		ws_write_frame(ws, BAR_TVREMOTE_WS_CLOSE, normal, 2, NULL);
	}
	if (ws->conn != NULL) {
		g_io_stream_close(ws->conn, NULL, NULL);
		g_object_unref(ws->conn);
	}
	g_clear_object(&ws->client);
	g_byte_array_unref(ws->rbuf);
	g_byte_array_unref(ws->message);
	g_free(ws);
}
