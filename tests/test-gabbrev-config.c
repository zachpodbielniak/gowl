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
 * test-gabbrev-config.c - gabbrev in the two configuration languages.
 *
 * The YAML side: a `gabbrev:' section is kept whole for the module,
 * with the config file's directory for relative paths, and a section
 * that is not a mapping is a counted problem.  The C side: the shipped
 * example, compiled the way a user's config.c is (it includes
 * <gowl/gowl.h>), registers what it says it does.
 */

#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>

#include <yaml-glib.h>

#include "gowl.h"
#include "gabbrev/gowl-gabbrev-private.h"
#include "../data/example-gabbrev.c"

/*
 * GTest makes every warning fatal, and a section that is not a mapping
 * warns on purpose (that IS the report).  This writer swallows the one
 * warning a test names, and counts it -- g_test_expect_message() does
 * not see structured logging.
 */
static const gchar *expected_warning = NULL;
static guint        saw_expected_warning = 0;

static GLogWriterOutput
expect_warning_writer(
	GLogLevelFlags   level,
	const GLogField *fields,
	gsize            n_fields,
	gpointer         user_data
){
	gsize i;

	if (expected_warning != NULL && (level & G_LOG_LEVEL_WARNING)) {
		for (i = 0; i < n_fields; i++) {
			if (g_strcmp0(fields[i].key, "MESSAGE") == 0
			    && strstr((const gchar *)fields[i].value,
			              expected_warning) != NULL) {
				saw_expected_warning++;
				return G_LOG_WRITER_HANDLED;
			}
		}
	}
	return g_log_writer_default(level, fields, n_fields, user_data);
}

static gchar *
write_config(const gchar *text)
{
	g_autofree gchar *dir = g_dir_make_tmp("gabbrev-config-XXXXXX", NULL);
	gchar *path = g_build_filename(dir, "config.yaml", NULL);

	g_assert_true(g_file_set_contents(path, text, -1, NULL));
	return path;
}

static void
remove_config(const gchar *path)
{
	g_autofree gchar *dir = g_path_get_dirname(path);

	g_unlink(path);
	g_rmdir(dir);
}

static void
test_section(void)
{
	g_autoptr(GowlConfig) config = gowl_config_new();
	g_autofree gchar *path = write_config(
		"modules:\n"
		"  gabbrev: {enabled: true}\n"
		"gabbrev:\n"
		"  toggle_key: ALT\n"
		"  files: [snips.yml]\n"
		"  matches:\n"
		"    - {trigger: ';x', replace: y}\n");
	g_autoptr(GError) error = NULL;
	g_autofree gchar *dir = g_path_get_dirname(path);
	GPtrArray *nodes;
	YamlMapping *m;

	g_assert_true(gowl_config_load_yaml(config, path, &error));
	g_assert_no_error(error);
	g_assert_cmpuint(gowl_config_get_problem_count(config), ==, 0);
	nodes = gowl_config_get_gabbrev_nodes(config);
	g_assert_cmpuint(nodes->len, ==, 1);
	m = yaml_node_get_mapping(g_ptr_array_index(nodes, 0));
	g_assert_nonnull(m);
	g_assert_cmpstr(yaml_mapping_get_string_member(m, "toggle_key"), ==, "ALT");
	g_assert_true(yaml_mapping_has_member(m, "matches"));
	g_assert_cmpstr(gowl_config_get_gabbrev_base_dir(config), ==, dir);

	/* a second section from code, and clearing */
	g_assert_true(gowl_config_add_gabbrev_yaml(config,
		"matches: [{trigger: ';z', replace: z}]", NULL));
	g_assert_cmpuint(nodes->len, ==, 2);
	g_assert_false(gowl_config_add_gabbrev_yaml(config, "- a list", NULL));
	g_assert_false(gowl_config_add_gabbrev_yaml(config, "matches: [", NULL));
	gowl_config_clear_gabbrev(config);
	g_assert_cmpuint(nodes->len, ==, 0);
	remove_config(path);
}

static void
test_section_not_a_mapping(void)
{
	g_autoptr(GowlConfig) config = gowl_config_new();
	g_autofree gchar *path = write_config("gabbrev: [1, 2]\n");

	expected_warning = "`gabbrev:' is a mapping";
	saw_expected_warning = 0;
	g_assert_true(gowl_config_load_yaml(config, path, NULL));
	expected_warning = NULL;
	g_assert_cmpuint(saw_expected_warning, ==, 1);
	g_assert_cmpuint(gowl_config_get_problem_count(config), ==, 1);
	g_assert_cmpuint(gowl_config_get_gabbrev_nodes(config)->len, ==, 0);
	remove_config(path);
}

static void
test_c_example(void)
{
	g_auto(GStrv) funcs = NULL;
	g_auto(GStrv) types = NULL;
	g_autoptr(GPtrArray) sources = NULL;
	g_autoptr(GowlGabbrevHandle) h = NULL;
	GowlGabbrevRequestInfo info;
	g_autoptr(GowlGabbrevRequest) req = NULL;
	g_autofree gchar *host = NULL;

	gowl_gabbrev_clear_sources();
	g_assert_true(add_gabbrev());
	funcs = gowl_gabbrev_list_funcs();
	types = gowl_gabbrev_list_var_types();
	g_assert_true(g_strv_contains((const gchar * const *)funcs,
	                              "example-hostname"));
	g_assert_true(g_strv_contains((const gchar * const *)types, "count"));
	sources = gowl_gabbrev_dup_sources();
	g_assert_cmpuint(sources->len, ==, 3);

	memset(&info, 0, sizeof info);
	info.var_name = "h";
	info.var_type = "c";
	req = gowl_gabbrev_request_new(&info, NULL, NULL, NULL);
	h = gowl_gabbrev_lookup_func("example-hostname");
	host = gowl_gabbrev_handle_call_func(h, req);
	g_assert_cmpstr(host, ==, g_get_host_name());

	gowl_gabbrev_unregister_func("example-hostname");
	gowl_gabbrev_unregister_var_type("count");
	gowl_gabbrev_clear_sources();
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_log_set_writer_func(expect_warning_writer, NULL, NULL);
	g_test_add_func("/gabbrev/config/section", test_section);
	g_test_add_func("/gabbrev/config/section-not-a-mapping",
	                test_section_not_a_mapping);
	g_test_add_func("/gabbrev/config/c-example", test_c_example);
	return g_test_run();
}
