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

	return g_test_run();
}
