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

#ifndef BAR_TVREMOTE_MODEL_H
#define BAR_TVREMOTE_MODEL_H

#include <glib.h>

G_BEGIN_DECLS

/*
 * The tvremote widget's pure half.
 *
 * Everything here is a function of its arguments: keymaps, the
 * escaping that keeps typed text from becoming a device shell command,
 * parsers for what `adb', Roku's ECP and SSDP answer with, the remote's
 * button layout and its hit-test, and the routing that sends volume to
 * a sound bar.  No subprocess, no socket, no plugin, no theme -- so
 * tests/test-bar-tvremote.c can feed each one exactly the input that
 * once went wrong.
 */

/**
 * BarTvremoteKind:
 * @BAR_TVREMOTE_KIND_FIRETV: Amazon Fire TV, over adb
 * @BAR_TVREMOTE_KIND_ANDROIDTV: Android TV / Google TV, over adb
 * @BAR_TVREMOTE_KIND_ROKU: Roku, over ECP (HTTP on 8060)
 * @BAR_TVREMOTE_KIND_SAMSUNG: Samsung Tizen, over its websocket API
 * @BAR_TVREMOTE_KIND_LG: LG webOS, over SSAP (websocket)
 * @BAR_TVREMOTE_KIND_CAST: a Chromecast, through the `catt' CLI
 * @BAR_TVREMOTE_KIND_APPLETV: an Apple TV, through pyatv's `atvremote'
 * @BAR_TVREMOTE_KIND_UNKNOWN: anything else; never controllable
 *
 * The device kinds, in the order the panel offers them.
 */
typedef enum {
	BAR_TVREMOTE_KIND_FIRETV = 0,
	BAR_TVREMOTE_KIND_ANDROIDTV,
	BAR_TVREMOTE_KIND_ROKU,
	BAR_TVREMOTE_KIND_SAMSUNG,
	BAR_TVREMOTE_KIND_LG,
	BAR_TVREMOTE_KIND_CAST,
	BAR_TVREMOTE_KIND_APPLETV,
	BAR_TVREMOTE_KIND_UNKNOWN
} BarTvremoteKind;

#define BAR_TVREMOTE_N_KINDS ((guint)BAR_TVREMOTE_KIND_UNKNOWN)

/**
 * BarTvremoteKey:
 *
 * Every button a remote can have.  Not every kind has every one; see
 * bar_tvremote_kind_supports().
 */
typedef enum {
	BAR_TVREMOTE_KEY_UP = 0,
	BAR_TVREMOTE_KEY_DOWN,
	BAR_TVREMOTE_KEY_LEFT,
	BAR_TVREMOTE_KEY_RIGHT,
	BAR_TVREMOTE_KEY_SELECT,
	BAR_TVREMOTE_KEY_BACK,
	BAR_TVREMOTE_KEY_HOME,
	BAR_TVREMOTE_KEY_MENU,
	BAR_TVREMOTE_KEY_PLAY_PAUSE,
	BAR_TVREMOTE_KEY_REWIND,
	BAR_TVREMOTE_KEY_FAST_FORWARD,
	BAR_TVREMOTE_KEY_VOL_UP,
	BAR_TVREMOTE_KEY_VOL_DOWN,
	BAR_TVREMOTE_KEY_MUTE,
	BAR_TVREMOTE_KEY_POWER,
	BAR_TVREMOTE_KEY_SLEEP,
	BAR_TVREMOTE_KEY_WAKEUP,
	BAR_TVREMOTE_KEY_SEARCH,
	BAR_TVREMOTE_KEY_SETTINGS,
	BAR_TVREMOTE_KEY_INPUT,
	BAR_TVREMOTE_N_KEYS
} BarTvremoteKey;

/* --- Kinds and keys ------------------------------------------------ */

const gchar     *bar_tvremote_kind_name      (BarTvremoteKind kind);
const gchar     *bar_tvremote_kind_title     (BarTvremoteKind kind);
BarTvremoteKind  bar_tvremote_kind_from_name (const gchar *name);
gint             bar_tvremote_kind_default_port (BarTvremoteKind kind);
gboolean         bar_tvremote_kind_uses_adb  (BarTvremoteKind kind);

const gchar     *bar_tvremote_key_name      (BarTvremoteKey key);
gint             bar_tvremote_key_from_name (const gchar *name);
const gchar     *bar_tvremote_key_label     (BarTvremoteKey key);
gboolean         bar_tvremote_key_is_volume (BarTvremoteKey key);
gboolean         bar_tvremote_kind_supports (BarTvremoteKind kind,
                                             BarTvremoteKey  key);

/* Per-protocol keymaps.  -1 / %NULL means "this kind has no such key". */
gint         bar_tvremote_android_keycode (BarTvremoteKind kind,
                                           BarTvremoteKey  key);
const gchar *bar_tvremote_roku_key        (BarTvremoteKey key);
const gchar *bar_tvremote_samsung_key     (BarTvremoteKey key);
const gchar *bar_tvremote_lg_key          (BarTvremoteKey key);
const gchar *bar_tvremote_catt_command    (BarTvremoteKey key);
const gchar *bar_tvremote_atv_command     (BarTvremoteKey key);

/* --- adb ----------------------------------------------------------- */

/**
 * BarTvremoteAdbState:
 *
 * What `adb get-state' (and `adb connect') says about a device.
 */
typedef enum {
	BAR_TVREMOTE_ADB_ABSENT = 0,     /* not connected at all */
	BAR_TVREMOTE_ADB_OFFLINE,        /* connected, not answering */
	BAR_TVREMOTE_ADB_UNAUTHORIZED,   /* waiting for "Allow USB debugging" */
	BAR_TVREMOTE_ADB_AUTHORIZING,    /* adb is still handshaking */
	BAR_TVREMOTE_ADB_DEVICE,         /* ready */
	BAR_TVREMOTE_ADB_REFUSED         /* nothing listening at the address */
} BarTvremoteAdbState;

BarTvremoteAdbState bar_tvremote_parse_adb_state   (const gchar *out,
                                                    const gchar *err);
BarTvremoteAdbState bar_tvremote_parse_adb_connect (const gchar *out,
                                                    const gchar *err);
gchar  *bar_tvremote_adb_serial        (const gchar *host, gint port);
gchar **bar_tvremote_adb_text_commands (const gchar *text, guint *dropped);
gchar  *bar_tvremote_adb_key_command   (BarTvremoteKind kind,
                                        BarTvremoteKey  key,
                                        gboolean        longpress);
gboolean bar_tvremote_valid_package    (const gchar *pkg);
gchar  *bar_tvremote_adb_launch_command (const gchar *pkg,
                                         gboolean     leanback);
gchar  *bar_tvremote_parse_focus       (const gchar *dumpsys);
GPtrArray *bar_tvremote_parse_pm_list  (const gchar *out);
gboolean bar_tvremote_parse_audio_volume (const gchar *dumpsys,
                                          gint        *index,
                                          gint        *max,
                                          gboolean    *muted);
gchar  *bar_tvremote_parse_media_title (const gchar *dumpsys);
const gchar *bar_tvremote_app_friendly_name (BarTvremoteKind kind,
                                             const gchar    *pkg);
gboolean bar_tvremote_app_is_curated   (const gchar *pkg);
GPtrArray *bar_tvremote_parse_mdns     (const gchar *out);

/* --- Apps ---------------------------------------------------------- */

/**
 * BarTvremoteApp:
 * @id: what launches it (a package, a Roku channel id, an LG app id)
 * @name: what the panel shows
 */
typedef struct {
	gchar *id;
	gchar *name;
} BarTvremoteApp;

BarTvremoteApp *bar_tvremote_app_new  (const gchar *id, const gchar *name);
void            bar_tvremote_app_free (BarTvremoteApp *app);
GPtrArray      *bar_tvremote_adb_apps (BarTvremoteKind kind,
                                       const gchar    *pm_third_party,
                                       const gchar    *pm_all);

/* --- Roku (ECP) ---------------------------------------------------- */

gchar     *bar_tvremote_urlencode       (const gchar *text);
gboolean   bar_tvremote_valid_host      (const gchar *host);
gchar     *bar_tvremote_http_url        (const gchar *host, gint port,
                                         const gchar *path);
gchar    **bar_tvremote_roku_text_paths (const gchar *text);
GPtrArray *bar_tvremote_roku_parse_apps (const gchar *xml);
gchar     *bar_tvremote_roku_parse_active (const gchar *xml);
gchar     *bar_tvremote_roku_parse_field  (const gchar *xml,
                                           const gchar *element);
gboolean   bar_tvremote_valid_app_id      (const gchar *id);

/* --- Discovery ----------------------------------------------------- */

gchar   *bar_tvremote_ssdp_request (const gchar *st);
gboolean bar_tvremote_ssdp_parse   (const gchar *response,
                                    gchar      **location,
                                    gchar      **st,
                                    gchar      **server);
BarTvremoteKind bar_tvremote_ssdp_kind (const gchar *st,
                                        const gchar *server);
gboolean bar_tvremote_url_host_port (const gchar *url,
                                     gchar      **host,
                                     gint        *port);
gchar   *bar_tvremote_route_iface  (const gchar *proc_net_route);
gboolean bar_tvremote_parse_cidr   (const gchar *cidr,
                                    guint32     *base,
                                    guint       *prefix);
gchar   *bar_tvremote_subnet_for   (guint32 addr, guint32 mask);
GPtrArray *bar_tvremote_subnet_hosts (const gchar *cidr);
BarTvremoteKind bar_tvremote_kind_for_port (gint port);

/* --- The device list ------------------------------------------------ */

/**
 * BarTvremoteDevice:
 * @id: a stable short name, unique in the list
 * @name: what the panel shows
 * @kind: the protocol
 * @host: an address or host name
 * @port: 0 for the kind's default
 * @token: (nullable): Samsung's pairing token; a secret
 * @client_key: (nullable): LG's pairing key; a secret
 * @volume_via: (nullable): the id of the device volume goes to
 */
typedef struct {
	gchar           *id;
	gchar           *name;
	BarTvremoteKind  kind;
	gchar           *host;
	gint             port;
	gchar           *token;
	gchar           *client_key;
	gchar           *volume_via;
} BarTvremoteDevice;

BarTvremoteDevice *bar_tvremote_device_new  (const gchar    *id,
                                             BarTvremoteKind kind,
                                             const gchar    *host);
BarTvremoteDevice *bar_tvremote_device_copy (const BarTvremoteDevice *dev);
void               bar_tvremote_device_free (BarTvremoteDevice *dev);
gint               bar_tvremote_device_port (const BarTvremoteDevice *dev);
BarTvremoteDevice *bar_tvremote_device_find (GPtrArray   *devices,
                                             const gchar *id);
gchar *bar_tvremote_device_make_id (GPtrArray      *devices,
                                    BarTvremoteKind kind,
                                    const gchar    *host);
const BarTvremoteDevice *bar_tvremote_route_volume (GPtrArray   *devices,
                                                    const gchar *id,
                                                    BarTvremoteKey key);

/**
 * BarTvremoteConfigError:
 * @BAR_TVREMOTE_CONFIG_ERROR_PARSE: not YAML, or not the expected shape
 * @BAR_TVREMOTE_CONFIG_ERROR_MODE: readable by group or others
 * @BAR_TVREMOTE_CONFIG_ERROR_SIZE: implausibly large
 */
typedef enum {
	BAR_TVREMOTE_CONFIG_ERROR_PARSE,
	BAR_TVREMOTE_CONFIG_ERROR_MODE,
	BAR_TVREMOTE_CONFIG_ERROR_SIZE
} BarTvremoteConfigError;

#define BAR_TVREMOTE_CONFIG_ERROR (bar_tvremote_config_error_quark())
GQuark bar_tvremote_config_error_quark (void);

GPtrArray *bar_tvremote_config_parse   (const gchar *text,
                                        gchar      **default_id,
                                        GError     **error);
gchar     *bar_tvremote_config_to_yaml (GPtrArray   *devices,
                                        const gchar *default_id);
GPtrArray *bar_tvremote_config_load    (const gchar *path,
                                        gchar      **default_id,
                                        GError     **error);
gboolean   bar_tvremote_config_save    (const gchar *path,
                                        GPtrArray   *devices,
                                        const gchar *default_id,
                                        GError     **error);

/* --- The remote's buttons ----------------------------------------- */

/**
 * BarTvremoteButton:
 * @key: what it sends
 * @x: left edge, canvas pixels
 * @y: top edge
 * @w: width
 * @h: height
 * @round: drawn as a circle (the D-pad's centre)
 *
 * One button of the on-screen remote.  The SAME rectangles are what
 * the canvas draws and what a click is tested against, so a button can
 * never be drawn in one place and answer in another.
 */
typedef struct {
	BarTvremoteKey key;
	gdouble        x;
	gdouble        y;
	gdouble        w;
	gdouble        h;
	gboolean       round;
} BarTvremoteButton;

#define BAR_TVREMOTE_MAX_BUTTONS (24)

gint  bar_tvremote_canvas_height (BarTvremoteKind kind);
gboolean bar_tvremote_has_dpad   (BarTvremoteKind kind);
guint bar_tvremote_layout (BarTvremoteKind    kind,
                           gint               width,
                           gint               height,
                           BarTvremoteButton *out,
                           gdouble           *ring_cx,
                           gdouble           *ring_cy,
                           gdouble           *ring_r);
gint  bar_tvremote_hit    (BarTvremoteKind kind,
                           gint            width,
                           gint            height,
                           gdouble         x,
                           gdouble         y);

/* --- The bar label ------------------------------------------------- */

/**
 * BarTvremoteLink:
 *
 * How the widget is getting on with a device, which is what decides
 * the bar's colour.
 */
typedef enum {
	BAR_TVREMOTE_LINK_UNKNOWN = 0,
	BAR_TVREMOTE_LINK_OFFLINE,
	BAR_TVREMOTE_LINK_CONNECTING,
	BAR_TVREMOTE_LINK_PAIRING,
	BAR_TVREMOTE_LINK_ONLINE,
	BAR_TVREMOTE_LINK_NO_TOOL
} BarTvremoteLink;

gchar *bar_tvremote_format_label (BarTvremoteLink  link,
                                  const gchar     *app,
                                  gint             volume,
                                  gboolean         muted);
gint   bar_tvremote_volume_percent (gint index, gint max);
gboolean bar_tvremote_parse_hex (const gchar *spec, gdouble rgb[3]);
gchar *bar_tvremote_clean_text (const gchar *text, gsize max_bytes);
gchar *bar_tvremote_json_escape (const gchar *text);

/* --- Reading the JSON the websocket TVs send (bar-tvremote-json.c) -- */

gchar     *bar_tvremote_json_string (const gchar *json,
                                     const gchar *first,
                                     ...) G_GNUC_NULL_TERMINATED;
gint64     bar_tvremote_json_int    (const gchar *json,
                                     gint64       fallback,
                                     const gchar *first,
                                     ...) G_GNUC_NULL_TERMINATED;
gint       bar_tvremote_json_bool   (const gchar *json,
                                     const gchar *first,
                                     ...) G_GNUC_NULL_TERMINATED;
GPtrArray *bar_tvremote_json_apps   (const gchar *json,
                                     const gchar *id_key,
                                     const gchar *name_key,
                                     const gchar *first,
                                     ...) G_GNUC_NULL_TERMINATED;

G_END_DECLS

#endif /* BAR_TVREMOTE_MODEL_H */
