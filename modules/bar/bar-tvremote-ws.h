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

#ifndef BAR_TVREMOTE_WS_H
#define BAR_TVREMOTE_WS_H

#include <glib.h>

G_BEGIN_DECLS

/*
 * A minimal RFC 6455 websocket client, for the two TV protocols that
 * speak it (Samsung Tizen and LG webOS).
 *
 * Minimal on purpose: text frames out, text frames in, ping answered,
 * close honoured, fragmented messages reassembled.  No extensions, no
 * subprotocols, no compression -- neither TV asks for any.  bar.so links
 * no websocket library (libsoup is only in the MCP module), and linking
 * one into a module that cmacs loads into itself is a bigger change
 * than two hundred lines of framing.
 *
 * Every call BLOCKS, with a deadline.  They are only ever made from a
 * bar worker thread, never from the compositor's.
 */

#define BAR_TVREMOTE_WS_TEXT   (0x1)
#define BAR_TVREMOTE_WS_BINARY (0x2)
#define BAR_TVREMOTE_WS_CLOSE  (0x8)
#define BAR_TVREMOTE_WS_PING   (0x9)
#define BAR_TVREMOTE_WS_PONG   (0xa)

/* The largest message accepted: an LG launch-point list with icons
   inlined is the biggest thing either TV sends, well under this. */
#define BAR_TVREMOTE_WS_MAX_PAYLOAD (4 * 1024 * 1024)

/* --- Pure framing (tested directly) -------------------------------- */

gchar      *bar_tvremote_ws_accept_key (const gchar *key);
gchar      *bar_tvremote_ws_new_key    (void);
gchar      *bar_tvremote_ws_handshake  (const gchar *host,
                                        gint         port,
                                        const gchar *path,
                                        const gchar *key);
gboolean    bar_tvremote_ws_check_response (const gchar *response,
                                            const gchar *key,
                                            GError     **error);
GByteArray *bar_tvremote_ws_frame      (guint8        opcode,
                                        const guint8 *payload,
                                        gsize         len,
                                        const guint8 *mask);
gssize      bar_tvremote_ws_parse      (const guint8 *data,
                                        gsize         len,
                                        guint8       *opcode,
                                        gboolean     *fin,
                                        gboolean     *masked,
                                        GBytes      **payload);

/* --- The client ---------------------------------------------------- */

typedef struct _BarTvremoteWs BarTvremoteWs;

BarTvremoteWs *bar_tvremote_ws_connect   (const gchar *host,
                                          gint         port,
                                          const gchar *path,
                                          gboolean     tls,
                                          guint        timeout_s,
                                          GError     **error);
gboolean       bar_tvremote_ws_send_text (BarTvremoteWs *ws,
                                          const gchar   *text,
                                          GError       **error);
gchar         *bar_tvremote_ws_recv_text (BarTvremoteWs *ws,
                                          gint           timeout_ms,
                                          GError       **error);
gboolean       bar_tvremote_ws_is_open   (BarTvremoteWs *ws);
void           bar_tvremote_ws_free      (BarTvremoteWs *ws);

G_END_DECLS

#endif /* BAR_TVREMOTE_WS_H */
