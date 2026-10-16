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

#ifndef BAR_TVREMOTE_BACKEND_H
#define BAR_TVREMOTE_BACKEND_H

#include <glib.h>

#include "bar-tvremote-model.h"
#include "bar-tvremote-ws.h"

G_BEGIN_DECLS

/*
 * The device half of the tvremote widget: one small vtable per kind of
 * device, and the per-device session state they keep between calls.
 *
 * EVERY function here blocks -- on a subprocess, a socket or a sleep
 * for a long press -- and is only ever called from a bar worker thread
 * (the async poll or gowl_bar_plugin_queue_work()), never from the
 * compositor's.  A session's `io' lock serialises the I/O to one
 * device, so a key pressed while the status poll is talking to the TV
 * waits its turn on the worker rather than interleaving frames.
 */

/**
 * BarTvremoteStatus:
 * @link: how the connection is
 * @app_id: (nullable): the foreground app's id
 * @app_name: (nullable): its display name
 * @title: (nullable): what is playing, where the device says
 * @volume: 0--100, or -1 when the device does not report it
 * @muted: whether it is muted
 * @detail: (nullable): a sentence for the panel: what is wrong, or what
 *   the user must do ("Accept the prompt on your TV")
 */
typedef struct {
	BarTvremoteLink link;
	gchar          *app_id;
	gchar          *app_name;
	gchar          *title;
	gint            volume;
	gboolean        muted;
	gchar          *detail;
} BarTvremoteStatus;

BarTvremoteStatus *bar_tvremote_status_new  (BarTvremoteLink link,
                                             const gchar    *detail);
BarTvremoteStatus *bar_tvremote_status_copy (const BarTvremoteStatus *st);
void               bar_tvremote_status_free (BarTvremoteStatus *st);

/**
 * BarTvremoteTools:
 * @adb: the adb command
 * @curl: the curl command
 * @catt: the catt command
 * @atvremote: the atvremote command
 * @save_secret: (nullable): called when pairing produces a token or a
 *   client key that must be written to the device list
 * @user_data: for @save_secret
 *
 * What a backend needs from the plugin: the commands the user
 * configured, and a way to persist what pairing hands back.
 */
typedef struct {
	gchar   *adb;
	gchar   *curl;
	gchar   *catt;
	gchar   *atvremote;
	void   (*save_secret) (gpointer     user_data,
	                       const gchar *device_id,
	                       const gchar *field,
	                       const gchar *value);
	gpointer user_data;
} BarTvremoteTools;

/**
 * BarTvremoteSession:
 *
 * What a backend keeps between calls for one device: the persistent
 * adb shell, the open websocket(s), reconnect backoff.  Owned by the
 * plugin, one per device id; @io serialises all I/O to the device.
 */
typedef struct {
	GMutex               io;

	/* adb */
	BarTvremoteAdbState  adb_state;
	gint64               next_connect;     /* monotonic µs */
	gint                 backoff_s;
	GPid                 shell_pid;        /* 0 when none */
	gint                 shell_fd;         /* -1 when none */
	gint64               restart_window;   /* start of the count, µs */
	guint                restarts;         /* respawns in the window */
	guint                spawns;           /* lifetime, for tests */

	/* websocket kinds */
	BarTvremoteWs       *ws;
	BarTvremoteWs       *pointer;          /* LG's button socket */
	gboolean             ready;            /* paired and registered */
	gint64               ws_next_try;
	gint                 ws_backoff_s;
	gboolean             play_next;        /* alternate PLAY/PAUSE */
	guint                msg_id;
} BarTvremoteSession;

BarTvremoteSession *bar_tvremote_session_new  (void);
void                bar_tvremote_session_free (BarTvremoteSession *s);
void                bar_tvremote_session_reset (BarTvremoteSession *s);
void                bar_tvremote_session_stop_shell (BarTvremoteSession *s);

/**
 * BarTvremoteBackend:
 * @key: press (or with @longpress hold) one key
 * @text: type text into the focused field
 * @status: connect if needed and report; never fails, the status says
 * @apps: the launchable apps
 * @launch: start one
 *
 * One per device kind.  All slots are called with the session's @io
 * lock held by the caller.
 */
typedef struct {
	gboolean           (*key)    (const BarTvremoteDevice *dev,
	                              BarTvremoteSession      *s,
	                              const BarTvremoteTools  *t,
	                              BarTvremoteKey           key,
	                              gboolean                 longpress,
	                              GError                 **error);
	gboolean           (*text)   (const BarTvremoteDevice *dev,
	                              BarTvremoteSession      *s,
	                              const BarTvremoteTools  *t,
	                              const gchar             *text,
	                              guint                   *dropped,
	                              GError                 **error);
	BarTvremoteStatus *(*status) (const BarTvremoteDevice *dev,
	                              BarTvremoteSession      *s,
	                              const BarTvremoteTools  *t);
	GPtrArray         *(*apps)   (const BarTvremoteDevice *dev,
	                              BarTvremoteSession      *s,
	                              const BarTvremoteTools  *t,
	                              GError                 **error);
	gboolean           (*launch) (const BarTvremoteDevice *dev,
	                              BarTvremoteSession      *s,
	                              const BarTvremoteTools  *t,
	                              const gchar             *app_id,
	                              GError                 **error);
} BarTvremoteBackend;

const BarTvremoteBackend *bar_tvremote_backend_for (BarTvremoteKind kind);

extern const BarTvremoteBackend bar_tvremote_adb_backend;
extern const BarTvremoteBackend bar_tvremote_roku_backend;
extern const BarTvremoteBackend bar_tvremote_samsung_backend;
extern const BarTvremoteBackend bar_tvremote_lg_backend;
extern const BarTvremoteBackend bar_tvremote_cast_backend;
extern const BarTvremoteBackend bar_tvremote_appletv_backend;

/* --- Helpers shared by the backends -------------------------------- */

gboolean bar_tvremote_run (const gchar * const *argv,
                           gint                 timeout_ms,
                           gchar              **out,
                           gchar              **err,
                           gint                *exit_status,
                           GError             **error);
gboolean bar_tvremote_have (const gchar *command);
const gchar *bar_tvremote_tool_package (const gchar *tool);

/* --- Discovery ------------------------------------------------------ */

/**
 * BarTvremoteFound:
 * @kind: what it looks like
 * @host: its address
 * @port: the port that answered
 * @how: (nullable): how it was found ("SSDP", "mDNS", "port 5555")
 */
typedef struct {
	BarTvremoteKind kind;
	gchar          *host;
	gint            port;
	gchar          *how;
} BarTvremoteFound;

void       bar_tvremote_found_free   (BarTvremoteFound *f);
gchar     *bar_tvremote_detect_subnet (void);
GPtrArray *bar_tvremote_scan_ports   (GPtrArray   *hosts,
                                      const gint  *ports,
                                      guint        n_ports,
                                      gint         timeout_ms,
                                      guint        concurrency);
GPtrArray *bar_tvremote_scan_ssdp    (gint timeout_ms);
GPtrArray *bar_tvremote_scan         (const BarTvremoteTools *t,
                                      const gchar            *subnet,
                                      GError                **error);

G_END_DECLS

#endif /* BAR_TVREMOTE_BACKEND_H */
