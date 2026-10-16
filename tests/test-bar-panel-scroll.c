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
 * A panel taller than its cap scrolls, and what is scrolled into view
 * must answer the pointer where it is drawn: a real headless
 * compositor, the real bar.so and a probe plugin built from a `.c'.
 */

#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>
#include <linux/input-event-codes.h>
#include <wayland-server-core.h>
#include <wlr/types/wlr_cursor.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#include "gowl.h"
#include "core/gowl-core-private.h"
#include "module/gowl-module-manager.h"
#include "interfaces/gowl-ipc-handler.h"
#include "core/gowl-seat.h"
#include "interfaces/gowl-bar-provider.h"

#ifndef GOWL_TEST_MODULE_DIR
#define GOWL_TEST_MODULE_DIR "build/release/modules"
#endif

/*
 * The probe's panel is taller than it may be: thirty rows, a text
 * field and a row of buttons at the bottom, in a centred panel capped
 * at 200 pixels.  Every action it hears goes to the log.
 */
static const gchar *probe_source =
	"#include <gowl/barkit/gowl-bar-plugin.h>\n"
	"#include <gowl/barkit/gowl-bar-plugin-proxy.h>\n"
	"#include <gowl/barkit/gowl-bar-registry.h>\n"
	"#include <gowl/barkit/gowl-bar-panel.h>\n"
	"#include <stdio.h>\n"
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
	"\treturn g_new0(gint, 1);\n"
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
	"\tGowlBarPanel *panel;\n"
	"\tGowlBarPanelItem *b;\n"
	"\tgchar id[16];\n"
	"\tgint i;\n"
	"\n"
	"\t(void)plugin; (void)data;\n"
	"\tpanel = gowl_bar_panel_new();\n"
	"\tgowl_bar_panel_set_width(panel, 300);\n"
	"\tgowl_bar_panel_set_max_height(panel, 200);\n"
	"\tgowl_bar_panel_set_centered(panel, TRUE);\n"
	"\tfor (i = 0; i < 30; i++) {\n"
	"\t\tg_snprintf(id, sizeof(id), \"r%d\", i);\n"
	"\t\tgowl_bar_panel_add_row(panel, id, NULL, id, NULL);\n"
	"\t}\n"
	"\tgowl_bar_panel_add_entry(panel, \"m\", \"Model\", \"\");\n"
	"\tb = gowl_bar_panel_add_buttons(panel, \"done\");\n"
	"\tgowl_bar_panel_add_button(b, \"Done\", TRUE);\n"
	"\treturn panel;\n"
	"}\n"
	"\n"
	"static void\n"
	"probe_action(GowlBarPlugin *plugin, gpointer data, const gchar *id,\n"
	"             gint index, gdouble value, guint button)\n"
	"{\n"
	"\tgchar *line;\n"
	"\n"
	"\t(void)plugin; (void)data; (void)value; (void)button;\n"
	"\tline = g_strdup_printf(\"action %s %d\", id, index);\n"
	"\tnote(line);\n"
	"\tg_free(line);\n"
	"}\n"
	"\n"
	"static void\n"
	"probe_text(GowlBarPlugin *plugin, gpointer data, const gchar *id,\n"
	"           const gchar *text, gboolean submitted)\n"
	"{\n"
	"\t(void)plugin; (void)data; (void)id; (void)text; (void)submitted;\n"
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
	"\tNULL,\n"
	"\tNULL,\n"
	"\tprobe_text, NULL\n"
	"};\n"
	"\n"
	"static const GowlBarPluginDesc descs[] = {\n"
	"\t{ GOWL_BAR_PLUGIN_ABI, \"probe\", \"Probe\",\n"
	"\t  \"Records panel clicks\", \"1.0.0\", &probe_vtable,\n"
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
	r->runtime = g_build_filename(parent, "gowl-bar-scroll-XXXXXX", NULL);
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

/* Open the probe's panel through the IPC, as a keybind would. */
static void
open_panel(Rig *r)
{
	g_autofree gchar *reply = NULL;

	reply = gowl_ipc_handler_handle_command(GOWL_IPC_HANDLER(r->bar),
	                                        "bar-panel", "probe");
	g_assert_cmpstr(reply, ==, "opened probe\n");
	settle(r, 50);
}

/* The only output: the bar's pointer interface takes coordinates
   relative to it. */
static gpointer
the_monitor(Rig *r)
{
	GList *mons;

	mons = gowl_compositor_get_monitors(r->compositor);
	g_assert_nonnull(mons);
	return mons->data;
}

/* Where the centred 300x200 panel lands on that output. */
static void
panel_origin(Rig *r, gint *x, gint *y)
{
	gint mx, my, mw, mh;

	gowl_monitor_get_geometry(GOWL_MONITOR(the_monitor(r)), &mx, &my,
	                          &mw, &mh);
	*x = (mw - 300) / 2;
	*y = (mh - 200) * 2 / 5;
}

/* The pointer, as far as the bar is concerned. */
static gint ptr_x, ptr_y;

static void
point(Rig *r, gint x, gint y)
{
	ptr_x = x;
	ptr_y = y;
	gowl_bar_provider_handle_motion(GOWL_BAR_PROVIDER(r->bar),
	                                the_monitor(r), x, y);
}

static void
click(Rig *r)
{
	gowl_bar_provider_handle_button(GOWL_BAR_PROVIDER(r->bar),
		the_monitor(r), ptr_x, ptr_y, BTN_LEFT, TRUE, 0);
	gowl_bar_provider_handle_button(GOWL_BAR_PROVIDER(r->bar),
		the_monitor(r), ptr_x, ptr_y, BTN_LEFT, FALSE, 0);
	settle(r, 30);
}

/* Wheel the panel all the way down: far more notches than it has. */
static void
scroll_to_bottom(Rig *r)
{
	gint i;

	for (i = 0; i < 60; i++)
		gowl_bar_provider_handle_axis(GOWL_BAR_PROVIDER(r->bar),
			the_monitor(r), ptr_x, ptr_y, 15.0, 1, 0);
	settle(r, 30);
}

/* Catches: a button at the bottom of a scrolled panel not answering a
   click -- the hit rectangles and the pixels disagreeing about where
   the scrolled rows are. */
static void
test_bottom_button_after_scrolling(void)
{
	Rig r;
	gint x, y;

	if (!rig_up(&r))
		return;
	open_panel(&r);
	panel_origin(&r, &x, &y);
	point(&r, x + 100, y + 100);
	scroll_to_bottom(&r);
	clear_log(&r);

	/* The buttons are the last thing in the panel, so after scrolling
	   to the end they sit just above its bottom padding. */
	point(&r, x + 150, y + 200 - 32);
	click(&r);
	g_assert_true(log_has(&r, "action done 0"));
	rig_down(&r);
}

/* Catches: the same click failing once the text field above it has the
   keyboard, which is the order a user takes: type a model, then Save. */
static void
test_bottom_button_with_the_field_focused(void)
{
	Rig r;
	gint x, y;

	if (!rig_up(&r))
		return;
	open_panel(&r);
	panel_origin(&r, &x, &y);
	point(&r, x + 100, y + 100);
	scroll_to_bottom(&r);

	/* The field sits right above the buttons: a click on it takes
	   the keyboard and is not an action. */
	clear_log(&r);
	point(&r, x + 150, y + 200 - 70);
	click(&r);
	{
		g_autofree gchar *text = read_log(&r);

		g_assert_cmpstr(text, ==, "");
	}

	/* Type into it, scroll once more for good measure, then press
	   the button. */
	key(&r, 0, XKB_KEY_a);
	scroll_to_bottom(&r);
	clear_log(&r);
	point(&r, x + 150, y + 200 - 32);
	click(&r);
	g_assert_true(log_has(&r, "action done 0"));
	rig_down(&r);
}

int
main(int argc, char *argv[])
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/bar-panel-scroll/bottom-button",
	                test_bottom_button_after_scrolling);
	g_test_add_func("/bar-panel-scroll/bottom-button-field-focused",
	                test_bottom_button_with_the_field_focused);

	return g_test_run();
}
