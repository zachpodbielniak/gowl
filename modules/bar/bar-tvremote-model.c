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
 * The tvremote widget's pure helpers.  See bar-tvremote-model.h for
 * why they are kept apart from the plugin.
 */

#include <string.h>

#include "bar-tvremote-model.h"

/* ----------------------------------------------------------------
 * Kinds
 * ---------------------------------------------------------------- */

typedef struct {
	const gchar *name;
	const gchar *title;
	gint         port;
} KindInfo;

/* Indexed by BarTvremoteKind.  Samsung and LG default to their TLS
   ports: current firmware on both refuses the plain-text ones, and a
   default that only works on a five-year-old set is not a default. */
static const KindInfo kind_info[] = {
	{ "firetv",    "Fire TV",    5555 },
	{ "androidtv", "Android TV", 5555 },
	{ "roku",      "Roku",       8060 },
	{ "samsung",   "Samsung",    8002 },
	{ "lg",        "LG webOS",   3001 },
	{ "cast",      "Chromecast", 0    },
	{ "appletv",   "Apple TV",   0    }
};

/**
 * bar_tvremote_kind_name:
 * @kind: a kind
 *
 * Returns: (transfer none): the name the config file uses, or
 *   "unknown"
 */
const gchar *
bar_tvremote_kind_name(BarTvremoteKind kind)
{
	if ((guint)kind >= BAR_TVREMOTE_N_KINDS)
		return "unknown";
	return kind_info[kind].name;
}

/**
 * bar_tvremote_kind_title:
 * @kind: a kind
 *
 * Returns: (transfer none): the human name of @kind
 */
const gchar *
bar_tvremote_kind_title(BarTvremoteKind kind)
{
	if ((guint)kind >= BAR_TVREMOTE_N_KINDS)
		return "Unknown";
	return kind_info[kind].title;
}

/**
 * bar_tvremote_kind_from_name:
 * @name: (nullable): a kind name, as written in the config
 *
 * Accepts the canonical names and the spellings people actually type
 * (`fire-tv', `googletv', `webos', `chromecast').
 *
 * Returns: the kind, or %BAR_TVREMOTE_KIND_UNKNOWN
 */
BarTvremoteKind
bar_tvremote_kind_from_name(const gchar *name)
{
	static const struct { const gchar *alias; BarTvremoteKind kind; } aliases[] = {
		{ "fire-tv", BAR_TVREMOTE_KIND_FIRETV },
		{ "fire", BAR_TVREMOTE_KIND_FIRETV },
		{ "android-tv", BAR_TVREMOTE_KIND_ANDROIDTV },
		{ "googletv", BAR_TVREMOTE_KIND_ANDROIDTV },
		{ "google-tv", BAR_TVREMOTE_KIND_ANDROIDTV },
		{ "tizen", BAR_TVREMOTE_KIND_SAMSUNG },
		{ "webos", BAR_TVREMOTE_KIND_LG },
		{ "chromecast", BAR_TVREMOTE_KIND_CAST },
		{ "apple-tv", BAR_TVREMOTE_KIND_APPLETV },
		{ "atv", BAR_TVREMOTE_KIND_APPLETV }
	};
	guint i;

	if (name == NULL)
		return BAR_TVREMOTE_KIND_UNKNOWN;
	for (i = 0; i < BAR_TVREMOTE_N_KINDS; i++)
		if (g_ascii_strcasecmp(name, kind_info[i].name) == 0)
			return (BarTvremoteKind)i;
	for (i = 0; i < G_N_ELEMENTS(aliases); i++)
		if (g_ascii_strcasecmp(name, aliases[i].alias) == 0)
			return aliases[i].kind;
	return BAR_TVREMOTE_KIND_UNKNOWN;
}

/**
 * bar_tvremote_kind_default_port:
 * @kind: a kind
 *
 * Returns: the port a device of @kind listens on, 0 for the CLI kinds
 */
gint
bar_tvremote_kind_default_port(BarTvremoteKind kind)
{
	if ((guint)kind >= BAR_TVREMOTE_N_KINDS)
		return 0;
	return kind_info[kind].port;
}

/**
 * bar_tvremote_kind_uses_adb:
 * @kind: a kind
 *
 * Returns: %TRUE for the two kinds driven through `adb'
 */
gboolean
bar_tvremote_kind_uses_adb(BarTvremoteKind kind)
{
	return kind == BAR_TVREMOTE_KIND_FIRETV ||
	       kind == BAR_TVREMOTE_KIND_ANDROIDTV;
}

/* ----------------------------------------------------------------
 * Keys
 * ---------------------------------------------------------------- */

/* Indexed by BarTvremoteKey: the action name (stable; the panel's ids
   and the docs use it) and the button caption. */
static const struct { const gchar *name; const gchar *label; } key_info[] = {
	{ "up",           "\xe2\x96\xb2" },          /* ▲ */
	{ "down",         "\xe2\x96\xbc" },          /* ▼ */
	{ "left",         "\xe2\x97\x80" },          /* ◀ */
	{ "right",        "\xe2\x96\xb6" },          /* ▶ */
	{ "select",       "OK" },
	{ "back",         "Back" },
	{ "home",         "Home" },
	{ "menu",         "Menu" },
	{ "play_pause",   "\xe2\x96\xb6\xe2\x9d\x9a\xe2\x9d\x9a" }, /* ▶❚❚ */
	{ "rewind",       "\xe2\x97\x80\xe2\x97\x80" },             /* ◀◀ */
	{ "fast_forward", "\xe2\x96\xb6\xe2\x96\xb6" },             /* ▶▶ */
	{ "vol_up",       "Vol +" },
	{ "vol_down",     "Vol \xe2\x88\x92" },      /* Vol − */
	{ "mute",         "Mute" },
	{ "power",        "Power" },
	{ "sleep",        "Sleep" },
	{ "wakeup",       "Wake" },
	{ "search",       "Search" },
	{ "settings",     "Settings" },
	{ "input",        "Input" }
};

G_STATIC_ASSERT(G_N_ELEMENTS(key_info) == BAR_TVREMOTE_N_KEYS);

/**
 * bar_tvremote_key_name:
 * @key: a key
 *
 * Returns: (transfer none): the key's action name, e.g. "play_pause"
 */
const gchar *
bar_tvremote_key_name(BarTvremoteKey key)
{
	if ((guint)key >= BAR_TVREMOTE_N_KEYS)
		return "none";
	return key_info[key].name;
}

/**
 * bar_tvremote_key_from_name:
 * @name: (nullable): an action name
 *
 * Returns: the #BarTvremoteKey, or -1
 */
gint
bar_tvremote_key_from_name(const gchar *name)
{
	guint i;

	if (name == NULL)
		return -1;
	for (i = 0; i < BAR_TVREMOTE_N_KEYS; i++)
		if (strcmp(name, key_info[i].name) == 0)
			return (gint)i;
	return -1;
}

/**
 * bar_tvremote_key_label:
 * @key: a key
 *
 * Returns: (transfer none): the caption drawn on the button
 */
const gchar *
bar_tvremote_key_label(BarTvremoteKey key)
{
	if ((guint)key >= BAR_TVREMOTE_N_KEYS)
		return "?";
	return key_info[key].label;
}

/**
 * bar_tvremote_key_is_volume:
 * @key: a key
 *
 * Returns: %TRUE for the three keys a `volume-via' device takes over
 */
gboolean
bar_tvremote_key_is_volume(BarTvremoteKey key)
{
	return key == BAR_TVREMOTE_KEY_VOL_UP ||
	       key == BAR_TVREMOTE_KEY_VOL_DOWN ||
	       key == BAR_TVREMOTE_KEY_MUTE;
}

/**
 * bar_tvremote_android_keycode:
 * @kind: %BAR_TVREMOTE_KIND_FIRETV or %BAR_TVREMOTE_KIND_ANDROIDTV
 * @key: a key
 *
 * The Android KEYCODE_* for @key.  The two adb kinds share almost all
 * of them; Fire OS has no settings or input-source key a shell can
 * press, Android TV has both.
 *
 * Returns: the key code, or -1 when @kind has no such key
 */
gint
bar_tvremote_android_keycode(BarTvremoteKind kind, BarTvremoteKey key)
{
	static const gint codes[BAR_TVREMOTE_N_KEYS] = {
		19,   /* UP: KEYCODE_DPAD_UP */
		20,   /* DOWN */
		21,   /* LEFT */
		22,   /* RIGHT */
		23,   /* SELECT: KEYCODE_DPAD_CENTER */
		4,    /* BACK */
		3,    /* HOME */
		82,   /* MENU: the hamburger button */
		85,   /* PLAY_PAUSE: KEYCODE_MEDIA_PLAY_PAUSE */
		89,   /* REWIND */
		90,   /* FAST_FORWARD */
		24,   /* VOL_UP */
		25,   /* VOL_DOWN */
		164,  /* MUTE: KEYCODE_VOLUME_MUTE */
		26,   /* POWER: toggles sleep */
		223,  /* SLEEP */
		224,  /* WAKEUP */
		84,   /* SEARCH */
		176,  /* SETTINGS */
		178   /* INPUT: KEYCODE_TV_INPUT */
	};

	if ((guint)key >= BAR_TVREMOTE_N_KEYS)
		return -1;
	if (kind == BAR_TVREMOTE_KIND_FIRETV &&
	    (key == BAR_TVREMOTE_KEY_SETTINGS || key == BAR_TVREMOTE_KEY_INPUT))
		return -1;
	if (!bar_tvremote_kind_uses_adb(kind))
		return -1;
	return codes[key];
}

/**
 * bar_tvremote_roku_key:
 * @key: a key
 *
 * Returns: (transfer none) (nullable): the ECP key name
 */
const gchar *
bar_tvremote_roku_key(BarTvremoteKey key)
{
	static const gchar *names[BAR_TVREMOTE_N_KEYS] = {
		"Up", "Down", "Left", "Right", "Select", "Back", "Home",
		"Info",          /* the `*' options button */
		"Play",          /* ECP's Play toggles */
		"Rev", "Fwd",
		"VolumeUp", "VolumeDown", "VolumeMute",
		"Power", "PowerOff", "PowerOn",
		"Search",
		NULL, NULL
	};

	if ((guint)key >= BAR_TVREMOTE_N_KEYS)
		return NULL;
	return names[key];
}

/**
 * bar_tvremote_samsung_key:
 * @key: a key
 *
 * Tizen has no play/pause TOGGLE; the backend alternates KEY_PAUSE and
 * KEY_PLAY for %BAR_TVREMOTE_KEY_PLAY_PAUSE, and this returns the
 * first of the two.
 *
 * Returns: (transfer none) (nullable): the KEY_* name
 */
const gchar *
bar_tvremote_samsung_key(BarTvremoteKey key)
{
	static const gchar *names[BAR_TVREMOTE_N_KEYS] = {
		"KEY_UP", "KEY_DOWN", "KEY_LEFT", "KEY_RIGHT", "KEY_ENTER",
		"KEY_RETURN", "KEY_HOME", "KEY_MENU",
		"KEY_PAUSE",
		"KEY_REWIND", "KEY_FF",
		"KEY_VOLUP", "KEY_VOLDOWN", "KEY_MUTE",
		"KEY_POWER", "KEY_POWEROFF",
		NULL,            /* waking needs Wake-on-LAN, not a key */
		NULL,
		NULL,
		"KEY_SOURCE"
	};

	if ((guint)key >= BAR_TVREMOTE_N_KEYS)
		return NULL;
	return names[key];
}

/**
 * bar_tvremote_lg_key:
 * @key: a key
 *
 * LG splits its remote in two: navigation and media buttons go down a
 * second "pointer input" socket as `button' messages, while volume and
 * power are SSAP requests on the main one.  The prefix says which:
 * `btn:NAME' or `ssap:URI'.
 *
 * Returns: (transfer none) (nullable): the encoded command
 */
const gchar *
bar_tvremote_lg_key(BarTvremoteKey key)
{
	static const gchar *names[BAR_TVREMOTE_N_KEYS] = {
		"btn:UP", "btn:DOWN", "btn:LEFT", "btn:RIGHT", "btn:ENTER",
		"btn:BACK", "btn:HOME", "btn:MENU",
		"btn:PLAY",                  /* alternated with PAUSE */
		"btn:REWIND", "btn:FASTFORWARD",
		"ssap:ssap://audio/volumeUp",
		"ssap:ssap://audio/volumeDown",
		"ssap:ssap://audio/setMute",
		"ssap:ssap://system/turnOff",
		"ssap:ssap://system/turnOff",
		NULL,
		NULL,
		NULL,
		NULL
	};

	if ((guint)key >= BAR_TVREMOTE_N_KEYS)
		return NULL;
	return names[key];
}

/**
 * bar_tvremote_catt_command:
 * @key: a key
 *
 * Returns: (transfer none) (nullable): the `catt' subcommand
 */
const gchar *
bar_tvremote_catt_command(BarTvremoteKey key)
{
	switch (key) {
	case BAR_TVREMOTE_KEY_PLAY_PAUSE:   return "play_toggle";
	case BAR_TVREMOTE_KEY_REWIND:       return "rewind";
	case BAR_TVREMOTE_KEY_FAST_FORWARD: return "ffwd";
	case BAR_TVREMOTE_KEY_VOL_UP:       return "volumeup";
	case BAR_TVREMOTE_KEY_VOL_DOWN:     return "volumedown";
	case BAR_TVREMOTE_KEY_MUTE:         return "volumemute";
	case BAR_TVREMOTE_KEY_POWER:        return "stop";
	default:                            return NULL;
	}
}

/**
 * bar_tvremote_atv_command:
 * @key: a key
 *
 * Returns: (transfer none) (nullable): the `atvremote' command
 */
const gchar *
bar_tvremote_atv_command(BarTvremoteKey key)
{
	switch (key) {
	case BAR_TVREMOTE_KEY_UP:           return "up";
	case BAR_TVREMOTE_KEY_DOWN:         return "down";
	case BAR_TVREMOTE_KEY_LEFT:         return "left";
	case BAR_TVREMOTE_KEY_RIGHT:        return "right";
	case BAR_TVREMOTE_KEY_SELECT:       return "select";
	case BAR_TVREMOTE_KEY_BACK:         return "menu";
	case BAR_TVREMOTE_KEY_HOME:         return "home";
	case BAR_TVREMOTE_KEY_MENU:         return "top_menu";
	case BAR_TVREMOTE_KEY_PLAY_PAUSE:   return "play_pause";
	case BAR_TVREMOTE_KEY_REWIND:       return "skip_backward";
	case BAR_TVREMOTE_KEY_FAST_FORWARD: return "skip_forward";
	case BAR_TVREMOTE_KEY_VOL_UP:       return "volume_up";
	case BAR_TVREMOTE_KEY_VOL_DOWN:     return "volume_down";
	case BAR_TVREMOTE_KEY_POWER:        return "turn_off";
	case BAR_TVREMOTE_KEY_SLEEP:        return "turn_off";
	case BAR_TVREMOTE_KEY_WAKEUP:       return "turn_on";
	default:                            return NULL;
	}
}

/**
 * bar_tvremote_kind_supports:
 * @kind: a kind
 * @key: a key
 *
 * Returns: %TRUE when a device of @kind has a way to press @key
 */
gboolean
bar_tvremote_kind_supports(BarTvremoteKind kind, BarTvremoteKey key)
{
	switch (kind) {
	case BAR_TVREMOTE_KIND_FIRETV:
	case BAR_TVREMOTE_KIND_ANDROIDTV:
		return bar_tvremote_android_keycode(kind, key) >= 0;
	case BAR_TVREMOTE_KIND_ROKU:
		return bar_tvremote_roku_key(key) != NULL;
	case BAR_TVREMOTE_KIND_SAMSUNG:
		return bar_tvremote_samsung_key(key) != NULL;
	case BAR_TVREMOTE_KIND_LG:
		return bar_tvremote_lg_key(key) != NULL;
	case BAR_TVREMOTE_KIND_CAST:
		return bar_tvremote_catt_command(key) != NULL;
	case BAR_TVREMOTE_KIND_APPLETV:
		return bar_tvremote_atv_command(key) != NULL;
	case BAR_TVREMOTE_KIND_UNKNOWN:
	default:
		return FALSE;
	}
}

/* ----------------------------------------------------------------
 * adb
 * ---------------------------------------------------------------- */

/**
 * bar_tvremote_parse_adb_state:
 * @out: (nullable): `adb -s SERIAL get-state' standard output
 * @err: (nullable): its standard error
 *
 * get-state prints `device' on stdout when all is well and an error on
 * stderr otherwise -- `error: device unauthorized.' while the TV's
 * "Allow USB debugging?" dialog is up, `device offline', `device
 * still authorizing', or `device 'x' not found'.  Both streams are
 * read because which one carries the word changed between adb
 * releases.
 *
 * Returns: the state
 */
BarTvremoteAdbState
bar_tvremote_parse_adb_state(const gchar *out, const gchar *err)
{
	g_autofree gchar *all = NULL;
	g_autofree gchar *first = NULL;
	gchar *nl;

	all = g_strdup_printf("%s\n%s", out != NULL ? out : "",
	                      err != NULL ? err : "");
	if (strstr(all, "unauthorized") != NULL)
		return BAR_TVREMOTE_ADB_UNAUTHORIZED;
	if (strstr(all, "authorizing") != NULL)
		return BAR_TVREMOTE_ADB_AUTHORIZING;
	if (strstr(all, "offline") != NULL)
		return BAR_TVREMOTE_ADB_OFFLINE;

	/* `device' must be the whole first line of stdout: the error
	   text `device 'x' not found' contains the word too. */
	first = g_strdup(out != NULL ? out : "");
	nl = strchr(first, '\n');
	if (nl != NULL)
		*nl = '\0';
	g_strstrip(first);
	if (strcmp(first, "device") == 0)
		return BAR_TVREMOTE_ADB_DEVICE;
	return BAR_TVREMOTE_ADB_ABSENT;
}

/**
 * bar_tvremote_parse_adb_connect:
 * @out: (nullable): `adb connect HOST:PORT' standard output
 * @err: (nullable): its standard error
 *
 * adb reports a refused connection on stdout and exits 0 (`failed to
 * connect to ...: Connection refused'), so the exit status alone says
 * nothing.  A connection to a set whose prompt is not yet accepted
 * reads `failed to authenticate to ...' on current adb.
 *
 * Returns: %BAR_TVREMOTE_ADB_DEVICE when connected (or already),
 *   %BAR_TVREMOTE_ADB_UNAUTHORIZED when waiting for the prompt,
 *   %BAR_TVREMOTE_ADB_REFUSED when nothing answered
 */
BarTvremoteAdbState
bar_tvremote_parse_adb_connect(const gchar *out, const gchar *err)
{
	g_autofree gchar *all = NULL;

	all = g_strdup_printf("%s\n%s", out != NULL ? out : "",
	                      err != NULL ? err : "");
	if (strstr(all, "authenticate") != NULL ||
	    strstr(all, "unauthorized") != NULL)
		return BAR_TVREMOTE_ADB_UNAUTHORIZED;
	if (strstr(all, "failed") != NULL || strstr(all, "unable") != NULL ||
	    strstr(all, "cannot") != NULL || strstr(all, "error") != NULL)
		return BAR_TVREMOTE_ADB_REFUSED;
	if (strstr(all, "connected to") != NULL)
		return BAR_TVREMOTE_ADB_DEVICE;
	return BAR_TVREMOTE_ADB_REFUSED;
}

/**
 * bar_tvremote_adb_serial:
 * @host: an address
 * @port: a port, or 0 for 5555
 *
 * Returns: (transfer full): the `-s' serial adb knows the device by
 */
gchar *
bar_tvremote_adb_serial(const gchar *host, gint port)
{
	if (port <= 0)
		port = 5555;
	/* adb writes IPv6 serials bracketed, like a URL. */
	if (strchr(host, ':') != NULL && host[0] != '[')
		return g_strdup_printf("[%s]:%d", host, port);
	return g_strdup_printf("%s:%d", host, port);
}

/* Append @text as one single-quoted argument for the device's shell.
   Inside single quotes nothing is special except the quote itself,
   which is written as '\'' (close, escaped quote, reopen). */
static void
device_shell_quote(GString *out, const gchar *text)
{
	const gchar *p;

	g_string_append_c(out, '\'');
	for (p = text; *p != '\0'; p++) {
		if (*p == '\'')
			g_string_append(out, "'\\''");
		else
			g_string_append_c(out, *p);
	}
	g_string_append_c(out, '\'');
}

/* Close the argument being built into a full `input text' line. */
static void
flush_text_line(GPtrArray *lines, GString *arg)
{
	GString *line;

	if (arg->len == 0)
		return;
	line = g_string_new("input text ");
	device_shell_quote(line, arg->str);
	g_ptr_array_add(lines, g_string_free(line, FALSE));
	g_string_truncate(arg, 0);
}

/**
 * bar_tvremote_adb_text_commands:
 * @text: what the user typed
 * @dropped: (out) (optional): how many characters could not be sent
 *
 * Turns typed text into `input text' lines for the persistent device
 * shell.  Three layers each need their own escaping, and getting any
 * one wrong either loses characters or runs them as commands:
 *
 *   1. The DEVICE SHELL parses the line.  The argument is single
 *      quoted, which leaves `$', a backtick, `;', `&', `|', `"', `\'
 *      and every other metacharacter literal; a single quote is
 *      spelled '\''.  A newline would end the command and start a new
 *      one, so control characters never get here at all.
 *   2. `input text' itself reads `%s' as a SPACE -- the documented way
 *      to type one, from when a space split the argument.  Spaces are
 *      sent that way.  It has no escape for a literal `%s', so a `%'
 *      followed by `s' ends one command after the `%' and the `s'
 *      starts the next.
 *   3. Android types through the virtual keyboard's key map, which
 *      has no keys for anything outside ASCII: those characters are
 *      dropped and counted, so the panel can say so rather than the
 *      TV silently typing half a word.
 *
 * At most 500 characters are taken.
 *
 * Returns: (transfer full) (array zero-terminated=1): the lines, in
 *   order; empty when there is nothing sendable
 */
gchar **
bar_tvremote_adb_text_commands(const gchar *text, guint *dropped)
{
	GPtrArray *lines;
	GString *arg;
	const gchar *p;
	guint n_dropped = 0;
	guint taken = 0;

	lines = g_ptr_array_new();
	arg = g_string_new(NULL);

	if (text != NULL && g_utf8_validate(text, -1, NULL)) {
		for (p = text; *p != '\0' && taken < 500;
		     p = g_utf8_next_char(p), taken++) {
			gunichar c = g_utf8_get_char(p);

			if (c >= 0x80 || c < 0x20 || c == 0x7f) {
				n_dropped++;
				continue;
			}
			if (c == ' ') {
				g_string_append(arg, "%s");
				continue;
			}
			g_string_append_c(arg, (gchar)c);
			if (c == '%' && p[1] == 's')
				flush_text_line(lines, arg);
		}
	} else if (text != NULL) {
		/* Not UTF-8: refuse it whole rather than guess. */
		n_dropped = (guint)strlen(text);
	}
	flush_text_line(lines, arg);
	g_string_free(arg, TRUE);
	g_ptr_array_add(lines, NULL);

	if (dropped != NULL)
		*dropped = n_dropped;
	return (gchar **)g_ptr_array_free(lines, FALSE);
}

/**
 * bar_tvremote_adb_key_command:
 * @kind: an adb kind
 * @key: a key
 * @longpress: hold rather than tap
 *
 * Returns: (transfer full) (nullable): the shell line, or %NULL when
 *   @kind has no such key
 */
gchar *
bar_tvremote_adb_key_command(BarTvremoteKind kind, BarTvremoteKey key,
                             gboolean longpress)
{
	gint code;

	code = bar_tvremote_android_keycode(kind, key);
	if (code < 0)
		return NULL;
	return g_strdup_printf("input keyevent %s%d",
	                       longpress ? "--longpress " : "", code);
}

/**
 * bar_tvremote_valid_package:
 * @pkg: (nullable): a string
 *
 * An Android package name: dotted segments of letters, digits and
 * underscores, starting with a letter.  Everything that becomes part
 * of a device shell line from outside goes through this, so a hostile
 * `pm list' line or app id can never carry a `;'.
 *
 * Returns: %TRUE when @pkg is one
 */
gboolean
bar_tvremote_valid_package(const gchar *pkg)
{
	const gchar *p;
	gboolean dot = FALSE;
	gboolean seg_start = TRUE;

	if (pkg == NULL || !g_ascii_isalpha(pkg[0]) || strlen(pkg) > 255)
		return FALSE;
	for (p = pkg; *p != '\0'; p++) {
		if (*p == '.') {
			if (seg_start)
				return FALSE;
			dot = TRUE;
			seg_start = TRUE;
			continue;
		}
		if (!g_ascii_isalnum(*p) && *p != '_')
			return FALSE;
		seg_start = FALSE;
	}
	return dot && !seg_start;
}

/**
 * bar_tvremote_adb_launch_command:
 * @pkg: a package
 * @leanback: ask for the TV launcher activity
 *
 * `monkey' launches a package's launcher activity without knowing its
 * name.  A TV app declares LEANBACK_LAUNCHER rather than LAUNCHER, so
 * that category is asked for first; a phone app side-loaded onto a
 * Fire TV has only the plain one, which is what the fallback is for.
 *
 * Returns: (transfer full) (nullable): the shell line, or %NULL for an
 *   invalid package
 */
gchar *
bar_tvremote_adb_launch_command(const gchar *pkg, gboolean leanback)
{
	if (!bar_tvremote_valid_package(pkg))
		return NULL;
	if (leanback)
		return g_strdup_printf("monkey -p %s -c "
		                       "android.intent.category.LEANBACK_LAUNCHER 1",
		                       pkg);
	return g_strdup_printf("monkey -p %s 1", pkg);
}

/* The package out of the first `pkg/activity' token on @line. */
static gchar *
component_package(const gchar *line)
{
	g_auto(GStrv) tokens = NULL;
	guint i;

	tokens = g_strsplit_set(line, " \t{}=", -1);
	for (i = 0; tokens[i] != NULL; i++) {
		gchar *slash = strchr(tokens[i], '/');
		g_autofree gchar *pkg = NULL;

		if (slash == NULL || slash == tokens[i])
			continue;
		pkg = g_strndup(tokens[i], (gsize)(slash - tokens[i]));
		if (bar_tvremote_valid_package(pkg))
			return g_steal_pointer(&pkg);
	}
	return NULL;
}

/**
 * bar_tvremote_parse_focus:
 * @dumpsys: (nullable): the mCurrentFocus / mFocusedApp lines of
 *   `dumpsys window'
 *
 * Which app is in front.  mCurrentFocus names the focused WINDOW, and
 * is the better answer when it names an activity; but while a dialog,
 * the notification shade or a transition has focus it names no
 * package at all (`Window{.. u0 NotificationShade}', or `null'), and
 * then mFocusedApp -- the activity behind it -- is.  Fire OS 7 nests
 * that as `AppWindowToken{.. token=Token{.. ActivityRecord{.. u0
 * pkg/.Activity t12}}}', Fire OS 8 writes `ActivityRecord{.. u0
 * pkg/.Activity t12}' directly; both are found by looking for the
 * first `pkg/activity' token rather than by position.
 *
 * Returns: (transfer full) (nullable): the package, or %NULL
 */
gchar *
bar_tvremote_parse_focus(const gchar *dumpsys)
{
	static const gchar *keys[] = {
		"mCurrentFocus", "mFocusedApp", "mResumedActivity",
		"topResumedActivity"
	};
	g_auto(GStrv) lines = NULL;
	guint k, i;

	if (dumpsys == NULL)
		return NULL;
	lines = g_strsplit(dumpsys, "\n", -1);
	for (k = 0; k < G_N_ELEMENTS(keys); k++) {
		for (i = 0; lines[i] != NULL; i++) {
			gchar *pkg;

			if (strstr(lines[i], keys[k]) == NULL)
				continue;
			pkg = component_package(lines[i]);
			if (pkg != NULL)
				return pkg;
		}
	}
	return NULL;
}

static gint
compare_strings(gconstpointer a, gconstpointer b)
{
	return strcmp(*(const gchar * const *)a, *(const gchar * const *)b);
}

/**
 * bar_tvremote_parse_pm_list:
 * @out: (nullable): `pm list packages' output
 *
 * Lines are `package:NAME', or `package:/path/base.apk=NAME' when `-f'
 * was given.  Anything that is not a valid package name is skipped.
 *
 * Returns: (transfer full) (element-type utf8): the packages, sorted,
 *   without duplicates
 */
GPtrArray *
bar_tvremote_parse_pm_list(const gchar *out)
{
	GPtrArray *pkgs;
	g_auto(GStrv) lines = NULL;
	guint i;

	pkgs = g_ptr_array_new_with_free_func(g_free);
	if (out == NULL)
		return pkgs;
	lines = g_strsplit(out, "\n", -1);
	for (i = 0; lines[i] != NULL; i++) {
		gchar *name, *eq;

		g_strstrip(lines[i]);
		if (!g_str_has_prefix(lines[i], "package:"))
			continue;
		name = lines[i] + strlen("package:");
		eq = strrchr(name, '=');
		if (eq != NULL)
			name = eq + 1;
		if (bar_tvremote_valid_package(name))
			g_ptr_array_add(pkgs, g_strdup(name));
	}
	g_ptr_array_sort(pkgs, compare_strings);
	for (i = 1; i < pkgs->len; i++) {
		if (strcmp(g_ptr_array_index(pkgs, i),
		           g_ptr_array_index(pkgs, i - 1)) == 0) {
			g_ptr_array_remove_index(pkgs, i);
			i--;
		}
	}
	return pkgs;
}

/* Parse "<number>" at @p; FALSE when there is none. */
static gboolean
parse_int_at(const gchar *p, gint *out)
{
	gchar *end = NULL;
	gint64 v;

	while (*p == ' ' || *p == '\t')
		p++;
	v = g_ascii_strtoll(p, &end, 10);
	if (end == p || v < 0 || v > 100000)
		return FALSE;
	*out = (gint)v;
	return TRUE;
}

/**
 * bar_tvremote_parse_audio_volume:
 * @dumpsys: (nullable): the STREAM_MUSIC section of `dumpsys audio'
 * @index: (out): the volume index
 * @max: (out): the largest index
 * @muted: (out): whether the stream is muted
 *
 * The section looks like
 *
 *   - STREAM_MUSIC:
 *      Muted: false
 *      Min: 0
 *      Max: 15
 *      streamVolume:9
 *      Current: 2 (speaker): 9, 400 (hdmi): 12, 40000000 (default): 15
 *      Devices: hdmi
 *
 * `streamVolume' is the answer when present (Android 11+).  Older
 * releases only list a per-output index on `Current', and the one
 * that matters is the output named on `Devices' -- a Fire TV Stick
 * plays through `hdmi', and the `default' entry beside it is usually
 * pinned at the maximum.
 *
 * Returns: %TRUE when an index and a maximum were both found
 */
gboolean
bar_tvremote_parse_audio_volume(const gchar *dumpsys, gint *index,
                                gint *max, gboolean *muted)
{
	g_auto(GStrv) lines = NULL;
	g_autofree gchar *current = NULL;
	g_autofree gchar *device = NULL;
	gboolean in_music = FALSE;
	gboolean have_index = FALSE;
	gboolean have_max = FALSE;
	guint i;

	*index = -1;
	*max = -1;
	*muted = FALSE;
	if (dumpsys == NULL)
		return FALSE;

	lines = g_strsplit(dumpsys, "\n", -1);
	for (i = 0; lines[i] != NULL; i++) {
		gchar *l = g_strstrip(lines[i]);

		if (g_str_has_prefix(l, "- STREAM_")) {
			if (in_music)
				break;
			in_music = g_str_has_prefix(l, "- STREAM_MUSIC");
			continue;
		}
		if (!in_music)
			continue;
		if (g_str_has_prefix(l, "Muted:"))
			*muted = (strstr(l, "true") != NULL);
		else if (g_str_has_prefix(l, "Max:"))
			have_max = parse_int_at(l + 4, max);
		else if (g_str_has_prefix(l, "streamVolume:"))
			have_index = parse_int_at(l + 13, index);
		else if (g_str_has_prefix(l, "Current:") && current == NULL)
			current = g_strdup(l + 8);
		else if (g_str_has_prefix(l, "Devices:") && device == NULL)
			device = g_strstrip(g_strdup(l + 8));
	}

	if (!have_index && current != NULL) {
		g_auto(GStrv) parts = g_strsplit(current, ",", -1);
		gint fallback = -1;
		guint j;

		/* "400 (hdmi): 12" -- the device name in parentheses. */
		for (j = 0; parts[j] != NULL; j++) {
			gchar *open = strchr(parts[j], '(');
			gchar *close = strchr(parts[j], ')');
			gchar *colon = strrchr(parts[j], ':');
			g_autofree gchar *name = NULL;
			gint v;

			if (open == NULL || close == NULL || colon == NULL ||
			    close < open || colon < close)
				continue;
			if (!parse_int_at(colon + 1, &v))
				continue;
			name = g_strndup(open + 1, (gsize)(close - open - 1));
			if (fallback < 0)
				fallback = v;
			if (device != NULL && g_strcmp0(name, device) == 0) {
				*index = v;
				have_index = TRUE;
				break;
			}
		}
		if (!have_index && fallback >= 0) {
			*index = fallback;
			have_index = TRUE;
		}
	}
	if (have_index && have_max && *index > *max)
		*index = *max;
	return have_index && have_max && *max > 0;
}

/**
 * bar_tvremote_parse_media_title:
 * @dumpsys: (nullable): `dumpsys media_session' output, or the
 *   `description=' lines of it
 *
 * A session's metadata reads `description=TITLE, SUBTITLE, EXTRA';
 * the first non-null one is what is playing.  A title containing a
 * comma is cut at it -- the line is not quoted, so that cannot be
 * told apart from the subtitle.
 *
 * Returns: (transfer full) (nullable): the title
 */
gchar *
bar_tvremote_parse_media_title(const gchar *dumpsys)
{
	const gchar *p;

	if (dumpsys == NULL)
		return NULL;
	for (p = strstr(dumpsys, "description="); p != NULL;
	     p = strstr(p + 1, "description=")) {
		const gchar *start = p + strlen("description=");
		const gchar *end = start;
		g_autofree gchar *title = NULL;

		while (*end != '\0' && *end != '\n' && *end != ',')
			end++;
		title = g_strstrip(g_strndup(start, (gsize)(end - start)));
		if (title[0] != '\0' && strcmp(title, "null") != 0)
			return bar_tvremote_clean_text(title, 120);
	}
	return NULL;
}

/* Packages worth a friendly name.  `launch' is FALSE for things the
   remote already has a button for -- the launcher is Home. */
static const struct {
	const gchar *pkg;
	const gchar *name;
	gboolean     launch;
} known_apps[] = {
	{ "com.netflix.ninja",                   "Netflix",      TRUE },
	{ "com.amazon.firebat",                  "Prime Video",  TRUE },
	{ "com.amazon.avod.thirdpartyclient",    "Prime Video",  TRUE },
	{ "com.amazon.avod",                     "Prime Video",  TRUE },
	{ "com.amazon.firetv.youtube",           "YouTube",      TRUE },
	{ "com.google.android.youtube.tv",       "YouTube",      TRUE },
	{ "com.google.android.youtube.tvkids",   "YouTube Kids", TRUE },
	{ "com.plexapp.android",                 "Plex",         TRUE },
	{ "org.xbmc.kodi",                       "Kodi",         TRUE },
	{ "org.jellyfin.androidtv",              "Jellyfin",     TRUE },
	{ "tv.emby.embyatv",                     "Emby",         TRUE },
	{ "tv.twitch.android.viewer",            "Twitch",       TRUE },
	{ "tv.twitch.android.app",               "Twitch",       TRUE },
	{ "com.disney.disneyplus",               "Disney+",      TRUE },
	{ "com.hulu.livingroomplus",             "Hulu",         TRUE },
	{ "com.hulu.plus",                       "Hulu",         TRUE },
	{ "com.wbd.stream",                      "Max",          TRUE },
	{ "com.hbo.hbonow",                      "Max",          TRUE },
	{ "com.cbs.ott",                         "Paramount+",   TRUE },
	{ "com.peacocktv.peacockandroid",        "Peacock",      TRUE },
	{ "com.apple.atve.amazon.appletv",       "Apple TV",     TRUE },
	{ "com.apple.atve.androidtv.appletv",    "Apple TV",     TRUE },
	{ "com.spotify.tv.android",              "Spotify",      TRUE },
	{ "tv.pluto.android",                    "Pluto TV",     TRUE },
	{ "com.crunchyroll.crunchyroid",         "Crunchyroll",  TRUE },
	{ "org.videolan.vlc",                    "VLC",          TRUE },
	{ "com.liskovsoft.smarttubetv.beta",     "SmartTube",    TRUE },
	{ "com.amazon.tv.launcher",              "Home",         FALSE },
	{ "com.google.android.tvlauncher",       "Home",         FALSE },
	{ "com.google.android.apps.tv.launcherx", "Home",        FALSE },
	{ "com.amazon.tv.settings.v2",           "Settings",     TRUE },
	{ "com.amazon.tv.settings",              "Settings",     TRUE },
	{ "com.android.tv.settings",             "Settings",     TRUE },
	{ "com.amazon.vizzini",                  "Alexa",        FALSE },
	{ "com.amazon.ftv.screensaver",          "Screensaver",  FALSE },
	{ "com.amazon.bueller.photos",           "Photos",       TRUE }
};

/**
 * bar_tvremote_app_friendly_name:
 * @kind: the device kind (reserved; the table is shared today)
 * @pkg: (nullable): a package
 *
 * Returns: (transfer none) (nullable): what to call @pkg, or %NULL
 *   when it is not in the table (the panel then shows the id)
 */
const gchar *
bar_tvremote_app_friendly_name(BarTvremoteKind kind, const gchar *pkg)
{
	guint i;

	(void)kind;
	if (pkg == NULL)
		return NULL;
	for (i = 0; i < G_N_ELEMENTS(known_apps); i++)
		if (strcmp(known_apps[i].pkg, pkg) == 0)
			return known_apps[i].name;
	return NULL;
}

/**
 * bar_tvremote_app_is_curated:
 * @pkg: (nullable): a package
 *
 * Returns: %TRUE when @pkg is a known app the apps list should offer
 *   even though it is a system package (Prime Video on Fire OS is)
 */
gboolean
bar_tvremote_app_is_curated(const gchar *pkg)
{
	guint i;

	if (pkg == NULL)
		return FALSE;
	for (i = 0; i < G_N_ELEMENTS(known_apps); i++)
		if (strcmp(known_apps[i].pkg, pkg) == 0)
			return known_apps[i].launch;
	return FALSE;
}

/* A launcher or similar the list should not offer. */
static gboolean
app_is_hidden(const gchar *pkg)
{
	guint i;

	for (i = 0; i < G_N_ELEMENTS(known_apps); i++)
		if (strcmp(known_apps[i].pkg, pkg) == 0)
			return !known_apps[i].launch;
	return FALSE;
}

/**
 * bar_tvremote_app_new:
 * @id: the launch id
 * @name: (nullable): the display name; @id when %NULL
 *
 * Both strings are cleaned (valid UTF-8, no control characters,
 * bounded), since they come from the device.
 *
 * Returns: (transfer full): a new app
 */
BarTvremoteApp *
bar_tvremote_app_new(const gchar *id, const gchar *name)
{
	BarTvremoteApp *app;

	app = g_new0(BarTvremoteApp, 1);
	app->id = bar_tvremote_clean_text(id, 255);
	app->name = bar_tvremote_clean_text(name != NULL ? name : id, 120);
	return app;
}

/**
 * bar_tvremote_app_free:
 * @app: (nullable): an app
 */
void
bar_tvremote_app_free(BarTvremoteApp *app)
{
	if (app == NULL)
		return;
	g_free(app->id);
	g_free(app->name);
	g_free(app);
}

static gint
compare_apps(gconstpointer a, gconstpointer b)
{
	const BarTvremoteApp *x = *(const BarTvremoteApp * const *)a;
	const BarTvremoteApp *y = *(const BarTvremoteApp * const *)b;
	g_autofree gchar *cx = g_utf8_casefold(x->name, -1);
	g_autofree gchar *cy = g_utf8_casefold(y->name, -1);
	gint r = g_utf8_collate(cx, cy);

	return (r != 0) ? r : strcmp(x->id, y->id);
}

/**
 * bar_tvremote_adb_apps:
 * @kind: the adb kind
 * @pm_third_party: (nullable): `pm list packages -3'
 * @pm_all: (nullable): `pm list packages'
 *
 * The launchable list is the user-installed packages PLUS the known
 * ones that ship as system packages: on Fire OS Prime Video is a
 * system app, so `-3' alone would leave the most-used app out.
 *
 * Returns: (transfer full) (element-type BarTvremoteApp): the apps,
 *   sorted by name
 */
GPtrArray *
bar_tvremote_adb_apps(BarTvremoteKind kind, const gchar *pm_third_party,
                      const gchar *pm_all)
{
	g_autoptr(GPtrArray) third = NULL;
	g_autoptr(GPtrArray) all = NULL;
	g_autoptr(GHashTable) seen = NULL;
	GPtrArray *apps;
	guint i;

	apps = g_ptr_array_new_with_free_func(
		(GDestroyNotify)bar_tvremote_app_free);
	seen = g_hash_table_new(g_str_hash, g_str_equal);
	third = bar_tvremote_parse_pm_list(pm_third_party);
	all = bar_tvremote_parse_pm_list(pm_all);

	for (i = 0; i < third->len; i++) {
		const gchar *pkg = g_ptr_array_index(third, i);

		if (app_is_hidden(pkg) || g_hash_table_contains(seen, pkg))
			continue;
		g_hash_table_add(seen, (gpointer)pkg);
		g_ptr_array_add(apps, bar_tvremote_app_new(pkg,
			bar_tvremote_app_friendly_name(kind, pkg)));
	}
	for (i = 0; i < all->len; i++) {
		const gchar *pkg = g_ptr_array_index(all, i);

		if (!bar_tvremote_app_is_curated(pkg) ||
		    g_hash_table_contains(seen, pkg))
			continue;
		g_hash_table_add(seen, (gpointer)pkg);
		g_ptr_array_add(apps, bar_tvremote_app_new(pkg,
			bar_tvremote_app_friendly_name(kind, pkg)));
	}
	g_ptr_array_sort(apps, compare_apps);
	return apps;
}

/**
 * bar_tvremote_parse_mdns:
 * @out: (nullable): `adb mdns services' output
 *
 * Lines are `NAME<TAB>_adb._tcp.<TAB>HOST:PORT' (or the TLS service
 * Android 11's wireless debugging uses).  The header line and anything
 * without an address are skipped.
 *
 * Returns: (transfer full) (element-type utf8): `HOST:PORT' strings
 */
GPtrArray *
bar_tvremote_parse_mdns(const gchar *out)
{
	GPtrArray *found;
	g_auto(GStrv) lines = NULL;
	guint i;

	found = g_ptr_array_new_with_free_func(g_free);
	if (out == NULL)
		return found;
	lines = g_strsplit(out, "\n", -1);
	for (i = 0; lines[i] != NULL; i++) {
		g_auto(GStrv) cols = NULL;
		guint n;
		gchar *addr, *colon;
		g_autofree gchar *host = NULL;

		if (strstr(lines[i], "_adb") == NULL)
			continue;
		cols = g_strsplit_set(lines[i], " \t", -1);
		n = g_strv_length(cols);
		if (n == 0)
			continue;
		addr = g_strstrip(cols[n - 1]);
		colon = strrchr(addr, ':');
		if (colon == NULL)
			continue;
		host = g_strndup(addr, (gsize)(colon - addr));
		if (!bar_tvremote_valid_host(host) ||
		    g_ascii_strtoll(colon + 1, NULL, 10) <= 0)
			continue;
		g_ptr_array_add(found, g_strdup(addr));
	}
	return found;
}

/* ----------------------------------------------------------------
 * Roku ECP
 * ---------------------------------------------------------------- */

/**
 * bar_tvremote_urlencode:
 * @text: bytes to put in a URL path segment
 *
 * Percent-encodes everything but RFC 3986's unreserved set, byte by
 * byte, so a multi-byte character becomes its UTF-8 bytes and a `/'
 * cannot start a new path segment.
 *
 * Returns: (transfer full): the encoded text
 */
gchar *
bar_tvremote_urlencode(const gchar *text)
{
	GString *out;
	const guchar *p;

	out = g_string_new(NULL);
	for (p = (const guchar *)text; *p != '\0'; p++) {
		if (g_ascii_isalnum(*p) || *p == '-' || *p == '_' ||
		    *p == '.' || *p == '~')
			g_string_append_c(out, (gchar)*p);
		else
			g_string_append_printf(out, "%%%02X", (guint)*p);
	}
	return g_string_free(out, FALSE);
}

/**
 * bar_tvremote_valid_host:
 * @host: (nullable): a string from the panel or the config
 *
 * A host name, an IPv4 address or an IPv6 one.  Nothing that could
 * turn a URL into a different URL -- `/', `@', `?', `#', whitespace --
 * and no leading `-', which a command-line tool would take for an
 * option.
 *
 * Returns: %TRUE when @host is usable
 */
gboolean
bar_tvremote_valid_host(const gchar *host)
{
	const gchar *p;
	gsize len;

	if (host == NULL || host[0] == '\0' || host[0] == '-')
		return FALSE;
	len = strlen(host);
	if (len > 253)
		return FALSE;
	for (p = host; *p != '\0'; p++)
		if (!g_ascii_isalnum(*p) && *p != '.' && *p != '-' &&
		    *p != ':' && *p != '[' && *p != ']' && *p != '_')
			return FALSE;
	return TRUE;
}

/**
 * bar_tvremote_http_url:
 * @host: a valid host
 * @port: a port
 * @path: an absolute path, already encoded
 *
 * Returns: (transfer full): the URL, IPv6 hosts bracketed
 */
gchar *
bar_tvremote_http_url(const gchar *host, gint port, const gchar *path)
{
	if (strchr(host, ':') != NULL && host[0] != '[')
		return g_strdup_printf("http://[%s]:%d%s", host, port, path);
	return g_strdup_printf("http://%s:%d%s", host, port, path);
}

/**
 * bar_tvremote_roku_text_paths:
 * @text: typed text
 *
 * ECP types one character per request, `/keypress/Lit_<char>', the
 * character URL-encoded (so `/' is `Lit_%2F' and `é' is `Lit_%C3%A9').
 * Control characters are skipped; at most 256 characters are taken.
 *
 * Returns: (transfer full) (array zero-terminated=1): the paths
 */
gchar **
bar_tvremote_roku_text_paths(const gchar *text)
{
	GPtrArray *paths;
	const gchar *p;
	guint taken = 0;

	paths = g_ptr_array_new();
	if (text != NULL && g_utf8_validate(text, -1, NULL)) {
		for (p = text; *p != '\0' && taken < 256;
		     p = g_utf8_next_char(p)) {
			gunichar c = g_utf8_get_char(p);
			gchar buf[8];
			gint n;
			g_autofree gchar *enc = NULL;

			if (c < 0x20 || c == 0x7f)
				continue;
			n = g_unichar_to_utf8(c, buf);
			buf[n] = '\0';
			enc = bar_tvremote_urlencode(buf);
			g_ptr_array_add(paths,
				g_strdup_printf("/keypress/Lit_%s", enc));
			taken++;
		}
	}
	g_ptr_array_add(paths, NULL);
	return (gchar **)g_ptr_array_free(paths, FALSE);
}

/* GMarkup walk shared by the Roku parsers. */
typedef struct {
	const gchar *want;         /* element whose text is collected */
	gboolean     in_want;
	gchar       *cur_id;
	GString     *text;
	GPtrArray   *apps;         /* for <app> lists, else NULL */
	gchar       *first;        /* first complete text of `want' */
} RokuWalk;

static void
roku_start(GMarkupParseContext *ctx, const gchar *element,
           const gchar **names, const gchar **values, gpointer user_data,
           GError **error)
{
	RokuWalk *w = user_data;
	guint i;

	(void)ctx;
	(void)error;
	if (strcmp(element, w->want) != 0)
		return;
	w->in_want = TRUE;
	g_string_truncate(w->text, 0);
	g_clear_pointer(&w->cur_id, g_free);
	for (i = 0; names[i] != NULL; i++)
		if (strcmp(names[i], "id") == 0)
			w->cur_id = g_strdup(values[i]);
}

static void
roku_end(GMarkupParseContext *ctx, const gchar *element, gpointer user_data,
         GError **error)
{
	RokuWalk *w = user_data;

	(void)ctx;
	(void)error;
	if (!w->in_want || strcmp(element, w->want) != 0)
		return;
	w->in_want = FALSE;
	if (w->apps != NULL) {
		if (w->cur_id != NULL && bar_tvremote_valid_app_id(w->cur_id) &&
		    w->apps->len < 500)
			g_ptr_array_add(w->apps,
				bar_tvremote_app_new(w->cur_id, w->text->str));
	} else if (w->first == NULL) {
		w->first = g_strdup(w->text->str);
	}
}

static void
roku_text(GMarkupParseContext *ctx, const gchar *text, gsize len,
          gpointer user_data, GError **error)
{
	RokuWalk *w = user_data;

	(void)ctx;
	(void)error;
	if (w->in_want && w->text->len < 1024)
		g_string_append_len(w->text, text, (gssize)len);
}

static const GMarkupParser roku_parser = {
	roku_start, roku_end, roku_text, NULL, NULL
};

static gboolean
roku_walk(const gchar *xml, RokuWalk *w)
{
	GMarkupParseContext *ctx;
	gboolean ok;

	if (xml == NULL)
		return FALSE;
	w->text = g_string_new(NULL);
	ctx = g_markup_parse_context_new(&roku_parser, 0, w, NULL);
	ok = g_markup_parse_context_parse(ctx, xml, -1, NULL) &&
	     g_markup_parse_context_end_parse(ctx, NULL);
	g_markup_parse_context_free(ctx);
	g_string_free(w->text, TRUE);
	g_clear_pointer(&w->cur_id, g_free);
	return ok;
}

/**
 * bar_tvremote_roku_parse_apps:
 * @xml: (nullable): the body of GET /query/apps
 *
 * `<apps><app id="12" type="appl" version="5.1">Netflix</app>...'.
 * Malformed XML yields what was read before the fault rather than
 * nothing: a channel list cut short is still a channel list.
 *
 * Returns: (transfer full) (element-type BarTvremoteApp): the apps
 */
GPtrArray *
bar_tvremote_roku_parse_apps(const gchar *xml)
{
	RokuWalk w;

	memset(&w, 0, sizeof(w));
	w.want = "app";
	w.apps = g_ptr_array_new_with_free_func(
		(GDestroyNotify)bar_tvremote_app_free);
	roku_walk(xml, &w);
	g_ptr_array_sort(w.apps, compare_apps);
	return w.apps;
}

/**
 * bar_tvremote_roku_parse_active:
 * @xml: (nullable): the body of GET /query/active-app
 *
 * `<active-app><app id="12" ...>Netflix</app></active-app>', or
 * `<app>Roku</app>' with no id on the home screen.
 *
 * Returns: (transfer full) (nullable): the app's name
 */
gchar *
bar_tvremote_roku_parse_active(const gchar *xml)
{
	g_autofree gchar *raw = NULL;

	raw = bar_tvremote_roku_parse_field(xml, "app");
	if (raw == NULL)
		return NULL;
	return bar_tvremote_clean_text(raw, 120);
}

/**
 * bar_tvremote_roku_parse_field:
 * @xml: (nullable): an ECP XML body
 * @element: an element name, e.g. "user-device-name"
 *
 * Returns: (transfer full) (nullable): the first such element's text,
 *   or %NULL when absent, empty or the XML is broken before it
 */
gchar *
bar_tvremote_roku_parse_field(const gchar *xml, const gchar *element)
{
	RokuWalk w;
	gchar *out;

	memset(&w, 0, sizeof(w));
	w.want = element;
	roku_walk(xml, &w);
	if (w.first == NULL)
		return NULL;
	out = bar_tvremote_clean_text(w.first, 120);
	g_free(w.first);
	if (out[0] == '\0') {
		g_free(out);
		return NULL;
	}
	return out;
}

/**
 * bar_tvremote_valid_app_id:
 * @id: (nullable): an app id from a device or a panel row
 *
 * Roku channel ids are numbers, or `tvinput.hdmi1'; LG's are
 * reverse-DNS; Samsung's are digits or `xxxxxxxxxx.Name'.  None needs
 * more than letters, digits, `.', `_', `:' and `-', and allowing no
 * more is what keeps an id safe in a URL path or a shell line.
 *
 * Returns: %TRUE when @id is plausible
 */
gboolean
bar_tvremote_valid_app_id(const gchar *id)
{
	const gchar *p;

	if (id == NULL || id[0] == '\0' || id[0] == '-' || strlen(id) > 128)
		return FALSE;
	for (p = id; *p != '\0'; p++)
		if (!g_ascii_isalnum(*p) && *p != '.' && *p != '_' &&
		    *p != ':' && *p != '-')
			return FALSE;
	return TRUE;
}

/* ----------------------------------------------------------------
 * Discovery
 * ---------------------------------------------------------------- */

/**
 * bar_tvremote_ssdp_request:
 * @st: the search target, e.g. "roku:ecp"
 *
 * Returns: (transfer full): an M-SEARCH datagram
 */
gchar *
bar_tvremote_ssdp_request(const gchar *st)
{
	return g_strdup_printf("M-SEARCH * HTTP/1.1\r\n"
	                       "HOST: 239.255.255.250:1900\r\n"
	                       "MAN: \"ssdp:discover\"\r\n"
	                       "MX: 2\r\n"
	                       "ST: %s\r\n\r\n", st);
}

/**
 * bar_tvremote_ssdp_parse:
 * @response: a datagram received on the SSDP socket
 * @location: (out) (optional): the LOCATION header
 * @st: (out) (optional): the ST (or NT) header
 * @server: (out) (optional): the SERVER header
 *
 * Header names are case-insensitive and devices disagree on case
 * (Roku writes `Location', LG `LOCATION').
 *
 * Returns: %TRUE when @response is an SSDP answer with a location
 */
gboolean
bar_tvremote_ssdp_parse(const gchar *response, gchar **location,
                        gchar **st, gchar **server)
{
	g_auto(GStrv) lines = NULL;
	g_autofree gchar *loc = NULL;
	g_autofree gchar *s = NULL;
	g_autofree gchar *srv = NULL;
	guint i;

	if (location != NULL)
		*location = NULL;
	if (st != NULL)
		*st = NULL;
	if (server != NULL)
		*server = NULL;
	if (response == NULL || !g_utf8_validate(response, -1, NULL))
		return FALSE;
	if (!g_str_has_prefix(response, "HTTP/1.1 200") &&
	    !g_str_has_prefix(response, "NOTIFY"))
		return FALSE;

	lines = g_strsplit(response, "\n", -1);
	for (i = 1; lines[i] != NULL; i++) {
		gchar *colon = strchr(lines[i], ':');
		g_autofree gchar *name = NULL;
		gchar *value;

		if (colon == NULL)
			continue;
		name = g_ascii_strdown(lines[i], colon - lines[i]);
		g_strstrip(name);
		value = g_strstrip(colon + 1);
		if (strcmp(name, "location") == 0 && loc == NULL)
			loc = g_strdup(value);
		else if ((strcmp(name, "st") == 0 || strcmp(name, "nt") == 0) &&
		         s == NULL)
			s = g_strdup(value);
		else if (strcmp(name, "server") == 0 && srv == NULL)
			srv = g_strdup(value);
	}
	if (loc == NULL)
		return FALSE;
	if (location != NULL)
		*location = g_steal_pointer(&loc);
	if (st != NULL)
		*st = g_steal_pointer(&s);
	if (server != NULL)
		*server = g_steal_pointer(&srv);
	return TRUE;
}

/**
 * bar_tvremote_ssdp_kind:
 * @st: (nullable): the ST header
 * @server: (nullable): the SERVER header
 *
 * Returns: the kind of device that answered
 */
BarTvremoteKind
bar_tvremote_ssdp_kind(const gchar *st, const gchar *server)
{
	g_autofree gchar *s = g_ascii_strdown(st != NULL ? st : "", -1);
	g_autofree gchar *v = g_ascii_strdown(server != NULL ? server : "", -1);

	if (strstr(s, "roku:ecp") != NULL || strstr(v, "roku") != NULL)
		return BAR_TVREMOTE_KIND_ROKU;
	if (strstr(s, "samsung.com") != NULL || strstr(v, "samsung") != NULL ||
	    strstr(v, "tizen") != NULL)
		return BAR_TVREMOTE_KIND_SAMSUNG;
	if (strstr(s, "lge-com") != NULL || strstr(s, "webos") != NULL ||
	    strstr(v, "webos") != NULL)
		return BAR_TVREMOTE_KIND_LG;
	return BAR_TVREMOTE_KIND_UNKNOWN;
}

/**
 * bar_tvremote_url_host_port:
 * @url: an http(s) URL, e.g. an SSDP location
 * @host: (out): the host, IPv6 brackets removed
 * @port: (out): the port, or the scheme's default
 *
 * Returns: %TRUE when @url has a usable host
 */
gboolean
bar_tvremote_url_host_port(const gchar *url, gchar **host, gint *port)
{
	const gchar *p, *end, *colon;
	gint def;

	*host = NULL;
	*port = 0;
	if (url == NULL)
		return FALSE;
	if (g_str_has_prefix(url, "http://")) {
		p = url + 7;
		def = 80;
	} else if (g_str_has_prefix(url, "https://")) {
		p = url + 8;
		def = 443;
	} else if (g_str_has_prefix(url, "ws://")) {
		p = url + 5;
		def = 80;
	} else if (g_str_has_prefix(url, "wss://")) {
		p = url + 6;
		def = 443;
	} else {
		return FALSE;
	}
	end = p;
	while (*end != '\0' && *end != '/' && *end != '?' && *end != '#')
		end++;
	if (*p == '[') {
		const gchar *close = memchr(p, ']', (gsize)(end - p));

		if (close == NULL)
			return FALSE;
		*host = g_strndup(p + 1, (gsize)(close - p - 1));
		colon = (close + 1 < end && close[1] == ':') ? close + 1 : NULL;
	} else {
		colon = memchr(p, ':', (gsize)(end - p));
		*host = g_strndup(p, (gsize)((colon != NULL ? colon : end) - p));
	}
	*port = def;
	if (colon != NULL) {
		gint64 v = g_ascii_strtoll(colon + 1, NULL, 10);

		if (v > 0 && v < 65536)
			*port = (gint)v;
	}
	if (!bar_tvremote_valid_host(*host)) {
		g_clear_pointer(host, g_free);
		return FALSE;
	}
	return TRUE;
}

/**
 * bar_tvremote_route_iface:
 * @proc_net_route: (nullable): the contents of /proc/net/route
 *
 * The interface the default route leaves by: the row whose
 * destination and mask are both zero and whose UP flag is set, lowest
 * metric winning (a laptop on Wi-Fi and Ethernet has two).
 *
 * Returns: (transfer full) (nullable): the interface name
 */
gchar *
bar_tvremote_route_iface(const gchar *proc_net_route)
{
	g_auto(GStrv) lines = NULL;
	gchar *best = NULL;
	gint64 best_metric = G_MAXINT64;
	guint i;

	if (proc_net_route == NULL)
		return NULL;
	lines = g_strsplit(proc_net_route, "\n", -1);
	for (i = 1; lines[i] != NULL; i++) {
		g_auto(GStrv) f = g_strsplit_set(lines[i], " \t", -1);
		GPtrArray *cols;
		guint j;
		gint64 flags, metric;

		/* Collapse the runs of blanks the kernel pads with. */
		cols = g_ptr_array_new();
		for (j = 0; f[j] != NULL; j++)
			if (f[j][0] != '\0')
				g_ptr_array_add(cols, f[j]);
		if (cols->len >= 8 &&
		    strcmp(g_ptr_array_index(cols, 1), "00000000") == 0 &&
		    strcmp(g_ptr_array_index(cols, 7), "00000000") == 0) {
			flags = g_ascii_strtoll(g_ptr_array_index(cols, 3),
			                        NULL, 16);
			metric = g_ascii_strtoll(g_ptr_array_index(cols, 6),
			                         NULL, 10);
			if ((flags & 0x1) != 0 && metric < best_metric) {
				g_free(best);
				best = g_strdup(g_ptr_array_index(cols, 0));
				best_metric = metric;
			}
		}
		g_ptr_array_free(cols, TRUE);
	}
	return best;
}

/**
 * bar_tvremote_parse_cidr:
 * @cidr: "a.b.c.d/p"
 * @base: (out): the network address, host byte order
 * @prefix: (out): the prefix length
 *
 * Returns: %TRUE when @cidr is a valid IPv4 CIDR
 */
gboolean
bar_tvremote_parse_cidr(const gchar *cidr, guint32 *base, guint *prefix)
{
	g_auto(GStrv) halves = NULL;
	g_auto(GStrv) octets = NULL;
	guint32 addr = 0, mask;
	gint64 p;
	guint i;

	if (cidr == NULL)
		return FALSE;
	halves = g_strsplit(cidr, "/", 2);
	if (halves[0] == NULL || halves[1] == NULL)
		return FALSE;
	octets = g_strsplit(g_strstrip(halves[0]), ".", -1);
	if (g_strv_length(octets) != 4)
		return FALSE;
	for (i = 0; i < 4; i++) {
		gchar *end = NULL;
		gint64 v = g_ascii_strtoll(octets[i], &end, 10);

		if (end == octets[i] || *end != '\0' || v < 0 || v > 255)
			return FALSE;
		addr = (addr << 8) | (guint32)v;
	}
	{
		gchar *end = NULL;

		p = g_ascii_strtoll(g_strstrip(halves[1]), &end, 10);
		if (end == halves[1] || *end != '\0' || p < 0 || p > 32)
			return FALSE;
	}
	mask = (p == 0) ? 0u : (guint32)(0xffffffffu << (32 - p));
	*base = addr & mask;
	*prefix = (guint)p;
	return TRUE;
}

/**
 * bar_tvremote_subnet_for:
 * @addr: an interface's IPv4 address, host byte order
 * @mask: its netmask, host byte order
 *
 * A probe of every address is bounded at a /24: a /16 is 65,534
 * connection attempts, which is a port scan of the office rather than
 * a look around the living room.  A wider network is narrowed to the
 * /24 the machine itself is in, where the TV almost always is.
 *
 * Returns: (transfer full): a CIDR string
 */
gchar *
bar_tvremote_subnet_for(guint32 addr, guint32 mask)
{
	guint prefix = 0;
	guint32 m = mask;
	guint32 base;

	while ((m & 0x80000000u) != 0) {
		prefix++;
		m <<= 1;
	}
	if (prefix < 24) {
		prefix = 24;
		mask = 0xffffff00u;
	}
	base = addr & mask;
	return g_strdup_printf("%u.%u.%u.%u/%u",
	                       (base >> 24) & 0xff, (base >> 16) & 0xff,
	                       (base >> 8) & 0xff, base & 0xff, prefix);
}

/**
 * bar_tvremote_subnet_hosts:
 * @cidr: a /24 or narrower IPv4 network
 *
 * Returns: (transfer full) (nullable) (element-type utf8): every host
 *   address in @cidr (network and broadcast excluded below /31), or
 *   %NULL when @cidr is invalid or wider than /24
 */
GPtrArray *
bar_tvremote_subnet_hosts(const gchar *cidr)
{
	GPtrArray *hosts;
	guint32 base, n, i, first, last;
	guint prefix;

	if (!bar_tvremote_parse_cidr(cidr, &base, &prefix) || prefix < 24)
		return NULL;
	n = 1u << (32 - prefix);
	first = 0;
	last = n - 1;
	if (prefix < 31) {
		first = 1;
		last = n - 2;
	}
	hosts = g_ptr_array_new_with_free_func(g_free);
	for (i = first; i <= last; i++) {
		guint32 a = base + i;

		g_ptr_array_add(hosts, g_strdup_printf("%u.%u.%u.%u",
			(a >> 24) & 0xff, (a >> 16) & 0xff,
			(a >> 8) & 0xff, a & 0xff));
	}
	return hosts;
}

/**
 * bar_tvremote_kind_for_port:
 * @port: an open TCP port found by a probe
 *
 * Returns: the kind a device answering there most likely is
 */
BarTvremoteKind
bar_tvremote_kind_for_port(gint port)
{
	switch (port) {
	case 5555:
		return BAR_TVREMOTE_KIND_FIRETV;
	case 8060:
		return BAR_TVREMOTE_KIND_ROKU;
	case 8001:
	case 8002:
		return BAR_TVREMOTE_KIND_SAMSUNG;
	case 3000:
	case 3001:
		return BAR_TVREMOTE_KIND_LG;
	default:
		return BAR_TVREMOTE_KIND_UNKNOWN;
	}
}

/* ----------------------------------------------------------------
 * Devices
 * ---------------------------------------------------------------- */

/**
 * bar_tvremote_device_new:
 * @id: a unique id
 * @kind: the protocol
 * @host: the address
 *
 * Returns: (transfer full): a device named after its kind and host
 */
BarTvremoteDevice *
bar_tvremote_device_new(const gchar *id, BarTvremoteKind kind,
                        const gchar *host)
{
	BarTvremoteDevice *dev;

	dev = g_new0(BarTvremoteDevice, 1);
	dev->id = g_strdup(id);
	dev->kind = kind;
	dev->host = g_strdup(host);
	dev->name = g_strdup_printf("%s (%s)", bar_tvremote_kind_title(kind),
	                            host != NULL ? host : "?");
	return dev;
}

/**
 * bar_tvremote_device_copy:
 * @dev: a device
 *
 * Returns: (transfer full): a deep copy
 */
BarTvremoteDevice *
bar_tvremote_device_copy(const BarTvremoteDevice *dev)
{
	BarTvremoteDevice *copy;

	copy = g_new0(BarTvremoteDevice, 1);
	copy->id = g_strdup(dev->id);
	copy->name = g_strdup(dev->name);
	copy->kind = dev->kind;
	copy->host = g_strdup(dev->host);
	copy->port = dev->port;
	copy->token = g_strdup(dev->token);
	copy->client_key = g_strdup(dev->client_key);
	copy->volume_via = g_strdup(dev->volume_via);
	return copy;
}

/**
 * bar_tvremote_device_free:
 * @dev: (nullable): a device
 *
 * The secrets are overwritten before they are freed, so a later heap
 * dump does not have them lying in freed memory.
 */
void
bar_tvremote_device_free(BarTvremoteDevice *dev)
{
	if (dev == NULL)
		return;
	if (dev->token != NULL)
		memset(dev->token, 0, strlen(dev->token));
	if (dev->client_key != NULL)
		memset(dev->client_key, 0, strlen(dev->client_key));
	g_free(dev->id);
	g_free(dev->name);
	g_free(dev->host);
	g_free(dev->token);
	g_free(dev->client_key);
	g_free(dev->volume_via);
	g_free(dev);
}

/**
 * bar_tvremote_device_port:
 * @dev: a device
 *
 * Returns: the configured port, or the kind's default
 */
gint
bar_tvremote_device_port(const BarTvremoteDevice *dev)
{
	return (dev->port > 0) ? dev->port
	                       : bar_tvremote_kind_default_port(dev->kind);
}

/**
 * bar_tvremote_device_find:
 * @devices: (element-type BarTvremoteDevice): the list
 * @id: (nullable): an id
 *
 * Returns: (transfer none) (nullable): the device
 */
BarTvremoteDevice *
bar_tvremote_device_find(GPtrArray *devices, const gchar *id)
{
	guint i;

	if (devices == NULL || id == NULL)
		return NULL;
	for (i = 0; i < devices->len; i++) {
		BarTvremoteDevice *d = g_ptr_array_index(devices, i);

		if (g_strcmp0(d->id, id) == 0)
			return d;
	}
	return NULL;
}

/**
 * bar_tvremote_device_make_id:
 * @devices: (element-type BarTvremoteDevice): the existing list
 * @kind: the new device's kind
 * @host: its host
 *
 * Returns: (transfer full): `KIND-HOST' with every non-alphanumeric
 *   byte made a `-', suffixed until it is unique
 */
gchar *
bar_tvremote_device_make_id(GPtrArray *devices, BarTvremoteKind kind,
                            const gchar *host)
{
	g_autofree gchar *base = NULL;
	gchar *id;
	gchar *p;
	guint n;

	base = g_strdup_printf("%s-%s", bar_tvremote_kind_name(kind), host);
	for (p = base; *p != '\0'; p++)
		if (!g_ascii_isalnum(*p))
			*p = '-';
	id = g_strdup(base);
	for (n = 2; bar_tvremote_device_find(devices, id) != NULL; n++) {
		g_free(id);
		id = g_strdup_printf("%s-%u", base, n);
	}
	return id;
}

/**
 * bar_tvremote_route_volume:
 * @devices: (element-type BarTvremoteDevice): the list
 * @id: the device a key was pressed for
 * @key: the key
 *
 * "Sound goes through": a TV whose audio leaves through a receiver or a
 * sound bar names that device in `volume-via', and its volume keys go
 * there instead.  The chain is followed (a TV through a Fire TV
 * Cube through an AV receiver), at most eight hops.  A name that does
 * not exist ends the chain at the last device that does; a LOOP is a
 * misconfiguration with no right answer, so the key goes to the device
 * it was pressed for rather than to an arbitrary member of the loop.
 *
 * Returns: (transfer none) (nullable): the device to send @key to, or
 *   %NULL when @id is unknown
 */
const BarTvremoteDevice *
bar_tvremote_route_volume(GPtrArray *devices, const gchar *id,
                          BarTvremoteKey key)
{
	const BarTvremoteDevice *start, *cur;
	g_autoptr(GHashTable) seen = NULL;
	guint hops;

	start = bar_tvremote_device_find(devices, id);
	if (start == NULL || !bar_tvremote_key_is_volume(key))
		return start;

	seen = g_hash_table_new(g_str_hash, g_str_equal);
	cur = start;
	g_hash_table_add(seen, cur->id);
	for (hops = 0; hops < 8; hops++) {
		const BarTvremoteDevice *next;

		if (cur->volume_via == NULL || cur->volume_via[0] == '\0')
			return cur;
		next = bar_tvremote_device_find(devices, cur->volume_via);
		if (next == NULL)
			return cur;
		if (g_hash_table_contains(seen, next->id))
			return start;
		g_hash_table_add(seen, next->id);
		cur = next;
	}
	return start;
}

/* ----------------------------------------------------------------
 * The remote's layout
 *
 * Laid out on a nominal 320-wide canvas and scaled to the real one,
 * so the proportions hold at any panel width and a theme scale.
 * ---------------------------------------------------------------- */

#define NOMINAL_W      (320.0)
#define PAD            (8.0)
#define TOP_H          (30.0)
#define RING_R         (78.0)
#define ROW_H          (34.0)
#define ROW_STEP       (42.0)
#define DPAD_H0        (344)
#define CAST_H0        (134)

/**
 * bar_tvremote_has_dpad:
 * @kind: a kind
 *
 * Returns: %TRUE when the remote for @kind has arrows and select
 */
gboolean
bar_tvremote_has_dpad(BarTvremoteKind kind)
{
	return bar_tvremote_kind_supports(kind, BAR_TVREMOTE_KEY_UP);
}

/**
 * bar_tvremote_canvas_height:
 * @kind: a kind
 *
 * Returns: the height to reserve for @kind's remote, in pixels
 */
gint
bar_tvremote_canvas_height(BarTvremoteKind kind)
{
	return bar_tvremote_has_dpad(kind) ? DPAD_H0 : CAST_H0;
}

/* Lay out the supported keys of @keys evenly across one row. */
static guint
layout_row(BarTvremoteKind kind, const BarTvremoteKey *keys, guint n_keys,
           gdouble y, BarTvremoteButton *out, guint at)
{
	BarTvremoteKey have[8];
	guint n = 0, i;
	gdouble col_w;

	for (i = 0; i < n_keys && n < G_N_ELEMENTS(have); i++)
		if (bar_tvremote_kind_supports(kind, keys[i]))
			have[n++] = keys[i];
	if (n == 0)
		return at;
	col_w = (NOMINAL_W - 2.0 * PAD - (gdouble)(n - 1) * PAD) / (gdouble)n;
	for (i = 0; i < n && at < BAR_TVREMOTE_MAX_BUTTONS; i++) {
		out[at].key = have[i];
		out[at].x = PAD + (gdouble)i * (col_w + PAD);
		out[at].y = y;
		out[at].w = col_w;
		out[at].h = ROW_H;
		out[at].round = FALSE;
		at++;
	}
	return at;
}

static guint
add_button(BarTvremoteButton *out, guint at, BarTvremoteKey key,
           gdouble x, gdouble y, gdouble w, gdouble h, gboolean round)
{
	if (at >= BAR_TVREMOTE_MAX_BUTTONS)
		return at;
	out[at].key = key;
	out[at].x = x;
	out[at].y = y;
	out[at].w = w;
	out[at].h = h;
	out[at].round = round;
	return at + 1;
}

/**
 * bar_tvremote_layout:
 * @kind: the device kind
 * @width: the canvas width in pixels
 * @height: the canvas height in pixels
 * @out: (out caller-allocates) (array fixed-size=24): the buttons
 * @ring_cx: (out) (optional): the D-pad ring's centre x
 * @ring_cy: (out) (optional): its centre y
 * @ring_r: (out) (optional): its radius, 0 when there is no D-pad
 *
 * The one description of where each button is.  The canvas draws these
 * rectangles and the click handler tests against these rectangles, so
 * the two cannot disagree.
 *
 * With a D-pad: a top row (power, and search / input / settings at the
 * right where the kind has them), the ring -- four arrow sectors
 * around a round select -- then back / home / menu, rewind / play /
 * forward, and volume down / mute / up.  Without one (a Chromecast):
 * the transport, volume and power rows only.  Keys a kind cannot send
 * are left out and the row's remaining buttons widen to fill it.
 *
 * Returns: how many buttons were written to @out
 */
guint
bar_tvremote_layout(BarTvremoteKind kind, gint width, gint height,
                    BarTvremoteButton *out, gdouble *ring_cx,
                    gdouble *ring_cy, gdouble *ring_r)
{
	static const BarTvremoteKey nav_row[] = {
		BAR_TVREMOTE_KEY_BACK, BAR_TVREMOTE_KEY_HOME, BAR_TVREMOTE_KEY_MENU
	};
	static const BarTvremoteKey media_row[] = {
		BAR_TVREMOTE_KEY_REWIND, BAR_TVREMOTE_KEY_PLAY_PAUSE,
		BAR_TVREMOTE_KEY_FAST_FORWARD
	};
	static const BarTvremoteKey vol_row[] = {
		BAR_TVREMOTE_KEY_VOL_DOWN, BAR_TVREMOTE_KEY_MUTE,
		BAR_TVREMOTE_KEY_VOL_UP
	};
	static const BarTvremoteKey power_row[] = { BAR_TVREMOTE_KEY_POWER };
	gdouble sx, sy, cx = 0.0, cy = 0.0, r = 0.0;
	gdouble h0, top_w;
	guint n = 0, i;

	if (bar_tvremote_has_dpad(kind)) {
		BarTvremoteKey right[2];
		guint n_right = 0;
		gdouble y;

		h0 = (gdouble)DPAD_H0;
		top_w = 88.0;
		if (bar_tvremote_kind_supports(kind, BAR_TVREMOTE_KEY_POWER))
			n = add_button(out, n, BAR_TVREMOTE_KEY_POWER, PAD, PAD,
			               top_w, TOP_H, FALSE);
		if (bar_tvremote_kind_supports(kind, BAR_TVREMOTE_KEY_SETTINGS))
			right[n_right++] = BAR_TVREMOTE_KEY_SETTINGS;
		if (bar_tvremote_kind_supports(kind, BAR_TVREMOTE_KEY_SEARCH))
			right[n_right++] = BAR_TVREMOTE_KEY_SEARCH;
		else if (bar_tvremote_kind_supports(kind, BAR_TVREMOTE_KEY_INPUT))
			right[n_right++] = BAR_TVREMOTE_KEY_INPUT;
		/* Right-aligned, last one at the edge. */
		for (i = 0; i < n_right; i++) {
			gdouble x = NOMINAL_W - PAD - top_w -
			            (gdouble)(n_right - 1 - i) * (top_w + PAD);

			n = add_button(out, n, right[i], x, PAD, top_w, TOP_H,
			               FALSE);
		}

		cx = NOMINAL_W / 2.0;
		cy = PAD + TOP_H + 10.0 + RING_R;
		r = RING_R;
		n = add_button(out, n, BAR_TVREMOTE_KEY_UP, cx - 0.42 * r,
		               cy - r, 0.84 * r, 0.52 * r, FALSE);
		n = add_button(out, n, BAR_TVREMOTE_KEY_DOWN, cx - 0.42 * r,
		               cy + 0.48 * r, 0.84 * r, 0.52 * r, FALSE);
		n = add_button(out, n, BAR_TVREMOTE_KEY_LEFT, cx - r,
		               cy - 0.42 * r, 0.52 * r, 0.84 * r, FALSE);
		n = add_button(out, n, BAR_TVREMOTE_KEY_RIGHT, cx + 0.48 * r,
		               cy - 0.42 * r, 0.52 * r, 0.84 * r, FALSE);
		n = add_button(out, n, BAR_TVREMOTE_KEY_SELECT, cx - 0.4 * r,
		               cy - 0.4 * r, 0.8 * r, 0.8 * r, TRUE);

		y = cy + r + 14.0;
		n = layout_row(kind, nav_row, G_N_ELEMENTS(nav_row), y, out, n);
		y += ROW_STEP;
		n = layout_row(kind, media_row, G_N_ELEMENTS(media_row), y, out, n);
		y += ROW_STEP;
		n = layout_row(kind, vol_row, G_N_ELEMENTS(vol_row), y, out, n);
	} else {
		gdouble y = PAD;

		h0 = (gdouble)CAST_H0;
		n = layout_row(kind, media_row, G_N_ELEMENTS(media_row), y, out, n);
		y += ROW_STEP;
		n = layout_row(kind, vol_row, G_N_ELEMENTS(vol_row), y, out, n);
		y += ROW_STEP;
		n = layout_row(kind, power_row, G_N_ELEMENTS(power_row), y, out, n);
	}

	/* Scale the nominal layout onto the real canvas. */
	sx = (width > 0) ? (gdouble)width / NOMINAL_W : 1.0;
	sy = (height > 0) ? (gdouble)height / h0 : 1.0;
	for (i = 0; i < n; i++) {
		out[i].x *= sx;
		out[i].w *= sx;
		out[i].y *= sy;
		out[i].h *= sy;
	}
	if (ring_cx != NULL)
		*ring_cx = cx * sx;
	if (ring_cy != NULL)
		*ring_cy = cy * sy;
	if (ring_r != NULL)
		*ring_r = r * MIN(sx, sy);
	return n;
}

/**
 * bar_tvremote_hit:
 * @kind: the device kind
 * @width: the canvas width
 * @height: the canvas height
 * @x: a click, canvas-local
 * @y: a click, canvas-local
 *
 * Returns: the #BarTvremoteKey under (@x, @y), or -1
 */
gint
bar_tvremote_hit(BarTvremoteKind kind, gint width, gint height,
                 gdouble x, gdouble y)
{
	BarTvremoteButton buttons[BAR_TVREMOTE_MAX_BUTTONS];
	guint n, i;

	n = bar_tvremote_layout(kind, width, height, buttons, NULL, NULL, NULL);
	for (i = 0; i < n; i++)
		if (x >= buttons[i].x && x < buttons[i].x + buttons[i].w &&
		    y >= buttons[i].y && y < buttons[i].y + buttons[i].h)
			return (gint)buttons[i].key;
	return -1;
}

/* ----------------------------------------------------------------
 * Presentation
 * ---------------------------------------------------------------- */

/**
 * bar_tvremote_format_label:
 * @link: the link state
 * @app: (nullable): the foreground app's display name
 * @volume: the volume in percent, or -1 when unknown
 * @muted: whether it is muted
 *
 * `Netflix · 40' while online; `pair' while the TV is waiting for its
 * prompt to be accepted; %NULL (the bar then shows only the TV glyph)
 * when there is nothing worth saying -- offline, unconfigured, or
 * online with neither an app nor a volume to report.
 *
 * Returns: (transfer full) (nullable): the label
 */
gchar *
bar_tvremote_format_label(BarTvremoteLink link, const gchar *app,
                          gint volume, gboolean muted)
{
	g_autofree gchar *vol = NULL;

	if (link == BAR_TVREMOTE_LINK_PAIRING)
		return g_strdup("pair");
	if (link != BAR_TVREMOTE_LINK_ONLINE)
		return NULL;
	if (muted)
		vol = g_strdup("muted");
	else if (volume >= 0)
		vol = g_strdup_printf("%d", volume);

	if (app != NULL && app[0] != '\0' && vol != NULL)
		return g_strdup_printf("%s \xc2\xb7 %s", app, vol);
	if (app != NULL && app[0] != '\0')
		return g_strdup(app);
	if (vol != NULL)
		return g_strdup_printf("vol %s", vol);
	return NULL;
}

/**
 * bar_tvremote_volume_percent:
 * @index: a volume index
 * @max: the stream's largest index
 *
 * Returns: 0--100, or -1 when either is unknown
 */
gint
bar_tvremote_volume_percent(gint index, gint max)
{
	if (index < 0 || max <= 0)
		return -1;
	if (index >= max)
		return 100;
	return (gint)((index * 100 + max / 2) / max);
}

/**
 * bar_tvremote_parse_hex:
 * @spec: (nullable): a colour setting
 * @rgb: (out caller-allocates) (array fixed-size=3): 0--1 components
 *
 * Only the `#rrggbb' form: the canvas paints a user's literal colour
 * itself, and a role name is resolved by the plugin.
 *
 * Returns: %TRUE when @spec is `#rrggbb'
 */
gboolean
bar_tvremote_parse_hex(const gchar *spec, gdouble rgb[3])
{
	guint i;

	if (spec == NULL || spec[0] != '#' || strlen(spec) != 7)
		return FALSE;
	for (i = 1; i < 7; i++)
		if (!g_ascii_isxdigit(spec[i]))
			return FALSE;
	for (i = 0; i < 3; i++)
		rgb[i] = (gdouble)(g_ascii_xdigit_value(spec[1 + 2 * i]) * 16 +
		                   g_ascii_xdigit_value(spec[2 + 2 * i])) / 255.0;
	return TRUE;
}

/**
 * bar_tvremote_clean_text:
 * @text: (nullable): text from a device, a CLI or a file
 * @max_bytes: the most to keep
 *
 * Makes outside text fit for a label: valid UTF-8, control characters
 * (newlines included) turned to spaces, cut on a character boundary at
 * @max_bytes, and trimmed.
 *
 * Returns: (transfer full): the clean text, never %NULL
 */
gchar *
bar_tvremote_clean_text(const gchar *text, gsize max_bytes)
{
	g_autofree gchar *valid = NULL;
	GString *out;
	const gchar *p;

	if (text == NULL)
		return g_strdup("");
	valid = g_utf8_make_valid(text, -1);
	out = g_string_new(NULL);
	for (p = valid; *p != '\0'; p = g_utf8_next_char(p)) {
		gunichar c = g_utf8_get_char(p);
		const gchar *next = g_utf8_next_char(p);

		if (out->len + (gsize)(next - p) > max_bytes)
			break;
		if (c < 0x20 || c == 0x7f || (c >= 0x80 && c < 0xa0))
			g_string_append_c(out, ' ');
		else
			g_string_append_len(out, p, next - p);
	}
	g_strstrip(out->str);
	out->len = strlen(out->str);
	return g_string_free(out, FALSE);
}

/**
 * bar_tvremote_json_escape:
 * @text: UTF-8 text
 *
 * Escapes @text for the inside of a JSON string literal: the quote,
 * the backslash and every control character.  g_strescape() is NOT
 * this -- it writes octal escapes JSON has no syntax for.
 *
 * Returns: (transfer full): the escaped text, without quotes
 */
gchar *
bar_tvremote_json_escape(const gchar *text)
{
	GString *out;
	const guchar *p;

	out = g_string_new(NULL);
	for (p = (const guchar *)text; *p != '\0'; p++) {
		switch (*p) {
		case '"':  g_string_append(out, "\\\""); break;
		case '\\': g_string_append(out, "\\\\"); break;
		case '\n': g_string_append(out, "\\n"); break;
		case '\r': g_string_append(out, "\\r"); break;
		case '\t': g_string_append(out, "\\t"); break;
		default:
			if (*p < 0x20)
				g_string_append_printf(out, "\\u%04x", (guint)*p);
			else
				g_string_append_c(out, (gchar)*p);
		}
	}
	return g_string_free(out, FALSE);
}
