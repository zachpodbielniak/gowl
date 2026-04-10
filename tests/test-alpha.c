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

#include "gowl.h"

#include <math.h>

/* ---- Client alpha accessor tests ---- */

static void
test_alpha_client_default(void)
{
	GowlClient *c;

	c = gowl_client_new();
	g_assert_true(fabsf(gowl_client_get_alpha(c) - 1.0f) < 0.001f);
	g_object_unref(c);
}

static void
test_alpha_client_set_get(void)
{
	GowlClient *c;

	c = gowl_client_new();
	gowl_client_set_alpha(c, 0.5f);
	g_assert_true(fabsf(gowl_client_get_alpha(c) - 0.5f) < 0.001f);
	g_object_unref(c);
}

static void
test_alpha_client_clamp_low(void)
{
	GowlClient *c;

	c = gowl_client_new();
	gowl_client_set_alpha(c, -0.5f);
	g_assert_true(fabsf(gowl_client_get_alpha(c) - 0.0f) < 0.001f);
	g_object_unref(c);
}

static void
test_alpha_client_clamp_high(void)
{
	GowlClient *c;

	c = gowl_client_new();
	gowl_client_set_alpha(c, 2.0f);
	g_assert_true(fabsf(gowl_client_get_alpha(c) - 1.0f) < 0.001f);
	g_object_unref(c);
}

static void
test_alpha_client_zero(void)
{
	GowlClient *c;

	c = gowl_client_new();
	gowl_client_set_alpha(c, 0.0f);
	g_assert_true(fabsf(gowl_client_get_alpha(c) - 0.0f) < 0.001f);
	g_object_unref(c);
}

static void
test_alpha_client_one(void)
{
	GowlClient *c;

	c = gowl_client_new();
	gowl_client_set_alpha(c, 0.3f);
	gowl_client_set_alpha(c, 1.0f);
	g_assert_true(fabsf(gowl_client_get_alpha(c) - 1.0f) < 0.001f);
	g_object_unref(c);
}

/* ---- Main ---- */

int
main(int argc, char *argv[])
{
	g_test_init(&argc, &argv, NULL);

	/* Client alpha accessors */
	g_test_add_func("/alpha/client/default", test_alpha_client_default);
	g_test_add_func("/alpha/client/set-get", test_alpha_client_set_get);
	g_test_add_func("/alpha/client/clamp-low", test_alpha_client_clamp_low);
	g_test_add_func("/alpha/client/clamp-high", test_alpha_client_clamp_high);
	g_test_add_func("/alpha/client/zero", test_alpha_client_zero);
	g_test_add_func("/alpha/client/one", test_alpha_client_one);

	return g_test_run();
}
