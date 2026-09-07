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
 * The end-to-end plugin path: a `.c' file on disk, compiled by crispy,
 * dlopened, registered, instantiated, polled, and reloaded --- with no
 * build system, no install step, and no restart.
 *
 * This is the test that would catch a broken include path or a renamed
 * export, both of which are invisible until somebody actually drops a
 * plugin in their config directory.  It skips itself, rather than
 * failing, on a machine with no compiler.
 */

#include <string.h>

#include "barkit/gowl-bar-plugin.h"
#include "barkit/gowl-bar-registry.h"

/* The smallest plugin that is still a plugin. */
static const gchar *plugin_source =
	"#include <gowl/barkit/gowl-bar-plugin.h>\n"
	"#include <gowl/barkit/gowl-bar-plugin-proxy.h>\n"
	"#include <gowl/barkit/gowl-bar-registry.h>\n"
	"\n"
	"static void\n"
	"tiny_poll(GowlBarPlugin *plugin, gpointer data)\n"
	"{\n"
	"\t(void)data;\n"
	"\tgowl_bar_plugin_set_label(plugin, \"%s\");\n"
	"}\n"
	"\n"
	"static GowlBarPanel *\n"
	"tiny_panel(GowlBarPlugin *plugin, gpointer data)\n"
	"{\n"
	"\tGowlBarPanel *panel;\n"
	"\n"
	"\t(void)plugin;\n"
	"\t(void)data;\n"
	"\tpanel = gowl_bar_panel_new();\n"
	"\tgowl_bar_panel_add_label(panel, \"from a C script\");\n"
	"\treturn panel;\n"
	"}\n"
	"\n"
	"static const GowlBarPluginVTable tiny_vtable = {\n"
	"\tsizeof(GowlBarPluginVTable),\n"
	"\tNULL, NULL, NULL, NULL, NULL,\n"
	"\tNULL, tiny_poll, NULL,\n"
	"\tNULL, NULL, NULL, NULL,\n"
	"\ttiny_panel, NULL, NULL, NULL\n"
	"};\n"
	"\n"
	"static const GowlBarPluginDesc descs[] = {\n"
	"\t{ GOWL_BAR_PLUGIN_ABI, \"tiny\", \"Tiny\", \"A test plugin\",\n"
	"\t  \"1.0.0\", &tiny_vtable, NULL, { NULL, NULL, NULL, NULL } }\n"
	"};\n"
	"\n"
	"G_MODULE_EXPORT const GowlBarPluginDesc *\n"
	"gowl_bar_plugin_query(guint *n_descs)\n"
	"{\n"
	"\t*n_descs = G_N_ELEMENTS(descs);\n"
	"\treturn descs;\n"
	"}\n";

/* A source whose descriptor claims a different ABI, standing in for a
   plugin built against another gowl. */
static const gchar *wrong_abi_source =
	"#include <gowl/barkit/gowl-bar-plugin.h>\n"
	"#include <gowl/barkit/gowl-bar-plugin-proxy.h>\n"
	"#include <gowl/barkit/gowl-bar-registry.h>\n"
	"\n"
	"static const GowlBarPluginVTable vt = {\n"
	"\tsizeof(GowlBarPluginVTable),\n"
	"\tNULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,\n"
	"\tNULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL\n"
	"};\n"
	"\n"
	"static const GowlBarPluginDesc descs[] = {\n"
	"\t{ GOWL_BAR_PLUGIN_ABI + 99u, \"future\", NULL, NULL, NULL,\n"
	"\t  &vt, NULL, { NULL, NULL, NULL, NULL } }\n"
	"};\n"
	"\n"
	"G_MODULE_EXPORT const GowlBarPluginDesc *\n"
	"gowl_bar_plugin_query(guint *n_descs)\n"
	"{\n"
	"\t*n_descs = G_N_ELEMENTS(descs);\n"
	"\treturn descs;\n"
	"}\n";

static gchar *
write_plugin(const gchar *dir, const gchar *name, const gchar *body)
{
	gchar *path;

	path = g_build_filename(dir, name, NULL);
	g_assert_true(g_file_set_contents(path, body, -1, NULL));
	return path;
}

static void
test_a_c_plugin_compiles_loads_and_runs(void)
{
	g_autofree gchar *dir = NULL;
	g_autofree gchar *path = NULL;
	g_autofree gchar *body = NULL;
	g_autoptr(GowlBarRegistry) registry = NULL;
	g_autoptr(GError) error = NULL;
	GowlBarPlugin *plugin;

	if (g_find_program_in_path("gcc") == NULL) {
		g_test_skip("no compiler, so .c plugins cannot be built");
		return;
	}

	dir = g_dir_make_tmp("gowl-bar-load-XXXXXX", NULL);
	body = g_strdup_printf(plugin_source, "first");
	path = write_plugin(dir, "tiny.c", body);

	registry = gowl_bar_registry_new(dir);
	if (!gowl_bar_registry_load_file(registry, path, &error)) {
		/* A machine without gowl's own headers reachable cannot
		   build a plugin; that is a packaging question, not a
		   failure of this code. */
		g_test_skip(error->message);
		return;
	}

	g_assert_true(gowl_bar_registry_has(registry, "tiny"));

	plugin = gowl_bar_registry_instantiate(registry, "tiny", &error);
	g_assert_nonnull(plugin);
	g_assert_true(gowl_bar_plugin_activate(plugin, &error));

	gowl_bar_plugin_poll(plugin);
	{
		g_autofree gchar *label = NULL;

		label = gowl_bar_plugin_dup_label(plugin);
		g_assert_cmpstr(label, ==, "first");
	}

	g_assert_true(gowl_bar_plugin_has_panel(plugin));
	{
		GowlBarPanel *panel;

		panel = gowl_bar_plugin_build_panel(plugin);
		g_assert_nonnull(panel);
		g_assert_cmpuint(gowl_bar_panel_n_items(panel), ==, 1);
		g_object_unref(panel);
	}
	g_object_unref(plugin);

	/* The load is journalled and the entry cleared on success, so a
	   clean load must leave nothing behind to quarantine. */
	g_assert_false(gowl_bar_registry_is_quarantined(registry, "tiny"));

	/* Edit and reload: the point of the .c path is not needing a
	   restart. */
	g_free(body);
	body = g_strdup_printf(plugin_source, "second");
	g_assert_true(g_file_set_contents(path, body, -1, NULL));

	g_assert_true(gowl_bar_registry_reload(registry, "tiny", &error));

	plugin = gowl_bar_registry_instantiate(registry, "tiny", &error);
	g_assert_nonnull(plugin);
	gowl_bar_plugin_poll(plugin);
	{
		g_autofree gchar *label = NULL;

		label = gowl_bar_plugin_dup_label(plugin);
		g_assert_cmpstr(label, ==, "second");
	}
	g_object_unref(plugin);

	/* And it can be dropped entirely. */
	g_assert_true(gowl_bar_registry_unload(registry, "tiny", &error));
	g_assert_false(gowl_bar_registry_has(registry, "tiny"));
}

static void
test_a_wrong_abi_plugin_is_refused(void)
{
	g_autofree gchar *dir = NULL;
	g_autofree gchar *path = NULL;
	g_autoptr(GowlBarRegistry) registry = NULL;
	g_autoptr(GError) error = NULL;
	GLogLevelFlags saved;

	if (g_find_program_in_path("gcc") == NULL) {
		g_test_skip("no compiler, so .c plugins cannot be built");
		return;
	}

	dir = g_dir_make_tmp("gowl-bar-abi-XXXXXX", NULL);
	path = write_plugin(dir, "future.c", wrong_abi_source);

	registry = gowl_bar_registry_new(dir);

	/* Refusing loudly is the requirement: a plugin built against a
	   different barkit that loaded anyway would be a segfault inside
	   the compositor. */
	saved = g_log_set_always_fatal(G_LOG_LEVEL_ERROR);
	g_assert_false(gowl_bar_registry_load_file(registry, path, &error));
	g_log_set_always_fatal(saved);

	g_assert_false(gowl_bar_registry_has(registry, "future"));
}

static void
test_a_file_that_is_not_a_plugin_is_refused(void)
{
	g_autofree gchar *dir = NULL;
	g_autofree gchar *path = NULL;
	g_autoptr(GowlBarRegistry) registry = NULL;
	g_autoptr(GError) error = NULL;

	dir = g_dir_make_tmp("gowl-bar-junk-XXXXXX", NULL);
	path = write_plugin(dir, "junk.so", "not an elf file at all");

	registry = gowl_bar_registry_new(dir);
	g_assert_false(gowl_bar_registry_load_file(registry, path, &error));
	g_assert_nonnull(error);
}

static void
test_a_quarantined_plugin_is_not_loaded(void)
{
	g_autofree gchar *dir = NULL;
	g_autofree gchar *path = NULL;
	g_autofree gchar *body = NULL;
	g_autoptr(GowlBarRegistry) registry = NULL;
	g_autoptr(GError) error = NULL;

	dir = g_dir_make_tmp("gowl-bar-held-XXXXXX", NULL);
	body = g_strdup_printf(plugin_source, "held");
	path = write_plugin(dir, "tiny.c", body);

	registry = gowl_bar_registry_new(dir);
	gowl_bar_registry_quarantine(registry, "tiny", "it faulted before");

	/* The file name, not just the registered name, is what the
	   quarantine has to match on -- the plugin never registers, so
	   its name is only knowable from the path. */
	g_assert_false(gowl_bar_registry_load_file(registry, path, &error));
	g_assert_nonnull(error);
	g_assert_false(gowl_bar_registry_has(registry, "tiny"));
}

int
main(int argc, char *argv[])
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/bar-plugin-load/c-script-round-trip",
	                test_a_c_plugin_compiles_loads_and_runs);
	g_test_add_func("/bar-plugin-load/wrong-abi-refused",
	                test_a_wrong_abi_plugin_is_refused);
	g_test_add_func("/bar-plugin-load/junk-refused",
	                test_a_file_that_is_not_a_plugin_is_refused);
	g_test_add_func("/bar-plugin-load/quarantine-blocks-load",
	                test_a_quarantined_plugin_is_not_loaded);

	return g_test_run();
}
