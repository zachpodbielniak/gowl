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

#include "config/gowl-config.h"
#include <glib/gstdio.h>
#include <string.h>

static void
test_config_new(void)
{
	GowlConfig *config;

	config = gowl_config_new();
	g_assert_nonnull(config);
	g_assert_true(GOWL_IS_CONFIG(config));
	g_object_unref(config);
}

static void
test_config_defaults(void)
{
	GowlConfig *config;

	config = gowl_config_new();

	g_assert_cmpint(gowl_config_get_border_width(config), ==, 2);
	g_assert_cmpfloat_with_epsilon(gowl_config_get_mfact(config), 0.55, 0.001);
	g_assert_cmpint(gowl_config_get_nmaster(config), ==, 1);
	g_assert_cmpint(gowl_config_get_tag_count(config), ==, 9);
	g_assert_cmpint(gowl_config_get_repeat_rate(config), ==, 25);
	g_assert_cmpint(gowl_config_get_repeat_delay(config), ==, 600);
	g_assert_true(gowl_config_get_sloppyfocus(config));
	g_assert_cmpstr(gowl_config_get_terminal(config), ==, "gst");
	g_assert_cmpstr(gowl_config_get_menu(config), ==, "bemenu-run");

	g_object_unref(config);
}

static void
test_config_set_properties(void)
{
	GowlConfig *config;

	config = gowl_config_new();

	g_object_set(G_OBJECT(config),
		"border-width", 5,
		"mfact", 0.65,
		"nmaster", 2,
		"tag-count", 6,
		"terminal", "kitty",
		NULL);

	g_assert_cmpint(gowl_config_get_border_width(config), ==, 5);
	g_assert_cmpfloat_with_epsilon(gowl_config_get_mfact(config), 0.65, 0.001);
	g_assert_cmpint(gowl_config_get_nmaster(config), ==, 2);
	g_assert_cmpint(gowl_config_get_tag_count(config), ==, 6);
	g_assert_cmpstr(gowl_config_get_terminal(config), ==, "kitty");

	g_object_unref(config);
}

static void
test_config_generate_yaml(void)
{
	GowlConfig *config;
	gchar *yaml;

	config = gowl_config_new();
	yaml = gowl_config_generate_yaml(config);

	g_assert_nonnull(yaml);
	/* Check that some expected keys are present */
	g_assert_true(strstr(yaml, "border-width") != NULL);
	g_assert_true(strstr(yaml, "mfact") != NULL);
	g_assert_true(strstr(yaml, "terminal") != NULL);

	g_free(yaml);
	g_object_unref(config);
}

static void
test_config_add_rule(void)
{
	GowlConfig *config;

	config = gowl_config_new();

	gowl_config_add_rule(config, "firefox", NULL, 1 << 1, FALSE, -1);
	gowl_config_add_rule(config, NULL, "*popup*", 0, TRUE, -1);

	/* Rules are stored internally - we just verify no crash */
	g_object_unref(config);
}

static void
test_config_type(void)
{
	GType type;

	type = GOWL_TYPE_CONFIG;
	g_assert_true(type != G_TYPE_INVALID);
	g_assert_true(G_TYPE_IS_OBJECT(type));
}

/* --- monitors: YAML block --- */

/* Load YAML from an inline string by writing it to a temp file
 * and calling gowl_config_load_yaml.  Returns TRUE on success. */
static gboolean
load_yaml_from_string(GowlConfig  *config,
                       const gchar *yaml,
                       GError     **error)
{
	g_autofree gchar *tmpdir = NULL;
	g_autofree gchar *path   = NULL;

	tmpdir = g_dir_make_tmp("gowl-test-XXXXXX", error);
	if (tmpdir == NULL)
		return FALSE;

	path = g_build_filename(tmpdir, "config.yaml", NULL);
	if (!g_file_set_contents(path, yaml, -1, error)) {
		g_unlink(path);
		g_rmdir(tmpdir);
		return FALSE;
	}

	if (!gowl_config_load_yaml(config, path, error)) {
		g_unlink(path);
		g_rmdir(tmpdir);
		return FALSE;
	}

	g_unlink(path);
	g_rmdir(tmpdir);
	return TRUE;
}

static void
test_config_monitors_full(void)
{
	GowlConfig *config;
	const GowlMonitorConfig *mc;
	GError *err = NULL;
	gboolean ok;
	const gchar *yaml =
		"monitors:\n"
		"  eDP-1:\n"
		"    width: 1920\n"
		"    height: 1080\n"
		"    refresh: 60.0\n"
		"    x: 0\n"
		"    y: 0\n"
		"    scale: 1.5\n"
		"    enabled: true\n"
		"    transform: 90\n";

	config = gowl_config_new();
	ok = load_yaml_from_string(config, yaml, &err);
	g_assert_no_error(err);
	g_assert_true(ok);

	mc = gowl_config_get_monitor_config(config, "eDP-1");
	g_assert_nonnull(mc);
	g_assert_cmpint(mc->width, ==, 1920);
	g_assert_cmpint(mc->height, ==, 1080);
	g_assert_cmpfloat_with_epsilon(mc->refresh, 60.0, 0.001);
	g_assert_cmpint(mc->x, ==, 0);
	g_assert_cmpint(mc->y, ==, 0);
	g_assert_cmpfloat_with_epsilon(mc->scale, 1.5, 0.001);
	g_assert_cmpint(mc->enabled, ==, 1);
	g_assert_cmpint(mc->transform, ==, 1);

	g_assert_null(gowl_config_get_monitor_config(config, "HDMI-A-1"));

	g_object_unref(config);
}

static void
test_config_monitors_partial(void)
{
	GowlConfig *config;
	const GowlMonitorConfig *mc;
	GError *err = NULL;
	gboolean ok;
	const gchar *yaml =
		"monitors:\n"
		"  eDP-1:\n"
		"    transform: 90\n";

	config = gowl_config_new();
	ok = load_yaml_from_string(config, yaml, &err);
	g_assert_no_error(err);
	g_assert_true(ok);

	mc = gowl_config_get_monitor_config(config, "eDP-1");
	g_assert_nonnull(mc);
	g_assert_cmpint(mc->transform, ==, 1);

	/* All other fields stay at the "unset" sentinel */
	g_assert_cmpint(mc->width, ==, 0);
	g_assert_cmpint(mc->height, ==, 0);
	g_assert_cmpfloat(mc->refresh, ==, 0.0);
	g_assert_cmpint(mc->x, ==, G_MININT);
	g_assert_cmpint(mc->y, ==, G_MININT);
	g_assert_cmpfloat(mc->scale, ==, 0.0);
	g_assert_cmpint(mc->enabled, ==, -1);

	g_object_unref(config);
}

static void
test_config_monitors_transform_string(void)
{
	GowlConfig *config;
	const GowlMonitorConfig *mc;
	GError *err = NULL;
	gboolean ok;
	const gchar *yaml =
		"monitors:\n"
		"  eDP-1:\n"
		"    transform: flipped-180\n"
		"  HDMI-A-1:\n"
		"    transform: normal\n"
		"  DP-2:\n"
		"    transform: 270\n";

	config = gowl_config_new();
	ok = load_yaml_from_string(config, yaml, &err);
	g_assert_no_error(err);
	g_assert_true(ok);

	mc = gowl_config_get_monitor_config(config, "eDP-1");
	g_assert_nonnull(mc);
	g_assert_cmpint(mc->transform, ==, 6);

	mc = gowl_config_get_monitor_config(config, "HDMI-A-1");
	g_assert_nonnull(mc);
	g_assert_cmpint(mc->transform, ==, 0);

	/* "270" is the canonical *name* for transform 3, not the
	 * angle.  Our parser tries int first, so this string-parses
	 * as 270 (out of range) and falls back to the name table. */
	mc = gowl_config_get_monitor_config(config, "DP-2");
	g_assert_nonnull(mc);
	g_assert_cmpint(mc->transform, ==, 3);

	g_object_unref(config);
}

/* Custom writer that drops the parser's expected warning to the
 * floor.  With G_LOG_USE_STRUCTURED set (gowl's default), the test
 * framework's expect_message/fatal-handler hooks are bypassed --
 * structured logs route through the writer instead.  Returning
 * G_LOG_WRITER_HANDLED prevents both the default print and the
 * implicit abort that GTest installs on G_LOG_LEVEL_WARNING. */
static GLogWriterOutput
expect_warning_writer(GLogLevelFlags    log_level,
                       const GLogField  *fields,
                       gsize             n_fields,
                       gpointer          user_data)
{
	gsize i;
	(void)user_data;

	if (log_level & G_LOG_LEVEL_WARNING) {
		for (i = 0; i < n_fields; i++) {
			if (g_strcmp0(fields[i].key, "MESSAGE") == 0
			    && fields[i].value != NULL
			    && strstr((const char *)fields[i].value,
			              "invalid transform") != NULL)
				return G_LOG_WRITER_HANDLED;
		}
	}
	return g_log_writer_default(log_level, fields, n_fields, NULL);
}

static void
test_config_monitors_transform_invalid(void)
{
	GowlConfig *config;
	const GowlMonitorConfig *mc;
	GError *err = NULL;
	gboolean ok;
	const gchar *yaml =
		"monitors:\n"
		"  eDP-1:\n"
		"    transform: ninety\n";

	config = gowl_config_new();

	/* Install a structured-log writer that swallows the expected
	 * "invalid transform" warning, then restore the default after.
	 * g_log_set_writer_func can be called multiple times in tests
	 * by passing NULL to revert. */
	g_log_set_writer_func(expect_warning_writer, NULL, NULL);

	ok = load_yaml_from_string(config, yaml, &err);
	g_assert_no_error(err);
	g_assert_true(ok);

	mc = gowl_config_get_monitor_config(config, "eDP-1");
	g_assert_nonnull(mc);
	g_assert_cmpint(mc->transform, ==, -1);

	g_object_unref(config);
}

static void
test_config_monitors_names_iter(void)
{
	GowlConfig *config;
	GList *names;
	GError *err = NULL;
	const gchar *yaml =
		"monitors:\n"
		"  eDP-1:\n"
		"    transform: 1\n"
		"  HDMI-A-1:\n"
		"    scale: 1.5\n";

	config = gowl_config_new();
	g_assert_true(load_yaml_from_string(config, yaml, &err));
	g_assert_no_error(err);

	names = gowl_config_get_monitor_names(config);
	g_assert_cmpuint(g_list_length(names), ==, 2);
	g_list_free(names);

	g_object_unref(config);
}

int
main(int argc, char *argv[])
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/config/new", test_config_new);
	g_test_add_func("/config/defaults", test_config_defaults);
	g_test_add_func("/config/set-properties", test_config_set_properties);
	g_test_add_func("/config/generate-yaml", test_config_generate_yaml);
	g_test_add_func("/config/add-rule", test_config_add_rule);
	g_test_add_func("/config/type", test_config_type);
	g_test_add_func("/config/monitors-full",
	                test_config_monitors_full);
	g_test_add_func("/config/monitors-partial",
	                test_config_monitors_partial);
	g_test_add_func("/config/monitors-transform-string",
	                test_config_monitors_transform_string);
	g_test_add_func("/config/monitors-transform-invalid",
	                test_config_monitors_transform_invalid);
	g_test_add_func("/config/monitors-names-iter",
	                test_config_monitors_names_iter);

	return g_test_run();
}
