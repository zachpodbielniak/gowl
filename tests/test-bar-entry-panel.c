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
 * Panel text fields and canvases, end to end: a real headless
 * compositor, the real bar.so, and a probe plugin compiled from a `.c'
 * file the way a user's own plugin is.
 *
 * test-bar-entry covers what a key does to the text.  This covers the
 * routing around it, which no unit test can reach:
 *
 *   - the autofocused field gets the keys, and j, k and space TYPE
 *     rather than navigating;
 *   - every edit reaches the plugin, and Return arrives as a submit;
 *   - the host keeps the text across the rebuild every edit causes,
 *     and gives way only when the plugin bumps the revision;
 *   - a Ctrl chord the field does not want is offered to the plugin;
 *   - a Super chord is NOT claimed, so the window manager still works;
 *   - Tab leaves a field, Escape closes the panel;
 *   - a canvas is painted by its plugin, and Return on a focused one
 *     reaches the plugin's action as (-1, -1).
 */

#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>
#include <wayland-server-core.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#include "gowl.h"
#include "core/gowl-core-private.h"
#include "module/gowl-module-manager.h"
#include "interfaces/gowl-ipc-handler.h"
#include "core/gowl-seat.h"

#ifndef GOWL_TEST_MODULE_DIR
#define GOWL_TEST_MODULE_DIR "build/release/modules"
#endif

/*
 * The probe writes one line per event to the file named by
 * GOWL_TEST_PROBE_LOG.  Submitting the text `clear' makes it bump the
 * entry's revision with an empty value, which is how a plugin replaces
 * what the user typed.
 */
static const gchar *probe_source =
	"#include <gowl/barkit/gowl-bar-plugin.h>\n"
	"#include <gowl/barkit/gowl-bar-plugin-proxy.h>\n"
	"#include <gowl/barkit/gowl-bar-registry.h>\n"
	"#include <gowl/barkit/gowl-bar-panel.h>\n"
	"#include <gowl/gowl-enums.h>\n"
	"#include <xkbcommon/xkbcommon-keysyms.h>\n"
	"#include <stdio.h>\n"
	"\n"
	"typedef struct { gint rev; gint focus; } Probe;\n"
	"\n"
	"static void\n"
	"note(const gchar *line)\n"
	"{\n"
	"\tconst gchar *path = g_getenv(\"GOWL_TEST_PROBE_LOG\");\n"
	"\tFILE *f;\n"
	"\n"
	"\tif (path == NULL || (f = fopen(path, \"a\")) == NULL)\n"
	"\t\treturn;\n"
	"\tfprintf(f, \"%s\\n\", line);\n"
	"\tfclose(f);\n"
	"}\n"
	"\n"
	"static gpointer\n"
	"probe_create(GowlBarPlugin *plugin)\n"
	"{\n"
	"\t(void)plugin;\n"
	"\treturn g_new0(Probe, 1);\n"
	"}\n"
	"\n"
	"static void\n"
	"probe_destroy(GowlBarPlugin *plugin, gpointer data)\n"
	"{\n"
	"\t(void)plugin;\n"
	"\tg_free(data);\n"
	"}\n"
	"\n"
	"static void\n"
	"probe_poll(GowlBarPlugin *plugin, gpointer data)\n"
	"{\n"
	"\t(void)data;\n"
	"\tgowl_bar_plugin_set_label(plugin, \"probe\");\n"
	"}\n"
	"\n"
	"static GowlBarPanel *\n"
	"probe_panel(GowlBarPlugin *plugin, gpointer data)\n"
	"{\n"
	"\tProbe *p = data;\n"
	"\tGowlBarPanel *panel;\n"
	"\tGowlBarPanelItem *e;\n"
	"\tgchar rev[16];\n"
	"\n"
	"\t(void)plugin;\n"
	"\tpanel = gowl_bar_panel_new();\n"
	"\te = gowl_bar_panel_add_entry(panel, \"q\", \"Ask\", \"\");\n"
	"\tgowl_bar_panel_item_set_prop(e, GOWL_BAR_ENTRY_PROP_AUTOFOCUS,\n"
	"\t                             \"true\");\n"
	"\tg_snprintf(rev, sizeof(rev), \"%d\", p->rev);\n"
	"\tgowl_bar_panel_item_set_prop(e, GOWL_BAR_ENTRY_PROP_REVISION, rev);\n"
	"\te = gowl_bar_panel_add_entry(panel, \"pw\", NULL, NULL);\n"
	"\tgowl_bar_panel_item_set_prop(e, GOWL_BAR_ENTRY_PROP_SECRET,\n"
	"\t                             \"true\");\n"
	"\tif (p->focus > 0) {\n"
	"\t\tg_snprintf(rev, sizeof(rev), \"%d\", p->focus);\n"
	"\t\tgowl_bar_panel_item_set_prop(e, GOWL_BAR_PANEL_PROP_FOCUS, rev);\n"
	"\t}\n"
	"\tgowl_bar_panel_add_canvas(panel, \"c\", 80);\n"
	"\treturn panel;\n"
	"}\n"
	"\n"
	"static void\n"
	"probe_text(GowlBarPlugin *plugin, gpointer data, const gchar *id,\n"
	"           const gchar *text, gboolean submitted)\n"
	"{\n"
	"\tProbe *p = data;\n"
	"\tgchar *line;\n"
	"\n"
	"\t(void)plugin;\n"
	"\tline = g_strdup_printf(\"text %s %d [%s]\", id, submitted ? 1 : 0,\n"
	"\t                       text);\n"
	"\tnote(line);\n"
	"\tg_free(line);\n"
	"\tif (submitted && g_strcmp0(text, \"clear\") == 0)\n"
	"\t\tp->rev++;\n"
	"\tif (submitted && g_strcmp0(text, \"go\") == 0)\n"
	"\t\tp->focus++;\n"
	"}\n"
	"\n"
	"static void\n"
	"probe_action(GowlBarPlugin *plugin, gpointer data, const gchar *id,\n"
	"             gint index, gdouble value, guint button)\n"
	"{\n"
	"\tgchar *line;\n"
	"\n"
	"\t(void)plugin; (void)data; (void)button;\n"
	"\tline = g_strdup_printf(\"action %s %d %d\", id, index, (gint)value);\n"
	"\tnote(line);\n"
	"\tg_free(line);\n"
	"}\n"
	"\n"
	"static gboolean\n"
	"probe_key(GowlBarPlugin *plugin, gpointer data, guint keysym,\n"
	"          guint modifiers, gint focused)\n"
	"{\n"
	"\t(void)plugin; (void)data; (void)focused;\n"
	"\tif (keysym == XKB_KEY_s && (modifiers & GOWL_KEY_MOD_CTRL)) {\n"
	"\t\tnote(\"key ctrl-s\");\n"
	"\t\treturn TRUE;\n"
	"\t}\n"
	"\treturn FALSE;\n"
	"}\n"
	"\n"
	"static void\n"
	"probe_canvas(GowlBarPlugin *plugin, gpointer data, const gchar *id,\n"
	"             cairo_t *cr, const GowlBarTheme *theme, gint w, gint h)\n"
	"{\n"
	"\tgchar *line;\n"
	"\n"
	"\t(void)plugin; (void)data;\n"
	"\tgowl_bar_theme_cairo_set(theme, cr, GOWL_BAR_COLOR_ACCENT);\n"
	"\tcairo_paint(cr);\n"
	"\tline = g_strdup_printf(\"canvas %s %d %d\", id, w > 0 ? 1 : 0, h);\n"
	"\tnote(line);\n"
	"\tg_free(line);\n"
	"}\n"
	"\n"
	"static const GowlBarPluginVTable probe_vtable = {\n"
	"\tsizeof(GowlBarPluginVTable),\n"
	"\tprobe_create, probe_destroy,\n"
	"\tNULL, NULL, NULL,\n"
	"\tNULL, probe_poll, NULL,\n"
	"\tNULL, NULL,\n"
	"\tNULL, NULL,\n"
	"\tprobe_panel, probe_action,\n"
	"\tNULL, NULL,\n"
	"\tprobe_key,\n"
	"\tNULL,\n"
	"\tprobe_text, probe_canvas\n"
	"};\n"
	"\n"
	"static const GowlBarPluginDesc descs[] = {\n"
	"\t{ GOWL_BAR_PLUGIN_ABI, \"probe\", \"Probe\",\n"
	"\t  \"Records panel text\", \"1.0.0\", &probe_vtable,\n"
	"\t  NULL, { NULL, NULL, NULL, NULL } }\n"
	"};\n"
	"\n"
	"G_MODULE_EXPORT const GowlBarPluginDesc *\n"
	"gowl_bar_plugin_query(guint *n_descs)\n"
	"{\n"
	"\t*n_descs = G_N_ELEMENTS(descs);\n"
	"\treturn descs;\n"
	"}\n";

typedef struct {
	gchar             *runtime;
	gchar             *plugin_dir;
	gchar             *log;
	GowlConfig        *config;
	GowlModuleManager *modules;
	GowlCompositor    *compositor;
	GowlModule        *bar;
} Rig;

static void
settle(Rig *r, gint ms)
{
	gint64 deadline = g_get_monotonic_time() + (gint64)ms * 1000;

	while (g_get_monotonic_time() < deadline)
		wl_event_loop_dispatch(r->compositor->event_loop, 5);
}

static void
configure_bar(Rig *r, GHashTable *bar_settings)
{
	GHashTable *outer;

	outer = g_hash_table_new(g_str_hash, g_str_equal);
	g_hash_table_insert(outer, (gpointer)"bar", bar_settings);
	gowl_module_manager_configure_all(r->modules, outer);
	g_hash_table_destroy(outer);
}

static gboolean
rig_up(Rig *r)
{
	const gchar *parent;
	GError *error = NULL;
	g_autofree gchar *module = NULL;
	g_autofree gchar *probe = NULL;

	memset(r, 0, sizeof(*r));
	g_log_set_always_fatal(G_LOG_LEVEL_CRITICAL | G_LOG_FATAL_MASK);

	if (g_find_program_in_path("gcc") == NULL) {
		g_test_skip("no compiler, so a .c plugin cannot be built");
		return FALSE;
	}

	parent = g_getenv("XDG_RUNTIME_DIR");
	if (parent == NULL || !g_file_test(parent, G_FILE_TEST_IS_DIR))
		parent = g_get_tmp_dir();
	r->runtime = g_build_filename(parent, "gowl-bar-entry-XXXXXX", NULL);
	g_assert_nonnull(g_mkdtemp_full(r->runtime, 0700));

	g_setenv("GOWL_DISABLE_SYSTEMD", "1", TRUE);
	g_setenv("XDG_RUNTIME_DIR", r->runtime, TRUE);
	g_setenv("WLR_BACKENDS", "headless", TRUE);
	g_setenv("WLR_RENDERER", "pixman", TRUE);
	g_setenv("WLR_HEADLESS_OUTPUTS", "1", TRUE);

	r->plugin_dir = g_build_filename(r->runtime, "bar-plugins", NULL);
	g_assert_cmpint(g_mkdir_with_parents(r->plugin_dir, 0700), ==, 0);
	probe = g_build_filename(r->plugin_dir, "probe.c", NULL);
	g_assert_true(g_file_set_contents(probe, probe_source, -1, NULL));

	r->log = g_build_filename(r->runtime, "probe.log", NULL);
	g_setenv("GOWL_TEST_PROBE_LOG", r->log, TRUE);

	module = g_build_filename(GOWL_TEST_MODULE_DIR, "bar.so", NULL);
	if (!g_file_test(module, G_FILE_TEST_EXISTS)) {
		g_test_skip("the bar module is not built; run `make' first");
		return FALSE;
	}
	r->modules = gowl_module_manager_new();
	if (!gowl_module_manager_load_module(r->modules, module, &error))
		g_error("could not load %s: %s", module, error->message);
	gowl_module_manager_activate_all(r->modules);

	r->config = gowl_config_new();
	r->compositor = gowl_compositor_new();
	gowl_compositor_set_config(r->compositor, r->config);
	gowl_compositor_set_module_manager(r->compositor, r->modules);
	if (!gowl_compositor_start(r->compositor, &error)) {
		g_test_skip("no headless compositor here");
		g_clear_error(&error);
		return FALSE;
	}
	gowl_module_manager_dispatch_startup(r->modules, r->compositor);
	settle(r, 200);

	{
		GHashTable *settings;

		settings = g_hash_table_new_full(g_str_hash, g_str_equal,
		                                 g_free, g_free);
		g_hash_table_insert(settings, g_strdup("plugin-dir"),
		                    g_strdup(r->plugin_dir));
		g_hash_table_insert(settings, g_strdup("widgets"),
		                    g_strdup("probe"));
		g_hash_table_insert(settings, g_strdup("height"),
		                    g_strdup("24"));
		configure_bar(r, settings);
		g_hash_table_destroy(settings);
	}
	settle(r, 1500);

	r->bar = gowl_module_manager_find_module(r->modules, "bar");
	g_assert_nonnull(r->bar);
	return TRUE;
}

static void
rig_down(Rig *r)
{
	if (r->compositor != NULL) {
		if (r->modules != NULL)
			gowl_module_manager_dispatch_shutdown(r->modules,
			                                      r->compositor);
		g_object_unref(r->compositor);
	}
	g_clear_object(&r->modules);
	g_clear_object(&r->config);
	g_unsetenv("GOWL_TEST_PROBE_LOG");
	if (r->runtime != NULL) {
		g_autofree gchar *cmd = NULL;

		cmd = g_strdup_printf("rm -rf '%s'", r->runtime);
		if (system(cmd) != 0)
			g_test_message("could not remove %s", r->runtime);
	}
	g_clear_pointer(&r->runtime, g_free);
	g_clear_pointer(&r->plugin_dir, g_free);
	g_clear_pointer(&r->log, g_free);
}

/* The probe's log, from the start of the test. */
static gchar *
read_log(Rig *r)
{
	gchar *text = NULL;

	if (!g_file_get_contents(r->log, &text, NULL, NULL))
		return g_strdup("");
	return text;
}

/* Forget everything logged so far. */
static void
clear_log(Rig *r)
{
	g_assert_true(g_file_set_contents(r->log, "", 0, NULL));
}

static gboolean
log_has(Rig *r, const gchar *line)
{
	g_autofree gchar *text = read_log(r);
	g_autofree gchar *needle = g_strdup_printf("%s\n", line);

	return strstr(text, needle) != NULL;
}

/* A press and its release, as a keyboard sends them.  Returns whether
   the press was claimed. */
static gboolean
key(Rig *r, guint modifiers, guint keysym)
{
	gboolean claimed;

	claimed = gowl_module_manager_dispatch_key(r->modules, modifiers,
	                                           keysym, TRUE);
	gowl_module_manager_dispatch_key(r->modules, modifiers, keysym,
	                                 FALSE);
	return claimed;
}

static void
open_panel(Rig *r)
{
	g_autofree gchar *reply = NULL;

	reply = gowl_ipc_handler_handle_command(GOWL_IPC_HANDLER(r->bar),
	                                        "bar-panel", "probe");
	g_assert_cmpstr(reply, ==, "opened probe\n");
	settle(r, 50);
}

static void
test_typing_reaches_the_plugin(void)
{
	Rig r;

	if (!rig_up(&r))
		return;

	open_panel(&r);
	clear_log(&r);

	/* The autofocused field types; j, k and space are letters. */
	g_assert_true(key(&r, 0, XKB_KEY_j));
	g_assert_true(key(&r, 0, XKB_KEY_k));
	g_assert_true(key(&r, 0, XKB_KEY_space));
	g_assert_true(key(&r, GOWL_KEY_MOD_SHIFT, XKB_KEY_X));
	g_assert_true(log_has(&r, "text q 0 [j]"));
	g_assert_true(log_has(&r, "text q 0 [jk]"));
	g_assert_true(log_has(&r, "text q 0 [jk ]"));
	/* Kept across the rebuild every edit triggers. */
	g_assert_true(log_has(&r, "text q 0 [jk X]"));

	g_assert_true(key(&r, 0, XKB_KEY_Return));
	g_assert_true(log_has(&r, "text q 1 [jk X]"));

	/* A Ctrl chord the field does not want reaches the plugin. */
	g_assert_true(key(&r, GOWL_KEY_MOD_CTRL, XKB_KEY_s));
	g_assert_true(log_has(&r, "key ctrl-s"));

	/* Cursor movement edits nothing and tells nobody. */
	clear_log(&r);
	g_assert_true(key(&r, 0, XKB_KEY_Left));
	g_assert_true(key(&r, 0, XKB_KEY_Home));
	g_assert_false(log_has(&r, "text q 0 [jk X]"));
	g_assert_true(key(&r, 0, XKB_KEY_a));
	g_assert_true(log_has(&r, "text q 0 [ajk X]"));

	rig_down(&r);
}

static void
test_the_plugin_replaces_text_by_revision(void)
{
	Rig r;
	const gchar *word = "clear";
	const gchar *c;

	if (!rig_up(&r))
		return;

	open_panel(&r);
	for (c = word; *c != '\0'; c++)
		key(&r, 0, (guint)*c);   /* ASCII keysyms equal the letter */
	key(&r, 0, XKB_KEY_Return);
	g_assert_true(log_has(&r, "text q 1 [clear]"));

	/* The probe bumped the revision with an empty value: the host
	   must take it, so the next letter starts a fresh field. */
	clear_log(&r);
	key(&r, 0, XKB_KEY_z);
	g_assert_true(log_has(&r, "text q 0 [z]"));

	rig_down(&r);
}

static void
test_tab_leaves_and_escape_closes(void)
{
	Rig r;

	if (!rig_up(&r))
		return;

	open_panel(&r);
	key(&r, 0, XKB_KEY_q);

	/* Tab moves the keyboard to the next field, the secret one. */
	clear_log(&r);
	g_assert_true(key(&r, 0, XKB_KEY_Tab));
	key(&r, 0, XKB_KEY_p);
	g_assert_true(log_has(&r, "text pw 0 [p]"));

	/* And Tab again lands on the canvas, where Return is an action
	   with no coordinates. */
	g_assert_true(key(&r, 0, XKB_KEY_Tab));
	clear_log(&r);
	g_assert_true(key(&r, 0, XKB_KEY_Return));
	g_assert_true(log_has(&r, "action c -1 -1"));

	/* Escape closes, and with no panel open the bar wants no keys. */
	g_assert_true(key(&r, 0, XKB_KEY_Escape));
	g_assert_false(key(&r, 0, XKB_KEY_a));

	rig_down(&r);
}

static void
test_window_manager_chords_are_not_eaten(void)
{
	Rig r;

	if (!rig_up(&r))
		return;

	open_panel(&r);
	clear_log(&r);

	/* Super+q with the field focused: not typed, not claimed. */
	g_assert_false(key(&r, GOWL_KEY_MOD_LOGO, XKB_KEY_q));
	g_assert_false(log_has(&r, "text q 0 [q]"));

	rig_down(&r);
}

/*
 * Paste reads the clipboard through a pipe the OWNER writes, and the
 * owner can only write while the compositor thread is free --- so the
 * host waits on the compositor's own loop.  A gowl-owned clipboard
 * source writes from that loop too, which is what lets this run in
 * one thread: a blocking read here would hang the test, exactly as it
 * would hang the desktop.
 */
static void
test_paste_arrives_through_the_loop(void)
{
	Rig r;
	GowlSeat *seat;

	if (!rig_up(&r))
		return;

	seat = gowl_compositor_get_seat(r.compositor);
	if (seat == NULL) {
		g_test_skip("the headless compositor has no seat");
		rig_down(&r);
		return;
	}
	gowl_seat_set_clipboard(seat, "pasted\ttext\nsecond line");

	open_panel(&r);
	key(&r, 0, XKB_KEY_a);
	clear_log(&r);
	g_assert_true(key(&r, GOWL_KEY_MOD_CTRL, XKB_KEY_v));
	settle(&r, 300);

	/* One-line field: tab and newline flattened to spaces, inserted
	   at the cursor. */
	g_assert_true(log_has(&r, "text q 0 [apasted text second line]"));

	rig_down(&r);
}

/*
 * A plugin moves the keyboard by changing an item's focus token, and a
 * token left in place does not pull it back on every rebuild.
 */
static void
test_the_plugin_can_move_focus(void)
{
	Rig r;

	if (!rig_up(&r))
		return;

	open_panel(&r);
	key(&r, 0, XKB_KEY_g);
	key(&r, 0, XKB_KEY_o);
	key(&r, 0, XKB_KEY_Return);   /* the probe now asks for "pw" */

	clear_log(&r);
	key(&r, 0, XKB_KEY_a);
	g_assert_true(log_has(&r, "text pw 0 [a]"));

	/* Back to q by hand; the unchanged token must not steal the
	   keyboard again as each keystroke rebuilds the panel. */
	key(&r, GOWL_KEY_MOD_SHIFT, XKB_KEY_ISO_Left_Tab);
	clear_log(&r);
	key(&r, 0, XKB_KEY_b);
	key(&r, 0, XKB_KEY_c);
	g_assert_true(log_has(&r, "text q 0 [gob]"));
	g_assert_true(log_has(&r, "text q 0 [gobc]"));

	rig_down(&r);
}

static void
test_a_canvas_is_painted(void)
{
	Rig r;

	if (!rig_up(&r))
		return;

	clear_log(&r);
	open_panel(&r);
	g_assert_true(log_has(&r, "canvas c 1 80"));

	rig_down(&r);
}

int
main(int argc, char *argv[])
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/bar-entry-panel/typing-reaches-the-plugin",
	                test_typing_reaches_the_plugin);
	g_test_add_func("/bar-entry-panel/revision-replaces-text",
	                test_the_plugin_replaces_text_by_revision);
	g_test_add_func("/bar-entry-panel/tab-and-escape",
	                test_tab_leaves_and_escape_closes);
	g_test_add_func("/bar-entry-panel/wm-chords-not-eaten",
	                test_window_manager_chords_are_not_eaten);
	g_test_add_func("/bar-entry-panel/plugin-moves-focus",
	                test_the_plugin_can_move_focus);
	g_test_add_func("/bar-entry-panel/paste",
	                test_paste_arrives_through_the_loop);
	g_test_add_func("/bar-entry-panel/canvas-painted",
	                test_a_canvas_is_painted);

	return g_test_run();
}
