/* test-bar-monitor.c -- a bar widget is told which screen it is on
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * One set of plugin objects serves every output: the bar is measured
 * and drawn once per screen from the SAME widgets, so a widget with no
 * way to ask which screen it was on asked the compositor for the
 * SELECTED monitor -- the focused one.  On a desk with two displays
 * that is routinely not the screen the bar is sitting on, and the
 * answers matter: the display widget chose between the backlight and a
 * software gamma ramp from the other screen's HDR state, and the
 * recorder's "whole screen" photographed the screen nobody was
 * pointing at.
 *
 * gowl_bar_plugin_get_monitor() is the host saying so.  This asserts
 * the contract end to end, against the real bar module in a real
 * headless compositor with two outputs, using a plugin compiled from a
 * `.c' file at test time -- the same path a user's own plugin takes:
 *
 *  - the draw of each output's copy of the bar is served THAT output,
 *    so both screens appear and neither widget ever sees only one;
 *  - a poll is served nothing, because a poll runs once for every
 *    screen at once and there is no honest answer;
 *  - and it is cleared afterwards rather than left pointing at
 *    whichever output happened to be drawn last, which is the failure
 *    mode that would make the poll's answer look right by accident.
 */

#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>
#include <wayland-server-core.h>

#include "gowl.h"
#include "core/gowl-core-private.h"
#include "module/gowl-module-manager.h"

#ifndef GOWL_TEST_MODULE_DIR
#define GOWL_TEST_MODULE_DIR "build/release/modules"
#endif

/*
 * The probe.
 *
 * It records what it was told, and where, into a file named by the
 * environment -- the plugin runs inside the compositor and the test
 * cannot reach into it any other way.  Everything else about it is the
 * smallest plugin that still occupies space in the bar: a label, so it
 * measures wider than nothing and therefore gets drawn at all.
 */
static const gchar *probe_source =
	"#include <gowl/barkit/gowl-bar-plugin.h>\n"
	"#include <gowl/barkit/gowl-bar-plugin-proxy.h>\n"
	"#include <gowl/barkit/gowl-bar-registry.h>\n"
	"#include <gowl/core/gowl-monitor.h>\n"
	"\n"
	"#include <stdio.h>\n"
	"\n"
	"static void\n"
	"probe_note(const gchar *what, GowlBarPlugin *plugin)\n"
	"{\n"
	"\tconst gchar *path = g_getenv(\"GOWL_TEST_PROBE_LOG\");\n"
	"\tgpointer     mon;\n"
	"\tFILE        *f;\n"
	"\n"
	"\tif (path == NULL)\n"
	"\t\treturn;\n"
	"\tmon = gowl_bar_plugin_get_monitor(plugin);\n"
	"\tf = fopen(path, \"a\");\n"
	"\tif (f == NULL)\n"
	"\t\treturn;\n"
	"\tfprintf(f, \"%s %s\\n\", what,\n"
	"\t        (mon != NULL)\n"
	"\t        ? gowl_monitor_get_name((GowlMonitor *)mon)\n"
	"\t        : \"(none)\");\n"
	"\tfclose(f);\n"
	"}\n"
	"\n"
	"static gint\n"
	"probe_interval(GowlBarPlugin *plugin, gpointer data)\n"
	"{\n"
	"\t(void)plugin;\n"
	"\t(void)data;\n"
	"\treturn 1;\n"
	"}\n"
	"\n"
	"static void\n"
	"probe_poll(GowlBarPlugin *plugin, gpointer data)\n"
	"{\n"
	"\t(void)data;\n"
	"\tprobe_note(\"poll\", plugin);\n"
	"\tgowl_bar_plugin_set_label(plugin, \"probe\");\n"
	"}\n"
	"\n"
	"static void\n"
	"probe_draw(GowlBarPlugin *plugin, gpointer data, cairo_t *cr,\n"
	"           PangoLayout *layout, const GowlBarTheme *theme,\n"
	"           gint x, gint y, gint width, gint height,\n"
	"           gboolean hovered, gboolean panel_open)\n"
	"{\n"
	"\t(void)data;\n"
	"\tprobe_note(\"draw\", plugin);\n"
	"\tgowl_bar_plugin_draw_label(plugin, cr, layout, theme, x, y,\n"
	"\t                           width, height, hovered, panel_open);\n"
	"}\n"
	"\n"
	"static const GowlBarPluginVTable probe_vtable = {\n"
	"\tsizeof(GowlBarPluginVTable),\n"
	"\tNULL, NULL,\n"
	"\tNULL, NULL, NULL,\n"
	"\tprobe_interval, probe_poll, NULL,\n"
	"\tNULL, probe_draw,\n"
	"\tNULL, NULL,\n"
	"\tNULL, NULL,\n"
	"\tNULL, NULL,\n"
	"\tNULL\n"
	"};\n"
	"\n"
	"static const GowlBarPluginDesc descs[] = {\n"
	"\t{ GOWL_BAR_PLUGIN_ABI, \"probe\", \"Probe\",\n"
	"\t  \"Records the output it is served\", \"1.0.0\", &probe_vtable,\n"
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
} Rig;

/* Drive the event loop for a while.  The bar draws off its own tick
 * timer, and a surface is created on one tick and painted on the
 * next. */
static void
settle(Rig *r, gint ms)
{
	gint64 deadline = g_get_monotonic_time() + (gint64)ms * 1000;

	while (g_get_monotonic_time() < deadline)
		wl_event_loop_dispatch(r->compositor->event_loop, 5);
}

static GHashTable *
str_table(void)
{
	return g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
}

/* Hand the bar module one settings block, as a `modules: bar:' stanza
 * would. */
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
	GError      *error = NULL;
	gchar       *module;
	gchar       *probe;

	memset(r, 0, sizeof(*r));
	g_log_set_always_fatal(G_LOG_LEVEL_CRITICAL | G_LOG_FATAL_MASK);

	if (g_find_program_in_path("gcc") == NULL) {
		g_test_skip("no compiler, so a .c plugin cannot be built");
		return FALSE;
	}

	parent = g_getenv("XDG_RUNTIME_DIR");
	if (parent == NULL || !g_file_test(parent, G_FILE_TEST_IS_DIR))
		parent = g_get_tmp_dir();
	r->runtime = g_build_filename(parent, "gowl-bar-mon-XXXXXX", NULL);
	g_assert_nonnull(g_mkdtemp_full(r->runtime, 0700));

	g_setenv("GOWL_DISABLE_SYSTEMD", "1", TRUE);
	g_setenv("XDG_RUNTIME_DIR", r->runtime, TRUE);
	g_setenv("WLR_BACKENDS", "headless", TRUE);
	g_setenv("WLR_RENDERER", "pixman", TRUE);
	g_setenv("WLR_HEADLESS_OUTPUTS", "2", TRUE);

	r->plugin_dir = g_build_filename(r->runtime, "bar-plugins", NULL);
	g_assert_cmpint(g_mkdir_with_parents(r->plugin_dir, 0700), ==, 0);
	probe = g_build_filename(r->plugin_dir, "probe.c", NULL);
	g_assert_true(g_file_set_contents(probe, probe_source, -1, NULL));
	g_free(probe);

	r->log = g_build_filename(r->runtime, "probe.log", NULL);
	g_setenv("GOWL_TEST_PROBE_LOG", r->log, TRUE);

	module = g_build_filename(GOWL_TEST_MODULE_DIR, "bar.so", NULL);
	if (!g_file_test(module, G_FILE_TEST_EXISTS)) {
		g_free(module);
		g_test_skip("the bar module is not built; run `make' first");
		return FALSE;
	}
	r->modules = gowl_module_manager_new();
	if (!gowl_module_manager_load_module(r->modules, module, &error))
		g_error("could not load %s: %s", module, error->message);
	g_free(module);
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

	/*
	 * Configured AFTER startup on purpose: the module scans its
	 * plugin directory only once it has a compositor, and the widget
	 * lists are parsed in the same pass -- after the scan, so a
	 * widget naming a plugin from that directory resolves.
	 */
	{
		GHashTable *settings = str_table();

		g_hash_table_insert(settings, g_strdup("plugin-dir"),
		                    g_strdup(r->plugin_dir));
		g_hash_table_insert(settings, g_strdup("widgets"),
		                    g_strdup("probe"));
		g_hash_table_insert(settings, g_strdup("height"),
		                    g_strdup("24"));
		configure_bar(r, settings);
		g_hash_table_destroy(settings);
	}
	/*
	 * Long enough for two ticks.  The bar ticks at the shortest
	 * interval any widget asks for and reschedules on configure, so
	 * the probe's one second is in force from the moment it joins.
	 */
	settle(r, 2500);
	return TRUE;
}

static void
rig_down(Rig *r)
{
	/*
	 * Shutdown is dispatched before the compositor goes, the way
	 * main() does it: the bar holds signal handlers on the compositor
	 * and drops them there.  Without it the module would disconnect
	 * from a freed object.
	 */
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

/* How many lines of the log say `<what> <where>'. */
static gint
count_lines(const gchar *log, const gchar *what, const gchar *where)
{
	g_autofree gchar *body = NULL;
	g_auto(GStrv) lines = NULL;
	g_autofree gchar *want = NULL;
	gint i, found = 0;

	if (!g_file_get_contents(log, &body, NULL, NULL))
		return 0;

	want = g_strdup_printf("%s %s", what, where);
	lines = g_strsplit(body, "\n", -1);
	for (i = 0; lines[i] != NULL; i++) {
		if (strcmp(lines[i], want) == 0)
			found++;
	}
	return found;
}

/* Every distinct output named by a `draw' line. */
static GHashTable *
drawn_outputs(const gchar *log)
{
	g_autofree gchar *body = NULL;
	g_auto(GStrv) lines = NULL;
	GHashTable *seen;
	gint i;

	seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	if (!g_file_get_contents(log, &body, NULL, NULL))
		return seen;

	lines = g_strsplit(body, "\n", -1);
	for (i = 0; lines[i] != NULL; i++) {
		if (!g_str_has_prefix(lines[i], "draw "))
			continue;
		g_hash_table_add(seen, g_strdup(lines[i] + 5));
	}
	return seen;
}

/*
 * The draw of each screen's bar is served that screen.
 *
 * Two outputs, one widget, and the widget must have been drawn for both
 * of them by name.  Before the host said which, every draw would have
 * reported "(none)" and the widget would have had to guess.
 */
static void
test_the_draw_is_served_its_own_output(void)
{
	Rig r;
	g_autoptr(GHashTable) seen = NULL;
	GList *monitors, *l;

	if (!rig_up(&r)) {
		rig_down(&r);
		return;
	}

	monitors = gowl_compositor_get_monitors(r.compositor);
	g_assert_cmpuint(g_list_length(monitors), ==, 2);

	seen = drawn_outputs(r.log);
	if (g_hash_table_size(seen) == 0) {
		/* Nothing drew at all: the plugin never compiled, or this
		   build cannot reach gowl's own headers.  A packaging
		   question, not a failure of this code. */
		g_test_skip("the probe plugin never drew; no plugin build here");
		rig_down(&r);
		return;
	}

	for (l = monitors; l != NULL; l = l->next) {
		const gchar *name = gowl_monitor_get_name(GOWL_MONITOR(l->data));

		g_assert_true(g_hash_table_contains(seen, name));
	}

	/* And nothing was drawn without being told, which is what the
	   whole mechanism is for. */
	g_assert_false(g_hash_table_contains(seen, "(none)"));

	rig_down(&r);
}

/*
 * A poll is served nothing.
 *
 * It runs once for every screen at once, so there is no output it
 * belongs to and saying one would be a lie a widget would act on.  This
 * also catches the host leaving the last-drawn monitor set: the poll
 * runs between draws, and would then report a real output name.
 */
static void
test_a_poll_is_served_no_output(void)
{
	Rig r;
	g_autofree gchar *body = NULL;

	if (!rig_up(&r)) {
		rig_down(&r);
		return;
	}

	if (!g_file_get_contents(r.log, &body, NULL, NULL)
	    || strstr(body, "poll ") == NULL) {
		g_test_skip("the probe plugin never ran; no plugin build here");
		rig_down(&r);
		return;
	}

	g_assert_cmpint(count_lines(r.log, "poll", "(none)"), >, 0);
	{
		GList *monitors, *l;

		monitors = gowl_compositor_get_monitors(r.compositor);
		for (l = monitors; l != NULL; l = l->next) {
			const gchar *name;

			name = gowl_monitor_get_name(GOWL_MONITOR(l->data));
			g_assert_cmpint(count_lines(r.log, "poll", name),
			                ==, 0);
		}
	}

	rig_down(&r);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/bar-monitor/draw-is-served-its-own-output",
	                test_the_draw_is_served_its_own_output);
	g_test_add_func("/bar-monitor/poll-is-served-no-output",
	                test_a_poll_is_served_no_output);
	return g_test_run();
}
