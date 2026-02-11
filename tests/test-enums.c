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

#include "gowl-enums.h"

static void
test_hook_point_type(void)
{
	GType type;

	type = gowl_hook_point_get_type();
	g_assert_true(type != G_TYPE_INVALID);
	g_assert_true(G_TYPE_IS_ENUM(type));
}

static void
test_hook_point_values(void)
{
	GEnumClass *klass;
	GEnumValue *val;

	klass = g_type_class_ref(GOWL_TYPE_HOOK_POINT);
	g_assert_nonnull(klass);

	val = g_enum_get_value(klass, GOWL_HOOK_POINT_STARTUP);
	g_assert_nonnull(val);

	val = g_enum_get_value(klass, GOWL_HOOK_POINT_SHUTDOWN);
	g_assert_nonnull(val);

	val = g_enum_get_value(klass, GOWL_HOOK_POINT_KEY_PRESS);
	g_assert_nonnull(val);

	val = g_enum_get_value(klass, GOWL_HOOK_POINT_CLIENT_NEW);
	g_assert_nonnull(val);

	val = g_enum_get_value(klass, GOWL_HOOK_POINT_LAYOUT_ARRANGE);
	g_assert_nonnull(val);

	val = g_enum_get_value(klass, GOWL_HOOK_POINT_LAST);
	g_assert_nonnull(val);

	g_type_class_unref(klass);
}

static void
test_cursor_mode_type(void)
{
	GType type;

	type = gowl_cursor_mode_get_type();
	g_assert_true(type != G_TYPE_INVALID);
	g_assert_true(G_TYPE_IS_ENUM(type));
}

static void
test_direction_type(void)
{
	GType type;

	type = gowl_direction_get_type();
	g_assert_true(type != G_TYPE_INVALID);
	g_assert_true(G_TYPE_IS_ENUM(type));
}

static void
test_key_mod_flags_type(void)
{
	GType type;

	type = gowl_key_mod_get_type();
	g_assert_true(type != G_TYPE_INVALID);
	g_assert_true(G_TYPE_IS_FLAGS(type));
}

static void
test_key_mod_values(void)
{
	guint combo;

	combo = GOWL_KEY_MOD_LOGO | GOWL_KEY_MOD_SHIFT;
	g_assert_cmpuint(combo & GOWL_KEY_MOD_LOGO, !=, 0);
	g_assert_cmpuint(combo & GOWL_KEY_MOD_SHIFT, !=, 0);
	g_assert_cmpuint(combo & GOWL_KEY_MOD_CTRL, ==, 0);
}

static void
test_action_type(void)
{
	GType type;

	type = gowl_action_get_type();
	g_assert_true(type != G_TYPE_INVALID);
	g_assert_true(G_TYPE_IS_ENUM(type));
}

static void
test_client_state_type(void)
{
	GType type;

	type = gowl_client_state_get_type();
	g_assert_true(type != G_TYPE_INVALID);
	g_assert_true(G_TYPE_IS_ENUM(type));
}

static void
test_idle_state_type(void)
{
	GType type;

	type = gowl_idle_state_get_type();
	g_assert_true(type != G_TYPE_INVALID);
	g_assert_true(G_TYPE_IS_ENUM(type));
}

static void
test_config_source_type(void)
{
	GType type;

	type = gowl_config_source_get_type();
	g_assert_true(type != G_TYPE_INVALID);
	g_assert_true(G_TYPE_IS_ENUM(type));
}

static void
test_layer_shell_layer_type(void)
{
	GType type;

	type = gowl_layer_shell_layer_get_type();
	g_assert_true(type != G_TYPE_INVALID);
	g_assert_true(G_TYPE_IS_ENUM(type));
}

int
main(int argc, char *argv[])
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/enums/hook-point/type", test_hook_point_type);
	g_test_add_func("/enums/hook-point/values", test_hook_point_values);
	g_test_add_func("/enums/cursor-mode/type", test_cursor_mode_type);
	g_test_add_func("/enums/direction/type", test_direction_type);
	g_test_add_func("/enums/key-mod/type", test_key_mod_flags_type);
	g_test_add_func("/enums/key-mod/values", test_key_mod_values);
	g_test_add_func("/enums/action/type", test_action_type);
	g_test_add_func("/enums/client-state/type", test_client_state_type);
	g_test_add_func("/enums/idle-state/type", test_idle_state_type);
	g_test_add_func("/enums/config-source/type", test_config_source_type);
	g_test_add_func("/enums/layer-shell-layer/type", test_layer_shell_layer_type);

	return g_test_run();
}
