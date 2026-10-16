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
 * The tvremote widget, offline.
 *
 * The pure helpers (keymaps, escaping, parsers, layout, config, the
 * websocket framing) are compiled straight into this test.  The widget
 * itself is driven through bar.so with a stub host, against:
 *
 *   - a fake `adb' (bash) that logs its argv, answers get-state /
 *     connect / dumpsys / pm list from files, and -- run as a bare
 *     `adb -s X shell' -- appends every stdin line to a log, which is
 *     how the persistent shell is observed;
 *   - a fake `curl' for Roku's ECP;
 *   - a fake websocket server (threads on 127.0.0.1) speaking just
 *     enough of Samsung's and LG's protocols.
 *
 * Nothing reaches the real network.
 */

#include <glib.h>
#include <glib/gstdio.h>
#include <gio/gio.h>
#include <signal.h>
#include <string.h>
#include <sys/stat.h>
#include <xkbcommon/xkbcommon-keysyms.h>
#include <linux/input-event-codes.h>

#define BAR_HARNESS_MODULE GOWL_TEST_BAR_MODULE
#include "bar-widget-harness.h"
#include "gowl-enums.h"

#ifndef GOWL_TEST_BAR_MODULE
#define GOWL_TEST_BAR_MODULE "build/release/modules/bar.so"
#endif
#ifndef GOWL_TEST_FIXTURES
#define GOWL_TEST_FIXTURES "tests/fixtures/bar"
#endif

#include "../modules/bar/bar-tvremote-model.c"
#include "../modules/bar/bar-tvremote-config.c"
#include "../modules/bar/bar-tvremote-json.c"
#include "../modules/bar/bar-tvremote-ws.c"
#include "../modules/bar/bar-tvremote-backend.h"

#define REG "bar_register_tvremote_plugins"

/* ── Helpers ─────────────────────────────────────────────────────── */

static gchar *
fixture(const gchar *name)
{
	gchar *path, *text = NULL;

	path = g_build_filename(GOWL_TEST_FIXTURES, "tvremote", name, NULL);
	g_assert_true(g_file_get_contents(path, &text, NULL, NULL));
	g_free(path);
	return text;
}

static gchar *
fixture_dir(void)
{
	return g_build_filename(GOWL_TEST_FIXTURES, "tvremote", NULL);
}

static gchar *
read_or_empty(const gchar *path)
{
	gchar *text = NULL;

	if (!g_file_get_contents(path, &text, NULL, NULL))
		return g_strdup("");
	return text;
}

static guint
count_lines(const gchar *path)
{
	g_autofree gchar *t = read_or_empty(path);
	guint n = 0;
	const gchar *p;

	for (p = t; *p != '\0'; p++)
		if (*p == '\n')
			n++;
	return n;
}

/* Wait (up to ~2 s) for @path to hold at least @n lines: the fake
   adb shell writes its log asynchronously. */
static gboolean
wait_lines(const gchar *path, guint n)
{
	guint i;

	for (i = 0; i < 100; i++) {
		if (count_lines(path) >= n)
			return TRUE;
		g_usleep(20000);
	}
	return FALSE;
}

static gboolean
file_has(const gchar *path, const gchar *needle)
{
	g_autofree gchar *t = read_or_empty(path);

	return strstr(t, needle) != NULL;
}

static gint
panel_find(GowlBarPanel *panel, const gchar *id)
{
	guint i;

	for (i = 0; i < gowl_bar_panel_n_items(panel); i++)
		if (g_strcmp0(gowl_bar_panel_item_get_id(
		        gowl_bar_panel_get_item(panel, i)), id) == 0)
			return (gint)i;
	return -1;
}

/* Write @yaml as the widget's device list at mode @mode. */
static gchar *
write_config(BarHarness *h, const gchar *yaml, guint mode)
{
	gchar *path = bar_harness_path(h, "tvremote.yaml");

	g_assert_true(g_file_set_contents(path, yaml, -1, NULL));
	g_assert_cmpint(g_chmod(path, mode), ==, 0);
	return path;
}

/* Install the fake adb and curl; returns the scratch dir they log to. */
static void
install_fakes(BarHarness *h)
{
	g_autofree gchar *fix = fixture_dir();
	g_autofree gchar *adb_body = NULL;
	g_autofree gchar *curl_body = NULL;
	g_autofree gchar *adb = NULL;
	g_autofree gchar *curl = NULL;

	adb_body = g_strdup_printf(
		"D='%s'; FIX='%s'\n"
		"echo \"$*\" >> \"$D/adb.argv\"\n"
		"serial=''\n"
		"if [ \"$1\" = -s ]; then serial=\"$2\"; shift 2; fi\n"
		"case \"$1\" in\n"
		"get-state)\n"
		"  st=$(cat \"$D/adb-state\" 2>/dev/null || echo device)\n"
		"  case \"$st\" in\n"
		"  device) echo device; exit 0;;\n"
		"  notfound) echo \"error: device '$serial' not found\" >&2; exit 1;;\n"
		"  *) echo \"error: device $st.\" >&2; exit 1;;\n"
		"  esac;;\n"
		"connect)\n"
		"  if [ -f \"$D/connect\" ]; then cat \"$D/connect\";"
		" else echo \"connected to $2\"; fi; exit 0;;\n"
		"disconnect) echo \"disconnected $2\"; exit 0;;\n"
		"shell)\n"
		"  shift\n"
		"  if [ $# -eq 0 ]; then\n"
		"    echo \"$$\" >> \"$D/spawns\"\n"
		"    while IFS= read -r line; do"
		" printf '%%s\\n' \"$line\" >> \"$D/shell.log\"; done\n"
		"    exit 0\n"
		"  fi\n"
		"  case \"$*\" in\n"
		"  *dumpsys*) cat \"$D/status.txt\";;\n"
		"  *'pm list'*) cat \"$FIX/pm-third.txt\"; echo ---;"
		" cat \"$FIX/pm-all.txt\";;\n"
		"  *LEANBACK*) if [ -f \"$D/no-leanback\" ]; then"
		" echo '** No activities found to run, monkey aborted.';"
		" else echo 'Events injected: 1'; fi;;\n"
		"  monkey*) echo 'Events injected: 1';;\n"
		"  esac\n"
		"  exit 0;;\n"
		"esac\n"
		"exit 0", h->tmp, fix);
	adb = bar_harness_script(h, "fake-adb", adb_body);

	curl_body = g_strdup_printf(
		"D='%s'; FIX='%s'\n"
		"url=\"${@: -1}\"; post=0\n"
		"for a in \"$@\"; do [ \"$a\" = POST ] && post=1; done\n"
		"echo \"$post $url\" >> \"$D/curl.log\"\n"
		"if [ -f \"$D/roku403\" ]; then printf '\\n403'; exit 0; fi\n"
		"case \"$url\" in\n"
		"*/query/device-info) cat \"$FIX/roku-device-info.xml\";;\n"
		"*/query/active-app) cat \"$FIX/roku-active.xml\";;\n"
		"*/query/apps) cat \"$FIX/roku-apps.xml\";;\n"
		"esac\n"
		"printf '\\n200'", h->tmp, fix);
	curl = bar_harness_script(h, "fake-curl", curl_body);

	bar_harness_set(h, "adb", adb);
	bar_harness_set(h, "curl", curl);
	/* The CLI kinds stay absent unless a test wants them. */
	bar_harness_set(h, "catt", "/nonexistent/catt");
	bar_harness_set(h, "atvremote", "/nonexistent/atvremote");
}

static void
set_status_fixture(BarHarness *h, const gchar *name)
{
	g_autofree gchar *text = fixture(name);
	g_autofree gchar *path = bar_harness_path(h, "status.txt");

	g_assert_true(g_file_set_contents(path, text, -1, NULL));
}

static void
set_flag(BarHarness *h, const gchar *name, const gchar *contents)
{
	g_autofree gchar *path = bar_harness_path(h, name);
	GError *error = NULL;

	if (contents == NULL) {
		g_unlink(path);
	} else {
		g_file_set_contents(path, contents, -1, &error);
		g_assert_no_error(error);
	}
}

static gboolean
widget_up(BarHarness *h, const gchar *yaml)
{
	g_autofree gchar *cfg = NULL;

	if (!bar_harness_up(h, REG, "tvremote"))
		return FALSE;
	install_fakes(h);
	set_status_fixture(h, "status-netflix.txt");
	if (yaml != NULL) {
		cfg = write_config(h, yaml, 0600);
		bar_harness_set(h, "config", cfg);
	} else {
		cfg = bar_harness_path(h, "tvremote.yaml");
		bar_harness_set(h, "config", cfg);
	}
	bar_harness_activate(h);
	return TRUE;
}

/* Paint the pad (records its size for the hit-test) and click the
   centre of @key. */
static void
click_key(BarHarness *h, BarTvremoteKind kind, BarTvremoteKey key)
{
	BarTvremoteButton b[BAR_TVREMOTE_MAX_BUTTONS];
	cairo_surface_t *surf;
	cairo_t *cr;
	guint n, i;
	gint w = 330, hgt = bar_tvremote_canvas_height(kind);

	surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, hgt);
	cr = cairo_create(surf);
	gowl_bar_plugin_panel_canvas(h->plugin, "pad", cr, h->host->theme, w,
	                             hgt);
	cairo_destroy(cr);
	cairo_surface_destroy(surf);

	n = bar_tvremote_layout(kind, w, hgt, b, NULL, NULL, NULL);
	for (i = 0; i < n; i++)
		if (b[i].key == key) {
			gowl_bar_plugin_panel_action(h->plugin, "pad",
				(gint)(b[i].x + b[i].w / 2.0),
				b[i].y + b[i].h / 2.0, BTN_LEFT);
			return;
		}
	g_assert_not_reached();
}

static gint
pad_index(BarHarness *h)
{
	g_autoptr(GowlBarPanel) panel = bar_harness_panel(h);

	return panel_find(panel, "pad");
}

/* ── Keymaps ─────────────────────────────────────────────────────── */

/* Catches: a Fire TV key mapped to the wrong KEYCODE (the TV does
   something else), or a key the remote draws that sends nothing. */
static void
test_keymap_firetv(void)
{
	static const struct { BarTvremoteKey key; gint code; } want[] = {
		{ BAR_TVREMOTE_KEY_UP, 19 }, { BAR_TVREMOTE_KEY_DOWN, 20 },
		{ BAR_TVREMOTE_KEY_LEFT, 21 }, { BAR_TVREMOTE_KEY_RIGHT, 22 },
		{ BAR_TVREMOTE_KEY_SELECT, 23 }, { BAR_TVREMOTE_KEY_BACK, 4 },
		{ BAR_TVREMOTE_KEY_HOME, 3 }, { BAR_TVREMOTE_KEY_MENU, 82 },
		{ BAR_TVREMOTE_KEY_PLAY_PAUSE, 85 },
		{ BAR_TVREMOTE_KEY_REWIND, 89 },
		{ BAR_TVREMOTE_KEY_FAST_FORWARD, 90 },
		{ BAR_TVREMOTE_KEY_VOL_UP, 24 }, { BAR_TVREMOTE_KEY_VOL_DOWN, 25 },
		{ BAR_TVREMOTE_KEY_MUTE, 164 }, { BAR_TVREMOTE_KEY_POWER, 26 },
		{ BAR_TVREMOTE_KEY_SLEEP, 223 }, { BAR_TVREMOTE_KEY_WAKEUP, 224 },
		{ BAR_TVREMOTE_KEY_SEARCH, 84 }
	};
	g_autofree gchar *lp = NULL;
	guint i;

	for (i = 0; i < G_N_ELEMENTS(want); i++)
		g_assert_cmpint(bar_tvremote_android_keycode(
			BAR_TVREMOTE_KIND_FIRETV, want[i].key), ==, want[i].code);
	/* Fire OS has no settings / input key a shell can press... */
	g_assert_cmpint(bar_tvremote_android_keycode(BAR_TVREMOTE_KIND_FIRETV,
		BAR_TVREMOTE_KEY_SETTINGS), ==, -1);
	g_assert_cmpint(bar_tvremote_android_keycode(BAR_TVREMOTE_KIND_FIRETV,
		BAR_TVREMOTE_KEY_INPUT), ==, -1);
	/* ...Android TV has both. */
	g_assert_cmpint(bar_tvremote_android_keycode(BAR_TVREMOTE_KIND_ANDROIDTV,
		BAR_TVREMOTE_KEY_SETTINGS), ==, 176);
	g_assert_cmpint(bar_tvremote_android_keycode(BAR_TVREMOTE_KIND_ROKU,
		BAR_TVREMOTE_KEY_UP), ==, -1);

	lp = bar_tvremote_adb_key_command(BAR_TVREMOTE_KIND_FIRETV,
	                                  BAR_TVREMOTE_KEY_SELECT, TRUE);
	g_assert_cmpstr(lp, ==, "input keyevent --longpress 23");
}

/* Catches: a kind whose drawn button has no protocol command. */
static void
test_keymap_every_kind(void)
{
	guint k, key;

	for (k = 0; k < BAR_TVREMOTE_N_KINDS; k++) {
		for (key = 0; key < BAR_TVREMOTE_N_KEYS; key++) {
			if (!bar_tvremote_kind_supports((BarTvremoteKind)k,
			                                (BarTvremoteKey)key))
				continue;
			g_assert_nonnull(bar_tvremote_key_name((BarTvremoteKey)key));
			g_assert_cmpint(bar_tvremote_key_from_name(
				bar_tvremote_key_name((BarTvremoteKey)key)), ==,
				(gint)key);
		}
		g_assert_cmpint(bar_tvremote_kind_from_name(
			bar_tvremote_kind_name((BarTvremoteKind)k)), ==, (gint)k);
	}
	g_assert_cmpstr(bar_tvremote_roku_key(BAR_TVREMOTE_KEY_PLAY_PAUSE), ==,
	                "Play");
	g_assert_cmpstr(bar_tvremote_samsung_key(BAR_TVREMOTE_KEY_SELECT), ==,
	                "KEY_ENTER");
	g_assert_cmpstr(bar_tvremote_lg_key(BAR_TVREMOTE_KEY_UP), ==, "btn:UP");
	g_assert_cmpstr(bar_tvremote_lg_key(BAR_TVREMOTE_KEY_VOL_UP), ==,
	                "ssap:ssap://audio/volumeUp");
	g_assert_cmpint(bar_tvremote_kind_from_name("fire-tv"), ==,
	                BAR_TVREMOTE_KIND_FIRETV);
	g_assert_cmpint(bar_tvremote_kind_from_name("toaster"), ==,
	                BAR_TVREMOTE_KIND_UNKNOWN);
}

/* ── adb text escaping ───────────────────────────────────────────── */

/* Run what the DEVICE shell would: `printf %s <arg as written>'. */
static gchar *
device_shell_parse(const gchar *quoted)
{
	g_autofree gchar *script = g_strconcat("printf %s ", quoted, NULL);
	const gchar *argv[] = { "/bin/sh", "-c", script, NULL };
	gchar *out = NULL;
	gint status = 0;

	g_assert_true(g_spawn_sync(NULL, (gchar **)argv, NULL, 0, NULL, NULL,
	                           &out, NULL, &status, NULL));
	g_assert_cmpint(status, ==, 0);
	return out;
}

/* Android's `input text': `%s' (and only that) becomes a space. */
static void
android_decode(GString *into, const gchar *arg)
{
	const gchar *p;

	for (p = arg; *p != '\0'; p++) {
		if (p[0] == '%' && p[1] == 's') {
			g_string_append_c(into, ' ');
			p++;
			continue;
		}
		g_string_append_c(into, *p);
	}
}

/* Typed text through all three layers -- the device shell, input
   text's %s rule, the concatenation of several commands -- must come
   out as the ASCII the user typed.  Catches: shell metacharacters
   reaching the device shell unquoted (a `$(reboot)' that runs), a
   quote that ends the argument early, a literal `%s' typed as a space,
   and non-ASCII silently mangled instead of counted. */
static void
test_adb_text_escaping(void)
{
	static const struct {
		const gchar *in;
		const gchar *out;
		guint dropped;
	} cases[] = {
		{ "hello world", "hello world", 0 },
		{ "it's", "it's", 0 },
		{ "$(reboot); `id` & | \"x\" \\ > < * ?", "$(reboot); `id` & | \"x\" \\ > < * ?", 0 },
		{ "100%sure", "100%sure", 0 },
		{ "50% off %s", "50% off %s", 0 },
		{ "caf\xc3\xa9 \xe2\x98\x95", "caf ", 2 },
		{ "line\nbreak\t!", "linebreak!", 2 },
		{ "''", "''", 0 }
	};
	guint c;

	for (c = 0; c < G_N_ELEMENTS(cases); c++) {
		g_auto(GStrv) lines = NULL;
		GString *typed = g_string_new(NULL);
		guint dropped = 99, i;

		lines = bar_tvremote_adb_text_commands(cases[c].in, &dropped);
		g_assert_cmpuint(dropped, ==, cases[c].dropped);
		for (i = 0; lines[i] != NULL; i++) {
			g_autofree gchar *arg = NULL;

			g_assert_true(g_str_has_prefix(lines[i], "input text '"));
			/* No line can carry a second command. */
			g_assert_null(strchr(lines[i], '\n'));
			arg = device_shell_parse(lines[i] + strlen("input text "));
			/* Spaces travel as %s, never raw. */
			g_assert_null(strchr(arg, ' '));
			android_decode(typed, arg);
		}
		g_assert_cmpstr(typed->str, ==, cases[c].out);
		g_string_free(typed, TRUE);
	}

	{
		g_auto(GStrv) lines = NULL;
		guint dropped = 0;

		lines = bar_tvremote_adb_text_commands("hi there", NULL);
		g_assert_cmpstr(lines[0], ==, "input text 'hi%sthere'");
		g_strfreev(g_steal_pointer(&lines));
		lines = bar_tvremote_adb_text_commands("\xff\xfe bad", &dropped);
		g_assert_null(lines[0]);
		g_assert_cmpuint(dropped, >, 0);
	}
}

/* ── adb parsers ─────────────────────────────────────────────────── */

/* Catches: the focused app read from the wrong line (a notification
   shade named as the app), Fire OS 7's nested ActivityRecord missed,
   and a hostile `package' with a `;' accepted. */
static void
test_adb_focus(void)
{
	static const struct { const gchar *file; const gchar *pkg; } cases[] = {
		{ "focus-fireos7.txt", "com.netflix.ninja" },
		{ "focus-fireos8.txt", "com.amazon.firebat" },
		{ "focus-androidtv12.txt", "com.google.android.youtube.tv" },
		{ "focus-launcher.txt", "com.amazon.tv.launcher" },
		{ "focus-null.txt", NULL },
		{ "focus-hostile.txt", NULL }
	};
	guint i;

	for (i = 0; i < G_N_ELEMENTS(cases); i++) {
		g_autofree gchar *text = fixture(cases[i].file);
		g_autofree gchar *pkg = bar_tvremote_parse_focus(text);

		g_assert_cmpstr(pkg, ==, cases[i].pkg);
	}
	g_assert_null(bar_tvremote_parse_focus(NULL));
	g_assert_null(bar_tvremote_parse_focus(""));
	g_assert_cmpstr(bar_tvremote_app_friendly_name(BAR_TVREMOTE_KIND_FIRETV,
		"com.amazon.firebat"), ==, "Prime Video");
	g_assert_cmpstr(bar_tvremote_app_friendly_name(BAR_TVREMOTE_KIND_FIRETV,
		"com.amazon.tv.launcher"), ==, "Home");
	g_assert_null(bar_tvremote_app_friendly_name(BAR_TVREMOTE_KIND_FIRETV,
		"com.example.sideloaded"));
}

/* Catches: duplicate or injected packages in the list, Prime Video
   (a SYSTEM app on Fire OS) left out, the launcher offered as an app,
   and an unknown package without even its id as a name. */
static void
test_adb_pm_and_apps(void)
{
	g_autofree gchar *third = fixture("pm-third.txt");
	g_autofree gchar *all = fixture("pm-all.txt");
	g_autoptr(GPtrArray) pkgs = bar_tvremote_parse_pm_list(third);
	g_autoptr(GPtrArray) apps = NULL;
	gboolean prime = FALSE, home = FALSE, side = FALSE, settings = FALSE;
	guint i;

	g_assert_cmpuint(pkgs->len, ==, 4);
	for (i = 0; i < pkgs->len; i++)
		g_assert_null(strchr(g_ptr_array_index(pkgs, i), ';'));
	g_assert_cmpstr(g_ptr_array_index(pkgs, 0), ==, "com.example.sideloaded");

	apps = bar_tvremote_adb_apps(BAR_TVREMOTE_KIND_FIRETV, third, all);
	for (i = 0; i < apps->len; i++) {
		BarTvremoteApp *a = g_ptr_array_index(apps, i);

		if (strcmp(a->id, "com.amazon.firebat") == 0)
			prime = (strcmp(a->name, "Prime Video") == 0);
		if (strcmp(a->name, "Home") == 0)
			home = TRUE;
		if (strcmp(a->id, "com.example.sideloaded") == 0)
			side = (strcmp(a->name, a->id) == 0);
		if (strcmp(a->name, "Settings") == 0)
			settings = TRUE;
		g_assert_true(bar_tvremote_valid_package(a->id));
	}
	g_assert_true(prime);
	g_assert_false(home);
	g_assert_true(side);
	g_assert_true(settings);
	g_assert_false(bar_tvremote_valid_package("com.x;rm"));
	g_assert_false(bar_tvremote_valid_package("nodots"));
	g_assert_false(bar_tvremote_valid_package(".com.x"));
	g_assert_null(bar_tvremote_adb_launch_command("a.b;reboot", TRUE));
}

/* Catches: an `unauthorized' read as offline (the user is never told
   to accept the prompt), and the `not found' error mistaken for
   `device' because it contains the word. */
static void
test_adb_states(void)
{
	g_assert_cmpint(bar_tvremote_parse_adb_state("device\n", ""), ==,
	                BAR_TVREMOTE_ADB_DEVICE);
	g_assert_cmpint(bar_tvremote_parse_adb_state("",
		"error: device unauthorized.\nThis adb server's $ADB_VENDOR_KEYS "
		"is not set"), ==, BAR_TVREMOTE_ADB_UNAUTHORIZED);
	g_assert_cmpint(bar_tvremote_parse_adb_state("", "error: device offline"),
	                ==, BAR_TVREMOTE_ADB_OFFLINE);
	g_assert_cmpint(bar_tvremote_parse_adb_state("",
		"error: device still authorizing"), ==,
		BAR_TVREMOTE_ADB_AUTHORIZING);
	g_assert_cmpint(bar_tvremote_parse_adb_state("",
		"error: device '1.2.3.4:5555' not found"), ==,
		BAR_TVREMOTE_ADB_ABSENT);
	g_assert_cmpint(bar_tvremote_parse_adb_state(NULL, NULL), ==,
	                BAR_TVREMOTE_ADB_ABSENT);

	g_assert_cmpint(bar_tvremote_parse_adb_connect(
		"connected to 192.168.1.20:5555\n", NULL), ==,
		BAR_TVREMOTE_ADB_DEVICE);
	g_assert_cmpint(bar_tvremote_parse_adb_connect(
		"already connected to 192.168.1.20:5555\n", NULL), ==,
		BAR_TVREMOTE_ADB_DEVICE);
	g_assert_cmpint(bar_tvremote_parse_adb_connect(
		"failed to connect to '192.168.1.20:5555': Connection refused\n",
		NULL), ==, BAR_TVREMOTE_ADB_REFUSED);
	g_assert_cmpint(bar_tvremote_parse_adb_connect(
		"failed to authenticate to 192.168.1.20:5555\n", NULL), ==,
		BAR_TVREMOTE_ADB_UNAUTHORIZED);

	{
		g_autofree gchar *s4 = bar_tvremote_adb_serial("10.0.0.5", 0);
		g_autofree gchar *s6 = bar_tvremote_adb_serial("fe80::1", 5555);

		g_assert_cmpstr(s4, ==, "10.0.0.5:5555");
		g_assert_cmpstr(s6, ==, "[fe80::1]:5555");
	}
}

/* Catches: the default-output index (pinned at 15) read instead of
   HDMI's, a neighbouring stream's volume read, and mute ignored. */
static void
test_adb_audio(void)
{
	g_autofree gchar *a11 = fixture("audio-android11.txt");
	g_autofree gchar *fo7 = fixture("audio-fireos7.txt");
	g_autofree gchar *mut = fixture("audio-muted.txt");
	g_autofree gchar *media = fixture("media-session.txt");
	g_autofree gchar *title = NULL;
	gint idx, max;
	gboolean muted;

	g_assert_true(bar_tvremote_parse_audio_volume(a11, &idx, &max, &muted));
	g_assert_cmpint(idx, ==, 7);
	g_assert_cmpint(max, ==, 15);
	g_assert_false(muted);
	g_assert_cmpint(bar_tvremote_volume_percent(idx, max), ==, 47);

	g_assert_true(bar_tvremote_parse_audio_volume(fo7, &idx, &max, &muted));
	g_assert_cmpint(idx, ==, 6);

	g_assert_true(bar_tvremote_parse_audio_volume(mut, &idx, &max, &muted));
	g_assert_true(muted);
	g_assert_cmpint(bar_tvremote_volume_percent(idx, max), ==, 40);

	g_assert_false(bar_tvremote_parse_audio_volume("nothing", &idx, &max,
	                                               &muted));
	g_assert_cmpint(bar_tvremote_volume_percent(-1, 15), ==, -1);

	title = bar_tvremote_parse_media_title(media);
	g_assert_cmpstr(title, ==, "Stranger Things");
	g_assert_null(bar_tvremote_parse_media_title(
		"metadata: size=0, description=null, null, null"));
}

/* ── Roku / HTTP ─────────────────────────────────────────────────── */

/* Catches: an id with shell or URL metacharacters accepted, an XML
   entity left escaped in a name, and a `/' in typed text starting a
   new URL path segment. */
static void
test_roku_parsing(void)
{
	g_autofree gchar *apps_xml = fixture("roku-apps.xml");
	g_autofree gchar *act = fixture("roku-active.xml");
	g_autofree gchar *home = fixture("roku-active-home.xml");
	g_autofree gchar *info = fixture("roku-device-info.xml");
	g_autoptr(GPtrArray) apps = bar_tvremote_roku_parse_apps(apps_xml);
	g_autofree gchar *a1 = NULL, *a2 = NULL, *name = NULL, *power = NULL;
	g_autofree gchar *enc = NULL, *url = NULL, *url6 = NULL;
	g_auto(GStrv) paths = NULL;
	gboolean xbox = FALSE;
	guint i;

	g_assert_cmpuint(apps->len, ==, 5);
	for (i = 0; i < apps->len; i++) {
		BarTvremoteApp *a = g_ptr_array_index(apps, i);

		g_assert_true(bar_tvremote_valid_app_id(a->id));
		if (strcmp(a->id, "tvinput.hdmi1") == 0)
			xbox = (strcmp(a->name, "Xbox & Co") == 0);
	}
	g_assert_true(xbox);

	a1 = bar_tvremote_roku_parse_active(act);
	a2 = bar_tvremote_roku_parse_active(home);
	g_assert_cmpstr(a1, ==, "Netflix");
	g_assert_cmpstr(a2, ==, "Roku");
	name = bar_tvremote_roku_parse_field(info, "user-device-name");
	power = bar_tvremote_roku_parse_field(info, "power-mode");
	g_assert_cmpstr(name, ==, "Living Room Roku");
	g_assert_cmpstr(power, ==, "PowerOn");
	g_assert_null(bar_tvremote_roku_parse_field("<a><b>x</a>", "c"));
	g_assert_null(bar_tvremote_roku_parse_active("<active-app>\xff</x>"));
	g_ptr_array_unref(bar_tvremote_roku_parse_apps("not xml at all <"));

	enc = bar_tvremote_urlencode("a b/\xc3\xa9?&");
	g_assert_cmpstr(enc, ==, "a%20b%2F%C3%A9%3F%26");
	url = bar_tvremote_http_url("192.168.1.134", 8060, "/keypress/Up");
	g_assert_cmpstr(url, ==, "http://192.168.1.134:8060/keypress/Up");
	url6 = bar_tvremote_http_url("fe80::1", 8060, "/x");
	g_assert_cmpstr(url6, ==, "http://[fe80::1]:8060/x");

	paths = bar_tvremote_roku_text_paths("a /\xc3\xa9\n");
	g_assert_cmpuint(g_strv_length(paths), ==, 4);
	g_assert_cmpstr(paths[0], ==, "/keypress/Lit_a");
	g_assert_cmpstr(paths[1], ==, "/keypress/Lit_%20");
	g_assert_cmpstr(paths[2], ==, "/keypress/Lit_%2F");
	g_assert_cmpstr(paths[3], ==, "/keypress/Lit_%C3%A9");

	g_assert_true(bar_tvremote_valid_host("192.168.1.20"));
	g_assert_true(bar_tvremote_valid_host("tv.lan"));
	g_assert_false(bar_tvremote_valid_host("1.2.3.4/evil"));
	g_assert_false(bar_tvremote_valid_host("-oProxy"));
	g_assert_false(bar_tvremote_valid_host("a b"));
	g_assert_false(bar_tvremote_valid_host("user@host"));
	g_assert_false(bar_tvremote_valid_host(""));
	g_assert_false(bar_tvremote_valid_app_id("12;rm"));
	g_assert_false(bar_tvremote_valid_app_id("../x"));
}

/* ── Discovery ───────────────────────────────────────────────────── */

/* Catches: case-sensitive header matching (LG writes LOCATION), an
   IPv6 location mangled, and a non-SSDP datagram accepted. */
static void
test_discover_parsing(void)
{
	g_autofree gchar *roku = fixture("ssdp-roku.txt");
	g_autofree gchar *lg = fixture("ssdp-lg.txt");
	g_autofree gchar *sam = fixture("ssdp-samsung.txt");
	g_autofree gchar *route = fixture("proc-net-route.txt");
	g_autofree gchar *mdns = fixture("mdns.txt");
	g_autofree gchar *iface = NULL;
	g_autoptr(GPtrArray) hosts = NULL;
	g_autoptr(GPtrArray) found = NULL;
	gchar *loc = NULL, *st = NULL, *srv = NULL, *host = NULL;
	gint port;
	guint32 base;
	guint prefix;

	g_assert_true(bar_tvremote_ssdp_parse(roku, &loc, &st, &srv));
	g_assert_cmpstr(loc, ==, "http://192.168.1.134:8060/");
	g_assert_cmpint(bar_tvremote_ssdp_kind(st, srv), ==,
	                BAR_TVREMOTE_KIND_ROKU);
	g_assert_true(bar_tvremote_url_host_port(loc, &host, &port));
	g_assert_cmpstr(host, ==, "192.168.1.134");
	g_assert_cmpint(port, ==, 8060);
	g_free(loc); g_free(st); g_free(srv); g_free(host);

	g_assert_true(bar_tvremote_ssdp_parse(lg, &loc, &st, &srv));
	g_assert_cmpint(bar_tvremote_ssdp_kind(st, srv), ==,
	                BAR_TVREMOTE_KIND_LG);
	g_free(loc); g_free(st); g_free(srv);

	g_assert_true(bar_tvremote_ssdp_parse(sam, &loc, &st, &srv));
	g_assert_cmpint(bar_tvremote_ssdp_kind(st, srv), ==,
	                BAR_TVREMOTE_KIND_SAMSUNG);
	g_assert_true(bar_tvremote_url_host_port(loc, &host, &port));
	g_assert_cmpstr(host, ==, "fe80::1");
	g_assert_cmpint(port, ==, 7678);
	g_free(loc); g_free(st); g_free(srv); g_free(host);

	g_assert_false(bar_tvremote_ssdp_parse("M-SEARCH * HTTP/1.1\r\n\r\n",
	                                       NULL, NULL, NULL));
	g_assert_false(bar_tvremote_ssdp_parse("HTTP/1.1 200 OK\r\nST: x\r\n\r\n",
	                                       NULL, NULL, NULL));
	g_assert_false(bar_tvremote_url_host_port("file:///etc/passwd", &host,
	                                          &port));

	/* The lower-metric default route wins. */
	iface = bar_tvremote_route_iface(route);
	g_assert_cmpstr(iface, ==, "enp2s0");

	g_assert_true(bar_tvremote_parse_cidr("192.168.5.77/24", &base, &prefix));
	g_assert_cmpuint(base, ==, 0xc0a80500u);
	g_assert_false(bar_tvremote_parse_cidr("300.1.1.1/24", &base, &prefix));
	g_assert_false(bar_tvremote_parse_cidr("1.2.3.4/33", &base, &prefix));
	{
		g_autofree gchar *wide = bar_tvremote_subnet_for(0x0a01020au,
		                                                 0xffff0000u);
		g_autofree gchar *narrow = bar_tvremote_subnet_for(0xc0a80119u,
		                                                   0xffffffc0u);

		/* A /16 is narrowed to our own /24, never scanned whole. */
		g_assert_cmpstr(wide, ==, "10.1.2.0/24");
		g_assert_cmpstr(narrow, ==, "192.168.1.0/26");
	}
	hosts = bar_tvremote_subnet_hosts("192.168.1.0/24");
	g_assert_cmpuint(hosts->len, ==, 254);
	g_assert_cmpstr(g_ptr_array_index(hosts, 0), ==, "192.168.1.1");
	g_assert_null(bar_tvremote_subnet_hosts("10.0.0.0/16"));

	found = bar_tvremote_parse_mdns(mdns);
	g_assert_cmpuint(found->len, ==, 2);
	g_assert_cmpstr(g_ptr_array_index(found, 0), ==, "192.168.1.44:5555");
}

typedef GPtrArray *(*ScanPortsFn)(GPtrArray *, const gint *, guint, gint,
                                  guint);

/* Catches: an open port missed or a closed one reported -- against a
   listener on loopback, never the LAN. */
static void
test_discover_probe(void)
{
	BarHarness h;
	ScanPortsFn scan;
	GSocket *listener;
	GSocketAddress *addr;
	GInetAddress *lo;
	g_autoptr(GPtrArray) hosts = NULL;
	g_autoptr(GPtrArray) found = NULL;
	gint ports[2];

	if (!bar_harness_up(&h, REG, NULL))
		return;
	scan = (ScanPortsFn)bar_harness_sym(&h, "bar_tvremote_scan_ports");
	g_assert_nonnull(scan);

	listener = g_socket_new(G_SOCKET_FAMILY_IPV4, G_SOCKET_TYPE_STREAM,
	                        G_SOCKET_PROTOCOL_TCP, NULL);
	lo = g_inet_address_new_loopback(G_SOCKET_FAMILY_IPV4);
	addr = g_inet_socket_address_new(lo, 0);
	g_assert_true(g_socket_bind(listener, addr, TRUE, NULL));
	g_assert_true(g_socket_listen(listener, NULL));
	g_object_unref(addr);
	addr = g_socket_get_local_address(listener, NULL);
	ports[0] = g_inet_socket_address_get_port(G_INET_SOCKET_ADDRESS(addr));
	ports[1] = 1;      /* nothing there */

	hosts = g_ptr_array_new_with_free_func(g_free);
	g_ptr_array_add(hosts, g_strdup("127.0.0.1"));
	found = scan(hosts, ports, 2, 300, 8);
	g_assert_cmpuint(found->len, ==, 1);
	g_assert_cmpint(((BarTvremoteFound *)g_ptr_array_index(found, 0))->port,
	                ==, ports[0]);

	g_object_unref(addr);
	g_object_unref(lo);
	g_object_unref(listener);
	bar_harness_down(&h);
}

/* ── Websocket framing ───────────────────────────────────────────── */

/* Catches: a wrong accept-key computation (every real TV would be
   rejected as "not a websocket server"). RFC 6455 section 1.3. */
static void
test_ws_accept_key(void)
{
	g_autofree gchar *acc = bar_tvremote_ws_accept_key(
		"dGhlIHNhbXBsZSBub25jZQ==");
	g_autofree gchar *req = bar_tvremote_ws_handshake("10.0.0.5", 8002,
		"/api/v2/x?name=Z293bA%3D%3D", "abc");
	GError *error = NULL;

	g_assert_cmpstr(acc, ==, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
	g_assert_nonnull(strstr(req, "GET /api/v2/x?name=Z293bA%3D%3D HTTP/1.1\r\n"));
	g_assert_nonnull(strstr(req, "Host: 10.0.0.5:8002\r\n"));
	g_assert_nonnull(strstr(req, "Sec-WebSocket-Version: 13\r\n"));

	g_assert_true(bar_tvremote_ws_check_response(
		"HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
		"Connection: Upgrade\r\n"
		"sec-websocket-accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n\r\n",
		"dGhlIHNhbXBsZSBub25jZQ==", &error));
	g_assert_no_error(error);
	g_assert_false(bar_tvremote_ws_check_response(
		"HTTP/1.1 101 Switching Protocols\r\n"
		"Sec-WebSocket-Accept: wrong=\r\n\r\n",
		"dGhlIHNhbXBsZSBub25jZQ==", &error));
	g_clear_error(&error);
	g_assert_false(bar_tvremote_ws_check_response(
		"HTTP/1.1 403 Forbidden\r\n\r\n", "x", &error));
	g_clear_error(&error);
}

/* Catches: an unmasked client frame (servers drop the connection),
   wrong masking, wrong extended lengths, and a short buffer parsed as
   a frame.  The masked "Hello" is RFC 6455 section 5.7's example. */
static void
test_ws_frames(void)
{
	static const guint8 mask[4] = { 0x37, 0xfa, 0x21, 0x3d };
	static const guint8 masked_hello[] = {
		0x81, 0x85, 0x37, 0xfa, 0x21, 0x3d, 0x7f, 0x9f, 0x4d, 0x51, 0x58
	};
	static const guint8 plain_hello[] = {
		0x81, 0x05, 0x48, 0x65, 0x6c, 0x6c, 0x6f
	};
	g_autoptr(GByteArray) f1 = NULL;
	g_autoptr(GByteArray) f2 = NULL;
	guint8 op = 0;
	gboolean fin = FALSE, masked = FALSE;
	GBytes *payload = NULL;
	gsize sizes[] = { 125, 126, 300, 65535, 70000 };
	guint i;

	f1 = bar_tvremote_ws_frame(BAR_TVREMOTE_WS_TEXT, (const guint8 *)"Hello",
	                           5, mask);
	g_assert_cmpmem(f1->data, f1->len, masked_hello, sizeof(masked_hello));
	f2 = bar_tvremote_ws_frame(BAR_TVREMOTE_WS_TEXT, (const guint8 *)"Hello",
	                           5, NULL);
	g_assert_cmpmem(f2->data, f2->len, plain_hello, sizeof(plain_hello));

	g_assert_cmpint(bar_tvremote_ws_parse(masked_hello, sizeof(masked_hello),
		&op, &fin, &masked, &payload), ==, (gssize)sizeof(masked_hello));
	g_assert_true(masked);
	g_assert_true(fin);
	g_assert_cmpint(op, ==, BAR_TVREMOTE_WS_TEXT);
	g_assert_cmpstr(g_bytes_get_data(payload, NULL), ==, "Hello");
	g_bytes_unref(payload);

	/* Incomplete: wait for more, do not guess. */
	g_assert_cmpint(bar_tvremote_ws_parse(masked_hello, 6, &op, &fin, NULL,
	                                      &payload), ==, 0);
	g_assert_cmpint(bar_tvremote_ws_parse(masked_hello, 1, &op, &fin, NULL,
	                                      &payload), ==, 0);
	/* Reserved bits set: no extensions were negotiated. */
	{
		guint8 bad[] = { 0xc1, 0x00 };

		g_assert_cmpint(bar_tvremote_ws_parse(bad, 2, &op, &fin, NULL,
		                                      &payload), ==, -1);
	}
	/* A 64-bit length beyond the cap is refused, not allocated. */
	{
		guint8 huge[] = { 0x82, 0x7f, 0, 0, 0, 1, 0, 0, 0, 0 };

		g_assert_cmpint(bar_tvremote_ws_parse(huge, sizeof(huge), &op, &fin,
		                                      NULL, &payload), ==, -1);
	}

	for (i = 0; i < G_N_ELEMENTS(sizes); i++) {
		guint8 *data = g_malloc(sizes[i]);
		g_autoptr(GByteArray) f = NULL;
		gsize j, got_len;
		const guint8 *got;

		for (j = 0; j < sizes[i]; j++)
			data[j] = (guint8)(j * 7);
		f = bar_tvremote_ws_frame(BAR_TVREMOTE_WS_BINARY, data, sizes[i],
		                          mask);
		g_assert_cmpint(bar_tvremote_ws_parse(f->data, f->len, &op, &fin,
		                                      &masked, &payload), ==,
		                (gssize)f->len);
		got = g_bytes_get_data(payload, &got_len);
		g_assert_cmpmem(got, got_len, data, sizes[i]);
		g_bytes_unref(payload);
		g_free(data);
	}
}

/* ── Layout ──────────────────────────────────────────────────────── */

/* Catches: a button drawn where no click reaches it, two buttons
   overlapping (one unreachable), a button off the canvas, and a Fire
   TV remote missing a button the brief requires. */
static void
test_layout_hit(void)
{
	static const gint sizes[][2] = {
		{ 330, 344 }, { 320, 344 }, { 300, 300 }, { 500, 420 }, { 344, 134 }
	};
	static const BarTvremoteKey fire_needs[] = {
		BAR_TVREMOTE_KEY_UP, BAR_TVREMOTE_KEY_DOWN, BAR_TVREMOTE_KEY_LEFT,
		BAR_TVREMOTE_KEY_RIGHT, BAR_TVREMOTE_KEY_SELECT,
		BAR_TVREMOTE_KEY_BACK, BAR_TVREMOTE_KEY_HOME, BAR_TVREMOTE_KEY_MENU,
		BAR_TVREMOTE_KEY_REWIND, BAR_TVREMOTE_KEY_PLAY_PAUSE,
		BAR_TVREMOTE_KEY_FAST_FORWARD, BAR_TVREMOTE_KEY_VOL_DOWN,
		BAR_TVREMOTE_KEY_MUTE, BAR_TVREMOTE_KEY_VOL_UP,
		BAR_TVREMOTE_KEY_POWER, BAR_TVREMOTE_KEY_SEARCH
	};
	guint k, s, i, j;

	for (k = 0; k < BAR_TVREMOTE_N_KINDS; k++) {
		for (s = 0; s < G_N_ELEMENTS(sizes); s++) {
			BarTvremoteButton b[BAR_TVREMOTE_MAX_BUTTONS];
			gint w = sizes[s][0], hh = sizes[s][1];
			guint n;

			n = bar_tvremote_layout((BarTvremoteKind)k, w, hh, b, NULL,
			                        NULL, NULL);
			g_assert_cmpuint(n, >, 0);
			for (i = 0; i < n; i++) {
				gdouble cx = b[i].x + b[i].w / 2.0;
				gdouble cy = b[i].y + b[i].h / 2.0;

				g_assert_true(bar_tvremote_kind_supports(
					(BarTvremoteKind)k, b[i].key));
				g_assert_cmpfloat(b[i].x, >=, 0.0);
				g_assert_cmpfloat(b[i].y, >=, 0.0);
				g_assert_cmpfloat(b[i].x + b[i].w, <=, (gdouble)w + 0.001);
				g_assert_cmpfloat(b[i].y + b[i].h, <=, (gdouble)hh + 0.001);
				g_assert_cmpint(bar_tvremote_hit((BarTvremoteKind)k, w, hh,
				                                 cx, cy), ==,
				                (gint)b[i].key);
				for (j = i + 1; j < n; j++) {
					gboolean apart =
						b[i].x + b[i].w <= b[j].x ||
						b[j].x + b[j].w <= b[i].x ||
						b[i].y + b[i].h <= b[j].y ||
						b[j].y + b[j].h <= b[i].y;

					g_assert_true(apart);
				}
			}
		}
	}

	for (i = 0; i < G_N_ELEMENTS(fire_needs); i++) {
		BarTvremoteButton b[BAR_TVREMOTE_MAX_BUTTONS];
		guint n = bar_tvremote_layout(BAR_TVREMOTE_KIND_FIRETV, 330, 344, b,
		                              NULL, NULL, NULL);
		gboolean found = FALSE;

		for (j = 0; j < n; j++)
			if (b[j].key == fire_needs[i])
				found = TRUE;
		g_assert_true(found);
	}
	/* Outside every button: nothing. */
	g_assert_cmpint(bar_tvremote_hit(BAR_TVREMOTE_KIND_FIRETV, 330, 344,
	                                 1.0, 340.0), ==, -1);
	g_assert_false(bar_tvremote_has_dpad(BAR_TVREMOTE_KIND_CAST));
}

/* ── Routing, config, presentation ───────────────────────────────── */

/* Catches: volume not following `volume-via' (sound goes through a
   sound bar but the TV gets the key), a loop recursing, and non-volume
   keys being rerouted. */
static void
test_volume_via(void)
{
	g_autoptr(GPtrArray) d = g_ptr_array_new_with_free_func(
		(GDestroyNotify)bar_tvremote_device_free);
	BarTvremoteDevice *tv, *cube, *avr, *x, *y;

	tv = bar_tvremote_device_new("tv", BAR_TVREMOTE_KIND_SAMSUNG, "1.1.1.1");
	cube = bar_tvremote_device_new("cube", BAR_TVREMOTE_KIND_FIRETV, "1.1.1.2");
	avr = bar_tvremote_device_new("avr", BAR_TVREMOTE_KIND_ROKU, "1.1.1.3");
	x = bar_tvremote_device_new("x", BAR_TVREMOTE_KIND_ROKU, "1.1.1.4");
	y = bar_tvremote_device_new("y", BAR_TVREMOTE_KIND_ROKU, "1.1.1.5");
	tv->volume_via = g_strdup("cube");
	cube->volume_via = g_strdup("avr");
	x->volume_via = g_strdup("y");
	y->volume_via = g_strdup("x");
	g_ptr_array_add(d, tv);
	g_ptr_array_add(d, cube);
	g_ptr_array_add(d, avr);
	g_ptr_array_add(d, x);
	g_ptr_array_add(d, y);

	g_assert_true(bar_tvremote_route_volume(d, "tv",
		BAR_TVREMOTE_KEY_VOL_UP) == avr);
	g_assert_true(bar_tvremote_route_volume(d, "tv",
		BAR_TVREMOTE_KEY_MUTE) == avr);
	g_assert_true(bar_tvremote_route_volume(d, "tv",
		BAR_TVREMOTE_KEY_HOME) == tv);
	g_assert_true(bar_tvremote_route_volume(d, "x",
		BAR_TVREMOTE_KEY_VOL_UP) == x);
	g_free(avr->volume_via);
	avr->volume_via = g_strdup("gone");
	g_assert_true(bar_tvremote_route_volume(d, "cube",
		BAR_TVREMOTE_KEY_VOL_DOWN) == avr);
	g_assert_null(bar_tvremote_route_volume(d, "nope",
		BAR_TVREMOTE_KEY_VOL_UP));
}

/* Catches: a name with `:', `#' or a quote changing the file's
   structure on the way back, a bad entry costing every good one, a
   duplicate id accepted, and a world-readable secrets file read. */
static void
test_config(void)
{
	g_autofree gchar *text = fixture("tvremote.yaml");
	g_autofree gchar *def = NULL;
	g_autofree gchar *def2 = NULL;
	g_autofree gchar *yaml = NULL;
	g_autofree gchar *dir = NULL;
	g_autofree gchar *path = NULL;
	g_autofree gchar *missing = NULL;
	g_autoptr(GPtrArray) d = NULL;
	g_autoptr(GPtrArray) back = NULL;
	g_autoptr(GPtrArray) loaded = NULL;
	BarTvremoteDevice *lg;
	GError *error = NULL;
	GStatBuf st;

	d = bar_tvremote_config_parse(text, &def, &error);
	g_assert_no_error(error);
	g_assert_cmpstr(def, ==, "living-room");
	g_assert_cmpuint(d->len, ==, 3);       /* toaster + duplicate dropped */
	g_assert_cmpstr(((BarTvremoteDevice *)g_ptr_array_index(d, 0))->name,
	                ==, "Living room: Fire TV");
	lg = g_ptr_array_index(d, 2);
	g_assert_cmpstr(lg->id, ==, "lg-192-168-1-77");
	g_assert_cmpstr(lg->client_key, ==, "abc123");
	g_assert_cmpint(bar_tvremote_device_port(lg), ==, 3001);

	g_free(lg->name);
	lg->name = g_strdup("Den: \"big\" # tv\\");
	yaml = bar_tvremote_config_to_yaml(d, def);
	back = bar_tvremote_config_parse(yaml, &def2, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(back->len, ==, 3);
	g_assert_cmpstr(((BarTvremoteDevice *)g_ptr_array_index(back, 2))->name,
	                ==, "Den: \"big\" # tv\\");
	g_assert_cmpstr(def2, ==, "living-room");

	g_assert_null(bar_tvremote_config_parse("- just\n- a list\n", NULL,
	                                        &error));
	g_clear_error(&error);
	g_assert_null(bar_tvremote_config_parse("devices: [\n", NULL, &error));
	g_clear_error(&error);

	/* Saved at 0600, atomically; refused when readable by others. */
	dir = g_dir_make_tmp("tvremote-cfg-XXXXXX", NULL);
	path = g_build_filename(dir, "sub", "tv.yaml", NULL);
	g_assert_true(bar_tvremote_config_save(path, d, def, &error));
	g_assert_no_error(error);
	g_assert_cmpint(g_stat(path, &st), ==, 0);
	g_assert_cmpint(st.st_mode & 0777, ==, 0600);
	loaded = bar_tvremote_config_load(path, NULL, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(loaded->len, ==, 3);
	g_assert_cmpint(g_chmod(path, 0644), ==, 0);
	g_assert_null(bar_tvremote_config_load(path, NULL, &error));
	g_assert_error(error, BAR_TVREMOTE_CONFIG_ERROR,
	               BAR_TVREMOTE_CONFIG_ERROR_MODE);
	g_clear_error(&error);
	/* Saving over a loose file tightens it. */
	g_assert_true(bar_tvremote_config_save(path, d, def, &error));
	g_assert_cmpint(g_stat(path, &st), ==, 0);
	g_assert_cmpint(st.st_mode & 0777, ==, 0600);
	missing = g_build_filename(dir, "none.yaml", NULL);
	g_ptr_array_unref(bar_tvremote_config_load(missing, NULL, &error));
	g_assert_no_error(error);

	g_unlink(path);
	{
		g_autofree gchar *sub = g_build_filename(dir, "sub", NULL);

		g_rmdir(sub);
	}
	g_rmdir(dir);
}

/* Catches: an empty label rendered as " · " and outside text with
   control characters or invalid UTF-8 reaching pango. */
static void
test_presentation(void)
{
	g_autofree gchar *a = bar_tvremote_format_label(BAR_TVREMOTE_LINK_ONLINE,
	                                                "Netflix", 47, FALSE);
	g_autofree gchar *b = bar_tvremote_format_label(BAR_TVREMOTE_LINK_ONLINE,
	                                                "Netflix", 47, TRUE);
	g_autofree gchar *c = bar_tvremote_format_label(BAR_TVREMOTE_LINK_ONLINE,
	                                                NULL, 30, FALSE);
	g_autofree gchar *p = bar_tvremote_format_label(BAR_TVREMOTE_LINK_PAIRING,
	                                                "x", 1, FALSE);
	g_autofree gchar *clean = bar_tvremote_clean_text("a\nb\xff\x01c", 64);
	g_autofree gchar *cut = bar_tvremote_clean_text("\xc3\xa9\xc3\xa9", 3);
	g_autofree gchar *esc = bar_tvremote_json_escape("q\"\\\n\x01");
	gdouble rgb[3];

	g_assert_cmpstr(a, ==, "Netflix \xc2\xb7 47");
	g_assert_cmpstr(b, ==, "Netflix \xc2\xb7 muted");
	g_assert_cmpstr(c, ==, "vol 30");
	g_assert_cmpstr(p, ==, "pair");
	g_assert_null(bar_tvremote_format_label(BAR_TVREMOTE_LINK_OFFLINE,
	                                        "Netflix", 4, FALSE));
	g_assert_null(bar_tvremote_format_label(BAR_TVREMOTE_LINK_ONLINE,
	                                        NULL, -1, FALSE));
	g_assert_true(g_utf8_validate(clean, -1, NULL));
	g_assert_null(strchr(clean, '\n'));
	g_assert_cmpstr(cut, ==, "\xc3\xa9");
	g_assert_cmpstr(esc, ==, "q\\\"\\\\\\n\\u0001");
	g_assert_true(bar_tvremote_parse_hex("#ff8000", rgb));
	g_assert_cmpfloat(rgb[0], ==, 1.0);
	g_assert_false(bar_tvremote_parse_hex("peach", rgb));
	g_assert_false(bar_tvremote_parse_hex("#ff80", rgb));
}

/* ── The widget: Fire TV ─────────────────────────────────────────── */

static const gchar FIRE_YAML[] =
	"default: tv\n"
	"devices:\n"
	"  - id: tv\n"
	"    name: Living room\n"
	"    kind: firetv\n"
	"    host: 192.168.1.20\n";

/* The connect / authorize state machine.  Catches: the "Allow USB
   debugging?" wait shown as offline (so nobody looks at the TV), the
   poll not speeding up while it waits, and the remote not coming alive
   the moment the prompt is accepted. */
static void
test_firetv_pairing(void)
{
	BarHarness h;
	g_autofree gchar *label = NULL;
	g_autofree gchar *tip = NULL;
	g_autofree gchar *argv_log = NULL;
	g_autoptr(GowlBarPanel) panel = NULL;

	if (!widget_up(&h, FIRE_YAML))
		return;
	set_flag(&h, "adb-state", "notfound");
	set_flag(&h, "connect", "failed to authenticate to 192.168.1.20:5555\n");

	bar_harness_poll(&h);
	label = gowl_bar_plugin_dup_label(h.plugin);
	tip = gowl_bar_plugin_dup_tooltip(h.plugin);
	g_assert_cmpstr(label, ==, "pair");
	g_assert_nonnull(strstr(tip, "Allow USB debugging"));
	g_assert_cmpint(gowl_bar_plugin_get_color(h.plugin), ==,
	                GOWL_BAR_COLOR_YELLOW);
	g_assert_cmpint(gowl_bar_plugin_get_interval(h.plugin), ==, 1);
	panel = bar_harness_panel(&h);
	g_assert_true(bar_harness_panel_mentions(panel, "Allow USB debugging"));
	argv_log = read_or_empty(bar_harness_path(&h, "adb.argv"));
	g_assert_nonnull(strstr(argv_log, "connect 192.168.1.20:5555"));

	/* Still waiting: unauthorized now comes from get-state itself. */
	set_flag(&h, "adb-state", "unauthorized");
	bar_harness_poll(&h);
	g_free(label);
	label = gowl_bar_plugin_dup_label(h.plugin);
	g_assert_cmpstr(label, ==, "pair");

	/* Accepted. */
	set_flag(&h, "adb-state", "device");
	bar_harness_poll(&h);
	g_free(label);
	label = gowl_bar_plugin_dup_label(h.plugin);
	g_assert_cmpstr(label, ==, "Netflix \xc2\xb7 47");
	g_assert_cmpint(gowl_bar_plugin_get_color(h.plugin), ==,
	                GOWL_BAR_COLOR_GREEN);
	g_free(tip);
	tip = gowl_bar_plugin_dup_tooltip(h.plugin);
	g_assert_nonnull(strstr(tip, "Stranger Things"));
	bar_harness_down(&h);
}

/* Persistent shell: many keys, ONE adb process, every press a line.
   Catches: a process per key (the 300-700 ms lag the design exists to
   avoid), the canvas hit-test sending the wrong key code, the panel
   keys not reaching the device, and typed text not escaped. */
static void
test_firetv_persistent_shell(void)
{
	BarHarness h;
	g_autofree gchar *log = NULL;
	g_autofree gchar *spawns = NULL;
	gint pad;

	if (!widget_up(&h, FIRE_YAML))
		return;
	bar_harness_poll(&h);
	log = bar_harness_path(&h, "shell.log");
	spawns = bar_harness_path(&h, "spawns");

	pad = pad_index(&h);
	g_assert_cmpint(pad, >=, 0);
	click_key(&h, BAR_TVREMOTE_KIND_FIRETV, BAR_TVREMOTE_KEY_UP);
	click_key(&h, BAR_TVREMOTE_KIND_FIRETV, BAR_TVREMOTE_KEY_SELECT);
	click_key(&h, BAR_TVREMOTE_KIND_FIRETV, BAR_TVREMOTE_KEY_MENU);
	g_assert_true(gowl_bar_plugin_panel_key(h.plugin, XKB_KEY_h, 0, pad));
	g_assert_true(gowl_bar_plugin_panel_key(h.plugin, XKB_KEY_BackSpace, 0,
	                                        pad));
	g_assert_true(gowl_bar_plugin_panel_key(h.plugin, XKB_KEY_L,
	                                        GOWL_KEY_MOD_SHIFT, pad));
	/* Return on the focused canvas arrives as (-1, -1). */
	gowl_bar_plugin_panel_action(h.plugin, "pad", -1, -1.0, BTN_LEFT);
	gowl_bar_plugin_panel_text(h.plugin, "text", "hi there", TRUE);
	/* Not on the pad, or with Ctrl held: not the widget's key. */
	g_assert_false(gowl_bar_plugin_panel_key(h.plugin, XKB_KEY_h, 0,
	                                         pad + 1));
	g_assert_false(gowl_bar_plugin_panel_key(h.plugin, XKB_KEY_h,
	                                         GOWL_KEY_MOD_CTRL, pad));

	g_assert_true(wait_lines(log, 8));
	g_assert_true(file_has(log, "input keyevent 19\n"));
	g_assert_true(file_has(log, "input keyevent 23\n"));
	g_assert_true(file_has(log, "input keyevent 82\n"));
	g_assert_true(file_has(log, "input keyevent 3\n"));
	g_assert_true(file_has(log, "input keyevent 4\n"));
	g_assert_true(file_has(log, "input keyevent --longpress 23\n"));
	g_assert_true(file_has(log, "input text 'hi%sthere'\n"));
	g_assert_cmpuint(count_lines(spawns), ==, 1);
	bar_harness_down(&h);
}

/* A focused canvas hears the arrows, j/k, Return and the wheel before
   the panel's navigation (host change e0288a6).  Catches: the remote
   still answering only w/s, the wheel scrolling the panel instead of
   changing the volume, and the canvas not taking the keyboard when the
   panel opens. */
static void
test_firetv_arrows_and_wheel(void)
{
	BarHarness h;
	g_autofree gchar *log = NULL;
	g_autoptr(GowlBarPanel) panel = NULL;
	GowlBarPanelItem *pad_item;
	gint pad;

	if (!widget_up(&h, FIRE_YAML))
		return;
	bar_harness_poll(&h);
	log = bar_harness_path(&h, "shell.log");

	pad = pad_index(&h);
	g_assert_cmpint(pad, >=, 0);
	panel = bar_harness_panel(&h);
	pad_item = gowl_bar_panel_find(panel, "pad");
	g_assert_nonnull(pad_item);
	g_assert_cmpstr(gowl_bar_panel_item_get_prop(pad_item,
	                GOWL_BAR_ENTRY_PROP_AUTOFOCUS), ==, "true");

	g_assert_true(gowl_bar_plugin_panel_key(h.plugin, XKB_KEY_Down, 0, pad));
	g_assert_true(gowl_bar_plugin_panel_key(h.plugin, XKB_KEY_k, 0, pad));
	g_assert_true(gowl_bar_plugin_panel_key(h.plugin, XKB_KEY_Return, 0,
	                                        pad));
	gowl_bar_plugin_panel_action(h.plugin, "pad", 10, 10.0,
	                             GOWL_BAR_BUTTON_SCROLL_UP);
	gowl_bar_plugin_panel_action(h.plugin, "pad", 10, 10.0,
	                             GOWL_BAR_BUTTON_SCROLL_DOWN);

	g_assert_true(wait_lines(log, 5));
	g_assert_true(file_has(log, "input keyevent 20\n"));   /* DPAD_DOWN */
	g_assert_true(file_has(log, "input keyevent 19\n"));   /* DPAD_UP */
	g_assert_true(file_has(log, "input keyevent 23\n"));   /* CENTER */
	g_assert_true(file_has(log, "input keyevent 24\n"));   /* VOL_UP */
	g_assert_true(file_has(log, "input keyevent 25\n"));   /* VOL_DOWN */
	bar_harness_down(&h);
}

/* The shell dies (the TV slept); the next key restarts it.  Catches: a
   dead shell swallowing every later key, and a SIGPIPE from writing to
   it taking the compositor down. */
static void
test_firetv_shell_restart(void)
{
	BarHarness h;
	g_autofree gchar *log = NULL;
	g_autofree gchar *spawns = NULL;
	g_autofree gchar *pids = NULL;
	gint pad;
	GPid pid;

	if (!widget_up(&h, FIRE_YAML))
		return;
	bar_harness_poll(&h);
	log = bar_harness_path(&h, "shell.log");
	spawns = bar_harness_path(&h, "spawns");
	pad = pad_index(&h);

	g_assert_true(gowl_bar_plugin_panel_key(h.plugin, XKB_KEY_h, 0, pad));
	g_assert_true(wait_lines(log, 1));
	pids = read_or_empty(spawns);
	pid = (GPid)g_ascii_strtoll(pids, NULL, 10);
	g_assert_cmpint(pid, >, 0);
	/* The fake shell the widget started, by its own PID. */
	g_assert_cmpint(kill(pid, SIGKILL), ==, 0);
	g_usleep(150000);

	g_assert_true(gowl_bar_plugin_panel_key(h.plugin, XKB_KEY_m, 0, pad));
	g_assert_true(wait_lines(log, 2));
	g_assert_true(file_has(log, "input keyevent 82\n"));
	g_assert_cmpuint(count_lines(spawns), ==, 2);
	bar_harness_down(&h);
}

/* Apps and launching.  Catches: the LEANBACK launch not tried first,
   no fallback for a phone app, and a hostile id reaching `monkey'. */
static void
test_firetv_apps(void)
{
	BarHarness h;
	g_autoptr(GowlBarPanel) panel = NULL;
	g_autofree gchar *argv_path = NULL;

	if (!widget_up(&h, FIRE_YAML))
		return;
	bar_harness_poll(&h);
	argv_path = bar_harness_path(&h, "adb.argv");

	gowl_bar_plugin_panel_action(h.plugin, "load-apps", 0, 0.0, BTN_LEFT);
	panel = bar_harness_panel(&h);
	g_assert_true(bar_harness_panel_mentions(panel, "Prime Video"));
	g_assert_true(bar_harness_panel_mentions(panel, "app:com.netflix.ninja"));
	g_assert_false(bar_harness_panel_mentions(panel, "com.x;rm"));

	gowl_bar_plugin_panel_action(h.plugin, "app:com.netflix.ninja", 0, 0.0,
	                             BTN_LEFT);
	g_assert_true(file_has(argv_path, "monkey -p com.netflix.ninja -c "
	                       "android.intent.category.LEANBACK_LAUNCHER 1"));

	set_flag(&h, "no-leanback", "1");
	gowl_bar_plugin_panel_action(h.plugin, "app:org.xbmc.kodi", 0, 0.0,
	                             BTN_LEFT);
	g_assert_true(file_has(argv_path, "monkey -p org.xbmc.kodi 1"));

	gowl_bar_plugin_panel_action(h.plugin, "app:com.x;reboot", 0, 0.0,
	                             BTN_LEFT);
	g_assert_false(file_has(argv_path, "reboot"));
	g_clear_object(&panel);
	panel = bar_harness_panel(&h);
	g_assert_true(bar_harness_panel_mentions(panel, "not an Android package"));
	bar_harness_down(&h);
}

/* Catches: a missing adb crashing or spinning instead of naming the
   Fedora package to install. */
static void
test_firetv_missing_adb(void)
{
	BarHarness h;
	g_autofree gchar *tip = NULL;
	g_autoptr(GowlBarPanel) panel = NULL;

	if (!widget_up(&h, FIRE_YAML))
		return;
	bar_harness_set(&h, "adb", "/nonexistent/adb");
	bar_harness_poll(&h);
	tip = gowl_bar_plugin_dup_tooltip(h.plugin);
	g_assert_nonnull(strstr(tip, "android-tools"));
	g_assert_null(gowl_bar_plugin_dup_label(h.plugin));
	g_assert_cmpint(gowl_bar_plugin_get_color(h.plugin), ==,
	                GOWL_BAR_COLOR_SUBTEXT);
	panel = bar_harness_panel(&h);
	g_assert_true(bar_harness_panel_mentions(panel, "android-tools"));
	bar_harness_down(&h);
}

/* ── The widget: Roku and volume-via ─────────────────────────────── */

/* Scroll on a Fire TV whose sound goes through a Roku TV: the volume
   goes to the Roku, play/pause still to the Fire TV.  Catches: volume
   sent to the wrong device, ECP URLs built wrong, and a Roku in
   "Limited" mode shown as merely offline. */
static void
test_roku_and_volume_via(void)
{
	static const gchar yaml[] =
		"default: tv\n"
		"devices:\n"
		"  - id: tv\n"
		"    kind: firetv\n"
		"    host: 192.168.1.20\n"
		"    volume-via: den\n"
		"  - id: den\n"
		"    name: Den Roku\n"
		"    kind: roku\n"
		"    host: 192.168.1.134\n";
	BarHarness h;
	g_autofree gchar *curl = NULL;
	g_autofree gchar *shell = NULL;
	g_autoptr(GowlBarPanel) panel = NULL;
	g_autoptr(GowlBarPanel) panel2 = NULL;

	if (!widget_up(&h, yaml))
		return;
	bar_harness_poll(&h);
	curl = bar_harness_path(&h, "curl.log");
	shell = bar_harness_path(&h, "shell.log");

	g_assert_true(gowl_bar_plugin_on_scroll(h.plugin, -1.0, -1, 0));
	g_assert_true(file_has(curl,
		"1 http://192.168.1.134:8060/keypress/VolumeUp\n"));
	g_assert_true(gowl_bar_plugin_on_scroll(h.plugin, 1.0, 2, 0));
	g_assert_true(file_has(curl, "keypress/VolumeDown"));
	g_assert_true(gowl_bar_plugin_on_click(h.plugin, BTN_MIDDLE, 0, 0, 0));
	g_assert_true(file_has(curl, "keypress/VolumeMute"));

	g_assert_true(gowl_bar_plugin_on_click(h.plugin, BTN_RIGHT, 0, 0, 0));
	g_assert_true(wait_lines(shell, 1));
	g_assert_true(file_has(shell, "input keyevent 85\n"));
	g_assert_false(file_has(shell, "keyevent 24"));
	g_assert_false(gowl_bar_plugin_on_click(h.plugin, BTN_LEFT, 0, 0, 0));

	/* The Roku's own panel: status and typing. */
	gowl_bar_plugin_panel_action(h.plugin, "dev:den", 0, 0.0, BTN_LEFT);
	panel = bar_harness_panel(&h);
	g_assert_true(bar_harness_panel_mentions(panel, "Den Roku"));
	g_assert_true(bar_harness_panel_mentions(panel, "Netflix"));
	gowl_bar_plugin_panel_text(h.plugin, "text", "a/b", TRUE);
	g_assert_true(file_has(curl, "keypress/Lit_%2F"));
	gowl_bar_plugin_panel_action(h.plugin, "load-apps", 0, 0.0, BTN_LEFT);
	gowl_bar_plugin_panel_action(h.plugin, "app:12", 0, 0.0, BTN_LEFT);
	g_assert_true(file_has(curl, "1 http://192.168.1.134:8060/launch/12\n"));

	set_flag(&h, "roku403", "1");
	gowl_bar_plugin_panel_action(h.plugin, "dev:den", 0, 0.0, BTN_LEFT);
	panel2 = bar_harness_panel(&h);
	g_assert_true(bar_harness_panel_mentions(panel2, "Control by mobile apps"));
	bar_harness_down(&h);
}

/* ── The widget: config, add, forget, scan ───────────────────────── */

/* Catches: a world-readable token file used anyway, and the widget
   not recovering once it is fixed. */
static void
test_widget_config_mode(void)
{
	BarHarness h;
	g_autofree gchar *path = NULL;
	g_autofree gchar *tip = NULL;
	g_autoptr(GowlBarPanel) panel = NULL;
	g_autoptr(GowlBarPanel) panel2 = NULL;

	if (!widget_up(&h, NULL))
		return;
	path = write_config(&h, FIRE_YAML, 0644);
	bar_harness_poll(&h);
	panel = bar_harness_panel(&h);
	g_assert_true(bar_harness_panel_mentions(panel, "chmod 600"));
	g_assert_false(bar_harness_panel_mentions(panel, "dev:tv"));
	tip = gowl_bar_plugin_dup_tooltip(h.plugin);
	g_assert_nonnull(strstr(tip, "chmod 600"));

	g_assert_cmpint(g_chmod(path, 0600), ==, 0);
	/* A chmod alone does not move the mtime; a re-save does. */
	g_usleep(10000);
	g_assert_true(g_file_set_contents(path, FIRE_YAML, -1, NULL));
	bar_harness_poll(&h);
	panel2 = bar_harness_panel(&h);
	g_assert_true(bar_harness_panel_mentions(panel2, "dev:tv"));
	g_assert_false(bar_harness_panel_mentions(panel2, "chmod 600"));
	bar_harness_down(&h);
}

/* Add from the panel, make default, forget with two clicks.  Catches:
   the list written world-readable, a bad host saved, a single click
   forgetting a TV, and the Fire TV setup steps not shown. */
static void
test_widget_add_forget(void)
{
	BarHarness h;
	g_autofree gchar *path = NULL;
	g_autofree gchar *saved = NULL;
	g_autoptr(GowlBarPanel) p1 = NULL;
	g_autoptr(GowlBarPanel) p2 = NULL;
	g_autoptr(GowlBarPanel) p3 = NULL;
	g_autoptr(GowlBarPanel) p4 = NULL;
	GStatBuf st;

	if (!widget_up(&h, NULL))
		return;
	path = bar_harness_path(&h, "tvremote.yaml");
	bar_harness_poll(&h);
	p1 = bar_harness_panel(&h);
	g_assert_true(bar_harness_panel_mentions(p1, "No TVs yet"));
	g_assert_true(bar_harness_panel_mentions(p1, "ADB debugging"));

	/* A bad host is refused, nothing written. */
	gowl_bar_plugin_panel_text(h.plugin, "add-host", "1.2.3.4/x", TRUE);
	g_assert_false(g_file_test(path, G_FILE_TEST_EXISTS));

	gowl_bar_plugin_panel_text(h.plugin, "add-host", "10.0.0.9", TRUE);
	g_assert_cmpint(g_stat(path, &st), ==, 0);
	g_assert_cmpint(st.st_mode & 0777, ==, 0600);
	saved = read_or_empty(path);
	g_assert_nonnull(strstr(saved, "\"10.0.0.9\""));
	g_assert_nonnull(strstr(saved, "kind: \"firetv\""));
	g_assert_nonnull(strstr(saved, "default: \"firetv-10-0-0-9\""));

	/* A Roku, via the kind buttons (index 2 = Roku). */
	gowl_bar_plugin_panel_action(h.plugin, "add-kind", 2, 0.0, BTN_LEFT);
	gowl_bar_plugin_panel_text(h.plugin, "add-host", "10.0.0.10", FALSE);
	gowl_bar_plugin_panel_action(h.plugin, "add", 0, 0.0, BTN_LEFT);
	g_free(saved);
	saved = read_or_empty(path);
	g_assert_nonnull(strstr(saved, "roku-10-0-0-10"));
	p2 = bar_harness_panel(&h);
	g_assert_true(bar_harness_panel_mentions(p2, "dev:roku-10-0-0-10"));

	/* Make the Roku (selected, as just added) the default. */
	gowl_bar_plugin_panel_action(h.plugin, "devops", 0, 0.0, BTN_LEFT);
	g_free(saved);
	saved = read_or_empty(path);
	g_assert_nonnull(strstr(saved, "default: \"roku-10-0-0-10\""));

	/* Forget: first click arms, second forgets. */
	gowl_bar_plugin_panel_action(h.plugin, "devops", 0, 0.0, BTN_LEFT);
	p3 = bar_harness_panel(&h);
	g_assert_true(bar_harness_panel_mentions(p3, "Click again to forget"));
	g_free(saved);
	saved = read_or_empty(path);
	g_assert_nonnull(strstr(saved, "roku-10-0-0-10"));
	gowl_bar_plugin_panel_action(h.plugin, "devops", 0, 0.0, BTN_LEFT);
	g_free(saved);
	saved = read_or_empty(path);
	g_assert_null(strstr(saved, "roku-10-0-0-10"));
	p4 = bar_harness_panel(&h);
	g_assert_false(bar_harness_panel_mentions(p4, "dev:roku-10-0-0-10"));
	bar_harness_down(&h);
}

/* Catches: a scan of a whole /16, and the CLI kinds offered without
   their tools.  (Fails before any packet leaves.) */
static void
test_widget_scan_and_kinds(void)
{
	BarHarness h;
	g_autoptr(GowlBarPanel) p1 = NULL;
	g_autoptr(GowlBarPanel) p2 = NULL;
	g_autofree gchar *catt = NULL;
	gint idx;

	if (!widget_up(&h, FIRE_YAML))
		return;
	bar_harness_set(&h, "scan-subnet", "10.0.0.0/16");
	gowl_bar_plugin_panel_action(h.plugin, "scan", 0, 0.0, BTN_LEFT);
	bar_harness_poll(&h);
	p1 = bar_harness_panel(&h);
	g_assert_true(bar_harness_panel_mentions(p1, "not an IPv4 /24"));
	g_assert_false(bar_harness_panel_mentions(p1, "Chromecast"));

	catt = bar_harness_script(&h, "fake-catt", "echo ok");
	bar_harness_set(&h, "catt", catt);
	bar_harness_poll(&h);
	p2 = bar_harness_panel(&h);
	g_assert_true(bar_harness_panel_mentions(p2, "Chromecast"));
	idx = panel_find(p2, "add-kind");
	g_assert_cmpint(idx, >=, 0);
	g_assert_cmpuint(gowl_bar_panel_item_n_children(
		gowl_bar_panel_get_item(p2, (guint)idx)), ==, 6);
	bar_harness_down(&h);
}

/* Catches: the per-widget colour keys not honoured (a literal dropped,
   a role ignored). */
static void
test_widget_colours(void)
{
	BarHarness h;
	gdouble rgba[4];

	if (!widget_up(&h, FIRE_YAML))
		return;
	bar_harness_set(&h, "color-online", "#112233");
	bar_harness_poll(&h);
	g_assert_true(gowl_bar_plugin_get_color_rgba(h.plugin, rgba));
	g_assert_cmpfloat_with_epsilon(rgba[0], 0x11 / 255.0, 0.001);

	bar_harness_set(&h, "color-online", "peach");
	bar_harness_poll(&h);
	g_assert_false(gowl_bar_plugin_get_color_rgba(h.plugin, rgba));
	g_assert_cmpint(gowl_bar_plugin_get_color(h.plugin), ==,
	                GOWL_BAR_COLOR_PEACH);

	set_flag(&h, "adb-state", "offline");
	set_flag(&h, "connect", "failed to connect to 192.168.1.20:5555\n");
	bar_harness_set(&h, "color-offline", "red");
	bar_harness_poll(&h);
	g_assert_cmpint(gowl_bar_plugin_get_color(h.plugin), ==,
	                GOWL_BAR_COLOR_RED);
	g_assert_null(gowl_bar_plugin_dup_label(h.plugin));
	bar_harness_down(&h);
}

/* ── A fake Samsung / LG websocket server ────────────────────────── */

typedef struct {
	GSocket     *listener;
	gint         port;
	gboolean     lg;
	GAsyncQueue *got;           /* gchar*: what the client sent */
	gint         stop;
	gint         unmasked;      /* client frames that were unmasked */
	GThread     *thread;
} FakeWs;

typedef struct {
	FakeWs  *f;
	GSocket *sock;
} FakeConn;

static gboolean
fake_send(GSocket *s, const gchar *text)
{
	g_autoptr(GByteArray) fr = bar_tvremote_ws_frame(BAR_TVREMOTE_WS_TEXT,
		(const guint8 *)text, strlen(text), NULL);
	gsize done = 0;

	while (done < fr->len) {
		gssize n = g_socket_send(s, (const gchar *)fr->data + done,
		                         fr->len - done, NULL, NULL);

		if (n <= 0)
			return FALSE;
		done += (gsize)n;
	}
	return TRUE;
}

/* Next text message from the client, or NULL when it went away. */
static gchar *
fake_recv(FakeConn *c, GByteArray *buf)
{
	for (;;) {
		guint8 op = 0;
		gboolean fin = FALSE, masked = FALSE;
		GBytes *payload = NULL;
		gssize used;
		gchar tmp[4096];
		gssize n;
		GError *error = NULL;

		used = bar_tvremote_ws_parse(buf->data, buf->len, &op, &fin,
		                             &masked, &payload);
		if (used < 0)
			return NULL;
		if (used > 0) {
			gchar *text;

			g_byte_array_remove_range(buf, 0, (guint)used);
			if (!masked)
				g_atomic_int_inc(&c->f->unmasked);
			if (op == BAR_TVREMOTE_WS_CLOSE) {
				g_bytes_unref(payload);
				return NULL;
			}
			text = g_strdup(g_bytes_get_data(payload, NULL));
			g_bytes_unref(payload);
			return text;
		}
		n = g_socket_receive(c->sock, tmp, sizeof(tmp), NULL, &error);
		if (n < 0 && g_error_matches(error, G_IO_ERROR,
		                             G_IO_ERROR_TIMED_OUT) &&
		    !g_atomic_int_get(&c->f->stop)) {
			g_clear_error(&error);
			continue;
		}
		g_clear_error(&error);
		if (n <= 0)
			return NULL;
		g_byte_array_append(buf, (const guint8 *)tmp, (guint)n);
	}
}

static void
fake_lg_reply(FakeConn *c, const gchar *msg)
{
	g_autofree gchar *type = bar_tvremote_json_string(msg, "type", NULL);
	g_autofree gchar *id = bar_tvremote_json_string(msg, "id", NULL);
	g_autofree gchar *uri = bar_tvremote_json_string(msg, "uri", NULL);
	g_autofree gchar *out = NULL;

	if (g_strcmp0(type, "register") == 0) {
		if (strstr(msg, "client-key") == NULL)
			fake_send(c->sock, "{\"type\":\"response\",\"id\":\"register_0\","
			          "\"payload\":{\"pairingType\":\"PROMPT\","
			          "\"returnValue\":true}}");
		fake_send(c->sock, "{\"type\":\"registered\",\"id\":\"register_0\","
		          "\"payload\":{\"client-key\":\"abc123def\"}}");
		return;
	}
	if (g_strcmp0(type, "request") != 0 || id == NULL || uri == NULL)
		return;
	if (strstr(uri, "getVolume") != NULL)
		out = g_strdup_printf("{\"type\":\"response\",\"id\":\"%s\","
		                      "\"payload\":{\"volumeStatus\":{\"volume\":12,"
		                      "\"muteStatus\":false},\"returnValue\":true}}",
		                      id);
	else if (strstr(uri, "getForegroundAppInfo") != NULL)
		out = g_strdup_printf("{\"type\":\"response\",\"id\":\"%s\","
		                      "\"payload\":{\"appId\":\"netflix\"}}", id);
	else if (strstr(uri, "getPointerInputSocket") != NULL)
		out = g_strdup_printf("{\"type\":\"response\",\"id\":\"%s\","
		                      "\"payload\":{\"socketPath\":"
		                      "\"ws://127.0.0.1:%d/pointer\"}}",
		                      id, c->f->port);
	else
		out = g_strdup_printf("{\"type\":\"response\",\"id\":\"%s\","
		                      "\"payload\":{\"returnValue\":true}}", id);
	fake_send(c->sock, out);
}

static gpointer
fake_conn_thread(gpointer data)
{
	FakeConn *c = data;
	GByteArray *buf = g_byte_array_new();
	g_autofree gchar *key = NULL;
	g_autofree gchar *path = NULL;
	g_autofree gchar *accept = NULL;
	g_autofree gchar *resp = NULL;
	gchar *end = NULL;

	g_socket_set_timeout(c->sock, 1);
	/* The upgrade request. */
	while (end == NULL) {
		gchar tmp[2048];
		GError *error = NULL;
		gssize n = g_socket_receive(c->sock, tmp, sizeof(tmp), NULL, &error);

		if (n < 0 && g_error_matches(error, G_IO_ERROR,
		                             G_IO_ERROR_TIMED_OUT) &&
		    !g_atomic_int_get(&c->f->stop)) {
			g_clear_error(&error);
			continue;
		}
		g_clear_error(&error);
		if (n <= 0)
			goto out;
		g_byte_array_append(buf, (const guint8 *)tmp, (guint)n);
		g_byte_array_append(buf, (const guint8 *)"", 1);
		end = strstr((gchar *)buf->data, "\r\n\r\n");
		g_byte_array_set_size(buf, buf->len - 1);
	}
	{
		gchar *k = strstr((gchar *)buf->data, "Sec-WebSocket-Key: ");
		gchar *g = (gchar *)buf->data + 4;
		gchar *sp = strchr(g, ' ');

		key = g_strndup(k + 19, (gsize)(strchr(k, '\r') - (k + 19)));
		path = g_strndup(g, (gsize)(sp - g));
	}
	g_byte_array_set_size(buf, 0);
	accept = bar_tvremote_ws_accept_key(key);
	resp = g_strdup_printf("HTTP/1.1 101 Switching Protocols\r\n"
	                       "Upgrade: websocket\r\nConnection: Upgrade\r\n"
	                       "Sec-WebSocket-Accept: %s\r\n\r\n", accept);
	g_socket_send(c->sock, resp, strlen(resp), NULL, NULL);
	g_async_queue_push(c->f->got, g_strdup_printf("PATH %s", path));

	if (!c->f->lg)
		fake_send(c->sock, "{\"event\":\"ms.channel.connect\",\"data\":"
		          "{\"id\":\"x\",\"token\":\"98765432\"}}");
	for (;;) {
		gchar *msg = fake_recv(c, buf);

		if (msg == NULL)
			break;
		if (c->f->lg && g_str_has_prefix(path, "/pointer")) {
			g_async_queue_push(c->f->got, g_strdup_printf("BTN %s", msg));
			g_free(msg);
			continue;
		}
		if (c->f->lg)
			fake_lg_reply(c, msg);
		else if (strstr(msg, "ed.installedApp.get") != NULL)
			fake_send(c->sock, "{\"event\":\"ed.installedApp.get\","
			          "\"data\":{\"data\":[{\"appId\":\"11101200001\","
			          "\"name\":\"Netflix\"},{\"appId\":\"bad;id\","
			          "\"name\":\"Evil\"}]}}");
		g_async_queue_push(c->f->got, msg);
	}
out:
	g_byte_array_unref(buf);
	g_object_unref(c->sock);
	g_free(c);
	return NULL;
}

static gpointer
fake_accept_thread(gpointer data)
{
	FakeWs *f = data;

	while (!g_atomic_int_get(&f->stop)) {
		GError *error = NULL;
		GSocket *s = g_socket_accept(f->listener, NULL, &error);
		FakeConn *c;

		if (s == NULL) {
			g_clear_error(&error);
			continue;
		}
		c = g_new0(FakeConn, 1);
		c->f = f;
		c->sock = s;
		g_thread_unref(g_thread_new("fake-ws-conn", fake_conn_thread, c));
	}
	return NULL;
}

/* Leaked on purpose: connection threads may outlive the test body by
   up to a second while their sockets time out. */
static FakeWs *
fake_ws_start(gboolean lg)
{
	FakeWs *f = g_new0(FakeWs, 1);
	GInetAddress *lo = g_inet_address_new_loopback(G_SOCKET_FAMILY_IPV4);
	GSocketAddress *addr = g_inet_socket_address_new(lo, 0);
	GSocketAddress *bound;

	f->lg = lg;
	f->got = g_async_queue_new_full(g_free);
	f->listener = g_socket_new(G_SOCKET_FAMILY_IPV4, G_SOCKET_TYPE_STREAM,
	                           G_SOCKET_PROTOCOL_TCP, NULL);
	g_assert_true(g_socket_bind(f->listener, addr, TRUE, NULL));
	g_assert_true(g_socket_listen(f->listener, NULL));
	g_socket_set_timeout(f->listener, 1);
	bound = g_socket_get_local_address(f->listener, NULL);
	f->port = g_inet_socket_address_get_port(G_INET_SOCKET_ADDRESS(bound));
	g_object_unref(bound);
	g_object_unref(addr);
	g_object_unref(lo);
	f->thread = g_thread_new("fake-ws", fake_accept_thread, f);
	return f;
}

static void
fake_ws_stop(FakeWs *f)
{
	g_atomic_int_set(&f->stop, 1);
	g_thread_join(f->thread);
}

/* Did the client send something containing @needle within ~3 s? */
static gboolean
fake_expect(FakeWs *f, const gchar *needle)
{
	gint64 deadline = g_get_monotonic_time() + 3 * G_USEC_PER_SEC;

	while (g_get_monotonic_time() < deadline) {
		g_autofree gchar *m = g_async_queue_timeout_pop(f->got, 200000);

		if (m != NULL && strstr(m, needle) != NULL)
			return TRUE;
	}
	return FALSE;
}

/* Samsung end to end.  Catches: the token from pairing not saved (a
   prompt on the TV every time), the token saved world-readable or
   shown in the tooltip, keys sent unmasked, a hostile app id listed. */
static void
test_samsung_ws(void)
{
	BarHarness h;
	FakeWs *f;
	g_autofree gchar *yaml = NULL;
	g_autofree gchar *saved = NULL;
	g_autofree gchar *tip = NULL;
	g_autoptr(GowlBarPanel) panel = NULL;
	GStatBuf st;

	f = fake_ws_start(FALSE);
	yaml = g_strdup_printf("default: tv\ndevices:\n  - id: tv\n"
	                       "    kind: samsung\n    host: 127.0.0.1\n"
	                       "    port: %d\n", f->port);
	if (!widget_up(&h, yaml)) {
		fake_ws_stop(f);
		return;
	}
	bar_harness_poll(&h);
	g_assert_true(fake_expect(f, "PATH /api/v2/channels/"
	                          "samsung.remote.control?name="));
	g_assert_cmpint(gowl_bar_plugin_get_color(h.plugin), ==,
	                GOWL_BAR_COLOR_GREEN);
	saved = read_or_empty(bar_harness_path(&h, "tvremote.yaml"));
	g_assert_nonnull(strstr(saved, "token: \"98765432\""));
	g_assert_cmpint(g_stat(bar_harness_path(&h, "tvremote.yaml"), &st), ==, 0);
	g_assert_cmpint(st.st_mode & 0777, ==, 0600);
	tip = gowl_bar_plugin_dup_tooltip(h.plugin);
	g_assert_null(strstr(tip, "98765432"));

	gowl_bar_plugin_panel_action(h.plugin, "pad", -1, -1.0, BTN_LEFT);
	g_assert_true(fake_expect(f, "\"DataOfCmd\":\"KEY_ENTER\""));
	gowl_bar_plugin_on_click(h.plugin, BTN_RIGHT, 0, 0, 0);
	g_assert_true(fake_expect(f, "KEY_PAUSE"));
	gowl_bar_plugin_panel_text(h.plugin, "text", "hi", TRUE);
	g_assert_true(fake_expect(f, "\"Cmd\":\"aGk=\""));

	gowl_bar_plugin_panel_action(h.plugin, "load-apps", 0, 0.0, BTN_LEFT);
	panel = bar_harness_panel(&h);
	g_assert_true(bar_harness_panel_mentions(panel, "app:11101200001"));
	g_assert_false(bar_harness_panel_mentions(panel, "bad;id"));
	g_assert_cmpint(g_atomic_int_get(&f->unmasked), ==, 0);
	bar_harness_down(&h);
	fake_ws_stop(f);
}

/* LG end to end.  Catches: the client key not saved, the foreground
   app and the nested volumeStatus not read, arrows not reaching the
   pointer socket, and volume not going over SSAP. */
static void
test_lg_ws(void)
{
	BarHarness h;
	FakeWs *f;
	g_autofree gchar *yaml = NULL;
	g_autofree gchar *saved = NULL;
	g_autofree gchar *label = NULL;
	gint pad;

	f = fake_ws_start(TRUE);
	yaml = g_strdup_printf("default: tv\ndevices:\n  - id: tv\n"
	                       "    kind: lg\n    host: 127.0.0.1\n"
	                       "    port: %d\n", f->port);
	if (!widget_up(&h, yaml)) {
		fake_ws_stop(f);
		return;
	}
	bar_harness_poll(&h);
	g_assert_true(fake_expect(f, "\"type\":\"register\""));
	label = gowl_bar_plugin_dup_label(h.plugin);
	g_assert_cmpstr(label, ==, "Netflix \xc2\xb7 12");
	saved = read_or_empty(bar_harness_path(&h, "tvremote.yaml"));
	g_assert_nonnull(strstr(saved, "client-key: \"abc123def\""));

	pad = pad_index(&h);
	g_assert_true(gowl_bar_plugin_panel_key(h.plugin, XKB_KEY_w, 0, pad));
	g_assert_true(fake_expect(f, "BTN type:button\nname:UP\n"));
	g_assert_true(gowl_bar_plugin_on_scroll(h.plugin, -1.0, -1, 0));
	g_assert_true(fake_expect(f, "ssap://audio/volumeUp"));
	g_assert_cmpint(g_atomic_int_get(&f->unmasked), ==, 0);
	bar_harness_down(&h);
	fake_ws_stop(f);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/bar/tvremote/keymap/firetv", test_keymap_firetv);
	g_test_add_func("/bar/tvremote/keymap/every-kind", test_keymap_every_kind);
	g_test_add_func("/bar/tvremote/adb/text-escaping", test_adb_text_escaping);
	g_test_add_func("/bar/tvremote/adb/focus", test_adb_focus);
	g_test_add_func("/bar/tvremote/adb/pm-and-apps", test_adb_pm_and_apps);
	g_test_add_func("/bar/tvremote/adb/states", test_adb_states);
	g_test_add_func("/bar/tvremote/adb/audio", test_adb_audio);
	g_test_add_func("/bar/tvremote/roku/parsing", test_roku_parsing);
	g_test_add_func("/bar/tvremote/discover/parsing", test_discover_parsing);
	g_test_add_func("/bar/tvremote/discover/probe", test_discover_probe);
	g_test_add_func("/bar/tvremote/ws/accept-key", test_ws_accept_key);
	g_test_add_func("/bar/tvremote/ws/frames", test_ws_frames);
	g_test_add_func("/bar/tvremote/layout/hit-test", test_layout_hit);
	g_test_add_func("/bar/tvremote/volume-via", test_volume_via);
	g_test_add_func("/bar/tvremote/config", test_config);
	g_test_add_func("/bar/tvremote/presentation", test_presentation);
	g_test_add_func("/bar/tvremote/widget/firetv-pairing", test_firetv_pairing);
	g_test_add_func("/bar/tvremote/widget/firetv-persistent-shell",
	                test_firetv_persistent_shell);
	g_test_add_func("/bar/tvremote/widget/firetv-arrows-and-wheel",
	                test_firetv_arrows_and_wheel);
	g_test_add_func("/bar/tvremote/widget/firetv-shell-restart",
	                test_firetv_shell_restart);
	g_test_add_func("/bar/tvremote/widget/firetv-apps", test_firetv_apps);
	g_test_add_func("/bar/tvremote/widget/firetv-missing-adb",
	                test_firetv_missing_adb);
	g_test_add_func("/bar/tvremote/widget/roku-volume-via",
	                test_roku_and_volume_via);
	g_test_add_func("/bar/tvremote/widget/config-mode", test_widget_config_mode);
	g_test_add_func("/bar/tvremote/widget/add-forget", test_widget_add_forget);
	g_test_add_func("/bar/tvremote/widget/scan-and-kinds",
	                test_widget_scan_and_kinds);
	g_test_add_func("/bar/tvremote/widget/colours", test_widget_colours);
	g_test_add_func("/bar/tvremote/widget/samsung-ws", test_samsung_ws);
	g_test_add_func("/bar/tvremote/widget/lg-ws", test_lg_ws);
	return g_test_run();
}
